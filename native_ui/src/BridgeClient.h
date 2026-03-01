#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class BridgeClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY snapshotChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY snapshotChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY snapshotChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY snapshotChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY snapshotChanged)
    Q_PROPERTY(double volume READ volume NOTIFY snapshotChanged)
    Q_PROPERTY(int queueLength READ queueLength NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList queueItems READ queueItems NOTIFY snapshotChanged)
    Q_PROPERTY(int selectedQueueIndex READ selectedQueueIndex NOTIFY snapshotChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit BridgeClient(QObject *parent = nullptr);

    QString playbackState() const;
    QString positionText() const;
    QString durationText() const;
    double positionSeconds() const;
    double durationSeconds() const;
    double volume() const;
    int queueLength() const;
    QStringList queueItems() const;
    int selectedQueueIndex() const;
    bool connected() const;

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void setVolume(double value);
    Q_INVOKABLE void playAt(int index);
    Q_INVOKABLE void selectQueueIndex(int index);
    Q_INVOKABLE void removeAt(int index);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void shutdown();

signals:
    void snapshotChanged();
    void connectedChanged();
    void bridgeError(const QString &message);

private:
    void startBridgeProcess();
    void sendCommand(const QString &cmd, double value = -1.0);
    void handleStdoutReady();
    void handleStderrReady();
    void handleProcessStarted();
    void handleProcessFinished();
    static QString formatSeconds(double seconds);

    QProcess m_process;
    QString m_playbackState{"Stopped"};
    QString m_positionText{"00:00"};
    QString m_durationText{"00:00"};
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    double m_volume{1.0};
    int m_queueLength{0};
    QStringList m_queueItems;
    int m_selectedQueueIndex{-1};
    bool m_connected{false};
};
