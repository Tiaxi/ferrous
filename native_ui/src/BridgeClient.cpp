#include "BridgeClient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QProcessEnvironment>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QMetaObject>
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
} // namespace

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &BridgeClient::handleStdoutReady);
    connect(&m_process, &QProcess::readyReadStandardError, this, &BridgeClient::handleStderrReady);
    connect(&m_process, &QProcess::started, this, &BridgeClient::handleProcessStarted);
    connect(&m_process, &QProcess::finished, this, &BridgeClient::handleProcessFinished);
    m_snapshotNotifyTimer.setSingleShot(true);
    m_snapshotNotifyTimer.setInterval(8);
    connect(&m_snapshotNotifyTimer, &QTimer::timeout, this, [this]() {
        if (m_snapshotChangedPending) {
            m_snapshotChangedPending = false;
            emit snapshotChanged();
        }
    });
    setupAnalysisSocketServer();
    startBridgeProcess();
}

BridgeClient::~BridgeClient() {
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

void BridgeClient::teardownAnalysisSocket(bool immediateDelete) {
    QLocalSocket *socket = m_analysisSocket;
    m_analysisSocket = nullptr;
    m_analysisSocketConnected = false;
    m_hasAnalysisFrameSeq = false;
    m_analysisBuffer.clear();
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

QStringList BridgeClient::queueItems() const {
    return m_queueItems;
}

int BridgeClient::selectedQueueIndex() const {
    return m_selectedQueueIndex;
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

QStringList BridgeClient::libraryAlbums() const {
    return m_libraryAlbums;
}

QVariantList BridgeClient::libraryTree() const {
    return m_libraryTree;
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
    if (index < 0 || index >= m_libraryAlbumArtists.size() || index >= m_libraryAlbumNames.size()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("replace_album_by_key"));
    obj.insert(QStringLiteral("artist"), m_libraryAlbumArtists[index]);
    obj.insert(QStringLiteral("album"), m_libraryAlbumNames[index]);
    sendJson(obj);
}

void BridgeClient::appendAlbumAt(int index) {
    if (index < 0 || index >= m_libraryAlbumArtists.size() || index >= m_libraryAlbumNames.size()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("append_album_by_key"));
    obj.insert(QStringLiteral("artist"), m_libraryAlbumArtists[index]);
    obj.insert(QStringLiteral("album"), m_libraryAlbumNames[index]);
    sendJson(obj);
}

void BridgeClient::scanRoot(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), QStringLiteral("scan_root"));
    obj.insert(QStringLiteral("path"), path);
    sendJson(obj);
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
    if (chunk.isEmpty()) {
        return;
    }
    m_analysisBuffer += chunk;

    bool changed = false;
    while (m_analysisBuffer.size() >= static_cast<qsizetype>(sizeof(quint32))) {
        const auto *lenPtr = reinterpret_cast<const uchar *>(m_analysisBuffer.constData());
        const quint32 frameBytes = qFromLittleEndian<quint32>(lenPtr);
        if (frameBytes == 0 || frameBytes > kMaxAnalysisFrameBytes) {
            emit bridgeError(QStringLiteral("invalid analysis frame size: %1").arg(frameBytes));
            m_analysisBuffer.clear();
            break;
        }
        const qsizetype totalBytes = static_cast<qsizetype>(sizeof(quint32) + frameBytes);
        if (m_analysisBuffer.size() < totalBytes) {
            break;
        }
        QByteArray frame = m_analysisBuffer.mid(sizeof(quint32), frameBytes);
        m_analysisBuffer.remove(0, totalBytes);

        if (frame.size() < 16) {
            continue;
        }
        const auto *data = reinterpret_cast<const uchar *>(frame.constData());
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
        if (frame.size() < expected) {
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
        scheduleSnapshotChanged();
    }
}

void BridgeClient::scheduleSnapshotChanged() {
    m_snapshotChangedPending = true;
    if (!m_snapshotNotifyTimer.isActive()) {
        m_snapshotNotifyTimer.start();
    }
}

void BridgeClient::startBridgeProcess() {
    QString command = qEnvironmentVariable("FERROUS_BRIDGE_CMD");
    if (command.isEmpty()) {
        // Prefer a prebuilt bridge binary for lower overhead and predictable runtime memory.
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QStringList candidates{
            appDir.absoluteFilePath(QStringLiteral("../../target/release/native_frontend")),
            QDir::current().absoluteFilePath(QStringLiteral("target/release/native_frontend")),
        };

        for (const QString &candidate : candidates) {
            const QFileInfo info(candidate);
            if (info.exists() && info.isFile() && info.isExecutable()) {
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

void BridgeClient::sendCommand(const QString &cmd, double value) {
    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    if (value >= 0.0) {
        obj.insert(QStringLiteral("value"), value);
    }

    sendJson(obj);
}

void BridgeClient::sendJson(const QJsonObject &obj) {
    if (m_process.state() != QProcess::Running) {
        emit bridgeError(QStringLiteral("bridge process is not running"));
        return;
    }
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    m_process.write(payload);
}

void BridgeClient::handleStdoutReady() {
    m_stdoutPumpScheduled = false;
    bool anySnapshotChanged = false;
    int processedLines = 0;
    constexpr int kMaxLinesPerPass = 256;

    auto processRoot = [&](const QJsonObject &root) {
        const QString event = root.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("snapshot")) {
            const QJsonObject playback = root.value(QStringLiteral("playback")).toObject();
            const QJsonObject queue = root.value(QStringLiteral("queue")).toObject();
            const QJsonObject library = root.value(QStringLiteral("library")).toObject();
            const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
            const QJsonObject analysis = root.value(QStringLiteral("analysis")).toObject();

            const QString nextState = playback.value(QStringLiteral("state")).toString();
            const double pos = playback.value(QStringLiteral("position_secs")).toDouble();
            const double dur = playback.value(QStringLiteral("duration_secs")).toDouble();
            const double vol = playback.value(QStringLiteral("volume")).toDouble();
            const int qlen = queue.value(QStringLiteral("len")).toInt();
            const int selected = queue.value(QStringLiteral("selected_index")).toInt(-1);
            const int sampleRate = analysis.value(QStringLiteral("sample_rate_hz")).toInt(m_sampleRateHz);

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
                if (!qFuzzyCompare(m_positionSeconds + 1.0, pos + 1.0)) {
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
            if (!qFuzzyCompare(m_volume + 1.0, vol + 1.0)) {
                m_volume = vol;
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
            const QJsonValue queueTracksValue = queue.value(QStringLiteral("tracks"));
            if (queueTracksValue.isArray()) {
                const QJsonArray queueTracks = queueTracksValue.toArray();
                QStringList items;
                items.reserve(queueTracks.size());
                for (const QJsonValue &track : queueTracks) {
                    const QJsonObject obj = track.toObject();
                    const QString title = obj.value(QStringLiteral("title")).toString();
                    const QString path = obj.value(QStringLiteral("path")).toString();
                    items.push_back(title.isEmpty() ? path : title);
                }
                if (m_queueItems != items) {
                    m_queueItems = items;
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
            if (!m_analysisSocketConnected) {
                const bool spectrogramReset = analysis.value(QStringLiteral("spectrogram_reset")).toBool();
                if (m_spectrogramReset != spectrogramReset) {
                    m_spectrogramReset = spectrogramReset;
                    changed = true;
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
                        changed = true;
                    }
                }
                if (m_sampleRateHz != sampleRate) {
                    m_sampleRateHz = sampleRate;
                    changed = true;
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
            const QJsonValue albumsValue = library.value(QStringLiteral("albums"));
            if (albumsValue.isArray()) {
                const QJsonArray albums = albumsValue.toArray();
                QStringList labels;
                QStringList artists;
                QStringList albumNames;
                QVariantList libraryTree;
                QStringList artistOrder;
                QHash<QString, int> artistToIndex;
                QVector<QVariantList> artistAlbums;
                labels.reserve(albums.size());
                artists.reserve(albums.size());
                albumNames.reserve(albums.size());
                for (qsizetype sourceIndex = 0; sourceIndex < albums.size(); ++sourceIndex) {
                    const QJsonValue &entry = albums[static_cast<int>(sourceIndex)];
                    const QJsonObject obj = entry.toObject();
                    const QString artist = obj.value(QStringLiteral("artist")).toString();
                    const QString name = obj.value(QStringLiteral("name")).toString();
                    const int count = obj.value(QStringLiteral("count")).toInt();
                    labels.push_back(QStringLiteral("%1 - %2 (%3)").arg(artist, name).arg(count));
                    artists.push_back(artist);
                    albumNames.push_back(name);

                    int artistIndex = artistToIndex.value(artist, -1);
                    if (artistIndex < 0) {
                        artistIndex = artistOrder.size();
                        artistToIndex.insert(artist, artistIndex);
                        artistOrder.push_back(artist);
                        artistAlbums.push_back(QVariantList{});
                    }

                    QVariantList trackTitles;
                    const QJsonValue tracksValue = obj.value(QStringLiteral("tracks"));
                    if (tracksValue.isArray()) {
                        const QJsonArray tracks = tracksValue.toArray();
                        trackTitles.reserve(tracks.size());
                        for (const QJsonValue &titleValue : tracks) {
                            const QString title = titleValue.toString();
                            if (!title.isEmpty()) {
                                trackTitles.push_back(title);
                            }
                        }
                    } else {
                        const QJsonValue pathsValue = obj.value(QStringLiteral("paths"));
                        if (pathsValue.isArray()) {
                            const QJsonArray paths = pathsValue.toArray();
                            trackTitles.reserve(paths.size());
                            for (const QJsonValue &pathValue : paths) {
                                const QString path = pathValue.toString();
                                if (path.isEmpty()) {
                                    continue;
                                }
                                const QFileInfo info(path);
                                QString title = info.completeBaseName();
                                if (title.isEmpty()) {
                                    title = info.fileName();
                                }
                                if (!title.isEmpty()) {
                                    trackTitles.push_back(title);
                                }
                            }
                        }
                    }

                    QVariantMap albumEntry;
                    albumEntry.insert(QStringLiteral("name"), name);
                    albumEntry.insert(QStringLiteral("count"), count);
                    albumEntry.insert(QStringLiteral("sourceIndex"), static_cast<int>(sourceIndex));
                    albumEntry.insert(QStringLiteral("tracks"), trackTitles);
                    artistAlbums[artistIndex].push_back(albumEntry);
                }

                for (int i = 0; i < artistOrder.size(); ++i) {
                    const QString &artist = artistOrder[i];
                    const QVariantList &albumsForArtist = artistAlbums[i];
                    int artistTrackCount = 0;
                    for (const QVariant &albumValue : albumsForArtist) {
                        artistTrackCount += albumValue.toMap().value(QStringLiteral("count")).toInt();
                    }
                    QVariantMap artistEntry;
                    artistEntry.insert(QStringLiteral("artist"), artist);
                    artistEntry.insert(QStringLiteral("count"), artistTrackCount);
                    artistEntry.insert(QStringLiteral("albums"), albumsForArtist);
                    libraryTree.push_back(artistEntry);
                }
                if (m_libraryAlbums != labels) {
                    m_libraryAlbums = labels;
                    changed = true;
                }
                if (m_libraryTree != libraryTree) {
                    m_libraryTree = libraryTree;
                    changed = true;
                }
                if (m_libraryAlbumArtists != artists) {
                    m_libraryAlbumArtists = artists;
                    changed = true;
                }
                if (m_libraryAlbumNames != albumNames) {
                    m_libraryAlbumNames = albumNames;
                    changed = true;
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
                        changed = true;
                    }
                }
            }
            if (changed) {
                anySnapshotChanged = true;
            }
        } else if (event == QStringLiteral("error")) {
            emit bridgeError(root.value(QStringLiteral("message")).toString());
        } else if (event == QStringLiteral("stopped")) {
            if (m_connected) {
                m_connected = false;
                emit connectedChanged();
            }
        }
    };

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
