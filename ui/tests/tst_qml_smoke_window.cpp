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
#include "TestBridgeStart.h"

#include <QGuiApplication>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>

// The main window, its layout, and what survives a restart.
class WindowSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void loadsMainQmlWithFallbackBridge();
    void loadsExtractedQmlSlicesWithFallbackProps();
    void splitsFitTheSmallestWindowAndKeepTheirDraggedShares();
    void windowLayoutSurvivesARestart();
    void aWindowSavedOffTheScreensOpensOnOne();
    void unmuteShowsTheVolumeAtOnce();
    void theWindowOpensWithoutASavedLayoutThatDoesNotCome();
    void quittingWhileMinimizedKeepsTheWindowMaximized();
    void aWindowOffScreenStopsTheClockAndTheFeeds();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void drawingWritesNoShaderCache();
    void nowPlayingFollowsBridgeClientsSnapshots();
    void aBackendThatCannotStartShowsWhyInRecentErrorsAndTheStatusBar();
};

void WindowSmokeTest::loadsMainQmlWithFallbackBridge() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *root = window;

    // The content starts below the menu bar.
    QObject *menuBar = qvariant_cast<QObject *>(root->property("menuBar"));
    QObject *contentItem = qvariant_cast<QObject *>(root->property("contentItem"));
    QVERIFY(menuBar != nullptr && contentItem != nullptr);
    QTRY_VERIFY(menuBar->property("height").toReal() > 0.0);
    QVERIFY2(contentItem->property("y").toReal() >= menuBar->property("height").toReal(),
             "Application content must start below the menu bar");

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
    // Two 24 px tool buttons on one plate, 4 px apart with 4 px around,
    // which keeps a focus ring inside it with 1 px of plate outside it.
    QCOMPARE(modeSwitch->property("width").toDouble(), 60.0);
    QCOMPARE(modeSwitch->property("height").toDouble(), 32.0);
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
        property bool stretchShortTracks: true
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
        property int lastFmDroppedScrobbleCount: 0
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
        function setStretchShortTracks(value) { stretchShortTracks = value }
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
        function copyDiagnosticsWithoutPaths() {}
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

    // A window of its own, for a window.
    Dialogs.PreferencesDialog {
        id: preferencesDialog
        uiBridge: bridge
        uiPalette: palette
        windowRoot: viewerWindowRoot
        spectrogramFftChoices: [512, 1024, 2048]
        promptAddLibraryRoot: function(context) {}
        openLibraryRootNameDialog: function(mode, path, name) {}
    }

    // Windows of their own, for a window.
    Dialogs.DiagnosticsDialog {
        uiBridge: bridge
        uiPalette: palette
        windowRoot: viewerWindowRoot
    }

    Dialogs.LibraryRootNameDialog {
        uiBridge: bridge
        uiPalette: palette
        windowRoot: viewerWindowRoot
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
        uiBridge: bridge
        uiPalette: palette
        windowRoot: viewerWindowRoot
        openAlbumArtViewerForSuggestion: function(row) {}
        openAlbumArtViewerForCurrentArt: function() {}
        endAlbumArtPreview: function(applied) {}
    }

    Dialogs.TagEditorDialog {
        tagEditorApi: tagEditorApi
        uiPalette: palette
        windowRoot: viewerWindowRoot
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

// The main window's splits: the sidebar beside the right split, which
// holds the queue over the visualization.
struct MainSplits {
    explicit MainSplits(QQuickWindow *window)
        : main(window->findChild<QQuickItem *>(QStringLiteral("mainSplit"))),
          sidebar(window->findChild<QQuickItem *>(QStringLiteral("sidebarPane"))),
          right(window->findChild<QQuickItem *>(QStringLiteral("rightSplit"))),
          queue(window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"))),
          visualization(splitItemAt(right, 1)) {}

    bool found() const {
        return main != nullptr && sidebar != nullptr && right != nullptr && queue != nullptr
            && visualization != nullptr;
    }
    // Laid out for the window: their last items reach their ends.
    bool laidOut() const {
        return qFuzzyCompare(right->x() + right->width(), main->width())
            && qFuzzyCompare(visualization->y() + visualization->height(), right->height());
    }
    qreal sidebarShare() const { return sidebar->width() / main->width(); }
    qreal queueShare() const { return queue->height() / right->height(); }
    // Drags the sidebar's handle by `sidebarDelta` and the queue's by
    // `queueDelta`, as the user does; whether each moved most of the way.
    bool drag(QQuickWindow *window, int sidebarDelta, int queueDelta) const {
        const qreal sidebarBefore = sidebar->width();
        dragSplitHandleBetween(window, sidebar, right, Qt::Horizontal, sidebarDelta);
        if (!QTest::qWaitFor([&]() { return sidebar->width() > sidebarBefore + sidebarDelta * 2.0 / 3; })) {
            return false;
        }
        const qreal queueBefore = queue->height();
        dragSplitHandleBetween(window, queue, visualization, Qt::Vertical, queueDelta);
        return QTest::qWaitFor([&]() { return queue->height() < queueBefore + queueDelta * 2.0 / 3; });
    }
    // Why the splits do not hold the shares `sidebar` and `queue`, or "".
    QString sharesProblem(qreal sidebarWas, qreal queueWas) const {
        if (std::abs(sidebarShare() - sidebarWas) < 0.01 && std::abs(queueShare() - queueWas) < 0.01) {
            return {};
        }
        return QStringLiteral("sidebar %1 of %2, was %3; queue %4 of %5, was %6")
            .arg(sidebar->width()).arg(main->width()).arg(sidebarWas)
            .arg(queue->height()).arg(right->height()).arg(queueWas);
    }

    QQuickItem *main;
    QQuickItem *sidebar;
    QQuickItem *right;
    QQuickItem *queue;
    QQuickItem *visualization;
};

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

// The settings the bridge saves, in the test's own config directory; see
// main().
QString savedSettingsPath() {
    return QDir(qEnvironmentVariable("XDG_CONFIG_HOME"))
        .filePath(QStringLiteral("ferrous/settings.txt"));
}

// Saves `settings` as the bridge's saved settings, as an earlier run left
// them.
bool writeSavedSettings(const QByteArray &settings) {
    QFile::remove(savedSettingsPath());
    QFile file(savedSettingsPath());
    return QDir().mkpath(QFileInfo(savedSettingsPath()).path()) && file.open(QIODevice::WriteOnly)
        && file.write(settings) == settings.size();
}

QByteArray readSavedSettings() {
    QFile file(savedSettingsPath());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The header of the queue's column `column`, while it shows.
QQuickItem *queueHeader(QQuickWindow *window, const char *column) {
    auto *queue = window->findChild<QQuickItem *>(QStringLiteral("mainQueuePane"));
    return queue != nullptr ? shownItemNamed(queue, QStringLiteral("queueHeader_") + QLatin1String(column))
                            : nullptr;
}

} // namespace

// The smallest window fits a laptop screen and shows every control whole;
// in a larger one, the splits keep the shares the user dragged them to as
// the window grows, rather than the sizes the window would have by default.
void WindowSmokeTest::splitsFitTheSmallestWindowAndKeepTheirDraggedShares() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    const MainSplits splits(window);
    auto *statusBar = qvariant_cast<QQuickItem *>(window->property("footer"));
    QVERIFY(splits.found() && statusBar != nullptr);
    // A 1080p screen at 150% has 1280 x 720 logical pixels, of which a
    // panel and the window's frame take some.
    const int minimumWidth = window->minimumWidth();
    const int minimumHeight = window->minimumHeight();
    QVERIFY2(minimumWidth > 0 && minimumWidth <= 1280 - 16 && minimumHeight > 0
                 && minimumHeight <= 720 - 80,
             qPrintable(QStringLiteral("%1 x %2").arg(minimumWidth).arg(minimumHeight)));

    window->resize(minimumWidth, minimumHeight);
    QTRY_COMPARE(window->size(), QSize(minimumWidth, minimumHeight));
    QTRY_COMPARE(splits.main->width(), qreal(minimumWidth));
    QTRY_VERIFY(splits.laidOut());

    // Every pane gets at least its own minimum.
    const auto atLeastItsMinimum = [](QQuickItem *pane, qreal size, const QString &minimum) {
        const qreal least = evaluateOn(pane, QStringLiteral("SplitView.") + minimum).toReal();
        return least > 0 && size >= least - 0.5;
    };
    QVERIFY2(atLeastItsMinimum(splits.sidebar, splits.sidebar->width(), QStringLiteral("minimumWidth")),
             qPrintable(QString::number(splits.sidebar->width())));
    QVERIFY2(atLeastItsMinimum(splits.right, splits.right->width(), QStringLiteral("minimumWidth")),
             qPrintable(QString::number(splits.right->width())));
    QVERIFY2(splits.right->width() >= splits.queue->property("minimumContentWidth").toReal() - 0.5,
             qPrintable(QStringLiteral("%1 < %2")
                            .arg(splits.right->width())
                            .arg(splits.queue->property("minimumContentWidth").toReal())));
    QVERIFY2(atLeastItsMinimum(splits.queue, splits.queue->height(), QStringLiteral("minimumHeight")),
             qPrintable(QString::number(splits.queue->height())));
    QVERIFY2(atLeastItsMinimum(splits.visualization, splits.visualization->height(),
                               QStringLiteral("minimumHeight")),
             qPrintable(QString::number(splits.visualization->height())));
    QVERIFY(shownInside(window, splits.main));
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
    QQuickItem *queue = splits.queue;
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
    QVERIFY(sceneRectOf(columns.last()).right() <= sceneRectOf(queue).right() + 0.5);

    // Dragged in a larger window, the splits keep their shares as it grows.
    window->resize(1400, 860);
    QTRY_COMPARE(window->size(), QSize(1400, 860));
    QTRY_COMPARE(splits.main->width(), 1400.0);
    QTRY_VERIFY(splits.laidOut());
    QVERIFY(splits.drag(window, 200, -100));
    const qreal sidebarShare = splits.sidebarShare();
    const qreal queueShare = splits.queueShare();
    window->resize(1700, 1000);
    QTRY_COMPARE(window->size(), QSize(1700, 1000));
    QTRY_COMPARE(splits.main->width(), 1700.0);
    QTRY_VERIFY(splits.laidOut());
    QTRY_VERIFY2(splits.sharesProblem(sidebarShare, queueShare).isEmpty(),
                 qPrintable(splits.sharesProblem(sidebarShare, queueShare)));
    QVERIFY(finishIncubation(engine));
}

// The window's place and size, maximized or not, the splits' shares, the
// visualization, Follow Current Track and the queue's column widths
// survive a restart, and so do the settings this build does not know.
void WindowSmokeTest::windowLayoutSurvivesARestart() {
    // On one of the large test screens, which has room for the window and
    // its columns beside the wider sidebar; saved by a build that also had a Genre column, which
    // this one keeps, and a setting it does not know.
    const QRect geometry(100'200, 150, 1500, 820);
    const QRect normalOnTheSmallScreen(40, 40, 880, 600);
    QVERIFY(writeSavedSettings("window_geometry=100200,150,1500,820\n"
                               "queue_column_widths=artist:260,genre:99,album:9\nfuture_key=1\n"));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    qreal sidebarShare = 0;
    qreal queueShare = 0;
    qreal album = 0;
    {
        BridgeClient bridge;
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QCOMPARE(window->geometry(), geometry);
        QQuickItem *artist = queueHeader(window, "artist");
        QQuickItem *albumHeader = queueHeader(window, "album");
        QVERIFY(artist != nullptr && albumHeader != nullptr);
        // As the window is first shown, without a jump from the default.
        QCOMPARE(artist->width(), 260.0);
        // The album's width was out of range: its default.
        const qreal defaultAlbum = albumHeader->width();
        QVERIFY(defaultAlbum > 100);
        QQuickItem *handle = shownItemNamed(window->contentItem(), QStringLiteral("queueColumnHandle_album"));
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

        const MainSplits splits(window);
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *queueController = window->findChild<QObject *>(QStringLiteral("queueController"));
        QVERIFY(splits.found() && viewer != nullptr && queueController != nullptr);
        QTRY_COMPARE(splits.main->width(), 1500.0);
        QTRY_VERIFY(splits.laidOut());
        QVERIFY(splits.drag(window, 120, -60));
        sidebarShare = splits.sidebarShare();
        queueShare = splits.queueShare();
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
        QVERIFY(queueController->setProperty("autoCenterSelection", false));

        QVERIFY(closeThroughTheQuitFlow(window));
    }
    const QByteArray saved = readSavedSettings();
    QVERIFY2(saved.contains("\nwindow_geometry=100200,150,1500,820\n"), saved.constData());
    QVERIFY2(saved.contains("\nvisualization_mode=1\n"), saved.constData());
    QVERIFY2(saved.contains("\nfollow_current_track=0\n"), saved.constData());
    const QByteArray columnWidths = "\nqueue_column_widths=artist:260,album:" + QByteArray::number(int(album))
        + ",genre:99\n";
    QVERIFY2(saved.contains(columnWidths), saved.constData());
    QVERIFY2(saved.contains("\nfuture_key=1\n"), saved.constData());

    {
        BridgeClient bridge;
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QCOMPARE(window->geometry(), geometry);
        const MainSplits splits(window);
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *queueController = window->findChild<QObject *>(QStringLiteral("queueController"));
        QVERIFY(splits.found() && viewer != nullptr && queueController != nullptr);
        QTRY_VERIFY2(splits.sharesProblem(sidebarShare, queueShare).isEmpty(),
                     qPrintable(splits.sharesProblem(sidebarShare, queueShare)));
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
        QCOMPARE(queueHeader(window, "artist")->width(), 260.0);
        QCOMPARE(queueHeader(window, "album")->width(), album);

        // In the narrowest window Album drops and Artist gives way, so the
        // title keeps its minimum, and they come back as the window grows
        // again.
        window->resize(window->minimumWidth(), window->minimumHeight());
        QTRY_COMPARE(window->width(), window->minimumWidth());
        QTRY_VERIFY(queueHeader(window, "artist")->width() < 260.0);
        QVERIFY(queueHeader(window, "title")->width()
                >= splits.queue->property("minimumTitleColumnWidth").toReal() - 1);
        QCOMPARE(queueHeader(window, "album"), nullptr);
        window->setGeometry(geometry);
        QTRY_COMPARE(queueHeader(window, "artist")->width(), 260.0);
        QTRY_COMPARE(queueHeader(window, "album")->width(), album);

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
    const QByteArray savedMaximized = readSavedSettings();
    QVERIFY2(savedMaximized.contains("\nwindow_geometry=40,40,880,600\n"), savedMaximized.constData());
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
    QVERIFY(writeSavedSettings("window_geometry=50000,40000,1200,700\nwindow_maximized=0\n"));
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
    QVERIFY2(onAScreen, qPrintable(describeRect(window->geometry())));
    QVERIFY(closeThroughTheQuitFlow(window));
    QFile::remove(savedSettingsPath());
}

// Unmuting showed the volume slider at 0, and the muted glyph, until a
// snapshot of the backend's brought the restored volume some 50 ms later.
// With the real bridge and backend: from the mute to the backend's snapshot
// after the unmute, the slider shows the volume in every frame, and the
// glyph the state, at once.
void WindowSmokeTest::unmuteShowsTheVolumeAtOnce() {
    QFile::remove(savedSettingsPath());
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    BridgeClient bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *slider = visibleItemNamed(window, QStringLiteral("volumeSlider"));
    QQuickItem *glyph = visibleItemNamed(window, QStringLiteral("muteButtonGlyph"));
    auto *muteAction = window->findChild<QObject *>(QStringLiteral("muteAction"));
    QVERIFY(slider && glyph && muteAction);
    const auto taken = [&bridge]() { return bridge.m_volumeRequestsTaken == bridge.m_volumeRequestsSent; };
    // The slider shows the volume in percent.
    const auto value = [slider]() { return slider->property("value").toDouble() / 100; };
    // The volume the test sets, within the rounding of the backend's float.
    const auto atVolume = [](double shown) { return std::abs(shown - 0.6) < 1e-3; };
    const auto mutedGlyph = [glyph]() {
        return glyph->property("source").toUrl().fileName() == QStringLiteral("volume-muted.svg");
    };

    // A volume of 0.6, which the backend has taken.
    bridge.setVolume(0.6);
    QTRY_VERIFY(taken());
    QTRY_VERIFY(atVolume(value()));
    QVERIFY(!mutedGlyph());

    // What every frame shows, from the mute on.
    struct Shown {
        double value;
        bool muted;
    };
    QList<Shown> frames;
    const QMetaObject::Connection recording = QObject::connect(
        window, &QQuickWindow::afterAnimating, window,
        [&]() { frames.push_back({value(), mutedGlyph()}); });
    const auto describeFrames = [&frames]() {
        QStringList lines;
        for (const Shown &frame : frames) {
            lines.push_back(QStringLiteral("%1%2").arg(frame.value).arg(frame.muted ? QStringLiteral(" muted") : QString()));
        }
        return lines.join(QStringLiteral(", "));
    };

    // Muted, the thumb stays where it was and the glyph is the muted one.
    QVERIFY(QMetaObject::invokeMethod(muteAction, "trigger"));
    QVERIFY2(atVolume(value()), qPrintable(QString::number(value())));
    QVERIFY(mutedGlyph());
    QTRY_VERIFY(taken());
    QVERIFY(bridge.m_volumeRequestsSent > 0);
    QCOMPARE(bridge.volume(), 0.0);
    window->update();
    QTRY_VERIFY(!frames.isEmpty() && frames.constLast().muted);

    // Unmuted, the same at once, and in every frame until the backend's
    // snapshot, and after.
    const qsizetype unmutedFrom = frames.size();
    QVERIFY(QMetaObject::invokeMethod(muteAction, "trigger"));
    QVERIFY2(atVolume(value()), qPrintable(QString::number(value())));
    QVERIFY(!mutedGlyph());
    QTRY_VERIFY(taken());
    window->update();
    QTRY_VERIFY(frames.size() > unmutedFrom + 1);
    QObject::disconnect(recording);
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const Shown &frame = frames.at(i);
        QVERIFY2(atVolume(frame.value), qPrintable(describeFrames()));
        if (i >= unmutedFrom) {
            QVERIFY2(!frame.muted, qPrintable(describeFrames()));
        }
    }
    QVERIFY(atVolume(bridge.volume()));
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

namespace {

// Cuts `bridge` off from its backend, so that only the snapshots a test
// delivers reach it.
void detachFromTheBackend(BridgeClient &bridge) {
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
}

// Hands `bridge` a snapshot, as its poll does.
bool deliverSnapshot(BridgeClient &bridge, const BinaryBridgeCodec::DecodedSnapshot &snapshot) {
    bridge.resetPollChanges();
    const bool applied = bridge.processBinarySnapshot(snapshot);
    bridge.schedulePollChanges();
    return applied;
}

// Whether the now-playing panel has settled, its growth and cross-fade
// ended: this ends those that run, as they would end, for checks of where
// they leave it (tst_ui_motion has their motion).
bool panelSettled(QQuickItem *panel) {
    bool running = false;
    for (const char *name : {"presenceAnimation", "handoverAnimation"}) {
        QObject *animation = qvariant_cast<QObject *>(panel->property(name));
        if (animation != nullptr && animation->property("running").toBool()) {
            QMetaObject::invokeMethod(animation, "complete");
            running = running || animation->property("running").toBool();
        }
    }
    return !running;
}

// Lays `window` out as its next frames would, where a change reaches the
// sidebar's layout a frame after the panel's own.
void layOut(QQuickWindow *window) {
    for (int frame = 0; frame < 2; ++frame) {
        (void)window->grabWindow();
    }
}

// Main.qml over a BridgeClient that the test hands its snapshots, as its
// poll would.
struct NowPlayingWindow {
    NowPlayingWindow(BridgeClient &bridge, QSize size) : bridge(bridge) {
        engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
        MainWindowOptions options;
        options.width = size.width();
        options.height = size.height();
        options.activate = false;
        window = loadMainWindow(engine, libraryModel, options);
        panel = window != nullptr ? named(window, "nowPlayingPanel") : nullptr;
    }
    QQuickItem *item(const char *name) const { return named(window, name); }
    bool deliver(const BinaryBridgeCodec::DecodedSnapshot &snapshot) const {
        return deliverSnapshot(bridge, snapshot);
    }

    BridgeClient &bridge;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    QQuickItem *panel = nullptr;
};

// A snapshot of `path` playing, tagged with `name`'s title, artist and
// album.
BinaryBridgeCodec::DecodedSnapshot playingTagged(const QString &path, const QString &name) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = path;
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = path;
    snapshot.metadata.title = name + QStringLiteral(" Title");
    snapshot.metadata.artist = name + QStringLiteral(" Artist");
    snapshot.metadata.album = name + QStringLiteral(" Album");
    return snapshot;
}

// Track changes whose path comes before the track's queue row and metadata,
// as BridgeClient tells them: the panel keeps the track before's lines,
// with no file name or blanks in between, until the next track's own come,
// tagged or with a tag that lacks its title, artist and album (#560).
void checkLinesKeptUntilTheNextTracksAreKnown(const NowPlayingWindow &ui) {
    QQuickItem *title = ui.item("nowPlayingTitle");
    QQuickItem *artistAlbum = ui.item("nowPlayingArtistAlbum");
    QVERIFY(title != nullptr && artistAlbum != nullptr);
    const auto expectLines = [&](const QString &titleText, const QString &artistAlbumText) {
        QTRY_VERIFY(panelSettled(ui.panel));
        QTRY_COMPARE(title->property("text").toString(), titleText);
        QTRY_COMPARE(artistAlbum->property("text").toString(), artistAlbumText);
        QCOMPARE(ui.panel->property("titleText").toString(), titleText);
        QCOMPARE(ui.panel->property("artistAlbumText").toString(), artistAlbumText);
    };
    // The lines stay as they were while the next track's are not known.
    const auto expectLinesHeld = [&](const QString &titleText, const QString &artistAlbumText) {
        QTest::qWait(50);
        QCOMPARE(ui.panel->property("titleText").toString(), titleText);
        QCOMPARE(ui.panel->property("artistAlbumText").toString(), artistAlbumText);
    };

    BinaryBridgeCodec::DecodedSnapshot snapshot = playingTagged(QStringLiteral("/fixture/a.flac"), QStringLiteral("A"));
    QVERIFY(ui.deliver(snapshot));
    expectLines(QStringLiteral("A Title"), QStringLiteral("A Artist — A Album"));

    // B plays, with the metadata still A's; then B's own comes.
    snapshot.playback.currentPath = QStringLiteral("/fixture/b.flac");
    QVERIFY(ui.deliver(snapshot));
    expectLinesHeld(QStringLiteral("A Title"), QStringLiteral("A Artist — A Album"));
    snapshot = playingTagged(QStringLiteral("/fixture/b.flac"), QStringLiteral("B"));
    QVERIFY(ui.deliver(snapshot));
    expectLines(QStringLiteral("B Title"), QStringLiteral("B Artist — B Album"));

    // A track whose tag has none of the three; then its own metadata.
    const QString untaggedPath = QStringLiteral("/fixture/untitled take.flac");
    snapshot.playback.currentPath = untaggedPath;
    QVERIFY(ui.deliver(snapshot));
    expectLinesHeld(QStringLiteral("B Title"), QStringLiteral("B Artist — B Album"));
    snapshot.metadata = {};
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = untaggedPath;
    QVERIFY(ui.deliver(snapshot));
    // Its file name for a title and, in the artist's row, its folder, not
    // a tag (as the bridge gives the title, never empty).
    QTRY_VERIFY(panelSettled(ui.panel));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("untitled take.flac"));
    QTRY_COMPARE(artistAlbum->property("text").toString(), QStringLiteral("fixture"));
    QCOMPARE(ui.panel->property("artistAlbumText").toString(), QString());
    QCOMPARE(ui.panel->property("folderText").toString(), QStringLiteral("fixture"));
    QCOMPARE(ui.bridge.currentTrackTitle(), QStringLiteral("untitled take.flac"));
    QVERIFY(ui.bridge.currentTrackArtist().isEmpty());
    QVERIFY(ui.bridge.currentTrackAlbum().isEmpty());
}

// A file without tags, as BridgeClient tells it, with its file name for a
// title: its details line says what the file is, its container and its
// length, "FLAC · 0:40", in the details' place and style, under the wide
// cover and in the large-font header, where the album's row, which the
// file has nothing for, is not kept, so the details follow the folder
// 4 px under it; the panel's height and the library's place are a tagged
// track's (at 24 pt the card stood some 100 px empty under "Unsorted").
// The bridge's format label is from the file's extension, and its length
// from the playback.
void checkTaglessFileFacts(const NowPlayingWindow &ui, bool large) {
    QQuickItem *header = ui.item("libraryHeader");
    QQuickItem *details = ui.item(large ? "nowPlayingNarrowDetails" : "nowPlayingDetails");
    QQuickItem *folder = ui.item(large ? "nowPlayingNarrowArtist" : "nowPlayingArtistAlbum");
    QObject *palette = qvariant_cast<QObject *>(ui.window->property("uiPalette"));
    QVERIFY(header != nullptr && details != nullptr && folder != nullptr && palette != nullptr);
    const QString facts = QStringLiteral("FLAC · 0:40");

    // A tagged track first.
    BinaryBridgeCodec::DecodedSnapshot snapshot =
        playingTagged(QStringLiteral("/fixture/Artist/Album/01.flac"), QStringLiteral("Tagged"));
    snapshot.playback.durationSeconds = 200.0;
    snapshot.playback.trackToken = 1;
    snapshot.metadata.genre = QStringLiteral("Jazz");
    snapshot.metadata.year = 2005;
    snapshot.metadata.trackNumber = 1;
    QVERIFY(ui.deliver(snapshot));
    QTRY_VERIFY(panelSettled(ui.panel) && ui.panel->property("presence").toReal() == 1.0);
    QCOMPARE(ui.panel->property("largeHeader").toBool(), large);
    QCOMPARE(ui.panel->property("layoutMode").toString(), large ? QStringLiteral("narrow") : QStringLiteral("wide"));
    layOut(ui.window);
    const qreal panelHeight = ui.panel->height();
    const qreal headerTop = sceneRectOf(header).top();

    // The file without tags, its own metadata after its path.
    const QString path = QStringLiteral("/fixture/Loose Files/track07.flac");
    snapshot.playback.currentPath = path;
    snapshot.playback.durationSeconds = 40.0;
    snapshot.playback.trackToken = 2;
    QVERIFY(ui.deliver(snapshot));
    snapshot.metadata = {};
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = path;
    QVERIFY(ui.deliver(snapshot));
    QTRY_COMPARE(ui.bridge.currentTrackTitle(), QStringLiteral("track07.flac"));
    QCOMPARE(ui.bridge.currentTrackFormatLabel(), QStringLiteral("FLAC"));
    QTRY_VERIFY2(ui.panel->property("untagged").toBool(),
                 qPrintable(QStringLiteral("\"%1\", \"%2\", \"%3\"")
                                .arg(ui.panel->property("titleText").toString(),
                                     ui.panel->property("artistAlbumText").toString(),
                                     ui.panel->property("detailsText").toString())));
    QTRY_COMPARE(ui.panel->property("fileFactsText").toString(), facts);
    QTRY_VERIFY(panelSettled(ui.panel));
    layOut(ui.window);
    QTRY_COMPARE(details->property("text").toString(), facts);
    QCOMPARE(folder->property("text").toString(), QStringLiteral("Loose Files"));
    // In the details' style: their size, and the tertiary colour.
    QCOMPARE(colorOf(details, "color"), colorOf(palette, "textTertiary"));
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(), palette->property("smallPointSize").toReal());
    // The panel and the library where a tagged track has them.
    QTRY_COMPARE(ui.panel->height(), panelHeight);
    QTRY_COMPARE(sceneRectOf(header).top(), headerTop);
    if (large) {
        // No album row: the details 4 px under the folder.
        const QRectF folderRect = folder->mapRectToScene(folder->boundingRect());
        const QRectF detailsRect = details->mapRectToScene(details->boundingRect());
        QVERIFY2(std::abs(detailsRect.top() - (folderRect.bottom() + 4)) <= 1.0,
                 qPrintable(QStringLiteral("the folder to %1, the details from %2")
                                .arg(folderRect.bottom()).arg(detailsRect.top())));
    }

    // Without a length, the container alone.
    snapshot.playback.durationSeconds = 0.0;
    QVERIFY(ui.deliver(snapshot));
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("FLAC"));
}

