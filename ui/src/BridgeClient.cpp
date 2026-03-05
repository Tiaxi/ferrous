#include "BridgeClient.h"

#include "DiagnosticsLog.h"
#include "FerrousBridgeFfi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

namespace {
constexpr quint8 kAnalysisFrameMagic = 0xA1;
constexpr quint8 kAnalysisFlagWaveform = 0x01;
constexpr quint8 kAnalysisFlagReset = 0x02;
constexpr quint8 kAnalysisFlagSpectrogram = 0x04;
constexpr quint32 kMaxAnalysisFrameBytes = 8 * 1024 * 1024;
constexpr int kMaxDiagnosticsLines = 2000;

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

        const QFileInfoList files = dir.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::Name);
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

QString playbackStateText(int state, const QString &fallback) {
    switch (state) {
    case 0:
        return QStringLiteral("Stopped");
    case 1:
        return QStringLiteral("Playing");
    case 2:
        return QStringLiteral("Paused");
    default:
        return fallback;
    }
}

} // namespace

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent) {
    m_fileBrowserName = detectFileBrowserName();
    m_diagnosticsLogPath = resolveDiagnosticsLogPath();
    reloadDiagnosticsFromDisk();
    logDiagnostic(QStringLiteral("ui"), QStringLiteral("BridgeClient started"));

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

    m_globalSearchDebounceTimer.setSingleShot(true);
    m_globalSearchDebounceTimer.setInterval(readEnvMillis("FERROUS_UI_SEARCH_DEBOUNCE_MS", 120));
    connect(&m_globalSearchDebounceTimer, &QTimer::timeout, this, &BridgeClient::flushGlobalSearchQuery);

    m_bridgePollTimer.setInterval(readEnvMillis("FERROUS_UI_BRIDGE_POLL_MS", 16));
    connect(&m_bridgePollTimer, &QTimer::timeout, this, &BridgeClient::pollInProcessBridge);

    startInProcessBridge();
}

BridgeClient::~BridgeClient() {
    m_bridgePollTimer.stop();
    m_analysisNotifyTimer.stop();
    m_globalSearchDebounceTimer.stop();
    if (m_ffiBridge != nullptr) {
        ferrous_ffi_bridge_destroy(m_ffiBridge);
        m_ffiBridge = nullptr;
    }
}

bool BridgeClient::startInProcessBridge() {
    m_ffiBridge = ferrous_ffi_bridge_create();
    if (m_ffiBridge == nullptr) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("failed to create in-process bridge"));
        emit bridgeError(QStringLiteral("failed to create in-process Rust bridge"));
        return false;
    }

    m_bridgePollTimer.start();
    if (!m_connected) {
        m_connected = true;
        emit connectedChanged();
    }
    logDiagnostic(QStringLiteral("bridge"), QStringLiteral("in-process bridge created"));
    requestSnapshot();
    return true;
}

