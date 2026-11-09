// SPDX-License-Identifier: GPL-3.0-or-later

// The now-playing test hands BridgeClient its snapshots, as BridgeClientTest
// does, so BridgeClient alone is read with its private members open; what
// it includes comes first, as it is.
#include <QAbstractListModel>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "../src/BinaryBridgeCodec.h"
#include "../src/GlobalSearchResultsModel.h"
#include "../src/PlaybackClock.h"
#include "../src/QueueRows.h"
#define private public
#include "../src/BridgeClient.h"
#undef private

#include "QmlSmokeTest.h"

// The main window, its layout, and what survives a restart.
class WindowSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void loadsMainQmlWithFallbackBridge();
    void loadsExtractedQmlSlicesWithFallbackProps();
    void mainWindowContentStartsBelowMenuBar();
    void draggedSplitSizesSurviveAWindowResize();
    void theSmallestWindowFitsALaptopScreenAndShowsEveryControl();
    void windowLayoutSurvivesARestart();
    void aWindowSavedOffTheScreensOpensOnOne();
    void queueColumnWidthsSurviveARestart();
    void theWindowOpensWithoutASavedLayoutThatDoesNotCome();
    void quittingWhileMinimizedKeepsTheWindowMaximized();
    void aWindowOffScreenStopsTheClockAndTheFeeds();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void drawingWritesNoShaderCache();
    void nowPlayingShowsNoTextOfTheTrackBefore();
};

void WindowSmokeTest::loadsMainQmlWithFallbackBridge() {
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

    // The preference pages are made when the dialog first opens, not with
    // the window, and stay once made.
    QObject *preferencesDialog = root->findChild<QObject *>(QStringLiteral("preferencesDialog"));
    QVERIFY(preferencesDialog != nullptr);
    QVERIFY(root->findChild<QObject *>(QStringLiteral("preferencesPages")) == nullptr);
    QVERIFY(QMetaObject::invokeMethod(preferencesDialog, "open"));
    QObject *preventDisplaySleepCheckBox = root->findChild<QObject *>(
        QStringLiteral("preventDisplaySleepInFullscreenCheckBox"));
    QVERIFY(preventDisplaySleepCheckBox != nullptr);
    QVERIFY(QMetaObject::invokeMethod(preferencesDialog, "close"));
    QTRY_VERIFY(!preferencesDialog->property("visible").toBool());
    QCOMPARE(root->findChild<QObject *>(QStringLiteral("preventDisplaySleepInFullscreenCheckBox")),
             preventDisplaySleepCheckBox);
    QVERIFY(preventDisplaySleepCheckBox->property("checked").toBool());

    QObject *libraryView = qvariant_cast<QObject *>(root->property("libraryViewRef"));
    QVERIFY2(libraryView != nullptr, "Main.qml did not publish the library view ref");
    QCOMPARE(qvariant_cast<QObject *>(libraryView->property("model")), static_cast<QObject *>(&libraryModel));

    auto *window = qobject_cast<QQuickWindow *>(root);
    QVERIFY(window != nullptr);
    QObject *modeSwitch = nullptr;
    const QList<QObject *> modeSwitches = root->findChildren<QObject *>(
        QStringLiteral("visualizationModeSwitch"));
    for (QObject *candidate : modeSwitches) {
        auto *item = qobject_cast<QQuickItem *>(candidate);
        if (item != nullptr && item->window() == window && item->isVisible()) {
            modeSwitch = candidate;
            break;
        }
    }
    QVERIFY2(modeSwitch != nullptr, "Visualization mode switch was not created");
    QCOMPARE(modeSwitch->property("selectedMode").toInt(), 0);
    // Two 24 px tool buttons on one plate, 2 px apart with 2 px around.
    QCOMPARE(modeSwitch->property("width").toDouble(), 54.0);
    QCOMPARE(modeSwitch->property("height").toDouble(), 28.0);
    QCOMPARE(modeSwitch->property("pointerHovered").toBool(), false);
    QJSValue setMode = qvariant_cast<QJSValue>(modeSwitch->property("setMode"));
    QVERIFY(setMode.isCallable());
    setMode.call(QJSValueList{QJSValue(1)});
    QTRY_COMPARE(modeSwitch->property("selectedMode").toInt(), 1);
    setMode.call(QJSValueList{QJSValue(0)});

    bool invoked = QMetaObject::invokeMethod(root, "openItunesArtworkDialog");
    QVERIFY(invoked);
    QObject *itunesDialog = findObjectByStringProperty(root, "title", QStringLiteral("Replace from iTunes"));
    QVERIFY2(itunesDialog != nullptr, "Could not find iTunes artwork dialog instance");
    QTRY_VERIFY(itunesDialog->property("visible").toBool());
}

