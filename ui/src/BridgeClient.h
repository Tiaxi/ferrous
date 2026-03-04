#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

struct FerrousFfiBridge;

class BridgeClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY snapshotChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY snapshotChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY snapshotChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY snapshotChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY snapshotChanged)
    Q_PROPERTY(double volume READ volume NOTIFY snapshotChanged)
    Q_PROPERTY(int queueLength READ queueLength NOTIFY snapshotChanged)
    Q_PROPERTY(QString queueDurationText READ queueDurationText NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList queueItems READ queueItems NOTIFY snapshotChanged)
    Q_PROPERTY(int selectedQueueIndex READ selectedQueueIndex NOTIFY snapshotChanged)
    Q_PROPERTY(int playingQueueIndex READ playingQueueIndex NOTIFY snapshotChanged)
    Q_PROPERTY(QString currentTrackPath READ currentTrackPath NOTIFY snapshotChanged)
    Q_PROPERTY(QString currentTrackCoverPath READ currentTrackCoverPath NOTIFY snapshotChanged)
    Q_PROPERTY(QByteArray waveformPeaksPacked READ waveformPeaksPacked NOTIFY analysisChanged)
    Q_PROPERTY(bool spectrogramReset READ spectrogramReset NOTIFY analysisChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY analysisChanged)
    Q_PROPERTY(double dbRange READ dbRange NOTIFY snapshotChanged)
    Q_PROPERTY(bool logScale READ logScale NOTIFY snapshotChanged)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY snapshotChanged)
    Q_PROPERTY(bool shuffleEnabled READ shuffleEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(bool showFps READ showFps NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryAlbums READ libraryAlbums NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList libraryTree READ libraryTree NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryVersion READ libraryVersion NOTIFY snapshotChanged)
    Q_PROPERTY(bool libraryScanInProgress READ libraryScanInProgress NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryRootCount READ libraryRootCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryTrackCount READ libraryTrackCount NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryRoots READ libraryRoots NOTIFY snapshotChanged)
    Q_PROPERTY(int librarySortMode READ librarySortMode NOTIFY snapshotChanged)
    Q_PROPERTY(QString fileBrowserName READ fileBrowserName NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanRootsCompleted READ libraryScanRootsCompleted NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanRootsTotal READ libraryScanRootsTotal NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanDiscovered READ libraryScanDiscovered NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanProcessed READ libraryScanProcessed NOTIFY snapshotChanged)
    Q_PROPERTY(double libraryScanFilesPerSecond READ libraryScanFilesPerSecond NOTIFY snapshotChanged)
    Q_PROPERTY(double libraryScanEtaSeconds READ libraryScanEtaSeconds NOTIFY snapshotChanged)
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
    QString queueDurationText() const;
    QStringList queueItems() const;
    int selectedQueueIndex() const;
    int playingQueueIndex() const;
    QString currentTrackPath() const;
    QString currentTrackCoverPath() const;
    QByteArray waveformPeaksPacked() const;
    bool spectrogramReset() const;
    int sampleRateHz() const;
    double dbRange() const;
    bool logScale() const;
    int repeatMode() const;
    bool shuffleEnabled() const;
    bool showFps() const;
    QStringList libraryAlbums() const;
    QVariantList libraryTree() const;
    int libraryVersion() const;
    bool libraryScanInProgress() const;
    int libraryRootCount() const;
    int libraryTrackCount() const;
    QStringList libraryRoots() const;
    int librarySortMode() const;
    QString fileBrowserName() const;
    int libraryScanRootsCompleted() const;
    int libraryScanRootsTotal() const;
    int libraryScanDiscovered() const;
    int libraryScanProcessed() const;
    double libraryScanFilesPerSecond() const;
    double libraryScanEtaSeconds() const;
    bool connected() const;

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void setVolume(double value);
    Q_INVOKABLE void setDbRange(double value);
    Q_INVOKABLE void setLogScale(bool value);
    Q_INVOKABLE void setRepeatMode(int mode);
    Q_INVOKABLE void setShuffleEnabled(bool value);
    Q_INVOKABLE void setShowFps(bool value);
    Q_INVOKABLE void playAt(int index);
    Q_INVOKABLE void selectQueueIndex(int index);
    Q_INVOKABLE void removeAt(int index);
    Q_INVOKABLE void moveQueue(int from, int to);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void replaceAlbumAt(int index);
    Q_INVOKABLE void appendAlbumAt(int index);
    Q_INVOKABLE void playTrack(const QString &path);
    Q_INVOKABLE void appendTrack(const QString &path);
    Q_INVOKABLE void replaceAlbumByKey(const QString &artist, const QString &album);
    Q_INVOKABLE void appendAlbumByKey(const QString &artist, const QString &album);
    Q_INVOKABLE void replaceArtistByName(const QString &artist);
    Q_INVOKABLE void appendArtistByName(const QString &artist);
    Q_INVOKABLE void replaceWithPaths(const QStringList &paths);
    Q_INVOKABLE void appendPaths(const QStringList &paths);
    Q_INVOKABLE QString libraryAlbumCoverAt(int index) const;
    Q_INVOKABLE QString libraryThumbnailSource(const QString &path) const;
    Q_INVOKABLE QString queuePathAt(int index) const;
    Q_INVOKABLE void addLibraryRoot(const QString &path);
    Q_INVOKABLE void removeLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanAllLibraryRoots();
    Q_INVOKABLE void setLibrarySortMode(int mode);
    Q_INVOKABLE void openInFileBrowser(const QString &path);
    Q_INVOKABLE void openContainingFolder(const QString &path);
    Q_INVOKABLE void scanRoot(const QString &path);
    Q_INVOKABLE void scanDefaultMusicRoot();
    Q_INVOKABLE QVariantMap takeSpectrogramRowsDeltaPacked();
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void shutdown();

signals:
    void snapshotChanged();
    void analysisChanged();
    void connectedChanged();
    void bridgeError(const QString &message);

private:
    bool startInProcessBridge();
    void pollInProcessBridge();
    void teardownAnalysisSocket(bool immediateDelete);
    void setupAnalysisSocketServer();
    void handleAnalysisSocketConnected();
    void handleAnalysisSocketReady();
    bool processBridgeJsonObject(const QJsonObject &root);
    void processAnalysisBytes(const QByteArray &chunk);
    void scheduleSnapshotChanged();
    void scheduleAnalysisChanged();
    static QString detectFileBrowserName();
    bool openUrlInFileBrowser(const QString &path, bool containingFolder) const;
    void startBridgeProcess();
    void sendLibraryRootCommand(const QString &cmd, const QString &path);
    void sendJson(const QJsonObject &obj);
    void sendCommand(const QString &cmd);
    void sendCommand(const QString &cmd, double value);
    void handleStdoutReady();
    void handleStderrReady();
    void handleProcessStarted();
    void handleProcessFinished();
    static QString formatSeconds(double seconds);

    QProcess m_process;
    FerrousFfiBridge *m_ffiBridge{nullptr};
    QTimer m_bridgePollTimer;
    QString m_playbackState{"Stopped"};
    QString m_positionText{"00:00"};
    QString m_durationText{"00:00"};
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    double m_volume{1.0};
    int m_queueLength{0};
    QString m_queueDurationText{"00:00"};
    QStringList m_queueItems;
    QStringList m_queuePaths;
    int m_selectedQueueIndex{-1};
    int m_playingQueueIndex{-1};
    QString m_currentTrackPath;
    QString m_currentTrackCoverPath;
    QByteArray m_waveformPeaksPacked;
    QByteArray m_spectrogramRowsPacked;
    int m_spectrogramPackedRows{0};
    int m_spectrogramPackedBins{0};
    bool m_spectrogramReset{false};
    int m_sampleRateHz{48000};
    double m_dbRange{90.0};
    bool m_logScale{false};
    int m_repeatMode{0};
    bool m_shuffleEnabled{false};
    bool m_showFps{false};
    QStringList m_libraryAlbums;
    QVariantList m_libraryTree;
    int m_libraryVersion{0};
    QStringList m_libraryAlbumArtists;
    QStringList m_libraryAlbumNames;
    QStringList m_libraryAlbumCoverPaths;
    QList<QStringList> m_libraryAlbumTrackPaths;
    QHash<QString, QString> m_trackCoverByPath;
    mutable QHash<QString, QString> m_libraryThumbnailSourceCache;
    bool m_libraryScanInProgress{false};
    int m_libraryRootCount{0};
    int m_libraryTrackCount{0};
    QStringList m_libraryRoots;
    int m_librarySortMode{0};
    QString m_fileBrowserName{QStringLiteral("File Manager")};
    int m_libraryScanRootsCompleted{0};
    int m_libraryScanRootsTotal{0};
    int m_libraryScanDiscovered{0};
    int m_libraryScanProcessed{0};
    double m_libraryScanFilesPerSecond{0.0};
    double m_libraryScanEtaSeconds{-1.0};
    QString m_libraryLastError;
    QString m_addRootCommand{QStringLiteral("add_root")};
    QString m_pendingAddRootPath;
    QString m_pendingAddRootCommand;
    int m_pendingAddRootAttempts{0};
    qint64 m_pendingAddRootIssuedMs{0};
    bool m_connected{false};
    bool m_useInProcessBridge{false};
    bool m_stdoutPumpScheduled{false};
    bool m_snapshotChangedPending{false};
    bool m_analysisChangedPending{false};
    bool m_pendingSeek{false};
    double m_pendingSeekTargetSeconds{0.0};
    qint64 m_pendingSeekUntilMs{0};
    int m_pendingQueueSelection{-1};
    qint64 m_pendingQueueSelectionUntilMs{0};
    QTimer m_snapshotNotifyTimer;
    QTimer m_analysisNotifyTimer;
    QByteArray m_stderrBuffer;
    QLocalServer m_analysisServer;
    QLocalSocket *m_analysisSocket{nullptr};
    QByteArray m_analysisBuffer;
    qsizetype m_analysisBufferReadOffset{0};
    QString m_analysisSocketName;
    bool m_analysisSocketConnected{false};
    bool m_hasAnalysisFrameSeq{false};
    quint32 m_lastAnalysisFrameSeq{0};
    quint64 m_analysisDroppedFrames{0};
};
