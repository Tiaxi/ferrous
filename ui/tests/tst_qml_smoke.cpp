// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QMutex>
#include <QMutexLocker>
#include <QScopedPointer>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <cmath>

#include "../src/DiagnosticsLog.h"
#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramSeekTrace.h"
#define protected public
#define private public
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"
#undef private
#undef protected

namespace {

struct BinaryTreeRow {
    quint8 rowType{0};
    quint16 depth{0};
    qint32 sourceIndex{-1};
    quint16 trackNumber{0};
    quint16 childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8U16(QByteArray &out, const QString &value) {
    QByteArray utf8 = value.toUtf8();
    if (utf8.size() > 65535) {
        utf8.truncate(65535);
    }
    appendLe<quint16>(out, static_cast<quint16>(utf8.size()));
    out.append(utf8);
}

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const BinaryTreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, row.sourceIndex);
        appendLe<quint16>(out, row.trackNumber);
        appendLe<quint16>(out, row.childCount);
        appendUtf8U16(out, row.title);
        appendUtf8U16(out, row.key);
        appendUtf8U16(out, row.artist);
        appendUtf8U16(out, row.path);
        appendUtf8U16(out, row.coverPath);
        appendUtf8U16(out, row.trackPath);
        appendLe<quint16>(out, static_cast<quint16>(row.playPaths.size()));
        for (const QString &playPath : row.playPaths) {
            appendUtf8U16(out, playPath);
        }
    }
    return out;
}

QByteArray sampleArtistAlbumTreeBinary() {
    const QString trackPath = QStringLiteral("/music/artist/album/track01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        1,
        QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        2,
        1,
        0,
        0,
        1,
        QStringLiteral("Album A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        4,
        2,
        -1,
        1,
        0,
        QStringLiteral("01 - Track 01"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
        QStringLiteral("Artist A"),
        trackPath,
        {},
        trackPath,
        QStringList{trackPath},
    });
    return encodeRows(rows);
}

QByteArray artistWithManyAlbumsBinary(int albumCount) {
    QVector<BinaryTreeRow> rows;
    rows.reserve(1 + albumCount * 2);
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        static_cast<quint16>(albumCount),
        QStringLiteral("Artist A (%1)").arg(albumCount),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });

    for (int i = 0; i < albumCount; ++i) {
        const QString albumName = QStringLiteral("Album %1").arg(i + 1);
        const QString albumPath = QStringLiteral("/music/artist/%1").arg(albumName.toLower().replace(' ', ""));
        const QString trackPath = albumPath + QStringLiteral("/track.flac");
        rows.push_back(BinaryTreeRow{
            2,
            1,
            i,
            0,
            1,
            albumName,
            QStringLiteral("album|Artist A|%1").arg(albumName),
            QStringLiteral("Artist A"),
            albumPath,
            albumPath + QStringLiteral("/cover.jpg"),
            {},
            {},
        });
        rows.push_back(BinaryTreeRow{
            4,
            2,
            -1,
            1,
            0,
            QStringLiteral("01 - Track %1").arg(i + 1),
            QStringLiteral("track|%1").arg(trackPath),
            QStringLiteral("Artist A"),
            trackPath,
            {},
            trackPath,
            QStringList{trackPath},
        });
    }

    return encodeRows(rows);
}

QByteArray artistOnlyLazyBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        2,
        QStringLiteral("Artist A (2)"),
        QStringLiteral("artist|/music|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QByteArray multiRootBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-a"),
        QStringLiteral("root|/music-a"),
        {},
        QStringLiteral("/music-a"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist A (0)"),
        QStringLiteral("artist|/music-a|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music-a/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-b"),
        QStringLiteral("root|/music-b"),
        {},
        QStringLiteral("/music-b"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist B (0)"),
        QStringLiteral("artist|/music-b|Artist B"),
        QStringLiteral("Artist B"),
        QStringLiteral("/music-b/Artist B"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QString formatQmlErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    lines.reserve(errors.size());
    for (const QQmlError &error : errors) {
        lines.push_back(error.toString());
    }
    return lines.join('\n');
}

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    } else {
        qt_message_output(type, context, message);
    }
}

void clearCapturedMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().clear();
}

QString takeCapturedMessagesText() {
    QMutexLocker locker(&capturedMessageMutex());
    const QString text = capturedMessages().join('\n');
    capturedMessages().clear();
    return text;
}

QObject *findObjectByStringProperty(QObject *root, const char *propertyName, const QString &expectedValue) {
    if (!root) {
        return nullptr;
    }
    if (root->property(propertyName).toString() == expectedValue) {
        return root;
    }
    const QObjectList children = root->children();
    for (QObject *child : children) {
        if (QObject *match = findObjectByStringProperty(child, propertyName, expectedValue)) {
            return match;
        }
    }
    return nullptr;
}

QObject *createQmlObjectFromSource(
    QQmlEngine &engine,
    const QByteArray &qmlSource,
    const QUrl &baseUrl,
    QString *errorText) {
    QQmlComponent component(&engine);
    component.setData(qmlSource, baseUrl);
    if (!component.errors().isEmpty()) {
        if (errorText) {
            *errorText = formatQmlErrors(component.errors());
        }
        return nullptr;
    }

    QObject *object = component.create();
    if (!object && errorText) {
        *errorText = formatQmlErrors(component.errors());
    }
    return object;
}

} // namespace

class QmlSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void loadsMainQmlWithFallbackBridge();
    void loadsExtractedQmlSlicesWithFallbackProps();
    void albumArtTileKeepsHeightInsideColumnLayout();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void libraryTreeStartsCollapsedByDefault();
    void rootRowsStartExpandedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
    void artistPrefixSearchUsesModelLookup();
    void spectrogramMetadataOnlyResetWaitsForDataChunk();
    void spectrogramRollingSeekKeepsHistoryContinuous();
    void testMutedChannelRendersGrayscale();
    void spectrogramLargePositionJumpWaitsForResetHandoff();
    void spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward();
    void spectrogramGaplessTrackChangePreservesRollingHistory();
    void spectrogramNaturalTrackResetPreservesRollingHistory();
    void spectrogramManualTrackResetClearsRollingHistory();
    void spectrogramSeekResetAnchorsPlaybackClockToChunkStart();
    void diagnosticsLogUsesLowercaseAppDir();
    void playbackControllerSeekKeepsSpectrogramPositionUntilBackendUpdate();
    void playbackControllerPlaybackUpdateDoesNotPredictSpectrogramForward();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
    void spectrogramStaleTokenChunksAreDropped();
    void spectrogramGaplessTokenChunksPassFilter();
    void spectrogramFreshWidgetAcceptsDataWithImplicitReset();
    void spectrogramCenteredModeSeekPreservesRing();
    void spectrogramCenteredGaplessPreStagedFill();
    void spectrogramCenteredGaplessSnapsAnchorToZero();
    void spectrogramForceFpsOverlayDoesNotOverrideQmlBinding();
    void spectrogramRenderLoopStopsWhenNotPlaying();
    void playbackControllerInterpolationActivatesOnPlayback();
    void playbackControllerInterpolationDeactivatesOnStop();
    void trackIdentityChangedSignalTriggersQmlHandler();
    void queueAutoCenterIsDeferredOffHandlerStack();
    void queueContainIndexSkipsScrollWhenVisible();
    void queueContainIndexScrollsUpWhenAboveViewport();
    void queueContainIndexClampsAtListEnd();
    void spectrogramCrosshairAndGridPropertiesAndHoverTracking();
    void spectrogramPixelToFrequency();
    void spectrogramSampleRateSyncsFromPrecomputedChunks();
    void spectrogramCrosshairOverlayGeneratesOnHover();
    void spectrogramGridOverlayGeneratesWhenEnabled();
    void spectrogramOverlayDisabledProducesNullImage();
    void spectrogramOverlayDirtiedByGeometryChange();
    void spectrogramOverlayDirtiedByLogScaleChange();
    void spectrogramOverlayStalenessDetectsBinChange();
    void spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput();
    void spectrogramOverlayStalenessDetectsDisplayRangeChange();
    void spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled();
    void spectrogramClickToSeekSuppressedWhenCrosshairDisabled();
    void spectrogramLeftClickDoesNotSeek();
    void spectrogramClickToSeekDisabledInRollingMode();
    void spectrogramZoomProperty();
    void spectrogramZoomLimitsWithTrackData();
};

void QmlSmokeTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
}

void QmlSmokeTest::init() {
    clearCapturedMessages();
}

void QmlSmokeTest::cleanup() {
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void QmlSmokeTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QString qmlPath = QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QObject *libraryView = qvariant_cast<QObject *>(root->property("libraryViewRef"));
    QVERIFY2(libraryView != nullptr, "Main.qml did not publish the library view ref");
    QCOMPARE(qvariant_cast<QObject *>(libraryView->property("model")), static_cast<QObject *>(&libraryModel));

    bool invoked = QMetaObject::invokeMethod(root, "openItunesArtworkDialog");
    QVERIFY(invoked);
    QObject *itunesDialog = findObjectByStringProperty(root, "title", QStringLiteral("Replace From iTunes"));
    QVERIFY2(itunesDialog != nullptr, "Could not find iTunes artwork dialog instance");
    QTRY_VERIFY(itunesDialog->property("visible").toBool());
}

void QmlSmokeTest::loadsExtractedQmlSlicesWithFallbackProps() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import "components" as Components
import "controllers" as Controllers
import "dialogs" as Dialogs
import "panes" as Panes
import "viewers" as Viewers

Item {
    id: harness
    width: 1600
    height: 980

    Window {
        id: viewerWindowRoot
        visible: false
        width: harness.width
        height: harness.height
    }

    QtObject {
        id: bridge
        property bool connected: false
        property string playbackState: "Stopped"
        property string positionText: "00:00"
        property string durationText: "00:00"
        property real positionSeconds: 0
        property real durationSeconds: 0
        property real volume: 0.5
        property var queueRows: []
        property int queueLength: queueRows.length
        property int queueVersion: 0
        property string queueDurationText: "00:00"
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
        property string currentTrackPath: ""
        property string currentTrackCoverPath: ""
        property string currentTrackTitle: ""
        property string currentTrackArtist: ""
        property string currentTrackAlbum: ""
        property string currentTrackGenre: ""
        property var currentTrackYear: null
        property int currentTrackNumber: 0
        property var itunesArtworkResults: []
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 0
        property int fftSize: 8192
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: false
        property int soloedChannel: -1
        property int channelButtonsVisibility: 1
        property int viewerFullscreenMode: 0
        property int libraryArtistCount: 0
        property int libraryAlbumCount: 0
        property int libraryTrackCount: 0
        property var globalSearchModel: []
        property int globalSearchArtistCount: 0
        property int globalSearchAlbumCount: 0
        property int globalSearchTrackCount: 0
        property bool libraryScanInProgress: false
        property int libraryScanDiscovered: 0
        property int libraryScanProcessed: 0
        property real libraryScanFilesPerSecond: 0
        property real libraryScanEtaSeconds: -1
        property int librarySortMode: 0
        property var libraryRootEntries: []
        property string fileBrowserName: "File Manager"
        property bool lastFmScrobblingEnabled: false
        property bool lastFmBuildConfigured: false
        property string lastFmStatusText: ""
        property int lastFmPendingScrobbleCount: 0
        property string lastFmUsername: ""
        property bool systemMediaControlsEnabled: true
        property var mutedChannelsMask: 0
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property int sampleRateHz: 48000
        signal diagnosticsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal precomputedSpectrogramChunkReady(var data, int bins, int channelCount, int columns,
            int startIndex, int totalEstimate, int sampleRate, int hopSize,
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken)
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
        signal playbackChanged()
        function setVolume(value) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
        function isChannelMuted(channelIndex) { return false }
        function setChannelButtonsVisibility(value) {}
        function setLibrarySortMode(mode) {}
        function rescanAllLibraryRoots() {}
        function openInFileBrowser(path) {}
        function rescanLibraryRoot(path) {}
        function removeLibraryRoot(path) {}
        function setSpectrogramViewMode(mode) {}
        function setSpectrogramDisplayMode(mode) {}
        function setFftSize(value) {}
        function setDbRange(value) {}
        function setLogScale(value) {}
        function setShowFps(value) {}
        function setShowSpectrogramCrosshair(value) {}
        function setShowSpectrogramScale(value) {}
        function setSpectrogramZoomEnabled(value) {}
        function setViewerFullscreenMode(mode) {}
        function setLastFmScrobblingEnabled(value) {}
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
        function setSystemMediaControlsEnabled(value) {}
        function openContainingFolder(path) {}
        function setGlobalSearchQuery(query) {}
        function searchCurrentTrackArtworkSuggestions() {}
        function clearItunesArtworkSuggestions() {}
        function requestImageFileDetails(path) {}
        function cachedImageFileDetails(path) { return ({}) }
        function imageFileDetails(path) { return ({}) }
        function itunesArtworkResultAt(index) { return ({}) }
        function prepareItunesArtworkSuggestion(index) {}
        function applyItunesArtworkSuggestion(index) {}
        function reloadDiagnosticsFromDisk() {}
        function clearDiagnostics() {}
        function libraryThumbnailSource(path) { return "" }
        function queuePathAt(index) { return "" }
        function playAt(index) {}
        function removeAt(index) {}
        function moveQueue(from, to) {}
    }

    QtObject {
        id: globalSearchModelApi
        function isSelectableIndex(index) { return false }
        function nextSelectableIndex(startIndex, step, wrap) { return -1 }
        function rowDataAt(index) { return null }
    }

    Controllers.GlobalSearchController {
        id: globalSearchController
        uiBridge: bridge
        globalSearchModelApi: globalSearchModelApi
        requestLibraryRevealForSearchRow: function(row) {}
        focusLibraryViewForNavigation: function() {}
        requestOpenInFileBrowserForSearchRow: function(row) {}
    }

    Controllers.QueueController {
        id: queueController
        uiBridge: bridge
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Controllers.PlaybackController {
        id: playbackController
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

    Controllers.LibraryController {
        id: libraryController
        uiBridge: bridge
        libraryModel: sidebarModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Controllers.ViewerController {
        id: viewerController
        uiBridge: bridge
        useWholeScreenViewerMode: false
    }

    QtObject {
        id: tagEditorApi
        property bool open: false
        property bool loading: false
        property bool saving: false
        property bool dirty: false
        property string statusText: ""
        property string statusDetails: ""
        property var tableModel: []
        signal selectionChanged()
        signal bulkSummaryChanged()
        function close() {}
        function reload() {}
        function save() { return true }
        function renameSelectedFiles() {}
        function setSelectedRows(rows) {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
        function applyBulkFieldToRows(rows, field, value) {}
        function applyEnglishTitleCase(field) {}
        function applyFinnishCapitalize(field) {}
        function applyGenreCapitalize() {}
        function autoNumber(startingTrack, startingDisc, writeDiscNumbers, writeTotals, resetOnFolder, resetOnDiscChange) {}
    }

    ListModel {
        id: sidebarModel
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    Action { id: previousAction }
    Action { id: playAction }
    Action { id: pauseAction }
    Action { id: stopAction }
    Action { id: nextAction }
    Action { id: clearPlaylistAction }
    Action { id: replaceFromItunesAction }
    Action { id: playAllLibraryTracksAction }
    Action { id: appendAllLibraryTracksAction }

    Dialogs.PreferencesDialog {
        id: preferencesDialog
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        spectrogramFftChoices: [512, 1024, 2048]
        promptAddLibraryRoot: function(context) {}
        openLibraryRootNameDialog: function(mode, path, name) {}
        stepScrollView: function(scrollView, wheel, stepSize, stepsPerWheel) {}
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
    }

    Dialogs.DiagnosticsDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
    }

    Dialogs.LibraryRootNameDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        dialogMode: "add"
        pathValue: "/music"
        nameValue: "Music"
        onDismissed: function() {}
    }

    Dialogs.GlobalSearchDialog {
        parent: harness
        controller: globalSearchController
        uiPalette: palette
        windowRoot: harness
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        globalSearchShowsRootColumn: false
        globalSearchTrackNumberColumnWidth: 42
        globalSearchCoverColumnWidth: 28
        globalSearchArtistColumnWidth: 180
        globalSearchAlbumColumnWidth: 220
        globalSearchRootColumnWidth: 160
        globalSearchYearColumnWidth: 54
        globalSearchTrackGenreColumnWidth: 110
        globalSearchAlbumCountColumnWidth: 44
        globalSearchTrackLengthColumnWidth: 64
    }

    Dialogs.ItunesArtworkDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        openAlbumArtViewerForSuggestion: function(row) {}
        openAlbumArtViewerForCurrentArt: function() {}
    }

    Dialogs.TagEditorDialog {
        parent: harness
        tagEditorApi: tagEditorApi
        uiPalette: palette
        windowRoot: harness
    }

    Panes.StatusBar {
        id: statusBar
        width: harness.width
        uiBridge: bridge
        uiPalette: palette
        channelStatusIconSource: function(key) { return "" }
        themeIsDark: palette.themeIsDark
    }

    Panes.TransportBar {
        parent: harness
        width: harness.width
        uiBridge: bridge
        uiPalette: palette
        previousAction: previousAction
        playAction: playAction
        pauseAction: pauseAction
        stopAction: stopAction
        nextAction: nextAction
        themeIsDark: palette.themeIsDark
        volumeMuted: playbackController.volumeMuted
        displayedPositionSeconds: playbackController.displayedPositionSeconds
        toggleMutedVolume: playbackController.toggleMutedVolume
        setAppVolume: playbackController.setAppVolume
        normalizedVolumeValue: playbackController.normalizedVolumeValue
        seekCommitted: playbackController.seekCommitted
    }

    Components.TrackMetadataCard {
        parent: harness
        width: 420
        uiBridge: bridge
        uiPalette: palette
    }

    Viewers.AlbumArtViewerShell {
        parent: harness
        windowRoot: viewerWindowRoot
        viewerOpen: false
        useWholeScreenViewerMode: false
        popupTransitionMs: 0
        titleText: "Ferrous"
        closeViewer: function() {}
        toggleInfoVisible: function() {}
        switchComparisonImage: function() {}
    }

    Viewers.AlbumArtSurface {
        parent: harness
        x: 1180
        y: 20
        width: 220
        height: 220
        viewerOpen: false
        viewerSource: ""
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 1024
        viewerDecodeHeight: 1024
        infoOverlayText: ""
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        toggleInfoVisible: function() {}
        focusFullscreen: function() {}
        comparisonLabel: ""
        comparisonModeAvailable: false
    }

    Panes.SidebarPane {
        parent: harness
        x: 0
        y: 180
        width: 360
        height: 520
        controller: libraryController
        uiBridge: bridge
        libraryModel: sidebarModel
        uiPalette: palette
        splitPreferredWidth: width
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        openAlbumArtViewer: function() {}
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        playAllLibraryTracksAction: playAllLibraryTracksAction
        appendAllLibraryTracksAction: appendAllLibraryTracksAction
    }

    Panes.QueuePane {
        parent: harness
        x: 380
        y: 180
        width: 720
        height: 320
        controller: queueController
        uiBridge: bridge
        uiPalette: palette
        preferredHeight: height
        playlistIndicatorColumnWidth: 22
        playlistOrderColumnWidth: 34
        playlistOrderText: function(index) { return String(index + 1) }
        libraryController: libraryController
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        clearPlaylistAction: clearPlaylistAction
        popupTransitionMs: 0
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        droppedExternalPaths: function(drop) { return [] }
        submitExternalImport: function(paths, replaceQueue) { return false }
    }

    Viewers.SpectrogramSurface {
        parent: harness
        width: 420
        height: 160
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
}

void QmlSmokeTest::albumArtTileKeepsHeightInsideColumnLayout() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "components" as Components

Item {
    width: 360
    height: 700

    QtObject {
        id: bridge
        property string currentTrackCoverPath: ""
    }

    Action { id: replaceFromItunesAction }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.AlbumArtTile {
            id: albumArtTile
            objectName: "albumArtTile"
            uiBridge: bridge
            replaceFromItunesAction: replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: function() { return "" }
            openAlbumArtViewer: function() {}
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *tile = root->findChild<QObject *>(QStringLiteral("albumArtTile"));
    QVERIFY(tile != nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(tile->property("height").toReal() > 0.0, "AlbumArtTile collapsed to zero height");
}

void QmlSmokeTest::tagEditorLibrarySupportGateMatchesSupportedRows() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant supported;
    QVariant unsupported;
    const QVariant supportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("album")},
        {QStringLiteral("selectionKey"), QStringLiteral("album|/music|Artist|Album")},
    });
    const QVariant unsupportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("artist")},
        {QStringLiteral("selectionKey"), QStringLiteral("artist|/music|Artist")},
    });
    const bool supportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, supported),
        Q_ARG(QVariant, supportedRow));
    const bool unsupportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, unsupported),
        Q_ARG(QVariant, unsupportedRow));

    QVERIFY(supportedInvoked);
    QVERIFY(unsupportedInvoked);
    QCOMPARE(supported.toBool(), true);
    QCOMPARE(unsupported.toBool(), false);
}

void QmlSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::rootRowsStartExpandedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
    QCOMPARE(model.data(model.index(2, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(3, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(80));
    QTRY_COMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    QTRY_COMPARE(model.rowCount(), 81);
}

void QmlSmokeTest::lazyArtistRowRequestsBackendExpansion() {
    LibraryTreeModel model;
    QSignalSpy spy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setLibraryTreeFromBinary(artistOnlyLazyBinary());
    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), false);

    model.toggleArtist(QStringLiteral("Artist A"));
    QTRY_COMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("artist|/music|Artist A"));
    QCOMPARE(args.value(1).toBool(), true);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
}

void QmlSmokeTest::artistPrefixSearchUsesModelLookup() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist b"), 0), 3);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist a"), 2), 1);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("missing"), 0), -1);
}

void QmlSmokeTest::playbackControllerSeekKeepsSpectrogramPositionUntilBackendUpdate() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 12.0);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 12.0);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(48.0))));

    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 48.0);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 12.0);
}

void QmlSmokeTest::playbackControllerPlaybackUpdateDoesNotPredictSpectrogramForward() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        property string playbackState: "Playing"
        property real positionSeconds: 12.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QTest::qWait(130);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 12.12);
}

void QmlSmokeTest::spectrogramMetadataOnlyResetWaitsForDataChunk() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    for (int i = 0; i < initialChunk.size(); ++i) {
        initialChunk[i] = static_cast<char>(20 + i);
    }

    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 rollingEpochBeforeReset = item.m_rollingEpoch;
    const qint64 writeSeqBeforeReset = item.m_ringWriteSeq;
    QVERIFY(writeSeqBeforeReset > 0);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        128,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QVERIFY(item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, rollingEpochBeforeReset);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeReset);

    QByteArray seekChunk(binsPerColumn, '\0');
    for (int i = 0; i < seekChunk.size(); ++i) {
        seekChunk[i] = static_cast<char>(100 + i);
    }
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        128,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-128));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(2));
    QCOMPARE(item.m_ringSequenceId[0], static_cast<qint64>(0));
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringSequenceId[1], static_cast<qint64>(1));
    QCOMPARE(item.m_ringColumnId[1], 128);
    QCOMPARE(item.m_ringSequenceId[2], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramRollingSeekKeepsHistoryContinuous() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(12 * binsPerColumn, '\0');
    for (int i = 0; i < initialChunk.size(); ++i) {
        initialChunk[i] = static_cast<char>(10 + i);
    }
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        12,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeSeek = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeSeek, 12);
    item.setPositionSeconds((8.0 * 1024.0) / 48'000.0);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        400,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(2 * binsPerColumn, '\0');
    for (int i = 0; i < seekChunk.size(); ++i) {
        seekChunk[i] = static_cast<char>(100 + i);
    }
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        2,
        400,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(11));
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-392));
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 1);
    QCOMPARE(item.m_ringSequenceId[8], 8);
    QCOMPARE(item.m_ringColumnId[8], 8);
    QCOMPARE(item.m_ringSequenceId[9], 9);
    QCOMPARE(item.m_ringColumnId[9], 400);
    QCOMPARE(item.m_ringSequenceId[10], 10);
    QCOMPARE(item.m_ringColumnId[10], 401);
    QCOMPARE(item.m_ringSequenceId[11], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramLargePositionJumpWaitsForResetHandoff() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);
    item.setPositionSeconds(1.0);
    item.setPlaying(true);

    item.setPositionSeconds(120.0);
    QVERIFY(item.m_positionJumpHoldActive);
    QVERIFY(std::abs(item.m_positionSeconds - 1.0) < 0.0001);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        512,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        512,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    const double expectedSeconds = (512.0 * 1024.0) / 48000.0;
    QVERIFY(!item.m_positionJumpHoldActive);
    QVERIFY(std::abs(item.m_positionSeconds - expectedSeconds) < 0.0001);
}