void WindowSmokeTest::loadsExtractedQmlSlicesWithFallbackProps() {
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
        property var playbackClock: null
        property string durationText: "00:00"
        property real durationSeconds: 0
        property real volume: 0.5
        property var queueRows: []
        property int queueLength: queueRows.length
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
        property int currentTrackTotal: 0
        property int currentTrackChannels: 0
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
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property var soloedChannels: []
        property int channelButtonsVisibility: 1
        property int viewerFullscreenMode: 0
        property bool preventDisplaySleepInFullscreen: true
        property bool showFullscreenTitleStrip: true
        property bool showLevelMeter: true
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
        property int lastFmAuthState: 0
        property bool lastFmShowScrobbleProgress: true
        property bool lastFmScrobblingPaused: false
        property int lastFmScrobbleState: 0
        property int lastFmScrobbleIneligibility: 0
        property int lastFmScrobbleListenedMs: 0
        property int lastFmScrobbleThresholdMs: 0
        property string lastFmScrobbleMessage: ""
        property bool systemMediaControlsEnabled: true
        property bool libraryWatchEnabled: true
        property var mutedChannelsMask: 0
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property int sampleRateHz: 48000
        signal diagnosticsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal precomputedSpectrogramChunkReady(var data, int bins, int channelCount, int columns,
            int startIndex, int totalEstimate, int sampleRate, int hopSize,
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken, var generation)
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
        signal playbackChanged()
        function setVolume(value) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
        function isChannelSilent(channelIndex) { return false }
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
        function setSpectrogramFrequencyCap(value) {}
        function setShowFps(value) {}
        function setShowSpectrogramCrosshair(value) {}
        function setShowSpectrogramScale(value) {}
        function setSpectrogramZoomEnabled(value) {}
        function setSpectrogramZoomLevel(level) {}
        function setSpectrogramWidgetWidth(width) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setViewerFullscreenMode(mode) {}
        function setPreventDisplaySleepInFullscreen(value) {}
        function setShowFullscreenTitleStrip(value) { showFullscreenTitleStrip = value }
        function setShowLevelMeter(value) { showLevelMeter = value }
        function setLastFmScrobblingEnabled(value) {}
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
        function setLastFmScrobblingPaused(value) {}
        function setLastFmShowScrobbleProgress(value) {}
        function setSystemMediaControlsEnabled(value) {}
        function setLibraryWatchEnabled(value) { libraryWatchEnabled = value }
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
        function libraryThumbnailSource(path, stamp) { return "" }
        function queuePathAt(index) { return "" }
        function playAt(index) {}
        function removeAt(index) {}
        function removeIndices(indices) {}
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
        property bool statusIsError: false
        property int selectedCount: 0
        property var operationFiles: []
        property var tableModel: []
        signal selectionChanged()
        signal bulkSummaryChanged()
        function close() {}
        function reload() {}
        function save() { return true }
        function renameSelectedFiles() {}
        function setSelectedRows(rows) {}
        function selectedRows() { return [] }
        function selectRange(first, last) {}
        function toggleRowSelected(row) {}
        function selectAllRows() {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
        function bulkValueMixed(field) { return false }
        function extraValuesNote(field) { return "" }
        function bulkFieldChanged(field) { return false }
        function bulkOriginalValue(field) { return "" }
        function bulkOriginalValueMixed(field) { return false }
        function revertBulkField(field) {}
        function fieldChanged(field) { return false }
        function revertField(field) {}
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
    Action { id: replaceFromItunesAction }
    Action { id: addLibraryFolderAction }

    Dialogs.PreferencesDialog {
        id: preferencesDialog
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        spectrogramFftChoices: [512, 1024, 2048]
        promptAddLibraryRoot: function(context) {}
        openLibraryRootNameDialog: function(mode, path, name) {}
    }

    Dialogs.DiagnosticsDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
    }

    Dialogs.LibraryRootNameDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
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
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
    }

    Dialogs.ItunesArtworkDialog {
        parent: harness
        uiBridge: bridge
        uiPalette: palette
        windowRoot: harness
        openAlbumArtViewerForSuggestion: function(row) {}
        openAlbumArtViewerForCurrentArt: function() {}
        endAlbumArtPreview: function(applied) {}
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
        volumeMuted: playbackController.volumeMuted
        keyboardSeekStepSeconds: 5
        displayedPositionSeconds: playbackController.positionSeconds
        toggleMutedVolume: playbackController.toggleMutedVolume
        setAppVolume: playbackController.setAppVolume
        normalizedVolumeValue: playbackController.normalizedVolumeValue
        seekCommitted: playbackController.seekCommitted
    }

    Components.NowPlayingPanel {
        parent: harness
        width: 420
        uiBridge: bridge
        uiPalette: palette
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        openAlbumArtViewer: function() {}
    }

    Viewers.AlbumArtViewerShell {
        parent: harness
        windowRoot: viewerWindowRoot
        uiPalette: palette
        viewerOpen: false
        useWholeScreenViewerMode: false
        titleText: "Ferrous"
        closeViewer: function() {}
        toggleInfoVisible: function() {}
        switchComparisonImage: function() {}
        openContextMenu: function() {}
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
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: palette
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
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        stepScrollView: function(view, wheel, stepSize, stepsPerWheel) {}
        addLibraryFolderAction: addLibraryFolderAction
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
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
        droppedExternalPaths: function(drop) { return [] }
    }

    Viewers.SpectrogramSurface {
        parent: harness
        width: 420
        height: 160
        uiBridge: bridge
        uiPalette: palette
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
}

void WindowSmokeTest::mainWindowContentStartsBelowMenuBar() {
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

    QObject *menuBar = qvariant_cast<QObject *>(root->property("menuBar"));
    QObject *contentItem = qvariant_cast<QObject *>(root->property("contentItem"));
    QVERIFY(menuBar != nullptr);
    QVERIFY(contentItem != nullptr);
    QTRY_VERIFY(menuBar->property("height").toReal() > 0.0);
    QVERIFY2(
        contentItem->property("y").toReal() >= menuBar->property("height").toReal(),
        "Application content must start below the menu bar");
}

namespace {

// The item at `index` of the split view `split`.
QQuickItem *splitItemAt(QQuickItem *split, int index) {
    QQuickItem *item = nullptr;
    if (split == nullptr
        || !QMetaObject::invokeMethod(split, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, index))) {
        return nullptr;
    }
    return item;
}

