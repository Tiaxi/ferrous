// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QByteArray>
#include <QList>
#include <QRect>
#include <QSize>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "BinaryBridgeCodec.h"
#include "GlobalSearchResultsModel.h"
#include "PlaybackClock.h"
#include "QueueRows.h"

struct FerrousFfiBridge;
struct FerrousFfiRenameGate;
class KLocalizedString;
class QNetworkReply;
class QSocketNotifier;
class QTemporaryDir;
class QQuickWindow;
namespace DiagnosticsLog {
class Writer;
}

class BridgeClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playbackChanged)
    // Where playback is; see PlaybackClock.
    Q_PROPERTY(PlaybackClock *playbackClock READ playbackClock CONSTANT)
    Q_PROPERTY(QString durationText READ durationText NOTIFY playbackChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY playbackChanged)
    Q_PROPERTY(double volume READ volume NOTIFY snapshotChanged)
    Q_PROPERTY(int queueLength READ queueLength NOTIFY snapshotChanged)
    Q_PROPERTY(QString queueDurationText READ queueDurationText NOTIFY snapshotChanged)
    Q_PROPERTY(QObject* queueRows READ queueRows CONSTANT)
    Q_PROPERTY(int selectedQueueIndex READ selectedQueueIndex NOTIFY snapshotChanged)
    // The queue change the user can undo: an ID that changes with each new
    // change (0 for none) and its kind (BinaryBridgeCodec::QueueUndoKind),
    // which names it in the Edit menu.
    Q_PROPERTY(double queueUndoId READ queueUndoId NOTIFY queueUndoChanged)
    Q_PROPERTY(int queueUndoKind READ queueUndoKind NOTIFY queueUndoChanged)
    Q_PROPERTY(int playingQueueIndex READ playingQueueIndex NOTIFY trackIdentityChanged)
    Q_PROPERTY(QString currentTrackPath READ currentTrackPath NOTIFY trackIdentityChanged)
    // Changes whenever a play starts: on every track change, also when the
    // same file plays again, and when a stopped track plays again. A seek, a
    // pause or a queue edit that keeps the playing track leaves it.
    Q_PROPERTY(double currentPlaySerial READ currentPlaySerial NOTIFY trackIdentityChanged)
    Q_PROPERTY(QString currentTrackCoverPath READ currentTrackCoverPath NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackTitle READ currentTrackTitle NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackArtist READ currentTrackArtist NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackAlbum READ currentTrackAlbum NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackAlbumArtist READ currentTrackAlbumArtist NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackGenre READ currentTrackGenre NOTIFY trackMetadataChanged)
    Q_PROPERTY(QVariant currentTrackYear READ currentTrackYear NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackNumber READ currentTrackNumber NOTIFY trackMetadataChanged)
    // The tracks on the current track's album, when the backend knows them
    // surely for its number; 0 otherwise.
    Q_PROPERTY(int currentTrackTotal READ currentTrackTotal NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackFormatLabel READ currentTrackFormatLabel NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackChannelLayoutText READ currentTrackChannelLayoutText NOTIFY trackMetadataChanged)
    Q_PROPERTY(QString currentTrackChannelLayoutIconKey READ currentTrackChannelLayoutIconKey NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackChannels READ currentTrackChannels NOTIFY trackMetadataChanged)
    Q_PROPERTY(QStringList currentTrackChannelLabels READ currentTrackChannelLabels NOTIFY trackMetadataChanged)
    // The keys of the library tree rows that hold the current track,
    // outermost first, as the backend groups the tree; empty when the
    // library does not have the track. The tree marks them while collapsed.
    Q_PROPERTY(QStringList currentTrackTreeKeys READ currentTrackTreeKeys NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackSampleRateHz READ currentTrackSampleRateHz NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackBitDepth READ currentTrackBitDepth NOTIFY trackMetadataChanged)
    Q_PROPERTY(int currentTrackCurrentBitrateKbps READ currentTrackCurrentBitrateKbps NOTIFY trackMetadataChanged)
    Q_PROPERTY(QByteArray waveformPeaksPacked READ waveformPeaksPacked NOTIFY analysisChanged)
    Q_PROPERTY(double waveformCoverageSeconds READ waveformCoverageSeconds NOTIFY analysisChanged)
    Q_PROPERTY(bool waveformComplete READ waveformComplete NOTIFY analysisChanged)
    // Whether the current track's file is cut off: it ends at
    // waveformCoverageSeconds, before the length its header gives.
    Q_PROPERTY(bool waveformEndsEarly READ waveformEndsEarly NOTIFY analysisChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY analysisChanged)
    Q_PROPERTY(int fftSize READ fftSize NOTIFY snapshotChanged)
    Q_PROPERTY(int spectrogramViewMode READ spectrogramViewMode NOTIFY snapshotChanged)
    Q_PROPERTY(int spectrogramDisplayMode READ spectrogramDisplayMode NOTIFY snapshotChanged)
    Q_PROPERTY(int viewerFullscreenMode READ viewerFullscreenMode NOTIFY snapshotChanged)
    Q_PROPERTY(bool preventDisplaySleepInFullscreen READ preventDisplaySleepInFullscreen NOTIFY snapshotChanged)
    Q_PROPERTY(bool showFullscreenTitleStrip READ showFullscreenTitleStrip NOTIFY snapshotChanged)
    Q_PROPERTY(double dbRange READ dbRange NOTIFY snapshotChanged)
    Q_PROPERTY(bool logScale READ logScale NOTIFY snapshotChanged)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY snapshotChanged)
    Q_PROPERTY(bool shuffleEnabled READ shuffleEnabled NOTIFY snapshotChanged)
    // Whether each channel, 0 to 63, is muted. QML numbers hold 53 bits, so
    // the 64-bit mask itself stays in C++.
    Q_PROPERTY(QList<bool> mutedChannels READ mutedChannels NOTIFY playbackChanged)
    Q_PROPERTY(QList<bool> soloedChannels READ soloedChannels NOTIFY playbackChanged)
    Q_PROPERTY(int channelButtonsVisibility READ channelButtonsVisibility NOTIFY snapshotChanged)
    Q_PROPERTY(bool showFps READ showFps NOTIFY snapshotChanged)
    Q_PROPERTY(bool showLevelMeter READ showLevelMeter NOTIFY snapshotChanged)
    Q_PROPERTY(bool showSpectrogramCrosshair READ showSpectrogramCrosshair NOTIFY snapshotChanged)
    Q_PROPERTY(bool showSpectrogramScale READ showSpectrogramScale NOTIFY snapshotChanged)
    Q_PROPERTY(bool spectrogramZoomEnabled READ spectrogramZoomEnabled NOTIFY snapshotChanged)
    // Whether the spectrogram shows only the audible band of the track's
    // rate family: up to 22.05 kHz for the 44.1 kHz family, 24 kHz for
    // other rates, or Nyquist if that is lower.
    Q_PROPERTY(bool spectrogramFrequencyCap READ spectrogramFrequencyCap NOTIFY snapshotChanged)
    Q_PROPERTY(bool systemMediaControlsEnabled READ systemMediaControlsEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(bool libraryWatchEnabled READ libraryWatchEnabled NOTIFY snapshotChanged)
    // The widths of the queue's columns the user resized, in logical pixels,
    // under "artist", "album" and "length"; a column that is not there has
    // its default width.
    Q_PROPERTY(QVariantMap queueColumnWidths READ queueColumnWidths NOTIFY queueColumnWidthsChanged)
    Q_PROPERTY(bool lastFmScrobblingEnabled READ lastFmScrobblingEnabled NOTIFY snapshotChanged)
    Q_PROPERTY(bool lastFmBuildConfigured READ lastFmBuildConfigured NOTIFY snapshotChanged)
    Q_PROPERTY(QString lastFmUsername READ lastFmUsername NOTIFY snapshotChanged)
    Q_PROPERTY(int lastFmAuthState READ lastFmAuthState NOTIFY snapshotChanged)
    Q_PROPERTY(int lastFmPendingScrobbleCount READ lastFmPendingScrobbleCount NOTIFY snapshotChanged)
    Q_PROPERTY(QString lastFmStatusText READ lastFmStatusText NOTIFY snapshotChanged)
    // Whether the status bar shows the scrobble progress indicator.
    Q_PROPERTY(bool lastFmShowScrobbleProgress READ lastFmShowScrobbleProgress NOTIFY snapshotChanged)
    // Whether scrobbling is paused until the app quits.
    Q_PROPERTY(bool lastFmScrobblingPaused READ lastFmScrobblingPaused NOTIFY lastFmScrobbleChanged)
    // The current play's scrobble progress, as the scrobbler counts it: a
    // BinaryBridgeCodec::ScrobbleState and ScrobbleIneligibility, the time
    // listened and the scrobble point (0 while unknown), and why the
    // scrobble waits or failed.
    Q_PROPERTY(int lastFmScrobbleState READ lastFmScrobbleState NOTIFY lastFmScrobbleChanged)
    Q_PROPERTY(int lastFmScrobbleIneligibility READ lastFmScrobbleIneligibility NOTIFY lastFmScrobbleChanged)
    Q_PROPERTY(int lastFmScrobbleListenedMs READ lastFmScrobbleListenedMs NOTIFY lastFmScrobbleChanged)
    Q_PROPERTY(int lastFmScrobbleThresholdMs READ lastFmScrobbleThresholdMs NOTIFY lastFmScrobbleChanged)
    Q_PROPERTY(QString lastFmScrobbleMessage READ lastFmScrobbleMessage NOTIFY lastFmScrobbleChanged)
    Q_PROPERTY(QStringList libraryAlbums READ libraryAlbums NOTIFY libraryChanged)
    Q_PROPERTY(QByteArray libraryTreeBinary READ libraryTreeBinary NOTIFY libraryChanged)
    Q_PROPERTY(int libraryVersion READ libraryVersion NOTIFY libraryChanged)
    Q_PROPERTY(bool libraryScanInProgress READ libraryScanInProgress NOTIFY libraryScanChanged)
    Q_PROPERTY(int libraryRootCount READ libraryRootCount NOTIFY libraryChanged)
    Q_PROPERTY(int libraryTrackCount READ libraryTrackCount NOTIFY libraryChanged)
    Q_PROPERTY(int libraryArtistCount READ libraryArtistCount NOTIFY libraryChanged)
    Q_PROPERTY(int libraryAlbumCount READ libraryAlbumCount NOTIFY libraryChanged)
    Q_PROPERTY(QStringList libraryExpandedKeys READ libraryExpandedKeys NOTIFY libraryChanged)
    Q_PROPERTY(bool libraryViewStateAvailable READ libraryViewStateAvailable NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryViewSelectionKey READ libraryViewSelectionKey NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryViewAnchorKey READ libraryViewAnchorKey NOTIFY libraryChanged)
    Q_PROPERTY(double libraryViewAnchorOffset READ libraryViewAnchorOffset NOTIFY libraryChanged)
    Q_PROPERTY(QStringList libraryRoots READ libraryRoots NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList libraryRootEntries READ libraryRootEntries NOTIFY libraryChanged)
    Q_PROPERTY(int librarySortMode READ librarySortMode NOTIFY libraryChanged)
    Q_PROPERTY(QString fileBrowserName READ fileBrowserName NOTIFY snapshotChanged)
    Q_PROPERTY(int libraryScanRootsCompleted READ libraryScanRootsCompleted NOTIFY libraryScanChanged)
    Q_PROPERTY(int libraryScanRootsTotal READ libraryScanRootsTotal NOTIFY libraryScanChanged)
    Q_PROPERTY(int libraryScanDiscovered READ libraryScanDiscovered NOTIFY libraryScanChanged)
    Q_PROPERTY(int libraryScanProcessed READ libraryScanProcessed NOTIFY libraryScanChanged)
    Q_PROPERTY(int libraryScanTotal READ libraryScanTotal NOTIFY libraryScanChanged)
    Q_PROPERTY(double libraryScanFilesPerSecond READ libraryScanFilesPerSecond NOTIFY libraryScanChanged)
    Q_PROPERTY(double libraryScanEtaSeconds READ libraryScanEtaSeconds NOTIFY libraryScanChanged)
    Q_PROPERTY(QVariantList globalSearchArtistResults READ globalSearchArtistResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QVariantList globalSearchAlbumResults READ globalSearchAlbumResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QVariantList globalSearchTrackResults READ globalSearchTrackResults NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchArtistCount READ globalSearchArtistCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchAlbumCount READ globalSearchAlbumCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(int globalSearchTrackCount READ globalSearchTrackCount NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(quint32 globalSearchSeq READ globalSearchSeq NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(bool globalSearchModelRetained READ globalSearchModelRetained NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(bool globalSearchBusy MEMBER m_globalSearchBusy NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QVariantList globalSearchTotals MEMBER m_globalSearchTotals NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(bool globalSearchCanExpand READ globalSearchCanExpand NOTIFY globalSearchResultsChanged)
    // When the shown search found nothing: a spelling correction of its
    // query that finds something, or empty.
    Q_PROPERTY(QString globalSearchSuggestion MEMBER m_globalSearchSuggestion NOTIFY globalSearchResultsChanged)
    Q_PROPERTY(QObject* globalSearchModel READ globalSearchModel CONSTANT)
    Q_PROPERTY(QVariantList itunesArtworkResults READ itunesArtworkResults NOTIFY itunesArtworkChanged)
    Q_PROPERTY(bool itunesArtworkLoading READ itunesArtworkLoading NOTIFY itunesArtworkChanged)
    Q_PROPERTY(QString itunesArtworkStatusText READ itunesArtworkStatusText NOTIFY itunesArtworkChanged)
    // The track the suggestions were searched for, which applying one
    // changes: its trackPath, album, artist and the coverPath it had then
    // (empty without art). It stays that track while playback moves on.
    Q_PROPERTY(QVariantMap itunesArtworkTarget READ itunesArtworkTarget NOTIFY itunesArtworkChanged)
    Q_PROPERTY(QString diagnosticsText READ diagnosticsText NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticsLogPath READ diagnosticsLogPath NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    // Why the backend stopped without being asked to; empty while it runs.
    Q_PROPERTY(QString backendFailure READ backendFailure NOTIFY backendFailureChanged)
    Q_PROPERTY(bool profileLogsEnabled READ profileLogsEnabled CONSTANT)

public:
    explicit BridgeClient(QObject *parent = nullptr);
    ~BridgeClient() override;

    // Finds the name of the desktop's file manager. It runs on a thread of
    // its own and returns the name, or nothing when it finds none.
    using FileBrowserNameDetector = QString (*)();
    // How clients made from now on find the file manager's name after they
    // start; by default with xdg-mime, and with nothing when `detector` is
    // null, which keeps the name guessed from $XDG_CURRENT_DESKTOP. Test
    // binaries set none in main(), before their first client, so that what
    // they see does not depend on the desktop they run in. Call it on the GUI
    // thread only.
    static void setFileBrowserNameDetector(FileBrowserNameDetector detector);

    // Adds a line to the diagnostics log (Help > Diagnostics).
    void logDiagnostic(const QString &category, const QString &message);
    // Adds an event to the log, as "[category] info: message", in the form
    // the backend's lines have. One line per event, never per frame.
    void logInfo(const QString &category, const QString &message);
    // Adds "[category] debug: message" to the log with FERROUS_LOG=debug
    // only: details that would crowd the trace logInfo writes.
    void logDebug(const QString &category, const QString &message);
    // Whether FERROUS_LOG=debug asks for logDebug's lines.
    static bool debugLoggingEnabled();
    // The log as the Diagnostics dialog shows it, without the library's
    // paths and file names; see DiagnosticsLog::withoutPaths.
    QString diagnosticsTextWithoutPaths() const;

    QString playbackState() const;
    PlaybackClock *playbackClock() const { return m_playbackClock; }
    QString durationText() const;
    // Changes on every track change, also when the same file plays again.
    quint64 currentTrackToken() const { return m_currentTrackToken; }
    double currentPlaySerial() const { return static_cast<double>(m_currentPlaySerial); }
    double durationSeconds() const;
    double volume() const;
    int queueLength() const;
    double queueUndoId() const;
    int queueUndoKind() const;
    QString queueDurationText() const;
    QObject *queueRows() const;
    int selectedQueueIndex() const;
    int playingQueueIndex() const;
    QString currentTrackPath() const;
    QString currentTrackCoverPath() const;
    QString currentTrackTitle() const;
    QString currentTrackArtist() const;
    QString currentTrackAlbum() const;
    QString currentTrackAlbumArtist() const;
    QString currentTrackGenre() const;
    QVariant currentTrackYear() const;
    int currentTrackNumber() const;
    int currentTrackTotal() const;
    QString currentTrackFormatLabel() const;
    QString currentTrackChannelLayoutText() const;
    QString currentTrackChannelLayoutIconKey() const;
    int currentTrackChannels() const;
    QStringList currentTrackChannelLabels() const;
    QStringList currentTrackTreeKeys() const;
    int currentTrackSampleRateHz() const;
    int currentTrackBitDepth() const;
    int currentTrackCurrentBitrateKbps() const;
    QByteArray waveformPeaksPacked() const;
    double waveformCoverageSeconds() const;
    bool waveformComplete() const;
    bool waveformEndsEarly() const;
    int sampleRateHz() const;
    int fftSize() const;
    int spectrogramViewMode() const;
    int spectrogramDisplayMode() const;
    int viewerFullscreenMode() const;
    bool preventDisplaySleepInFullscreen() const;
    bool showFullscreenTitleStrip() const;
    double dbRange() const;
    bool logScale() const;
    int repeatMode() const;
    bool shuffleEnabled() const;
    quint64 mutedChannelsMask() const;
    QList<bool> mutedChannels() const;
    quint64 soloedChannelsMask() const;
    QList<bool> soloedChannels() const;
    int channelButtonsVisibility() const;
    bool showFps() const;
    bool showLevelMeter() const;
    bool showSpectrogramCrosshair() const;
    bool showSpectrogramScale() const;
    bool spectrogramZoomEnabled() const;
    bool spectrogramFrequencyCap() const;
    bool systemMediaControlsEnabled() const;
    bool libraryWatchEnabled() const;
    QVariantMap queueColumnWidths() const;
    bool lastFmScrobblingEnabled() const;
    bool lastFmBuildConfigured() const;
    QString lastFmUsername() const;
    int lastFmAuthState() const;
    int lastFmPendingScrobbleCount() const;
    QString lastFmStatusText() const;
    bool lastFmShowScrobbleProgress() const;
    bool lastFmScrobblingPaused() const;
    int lastFmScrobbleState() const;
    int lastFmScrobbleIneligibility() const;
    int lastFmScrobbleListenedMs() const;
    int lastFmScrobbleThresholdMs() const;
    QString lastFmScrobbleMessage() const;
    QStringList libraryAlbums() const;
    QByteArray libraryTreeBinary() const;
    int libraryVersion() const;
    bool libraryScanInProgress() const;
    int libraryRootCount() const;
    int libraryTrackCount() const;
    int libraryArtistCount() const;
    int libraryAlbumCount() const;
    QStringList libraryExpandedKeys() const;
    bool libraryViewStateAvailable() const;
    QString libraryViewSelectionKey() const;
    QString libraryViewAnchorKey() const;
    double libraryViewAnchorOffset() const;
    QStringList libraryRoots() const;
    QVariantList libraryRootEntries() const;
    int librarySortMode() const;
    QString fileBrowserName() const;
    int libraryScanRootsCompleted() const;
    int libraryScanRootsTotal() const;
    int libraryScanDiscovered() const;
    int libraryScanProcessed() const;
    // The files the scan will have processed when it ends: more than those
    // processed while work remains; -1 while the scan cannot tell.
    int libraryScanTotal() const;
    double libraryScanFilesPerSecond() const;
    double libraryScanEtaSeconds() const;
    QVariantList globalSearchArtistResults() const;
    QVariantList globalSearchAlbumResults() const;
    QVariantList globalSearchTrackResults() const;
    int globalSearchArtistCount() const;
    int globalSearchAlbumCount() const;
    int globalSearchTrackCount() const;
    quint32 globalSearchSeq() const;
    bool globalSearchModelRetained() const;
    QObject *globalSearchModel() const;
    QVariantList itunesArtworkResults() const;
    bool itunesArtworkLoading() const;
    QString itunesArtworkStatusText() const;
    QVariantMap itunesArtworkTarget() const;
    QString diagnosticsText() const;
    QString diagnosticsLogPath() const;
    bool connected() const;
    QString backendFailure() const;
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
    Q_INVOKABLE void setPreventDisplaySleepInFullscreen(bool value);
    Q_INVOKABLE void setShowFullscreenTitleStrip(bool value);
    Q_INVOKABLE void setDbRange(double value);
    // The settings of a new install, by the name of their properties: what
    // the backend starts with when it has no settings file.
    Q_INVOKABLE QVariantMap settingDefaults() const;
    Q_INVOKABLE void setLogScale(bool value);
    Q_INVOKABLE void setRepeatMode(int mode);
    Q_INVOKABLE void setShuffleEnabled(bool value);
    Q_INVOKABLE void toggleChannelMute(int channelIndex);
    Q_INVOKABLE void soloChannel(int channelIndex);
    Q_INVOKABLE void setChannelButtonsVisibility(int value);
    Q_INVOKABLE bool isChannelSilent(int channelIndex) const;
    Q_INVOKABLE void setShowFps(bool value);
    Q_INVOKABLE void setShowLevelMeter(bool value);
    Q_INVOKABLE void setShowSpectrogramCrosshair(bool value);
    Q_INVOKABLE void setShowSpectrogramScale(bool value);
    Q_INVOKABLE void setSpectrogramZoomEnabled(bool value);
    Q_INVOKABLE void setSpectrogramFrequencyCap(bool value);
    Q_INVOKABLE void setSpectrogramZoomLevel(float level);
    Q_INVOKABLE void setSpectrogramWidgetWidth(int width);
    Q_INVOKABLE void registerSpectrogramItem(QObject *item, int channelIndex);
    Q_INVOKABLE void unregisterSpectrogramItem(QObject *item);
    Q_INVOKABLE void setSystemMediaControlsEnabled(bool value);
    Q_INVOKABLE void setLibraryWatchEnabled(bool value);
    // Keep `width` for the queue's "artist", "album" or "length" column, to
    // save it with the settings; 0 restores its default. A width out of the
    // range the backend keeps is ignored.
    Q_INVOKABLE void setQueueColumnWidth(const QString &column, int width);
    // Whether the backend has sent the window layout saved in the settings,
    // which it does once at startup; savedWindowLayoutRead tells when.
    Q_INVOKABLE bool hasSavedWindowLayout() const;
    // That layout, for the window to open with before it is shown. The keys
    // are "x", "y", "width", and "height" (if a geometry was saved, fitted
    // to the screens there were when it arrived; see fitWindowToScreens),
    // "maximized", "sidebarWidthRatio" and "queueHeightRatio" (if saved),
    // "visualizationMode", and "followCurrentTrack". Empty until it has
    // arrived.
    Q_INVOKABLE QVariantMap savedWindowLayout() const;
    // The layout the backend sent as a map, as savedWindowLayout gives it.
    static QVariantMap windowLayoutMap(
        const BinaryBridgeCodec::WindowLayout &saved, const QList<QRect> &screens);
    // Keep `layout`, with the keys of savedWindowLayout, to save it with the
    // settings. The geometry is the window's when it is neither maximized
    // nor fullscreen.
    Q_INVOKABLE void setWindowLayout(const QVariantMap &layout);
    // Where a window saved at `saved` opens on screens whose available areas
    // are `screens`: on the screen it overlaps most, or centred on the first
    // one when it overlaps none, such as one saved on a monitor that is gone.
    // It keeps its size where the screen has room for it, and at least
    // `minimum` where the screen has room for that; it is moved fully onto
    // the screen.
    static QRect fitWindowToScreens(
        const QRect &saved, const QList<QRect> &screens, const QSize &minimum);
    Q_INVOKABLE void setLastFmScrobblingEnabled(bool value);
    Q_INVOKABLE void beginLastFmAuth();
    Q_INVOKABLE void completeLastFmAuth();
    Q_INVOKABLE void disconnectLastFm();
    Q_INVOKABLE void setLastFmScrobblingPaused(bool value);
    Q_INVOKABLE void setLastFmShowScrobbleProgress(bool value);
    Q_INVOKABLE void playAt(int index);
    Q_INVOKABLE void selectQueueIndex(int index);
    Q_INVOKABLE void removeAt(int index);
    Q_INVOKABLE void removeIndices(const QVariantList &indices);
    Q_INVOKABLE void moveQueue(int from, int to);
    // Move `rows` together, in their order, before row `before` (the queue
    // length for the end), as one change to undo.
    Q_INVOKABLE void moveQueueRows(const QVariantList &rows, int before);
    // Move each run of `rows` one row up (`direction` < 0) or down.
    Q_INVOKABLE void shiftQueueRows(const QVariantList &rows, int direction);
    Q_INVOKABLE void clearQueue();
    // Undo the change of queueUndoId. The backend refuses, with an error,
    // one that a newer change has withdrawn.
    Q_INVOKABLE void undoQueueChange();
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
    // Files, folders or playlists, inserted before row `row`.
    Q_INVOKABLE void insertPaths(const QStringList &paths, int row);
    // Library rows, as the library model's queue items, as one change to the
    // queue: replacing it and playing, appending, or inserting before `row`.
    Q_INVOKABLE void playLibraryItems(const QVariant &items);
    Q_INVOKABLE void appendLibraryItems(const QVariant &items);
    Q_INVOKABLE void insertLibraryItems(const QVariant &items, int row);
    Q_INVOKABLE QString libraryAlbumCoverAt(int index) const;
    // stamp is the library tree's coverStamp role.
    Q_INVOKABLE QString libraryThumbnailSource(
        const QString &path, const QString &stamp = QString()) const;
    Q_INVOKABLE QString queuePathAt(int index) const;
    // The first row of the queue that holds `path`, or -1. It reads the
    // rows; queueContainsPath answers from an index.
    Q_INVOKABLE int queueIndexOfPath(const QString &path) const;
    // Whether a row of the queue holds `path`, whatever the queue's length.
    Q_INVOKABLE bool queueContainsPath(const QString &path) const;
    Q_INVOKABLE QVariant queueTrackNumberAt(int index) const;
    Q_INVOKABLE void addLibraryRoot(const QString &path, const QString &name = QString());
    Q_INVOKABLE void setLibraryRootName(const QString &path, const QString &name);
    Q_INVOKABLE void removeLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanLibraryRoot(const QString &path);
    Q_INVOKABLE void rescanAllLibraryRoots();
    Q_INVOKABLE void setLibraryNodeExpanded(const QString &key, bool expanded);
    Q_INVOKABLE void setLibraryViewState(
        const QString &selectionKey,
        const QString &anchorKey,
        double anchorOffset);
    Q_INVOKABLE void showTrackInLibrary(const QString &path);
    Q_INVOKABLE void setLibrarySortMode(int mode);
    Q_INVOKABLE void setGlobalSearchQuery(const QString &query);
    // Shows every result of `rowType` ("artist", "album" or "track"), up to
    // GlobalSearchResultsModel::kMaxShownPerType; with no type, of the
    // type with the most.
    Q_INVOKABLE void expandGlobalSearch(const QString &rowType = QString());
    bool globalSearchCanExpand() const;
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
    // Ask the backend to save the settings and the queue now. Answers with
    // stateSaveFinished carrying the returned request ID (never 0), so the
    // window can close once they are on disk.
    Q_INVOKABLE quint32 saveStateBeforeQuit();
    Q_INVOKABLE void clearDiagnostics();
    Q_INVOKABLE void reloadDiagnosticsFromDisk();
    // Puts diagnosticsTextWithoutPaths on the clipboard, for a bug report.
    Q_INVOKABLE void copyDiagnosticsWithoutPaths();
    // Show an error from the UI in Recent Issues, as the bridge's are: it is
    // logged, then sent with bridgeError. `category` and `severity` are
    // BinaryBridgeCodec's; an unknown one counts as a system error.
    Q_INVOKABLE void reportUiError(int category, int severity, const QString &subject,
                                   const QString &message);
    // Tag editor renames go through this gate so the bridge applies them in
    // order with the imports that read the same files. Null without a bridge.
    std::shared_ptr<FerrousFfiRenameGate> renameGate() const;

signals:
    // A seek landed at `seconds`, and no newer seek waits.
    void seekLanded(double seconds);
    void playbackChanged();
    void trackIdentityChanged();
    void trackMetadataChanged();
    void snapshotChanged();
    // The library's rows, roots, counts, view state and sort mode changed.
    void libraryChanged();
    // A scan's progress changed; it changes many times a second.
    void libraryScanChanged();
    // The scrobble progress or the scrobbling pause changed; the progress
    // changes with each snapshot while a track plays.
    void lastFmScrobbleChanged();
    void analysisChanged();
    void precomputedSpectrogramChunkReady(
        const QByteArray &data, int bins, int channelCount, int columns,
        int startIndex, int totalEstimate, int sampleRate, int hopSize,
        float coverage, bool complete, bool bufferReset, bool clearHistory,
        quint64 trackToken, quint64 generation);
    void precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset);
    void libraryTreeFrameReceived(int version, const QByteArray &treeBytes);
    void libraryRevealRequested(const QStringList &expandKeys, const QString &selectionKey);
    void globalSearchResultsChanged();
    void itunesArtworkChanged();
    void imageFileDetailsChanged(const QString &path);
    void diagnosticsChanged();
    void connectedChanged();
    void backendFailureChanged();
    // An error for the user: the message, its BinaryBridgeCodec::ErrorCategory
    // and ErrorSeverity, and the file or folder it is about, or empty.
    void bridgeError(const QString &message, int category, int severity, const QString &subject);
    void queueUndoChanged();
    // An undo brought back these rows, which the queue view selects; they
    // are rows of the queue as it is when this is emitted.
    void queueUndoRestored(const QVariantList &rows);
    // The answer to the saveStateBeforeQuit call that returned requestId:
    // whether the settings and the queue were saved, and what to show the
    // user if not.
    void stateSaveFinished(quint32 requestId, bool saved, const QString &message);
    // The backend sent the window layout saved in the settings.
    void savedWindowLayoutRead();
    void queueColumnWidthsChanged();

private:
    struct SearchWorkerInputFrame {
        quint32 seq{0};
        QByteArray payload;
        qint64 ffiPoppedAtMs{0};
        qint64 ffiPopMs{0};
    };

    struct SearchWorkerOutputFrame {
        quint32 seq{0};
        QVariantList totals;
        QVariantList artistRows;
        QVariantList albumRows;
        QVariantList trackRows;
        int artistCount{0};
        int albumCount{0};
        int trackCount{0};
        QVector<GlobalSearchResultsModel::SearchDisplayRow> displayRows;
        QString suggestion;
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
        QString assetUrl;
        double relevanceScore{0.0};
        int apiOrder{0};
    };

    struct ItunesArtworkAssetJobResult {
        quint64 generation{0};
        int candidateIndex{-1};
        bool success{false};
        QString errorMessage;
        QString normalizedPath;
        QString downloadPath;
        QVariantMap imageDetails;
    };

    struct BridgePollRunResult {
        int processedAnalysisFrames{0};
        int processedPrecomputedFrames{0};
        int processedTreeFrames{0};
        int processedSearchFrames{0};
        int processedEvents{0};
        qsizetype processedAnalysisBytes{0};
        qsizetype processedPrecomputedBytes{0};
        double precomputedPopMs{0.0};
        double precomputedDispatchMs{0.0};
        double maxPrecomputedFrameMs{0.0};
        bool analysisCapSaturated{false};
        bool precomputedCapSaturated{false};
        bool treeCapSaturated{false};
        bool searchCapSaturated{false};
        bool eventCapSaturated{false};
        bool budgetExhausted{false};

        bool anyWorkProcessed() const {
            return processedAnalysisFrames > 0
                || processedPrecomputedFrames > 0
                || processedTreeFrames > 0
                || processedSearchFrames > 0
                || processedEvents > 0;
        }

        bool shouldContinueImmediately() const {
            return budgetExhausted
                || analysisCapSaturated
                || precomputedCapSaturated
                || treeCapSaturated
                || searchCapSaturated
                || eventCapSaturated;
        }
    };

    struct PrecomputedDispatchStats {
        bool valid{false};
        double dispatchMs{0.0};
        int bins{0};
        int columns{0};
        int channelCount{0};
        int startIndex{0};
        int totalEstimate{0};
        int sampleRate{0};
        int hopSize{0};
        float coverage{0.0f};
        bool complete{false};
        bool bufferReset{false};
        bool clearHistory{false};
        quint64 trackToken{0};
        quint64 generation{0};
        // The width the session decodes for (0 if unknown).
        int widgetWidth{0};
        // The FFT size of the columns' spectra (0 if unknown): a bin is
        // sampleRate / fftSize Hz, whatever the bin count.
        int fftSize{0};
        // The frames of the file the session's columns are of (0 if
        // unknown): with fftSize, where a session at another zoom ends.
        quint64 trackFrames{0};
        // Where the session's centered view ends (0 if unknown).
        int endColumn{0};
        qsizetype dataBytes{0};
    };

    struct SpectrogramRoute {
        QPointer<QObject> item;
        int channelIndex{0};
        QMetaObject::Connection visibilityConnection{};
        QMetaObject::Connection windowConnection{};
        QMetaObject::Connection destructionConnection{};
    };

    void scheduleSpectrogramActivityUpdate();
    void updateSpectrogramActivity();
    void observeSpectrogramWindow(QQuickWindow *window);
    bool readSpectrogramStops(quint64 &count, quint64 &generation) const;
    // Whether a frame of `generation` is from a session before analysis
    // last turned off.
    bool staleSpectrogramFrame(quint64 generation);
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
    int queuePathFirstIndex(const QString &path) const;
    // Apply the rows of a queue section that fits the client's rows, taking
    // them from `prepared` when they were made already.
    // Returns false if its operations did not fit them after all.
    bool applyQueueRows(const BinaryBridgeCodec::DecodedQueue &queue,
                        PreparedQueueRows *prepared);
    // After the snapshot's rows are in place.
    void applyQueueUndo(const BinaryBridgeCodec::DecodedQueueUndo &undo);
    void applyBackendQueueColumnWidths(
        const BinaryBridgeCodec::QueueColumnWidths &widths, quint32 generation);
    // Read the size and modification time of the artwork target's track,
    // for the search of `generation`, into its "identity": empty if the file
    // is not there.
    void captureItunesArtworkTargetIdentity(quint64 generation);
    // Ask for the whole queue, once, after a section the rows cannot take.
    void requestQueueResync();
    void cancelItunesArtworkRequests();
    static ItunesArtworkAssetJobResult processItunesArtworkAssetPayload(
        const QByteArray &payload,
        const QString &tempDirPath,
        quint64 generation,
        int candidateIndex);
    // The largest rendition of the iTunes artwork whose 100 px thumbnail is
    // `artworkUrl100`, or empty when it is not an Apple thumbnail URL.
    static QString itunesArtworkUrl(const QString &artworkUrl100);
    void startItunesArtworkAssetDownload(int candidateIndex);
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
    // `preparedRows`, if given, has the snapshot's queue rows made already
    // (see QueueSnapshotWorker); the rows are taken from it.
    bool processBinarySnapshot(const BinaryBridgeCodec::DecodedSnapshot &snapshot,
                               PreparedQueueRows *preparedRows = nullptr);
    // Apply the snapshots the worker decoded, in their order.
    void applyWorkerSnapshots();
    // Whether snapshot packets wait in the worker; until they are applied,
    // later packets go there too, and library tree frames wait.
    bool workerSnapshotsPending() const;
    void submitWorkerSnapshot(QByteArray packet);
    // Apply a library tree frame from the bridge, or hold it while
    // snapshots before it are on the worker.
    void receiveLibraryTreeFrame(int version, const QByteArray &treeBytes);
    // Apply the held library tree once the snapshots submitted before it
    // were applied (up to `appliedSequence`), or with `all`, in any case.
    void applyDeferredTreeFrame(quint64 appliedSequence, bool all);
    void resetPollChanges();
    void schedulePollChanges();
    void processAnalysisBytes(const QByteArray &chunk);
    PrecomputedDispatchStats parsePrecomputedSpectrogramFrame(const QByteArray &raw);
    void routePrecomputedSpectrogramFrame(
        const char *data,
        qsizetype dataSize,
        const PrecomputedDispatchStats &stats);
    bool processSearchResultsFrame(const BinaryBridgeCodec::DecodedSearchResults &frame);
    void flushGlobalSearchQuery();
    void logProfileDiagnostic(const QString &category, const QString &message) const;
    void appendDiagnosticLine(const QString &line);
    void flushPendingDiagnosticDiskLines();
    void rebuildDiagnosticsText();
    static QString resolveDiagnosticsLogPath();
    void schedulePlaybackChanged();
    void scheduleTrackIdentityChanged();
    void scheduleTrackMetadataChanged();
    void scheduleSnapshotChanged();
    void scheduleLibraryChanged();
    void scheduleLibraryScanChanged();
    void scheduleLastFmScrobbleChanged();
    void scheduleAnalysisChanged();
    void shutdownBridgeGracefully();
    void setBackendFailure(const QString &message);
    static QString detectFileBrowserNameHeuristic();
    static QString detectFileBrowserName();
    static FileBrowserNameDetector s_fileBrowserNameDetector;
    // What `program` prints, trimmed, or nothing when it fails or takes
    // longer than `timeoutMs`; then it is killed and waited for, up to a
    // second.
    static QString runQuery(const QString &program, const QStringList &arguments, int timeoutMs);
    bool openUrlInFileBrowser(const QString &path, bool containingFolder) const;
    // Returns whether the bridge took the command.
    bool sendBinaryCommand(const QByteArray &payload);
    // Tell the UI about an error, which Recent Issues shows, and add it to
    // the diagnostics log with its category and severity; see bridgeError.
    // What the user need not see goes to logDiagnostic only.
    void reportError(BinaryBridgeCodec::ErrorCategory category, const QString &message,
                     const QString &subject = {},
                     BinaryBridgeCodec::ErrorSeverity severity =
                         BinaryBridgeCodec::ErrorSeverityError);
    void sendPathListCommand(quint16 cmdId, const QStringList &paths,
                             const KLocalizedString &tooLargeMessage);
    void sendLibraryItems(
        BinaryBridgeCodec::QueuePlacement placement,
        BinaryBridgeCodec::LibraryQueueItems items,
        int row);
    // The distinct rows of `rows` that the queue has, ascending.
    QList<quint32> queueRowList(const QVariantList &rows) const;
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
    // Fed the engine's position samples; a child of this client.
    PlaybackClock *m_playbackClock{nullptr};
    QString m_durationText{"00:00"};
    double m_durationSeconds{0.0};
    double m_volume{1.0};
    // The rows' count, which follows the model's row signals.
    int m_queueLength{0};
    QString m_queueDurationText{"00:00"};
    QueueRowsModel m_queueRowsModel;
    // Decodes large snapshots and frees dropped queue rows off the GUI
    // thread; stopped first when the client goes.
    std::unique_ptr<QueueSnapshotWorker> m_snapshotWorker;
    // The library tree frame taken while snapshots before it were on the
    // worker, which waits until the one with sequence number
    // `afterSnapshot` is applied. Each tree is the whole library, so a
    // newer one replaces it, as the bridge keeps only the newest.
    struct DeferredTreeFrame {
        int version{0};
        QByteArray bytes;
        quint64 afterSnapshot{0};
    };
    std::optional<DeferredTreeFrame> m_deferredTreeFrame;
    // The sequence number of the last snapshot packet submitted to the worker.
    quint64 m_lastWorkerSnapshot{0};
    // The backend's revision of the rows, which commands naming rows carry;
    // 0 while the rows match none.
    quint64 m_queueRevision{0};
    // A whole queue was asked for and has not arrived yet.
    bool m_queueResyncRequested{false};
    quint64 m_queueUndoId{0};
    int m_queueUndoKind{0};
    // The undo whose restored rows were last selected.
    quint64 m_queueUndoRestoredId{0};
    // The first row of each path, built when needed after the rows change.
    mutable QHash<QString, int> m_queuePathFirstIndex;
    mutable bool m_queuePathFirstIndexStale{true};
    int m_selectedQueueIndex{-1};
    int m_playingQueueIndex{-1};
    QString m_currentTrackPath;
    // Backend track token of the current track; it also changes when the
    // same file starts again, as with repeat-one.
    quint64 m_currentTrackToken{0};
    // The bridge's play serial; see currentPlaySerial.
    quint64 m_currentPlaySerial{0};
    QString m_currentTrackCoverPath;
    // The track the title, artist, album, genre, year and number describe:
    // the current track's, or while its own are not known yet, the track
    // before's (see processBinarySnapshot()).
    QString m_currentTrackTextPath;
    QString m_currentTrackTitle;
    QString m_currentTrackArtist;
    QString m_currentTrackAlbum;
    QString m_currentTrackAlbumArtist;
    QString m_currentTrackGenre;
    QVariant m_currentTrackYear;
    int m_currentTrackNumber{0};
    int m_currentTrackTotal{0};
    QString m_currentTrackFormatLabel;
    int m_currentTrackChannels{0};
    QStringList m_currentTrackChannelLabels;
    QStringList m_currentTrackTreeKeys;
    int m_currentTrackSampleRateHz{0};
    int m_currentTrackBitDepth{0};
    int m_currentTrackCurrentBitrateKbps{0};
    QByteArray m_waveformPeaksPacked;
    double m_waveformCoverageSeconds{0.0};
    bool m_waveformComplete{false};
    bool m_waveformEndsEarly{false};
    // The track the waveform state is of, from the analysis frames.
    quint64 m_waveformTrackToken{0};
    int m_sampleRateHz{48000};
    int m_fftSize{8192};
    int m_spectrogramViewMode{0};
    int m_spectrogramDisplayMode{0};
    int m_viewerFullscreenMode{0};
    bool m_preventDisplaySleepInFullscreen{true};
    bool m_showFullscreenTitleStrip{true};
    double m_dbRange{132.0};
    bool m_logScale{false};
    int m_repeatMode{0};
    bool m_shuffleEnabled{false};
    quint64 m_mutedChannelsMask{0};
    quint64 m_soloedChannelsMask{0};
    int m_channelButtonsVisibility{1};
    bool m_showFps{false};
    bool m_showLevelMeter{false};
    bool m_showSpectrogramCrosshair{false};
    bool m_showSpectrogramScale{false};
    bool m_spectrogramZoomEnabled{true};
    bool m_spectrogramFrequencyCap{false};
    bool m_systemMediaControlsEnabled{true};
    bool m_lastFmScrobblingEnabled{false};
    bool m_lastFmBuildConfigured{false};
    QString m_lastFmUsername;
    int m_lastFmAuthState{0};
    int m_lastFmPendingScrobbleCount{0};
    QString m_lastFmStatusText;
    QString m_lastFmAuthUrl;
    bool m_lastFmShowScrobbleProgress{true};
    bool m_lastFmScrobblingPaused{false};
    int m_lastFmScrobbleState{0};
    int m_lastFmScrobbleIneligibility{0};
    int m_lastFmScrobbleListenedMs{0};
    int m_lastFmScrobbleThresholdMs{0};
    QString m_lastFmScrobbleMessage;
    QString m_lastOpenedExternalUrl;
    QStringList m_libraryAlbums;
    QByteArray m_libraryTreeBinary;
    int m_libraryVersion{0};
    QStringList m_libraryAlbumArtists;
    QStringList m_libraryAlbumNames;
    QStringList m_libraryAlbumCoverPaths;
    QList<QStringList> m_libraryAlbumTrackPaths;
    mutable QHash<QString, QString> m_libraryThumbnailSourceCache;
    bool m_libraryScanInProgress{false};
    int m_libraryRootCount{0};
    int m_libraryTrackCount{0};
    int m_libraryArtistCount{0};
    int m_libraryAlbumCount{0};
    QStringList m_libraryExpandedKeys;
    bool m_libraryViewStateAvailable{false};
    QString m_libraryViewSelectionKey;
    QString m_libraryViewAnchorKey;
    double m_libraryViewAnchorOffset{0.0};
    quint32 m_libraryRevealGeneration{0};
    QStringList m_libraryRoots;
    QVariantList m_libraryRootEntries;
    int m_librarySortMode{0};
    QString m_fileBrowserName{QStringLiteral("File Manager")};
    int m_libraryScanRootsCompleted{0};
    int m_libraryScanRootsTotal{0};
    int m_libraryScanDiscovered{0};
    int m_libraryScanProcessed{0};
    int m_libraryScanTotal{-1};
    double m_libraryScanFilesPerSecond{0.0};
    double m_libraryScanEtaSeconds{-1.0};
    QVariantList m_globalSearchArtistResults;
    QVariantList m_globalSearchAlbumResults;
    QVariantList m_globalSearchTrackResults;
    int m_globalSearchArtistCount{0};
    int m_globalSearchAlbumCount{0};
    int m_globalSearchTrackCount{0};
    quint32 m_globalSearchSeq{0};
    bool m_globalSearchModelRetained{false};
    bool m_globalSearchBusy{false};
    QVariantList m_globalSearchTotals{0, 0, 0};
    quint32 m_globalSearchLimit{0};
    quint32 m_lastGlobalSearchLimitSent{0};
    quint32 m_deferredSearchSeq{0};
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
    QVector<ItunesArtworkCandidate> m_itunesArtworkCandidates;
    QVariantList m_itunesArtworkResults;
    bool m_itunesArtworkLoading{false};
    QString m_itunesArtworkStatusText;
    // What itunesArtworkTarget describes.
    QVariantMap m_itunesArtworkTarget;
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
    // Writes the log's lines off the GUI thread. Destroyed after the
    // destructor's last flush, it writes what still waits.
    std::unique_ptr<DiagnosticsLog::Writer> m_diagnosticsWriter;
    QString m_libraryLastError;
    // The roots, with their reasons, already reported or logged as not watched.
    QSet<QString> m_reportedUnwatchedRoots;
    bool m_libraryWatchEnabled{true};
    QString m_pendingAddRootPath;
    qint64 m_pendingAddRootIssuedMs{0};
    bool m_connected{false};
    QString m_backendFailure;
    quint32 m_lastStateSaveRequestId{0};
    // The window layout the backend sent at startup.
    bool m_hasSavedWindowLayout{false};
    QVariantMap m_savedWindowLayout;
    // The queue's column widths, and the generation of the latest ones sent
    // to the backend; see applyBackendQueueColumnWidths.
    BinaryBridgeCodec::QueueColumnWidths m_queueColumnWidths;
    quint32 m_queueColumnWidthsGeneration{0};
    // Set once the UI has asked the backend to stop, after which its stop is expected.
    bool m_bridgeShutdownRequested{false};
    bool m_loggedStartupQueueMissing{false};
    bool m_loggedStartupQueuePresent{false};
    bool m_playbackChangedPending{false};
    bool m_trackIdentityChangedPending{false};
    bool m_trackMetadataChangedPending{false};
    bool m_snapshotChangedPending{false};
    bool m_libraryChangedPending{false};
    bool m_libraryScanChangedPending{false};
    bool m_lastFmScrobbleChangedPending{false};
    bool m_analysisChangedPending{false};
    bool m_pollPlaybackChanged{false};
    bool m_pollTrackIdentityChanged{false};
    bool m_pollTrackMetadataChanged{false};
    bool m_pollSnapshotChanged{false};
    bool m_pollLibraryChanged{false};
    bool m_pollLibraryScanChanged{false};
    bool m_pollLastFmScrobbleChanged{false};
    // The playback clock holds the target of the seek with serial
    // m_pendingSeekSerial until the engine is done with that seek, and
    // ignores the samples from before it. The engine numbers seeks in the
    // order it receives them, which is the order they are sent.
    bool m_pendingSeek{false};
    double m_pendingSeekTargetSeconds{0.0};
    quint64 m_pendingSeekSerial{0};
    quint64 m_seeksSent{0};
    quint64 m_seekLandedSerial{0};
    // Tests capture commands here instead of sending them to the bridge.
    std::function<bool(const QByteArray &)> m_sendCommandOverride;
    // The serial of the last selection command sent. The queue sections
    // before its echo predate it, so the row it chose stands over theirs.
    // The bridge answers each command, in the order they are sent.
    quint64 m_queueSelectionsSent{0};
    QTimer m_diagnosticsFlushTimer;
    QTimer m_snapshotNotifyTimer;
    QTimer m_globalSearchDebounceTimer;
    QTimer m_searchApplyDispatchTimer;
    int m_searchApplyDispatchMs{12};
    QTimer m_searchModelApplyTimer;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> m_deferredSearchDisplayRows;
    QString m_deferredSearchSuggestion;
    QString m_globalSearchSuggestion;
    QVector<SpectrogramRoute> m_spectrogramRoutes;
    QSet<QQuickWindow *> m_spectrogramWindows;
    bool m_spectrogramActivityPending{false};
    bool m_spectrogramActive{true};
    // Turning analysis off releases the panes' columns. Spectral frames of
    // the sessions before still on their way are dropped from then until
    // one of a newer session arrives: analysis counts the times it turned
    // off, and publishes the generation the latest made (see
    // staleSpectrogramFrame).
    quint64 m_spectrogramStopsSent{0};
    bool m_spectrogramStaleGate{false};
    bool m_spectrogramStopKnown{false};
    quint64 m_spectrogramStopGeneration{0};
    // Stands in for the bridge's count and generation in tests.
    std::function<bool(quint64 &count, quint64 &generation)> m_spectrogramStopsOverride;
    std::thread m_searchApplyThread;
    mutable std::mutex m_searchApplyMutex;
    std::condition_variable m_searchApplyCv;
    bool m_searchApplyStop{false};
    std::optional<SearchWorkerInputFrame> m_searchPendingInputFrame;
    quint64 m_searchInputCoalescedDrops{0};
    mutable std::mutex m_searchOutputMutex;
    std::optional<SearchWorkerOutputFrame> m_searchPendingOutputFrame;
    quint64 m_searchOutputCoalescedDrops{0};
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
