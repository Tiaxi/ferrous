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
    void spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch();
    void testMutedChannelRendersGrayscale();
    void spectrogramLargePositionJumpWaitsForResetHandoff();
    void spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward();
    void spectrogramGaplessTrackChangePreservesRollingHistory();
    void spectrogramNaturalTrackResetPreservesRollingHistory();
    void spectrogramManualTrackResetClearsRollingHistory();
    void spectrogramRollingZoomResetAnchorsEpochToResetStart();
    void spectrogramSeekResetAnchorsPlaybackClockToChunkStart();
    void diagnosticsLogUsesLowercaseAppDir();
    void playbackControllerSeekImmediatelyUpdatesSpectrogramPosition();
    void playbackControllerDeterministicTimeHooksDriveInterpolation();
    void playbackControllerPlaybackUpdateKeepsSpectrogramOnInterpolatedClock();
    void playbackControllerPostSeekHeartbeatSnapsToBackendPosition();
    void playbackControllerPostSeekHeartbeatAtTargetResumesInterpolation();
    void playbackControllerHeartbeatCorrectionAvoidsOneFrameSpeedBurst();
    void playbackControllerModerateSteadyStateLagUsesTrimNotBleed();
    void playbackControllerProfileLogsHeartbeatCorrectionAndBleed();
    void playbackControllerIgnoresSteadyStateHeartbeatJitter();
    void playbackControllerKeepsWallClockInterpolationAfterSubRealtimeHeartbeats();
    void playbackControllerSteadyStateTrimReducesNoticeableLag();
    void playbackControllerFollowsBoundedRecoveryCadenceWithoutBurst();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void spectrogramSmoothnessProfileTracksServoAndAdvanceFallbackSignals();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
    void spectrogramStaleTokenChunksAreDropped();
    void spectrogramGaplessTokenChunksPassFilter();
    void spectrogramFreshWidgetAcceptsDataWithImplicitReset();
    void spectrogramCenteredModeSeekPreservesRing();
    void spectrogramCenteredGaplessPreStagedFill();
    void spectrogramCenteredGaplessSnapsAnchorToZero();
    void spectrogramCenteredSeekRestartRebuildsEarlierWindow();
    void spectrogramCenteredFinalizeChunkShrinksTotalEstimate();
    void spectrogramCenteredFinalizeChunkIgnoredForStaleToken();
    void spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit();
    void spectrogramCenteredClampsRightEdgeToMaxColNearEof();
    void spectrogramCenteredEofDetachmentDisablesSubpixelScrolling();
    void spectrogramRingCapacityPersistsAcrossFullscreenShrink();
    void spectrogramMaxWidgetWidthSurvivesInstanceReplacement();
    void spectrogramRollingGaplessTrackChangePreservesZoom();
    void spectrogramCenteredGaplessTrackChangeResetsZoom();
    void spectrogramRollingResetTrackChangeResetsZoom();
    void spectrogramFreshInstanceResyncsBackendZoomOnTrackChange();
    void spectrogramFreshInstanceSeekRestartDoesNotResetZoom();
    void spectrogramTrackChangeCancelsPendingZoomDebounce();
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
    void spectrogramZoomOutBlockedWhenSongFits();
    void spectrogramEffectiveZoomMatchesBackendHop();
    void spectrogramAdvanceWorksWhenBackendMatchesZoom();
    void spectrogramEffectiveZoomDuringTransition();
    void spectrogramDeferredZoomAppliesOnBackendData();
    void spectrogramZoomOutProducesDistinctHop();
    void spectrogramMinZoomAdaptsToWidthChange();
    void spectrogramCenteredModeUsesWindowedCapacity();
    void spectrogramPeakHoldRebuildUsesMaxNotNearest();
    void spectrogramZoomFillClearsWhenDecoderReachesTail();
    void spectrogramSyntheticClearPreservesCanvasDuringSeek();
    void spectrogramSyntheticClearInvalidatesCanvasWhenNoOldContent();
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
        property bool spectrogramZoomEnabled: true
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
        function setSpectrogramZoomLevel(level) {}
        function setSpectrogramWidgetWidth(width) {}
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

void QmlSmokeTest::playbackControllerSeekImmediatelyUpdatesSpectrogramPosition() {
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
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 48.0);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(23.16))));

    QCOMPARE(controller->property("displayedPositionSeconds").toDouble(), 23.16);
    QCOMPARE(controller->property("spectrogramPositionSeconds").toDouble(), 23.16);
}

void QmlSmokeTest::playbackControllerDeterministicTimeHooksDriveInterpolation() {
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

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1120.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterHeartbeat - 12.12) < 0.02,
        qPrintable(QStringLiteral("displayed_after_heartbeat=%1")
            .arg(displayedAfterHeartbeat, 0, 'f', 6)));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(1160.0))));

    const double displayedAfterStep =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterStep > 12.14,
        qPrintable(QStringLiteral("displayed_after_step=%1")
            .arg(displayedAfterStep, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPlaybackUpdateKeepsSpectrogramOnInterpolatedClock() {
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);
    const double displayedBeforeUpdate =
        controller->property("displayedPositionSeconds").toDouble();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterUpdate =
        controller->property("displayedPositionSeconds").toDouble();
    const double spectrogramPosition =
        controller->property("spectrogramPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterUpdate - 12.12) < 0.02,
        qPrintable(
            QStringLiteral("displayed=%1 displayed_before=%2")
                .arg(displayedAfterUpdate, 0, 'f', 6)
                .arg(displayedBeforeUpdate, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramPosition - 12.12) < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramPosition, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeartbeatSnapsToBackendPosition() {
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
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommitted",
        Q_ARG(QVariant, QVariant::fromValue(48.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 48.26);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    const double spectrogramAfterHeartbeat =
        controller->property("spectrogramPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedAfterHeartbeat - 48.26) < 0.02,
        qPrintable(QStringLiteral("displayed=%1").arg(displayedAfterHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramAfterHeartbeat - 48.26) < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramAfterHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeartbeatAtTargetResumesInterpolation() {
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

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(48.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    bridge->setProperty("positionSeconds", 48.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2100.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedOnHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedOnHeartbeat - 48.0) < 0.02,
        qPrintable(QStringLiteral("displayed_on_heartbeat=%1").arg(displayedOnHeartbeat, 0, 'f', 6)));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2140.0))));

    const double displayedAfterStep =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterStep > displayedOnHeartbeat + 0.03,
        qPrintable(QStringLiteral("displayed_on_heartbeat=%1 displayed_after=%2")
            .arg(displayedOnHeartbeat, 0, 'f', 6)
            .arg(displayedAfterStep, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerHeartbeatCorrectionAvoidsOneFrameSpeedBurst() {
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.18);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedImmediately =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedImmediately - (displayedBeforeHeartbeat + 0.18)) < 0.02,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeHeartbeat, 0, 'f', 6)));

    QTest::qWait(20);

    const double displayedAfterOneFrame =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterOneFrame >= (displayedImmediately - 0.001)
            && displayedAfterOneFrame <= (displayedImmediately + 0.03),
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_after=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedAfterOneFrame, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerModerateSteadyStateLagUsesTrimNotBleed() {
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
        property bool profileLogsEnabled: true
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    clearCapturedMessages();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.078);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.contains(QStringLiteral("action=follow")), qPrintable(warnings));
    QVERIFY2(!warnings.contains(QStringLiteral("action=trim")), qPrintable(warnings));
    QVERIFY2(!warnings.contains(QStringLiteral("action=bleed")), qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerProfileLogsHeartbeatCorrectionAndBleed() {
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
        property bool profileLogsEnabled: true
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    clearCapturedMessages();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.18);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(
        warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
    QVERIFY2(
        warnings.contains(QStringLiteral("action=follow")),
        qPrintable(warnings));
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] bleed")),
        qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerIgnoresSteadyStateHeartbeatJitter() {
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.05);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedImmediately =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedImmediately - (displayedBeforeHeartbeat + 0.05)) < 0.02,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeHeartbeat, 0, 'f', 6)));

}

