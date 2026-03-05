#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVariantList>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "BinaryBridgeCodec.h"
#include "GlobalSearchResultsModel.h"

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
    Q_PROPERTY(int queueVersion READ queueVersion NOTIFY snapshotChanged)
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
    Q_PROPERTY(QByteArray libraryTreeBinary READ libraryTreeBinary NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryVersion READ libraryVersion NOTIFY snapshotChanged)
    Q_PROPERTY(bool libraryScanInProgress READ libraryScanInProgress NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryRootCount READ libraryRootCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryTrackCount READ libraryTrackCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryArtistCount READ libraryArtistCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryAlbumCount READ libraryAlbumCount NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryRoots READ libraryRoots NOTIFY snapshotChanged)
    Q_PROPERTY(int librarySortMode READ librarySortMode NOTIFY snapshotChanged)
    Q_PROPERTY(QString fileBrowserName READ fileBrowserName NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanRootsCompleted READ libraryScanRootsCompleted NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanRootsTotal READ libraryScanRootsTotal NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanDiscovered READ libraryScanDiscovered NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanProcessed READ libraryScanProcessed NOTIFY snapshotChanged)
    Q_PROPERTY(double libraryScanFilesPerSecond READ libraryScanFilesPerSecond NOTIFY snapshotChanged)
    Q_PROPERTY(double libraryScanEtaSeconds READ libraryScanEtaSeconds NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList globalSearchArtistResults READ globalSearchArtistResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QVariantList globalSearchAlbumResults READ globalSearchAlbumResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QVariantList globalSearchTrackResults READ globalSearchTrackResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchArtistCount READ globalSearchArtistCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchAlbumCount READ globalSearchAlbumCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchTrackCount READ globalSearchTrackCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(quint32 globalSearchSeq READ globalSearchSeq NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QObject* globalSearchModel READ globalSearchModel CONSTANT)
    Q_PROPERTY(QString diagnosticsText READ diagnosticsText NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticsLogPath READ diagnosticsLogPath NOTIFY diagnosticsChanged)
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
    int queueVersion() const;
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
    QByteArray libraryTreeBinary() const;
    int libraryVersion() const;
    bool libraryScanInProgress() const;
    int libraryRootCount() const;
    int libraryTrackCount() const;
    int libraryArtistCount() const;
    int libraryAlbumCount() const;
    QStringList libraryRoots() const;
    int librarySortMode() const;
    QString fileBrowserName() const;
    int libraryScanRootsCompleted() const;
    int libraryScanRootsTotal() const;
    int libraryScanDiscovered() const;
    int libraryScanProcessed() const;
    double libraryScanFilesPerSecond() const;
    double libraryScanEtaSeconds() const;
    QVariantList globalSearchArtistResults() const;
    QVariantList globalSearchAlbumResults() const;
    QVariantList globalSearchTrackResults() const;
    int globalSearchArtistCount() const;
    int globalSearchAlbumCount() const;
    int globalSearchTrackCount() const;
    quint32 globalSearchSeq() const;
    QObject *globalSearchModel() const;
    QString diagnosticsText() const;
    QString diagnosticsLogPath() const;
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
    Q_INVOKABLE void setLibraryNodeExpanded(const QString &key, bool expanded);
    Q_INVOKABLE void setLibrarySortMode(int mode);
    Q_INVOKABLE void setGlobalSearchQuery(const QString &query);
    Q_INVOKABLE void openInFileBrowser(const QString &path);
    Q_INVOKABLE void openContainingFolder(const QString &path);
    Q_INVOKABLE void scanRoot(const QString &path);
    Q_INVOKABLE void scanDefaultMusicRoot();
    Q_INVOKABLE QVariantMap takeSpectrogramRowsDeltaPacked();
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void clearDiagnostics();
    Q_INVOKABLE void reloadDiagnosticsFromDisk();

signals:
    void snapshotChanged();
    void analysisChanged();
    void libraryTreeFrameReceived(int version, const QByteArray &treeBytes);
    void globalSearchResultsChanged();
    void diagnosticsChanged();
    void connectedChanged();
    void bridgeError(const QString &message);

private:
    struct SearchWorkerInputFrame {
        quint32 seq{0};
        QByteArray payload;
        qint64 ffiPoppedAtMs{0};
        qint64 ffiPopMs{0};
    };

    struct SearchWorkerOutputFrame {
        quint32 seq{0};
        QVariantList artistRows;
        QVariantList albumRows;
        QVariantList trackRows;
        QVector<QVariantMap> displayRows;
        QString decodeError;
        qint64 ffiPoppedAtMs{0};
        qint64 ffiPopMs{0};
        qint64 decodeMs{0};
        qint64 materializeMs{0};
        qint64 workerTotalMs{0};
        quint64 coalescedInputDrops{0};
    };

    bool startInProcessBridge();
    void startSearchApplyWorker();
    void stopSearchApplyWorker();
    void searchApplyWorkerLoop();
    void enqueueSearchFrame(quint32 seq, QByteArray payload, qint64 ffiPopMs);
    bool applyPreparedSearchResultsFrame(SearchWorkerOutputFrame frame);
    void pollInProcessBridge();
    void applyLibraryTreeFrame(int version, const QByteArray &treeBytes);
    bool processBinarySnapshot(const BinaryBridgeCodec::DecodedSnapshot &snapshot);
    void processAnalysisBytes(const QByteArray &chunk);
    bool processSearchResultsFrame(const BinaryBridgeCodec::DecodedSearchResults &frame);
    void flushGlobalSearchQuery();
    void logDiagnostic(const QString &category, const QString &message);
    void appendDiagnosticLine(const QString &line);
    void rebuildDiagnosticsText();
    static QString resolveDiagnosticsLogPath();
    void scheduleSnapshotChanged();
    void scheduleAnalysisChanged();
    static QString detectFileBrowserName();
    bool openUrlInFileBrowser(const QString &path, bool containingFolder) const;
    void sendBinaryCommand(const QByteArray &payload);
    void sendLibraryRootCommand(quint16 cmdId, const QString &path);
    static QString formatSeconds(double seconds);
    static QString formatDurationCompact(double seconds);

    FerrousFfiBridge *m_ffiBridge{nullptr};
    QTimer m_bridgePollTimer;
    QString m_playbackState{"Stopped"};
    QString m_positionText{"00:00"};
    QString m_durationText{"00:00"};
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    double m_volume{1.0};
    int m_queueLength{0};
    int m_queueVersion{0};
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
    QByteArray m_libraryTreeBinary;
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
    int m_libraryArtistCount{0};
    int m_libraryAlbumCount{0};
    QStringList m_libraryRoots;
    int m_librarySortMode{0};
    QString m_fileBrowserName{QStringLiteral("File Manager")};
    int m_libraryScanRootsCompleted{0};
    int m_libraryScanRootsTotal{0};
    int m_libraryScanDiscovered{0};
    int m_libraryScanProcessed{0};
    double m_libraryScanFilesPerSecond{0.0};
    double m_libraryScanEtaSeconds{-1.0};
    QVariantList m_globalSearchArtistResults;
    QVariantList m_globalSearchAlbumResults;
    QVariantList m_globalSearchTrackResults;
    int m_globalSearchArtistCount{0};
    int m_globalSearchAlbumCount{0};
    int m_globalSearchTrackCount{0};
    quint32 m_globalSearchSeq{0};
    GlobalSearchResultsModel m_globalSearchModel;
    quint32 m_nextGlobalSearchSeq{1};
    quint32 m_latestGlobalSearchSeqSent{0};
    QHash<quint32, qint64> m_globalSearchSentAtMs;
    int m_globalSearchDebounceMs{90};
    int m_globalSearchShortDebounceMs{160};
    int m_globalSearchShortDebounceChars{1};
    bool m_publishLegacySearchLists{false};
    QString m_pendingGlobalSearchQuery;
    QString m_lastGlobalSearchQuerySent;
    QString m_diagnosticsText;
    QString m_diagnosticsLogPath;
    QStringList m_diagnosticsLines;
    QString m_libraryLastError;
    QString m_pendingAddRootPath;
    qint64 m_pendingAddRootIssuedMs{0};
    bool m_connected{false};
    bool m_snapshotChangedPending{false};
    bool m_analysisChangedPending{false};
    bool m_pendingSeek{false};
    double m_pendingSeekTargetSeconds{0.0};
    qint64 m_pendingSeekUntilMs{0};
    int m_pendingQueueSelection{-1};
    qint64 m_pendingQueueSelectionUntilMs{0};
    QTimer m_snapshotNotifyTimer;
    QTimer m_analysisNotifyTimer;
    QTimer m_globalSearchDebounceTimer;
    std::thread m_searchApplyThread;
    std::mutex m_searchApplyMutex;
    std::condition_variable m_searchApplyCv;
    bool m_searchApplyStop{false};
    std::optional<SearchWorkerInputFrame> m_searchPendingInputFrame;
    quint64 m_searchInputCoalescedDrops{0};
    quint64 m_searchFramesReceived{0};
    quint64 m_searchFramesApplied{0};
    quint64 m_searchFramesDroppedStale{0};
    quint64 m_searchFramesDecodeErrors{0};
    QByteArray m_analysisBuffer;
    qsizetype m_analysisBufferReadOffset{0};
    bool m_hasAnalysisFrameSeq{false};
    quint32 m_lastAnalysisFrameSeq{0};
    quint64 m_analysisDroppedFrames{0};
};