void BridgeClient::pollInProcessBridge() {
    if (m_ffiBridge == nullptr) {
        return;
    }
    ferrous_ffi_bridge_poll(m_ffiBridge, 64);

    bool anySnapshotChanged = false;
    int processedAnalysisFrames = 0;
    constexpr int kMaxAnalysisFramesPerPass = 8;
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

    int processedTreeFrames = 0;
    constexpr int kMaxTreeFramesPerPass = 4;
    while (processedTreeFrames < kMaxTreeFramesPerPass) {
        std::size_t len = 0;
        std::uint32_t version = 0;
        std::uint8_t *treePtr = ferrous_ffi_bridge_pop_library_tree(m_ffiBridge, &len, &version);
        if (treePtr == nullptr || len == 0) {
            break;
        }
        processedTreeFrames++;
        const QByteArray treeBytes(
            reinterpret_cast<const char *>(treePtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_library_tree(treePtr, len);
        const int versionInt = version > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(version);
        applyLibraryTreeFrame(versionInt, treeBytes);
    }

    bool anySearchChanged = false;
    int processedSearchFrames = 0;
    constexpr int kMaxSearchFramesPerPass = 4;
    while (processedSearchFrames < kMaxSearchFramesPerPass) {
        std::size_t len = 0;
        std::uint32_t seq = 0;
        std::uint8_t *searchPtr = ferrous_ffi_bridge_pop_search_results(
            m_ffiBridge,
            &len,
            &seq);
        if (searchPtr == nullptr || len == 0) {
            break;
        }
        processedSearchFrames++;
        const QByteArray payload(
            reinterpret_cast<const char *>(searchPtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_search_results(searchPtr, len);

        BinaryBridgeCodec::DecodedSearchResults decoded;
        QString decodeError;
        if (!BinaryBridgeCodec::decodeSearchResultsFrame(payload, &decoded, &decodeError)) {
            logDiagnostic(
                QStringLiteral("search"),
                QStringLiteral("decode error: %1").arg(decodeError));
            emit bridgeError(QStringLiteral("invalid search frame: %1").arg(decodeError));
            continue;
        }
        if (seq != 0) {
            decoded.seq = seq;
        }
        anySearchChanged |= processSearchResultsFrame(decoded);
    }

    int processedEvents = 0;
    constexpr int kMaxEventsPerPass = 3;
    while (processedEvents < kMaxEventsPerPass) {
        std::size_t len = 0;
        std::uint8_t *packetPtr = ferrous_ffi_bridge_pop_binary_event(m_ffiBridge, &len);
        if (packetPtr == nullptr || len == 0) {
            break;
        }
        processedEvents++;
        const QByteArray packet(
            reinterpret_cast<const char *>(packetPtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_binary_event(packetPtr, len);

        BinaryBridgeCodec::DecodedSnapshot decoded;
        QString decodeError;
        if (!BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &decodeError)) {
            logDiagnostic(
                QStringLiteral("bridge"),
                QStringLiteral("snapshot decode error: %1").arg(decodeError));
            emit bridgeError(QStringLiteral("invalid bridge packet: %1").arg(decodeError));
            continue;
        }
        anySnapshotChanged |= processBinarySnapshot(decoded);
    }

    if (anySnapshotChanged) {
        scheduleSnapshotChanged();
    }
    if (anySearchChanged) {
        emit globalSearchResultsChanged();
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

int BridgeClient::queueVersion() const {
    return m_queueVersion;
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

QByteArray BridgeClient::libraryTreeBinary() const {
    return m_libraryTreeBinary;
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

int BridgeClient::libraryArtistCount() const {
    return m_libraryArtistCount;
}

int BridgeClient::libraryAlbumCount() const {
    return m_libraryAlbumCount;
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

QVariantList BridgeClient::globalSearchArtistResults() const {
    return m_globalSearchArtistResults;
}

QVariantList BridgeClient::globalSearchAlbumResults() const {
    return m_globalSearchAlbumResults;
}

QVariantList BridgeClient::globalSearchTrackResults() const {
    return m_globalSearchTrackResults;
}

quint32 BridgeClient::globalSearchSeq() const {
    return m_globalSearchSeq;
}

QString BridgeClient::diagnosticsText() const {
    return m_diagnosticsText;
}

QString BridgeClient::diagnosticsLogPath() const {
    return m_diagnosticsLogPath;
}

bool BridgeClient::connected() const {
    return m_connected;
}

void BridgeClient::play() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPlay));
}

void BridgeClient::pause() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPause));
}

void BridgeClient::stop() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdStop));
}

void BridgeClient::next() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdNext));
}

void BridgeClient::previous() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPrevious));
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF64(BinaryBridgeCodec::CmdSeek, target));
}

void BridgeClient::setVolume(double value) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF64(
        BinaryBridgeCodec::CmdSetVolume,
        std::clamp(value, 0.0, 1.0)));
}

void BridgeClient::setDbRange(double value) {
    const double clamped = std::clamp(value, 50.0, 120.0);
    if (!qFuzzyCompare(m_dbRange + 1.0, clamped + 1.0)) {
        m_dbRange = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF32(
        BinaryBridgeCodec::CmdSetDbRange,
        static_cast<float>(clamped)));
}

void BridgeClient::setLogScale(bool value) {
    if (m_logScale != value) {
        m_logScale = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetLogScale,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setRepeatMode(int mode) {
    const int clamped = std::clamp(mode, 0, 2);
    if (m_repeatMode != clamped) {
        m_repeatMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetRepeatMode,
        static_cast<quint8>(clamped)));
}

void BridgeClient::setShuffleEnabled(bool value) {
    if (m_shuffleEnabled != value) {
        m_shuffleEnabled = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetShuffle,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setShowFps(bool value) {
    if (m_showFps != value) {
        m_showFps = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetShowFps,
        static_cast<quint8>(value ? 1 : 0)));
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU32(
        BinaryBridgeCodec::CmdPlayAt,
        static_cast<quint32>(index)));
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandI32(
        BinaryBridgeCodec::CmdSelectQueue,
        static_cast<qint32>(index)));
}