// Drags the handle between `item` and `next`, the item after it in their
// split view, by `delta`, as the user does.
void dragSplitHandleBetween(QQuickWindow *window, QQuickItem *item, QQuickItem *next,
                            Qt::Orientation orientation, int delta) {
    const bool horizontal = orientation == Qt::Horizontal;
    // In the middle of the handle, which fills the gap between them.
    const QPointF end = item->mapToScene(QPointF(item->width(), item->height()));
    const QPointF start = next->mapToScene(QPointF(0, 0));
    QPoint at = horizontal
        ? QPointF((end.x() + start.x()) / 2, (start.y() + end.y()) / 2).toPoint()
        : QPointF((start.x() + end.x()) / 2, (end.y() + start.y()) / 2).toPoint();
    QTest::mouseMove(window, at);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
    const QPoint step = horizontal ? QPoint(delta / 10, 0) : QPoint(0, delta / 10);
    for (int i = 0; i < 10; ++i) {
        at += step;
        QTest::mouseMove(window, at);
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at);
}

} // namespace

void WindowSmokeTest::draggedSplitSizesSurviveAWindowResize() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    window->resize(1400, 860);
    QTRY_COMPARE(window->width(), 1400);
    QTRY_COMPARE(window->height(), 860);
    auto *mainSplit = window->findChild<QQuickItem *>(QStringLiteral("mainSplit"));
    QQuickItem *sidebar = splitItemAt(mainSplit, 0);
    QQuickItem *rightSplit = splitItemAt(mainSplit, 1);
    QQuickItem *queue = splitItemAt(rightSplit, 0);
    QVERIFY(mainSplit != nullptr && sidebar != nullptr && rightSplit != nullptr
            && queue != nullptr);

    // The splits have been laid out for the window when their last items
    // reach their ends.
    QQuickItem *visualization = splitItemAt(rightSplit, 1);
    QVERIFY(visualization != nullptr);
    const auto laidOut = [&]() {
        return qFuzzyCompare(rightSplit->x() + rightSplit->width(), mainSplit->width())
            && qFuzzyCompare(visualization->y() + visualization->height(), rightSplit->height());
    };
    QTRY_COMPARE(mainSplit->width(), 1400.0);
    QTRY_VERIFY(laidOut());
    const qreal sidebarBefore = sidebar->width();
    dragSplitHandleBetween(window, sidebar, rightSplit, Qt::Horizontal, 200);
    QTRY_VERIFY2(sidebar->width() > sidebarBefore + 150,
                 qPrintable(QStringLiteral("%1 -> %2").arg(sidebarBefore).arg(sidebar->width())));
    const qreal queueBefore = queue->height();
    dragSplitHandleBetween(window, queue, visualization, Qt::Vertical, -100);
    QTRY_VERIFY2(queue->height() < queueBefore - 70,
                 qPrintable(QStringLiteral("%1 -> %2").arg(queueBefore).arg(queue->height())));
    const qreal sidebarShare = sidebar->width() / mainSplit->width();
    const qreal queueShare = queue->height() / rightSplit->height();

    // A larger window keeps the shares the user dragged to, rather than
    // the sizes the window would have by default.
    window->resize(1700, 1000);
    QTRY_COMPARE(window->width(), 1700);
    QTRY_COMPARE(window->height(), 1000);
    QTRY_COMPARE(mainSplit->width(), 1700.0);
    QTRY_VERIFY(laidOut());
    QTRY_VERIFY2(std::abs(sidebar->width() / mainSplit->width() - sidebarShare) < 0.01,
                 qPrintable(QStringLiteral("sidebar %1 of %2, was %3")
                                .arg(sidebar->width())
                                .arg(mainSplit->width())
                                .arg(sidebarShare)));
    QTRY_VERIFY2(std::abs(queue->height() / rightSplit->height() - queueShare) < 0.01,
                 qPrintable(QStringLiteral("queue %1 of %2, was %3")
                                .arg(queue->height())
                                .arg(rightSplit->height())
                                .arg(queueShare)));
    QVERIFY(finishIncubation(engine));
}

namespace {

// Whether `item` is shown, and inside `window`.
bool shownInside(QQuickWindow *window, QQuickItem *item) {
    if (item == nullptr || !item->isVisible()) {
        return false;
    }
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return rect.left() >= -0.5 && rect.top() >= -0.5 && rect.right() <= window->width() + 0.5
        && rect.bottom() <= window->height() + 0.5;
}

// Whether `items` are side by side from left to right without overlapping,
// each at least `minimumWidth` wide.
QString overlapsInRow(const QList<QQuickItem *> &items, qreal minimumWidth) {
    qreal right = -1e9;
    for (QQuickItem *item : items) {
        const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        if (rect.left() < right - 0.5 || rect.width() < minimumWidth) {
            return QStringLiteral("%1 at %2..%3 after %4")
                .arg(item->objectName())
                .arg(rect.left())
                .arg(rect.right())
                .arg(right);
        }
        right = rect.right();
    }
    return {};
}

// Closes `window` as the user does and waits until it has closed: after
// the backend has saved the settings and the queue.
bool closeThroughTheQuitFlow(QQuickWindow *window) {
    return QMetaObject::invokeMethod(window, "close")
        && QTest::qWaitFor([window]() { return !window->isVisible(); }, 15000);
}

QString savedSettingsPath() {
    return QDir(qEnvironmentVariable("XDG_CONFIG_HOME"))
        .filePath(QStringLiteral("ferrous/settings.txt"));
}

} // namespace

