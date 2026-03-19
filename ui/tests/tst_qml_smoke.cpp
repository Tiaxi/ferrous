#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QImage>
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

#include "../src/DiagnosticsLog.h"
#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramSeekTrace.h"
#define private public
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"
#undef private

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
    void spectrogramSurfaceDefersPackedDeltaFlush();
    void spectrogramItemRendersNonBackgroundPixels();
    void spectrogramItemRendersRowsAppendedAfterInitialBlankFrame();
    void spectrogramItemSurvivesRepeatedIncrementalTextureReplacement();
    void spectrogramWrappedHistoryStaysContinuousAcrossTileBoundary();
    void spectrogramChunkedResetBurstSeedsHistoryAcrossChunks();
    void spectrogramSeedsOnlyFirstResetBurstIntoHistory();
    void spectrogramSteadyStateAppendKeepsRowsPendingForAnimation();
    void spectrogramQueuedDrainConsumesReadyRows();
    void spectrogramHaltDropsPendingMotion();
    void diagnosticsLogUsesLowercaseAppDir();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
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
        property var itunesArtworkResults: []
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property int spectrogramViewMode: 0
        property int fftSize: 8192
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
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
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property int sampleRateHz: 48000
        signal diagnosticsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal snapshotChanged()
        function setVolume(value) {}
        function setLibrarySortMode(mode) {}
        function rescanAllLibraryRoots() {}
        function openInFileBrowser(path) {}
        function rescanLibraryRoot(path) {}
        function removeLibraryRoot(path) {}
        function setSpectrogramViewMode(mode) {}
        function setFftSize(value) {}
        function setDbRange(value) {}
        function setLogScale(value) {}
        function setShowFps(value) {}
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
        uiPalette: palette
        sections: [{ text: "Ready", emphasis: false, stretch: true }]
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
        queueTrackNumberText: function(index) { return "--" }
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
        queueTrackNumberText: function(index) { return "--" }
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

void QmlSmokeTest::spectrogramItemRendersNonBackgroundPixels() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    constexpr int rowCount = 320;
    constexpr int binsPerRow = 128;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            const int index = row * binsPerRow + bin;
            packedRows[index] = static_cast<char>((row * 5 + bin * 3) % 256);
        }
    }
    item->appendPackedRows(packedRows, rowCount, binsPerRow);

    window.show();
    QTest::qWait(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const QImage frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed");

    const QColor background(0x0b, 0x0b, 0x0f);
    int minX = frame.width();
    int maxX = -1;
    int nonBackgroundPixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            if (frame.pixelColor(x, y) != background) {
                ++nonBackgroundPixels;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    QVERIFY2(nonBackgroundPixels > (frame.width() * frame.height()) / 50,
        "Spectrogram rendered too few non-background pixels");
    QVERIFY2(maxX >= 0 && (maxX - minX) > frame.width() / 3,
        "Spectrogram pixels did not span enough horizontal width");
}

void QmlSmokeTest::spectrogramItemRendersRowsAppendedAfterInitialBlankFrame() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    window.show();
    QTest::qWait(50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    constexpr int rowCount = 320;
    constexpr int binsPerRow = 128;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            const int index = row * binsPerRow + bin;
            packedRows[index] = static_cast<char>((row * 11 + bin * 7) % 256);
        }
    }
    item->appendPackedRows(packedRows, rowCount, binsPerRow);

    QTest::qWait(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const QImage frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed after delayed append");

    const QColor background(0x0b, 0x0b, 0x0f);
    int minX = frame.width();
    int maxX = -1;
    int nonBackgroundPixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            if (frame.pixelColor(x, y) != background) {
                ++nonBackgroundPixels;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    QVERIFY2(nonBackgroundPixels > (frame.width() * frame.height()) / 50,
        "Spectrogram stayed nearly blank after delayed append");
    QVERIFY2(maxX >= 0 && (maxX - minX) > frame.width() / 3,
        "Delayed spectrogram append only rendered a narrow strip");
}

void QmlSmokeTest::spectrogramItemSurvivesRepeatedIncrementalTextureReplacement() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    constexpr int initialRows = 320;
    constexpr int burstRows = 96;
    constexpr int binsPerRow = 128;

    auto makePackedRows = [](int rowCount, int bins, int seed) {
        QByteArray packedRows;
        packedRows.resize(rowCount * bins);
        for (int row = 0; row < rowCount; ++row) {
            for (int bin = 0; bin < bins; ++bin) {
                packedRows[row * bins + bin] = static_cast<char>((seed + row * 17 + bin * 5) % 256);
            }
        }
        return packedRows;
    };

    item->appendPackedRows(makePackedRows(initialRows, binsPerRow, 11), initialRows, binsPerRow);

    window.show();
    QTest::qWait(80);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 80);

    for (int burst = 0; burst < 6; ++burst) {
        item->appendPackedRows(makePackedRows(burstRows, binsPerRow, 40 + burst * 13), burstRows, binsPerRow);
        QTest::qWait(80);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 80);
        const QImage frame = window.grabWindow();
        QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed during incremental replacement");
    }
}

