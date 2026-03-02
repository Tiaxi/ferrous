#pragma once

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

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
    Q_PROPERTY(QByteArray waveformPeaksPacked READ waveformPeaksPacked NOTIFY snapshotChanged)
    Q_PROPERTY(bool spectrogramReset READ spectrogramReset NOTIFY snapshotChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY snapshotChanged)
    Q_PROPERTY(double dbRange READ dbRange NOTIFY snapshotChanged)
    Q_PROPERTY(bool logScale READ logScale NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryAlbums READ libraryAlbums NOTIFY snapshotChanged)
    Q_PROPERTY(bool libraryScanInProgress READ libraryScanInProgress NOTIFY snapshotChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit BridgeClient(QObject *parent = nullptr);
    ~BridgeClient() override;

    QString playbackState() const;
    QString positionText() const;
    QString durationText() const;
    double positionSeconds() const;
    double durationSeconds() const;
    double volume() const;
    int queueLength() const;
    QStringList queueItems() const;
    int selectedQueueIndex() const;
    QByteArray waveformPeaksPacked() const;
    bool spectrogramReset() const;
    int sampleRateHz() const;
    double dbRange() const;
    bool logScale() const;
    QStringList libraryAlbums() const;
    bool libraryScanInProgress() const;
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
    Q_INVOKABLE void moveQueue(int from, int to);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void replaceAlbumAt(int index);
    Q_INVOKABLE void appendAlbumAt(int index);
    Q_INVOKABLE void scanRoot(const QString &path);
    Q_INVOKABLE void scanDefaultMusicRoot();
    Q_INVOKABLE QVariantMap takeSpectrogramRowsDeltaPacked();
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void shutdown();

signals:
    void snapshotChanged();
    void connectedChanged();
    void bridgeError(const QString &message);

private:
    void teardownAnalysisSocket(bool immediateDelete);
    void setupAnalysisSocketServer();
    void handleAnalysisSocketConnected();
    void handleAnalysisSocketReady();
    void scheduleSnapshotChanged();
    void startBridgeProcess();
    void sendJson(const QJsonObject &obj);
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
    QByteArray m_waveformPeaksPacked;
    QByteArray m_spectrogramRowsPacked;
    int m_spectrogramPackedRows{0};
    int m_spectrogramPackedBins{0};
    bool m_spectrogramReset{false};
    int m_sampleRateHz{48000};
    double m_dbRange{90.0};
    bool m_logScale{false};
    QStringList m_libraryAlbums;
    QStringList m_libraryAlbumArtists;
    QStringList m_libraryAlbumNames;
    bool m_libraryScanInProgress{false};
    bool m_connected{false};
    bool m_stdoutPumpScheduled{false};
    bool m_snapshotChangedPending{false};
    bool m_pendingSeek{false};
    double m_pendingSeekTargetSeconds{0.0};
    qint64 m_pendingSeekUntilMs{0};
    int m_pendingQueueSelection{-1};
    qint64 m_pendingQueueSelectionUntilMs{0};
    QTimer m_snapshotNotifyTimer;
    QByteArray m_stderrBuffer;
    QLocalServer m_analysisServer;
    QLocalSocket *m_analysisSocket{nullptr};
    QByteArray m_analysisBuffer;
    QString m_analysisSocketName;
    bool m_analysisSocketConnected{false};
    bool m_hasAnalysisFrameSeq{false};
    quint32 m_lastAnalysisFrameSeq{0};
    quint64 m_analysisDroppedFrames{0};
};
