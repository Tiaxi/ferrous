#include "BridgeClient.h"

#include "FerrousBridgeFfi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QDir>
#include <QProcessEnvironment>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <QtEndian>

namespace {
constexpr quint8 kAnalysisFrameMagic = 0xA1;
constexpr quint8 kAnalysisFlagWaveform = 0x01;
constexpr quint8 kAnalysisFlagReset = 0x02;
constexpr quint8 kAnalysisFlagSpectrogram = 0x04;
constexpr quint32 kMaxAnalysisFrameBytes = 8 * 1024 * 1024;

bool isNewerSeq(quint32 seq, quint32 last) {
    return static_cast<qint32>(seq - last) > 0;
}

QString normalizeLocalPathArg(const QString &path) {
    QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    if (trimmed.startsWith(QStringLiteral("QUrl(\"")) && trimmed.endsWith(QStringLiteral("\")"))) {
        trimmed = trimmed.mid(6, trimmed.size() - 8);
    }

    const QUrl asUrl(trimmed);
    if (asUrl.isValid() && asUrl.isLocalFile()) {
        const QString localPath = asUrl.toLocalFile().trimmed();
        if (!localPath.isEmpty()) {
            return localPath;
        }
    }

    if (trimmed.startsWith(QStringLiteral("file://"))) {
        const QString localPath = QUrl(trimmed).toLocalFile().trimmed();
        if (!localPath.isEmpty()) {
            return localPath;
        }
    }

    return trimmed;
}

int readEnvMillis(const char *key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(key, &ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, 8, 1000);
}

QString findAlbumCoverPath(const QStringList &trackPaths) {
    static const QSet<QString> kImageExts{
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("png"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
    };
    static const QStringList kPreferredBases{
        QStringLiteral("cover"),
        QStringLiteral("folder"),
        QStringLiteral("front"),
        QStringLiteral("album"),
        QStringLiteral("artwork"),
    };

    QString bestPath;
    int bestScore = std::numeric_limits<int>::max();
    QSet<QString> scannedDirs;
    for (const QString &trackPath : trackPaths) {
        const QFileInfo trackInfo(trackPath);
        if (!trackInfo.exists()) {
            continue;
        }
        const QDir dir = trackInfo.dir();
        const QString dirPath = dir.absolutePath();
        if (scannedDirs.contains(dirPath)) {
            continue;
        }
        scannedDirs.insert(dirPath);

        const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &info : files) {
            const QString ext = info.suffix().toLower();
            if (!kImageExts.contains(ext)) {
                continue;
            }
            const QString base = info.completeBaseName().toLower();
            int score = 4;
            for (int i = 0; i < kPreferredBases.size(); ++i) {
                const QString &preferred = kPreferredBases[i];
                if (base == preferred) {
                    score = i;
                    break;
                }
                if (base.startsWith(preferred)) {
                    score = i + 1;
                }
            }
            if (bestPath.isEmpty() || score < bestScore
                || (score == bestScore && info.absoluteFilePath() < bestPath)) {
                bestPath = info.absoluteFilePath();
                bestScore = score;
            }
        }
    }
    return bestPath;
}

QString findTrackCoverUrl(const QString &trackPath) {
    if (trackPath.trimmed().isEmpty()) {
        return {};
    }
    const QString coverPath = findAlbumCoverPath({trackPath});
    if (coverPath.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(coverPath).toString();
}
} // namespace

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent) {
    m_fileBrowserName = detectFileBrowserName();
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &BridgeClient::handleStdoutReady);
    connect(&m_process, &QProcess::readyReadStandardError, this, &BridgeClient::handleStderrReady);
    connect(&m_process, &QProcess::started, this, &BridgeClient::handleProcessStarted);
    connect(&m_process, &QProcess::finished, this, &BridgeClient::handleProcessFinished);
    m_snapshotNotifyTimer.setSingleShot(true);
    m_snapshotNotifyTimer.setInterval(readEnvMillis("FERROUS_UI_SNAPSHOT_NOTIFY_MS", 100));
    connect(&m_snapshotNotifyTimer, &QTimer::timeout, this, [this]() {
        if (m_snapshotChangedPending) {
            m_snapshotChangedPending = false;
            emit snapshotChanged();
        }
    });
    m_analysisNotifyTimer.setSingleShot(true);
    m_analysisNotifyTimer.setInterval(readEnvMillis("FERROUS_UI_ANALYSIS_NOTIFY_MS", 16));
    connect(&m_analysisNotifyTimer, &QTimer::timeout, this, [this]() {
        if (m_analysisChangedPending) {
            m_analysisChangedPending = false;
            emit analysisChanged();
        }
    });
    m_bridgePollTimer.setInterval(readEnvMillis("FERROUS_UI_BRIDGE_POLL_MS", 16));
    connect(&m_bridgePollTimer, &QTimer::timeout, this, &BridgeClient::pollInProcessBridge);

    const QString bridgeMode = qEnvironmentVariable("FERROUS_BRIDGE_MODE").trimmed().toLower();
    const bool forceProcessBridge = bridgeMode == QStringLiteral("process");
    if (!forceProcessBridge && startInProcessBridge()) {
        m_useInProcessBridge = true;
    } else {
        setupAnalysisSocketServer();
        startBridgeProcess();
    }
}

BridgeClient::~BridgeClient() {
    m_bridgePollTimer.stop();
    m_analysisNotifyTimer.stop();
    if (m_ffiBridge != nullptr) {
        ferrous_ffi_bridge_destroy(m_ffiBridge);
        m_ffiBridge = nullptr;
    }
    teardownAnalysisSocket(true);
    if (m_analysisServer.isListening()) {
        const QString serverName = m_analysisServer.fullServerName();
        m_analysisServer.close();
        if (!serverName.isEmpty()) {
            QLocalServer::removeServer(serverName);
        }
    }
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(500)) {
            m_process.kill();
            m_process.waitForFinished(500);
        }
    }
}

bool BridgeClient::startInProcessBridge() {
    m_ffiBridge = ferrous_ffi_bridge_create();
    if (m_ffiBridge == nullptr) {
        emit bridgeError(QStringLiteral("failed to create in-process Rust bridge"));
        return false;
    }
    // In-process mode always uses binary analysis frames (no JSON analysis payload).
    m_analysisSocketConnected = true;
    m_bridgePollTimer.start();
    if (!m_connected) {
        m_connected = true;
        emit connectedChanged();
    }
    requestSnapshot();
    return true;
}

