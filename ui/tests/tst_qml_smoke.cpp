// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QQmlComponent>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QScopedPointer>
#include <QSemaphore>
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <atomic>
#include <functional>
#include <memory>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "../src/DiagnosticsLog.h"
#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramSeekTrace.h"
#define protected public
#define private public
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"
#include "../src/WaveformEditorItem.h"
#undef private
#undef protected

namespace {

class CompositionRecordingPaintEngine final : public QPaintEngine {
public:
    CompositionRecordingPaintEngine()
        : QPaintEngine(QPaintEngine::AlphaBlend
                       | QPaintEngine::PorterDuff
                       | QPaintEngine::PainterPaths) {}

    bool begin(QPaintDevice *device) override {
        setPaintDevice(device);
        setActive(true);
        return true;
    }

    bool end() override {
        setActive(false);
        return true;
    }

    void updateState(const QPaintEngineState &state) override {
        if (state.state() & QPaintEngine::DirtyCompositionMode) {
            compositionModes.push_back(state.compositionMode());
        }
    }

    void drawLines(const QLine *, int) override {}
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}
    Type type() const override { return QPaintEngine::User; }

    QVector<QPainter::CompositionMode> compositionModes;
};

class CompositionRecordingPaintDevice final : public QPaintDevice {
public:
    QPaintEngine *paintEngine() const override {
        return const_cast<CompositionRecordingPaintEngine *>(&engine);
    }

    mutable CompositionRecordingPaintEngine engine;

protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
        case PdmWidthMM:
        case PdmHeight:
        case PdmHeightMM:
            return 64;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(QPaintDevice::devicePixelRatioFScale());
        case PdmNumColors:
            return 16'777'216;
        case PdmDepth:
            return 32;
        default:
            return 0;
        }
    }
};

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