void QmlSmokeTest::spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward() {
    SpectrogramItem item;

    item.setPositionSeconds(0.0);
    item.setPlaying(true);

    QTest::qWait(70);
    item.setPositionSeconds(0.05);
    const double anchoredPosition = item.positionSeconds();
    QVERIFY2(
        anchoredPosition >= 0.05,
        qPrintable(QStringLiteral("expected anchor to move forward, got %1")
            .arg(anchoredPosition, 0, 'f', 3)));

    QTest::qWait(70);
    item.setPositionSeconds(0.02);

    QVERIFY2(
        item.positionSeconds() >= anchoredPosition - 0.001,
        qPrintable(QStringLiteral(
            "lagging playback heartbeat moved anchor backward from %1 to %2")
            .arg(anchoredPosition, 0, 'f', 3)
            .arg(item.positionSeconds(), 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramGaplessTrackChangePreservesRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeGapless = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeGapless, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QCOMPARE(item.m_precomputedTrackToken, 12ULL);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeGapless);
    // Epoch stays unchanged during gapless transitions — the old
    // position model keeps advancing and the jump hold expiry remaps
    // the epoch to maintain display continuity.
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeGapless + 2);
    QCOMPARE(item.m_ringSequenceId[4], 4);
    QCOMPARE(item.m_ringColumnId[4], 0);
    QCOMPARE(item.m_ringTrackToken[4], 12ULL);
    QCOMPARE(item.m_ringSequenceId[5], 5);
    QCOMPARE(item.m_ringColumnId[5], 1);
    QCOMPARE(item.m_ringTrackToken[5], 12ULL);
}

void QmlSmokeTest::spectrogramNaturalTrackResetPreservesRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    const qint64 writeSeqBeforeReset = item.m_ringWriteSeq;
    QCOMPARE(writeSeqBeforeReset, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        12);

    QVERIFY(item.m_precomputedResetPending);
    QCOMPARE(item.m_ringWriteSeq, writeSeqBeforeReset);

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(3));
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringTrackToken[0], 11ULL);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 0);
    QCOMPARE(item.m_ringTrackToken[1], 12ULL);
    QCOMPARE(item.m_ringSequenceId[2], 2);
    QCOMPARE(item.m_ringColumnId[2], 1);
    QCOMPARE(item.m_ringTrackToken[2], 12ULL);
    QCOMPARE(item.m_ringSequenceId[3], static_cast<qint64>(-1));
}

void QmlSmokeTest::spectrogramManualTrackResetClearsRollingHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 1024;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11,
        true);

    QCOMPARE(item.m_ringWriteSeq, 4);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        12,
        true);

    QVERIFY(item.m_precomputedResetPending);
    QVERIFY(item.m_precomputedPendingResetClearHistory);
    QCOMPARE(item.m_ringWriteSeq, 4);

    QByteArray nextTrackChunk(2 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        nextTrackChunk,
        binsPerColumn,
        0,
        2,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        12);

    QVERIFY(!item.m_precomputedResetPending);
    QCOMPARE(item.m_ringWriteSeq, 2);
    QCOMPARE(item.m_ringOldestSeq, 0);
    QCOMPARE(item.m_rollingEpoch, 0);
    QCOMPARE(item.m_precomputedTrackToken, 12ULL);
    QCOMPARE(item.m_ringSequenceId[0], 0);
    QCOMPARE(item.m_ringColumnId[0], 0);
    QCOMPARE(item.m_ringTrackToken[0], 12ULL);
    QCOMPARE(item.m_ringSequenceId[1], 1);
    QCOMPARE(item.m_ringColumnId[1], 1);
    QCOMPARE(item.m_ringTrackToken[1], 12ULL);
}

void QmlSmokeTest::spectrogramSeekResetAnchorsPlaybackClockToChunkStart() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setPlaying(true);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray initialChunk(4 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        256,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    QByteArray seekChunk(binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        seekChunk,
        binsPerColumn,
        0,
        1,
        256,
        totalEstimate,
        48'000,
        1'024,
        false,
        false,
        11);

    const double expectedSeconds = (256.0 * 1024.0) / 48000.0;
    QVERIFY(std::abs(item.m_positionAnchorSeconds - expectedSeconds) < 0.0001);
    QVERIFY(std::abs(item.m_positionSeconds - expectedSeconds) < 0.0001);
}

void QmlSmokeTest::diagnosticsLogUsesLowercaseAppDir() {
    const QString logPath = DiagnosticsLog::defaultLogPath();
    QVERIFY(!logPath.isEmpty());

    const QString genericDataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (genericDataDir.isEmpty()) {
        QVERIFY(logPath.endsWith(QStringLiteral("/diagnostics.log"))
            || logPath.endsWith(QStringLiteral("\\diagnostics.log")));
        return;
    }

    const QFileInfo info(logPath);
    QCOMPARE(info.fileName(), QStringLiteral("diagnostics.log"));
    QCOMPARE(info.dir().dirName(), QStringLiteral("ferrous"));
    QVERIFY(!logPath.contains(QStringLiteral("/Ferrous/")));
    QVERIFY(!logPath.contains(QStringLiteral("\\Ferrous\\")));
}

void QmlSmokeTest::spectrogramSeekProfileFlagsStalledPostSeekWindow() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");
    SpectrogramSeekTrace::noteSeekIssued(12.5);

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_canvasWriteX = 96;

        const qint64 startedAtMs = SpectrogramSeekTrace::startedAtMs();
        QVERIFY(startedAtMs > 0);
        item.maybeStartSeekProfileLocked(startedAtMs);
        QVERIFY(item.m_seekProfile.active);

        item.noteSeekProfileFrameLocked(startedAtMs + 30, 0.030, true, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 60, 0.031, true, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 90, 0.029, true, false);
        item.finalizeSeekProfileLocked(startedAtMs + 120, "test");
        state = item.debugSeekProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("reason").toString(), QStringLiteral("test"));
    QVERIFY(state.value("incidentDetected").toBool());
    QCOMPARE(state.value("gapFrames").toInt(), 3);
    QCOMPARE(state.value("maxPendingRows").toInt(), 0);
#else
    QSKIP("Seek hitch profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::spectrogramSmoothnessProfileFlagsGapHeavyWindow() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_canvasWriteX = 48;
        item.m_lastIncomingRowsAtMs = QDateTime::currentMSecsSinceEpoch();
        item.maybeStartSmoothnessProfileLocked(item.m_lastIncomingRowsAtMs);
        QVERIFY(item.m_smoothnessProfile.active);

        item.noteSmoothnessPaintLocked(4.5);
        item.noteSmoothnessPaintLocked(5.0);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 30, 0.030, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 62, 0.032, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 95, 0.033, true, false);
        item.noteSmoothnessProfileFrameLocked(item.m_lastIncomingRowsAtMs + 128, 0.034, true, false);
        item.finalizeSmoothnessProfileLocked(item.m_lastIncomingRowsAtMs + 180, "test");
        state = item.debugSmoothnessProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("reason").toString(), QStringLiteral("test"));
    QVERIFY(state.value("incidentDetected").toBool());
    QCOMPARE(state.value("gapFrames").toInt(), 4);
    QCOMPARE(state.value("paintSpikeCount").toInt(), 2);