void BridgeClient::removeAt(int index) {
    if (index < 0) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU32(
        BinaryBridgeCodec::CmdRemoveAt,
        static_cast<quint32>(index)));
}

void BridgeClient::moveQueue(int from, int to) {
    if (from < 0 || to < 0) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandMoveQueue(
        static_cast<quint32>(from),
        static_cast<quint32>(to)));
}

void BridgeClient::clearQueue() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdClearQueue));
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
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(BinaryBridgeCodec::CmdPlayTrack, trimmed));
}

void BridgeClient::appendTrack(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(BinaryBridgeCodec::CmdAddTrack, trimmed));
}

void BridgeClient::replaceAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(
        BinaryBridgeCodec::CmdReplaceAlbumByKey,
        artist,
        album));
}

void BridgeClient::appendAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(
        BinaryBridgeCodec::CmdAppendAlbumByKey,
        artist,
        album));
}

void BridgeClient::replaceArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdReplaceArtistByKey,
        artist));
}

void BridgeClient::appendArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdAppendArtistByKey,
        artist));
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringList(
        BinaryBridgeCodec::CmdReplaceAlbum,
        sanitized));
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringList(
        BinaryBridgeCodec::CmdAppendAlbum,
        sanitized));
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
        it != m_libraryThumbnailSourceCache.constEnd()) {
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
    m_pendingAddRootIssuedMs = QDateTime::currentMSecsSinceEpoch();
    sendLibraryRootCommand(BinaryBridgeCodec::CmdAddRoot, normalized);
}

void BridgeClient::removeLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendLibraryRootCommand(BinaryBridgeCodec::CmdRemoveRoot, normalized);
}

void BridgeClient::rescanLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendLibraryRootCommand(BinaryBridgeCodec::CmdRescanRoot, normalized);
}

void BridgeClient::rescanAllLibraryRoots() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdRescanAll));
}

void BridgeClient::setLibraryNodeExpanded(const QString &key, bool expanded) {
    const QString normalized = key.trimmed();
    if (normalized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringBool(
        BinaryBridgeCodec::CmdSetNodeExpanded,
        normalized,
        expanded));
}

void BridgeClient::setLibrarySortMode(int mode) {
    const int clamped = std::clamp(mode, 0, 1);
    if (m_librarySortMode != clamped) {
        m_librarySortMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandI32(
        BinaryBridgeCodec::CmdSetLibrarySortMode,
        static_cast<qint32>(clamped)));
}

void BridgeClient::setGlobalSearchQuery(const QString &query) {
    const QString nextQuery = query;
    if (!m_globalSearchDebounceTimer.isActive()
        && m_pendingGlobalSearchQuery == nextQuery
        && m_lastGlobalSearchQuerySent == nextQuery) {
        return;
    }
    if (m_pendingGlobalSearchQuery == nextQuery && m_globalSearchDebounceTimer.isActive()) {
        return;
    }
    m_pendingGlobalSearchQuery = nextQuery;

    if (nextQuery.trimmed().isEmpty()) {
        bool changed = false;
        if (!m_globalSearchArtistResults.isEmpty()) {
            m_globalSearchArtistResults.clear();
            changed = true;
        }
        if (!m_globalSearchAlbumResults.isEmpty()) {
            m_globalSearchAlbumResults.clear();
            changed = true;
        }
        if (!m_globalSearchTrackResults.isEmpty()) {
            m_globalSearchTrackResults.clear();
            changed = true;
        }
        if (changed) {
            emit globalSearchResultsChanged();
        }
        logDiagnostic(QStringLiteral("search"), QStringLiteral("clear query"));
        m_globalSearchDebounceTimer.stop();
        flushGlobalSearchQuery();
        return;
    }

    m_globalSearchDebounceTimer.start();
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
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdRequestSnapshot));
}

void BridgeClient::shutdown() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdShutdown));
}

void BridgeClient::clearDiagnostics() {
    m_diagnosticsLines.clear();
    m_diagnosticsText.clear();
    if (!m_diagnosticsLogPath.isEmpty()) {
        QFile::remove(m_diagnosticsLogPath);
    }
    emit diagnosticsChanged();
    logDiagnostic(QStringLiteral("ui"), QStringLiteral("diagnostics cleared"));
}