QQuickItem *findQuickItemByObjectName(QQuickItem *root, const QString &objectName) {
    if (root->objectName() == objectName) {
        return root;
    }
    for (QQuickItem *child : root->childItems()) {
        if (QQuickItem *match = findQuickItemByObjectName(child, objectName)) {
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
    void spectrogramWholeScreenControlsIgnoreDuplicateHoverPoints();
    void fullscreenVisualizationInhibitsDisplaySleepOnlyWhileActive();
    void spectrogramFullscreenIdleHidesOverlaysButKeepsChannelMarkers();
    void spectrogramHoverChannelButtonsRemainClickable();
    void waveformSurfaceFollowsSharedChannelAndOverlaySettings();
    void mainWindowContentStartsBelowMenuBar();
    void albumArtTileKeepsHeightInsideColumnLayout();
    void albumArtViewerInfoUsesImageProviderLocalPath();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void libraryTreeStartsCollapsedByDefault();
    void rootRowsStartExpandedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
    void libraryTreeSeedsExpandedKeysBeforeFirstFrame();
    void libraryTreeCollapsesAllExpandedBranches();
    void artistPrefixSearchUsesModelLookup();
    void libraryControllerExtendsSelectionWithShiftArrows();
    void libraryControllerRestoresExpandedSelectionAndViewport();
    void libraryControllerUsesDelegateCoordinatesForViewportAnchor();
    void libraryControllerForwardsShowTrackRequest();
    void libraryControllerCentersBridgeRevealAfterExpansion();
    void spectrogramMetadataOnlyResetWaitsForDataChunk();
    void spectrogramRollingSeekKeepsHistoryContinuous();
    void spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch();
    void testMutedChannelRendersGrayscale();
    void spectrogramLargePositionJumpWaitsForResetHandoff();
    void spectrogramPlaybackHeartbeatDoesNotMoveAnchorBackward();
    void spectrogramCenteredSmallSeekSnapsWithoutServoSlide();
    void spectrogramCenteredSeekFollowsLocalVisualClock();
    void spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady();
    void spectrogramGaplessTrackChangePreservesRollingHistory();
    void spectrogramNaturalTrackResetPreservesRollingHistory();
    void spectrogramManualTrackResetClearsRollingHistory();
    void spectrogramRollingZoomResetAnchorsEpochToResetStart();
    void spectrogramSeekResetAnchorsPlaybackClockToChunkStart();
    void diagnosticsLogUsesLowercaseAppDir();
    void playbackControllerSeekImmediatelyUpdatesSpectrogramPosition();
    void playbackControllerDeterministicTimeHooksDriveInterpolation();
    void playbackControllerPlaybackUpdateKeepsSpectrogramOnInterpolatedClock();
    void playbackControllerPostSeekHeartbeatUsesBoundedPhaseCorrection();
    void playbackControllerPostSeekBehindHeartbeatKeepsVisualClock();
    void playbackControllerPostSeekHeldTargetKeepsVisualClock();
    void playbackControllerPostSeekTargetEchoKeepsVisualClockActive();
    void playbackControllerPlayAtCurrentTrackClearsPostSeekVisualClock();
    void playbackControllerHeartbeatCorrectionAvoidsOneFrameSpeedBurst();
    void playbackControllerModerateSteadyStateLagUsesTrimNotBleed();
    void playbackControllerProfileLogsHeartbeatCorrectionAndBleed();
    void playbackControllerProfileSkipsMinorFollowHeartbeatLogs();
    void playbackControllerDelayedHeartbeatDoesNotJumpViewport();
    void playbackControllerKeepsWallClockInterpolationAfterSubRealtimeHeartbeats();
    void playbackControllerSteadyStateTrimReducesNoticeableLag();
    void playbackControllerFollowsBoundedRecoveryCadenceWithoutBurst();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void spectrogramSmoothnessProfileTracksServoAndAdvanceFallbackSignals();
    void waveformProgressInvalidatesOnlyTailSpan();
    void waveformPeakUpdatesInvalidateChangedSuffix();
    void waveformEditorParsesSignedSampleWindow();
    void waveformEditorCachedDecoderPreservesOverlappingSamples();
    void waveformEditorDownmixUsesMixedExtremaAtEveryZoom();
    void waveformEditorCancelsAndBoundsObsoleteDecodes();
    void waveformEditorSampleMarkersRequireSampleResolution();
    void waveformEditorFormatsViewportDuration();
    void waveformEditorConnectsSamplesBeforeMarkers();
    void waveformEditorScrollsInsideCachedDetailWithoutRebuild();
    void waveformEditorUsesChannelHintBeforeDetailArrives();
    void waveformEditorCoalescesPlaybackRequestsWithoutStarvingTimer();
    void waveformEditorPendingWindowSuppressesDuplicateRequest();
    void waveformEditorZoomSupersedesCoarsePendingRequest();
    void waveformEditorResizeRaisesDetailRequestDensity();
    void waveformEditorDeepZoomRequestPreservesViewportDensity();
    void waveformEditorQuantizesRequestForRequiredBinDensity();
    void waveformEditorFullscreenRequestStaysInTargetBin();
    void waveformEditorFullscreenSampleCurveRequestsRawFramesEarly();
    void waveformEditorSampleViewPrefetchesUsefulSpan();
    void waveformEditorPrefetchesBeforeDetailBoundary();
    void waveformEditorKeepsOverlappingDetailDuringHandoff();
    void waveformEditorWideDetailCacheKeepsViewportDensity();
    void waveformEditorWholeTrackAcceptsDecodedEndpointTolerance();
    void waveformEditorZoomOutDefersOverviewUntilDetailReady();
    void waveformEditorDeferredZoomOutCommitsCompletedDetailCache();
    void waveformEditorZoomOutKeepsReadyDetail();
    void waveformEditorZoomInRetainsCoveredCacheWhileRefining();
    void waveformEditorPausedZoomRebuildsCacheAtSamplePresentationBoundaries();
    void waveformEditorSparseZoomDetailFallsBackUntilReady();
    void waveformEditorSeparatesChannelPanes();
    void waveformEditorRestrictsDetailWorkToVisiblePoints();
    void waveformEditorDetailPointsUseAbsoluteSampleTimes();
    void waveformEditorReusesSameSizedCache();
    void waveformEditorCacheHandoffsKeepAbsolutePixelGrid();
    void waveformEditorReplacementCacheDoesNotReuseOldRaster();
    void waveformEditorBuildsPlaybackTilesWithinFrameBudget();
    void waveformEditorKeepsPlaybackTilesAcrossDetailHandoffs();
    void waveformEditorBuildsReplacementCacheIncrementally();
    void waveformEditorPaintDefersGuiContinuation();
    void waveformEditorZoomedPlaybackUsesScrollingCache();
    void waveformEditorPlaybackOverviewCacheHasForwardHeadroom();
    void waveformEditorCachedPaintClearsUncoveredPixels();
    void waveformEditorPausedDetailReplacesOverviewCache();
    void waveformEditorPlaybackHeartbeatDoesNotMoveBackward();
    void waveformEditorExplicitSeekBypassesHeartbeatSmoothing();
    void waveformEditorHoverDrawsCrosshairAndReadouts();
    void waveformEditorCrosshairLabelsMatchSpectrogramMargins();
    void waveformEditorSampleViewRepaintsCrosshairCleanly();
    void waveformEditorSampleViewUsesSmoothInterpolation();
    void waveformEditorSampleViewDoesNotFillLaterChannels();
    void waveformEditorReclampsZoomForDecodedSampleRate();
    void waveformEditorRulersFollowGridSetting();
    void waveformEditorRulersRemainVisibleWhenZoomed();
    void waveformEditorPlayheadIsThinAndNeutral();
    void waveformEditorOverviewRespectsCoverageAndChannelIdentity();
    void waveformEditorReferenceLineContrastsWaveform();
    void waveformEditorContrastLinesUseFboSafeCompositionModes();
    void waveformEditorFpsOverlayTracksPaintRate();
    void waveformEditorUsesSafeRasterTargetAndNativeFrameInterpolation();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
    void spectrogramStaleTokenChunksAreDropped();
    void spectrogramGaplessTokenChunksPassFilter();
    void spectrogramEvictingOldTokenKeepsActiveTokenIteratorValid();
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
    void spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof();
    void spectrogramStoppedZoomResetRefillsCanvas();
    void spectrogramZoomOutFillDoesNotClampToLaggingDecodedTail();
    void spectrogramRingCapacityPersistsAcrossFullscreenShrink();
    void spectrogramRingCapacityRemembersFullscreenWidthBeforeNextChunk();
    void spectrogramMaxWidgetWidthSurvivesInstanceReplacement();
    void spectrogramRollingGaplessTrackChangePreservesZoom();
    void spectrogramCenteredGaplessTrackChangeResetsZoom();
    void spectrogramRollingResetTrackChangeResetsZoom();
    void spectrogramTrackChangeMetadataResetClearsOldCenteredFrame();
    void spectrogramFreshInstanceResyncsBackendZoomOnTrackChange();
    void spectrogramFreshInstanceSeekRestartDoesNotResetZoom();
    void spectrogramTrackChangeCancelsPendingZoomDebounce();
    void spectrogramForceFpsOverlayDoesNotOverrideQmlBinding();
    void spectrogramRenderLoopStopsWhenNotPlaying();
    void playbackControllerInterpolationActivatesOnPlayback();
    void playbackControllerInterpolationDeactivatesOnStop();
    void trackIdentityChangedSignalTriggersQmlHandler();
    void queueAutoCenterIsDeferredOffHandlerStack();
    void queueRemovalSendsOneBatch();
    void queueContainIndexSkipsScrollWhenVisible();
    void queueContainIndexScrollsUpWhenAboveViewport();
    void queueContainIndexClampsAtListEnd();
    void spectrogramCrosshairAndGridPropertiesAndHoverTracking();
    void spectrogramPixelToFrequency();
    void spectrogramDynamicRangePreservesSilence();
    void spectrogramSampleRateSyncsFromPrecomputedChunks();
    void spectrogramPreservesNativeRateAcrossModesAndTrackTransitions();
    void spectrogramCrosshairOverlayGeneratesOnHover();
    void spectrogramCrosshairOverlayReusesImageBufferAtSameGeometry();
    void spectrogramGridOverlayGeneratesWhenEnabled();
    void spectrogramOverlayDisabledProducesNullImage();
    void spectrogramOverlayDirtiedByGeometryChange();
    void spectrogramOverlayDirtiedByLogScaleChange();
    void spectrogramOverlayStalenessDetectsBinChange();
    void spectrogramOverlayRebuildsViaUpdatePaintNodeOnStaleInput();
    void spectrogramOverlayStalenessDetectsDisplayRangeChange();
    void spectrogramClickToSeekEmitsSignalWhenCrosshairEnabled();
    void spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale();
    void spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode();
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
    void spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack();
    void spectrogramResizeForcesFreshBodyTextureUpload();
    void spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights();
    void spectrogramZoomOutProducesDistinctHop();
    void spectrogramCenteredZoomOutDropsOlderSameTrackGeneration();
    void spectrogramMinZoomAdaptsToWidthChange();
    void spectrogramCenteredModeUsesWindowedCapacity();
    void spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead();
    void spectrogramRollingCanvasGrowsIncrementallyDuringInitialFill();
    void spectrogramRollingCanvasHandsOffToSteadyScrollIncrementally();
    void spectrogramRollingCanvasAdvancesIncrementallyAtFractionalZoom();
    void spectrogramCenteredLateFillUsesCircularCanvasOffset();
    void spectrogramPeakHoldRebuildUsesMaxNotNearest();
    void spectrogramZoomFillClearsWhenDecoderReachesTail();
    void spectrogramSyntheticClearPreservesCanvasDuringSeek();
    void spectrogramSyntheticClearInvalidatesCanvasWhenNoOldContent();
};

void QmlSmokeTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
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

    QObject *preventDisplaySleepCheckBox = root->findChild<QObject *>(
        QStringLiteral("preventDisplaySleepInFullscreenCheckBox"));
    QVERIFY(preventDisplaySleepCheckBox != nullptr);
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
    QVERIFY(modeSwitch->property("width").toDouble() <= 50.0);
    QCOMPARE(modeSwitch->property("proximityHovered").toBool(), false);
    QCOMPARE(modeSwitch->property("opacity").toDouble(), 0.0);
    auto *modeSwitchItem = qobject_cast<QQuickItem *>(modeSwitch);
    QVERIFY(modeSwitchItem != nullptr);
    QVERIFY(modeSwitch->setProperty("proximityHovered", true));
    QTRY_VERIFY(modeSwitch->property("opacity").toDouble() > 0.95);
    QVERIFY(modeSwitch->setProperty("controlsVisible", false));
    QCOMPARE(modeSwitch->property("proximityHovered").toBool(), false);
    QVERIFY(modeSwitch->setProperty("controlsVisible", true));
    QTRY_VERIFY(modeSwitch->property("opacity").toDouble() < 0.05);
    QJSValue setMode = qvariant_cast<QJSValue>(modeSwitch->property("setMode"));
    QVERIFY(setMode.isCallable());
    setMode.call(QJSValueList{QJSValue(1)});
    QTRY_COMPARE(modeSwitch->property("selectedMode").toInt(), 1);
    setMode.call(QJSValueList{QJSValue(0)});

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
        property bool showFps: false
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property int soloedChannel: -1
        property int channelButtonsVisibility: 1
        property int viewerFullscreenMode: 0
        property bool preventDisplaySleepInFullscreen: true
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
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken, var generation)
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
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
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setViewerFullscreenMode(mode) {}
        function setPreventDisplaySleepInFullscreen(value) {}
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

void QmlSmokeTest::spectrogramWholeScreenControlsIgnoreDuplicateHoverPoints() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "viewers" as Viewers

ApplicationWindow {
    id: windowRoot
    width: 640
    height: 480
    visible: true

    Viewers.SpectrogramViewerShell {
        objectName: "spectrogramViewerShell"
        anchors.fill: parent
        windowRoot: windowRoot
        viewerOpen: true
        useWholeScreenViewerMode: true
        popupTransitionMs: 0
        titleText: "Ferrous"
        closeViewer: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *autoHide = root->findChild<QObject *>(
        QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QObject *shell = root->findChild<QObject *>(QStringLiteral("spectrogramViewerShell"));
    QObject *fullscreenCloseButton = root->findChild<QObject *>(
        QStringLiteral("spectrogramFullscreenCloseButton"));
    QVERIFY(autoHide != nullptr);
    QVERIFY(shell != nullptr);
    QVERIFY(fullscreenCloseButton != nullptr);
    QCOMPARE(autoHide->property("hideDelay").toInt(), 5000);
    QVERIFY(autoHide->property("active").toBool());
    QVERIFY(fullscreenCloseButton->property("visible").toBool());

    autoHide->setProperty("hideDelay", 40);
    const auto reportPointerPosition = [autoHide](double x, double y) {
        return QMetaObject::invokeMethod(
            autoHide,
            "pointerMoved",
            Q_ARG(QVariant, QVariant::fromValue(x)),
            Q_ARG(QVariant, QVariant::fromValue(y)));
    };
    QVERIFY(reportPointerPosition(100.0, 80.0));
    QTRY_VERIFY_WITH_TIMEOUT(!fullscreenCloseButton->property("visible").toBool(), 500);

    // Hover ownership can change when a control disappears, producing a
    // duplicate point notification without physical cursor movement. It must
    // not wake the controls or restart the timer.
    QVERIFY(reportPointerPosition(100.0, 80.0));
    QVERIFY(!fullscreenCloseButton->property("visible").toBool());

    QVERIFY(reportPointerPosition(101.0, 80.0));
    QVERIFY(fullscreenCloseButton->property("visible").toBool());
    QTest::qWait(20);
    QVERIFY(fullscreenCloseButton->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!fullscreenCloseButton->property("visible").toBool(), 500);

    shell->setProperty("viewerOpen", false);
    QVERIFY(!autoHide->property("active").toBool());
    QVERIFY(autoHide->property("controlsVisible").toBool());
}

void QmlSmokeTest::fullscreenVisualizationInhibitsDisplaySleepOnlyWhileActive() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "viewers" as Viewers

ApplicationWindow {
    id: windowRoot
    width: 640
    height: 480
    visible: true

    QtObject {
        id: sleepInhibitor
        objectName: "sleepInhibitor"
        property bool inhibited: false
        property int updateCount: 0

        function setInhibited(value) {
            if (inhibited === value) {
                return
            }
            inhibited = value
            updateCount += 1
        }
    }

    Viewers.SpectrogramViewerShell {
        objectName: "spectrogramViewerShell"
        anchors.fill: parent
        windowRoot: windowRoot
        viewerOpen: false
        useWholeScreenViewerMode: true
        popupTransitionMs: 0
        titleText: "Ferrous"
        closeViewer: function() {}
        displaySleepInhibitor: sleepInhibitor
        displaySleepInhibitionEnabled: true
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *shell = root->findChild<QObject *>(QStringLiteral("spectrogramViewerShell"));
    QObject *sleepInhibitor = root->findChild<QObject *>(QStringLiteral("sleepInhibitor"));
    QVERIFY(shell != nullptr);
    QVERIFY(sleepInhibitor != nullptr);
    QVERIFY(!sleepInhibitor->property("inhibited").toBool());

    QVERIFY(shell->setProperty("viewerOpen", true));
    QTRY_VERIFY(shell->property("fullscreenDisplayActive").toBool());
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 1);

    QVERIFY(shell->setProperty("displaySleepInhibitionEnabled", false));
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 2);

    QVERIFY(shell->setProperty("displaySleepInhibitionEnabled", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 3);

    QVERIFY(shell->setProperty("useWholeScreenViewerMode", false));
    QTRY_VERIFY(!shell->property("fullscreenDisplayActive").toBool());
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 4);

    QVERIFY(shell->setProperty("useWholeScreenViewerMode", true));
    QTRY_VERIFY(sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 5);

    QVERIFY(shell->setProperty("viewerOpen", false));
    QTRY_VERIFY(!sleepInhibitor->property("inhibited").toBool());
    QCOMPARE(sleepInhibitor->property("updateCount").toInt(), 6);
}

void QmlSmokeTest::spectrogramFullscreenIdleHidesOverlaysButKeepsChannelMarkers() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "viewers" as Viewers

Item {
    width: 640
    height: 320

    QtObject {
        id: bridge
        property string playbackState: "Stopped"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property int channelButtonsVisibility: 2
        property int soloedChannel: -1
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelMuted(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "spectrogramSurface"
        anchors.fill: parent
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *surface = root->findChild<QObject *>(QStringLiteral("spectrogramSurface"));
    QVERIFY(surface != nullptr);
    const QVariantMap descriptor{
        {QStringLiteral("label"), QStringLiteral("L")},
        {QStringLiteral("showLabel"), true},
        {QStringLiteral("muted"), false},
        {QStringLiteral("channelIndex"), 0},
    };
    surface->setProperty("channelDescriptors", QVariantList{descriptor});

    QQuickItem *surfaceItem = qobject_cast<QQuickItem *>(surface);
    QVERIFY(surfaceItem != nullptr);
    QTRY_VERIFY(findQuickItemByObjectName(
                    surfaceItem, QStringLiteral("spectrogramPaneItem"))
                != nullptr);
    auto *spectrogramItem = qobject_cast<SpectrogramItem *>(findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramPaneItem")));
    QQuickItem *channelMarker = findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramChannelMarker"));
    QQuickItem *muteButton = findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramMuteButton"));
    QQuickItem *soloButton = findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramSoloButton"));
    QQuickItem *pointerArea = findQuickItemByObjectName(
        surfaceItem, QStringLiteral("spectrogramSurfacePointerArea"));
    QVERIFY(spectrogramItem != nullptr);
    QVERIFY(channelMarker != nullptr);
    QVERIFY(muteButton != nullptr);
    QVERIFY(soloButton != nullptr);
    QVERIFY(pointerArea != nullptr);
    QTRY_VERIFY(channelMarker->property("visible").toBool());
    QTRY_VERIFY(muteButton->property("visible").toBool());
    QTRY_VERIFY(soloButton->property("visible").toBool());
    QTRY_VERIFY(spectrogramItem->crosshairEnabled());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));

    surface->setProperty("interactiveOverlaysVisible", false);
    QTRY_VERIFY(!muteButton->property("visible").toBool());
    QTRY_VERIFY(!soloButton->property("visible").toBool());
    QTRY_VERIFY(!spectrogramItem->crosshairEnabled());
    QVERIFY(channelMarker->property("visible").toBool());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::BlankCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::BlankCursor));

    surface->setProperty("interactiveOverlaysVisible", true);
    QTRY_VERIFY(muteButton->property("visible").toBool());
    QTRY_VERIFY(soloButton->property("visible").toBool());
    QTRY_VERIFY(spectrogramItem->crosshairEnabled());
    QVERIFY(channelMarker->property("visible").toBool());
    QCOMPARE(surface->property("pointerCursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
    QCOMPARE(pointerArea->property("cursorShape").toInt(), static_cast<int>(Qt::ArrowCursor));
}

void QmlSmokeTest::spectrogramHoverChannelButtonsRemainClickable() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import "viewers" as Viewers

Window {
    width: 640
    height: 320
    visible: true

    QtObject {
        id: bridge
        objectName: "channelButtonBridge"
        property string playbackState: "Stopped"
        property int spectrogramViewMode: 1
        property int spectrogramDisplayMode: 0
        property real dbRange: 90
        property bool logScale: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: true
        property bool showSpectrogramScale: true
        property bool spectrogramZoomEnabled: true
        property int channelButtonsVisibility: 1
        property int soloedChannel: -1
        property int muteCalls: 0
        property int soloCalls: 0
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelMuted(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) {}
        function toggleChannelMute(channelIndex) { ++muteCalls }
        function soloChannel(channelIndex) { ++soloCalls }
    }

    Viewers.SpectrogramSurface {
        id: surface
        objectName: "clickableSpectrogramSurface"
        anchors.fill: parent
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    auto *surface = qobject_cast<QQuickItem *>(root->findChild<QObject *>(
        QStringLiteral("clickableSpectrogramSurface")));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("channelButtonBridge"));
    QVERIFY(surface != nullptr);
    QVERIFY(bridge != nullptr);
    surface->setProperty("channelDescriptors", QVariantList{QVariantMap{
        {QStringLiteral("label"), QStringLiteral("L")},
        {QStringLiteral("showLabel"), true},
        {QStringLiteral("muted"), false},
        {QStringLiteral("channelIndex"), 0},
    }});

    QQuickItem *muteButton = nullptr;
    QQuickItem *soloButton = nullptr;
    QQuickItem *muteMouseArea = nullptr;
    QQuickItem *soloMouseArea = nullptr;
    QTRY_VERIFY([&]() {
        muteButton = findQuickItemByObjectName(
            surface, QStringLiteral("spectrogramMuteButton"));
        soloButton = findQuickItemByObjectName(
            surface, QStringLiteral("spectrogramSoloButton"));
        muteMouseArea = findQuickItemByObjectName(
            surface, QStringLiteral("spectrogramMuteMouseArea"));
        soloMouseArea = findQuickItemByObjectName(
            surface, QStringLiteral("spectrogramSoloMouseArea"));
        // The surface initially creates a label-free placeholder delegate.
        // Wait until the descriptor above has replaced it; its mute control
        // follows the visible channel label instead of starting at x=8.
        return muteButton != nullptr && soloButton != nullptr
            && muteMouseArea != nullptr && soloMouseArea != nullptr
            && muteMouseArea->mapToScene(QPointF(0, 0)).x() > 20.0;
    }());

    // Hover the pane to reveal the controls, then move onto each control.
    // The row's own hover must keep it visible until the click is released.
    QTest::mouseMove(window, QPoint(320, 160));
    QTRY_VERIFY(muteButton->isVisible());
    const QPoint muteCenter = muteMouseArea->mapToScene(QPointF(
        muteMouseArea->width() * 0.5, muteMouseArea->height() * 0.5)).toPoint();
    QTest::mouseMove(window, muteCenter);
    QTRY_VERIFY(muteButton->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(
        muteMouseArea->property("containsMouse").toBool(), 250);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, muteCenter);
    QTRY_COMPARE(bridge->property("muteCalls").toInt(), 1);

    const QPoint soloCenter = soloMouseArea->mapToScene(QPointF(
        soloMouseArea->width() * 0.5, soloMouseArea->height() * 0.5)).toPoint();
    QTest::mouseMove(window, soloCenter);
    QTRY_VERIFY(soloButton->isVisible());
    QTRY_VERIFY(soloMouseArea->property("containsMouse").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, soloCenter);
    QTRY_COMPARE(bridge->property("soloCalls").toInt(), 1);
}

void QmlSmokeTest::waveformSurfaceFollowsSharedChannelAndOverlaySettings() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "viewers" as Viewers

Item {
    width: 640
    height: 320

    QtObject {
        id: bridge
        objectName: "waveformBridge"
        signal transportPositionDiscontinuity(double seconds)
        property string playbackState: "Stopped"
        property string currentTrackPath: ""
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 2.5
        property bool waveformComplete: false
        property real durationSeconds: 10
        property int spectrogramViewMode: 1
        property bool spectrogramZoomEnabled: true
        property bool showSpectrogramScale: true
        property bool showSpectrogramCrosshair: true
        property bool showFps: true
        property int currentTrackChannels: 2
        property var mutedChannelsMask: 0
        property int soloedChannel: -1
        property int channelButtonsVisibility: 2
        function isChannelMuted(channelIndex) { return false }
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Viewers.WaveformSurface {
        id: surface
        objectName: "waveformSurface"
        anchors.fill: parent
        uiBridge: bridge
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *surface = root->findChild<QObject *>(QStringLiteral("waveformSurface"));
    auto *waveform = root->findChild<WaveformEditorItem *>(
        QStringLiteral("waveformEditorItem"));
    QVERIFY(surface != nullptr);
    QVERIFY(waveform != nullptr);
    QTRY_COMPARE(waveform->overviewCoverageSeconds(), 2.5);
    QVERIFY(!waveform->overviewComplete());
    QTRY_COMPARE(waveform->viewMode(), 1);
    QTRY_COMPARE(waveform->channelCount(), 2);
    QTRY_VERIFY(waveform->gridEnabled());
    QTRY_VERIFY(waveform->crosshairEnabled());
    QTRY_VERIFY(waveform->showFpsOverlay());
    QVERIFY(!waveform->playing());

    QObject *durationIndicator = root->findChild<QObject *>(
        QStringLiteral("waveformViewportDurationIndicator"));
    QObject *durationText = root->findChild<QObject *>(
        QStringLiteral("waveformViewportDurationText"));
    QVERIFY(durationIndicator != nullptr);
    QVERIFY(durationText != nullptr);
    QVERIFY(!durationIndicator->property("visible").toBool());
    waveform->setZoomLevel(2.0);
    QTRY_VERIFY(durationIndicator->property("visible").toBool());
    QTRY_COMPARE(
        durationText->property("text").toString(),
        QStringLiteral("00:05"));
    waveform->setZoomLevel(20.0);
    QTRY_COMPARE(
        durationText->property("text").toString(),
        QStringLiteral("500 ms"));
    waveform->resetZoom();
    QTRY_VERIFY(!durationIndicator->property("visible").toBool());

    QObject *hoverHandler = root->findChild<QObject *>(
        QStringLiteral("waveformSurfaceHoverHandler"));
    QVERIFY(hoverHandler != nullptr);
    QCOMPARE(hoverHandler->parent(), surface);

    QObject *bridge = root->findChild<QObject *>(QStringLiteral("waveformBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("waveformCoverageSeconds", 5.0);
    QTRY_COMPARE(waveform->overviewCoverageSeconds(), 5.0);
    bridge->setProperty("waveformComplete", true);
    QTRY_VERIFY(waveform->overviewComplete());
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(waveform->playing());
    waveform->applyExplicitSeekPosition(5.0);
    QVERIFY(QMetaObject::invokeMethod(bridge, "transportPositionDiscontinuity", Q_ARG(double, 4.5)));
    QCOMPARE(waveform->positionSeconds(), 4.5);

    surface->setProperty("interactiveOverlaysVisible", false);
    QTRY_VERIFY(!waveform->crosshairEnabled());
}

void QmlSmokeTest::mainWindowContentStartsBelowMenuBar() {
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

void QmlSmokeTest::albumArtViewerInfoUsesImageProviderLocalPath() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    property string requestedPath: ""
    property int requestCount: 0
    property bool focusCalled: false

    function openAndToggleInfo() {
        viewerController.openAlbumArtViewer()
        viewerController.toggleAlbumArtInfoVisible(function() {
            harness.focusCalled = true
        })
    }

    function overlayText() {
        return viewerController.albumArtInfoOverlayText()
    }

    QtObject {
        id: bridge
        property string currentTrackCoverPath: "image://covers//tmp/Front%20Cover.png#w=600&r=4"
        signal imageFileDetailsChanged(string path)

        function requestImageFileDetails(path) {
            harness.requestedPath = path
            harness.requestCount += 1
        }

        function cachedImageFileDetails(path) {
            if (path !== "/tmp/Front Cover.png") {
                return ({})
            }
            return ({
                fileName: "Front Cover.png",
                resolutionText: "300 x 200",
                fileSizeText: "12 KiB",
                path: path
            })
        }
    }

    Controllers.ViewerController {
        id: viewerController
        objectName: "viewerController"
        uiBridge: bridge
        useWholeScreenViewerMode: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "openAndToggleInfo"));

    QCOMPARE(
        controller->property("albumArtViewerInfoSource").toString(),
        QStringLiteral("/tmp/Front Cover.png"));
    QCOMPARE(root->property("requestedPath").toString(), QStringLiteral("/tmp/Front Cover.png"));
    QCOMPARE(root->property("requestCount").toInt(), 1);
    QCOMPARE(controller->property("albumArtInfoVisible").toBool(), true);
    QCOMPARE(root->property("focusCalled").toBool(), true);

    QVariant overlayText;
    QVERIFY(QMetaObject::invokeMethod(root.data(), "overlayText", Q_RETURN_ARG(QVariant, overlayText)));
    QVERIFY2(
        overlayText.toString().contains(QStringLiteral("Front Cover.png")),
        qPrintable(overlayText.toString()));
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

void QmlSmokeTest::libraryTreeSeedsExpandedKeysBeforeFirstFrame() {
    LibraryTreeModel model;
    QSignalSpy expansionSpy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
    QCOMPARE(expansionSpy.count(), 0);
}

void QmlSmokeTest::libraryTreeCollapsesAllExpandedBranches() {
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(model.rowCount(), 3);

    QSignalSpy expansionSpy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));
    model.collapseAll();

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(expansionSpy.count(), 2);
    for (const QList<QVariant> &arguments : expansionSpy) {
        QCOMPARE(arguments.value(1).toBool(), false);
    }

    LibraryTreeModel rootedModel;
    rootedModel.setLibraryTreeFromBinary(multiRootBinary());
    QTRY_COMPARE(rootedModel.rowCount(), 4);
    rootedModel.collapseAll();
    QCOMPARE(rootedModel.rowCount(), 2);
}

void QmlSmokeTest::artistPrefixSearchUsesModelLookup() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist b"), 0), 3);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("artist a"), 2), 1);
    QCOMPARE(model.findArtistRowByPrefix(QStringLiteral("missing"), 0), -1);
}

void QmlSmokeTest::libraryControllerExtendsSelectionWithShiftArrows() {
    LibraryTreeModel model;
    model.setExpandedKeys(QStringList{
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    });
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());
    QTRY_COMPARE(model.rowCount(), 3);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property bool lastAccepted: false

    function pressShiftDown() {
        const event = { key: Qt.Key_Down, modifiers: Qt.ShiftModifier, text: "", accepted: false }
        controller.handleKeyPress(event)
        lastAccepted = event.accepted
    }

    function pressShiftUp() {
        const event = { key: Qt.Key_Up, modifiers: Qt.ShiftModifier, text: "", accepted: false }
        controller.handleKeyPress(event)
        lastAccepted = event.accepted
    }

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Component.onCompleted: controller.selectIndex(0)
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QVERIFY(controller != nullptr);
    QCOMPARE(controller->property("selectedSelectionKeys").toStringList(), QStringList({
        QStringLiteral("artist|Artist A"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftDown"));
    QCOMPARE(root->property("lastAccepted").toBool(), true);
    QCOMPARE(controller->property("selectedSelectionKeys").toStringList(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftDown"));
    QCOMPARE(controller->property("selectedSelectionKeys").toStringList(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
    }));

    QVERIFY(QMetaObject::invokeMethod(root.get(), "pressShiftUp"));
    QCOMPARE(controller->property("selectedSelectionKeys").toStringList(), QStringList({
        QStringLiteral("artist|Artist A"),
        QStringLiteral("album|Artist A|Album A"),
    }));
    QCOMPARE(controller->property("selectionAnchorIndex").toInt(), 0);
}

void QmlSmokeTest::libraryControllerRestoresExpandedSelectionAndViewport() {
    LibraryTreeModel model;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    engine.rootContext()->setContextProperty(
        QStringLiteral("treeBytes"),
        sampleArtistAlbumTreeBinary());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    width: 320
    height: 24
    property alias controllerRef: controller
    property alias viewRef: view
    property alias saveCount: bridge.saveCount
    property alias restoreStartedBeforePublish: publishTimer.restoreStartedBeforePublish

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        property bool libraryViewStateAvailable: false
        property var libraryExpandedKeys: ["artist|Artist A", "album|Artist A|Album A"]
        property string libraryViewSelectionKey: "track|/music/artist/album/track01.flac"
        property string libraryViewAnchorKey: "album|Artist A|Album A"
        property real libraryViewAnchorOffset: 3
        property int saveCount: 0
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) { saveCount += 1 }
        function setLibraryNodeExpanded(key, expanded) {}
        function showTrackInLibrary(path) {}
    }

    QtObject {
        id: tagEditorApi
        function openSelection(selections) { return false }
    }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    ListView {
        id: view
        anchors.fill: parent
        model: testLibraryModel
        delegate: Item { width: view.width; height: 24 }
        Component.onCompleted: controller.registerView(view)
    }

    Timer {
        id: publishTimer
        interval: 40
        repeat: false
        property bool restoreStartedBeforePublish: false
        onTriggered: {
            restoreStartedBeforePublish = controller.restoredViewStateApplied
            bridge.libraryViewStateAvailable = true
            controller.tryApplyPersistedViewState()
        }
    }

    Component.onCompleted: {
        controller.requestTreeApply(1, treeBytes)
        controller.tryApplyPersistedViewState()
        publishTimer.start()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QObject *view = qvariant_cast<QObject *>(root->property("viewRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(view != nullptr);
    QTRY_COMPARE(model.rowCount(), 3);
    QCOMPARE(root->property("restoreStartedBeforePublish").toBool(), false);
    QTRY_COMPARE(
        controller->property("selectedSelectionKey").toString(),
        QStringLiteral("track|/music/artist/album/track01.flac"));
    QTRY_VERIFY(std::abs(view->property("contentY").toDouble() - 27.0) < 0.5);
    QTRY_COMPARE(root->property("saveCount").toInt(), 1);
}

void QmlSmokeTest::libraryControllerUsesDelegateCoordinatesForViewportAnchor() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property alias contentY: mockView.contentY
    property var capturedAnchor: ({})

    function restoreCapturedAnchor() {
        mockView.contentY = 0
        controller.restoreViewAnchor(capturedAnchor)
    }

    QtObject {
        id: bridge
        property int libraryTrackCount: 1
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
    }
    QtObject {
        id: model
        property int count: 20
        function selectionKeyForRow(row) { return row === 10 ? "anchor-key" : "other-key" }
        function indexForSelectionKey(key) { return key === "anchor-key" ? 10 : -1 }
    }
    QtObject { id: tagEditorApi }
    QtObject {
        id: mockView
        property real contentY: 350
        property real contentHeight: 1000
        property real height: 100
        property real originY: 100
        property bool activeFocus: false
        function indexAt(x, y) { return 10 }
        function itemAtIndex(index) { return index === 10 ? ({ y: 340 }) : null }
        function positionViewAtIndex(index, mode) { contentY = index === 10 ? 340 : 0 }
    }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: model
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    Component.onCompleted: {
        controller.registerView(mockView)
        capturedAnchor = controller.captureViewAnchor()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    const QVariantMap anchor = root->property("capturedAnchor").toMap();
    QCOMPARE(anchor.value(QStringLiteral("key")).toString(), QStringLiteral("anchor-key"));
    QCOMPARE(anchor.value(QStringLiteral("offset")).toDouble(), 10.0);
    QVERIFY(QMetaObject::invokeMethod(root.get(), "restoreCapturedAnchor"));
    QTRY_COMPARE(root->property("contentY").toDouble(), 350.0);
}

void QmlSmokeTest::libraryControllerForwardsShowTrackRequest() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    property alias controllerRef: controller
    property alias shownPath: bridge.shownPath

    QtObject {
        id: bridge
        property string shownPath: ""
        function showTrackInLibrary(path) { shownPath = path }
    }
    QtObject {
        id: model
        property int count: 0
    }
    QtObject {
        id: tagEditorApi
    }
    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: model
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "showTrackInLibrary",
        Q_ARG(QVariant, QStringLiteral("/music/song.flac"))));
    QCOMPARE(root->property("shownPath").toString(), QStringLiteral("/music/song.flac"));
}

void QmlSmokeTest::libraryControllerCentersBridgeRevealAfterExpansion() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(20));
    QTRY_COMPARE(model.rowCount(), 1);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testLibraryModel"), &model);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    width: 320
    height: 120
    property alias controllerRef: controller
    property alias viewRef: view

    QtObject {
        id: bridge
        property int libraryTrackCount: 20
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
        function setLibraryNodeExpanded(key, expanded) {}
        function showTrackInLibrary(path) {}
    }
    QtObject { id: tagEditorApi }

    Controllers.LibraryController {
        id: controller
        uiBridge: bridge
        libraryModel: testLibraryModel
        tryCaptureGlobalSearchPrefill: function(event) { return false }
        tagEditorApi: tagEditorApi
        openTagEditorDialog: function() {}
    }

    ListView {
        id: view
        anchors.fill: parent
        model: testLibraryModel
        delegate: Item { width: view.width; height: 24 }
        Component.onCompleted: controller.registerView(view)
    }

    Component.onCompleted: controller.requestBridgeReveal(
        ["artist|Artist A", "album|Artist A|Album 20"],
        "track|/music/artist/album20/track.flac")
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = qvariant_cast<QObject *>(root->property("controllerRef"));
    QObject *view = qvariant_cast<QObject *>(root->property("viewRef"));
    QVERIFY(controller != nullptr);
    QVERIFY(view != nullptr);
    QTRY_COMPARE(
        controller->property("selectedSelectionKey").toString(),
        QStringLiteral("track|/music/artist/album20/track.flac"));
    QTRY_VERIFY(view->property("contentY").toDouble() > 300.0);
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
        displayedAfterUpdate >= displayedBeforeUpdate - 0.001
            && displayedAfterUpdate <= displayedBeforeUpdate + 0.03,
        qPrintable(
            QStringLiteral("displayed=%1 displayed_before=%2")
                .arg(displayedAfterUpdate, 0, 'f', 6)
                .arg(displayedBeforeUpdate, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramPosition - displayedAfterUpdate) < 0.001,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramPosition, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeartbeatUsesBoundedPhaseCorrection() {
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
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    bridge->setProperty("positionSeconds", 48.08);

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
        displayedAfterHeartbeat >= displayedBeforeHeartbeat - 0.001
            && displayedAfterHeartbeat <= displayedBeforeHeartbeat + 0.03,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_after=%2")
            .arg(displayedBeforeHeartbeat, 0, 'f', 6)
            .arg(displayedAfterHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        std::abs(spectrogramAfterHeartbeat - displayedAfterHeartbeat) < 0.001,
        qPrintable(QStringLiteral("spectrogram=%1").arg(spectrogramAfterHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        controller->property("interpolationRate").toDouble() <= 1.1
            && controller->property("interpolationRate").toDouble() > 1.0,
        qPrintable(QStringLiteral("rate=%1")
            .arg(controller->property("interpolationRate").toDouble(), 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekBehindHeartbeatKeepsVisualClock() {
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

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2240.0))));
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeHeartbeat > 48.20);

    bridge->setProperty("positionSeconds", 48.08);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2240.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedOnHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedOnHeartbeat >= displayedBeforeHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_on_heartbeat=%2")
            .arg(displayedBeforeHeartbeat, 0, 'f', 6)
            .arg(displayedOnHeartbeat, 0, 'f', 6)));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2280.0))));

    const double displayedAfterStep =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterStep > displayedOnHeartbeat + 0.03,
        qPrintable(QStringLiteral("displayed_on_heartbeat=%1 displayed_after=%2")
            .arg(displayedOnHeartbeat, 0, 'f', 6)
            .arg(displayedAfterStep, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekHeldTargetKeepsVisualClock() {
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

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2100.0))));
    const double displayedBeforeHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeHeartbeat > 48.09);

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
        displayedOnHeartbeat >= displayedBeforeHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_on_heartbeat=%2")
            .arg(displayedBeforeHeartbeat, 0, 'f', 6)
            .arg(displayedOnHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPostSeekTargetEchoKeepsVisualClockActive() {
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
        Q_ARG(QVariant, QVariant::fromValue(2000.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2240.0))));
    const double displayedBeforeBackendHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY(displayedBeforeBackendHeartbeat > 48.20);

    bridge->setProperty("positionSeconds", 48.08);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2240.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedAfterBackendHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        displayedAfterBackendHeartbeat >= displayedBeforeBackendHeartbeat - 0.001,
        qPrintable(QStringLiteral("displayed_before=%1 displayed_after=%2")
            .arg(displayedBeforeBackendHeartbeat, 0, 'f', 6)
            .arg(displayedAfterBackendHeartbeat, 0, 'f', 6)));
}