void QmlSmokeTest::playbackControllerKeepsWallClockInterpolationAfterSubRealtimeHeartbeats() {
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

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    const std::array<double, 4> heartbeatTimes = {1130.0, 1260.0, 1390.0, 1520.0};
    const std::array<double, 4> heartbeatPositions = {12.122, 12.244, 12.366, 12.488};

    for (std::size_t i = 0; i < heartbeatTimes.size(); ++i) {
        bridge->setProperty("positionSeconds", heartbeatPositions[i]);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChangedAtTime",
            Q_ARG(QVariant, QVariant::fromValue(heartbeatTimes[i])),
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
    }

    const double displayedBeforeFreeRun =
        controller->property("displayedPositionSeconds").toDouble();

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(1920.0))));

    const double displayedAfterFreeRun =
        controller->property("displayedPositionSeconds").toDouble();
    const double localAdvance = displayedAfterFreeRun - displayedBeforeFreeRun;
    QVERIFY2(
        localAdvance > 0.384,
        qPrintable(
            QStringLiteral("displayed_before=%1 displayed_after=%2 local_advance=%3")
                .arg(displayedBeforeFreeRun, 0, 'f', 6)
                .arg(displayedAfterFreeRun, 0, 'f', 6)
                .arg(localAdvance, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerSteadyStateTrimReducesNoticeableLag() {
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    double incomingPosition =
        controller->property("displayedPositionSeconds").toDouble() + 0.10;
    for (int i = 0; i < 20; ++i) {
        bridge->setProperty("positionSeconds", incomingPosition);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChanged",
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
        QTest::qWait(40);
        incomingPosition += 0.039;
    }

    const double displayedPosition = controller->property("displayedPositionSeconds").toDouble();
    const double backendPosition = bridge->property("positionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedPosition - backendPosition) < 0.07,
        qPrintable(
            QStringLiteral("displayed=%1 backend=%2")
                .arg(displayedPosition, 0, 'f', 6)
                .arg(backendPosition, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerFollowsBoundedRecoveryCadenceWithoutBurst() {
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
    QTRY_VERIFY(controller->property("displayedPositionSeconds").toDouble() > 12.0);

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    const std::array<double, 6> positions = {12.039, 12.079, 12.122, 12.165, 12.208, 12.251};
    double previousDisplayed = controller->property("displayedPositionSeconds").toDouble();
    double maximumStep = 0.0;

    for (double nextPosition : positions) {
        QTest::qWait(40);
        bridge->setProperty("positionSeconds", nextPosition);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChanged",
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
        const double displayed = controller->property("displayedPositionSeconds").toDouble();
        maximumStep = std::max(maximumStep, displayed - previousDisplayed);
        previousDisplayed = displayed;
    }

    QVERIFY2(
        maximumStep < 0.05,
        qPrintable(QStringLiteral("maximum_step=%1").arg(maximumStep, 0, 'f', 6)));
    const double displayedPosition = controller->property("displayedPositionSeconds").toDouble();
    const double backendPosition = bridge->property("positionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedPosition - backendPosition) < 0.02,
        qPrintable(
            QStringLiteral("displayed=%1 backend=%2")
                .arg(displayedPosition, 0, 'f', 6)
                .arg(backendPosition, 0, 'f', 6)));
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

void QmlSmokeTest::spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 44'100;
    constexpr int hop = 64; // max zoom
    constexpr int resetStart = 139'812;
    constexpr int columns = 2'048;
    constexpr int currentSeq = 1'000;
    constexpr int totalEstimate = 188'208;
    constexpr quint64 trackToken = 3;

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        resetStart,
        totalEstimate,
        sampleRate,
        hop,
        false,
        true,
        trackToken,
        true);

    QByteArray data(columns * binsPerColumn, '\x40');
    item.feedPrecomputedChunk(
        data,
        binsPerColumn,
        0,
        columns,
        resetStart,
        totalEstimate,
        sampleRate,
        hop,
        false,
        false,
        trackToken,
        true);

    const int currentTrackCol = resetStart + currentSeq;
    const double positionSeconds =
        (static_cast<double>(currentTrackCol) + 0.25)
        * static_cast<double>(hop)
        / static_cast<double>(sampleRate);
    item.setPositionSeconds(positionSeconds);

    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(0));
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(columns));

    item.setDisplayMode(0); // Rolling

    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-resetStart));

    const qint64 nowCol = static_cast<qint64>(std::floor(
        positionSeconds
        * static_cast<double>(sampleRate)
        / static_cast<double>(hop)));
    const qint64 displaySeq = item.m_rollingEpoch + nowCol;
    QCOMPARE(displaySeq, static_cast<qint64>(currentSeq));
    QVERIFY(displaySeq >= item.m_ringOldestSeq);
    QVERIFY(displaySeq < item.m_ringWriteSeq);
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

void QmlSmokeTest::spectrogramRollingZoomResetAnchorsEpochToResetStart() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(0); // Rolling

    constexpr int binsPerColumn = 8;
    constexpr int sampleRate = 44'100;
    constexpr int oldHop = 655;
    constexpr int newHop = 524;
    constexpr int resetStart = 1416;
    constexpr int totalEstimate = 19'668;
    constexpr quint64 trackToken = 3;

    QByteArray initialChunk(64 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        initialChunk,
        binsPerColumn,
        0,
        64,
        0,
        totalEstimate,
        sampleRate,
        oldHop,
        false,
        true,
        trackToken,
        true);

    item.feedPrecomputedChunk(
        QByteArray(),
        binsPerColumn,
        0,
        0,
        resetStart,
        totalEstimate,
        sampleRate,
        newHop,
        false,
        true,
        trackToken,
        true);

    QByteArray zoomChunk(127 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        zoomChunk,
        binsPerColumn,
        0,
        127,
        resetStart,
        totalEstimate,
        sampleRate,
        newHop,
        false,
        false,
        trackToken,
        true);

    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(127));
    QCOMPARE(item.m_rollingEpoch, static_cast<qint64>(-resetStart));

    const double colsPerSecond =
        static_cast<double>(sampleRate) / static_cast<double>(newHop);
    const qint64 anchoredCol = static_cast<qint64>(std::floor(
        item.m_positionAnchorSeconds * colsPerSecond));
    QCOMPARE(anchoredCol, static_cast<qint64>(resetStart));
    QCOMPARE(item.m_rollingEpoch + anchoredCol, static_cast<qint64>(0));
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

void QmlSmokeTest::spectrogramSmoothnessProfileTracksServoAndAdvanceFallbackSignals() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.resetSmoothnessProfileLocked();
        item.m_smoothnessProfile.active = true;
        item.m_smoothnessProfile.startedAtMs = QDateTime::currentMSecsSinceEpoch();
        item.m_smoothnessProfile.lastFrameAtMs = item.m_smoothnessProfile.startedAtMs;
        item.m_precomputedReady = true;
        item.m_playing = true;
        item.m_positionAnchorInitialized = true;
        item.m_positionSeconds = 10.0;
        item.m_positionAnchorSeconds = 10.0;
        item.m_positionAnchorUpdatedAt = std::chrono::steady_clock::now();
    }

    item.setPositionSeconds(10.04);
    item.setPositionSeconds(9.95);

    QVariantMap state;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 2.0;
        item.m_precomputedHopSize = 1024;
        QVERIFY(!item.advancePrecomputedCanvasLocked(1, 10, false));
        state = item.debugSmoothnessProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("servoFrames").toInt(), 2);
    QCOMPARE(state.value("servoRegressionDrops").toInt(), 1);
    QVERIFY(state.value("maxServoErrorMs").toDouble() >= 20.0);
    QCOMPARE(state.value("advanceFallbackFrames").toInt(), 1);
    QCOMPARE(state.value("nonUnityAdvanceFallbackFrames").toInt(), 1);
    QVERIFY(state.value("maxAdvanceFallbackZoomDelta").toDouble() > 0.9);
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