void BridgeClient::pollInProcessBridge() {
    if (m_ffiBridge == nullptr) {
        return;
    }
    ferrous_ffi_bridge_poll(m_ffiBridge, 64);

    bool anySnapshotChanged = false;
    int processedJsonEvents = 0;
    constexpr int kMaxJsonEventsPerPass = 2;
    while (processedJsonEvents < kMaxJsonEventsPerPass) {
        char *linePtr = ferrous_ffi_bridge_pop_json_event(m_ffiBridge);
        if (linePtr == nullptr) {
            break;
        }
        processedJsonEvents++;
        const QByteArray line(linePtr);
        ferrous_ffi_bridge_free_json_event(linePtr);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit bridgeError(QStringLiteral("invalid bridge json: %1").arg(QString::fromUtf8(line)));
            continue;
        }
        anySnapshotChanged |= processBridgeJsonObject(doc.object());
    }

    int processedAnalysisFrames = 0;
    constexpr int kMaxAnalysisFramesPerPass = 6;
    while (processedAnalysisFrames < kMaxAnalysisFramesPerPass) {
        std::size_t len = 0;
        std::uint8_t *framePtr = ferrous_ffi_bridge_pop_analysis_frame(m_ffiBridge, &len);
        if (framePtr == nullptr || len == 0) {
            break;
        }
        processedAnalysisFrames++;
        const QByteArray chunk(
            reinterpret_cast<const char *>(framePtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_analysis_frame(framePtr, len);
        processAnalysisBytes(chunk);
    }

    if (anySnapshotChanged) {
        scheduleSnapshotChanged();
    }
}

void BridgeClient::teardownAnalysisSocket(bool immediateDelete) {
    QLocalSocket *socket = m_analysisSocket;
    m_analysisSocket = nullptr;
    m_analysisSocketConnected = false;
    m_hasAnalysisFrameSeq = false;
    m_analysisBuffer.clear();
    m_analysisBufferReadOffset = 0;
    if (socket == nullptr) {
        return;
    }
    socket->disconnect(this);
    socket->close();
    if (immediateDelete) {
        delete socket;
    } else {
        socket->deleteLater();
    }
}

QString BridgeClient::playbackState() const {
    return m_playbackState;
}

QString BridgeClient::positionText() const {
    return m_positionText;
}

QString BridgeClient::durationText() const {
    return m_durationText;
}

double BridgeClient::positionSeconds() const {
    return m_positionSeconds;
}

double BridgeClient::durationSeconds() const {
    return m_durationSeconds;
}

double BridgeClient::volume() const {
    return m_volume;
}

int BridgeClient::queueLength() const {
    return m_queueLength;
}

QString BridgeClient::queueDurationText() const {
    return m_queueDurationText;
}

QStringList BridgeClient::queueItems() const {
    return m_queueItems;
}

int BridgeClient::selectedQueueIndex() const {
    return m_selectedQueueIndex;
}

int BridgeClient::playingQueueIndex() const {
    return m_playingQueueIndex;
}

QString BridgeClient::currentTrackPath() const {
    return m_currentTrackPath;
}

QString BridgeClient::currentTrackCoverPath() const {
    return m_currentTrackCoverPath;
}

QByteArray BridgeClient::waveformPeaksPacked() const {
    return m_waveformPeaksPacked;
}

bool BridgeClient::spectrogramReset() const {
    return m_spectrogramReset;
}

int BridgeClient::sampleRateHz() const {
    return m_sampleRateHz;
}

double BridgeClient::dbRange() const {
    return m_dbRange;
}

bool BridgeClient::logScale() const {
    return m_logScale;
}

int BridgeClient::repeatMode() const {
    return m_repeatMode;
}

bool BridgeClient::shuffleEnabled() const {
    return m_shuffleEnabled;
}

bool BridgeClient::showFps() const {
    return m_showFps;
}

QStringList BridgeClient::libraryAlbums() const {
    return m_libraryAlbums;
}

QVariantList BridgeClient::libraryTree() const {
    return m_libraryTree;
}

int BridgeClient::libraryVersion() const {
    return m_libraryVersion;
}

bool BridgeClient::libraryScanInProgress() const {
    return m_libraryScanInProgress;
}

int BridgeClient::libraryRootCount() const {
    return m_libraryRootCount;
}

int BridgeClient::libraryTrackCount() const {
    return m_libraryTrackCount;
}

QStringList BridgeClient::libraryRoots() const {
    return m_libraryRoots;
}

int BridgeClient::librarySortMode() const {
    return m_librarySortMode;
}

QString BridgeClient::fileBrowserName() const {
    return m_fileBrowserName;
}

int BridgeClient::libraryScanRootsCompleted() const {
    return m_libraryScanRootsCompleted;
}

int BridgeClient::libraryScanRootsTotal() const {
    return m_libraryScanRootsTotal;
}

int BridgeClient::libraryScanDiscovered() const {
    return m_libraryScanDiscovered;
}

int BridgeClient::libraryScanProcessed() const {
    return m_libraryScanProcessed;
}

double BridgeClient::libraryScanFilesPerSecond() const {
    return m_libraryScanFilesPerSecond;
}

double BridgeClient::libraryScanEtaSeconds() const {
    return m_libraryScanEtaSeconds;
}

bool BridgeClient::connected() const {
    return m_connected;
}

void BridgeClient::play() {
    sendCommand(QStringLiteral("play"));
}

void BridgeClient::pause() {
    sendCommand(QStringLiteral("pause"));
}

void BridgeClient::stop() {
    sendCommand(QStringLiteral("stop"));
}

void BridgeClient::next() {
    sendCommand(QStringLiteral("next"));
}

void BridgeClient::previous() {
    sendCommand(QStringLiteral("prev"));
}

void BridgeClient::seek(double seconds) {
    const double target = std::max(0.0, seconds);
    m_pendingSeek = true;
    m_pendingSeekTargetSeconds = target;
    m_pendingSeekUntilMs = QDateTime::currentMSecsSinceEpoch() + 900;
    bool changed = false;
    if (!qFuzzyCompare(m_positionSeconds + 1.0, target + 1.0)) {
        m_positionSeconds = target;
        changed = true;
    }
    const QString targetText = formatSeconds(target);
    if (m_positionText != targetText) {
        m_positionText = targetText;
        changed = true;
    }
    if (changed) {
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("seek"), target);
}

void BridgeClient::setVolume(double value) {
    sendCommand(QStringLiteral("set_volume"), std::clamp(value, 0.0, 1.0));
}

void BridgeClient::setDbRange(double value) {
    const double clamped = std::clamp(value, 50.0, 120.0);
    if (!qFuzzyCompare(m_dbRange + 1.0, clamped + 1.0)) {
        m_dbRange = clamped;
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("set_db_range"), clamped);
}

void BridgeClient::setLogScale(bool value) {
    if (m_logScale != value) {
        m_logScale = value;
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("set_log_scale"), value ? 1.0 : 0.0);
}

void BridgeClient::setRepeatMode(int mode) {
    const int clamped = std::clamp(mode, 0, 2);
    if (m_repeatMode != clamped) {
        m_repeatMode = clamped;
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("set_repeat_mode"), static_cast<double>(clamped));
}

void BridgeClient::setShuffleEnabled(bool value) {
    if (m_shuffleEnabled != value) {
        m_shuffleEnabled = value;
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("set_shuffle"), value ? 1.0 : 0.0);
}

void BridgeClient::setShowFps(bool value) {
    if (m_showFps != value) {
        m_showFps = value;
        scheduleSnapshotChanged();
    }
    sendCommand(QStringLiteral("set_show_fps"), value ? 1.0 : 0.0);
}

void BridgeClient::playAt(int index) {
    if (index < 0) {
        return;
    }
    if (m_selectedQueueIndex != index) {
        m_selectedQueueIndex = index;
        emit snapshotChanged();
    }
    m_pendingQueueSelection = index;
    m_pendingQueueSelectionUntilMs = QDateTime::currentMSecsSinceEpoch() + 700;
    sendCommand(QStringLiteral("play_at"), static_cast<double>(index));
}

void BridgeClient::selectQueueIndex(int index) {
    if (index < 0) {
        return;
    }
    if (m_selectedQueueIndex != index) {
        m_selectedQueueIndex = index;
        emit snapshotChanged();
    }
    m_pendingQueueSelection = index;
    m_pendingQueueSelectionUntilMs = QDateTime::currentMSecsSinceEpoch() + 700;
    sendCommand(QStringLiteral("select_queue"), static_cast<double>(index));
}

void BridgeClient::removeAt(int index) {
    if (index < 0) {
        return;
    }
    sendCommand(QStringLiteral("remove_at"), static_cast<double>(index));
}

void BridgeClient::moveQueue(int from, int to) {
    if (from < 0 || to < 0) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("move_queue"));
    obj.insert(QStringLiteral("from"), from);
    obj.insert(QStringLiteral("to"), to);
    sendJson(obj);
}