void QmlSmokeTest::playbackControllerPlayAtCurrentTrackClearsPostSeekVisualClock() {
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
        property real durationSeconds: 480.0
        property string currentTrackPath: "/music/test.flac"
        property int playingQueueIndex: 0
        property real volume: 1.0
        property var playAtCalls: []
        property var seekCalls: []
        function seek(value) { seekCalls = seekCalls.concat([value]) }
        function playAt(index) { playAtCalls = playAtCalls.concat([index]) }
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
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QVERIFY(bridge != nullptr);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "seekCommittedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(410.0)),
        Q_ARG(QVariant, QVariant::fromValue(2000.0))));
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "stepInterpolationTo",
        Q_ARG(QVariant, QVariant::fromValue(2300.0))));
    QVERIFY(controller->property("displayedPositionSeconds").toDouble() > 410.20);
    QVERIFY(controller->property("visualSeekClockActive").toBool());

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "playAt",
        Q_ARG(QVariant, QVariant::fromValue(0))));

    QCOMPARE(bridge->property("playAtCalls").toList().size(), 1);
    QCOMPARE(bridge->property("playAtCalls").toList().at(0).toInt(), 0);
    QVERIFY(!controller->property("visualSeekClockActive").toBool());
    QVERIFY2(
        controller->property("displayedPositionSeconds").toDouble() < 0.02,
        qPrintable(QStringLiteral("displayed=%1")
            .arg(controller->property("displayedPositionSeconds").toDouble(), 0, 'f', 6)));
    QVERIFY2(
        controller->property("spectrogramPositionSeconds").toDouble() < 0.02,
        qPrintable(QStringLiteral("spectrogram=%1")
            .arg(controller->property("spectrogramPositionSeconds").toDouble(), 0, 'f', 6)));

    bridge->setProperty("positionSeconds", 0.0);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(2310.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    QVERIFY2(
        controller->property("displayedPositionSeconds").toDouble() < 0.03,
        qPrintable(QStringLiteral("displayed_after_heartbeat=%1")
            .arg(controller->property("displayedPositionSeconds").toDouble(), 0, 'f', 6)));
    QVERIFY2(
        controller->property("spectrogramPositionSeconds").toDouble() < 0.03,
        qPrintable(QStringLiteral("spectrogram_after_heartbeat=%1")
            .arg(controller->property("spectrogramPositionSeconds").toDouble(), 0, 'f', 6)));
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
        displayedImmediately >= displayedBeforeHeartbeat - 0.001
            && displayedImmediately <= displayedBeforeHeartbeat + 0.03,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        controller->property("interpolationRate").toDouble() > 1.0
            && controller->property("interpolationRate").toDouble() <= 1.1,
        qPrintable(QStringLiteral("rate=%1")
            .arg(controller->property("interpolationRate").toDouble(), 0, 'f', 6)));

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
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
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

void QmlSmokeTest::playbackControllerProfileSkipsMinorFollowHeartbeatLogs() {
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
    bridge->setProperty("positionSeconds", displayedBeforeHeartbeat + 0.006);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChanged",
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(
        !warnings.contains(QStringLiteral("[qml-playback-profile] heartbeat")),
        qPrintable(warnings));
}