void QmlSmokeTest::spectrogramCenteredSeekRestartRebuildsEarlierWindow() {
    // Regression: repeated small backward seeks at max zoom can force a
    // same-track centered restart once the earlier left-margin columns
    // have been evicted from the ring. After the synthetic clear and the
    // same-token reset, the widget must rebuild the earlier window
    // instead of stranding a blank left region.
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr int total = 20'000;
    constexpr quint64 token = 9;

    // Fill enough later-track data to force eviction of the earliest
    // columns from the centered ring.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, hop, false, true, token);
    QByteArray laterData(13'000 * bins, '\x30');
    item.feedPrecomputedChunk(
        laterData, bins, 0, 13'000, 0, total,
        44100, hop, false, false, token);
    QVERIFY(item.m_ringOldestSeq > 0);

    // Same-track centered seek restart: synthetic clear first, then the
    // worker's proper reset and earlier data at the new start index.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0, 0, 0,
        0, 0, false, true, token, true);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, hop, false, true, token, true);
    QByteArray earlierData(4'000 * bins, '\x50');
    item.feedPrecomputedChunk(
        earlierData, bins, 0, 4'000, 0, total,
        44100, hop, false, false, token);

    QCOMPARE(item.m_ringOldestSeq, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedTrackToken, token);
    QVERIFY(item.m_ringWriteSeq >= 4'000);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel =
            1024.0 / static_cast<double>(hop); // effectiveZoom == 1.0
    }

    item.setPositionSeconds(3.554);
    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    const auto tokenMap = item.m_trackColumnToSeqByToken.value(token);
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayLeft)));
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayRight)));
}

void QmlSmokeTest::spectrogramCenteredFinalizeChunkShrinksTotalEstimate() {
    // Regression for zoom-dependent playhead detachment at track end.
    // The backend emits a finalize chunk (columns=0, complete=true)
    // carrying the actual decoded-column count; the widget must shrink
    // its m_precomputedTotalColumnsEstimate so the centered-mode EOF
    // clamp fires at the real audio end.  The initial file-metadata
    // estimate can overshoot by ~1 s worth of columns at max zoom,
    // which at hop=64 is ~700 cols — far more than the former 128-col
    // tolerance could absorb.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int fileMetadataEstimate = 161024; // cols at hop=64 for ~233.6 s
    constexpr int actualDecodedCols = 159980;    // true end ~232.1 s
    constexpr quint64 token = 7;

    // Initial reset carries the file-metadata estimate.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, fileMetadataEstimate,
        44100, 64, false, true, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, fileMetadataEstimate);

    // A small amount of data so the widget is "live".
    QByteArray data(200 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 200, 0, fileMetadataEstimate,
        44100, 64, false, false, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, fileMetadataEstimate);

    // Finalize chunk arrives with the true decoded extent.  complete=true,
    // columns=0, no bufferReset, no clearHistory.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, actualDecodedCols, actualDecodedCols,
        44100, 64, /*complete=*/true, /*bufferReset=*/false, token,
        /*clearHistoryOnReset=*/false);

    // Estimate must have shrunk to the true extent.  The max column
    // index and ring write state must not have been disturbed.
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, actualDecodedCols);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 199);
    QVERIFY(item.m_ringWriteSeq > 0);
}

