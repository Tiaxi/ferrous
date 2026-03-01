#include "BridgeClient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &BridgeClient::handleStdoutReady);
    connect(&m_process, &QProcess::readyReadStandardError, this, &BridgeClient::handleStderrReady);
    connect(&m_process, &QProcess::started, this, &BridgeClient::handleProcessStarted);
    connect(&m_process, &QProcess::finished, this, &BridgeClient::handleProcessFinished);
    startBridgeProcess();
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

QVariantList BridgeClient::waveformPeaks() const {
    return m_waveformPeaks;
}

QVariantList BridgeClient::spectrogramRowsDelta() const {
    return m_spectrogramRowsDelta;
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

bool BridgeClient::libraryScanInProgress() const {
    return m_libraryScanInProgress;
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
    sendCommand(QStringLiteral("seek"), std::max(0.0, seconds));
}

void BridgeClient::setVolume(double value) {
    sendCommand(QStringLiteral("set_volume"), std::clamp(value, 0.0, 1.0));
}

void BridgeClient::playAt(int index) {
    if (index < 0) {
        return;
    }
    sendCommand(QStringLiteral("play_at"), static_cast<double>(index));
}

void BridgeClient::selectQueueIndex(int index) {
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

QVariantList BridgeClient::takeSpectrogramRowsDelta() {
    QVariantList out = m_spectrogramRowsDelta;
    if (!m_spectrogramRowsDelta.isEmpty()) {
        m_spectrogramRowsDelta.clear();
    }
    return out;
}

void BridgeClient::requestSnapshot() {
    sendCommand(QStringLiteral("request_snapshot"));
}

void BridgeClient::shutdown() {
    sendCommand(QStringLiteral("shutdown"));
}

void BridgeClient::startBridgeProcess() {
    QString command = qEnvironmentVariable("FERROUS_BRIDGE_CMD");
    if (command.isEmpty()) {
        // Prefer a prebuilt bridge binary for lower overhead and predictable runtime memory.
        const QDir appDir(QCoreApplication::applicationDirPath());
        const QStringList candidates{
            appDir.absoluteFilePath(QStringLiteral("../../target/debug/native_frontend")),
            appDir.absoluteFilePath(QStringLiteral("../../target/release/native_frontend")),
            QDir::current().absoluteFilePath(QStringLiteral("target/debug/native_frontend")),
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
            command = QStringLiteral("cargo run --bin native_frontend --features gst -- --json-bridge");
        }
    }

    const QString shell = QStringLiteral("/bin/sh");
    const QStringList args{QStringLiteral("-lc"), command};
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
    constexpr int kMaxLinesPerPass = 48;
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

        const QJsonObject root = doc.object();
        const QString event = root.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("snapshot")) {
            const QJsonObject playback = root.value(QStringLiteral("playback")).toObject();
            const QJsonObject queue = root.value(QStringLiteral("queue")).toObject();
            const QJsonObject library = root.value(QStringLiteral("library")).toObject();
            const QJsonObject analysis = root.value(QStringLiteral("analysis")).toObject();
            const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();

            const QString nextState = playback.value(QStringLiteral("state")).toString();
            const double pos = playback.value(QStringLiteral("position_secs")).toDouble();
            const double dur = playback.value(QStringLiteral("duration_secs")).toDouble();
            const double vol = playback.value(QStringLiteral("volume")).toDouble();
            const int qlen = queue.value(QStringLiteral("len")).toInt();
            const int selected = queue.value(QStringLiteral("selected_index")).toInt(-1);
            const int sampleRate = analysis.value(QStringLiteral("sample_rate_hz")).toInt(m_sampleRateHz);

            bool changed = false;
            if (m_playbackState != nextState) {
                m_playbackState = nextState;
                changed = true;
            }
            const QString posText = formatSeconds(pos);
            if (m_positionText != posText) {
                m_positionText = posText;
                changed = true;
            }
            if (!qFuzzyCompare(m_positionSeconds + 1.0, pos + 1.0)) {
                m_positionSeconds = pos;
                changed = true;
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
            if (m_selectedQueueIndex != selected) {
                m_selectedQueueIndex = selected;
                changed = true;
            }
            const bool spectrogramReset = analysis.value(QStringLiteral("spectrogram_reset")).toBool();
            if (m_spectrogramReset != spectrogramReset) {
                m_spectrogramReset = spectrogramReset;
                changed = true;
            }
            const QJsonValue spectrogramRowsValue = analysis.value(QStringLiteral("spectrogram_rows"));
            if (spectrogramRowsValue.isArray()) {
                const QJsonArray rowsArr = spectrogramRowsValue.toArray();
                QVariantList rowsDelta;
                rowsDelta.reserve(rowsArr.size());
                for (const QJsonValue &rowValue : rowsArr) {
                    const QJsonArray rowArr = rowValue.toArray();
                    if (rowArr.isEmpty()) {
                        continue;
                    }
                    QVariantList row;
                    row.reserve(rowArr.size());
                    for (const QJsonValue &v : rowArr) {
                        row.push_back(v.toDouble());
                    }
                    rowsDelta.push_back(row);
                }
                if (!rowsDelta.isEmpty()) {
                    m_spectrogramRowsDelta += rowsDelta;
                    constexpr int kMaxPendingSpectrogramRows = 64;
                    if (m_spectrogramRowsDelta.size() > kMaxPendingSpectrogramRows) {
                        m_spectrogramRowsDelta = m_spectrogramRowsDelta.mid(
                            m_spectrogramRowsDelta.size() - kMaxPendingSpectrogramRows);
                    }
                    changed = true;
                }
            }
            if (m_sampleRateHz != sampleRate) {
                m_sampleRateHz = sampleRate;
                changed = true;
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
            const QJsonValue albumsValue = library.value(QStringLiteral("albums"));
            if (albumsValue.isArray()) {
                const QJsonArray albums = albumsValue.toArray();
                QStringList labels;
                QStringList artists;
                QStringList albumNames;
                labels.reserve(albums.size());
                artists.reserve(albums.size());
                albumNames.reserve(albums.size());
                for (const QJsonValue &entry : albums) {
                    const QJsonObject obj = entry.toObject();
                    const QString artist = obj.value(QStringLiteral("artist")).toString();
                    const QString name = obj.value(QStringLiteral("name")).toString();
                    const int count = obj.value(QStringLiteral("count")).toInt();
                    labels.push_back(QStringLiteral("%1 - %2 (%3)").arg(artist, name).arg(count));
                    artists.push_back(artist);
                    albumNames.push_back(name);
                }
                if (m_libraryAlbums != labels) {
                    m_libraryAlbums = labels;
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
            const QJsonValue waveformValue = analysis.value(QStringLiteral("waveform_peaks"));
            if (waveformValue.isArray()) {
                QVariantList peaks;
                const QJsonArray arr = waveformValue.toArray();
                peaks.reserve(arr.size());
                for (const QJsonValue &v : arr) {
                    peaks.push_back(v.toDouble());
                }
                if (m_waveformPeaks != peaks) {
                    m_waveformPeaks = peaks;
                    changed = true;
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
    }
    if (anySnapshotChanged) {
        emit snapshotChanged();
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
    const QList<QByteArray> lines = chunk.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        // Keep high-frequency profiling output out of QML signal path to avoid UI stalls.
        if (line.startsWith(QStringLiteral("[analysis]")) || line.startsWith(QStringLiteral("[gst]"))) {
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