void WindowSmokeTest::theSmallestWindowFitsALaptopScreenAndShowsEveryControl() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    // A 1080p screen at 150% has 1280 x 720 logical pixels, of which a
    // panel and the window's frame take some.
    const int minimumWidth = window->minimumWidth();
    const int minimumHeight = window->minimumHeight();
    QVERIFY2(minimumWidth > 0 && minimumWidth <= 1280 - 16 && minimumHeight > 0
                 && minimumHeight <= 720 - 80,
             qPrintable(QStringLiteral("%1 x %2").arg(minimumWidth).arg(minimumHeight)));

    window->resize(minimumWidth, minimumHeight);
    QTRY_COMPARE(window->width(), minimumWidth);
    QTRY_COMPARE(window->height(), minimumHeight);
    auto *mainSplit = window->findChild<QQuickItem *>(QStringLiteral("mainSplit"));
    auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebarPane"));
    auto *rightSplit = window->findChild<QQuickItem *>(QStringLiteral("rightSplit"));
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    QQuickItem *visualization = splitItemAt(rightSplit, 1);
    auto *statusBar = qvariant_cast<QQuickItem *>(window->property("footer"));
    QVERIFY(mainSplit != nullptr && sidebar != nullptr && rightSplit != nullptr
            && queue != nullptr && visualization != nullptr && statusBar != nullptr);
    QTRY_COMPARE(mainSplit->width(), qreal(minimumWidth));
    QTRY_VERIFY(qFuzzyCompare(rightSplit->x() + rightSplit->width(), mainSplit->width()));
    QTRY_VERIFY(qFuzzyCompare(visualization->y() + visualization->height(), rightSplit->height()));

    // Every pane gets at least its own minimum.
    QVERIFY2(sidebar->width() >= 250 - 0.5, qPrintable(QString::number(sidebar->width())));
    QVERIFY2(rightSplit->width() >= queue->property("minimumContentWidth").toReal() - 0.5,
             qPrintable(QStringLiteral("%1 < %2")
                            .arg(rightSplit->width())
                            .arg(queue->property("minimumContentWidth").toReal())));
    QVERIFY2(queue->height() >= 220 - 0.5, qPrintable(QString::number(queue->height())));
    QVERIFY2(visualization->height() >= 220 - 0.5,
             qPrintable(QString::number(visualization->height())));
    QVERIFY(shownInside(window, mainSplit));
    QVERIFY(shownInside(window, statusBar));

    // The playback controls stay whole and apart.
    QList<QQuickItem *> controls;
    for (const char *name : {"transportButton_previous", "transportButton_playPause",
                             "transportButton_stop",
                             "transportButton_next", "seekSlider", "muteButton",
                             "volumeSlider"}) {
        QQuickItem *control = shownItemNamed(window->contentItem(), QString::fromLatin1(name));
        QVERIFY2(shownInside(window, control), name);
        controls.push_back(control);
    }
    const QString controlOverlap = overlapsInRow(controls, 8);
    QVERIFY2(controlOverlap.isEmpty(), qPrintable(controlOverlap));
    QQuickItem *seek = controls.at(4);
    QVERIFY2(seek->width() >= 100, qPrintable(QString::number(seek->width())));

    // So do the queue's columns, with room for the titles: Album drops
    // first.
    QCOMPARE(shownItemNamed(queue, QStringLiteral("queueHeader_album")), nullptr);
    QCOMPARE(shownItemNamed(queue, QStringLiteral("queueColumnHandle_album")), nullptr);
    QList<QQuickItem *> columns;
    for (const char *name : {"queueHeader_indicator", "queueHeader_order", "queueHeader_title",
                             "queueHeader_artist", "queueHeader_length"}) {
        QQuickItem *column = shownItemNamed(queue, QString::fromLatin1(name));
        QVERIFY2(shownInside(window, column), name);
        columns.push_back(column);
    }
    const QString columnOverlap = overlapsInRow(columns, 1);
    QVERIFY2(columnOverlap.isEmpty(), qPrintable(columnOverlap));
    QVERIFY2(columns.at(2)->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1,
             qPrintable(QString::number(columns.at(2)->width())));
    const QRectF queueRect = queue->mapRectToScene(QRectF(0, 0, queue->width(), queue->height()));
    const QRectF lengthRect = columns.last()->mapRectToScene(
        QRectF(0, 0, columns.last()->width(), columns.last()->height()));
    QVERIFY(lengthRect.right() <= queueRect.right() + 0.5);

    QVERIFY(finishIncubation(engine));
}