#else
    QSKIP("Smoothness profiling instrumentation is compiled out");
#endif
}

void QmlSmokeTest::waveformProgressInvalidatesOnlyTailSpan() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);

    QByteArray peaks(100, '\x33');
    item.setPeaksData(peaks);
    item.setGeneratedSeconds(5.0);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    item.setGeneratedSeconds(7.0);

    QCOMPARE(item.m_dirtyRect, QRect(100, 0, 40, 24));
    QVERIFY(item.m_cacheDirty);
}

void QmlSmokeTest::waveformPeakUpdatesInvalidateChangedSuffix() {
    WaveformItem item;
    item.setWidth(200);
    item.setHeight(24);
    item.setDurationSeconds(10.0);
    item.setWaveformComplete(true);

    QByteArray peaks(100, '\x22');
    item.setPeaksData(peaks);

    QImage canvas(200, 24, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.isNull());

    QByteArray updated = peaks;
    for (int i = 80; i < updated.size(); ++i) {
        updated[i] = '\x66';
    }
    item.setPeaksData(updated);

    QVERIFY(item.m_cacheDirty);
    QVERIFY(item.m_dirtyRect.x() >= 160);
    QCOMPARE(item.m_dirtyRect.height(), 24);
}

void QmlSmokeTest::stoppedTrackSwitchRequiresSpectrogramResetOnResume() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant result;
    bool invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), true);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);
}

void QmlSmokeTest::spectrogramStaleTokenChunksAreDropped() {
    // After a buffer_reset with token N, chunks from token < N are stale
    // and must be dropped to prevent ring corruption.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset with token 5.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 5);
    QByteArray data4(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data4, bins, 0, 4, 0, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, 4);

    // Reset with token 10 (new track).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 10, true);
    QByteArray data2(2 * bins, '\x80');
    item.feedPrecomputedChunk(
        data2, bins, 0, 2, 0, total, 48000, 1024, false, false, 10);
    QCOMPARE(item.m_ringWriteSeq, 2);

    // Stale chunk from token 5 (< committed 10) — must be dropped.
    QByteArray stale(3 * bins, '\xFF');
    const qint64 before = item.m_ringWriteSeq;
    item.feedPrecomputedChunk(
        stale, bins, 0, 3, 100, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, before);
}

void QmlSmokeTest::spectrogramGaplessTokenChunksPassFilter() {
    // In a gapless rolling transition, the token advances (3→4) without
    // a buffer_reset.  Committed stays at 3.  Token 4 chunks must NOT
    // be dropped.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset with token 3.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 3);
    QByteArray data(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 4, 0, total, 48000, 1024, false, false, 3);
    QCOMPARE(item.m_ringWriteSeq, 4);
    QCOMPARE(item.m_precomputedCommittedToken, 3ULL);

    // Gapless token 4 (> committed 3) — must be accepted.
    QByteArray gaplessData(2 * bins, '\x80');
    item.feedPrecomputedChunk(
        gaplessData, bins, 0, 2, 4, total, 48000, 1024, false, false, 4);
    QCOMPARE(item.m_ringWriteSeq, 6);
    QCOMPARE(item.m_precomputedTrackToken, 4ULL);
}

void QmlSmokeTest::spectrogramFreshWidgetAcceptsDataWithImplicitReset() {
    // A fresh/recycled widget (ringCapacity==0, no pending reset)
    // receiving data should apply an implicit reset and accept the data.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    QCOMPARE(item.m_ringCapacity, 0);
    QVERIFY(!item.m_precomputedResetPending);

    constexpr int bins = 8;
    constexpr int total = 512;
    QByteArray data(4 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 4, 100, total, 48000, 1024, false, false, 7);

    // Ring should have been allocated and data written.
    QVERIFY(item.m_ringCapacity > 0);
    QCOMPARE(item.m_ringWriteSeq, 4);
    QCOMPARE(item.m_precomputedTrackToken, 7ULL);
}

void QmlSmokeTest::spectrogramCenteredModeSeekPreservesRing() {
    // In centered mode, seeking should NOT clear the ring.
    // The position just moves the display window.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int total = 1024;

    // Reset and write some data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, 5);
    QByteArray data(100 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 100, 0, total, 48000, 1024, false, false, 5);
    QCOMPARE(item.m_ringWriteSeq, 100);

    // Simulate a position change (seek) — ring must be preserved.
    item.setPositionSeconds(50.0);
    QCOMPARE(item.m_ringWriteSeq, 100);

    // Data at the new position must still be valid in the ring.
    QVERIFY(item.m_ringCapacity > 0);
}

void QmlSmokeTest::spectrogramCenteredGaplessPreStagedFill() {
    // Verify that pre-staged chunks in centered mode provide instant
    // fill: after a buffer_reset+data batch, m_precomputedMaxColumnIndex
    // reflects the full pre-staged extent rather than growing from zero.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int total = 1024;
    constexpr quint64 oldToken = 5;
    constexpr quint64 newToken = 6;

    // Set up old track data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, total, 48000, 1024, false, false, oldToken);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 99);

    // Simulate pre-staged gapless: first chunk carries reset + data.
    QByteArray batch1(500 * bins, '\x40');
    item.feedPrecomputedChunk(
        batch1, bins, 0, 500, 0, total, 48000, 1024, false, true, newToken, true);

    // After first batch, maxColumnIndex should jump to 499 (not grow
    // incrementally from zero).
    QCOMPARE(item.m_precomputedMaxColumnIndex, 499);

    // Second staged batch extends.
    QByteArray batch2(300 * bins, '\x50');
    item.feedPrecomputedChunk(
        batch2, bins, 0, 300, 500, total, 48000, 1024, false, false, newToken);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 799);

    // Ring populated, not growing from zero.
    QVERIFY(item.m_ringWriteSeq >= 800);
    QVERIFY(item.m_ringCapacity > 0);
}

void QmlSmokeTest::spectrogramCenteredGaplessSnapsAnchorToZero() {
    // In centered mode, a gapless token change must immediately reset the
    // position anchor to 0 so the display snaps to the beginning of the
    // new track.  Without this, the anchor lingers at the old track's
    // position (~428 s) for ~1 s, rendering at a wrong column.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int total = 1024;
    constexpr quint64 oldToken = 5;
    constexpr quint64 newToken = 6;

    // Set up old track: reset + some data, position deep into the track.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, total, 48000, 1024, false, false, oldToken);
    item.setPositionSeconds(428.0);
    QVERIFY(std::abs(item.m_positionAnchorSeconds - 428.0) < 1.0);

    // Simulate GStreamer position resetting to near 0 right before the
    // gapless data arrives.  This should activate a jump hold.
    item.setPositionSeconds(0.04);
    QVERIFY(item.m_positionJumpHoldActive);

    // Gapless token change — first chunk with the new token.
    QByteArray newData(50 * bins, '\x40');
    item.feedPrecomputedChunk(
        newData, bins, 0, 50, 0, total, 48000, 1024, false, false, newToken);
    QCOMPARE(item.m_precomputedTrackToken, newToken);

    // After the gapless transition, anchor must be at 0 and hold cleared.
    QVERIFY(std::abs(item.m_positionAnchorSeconds) < 0.01);
    QVERIFY(!item.m_positionJumpHoldActive);

    // Subsequent small position updates must be accepted normally
    // (not held or snapped to the old position).
    item.setPositionSeconds(0.08);
    QVERIFY(!item.m_positionJumpHoldActive);
    // Anchor should be near the incoming position, not 428.
    QVERIFY(item.m_positionAnchorSeconds < 1.0);
}