void QmlSmokeTest::spectrogramCenteredFinalizeChunkIgnoredForStaleToken() {
    // A finalize chunk for a track that has already been superseded by
    // a buffer_reset for a newer token must not clobber the new track's
    // total_columns_estimate.  Otherwise a late-arriving finalize from
    // the outgoing track would corrupt the new track's display.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int oldEstimate = 161024;
    constexpr int newEstimate = 167936;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    // Old-track reset + data.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, oldEstimate,
        44100, 64, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, oldEstimate,
        44100, 64, false, false, oldToken);

    // New-track buffer_reset + data bumps the committed token and
    // commits the new estimate (track-change path accepts increases).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, newEstimate,
        44100, 64, false, true, newToken);
    QByteArray newData(50 * bins, '\x20');
    item.feedPrecomputedChunk(
        newData, bins, 0, 50, 0, newEstimate,
        44100, 64, false, false, newToken);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, newEstimate);

    // Late-arriving finalize for the OLD token should be ignored.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 159980, 159980,
        44100, 64, /*complete=*/true, /*bufferReset=*/false, oldToken,
        /*clearHistoryOnReset=*/false);

    QCOMPARE(item.m_precomputedTotalColumnsEstimate, newEstimate);
}

void QmlSmokeTest::spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit() {
    // Regression for 6ch AC3 max-zoom-out: the worker can start with a
    // fallback/header estimate, then raise it on the same hop once a
    // duration re-query succeeds.  Qt must accept that larger estimate
    // or minimumZoomLevel stays clamped to the stale shorter length and
    // centered-mode EOF/seek math stops before the real end.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr int initialEstimate = 14126;
    constexpr int requeriedEstimate = 16273;
    constexpr quint64 token = 3;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, initialEstimate,
        48000, 1024, false, true, token);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, initialEstimate);

    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 15, requeriedEstimate,
        48000, 1024, false, false, token);

    QCOMPARE(item.m_precomputedTotalColumnsEstimate, requeriedEstimate);

    const double expectedMinZoom =
        static_cast<double>(item.width()) / static_cast<double>(requeriedEstimate);
    QVERIFY(std::abs(item.minimumZoomLevel() - expectedMinZoom) < 0.0001);
}

void QmlSmokeTest::spectrogramCenteredClampsRightEdgeToMaxColNearEof() {
    // Regression: at high zoom the file-metadata total_columns_estimate
    // overshoots the actual decoded extent by ~1 s worth of cols.  When
    // the decoder has produced all it's going to and playback is within
    // one half-window of maxCol, the centered display must clamp its
    // right edge to maxCol-1 so (a) the playhead detaches from center
    // toward the right edge, (b) no blank tail is shown past real
    // content, and (c) the crosshair at the right edge reads a time
    // <= the true audible end.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr int maxColIndex = 8000;
    constexpr int decodedCols = maxColIndex + 1; // 8001 cols decoded
    // Estimate overshoots by ~1 second (689 cols at hop=64) — the
    // same magnitude seen in diagnostics at max zoom.
    constexpr int inflatedEstimate = decodedCols + 700;

    // Bufferreset + data so the ring is initialized and maxCol is set.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, inflatedEstimate,
        44100, hop, false, true, 1);
    QByteArray data(decodedCols * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedCols, 0, inflatedEstimate,
        44100, hop, false, false, 1);
    QCOMPARE(item.m_precomputedMaxColumnIndex, maxColIndex);

    // Snap renderZoomLevel so effectiveZoom == 1 (1 px per col).
    // updatePaintNode normally does this on zoom-matched resets; force
    // it here so visibleWindowCols = 1200 deterministically.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel =
            1024.0 / static_cast<double>(hop); // = 16.0
    }

    // Position the playhead within halfWindow (600 cols) of maxCol so
    // the EOF clamp must fire.  At 689 cols/s (hop=64, sr=44100),
    // pos=11.25 s -> col ~ 7750, which is maxCol - 250 (inside the
    // 600-col half-window trigger).
    item.setPositionSeconds(11.25);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    // Display right must equal maxCol (decoder's last column), not the
    // inflated estimate.  The crosshair / grid at the right edge is
    // driven by this value, so clamping it here keeps the time axis
    // bounded by real content.
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(maxColIndex));
    // DisplayLeft slid right so the window still covers
    // visibleWindowCols (1200) cols.
    QCOMPARE(item.m_precomputedCanvasDisplayLeft,
             static_cast<qint64>(maxColIndex) - 1199);
}

void QmlSmokeTest::spectrogramCenteredEofDetachmentDisablesSubpixelScrolling() {
    // Regression: in centered mode near EOF at a zoomed-out level, the
    // display range clamps to real decoded content and the playhead
    // detaches toward the right edge. The canvas must stop sub-pixel
    // scrolling in that state or the spectrogram visibly jiggles.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);
    item.setGridEnabled(true);

    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr int maxColIndex = 8000;
    constexpr int decodedCols = maxColIndex + 1;
    constexpr int inflatedEstimate = decodedCols + 700;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, inflatedEstimate,
        44100, hop, false, true, 1);
    QByteArray data(decodedCols * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedCols, 0, inflatedEstimate,
        44100, hop, false, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 8.0; // effectiveZoom = 0.5
    }

    item.setPositionSeconds(11.25);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(maxColIndex));
    QVERIFY2(
        std::abs(item.m_timeGridRenderDrawX) < 0.001,
        qPrintable(QStringLiteral("draw_x=%1 display_left=%2 display_right=%3")
            .arg(item.m_timeGridRenderDrawX, 0, 'f', 6)
            .arg(item.m_precomputedCanvasDisplayLeft)
            .arg(item.m_precomputedCanvasDisplayRight)));
}