void WindowSmokeTest::windowLayoutSurvivesARestart() {
    // The bridge runs with the test's own config directory; see main().
    QFile::remove(savedSettingsPath());
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    // On one of the large test screens, which has room for the window.
    const QRect geometry(100'200, 150, 1300, 820);
    const QRect normalOnTheSmallScreen(40, 40, 880, 600);
    qreal sidebarShare = 0;
    qreal queueShare = 0;
    {
        BridgeClient bridge;
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        window->setGeometry(geometry);
        QTRY_COMPARE(window->geometry(), geometry);
        auto *mainSplit = window->findChild<QQuickItem *>(QStringLiteral("mainSplit"));
        auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebarPane"));
        auto *rightSplit = window->findChild<QQuickItem *>(QStringLiteral("rightSplit"));
        auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
        QQuickItem *visualization = splitItemAt(rightSplit, 1);
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *queueController = window->findChild<QObject *>(QStringLiteral("queueController"));
        QVERIFY(mainSplit != nullptr && sidebar != nullptr && rightSplit != nullptr
                && queue != nullptr && visualization != nullptr && viewer != nullptr
                && queueController != nullptr);
        QTRY_COMPARE(mainSplit->width(), 1300.0);
        QTRY_VERIFY(qFuzzyCompare(rightSplit->x() + rightSplit->width(), mainSplit->width()));
        QTRY_VERIFY(
            qFuzzyCompare(visualization->y() + visualization->height(), rightSplit->height()));
        const qreal sidebarBefore = sidebar->width();
        dragSplitHandleBetween(window, sidebar, rightSplit, Qt::Horizontal, 120);
        QTRY_VERIFY(sidebar->width() > sidebarBefore + 100);
        const qreal queueBefore = queue->height();
        dragSplitHandleBetween(window, queue, visualization, Qt::Vertical, -60);
        QTRY_VERIFY(queue->height() < queueBefore - 40);
        sidebarShare = sidebar->width() / mainSplit->width();
        queueShare = queue->height() / rightSplit->height();
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
        QVERIFY(queueController->setProperty("autoCenterSelection", false));

        QVERIFY(closeThroughTheQuitFlow(window));
    }
    QFile settings(savedSettingsPath());
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QByteArray saved = settings.readAll();
    settings.close();
    QVERIFY2(saved.contains("\nwindow_geometry=100200,150,1300,820\n"), saved.constData());
    QVERIFY2(saved.contains("\nvisualization_mode=1\n"), saved.constData());
    QVERIFY2(saved.contains("\nfollow_current_track=0\n"), saved.constData());

    {
        BridgeClient bridge;
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QCOMPARE(window->geometry(), geometry);
        auto *mainSplit = window->findChild<QQuickItem *>(QStringLiteral("mainSplit"));
        auto *sidebar = window->findChild<QQuickItem *>(QStringLiteral("sidebarPane"));
        auto *rightSplit = window->findChild<QQuickItem *>(QStringLiteral("rightSplit"));
        auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *queueController =
            window->findChild<QObject *>(QStringLiteral("queueController"));
        QTRY_VERIFY2(std::abs(sidebar->width() / mainSplit->width() - sidebarShare) < 0.01,
                     qPrintable(QStringLiteral("%1 of %2, was %3")
                                    .arg(sidebar->width())
                                    .arg(mainSplit->width())
                                    .arg(sidebarShare)));
        QTRY_VERIFY2(std::abs(queue->height() / rightSplit->height() - queueShare) < 0.01,
                     qPrintable(QStringLiteral("%1 of %2, was %3")
                                    .arg(queue->height())
                                    .arg(rightSplit->height())
                                    .arg(queueShare)));
        QCOMPARE(viewer->property("visualizationMode").toInt(), 1);
        QCOMPARE(queueController->property("autoCenterSelection").toBool(), false);
        QObject *followAction = nullptr;
        for (QObject *object : window->findChildren<QObject *>()) {
            if (object->property("text").toString()
                == QStringLiteral("&Follow Current Track in Queue")) {
                followAction = object;
            }
        }
        QVERIFY(followAction != nullptr);
        QCOMPARE(followAction->property("checked").toBool(), false);

        // Maximized, it comes back maximized, keeping the geometry it has
        // when it is not. On the small test screen, so that the
        // visualization does not grow as large as the large ones, which
        // the spectrogram's buffers would keep for later tests.
        window->setGeometry(normalOnTheSmallScreen);
        QTRY_COMPARE(window->geometry(), normalOnTheSmallScreen);
        window->showMaximized();
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        QVERIFY(window->geometry() != normalOnTheSmallScreen);
        // Quit while minimized, as from the desktop's media controls.
        window->showMinimized();
        QTRY_COMPARE(window->visibility(), QWindow::Minimized);
        QVERIFY(closeThroughTheQuitFlow(window));
    }
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QByteArray savedMaximized = settings.readAll();
    settings.close();
    QVERIFY2(savedMaximized.contains("\nwindow_geometry=40,40,880,600\n"),
             savedMaximized.constData());
    QVERIFY2(savedMaximized.contains("\nwindow_maximized=1\n"), savedMaximized.constData());
    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(window->visibility(), QWindow::Maximized);
    QObject *layout = window->findChild<QObject *>(QStringLiteral("windowLayoutController"));
    QVERIFY(layout != nullptr);
    // No wider than the 800 x 800 screen it was saved on.
    QCOMPARE(layout->property("normalGeometry").toRectF(), QRectF(0, 40, 800, 600));
    QVERIFY(closeThroughTheQuitFlow(window));
    QFile::remove(savedSettingsPath());
}

void WindowSmokeTest::aWindowSavedOffTheScreensOpensOnOne() {
    // As if it was saved on a monitor that has been disconnected since.
    QFile::remove(savedSettingsPath());
    QVERIFY(QDir().mkpath(QFileInfo(savedSettingsPath()).path()));
    QFile settings(savedSettingsPath());
    QVERIFY(settings.open(QIODevice::WriteOnly));
    settings.write("window_geometry=50000,40000,1200,700\nwindow_maximized=0\n");
    settings.close();
    const auto restoreQuit = keepRunningWhenTheWindowCloses();

    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    bool onAScreen = false;
    for (const QScreen *screen : QGuiApplication::screens()) {
        onAScreen |= screen->geometry().contains(window->geometry().center());
    }
    QVERIFY2(onAScreen, qPrintable(QStringLiteral("%1,%2 %3x%4")
                                       .arg(window->x())
                                       .arg(window->y())
                                       .arg(window->width())
                                       .arg(window->height())));
    QVERIFY(closeThroughTheQuitFlow(window));
    QFile::remove(savedSettingsPath());
}

void WindowSmokeTest::queueColumnWidthsSurviveARestart() {
    // Saved by a build that also had a Genre column, which this one keeps,
    // on one of the large test screens, which has room for the columns.
    QFile::remove(savedSettingsPath());
    QVERIFY(QDir().mkpath(QFileInfo(savedSettingsPath()).path()));
    QFile settings(savedSettingsPath());
    QVERIFY(settings.open(QIODevice::WriteOnly));
    settings.write("window_geometry=100200,150,1300,820\n"
                   "queue_column_widths=artist:260,genre:99,album:9\nfuture_key=1\n");
    settings.close();
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto header = [](QQuickWindow *window, const char *column) {
        auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
        return queue != nullptr
            ? shownItemNamed(queue, QStringLiteral("queueHeader_") + QLatin1String(column))
            : nullptr;
    };
    qreal album = 0;
    {
        BridgeClient bridge;
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QQuickItem *artist = header(window, "artist");
        QQuickItem *albumHeader = header(window, "album");
        QVERIFY(artist != nullptr && albumHeader != nullptr);
        // As the window is first shown, without a jump from the default.
        QCOMPARE(artist->width(), 260.0);
        // The album's width was out of range: its default.
        const qreal defaultAlbum = albumHeader->width();
        QVERIFY(defaultAlbum > 100);

        QQuickItem *handle = shownItemNamed(window->contentItem(),
                                            QStringLiteral("queueColumnHandle_album"));
        QVERIFY(handle != nullptr);
        QPoint at = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint();
        QTest::mouseMove(window, at);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at);
        for (int step = 0; step < 5; ++step) {
            at -= QPoint(8, 0);
            QTest::mouseMove(window, at);
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(albumHeader->width(), defaultAlbum + 40);
        album = albumHeader->width();
        QVERIFY(closeThroughTheQuitFlow(window));
    }
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QByteArray saved = settings.readAll();
    settings.close();
    const QByteArray line = "\nqueue_column_widths=artist:260,album:" + QByteArray::number(int(album))
        + ",genre:99\n";
    QVERIFY2(saved.contains(line), saved.constData());
    QVERIFY2(saved.contains("\nfuture_key=1\n"), saved.constData());

    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(header(window, "artist")->width(), 260.0);
    QCOMPARE(header(window, "album")->width(), album);

    // In the narrowest window Album drops and Artist gives way, so the title
    // keeps its minimum, and they come back as the window grows again.
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    const QRect wide = window->geometry();
    window->resize(window->minimumWidth(), window->minimumHeight());
    QTRY_COMPARE(window->width(), window->minimumWidth());
    QTRY_VERIFY(header(window, "artist")->width() < 260.0);
    QVERIFY(header(window, "title")->width()
            >= queue->property("minimumTitleColumnWidth").toReal() - 1);
    QCOMPARE(header(window, "album"), nullptr);
    window->setGeometry(wide);
    QTRY_COMPARE(header(window, "artist")->width(), 260.0);
    QTRY_COMPARE(header(window, "album")->width(), album);
    QVERIFY(closeThroughTheQuitFlow(window));
    QFile::remove(savedSettingsPath());
}

namespace {

// A window with its layout controller, over a bridge that sends the saved
// layout only when the test calls sendLayout.
const QByteArray kWindowLayoutHarness = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import "controllers" as Controllers

Window {
    id: harness
    visible: false
    property alias controller: layoutController
    property var sentLayouts: []

    QtObject {
        id: fakeBridge
        property bool sent: false
        property var layout: ({})
        signal savedWindowLayoutRead()
        function hasSavedWindowLayout() { return fakeBridge.sent }
        function savedWindowLayout() { return fakeBridge.layout }
        function setWindowLayout(layout) {
            harness.sentLayouts = harness.sentLayouts.concat([layout])
        }
    }
    QtObject {
        id: fakeViewer
        property int visualizationMode: 0
    }
    QtObject {
        id: fakeQueue
        property bool autoCenterSelection: true
    }

    function sendLayout(layout) {
        fakeBridge.layout = layout
        fakeBridge.sent = true
        fakeBridge.savedWindowLayoutRead()
    }

    Controllers.WindowLayoutController {
        id: layoutController
        uiBridge: fakeBridge
        window: harness
        viewerController: fakeViewer
        queueController: fakeQueue
        layoutDeadlineMs: 300
    }

    width: layoutController.initialWidth
    height: layoutController.initialHeight
    Component.onCompleted: layoutController.showWindow()
}
)QML");

// Has the harness's bridge send `layout` as the saved layout.
bool sendSavedLayout(QQuickWindow *harness, const QVariantMap &layout) {
    return QMetaObject::invokeMethod(harness, "sendLayout", Q_ARG(QVariant, QVariant(layout)));
}

QQuickWindow *createWindowLayoutHarness(QQmlEngine &engine, QString *errorText) {
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    return qobject_cast<QQuickWindow *>(
        createQmlObjectFromSource(engine, kWindowLayoutHarness, baseUrl, errorText));
}

} // namespace