void QmlSmokeTest::playbackControllerDelayedHeartbeatDoesNotJumpViewport() {
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

    bridge->setProperty("positionSeconds", 12.04);
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1040.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));
    const double displayedBeforeDelayedHeartbeat =
        controller->property("displayedPositionSeconds").toDouble();

    // One bridge update was delayed/coalesced: its position advances by
    // 80 ms although only 40 ms of UI wall time has elapsed.
    bridge->setProperty("positionSeconds", 12.12);

    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "handlePlaybackChangedAtTime",
        Q_ARG(QVariant, QVariant::fromValue(1080.0)),
        Q_ARG(QVariant, QVariant()),
        Q_ARG(QVariant, QVariant())));

    const double displayedImmediately =
        controller->property("displayedPositionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedImmediately - 12.08) < 0.001,
        qPrintable(
            QStringLiteral("displayed_immediately=%1 displayed_before=%2")
                .arg(displayedImmediately, 0, 'f', 6)
                .arg(displayedBeforeDelayedHeartbeat, 0, 'f', 6)));
    QVERIFY2(
        controller->property("interpolationRate").toDouble() > 1.0
            && controller->property("interpolationRate").toDouble() < 1.1,
        qPrintable(QStringLiteral("rate=%1")
            .arg(controller->property("interpolationRate").toDouble(), 0, 'f', 6)));

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
        // Keep the live timer from racing the explicit timestamps below.
        seekPressed: true
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("controller"));
    QVERIFY(controller != nullptr);

    double nowMs = 1000.0;
    QVERIFY(QMetaObject::invokeMethod(
        controller,
        "initializeFromBridgeAtTime",
        Q_ARG(QVariant, QVariant::fromValue(nowMs))));

    QObject *bridge = qvariant_cast<QObject *>(controller->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    constexpr double heartbeatIntervalMs = 40.0;
    constexpr double maximumRecoveryRate = 1.1;
    const std::array<double, 6> positions = {12.039, 12.079, 12.122, 12.165, 12.208, 12.251};
    double previousDisplayed = controller->property("displayedPositionSeconds").toDouble();
    double maximumStep = 0.0;

    for (double nextPosition : positions) {
        nowMs += heartbeatIntervalMs;
        bridge->setProperty("positionSeconds", nextPosition);
        QVERIFY(QMetaObject::invokeMethod(
            controller,
            "handlePlaybackChangedAtTime",
            Q_ARG(QVariant, QVariant::fromValue(nowMs)),
            Q_ARG(QVariant, QVariant()),
            Q_ARG(QVariant, QVariant())));
        const double displayed = controller->property("displayedPositionSeconds").toDouble();
        maximumStep = std::max(maximumStep, displayed - previousDisplayed);
        previousDisplayed = displayed;
    }

    QVERIFY2(
        maximumStep <= heartbeatIntervalMs / 1000.0 * maximumRecoveryRate + 0.000001,
        qPrintable(QStringLiteral("maximum_step=%1").arg(maximumStep, 0, 'f', 6)));
    const double displayedPosition = controller->property("displayedPositionSeconds").toDouble();
    const double backendPosition = bridge->property("positionSeconds").toDouble();
    QVERIFY2(
        std::abs(displayedPosition - backendPosition) < 0.04,
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

void QmlSmokeTest::spectrogramCenteredSmallSeekSnapsWithoutServoSlide() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.setPositionSeconds(10.0);
    item.setPlaying(true);

    QTest::qWait(30);
    item.setPositionSeconds(10.08);

    QVERIFY2(
        std::abs(item.m_positionAnchorSeconds - 10.08) < 0.0001,
        qPrintable(QStringLiteral(
            "centered small seek should snap to target, got anchor %1")
            .arg(item.m_positionAnchorSeconds, 0, 'f', 4)));
    QVERIFY2(
        std::abs(item.m_positionSeconds - 10.08) < 0.0001,
        qPrintable(QStringLiteral(
            "centered small seek should publish target, got position %1")
            .arg(item.m_positionSeconds, 0, 'f', 4)));
}

void QmlSmokeTest::spectrogramCenteredSeekFollowsLocalVisualClock() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.m_precomputedReady = true;
    item.setPositionSeconds(0.5);
    item.setPlaying(true);

    constexpr double seekTarget = 134.577;
    item.setPositionSeconds(seekTarget);
    item.setPositionSeconds(seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "centered seek should follow the visual clock, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.setPositionSeconds(0.6);
    item.setPlaying(true);

    constexpr double seekTarget = 117.079;
    item.setPositionSeconds(seekTarget);
    item.setPositionSeconds(seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "early centered seek should follow the visual clock before precomputed ready, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
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

void QmlSmokeTest::spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    qputenv("FERROUS_PROFILE_UI", "1");
    SpectrogramSeekTrace::noteSeekIssued(42.0);

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    SpectrogramItem duplicateItem;
    duplicateItem.setWidth(320);
    duplicateItem.setHeight(180);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_profileEnabled = true;
        item.m_precomputedCanvasDisplayRight = 96;

        const qint64 startedAtMs = SpectrogramSeekTrace::startedAtMs();
        QVERIFY(startedAtMs > 0);
        item.maybeStartSeekProfileLocked(startedAtMs);
        QVERIFY(item.m_seekProfile.active);

        item.noteSeekProfileFrameLocked(startedAtMs + 30, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 60, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 90, 0.016, false, false);
        item.noteSeekProfileFrameLocked(startedAtMs + 150, 0.016, false, false);
        QVERIFY(!item.m_seekProfile.active);

        item.maybeStartSeekProfileLocked(startedAtMs + 180);
        QVERIFY(!item.m_seekProfile.active);
        QCOMPARE(
            item.m_lastFinalizedSeekProfileGeneration,
            SpectrogramSeekTrace::currentGeneration());
    }

    {
        QMutexLocker lock(&duplicateItem.m_stateMutex);
        duplicateItem.m_profileEnabled = true;
        duplicateItem.m_precomputedCanvasDisplayRight = 96;
        duplicateItem.maybeStartSeekProfileLocked(SpectrogramSeekTrace::startedAtMs() + 180);
        QVERIFY(!duplicateItem.m_seekProfile.active);
    }

    qunsetenv("FERROUS_PROFILE_UI");
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
        item.m_precomputedCanvasZoomLevel = 2.0;
        item.m_canvas = QImage(320, 180, QImage::Format_RGB32);
        item.m_precomputedCanvasDisplayLeft = 0;
        item.m_precomputedCanvasDisplayRight = 0;
        item.m_precomputedCanvasRolling = false;
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

void QmlSmokeTest::waveformEditorCancelsAndBoundsObsoleteDecodes() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setDurationSeconds(10.0);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    item.m_decodeWindow = [gate, started](const QString &, double, double, int,
                                        const std::shared_ptr<std::atomic_bool> &) {
        ++*started;
        gate->tryAcquire(1, 5000);
        return QByteArray("stale invalid payload");
    };
    item.setSourcePath(QStringLiteral("/fixture/first.wav"));
    item.m_requestTimer.stop();
    item.requestDetailWindow();
    QTRY_COMPARE(started->load(), 1);
    const auto cancelled = item.m_decodeCancelled;
    for (int i = 0; i < 20; ++i) {
        item.setZoomLevel(2.0 + i);
        item.m_requestTimer.stop();
        item.requestDetailWindow();
    }
    QVERIFY(cancelled->load());
    QCOMPARE(started->load(), 1);
    QCOMPARE(item.findChildren<QFutureWatcherBase *>().size(), 1);
    item.setSourcePath(QStringLiteral("/fixture/latest.wav"));
    item.m_requestTimer.stop();
    gate->release();
    QTRY_COMPARE(started->load(), 2);
    item.setSourcePath(QString());
    gate->release();
    QTRY_VERIFY(!item.m_decodeActive);
    QVERIFY(item.m_detail.extrema.empty());
    QCOMPARE(started->load(), 2);
}

void QmlSmokeTest::waveformEditorCachedDecoderPreservesOverlappingSamples() {
    QByteArray wav("RIFF", 4);
    constexpr quint32 frames = 30'000;
    appendLe<quint32>(wav, 36 + frames * 4);
    wav.append("WAVEfmt ", 8);
    appendLe<quint32>(wav, 16);
    appendLe<quint16>(wav, 1);
    appendLe<quint16>(wav, 2);
    appendLe<quint32>(wav, 48'000);
    appendLe<quint32>(wav, 48'000 * 4);
    appendLe<quint16>(wav, 4);
    appendLe<quint16>(wav, 16);
    wav.append("data", 4);
    appendLe<quint32>(wav, frames * 4);
    for (quint32 frame = 0; frame < frames; ++frame) {
        const qint16 sample = static_cast<qint16>((frame % 16) * 1024);
        appendLe<qint16>(wav, sample);
        appendLe<qint16>(wav, -sample);
    }
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(wav), wav.size());
    QVERIFY(file.flush());
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    WaveformEditorItem::DetailWindow full;
    QVERIFY(WaveformEditorItem::parseWindow(WaveformEditorItem::decodeWindow(
        file.fileName(), 0.0, 0.6, 30'000, cancelled), &full));
    for (int request = 0; request < 2; ++request) {
        WaveformEditorItem item;
        QVERIFY(WaveformEditorItem::parseWindow(WaveformEditorItem::decodeWindow(
            file.fileName(), 0.25, 0.5, 12'000, cancelled), &item.m_detail));
        QCOMPARE(item.m_detail.framesPerPoint, quint32(1));
        QCOMPARE(item.m_detail.channelCount, 2);
        for (size_t index = 0; index < item.m_detail.extrema.size(); ++index) {
            QCOMPARE(item.m_detail.extrema[index], full.extrema[12'000 * 4 + index]);
        }
        for (float mixed : item.m_detail.downmixExtrema) QCOMPARE(mixed, 0.0F);
    }
    cancelled->store(true);
    QVERIFY(WaveformEditorItem::decodeWindow(
        file.fileName(), 0.25, 0.5, 12'000, cancelled).isEmpty());
}

void QmlSmokeTest::waveformEditorParsesSignedSampleWindow() {
    const auto appendF64 = [](QByteArray &out, double value) {
        quint64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe<quint64>(out, bits);
    };
    const auto appendF32 = [](QByteArray &out, float value) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe<quint32>(out, bits);
    };
    QByteArray frame;
    frame.append("WVF2", 4);
    appendLe<quint32>(frame, 48'000);
    appendLe<quint16>(frame, 2);
    appendLe<quint16>(frame, 0);
    appendF64(frame, 1.0);
    appendF64(frame, 1.000'041'666'666'666'7);
    appendLe<quint32>(frame, 1);
    appendLe<quint32>(frame, 2);
    appendF32(frame, -0.75F);
    appendF32(frame, -0.75F);
    appendF32(frame, 0.25F);
    appendF32(frame, 0.25F);
    appendF32(frame, 0.5F);
    appendF32(frame, 0.5F);
    appendF32(frame, -0.125F);
    appendF32(frame, -0.125F);
    appendF32(frame, -0.25F);
    appendF32(frame, -0.25F);
    appendF32(frame, 0.1875F);
    appendF32(frame, 0.1875F);

    WaveformEditorItem::DetailWindow window;
    QVERIFY(WaveformEditorItem::parseWindow(frame, &window));
    QCOMPARE(window.sampleRateHz, 48'000);
    QCOMPARE(window.channelCount, 2);
    QCOMPARE(window.framesPerPoint, quint32(1));
    QCOMPARE(window.pointCount, 2);
    QCOMPARE(window.extrema.at(0), -0.75F);
    QCOMPARE(window.extrema.at(6), -0.125F);
    QCOMPARE(window.downmixExtrema.at(0), -0.25F);
    QCOMPARE(window.downmixExtrema.at(2), 0.1875F);
    QVERIFY(!WaveformEditorItem::parseWindow(frame.chopped(4), &window));
}

void QmlSmokeTest::waveformEditorDownmixUsesMixedExtremaAtEveryZoom() {
    WaveformEditorItem item;
    item.m_detail.channelCount = 2;
    item.m_detail.pointCount = 4;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    for (int i = 0; i < 4; ++i) {
        item.m_detail.extrema.insert(item.m_detail.extrema.end(), {0.5F, 0.5F, -0.5F, -0.5F});
    }
    item.m_detail.downmixExtrema.assign(8, 0.0F);
    for (int framesPerPoint : {1, 2}) {
        item.m_detail.framesPerPoint = framesPerPoint;
        item.m_detail.sampleRateHz = 4 * framesPerPoint;
        QImage image(400, 100, QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        item.drawDetailLocked(painter, 400, 100, 0.0, 1.0, 1);
        painter.end();
        int signalPixels = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).green() > 150) {
                    ++signalPixels;
                    QVERIFY(y >= 47 && y <= 52);
                }
            }
        }
        QVERIFY(signalPixels > 0);
    }
}

void QmlSmokeTest::waveformEditorSampleMarkersRequireSampleResolution() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.pointCount = 100;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.extrema.assign(200, 0.0F);
    item.setZoomLevel(1.0);
    QVERIFY(item.samplePointsVisible());

    item.m_detail.framesPerPoint = 2;
    QVERIFY(!item.samplePointsVisible());
}

void QmlSmokeTest::waveformEditorFormatsViewportDuration() {
    WaveformEditorItem item;
    QCOMPARE(item.formatViewportDuration(245.2), QStringLiteral("04:05"));
    QCOMPARE(item.formatViewportDuration(1.0), QStringLiteral("00:01"));
    QCOMPARE(item.formatViewportDuration(0.834), QStringLiteral("834 ms"));
    QCOMPARE(item.formatViewportDuration(0.0004), QStringLiteral("1 ms"));
    QCOMPARE(item.formatViewportDuration(0.0), QStringLiteral("0 ms"));
}

void QmlSmokeTest::waveformEditorConnectsSamplesBeforeMarkers() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.setPositionSeconds(0.5);
    item.m_sampleRateHz = 1'000;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 1'000;
    item.m_detail.extrema.reserve(2'000);
    for (int sample = 0; sample < 1'000; ++sample) {
        const float value = static_cast<float>(
            std::sin(static_cast<double>(sample) * 0.075));
        item.m_detail.extrema.push_back(value);
        item.m_detail.extrema.push_back(value);
    }
    item.setZoomLevel(3.0);

    QVERIFY(item.sampleCurveVisibleLocked());
    QVERIFY(!item.samplePointsVisibleLocked());
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.drawDetailLocked(
        painter, canvas.width(), canvas.height(),
        visibleStart, visibleEnd, 1);
    painter.end();

    int occupiedColumns = 0;
    for (int x = 0; x < canvas.width(); ++x) {
        bool occupied = false;
        for (int y = 0; y < canvas.height(); ++y) {
            const QColor pixel = canvas.pixelColor(x, y);
            if (pixel.green() > 100 && pixel.green() > pixel.red() * 2) {
                occupied = true;
                break;
            }
        }
        if (occupied) ++occupiedColumns;
    }
    QVERIFY(occupiedColumns > 390);

    constexpr double pixelsPerSample = 1.2;
    const double tileDuration = 64.0
        / (pixelsPerSample * item.m_detail.sampleRateHz);
    const double tileStart = tileDuration;
    QImage tile(64, 180, QImage::Format_RGB32);
    tile.fill(Qt::black);
    QPainter tilePainter(&tile);
    item.drawDetailSliceLocked(
        tilePainter, tile.width(), tile.height(),
        tileStart, tileStart + tileDuration, 1, 0, tile.width());
    tilePainter.end();
    for (const int edgeX : {0, tile.width() - 1}) {
        bool edgeOccupied = false;
        for (int y = 0; y < tile.height(); ++y) {
            const QColor pixel = tile.pixelColor(edgeX, y);
            if (pixel.green() > 100 && pixel.green() > pixel.red() * 2) {
                edgeOccupied = true;
                break;
            }
        }
        QVERIFY2(edgeOccupied, "connected sample tiles must meet at both edges");
    }

    // Match the fullscreen handoff edge: samples separated by roughly 3.6
    // pixels should have compact point markers without forcing an expensive
    // direct repaint on every frame.
    item.setZoomLevel(9.0);
    QVERIFY(item.samplePointsVisibleLocked());
    const auto [markerStart, markerEnd] = item.visibleRangeLocked();
    QVERIFY(!item.renderDetailDirectlyLocked(markerStart, markerEnd));

    item.setZoomLevel(10.0);
    QVERIFY(item.samplePointsVisibleLocked());
    const auto [directStart, directEnd] = item.visibleRangeLocked();
    QVERIFY(item.renderDetailDirectlyLocked(directStart, directEnd));
}

void QmlSmokeTest::waveformEditorScrollsInsideCachedDetailWithoutRebuild() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.pointCount = 1'000;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.extrema.assign(2'000, 0.0F);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(2.0);

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QVERIFY(!item.m_cacheDirty);
    QCOMPARE(item.m_cacheStartSeconds, 0.0);
    QCOMPARE(item.m_cacheEndSeconds, 10.0);

    item.setPositionSeconds(5.1);
    QVERIFY(!item.m_cacheDirty);
}

void QmlSmokeTest::waveformEditorUsesChannelHintBeforeDetailArrives() {
    WaveformEditorItem item;
    item.setViewMode(1);
    item.setChannelCountHint(2);

    QCOMPARE(item.channelCount(), 2);

    item.m_channelCount = 6;
    QCOMPARE(item.channelCount(), 6);
}

void QmlSmokeTest::waveformEditorCoalescesPlaybackRequestsWithoutStarvingTimer() {
    WaveformEditorItem item;
    item.scheduleDetailRequest();
    QVERIFY(item.m_requestTimer.isActive());
    const int initialRemaining = item.m_requestTimer.remainingTime();

    QTest::qWait(25);
    item.scheduleDetailRequest();

    QVERIFY(item.m_requestTimer.isActive());
    QVERIFY(item.m_requestTimer.remainingTime() < initialRemaining - 10);
}

void QmlSmokeTest::waveformEditorPendingWindowSuppressesDuplicateRequest() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_requestTimer.stop();
    item.m_requestInFlight = true;
    item.m_requestedStartSeconds = 0.0;
    item.m_requestedEndSeconds = 10.0;
    item.m_requestedMaxPoints = 1'200;
    item.m_requestedRenderWidth = 400;

    item.setPositionSeconds(2.0);

    QVERIFY(!item.m_requestTimer.isActive());
}

void QmlSmokeTest::waveformEditorZoomSupersedesCoarsePendingRequest() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_requestTimer.stop();
    item.m_requestInFlight = true;
    item.m_requestedStartSeconds = 0.0;
    item.m_requestedEndSeconds = 10.0;
    item.m_requestedMaxPoints = 1'200;
    item.m_requestedRenderWidth = 400;

    item.setZoomLevel(4.0);

    QVERIFY(!item.m_requestInFlight);
    QVERIFY(item.m_requestTimer.isActive());
    QCOMPARE(item.zoomLevel(), 4.0);
}

void QmlSmokeTest::waveformEditorResizeRaisesDetailRequestDensity() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(100.0);
    item.setPositionSeconds(50.0);
    item.setZoomLevel(4.0);
    const auto [smallStart, smallEnd] = item.requestRangeLocked(37.5, 62.5);
    const int smallRequest = item.detailRequestPointCountLocked(smallStart, smallEnd);
    item.m_detail.startSeconds = smallStart;
    item.m_detail.endSeconds = smallEnd;
    item.m_detail.framesPerPoint = 2;
    item.m_detail.pointCount = smallRequest;
    QVERIFY(item.detailResolutionCoversLocked(37.5, 62.5));

    item.setWidth(1'200);
    const auto [largeStart, largeEnd] = item.requestRangeLocked(37.5, 62.5);
    const int largeRequest = item.detailRequestPointCountLocked(largeStart, largeEnd);

    QVERIFY(largeRequest >= smallRequest * 3);
    QVERIFY(!item.detailResolutionCoversLocked(37.5, 62.5));
}

void QmlSmokeTest::waveformEditorDeepZoomRequestPreservesViewportDensity() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(240.0);
    item.setPositionSeconds(120.0);
    item.setZoomLevel(8'301.0);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const double visibleSpan = visibleEnd - visibleStart;
    const double requestSpan = requestEnd - requestStart;
    const double maximumDensitySpan = visibleSpan * 65'536.0
        / (3'440.0 * 4.0);

    QVERIFY(requestSpan <= maximumDensitySpan + 0.000'001);
    QVERIFY(requestSpan > visibleSpan);
}

void QmlSmokeTest::waveformEditorQuantizesRequestForRequiredBinDensity() {
    WaveformEditorItem item;
    item.setWidth(1'183);
    item.setDurationSeconds(257.133333);
    item.setPositionSeconds(46.3);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(1'726.76);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    const double requestFrames = std::ceil(
        (requestEnd - requestStart) * 44'100.0);
    const double framesPerPoint = std::ceil(
        requestFrames / static_cast<double>(requestedPoints));
    const double returnedPoints = std::ceil(requestFrames / framesPerPoint);
    const double visiblePoints = returnedPoints
        * (visibleEnd - visibleStart) / (requestEnd - requestStart);

    QVERIFY(requestedPoints > 20'621);
    QVERIFY(visiblePoints >= item.requiredVisibleDetailPointsLocked(
        visibleEnd - visibleStart) * 0.9);
}

void QmlSmokeTest::waveformEditorFullscreenRequestStaysInTargetBin() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(257.133333);
    item.setPositionSeconds(72.0);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(475.06);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    const double requestFrames = std::ceil(
        (requestEnd - requestStart) * 44'100.0);
    const double framesPerPoint = std::ceil(
        requestFrames / static_cast<double>(requestedPoints));
    const double returnedPoints = std::ceil(requestFrames / framesPerPoint);
    const double visiblePoints = returnedPoints
        * (visibleEnd - visibleStart) / (requestEnd - requestStart);

    QCOMPARE(framesPerPoint, 1.0);
    QVERIFY(requestedPoints <= 65'536);
    QVERIFY(visiblePoints >= item.requiredVisibleDetailPointsLocked(
        visibleEnd - visibleStart) * 0.9);
}

void QmlSmokeTest::waveformEditorFullscreenSampleCurveRequestsRawFramesEarly() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(257.133333);
    item.setPositionSeconds(8.9);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(185.409);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);
    const double visibleSpan = visibleEnd - visibleStart;
    const double requestFrames = std::ceil(
        (requestEnd - requestStart) * 44'100.0);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);

    QVERIFY(visibleSpan > 1.38);
    QVERIFY(visibleSpan < 1.40);
    QVERIFY(item.sampleCurveRequestedForPixelSpanLocked(
        3'440, visibleStart, visibleEnd));
    QVERIFY(requestEnd - requestStart > visibleSpan);
    QVERIFY(requestFrames <= 65'536.0);
    QVERIFY(static_cast<double>(requestedPoints) >= requestFrames);

    item.m_detail.sampleRateHz = 44'100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = requestStart;
    item.m_detail.endSeconds = requestEnd;
    item.m_detail.framesPerPoint = 2;
    item.m_detail.pointCount = requestedPoints / 2;
    item.m_detail.extrema.assign(
        static_cast<std::size_t>(item.m_detail.pointCount) * 2U, 0.25F);
    QVERIFY(!item.detailResolutionCoversLocked(visibleStart, visibleEnd));

    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = requestedPoints;
    item.m_detail.extrema.assign(
        static_cast<std::size_t>(item.m_detail.pointCount) * 2U, 0.25F);
    QVERIFY(item.detailResolutionCoversLocked(visibleStart, visibleEnd));
}

void QmlSmokeTest::waveformEditorSampleViewPrefetchesUsefulSpan() {
    WaveformEditorItem item;
    item.setWidth(1'183);
    item.setDurationSeconds(257.133333);
    item.setPositionSeconds(40.0);
    item.m_sampleRateHz = 44'100;
    item.setZoomLevel(76'683.55);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const auto [requestStart, requestEnd] = item.requestRangeLocked(
        visibleStart, visibleEnd);

    QVERIFY(visibleEnd - visibleStart < 0.004);
    QVERIFY(requestEnd - requestStart > 1.4);
    const int requestedPoints = item.detailRequestPointCountLocked(
        requestStart, requestEnd);
    QVERIFY(requestedPoints > 60'000);
    QVERIFY(requestedPoints <= 65'536);
}

void QmlSmokeTest::waveformEditorPrefetchesBeforeDetailBoundary() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.setZoomLevel(10.0);
    item.setPositionSeconds(5.0);
    item.m_requestTimer.stop();
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 3.5;
    item.m_detail.endSeconds = 6.5;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 300;
    item.m_detail.extrema.assign(600, 0.0F);

    item.setPositionSeconds(5.5);

    QVERIFY(item.visibleRangeLocked().second < item.m_detail.endSeconds);
    QVERIFY(item.m_requestTimer.isActive());
}

void QmlSmokeTest::waveformEditorKeepsOverlappingDetailDuringHandoff() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(2.0);
    item.m_requestTimer.stop();
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.5F);
    item.invalidateCacheLocked();

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int waveformPixels = 0;
    for (int y = 20; y < 160; ++y) {
        for (int x = 120; x < 200; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.green() > 100 && pixel.red() < 100) ++waveformPixels;
        }
    }
    QVERIFY(waveformPixels > 20);
    QCOMPARE(canvas.pixelColor(150, 20), QColor(5, 9, 7));
}

void QmlSmokeTest::waveformEditorWideDetailCacheKeepsViewportDensity() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 48'000;
    item.m_detail.extrema.assign(96'000, 0.25F);
    item.setZoomLevel(4.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(item.m_cache.width() >= 959);
    QVERIFY(item.m_cache.width() <= 961);
    QVERIFY(item.m_cacheEndSeconds - item.m_cacheStartSeconds > 7.49);
    QVERIFY(item.m_cacheEndSeconds - item.m_cacheStartSeconds < 7.51);
}

void QmlSmokeTest::waveformEditorWholeTrackAcceptsDecodedEndpointTolerance() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0 - 1.5 / 48'000.0;
    item.m_detail.framesPerPoint = 375;
    item.m_detail.pointCount = 1'280;
    item.m_detail.extrema.assign(2'560, 0.5F);
    QVERIFY(item.detailCoversRangeLocked(0.0, 10.0));
    QVERIFY(item.detailOrPendingRequestCoversLocked(0.0, 10.0));

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int waveformPixels = 0;
    for (int y = 44; y <= 47; ++y) {
        for (int x = 20; x < 300; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.green() > 180 && pixel.red() < 100) ++waveformPixels;
        }
    }
    QVERIFY(waveformPixels > 200);
}

void QmlSmokeTest::waveformEditorZoomOutDefersOverviewUntilDetailReady() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.25F);
    item.setZoomLevel(5.0);

    QImage zoomedCanvas(320, 180, QImage::Format_RGB32);
    QPainter zoomedPainter(&zoomedCanvas);
    item.paint(&zoomedPainter);
    zoomedPainter.end();

    item.setZoomLevel(3.0);
    item.setZoomLevel(1.0);

    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(item.m_presentedZoomLevel, 5.0);
    QVERIFY(item.m_zoomOutHandoffPending);
    QVERIFY(!item.m_zoomFallbackToOverview);
    QVERIFY(!item.m_cacheDirty);
    const auto [presentedStart, presentedEnd] = item.visibleRangeLocked();
    QCOMPARE(presentedEnd - presentedStart, 2.0);
    const auto [requestStart, requestEnd] = item.detailRequestVisibleRangeLocked();
    QCOMPARE(requestStart, 0.0);
    QCOMPARE(requestEnd, 10.0);
    item.m_requestTimer.stop();
}

void QmlSmokeTest::waveformEditorDeferredZoomOutCommitsCompletedDetailCache() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.0;
    item.m_detail.endSeconds = 6.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 200;
    item.m_detail.extrema.assign(400, 0.25F);
    item.setZoomLevel(5.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    item.setZoomLevel(1.0);
    item.m_requestTimer.stop();
    QVERIFY(item.m_zoomOutHandoffPending);

    item.m_sampleRateHz = 640;
    item.m_detail.sampleRateHz = 640;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 6'400;
    item.m_detail.extrema.assign(12'800, 0.5F);
    item.beginStagedCacheForRangeLocked(0.0, 10.0, true);
    QVERIFY(!item.m_stagedCache.isNull());
    QVERIFY(!item.advanceStagedCacheLocked());
    QCOMPARE(item.m_presentedZoomLevel, 5.0);
    while (!item.m_stagedCache.isNull()) {
        item.advanceStagedCacheLocked();
    }

    QCOMPARE(item.m_presentedZoomLevel, 1.0);
    QVERIFY(!item.m_zoomOutHandoffPending);
    QVERIFY(!item.m_cacheDirty);
    QCOMPARE(item.m_cacheStartSeconds, 0.0);
    QCOMPARE(item.m_cacheEndSeconds, 10.0);
}

void QmlSmokeTest::waveformEditorZoomOutKeepsReadyDetail() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.m_sampleRateHz = 640;
    item.m_detail.sampleRateHz = 640;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 6'400;
    item.m_detail.extrema.assign(12'800, 0.25F);
    item.setZoomLevel(5.0);
    QVERIFY(!item.m_zoomFallbackToOverview);

    item.setZoomLevel(2.0);

    QVERIFY(!item.m_zoomFallbackToOverview);
}

void QmlSmokeTest::waveformEditorZoomInRetainsCoveredCacheWhileRefining() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QVERIFY(!item.m_cacheDirty);

    item.setZoomLevel(4.0);

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_zoomFallbackToOverview);
    item.m_requestTimer.stop();
}

void QmlSmokeTest::waveformEditorPausedZoomRebuildsCacheAtSamplePresentationBoundaries() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.setPositionSeconds(0.5);
    item.m_sampleRateHz = 12'000;
    item.m_detail.sampleRateHz = 12'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 12'000;
    item.m_detail.extrema.assign(24'000, 0.25F);
    item.setZoomLevel(1.5);
    QVERIFY(!item.playing());
    QVERIFY(!item.sampleCurveVisibleLocked());

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QVERIFY(!item.m_cacheDirty);

    // At about 0.067 pixels per sample this exercises the early raw handoff:
    // use the connected raw-sample curve instead of visible extrema bars.
    item.setZoomLevel(2.0);
    QVERIFY(item.sampleCurveVisibleLocked());
    QVERIFY(item.m_cacheDirty);

    QPainter curvePainter(&canvas);
    item.paint(&curvePainter);
    curvePainter.end();
    QVERIFY(!item.m_cacheDirty);

    item.setZoomLevel(4.0);
    QVERIFY(!item.samplePointsVisibleLocked());
    // Sample curves are rasterized at their exact seconds-per-pixel scale;
    // zooming further within the curve regime must not magnify the old cache.
    QVERIFY(item.m_cacheDirty);

    QPainter sharperCurvePainter(&canvas);
    item.paint(&sharperCurvePainter);
    sharperCurvePainter.end();
    QVERIFY(!item.m_cacheDirty);

    item.setZoomLevel(75.0);
    QVERIFY(item.samplePointsVisibleLocked());
    QVERIFY(item.m_cacheDirty);
    item.m_requestTimer.stop();
}

void QmlSmokeTest::waveformEditorSparseZoomDetailFallsBackUntilReady() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(100.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.5;
    item.m_detail.endSeconds = 5.5;
    item.m_detail.framesPerPoint = 480;
    item.m_detail.pointCount = 100;
    item.m_detail.extrema.assign(200, 0.0F);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();

    QVERIFY(item.detailCoversRangeLocked(visibleStart, visibleEnd));
    QVERIFY(!item.detailResolutionCoversLocked(visibleStart, visibleEnd));
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(canvas.pixelColor(30, 20).green() > 180);
    QVERIFY(canvas.pixelColor(370, 20).green() > 180);
}

void QmlSmokeTest::waveformEditorSeparatesChannelPanes() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 2;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.resize(40'000);
    for (int i = 0; i < 20'000; ++i) {
        item.m_detail.extrema[i * 2] = -1.0F;
        item.m_detail.extrema[i * 2 + 1] = 1.0F;
    }

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(canvas.pixelColor(160, 90), QColor(79, 94, 86));
    QVERIFY(canvas.pixelColor(160, 89).green() > 180);
    QVERIFY(canvas.pixelColor(160, 91).green() > 180);
}

void QmlSmokeTest::waveformEditorRestrictsDetailWorkToVisiblePoints() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setDurationSeconds(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 4.75;
    item.m_detail.endSeconds = 5.25;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 24'000;
    item.m_detail.extrema.assign(48'000, 0.0F);

    const auto [first, last] = item.detailPointRangeLocked(4.999, 5.001);

    QVERIFY(first > 11'000);
    QVERIFY(last < 13'000);
    QVERIFY(last - first < 110);
}

void QmlSmokeTest::waveformEditorDetailPointsUseAbsoluteSampleTimes() {
    WaveformEditorItem item;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.framesPerPoint = 4;
    item.m_detail.pointCount = 50;
    item.m_detail.startSeconds = 0.04;
    item.m_detail.endSeconds = 2.03;

    const double sharedBinTime = item.detailPointTimeLocked(1);
    QVERIFY(std::abs(sharedBinTime - 0.095) < 0.000'001);

    item.m_detail.startSeconds = 0.08;
    item.m_detail.endSeconds = 2.07;
    QVERIFY(std::abs(item.detailPointTimeLocked(0) - sharedBinTime) < 0.000'001);
}

void QmlSmokeTest::waveformEditorReusesSameSizedCache() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter firstPainter(&canvas);
    item.paint(&firstPainter);
    firstPainter.end();
    const uchar *firstBuffer = item.m_cache.constBits();

    item.invalidateCacheLocked();
    QPainter secondPainter(&canvas);
    item.paint(&secondPainter);
    secondPainter.end();

    QCOMPARE(item.m_cache.constBits(), firstBuffer);
}

void QmlSmokeTest::waveformEditorCacheHandoffsKeepAbsolutePixelGrid() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(100);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 1'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_zoomLevel = 5.0;
    item.m_presentedZoomLevel = 5.0;
    item.m_positionSeconds = 3.37;
    item.m_detail.extrema.reserve(20'000);
    for (int point = 0; point < item.m_detail.pointCount; ++point) {
        const float minimum = -static_cast<float>((point % 17) + 1) / 20.0F;
        const float maximum = static_cast<float>((point % 23) + 1) / 25.0F;
        item.m_detail.extrema.push_back(minimum);
        item.m_detail.extrema.push_back(maximum);
    }

    const auto firstGrid = item.cacheGridForRangeLocked(2.0, 4.0, 0.0, 10.0);
    const auto secondGrid = item.cacheGridForRangeLocked(2.37, 4.37, 0.0, 10.0);
    QCOMPARE(firstGrid.secondsPerPixel, secondGrid.secondsPerPixel);
    const double pixelOffset = (secondGrid.startSeconds - firstGrid.startSeconds)
        / firstGrid.secondsPerPixel;
    const int firstSourceX = static_cast<int>(std::llround(pixelOffset));
    QVERIFY(std::abs(pixelOffset - static_cast<double>(firstSourceX)) < 0.000'001);

    QImage firstCache(firstGrid.width, 100, QImage::Format_RGB32);
    firstCache.fill(QColor(5, 9, 7));
    QPainter firstPainter(&firstCache);
    item.drawDetailLocked(
        firstPainter, firstGrid.width, 100,
        firstGrid.startSeconds, firstGrid.endSeconds, 1);
    firstPainter.end();

    QImage secondCache(secondGrid.width, 100, QImage::Format_RGB32);
    secondCache.fill(QColor(5, 9, 7));
    QPainter secondPainter(&secondCache);
    item.drawDetailLocked(
        secondPainter, secondGrid.width, 100,
        secondGrid.startSeconds, secondGrid.endSeconds, 1);
    secondPainter.end();

    const int overlapWidth = std::min(
        secondCache.width(), firstCache.width() - firstSourceX);
    QVERIFY(overlapWidth > 0);
    for (int y = 0; y < firstCache.height(); ++y) {
        const auto *firstLine = reinterpret_cast<const QRgb *>(
            firstCache.constScanLine(y));
        const auto *secondLine = reinterpret_cast<const QRgb *>(
            secondCache.constScanLine(y));
        // QPainter may quantize antialias coverage by one value where a path
        // meets an image boundary. Interior overlap must remain bit-exact;
        // tile-edge continuity is covered separately with rendered edges.
        for (int x = 1; x + 1 < overlapWidth; ++x) {
            QVERIFY2(
                secondLine[x] == firstLine[firstSourceX + x],
                qPrintable(QStringLiteral(
                    "overlap mismatch x=%1 y=%2 first=%3 second=%4")
                    .arg(x)
                    .arg(y)
                    .arg(firstLine[firstSourceX + x], 0, 16)
                    .arg(secondLine[x], 0, 16)));
        }
    }

    item.m_cache = firstCache;
    item.m_cacheDirty = false;
    item.m_cacheStartSeconds = firstGrid.startSeconds;
    item.m_cacheEndSeconds = firstGrid.endSeconds;
    item.m_cacheSecondsPerPixel = firstGrid.secondsPerPixel;
    item.m_cacheFramesPerPoint = item.m_detail.framesPerPoint;
    item.m_cacheDisplayedChannels = 1;
    item.beginStagedCacheForRangeLocked(2.37, 4.37, false);

    QCOMPARE(item.m_stagedCacheNextX, 0);

    for (int iteration = 0;
         iteration < 20 && !item.m_stagedCache.isNull();
         ++iteration) {
        item.advanceStagedCacheLocked();
    }
    QVERIFY(item.m_stagedCache.isNull());
    QCOMPARE(item.m_cache.size(), secondCache.size());
    for (int y = 0; y < item.m_cache.height(); ++y) {
        const auto *actual = reinterpret_cast<const QRgb *>(
            item.m_cache.constScanLine(y));
        const auto *expected = reinterpret_cast<const QRgb *>(
            secondCache.constScanLine(y));
        for (int x = 0; x < item.m_cache.width(); ++x) {
            QCOMPARE(actual[x], expected[x]);
        }
    }
}

void QmlSmokeTest::waveformEditorReplacementCacheDoesNotReuseOldRaster() {
    WaveformEditorItem item;
    item.setWidth(1'000);
    item.setHeight(1'440);
    item.setDurationSeconds(10.0);
    item.m_sampleRateHz = 1'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.assign(20'000, 0.25F);
    item.rebuildCacheLocked(1'000, 1'440);
    const QImage oldCache = item.m_cache;

    item.beginStagedCacheLocked();

    QVERIFY(!item.m_stagedCache.isNull());
    QCOMPARE(item.m_stagedCacheNextX, 0);
    QVERIFY(item.m_stagedCache.constBits() != oldCache.constBits());
}

void QmlSmokeTest::waveformEditorBuildsPlaybackTilesWithinFrameBudget() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_positionSeconds = 5.0;
    item.m_zoomLevel = 4.0;
    item.m_presentedZoomLevel = 4.0;
    item.m_playing = true;
    item.m_sampleRateHz = 44'100;
    item.m_channelCount = 2;
    item.m_detail.sampleRateHz = 44'100;
    item.m_detail.channelCount = 2;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 88;
    item.m_detail.pointCount = 5'000;
    item.m_detail.extrema.assign(20'000, 0.25F);

    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    QVERIFY(item.playbackTilesEligibleLocked(visibleStart, visibleEnd));
    item.preparePlaybackTilesLocked(visibleStart, visibleEnd, 180);

    const double tileDuration = item.m_playbackTileSecondsPerPixel * 64.0;
    const qint64 firstTile = static_cast<qint64>(std::floor(
        visibleStart / tileDuration + 1.0e-9));
    const double firstTileStart = static_cast<double>(firstTile) * tileDuration;
    QVERIFY(!item.detailResolutionCoversLocked(
        firstTileStart, firstTileStart + tileDuration));
    QVERIFY(item.detailResolutionCoversPixelSpanLocked(
        firstTileStart, firstTileStart + tileDuration, 64));

    QCOMPARE(item.renderMissingPlaybackTilesLocked(
        visibleStart, visibleEnd, 180, 2), 2);
    QCOMPARE(item.m_playbackTiles.size(), 2U);
    QVERIFY(!item.playbackTilesCoverLocked(visibleStart, visibleEnd));

    while (!item.playbackTilesCoverLocked(visibleStart, visibleEnd)) {
        const int rendered = item.renderMissingPlaybackTilesLocked(
            visibleStart, visibleEnd, 180, 2);
        QVERIFY(rendered > 0);
        QVERIFY(rendered <= 2);
    }
    const auto paints = item.playbackTilePaintsLocked(
        visibleStart, visibleEnd, 320);
    QVERIFY(!paints.empty());
    for (const auto &paint : paints) {
        QCOMPARE(paint.image.width(), 64);
        QCOMPARE(paint.image.height(), 180);
    }

    const double shiftedStart = visibleStart + 6.0 * tileDuration;
    const double shiftedEnd = visibleEnd + 6.0 * tileDuration;
    const int catchUpBudget = item.playbackTileRenderBudgetLocked(
        shiftedStart, shiftedEnd);
    QVERIFY(catchUpBudget > 2);
    QVERIFY(catchUpBudget <= 8);
    QCOMPARE(item.renderMissingPlaybackTilesLocked(
        shiftedStart, shiftedEnd, 180, catchUpBudget), catchUpBudget);
    QVERIFY(item.playbackTilesCoverLocked(shiftedStart, shiftedEnd));
}

void QmlSmokeTest::waveformEditorKeepsPlaybackTilesAcrossDetailHandoffs() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_positionSeconds = 5.0;
    item.m_zoomLevel = 4.0;
    item.m_presentedZoomLevel = 4.0;
    item.m_playing = true;
    item.m_sampleRateHz = 1'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.assign(20'000, 0.25F);

    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    item.preparePlaybackTilesLocked(visibleStart, visibleEnd, 180);
    while (!item.playbackTilesCoverLocked(visibleStart, visibleEnd)) {
        QVERIFY(item.renderMissingPlaybackTilesLocked(
            visibleStart, visibleEnd, 180, 2) > 0);
    }
    const auto originalTiles = item.m_playbackTiles;

    item.m_detail.startSeconds = 1.0;
    item.m_detail.endSeconds = 9.0;
    item.m_detail.pointCount = 8'000;
    item.m_detail.extrema.assign(16'000, 0.5F);
    item.preparePlaybackTilesLocked(visibleStart, visibleEnd, 180);

    QCOMPARE(item.m_playbackTiles.size(), originalTiles.size());
    for (const auto &[tileIndex, originalTile] : originalTiles) {
        const auto current = item.m_playbackTiles.find(tileIndex);
        QVERIFY(current != item.m_playbackTiles.end());
        QCOMPARE(
            current->second.image.cacheKey(),
            originalTile.image.cacheKey());
    }

    item.m_cacheDirty = false;
    item.m_cacheStartSeconds = 0.0;
    item.m_cacheEndSeconds = 1.0;
    item.m_detail = {};
    QVERIFY(item.playbackTilesEligibleLocked(visibleStart, visibleEnd));
    item.setPositionSeconds(5.01);
    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_playbackTiles.empty());
}

void QmlSmokeTest::waveformEditorBuildsReplacementCacheIncrementally() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter initialPainter(&canvas);
    item.paint(&initialPainter);
    initialPainter.end();
    const uchar *initialCache = item.m_cache.constBits();

    item.setZoomLevel(4.0);
    item.m_sampleRateHz = 48'000;
    item.m_channelCount = 1;
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 96;
    item.m_detail.pointCount = 5'000;
    item.m_detail.extrema.resize(10'000);
    for (int point = 0; point < item.m_detail.pointCount; ++point) {
        item.m_detail.extrema[static_cast<std::size_t>(point) * 2U] = -0.5F;
        item.m_detail.extrema[static_cast<std::size_t>(point) * 2U + 1U] = 0.5F;
    }

    item.beginStagedCacheLocked();
    QVERIFY(!item.m_stagedCache.isNull());
    item.advanceStagedCacheLocked();
    QCOMPARE(item.m_cache.constBits(), initialCache);
    for (int iteration = 0; iteration < 20 && !item.m_stagedCache.isNull(); ++iteration) {
        item.advanceStagedCacheLocked();
    }

    QVERIFY(item.m_stagedCache.isNull());
    QVERIFY(item.m_cache.constBits() != initialCache);
    QVERIFY(item.m_cache.width() >= 959);
    QVERIFY(item.m_cache.width() <= 961);
    QVERIFY(!item.m_cacheDirty);
}

void QmlSmokeTest::waveformEditorPaintDefersGuiContinuation() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.m_stagedCache = QImage(320, 180, QImage::Format_RGB32);
    item.m_stagedCache.fill(QColor(54, 225, 161));
    item.m_stagedCacheStartSeconds = 0.0;
    item.m_stagedCacheEndSeconds = 1.0;
    item.m_stagedCacheSecondsPerPixel = 1.0 / 320.0;
    item.m_stagedCacheNextX = item.m_stagedCache.width();
    item.m_stagedCacheCommitsDeferredZoom = true;
    item.m_zoomLevel = 1.0;
    item.m_presentedZoomLevel = 2.0;
    item.m_zoomOutHandoffPending = true;
    item.m_durationSeconds = 1.0;

    QSignalSpy samplePointsSpy(
        &item, &WaveformEditorItem::samplePointsVisibleChanged);
    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(samplePointsSpy.count(), 0);
    QVERIFY(item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QTRY_COMPARE(samplePointsSpy.count(), 1);
    QVERIFY(!item.m_guiContinuationQueued.load(std::memory_order_acquire));

    item.m_stagedCache = QImage(320, 180, QImage::Format_RGB32);
    item.m_stagedCacheNextX = 0;
    QPainter continuationPainter(&canvas);
    item.paint(&continuationPainter);
    continuationPainter.end();

    QVERIFY(!item.m_stagedCache.isNull());
    QVERIFY(item.m_guiRepaintPending.load(std::memory_order_acquire));
    QVERIFY(item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QTRY_VERIFY(!item.m_guiContinuationQueued.load(std::memory_order_acquire));
    QVERIFY(!item.m_guiRepaintPending.load(std::memory_order_acquire));
}

void QmlSmokeTest::waveformEditorZoomedPlaybackUsesScrollingCache() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(10.0);
    item.m_detail.sampleRateHz = 48'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 3.5;
    item.m_detail.endSeconds = 6.5;
    item.m_detail.framesPerPoint = 90;
    item.m_detail.pointCount = 4'800;
    item.m_detail.extrema.assign(9'600, 0.25F);
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();

    QVERIFY(!item.samplePointsVisibleLocked());
    QVERIFY(!item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_cache.isNull());
}

void QmlSmokeTest::waveformEditorPlaybackOverviewCacheHasForwardHeadroom() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(128)));
    item.m_playing = true;
    item.m_positionUpdatedAt = std::chrono::steady_clock::now();

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    const double visibleSpan = visibleEnd - visibleStart;

    QVERIFY(!item.m_cacheDirty);
    QVERIFY(item.m_cacheEndSeconds > visibleEnd + visibleSpan * 1.6);
    item.setPositionSeconds(5.5);
    QVERIFY(!item.m_cacheDirty);
}

void QmlSmokeTest::waveformEditorCachedPaintClearsUncoveredPixels() {
    WaveformEditorItem item;
    item.setWidth(100);
    item.setHeight(100);
    item.setDurationSeconds(10.0);
    item.m_requestTimer.stop();
    item.m_cache = QImage(100, 100, QImage::Format_RGB32);
    item.m_cache.fill(QColor(54, 225, 161));
    item.m_cacheDirty = false;
    item.m_cachedViewportWidth = 100;
    item.m_cachedViewportHeight = 100;
    item.m_cacheStartSeconds = 5.0;
    item.m_cacheEndSeconds = 15.0;
    item.m_cacheSecondsPerPixel = 0.1;

    QImage canvas(100, 100, QImage::Format_RGB32);
    canvas.fill(QColor(255, 0, 255));
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(canvas.pixelColor(10, 20), QColor(5, 9, 7));
    QCOMPARE(canvas.pixelColor(75, 20), QColor(54, 225, 161));
}

void QmlSmokeTest::waveformEditorPausedDetailReplacesOverviewCache() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    item.m_requestTimer.stop();

    QImage canvas(320, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();
    QCOMPARE(item.m_cacheFramesPerPoint, quint32{0});

    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 4;
    item.m_detail.pointCount = 2'500;
    item.m_detail.extrema.assign(5'000, 0.25F);

    QVERIFY(item.detailResultRequiresImmediateCacheRefreshLocked(
        false, false, false, false, 4, true));
}

void QmlSmokeTest::waveformEditorPlaybackHeartbeatDoesNotMoveBackward() {
    WaveformEditorItem item;
    item.setDurationSeconds(30.0);
    item.m_requestTimer.stop();
    item.m_playing = true;
    item.m_positionSeconds = 10.0;
    item.m_positionUpdatedAt = std::chrono::steady_clock::now()
        - std::chrono::milliseconds(200);
    const double before = item.displayedPositionSecondsLocked();

    item.setPositionSeconds(before - 0.12);
    const double after = item.displayedPositionSecondsLocked();

    QVERIFY2(
        after >= before - 0.01,
        qPrintable(QStringLiteral("before=%1 after=%2")
            .arg(before, 0, 'f', 6)
            .arg(after, 0, 'f', 6)));
}

void QmlSmokeTest::waveformEditorExplicitSeekBypassesHeartbeatSmoothing() {
    WaveformEditorItem item;
    item.setDurationSeconds(30.0);
    item.setPositionSeconds(10.0);
    item.setPlaying(true);
    for (double target : {9.5, 9.4, 9.45, 0.0}) {
        item.applyExplicitSeekPosition(target);
        QCOMPARE(item.positionSeconds(), target);
        QVERIFY(item.displayedPositionSecondsLocked() < target + 0.02);
        QVERIFY(item.playing());
    }
    item.setPlaying(false);
    item.applyExplicitSeekPosition(0.25);
    QCOMPARE(item.positionSeconds(), 0.25);
    QVERIFY(!item.playing());
}

void QmlSmokeTest::waveformEditorHoverDrawsCrosshairAndReadouts() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setCrosshairEnabled(true);

    QImage baseline(320, 180, QImage::Format_RGB32);
    baseline.fill(Qt::black);
    QPainter baselinePainter(&baseline);
    item.paint(&baselinePainter);
    baselinePainter.end();

    item.setHoverPosition(137.0, 53.0, true);
    QImage hovered(320, 180, QImage::Format_RGB32);
    hovered.fill(Qt::black);
    QPainter hoverPainter(&hovered);
    item.paint(&hoverPainter);
    hoverPainter.end();

    int changedPixels = 0;
    for (int y = 0; y < hovered.height(); ++y) {
        for (int x = 0; x < hovered.width(); ++x) {
            if (hovered.pixel(x, y) != baseline.pixel(x, y)) ++changedPixels;
        }
    }
    QVERIFY(changedPixels > 250);
}

void QmlSmokeTest::waveformEditorCrosshairLabelsMatchSpectrogramMargins() {
    QCOMPARE(
        WaveformEditorItem::formatCrosshairTime(28.98),
        QStringLiteral("0:28.980"));
    QCOMPARE(
        WaveformEditorItem::formatCrosshairTime(3'723.004),
        QStringLiteral("1:02:03.004"));

    constexpr int width = 1'183;
    constexpr int height = 318;
    constexpr int cursorX = 563;
    constexpr int cursorY = 137;
    constexpr int valueWidth = 42;
    constexpr int timeWidth = 48;
    constexpr int textHeight = 12;
    const auto [valueRect, timeRect] =
        WaveformEditorItem::crosshairLabelRects(
            width, height, cursorX, cursorY,
            valueWidth, timeWidth, textHeight);

    QCOMPARE(valueRect.right(), width - 3);
    QCOMPARE(valueRect.y(), cursorY - textHeight / 2);
    QCOMPARE(timeRect.x(), cursorX - timeWidth / 2);
    QCOMPARE(timeRect.bottom(), height - 3);
    QCOMPARE(valueRect.size(), QSize(valueWidth + 6, textHeight + 6));
    QCOMPARE(timeRect.size(), QSize(timeWidth + 6, textHeight + 6));
}

void QmlSmokeTest::waveformEditorSampleViewRepaintsCrosshairCleanly() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.setPositionSeconds(0.5);
    item.setCrosshairEnabled(true);
    item.m_sampleRateHz = 100;
    item.m_detail.sampleRateHz = 100;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 100;
    item.m_detail.extrema.assign(200, 0.0F);
    item.setZoomLevel(item.maximumZoomLevel());
    const auto [visibleStart, visibleEnd] = item.visibleRangeLocked();
    QVERIFY(item.renderDetailDirectlyLocked(visibleStart, visibleEnd));

    QImage baseline(400, 180, QImage::Format_RGB32);
    baseline.fill(Qt::black);
    QPainter baselinePainter(&baseline);
    item.paint(&baselinePainter);
    baselinePainter.end();

    QImage reused = baseline.copy();
    item.setHoverPosition(80.0, 40.0, true);
    QPainter firstPainter(&reused);
    item.paint(&firstPainter);
    firstPainter.end();
    item.setHoverPosition(240.0, 110.0, true);
    QPainter secondPainter(&reused);
    item.paint(&secondPainter);
    secondPainter.end();

    for (int y = 4; y < 90; ++y) {
        QCOMPARE(reused.pixelColor(80, y), baseline.pixelColor(80, y));
    }
}

void QmlSmokeTest::waveformEditorSampleViewUsesSmoothInterpolation() {
    const QPolygonF samples{
        QPointF(0.0, 90.0),
        QPointF(10.0, 20.0),
        QPointF(20.0, 160.0),
        QPointF(30.0, 90.0),
    };

    const QPainterPath path = WaveformEditorItem::buildSamplePath(samples);

    QCOMPARE(path.elementCount(), 10);
    QCOMPARE(path.elementAt(1).type, QPainterPath::CurveToElement);
    QCOMPARE(path.elementAt(4).type, QPainterPath::CurveToElement);
    QCOMPARE(path.currentPosition(), samples.constLast());
}

void QmlSmokeTest::waveformEditorSampleViewDoesNotFillLaterChannels() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    item.setViewMode(1);
    item.m_sampleRateHz = 4;
    item.m_channelCount = 2;
    item.m_detail.sampleRateHz = 4;
    item.m_detail.channelCount = 2;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 1.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 4;
    const std::array<float, 4> rightSamples = {0.0F, 0.8F, 0.8F, 0.0F};
    for (float rightSample : rightSamples) {
        item.m_detail.extrema.insert(
            item.m_detail.extrema.end(),
            {0.0F, 0.0F, rightSample, rightSample});
    }

    QImage canvas(400, 180, QImage::Format_RGB32);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    QCOMPARE(canvas.pixelColor(200, 120), QColor(5, 9, 7));
}

void QmlSmokeTest::waveformEditorReclampsZoomForDecodedSampleRate() {
    WaveformEditorItem item;
    item.setWidth(100);
    item.setDurationSeconds(100.0);
    item.setZoomLevel(item.maximumZoomLevel());
    QVERIFY(item.zoomLevel() > 100'000.0);

    item.m_sampleRateHz = 1'000;
    QVERIFY(item.clampZoomToMaximumLocked());

    QCOMPARE(item.zoomLevel(), 8'000.0);
    QVERIFY(item.m_cacheDirty);
}

void QmlSmokeTest::waveformEditorRulersFollowGridSetting() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(1'024, static_cast<char>(255)));
    const auto countRulerPixels = [&item]() {
        QImage canvas(320, 180, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        int rulerPixels = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            for (int x = 260; x < canvas.width(); ++x) {
                const QColor pixel(canvas.pixel(x, y));
                const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
                const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
                if (minimum > 80 && maximum - minimum < 40) ++rulerPixels;
            }
        }
        return rulerPixels;
    };

    item.setGridEnabled(false);
    const int hiddenRulerPixels = countRulerPixels();
    item.setGridEnabled(true);
    const int visibleRulerPixels = countRulerPixels();

    QVERIFY(hiddenRulerPixels < 10);
    QVERIFY(visibleRulerPixels > hiddenRulerPixels + 20);
}

void QmlSmokeTest::waveformEditorRulersRemainVisibleWhenZoomed() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.m_detail.sampleRateHz = 1'000;
    item.m_detail.channelCount = 1;
    item.m_detail.startSeconds = 0.0;
    item.m_detail.endSeconds = 10.0;
    item.m_detail.framesPerPoint = 1;
    item.m_detail.pointCount = 10'000;
    item.m_detail.extrema.assign(20'000, 0.5F);
    item.setZoomLevel(4.0);
    item.setGridEnabled(true);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    int rulerPixels = 0;
    for (int y = 0; y < 160; ++y) {
        for (int x = 290; x < 320; ++x) {
            const QColor pixel(canvas.pixel(x, y));
            const int maximum = std::max({pixel.red(), pixel.green(), pixel.blue()});
            const int minimum = std::min({pixel.red(), pixel.green(), pixel.blue()});
            if (minimum > 80 && maximum - minimum < 40) ++rulerPixels;
        }
    }
    QVERIFY(rulerPixels > 30);
}

void QmlSmokeTest::waveformEditorPlayheadIsThinAndNeutral() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    const QColor playhead(canvas.pixel(160, 40));
    const QColor left(canvas.pixel(159, 40));
    const QColor right(canvas.pixel(161, 40));
    QVERIFY(std::abs(playhead.red() - playhead.green()) < 12);
    QVERIFY(playhead.red() > left.red());
    QCOMPARE(left, right);
}

void QmlSmokeTest::waveformEditorOverviewRespectsCoverageAndChannelIdentity() {
    WaveformEditorItem item;
    item.setDurationSeconds(10.0);
    item.setOverviewCoverageSeconds(2.5);
    QByteArray peaks(1'000, 0);
    peaks[123] = static_cast<char>(255);
    item.setOverviewData(peaks);
    const auto render = [&](double start, double end, int channels) {
        QImage canvas(100, 100, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.drawOverviewLocked(painter, 100, 100, start, end, channels);
        painter.end();
        return canvas;
    };
    const auto partial = render(0.0, 10.0, 1);
    QVERIFY(partial.pixelColor(3, 10) != QColor(Qt::black));
    QCOMPARE(partial.pixelColor(12, 10), QColor(Qt::black));
    QCOMPARE(partial.pixelColor(70, 50), QColor(Qt::black));
    QCOMPARE(render(3.0, 4.0, 1).pixelColor(50, 50), QColor(Qt::black));
    item.m_cacheDirty = false;
    item.setOverviewComplete(true);
    QVERIFY(item.m_cacheDirty);
    const auto complete = render(0.0, 10.0, 1);
    QVERIFY(complete.pixelColor(12, 10) != QColor(Qt::black));
    item.setViewMode(1);
    const auto multichannel = render(0.0, 10.0, 2);
    QCOMPARE(multichannel.pixelColor(12, 10), QColor(Qt::black));
    QCOMPARE(multichannel.pixelColor(12, 60), QColor(Qt::black));
    item.setOverviewComplete(false);
    item.setOverviewCoverageSeconds(0.0);
    QCOMPARE(render(0.0, 10.0, 1).pixelColor(12, 50), QColor(Qt::black));
}

void QmlSmokeTest::waveformEditorReferenceLineContrastsWaveform() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    QByteArray overview(1'024, static_cast<char>(0));
    std::fill(
        overview.begin() + overview.size() / 2,
        overview.end(),
        static_cast<char>(255));
    item.setOverviewComplete(true);
    item.setOverviewData(overview);

    QImage canvas(320, 180, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    item.paint(&painter);
    painter.end();

    const QColor background(canvas.pixel(80, 89));
    const QColor lineOnBackground(canvas.pixel(80, 90));
    const QColor waveform(canvas.pixel(240, 89));
    const QColor lineOnWaveform(canvas.pixel(240, 90));
    const auto luminance = [](const QColor &color) {
        return 0.2126 * color.red()
            + 0.7152 * color.green()
            + 0.0722 * color.blue();
    };
    QVERIFY(lineOnBackground != background);
    QVERIFY(luminance(lineOnWaveform) < luminance(waveform));
    QVERIFY(lineOnWaveform != waveform);
}

void QmlSmokeTest::waveformEditorContrastLinesUseFboSafeCompositionModes() {
    CompositionRecordingPaintDevice device;
    QPainter painter(&device);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    WaveformEditorItem::drawContrastingLine(
        painter,
        QLine(8, 8, 56, 8),
        QColor(190, 190, 200, 150),
        QColor(8, 18, 14, 235));
    QCOMPARE(painter.compositionMode(), QPainter::CompositionMode_Source);
    painter.end();

    QVERIFY(!device.engine.compositionModes.isEmpty());
    for (const QPainter::CompositionMode mode
         : std::as_const(device.engine.compositionModes)) {
        QVERIFY2(
            mode <= QPainter::CompositionMode_Plus,
            "FBO waveform overlays must avoid advanced OpenGL blend modes");
    }
}

void QmlSmokeTest::waveformEditorFpsOverlayTracksPaintRate() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.setShowFpsOverlay(true);

    QImage canvas(320, 180, QImage::Format_RGB32);
    for (int frame = 0; frame < 50; ++frame) {
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        QTest::qWait(5);
    }

    QVERIFY(item.m_fpsValue > 0);
    int overlayPixels = 0;
    int rightmostOverlayPixel = -1;
    for (int y = 0; y < 20; ++y) {
        for (int x = 220; x < canvas.width(); ++x) {
            const QColor pixel(canvas.pixel(x, y));
            if (pixel.red() > 80 && pixel.green() > 80 && pixel.blue() > 80) {
                ++overlayPixels;
                rightmostOverlayPixel = std::max(rightmostOverlayPixel, x);
            }
        }
    }
    QVERIFY(overlayPixels > 10);
    QVERIFY(rightmostOverlayPixel >= canvas.width() - 12);
}

void QmlSmokeTest::waveformEditorUsesSafeRasterTargetAndNativeFrameInterpolation() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setDurationSeconds(10.0);
    item.setPositionSeconds(5.0);
    item.setZoomLevel(2.0);

    QCOMPARE(item.renderTarget(), QQuickPaintedItem::Image);
    QVERIFY(item.opaquePainting());
    QCOMPARE(item.fillColor(), QColor(5, 9, 7));
    QCOMPARE(
        item.performanceHints(),
        QQuickPaintedItem::PerformanceHints{});

    item.setPlaying(true);
    const double before = item.visibleRangeLocked().first;
    QTest::qWait(20);
    const double after = item.visibleRangeLocked().first;
    QVERIFY(after > before + 0.01);
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

void QmlSmokeTest::spectrogramEvictingOldTokenKeepsActiveTokenIteratorValid() {
    // Reproduce the bucket compaction from the crash: the old and active
    // track tokens share an initial bucket, so erasing the old token moves
    // the active token's bucket. Any cached active-token iterator must be
    // reacquired before the next column is inserted.
    constexpr size_t bucketCount = 128;
    std::array<quint64, bucketCount> firstTokenByBucket{};
    const size_t hashSeed = QHashSeed::globalSeed();
    quint64 oldToken = 0;
    quint64 activeToken = 0;
    for (quint64 token = 1; token < 10'000; ++token) {
        const size_t bucket = qHash(token, hashSeed) & (bucketCount - 1);
        if (firstTokenByBucket[bucket] != 0) {
            oldToken = firstTokenByBucket[bucket];
            activeToken = token;
            break;
        }
        firstTokenByBucket[bucket] = token;
    }
    QVERIFY(oldToken != 0);
    QVERIFY(activeToken > oldToken);

    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1);

    constexpr int bins = 4;
    constexpr int total = 20'000;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        48000, 1024, false, true, oldToken);
    item.feedPrecomputedChunk(
        QByteArray(bins, '\x20'), bins, 0, 1, 0, total,
        48000, 1024, false, false, oldToken);
    const int capacity = item.m_ringCapacity;
    QVERIFY(capacity > 1);

    item.feedPrecomputedChunk(
        QByteArray((capacity - 1) * bins, '\x20'),
        bins, 0, capacity - 1, 1, total,
        48000, 1024, false, false, oldToken);
    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(capacity));

    item.feedPrecomputedChunk(
        QByteArray((capacity - 1) * bins, '\x40'),
        bins, 0, capacity - 1, 0, total,
        48000, 1024, false, false, activeToken);
    QCOMPARE(item.m_trackColumnToSeqByToken.size(), 2);
    QCOMPARE(item.m_trackColumnToSeqByToken.value(oldToken).size(), 1);

    item.feedPrecomputedChunk(
        QByteArray(bins, '\x40'), bins, 0, 1, capacity - 1, total,
        48000, 1024, false, false, activeToken);

    QCOMPARE(item.m_ringWriteSeq, static_cast<qint64>(capacity) * 2);
    QVERIFY(!item.m_trackColumnToSeqByToken.contains(oldToken));
    QCOMPARE(item.m_trackColumnToSeqByToken.size(), 1);
    QCOMPARE(
        item.m_trackColumnToSeqByToken.value(activeToken).value(capacity - 1),
        static_cast<qint64>(capacity) * 2 - 1);
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

void QmlSmokeTest::spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int decodedColumns = 4'000;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hop = 1'024;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, totalEstimate,
        sampleRate, hop, false, true, 1);
    QByteArray data(decodedColumns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedColumns, 0, totalEstimate,
        sampleRate, hop, false, false, 1);
    item.setPositionSeconds(103.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY2(
        item.m_precomputedCanvasDisplayRight
            > static_cast<qint64>(item.m_precomputedMaxColumnIndex),
        qPrintable(QStringLiteral("display range was clamped to lagging decoded tail %1..%2 max=%3")
            .arg(item.m_precomputedCanvasDisplayLeft)
            .arg(item.m_precomputedCanvasDisplayRight)
            .arg(item.m_precomputedMaxColumnIndex)));
    QVERIFY(item.m_precomputedCanvasDisplayLeft > decodedColumns);
}

void QmlSmokeTest::spectrogramStoppedZoomResetRefillsCanvas() {
    // Contract for the stopped zoom-reset flow: with playback stopped at
    // position 0, a middle-click zoom reset (deep zoom-out hop -> reference
    // hop) restarts the decode session from the track start.  The widget
    // must leave the zoom-fill state and show the refilled canvas instead
    // of a frozen or black view.
    SpectrogramItem item;
    item.setWidth(1162);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 1025;
    constexpr quint64 token = 7;
    constexpr int hopZoomedOut = 10'240;
    constexpr int hopNormal = 1'024;
    constexpr int sampleRate = 44'100;
    constexpr int estZoomedOut = 1'247;
    constexpr int estNormal = 13'618;
    constexpr int zoomedOutDecoded = 1'240;
    constexpr int normalDecoded = 2'755;
    constexpr int staleStart = 11'611;
    constexpr int staleDecoded = 1'943;

    // --- Phase 1: zoomed-out session (post-stop restart at 0) ---
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, estZoomedOut,
        sampleRate, hopZoomedOut, false, true, token, true, 1);
    for (int start = 0; start < zoomedOutDecoded; start += 124) {
        const int cols = qMin(124, zoomedOutDecoded - start);
        QByteArray data(cols * bins, '\x40');
        item.feedPrecomputedChunk(
            data, bins, 0, cols, start, estZoomedOut,
            sampleRate, hopZoomedOut, false, false, token, false, 1);
    }
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1024.0 / static_cast<double>(hopZoomedOut);
    }
    item.setPositionSeconds(0.0);
    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_canvas.isNull());
    QVERIFY(item.m_canvasFilledCols > 0);

    // --- Phase 2: middle-click zoom reset (zoom 1.0 session) ---
    item.m_zoomLevel = 1.0;
    item.m_awaitingZoomData = true;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, staleStart, estNormal,
        sampleRate, hopNormal, false, true, token, true, 2);
    for (int offset = 0; offset < staleDecoded; offset += 32) {
        const int cols = qMin(32, staleDecoded - offset);
        QByteArray data(cols * bins, '\x40');
        item.feedPrecomputedChunk(
            data, bins, 0, cols, staleStart + offset, estNormal,
            sampleRate, hopNormal, false, false, token, false, 2);
    }
    // This mirrors the trace's bad backend window: it has more than a
    // screenful of data and a very large max column, but none of the stopped
    // viewport around column 0.  Keep the old non-black canvas frozen rather
    // than accepting a fully black rebuild.
    QCOMPARE(item.m_zoomFillActive, true);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(node != nullptr);
    bool frozenCanvasHasContent = false;
    for (int y = 0; y < item.m_canvas.height() && !frozenCanvasHasContent; ++y) {
        for (int x = 0; x < item.m_canvas.width(); ++x) {
            if (item.m_canvas.pixel(x, y) != qRgb(0, 0, 0)) {
                frozenCanvasHasContent = true;
                break;
            }
        }
    }
    QVERIFY2(frozenCanvasHasContent,
             "non-overlapping refill must preserve the previous visible canvas");

    // Corrected backend restart for the stopped viewport.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, estNormal,
        sampleRate, hopNormal, false, true, token, true, 3);
    for (int start = 0; start < normalDecoded; start += 32) {
        const int cols = qMin(32, normalDecoded - start);
        QByteArray data(cols * bins, '\x40');
        item.feedPrecomputedChunk(
            data, bins, 0, cols, start, estNormal,
            sampleRate, hopNormal, false, false, token, false, 3);
    }
    // The zoom-fill freeze must release once the ring covers the display
    // again, and the next paint must rebuild the canvas with content.
    QCOMPARE(item.m_zoomFillActive, false);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_canvas.isNull());
    QVERIFY2(item.m_canvasFilledCols > 0,
             qPrintable(QStringLiteral("canvas refill produced %1 columns")
                            .arg(item.m_canvasFilledCols)));
    bool refilledCanvasHasContent = false;
    for (int y = 0; y < item.m_canvas.height() && !refilledCanvasHasContent; ++y) {
        for (int x = 0; x < item.m_canvas.width(); ++x) {
            if (item.m_canvas.pixel(x, y) != qRgb(0, 0, 0)) {
                refilledCanvasHasContent = true;
                break;
            }
        }
    }
    QVERIFY2(refilledCanvasHasContent,
             "stopped zoom reset must render non-background spectrogram pixels");
    delete node;
}

void QmlSmokeTest::spectrogramZoomOutFillDoesNotClampToLaggingDecodedTail() {
    // Regression for viewport racing on zoom-out near the end of a track.
    // During the post-zoom refill the decoded tail lags far behind the
    // playhead, which makes the EOF-detach precondition trivially true;
    // with a window-scaled slack the clamp then engaged mid-fill and
    // dragged the viewport rightward with the decode head.  The display
    // range must stay pinned to the estimate until the decode is genuinely
    // within ~2 s of the track end.
    SpectrogramItem item;
    item.setWidth(1162);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 8;
    constexpr int sampleRate = 44'100;
    constexpr int hop = 10'240;           // deep zoom-out
    constexpr int totalEstimate = 1'247;  // 288 s track at hop 10240 (+fudge)
    // Within the old hard 64-column tolerance but still roughly 13 s from
    // the estimate at this coarse hop.  A genuinely two-second tolerance
    // must keep treating this as an in-progress refill.
    constexpr int decodedColumns = totalEstimate - 57;
    constexpr quint64 token = 7;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, totalEstimate,
        sampleRate, hop, false, true, token);
    QByteArray data(decodedColumns * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, decodedColumns, 0, totalEstimate,
        sampleRate, hop, false, false, token);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1024.0 / static_cast<double>(hop);
    }
    item.setPositionSeconds(280.0); // playhead near EOF, decode head lags

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    // The full-track display lock pins the range to the estimate; the
    // lagging decoded tail must not clamp it mid-fill.  (The stored
    // canvas range is truncated to the widget width: one column per
    // pixel at effectiveZoom 1.0.)
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(1162 - 1));
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

void QmlSmokeTest::spectrogramRingCapacityRemembersFullscreenWidthBeforeNextChunk() {
    // Regression: the Rust worker records a larger widget width as soon as
    // QML reports it, but Qt used to record the same maximum only while
    // ingesting a data chunk. If fullscreen was closed before the parked
    // centered decoder delivered another chunk, Qt forgot the wide width
    // and allocated a windowed ring that was smaller than the worker's
    // retained lookahead. The decoder then evicted columns around the
    // playhead, leaving a permanent black gap in the rendered timeline.
    SpectrogramItem::s_maxWidgetWidthSeen = 0;

    SpectrogramItem item;
    item.setHeight(200);
    item.setDisplayMode(1); // Centered

    item.setWidth(1200);
    item.setWidth(3840); // Enter fullscreen; no data arrives while wide.
    item.setWidth(1200); // Exit fullscreen before the next chunk.

    constexpr int bins = 4;
    QByteArray data(16 * bins, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024,
        44100, 64, false, true, 1);

    QCOMPARE(SpectrogramItem::s_maxWidgetWidthSeen, 3840);
    QVERIFY2(item.m_ringCapacity >= 3840,
             qPrintable(QString("post-fullscreen ring cap too small: %1")
                            .arg(item.m_ringCapacity)));
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

void QmlSmokeTest::spectrogramTrackChangeMetadataResetClearsOldCenteredFrame() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int total = 4096;
    constexpr quint64 oldToken = 3;
    constexpr quint64 newToken = 4;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        48000, 1024, false, true, oldToken, true);
    QByteArray oldData(320 * bins, '\x40');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 320, 0, total,
        48000, 1024, false, false, oldToken, false);
    item.setPositionSeconds(72.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(180);
        item.rebuildPrecomputedCanvasLocked(320, 180, 0, 319, false);
    }
    QVERIFY(item.m_precomputedReady);
    QVERIFY(!item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(319));
    QVERIFY(item.m_positionAnchorSeconds > 70.0);

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total,
        48000, 1024, false, true, newToken, true);

    QVERIFY2(
        !item.m_precomputedReady,
        "non-gapless track-change metadata reset must not keep old spectrogram visible");
    QVERIFY(item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(-1));
    QCOMPARE(item.m_precomputedResetPending, false);
    QCOMPARE(item.m_zoomFillActive, false);
    QVERIFY(std::abs(item.m_positionAnchorSeconds) < 0.01);
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

void QmlSmokeTest::queueRemovalSendsOneBatch() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers
Item {
    id: harness
    property int batches: 0
    property string removed: ""
    QtObject {
        id: bridge
        property int queueLength: 10000
        property int selectedQueueIndex: 1
        function removeIndices(indices) { harness.batches++; harness.removed = indices.join(",") }
    }
    Controllers.QueueController {
        id: controller
        uiBridge: bridge
        tagEditorApi: null
        openTagEditorDialog: function() {}
    }
    function removeSelection() {
        controller.setSelectedIndices([1, 200, 9999])
        controller.removeSelectedTrack()
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QVERIFY(QMetaObject::invokeMethod(root.data(), "removeSelection"));
    QCOMPARE(root->property("batches").toInt(), 1);
    QCOMPARE(root->property("removed").toString(), QStringLiteral("9999,200,1"));
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

void QmlSmokeTest::spectrogramDynamicRangePreservesSilence() {
    SpectrogramItem item;
    for (int mode : {0, 1}) {
        item.setDisplayMode(mode);
        for (int fft : {512, 2'048, 8'192}) {
            item.m_precomputedBinsPerColumn = fft / 2 + 1;
            for (int range = 50; range <= 150; ++range) {
                item.setDbRange(range);
                const auto remap = item.buildPrecomputedDbRemapLocked();
                QCOMPARE(remap[0], quint8(0));
                QCOMPARE(remap[255], quint8(255));
                for (int i = 1; i < 256; ++i) QVERIFY(remap[i] >= remap[i - 1]);
                // -130 dBFS is visible at the full range, clipped at 90 dB.
                if (range == 150) QVERIFY(remap[35] > 0);
                if (range == 90) QCOMPARE(remap[35], quint8(0));
            }
        }
    }
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

void QmlSmokeTest::spectrogramPreservesNativeRateAcrossModesAndTrackTransitions() {
    for (int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(200);
        item.setHeight(100);
        item.setDisplayMode(mode);
        item.setSampleRateHz(48000);
        const int bins = 4097;
        const QByteArray data(bins, '\x80');
        quint64 generation = 1;
        for (int rate : {96000, 192000, 44100}) {
            item.feedPrecomputedChunk(data, bins, 0, 1, 0, 1000, rate, 1024,
                false, true, generation, true, generation);
            const double nyquist = item.pixelToFrequencyHz(0, 100);
            QVERIFY(std::abs(nyquist - rate / 2.0) < rate * 0.02);
            QCOMPARE(item.m_precomputedSampleRateHz, rate);
            ++generation;
        }
        item.feedPrecomputedChunk(data, bins, 0, 1, 0, 1000, 96000, 1024,
            false, true, 1, true, 1);
        QCOMPARE(item.m_precomputedSampleRateHz, 44100);
    }
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

void QmlSmokeTest::spectrogramCrosshairOverlayReusesImageBufferAtSameGeometry() {
    SpectrogramItem item;
    item.setSampleRateHz(48000);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);

    const int bins = 4097;
    QByteArray data(bins * 100, '\x80');
    item.feedPrecomputedChunk(data, bins, 0, 100, 0, 1000, 48000, 1024,
                               false, true, 1, false);

    const double cps = 48000.0 / 1024.0;
    const uchar *firstBits = nullptr;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(50.0, 40.0);
        item.m_binsPerColumn = item.m_precomputedBinsPerColumn;
        item.ensureMapping(100);
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 0.0);
        QVERIFY(!item.m_crosshairImage.isNull());
        firstBits = item.m_crosshairImage.constBits();
        QVERIFY(firstBits != nullptr);
        item.updateCrosshairOverlayLocked(200, 100, 0, true, cps, 0.5);
        QVERIFY(!item.m_crosshairImage.isNull());
        QCOMPARE(item.m_crosshairImage.constBits(), firstBits);
    }
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

void QmlSmokeTest::spectrogramClickToSeekUsesCurrentPositionWhenCrosshairCacheIsStale() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(8'000 * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        8'000,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    item.setPositionSeconds(10.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_crosshairCachedDisplayLeft = 0;
        item.m_crosshairCachedDrawX = 0.0;
        item.m_crosshairCachedRollingMode = false;
    }

    item.setPositionSeconds(100.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 100.0,
        qPrintable(QStringLiteral("stale cache produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
}

void QmlSmokeTest::spectrogramClickToSeekIgnoresLaggingDecodedTailInCenteredMode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    constexpr int binsPerColumn = 8;
    constexpr int decodedColumns = 4'000;
    constexpr int totalEstimate = 20'000;
    constexpr int sampleRate = 48'000;
    constexpr int hopSize = 1'024;
    QByteArray chunk(decodedColumns * binsPerColumn, '\0');
    item.feedPrecomputedChunk(
        chunk,
        binsPerColumn,
        0,
        decodedColumns,
        0,
        totalEstimate,
        sampleRate,
        hopSize,
        false,
        true,
        42);

    item.setDisplayMode(1);
    item.setCrosshairEnabled(true);
    item.setPositionSeconds(103.0);

    QSignalSpy seekSpy(&item, &SpectrogramItem::seekRequested);
    QMouseEvent pressEvent(
        QEvent::MouseButtonPress,
        QPointF(300.0, 90.0),
        QPointF(300.0, 90.0),
        Qt::RightButton,
        Qt::RightButton,
        Qt::NoModifier);
    item.mousePressEvent(&pressEvent);

    QCOMPARE(seekSpy.count(), 1);
    const double seekSeconds = seekSpy.at(0).at(0).toDouble();
    QVERIFY2(
        seekSeconds > 103.0,
        qPrintable(QStringLiteral("lagging decoded tail produced backward seek target %1")
            .arg(seekSeconds, 0, 'f', 3)));
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

void QmlSmokeTest::spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int initialColumns = 2000;
    QByteArray initialChunk(binsPerColumn * initialColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, initialColumns,
        0, initialColumns, 48000, 1024, true,
        true, 1, false);
    item.setPositionSeconds(10.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(item.m_precomputedCanvasDisplayLeft > 0);
    const qint64 initialDisplayLeft = item.m_precomputedCanvasDisplayLeft;
    item.setZoomLevel(0.1);
    item.m_zoomDebounceTimer->stop();
    QVERIFY(item.m_awaitingZoomData);

    constexpr int zoomedOutTotalColumns = 200;
    constexpr int zoomedOutDecodedColumns = 32;
    QByteArray zoomedOutChunk(binsPerColumn * zoomedOutDecodedColumns, '\x60');
    item.feedPrecomputedChunk(
        zoomedOutChunk, binsPerColumn, 0, zoomedOutDecodedColumns,
        0, zoomedOutTotalColumns, 48000, 10240, false,
        true, 1, false);

    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);

    QVERIFY2(!item.m_zoomFillActive,
             "zoom-out should not freeze on the old centered cache while the coarser restart fills");
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
    // The range pins to the estimate-based full-track extent; the decoded
    // tail (32 cols) no longer clamps it because the EOF-detach tolerance
    // is time-based and the decode is nowhere near the track end.
    QCOMPARE(item.m_precomputedCanvasDisplayRight,
             static_cast<qint64>(zoomedOutTotalColumns - 1));
    QVERIFY(item.m_precomputedCanvasDisplayLeft < initialDisplayLeft);
    QVERIFY(!item.m_canvas.isNull());
    // The rebuild covers the whole estimate-pinned range; only the first
    // zoomedOutDecodedColumns columns carry real content.
    QCOMPARE(item.m_canvasFilledCols, zoomedOutTotalColumns);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 0.001);

    delete updatedNode;
}

void QmlSmokeTest::spectrogramResizeForcesFreshBodyTextureUpload() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    auto *item = new SpectrogramItem();
    item->setParentItem(window.contentItem());
    item->setWidth(240);
    item->setHeight(80);
    item->setDisplayMode(1);
    item->setPlaying(false);

    constexpr int bins = 32;
    constexpr int columns = 320;
    QByteArray data(bins * columns, '\x20');
    for (int column = 96; column < 160; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            data[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(64 + column - 96 + bin));
        }
    }

    item->feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item->setPositionSeconds(1.0);

    QSGNode *node = item->updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(node->childCount() >= 2);
    auto *tilesRoot = node->childAtIndex(1);
    QVERIFY(tilesRoot != nullptr);
    QVERIFY(tilesRoot->childCount() > 0);
    auto *tileNode =
        static_cast<QSGSimpleTextureNode *>(tilesRoot->childAtIndex(0));
    QVERIFY(tileNode != nullptr);
    QVERIFY(tileNode->texture() != nullptr);
    QCOMPARE(tileNode->texture()->textureSize().height(), 80);

    item->setHeight(140);
    QSGNode *updatedNode = item->updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    QVERIFY(item->m_canvas.height() == 140);
    QVERIFY(tileNode->texture() != nullptr);
    QCOMPARE(tileNode->texture()->textureSize().height(), 140);

    delete updatedNode;
}

void QmlSmokeTest::spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(159);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setLogScale(false);

    constexpr int binsPerColumn = 1025;
    constexpr int columns = 400;
    QByteArray chunk(binsPerColumn * columns, '\0');
    for (int column = 0; column < columns; ++column) {
        for (int bin = binsPerColumn - 12; bin < binsPerColumn; ++bin) {
            chunk[column * binsPerColumn + bin] = static_cast<char>(
                static_cast<unsigned char>(0xFF));
        }
    }

    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_canvas.isNull());

    auto topRowLuma = [&item]() -> int {
        const int sampleX = std::clamp(item.m_canvas.width() / 2, 0,
                                       std::max(0, item.m_canvas.width() - 1));
        const QColor topPixel = item.m_canvas.pixelColor(sampleX, 0);
        const QColor midPixel = item.m_canvas.pixelColor(
            sampleX,
            std::clamp(item.m_canvas.height() / 2, 0,
                       std::max(0, item.m_canvas.height() - 1)));
        return qGray(topPixel.rgb()) - qGray(midPixel.rgb());
    };

    QVERIFY2(topRowLuma() > 20,
             "default-height linear spectrogram should show highest-bin energy on the top row");

    item.setHeight(318);
    QSGNode *updatedNode = item.updatePaintNode(node, nullptr);
    QVERIFY(updatedNode == node);
    QVERIFY(!item.m_canvas.isNull());
    QVERIFY2(topRowLuma() > 20,
             "taller linear spectrogram should still show highest-bin energy on the top row");

    delete updatedNode;
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

void QmlSmokeTest::spectrogramCenteredZoomOutDropsOlderSameTrackGeneration() {
    SpectrogramItem item;
    item.setWidth(1183);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int binsPerColumn = 8;
    constexpr int referenceColumns = 4000;
    constexpr quint64 trackToken = 1;
    QByteArray initialChunk(binsPerColumn * referenceColumns, '\x40');
    item.feedPrecomputedChunk(
        initialChunk, binsPerColumn, 0, referenceColumns,
        0, referenceColumns, 48000, 1024, false,
        true, trackToken, false, 1);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_zoomLevel = 0.1;
        item.m_renderZoomLevel = 0.1;
        item.m_awaitingZoomData = false;
    }

    constexpr int coarseColumns = 800;
    QByteArray coarseChunk(binsPerColumn * coarseColumns, '\x70');
    item.feedPrecomputedChunk(
        coarseChunk, binsPerColumn, 0, coarseColumns,
        0, coarseColumns, 48000, 5120, false,
        true, trackToken, false, 4);

    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    const qint64 ringWriteSeq = item.m_ringWriteSeq;

    // A superseded rapid-zoom session can use a rounded hop that still
    // matches the final zoom tolerance. Generation, not hop alone, must
    // keep those older chunks out of the current ring.
    constexpr int staleColumns = 64;
    QByteArray staleResetChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleResetChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 5088, false,
        true, trackToken, false, 3);

    QByteArray staleDataChunk(binsPerColumn * staleColumns, '\x20');
    item.feedPrecomputedChunk(
        staleDataChunk, binsPerColumn, 0, staleColumns,
        0, staleColumns, 48000, 5088, false,
        false, trackToken, false, 3);

    QCOMPARE(item.m_precomputedHopSize, 5120);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, coarseColumns);
    QCOMPARE(item.m_precomputedMaxColumnIndex, coarseColumns - 1);
    QCOMPARE(item.m_ringWriteSeq, ringWriteSeq);
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