void QmlSmokeTest::spectrogramWrappedHistoryStaysContinuousAcrossTileBoundary() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    constexpr int rowCount = 323;
    constexpr int binsPerRow = 96;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    std::fill(packedRows.begin(), packedRows.end(), static_cast<char>(255));
    item->appendPackedRows(packedRows, rowCount, binsPerRow);

    window.show();
    QTest::qWait(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const QImage frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed for wrapped history continuity");

    const QColor background(0x0b, 0x0b, 0x0f);
    int maxBackgroundRun = 0;
    int currentBackgroundRun = 0;
    for (int x = 0; x < frame.width(); ++x) {
        int nonBackgroundPixels = 0;
        for (int y = 0; y < frame.height(); ++y) {
            if (frame.pixelColor(x, y) != background) {
                ++nonBackgroundPixels;
            }
        }
        if (nonBackgroundPixels < frame.height() / 4) {
            currentBackgroundRun += 1;
            maxBackgroundRun = std::max(maxBackgroundRun, currentBackgroundRun);
        } else {
            currentBackgroundRun = 0;
        }
    }

    QVERIFY2(
        maxBackgroundRun <= 1,
        qPrintable(QStringLiteral("Wrapped spectrogram history had a background gap of %1 columns")
                       .arg(maxBackgroundRun)));
}

void QmlSmokeTest::spectrogramChunkedResetBurstSeedsHistoryAcrossChunks() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int rowsPerChunk = 4;
    constexpr int binsPerRow = 32;
    QByteArray firstChunk;
    firstChunk.resize(rowsPerChunk * binsPerRow);
    QByteArray secondChunk;
    secondChunk.resize(rowsPerChunk * binsPerRow);
    for (int row = 0; row < rowsPerChunk; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            firstChunk[row * binsPerRow + bin] = static_cast<char>((row * 11 + bin * 3) % 256);
            secondChunk[row * binsPerRow + bin] = static_cast<char>((50 + row * 7 + bin * 5) % 256);
        }
    }

    item.appendPackedRows(firstChunk, rowsPerChunk, binsPerRow, true);
    item.appendPackedRows(secondChunk, rowsPerChunk, binsPerRow, true);

    QCOMPARE(item.m_columns.size(), static_cast<size_t>(6));
    QCOMPARE(item.m_pendingColumns.size(), static_cast<size_t>(2));
}

void QmlSmokeTest::spectrogramSurfaceDefersPackedDeltaFlush() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "viewers" as Viewers

Item {
    width: 420
    height: 160

    QtObject {
        id: bridge
        property int spectrogramViewMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
        property int sampleRateHz: 48000
    }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "surface"
        anchors.fill: parent
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *surface = root->findChild<QObject *>(QStringLiteral("surface"));
    QVERIFY(surface != nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QVariantMap channel;
    channel.insert(QStringLiteral("label"), QStringLiteral("L"));
    channel.insert(QStringLiteral("rows"), 10);
    channel.insert(QStringLiteral("bins"), 2);
    channel.insert(QStringLiteral("data"), QByteArray::fromHex("0102030405060708090a0b0c0d0e0f1011121314"));

    const bool invoked = QMetaObject::invokeMethod(
        surface,
        "appendPackedDelta",
        Q_ARG(QVariant, QVariant::fromValue(QVariantList{channel})),
        Q_ARG(QVariant, false));
    QVERIFY(invoked);

    QVERIFY(surface->property("pendingPackedFlushScheduled").toBool());
    QCOMPARE(surface->property("pendingPackedBatches").toList().size(), 1);

    QTRY_VERIFY(surface->property("pendingPackedBatches").toList().isEmpty());
    QVERIFY(!surface->property("pendingPackedFlushScheduled").toBool());
}

void QmlSmokeTest::spectrogramSeedsOnlyFirstResetBurstIntoHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int rowCount = 24;
    constexpr int binsPerRow = 32;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            packedRows[row * binsPerRow + bin] = static_cast<char>((row * 13 + bin * 5) % 256);
        }
    }

    item.appendPackedRows(packedRows, rowCount, binsPerRow);

    QCOMPARE(item.m_columns.size(), static_cast<size_t>(rowCount - 2));
    QCOMPARE(item.m_pendingColumns.size(), static_cast<size_t>(2));
    QCOMPARE(item.m_binsPerColumn, binsPerRow);
    QVERIFY(!item.m_seedHistoryOnNextAppend);
}