void BridgeClient::clearQueue() {
    sendCommand(QStringLiteral("clear_queue"));
}

void BridgeClient::replaceAlbumAt(int index) {
    if (index < 0 || index >= m_libraryAlbumTrackPaths.size()) {
        return;
    }
    replaceWithPaths(m_libraryAlbumTrackPaths[index]);
}

void BridgeClient::appendAlbumAt(int index) {
    if (index < 0 || index >= m_libraryAlbumTrackPaths.size()) {
        return;
    }
    appendPaths(m_libraryAlbumTrackPaths[index]);
}

void BridgeClient::playTrack(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("play_track"));
    obj.insert(QStringLiteral("path"), path);
    sendJson(obj);
}

void BridgeClient::appendTrack(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("add_track"));
    obj.insert(QStringLiteral("path"), path);
    sendJson(obj);
}

void BridgeClient::replaceAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("replace_album_by_key"));
    obj.insert(QStringLiteral("artist"), artist);
    obj.insert(QStringLiteral("album"), album);
    sendJson(obj);
}

void BridgeClient::appendAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("append_album_by_key"));
    obj.insert(QStringLiteral("artist"), artist);
    obj.insert(QStringLiteral("album"), album);
    sendJson(obj);
}

void BridgeClient::replaceArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("replace_artist_by_key"));
    obj.insert(QStringLiteral("artist"), artist);
    sendJson(obj);
}

void BridgeClient::appendArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("append_artist_by_key"));
    obj.insert(QStringLiteral("artist"), artist);
    sendJson(obj);
}

void BridgeClient::replaceWithPaths(const QStringList &paths) {
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty()) {
            sanitized.push_back(trimmed);
        }
    }
    if (sanitized.isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("replace_album"));
    obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(sanitized));
    sendJson(obj);
}

void BridgeClient::appendPaths(const QStringList &paths) {
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty()) {
            sanitized.push_back(trimmed);
        }
    }
    if (sanitized.isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("append_album"));
    obj.insert(QStringLiteral("paths"), QJsonArray::fromStringList(sanitized));
    sendJson(obj);
}

QString BridgeClient::libraryAlbumCoverAt(int index) const {
    if (index < 0 || index >= m_libraryAlbumCoverPaths.size()) {
        return {};
    }
    const QString path = m_libraryAlbumCoverPaths[index];
    if (path.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(path).toString();
}

QString BridgeClient::libraryThumbnailSource(const QString &path) const {
    if (path.isEmpty()) {
        return {};
    }

    if (path.startsWith(QStringLiteral("qrc:/")) || path.startsWith(QStringLiteral(":/"))) {
        return path;
    }

    QUrl url(path);
    QString localPath;
    if (url.isValid() && url.isLocalFile()) {
        localPath = url.toLocalFile();
    } else if (path.startsWith(QStringLiteral("file://"))) {
        localPath = QUrl(path).toLocalFile();
        url = QUrl(path);
    } else {
        localPath = path;
        url = QUrl::fromLocalFile(path);
    }

    const QFileInfo info(localPath);
    if (!info.exists()) {
        return path;
    }

    const QString canonicalPath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath()
        : info.canonicalFilePath();
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    const QString cacheKey = canonicalPath
        + QStringLiteral("|")
        + QString::number(mtimeMs);
    if (const auto it = m_libraryThumbnailSourceCache.constFind(cacheKey);
        it != m_libraryThumbnailSourceCache.constEnd())
    {
        return it.value();
    }

    QUrl coverUrl = QUrl::fromLocalFile(canonicalPath);
    QUrlQuery query(coverUrl);
    query.removeAllQueryItems(QStringLiteral("v"));
    query.addQueryItem(QStringLiteral("v"), QString::number(mtimeMs));
    coverUrl.setQuery(query);

    const QString result = coverUrl.toString(QUrl::FullyEncoded);
    m_libraryThumbnailSourceCache.insert(cacheKey, result);
    if (m_libraryThumbnailSourceCache.size() > 4096) {
        m_libraryThumbnailSourceCache.clear();
        m_libraryThumbnailSourceCache.insert(cacheKey, result);
    }
    return result;
}

QString BridgeClient::queuePathAt(int index) const {
    if (index < 0 || index >= m_queuePaths.size()) {
        return {};
    }
    return m_queuePaths[index];
}

void BridgeClient::addLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    m_pendingAddRootPath = normalized;
    m_pendingAddRootCommand = m_addRootCommand;
    m_pendingAddRootAttempts = 1;
    m_pendingAddRootIssuedMs = QDateTime::currentMSecsSinceEpoch();
    sendLibraryRootCommand(m_pendingAddRootCommand, normalized);
}

void BridgeClient::removeLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("remove_root"));
    obj.insert(QStringLiteral("path"), normalized);
    sendJson(obj);
}

void BridgeClient::rescanLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("rescan_root"));
    obj.insert(QStringLiteral("path"), normalized);
    sendJson(obj);
}

void BridgeClient::rescanAllLibraryRoots() {
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("rescan_all"));
    sendJson(obj);
}

void BridgeClient::setLibrarySortMode(int mode) {
    const int clamped = std::clamp(mode, 0, 1);
    if (m_librarySortMode != clamped) {
        m_librarySortMode = clamped;
        scheduleSnapshotChanged();
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("set_library_sort_mode"));
    obj.insert(QStringLiteral("value"), clamped);
    sendJson(obj);
}

void BridgeClient::openInFileBrowser(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    const bool ok = openUrlInFileBrowser(path, false);
    if (!ok) {
        emit bridgeError(QStringLiteral("failed to open in %1: %2")
                             .arg(m_fileBrowserName, path));
    }
}