void WindowSmokeTest::theWindowOpensWithoutASavedLayoutThatDoesNotCome() {
    QQmlEngine engine;
    QString errorText;
    QScopedPointer<QQuickWindow> window(createWindowLayoutHarness(engine, &errorText));
    QVERIFY2(window != nullptr, qPrintable(errorText));
    // Held back for the saved layout...
    QVERIFY(!window->isVisible());
    // ...but not beyond the deadline, as for settings on a hung drive.
    QTRY_VERIFY_WITH_TIMEOUT(window->isVisible(), 3000);
    QCOMPARE(window->visibility(), QWindow::Windowed);
    QCOMPARE(window->size(), QSize(1600, 980));
    // A layout that comes later does not move the open window.
    QVERIFY(sendSavedLayout(window.get(), QVariantMap{{QStringLiteral("x"), 10}, {QStringLiteral("y"), 10},
                                    {QStringLiteral("width"), 900},
                                    {QStringLiteral("height"), 600},
                                    {QStringLiteral("maximized"), true}}));
    QCoreApplication::processEvents();
    QCOMPARE(window->size(), QSize(1600, 980));
    QCOMPARE(window->visibility(), QWindow::Windowed);

    // One that comes in time is applied before the window is shown.
    QScopedPointer<QQuickWindow> second(createWindowLayoutHarness(engine, &errorText));
    QVERIFY2(second != nullptr, qPrintable(errorText));
    QSignalSpy shown(second.get(), &QWindow::visibleChanged);
    QVERIFY(sendSavedLayout(second.get(), QVariantMap{{QStringLiteral("x"), 40}, {QStringLiteral("y"), 50},
                                    {QStringLiteral("width"), 900},
                                    {QStringLiteral("height"), 600},
                                    {QStringLiteral("maximized"), false}}));
    QVERIFY(second->isVisible());
    QCOMPARE(shown.count(), 1);
    QCOMPARE(second->size(), QSize(900, 600));
}