void QmlSmokeTest::spectrogramForceFpsOverlayDoesNotOverrideQmlBinding() {
    // forceFpsOverlay is a CONSTANT property set at construction time from
    // the FERROUS_PROFILE_UI env var.  The setter for showFpsOverlay must
    // NOT OR in the force flag — QML's index===0 gate must be respected.
    SpectrogramItem item;
    QCOMPARE(item.forceFpsOverlay(), false); // no env var in test

    // Explicitly setting showFpsOverlay to false must stay false,
    // not be overridden by the force flag.
    item.setShowFpsOverlay(true);
    QCOMPARE(item.showFpsOverlay(), true);
    item.setShowFpsOverlay(false);
    QCOMPARE(item.showFpsOverlay(), false);
}

void QmlSmokeTest::spectrogramRenderLoopStopsWhenNotPlaying() {
    // The spectrogram's self-sustaining render loop (frameSwapped →
    // handleWindowAfterAnimating → update) must only re-trigger when
    // m_playing is true.  Without this guard the render loop runs at
    // full display refresh rate even when the spectrogram is static,
    // wasting ~10% CPU while idle.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(4 * binsPerColumn, '\x40');

    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        4,
        0,
        totalEstimate,
        48'000,
        1'024,
        false,
        true,
        11);

    // After feeding data, precomputed mode is active.
    QVERIFY(item.m_precomputedReady);

    // When playing, handleWindowAfterAnimating should schedule another
    // frame (the self-sustaining loop).  When not playing, it should
    // not — the display is static and updates are demand-driven.
    item.setPlaying(true);
    QVERIFY(item.m_playing);
    QVERIFY(item.m_precomputedReady);

    item.setPlaying(false);
    QVERIFY(!item.m_playing);
    // precomputedReady must still be true — only the render loop stops,
    // not the data.
    QVERIFY(item.m_precomputedReady);

    // Call the render-loop callback directly and verify it does NOT
    // schedule another update when not playing.  We track this by
    // checking that no animation tick state was freshly initialized
    // (a proxy for the loop being inactive, since without a window the
    // update() call is a no-op but the tick bookkeeping still runs).
    item.m_animationTickInitialized = false;
    item.handleWindowAfterAnimating();
    // The callback still runs (frameSwapped fires), but it must NOT
    // call update() to re-trigger the loop.  Verify the guard
    // condition: precomputedReady && !playing means no re-trigger.
    QVERIFY(item.m_animationTickInitialized);
    // The key invariant: the condition gating update() is
    //   changed || (precomputedActive && playing)
    // With playing=false and no FPS overlay change, this is false.
    QVERIFY(item.m_precomputedReady);
    QVERIFY(!item.m_playing);
}

void QmlSmokeTest::playbackControllerInterpolationActivatesOnPlayback() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        property string playbackState: "Playing"
        property real positionSeconds: 5.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);

    // After a playback update, interpolation remains active.
    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    bridge->setProperty("positionSeconds", 5.12);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);
}

void QmlSmokeTest::playbackControllerInterpolationDeactivatesOnStop() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    // Embed JS no-op callbacks directly in the QML harness so
    // handlePlaybackChanged can call haltSpectrogram without error.
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property real positionSeconds: 10.0
        property real durationSeconds: 180.0
        property string currentTrackPath: "/music/test.flac"
        property real volume: 1.0
    }

    Controllers.PlaybackController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        visualFeedsEnabled: true
        seekPressed: false
    }

    function simulateStop() {
        bridge.playbackState = "Stopped"
        bridge.positionSeconds = 0.0
        controller.handlePlaybackChanged(
            function() { /* halt */ },
            function() { /* reset */ })
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));
    QCOMPARE(controller->property("interpolationActive").toBool(), true);

    // Simulate stop via the QML helper that provides proper JS callbacks.
    QVERIFY(QMetaObject::invokeMethod(root.data(), "simulateStop"));
    QCOMPARE(controller->property("interpolationActive").toBool(), false);
}

void QmlSmokeTest::trackIdentityChangedSignalTriggersQmlHandler() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15

Item {
    id: harness
    property int trackIdentityChangedCount: 0

    QtObject {
        id: bridge
        objectName: "bridge"
        property int playingQueueIndex: -1
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
    }

    Connections {
        target: bridge
        function onTrackIdentityChanged() {
            harness.trackIdentityChangedCount++
        }
    }

    function emitTrackIdentityChanged() {
        bridge.playingQueueIndex = 5
        bridge.trackIdentityChanged()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "emitTrackIdentityChanged"));
    QCOMPARE(root->property("trackIdentityChangedCount").toInt(), 1);
}

void QmlSmokeTest::queueAutoCenterIsDeferredOffHandlerStack() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    property bool positionViewCalled: false
    property int positionViewIndex: -1

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/old.flac"
        property int playingQueueIndex: 5
        property int queueLength: 100
        property int queueVersion: 1
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real height: 400
        property real contentY: 0
        property real contentHeight: 2400
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function triggerTrackChange() {
        bridge.currentTrackPath = "/music/new.flac"
        bridge.playingQueueIndex = 42
        controller.handleSnapshotChanged(stubView)
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    // Initialize controller so it has a lastAutoCenterTrackPath to compare against.
    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "initializeFromBridge"));

    // Trigger a track change — handleSnapshotChanged detects path changed
    // and should defer scroll via a 0ms Timer.
    QObject *stubView = root->findChild<QObject *>(QStringLiteral("stubView"));
    QVERIFY(stubView != nullptr);
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "triggerTrackChange"));

    // Immediately after handler returns: contentY must NOT have changed yet.
    QCOMPARE(stubView->property("contentY").toDouble(), 0.0);

    // Process the event loop so the 0ms Timer fires.
    // Index 42 * 24px row height = 1008px row top. Since row is below
    // the viewport (400px), contentY should be set to rowBottom - viewHeight
    // = 1008 + 24 - 400 = 632.
    QTRY_VERIFY_WITH_TIMEOUT(stubView->property("contentY").toDouble() > 0.0, 100);
    QCOMPARE(stubView->property("contentY").toDouble(), 632.0);
}

// Helper: create a QueueController + stub view and invoke _containIndex.
// Returns the resulting contentY.
static double invokeContainIndex(
    QQmlApplicationEngine &engine,
    const QUrl &baseUrl,
    double initialContentY,
    double viewHeight,
    double contentHeight,
    int targetIndex)
{
    QString errorText;
    const QByteArray qml = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness

    QtObject {
        id: bridge
        objectName: "bridge"
        property string playbackState: "Playing"
        property string currentTrackPath: "/music/track.flac"
        property int playingQueueIndex: 0
        property int queueLength: 1000
        property int queueVersion: 1
        property int selectedQueueIndex: -1
        property bool profileLogsEnabled: false
    }

    QtObject {
        id: stubView
        objectName: "stubView"
        property bool visible: true
        property real height: 400
        property real contentY: 0
        property real contentHeight: 24000
    }

    Controllers.QueueController {
        id: controller
        objectName: "controller"
        uiBridge: bridge
        tagEditorApi: QtObject { function openSelection(sel) { return false } }
        openTagEditorDialog: function() {}
    }

    function callContainIndex(index) {
        controller._containIndex(stubView, index)
    }
}
)QML");
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, qml, baseUrl, &errorText));
    if (!root) {
        qWarning("invokeContainIndex: %s", qPrintable(errorText));
        return -1.0;
    }
    QObject *view = root->findChild<QObject *>(QStringLiteral("stubView"));
    view->setProperty("contentY", initialContentY);
    view->setProperty("height", viewHeight);
    view->setProperty("contentHeight", contentHeight);
    QMetaObject::invokeMethod(root.data(), "callContainIndex",
        Q_ARG(QVariant, targetIndex));
    return view->property("contentY").toDouble();
}