void BridgeClient::openContainingFolder(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    const bool ok = openUrlInFileBrowser(path, true);
    if (!ok) {
        emit bridgeError(QStringLiteral("failed to open containing folder in %1: %2")
                             .arg(m_fileBrowserName, path));
    }
}

void BridgeClient::scanRoot(const QString &path) {
    addLibraryRoot(path);
}

void BridgeClient::scanDefaultMusicRoot() {
    const QString home = QDir::homePath();
    const QString music = QDir(home).filePath(QStringLiteral("Music"));
    scanRoot(music);
}

QVariantMap BridgeClient::takeSpectrogramRowsDeltaPacked() {
    QVariantMap out;
    out.insert(QStringLiteral("rows"), m_spectrogramPackedRows);
    out.insert(QStringLiteral("bins"), m_spectrogramPackedBins);
    out.insert(QStringLiteral("data"), m_spectrogramRowsPacked);
    m_spectrogramRowsPacked.clear();
    m_spectrogramPackedRows = 0;
    return out;
}

void BridgeClient::requestSnapshot() {
    sendCommand(QStringLiteral("request_snapshot"));
}

void BridgeClient::shutdown() {
    sendCommand(QStringLiteral("shutdown"));
}

void BridgeClient::setupAnalysisSocketServer() {
    connect(&m_analysisServer, &QLocalServer::newConnection, this, &BridgeClient::handleAnalysisSocketConnected);

#ifdef Q_OS_UNIX
    const QString socketBase = QStringLiteral("ferrous-analysis-%1-%2.sock")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch());
    m_analysisSocketName = QDir::temp().filePath(socketBase);
#else
    m_analysisSocketName = QStringLiteral("ferrous-analysis-%1-%2")
                               .arg(QCoreApplication::applicationPid())
                               .arg(QDateTime::currentMSecsSinceEpoch());
#endif

    QLocalServer::removeServer(m_analysisSocketName);
    if (!m_analysisServer.listen(m_analysisSocketName)) {
        emit bridgeError(QStringLiteral("failed to listen analysis socket: %1")
                             .arg(m_analysisServer.errorString()));
        m_analysisSocketName.clear();
    }
}

void BridgeClient::handleAnalysisSocketConnected() {
    teardownAnalysisSocket(false);

    m_analysisSocket = m_analysisServer.nextPendingConnection();
    if (m_analysisSocket == nullptr) {
        return;
    }
    m_analysisBuffer.clear();
    m_analysisBufferReadOffset = 0;
    m_analysisSocketConnected = true;
    m_hasAnalysisFrameSeq = false;
    connect(m_analysisSocket, &QLocalSocket::readyRead, this, &BridgeClient::handleAnalysisSocketReady);
    QLocalSocket *socket = m_analysisSocket;
    connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
        if (m_analysisSocket == socket) {
            m_analysisSocket = nullptr;
            m_analysisSocketConnected = false;
            m_analysisBuffer.clear();
        }
        socket->deleteLater();
    });
}

void BridgeClient::handleAnalysisSocketReady() {
    if (m_analysisSocket == nullptr) {
        return;
    }
    const QByteArray chunk = m_analysisSocket->readAll();
    processAnalysisBytes(chunk);
}