void WindowSmokeTest::quittingWhileMinimizedKeepsTheWindowMaximized() {
    QQmlEngine engine;
    QString errorText;
    QScopedPointer<QQuickWindow> window(createWindowLayoutHarness(engine, &errorText));
    QVERIFY2(window != nullptr, qPrintable(errorText));
    QVERIFY(sendSavedLayout(window.get(), QVariantMap{{QStringLiteral("x"), 40}, {QStringLiteral("y"), 40},
                                    {QStringLiteral("width"), 700},
                                    {QStringLiteral("height"), 500},
                                    {QStringLiteral("maximized"), false}}));
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    QObject *controller = window->property("controller").value<QObject *>();
    QVERIFY(controller != nullptr);
    const auto savedLayout = [&]() {
        QVariant layout;
        QMetaObject::invokeMethod(controller, "layout", Q_RETURN_ARG(QVariant, layout));
        return layout.toMap();
    };

    window->showMaximized();
    QTRY_COMPARE(window->visibility(), QWindow::Maximized);
    window->showMinimized();
    QTRY_COMPARE(window->visibility(), QWindow::Minimized);
    // As MPRIS or the task manager quits it now.
    QVERIFY(QMetaObject::invokeMethod(controller, "persist"));
    const QVariantList sent = window->property("sentLayouts").toList();
    QVERIFY(!sent.isEmpty());
    const QVariantMap last = sent.last().toMap();
    QCOMPARE(last.value(QStringLiteral("maximized")).toBool(), true);
    QCOMPARE(last.value(QStringLiteral("width")).toInt(), 700);
    QCOMPARE(last.value(QStringLiteral("height")).toInt(), 500);
    QCOMPARE(savedLayout().value(QStringLiteral("maximized")).toBool(), true);

    // Shown normally and minimized again, it is saved as not maximized.
    window->showNormal();
    QTRY_COMPARE(window->visibility(), QWindow::Windowed);
    window->showMinimized();
    QTRY_COMPARE(window->visibility(), QWindow::Minimized);
    QCOMPARE(savedLayout().value(QStringLiteral("maximized")).toBool(), false);
}

void WindowSmokeTest::aWindowOffScreenStopsTheClockAndTheFeeds() {
    // On Wayland a minimized window, or one whose display is off, keeps its
    // visibility and only stops being exposed. Once it has stayed so for a
    // while, the clock's frame updates, the level meter and the views'
    // frames stop (BridgeClientTest covers the analysis); a brief
    // unexposure, such as a desktop switch, stops nothing. Back on screen,
    // they show playback as it is then.
    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    PlaybackClock *clock = bridge.playbackClock();
    qint64 nowNs = monotonicNowNs();
    clock->setTimeSource([&nowNs]() { return nowNs; });
    QTRY_VERIFY(clock->frameUpdatesEnabled());
    // Playing, as the engine reports it. The backend plays nothing, and
    // no event loop runs between these samples and the checks of them.
    const auto play = [&](double position) {
        PlaybackSample sample;
        sample.positionSeconds = position;
        sample.sampledAtNs = nowNs;
        sample.advancing = true;
        sample.durationSeconds = 300.0;
        sample.trackToken = 1;
        clock->applySample(sample, true);
    };
    play(10.0);
    QVERIFY(clock->nextTickNs().has_value());
    auto *meter = window->findChild<LevelMeterItem *>(QStringLiteral("levelMeter"));
    QVERIFY(meter != nullptr);
    meter->setVisible(true);
    QVERIFY(meter->visibleForPlayback());
    QSignalSpy feedChanges(clock, &PlaybackClock::frameUpdatesEnabledChanged);

    setTestExposed(*window, false);
    QCoreApplication::processEvents();
    QVERIFY(clock->frameUpdatesEnabled());
    QVERIFY(meter->visibleForPlayback());
    setTestExposed(*window, true);
    QCOMPARE(feedChanges.size(), 0);

    setTestExposed(*window, false);
    endTestUnexposedGrace(*window);
    QVERIFY(!clock->frameUpdatesEnabled());
    QVERIFY(!clock->nextTickNs().has_value());
    QVERIFY(!meter->visibleForPlayback());
    // The panes the repeater made are items of the scene, not children.
    QList<SpectrogramItem *> panes;
    std::function<void(QQuickItem *)> collect = [&panes, &collect](QQuickItem *item) {
        if (auto *pane = qobject_cast<SpectrogramItem *>(item)) {
            panes.push_back(pane);
        }
        for (QQuickItem *child : item->childItems()) {
            collect(child);
        }
    };
    collect(window->contentItem());
    QVERIFY(!panes.isEmpty());
    for (SpectrogramItem *pane : panes) {
        QVERIFY(!pane->showsPlaybackFrames());
        QVERIFY(pane->m_frameScheduler == nullptr || pane->m_frameScheduler->dueNs(pane) < 0);
    }
    // The engine's samples still move it, and nothing else does.
    nowNs += 5'000'000'000;
    play(15.0);
    QCOMPARE(clock->positionSeconds(), 15.0);
    QVERIFY(!clock->nextTickNs().has_value());
    nowNs += 2'000'000'000;
    QCOMPARE(clock->positionSeconds(), 15.0);

    setTestExposed(*window, true);
    QVERIFY(clock->frameUpdatesEnabled());
    QCOMPARE(clock->positionSeconds(), 17.0);
    QCOMPARE(clock->positionText(), QStringLiteral("00:17"));
    QVERIFY(clock->nextTickNs().has_value());
    QVERIFY(meter->visibleForPlayback());
    for (SpectrogramItem *pane : panes) {
        QCOMPARE(pane->showsPlaybackFrames(), pane->isVisible());
    }
    clock->setTimeSource({});
    QVERIFY(closeThroughTheQuitFlow(window));
}

void WindowSmokeTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void WindowSmokeTest::drawingWritesNoShaderCache() {
    // Mesa's thread wrote one into the directories of the first test that
    // drew while they were removed, and that test failed.
    QCOMPARE(qgetenv("MESA_SHADER_CACHE_DISABLE"), QByteArray("true"));
    QCOMPARE(qgetenv("AMD_COMGR_CACHE"), QByteArray("0"));
}

std::unique_ptr<QmlSmokeTest> makeWindowSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WindowSmokeTest>(userDirectoriesParent);
}

// A track whose tag lacks its title, artist and album, after a tagged one
// and before its queue row comes: the panel shows the new track's file name
// and nothing of the track before, as BridgeClient tells it (#560).
void WindowSmokeTest::nowPlayingShowsNoTextOfTheTrackBefore() {
    BridgeClient bridge;
    // Only the snapshots below reach it, not the real bridge's.
    bridge.m_bridgePollTimer.stop();
    if (bridge.m_bridgeWakeNotifier != nullptr) {
        bridge.m_bridgeWakeNotifier->setEnabled(false);
        delete bridge.m_bridgeWakeNotifier;
        bridge.m_bridgeWakeNotifier = nullptr;
    }
    bridge.m_bridgeWakeFd = -1;
    bridge.shutdownBridgeGracefully();
    if (bridge.m_ffiBridge != nullptr) {
        ferrous_ffi_bridge_destroy(bridge.m_ffiBridge);
        bridge.m_ffiBridge = nullptr;
    }
    bridge.m_connected = false;

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    auto *panel = window->findChild<QQuickItem *>(QStringLiteral("nowPlayingPanel"));
    auto *title = window->findChild<QQuickItem *>(QStringLiteral("nowPlayingTitle"));
    auto *artistAlbum = window->findChild<QQuickItem *>(QStringLiteral("nowPlayingArtistAlbum"));
    QVERIFY(panel != nullptr && title != nullptr && artistAlbum != nullptr);
    const auto deliver = [&bridge](const BinaryBridgeCodec::DecodedSnapshot &snapshot) {
        bridge.resetPollChanges();
        const bool applied = bridge.processBinarySnapshot(snapshot);
        bridge.schedulePollChanges();
        return applied;
    };

    const QString taggedPath = QStringLiteral("/fixture/tagged.flac");
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = taggedPath;
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = taggedPath;
    snapshot.metadata.title = QStringLiteral("Tagged Title");
    snapshot.metadata.artist = QStringLiteral("Tagged Artist");
    snapshot.metadata.album = QStringLiteral("Tagged Album");
    QVERIFY(deliver(snapshot));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Tagged Title"));
    QTRY_COMPARE(artistAlbum->property("text").toString(),
                 QStringLiteral("Tagged Artist — Tagged Album"));

    // The next track, with the metadata still the track before's.
    snapshot.playback.currentPath = QStringLiteral("/fixture/untitled take.flac");
    QVERIFY(deliver(snapshot));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("untitled take.flac"));
    QTRY_COMPARE(artistAlbum->property("text").toString(), QString());

    // Its own metadata, which has none of the three.
    snapshot.metadata = {};
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = snapshot.playback.currentPath;
    QVERIFY(deliver(snapshot));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("untitled take.flac"));
    QTRY_COMPARE(artistAlbum->property("text").toString(), QString());
    QCOMPARE(panel->property("titleText").toString(), QStringLiteral("untitled take.flac"));
    QCOMPARE(panel->property("artistAlbumText").toString(), QString());
    QCOMPARE(bridge.currentTrackTitle(), QStringLiteral("untitled take.flac"));
    QVERIFY(bridge.currentTrackArtist().isEmpty());
    QVERIFY(bridge.currentTrackAlbum().isEmpty());
}

#include "tst_qml_smoke_window.moc"