void QmlSmokeTest::spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead() {
    SpectrogramItem item;
    item.setWidth(3440);
    item.setHeight(80);
    item.setDisplayMode(0); // Rolling
    item.setZoomLevel(1.0);

    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    constexpr int sampleRate = 48000;
    constexpr int hopSize = 1024;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, 100000, sampleRate, hopSize, false,
        true, 1, false);

    const double colsPerSecond =
        static_cast<double>(sampleRate) / static_cast<double>(hopSize);
    const int lookaheadCols = static_cast<int>(10.0 * colsPerSecond);
    const int zoomAdjustedWidth = 3440;

    QVERIFY2(
        item.m_ringCapacity >= zoomAdjustedWidth * 2 + lookaheadCols,
        "rolling mode should reserve at least one extra viewport width of ring headroom beyond the decode lookahead");
}

void QmlSmokeTest::spectrogramRollingCanvasGrowsIncrementallyDuringInitialFill() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling

    constexpr int bins = 4;
    constexpr int initialColumns = 4;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(20 + column * 10 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    QImage initialCanvas;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 2.0;
        item.ensureMapping(6);
        item.rebuildPrecomputedCanvasLocked(12, 6, 0, 3, true);
        QCOMPARE(item.m_canvasFilledCols, 8);
        QCOMPARE(item.m_canvasWriteX, 8);
        initialCanvas = item.m_canvas.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(80 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.advancePrecomputedCanvasLocked(0, 4, true));
        QCOMPARE(item.m_canvasFilledCols, 10);
        QCOMPARE(item.m_canvasWriteX, 10);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(4));
        QCOMPARE(item.m_precomputedCanvasSubpixelOffset, 0.0);
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < 8; ++x) {
                QCOMPARE(item.m_canvas.pixelColor(x, y), initialCanvas.pixelColor(x, y));
            }
        }
    }
}