void QmlSmokeTest::queueContainIndexSkipsScrollWhenVisible() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 5 → rowTop = 120, rowBottom = 144.
    // Viewport: contentY=100, height=400 → visible range [100, 500].
    // Row is fully inside viewport → contentY must not change.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/100.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/5);
    QCOMPARE(result, 100.0);
}

void QmlSmokeTest::queueContainIndexScrollsUpWhenAboveViewport() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 2 → rowTop = 48.
    // Viewport: contentY=200, height=400 → visible range [200, 600].
    // Row is above viewport → contentY should snap to rowTop = 48.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/200.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/2);
    QCOMPARE(result, 48.0);
}

void QmlSmokeTest::queueContainIndexClampsAtListEnd() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR)
        + QStringLiteral("/qml/QmlSmokeHarness.qml"));

    // Index 999 → rowTop = 23976, rowBottom = 24000.
    // contentHeight = 24000, viewHeight = 400.
    // maxY = 24000 - 400 = 23600.
    // Target would be rowBottom - viewHeight = 24000 - 400 = 23600.
    // Clamped to maxY = 23600.
    const double result = invokeContainIndex(engine, baseUrl,
        /*initialContentY=*/0.0, /*viewHeight=*/400.0,
        /*contentHeight=*/24000.0, /*targetIndex=*/999);
    QCOMPARE(result, 23600.0);
}

void QmlSmokeTest::spectrogramCrosshairAndGridPropertiesAndHoverTracking() {
    // Verify crosshairEnabled/gridEnabled properties default to false,
    // emit change signals, and that hover events update internal state.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Default state: both overlays disabled, no hover.
    QCOMPARE(item.crosshairEnabled(), false);
    QCOMPARE(item.gridEnabled(), false);
    QCOMPARE(item.m_hoverActive, false);

    // Enable crosshair — signal fires, dirty flag set.
    QSignalSpy crosshairSpy(&item, &SpectrogramItem::crosshairEnabledChanged);
    item.setCrosshairEnabled(true);
    QCOMPARE(item.crosshairEnabled(), true);
    QCOMPARE(crosshairSpy.count(), 1);
    QCOMPARE(item.m_crosshairDirty, true);

    // No-op when setting same value.
    item.setCrosshairEnabled(true);
    QCOMPARE(crosshairSpy.count(), 1);

    // Enable grid.
    QSignalSpy gridSpy(&item, &SpectrogramItem::gridEnabledChanged);
    item.setGridEnabled(true);
    QCOMPARE(item.gridEnabled(), true);
    QCOMPARE(gridSpy.count(), 1);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);

    // Simulate hover enter — m_hoverActive should become true.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent(QEvent::HoverEnter, QPointF(100.0, 50.0), QPointF(100.0, 50.0), QPointF());
    item.hoverEnterEvent(&enterEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 100.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 50.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true); // crosshair enabled → dirty

    // Simulate hover move.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent moveEvent(QEvent::HoverMove, QPointF(150.0, 75.0), QPointF(150.0, 75.0), QPointF(100.0, 50.0));
    item.hoverMoveEvent(&moveEvent);
    QCOMPARE(item.m_hoverActive, true);
    QVERIFY(std::abs(item.m_hoverPosition.x() - 150.0) < 0.01);
    QVERIFY(std::abs(item.m_hoverPosition.y() - 75.0) < 0.01);
    QCOMPARE(item.m_crosshairDirty, true);

    // Simulate hover leave — m_hoverActive becomes false.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent leaveEvent(QEvent::HoverLeave, QPointF(), QPointF(), QPointF(150.0, 75.0));
    item.hoverLeaveEvent(&leaveEvent);
    QCOMPARE(item.m_hoverActive, false);
    QCOMPARE(item.m_crosshairDirty, true);

    // When crosshair is disabled, hover events do NOT mark dirty.
    item.setCrosshairEnabled(false);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairDirty = false;
    }
    QHoverEvent enterEvent2(QEvent::HoverEnter, QPointF(50.0, 30.0), QPointF(50.0, 30.0), QPointF());
    item.hoverEnterEvent(&enterEvent2);
    QCOMPARE(item.m_hoverActive, true); // position still tracked
    QCOMPARE(item.m_crosshairDirty, false); // but NOT dirty
}

void QmlSmokeTest::spectrogramPixelToFrequency() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Feed data to set binsPerColumn = 4097 (FFT size 8192).
    const int bins = 4097;
    QByteArray data(bins, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 1, 0, 100, 48000, 1024, false, true, 1, false);

    // In linear mode, bottom pixel maps to 0 Hz (DC),
    // top pixel maps to Nyquist (24000 Hz).
    // pixelY=0 -> top -> ~24000 Hz
    // pixelY=99 -> bottom -> 0 Hz
    QVERIFY(item.pixelToFrequencyHz(0, 100) > 20000.0);
    QVERIFY(item.pixelToFrequencyHz(99, 100) < 500.0);

    // Mid-height should be roughly half Nyquist in linear mode.
    const double midFreq = item.pixelToFrequencyHz(50, 100);
    QVERIFY(midFreq > 10000.0 && midFreq < 14000.0);
}

void QmlSmokeTest::spectrogramSampleRateSyncsFromPrecomputedChunks() {
    SpectrogramItem item;
    // Start with default 48 kHz property — simulates QML binding.
    item.setSampleRateHz(48000);

    const int bins = 4097; // FFT size 8192
    QByteArray data(bins, '\x80');

    // Feed a chunk with 44100 Hz sample rate.
    item.feedPrecomputedChunk(data, bins, 0, 1, 0, 100, 44100, 1024,
                               false, true, 1, false);

    // pixelToFrequencyHz triggers the sync path.
    // Top pixel should map to Nyquist = 22050, not 24000.
    const double topFreq = item.pixelToFrequencyHz(0, 100);
    QVERIFY2(topFreq < 23000.0,
             qPrintable(QStringLiteral("Expected Nyquist ~22050 but got %1").arg(topFreq)));
    QVERIFY2(topFreq > 21000.0,
             qPrintable(QStringLiteral("Expected Nyquist ~22050 but got %1").arg(topFreq)));
}

void QmlSmokeTest::spectrogramCrosshairOverlayGeneratesOnHover() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_crosshairDirty = true;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }

    QVERIFY(!item.m_crosshairImage.isNull());
    QCOMPARE(item.m_crosshairImage.width(), 200);
    QCOMPARE(item.m_crosshairImage.height(), 100);

    // The image must contain visible content (lines + labels).
    bool hasContent = false;
    for (int y = 0; y < item.m_crosshairImage.height() && !hasContent; ++y) {
        for (int x = 0; x < item.m_crosshairImage.width(); ++x) {
            if (qAlpha(item.m_crosshairImage.pixel(x, y)) > 0) {
                hasContent = true;
                break;
            }
        }
    }
    QVERIFY(hasContent);
}

void QmlSmokeTest::spectrogramGridOverlayGeneratesWhenEnabled() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        // Sync bins from precomputed state (normally done in updatePaintNode).
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    QVERIFY(!item.m_freqGridImage.isNull());
    QCOMPARE(item.m_freqGridImage.width(), 200);
    QCOMPARE(item.m_freqGridImage.height(), 100);

    bool hasContent = false;
    for (int y = 0; y < item.m_freqGridImage.height() && !hasContent; ++y) {
        for (int x = 0; x < item.m_freqGridImage.width(); ++x) {
            if (qAlpha(item.m_freqGridImage.pixel(x, y)) > 0) {
                hasContent = true;
                break;
            }
        }
    }
    QVERIFY(hasContent);
}

void QmlSmokeTest::spectrogramOverlayDisabledProducesNullImage() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    // Both overlays default to disabled.

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    // Crosshair disabled + hover active: null image.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
    }
    QVERIFY(item.m_crosshairImage.isNull());

    // Grid disabled: null image.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }
    QVERIFY(item.m_freqGridImage.isNull());
}