void BridgeClient::reloadDiagnosticsFromDisk() {
    QStringList lines;
    if (!m_diagnosticsLogPath.isEmpty()) {
        QFile file(m_diagnosticsLogPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                const QString line = in.readLine();
                if (!line.isNull()) {
                    lines.push_back(line);
                }
            }
        }
    }
    if (lines.size() > kMaxDiagnosticsLines) {
        lines = lines.mid(lines.size() - kMaxDiagnosticsLines);
    }
    m_diagnosticsLines = std::move(lines);
    rebuildDiagnosticsText();
    emit diagnosticsChanged();
}

void BridgeClient::logDiagnostic(const QString &category, const QString &message) {
    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const QString cat = category.trimmed().isEmpty() ? QStringLiteral("app") : category.trimmed();
    QString msg = message;
    msg.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    msg.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    const QString line = QStringLiteral("[%1] [%2] %3").arg(ts, cat, msg);

    appendDiagnosticLine(line);

    if (m_diagnosticsLogPath.isEmpty()) {
        return;
    }
    const bool written = DiagnosticsLog::appendLine(m_diagnosticsLogPath, line);
    (void)written;
}

void BridgeClient::appendDiagnosticLine(const QString &line) {
    if (line.isEmpty()) {
        return;
    }
    m_diagnosticsLines.push_back(line);
    if (m_diagnosticsLines.size() > kMaxDiagnosticsLines) {
        const int removeCount = m_diagnosticsLines.size() - kMaxDiagnosticsLines;
        m_diagnosticsLines.erase(
            m_diagnosticsLines.begin(),
            m_diagnosticsLines.begin() + removeCount);
    }
    rebuildDiagnosticsText();
    emit diagnosticsChanged();
}

void BridgeClient::rebuildDiagnosticsText() {
    m_diagnosticsText = m_diagnosticsLines.join(QLatin1Char('\n'));
}

QString BridgeClient::resolveDiagnosticsLogPath() {
    return DiagnosticsLog::defaultLogPath();
}

bool BridgeClient::processSearchResultsFrame(const BinaryBridgeCodec::DecodedSearchResults &frame) {
    if (m_latestGlobalSearchSeqSent != 0
        && frame.seq != m_latestGlobalSearchSeqSent
        && !isNewerSeq(frame.seq, m_latestGlobalSearchSeqSent)) {
        logDiagnostic(
            QStringLiteral("search"),
            QStringLiteral("drop stale frame seq=%1 latestSent=%2")
                .arg(frame.seq)
                .arg(m_latestGlobalSearchSeqSent));
        return false;
    }
    if (m_globalSearchSeq != 0
        && frame.seq != m_globalSearchSeq
        && !isNewerSeq(frame.seq, m_globalSearchSeq)) {
        logDiagnostic(
            QStringLiteral("search"),
            QStringLiteral("drop non-new frame seq=%1 current=%2")
                .arg(frame.seq)
                .arg(m_globalSearchSeq));
        return false;
    }

    QVariantList artistRows;
    QVariantList albumRows;
    QVariantList trackRows;
    artistRows.reserve(frame.rows.size());
    albumRows.reserve(frame.rows.size());
    trackRows.reserve(frame.rows.size());

    for (const auto &row : frame.rows) {
        QVariantMap item;
        item.insert(QStringLiteral("rowType"), row.rowType);
        item.insert(QStringLiteral("score"), row.score);
        item.insert(QStringLiteral("label"), row.label);
        item.insert(QStringLiteral("artist"), row.artist);
        item.insert(QStringLiteral("album"), row.album);
        item.insert(QStringLiteral("genre"), row.genre);
        item.insert(QStringLiteral("count"), row.count);
        item.insert(QStringLiteral("coverPath"), row.coverPath);
        item.insert(
            QStringLiteral("coverUrl"),
            row.coverPath.isEmpty() ? QString{} : libraryThumbnailSource(row.coverPath));
        item.insert(QStringLiteral("artistKey"), row.artistKey);
        item.insert(QStringLiteral("albumKey"), row.albumKey);
        item.insert(QStringLiteral("sectionKey"), row.sectionKey);
        item.insert(QStringLiteral("trackKey"), row.trackKey);
        item.insert(QStringLiteral("trackPath"), row.trackPath);
        if (row.year != std::numeric_limits<int>::min()) {
            item.insert(QStringLiteral("year"), row.year);
        } else {
            item.insert(QStringLiteral("year"), QVariant{});
        }
        if (row.trackNumber > 0) {
            item.insert(QStringLiteral("trackNumber"), row.trackNumber);
        } else {
            item.insert(QStringLiteral("trackNumber"), QVariant{});
        }
        item.insert(QStringLiteral("lengthSeconds"), row.lengthSeconds);
        item.insert(
            QStringLiteral("lengthText"),
            row.lengthSeconds >= 0.0f
                ? formatDurationCompact(static_cast<double>(row.lengthSeconds))
                : QStringLiteral("--:--"));
        switch (row.rowType) {
        case BinaryBridgeCodec::SearchRowArtist:
            artistRows.push_back(item);
            break;
        case BinaryBridgeCodec::SearchRowAlbum:
            albumRows.push_back(item);
            break;
        case BinaryBridgeCodec::SearchRowTrack:
            trackRows.push_back(item);
            break;
        default:
            break;
        }
    }

    if (m_globalSearchSeq == frame.seq
        && m_globalSearchArtistResults == artistRows
        && m_globalSearchAlbumResults == albumRows
        && m_globalSearchTrackResults == trackRows) {
        logDiagnostic(
            QStringLiteral("search"),
            QStringLiteral("frame seq=%1 unchanged").arg(frame.seq));
        return false;
    }

    logDiagnostic(
        QStringLiteral("search"),
        QStringLiteral("apply frame seq=%1 artists=%2 albums=%3 tracks=%4")
            .arg(frame.seq)
            .arg(artistRows.size())
            .arg(albumRows.size())
            .arg(trackRows.size()));
    m_globalSearchSeq = frame.seq;
    m_globalSearchArtistResults = std::move(artistRows);
    m_globalSearchAlbumResults = std::move(albumRows);
    m_globalSearchTrackResults = std::move(trackRows);
    return true;
}