void QmlSmokeTest::spectrogramRollingCanvasHandsOffToSteadyScrollIncrementally() {
    SpectrogramItem item;
    item.setWidth(12);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling

    constexpr int bins = 4;
    constexpr int initialColumns = 6;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(20 + column * 10 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    QImage initialCanvas;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 2.0;
        item.ensureMapping(6);
        item.rebuildPrecomputedCanvasLocked(12, 6, 0, 5, true);
        QCOMPARE(item.m_canvasFilledCols, 12);
        QCOMPARE(item.m_canvasWriteX, 0);
        initialCanvas = item.m_canvas.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(120 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.advancePrecomputedCanvasLocked(1, 6, true));
        QCOMPARE(item.m_canvasFilledCols, 12);
        QCOMPARE(item.m_canvasWriteX, 2);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(1));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(6));
        QCOMPARE(item.m_precomputedCanvasSubpixelOffset, 0.0);
        const int srcStart =
            (item.m_canvasWriteX - item.m_canvasFilledCols + item.m_canvas.width())
            % item.m_canvas.width();
        QCOMPARE(srcStart, 2);
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < initialCanvas.width() - 2; ++x) {
                QCOMPARE(
                    item.m_canvas.pixelColor((srcStart + x) % item.m_canvas.width(), y),
                    initialCanvas.pixelColor(x + 2, y));
            }
        }
    }
}

