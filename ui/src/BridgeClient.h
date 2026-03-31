// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QSet>
#include <QVariant>
#include <QVariantMap>
#include <QVariantList>
#include <QVector>

#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "BinaryBridgeCodec.h"
#include "GlobalSearchResultsModel.h"

struct FerrousFfiBridge;
class QNetworkReply;
class QSocketNotifier;
class QTemporaryDir;

struct QueueRowData {
    QString title;
    QString artist;
    QString album;
    QString coverPath;
    QString genre;
    QString lengthText;
    QString path;
    int trackNumber{0};
    int year{std::numeric_limits<int>::min()};

    bool operator==(const QueueRowData &other) const = default;
};

class QueueRowsModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        AlbumRole,
        CoverPathRole,
        GenreRole,
        LengthTextRole,
        PathRole,
        TrackNumberRole,
        YearRole,
    };

    explicit QueueRowsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool setRows(QVector<QueueRowData> rows);
    const QueueRowData *rowAt(int index) const;
    QVariant trackNumberAt(int index) const;

private:
    QVector<QueueRowData> m_rows;
};

class BridgeClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playbackChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY playbackChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY playbackChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY playbackChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY playbackChanged)
    Q_PROPERTY(double volume READ volume NOTIFY snapshotChanged)
    Q_PROPERTY(int queueLength READ queueLength NOTIFY snapshotChanged)
    Q_PROPERTY(int queueVersion READ queueVersion NOTIFY snapshotChanged)
    Q_PROPERTY(QString queueDurationText READ queueDurationText NOTIFY snapshotChanged)
    Q_PROPERTY(QObject* queueRows READ queueRows CONSTANT)
    Q_PROPERTY(int selectedQueueIndex READ selectedQueueIndex NOTIFY snapshotChanged)
    Q_PROPERTY(int playingQueueIndex READ playingQueueIndex NOTIFY trackIdentityChanged)
    Q_PROPERTY(QString currentTrackPath READ currentTrackPath NOTIFY trackIdentityChanged)
    Q_PROPERTY(QString currentTrackCoverPath READ currentTrackCoverPath NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackTitle READ currentTrackTitle NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackArtist READ currentTrackArtist NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackAlbum READ currentTrackAlbum NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackGenre READ currentTrackGenre NOTIFY trackMetadataChanged)
    Q_PROPERTY(QVariant currentTrackYear READ currentTrackYear NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackNumber READ currentTrackNumber NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackFormatLabel READ currentTrackFormatLabel NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackChannelLayoutText READ currentTrackChannelLayoutText NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackChannelLayoutIconKey READ currentTrackChannelLayoutIconKey NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackSampleRateHz READ currentTrackSampleRateHz NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackBitDepth READ currentTrackBitDepth NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackCurrentBitrateKbps READ currentTrackCurrentBitrateKbps NOTIFY trackMetadataChanged)
    Q_PROPERTY(QByteArray waveformPeaksPacked READ waveformPeaksPacked NOTIFY analysisChanged)
    Q_PROPERTY(double waveformCoverageSeconds READ waveformCoverageSeconds NOTIFY analysisChanged)
    Q_PROPERTY(bool waveformComplete READ waveformComplete NOTIFY analysisChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY analysisChanged)
    Q_PROPERTY(int fftSize READ fftSize NOTIFY snapshotChanged)
    Q_PROPERTY(int spectrogramViewMode READ spectrogramViewMode NOTIFY snapshotChanged)
    Q_PROPERTY(int spectrogramDisplayMode READ spectrogramDisplayMode NOTIFY snapshotChanged)
    Q_PROPERTY(int viewerFullscreenMode READ viewerFullscreenMode NOTIFY snapshotChanged)
    Q_PROPERTY(double dbRange READ dbRange NOTIFY snapshotChanged)
    Q_PROPERTY(bool logScale READ logScale NOTIFY snapshotChanged)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY snapshotChanged)
    Q_PROPERTY(bool shuffleEnabled READ shuffleEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(quint64 mutedChannelsMask READ mutedChannelsMask NOTIFY playbackChanged)
    Q_PROPERTY(bool showFps READ showFps NOTIFY snapshotChanged)
    Q_PROPERTY(bool showSpectrogramCrosshair READ showSpectrogramCrosshair NOTIFY snapshotChanged)
    Q_PROPERTY(bool showSpectrogramScale READ showSpectrogramScale NOTIFY snapshotChanged)
    Q_PROPERTY(bool systemMediaControlsEnabled READ systemMediaControlsEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(bool lastFmScrobblingEnabled READ lastFmScrobblingEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(bool lastFmBuildConfigured READ lastFmBuildConfigured NOTIFY snapshotChanged)
    Q_PROPERTY(QString lastFmUsername READ lastFmUsername NOTIFY snapshotChanged)
    Q_PROPERTY(int lastFmAuthState READ lastFmAuthState NOTIFY snapshotChanged)
    Q_PROPERTY(int lastFmPendingScrobbleCount READ lastFmPendingScrobbleCount NOTIFY snapshotChanged)
    Q_PROPERTY(QString lastFmStatusText READ lastFmStatusText NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryAlbums READ libraryAlbums NOTIFY snapshotChanged)
    Q_PROPERTY(QByteArray libraryTreeBinary READ libraryTreeBinary NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryVersion READ libraryVersion NOTIFY snapshotChanged)
    Q_PROPERTY(bool libraryScanInProgress READ libraryScanInProgress NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryRootCount READ libraryRootCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryTrackCount READ libraryTrackCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryArtistCount READ libraryArtistCount NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryAlbumCount READ libraryAlbumCount NOTIFY snapshotChanged)
    Q_PROPERTY(QStringList libraryRoots READ libraryRoots NOTIFY snapshotChanged)
    Q_PROPERTY(QVariantList libraryRootEntries READ libraryRootEntries NOTIFY snapshotChanged)
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
    Q_PROPERTY(QVariantList itunesArtworkResults READ itunesArtworkResults NOTIFY itunesArtworkChanged)
    Q_PROPERTY(bool itunesArtworkLoading READ itunesArtworkLoading NOTIFY itunesArtworkChanged)
    Q_PROPERTY(QString itunesArtworkStatusText READ itunesArtworkStatusText NOTIFY itunesArtworkChanged)
    Q_PROPERTY(QString diagnosticsText READ diagnosticsText NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticsLogPath READ diagnosticsLogPath NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool profileLogsEnabled READ profileLogsEnabled CONSTANT)

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
    QObject *queueRows() const;
    int selectedQueueIndex() const;
    int playingQueueIndex() const;
    QString currentTrackPath() const;
    QString currentTrackCoverPath() const;
    QString currentTrackTitle() const;
    QString currentTrackArtist() const;
    QString currentTrackAlbum() const;
    QString currentTrackGenre() const;
    QVariant currentTrackYear() const;
    int currentTrackNumber() const;
    QString currentTrackFormatLabel() const;
    QString currentTrackChannelLayoutText() const;
    QString currentTrackChannelLayoutIconKey() const;
    int currentTrackSampleRateHz() const;
    int currentTrackBitDepth() const;
    int currentTrackCurrentBitrateKbps() const;
    QByteArray waveformPeaksPacked() const;
    double waveformCoverageSeconds() const;
    bool waveformComplete() const;
    int sampleRateHz() const;
    int fftSize() const;
    int spectrogramViewMode() const;
    int spectrogramDisplayMode() const;
    int viewerFullscreenMode() const;
    double dbRange() const;
    bool logScale() const;
    int repeatMode() const;
    bool shuffleEnabled() const;
    quint64 mutedChannelsMask() const;
    bool showFps() const;
    bool showSpectrogramCrosshair() const;
    bool showSpectrogramScale() const;
    bool systemMediaControlsEnabled() const;
    bool lastFmScrobblingEnabled() const;
    bool lastFmBuildConfigured() const;
    QString lastFmUsername() const;
    int lastFmAuthState() const;
    int lastFmPendingScrobbleCount() const;
    QString lastFmStatusText() const;
    QStringList libraryAlbums() const;
    QByteArray libraryTreeBinary() const;
    int libraryVersion() const;
    bool libraryScanInProgress() const;
    int libraryRootCount() const;
    int libraryTrackCount() const;
    int libraryArtistCount() const;
    int libraryAlbumCount() const;
    QStringList libraryRoots() const;
    QVariantList libraryRootEntries() const;
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
    QVariantList itunesArtworkResults() const;
    bool itunesArtworkLoading() const;
    QString itunesArtworkStatusText() const;
    QString diagnosticsText() const;
    QString diagnosticsLogPath() const;
    bool connected() const;
    bool profileLogsEnabled() const { return m_profileUiEnabled; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void setVolume(double value);
    Q_INVOKABLE void setFftSize(int value);
    Q_INVOKABLE void setSpectrogramViewMode(int value);
    Q_INVOKABLE void setSpectrogramDisplayMode(int value);
    Q_INVOKABLE void setViewerFullscreenMode(int value);
    Q_INVOKABLE void setDbRange(double value);
    Q_INVOKABLE void setLogScale(bool value);
    Q_INVOKABLE void setRepeatMode(int mode);
    Q_INVOKABLE void setShuffleEnabled(bool value);
    Q_INVOKABLE void toggleChannelMute(int channelIndex);
    Q_INVOKABLE void soloChannel(int channelIndex);
    Q_INVOKABLE void setShowFps(bool value);
    Q_INVOKABLE void setShowSpectrogramCrosshair(bool value);
    Q_INVOKABLE void setShowSpectrogramScale(bool value);
    Q_INVOKABLE void setSystemMediaControlsEnabled(bool value);
    Q_INVOKABLE void setLastFmScrobblingEnabled(bool value);
    Q_INVOKABLE void beginLastFmAuth();
    Q_INVOKABLE void completeLastFmAuth();
    Q_INVOKABLE void disconnectLastFm();
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
    Q_INVOKABLE void replaceRootByPath(const QString &rootPath);
    Q_INVOKABLE void appendRootByPath(const QString &rootPath);
    Q_INVOKABLE void replaceAllLibraryTracks();
    Q_INVOKABLE void appendAllLibraryTracks();
    Q_INVOKABLE void replaceWithPaths(const QStringList &paths);
    Q_INVOKABLE void appendPaths(const QStringList &paths);
    Q_INVOKABLE QString libraryAlbumCoverAt(int index) const;
    Q_INVOKABLE QString libraryThumbnailSource(const QString &path) const;
    Q_INVOKABLE QString queuePathAt(int index) const;
    Q_INVOKABLE QVariant queueTrackNumberAt(int index) const;
    Q_INVOKABLE void addLibraryRoot(const QString &path, const QString &name = QString());
    Q_INVOKABLE void setLibraryRootName(const QString &path, const QString &name);
    Q_INVOKABLE void removeLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanAllLibraryRoots();
    Q_INVOKABLE void setLibraryNodeExpanded(const QString &key, bool expanded);
    Q_INVOKABLE void setLibrarySortMode(int mode);
    Q_INVOKABLE void setGlobalSearchQuery(const QString &query);
    Q_INVOKABLE void searchCurrentTrackArtworkSuggestions();
    Q_INVOKABLE void clearItunesArtworkSuggestions();
    Q_INVOKABLE QVariantMap itunesArtworkResultAt(int index) const;
    Q_INVOKABLE void prepareItunesArtworkSuggestion(int index);
    Q_INVOKABLE void applyItunesArtworkSuggestion(int index);
    Q_INVOKABLE void openInFileBrowser(const QString &path);
    Q_INVOKABLE void openContainingFolder(const QString &path);
    Q_INVOKABLE void refreshEditedPaths(const QStringList &paths);
    Q_INVOKABLE void requestImageFileDetails(const QString &path);
    Q_INVOKABLE QVariantMap cachedImageFileDetails(const QString &path) const;
    Q_INVOKABLE QVariantMap imageFileDetails(const QString &path) const;
    Q_INVOKABLE void scanRoot(const QString &path);
    Q_INVOKABLE void scanDefaultMusicRoot();
    Q_INVOKABLE void requestSnapshot();
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void clearDiagnostics();
    Q_INVOKABLE void reloadDiagnosticsFromDisk();
    QByteArray renameEditedFiles(const QByteArray &payload);

signals:
    void playbackChanged();
    void trackIdentityChanged();
    void trackMetadataChanged();
    void snapshotChanged();
    void analysisChanged();
    void precomputedSpectrogramChunkReady(
        const QByteArray &data, int bins, int channelCount, int columns,
        int startIndex, int totalEstimate, int sampleRate, int hopSize,
        float coverage, bool complete, bool bufferReset, bool clearHistory,
        quint64 trackToken);
    void libraryTreeFrameReceived(int version, const QByteArray &treeBytes);
    void globalSearchResultsChanged();
    void itunesArtworkChanged();
    void imageFileDetailsChanged(const QString &path);
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
        int artistCount{0};
        int albumCount{0};
        int trackCount{0};
        QVector<GlobalSearchResultsModel::SearchDisplayRow> displayRows;
        QString decodeError;
        qint64 ffiPoppedAtMs{0};
        qint64 ffiPopMs{0};
        qint64 decodeMs{0};
        qint64 materializeMs{0};
        qint64 workerTotalMs{0};
        quint64 coalescedInputDrops{0};
        quint64 coalescedOutputDrops{0};
    };

    struct ItunesArtworkCandidate {
        QString albumTitle;
        QString artistName;
        QString collectionUrl;
        QString previewUrl;
        QStringList assetUrls;
        double relevanceScore{0.0};
        int apiOrder{0};
    };

    struct ItunesArtworkAssetJobResult {
        quint64 generation{0};
        int candidateIndex{-1};
        int assetUrlIndex{0};
        bool usedFallback{false};
        bool success{false};
        QString errorMessage;
        QString normalizedPath;
        QString downloadPath;
        QVariantMap imageDetails;
    };

    struct BridgePollRunResult {
        int processedAnalysisFrames{0};
        int processedTreeFrames{0};
        int processedSearchFrames{0};
        int processedEvents{0};
        qsizetype processedAnalysisBytes{0};
        bool analysisCapSaturated{false};
        bool treeCapSaturated{false};
        bool searchCapSaturated{false};
        bool eventCapSaturated{false};
        bool budgetExhausted{false};

        bool anyWorkProcessed() const {
            return processedAnalysisFrames > 0
                || processedTreeFrames > 0
                || processedSearchFrames > 0
                || processedEvents > 0;
        }

        bool shouldContinueImmediately() const {
            return budgetExhausted
                || analysisCapSaturated
                || treeCapSaturated
                || searchCapSaturated
                || eventCapSaturated;
        }
    };

    bool startInProcessBridge();
    void startSearchApplyWorker();
    void stopSearchApplyWorker();
    void searchApplyWorkerLoop();
    void enqueueSearchFrame(quint32 seq, QByteArray payload, qint64 ffiPopMs);
    void queuePreparedSearchResultsFrame(SearchWorkerOutputFrame frame);
    void scheduleSearchApplyDispatch();
    void dispatchPendingSearchApplyFrame();
    int searchApplyDispatchDelayMs() const;
    bool applyPreparedSearchResultsFrame(SearchWorkerOutputFrame frame);
    void applyDeferredSearchDisplayRows();
    void startCoverLookupWorker();
    void stopCoverLookupWorker();
    void requestTrackCoverLookup(const QString &trackPath);
    void coverLookupWorkerLoop();
    void applyTrackCoverLookupResult(const QString &trackPath, const QString &coverUrl);
    void cacheTrackCoverForPath(const QString &trackPath, const QString &coverUrl);
    void rebuildQueuePathFirstIndex();
    int queuePathFirstIndex(const QString &path) const;
    QString coverUrlForPath(const QString &path) const;
    void bumpCoverRefreshNonce(const QString &path);
    void cancelItunesArtworkRequests();
    static ItunesArtworkAssetJobResult processItunesArtworkAssetPayload(
        const QByteArray &payload,
        const QString &tempDirPath,
        quint64 generation,
        int candidateIndex,
        int assetUrlIndex);
    void startItunesArtworkAssetDownload(
        int candidateIndex,
        int assetUrlIndex = 0);
    void applyItunesArtworkAssetJobResult(ItunesArtworkAssetJobResult result);
    void resetItunesArtworkTempDir();
    bool ensureItunesArtworkTempDir();
    void updateItunesArtworkResult(int index, const QVariantMap &row);
    void startFileBrowserNameDetection();
    void applyDetectedFileBrowserName(const QString &name);
    void applyImageFileDetailsResult(const QString &requestedPath, QVariantMap details);
    void cacheImageFileDetails(const QString &requestedPath, const QVariantMap &details);
    void scheduleBridgePoll(int delayMs);
    BridgePollRunResult drainBridgeQueues(qint64 budgetMs);
    void pollInProcessBridge();
    void applyLibraryTreeFrame(int version, const QByteArray &treeBytes);
    bool processBinarySnapshot(const BinaryBridgeCodec::DecodedSnapshot &snapshot);
    void processAnalysisBytes(const QByteArray &chunk);
    void parsePrecomputedSpectrogramFrame(const QByteArray &raw);
    bool processSearchResultsFrame(const BinaryBridgeCodec::DecodedSearchResults &frame);
    void flushGlobalSearchQuery();
    void logDiagnostic(const QString &category, const QString &message);
    void appendDiagnosticLine(const QString &line);
    void flushPendingDiagnosticDiskLines();
    void rebuildDiagnosticsText();
    static QString resolveDiagnosticsLogPath();
    void schedulePlaybackChanged();
    void scheduleTrackIdentityChanged();
    void scheduleTrackMetadataChanged();
    void scheduleSnapshotChanged();
    void scheduleAnalysisChanged();
    void shutdownBridgeGracefully();
    static QString detectFileBrowserNameHeuristic();
    static QString detectFileBrowserName();
    bool openUrlInFileBrowser(const QString &path, bool containingFolder) const;
    void sendBinaryCommand(const QByteArray &payload);
    void sendLibraryRootCommand(quint16 cmdId, const QString &path);
    void sendLibraryRootCommand(quint16 cmdId, const QString &path, const QString &name);
    static QString formatSeconds(double seconds);
    static QString formatDurationCompact(double seconds);

    FerrousFfiBridge *m_ffiBridge{nullptr};
    QSocketNotifier *m_bridgeWakeNotifier{nullptr};
    int m_bridgeWakeFd{-1};
    QTimer m_bridgePollTimer;
    int m_bridgePollBudgetMs{5};
    QString m_playbackState{"Stopped"};
    QString m_positionText{"00:00"};
    QString m_durationText{"00:00"};
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    double m_volume{1.0};
    int m_queueLength{0};
    int m_queueVersion{0};
    QString m_queueDurationText{"00:00"};
    QueueRowsModel m_queueRowsModel;
    QStringList m_queuePaths;
    QHash<QString, int> m_queuePathFirstIndex;
    int m_selectedQueueIndex{-1};
    int m_playingQueueIndex{-1};
    QString m_currentTrackPath;
    QString m_currentTrackCoverPath;
    QString m_currentTrackTitle;
    QString m_currentTrackArtist;
    QString m_currentTrackAlbum;
    QString m_currentTrackGenre;
    QVariant m_currentTrackYear;
    int m_currentTrackNumber{0};
    QString m_currentTrackFormatLabel;
    int m_currentTrackChannels{0};
    int m_currentTrackSampleRateHz{0};
    int m_currentTrackBitDepth{0};
    int m_currentTrackCurrentBitrateKbps{0};
    QByteArray m_waveformPeaksPacked;
    double m_waveformCoverageSeconds{0.0};
    bool m_waveformComplete{false};
    int m_sampleRateHz{48000};
    int m_fftSize{8192};
    int m_spectrogramViewMode{0};
    int m_spectrogramDisplayMode{0};
    int m_viewerFullscreenMode{0};
    double m_dbRange{132.0};
    bool m_logScale{false};
    int m_repeatMode{0};
    bool m_shuffleEnabled{false};
    quint64 m_mutedChannelsMask{0};
    bool m_showFps{false};
    bool m_showSpectrogramCrosshair{false};
    bool m_showSpectrogramScale{false};
    bool m_systemMediaControlsEnabled{true};
    bool m_lastFmScrobblingEnabled{false};
    bool m_lastFmBuildConfigured{false};
    QString m_lastFmUsername;
    int m_lastFmAuthState{0};
    int m_lastFmPendingScrobbleCount{0};
    QString m_lastFmStatusText;
    QString m_lastFmAuthUrl;
    QString m_lastOpenedExternalUrl;
    QStringList m_libraryAlbums;
    QByteArray m_libraryTreeBinary;
    int m_libraryVersion{0};
    QStringList m_libraryAlbumArtists;
    QStringList m_libraryAlbumNames;
    QStringList m_libraryAlbumCoverPaths;
    QList<QStringList> m_libraryAlbumTrackPaths;
    QHash<QString, QString> m_trackCoverByPath;
    QHash<QString, QString> m_trackCoverByDirectory;
    mutable QHash<QString, QString> m_coverUrlCacheByPath;
    mutable QHash<QString, QString> m_coverCanonicalPathCacheByPath;
    mutable QHash<QString, QString> m_libraryThumbnailSourceCache;
    bool m_libraryScanInProgress{false};
    int m_libraryRootCount{0};
    int m_libraryTrackCount{0};
    int m_libraryArtistCount{0};
    int m_libraryAlbumCount{0};
    QStringList m_libraryRoots;
    QVariantList m_libraryRootEntries;
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
    QHash<QString, quint64> m_coverRefreshNonceByPath;
    quint64 m_nextCoverRefreshNonce{1};
    int m_globalSearchDebounceMs{90};
    int m_globalSearchShortDebounceMs{160};
    int m_globalSearchShortDebounceChars{1};
    bool m_publishLegacySearchLists{false};
    QString m_pendingGlobalSearchQuery;
    QString m_lastGlobalSearchQuerySent;
    QVector<ItunesArtworkCandidate> m_itunesArtworkCandidates;
    QVariantList m_itunesArtworkResults;
    bool m_itunesArtworkLoading{false};
    QString m_itunesArtworkStatusText;
    quint64 m_itunesArtworkGeneration{0};
    QNetworkAccessManager m_itunesArtworkNetwork;
    QSet<QNetworkReply *> m_itunesArtworkReplies;
    std::unique_ptr<QTemporaryDir> m_itunesArtworkTempDir;
    QHash<QString, QVariantMap> m_imageFileDetailsCache;
    QSet<QString> m_pendingImageFileDetailsPaths;
    QString m_diagnosticsText;
    QString m_diagnosticsLogPath;
    QStringList m_diagnosticsLines;
    QStringList m_pendingDiagnosticsDiskLines;
    QString m_libraryLastError;
    QString m_pendingAddRootPath;
    qint64 m_pendingAddRootIssuedMs{0};
    bool m_connected{false};
    bool m_loggedStartupQueueMissing{false};
    bool m_loggedStartupQueuePresent{false};
    bool m_playbackChangedPending{false};
    bool m_trackIdentityChangedPending{false};
    bool m_trackMetadataChangedPending{false};
    bool m_snapshotChangedPending{false};
    bool m_analysisChangedPending{false};
    bool m_pollPlaybackChanged{false};
    bool m_pollTrackIdentityChanged{false};
    bool m_pollTrackMetadataChanged{false};
    bool m_pollSnapshotChanged{false};
    bool m_pendingSeek{false};
    double m_pendingSeekTargetSeconds{0.0};
    qint64 m_pendingSeekStartedAtMs{0};
    qint64 m_pendingSeekUntilMs{0};
    int m_pendingQueueSelection{-1};
    qint64 m_pendingQueueSelectionUntilMs{0};
    QTimer m_diagnosticsFlushTimer;
    QTimer m_snapshotNotifyTimer;
    QTimer m_globalSearchDebounceTimer;
    QTimer m_searchApplyDispatchTimer;
    int m_searchApplyDispatchMs{12};
    QTimer m_searchModelApplyTimer;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> m_deferredSearchDisplayRows;
    std::thread m_searchApplyThread;
    mutable std::mutex m_searchApplyMutex;
    std::condition_variable m_searchApplyCv;
    bool m_searchApplyStop{false};
    std::optional<SearchWorkerInputFrame> m_searchPendingInputFrame;
    quint64 m_searchInputCoalescedDrops{0};
    mutable std::mutex m_searchOutputMutex;
    std::optional<SearchWorkerOutputFrame> m_searchPendingOutputFrame;
    quint64 m_searchOutputCoalescedDrops{0};
    std::thread m_coverLookupThread;
    std::mutex m_coverLookupMutex;
    std::condition_variable m_coverLookupCv;
    bool m_coverLookupStop{false};
    std::optional<QString> m_coverLookupPendingPath;
    QString m_coverLookupInFlightPath;
    QString m_pendingAppliedArtworkTrackPath;
    quint64 m_searchFramesReceived{0};
    quint64 m_searchFramesApplied{0};
    quint64 m_searchFramesDroppedStale{0};
    quint64 m_searchFramesDecodeErrors{0};
    bool m_profileUiEnabled{false};
    qint64 m_lastBridgePollProfileLogMs{0};
    qint64 m_lastAnalysisProfileLogMs{0};
    qint64 m_lastSnapshotApplyProfileLogMs{0};
    qint64 m_lastUiStallProfileLogMs{0};
    QTimer m_uiStallWatchdogTimer;
    QElapsedTimer m_uiStallWatchdogElapsed;
    qint64 m_uiStallWatchdogLastTickMs{0};
    QByteArray m_analysisBuffer;
    qsizetype m_analysisBufferReadOffset{0};
    bool m_hasAnalysisFrameSeq{false};
    quint32 m_lastAnalysisFrameSeq{0};
    quint64 m_analysisDroppedFrames{0};
};