void BridgeClient::flushGlobalSearchQuery() {
    if (m_ffiBridge == nullptr) {
        logDiagnostic(QStringLiteral("search"), QStringLiteral("skip send: bridge unavailable"));
        return;
    }
    if (m_pendingGlobalSearchQuery == m_lastGlobalSearchQuerySent) {
        logDiagnostic(QStringLiteral("search"), QStringLiteral("skip duplicate query"));
        return;
    }
    const quint32 seq = m_nextGlobalSearchSeq++;
    m_latestGlobalSearchSeqSent = seq;
    m_lastGlobalSearchQuerySent = m_pendingGlobalSearchQuery;
    const QString trimmedQuery = m_pendingGlobalSearchQuery.trimmed();
    QString preview = trimmedQuery;
    if (preview.size() > 64) {
        preview = preview.left(64) + QStringLiteral("...");
    }
    logDiagnostic(
        QStringLiteral("search"),
        QStringLiteral("send query seq=%1 chars=%2 text=\"%3\"")
            .arg(seq)
            .arg(trimmedQuery.size())
            .arg(preview));
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandSearchQuery(
        BinaryBridgeCodec::CmdSetSearchQuery,
        seq,
        m_pendingGlobalSearchQuery));
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
            || lowered.contains(QStringLiteral("org.gnome.files"))) {
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
    proc.start(
        QStringLiteral("xdg-mime"),
        {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("inode/directory")});
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

void BridgeClient::sendBinaryCommand(const QByteArray &payload) {
    if (payload.isEmpty()) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("drop empty command payload"));
        emit bridgeError(QStringLiteral("failed to encode binary bridge command"));
        return;
    }
    if (m_ffiBridge == nullptr) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("drop command: bridge not initialized"));
        emit bridgeError(QStringLiteral("bridge is not initialized"));
        return;
    }
    const auto *ptr = reinterpret_cast<const std::uint8_t *>(payload.constData());
    if (!ferrous_ffi_bridge_send_binary(m_ffiBridge, ptr, static_cast<std::size_t>(payload.size()))) {
        logDiagnostic(
            QStringLiteral("bridge"),
            QStringLiteral("failed to send command bytes=%1").arg(payload.size()));
        emit bridgeError(QStringLiteral("failed to send command to in-process bridge"));
    }
}