void BridgeClient::processAnalysisBytes(const QByteArray &chunk) {
    if (chunk.isEmpty()) {
        return;
    }
    m_analysisBuffer += chunk;

    bool changed = false;
    qsizetype readOffset = m_analysisBufferReadOffset;
    const qsizetype totalSize = m_analysisBuffer.size();
    const auto *base = reinterpret_cast<const uchar *>(m_analysisBuffer.constData());

    while ((totalSize - readOffset) >= static_cast<qsizetype>(sizeof(quint32))) {
        const auto *lenPtr = base + readOffset;
        const quint32 frameBytes = qFromLittleEndian<quint32>(lenPtr);
        if (frameBytes == 0 || frameBytes > kMaxAnalysisFrameBytes) {
            emit bridgeError(QStringLiteral("invalid analysis frame size: %1").arg(frameBytes));
            m_analysisBuffer.clear();
            m_analysisBufferReadOffset = 0;
            break;
        }
        const qsizetype totalBytes = static_cast<qsizetype>(sizeof(quint32) + frameBytes);
        if ((totalSize - readOffset) < totalBytes) {
            break;
        }
        const auto *data = base + readOffset + sizeof(quint32);
        readOffset += totalBytes;

        if (frameBytes < 16) {
            continue;
        }
        if (data[0] != kAnalysisFrameMagic) {
            continue;
        }
        const quint32 sampleRate = qFromLittleEndian<quint32>(data + 1);
        const quint8 flags = data[5];
        const quint16 waveformLen = qFromLittleEndian<quint16>(data + 6);
        const quint16 rowCount = qFromLittleEndian<quint16>(data + 8);
        const quint16 binCount = qFromLittleEndian<quint16>(data + 10);
        const quint32 frameSeq = qFromLittleEndian<quint32>(data + 12);
        const qsizetype expected = 16 + static_cast<qsizetype>(waveformLen)
            + static_cast<qsizetype>(rowCount) * static_cast<qsizetype>(binCount);
        if (static_cast<qsizetype>(frameBytes) < expected) {
            continue;
        }
        if (m_hasAnalysisFrameSeq && !isNewerSeq(frameSeq, m_lastAnalysisFrameSeq)) {
            m_analysisDroppedFrames++;
            continue;
        }
        m_hasAnalysisFrameSeq = true;
        m_lastAnalysisFrameSeq = frameSeq;

        const uchar *cursor = data + 16;

        if (sampleRate > 0 && m_sampleRateHz != static_cast<int>(sampleRate)) {
            m_sampleRateHz = static_cast<int>(sampleRate);
            changed = true;
        }

        const bool spectrogramReset = (flags & kAnalysisFlagReset) != 0;
        if (m_spectrogramReset != spectrogramReset) {
            m_spectrogramReset = spectrogramReset;
            changed = true;
        }
        if (spectrogramReset) {
            if (m_spectrogramPackedRows > 0 || !m_spectrogramRowsPacked.isEmpty()) {
                m_spectrogramRowsPacked.clear();
                m_spectrogramPackedRows = 0;
                changed = true;
            }
            m_spectrogramPackedBins = 0;
        }

        if ((flags & kAnalysisFlagWaveform) != 0) {
            QByteArray peaks(reinterpret_cast<const char *>(cursor), waveformLen);
            cursor += waveformLen;
            if (m_waveformPeaksPacked != peaks) {
                m_waveformPeaksPacked = peaks;
                changed = true;
            }
        } else {
            cursor += waveformLen;
        }

        if ((flags & kAnalysisFlagSpectrogram) != 0 && rowCount > 0 && binCount > 0) {
            if (m_spectrogramPackedRows == 0) {
                m_spectrogramPackedBins = binCount;
            }
            if (m_spectrogramPackedBins == static_cast<int>(binCount)) {
                const qsizetype bytes = static_cast<qsizetype>(rowCount) * static_cast<qsizetype>(binCount);
                m_spectrogramRowsPacked.append(reinterpret_cast<const char *>(cursor), bytes);
                m_spectrogramPackedRows += rowCount;
                constexpr int kMaxPendingSpectrogramRows = 512;
                if (m_spectrogramPackedRows > kMaxPendingSpectrogramRows && m_spectrogramPackedBins > 0) {
                    const int dropRows = m_spectrogramPackedRows - kMaxPendingSpectrogramRows;
                    const qsizetype dropBytes = static_cast<qsizetype>(dropRows)
                        * static_cast<qsizetype>(m_spectrogramPackedBins);
                    m_spectrogramRowsPacked.remove(0, dropBytes);
                    m_spectrogramPackedRows = kMaxPendingSpectrogramRows;
                }
                if (m_spectrogramPackedRows > 0) {
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        scheduleAnalysisChanged();
    }

    if (m_analysisBuffer.isEmpty()) {
        m_analysisBufferReadOffset = 0;
        return;
    }
    if (readOffset >= m_analysisBuffer.size()) {
        m_analysisBuffer.clear();
        m_analysisBufferReadOffset = 0;
        return;
    }

    // Avoid front-removing on every frame; compact periodically.
    if (readOffset > (64 * 1024) || readOffset > (m_analysisBuffer.size() / 2)) {
        m_analysisBuffer.remove(0, readOffset);
        m_analysisBufferReadOffset = 0;
    } else {
        m_analysisBufferReadOffset = readOffset;
    }
}

void BridgeClient::scheduleSnapshotChanged() {
    m_snapshotChangedPending = true;
    if (!m_snapshotNotifyTimer.isActive()) {
        m_snapshotNotifyTimer.start();
    }
}

void BridgeClient::scheduleAnalysisChanged() {
    m_analysisChangedPending = true;
    if (!m_analysisNotifyTimer.isActive()) {
        m_analysisNotifyTimer.start();
    }
}

QString BridgeClient::detectFileBrowserName() {
    auto fromDesktopId = [](const QString &desktopId) -> QString {
        const QString lowered = desktopId.trimmed().toLower();
        if (lowered.contains(QStringLiteral("dolphin"))) {
            return QStringLiteral("Dolphin");
        }
        if (lowered.contains(QStringLiteral("nautilus"))
            || lowered.contains(QStringLiteral("org.gnome.files")))
        {
            return QStringLiteral("Files");
        }
        if (lowered.contains(QStringLiteral("thunar"))) {
            return QStringLiteral("Thunar");
        }
        if (lowered.contains(QStringLiteral("nemo"))) {
            return QStringLiteral("Nemo");
        }
        if (lowered.contains(QStringLiteral("pcmanfm"))) {
            return QStringLiteral("PCManFM");
        }
        if (!lowered.isEmpty()) {
            QString base = lowered;
            if (base.endsWith(QStringLiteral(".desktop"))) {
                base.chop(8);
            }
            const int slash = base.lastIndexOf('/');
            if (slash >= 0 && slash + 1 < base.size()) {
                base = base.mid(slash + 1);
            }
            if (base.startsWith(QStringLiteral("org.kde."))) {
                base = base.mid(QStringLiteral("org.kde.").size());
            } else if (base.startsWith(QStringLiteral("org.gnome."))) {
                base = base.mid(QStringLiteral("org.gnome.").size());
            }
            if (!base.isEmpty()) {
                base[0] = base[0].toUpper();
                return base;
            }
        }
        return QString{};
    };

    QProcess proc;
    proc.start(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("inode/directory")});
    if (proc.waitForFinished(250)) {
        const QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        const QString detected = fromDesktopId(output);
        if (!detected.isEmpty()) {
            return detected;
        }
    }

    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (desktop.contains(QStringLiteral("kde"))) {
        return QStringLiteral("Dolphin");
    }

    return QStringLiteral("File Manager");
}

bool BridgeClient::openUrlInFileBrowser(const QString &path, bool containingFolder) const {
    if (path.trimmed().isEmpty()) {
        return false;
    }

    QString localPath = path.trimmed();
    const QUrl maybeUrl(localPath);
    if (maybeUrl.isValid() && maybeUrl.isLocalFile()) {
        localPath = maybeUrl.toLocalFile();
    }

    QFileInfo info(localPath);
    QString targetPath;
    if (containingFolder) {
        targetPath = info.absolutePath();
    } else if (info.isFile()) {
        targetPath = info.absolutePath();
    } else {
        targetPath = info.absoluteFilePath();
    }

    if (targetPath.isEmpty()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
}

void BridgeClient::startBridgeProcess() {
    QString command = qEnvironmentVariable("FERROUS_BRIDGE_CMD");
    if (command.isEmpty()) {
        // Prefer a prebuilt bridge binary for lower overhead and predictable runtime memory.
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QFileInfo appBinaryInfo(QCoreApplication::applicationFilePath());
        const QDateTime appBuiltAt = appBinaryInfo.lastModified();
        const QStringList candidates{
            appDir.absoluteFilePath(QStringLiteral("../../target/release/native_frontend")),
            QDir::current().absoluteFilePath(QStringLiteral("target/release/native_frontend")),
        };

        for (const QString &candidate : candidates) {
            const QFileInfo info(candidate);
            if (info.exists() && info.isFile() && info.isExecutable()) {
                // Avoid stale bridge binaries from older builds when the UI executable is newer.
                if (appBuiltAt.isValid() && info.lastModified().isValid()
                    && info.lastModified() < appBuiltAt) {
                    continue;
                }
                command = QStringLiteral("\"%1\" --json-bridge").arg(info.absoluteFilePath());
                break;
            }
        }

        if (command.isEmpty()) {
            command =
                QStringLiteral("cargo run --release --bin native_frontend --features gst -- --json-bridge");
        }
    }

    const QString shell = QStringLiteral("/bin/sh");
    const QStringList args{QStringLiteral("-lc"), command};
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_analysisSocketName.isEmpty()) {
        env.insert(QStringLiteral("FERROUS_ANALYSIS_SOCKET_PATH"), m_analysisSocketName);
    }
    m_process.setProcessEnvironment(env);
    m_process.start(shell, args);
}

void BridgeClient::sendLibraryRootCommand(const QString &cmd, const QString &path) {
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    obj.insert(QStringLiteral("path"), path);
    sendJson(obj);
}

void BridgeClient::sendCommand(const QString &cmd) {
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    sendJson(obj);
}

void BridgeClient::sendCommand(const QString &cmd, double value) {
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    obj.insert(QStringLiteral("value"), value);
    sendJson(obj);
}

void BridgeClient::sendJson(const QJsonObject &obj) {
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    if (m_useInProcessBridge && m_ffiBridge != nullptr) {
        if (!ferrous_ffi_bridge_send_json(m_ffiBridge, payload.constData())) {
            emit bridgeError(QStringLiteral("failed to send command to in-process bridge"));
        }
        return;
    }
    if (m_process.state() != QProcess::Running) {
        emit bridgeError(QStringLiteral("bridge process is not running"));
        return;
    }
    m_process.write(payload);
}

bool BridgeClient::processBridgeJsonObject(const QJsonObject &root) {
    const QString event = root.value(QStringLiteral("event")).toString();
    if (event == QStringLiteral("snapshot")) {
        const QJsonObject playback = root.value(QStringLiteral("playback")).toObject();
        const QJsonObject queue = root.value(QStringLiteral("queue")).toObject();
        const QJsonObject library = root.value(QStringLiteral("library")).toObject();
        const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
        const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
        const QJsonObject analysis = root.value(QStringLiteral("analysis")).toObject();

        const QString nextState = playback.value(QStringLiteral("state")).toString();
        const double pos = playback.value(QStringLiteral("position_secs")).toDouble();
        const double dur = playback.value(QStringLiteral("duration_secs")).toDouble();
        const int repeatMode = std::clamp(playback.value(QStringLiteral("repeat_mode")).toInt(m_repeatMode), 0, 2);
        const bool shuffleEnabled =
            playback.value(QStringLiteral("shuffle_enabled")).toBool(m_shuffleEnabled);
        const QString currentPath = playback.value(QStringLiteral("current_path")).toString();
        int playing = playback.value(QStringLiteral("current_queue_index")).toInt(-1);
        const int qlen = queue.value(QStringLiteral("len")).toInt();
        const double queueDurationSecs =
            queue.value(QStringLiteral("total_duration_secs")).toDouble(0.0);
        const int queueUnknownDurationCount =
            std::max(0, queue.value(QStringLiteral("unknown_duration_count")).toInt(0));
        const int selected = queue.value(QStringLiteral("selected_index")).toInt(-1);
        const int sampleRate = analysis.value(QStringLiteral("sample_rate_hz")).toInt(m_sampleRateHz);
        const QString metadataSourcePath = metadata.value(QStringLiteral("source_path")).toString();
        const QString metadataCoverPath = metadata.value(QStringLiteral("cover_path")).toString();
        QString metadataCoverUrl;
        if (!metadataCoverPath.trimmed().isEmpty() && metadataSourcePath == currentPath) {
            const QUrl maybeUrl(metadataCoverPath);
            if (maybeUrl.isValid() && maybeUrl.isLocalFile()) {
                metadataCoverUrl = maybeUrl.toString();
            } else {
                metadataCoverUrl = QUrl::fromLocalFile(metadataCoverPath).toString();
            }
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        bool changed = false;
        bool analysisOnlyChanged = false;
        if (m_playbackState != nextState) {
            m_playbackState = nextState;
            changed = true;
        }
        bool applyIncomingPosition = true;
        if (m_pendingSeek) {
            if (nowMs >= m_pendingSeekUntilMs) {
                m_pendingSeek = false;
            } else if (std::abs(pos - m_pendingSeekTargetSeconds) <= 0.8) {
                m_pendingSeek = false;
            } else {
                applyIncomingPosition = false;
            }
        }
        if (applyIncomingPosition) {
            const QString posText = formatSeconds(pos);
            if (m_positionText != posText) {
                m_positionText = posText;
                changed = true;
            }
            if (std::abs(m_positionSeconds - pos) >= 0.03) {
                m_positionSeconds = pos;
                changed = true;
            }
        }
        const QString durText = formatSeconds(dur);
        if (m_durationText != durText) {
            m_durationText = durText;
            changed = true;
        }
        if (!qFuzzyCompare(m_durationSeconds + 1.0, dur + 1.0)) {
            m_durationSeconds = dur;
            changed = true;
        }
        if (m_repeatMode != repeatMode) {
            m_repeatMode = repeatMode;
            changed = true;
        }
        if (m_shuffleEnabled != shuffleEnabled) {
            m_shuffleEnabled = shuffleEnabled;
            changed = true;
        }
        const QJsonValue settingsVolumeValue = settings.value(QStringLiteral("volume"));
        const double uiVol = settingsVolumeValue.isDouble()
            ? settingsVolumeValue.toDouble()
            : m_volume;
        if (std::abs(m_volume - uiVol) > 0.0005) {
            m_volume = uiVol;
            changed = true;
        }
        if (m_queueLength != qlen) {
            m_queueLength = qlen;
            changed = true;
            if (m_pendingQueueSelection >= qlen) {
                m_pendingQueueSelection = -1;
                m_pendingQueueSelectionUntilMs = 0;
            }
        }
        QString nextQueueDurationText = formatSeconds(queueDurationSecs);
        if (queueUnknownDurationCount > 0) {
            nextQueueDurationText = QStringLiteral("%1+?").arg(nextQueueDurationText);
        }
        if (m_queueDurationText != nextQueueDurationText) {
            m_queueDurationText = nextQueueDurationText;
            changed = true;
        }
        const QJsonValue queueTracksValue = queue.value(QStringLiteral("tracks"));
        if (queueTracksValue.isArray()) {
            const QJsonArray queueTracks = queueTracksValue.toArray();
            QStringList items;
            QStringList paths;
            items.reserve(queueTracks.size());
            paths.reserve(queueTracks.size());
            for (const QJsonValue &track : queueTracks) {
                const QJsonObject obj = track.toObject();
                const QString title = obj.value(QStringLiteral("title")).toString();
                const QString path = obj.value(QStringLiteral("path")).toString();
                paths.push_back(path);
                items.push_back(title.isEmpty() ? path : title);
            }
            if (m_queueItems != items) {
                m_queueItems = items;
                changed = true;
            }
            if (m_queuePaths != paths) {
                m_queuePaths = paths;
                changed = true;
            }
        }
        if (m_pendingQueueSelection >= 0) {
            if (selected == m_pendingQueueSelection) {
                m_pendingQueueSelection = -1;
                m_pendingQueueSelectionUntilMs = 0;
                if (m_selectedQueueIndex != selected) {
                    m_selectedQueueIndex = selected;
                    changed = true;
                }
            } else if (nowMs >= m_pendingQueueSelectionUntilMs) {
                m_pendingQueueSelection = -1;
                m_pendingQueueSelectionUntilMs = 0;
                if (m_selectedQueueIndex != selected) {
                    m_selectedQueueIndex = selected;
                    changed = true;
                }
            }
        } else if (m_selectedQueueIndex != selected) {
            m_selectedQueueIndex = selected;
            changed = true;
        }
        if (m_currentTrackPath != currentPath) {
            m_currentTrackPath = currentPath;
            changed = true;
        }
        if (nextState == QStringLiteral("Stopped")) {
            playing = -1;
        } else if (playing < 0 && !currentPath.isEmpty() && !m_queuePaths.isEmpty()) {
            playing = m_queuePaths.indexOf(currentPath);
        }
        if (m_playingQueueIndex != playing) {
            m_playingQueueIndex = playing;
            changed = true;
        }
        QString currentCover = metadataCoverUrl;
        if (currentCover.isEmpty() && !currentPath.isEmpty()) {
            const auto cached = m_trackCoverByPath.constFind(currentPath);
            if (cached != m_trackCoverByPath.constEnd()) {
                currentCover = cached.value();
            } else {
                currentCover = findTrackCoverUrl(currentPath);
                m_trackCoverByPath.insert(currentPath, currentCover);
                if (m_trackCoverByPath.size() > 4096) {
                    m_trackCoverByPath.clear();
                    m_trackCoverByPath.insert(currentPath, currentCover);
                }
            }
        }
        if (m_currentTrackCoverPath != currentCover) {
            m_currentTrackCoverPath = currentCover;
            changed = true;
        }
        if (!m_analysisSocketConnected) {
            const bool spectrogramReset = analysis.value(QStringLiteral("spectrogram_reset")).toBool();
            if (m_spectrogramReset != spectrogramReset) {
                m_spectrogramReset = spectrogramReset;
                analysisOnlyChanged = true;
            }
            if (spectrogramReset) {
                if (m_spectrogramPackedRows > 0 || !m_spectrogramRowsPacked.isEmpty()) {
                    m_spectrogramRowsPacked.clear();
                    m_spectrogramPackedRows = 0;
                    analysisOnlyChanged = true;
                }
                m_spectrogramPackedBins = 0;
            }
            const QJsonValue spectrogramRowsValue = analysis.value(QStringLiteral("spectrogram_rows"));
            if (spectrogramRowsValue.isArray()) {
                const QJsonArray rowsArr = spectrogramRowsValue.toArray();
                int rowsAdded = 0;
                int bins = m_spectrogramPackedBins;
                for (const QJsonValue &rowValue : rowsArr) {
                    const QJsonArray rowArr = rowValue.toArray();
                    if (rowArr.isEmpty()) {
                        continue;
                    }
                    if (bins == 0) {
                        bins = rowArr.size();
                        m_spectrogramPackedBins = bins;
                    }
                    if (rowArr.size() != bins) {
                        continue;
                    }
                    QByteArray packedRow;
                    packedRow.resize(bins);
                    for (qsizetype i = 0; i < rowArr.size(); ++i) {
                        const double raw = rowArr[static_cast<int>(i)].toDouble();
                        const int u8 = std::clamp<int>(static_cast<int>(std::lround(raw)), 0, 255);
                        packedRow[i] = static_cast<char>(u8);
                    }
                    m_spectrogramRowsPacked.append(packedRow);
                    rowsAdded++;
                }
                if (rowsAdded > 0) {
                    m_spectrogramPackedRows += rowsAdded;
                    constexpr int kMaxPendingSpectrogramRows = 512;
                    if (m_spectrogramPackedRows > kMaxPendingSpectrogramRows
                        && m_spectrogramPackedBins > 0) {
                        const int dropRows = m_spectrogramPackedRows - kMaxPendingSpectrogramRows;
                        const qsizetype dropBytes = static_cast<qsizetype>(dropRows)
                            * static_cast<qsizetype>(m_spectrogramPackedBins);
                        m_spectrogramRowsPacked.remove(0, dropBytes);
                        m_spectrogramPackedRows = kMaxPendingSpectrogramRows;
                    }
                    analysisOnlyChanged = true;
                }
            }
            if (m_sampleRateHz != sampleRate) {
                m_sampleRateHz = sampleRate;
                analysisOnlyChanged = true;
            }
        }
        const double dbRange = settings.value(QStringLiteral("db_range")).toDouble(m_dbRange);
        if (!qFuzzyCompare(m_dbRange + 1.0, dbRange + 1.0)) {
            m_dbRange = dbRange;
            changed = true;
        }
        const bool logScale = settings.value(QStringLiteral("log_scale")).toBool(m_logScale);
        if (m_logScale != logScale) {
            m_logScale = logScale;
            changed = true;
        }
        const bool showFps = settings.value(QStringLiteral("show_fps")).toBool(m_showFps);
        if (m_showFps != showFps) {
            m_showFps = showFps;
            changed = true;
        }
        const int settingsSortMode = std::clamp(
            settings.value(QStringLiteral("library_sort_mode")).toInt(m_librarySortMode),
            0,
            1);
        if (m_librarySortMode != settingsSortMode) {
            m_librarySortMode = settingsSortMode;
            changed = true;
        }
        const bool scanInProgress = library.value(QStringLiteral("scan_in_progress")).toBool();
        if (m_libraryScanInProgress != scanInProgress) {
            m_libraryScanInProgress = scanInProgress;
            changed = true;
        }
        const int roots = library.value(QStringLiteral("roots")).toInt(m_libraryRootCount);
        if (m_libraryRootCount != roots) {
            m_libraryRootCount = roots;
            changed = true;
        }
        const int tracks = library.value(QStringLiteral("tracks")).toInt(m_libraryTrackCount);
        if (m_libraryTrackCount != tracks) {
            m_libraryTrackCount = tracks;
            changed = true;
        }
        QStringList rootPaths;
        const QJsonValue rootPathsValue = library.value(QStringLiteral("root_paths"));
        if (rootPathsValue.isArray()) {
            for (const QJsonValue &rootValue : rootPathsValue.toArray()) {
                const QString path = rootValue.toString();
                if (!path.isEmpty()) {
                    rootPaths.push_back(path);
                }
            }
        }
        if (m_libraryRoots != rootPaths) {
            m_libraryRoots = rootPaths;
            changed = true;
        }
        const QString libraryLastError = library.value(QStringLiteral("last_error")).toString();
        if (m_libraryLastError != libraryLastError) {
            m_libraryLastError = libraryLastError;
            if (!m_libraryLastError.trimmed().isEmpty()) {
                emit bridgeError(QStringLiteral("library: %1").arg(m_libraryLastError));
            }
        }
        if (!m_pendingAddRootPath.isEmpty()) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const bool fresh = m_pendingAddRootIssuedMs > 0 && (nowMs - m_pendingAddRootIssuedMs) <= 10000;
            const bool rootAppeared = rootPaths.contains(m_pendingAddRootPath);
            if (!fresh || rootAppeared || m_libraryScanInProgress) {
                m_pendingAddRootPath.clear();
                m_pendingAddRootCommand.clear();
                m_pendingAddRootAttempts = 0;
                m_pendingAddRootIssuedMs = 0;
            }
        }

        const int sortMode = std::clamp(
            library.value(QStringLiteral("sort_mode")).toInt(m_librarySortMode),
            0,
            1);
        if (m_librarySortMode != sortMode) {
            m_librarySortMode = sortMode;
            changed = true;
        }

        const QJsonObject scanProgress = library.value(QStringLiteral("progress")).toObject();
        const int rootsCompleted =
            std::max(0, scanProgress.value(QStringLiteral("roots_completed")).toInt(0));
        const int rootsTotal =
            std::max(0, scanProgress.value(QStringLiteral("roots_total")).toInt(0));
        const int discovered =
            std::max(0, scanProgress.value(QStringLiteral("supported_files_discovered")).toInt(0));
        const int processed =
            std::max(0, scanProgress.value(QStringLiteral("supported_files_processed")).toInt(0));
        const double filesPerSecond = scanProgress.value(QStringLiteral("files_per_second")).toDouble(0.0);
        const double etaSeconds = scanProgress.value(QStringLiteral("eta_seconds")).isDouble()
            ? scanProgress.value(QStringLiteral("eta_seconds")).toDouble(-1.0)
            : -1.0;
        if (m_libraryScanRootsCompleted != rootsCompleted) {
            m_libraryScanRootsCompleted = rootsCompleted;
            changed = true;
        }
        if (m_libraryScanRootsTotal != rootsTotal) {
            m_libraryScanRootsTotal = rootsTotal;
            changed = true;
        }
        if (m_libraryScanDiscovered != discovered) {
            m_libraryScanDiscovered = discovered;
            changed = true;
        }
        if (m_libraryScanProcessed != processed) {
            m_libraryScanProcessed = processed;
            changed = true;
        }
        if (!qFuzzyCompare(m_libraryScanFilesPerSecond + 1.0, filesPerSecond + 1.0)) {
            m_libraryScanFilesPerSecond = filesPerSecond;
            changed = true;
        }
        if (!qFuzzyCompare(m_libraryScanEtaSeconds + 2.0, etaSeconds + 2.0)) {
            m_libraryScanEtaSeconds = etaSeconds;
            changed = true;
        }

        const QJsonValue treeValue = library.value(QStringLiteral("tree"));
        if (treeValue.isArray()) {
            const QVariantList tree = treeValue.toArray().toVariantList();
            // The backend only includes a tree payload when it decides the tree changed.
            // Avoid deep QVariantList equality checks here: large trees make that comparison expensive.
            m_libraryTree = tree;
            changed = true;
            const bool libraryStructureChanged = true;
            if (libraryStructureChanged) {
                m_libraryAlbums.clear();
                m_libraryAlbumArtists.clear();
                m_libraryAlbumNames.clear();
                m_libraryAlbumCoverPaths.clear();
                m_libraryAlbumTrackPaths.clear();
                m_trackCoverByPath.clear();
                if (!m_currentTrackPath.isEmpty()) {
                    const QString refreshedCover = findTrackCoverUrl(m_currentTrackPath);
                    m_trackCoverByPath.insert(m_currentTrackPath, refreshedCover);
                    if (m_currentTrackCoverPath != refreshedCover) {
                        m_currentTrackCoverPath = refreshedCover;
                        changed = true;
                    }
                }
                m_libraryVersion = m_libraryVersion < std::numeric_limits<int>::max()
                    ? m_libraryVersion + 1
                    : 1;
            }
        }
        if (!m_analysisSocketConnected) {
            const QJsonValue waveformValue = analysis.value(QStringLiteral("waveform_peaks"));
            if (waveformValue.isArray()) {
                const QJsonArray arr = waveformValue.toArray();
                QByteArray peaks;
                peaks.resize(arr.size());
                qsizetype i = 0;
                for (const QJsonValue &v : arr) {
                    const double f = std::clamp(v.toDouble(), 0.0, 1.0);
                    const int u8 = std::clamp<int>(static_cast<int>(std::lround(f * 255.0)), 0, 255);
                    peaks[i++] = static_cast<char>(u8);
                }
                if (m_waveformPeaksPacked != peaks) {
                    m_waveformPeaksPacked = peaks;
                    analysisOnlyChanged = true;
                }
            }
        }
        if (analysisOnlyChanged) {
            scheduleAnalysisChanged();
        }
        return changed;
    }
    if (event == QStringLiteral("error")) {
        const QString message = root.value(QStringLiteral("message")).toString();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool pendingFresh = !m_pendingAddRootPath.isEmpty()
            && m_pendingAddRootIssuedMs > 0
            && (nowMs - m_pendingAddRootIssuedMs) <= 3000;
        if (pendingFresh && m_pendingAddRootAttempts == 1) {
            if (m_pendingAddRootCommand == QStringLiteral("add_root")
                && message.contains(QStringLiteral("unknown command 'add_root'")))
            {
                m_addRootCommand = QStringLiteral("scan_root");
                m_pendingAddRootCommand = m_addRootCommand;
                m_pendingAddRootAttempts = 2;
                m_pendingAddRootIssuedMs = nowMs;
                sendLibraryRootCommand(m_pendingAddRootCommand, m_pendingAddRootPath);
                return false;
            }
            if (m_pendingAddRootCommand == QStringLiteral("scan_root")
                && message.contains(QStringLiteral("unknown command 'scan_root'")))
            {
                m_addRootCommand = QStringLiteral("add_root");
                m_pendingAddRootCommand = m_addRootCommand;
                m_pendingAddRootAttempts = 2;
                m_pendingAddRootIssuedMs = nowMs;
                sendLibraryRootCommand(m_pendingAddRootCommand, m_pendingAddRootPath);
                return false;
            }
        }
        emit bridgeError(message);
        return false;
    }
    if (event == QStringLiteral("stopped")) {
        if (m_connected) {
            m_connected = false;
            emit connectedChanged();
        }
        return false;
    }
    return false;
}

void BridgeClient::handleStdoutReady() {
    m_stdoutPumpScheduled = false;
    bool anySnapshotChanged = false;
    int processedLines = 0;
    constexpr int kMaxLinesPerPass = 256;

    auto processRoot = [&](const QJsonObject &root) { anySnapshotChanged |= processBridgeJsonObject(root); };

    while (m_process.canReadLine() && processedLines < kMaxLinesPerPass) {
        processedLines++;
        const QByteArray line = m_process.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit bridgeError(QStringLiteral("invalid bridge json: %1").arg(QString::fromUtf8(line)));
            continue;
        }
        processRoot(doc.object());
    }
    if (anySnapshotChanged) {
        scheduleSnapshotChanged();
    }
    if (m_process.canReadLine() && !m_stdoutPumpScheduled) {
        m_stdoutPumpScheduled = true;
        QMetaObject::invokeMethod(this, [this]() { handleStdoutReady(); }, Qt::QueuedConnection);
    }
}

void BridgeClient::handleStderrReady() {
    const QByteArray chunk = m_process.readAllStandardError();
    if (chunk.isEmpty()) {
        return;
    }
    m_stderrBuffer += chunk;
    for (;;) {
        const qsizetype newline = m_stderrBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QByteArray rawLine = m_stderrBuffer.left(newline);
        m_stderrBuffer.remove(0, newline + 1);
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        // Keep high-frequency profiling output out of QML signal path to avoid UI stalls.
        if (line.contains(QStringLiteral("[analysis]"))
            || line.contains(QStringLiteral("[gst]"))
            || line.contains(QStringLiteral("[bridge]"))
            || line.contains(QStringLiteral("[bridge-json]"))) {
            std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
            continue;
        }
        emit bridgeError(line);
    }
}

void BridgeClient::handleProcessStarted() {
    if (!m_connected) {
        m_connected = true;
        emit connectedChanged();
    }
    requestSnapshot();
}

void BridgeClient::handleProcessFinished() {
    teardownAnalysisSocket(false);
    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }
}

QString BridgeClient::formatSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int minutes = total / 60;
    const int secs = total % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}