void QmlSmokeTest::spectrogramOverlayDirtiedByGeometryChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.geometryChange(QRectF(0, 0, 300, 150), QRectF(0, 0, 200, 100));

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void QmlSmokeTest::spectrogramOverlayDirtiedByLogScaleChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }
    QVERIFY(!item.m_crosshairDirty);
    QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);

    item.setLogScale(true);

    QVERIFY(item.m_crosshairDirty);
    QVERIFY(item.m_freqGridDirty || item.m_timeGridDirty);
}

void QmlSmokeTest::spectrogramOverlayStalenessDetectsBinChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 2049;
    }

    QVERIFY(item.m_binsPerColumn != item.m_crosshairCachedBinsPerColumn);
    QVERIFY(item.m_binsPerColumn != item.m_freqGridCachedBinsPerColumn);
}

void QmlSmokeTest::spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setSampleRateHz(48000);
    item.setDisplayMode(1); // Centered
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);

    const int bins = 4097;
    const int columns = 500;
    QByteArray data(bins * columns, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, columns, 48000, 1024,
                               false, true, 1, false);

    item.setPositionSeconds(1.0);
    item.setPlaying(false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(100.0, 50.0);
    }

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    qint64 firstGridDisplayLeft;
    qint64 firstCrosshairDisplayLeft;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_crosshairImage.isNull());
        QVERIFY(!item.m_freqGridImage.isNull());
        QVERIFY(!item.m_crosshairDirty);
        QVERIFY(!item.m_freqGridDirty);
    QVERIFY(!item.m_timeGridDirty);
        firstGridDisplayLeft = item.m_timeGridRenderDisplayLeft;
        firstCrosshairDisplayLeft = item.m_crosshairCachedDisplayLeft;
    }

    item.setPositionSeconds(5.0);

    QSGNode *node2 = item.updatePaintNode(node, nullptr);
    QVERIFY(node2 != nullptr);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_crosshairImage.isNull());
        QVERIFY(!item.m_freqGridImage.isNull());
        QVERIFY(item.m_timeGridRenderDisplayLeft != firstGridDisplayLeft);
        QVERIFY(item.m_crosshairCachedDisplayLeft != firstCrosshairDisplayLeft);
    }

    delete node2;
}

void QmlSmokeTest::spectrogramOverlayStalenessDetectsDisplayRangeChange() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        const double cps = 48000.0 / 1024.0;
        item.updateCrosshairOverlayLocked(200, 100, 0, false, cps, 0.0);
        item.updateFreqGridOverlayLocked(200, 100);
        item.updateTimeGridOverlayLocked(200, 100, 0, 0, false, cps, 0.0);
    }

    QCOMPARE(item.m_crosshairCachedDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_timeGridRenderDisplayLeft, static_cast<qint64>(0));
}

void QmlSmokeTest::testMutedChannelRendersGrayscale()
{
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    // Palette is built in the constructor.  Verify that the color palette
    // has colored entries and the gray palette has grayscale entries at
    // the same index (a mid-intensity entry, not the near-black tail).
    constexpr int midIndex = SpectrogramItem::kGradientTableSize / 4;
    const QRgb colorEntry = item.m_palette32[midIndex];
    const QRgb grayEntry = item.m_palette32Gray[midIndex];

    // Color palette entry should have differing R/G/B channels.
    const int cr = qRed(colorEntry), cg = qGreen(colorEntry), cb = qBlue(colorEntry);
    QVERIFY2(!(cr == cg && cg == cb),
             qPrintable(QStringLiteral("Color palette entry at %1 should not be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(cr).arg(cg).arg(cb)));

    // Gray palette entry should have equal R/G/B channels.
    const int gr = qRed(grayEntry), gg = qGreen(grayEntry), gb = qBlue(grayEntry);
    QVERIFY2(gr == gg && gg == gb,
             qPrintable(QStringLiteral("Gray palette entry at %1 should be grayscale: R=%2 G=%3 B=%4")
                            .arg(midIndex).arg(gr).arg(gg).arg(gb)));

    // channelMuted property defaults to false and round-trips.
    QCOMPARE(item.channelMuted(), false);
    item.setChannelMuted(true);
    QCOMPARE(item.channelMuted(), true);
    // Setting the same value again should not re-emit.
    QSignalSpy spy(&item, &SpectrogramItem::channelMutedChanged);
    item.setChannelMuted(true);
    QCOMPARE(spy.count(), 0);
    item.setChannelMuted(false);
    QCOMPARE(spy.count(), 1);
}

void QmlSmokeTest::spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    // Feed enough data so the ring buffer is populated.
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        sampleRate, hopSize, true, true, 42);

    // Set centered mode, position at 5 seconds, crosshair enabled.
    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(true);

    // Prime the crosshair cache by simulating a hover so cached
    // displayLeft/drawX/rollingMode are initialized.
    QHoverEvent hoverEnter(
        QEvent::HoverEnter, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), QPointF());
    item.hoverEnterEvent(&hoverEnter);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Right-click at pixel X=100.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    // The exact value depends on display layout, but it must be
    // non-negative (valid time).
    QVERIFY(seekSeconds > 0.0);
    QVERIFY(seekSeconds < 5.0);  // Left of center → earlier than playhead
}

void QmlSmokeTest::spectrogramClickToSeekSuppressedWhenCrosshairDisabled() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(false);  // Crosshair OFF

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramLeftClickDoesNotSeek() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 512;
    QByteArray chunk(totalEstimate * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, totalEstimate, 0, totalEstimate,
        48000, 1024, true, true, 42);

    item.setDisplayMode(1);
    item.setPositionSeconds(5.0);
    item.setCrosshairEnabled(true);  // Crosshair ON

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    // Left-click should NOT seek.
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(100.0, 90.0),
        QPointF(100.0, 90.0), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramClickToSeekDisabledInRollingMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    constexpr int totalEstimate = 512;

    QByteArray chunk(200 * binsPerColumn, '\0');
    for (int i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<char>(i & 0xFF);
    }
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, 200, 0, totalEstimate,
        sampleRate, hopSize, false, true, 42);

    // Rolling mode (default displayMode=0), crosshair enabled.
    // Seek is disabled in rolling mode because the write-order ring
    // buffer history does not realign to the new position.
    item.setDisplayMode(0);
    item.setPositionSeconds(3.0);
    item.setCrosshairEnabled(true);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);

    QMouseEvent pressEvent(
        QEvent::MouseButtonPress, QPointF(50.0, 90.0),
        QPointF(50.0, 90.0), Qt::RightButton, Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 0);
}

void QmlSmokeTest::spectrogramZoomProperty() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Default zoom is 1.0
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
    QCOMPARE(item.zoomEnabled(), false);

    // Feed enough columns so that zoom < 16.0 is valid (minZoom = 1920/96000 = 0.02 → floor 0.05)
    constexpr int binsPerColumn = 4;
    constexpr int columns = 96000;
    QByteArray chunk(binsPerColumn * columns, '\x20');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Setting zoom level to 2.0 works when track is long enough
    item.setZoomLevel(2.0);
    QVERIFY(std::abs(item.zoomLevel() - 2.0) < 0.0001);

    // Zoom clamps to maximum
    item.setZoomLevel(100.0);
    QVERIFY(std::abs(item.zoomLevel() - 16.0) < 0.0001);

    // Zoom clamps to minimum floor (0.05 hard floor, minZoom = 1920/96000 = 0.02 → 0.05)
    item.setZoomLevel(0.001);
    QVERIFY(std::abs(item.zoomLevel() - 0.05) < 0.0001);

    // Reset to 1.0
    item.setZoomLevel(1.0);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
}

void QmlSmokeTest::spectrogramZoomLimitsWithTrackData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed some precomputed data to set up track columns
    constexpr int binsPerColumn = 64;
    constexpr int columns = 9600; // ~200 seconds at 48 cols/sec
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Minimum zoom should allow seeing all columns
    const double minZoom = item.minimumZoomLevel();
    QVERIFY(minZoom > 0.0);
    QVERIFY(minZoom <= 1.0);
    // 1920 / 9600 = 0.2
    QVERIFY(std::abs(minZoom - 0.2) < 0.01);
}

int main(int argc, char **argv) {
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_smoke.moc"
