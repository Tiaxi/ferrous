#include "BridgeClient.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

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

void BridgeClient::requestSnapshot() {
    sendCommand(QStringLiteral("request_snapshot"));
}

void BridgeClient::shutdown() {
    sendCommand(QStringLiteral("shutdown"));
}

void BridgeClient::startBridgeProcess() {
    QString command = qEnvironmentVariable("FERROUS_BRIDGE_CMD");
    if (command.isEmpty()) {
        // Dev default: run Rust bridge through cargo in current repo.
        command = QStringLiteral("cargo run --bin native_frontend --features gst -- --json-bridge");
    }

    const QString shell = QStringLiteral("/bin/sh");
    const QStringList args{QStringLiteral("-lc"), command};
    m_process.start(shell, args);
}

void BridgeClient::sendCommand(const QString &cmd, double value) {
    if (m_process.state() != QProcess::Running) {
        emit bridgeError(QStringLiteral("bridge process is not running"));
        return;
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("cmd"), cmd);
    if (value >= 0.0) {
        obj.insert(QStringLiteral("value"), value);
    }

    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    m_process.write(payload);
}

void BridgeClient::handleStdoutReady() {
    while (m_process.canReadLine()) {
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

            const QString nextState = playback.value(QStringLiteral("state")).toString();
            const double pos = playback.value(QStringLiteral("position_secs")).toDouble();
            const double dur = playback.value(QStringLiteral("duration_secs")).toDouble();
            const double vol = playback.value(QStringLiteral("volume")).toDouble();
            const int qlen = queue.value(QStringLiteral("len")).toInt();

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
            if (changed) {
                emit snapshotChanged();
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
}

void BridgeClient::handleStderrReady() {
    const QString text = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
    if (!text.isEmpty()) {
        emit bridgeError(text);
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