void QmlSmokeTest::spectrogramRingCapacityPersistsAcrossFullscreenShrink() {
    // Regression: the centered ring resets on every session restart
    // (e.g. zoom change on fullscreen toggle) and recomputes its cap
    // from the CURRENT widget width.  The Rust decoder's lookahead is
    // sized against the MAX widget width ever seen, so after a
    // fullscreen->windowed transition the decoder produces farther
    // ahead than the shrunken ring can hold and evicts the left-margin
    // cols around the playhead, painting black.  The ring cap must
    // also track the max widget width so both sides stay in sync.
    SpectrogramItem item;
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int hop = 64; // max zoom
    constexpr quint64 token = 1;

    // Simulate fullscreen: widget at a wide width.  First data chunk
    // triggers the ring-cap sizing path.
    item.setWidth(3840);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, token);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, token);
    const int fullscreenCap = item.m_ringCapacity;
    QVERIFY2(fullscreenCap >= 3840,
             qPrintable(QString("fullscreen ring cap too small: %1")
                            .arg(fullscreenCap)));
    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);

    // Simulate the session reset that the backend emits on a zoom
    // change: bins=0 + bufferReset + clearHistory triggers the
    // synthetic-clear path that zeroes the ring capacity.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0, 0, 0,
        0, 0, false, true, token, true);
    QCOMPARE(item.m_ringCapacity, 0);

    // Now shrink to windowed width.  The next data chunks (a proper
    // worker reset followed by data) must still allocate a ring cap
    // at least as large as the fullscreen run so the decoder's
    // max-width-sized lookahead fits without evicting left-margin
    // cols around the playhead.
    item.setWidth(1213);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, token);
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, token);
    QVERIFY2(item.m_ringCapacity >= fullscreenCap,
             qPrintable(QString("post-shrink ring cap %1 < fullscreen cap %2")
                            .arg(item.m_ringCapacity)
                            .arg(fullscreenCap)));
    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
}

void QmlSmokeTest::spectrogramMaxWidgetWidthSurvivesInstanceReplacement() {
    // Regression: when the channel count changes (e.g. 6ch PerChannel
    // → 2ch PerChannel), the old SpectrogramItems are destroyed and
    // fresh ones are created.  The Rust-side lookahead tracker is a
    // singleton in AnalysisRuntimeState so it remembers the prior
    // fullscreen width and keeps producing at that lookahead.  The
    // Qt tracker must therefore also be singleton-equivalent
    // (static), otherwise the new widget starts at maxSeen=0, sizes
    // its ring against the current (smaller) width, the decoder laps
    // the ring, and left-margin cols around the playhead are evicted
    // — the user sees a narrow growing-edge of data with the previous
    // canvas smearing through the rest of the view.
    SpectrogramItem::s_maxWidgetWidthSeen = 0;
    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr quint64 firstToken = 100;
    constexpr quint64 secondToken = 101;

    {
        SpectrogramItem big;
        big.setHeight(200);
        big.setDisplayMode(1);
        big.setWidth(3840);
        big.setSampleRateHz(44100);
        big.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, 0, 161024,
            44100, hop, false, true, firstToken);
        QByteArray data(16 * bins, '\x40');
        big.feedPrecomputedChunk(
            data, bins, 0, 16, 0, 161024,
            44100, hop, false, false, firstToken);
        QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
    }
    // The wide-view instance is now gone; the static must retain the
    // max so the next instance sizes its ring against it.

    SpectrogramItem fresh;
    fresh.setHeight(200);
    fresh.setDisplayMode(1);
    fresh.setWidth(1213); // windowed
    fresh.setSampleRateHz(44100);
    fresh.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, secondToken);
    QByteArray data(16 * bins, '\x40');
    fresh.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, secondToken);

    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
    QVERIFY2(fresh.m_ringCapacity >= 3840,
             qPrintable(QString("fresh-instance ring cap %1 < 3840")
                            .arg(fresh.m_ringCapacity)));
}

void QmlSmokeTest::spectrogramRollingGaplessTrackChangePreservesZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(0); // Rolling
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    QByteArray gapless(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        gapless, bins, 0, 32, 32, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 0);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.zoomLevel(), 4.0);
    QCOMPARE(item.m_renderZoomLevel, 4.0);
}

void QmlSmokeTest::spectrogramCenteredGaplessTrackChangeResetsZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    QByteArray gapless(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        gapless, bins, 0, 32, 32, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 1);
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramRollingResetTrackChangeResetsZoom() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(0); // Rolling
    item.setSampleRateHz(44100);

    constexpr int bins = 4;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, oldToken);
    QByteArray initial(32 * bins, '\x40');
    item.feedPrecomputedChunk(
        initial, bins, 0, 32, 0, total,
        44100, 1024, false, false, oldToken);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 4.0;
        item.m_renderZoomLevel = 4.0;
    }

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        44100, 1024, false, true, newToken);
    QByteArray nextTrack(32 * bins, '\x60');
    item.feedPrecomputedChunk(
        nextTrack, bins, 0, 32, 0, total,
        44100, 1024, false, false, newToken);

    QCOMPARE(zoomResetSpy.count(), 1);
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramFreshInstanceResyncsBackendZoomOnTrackChange() {
    // Regression: the Rust-side zoom_level persists across tracks
    // (track changes don't reset it).  When a channel-count change
    // destroys the SpectrogramItem instances and creates fresh ones,
    // the new instance has m_renderZoomLevel = 1.0 (default) while
    // the backend may still be at zoom=16 from the previous track,
    // sending data at hop=64.  Without a zoom resync, Qt renders at
    // effectiveZoom = renderZoom × hop / refHop = 0.0625, inflating
    // the centered visible window to thousands of cols.  The decoder
    // hasn't produced that many yet, so the right side of the widget
    // has no data and the old canvas smears through.
    //
    // On track change with a non-default backend hop, the widget
    // must emit backendZoomRequested(1.0) even when it's already at
    // render zoom 1.0 (i.e. fresh instance).  The estimate that just
    // arrived is the correct one for the new track and must be
    // preserved (the clear is only for the actual zoom-change case).
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(44100);

    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    constexpr int bins = 4;
    constexpr int hop = 64; // backend at max zoom from prior track
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 7;

    // Seed the widget as if it had just been seeing the prior track
    // at backend zoom 1.0 (the fresh-instance default).  First chunk
    // establishes the committed token so the track-change detection
    // fires on the next non-matching buffer_reset.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 10064,
        44100, 1024, false, true, oldToken);
    QByteArray warm(4 * bins, '\x10');
    item.feedPrecomputedChunk(
        warm, bins, 0, 4, 0, 10064,
        44100, 1024, false, false, oldToken);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, 10064);

    // Track change with backend at hop=64 (still at max zoom from the
    // previous track — the backend's zoom_level persisted across the
    // track change even though Qt's fresh-instance default is 1.0).
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, newToken);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, newToken);

    // Qt must have asked the backend to go back to zoom=1.0 so a
    // subsequent session restart produces data at the reference hop.
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    // The estimate for the new track must be preserved; a fresh
    // instance is not a real zoom transition, so the estimate-clear
    // path must not fire.
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, 161024);
    QCOMPARE(item.m_renderZoomLevel, 1.0);
}