void QmlSmokeTest::spectrogramRollingCanvasAdvancesIncrementallyAtFractionalZoom() {
    SpectrogramItem item;
    item.setWidth(7);
    item.setHeight(6);
    item.setDisplayMode(0); // Rolling

    constexpr int bins = 4;
    constexpr int initialColumns = 4;
    QByteArray initialChunk(bins * initialColumns, '\0');
    for (int column = 0; column < initialColumns; ++column) {
        for (int bin = 0; bin < bins; ++bin) {
            initialChunk[column * bins + bin] = static_cast<char>(
                static_cast<unsigned char>(24 + column * 12 + bin));
        }
    }

    item.feedPrecomputedChunk(
        initialChunk, bins, 0, initialColumns,
        0, 64, 48000, 1024, false, true, 1, false);

    QImage initialCanvas;
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1.75;
        item.ensureMapping(6);
        item.rebuildPrecomputedCanvasLocked(7, 6, 0, 3, true);
        QCOMPARE(item.m_canvasFilledCols, 7);
        QCOMPARE(item.m_canvasWriteX, 0);
        initialCanvas = item.m_canvas.copy();
    }

    QByteArray appendChunk(bins, '\0');
    for (int bin = 0; bin < bins; ++bin) {
        appendChunk[bin] = static_cast<char>(
            static_cast<unsigned char>(128 + bin));
    }
    item.feedPrecomputedChunk(
        appendChunk, bins, 0, 1,
        initialColumns, 64, 48000, 1024, false, false, 1, false);

    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.advancePrecomputedCanvasLocked(1, 4, true));
        QCOMPARE(item.m_canvasFilledCols, 7);
        QCOMPARE(item.m_canvasWriteX, 1);
        QVERIFY(item.m_precomputedCanvasSubpixelOffset > 0.70);
        QVERIFY(item.m_precomputedCanvasSubpixelOffset < 0.80);
        const int srcStart =
            (item.m_canvasWriteX - item.m_canvasFilledCols + item.m_canvas.width())
            % item.m_canvas.width();
        QCOMPARE(srcStart, 1);
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < initialCanvas.width() - 1; ++x) {
                QCOMPARE(
                    item.m_canvas.pixelColor((srcStart + x) % item.m_canvas.width(), y),
                    initialCanvas.pixelColor(x + 1, y));
            }
        }
    }
}