// Stop and Clear, and the queue running out, as BridgeClient tells them:
// one snapshot of no track, stopped, told as the playback state, then the
// track's identity, then its metadata, each a signal of its own, the panel
// idle from the first. While the panel shrinks into the compact row it
// holds the track that showed, its cover and its lines, whichever of them
// it hears of first; every frame shows that track, it and the row handing
// over, or the row, and the tile is never at rest while it shows (the
// cover and the lines went in one frame, and the tile's own disc mark stood
// alone in the band). Stop alone keeps the track.
void checkTrackHeldAsThePanelShrinks(const NowPlayingWindow &ui, const QString &coverUrl) {
    QQuickItem *title = ui.item("nowPlayingTitle");
    QQuickItem *tile = ui.item("nowPlayingCover");
    QQuickItem *area = ui.item("nowPlayingCoverArea");
    QQuickItem *row = ui.item("nowPlayingIdleRow");
    QQuickItem *lines = ui.item("nowPlayingLines");
    auto *growth = qvariant_cast<QObject *>(ui.panel->property("presenceAnimation"));
    QVERIFY(title && tile && area && row && lines && growth);
    QVERIFY(ui.item("albumArtPlaceholderGlyph") == nullptr);
    // Every frame of the move, on the test's time.
    SteppedAnimationDriver animations;

    for (const bool runsOut : {false, true}) {
        const QString where = runsOut ? QStringLiteral("runs out") : QStringLiteral("stop and clear");
        BinaryBridgeCodec::DecodedSnapshot snapshot = playingTagged(QStringLiteral("/fixture/a.flac"), QStringLiteral("A"));
        snapshot.metadata.coverPath = coverUrl;
        QVERIFY(ui.deliver(snapshot));
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("A Title"));
        QTRY_COMPARE(tile->property("coverPath").toString(), coverUrl);
        QTRY_VERIFY(tile->property("hasCover").toBool());
        (void)animations.step(1000);
        QVERIFY(ui.panel->property("presence").toDouble() == 1.0 && !growth->property("running").toBool());
        QCOMPARE(ui.panel->property("layoutMode").toString(), QStringLiteral("wide"));

        if (!runsOut) {
            // Stop keeps the track, and nothing moves.
            snapshot.playback.state = 0;
            QVERIFY(ui.deliver(snapshot));
            QTRY_COMPARE(ui.bridge.playbackState(), QStringLiteral("Stopped"));
            (void)animations.step(50);
            QVERIFY(!ui.panel->property("idle").toBool());
            QVERIFY(!growth->property("running").toBool());
            QCOMPARE(title->property("text").toString(), QStringLiteral("A Title"));
        }

        QString problem;
        QList<std::pair<double, double>> shown;
        const auto look = [&]() {
            if (!ui.panel->property("idle").toBool()) {
                return;
            }
            const double cover = area->isVisible() && tile->isVisible() ? tile->opacity() : 0.0;
            const double rowShown = row->isVisible() ? row->opacity() : 0.0;
            const double linesShown = lines->isVisible() && title->isVisible() ? lines->opacity() : 0.0;
            shown.push_back({cover, rowShown});
            if (!problem.isEmpty()) {
                return;
            }
            if (cover > 0 && (tile->property("idle").toBool() || !tile->property("hasCover").toBool()
                              || tile->property("coverPath").toString() != coverUrl)) {
                problem = QStringLiteral("the tile at %1 shows \"%2\"").arg(cover)
                    .arg(tile->property("coverPath").toString());
            } else if (linesShown > 0 && title->property("text").toString() != QStringLiteral("A Title")) {
                problem = QStringLiteral("the title \"%1\" at %2").arg(title->property("text").toString())
                    .arg(linesShown);
            } else if (std::max(cover, rowShown) < 0.49) {
                problem = QStringLiteral("the cover %1, the row %2").arg(cover).arg(rowShown);
            }
        };
        // No track: the queue cleared, or played out; then each frame as
        // the panel shrinks, and a few after.
        snapshot.playback.state = 0;
        snapshot.playback.currentPath.clear();
        snapshot.metadata = {};
        QVERIFY(ui.deliver(snapshot));
        QCoreApplication::processEvents();
        look();
        const auto shrunk = [&]() {
            return ui.panel->property("idle").toBool() && !growth->property("running").toBool()
                && ui.panel->property("presence").toDouble() == 0.0;
        };
        for (int ms = 0, after = 0; ms < 5000 && after < 3; ms += 16) {
            after += shrunk() ? 1 : 0;
            (void)animations.step(16);
            look();
        }
        QVERIFY2(problem.isEmpty(), qPrintable(where + QStringLiteral(": ") + problem));
        QVERIFY2(shrunk() && shown.size() >= 3, qPrintable(where + QStringLiteral(": %1 frames").arg(shown.size())));
        // The cover whole at the start, the row whole at the end.
        QCOMPARE(shown.first().first, 1.0);
        QCOMPARE(shown.last().second, 1.0);
        QVERIFY(ui.bridge.currentTrackTitle().isEmpty());
        QVERIFY(ui.bridge.currentTrackCoverPath().isEmpty());
        QVERIFY(!ui.panel->property("holdsTrack").toBool());
        QVERIFY(!area->isVisible());
    }
}

} // namespace