void BridgeClient::sendLibraryRootCommand(quint16 cmdId, const QString &path) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(cmdId, path));
}

void BridgeClient::applyLibraryTreeFrame(int version, const QByteArray &treeBytes) {
    m_libraryTreeBinary = treeBytes;
    m_libraryVersion = version;

    m_libraryAlbums.clear();
    m_libraryAlbumArtists.clear();
    m_libraryAlbumNames.clear();
    m_libraryAlbumCoverPaths.clear();
    m_libraryAlbumTrackPaths.clear();
    m_trackCoverByPath.clear();

    bool coverChanged = false;
    if (!m_currentTrackPath.isEmpty()) {
        const QString refreshedCover = findTrackCoverUrl(m_currentTrackPath);
        m_trackCoverByPath.insert(m_currentTrackPath, refreshedCover);
        if (m_currentTrackCoverPath != refreshedCover) {
            m_currentTrackCoverPath = refreshedCover;
            coverChanged = true;
        }
    }

    emit libraryTreeFrameReceived(version, treeBytes);
    if (coverChanged) {
        scheduleSnapshotChanged();
    }
}

bool BridgeClient::processBinarySnapshot(const BinaryBridgeCodec::DecodedSnapshot &snapshot) {
    if (snapshot.hasStopped) {
        if (m_connected) {
            m_connected = false;
            emit connectedChanged();
        }
        return false;
    }

    if (!snapshot.errorMessage.trimmed().isEmpty()) {
        emit bridgeError(snapshot.errorMessage);
        return false;
    }

    if (!snapshot.playback.present
        && !snapshot.queue.present
        && !snapshot.library.present
        && !snapshot.metadata.present
        && !snapshot.settings.present) {
        return false;
    }

    const QString nextState = playbackStateText(snapshot.playback.state, m_playbackState);
    const double pos = snapshot.playback.present ? snapshot.playback.positionSeconds : m_positionSeconds;
    const double dur = snapshot.playback.present ? snapshot.playback.durationSeconds : m_durationSeconds;
    const int repeatMode = std::clamp(snapshot.playback.present ? snapshot.playback.repeatMode : m_repeatMode, 0, 2);
    const bool shuffleEnabled = snapshot.playback.present ? snapshot.playback.shuffleEnabled : m_shuffleEnabled;
    const QString currentPath = snapshot.playback.present ? snapshot.playback.currentPath : m_currentTrackPath;
    int playing = snapshot.playback.present ? snapshot.playback.currentQueueIndex : m_playingQueueIndex;

    const int qlen = snapshot.queue.present ? snapshot.queue.len : m_queueLength;
    const double queueDurationSecs = snapshot.queue.present ? snapshot.queue.totalDurationSeconds : 0.0;
    const int queueUnknownDurationCount = std::max(0, snapshot.queue.present ? snapshot.queue.unknownDurationCount : 0);
    const int selected = snapshot.queue.present ? snapshot.queue.selectedIndex : m_selectedQueueIndex;

    const QString metadataSourcePath = snapshot.metadata.present ? snapshot.metadata.sourcePath : QString{};
    const QString metadataCoverPath = snapshot.metadata.present ? snapshot.metadata.coverPath : QString{};
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

    const double settingsVolume = snapshot.settings.present
        ? static_cast<double>(snapshot.settings.volume)
        : m_volume;
    if (std::abs(m_volume - settingsVolume) > 0.0005) {
        m_volume = settingsVolume;
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

    if (snapshot.queue.present) {
        QStringList items;
        QStringList paths;
        items.reserve(snapshot.queue.tracks.size());
        paths.reserve(snapshot.queue.tracks.size());
        for (const auto &track : snapshot.queue.tracks) {
            paths.push_back(track.path);
            items.push_back(track.title.isEmpty() ? track.path : track.title);
        }
        if (m_queueItems != items) {
            m_queueItems = items;
            changed = true;
        }
        if (m_queuePaths != paths) {
            m_queuePaths = paths;
            m_queueVersion = m_queueVersion < std::numeric_limits<int>::max()
                ? (m_queueVersion + 1)
                : 1;
            changed = true;
        }
    }

    if (m_pendingQueueSelection >= 0) {
        if (selected == m_pendingQueueSelection || nowMs >= m_pendingQueueSelectionUntilMs) {
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

    const double dbRange = snapshot.settings.present
        ? static_cast<double>(snapshot.settings.dbRange)
        : m_dbRange;
    if (!qFuzzyCompare(m_dbRange + 1.0, dbRange + 1.0)) {
        m_dbRange = dbRange;
        changed = true;
    }

    const bool logScale = snapshot.settings.present ? snapshot.settings.logScale : m_logScale;
    if (m_logScale != logScale) {
        m_logScale = logScale;
        changed = true;
    }

    const bool showFps = snapshot.settings.present ? snapshot.settings.showFps : m_showFps;
    if (m_showFps != showFps) {
        m_showFps = showFps;
        changed = true;
    }

    const int settingsSortMode = std::clamp(
        snapshot.settings.present ? snapshot.settings.librarySortMode : m_librarySortMode,
        0,
        1);
    if (m_librarySortMode != settingsSortMode) {
        m_librarySortMode = settingsSortMode;
        changed = true;
    }

    const bool scanInProgress = snapshot.library.present ? snapshot.library.scanInProgress : m_libraryScanInProgress;
    if (m_libraryScanInProgress != scanInProgress) {
        m_libraryScanInProgress = scanInProgress;
        changed = true;
    }

    const int roots = snapshot.library.present ? snapshot.library.rootCount : m_libraryRootCount;
    if (m_libraryRootCount != roots) {
        m_libraryRootCount = roots;
        changed = true;
    }

    const int tracks = snapshot.library.present ? snapshot.library.trackCount : m_libraryTrackCount;
    if (m_libraryTrackCount != tracks) {
        m_libraryTrackCount = tracks;
        changed = true;
    }

    const int artists = snapshot.library.present ? snapshot.library.artistCount : m_libraryArtistCount;
    if (m_libraryArtistCount != artists) {
        m_libraryArtistCount = artists;
        changed = true;
    }

    const int albums = snapshot.library.present ? snapshot.library.albumCount : m_libraryAlbumCount;
    if (m_libraryAlbumCount != albums) {
        m_libraryAlbumCount = albums;
        changed = true;
    }

    const QStringList rootPaths = snapshot.library.present ? snapshot.library.rootPaths : m_libraryRoots;
    if (m_libraryRoots != rootPaths) {
        m_libraryRoots = rootPaths;
        changed = true;
    }

    const QString libraryLastError = snapshot.library.present ? snapshot.library.lastError : m_libraryLastError;
    if (m_libraryLastError != libraryLastError) {
        m_libraryLastError = libraryLastError;
        if (!m_libraryLastError.trimmed().isEmpty()) {
            emit bridgeError(QStringLiteral("library: %1").arg(m_libraryLastError));
        }
    }

    if (!m_pendingAddRootPath.isEmpty()) {
        const bool fresh = m_pendingAddRootIssuedMs > 0 && (nowMs - m_pendingAddRootIssuedMs) <= 10000;
        const bool rootAppeared = rootPaths.contains(m_pendingAddRootPath);
        if (!fresh || rootAppeared || m_libraryScanInProgress) {
            m_pendingAddRootPath.clear();
            m_pendingAddRootIssuedMs = 0;
        }
    }

    const int librarySortMode = std::clamp(
        snapshot.library.present ? snapshot.library.sortMode : m_librarySortMode,
        0,
        1);
    if (m_librarySortMode != librarySortMode) {
        m_librarySortMode = librarySortMode;
        changed = true;
    }

    const int rootsCompleted = snapshot.library.present ? std::max(0, snapshot.library.rootsCompleted) : m_libraryScanRootsCompleted;
    const int rootsTotal = snapshot.library.present ? std::max(0, snapshot.library.rootsTotal) : m_libraryScanRootsTotal;
    const int discovered = snapshot.library.present ? std::max(0, snapshot.library.filesDiscovered) : m_libraryScanDiscovered;
    const int processed = snapshot.library.present ? std::max(0, snapshot.library.filesProcessed) : m_libraryScanProcessed;
    const double filesPerSecond = snapshot.library.present ? snapshot.library.filesPerSecond : m_libraryScanFilesPerSecond;
    const double etaSeconds = snapshot.library.present ? snapshot.library.etaSeconds : m_libraryScanEtaSeconds;

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

    return changed;
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

QString BridgeClient::formatDurationCompact(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(secs, 2, 10, QChar('0'));
}