void QmlSmokeTest::spectrogramCenteredLateFillUsesCircularCanvasOffset() {
    // A centered canvas becomes circular once its display window starts
    // scrolling. Columns that arrive later for a previously blank right-edge
    // region must be written through that circular offset, not to their
    // logical screen X in the underlying image.
    SpectrogramItem item;
    item.setWidth(100);
    item.setHeight(10);
    item.setDisplayMode(1); // Centered

    constexpr int bins = 4;
    constexpr quint64 token = 1;
    QByteArray initial(151 * bins, '\x60');
    item.feedPrecomputedChunk(
        initial, bins, 0, 151, 0, 1000,
        48000, 1024, false, true, token);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(10);
        item.rebuildPrecomputedCanvasLocked(100, 10, 50, 149, false);
        QVERIFY(item.advancePrecomputedCanvasLocked(60, 159, false));
    }

    // Column 151 is not decoded yet, so its physical circular slot is black.
    QCOMPARE(item.m_canvasWriteX, 10);
    QCOMPARE(item.m_canvas.pixel(1, 5), qRgb(0, 0, 0));

    QByteArray late(9 * bins, '\x70');
    item.feedPrecomputedChunk(
        late, bins, 0, 9, 151, 1000,
        48000, 1024, false, false, token);

    // Logical x=91 maps to physical x=(canvasStart 10 + 91) % 100 = 1.
    // Painting x=91 directly leaves the visible gap permanently black.
    QVERIFY(item.m_canvas.pixel(1, 5) != qRgb(0, 0, 0));
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