void QmlSmokeTest::spectrogramSteadyStateAppendKeepsRowsPendingForAnimation() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int initialRows = 24;
    constexpr int extraRows = 8;
    constexpr int binsPerRow = 32;
    QByteArray initialPackedRows;
    initialPackedRows.resize(initialRows * binsPerRow);
    for (int row = 0; row < initialRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            initialPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 7 + bin * 3) % 256);
        }
    }
    item.appendPackedRows(initialPackedRows, initialRows, binsPerRow);

    const size_t seededColumns = item.m_columns.size();
    const size_t seededPending = item.m_pendingColumns.size();

    QByteArray extraPackedRows;
    extraPackedRows.resize(extraRows * binsPerRow);
    for (int row = 0; row < extraRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            extraPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 17 + bin * 11) % 256);
        }
    }
    item.appendPackedRows(extraPackedRows, extraRows, binsPerRow);

    QCOMPARE(item.m_columns.size(), seededColumns);
    QCOMPARE(item.m_pendingColumns.size(), seededPending + static_cast<size_t>(extraRows));
    QVERIFY(!item.m_seedHistoryOnNextAppend);
}

void QmlSmokeTest::spectrogramQueuedDrainConsumesReadyRows() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int initialRows = 24;
    constexpr int extraRows = 8;
    constexpr int binsPerRow = 32;
    QByteArray initialPackedRows;
    initialPackedRows.resize(initialRows * binsPerRow);
    for (int row = 0; row < initialRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            initialPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 5 + bin * 7) % 256);
        }
    }
    item.appendPackedRows(initialPackedRows, initialRows, binsPerRow);

    QByteArray extraPackedRows;
    extraPackedRows.resize(extraRows * binsPerRow);
    for (int row = 0; row < extraRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            extraPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 19 + bin * 3) % 256);
        }
    }
    item.appendPackedRows(extraPackedRows, extraRows, binsPerRow);

    const size_t columnsBefore = item.m_columns.size();
    const size_t pendingBefore = item.m_pendingColumns.size();
    QVERIFY(pendingBefore >= static_cast<size_t>(extraRows));

    item.m_pendingPhase = 3.0;
    // Use a small but valid dt (1ms) so gapDetected is false and the drain
    // is governed by pendingPhase rather than consuming the entire backlog.
    // At 46.875 rows/s with max boost=3× the phase advances by ~0.14 per ms,
    // keeping floor(3 + advance) == 3 regardless of backlog size.
    QVERIFY(item.advanceAnimationLocked(0.001));

    QCOMPARE(item.m_columns.size(), columnsBefore + 3);
    QCOMPARE(item.m_pendingColumns.size(), pendingBefore - 3);
    QVERIFY(item.m_pendingPhase >= 0.0);
    QVERIFY(item.m_pendingPhase < 1.0);
}

void QmlSmokeTest::spectrogramHaltDropsPendingMotion() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int initialRows = 24;
    constexpr int extraRows = 8;
    constexpr int binsPerRow = 32;
    QByteArray initialPackedRows;
    initialPackedRows.resize(initialRows * binsPerRow);
    for (int row = 0; row < initialRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            initialPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 7 + bin * 3) % 256);
        }
    }
    item.appendPackedRows(initialPackedRows, initialRows, binsPerRow);

    QByteArray extraPackedRows;
    extraPackedRows.resize(extraRows * binsPerRow);
    for (int row = 0; row < extraRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            extraPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 17 + bin * 11) % 256);
        }
    }
    item.appendPackedRows(extraPackedRows, extraRows, binsPerRow);
    QVERIFY(!item.m_pendingColumns.empty());

    item.halt();

    QVERIFY(item.m_pendingColumns.empty());
    QCOMPARE(item.m_pendingPhase, 0.0);
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
        item.m_pendingPhase = 0.25;
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));

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
    QCOMPARE(state.value("maxPendingRows").toInt(), 4);
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
        item.m_pendingPhase = 0.5;
        item.m_lastIncomingRowsAtMs = QDateTime::currentMSecsSinceEpoch();
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));
        item.m_pendingColumns.emplace_back(std::vector<quint8>(64, 1));
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

int main(int argc, char **argv) {
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_smoke.moc"