// The now-playing panel as BridgeClient tells it the tracks, from the
// snapshots that the backend would send, in the order it sends them: the
// checks above, each at 1600 x 1000, and at 24 pt in a 1000 x 720 window
// the tagless file's.
void WindowSmokeTest::nowPlayingFollowsBridgeClientsSnapshots() {
    BridgeClient bridge;
    detachFromTheBackend(bridge);
    QTemporaryDir covers;
    QVERIFY(covers.isValid());
    const QString coverFile = covers.filePath(QStringLiteral("cover.png"));
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(QColor(200, 40, 40));
    QVERIFY(image.save(coverFile));
    {
        const NowPlayingWindow ui(bridge, QSize(1600, 1000));
        QVERIFY2(ui.panel != nullptr, "Main.qml failed to show");
        checkTaglessFileFacts(ui, false);
        if (QTest::currentTestFailed()) {
            return;
        }
        checkTrackHeldAsThePanelShrinks(ui, QUrl::fromLocalFile(coverFile).toString());
        if (QTest::currentTestFailed()) {
            return;
        }
        checkLinesKeptUntilTheNextTracksAreKnown(ui);
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    const ScopedApplicationFont font(24);
    const NowPlayingWindow ui(bridge, QSize(1000, 720));
    QVERIFY2(ui.panel != nullptr, "Main.qml failed to show");
    checkTaglessFileFacts(ui, true);
}

// A backend that could not start fails before the window is made: the
// window shows it as it shows one that stopped later.
void WindowSmokeTest::aBackendThatCannotStartShowsWhyInRecentErrorsAndTheStatusBar() {
    const QString reason = QStringLiteral("failed to start the bridge thread: no threads in this test");
    const ScopedFailingBridgeStart failing(reason);
    BridgeClient bridge;
    const QString failure = bridge.backendFailure();
    QVERIFY2(failure.contains(reason), qPrintable(failure));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");

    // In Recent Issues, as an error of the system's.
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    auto *entries = qvariant_cast<QAbstractItemModel *>(errors->property("entries"));
    QVERIFY(entries != nullptr);
    const QStringList messages = errorHistoryMessages(errors);
    const int row = static_cast<int>(messages.indexOf(failure));
    QVERIFY2(row >= 0, qPrintable(messages.join(QStringLiteral("; "))));
    const QHash<int, QByteArray> roles = entries->roleNames();
    QCOMPARE(entries->index(row, 0).data(roles.key("category")).toInt(),
             int(BinaryBridgeCodec::ErrorCategorySystem));
    QCOMPARE(entries->index(row, 0).data(roles.key("severity")).toInt(),
             int(BinaryBridgeCodec::ErrorSeverityError));
    QCOMPARE(entries->index(row, 0).data(roles.key("count")).toInt(), 1);
    QVERIFY(errors->property("unseenCount").toInt() >= 1);

    // The status bar shows the engine stopped, not merely not running, with
    // the reason in its tooltip.
    auto *stopped = window->findChild<QQuickItem *>(QStringLiteral("statusBarBackendStoppedLabel"));
    auto *notRunning = window->findChild<QQuickItem *>(QStringLiteral("statusBarDisconnectedLabel"));
    QVERIFY(stopped != nullptr && notRunning != nullptr);
    QTRY_VERIFY(stopped->isVisible());
    QVERIFY(!notRunning->isVisible());
    QCOMPARE(evaluateOn(stopped, QStringLiteral("ToolTip.text")).toString(), failure);
    QCOMPARE(evaluateOn(stopped, QStringLiteral("Accessible.description")).toString(), failure);
    auto *transportBar = window->findChild<QQuickItem *>(QStringLiteral("transportBar"));
    QVERIFY(transportBar != nullptr);
    QVERIFY(!transportBar->isEnabled());
}

#include "tst_qml_smoke_window.moc"