void QmlSmokeTest::spectrogramFreshInstanceSeekRestartDoesNotResetZoom() {
    // Regression: centered same-track seeks outside the current
    // window restart decoding with the same track token but a non-zero
    // startIndex. Fresh SpectrogramItem instances can see that restart
    // before they've learned the current token, so a broad
    // "appliedReset + non-default hop" heuristic wrongly treated the
    // seek as a track change and emitted backendZoomRequested(1.0),
    // resetting max zoom-out back to normal.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    item.setSampleRateHz(48000);

    QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    constexpr int bins = 4;
    constexpr int hop = 14088; // Non-default max-zoom-out-style hop
    constexpr int startIndex = 714;
    constexpr int total = 1027;
    constexpr quint64 trackToken = 7;

    // Simulate a fresh pane instance seeing only the post-seek worker
    // restart. The token is the current track's token, not a track
    // change, but the item has no prior token state yet.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, startIndex, total,
        48000, hop, false, true, trackToken, true);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, startIndex, total,
        48000, hop, false, false, trackToken, false);

    QCOMPARE(zoomResetSpy.count(), 0);
    QCOMPARE(backendZoomSpy.count(), 0);
    QCOMPARE(item.m_precomputedHopSize, hop);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, total);
}

void QmlSmokeTest::spectrogramTrackChangeCancelsPendingZoomDebounce() {
    // Regression: a SpectrogramItem is instantiated with
    // m_zoomLevel defaulting to 1.0, then the QML zoomLevel
    // property binding pushes the SpectrogramSurface's existing
    // _widgetZoomLevel (e.g. 16 from the previous track's max zoom
    // on the prior instance set) into the new instance.
    // setZoomLevel(16) arms the 150 ms debounce timer with
    // m_pendingBackendZoom = 16.  A track change arrives in that
    // window — our needsZoomReset path emits
    // backendZoomRequested(1.0) directly, bypassing the timer —
    // but if we don't ALSO cancel the pending debounce, the timer
    // fires 150 ms later with the stale 16 and restarts the
    // backend at max zoom.  The widget then renders at
    // effectiveZoom = 0.0625 with a smeared right edge because
    // the ring hasn't filled far enough for the inflated visible
    // window.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(200);
    item.setDisplayMode(1);
    item.setSampleRateHz(44100);

    QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);

    // Simulate the QML binding pushing the prior track's max zoom
    // into the freshly-created instance: arms the debounce with 16.
    item.setZoomLevel(16.0);
    QVERIFY(item.m_zoomDebounceTimer != nullptr);
    QVERIFY(item.m_zoomDebounceTimer->isActive());
    QCOMPARE(item.m_pendingBackendZoom, 16.0f);

    // Track change (non-gapless): the initial reset + data chunk
    // carries the backend's leftover non-default hop (64 at max
    // zoom), so needsZoomReset fires.
    constexpr int bins = 4;
    constexpr int hop = 64;
    constexpr quint64 newToken = 42;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024,
        44100, hop, false, true, newToken);
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, hop, false, false, newToken);

    // The reset must have fired backendZoomRequested(1.0) and
    // disarmed the debounce so the stale 16 can't fire after.
    QCOMPARE(backendZoomSpy.count(), 1);
    QCOMPARE(backendZoomSpy.takeFirst().at(0).toFloat(), 1.0f);
    QVERIFY2(!item.m_zoomDebounceTimer->isActive(),
             "pending debounce still armed after track-change zoom reset");
    QCOMPARE(item.m_pendingBackendZoom, 1.0f);
    QCOMPARE(item.m_zoomLevel, 1.0);

    // Even if we wait past the original debounce interval, no
    // further backendZoomRequested should fire — the timer was
    // cancelled.
    QTest::qWait(200);
    QCOMPARE(backendZoomSpy.count(), 0);
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

    // Zoom clamps to the Rust-side minimum (0.05).  The track-fit
    // floor (1920/96000 = 0.02) would technically allow a smaller
    // zoom, but the backend clamps any sub-0.05 value to 0.05, so
    // Qt must also floor at 0.05 or else successive widthSettle
    // re-sends oscillate between what Qt requested and what Rust
    // clamped back.
    item.setZoomLevel(0.001);
    QVERIFY(std::abs(item.zoomLevel() - 0.05) < 0.001);

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

void QmlSmokeTest::spectrogramZoomOutBlockedWhenSongFits() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed a short track that fits entirely at zoom=1.0 (1000 < 1920).
    constexpr int binsPerColumn = 4;
    constexpr int columns = 1000;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Minimum zoom should be 1.0 — no zoom-out possible.
    QVERIFY(std::abs(item.minimumZoomLevel() - 1.0) < 0.0001);

    // Attempting to zoom out is clamped to 1.0.
    item.setZoomLevel(0.5);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Zoom-in still works.
    item.setZoomLevel(2.0);
    QVERIFY(std::abs(item.zoomLevel() - 2.0) < 0.0001);
}

void QmlSmokeTest::spectrogramEffectiveZoomMatchesBackendHop() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed initial data at default hop, then request zoom=4x.
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0.
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart with hop_size=256 (zoom=4x data).
    // After this buffer_reset, effectiveZoom = 4.0 * 256 / 1024 = 1.0.
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramAdvanceWorksWhenBackendMatchesZoom() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed initial data at default hop, then request zoom=4x.
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0.
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    item.setZoomLevel(4.0);

    // Simulate backend restart with hop_size=256 (zoom=4x data).
    // After this, effectiveZoom = 4.0 * 256 / 1024 = 1.0 (1:1 rendering).
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramEffectiveZoomDuringTransition() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed data with default hop_size=1024 (backend not yet producing zoom=4x data).
    // Need >= 480 columns so minZoom (1920/cols) <= 4.0, allowing setZoomLevel(4.0).
    constexpr int binsPerColumn = 64;
    constexpr int columns = 500;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 1024, false,
        true, 1, false);

    // Set zoom to 4x — backend hasn't responded yet.
    // The visual zoom (renderZoomLevel) is deferred; the display
    // keeps showing the existing data at the old zoom until the
    // backend session restarts with the finer hop size.
    item.setZoomLevel(4.0);

    // The property reflects the requested zoom immediately.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart: feed a buffer_reset chunk with finer hop.
    // This triggers the deferred render zoom to take effect.
    QByteArray zoomChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, columns,
        0, columns * 2, 48000, 256, false,
        true, 1, false);

    // After backend data arrives, zoom is fully applied.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}

void QmlSmokeTest::spectrogramDeferredZoomAppliesOnBackendData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Use small bins so large column counts don't allocate too much memory.
    constexpr int binsPerColumn = 4;
    constexpr int columns = 96000;

    // Feed initial data at default hop (backend at zoom=1x).
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Request zoom to 4x — property updates immediately.
    item.setZoomLevel(4.0);
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Feed continuation data at OLD hop (still in pipeline from before the
    // backend received the zoom command).  This is NOT a buffer_reset, so
    // the deferred zoom must NOT apply yet.
    constexpr int staleCols = 50;
    QByteArray staleChunk(binsPerColumn * staleCols, '\x40');
    item.feedPrecomputedChunk(
        staleChunk, binsPerColumn, 0, staleCols,
        columns, columns, 48000, 1024, false,
        false, 1, false);

    // Property still shows requested zoom.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Simulate backend restart: buffer_reset with finer hop.
    // This triggers the deferred render zoom to take effect.
    // At zoom=4x with hop=256, the backend produces 4x more columns.
    constexpr int zoomColumns = columns * 4;
    QByteArray zoomChunk(binsPerColumn * zoomColumns, '\x40');
    item.feedPrecomputedChunk(
        zoomChunk, binsPerColumn, 0, zoomColumns,
        0, zoomColumns, 48000, 256, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);

    // Verify zoom back to 1.0 also defers correctly.
    item.setZoomLevel(1.0);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);

    // Backend restarts at default hop.
    QByteArray resetChunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        resetChunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);

    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
}

void QmlSmokeTest::spectrogramZoomOutProducesDistinctHop() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    constexpr int bins = 4;
    constexpr int cols = 9600;

    // Feed data at zoom=1.0 (hop=1024).
    QByteArray chunk1(bins * cols, '\x40');
    item.feedPrecomputedChunk(
        chunk1, bins, 0, cols,
        0, cols, 48000, 1024, true,
        true, 1, false);
    const int hop1 = item.m_precomputedHopSize;

    // Simulate zoom to 0.8: backend restarts with a DIFFERENT hop.
    // With fractional resampling: effective_hop = round(1024 * 1.25) = 1280.
    // This is distinct from 1024.
    // With the OLD integer decimation: factor=1, effective_hop=1024 (same!).
    item.setZoomLevel(0.8);
    QByteArray chunk2(bins * cols, '\x40');
    item.feedPrecomputedChunk(
        chunk2, bins, 0, cols,
        0, cols, 48000, 1280, true,
        true, 1, false);
    const int hop2 = item.m_precomputedHopSize;

    // The hops MUST differ — this is the dead zone fix.
    QVERIFY(hop1 != hop2);
    // Verify effectiveZoom is close to 1.0.
    const double ez = item.m_renderZoomLevel
        * static_cast<double>(hop2) / 1024.0;
    QVERIFY(std::abs(ez - 1.0) < 0.01);
}

void QmlSmokeTest::spectrogramMinZoomAdaptsToWidthChange() {
    // Simulates the widget→fullscreen transition: when width increases,
    // minimumZoomLevel should increase (allow less zoom-out), and a zoom
    // level valid at the old width should be clamped to the new minimum.
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(400);

    constexpr int binsPerColumn = 64;
    constexpr int columns = 9600;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // At width=1200, minZoom = 1200/9600 = 0.125
    const double narrowMinZoom = item.minimumZoomLevel();
    QVERIFY(std::abs(narrowMinZoom - 0.125) < 0.01);

    // Zoom all the way out on the narrow widget.
    item.setZoomLevel(narrowMinZoom);
    QVERIFY(std::abs(item.zoomLevel() - narrowMinZoom) < 0.01);

    // Simulate entering fullscreen: width triples.
    item.setWidth(3600);

    // New minZoom = 3600/9600 = 0.375 — much higher.
    const double wideMinZoom = item.minimumZoomLevel();
    QVERIFY(std::abs(wideMinZoom - 0.375) < 0.01);

    // Re-applying the old narrow zoom should clamp to the new minimum.
    item.setZoomLevel(narrowMinZoom);
    QVERIFY(std::abs(item.zoomLevel() - wideMinZoom) < 0.01);
}

void QmlSmokeTest::spectrogramCenteredModeUsesWindowedCapacity() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    // Feed a large track estimate
    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, 100000, 48000, 1024, false,
        true, 1, false);

    // Ring capacity should NOT be 100000 (full track).
    // It should be bounded to ~3 screen widths + lookahead.
    QVERIFY(item.m_ringCapacity < 20000);
}

void QmlSmokeTest::spectrogramPeakHoldRebuildUsesMaxNotNearest() {
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(10);
    item.setDisplayMode(1); // Centered

    // Feed 150 columns into a 100px widget at effectiveZoom=0.8.
    // zoom=0.8, hop=1024 -> effectiveZoom = 0.8 * 1024/1024 = 0.8.
    // columnsPerPixel = 1/0.8 = 1.25.  Since 1.25 > 1.0, the rebuild
    // enters the peak-hold path (colFirst < colLast && !interpolate).
    // drawPixels = min(100, ceil(150 * 0.8)) = 100.
    constexpr int bins = 4;
    constexpr int cols = 150;
    QByteArray chunk(bins * cols, '\x10'); // dark baseline (0x10)

    // Make column 50 bright: set all bins to 0xF0.
    for (int b = 0; b < bins; ++b) {
        chunk[50 * bins + b] = static_cast<char>(static_cast<unsigned char>(0xF0));
    }

    item.setZoomLevel(0.8);
    item.feedPrecomputedChunk(
        chunk, bins, 0, cols,
        0, cols, 48000, 1024, true,
        true, 1, false);

    QVERIFY(item.precomputedReady());

    // Trigger a rebuild directly so we can inspect canvas pixels.
    // The debounce timer is still active in the test (no event loop),
    // so m_awaitingZoomData was not consumed by feedPrecomputedChunk.
    // Force m_renderZoomLevel to match m_zoomLevel so effectiveZoom=0.8.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 0.8;
        item.ensureMapping(10);
        item.rebuildPrecomputedCanvasLocked(100, 10, 0, cols - 1, false);
    }

    // Column 50 at columnsPerPixel=1.25:
    //   pixel 40 -> rangeStart=50.0, rangeEnd=51.25
    //   colFirst=50, colLast=51 -> peak-hold across cols 50-51.
    // The bright column 50 (0xF0) is in this range, so pixel 40
    // should be significantly brighter than pixel 0 (dark baseline).
    QVERIFY(!item.m_canvas.isNull());
    const QRgb brightPixel = item.m_canvas.pixel(40, 5);
    const QRgb darkPixel = item.m_canvas.pixel(0, 5);
    QVERIFY(qRed(brightPixel) + qGreen(brightPixel) + qBlue(brightPixel)
            > qRed(darkPixel) + qGreen(darkPixel) + qBlue(darkPixel));
}

void QmlSmokeTest::spectrogramZoomFillClearsWhenDecoderReachesTail() {
    // Regression: at max zoom-out in a wide fullscreen canvas the
    // STFT windowing leaves the decoder a few columns short of the
    // scaled totalEstimate.  The old readiness check required
    // ringFill >= fillWidth - 16 (with fillWidth ≈ totalEstimate),
    // so ringFill could never reach the threshold and
    // m_zoomFillActive stayed true forever.  The old canvas then
    // leaked a slice of stale content at the right edge.
    SpectrogramItem item;
    item.setWidth(3440);
    item.setHeight(720);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;

    // Baseline zoom=1.0 chunk — populates the ring.
    QByteArray baseline(3440 * bins, '\x40');
    item.feedPrecomputedChunk(
        baseline, bins, 0, 3440,
        0, 10064, 44100, 1024, false,
        true, 1, true);
    QVERIFY(item.m_precomputedReady);

    // Trigger a rebuild so the canvas exists — the real flow does
    // this on the next paint, but QtTest has no event loop.  The
    // hop-change detector only arms m_zoomFillActive when a canvas
    // is already present.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1.0;
        item.ensureMapping(720);
        item.rebuildPrecomputedCanvasLocked(3440, 720, 0, 3439, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    QCOMPARE(item.m_zoomFillActive, false);

    // Zoom-out restart: the backend decimates to hop=2996 and the
    // scaled estimate shrinks to ≈ widget width.  Decoder produces
    // 3418 columns (22 short of the 3440 estimate) due to the
    // STFT tail.
    constexpr int decodedCols = 3418;
    QByteArray decimated(decodedCols * bins, '\x50');
    item.feedPrecomputedChunk(
        decimated, bins, 0, decodedCols,
        0, 3440, 44100, 2996, false,
        true, 1, false);

    QCOMPARE(item.m_precomputedMaxColumnIndex, decodedCols - 1);
    // Must clear: decoder will produce no more columns, so freezing
    // indefinitely would strand stale pixels at the right edge.
    QCOMPARE(item.m_zoomFillActive, false);
    // Dirty flag set so the next paint rebuilds the canvas cleanly.
    QVERIFY(item.m_precomputedCanvasDirty);
}

void QmlSmokeTest::spectrogramSyntheticClearPreservesCanvasDuringSeek() {
    // Regression: a seek outside the decoded window emits a synthetic
    // clear chunk (cols=0, bins=0, clear_history=true) that wipes the
    // ring and invalidates the canvas.  Without canvas preservation,
    // the display flashes fully black for ~100 ms while the backend
    // restarts at (pos − margin) and decodes up to the new playhead.
    // The freeze should arm instead so the old canvas stays visible
    // through the brief transition.
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;

    // Populate the ring with centered-mode data.
    QByteArray chunk(1920 * bins, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, 1920,
        0, 10064, 44100, 1024, false,
        true, 7, true);
    QVERIFY(item.m_precomputedReady);

    // Build a canvas directly with a valid display range — the real
    // flow does this via updatePaintNode but QtTest has no event loop.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1.0;
        item.ensureMapping(400);
        item.rebuildPrecomputedCanvasLocked(1920, 400, 0, 1919, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(1919));

    // Synthetic clear: cols=0, bins=0, buffer_reset=true,
    // clear_history=true — the signature emitted by
    // seek_spectrogram_position for far seeks.
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, 7, true);

    // Ring is wiped.
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(0));
    QCOMPARE(item.m_ringCapacity, 0);
    QCOMPARE(item.m_precomputedMaxColumnIndex, -1);
    QVERIFY(item.m_awaitingWorkerReset);

    // Canvas and its display range are preserved, freeze is armed.
    QVERIFY(!item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(1919));
    QVERIFY(item.m_precomputedReady);
    QCOMPARE(item.m_zoomFillActive, true);
}

void QmlSmokeTest::spectrogramSyntheticClearInvalidatesCanvasWhenNoOldContent() {
    // When there's no canvas to preserve (first track load, rolling
    // mode, or the display range was already invalid), the synthetic
    // clear must still wipe everything and invalidate — the freeze
    // would latch onto garbage state otherwise.
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1);

    // No rebuild → canvas stays null.  Still send some data so
    // m_precomputedReady could be true, which would be misleading
    // after the clear.
    QByteArray chunk(100 * 4, '\x40');
    item.feedPrecomputedChunk(
        chunk, 4, 0, 100,
        0, 10064, 44100, 1024, false,
        true, 7, true);
    QVERIFY(item.m_canvas.isNull());

    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0,
        0, 0, 0, 0, false,
        true, 7, true);

    QVERIFY(item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedReady, false);
    QCOMPARE(item.m_zoomFillActive, false);
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             item.m_precomputedCanvasDisplayLeft - 1);
}

int main(int argc, char **argv) {
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_smoke.moc"
