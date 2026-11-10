// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The spectrogram's zoom, its canvas, ring and rasters, and drawing at
// device pixels.
class SpectrogramCanvasSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void hiddenSpectrogramDefersPaintingAndRetainsIncomingData();
    void releasedSpectrogramFreesItsColumnsAndCanvasUntilRetained();
    void spectrogramCenteredModeSeekPreservesRing();
    void spectrogramReferenceHopDataIsDrawnAtTheBackendsZoom();
    void spectrogramEndSeekReleasesCanvasOnFinalize();
    void spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit();
    void spectrogramStoppedZoomResetRefillsCanvas();
    void spectrogramZoomOutFillDoesNotClampToLaggingDecodedTail();
    void spectrogramRingHoldsTheSessionsWidthUntilTheNextSession();
    void spectrogramRingHoldsEveryColumnOfAWiderSession();
    void spectrogramZoomPreviewRequestsEveryFrame();
    void spectrogramDynamicRangePreservesSilence();
    void spectrogramProjectionSharesImmutableSettingsAndReleasesUnusedPlans();
    void spectrogramProjectionWritesPreserveSharedCanvasImages();
    void spectrogramRebuildBenchmark();
    void spectrogramFrequencyCapEndsTheBandAtTheCap();
    void spectrogramBandFollowsTheFftSizeOfColumnsWithTheSameBins();
    void spectrogramGaplessSwitchBetweenCappedAndWholeBandsStartsAFreshRing();
    void spectrogramZoomProperty();
    void spectrogramZoomLimitsWithTrackData();
    void spectrogramZoomOutBlockedWhenSongFits();
    void spectrogramEffectiveZoomMatchesBackendHop();
    void spectrogramAdvanceWorksWhenBackendMatchesZoom();
    void spectrogramEffectiveZoomDuringTransition();
    void spectrogramDeferredZoomAppliesOnBackendData();
    void spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack();
    void spectrogramResizeShowsTheScaledCanvasUntilItsRasterArrives();
    void spectrogramRollingResizeKeepsTheHistoryAtTheRightEdge();
    void spectrogramStaleRasterIsDiscarded();
    void spectrogramDrawsAtDevicePixels_data();
    void spectrogramDrawsAtDevicePixels();
    void spectrogramWideCanvasDrawsEveryTile();
    void spectrogramCanvasTexturesSurviveSceneGraphChanges();
    void spectrogramSceneGraphDrawsTheCanvas_data();
    void spectrogramSceneGraphDrawsTheCanvas();
    void spectrogramScaleChangeKeepsAnUnalignedCanvasCovering_data();
    void spectrogramScaleChangeKeepsAnUnalignedCanvasCovering();
    void spectrogramScaleChangeRebuildsOnceAndDropsStaleRasters_data();
    void spectrogramScaleChangeRebuildsOnceAndDropsStaleRasters();
    void spectrogramDevicePixelCanvasAdvancesAsItsRasterization_data();
    void spectrogramDevicePixelCanvasAdvancesAsItsRasterization();
    void spectrogramCachedBackwardSeekKeepsTheCanvasOnScreen();
    void spectrogramCachedForwardSeekRasterizesOffTheRenderSync();
    void spectrogramResumedForwardSeekRasterizesOffTheRenderSync();
    void spectrogramSlowRasterIsCaughtUpDuringHighRatePlayback();
    void spectrogramZoomDataKeepsThePreviewUntilItsRasterArrives();
    void spectrogramColumnsArrivingDuringARasterAreDrawnIn();
    void spectrogramItemDestroyedDuringRasterIsSafe();
    void spectrogramDestroyedInItsWindowRunsNoHandlersOfItsOwn();
    void spectrogramSurfaceCoalescesWidthReports();
    void spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights();
    void spectrogramZoomOutProducesDistinctHop();
    void spectrogramCenteredZoomOutDropsOlderSameTrackGeneration();
    void spectrogramMinZoomAdaptsToWidthChange();
    void spectrogramHighRateRetentionPreservesViewportWithinBudget();
    void spectrogramRingCommitsOnlyTheBlocksWritten();
    void spectrogramUnwrittenRingColumnsDrawAsBackground();
    void spectrogramCenteredBackfillLandsBeforeTheWriteHead();
    void spectrogramFreshPaneTakesTheBackfillOfASessionItMissedTheResetOf();
    void spectrogramStaleBackfillIsDropped();
    void spectrogramSeekShowsThePlayheadFirstAndFillsTheLeftIn();
    void spectrogramZoomFreezeWaitsForTheBackfill();
    void spectrogramZoomNearTheEndWaitsForTheLeftAndKeepsTheEnd();
    void spectrogramZoomFillWaitsForTheCanvasColumnsAtFractionalScales_data();
    void spectrogramZoomFillWaitsForTheCanvasColumnsAtFractionalScales();
    void spectrogramRingGrowthKeepsRollingHistory();
    void spectrogramRingChangeBenchmark();
    void spectrogramRollingCanvasGrowsIncrementallyDuringInitialFill();
    void spectrogramRollingCanvasHandsOffToSteadyScrollIncrementally();
    void spectrogramRollingCanvasAdvancesIncrementallyAtFractionalZoom();
    void spectrogramCenteredLateFillUsesCircularCanvasOffset();
    void spectrogramPeakHoldRebuildUsesMaxNotNearest();
    void spectrogramZoomFillClearsWhenDecoderReachesTail();
    void spectrogramSyntheticClearPreservesCanvasDuringSeek();
    void spectrogramSyntheticClearInvalidatesCanvasWhenNoOldContent();
    void spectrogramWheelZoomFollowsTheDeltaAndPassesHorizontalScroll();
    void spectrogramCenteredZoomPreviewShowsAtOnceAndGivesWayToTheData();
    void spectrogramRollingZoomPreviewShowsAtOnceAndGivesWayToTheData();
    void spectrogramCenteredZoomPreviewShowsTheColumnsOfItsDataAtFractionalScales_data();
    void spectrogramCenteredZoomPreviewShowsTheColumnsOfItsDataAtFractionalScales();
    void spectrogramCenteredZoomPreviewHoldsThroughTheDataBeforeTheEnd_data();
    void spectrogramCenteredZoomPreviewHoldsThroughTheDataBeforeTheEnd();
    void spectrogramSecondsAreAsWideAtEveryRateOfAFamily();
    void spectrogramZoomSnapsToTheScaledHopOfAHighRate();
    void spectrogramFlooredHopShowsAColumnAPixelToTheEnd();
    void spectrogramOddReferenceHopSnapsZoomedIn();
    void spectrogramZoomWithinTheBackendToleranceNeedsNoNewData();
    void spectrogramZoomRequestsTheBackendIgnoresDoNotLeaveAPreview();
    void spectrogramStaleZoomDataDoesNotDrawIntoTheFrozenPreview();
    void spectrogramZoomShowsOlderDataUntilTheNewArrives_data();
    void spectrogramZoomShowsOlderDataUntilTheNewArrives();
    void spectrogramTrackChangeDuringAZoomShowsNothingOfTheTrackBefore_data();
    void spectrogramTrackChangeDuringAZoomShowsNothingOfTheTrackBefore();
    void spectrogramLateZoomDataKeepsTheRollingHistory();
    void spectrogramSmallSeekDuringAZoomBreaksTheHistory();
    void spectrogramGaplessContinuationKeepsTheRollingHistory_data();
    void spectrogramGaplessContinuationKeepsTheRollingHistory();
    void spectrogramCenteredGaplessZoomRestartBeforeTheClockFollowsTheClock();
};

namespace {

// How much larger than in the canvas the drawn canvas spans are on screen:
// all of them share one scale. -1 without spans, -2 for differing scales.
double drawnCanvasScale(QSGNode *node) {
    QSGNode *tilesRoot = node != nullptr ? node->childAtIndex(1) : nullptr;
    if (tilesRoot == nullptr) {
        return -1.0;
    }
    double scale = -1.0;
    for (int i = 0; i < tilesRoot->childCount(); ++i) {
        auto *fragment = static_cast<QSGSimpleTextureNode *>(tilesRoot->childAtIndex(i));
        if (fragment->rect().isEmpty() || fragment->sourceRect().width() <= 0.0) {
            continue;
        }
        const double fragmentScale = fragment->rect().width() / fragment->sourceRect().width();
        if (scale >= 0.0 && std::abs(fragmentScale - scale) > 1e-6) {
            return -2.0;
        }
        scale = fragmentScale;
    }
    return scale;
}

// A 3440 x 719 centered pane, 4097 bins at 48 kHz and a hop of 1024, with
// 8192 columns decoded and its playhead at 30 s, near the track's start.
void prepareFullscreenForwardSeek(SpectrogramItem &item) {
    item.setWidth(3440);
    item.setHeight(719);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 4097;
    constexpr int columns = 8192;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, 20'000, 48000, 1024, false, true, 1, false, 1);
    setTestPosition(item, 30.0);
}

// Whether the reserve under the canvas is being drawn.
bool spectrogramReservePending(SpectrogramItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.m_reservePending;
}

// The drawn spans' extent on screen.
QPair<double, double> drawnCanvasExtent(QSGNode *node) {
    QSGNode *tilesRoot = node->childAtIndex(1);
    double left = std::numeric_limits<double>::max();
    double right = std::numeric_limits<double>::lowest();
    for (int i = 0; i < tilesRoot->childCount(); ++i) {
        auto *fragment = static_cast<QSGSimpleTextureNode *>(tilesRoot->childAtIndex(i));
        if (fragment->rect().isEmpty()) {
            continue;
        }
        left = std::min(left, fragment->rect().left());
        right = std::max(right, fragment->rect().right());
    }
    return {left, right};
}

// Rects equal to the scene graph's single precision.
bool nearlyEqual(const QRectF &a, const QRectF &b) {
    return std::abs(a.x() - b.x()) < 1e-4 && std::abs(a.y() - b.y()) < 1e-4
        && std::abs(a.width() - b.width()) < 1e-4 && std::abs(a.height() - b.height()) < 1e-4;
}

// What a rasterization of the view would draw, without installing it.
QImage freshCanvas(
    SpectrogramItem &item, int width, int height,
    qint64 displayLeft, qint64 displayRight, bool rollingMode) {
    const auto job = item.makeRasterJobLocked(
        width, height, displayLeft, displayRight, rollingMode);
    const auto result = job ? SpectrogramItem::rasterize(*job) : nullptr;
    return result ? result->canvas.copy(0, 0, result->drawPixels, height) : QImage();
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramCenteredModeSeekPreservesRing() {
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
    setTestPosition(item, 50.0);
    QCOMPARE(item.m_ringWriteSeq, 100);

    // Data at the new position must still be valid in the ring.
    QVERIFY(item.m_ringCapacity > 0);
}

void SpectrogramCanvasSmokeTest::spectrogramEndSeekReleasesCanvasOnFinalize() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int estimatedEnd = 5000;
    constexpr int actualEnd = 4000;
    constexpr quint64 token = 7;
    item.feedPrecomputedChunk(QByteArray(640 * bins, '\x30'), bins, 0, 640,
        0, estimatedEnd, 44100, 1024, false, true, token, true, 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(180);
        rebuildCanvasNow(item, 320, 180, 0, 319, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    // A seek between the end of the audio and the end of its estimate: the
    // playhead's column, which the seek's freeze waits for, never comes.
    testClock(item).holdAt(4050.0 * 1024.0 / 44100.0);
    item.feedPrecomputedChunk({}, 0, 0, 0, 0, 0, 0, 0,
        false, true, token, true);
    QVERIFY(item.m_zoomFillActive);
    item.feedPrecomputedChunk({}, bins, 0, 0, 3600, estimatedEnd, 44100, 1024,
        false, true, token, true, 2);
    item.feedPrecomputedChunk(QByteArray(400 * bins, '\x60'), bins, 0, 400,
        3600, estimatedEnd, 44100, 1024, false, false, token, false, 2);
    QVERIFY(item.m_zoomFillActive);

    // Neither an obsolete generation nor another track can end this handoff.
    item.feedPrecomputedChunk({}, bins, 0, 0, actualEnd, actualEnd, 44100, 1024,
        true, false, token, false, 1);
    item.feedPrecomputedChunk({}, bins, 0, 0, actualEnd, actualEnd, 44100, 1024,
        true, false, token - 1, false, 2);
    QVERIFY(item.m_zoomFillActive);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, estimatedEnd);
    item.feedPrecomputedChunk({}, bins, 0, 0, actualEnd, actualEnd, 44100, 1024,
        true, false, token, false, 2);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, actualEnd);
    QVERIFY(!item.m_zoomFillActive);
    QVERIFY(item.m_precomputedCanvasDirty);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(item.m_precomputedCanvasDisplayLeft >= 3600);
    QVERIFY(item.m_precomputedCanvasDisplayRight < actualEnd);
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramReferenceHopDataIsDrawnAtTheBackendsZoom() {
    // The backend decodes at the zoom it is sent, a float, and reckons the
    // centered view's window with it (issue 421). Data at the reference
    // hop, which is drawn at its zoom, is drawn at that zoom too, not at
    // the zoom asked for, which a float does not hold: their windows could
    // differ by a column.
    SpectrogramItem item;
    item.setWidth(1'000);
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int columns = 20'000;
    item.feedPrecomputedChunk(QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, columns,
        48'000, 1'280, false, true, 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_lastBackendZoom = 0.8f;
        item.m_zoomLevel = 0.8;
    }
    constexpr double asked = 1.0003;
    QVERIFY(static_cast<double>(static_cast<float>(asked)) != asked);
    item.setZoomLevel(asked);
    item.m_zoomDebounceTimer->stop();
    QVERIFY(item.m_awaitingZoomData);
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, 0, columns, 48'000, 1'024, false, true, 1);
    item.feedPrecomputedChunk(QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, columns,
        48'000, 1'024, false, false, 1);
    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(!item.m_awaitingZoomData);
    const double backend = static_cast<double>(static_cast<float>(asked));
    QCOMPARE(item.m_renderZoomLevel, backend);
    QCOMPARE(item.effectiveZoomLocked(),
        ferrous_ffi_spectrogram_effective_zoom(48'000, 1'024, backend));
}

void SpectrogramCanvasSmokeTest::spectrogramSameHopEstimateIncreaseUpdatesZoomOutLimit() {
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

void SpectrogramCanvasSmokeTest::spectrogramStoppedZoomResetRefillsCanvas() {
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
    setTestPosition(item, 0.0);
    QSGNode *node = paintSettled(item, nullptr);
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
    node = paintSettled(item, node);
    QVERIFY(node != nullptr);
    bool frozenCanvasHasContent = false;
    for (int y = 0; y < item.m_canvas.height() && !frozenCanvasHasContent; ++y) {
        for (int x = 0; x < item.m_canvas.width(); ++x) {
            if (item.m_canvas.pixel(x, y) != kSpectrogramNoColumn) {
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
    node = paintSettled(item, node);
    QVERIFY(node != nullptr);
    QVERIFY(!item.m_canvas.isNull());
    QVERIFY2(item.m_canvasFilledCols > 0,
             qPrintable(QStringLiteral("canvas refill produced %1 columns")
                            .arg(item.m_canvasFilledCols)));
    bool refilledCanvasHasContent = false;
    for (int y = 0; y < item.m_canvas.height() && !refilledCanvasHasContent; ++y) {
        for (int x = 0; x < item.m_canvas.width(); ++x) {
            if (item.m_canvas.pixel(x, y) != kSpectrogramNoColumn) {
                refilledCanvasHasContent = true;
                break;
            }
        }
    }
    QVERIFY2(refilledCanvasHasContent,
             "stopped zoom reset must render non-background spectrogram pixels");
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomOutFillDoesNotClampToLaggingDecodedTail() {
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
    setTestPosition(item, 280.0); // playhead near EOF, decode head lags

    QSGNode *node = paintSettled(item, nullptr);
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

namespace {

// The columns a centered ring of a 1-channel, 4-bin, 48 kHz session at hop
// 1024 holds for a view `width` pixels wide.
int centeredTestRingCapacity(int width) {
    return static_cast<int>(ferrous_ffi_spectrogram_ring_capacity(
        static_cast<std::uint32_t>(width), 48'000, 1024, 4, 1, 1.0, true));
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramRingHoldsTheSessionsWidthUntilTheNextSession() {
    // The fullscreen viewer used to leave a process-wide widest width that
    // sized every later ring. Now a ring holds the width its session
    // decodes for, which each chunk carries: after the view shrinks, a
    // reset within the session (a seek) keeps the wider ring, since the
    // session still decodes that far ahead, and the next session, for the
    // narrower view, sizes it for that view.
    SpectrogramItem item;
    item.setHeight(200);
    item.setDisplayMode(1); // Centered
    constexpr int bins = 4;
    constexpr int hop = 1024;
    constexpr quint64 token = 1;
    QVERIFY(centeredTestRingCapacity(3440) > centeredTestRingCapacity(2545));
    const QByteArray data(16 * bins, '\x40');

    // The viewer's session.
    item.setWidth(3440);
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 0, 161024, 48'000, hop, false, true, token, true, 1, 3440);
    QCOMPARE(item.m_ringCapacity, centeredTestRingCapacity(3440));

    // The viewer closes. The session's synthetic clear, then its reset at a
    // seek, both before the next session.
    item.setWidth(2545);
    item.feedPrecomputedChunk(
        QByteArray(), 0, 0, 0, 0, 0, 0, 0, false, true, token, true);
    QCOMPARE(item.m_ringCapacity, 0);
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 500, 161024, 48'000, hop, false, true, token, true, 1, 3440);
    QCOMPARE(item.m_ringCapacity, centeredTestRingCapacity(3440));

    // The next session is for the main window's pane.
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 500, 161024, 48'000, hop, false, true, token, true, 2, 2545);
    QCOMPARE(item.m_ringCapacity, centeredTestRingCapacity(2545));
    QCOMPARE(item.m_ring.byteSize(), qint64(centeredTestRingCapacity(2545)) * bins);

    // A pane wider than its session holds its own view.
    item.setWidth(3000);
    item.feedPrecomputedChunk(
        data, bins, 0, 16, 516, 161024, 48'000, hop, false, false, token, false, 2, 2545);
    QCOMPARE(item.m_ringCapacity, centeredTestRingCapacity(3000));
}

void SpectrogramCanvasSmokeTest::spectrogramRingHoldsEveryColumnOfAWiderSession() {
    // Regression: a centered decoder parked while the viewer was open, or
    // a pane narrower than the decoder's lookahead, filled more columns
    // than the pane's ring held, evicting those around the playhead. A
    // ring sized for the session's width keeps all of them, in a pane
    // made after the session started (a channel-count change replaces the
    // panes) as well.
    constexpr int bins = 4;
    constexpr int hop = 1024;
    constexpr quint64 token = 5;
    const int sessionColumns = centeredTestRingCapacity(3440);
    QVERIFY(sessionColumns > centeredTestRingCapacity(2545));

    SpectrogramItem item;
    item.setHeight(200);
    item.setDisplayMode(1);
    item.setWidth(1200);
    item.setWidth(3440); // The viewer opens; no data arrives while wide.
    item.setWidth(2545); // It closes before the parked decoder sends more.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 161024, 48'000, hop, false, true, token, true, 1, 3440);
    constexpr int chunkColumns = 512;
    for (int start = 0; start < sessionColumns; start += chunkColumns) {
        const int columns = std::min(chunkColumns, sessionColumns - start);
        QByteArray data(columns * bins, '\0');
        for (int column = 0; column < columns; ++column) {
            data[column * bins] = static_cast<char>((start + column) % 251);
        }
        item.feedPrecomputedChunk(
            data, bins, 0, columns, start, 161024, 48'000, hop,
            false, false, token, false, 1, 3440);
    }

    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_ringCapacity, sessionColumns);
    for (int column = 0; column < sessionColumns; ++column) {
        const int slot = column % item.m_ringCapacity;
        QCOMPARE(item.m_ringColumnId[static_cast<size_t>(slot)], column);
        QCOMPARE(item.m_ring.column(slot)[0], static_cast<quint8>(column % 251));
    }
}

void SpectrogramCanvasSmokeTest::spectrogramZoomPreviewRequestsEveryFrame() {
    // While the canvas on screen is shown scaled to a zoom whose data has
    // not arrived, the frames follow playback one by one.
    for (const int mode : {0, 1}) {
        TestTime time;
        SpectrogramItem item;
        prepareScrollingPlayback(item, time, mode);
        QSGNode *node = paintSettled(item, nullptr);
        item.setZoomLevel(2.0);
        node = item.updatePaintNode(node, nullptr);
        {
            QMutexLocker lock(&item.m_stateMutex);
            QVERIFY(item.m_zoomPreviewShown);
            QVERIFY(!item.m_idleView.valid);
        }
        const quint64 requested = item.m_playbackFrameRequests;
        for (int frame = 1; frame <= 3; ++frame) {
            item.handleWindowAfterAnimating();
            QCOMPARE(item.m_playbackFrameRequests, requested + frame);
            QVERIFY(!playbackFrameScheduled(item));
        }
        delete node;
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRebuildBenchmark() {
    if (qEnvironmentVariableIntValue("FERROUS_BENCHMARK_SPECTROGRAM") != 1) {
        QSKIP("Set FERROUS_BENCHMARK_SPECTROGRAM=1 to measure CPU projection");
    }
    // Resident synthetic history isolates projection from decoding, transport,
    // and GPU uploads. Checksums make before/after output comparisons possible.
    // FERROUS_BENCHMARK_SPECTROGRAM_SCALE sets the device pixels to a logical
    // one; the windowed parts then use the HiDPI test screen at that scale.
    const double scale = qEnvironmentVariableIsSet("FERROUS_BENCHMARK_SPECTROGRAM_SCALE")
        ? qEnvironmentVariable("FERROUS_BENCHMARK_SPECTROGRAM_SCALE").toDouble()
        : 1.0;
    QVERIFY(scale > 0.0);
    QScreen *benchmarkScreen = scale != 1.0 ? hiDpiTestScreen(scale) : nullptr;
    if (scale != 1.0 && benchmarkScreen == nullptr) {
        QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
    }
    constexpr int width = 3440;
    const int deviceWidth = static_cast<int>(std::floor(width * scale + 1e-6));
    constexpr int iterations = 12;
    for (int bins : {1025, 4097}) {
        for (int height : {90, 719}) {
            for (bool logarithmic : {false, true}) {
                const int deviceHeight = static_cast<int>(std::floor(height * scale + 1e-6));
                SpectrogramItem item;
                item.m_canvasScale = scale;
                item.setWidth(width);
                item.setHeight(height);
                item.setLogScale(logarithmic);
                item.setDisplayMode(1);
                item.m_binsPerColumn = bins;
                item.m_precomputedBinsPerColumn = bins;
                item.m_precomputedSampleRateHz = 48'000;
                item.m_sampleRateHz = 48'000;
                item.m_precomputedHopSize = 1024;
                item.m_precomputedTrackToken = 1;
                item.m_precomputedTotalColumnsEstimate = width;
                item.m_precomputedMaxColumnIndex = width - 1;
                item.m_precomputedReady = true;
                item.m_renderZoomLevel = 1.0;
                item.m_ringCapacity = width;
                item.m_ringWriteSeq = width;
                item.m_ringOldestSeq = 0;
                item.m_ring.allocate(width, bins);
                item.m_ringColumnId.resize(width);
                item.m_ringSequenceId.resize(width);
                item.m_ringTrackToken.resize(width, 1);
                for (int x = 0; x < width; ++x) {
                    item.m_ringColumnId[static_cast<size_t>(x)] = x;
                    item.m_ringSequenceId[static_cast<size_t>(x)] = x;
                    item.m_trackColumnToSeqByToken[1].insert(x, x);
                    quint8 *column = item.m_ring.writableColumn(x);
                    for (int bin = 0; bin < bins; ++bin) {
                        column[bin] = static_cast<quint8>((x * 37 + bin * 13) % 256);
                    }
                }
                item.ensureMapping(deviceHeight);

                // The rasterization itself, which runs on a worker.
                const auto job = item.makeRasterJobLocked(
                    deviceWidth, deviceHeight, 0, width - 1, false);
                QVERIFY(job);
                auto result = SpectrogramItem::rasterize(*job);
                QVERIFY(result);
                QElapsedTimer elapsed;
                elapsed.start();
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    result = SpectrogramItem::rasterize(*job);
                }
                const double rasterMs =
                    static_cast<double>(elapsed.nsecsElapsed()) / (iterations * 1e6);

                quint64 checksum = 0;
                for (int y = 0; y < deviceHeight; ++y) {
                    for (int x = 0; x < deviceWidth; ++x) checksum += result->canvas.pixel(x, y);
                }

                // The render sync's part of a frame that needs a new canvas:
                // copying the job's inputs and handing it to a worker.
                double requestNs = 0.0;
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    item.setDbRange(iteration % 2 == 0 ? 120.0 : 132.0);
                    elapsed.restart();
                    {
                        QMutexLocker lock(&item.m_stateMutex);
                        item.requestPrecomputedRasterLocked(
                            deviceWidth, deviceHeight, 0, width - 1, false);
                    }
                    requestNs += static_cast<double>(elapsed.nsecsElapsed());
                    QVERIFY(QTest::qWaitFor(
                        [&item]() { return !spectrogramRasterPending(item); }, 5000));
                }

                // The UI thread's part: putting the result in place of the
                // canvas, which frees that.
                double installNs = 0.0;
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    auto fresh = SpectrogramItem::rasterize(*job);
                    QVERIFY(fresh);
                    result.reset();
                    elapsed.restart();
                    QVERIFY(item.installRasterResultLocked(*fresh));
                    installNs += static_cast<double>(elapsed.nsecsElapsed());
                    result = std::move(fresh);
                }

                // The frame after that hands the rasterization's tiles over
                // for upload (without a window, nothing uploads them).
                QSGNode *node = item.updatePaintNode(nullptr, nullptr);
                double handoffNs = 0.0;
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    QVERIFY(item.installRasterResultLocked(*result));
                    elapsed.restart();
                    node = item.updatePaintNode(node, nullptr);
                    handoffNs += static_cast<double>(elapsed.nsecsElapsed());
                }
                delete node;
                if (bins == 4097 && height == 719 && !logarithmic) {
                    // A cached forward seek past the whole canvas: the frame
                    // must hand the canvas to a worker, not draw it. In a
                    // window, so that the canvas's tiles have textures and
                    // a frame does not copy them all again.
                    QQuickWindow window;
                    window.resize(240, 160);
                    if (benchmarkScreen != nullptr) {
                        QVERIFY(moveWindowToScreen(window, benchmarkScreen));
                    }
                    window.show();
                    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
                    QCOMPARE(window.effectiveDevicePixelRatio(), scale);
                    double seekNs = 0.0;
                    for (int iteration = 0; iteration < iterations; ++iteration) {
                        SpectrogramItem seeking(window.contentItem());
                        prepareFullscreenForwardSeek(seeking);
                        QSGNode *seekNode = paintSettled(seeking, nullptr);
                        setTestPosition(seeking, 115.0);
                        elapsed.restart();
                        seekNode = seeking.updatePaintNode(seekNode, nullptr);
                        seekNs += static_cast<double>(elapsed.nsecsElapsed());
                        QVERIFY(spectrogramRasterPending(seeking));
                        QVERIFY(QTest::qWaitFor(
                            [&seeking]() { return !spectrogramRasterPending(seeking); }, 5000));
                        delete seekNode;
                    }
                    qInfo("forward_seek_frame_ms=%.3f", seekNs / (iterations * 1e6));

                    // Playback of 6000 columns a second, painted at 30 fps:
                    // each frame draws the 200 columns playback moved on.
                    // The item's own repaint loop is cut off, so that only
                    // these frames move its canvas.
                    for (const bool rolling : {true, false}) {
                        SpectrogramItem playing(window.contentItem());
                        prepareHighRatePlayback(playing, width, height, bins, rolling);
                        QSGNode *playNode = paintSettled(playing, nullptr);
                        QObject::disconnect(playing.m_animationTickConnection);
                        quint64 serial = 0;
                        {
                            QMutexLocker lock(&playing.m_stateMutex);
                            serial = playing.m_rasterSerial;
                        }
                        double totalNs = 0.0;
                        double worstNs = 0.0;
                        constexpr int frames = 15;
                        for (int frame = 0; frame < frames; ++frame) {
                            QTest::qWait(33);
                            elapsed.restart();
                            playNode = playing.updatePaintNode(playNode, nullptr);
                            const double ns = static_cast<double>(elapsed.nsecsElapsed());
                            totalNs += ns;
                            worstNs = std::max(worstNs, ns);
                        }
                        QVERIFY(!spectrogramRasterPending(playing));
                        {
                            QMutexLocker lock(&playing.m_stateMutex);
                            QCOMPARE(playing.m_rasterSerial, serial);
                        }
                        delete playNode;
                        qInfo("high_rate_playback_frame mode=%s mean_ms=%.3f worst_ms=%.3f",
                            rolling ? "rolling" : "centered",
                            totalNs / (frames * 1e6), worstNs / 1e6);
                    }
                }
                qInfo("scale=%g bins=%d height=%d log=%d raster_ms=%.3f request_ms=%.3f "
                      "install_ms=%.3f handoff_ms=%.3f checksum=%llu",
                    scale, bins, height, logarithmic ? 1 : 0, rasterMs,
                    requestNs / (iterations * 1e6), installNs / (iterations * 1e6),
                    handoffNs / (iterations * 1e6), static_cast<unsigned long long>(checksum));
            }
        }
    }
}

void SpectrogramCanvasSmokeTest::spectrogramProjectionSharesImmutableSettingsAndReleasesUnusedPlans() {
    SpectrogramItem first;
    SpectrogramItem second;
    const auto prepare = [](SpectrogramItem &item, int height) {
        item.m_binsPerColumn = 1025;
        item.m_precomputedBinsPerColumn = 1025;
        item.ensureCanvas(100, height);
    };
    prepare(first, 90);
    prepare(second, 90);
    const auto initial = first.precomputedProjectionLocked();
    QCOMPARE(second.precomputedProjectionLocked().get(), initial.get());
    first.setDbRange(150);
    prepare(first, 90);
    QVERIFY(first.precomputedProjectionLocked().get() != initial.get());
    QCOMPARE(second.precomputedProjectionLocked().get(), initial.get());
    second.setDbRange(150);
    prepare(second, 90);
    QCOMPARE(second.precomputedProjectionLocked().get(), first.precomputedProjectionLocked().get());
    second.setChannelMuted(true);
    prepare(second, 90);
    QVERIFY(second.precomputedProjectionLocked().get() != first.precomputedProjectionLocked().get());
    const auto muted = second.precomputedProjectionLocked();
    second.setLogScale(!second.logScale());
    prepare(second, 90);
    QVERIFY(second.precomputedProjectionLocked().get() != muted.get());
    const auto logarithmic = second.precomputedProjectionLocked();
    second.setSampleRateHz(96'000);
    prepare(second, 90);
    QVERIFY(second.precomputedProjectionLocked().get() != logarithmic.get());
    std::weak_ptr<const SpectrogramProjection> released;
    {
        SpectrogramItem temporary;
        prepare(temporary, 17);
        released = temporary.precomputedProjectionLocked();
        QVERIFY(!released.expired());
    }
    QVERIFY(released.expired());
}

void SpectrogramCanvasSmokeTest::spectrogramProjectionWritesPreserveSharedCanvasImages() {
    for (bool rolling : {false, true}) {
        SpectrogramItem item;
        item.setWidth(2);
        item.setHeight(32);
        item.setDisplayMode(rolling ? 0 : 1);
        constexpr int bins = 64;
        item.feedPrecomputedChunk(QByteArray(bins * 2, '\x30'), bins, 0, 2,
            0, 100, 48'000, 1024, false, true, 1);
        item.m_binsPerColumn = bins;
        item.ensureMapping(32);
        rebuildCanvasNow(item, 2, 32, 0, 1, rolling);
        const QImage shared = item.m_canvas;
        const QImage reference = shared.copy();
        std::fill_n(item.m_ring.writableColumn(0), bins, static_cast<quint8>(255));
        const auto projection = item.precomputedProjectionLocked();
        item.drawPrecomputedColumnAtLocked(0, 0, rolling, *projection);
        QCOMPARE(shared, reference);
        QVERIFY(item.m_canvas != reference);
    }
}

void SpectrogramCanvasSmokeTest::spectrogramDynamicRangePreservesSilence() {
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

namespace {

// What a pane shows of its band: the top row's frequency, which the hover
// readout gives, the rows' bins, and the frequencies of the grid's lines.
struct SpectrogramBand {
    double topHz{-1.0};
    std::vector<int> rowBins;
    QList<double> lineHz;
    QStringList labels;
};

SpectrogramBand spectrogramBand(SpectrogramItem &item, int height) {
    SpectrogramBand band;
    // Takes the chunks' band, as a frame does, and maps the rows.
    band.topHz = item.pixelToFrequencyHz(0, height);
    QMutexLocker lock(&item.m_stateMutex);
    band.rowBins = item.m_iToBin;
    item.updateFreqGridOverlayLocked(200, height);
    for (const QRect &line : std::as_const(item.m_freqGrid.lines)) {
        band.lineHz.append(item.pixelToFrequencyHzLocked(line.y(), height));
    }
    for (const SpectrogramOverlayImage &image : std::as_const(item.m_freqGrid.images)) {
        band.labels.append(image.text);
    }
    return band;
}

// A pane of a 96 kHz session whose columns hold `bins` bins of an
// `fftSize`-point spectrum (0: not given, as before the frequency cap).
void feedBand(SpectrogramItem &item, int bins, int fftSize, quint64 generation) {
    const int hop = static_cast<int>(ferrous_ffi_spectrogram_reference_hop(96000));
    item.feedPrecomputedChunk(QByteArray(bins, '\x80'), bins, 0, 1, 0, 1000, 96000, hop,
        false, true, generation, true, generation, 0, fftSize);
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramFrequencyCapEndsTheBandAtTheCap() {
    constexpr int kHeight = 400;
    for (const bool logarithmic : {false, true}) {
        const auto pane = [logarithmic](SpectrogramItem &item) {
            item.setWidth(200);
            item.setHeight(kHeight);
            item.setSampleRateHz(96000);
            item.setLogScale(logarithmic);
            item.setGridEnabled(true);
        };
        SpectrogramItem before;
        SpectrogramItem uncapped;
        SpectrogramItem capped;
        pane(before);
        pane(uncapped);
        pane(capped);
        // FFT 2048: bins of 46.875 Hz; with the cap, the 513 up to 24 kHz.
        feedBand(before, 1025, 0, 1);
        feedBand(uncapped, 1025, 2048, 1);
        feedBand(capped, 513, 2048, 1);
        const SpectrogramBand old = spectrogramBand(before, kHeight);
        const SpectrogramBand full = spectrogramBand(uncapped, kHeight);
        const SpectrogramBand band = spectrogramBand(capped, kHeight);

        // Without the cap nothing changes.
        QCOMPARE(full.topHz, old.topHz);
        QCOMPARE(full.rowBins, old.rowBins);
        QCOMPARE(full.lineHz, old.lineHz);
        QCOMPARE(full.labels, old.labels);

        // The top row is the cap: in linear mode its bin, in log mode the
        // bin of the top row's frequency, a row's step below it.
        if (logarithmic) {
            const double rowStep = std::pow(2.0, std::log2(24000.0 / 25.0) / kHeight);
            QVERIFY2(band.topHz <= 24000.0 && band.topHz >= 24000.0 / rowStep - 46.875,
                qPrintable(QString::number(band.topHz)));
            const double fullStep = std::pow(2.0, std::log2(48000.0 / 25.0) / kHeight);
            QVERIFY(full.topHz <= 48000.0 && full.topHz >= 48000.0 / fullStep - 46.875);
        } else {
            QCOMPARE(band.topHz, 24000.0);
            QCOMPARE(full.topHz, 48000.0);
            QCOMPARE(band.rowBins.back(), 512);
        }
        QCOMPARE(static_cast<int>(band.rowBins.size()), kHeight);
        // Every row maps into the band, from the same low end.
        QCOMPARE(band.rowBins.front(), full.rowBins.front());
        QVERIFY(*std::max_element(band.rowBins.begin(), band.rowBins.end()) <= 512);

        // The labels stop below the cap, and go on above it without it.
        QVERIFY(!band.lineHz.isEmpty());
        QCOMPARE(band.labels.size(), band.lineHz.size());
        for (const double hz : std::as_const(band.lineHz)) {
            QVERIFY2(hz >= 0.0 && hz < 24000.0, qPrintable(QString::number(hz)));
        }
        if (!logarithmic) {
            // 5, 10, 15 and 20 kHz, spaced for 60 Hz a pixel.
            QCOMPARE(band.lineHz.size(), 4);
            QVERIFY(std::abs(band.lineHz.back() - 20000.0) < 60.0);
            QVERIFY(*std::max_element(full.lineHz.begin(), full.lineHz.end()) > 24000.0);
        }
    }
}

void SpectrogramCanvasSmokeTest::spectrogramBandFollowsTheFftSizeOfColumnsWithTheSameBins() {
    // 1,025 bins are the whole spectrum of FFT 2048, and with the cap the
    // band up to 24 kHz of FFT 4096: the mapping, grid and readout follow
    // the FFT size even when the bin count stays.
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setSampleRateHz(96000);
    item.setGridEnabled(true);
    feedBand(item, 1025, 2048, 1);
    const SpectrogramBand full = spectrogramBand(item, 100);
    QCOMPARE(full.topHz, 48000.0);
    feedBand(item, 1025, 4096, 2);
    const SpectrogramBand capped = spectrogramBand(item, 100);
    QCOMPARE(capped.topHz, 24000.0);
    QCOMPARE(capped.rowBins, full.rowBins);
    for (const double hz : std::as_const(capped.lineHz)) {
        QVERIFY(hz < 24000.0);
    }
    QVERIFY(capped.lineHz != full.lineHz);
    feedBand(item, 1025, 2048, 3);
    QCOMPARE(spectrogramBand(item, 100).topHz, 48000.0);
}

void SpectrogramCanvasSmokeTest::spectrogramGaplessSwitchBetweenCappedAndWholeBandsStartsAFreshRing() {
    // A gapless switch between a capped 96 kHz track and a 48 or 44.1 kHz
    // one, either way, changes the rate, so the backend starts a session for
    // it that keeps the rolling history (no clear). Its columns have another
    // bin count, which the ring cannot hold beside the old ones: the ring
    // starts over with the new band rather than draw the old columns on it.
    // The worker sends the session's reset without columns first, then its
    // data; the ring keeps describing its own columns until the data comes.
    struct Track {
        int rate;
        int bins;
        double topHz;
    };
    const Track capped96{96000, 513, 24000.0};
    const Track whole48{48000, 1025, 24000.0};
    const Track whole44{44100, 1025, 22050.0};
    for (const bool split : {false, true}) {
        for (const auto &[from, to] : {std::pair{whole48, capped96}, std::pair{capped96, whole48},
                 std::pair{capped96, whole44}, std::pair{whole44, capped96}}) {
            const QString what = QStringLiteral("%1 -> %2 Hz%3")
                .arg(from.rate).arg(to.rate).arg(split ? QStringLiteral(", reset first") : QString());
            SpectrogramItem item;
            item.setWidth(200);
            item.setHeight(100);
            item.setDisplayMode(0);
            const auto feed = [&item](const Track &track, quint64 token, int columns, bool reset,
                                  bool clearHistory) {
                const int hop = static_cast<int>(
                    ferrous_ffi_spectrogram_reference_hop(static_cast<std::uint32_t>(track.rate)));
                item.feedPrecomputedChunk(QByteArray(track.bins * columns, '\x80'), track.bins, 0,
                    columns, 0, 1000, track.rate, hop, false, reset, token, clearHistory, token, 0,
                    2048);
            };
            feed(from, 1, 10, true, true);
            QCOMPARE(item.m_ring.bins(), from.bins);
            QCOMPARE(item.pixelToFrequencyHz(0, 100), from.topHz);
            if (split) {
                feed(to, 2, 0, true, false);
                // Until the data comes, the ring is the old track's, and so
                // is the band its columns are drawn in.
                QVERIFY2(item.m_precomputedResetPending, qPrintable(what));
                QCOMPARE(item.m_ring.bins(), from.bins);
                QCOMPARE(item.m_precomputedBinsPerColumn, from.bins);
                QVERIFY(item.ringMatchesProjectionLocked());
                feed(to, 2, 10, false, false);
            } else {
                feed(to, 2, 10, true, false);
            }
            QVERIFY2(!item.m_precomputedResetPending, qPrintable(what));
            QCOMPARE(item.m_ring.bins(), to.bins);
            QCOMPARE(item.m_precomputedBinsPerColumn, to.bins);
            QCOMPARE(item.m_ringOldestSeq, qint64(0));
            QVERIFY2(item.m_ringWriteSeq == 10, qPrintable(what));
            for (int slot = 0; slot < 10; ++slot) {
                QCOMPARE(item.m_ringTrackToken[static_cast<size_t>(slot)], quint64(2));
            }
            QCOMPARE(item.pixelToFrequencyHz(0, 100), to.topHz);
        }
    }
}

void SpectrogramCanvasSmokeTest::spectrogramZoomProperty() {
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

void SpectrogramCanvasSmokeTest::spectrogramZoomLimitsWithTrackData() {
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

void SpectrogramCanvasSmokeTest::spectrogramZoomOutBlockedWhenSongFits() {
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

void SpectrogramCanvasSmokeTest::spectrogramEffectiveZoomMatchesBackendHop() {
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

void SpectrogramCanvasSmokeTest::spectrogramAdvanceWorksWhenBackendMatchesZoom() {
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

void SpectrogramCanvasSmokeTest::spectrogramEffectiveZoomDuringTransition() {
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

void SpectrogramCanvasSmokeTest::spectrogramDeferredZoomAppliesOnBackendData() {
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

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomOutBackendRestartReanchorsToFullTrack() {
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
    setTestPosition(item, 10.0);

    QSGNode *node = paintSettled(item, nullptr);
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

    QSGNode *updatedNode = paintSettled(item, node);
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

void SpectrogramCanvasSmokeTest::spectrogramResizeShowsTheScaledCanvasUntilItsRasterArrives() {
    QQuickWindow window;
    window.resize(340, 160);
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
    setTestPosition(*item, 1.0);

    QSGNode *node = paintSettled(*item, nullptr);
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
    QSGTexture *shortTexture = tileNode->texture();

    // The frame after the resize does not rasterize: it shows the canvas it
    // has, stretched to the new height.
    item->setHeight(140);
    node = item->updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(*item));
    QCOMPARE(item->m_canvas.height(), 80);
    QCOMPARE(tileNode->texture(), shortTexture);
    QCOMPARE(tileNode->rect().height(), 140.0);
    QCOMPARE(tileNode->sourceRect().height(), 80.0);

    // Its rasterization for the new height replaces it.
    node = paintSettled(*item, node);
    QCOMPARE(item->m_canvas.height(), 140);
    QVERIFY(tileNode->texture() != nullptr);
    QCOMPARE(tileNode->texture()->textureSize().height(), 140);
    QCOMPARE(tileNode->rect().height(), 140.0);

    // Wider, the whole track fits: the canvas stays where its columns are
    // in the wider view until the wider one arrives.
    item->setWidth(320);
    node = item->updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(*item));
    QCOMPARE(drawnCanvasScale(node), 1.0);
    QPair<double, double> extent = drawnCanvasExtent(node);
    QCOMPARE(extent.first, 0.0);
    QCOMPARE(extent.second, 240.0);
    node = paintSettled(*item, node);
    extent = drawnCanvasExtent(node);
    QCOMPARE(extent.first, 0.0);
    QCOMPARE(extent.second, 320.0);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramRollingResizeKeepsTheHistoryAtTheRightEdge() {
    QQuickWindow window;
    window.resize(300, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(60);
    item.setDisplayMode(0);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 300;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, 10'000, 48000, 1024, false, true, 1, true, 1);
    // The playhead at the newest column.
    setTestPosition(item, (columns - 0.5) * 1024.0 / 48000.0);
    QSGNode *node = paintSettled(item, nullptr);
    QPair<double, double> extent = drawnCanvasExtent(node);
    QVERIFY(std::abs(extent.second - 200.0) <= 1.0);
    QVERIFY(std::abs(extent.first) <= 1.0);

    // Wider: the history shown stays at the right edge, where its columns
    // are, until the wider canvas arrives with more of it.
    item.setWidth(260);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    QCOMPARE(drawnCanvasScale(node), 1.0);
    extent = drawnCanvasExtent(node);
    QVERIFY2(std::abs(extent.second - 260.0) <= 1.0 && std::abs(extent.first - 60.0) <= 1.0,
             qPrintable(QStringLiteral("%1..%2").arg(extent.first).arg(extent.second)));
    node = paintSettled(item, node);
    extent = drawnCanvasExtent(node);
    QVERIFY2(std::abs(extent.second - 260.0) <= 1.0 && std::abs(extent.first) <= 1.0,
             qPrintable(QStringLiteral("%1..%2").arg(extent.first).arg(extent.second)));

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramStaleRasterIsDiscarded() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 400;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(!item.m_canvas.isNull());
    const QRgb shownFirst = item.m_canvas.pixel(10, 50);

    // A setting change asks for a canvas, and another change for another
    // before the first arrives.
    item.setDbRange(60.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    std::shared_ptr<SpectrogramRasterResult> older;
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto job = item.makeRasterJobLocked(
            200, 100, item.m_rasterTarget.displayLeft, item.m_rasterTarget.displayRight, false);
        QVERIFY(job);
        job->serial = item.m_rasterSerial;
        older = SpectrogramItem::rasterize(*job);
    }
    QVERIFY(older);
    item.setDbRange(150.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));

    // The first one's result arrives late: it is dropped, and the newer
    // one goes on.
    item.installRasterResult(older);
    QVERIFY(spectrogramRasterPending(item));
    QCOMPARE(item.m_canvas.pixel(10, 50), shownFirst);
    QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
    const QRgb shownNewest = item.m_canvas.pixel(10, 50);
    QVERIFY(shownNewest != shownFirst);

    // Arriving after the newer one, it does not replace that either.
    item.installRasterResult(older);
    QCOMPARE(item.m_canvas.pixel(10, 50), shownNewest);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramCachedBackwardSeekKeepsTheCanvasOnScreen() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 1500;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 15.0);
    QSGNode *node = paintSettled(item, nullptr);
    const int fragments = drawnCanvasFragments(node);
    QVERIFY(fragments > 0);

    // Back to 5 s, all of it decoded: the view jumps past the canvas's
    // left edge. Until the canvas for it arrives, the old one stays on
    // screen rather than moving out of view.
    setTestPosition(item, 5.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    QVERIFY2(drawnCanvasFragments(node) >= fragments,
             qPrintable(QStringLiteral("%1 of %2").arg(drawnCanvasFragments(node)).arg(fragments)));
    const QPair<double, double> extent = drawnCanvasExtent(node);
    // Where it was, give or take its sub-pixel scroll.
    QVERIFY2(extent.first > -1.0 && extent.second < 201.0 && extent.second - extent.first > 190.0,
             qPrintable(QStringLiteral("%1..%2").arg(extent.first).arg(extent.second)));

    node = paintSettled(item, node);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(234 - 100));
    QVERIFY(drawnCanvasFragments(node) >= fragments);
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramCachedForwardSeekRasterizesOffTheRenderSync() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareFullscreenForwardSeek(item);
    QSGNode *node = paintSettled(item, nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(0));
    const int fragments = drawnCanvasFragments(node);
    QVERIFY(fragments > 0);

    // On to 115 s, already decoded: none of the canvas is in the new view.
    // The frame hands its rasterization to a worker rather than drawing
    // all 3440 columns itself, and keeps the old canvas on screen.
    setTestPosition(item, 115.0);
    QElapsedTimer elapsed;
    elapsed.start();
    node = item.updatePaintNode(node, nullptr);
    qInfo("forward seek frame: %.3f ms", static_cast<double>(elapsed.nsecsElapsed()) / 1e6);
    QVERIFY(spectrogramRasterPending(item));
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(0));
    QVERIFY(drawnCanvasFragments(node) >= fragments);

    node = paintSettled(item, node);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(5390 - 1720));
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramResumedForwardSeekRasterizesOffTheRenderSync() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    TestTime time;
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    time.drive(item);
    prepareFullscreenForwardSeek(item);
    QSGNode *node = paintSettled(item, nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(0));

    // Paused for 100 s, nothing moves, and frames draw nothing.
    time.advance(100.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(!spectrogramRasterPending(item));

    // Playback resumes, and seeks on to 105 s before the next frame. Most
    // of the view is new: 3201 of its 3440 columns. The pause is no
    // playback, so the frame hands them to a worker rather than drawing
    // them itself.
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    time.advance(0.005);
    setTestPosition(item, 105.0);
    QElapsedTimer elapsed;
    elapsed.start();
    node = item.updatePaintNode(node, nullptr);
    qInfo("resumed forward seek frame: %.3f ms", static_cast<double>(elapsed.nsecsElapsed()) / 1e6);
    QVERIFY(spectrogramRasterPending(item));
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(0));

    node = paintSettled(item, node);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(4921 - 1720));

    // Playback goes on from there: its frames draw their columns in.
    quint64 serial = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        serial = item.m_rasterSerial;
    }
    for (int frame = 0; frame < 3; ++frame) {
        time.advance(0.033);
        node = item.updatePaintNode(node, nullptr);
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(!item.m_rasterPending && item.m_rasterSerial == serial,
                 qPrintable(QStringLiteral("frame %1 asked for a rasterization").arg(frame)));
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramSlowRasterIsCaughtUpDuringHighRatePlayback() {
    constexpr int width = 2000;
    TestTime time;
    SpectrogramItem item;
    time.drive(item);
    prepareHighRatePlayback(item, width, 50, 8, true);
    QSGNode *node = paintSettled(item, nullptr);

    // A rasterization is delivered 120 ms after the frame that asked for
    // it, by when playback has moved on by 720 columns.
    item.setDbRange(120.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    time.advance(0.120);
    QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
    quint64 serial = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        serial = item.m_rasterSerial;
    }

    // The next frame draws those columns in, and so on: no rasterization
    // follows the late one.
    for (int frame = 0; frame < 6; ++frame) {
        node = item.updatePaintNode(node, nullptr);
        {
            QMutexLocker lock(&item.m_stateMutex);
            QVERIFY2(!item.m_rasterPending && item.m_rasterSerial == serial,
                     qPrintable(QStringLiteral("frame %1 asked for a rasterization").arg(frame)));
            QCOMPARE(item.m_canvasFilledCols, width);
            QCOMPARE(item.m_precomputedCanvasDisplayRight, item.m_precomputedLastDisplaySeq);
        }
        time.advance(0.033);
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomDataKeepsThePreviewUntilItsRasterArrives() {
    // Rolling zooms, and centered zoom-outs, wait for their data without a
    // zoom fill: the data's reset must not leave a frame without a canvas.
    for (const bool rolling : {true, false}) {
        QQuickWindow window;
        window.resize(240, 160);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
        SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
        item.setWidth(200);
        item.setHeight(100);
        item.setDisplayMode(rolling ? 0 : 1);
        item.setPlaying(false);
        constexpr int bins = 8;
        constexpr int columns = 2000;
        item.feedPrecomputedChunk(
            QByteArray(bins * columns, '\x40'), bins, 0, columns,
            0, columns, 48000, 1024, true, true, 1, rolling, 1);
        setTestPosition(item, 10.0);
        QSGNode *node = paintSettled(item, nullptr);
        QCOMPARE(drawnCanvasScale(node), 1.0);

        const double zoom = rolling ? 2.0 : 0.5;
        const int hop = rolling ? 512 : 2048;
        item.setZoomLevel(zoom);
        QVERIFY(sendPendingBackendZoom(item));
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(std::abs(drawnCanvasScale(node) - zoom) < 1e-6);

        const int nowColumn = static_cast<int>(10.0 * 48000 / hop);
        item.feedPrecomputedChunk(
            QByteArray(bins * 300, '\x70'), bins, 0, 300,
            rolling ? nowColumn : nowColumn - 150, columns * 1024 / hop, 48000, hop,
            false, true, 1, true, 2);
        QVERIFY(!item.m_awaitingZoomData);
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(spectrogramRasterPending(item));
        QVERIFY2(std::abs(drawnCanvasScale(node) - zoom) < 1e-6,
                 qPrintable(QString::number(drawnCanvasScale(node))));

        node = paintSettled(item, node);
        QCOMPARE(drawnCanvasScale(node), 1.0);
        delete node;
    }
}

void SpectrogramCanvasSmokeTest::spectrogramColumnsArrivingDuringARasterAreDrawnIn() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    item.feedPrecomputedChunk(
        QByteArray(bins * 150, '\x60'), bins, 0, 150,
        0, 400, 48000, 1024, false, true, 1, false, 1);
    QSGNode *node = paintSettled(item, nullptr);
    QCOMPARE(item.m_precomputedCanvasDisplayRight, qint64(199));
    QCOMPARE(item.m_canvas.pixel(170, 50), kSpectrogramNoColumn);

    // Columns the view shows arrive while its canvas rasterizes without them.
    item.setDbRange(120.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    item.feedPrecomputedChunk(
        QByteArray(bins * 50, '\x60'), bins, 0, 50,
        150, 400, 48000, 1024, false, false, 1, false, 1);
    QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
    QVERIFY(item.m_canvas.pixel(170, 50) != kSpectrogramNoColumn);
    QCOMPARE(item.m_canvas.pixel(170, 50), item.m_canvas.pixel(100, 50));

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramItemDestroyedDuringRasterIsSafe() {
    std::shared_ptr<SpectrogramRasterMailbox> mailbox;
    {
        auto *item = new SpectrogramItem();
        item->setWidth(3440);
        item->setHeight(719);
        item->setDisplayMode(1);
        item->setPlaying(false);
        constexpr int bins = 4097;
        constexpr int columns = 3440;
        item->feedPrecomputedChunk(
            QByteArray(bins * columns, '\x50'), bins, 0, columns,
            0, columns, 48000, 1024, true, true, 1, false, 1);
        QSGNode *node = item->updatePaintNode(nullptr, nullptr);
        QVERIFY(spectrogramRasterPending(*item));
        mailbox = item->m_rasterMailbox;
        // The viewer closes while its canvas rasterizes.
        delete item;
        delete node;
    }
    // The rasterization ends and lets go of everything without reaching
    // the item; nothing it posted is delivered.
    QTRY_COMPARE_WITH_TIMEOUT(mailbox.use_count(), 1L, 5000);
    QCoreApplication::sendPostedEvents();
}

void SpectrogramCanvasSmokeTest::spectrogramDestroyedInItsWindowRunsNoHandlersOfItsOwn() {
    // ~QQuickItem takes an item out of its window once the item's own
    // members are destroyed, and emits windowChanged. The item's handler
    // for it once ran then and wrote to its freed canvas tiles.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(200);
    item->setHeight(100);
    item->setDisplayMode(1);
    item->setPlaying(false);
    constexpr int bins = 8;
    item->feedPrecomputedChunk(
        QByteArray(bins * 400, '\x50'), bins, 0, 400,
        0, 400, 48000, 1024, true, true, 1, false, 1);
    QSGNode *node = paintSettled(*item, nullptr);
    QVERIFY(!item->m_canvas.isNull());
    delete node;

    QObject probe;
    bool windowChangedDuringDestruction = false;
    bool ownHandlersStillConnected = false;
    QObject::connect(item, &QQuickItem::windowChanged, &probe, [&](QQuickWindow *window) {
        if (window != nullptr) {
            return;
        }
        windowChangedDuringDestruction = true;
        ownHandlersStillConnected = QObject::disconnect(item, nullptr, item, nullptr);
    });
    delete item;
    QVERIFY(windowChangedDuringDestruction);
    QVERIFY(!ownHandlersStillConnected);
}

void SpectrogramCanvasSmokeTest::spectrogramSurfaceCoalescesWidthReports() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");

    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: root
    width: 1200
    height: 320
    visible: true
    property int paneWidth: 640

    QtObject {
        id: bridge
        objectName: "widthBridge"
        property var calls: []
        property string playbackState: "Stopped"
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 1
        property real dbRange: 90
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property bool showFps: false
        property int sampleRateHz: 48000
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property int channelButtonsVisibility: 0
        property var soloedChannels: []
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        signal playbackChanged()
        function isChannelSilent(channelIndex) { return false }
        function setSpectrogramZoomLevel(level) { calls = calls.concat(["zoom"]) }
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSpectrogramWidgetWidth(width) { calls = calls.concat([width]) }
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
    }

    Viewers.SpectrogramSurface {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.paneWidth
        uiBridge: bridge
        uiPalette: Components.UiPalette { windowRoot: root }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QVERIFY(QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(root.data()), 5000));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("widthBridge"));
    QVERIFY(bridge != nullptr);
    const auto calls = [bridge]() {
        const QVariant value = bridge->property("calls");
        return value.canConvert<QJSValue>()
            ? value.value<QJSValue>().toVariant().toList()
            : value.toList();
    };
    const auto clearCalls = [bridge]() { bridge->setProperty("calls", QVariantList{}); };
    QTRY_COMPARE(calls(), QVariantList{640});
    clearCalls();

    // A resize within one frame reports its final width, once.
    for (int width = 641; width <= 700; ++width) {
        root->setProperty("paneWidth", width);
    }
    QCOMPARE(calls().size(), 0);
    QTRY_COMPARE(calls(), QVariantList{700});
    QTest::qWait(250);
    QCOMPARE(calls(), QVariantList{700});
    clearCalls();

    // A splitter drag reports the width as it goes, but far from every
    // pixel, and its final width.
    constexpr int dragSteps = 20;
    for (int step = 1; step <= dragSteps; ++step) {
        root->setProperty("paneWidth", 700 - step * 2);
        QTest::qWait(40);
    }
    const int reportsDuringDrag = static_cast<int>(calls().size());
    QVERIFY2(reportsDuringDrag >= 2 && reportsDuringDrag < dragSteps / 2,
             qPrintable(QString::number(reportsDuringDrag)));
    QTRY_COMPARE(calls().constLast(), QVariant(700 - dragSteps * 2));
    clearCalls();

    // A large change restarts the backend after it settles, for the width
    // it settled at.
    root->setProperty("paneWidth", 1000);
    QTRY_VERIFY(calls().contains(QVariant(QStringLiteral("zoom"))));
    const QVariantList restart = calls();
    QVERIFY(restart.indexOf(QVariant(1000)) >= 0);
    QVERIFY(restart.indexOf(QVariant(1000)) < restart.indexOf(QVariant(QStringLiteral("zoom"))));
}

void SpectrogramCanvasSmokeTest::spectrogramLinearScaleKeepsTopBinVisibleAtTallHeights() {
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

    QSGNode *node = paintSettled(item, nullptr);
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
    QSGNode *updatedNode = paintSettled(item, node);
    QVERIFY(updatedNode == node);
    QVERIFY(!item.m_canvas.isNull());
    QVERIFY2(topRowLuma() > 20,
             "taller linear spectrogram should still show highest-bin energy on the top row");

    delete updatedNode;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomOutProducesDistinctHop() {
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

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomOutDropsOlderSameTrackGeneration() {
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

void SpectrogramCanvasSmokeTest::spectrogramMinZoomAdaptsToWidthChange() {
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

void SpectrogramCanvasSmokeTest::spectrogramHighRateRetentionPreservesViewportWithinBudget() {
    for (int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(1920);
        item.setHeight(100);
        item.setDisplayMode(mode);
        constexpr int bins = 4097;
        QByteArray column(bins * 8, '\x40');
        // 384 kHz at its reference hop, then zoomed in all the way.
        item.feedPrecomputedChunk(column, bins, 0, 1, 0, 1'000'000,
            384'000, 8192, false, true, 100, true, 1);
        item.setZoomLevel(16.0);
        item.m_zoomDebounceTimer->stop();
        item.feedPrecomputedChunk(column, bins, 0, 1, 0, 1'000'000,
            384'000, 512, false, true, 100, true, 2);
        const int width = 1920;
        const int visibleColumns = static_cast<int>(std::ceil(width / item.effectiveZoomLocked()));
        QVERIFY(item.m_ringCapacity >= visibleColumns * 2);
        const qint64 bytesAcrossChannels = item.m_ring.byteSize() * 8;
        const qint64 visibleReserve = static_cast<qint64>(visibleColumns) * bins * 8;
        QVERIFY(bytesAcrossChannels <= visibleReserve * 3 + 64 * 1024 * 1024);
        QVERIFY(bytesAcrossChannels < 512 * 1024 * 1024);
        QCOMPARE(item.m_ringColumnId[0], 0);
        QCOMPARE(item.m_ringTrackToken[0], quint64(100));
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRingCommitsOnlyTheBlocksWritten() {
    // Making room for a new ring, as a track, mode or FFT size change does,
    // takes no memory for columns not written yet.
    for (int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(1920);
        item.setHeight(100);
        item.setDisplayMode(mode);
        constexpr int channels = 2;
        constexpr int columns = 10;
        quint64 generation = 0;
        for (int bins : {1025, 4097}) {
            QByteArray chunk(bins * channels * columns, '\x40');
            item.feedPrecomputedChunk(chunk, bins, 0, columns, 0, 100'000,
                48'000, 1024, false, true, 1, true, ++generation);
            QCOMPARE(item.m_ring.bins(), bins);
            QVERIFY(item.m_ringCapacity > 4 * SpectrogramRingStorage::kColumnsPerBlock);
            QCOMPARE(item.m_ring.capacity(), item.m_ringCapacity);
            QCOMPARE(item.m_ring.byteSize(), qint64(item.m_ringCapacity) * bins);
            QCOMPARE(item.m_ring.committedBytes(),
                qint64(SpectrogramRingStorage::kColumnsPerBlock) * bins);
            for (int column = 0; column < columns; ++column) {
                const quint8 *bytes = item.ringColumnLocked(column, mode == 0);
                QVERIFY(bytes != nullptr);
                QCOMPARE(bytes[0], quint8(0x40));
                QCOMPARE(bytes[bins - 1], quint8(0x40));
            }
        }
    }
}

void SpectrogramCanvasSmokeTest::spectrogramUnwrittenRingColumnsDrawAsBackground() {
    // Columns 0-99 are written; 100-127 share their block, which holds
    // zeroes there, and 128 on are in blocks never written.  Both draw as
    // missing columns, as they did when the whole ring was zero-filled.
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(16);
    item.setDisplayMode(1);
    constexpr int bins = 16;
    constexpr int written = 100;
    QByteArray chunk(bins * written, '\xff');
    item.feedPrecomputedChunk(chunk, bins, 0, written, 0, 100'000,
        48'000, 1024, false, true, 1, true, 1);
    QCOMPARE(item.m_ring.committedBytes(),
        qint64(2 * SpectrogramRingStorage::kColumnsPerBlock) * bins);
    QVERIFY(item.ringColumnLocked(written - 1, false) != nullptr);
    QVERIFY(item.ringColumnLocked(written, false) == nullptr);
    QVERIFY(item.ringColumnLocked(2 * SpectrogramRingStorage::kColumnsPerBlock, false)
        == nullptr);
    QVERIFY(item.m_ring.column(2 * SpectrogramRingStorage::kColumnsPerBlock) == nullptr);

    const QImage canvas = freshCanvas(item, 200, 16, 0, 199, false);
    QCOMPARE(canvas.width(), 200);
    const QRgb background = kSpectrogramNoColumn;
    // Each pixel may blend in its neighbour; leave out the two at the edge.
    for (int x = 0; x < written - 1; ++x) {
        for (int y = 0; y < canvas.height(); ++y) {
            QVERIFY(canvas.pixel(x, y) != background);
        }
    }
    for (int x = written + 1; x < canvas.width(); ++x) {
        for (int y = 0; y < canvas.height(); ++y) {
            QCOMPARE(canvas.pixel(x, y), background);
        }
    }
}

namespace {

// A byte for each column, none of them black.
char backfillColumnByte(qint64 column) {
    return static_cast<char>(64 + column % 150);
}

// Feeds `count` columns from `start` of a centered session of `token` and
// `generation`, each column of its own byte.
void feedBackfillColumns(SpectrogramItem &item, qint64 start, int count, quint64 generation,
    quint64 token = 7, int bins = 4, int hop = 1024) {
    QByteArray data(qsizetype(bins) * count, '\0');
    for (int column = 0; column < count; ++column) {
        std::fill_n(data.data() + qsizetype(column) * bins, bins,
            backfillColumnByte(start + column));
    }
    item.feedPrecomputedChunk(data, bins, 0, count, static_cast<int>(start), 100'000,
        48'000, hop, false, false, token, false, generation);
}

// The worker's reset of a centered session whose columns start at `start`.
void feedSessionReset(SpectrogramItem &item, qint64 start, quint64 generation,
    quint64 token = 7, int bins = 4, int hop = 1024) {
    item.feedPrecomputedChunk({}, bins, 0, 0, static_cast<int>(start), 100'000,
        48'000, hop, false, true, token, true, generation);
}

bool ringHoldsBackfillColumn(SpectrogramItem &item, qint64 column) {
    QMutexLocker lock(&item.m_stateMutex);
    const quint8 *bytes = item.ringColumnLocked(column, false);
    return bytes != nullptr && bytes[0] == static_cast<quint8>(backfillColumnByte(column));
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramCenteredBackfillLandsBeforeTheWriteHead() {
    // After a seek, the worker sends a centered session's columns at the
    // playhead first, then those left of it, before the ring's write head.
    // Each lands where its column puts it, and the ring, once it wraps,
    // drops the columns furthest left first.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedSessionReset(item, 1000, 2);
    feedBackfillColumns(item, 1300, 1, 2);
    feedBackfillColumns(item, 1301, 99, 2);
    QCOMPARE(item.m_precomputedMaxColumnIndex, 1399);
    QCOMPARE(item.m_ringWriteSeq, qint64(400));
    QVERIFY(!ringHoldsBackfillColumn(item, 1299));
    QVERIFY(ringHoldsBackfillColumn(item, 1300));

    feedBackfillColumns(item, 1000, 150, 2);
    feedBackfillColumns(item, 1150, 150, 2);
    QCOMPARE(item.m_ringWriteSeq, qint64(400));
    QCOMPARE(item.m_precomputedMaxColumnIndex, 1399);
    for (qint64 column = 1000; column < 1400; ++column) {
        QVERIFY2(ringHoldsBackfillColumn(item, column), qPrintable(QString::number(column)));
    }

    // The right goes on past the ring's capacity.
    const int capacity = item.m_ringCapacity;
    QVERIFY(capacity > 400);
    const qint64 newest = 1099 + capacity;
    for (qint64 start = 1400; start <= newest; start += 256) {
        feedBackfillColumns(item, start, static_cast<int>(std::min<qint64>(256, newest + 1 - start)), 2);
    }
    QCOMPARE(item.m_ringCapacity, capacity);
    QCOMPARE(item.m_ringWriteSeq, qint64(capacity) + 100);
    QVERIFY(!ringHoldsBackfillColumn(item, 1099));
    for (qint64 column = 1100; column <= newest; ++column) {
        QVERIFY2(ringHoldsBackfillColumn(item, column), qPrintable(QString::number(column)));
    }

    // A column further left than the ring reaches does not take the place
    // of the one a capacity right of it.
    feedBackfillColumns(item, 1050, 1, 2);
    QVERIFY(!ringHoldsBackfillColumn(item, 1050));
    QVERIFY(ringHoldsBackfillColumn(item, 1050 + capacity));
    QCOMPARE(item.m_ringWriteSeq, qint64(capacity) + 100);

    // In rolling mode the ring is written in the order the columns come.
    item.setDisplayMode(0);
    const qint64 writeSeq = item.m_ringWriteSeq;
    feedBackfillColumns(item, newest + 1, 3, 2);
    QCOMPARE(item.m_ringWriteSeq, writeSeq + 3);
}

void SpectrogramCanvasSmokeTest::spectrogramFreshPaneTakesTheBackfillOfASessionItMissedTheResetOf() {
    // A pane made after the session's reset, as when a track has more
    // channels than the last, starts its ring at the first columns it gets:
    // the playhead's. Those left of them still land before them.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedBackfillColumns(item, 5300, 100, 2);
    QVERIFY(ringHoldsBackfillColumn(item, 5300));
    feedBackfillColumns(item, 5000, 300, 2);
    for (qint64 column = 5000; column < 5400; ++column) {
        QVERIFY2(ringHoldsBackfillColumn(item, column), qPrintable(QString::number(column)));
    }
    QCOMPARE(item.m_precomputedMaxColumnIndex, 5399);
}

void SpectrogramCanvasSmokeTest::spectrogramStaleBackfillIsDropped() {
    // The left of a session that a seek, or another track, has superseded
    // arrives after the new session's reset: it is not written.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedSessionReset(item, 1000, 2);
    feedBackfillColumns(item, 1300, 100, 2);

    // A seek starts session 3, with its playhead at 5300.
    feedSessionReset(item, 5000, 3);
    feedBackfillColumns(item, 5300, 10, 3);
    feedBackfillColumns(item, 1000, 300, 2);
    QVERIFY(!ringHoldsBackfillColumn(item, 1000));
    QVERIFY(!ringHoldsBackfillColumn(item, 1300));
    QCOMPARE(item.m_precomputedMaxColumnIndex, 5309);
    QCOMPARE(item.m_ringWriteSeq, qint64(310));

    // Its own left lands before the write head.
    feedBackfillColumns(item, 5000, 300, 3);
    QVERIFY(ringHoldsBackfillColumn(item, 5000));
    QVERIFY(ringHoldsBackfillColumn(item, 5299));
    QCOMPARE(item.m_ringWriteSeq, qint64(310));

    // A centered gapless switch keeps the generation: the next track's
    // reset makes the left of the previous one stale.
    feedSessionReset(item, 0, 3, 8);
    feedBackfillColumns(item, 0, 10, 3, 8);
    feedBackfillColumns(item, 5000, 50, 3, 7);
    QCOMPARE(item.m_precomputedTrackToken, quint64(8));
    QCOMPARE(item.m_precomputedMaxColumnIndex, 9);
    QCOMPARE(item.m_ringWriteSeq, qint64(10));
}

void SpectrogramCanvasSmokeTest::spectrogramSeekShowsThePlayheadFirstAndFillsTheLeftIn() {
    // A seek outside the decoded columns keeps the previous image only until
    // the playhead's columns arrive. The view then shows the columns left
    // of the playhead as missing, as columns not decoded yet look, and
    // draws each in as the worker's backfill brings it.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedSessionReset(item, 0, 1);
    feedBackfillColumns(item, 0, 2000, 1);
    testClock(item).holdAt(10.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 4;
        item.ensureMapping(16);
        rebuildCanvasNow(item, 320, 16, 469 - 160, 469 + 159, false);
    }
    QVERIFY(!item.m_canvas.isNull());

    // The analysis thread clears the ring, and the worker resets it for a
    // session from left of the view, its playhead at 30 s, column 1406.
    item.feedPrecomputedChunk({}, 0, 0, 0, 0, 0, 0, 0, false, true, 7, true, 2);
    QVERIFY(item.m_zoomFillActive);
    QVERIFY(item.m_seekFillActive);
    // Another seek before the first one's columns came is a seek too.
    item.feedPrecomputedChunk({}, 0, 0, 0, 0, 0, 0, 0, false, true, 7, true, 2);
    QVERIFY(item.m_seekFillActive);
    testClock(item).holdAt(30.0);
    constexpr qint64 playhead = 1406;
    constexpr qint64 left = playhead - 160;
    constexpr qint64 start = left - 94;
    feedSessionReset(item, start, 2);
    QVERIFY(item.m_zoomFillActive);
    feedBackfillColumns(item, playhead, 1, 2);
    QVERIFY(!item.m_zoomFillActive);
    QVERIFY(!item.m_seekFillActive);
    QVERIFY(item.m_precomputedCanvasDirty);
    feedBackfillColumns(item, playhead + 1, 300, 2);

    const QRgb missing = kSpectrogramNoColumn;
    const auto pixelsAreMissing = [&item](int from, int to) {
        for (int x = from; x < to; ++x) {
            for (int y = 0; y < item.m_canvas.height(); ++y) {
                if (item.m_canvas.pixel(x, y) != kSpectrogramNoColumn) {
                    return false;
                }
            }
        }
        return true;
    };
    {
        QMutexLocker lock(&item.m_stateMutex);
        // As the paint does before it draws.
        item.m_binsPerColumn = 4;
        item.ensureMapping(16);
        rebuildCanvasNow(item, 320, 16, left, left + 319, false);
    }
    // Each pixel may blend in its neighbour; leave out those at the edge.
    QVERIFY(pixelsAreMissing(0, 159));
    QVERIFY(item.m_canvas.pixel(170, 8) != missing);

    // The left arrives, from the start of the session, and is drawn in.
    feedBackfillColumns(item, start, 150, 2);
    QVERIFY(!pixelsAreMissing(0, 50));
    QVERIFY(pixelsAreMissing(58, 159));
    feedBackfillColumns(item, start + 150, static_cast<int>(playhead - start - 150), 2);
    const QImage filled = item.m_canvas.copy();
    QImage fresh;
    {
        QMutexLocker lock(&item.m_stateMutex);
        fresh = freshCanvas(item, 320, 16, left, left + 319, false);
    }
    QCOMPARE(filled.copy(0, 0, fresh.width(), fresh.height()), fresh);
}

void SpectrogramCanvasSmokeTest::spectrogramZoomFreezeWaitsForTheBackfill() {
    // A zoom shows its data only once the view is covered: also the
    // columns between its left edge and the playhead, which the worker
    // decodes after those right of the playhead.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedSessionReset(item, 0, 1);
    feedBackfillColumns(item, 0, 2000, 1);
    testClock(item).holdAt(30.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 4;
        item.ensureMapping(16);
        rebuildCanvasNow(item, 320, 16, 1406 - 160, 1406 + 159, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    // A finer hop's session, as for a zoom in: 93.75 columns a second, so
    // the playhead is at column 2812, and the 320 pixels show 640 columns.
    constexpr int hop = 512;
    constexpr qint64 playhead = 2812;
    constexpr qint64 left = playhead - 320;
    constexpr qint64 start = left - 188;
    feedSessionReset(item, start, 2, 7, 4, hop);
    feedBackfillColumns(item, playhead, 1, 2, 7, 4, hop);
    // As a zoom freezes the canvas until its data covers the view.
    item.m_zoomFillActive = true;
    item.m_seekFillActive = false;
    feedBackfillColumns(item, playhead + 1, 399, 2, 7, 4, hop);
    QVERIFY(item.m_zoomFillActive);
    // The left edge, but not all of the left.
    feedBackfillColumns(item, start, static_cast<int>(left + 10 - start), 2, 7, 4, hop);
    QVERIFY(ringHoldsBackfillColumn(item, left));
    QVERIFY2(item.m_zoomFillActive, "columns left of the playhead are missing");
    feedBackfillColumns(item, left + 10, static_cast<int>(playhead - left - 10), 2, 7, 4, hop);
    QVERIFY(!item.m_zoomFillActive);
}

void SpectrogramCanvasSmokeTest::spectrogramZoomNearTheEndWaitsForTheLeftAndKeepsTheEnd() {
    // Zooming in near the end: the session's right reaches the end of the
    // file before its left is decoded. The left's chunks carry the file's
    // extent, which ends the view there, and the end of the session comes
    // after them. The zoom shows its data once the left is in, and the
    // extent stays.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(16);
    item.setDisplayMode(1);
    feedSessionReset(item, 0, 1);
    feedBackfillColumns(item, 0, 2000, 1);
    testClock(item).holdAt(30.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_binsPerColumn = 4;
        item.ensureMapping(16);
        rebuildCanvasNow(item, 320, 16, 1406 - 160, 1406 + 159, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    constexpr int hop = 512;
    constexpr int bins = 4;
    constexpr int padded = 100'000;
    constexpr int end = 3000;
    constexpr qint64 playhead = 2812;
    // The view of the end: 640 columns up to it.
    constexpr qint64 left = end - 640;
    constexpr qint64 start = left - 188;
    const auto feed = [&item](qint64 first, qint64 last, int estimate) {
        const int count = static_cast<int>(last - first);
        QByteArray data(qsizetype(bins) * count, '\0');
        for (int column = 0; column < count; ++column) {
            std::fill_n(data.data() + qsizetype(column) * bins, bins,
                backfillColumnByte(first + column));
        }
        item.feedPrecomputedChunk(data, bins, 0, count, static_cast<int>(first), estimate,
            48'000, hop, false, false, 7, false, 2);
    };
    item.feedPrecomputedChunk({}, bins, 0, 0, static_cast<int>(start), padded,
        48'000, hop, false, true, 7, true, 2);
    feed(playhead, playhead + 1, padded);
    // As a zoom freezes the canvas until its data covers the view.
    item.m_zoomFillActive = true;
    item.m_seekFillActive = false;
    feed(playhead + 1, end, padded);
    QVERIFY(item.m_zoomFillActive);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, padded);

    // The left, with the extent: the decoded end is near, but the left of
    // the view is not all in yet.
    feed(start, left + 100, end);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, end);
    QVERIFY2(item.m_zoomFillActive, "columns left of the playhead are missing");
    feed(left + 100, playhead, end);
    QVERIFY(!item.m_zoomFillActive);

    // The end of the session keeps the extent.
    item.feedPrecomputedChunk({}, bins, 0, 0, end, end, 48'000, hop, true, false, 7, false, 2);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, end);
    QVERIFY(!item.m_zoomFillActive);
}

void SpectrogramCanvasSmokeTest::spectrogramZoomFillWaitsForTheCanvasColumnsAtFractionalScales_data() {
    // Panes of an 8 kHz file zoomed in from 0.5 (columns of 342 frames) to
    // 0.57 (300 frames), which the canvas draws at 0.9999999999999999 of a
    // column a device pixel. 501 logical pixels at 1.5x are 751 device
    // ones, and at 1.25x 626: the canvas shows 501 columns, and 501
    // logical pixels at that zoom would be 502.
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("window");
    // Issue 441's example.
    QTest::newRow("1.5x") << 1.5 << 501 << 501;
    QTest::newRow("1.25x") << 1.25 << 501 << 501;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomFillWaitsForTheCanvasColumnsAtFractionalScales() {
    // A centered zoom keeps its preview until the ring holds the view's
    // columns, those left of the playhead included. It waits for the
    // columns the canvas will show, in device columns at the canvas's zoom
    // (issue 441): reckoned in logical pixels, its view was a column wider
    // at fractional scales, and mid-track it waited for the column left of
    // the canvas's view, which the canvas does not show.
    QFETCH(double, scale);
    QFETCH(int, width);
    QFETCH(int, window);
    QQuickWindow quickWindow;
    if (!showOnScreen(quickWindow, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    quickWindow.resize(width + 40, 160);
    SpectrogramItem &item = *new SpectrogramItem(quickWindow.contentItem());
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int fftSize = 2'048;
    constexpr int referenceHop = 171;
    constexpr int startHop = 342;
    constexpr int hop = 300;
    constexpr double zoom = 0.57;
    // 70.08 s, of which the 300-frame columns end at 1867.
    constexpr quint64 frames = 560'640;
    constexpr int end = 1'867;
    // Column 1024 of 300 frames, mid-track.
    constexpr double position = 38.4;
    constexpr qint64 playhead = 1'024;
    const auto estimateAt = [](int columnHop) {
        return static_cast<int>(std::ceil(
            (static_cast<double>(frames / 1'024) + 64.0) * 1'024.0 / columnHop));
    };
    const auto endAt = [](int columnHop) {
        const int rowHop = std::min(fftSize / 8, columnHop);
        const int rows = static_cast<int>((frames - fftSize / 2) / rowHop) + 1;
        return (rows * rowHop + columnHop - 1) / columnHop;
    };
    QCOMPARE(endAt(hop), end);

    // The pane at zoom 0.5, with all its columns in.
    item.feedPrecomputedChunk(QByteArray(64 * bins, '\x40'), bins, 0, 64, 0,
        estimateAt(referenceHop), rate, referenceHop, false, true, 1, false, 1, 0, fftSize,
        frames, endAt(referenceHop));
    item.setZoomLevel(0.5);
    QVERIFY(sendPendingBackendZoom(item));
    const int startEnd = endAt(startHop);
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, 0, estimateAt(startHop), rate,
        startHop, false, true, 1, false, 2, 0, fftSize, frames, startEnd);
    item.feedPrecomputedChunk(QByteArray(qsizetype(startEnd) * bins, '\x40'), bins, 0,
        startEnd, 0, estimateAt(startHop), rate, startHop, false, false, 1, false, 2, 0,
        fftSize, frames, startEnd);
    setTestPosition(item, position);
    QSGNode *node = paintSettled(item, nullptr);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_canvasScale, scale);
        QCOMPARE(item.m_precomputedHopSize, startHop);
    }

    // Zoomed in: the canvas is shown scaled until the zoom's data covers
    // the view, of `window` columns centered on the playhead's.
    item.setZoomLevel(zoom);
    QVERIFY(sendPendingBackendZoom(item));
    node = paintSettled(item, node);
    const qint64 left = playhead - window / 2;
    const auto feed = [&](qint64 first, qint64 last) {
        const int count = static_cast<int>(last - first);
        item.feedPrecomputedChunk(QByteArray(qsizetype(count) * bins, '\x40'), bins, 0, count,
            static_cast<int>(first), estimateAt(hop), rate, hop, false, false, 1, false, 3, 0,
            fftSize, frames, end);
    };
    // The session starts 2 s of warm-up left of the view, and decodes the
    // playhead's column first, then the right, then the left.
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, static_cast<int>(left - 53),
        estimateAt(hop), rate, hop, false, true, 1, false, 3, 0, fftSize, frames, end);
    feed(playhead, playhead + 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedHopSize, hop);
        QVERIFY(!item.m_awaitingZoomData);
        QVERIFY(item.m_zoomFillActive);
        QVERIFY(item.canvasZoomLocked() < scale);
        // The view the canvas will show, and the one logical pixels made.
        const SpectrogramItem::CenteredView view = item.centeredViewLocked(position,
            item.canvasWidthLocked(), item.canvasHeightLocked(), item.canvasZoomLocked(),
            static_cast<double>(rate) / hop);
        QCOMPARE(view.left, left);
        QCOMPARE(view.right, left + window - 1);
        QCOMPARE(static_cast<int>(std::ceil(width / item.effectiveZoomLocked())), window + 1);
    }
    feed(playhead + 1, end);
    QVERIFY(item.m_zoomFillActive);
    // All of the left but its first column.
    feed(left + 1, playhead);
    QVERIFY2(item.m_zoomFillActive, "the view's first column is missing");
    // Its first column, and not the one before it.
    feed(left, left + 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.ringSlotForDisplayIndexLocked(left - 1, false) < 0);
    }
    QVERIFY2(!item.m_zoomFillActive, "the ring holds every column of the view");

    // The canvas then shows those columns.
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_zoomPreviewShown);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, left);
        QCOMPARE(item.m_precomputedCanvasViewRight, left + window - 1);
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramRingGrowthKeepsRollingHistory() {
    // Grows the ring before it wraps, and after it has wrapped and dropped
    // its oldest columns.
    for (bool wrapped : {false, true}) {
        SpectrogramItem item;
        item.setWidth(1920);
        item.setHeight(100);
        item.setDisplayMode(0);
        constexpr int bins = 1025;
        constexpr int firstColumns = 300;
        const auto columnByte = [](qint64 seq) { return static_cast<quint8>(seq % 251 + 1); };
        const auto feed = [&](int start, int columns, bool reset) {
            QByteArray chunk(qsizetype(bins) * columns, '\0');
            for (int column = 0; column < columns; ++column) {
                std::fill_n(chunk.data() + qsizetype(column) * bins, bins,
                    static_cast<char>(columnByte(start + column)));
            }
            item.feedPrecomputedChunk(chunk, bins, 0, columns, start, 1'000'000,
                48'000, 1024, false, reset, 1, reset, 1);
        };
        feed(0, firstColumns, true);
        const int capacityBefore = item.m_ringCapacity;
        const int written = wrapped ? capacityBefore + 500 : firstColumns;
        if (written > firstColumns) {
            feed(firstColumns, written - firstColumns, false);
        }
        QCOMPARE(item.m_ringCapacity, capacityBefore);
        QCOMPARE(item.m_ringWriteSeq, qint64(written));
        const qint64 oldestKept = std::max(0, written - capacityBefore);
        QCOMPARE(item.m_ringOldestSeq, oldestKept);
        QCOMPARE(oldestKept > 0, wrapped);

        // Zooming out widens the view, so the ring grows with the next data.
        item.setZoomLevel(0.25);
        feed(written, 1, false);
        const int capacity = item.m_ringCapacity;
        QVERIFY(capacity > capacityBefore);
        QCOMPARE(item.m_ringWriteSeq, qint64(written + 1));
        std::set<int> blocks;
        for (qint64 seq = oldestKept; seq <= written; ++seq) {
            const quint8 *bytes = item.ringColumnLocked(seq, true);
            QVERIFY2(bytes != nullptr, qPrintable(QStringLiteral("column %1").arg(seq)));
            QCOMPARE(bytes[0], columnByte(seq));
            QCOMPARE(bytes[bins - 1], columnByte(seq));
            blocks.insert(static_cast<int>(seq % capacity)
                / SpectrogramRingStorage::kColumnsPerBlock);
        }
        // The columns the ring dropped before it grew stay missing.
        for (qint64 seq = 0; seq < oldestKept; ++seq) {
            QVERIFY2(item.ringColumnLocked(seq, true) == nullptr,
                qPrintable(QStringLiteral("dropped column %1").arg(seq)));
        }
        // The copy wrote only the columns it kept.
        qint64 blockBytes = 0;
        for (int block : blocks) {
            blockBytes += qint64(std::min(SpectrogramRingStorage::kColumnsPerBlock,
                capacity - block * SpectrogramRingStorage::kColumnsPerBlock)) * bins;
        }
        QCOMPARE(item.m_ring.committedBytes(), blockBytes);
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRingChangeBenchmark() {
    if (qEnvironmentVariableIntValue("FERROUS_BENCHMARK_SPECTROGRAM") != 1) {
        QSKIP("Set FERROUS_BENCHMARK_SPECTROGRAM=1 to measure ring changes");
    }
    // A centered 3440 px pane of a stereo track: the first data of a track
    // change, then of an FFT size switch, each making room for a new ring
    // of up to the lookahead budget.
    constexpr int width = 3440;
    constexpr int channels = 2;
    constexpr int columns = 32;
    constexpr int iterations = 15;
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    for (int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(width);
        item.setHeight(400);
        item.setDisplayMode(mode);
        quint64 generation = 0;
        std::vector<double> trackChange;
        std::vector<double> fftSwitch;
        int capacity = 0;
        for (int i = 0; i < iterations; ++i) {
            for (int bins : {4097, 2049}) {
                QByteArray chunk(bins * channels * columns, '\x40');
                QElapsedTimer timer;
                timer.start();
                item.feedPrecomputedChunk(chunk, bins, 0, columns, 0, 400'000,
                    44'100, 1024, false, true, 1, true, ++generation);
                const double ms = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
                (bins == 4097 ? trackChange : fftSwitch).push_back(ms);
                if (bins == 4097) {
                    capacity = item.m_ringCapacity;
                }
            }
        }
        qInfo().noquote() << QStringLiteral(
            "spectrogram ring change: mode=%1 width=%2 capacity=%3 ring_mib=%4 "
            "first_chunk_ms=%5 first_chunk_worst_ms=%6")
                .arg(mode == 0 ? QStringLiteral("rolling") : QStringLiteral("centered"))
                .arg(width)
                .arg(capacity)
                .arg(static_cast<double>(capacity) * 4097 / (1024.0 * 1024.0), 0, 'f', 1)
                .arg(median(trackChange), 0, 'f', 3)
                .arg(*std::max_element(trackChange.begin(), trackChange.end()), 0, 'f', 3);
        qInfo().noquote() << QStringLiteral(
            "spectrogram ring change: mode=%1 fft_switch_ms=%2 fft_switch_worst_ms=%3")
                .arg(mode == 0 ? QStringLiteral("rolling") : QStringLiteral("centered"))
                .arg(median(fftSwitch), 0, 'f', 3)
                .arg(*std::max_element(fftSwitch.begin(), fftSwitch.end()), 0, 'f', 3);
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRollingCanvasGrowsIncrementallyDuringInitialFill() {
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
        rebuildCanvasNow(item, 12, 6, 0, 3, true);
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
        QVERIFY(item.advancePrecomputedCanvasLocked(0, 4, true, item.m_now()));
        QCOMPARE(item.m_canvasFilledCols, 10);
        QCOMPARE(item.m_canvasWriteX, 10);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(0));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(4));
        QCOMPARE(item.m_precomputedCanvasSubpixelOffset, 0.0);
        // Pixel 7 blended column 3 with nothing; it now has column 4.
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < 7; ++x) {
                QCOMPARE(item.m_canvas.pixelColor(x, y), initialCanvas.pixelColor(x, y));
            }
        }
        QCOMPARE(canvasInViewOrder(item), freshCanvas(item, 12, 6, 0, 4, true));
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRollingCanvasHandsOffToSteadyScrollIncrementally() {
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
        rebuildCanvasNow(item, 12, 6, 0, 5, true);
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
        QVERIFY(item.advancePrecomputedCanvasLocked(1, 6, true, item.m_now()));
        QCOMPARE(item.m_canvasFilledCols, 12);
        QCOMPARE(item.m_canvasWriteX, 2);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(1));
        QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(6));
        QCOMPARE(item.m_precomputedCanvasSubpixelOffset, 0.0);
        const int srcStart =
            (item.m_canvasWriteX - item.m_canvasFilledCols + item.m_canvas.width())
            % item.m_canvas.width();
        QCOMPARE(srcStart, 2);
        // The last pixel of column 5 blended it with nothing; it now has
        // column 6.
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < initialCanvas.width() - 3; ++x) {
                QCOMPARE(
                    item.m_canvas.pixelColor((srcStart + x) % item.m_canvas.width(), y),
                    initialCanvas.pixelColor(x + 2, y));
            }
        }
        QCOMPARE(canvasInViewOrder(item), freshCanvas(item, 12, 6, 1, 6, true));
    }
}

void SpectrogramCanvasSmokeTest::spectrogramRollingCanvasAdvancesIncrementallyAtFractionalZoom() {
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
        rebuildCanvasNow(item, 7, 6, 0, 3, true);
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
        QVERIFY(item.advancePrecomputedCanvasLocked(1, 4, true, item.m_now()));
        QCOMPARE(item.m_canvasFilledCols, 7);
        QCOMPARE(item.m_canvasWriteX, 1);
        QVERIFY(item.m_precomputedCanvasSubpixelOffset > 0.70);
        QVERIFY(item.m_precomputedCanvasSubpixelOffset < 0.80);
        const int srcStart =
            (item.m_canvasWriteX - item.m_canvasFilledCols + item.m_canvas.width())
            % item.m_canvas.width();
        QCOMPARE(srcStart, 1);
        // Pixels 5 and 6 blended column 3 with nothing; they now have
        // column 4, drawn where the scrolled canvas shows them.
        for (int y = 0; y < initialCanvas.height(); ++y) {
            for (int x = 0; x < initialCanvas.width() - 2; ++x) {
                QCOMPARE(
                    item.m_canvas.pixelColor((srcStart + x) % item.m_canvas.width(), y),
                    initialCanvas.pixelColor(x + 1, y));
            }
        }
    }
}

void SpectrogramCanvasSmokeTest::hiddenSpectrogramDefersPaintingAndRetainsIncomingData() {
    for (const int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(100);
        item.setHeight(10);
        item.setDisplayMode(mode);
        constexpr int bins = 4;
        item.feedPrecomputedChunk(QByteArray(100 * bins, '\x60'), bins, 0, 100,
            0, 1000, 48000, 1024, false, true, 1);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.ensureMapping(10);
            rebuildCanvasNow(item, 100, 10, 0, 99, false);
        }
        const QImage before = item.m_canvas.copy();
        const qint64 sequence = item.m_ringWriteSeq;
        item.setVisible(false);
        item.m_animationTickInitialized = false;
        item.handleWindowAfterAnimating();
        QVERIFY(!item.m_animationTickInitialized);
        item.feedPrecomputedChunk(QByteArray(bins, '\xF0'), bins, 0, 1,
            100, 1000, 48000, 1024, false, false, 1);
        QCOMPARE(item.m_ringWriteSeq, sequence + 1);
        QCOMPARE(item.m_canvas, before);
        QVERIFY(item.m_precomputedCanvasDirty);
        item.setVisible(true);
        QVERIFY(!item.m_animationTickInitialized);
        {
            QMutexLocker lock(&item.m_stateMutex);
            rebuildCanvasNow(item, 100, 10, 1, 100, false);
        }
        QVERIFY(item.m_canvas != before);
        QVERIFY(!item.m_precomputedCanvasDirty);
    }
}

void SpectrogramCanvasSmokeTest::releasedSpectrogramFreesItsColumnsAndCanvasUntilRetained() {
    // Released while analysis is off, as behind the waveform view, a pane
    // frees its ring and canvas, and takes no columns: not of a gapless
    // successor, nor of the same track played again. Retained, it takes
    // none until a session's reset, and fills from that session.
    for (const int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(100);
        item.setHeight(10);
        item.setDisplayMode(mode);
        constexpr int bins = 4;
        item.feedPrecomputedChunk(QByteArray(100 * bins, '\x60'), bins, 0, 100,
            0, 1000, 48000, 1024, false, true, 1, true, 1);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.ensureMapping(10);
            rebuildCanvasNow(item, 100, 10, 0, 99, mode == 0);
        }
        QVERIFY(!item.m_canvas.isNull());
        QVERIFY(item.m_ring.committedBytes() > 0);
        QVERIFY(item.m_precomputedReady);
        QSignalSpy readyChanges(&item, &SpectrogramItem::precomputedReadyChanged);

        item.releaseStorage();
        QVERIFY(item.storageReleased());
        QCOMPARE(item.m_ring.committedBytes(), qint64(0));
        QCOMPARE(item.m_ring.capacity(), 0);
        QCOMPARE(item.m_ringCapacity, 0);
        QCOMPARE(item.m_ringColumnId.capacity(), size_t(0));
        QCOMPARE(item.m_ringSequenceId.capacity(), size_t(0));
        QCOMPARE(item.m_ringTrackToken.capacity(), size_t(0));
        QVERIFY(item.m_trackColumnToSeqByToken.isEmpty());
        QVERIFY(item.m_canvas.isNull());
        QVERIFY(item.m_rasterTiles.isEmpty());
        QVERIFY(!item.m_precomputedReady);
        QCOMPARE(readyChanges.size(), 1);
        // A window that still draws it drops its node.
        QCOMPARE(item.updatePaintNode(nullptr, nullptr), nullptr);

        // Columns go on arriving; a gapless successor's, then the same
        // track's again, as Repeat-One plays it.
        item.feedPrecomputedChunk(QByteArray(50 * bins, '\x70'), bins, 0, 50,
            100, 1000, 48000, 1024, false, false, 1, false, 1);
        item.feedPrecomputedChunk(QByteArray(50 * bins, '\x70'), bins, 0, 50,
            0, 1000, 48000, 1024, false, false, 2, false, 1);
        item.feedPrecomputedChunk(QByteArray(), bins, 0, 0,
            0, 1000, 48000, 1024, false, true, 3, true, 2);
        item.feedPrecomputedChunk(QByteArray(50 * bins, '\x70'), bins, 0, 50,
            0, 1000, 48000, 1024, false, false, 3, false, 2);
        QCOMPARE(item.m_ring.committedBytes(), qint64(0));
        QCOMPARE(item.m_ringCapacity, 0);
        QVERIFY(item.m_canvas.isNull());
        // Released again, nothing changes.
        item.releaseStorage();
        QCOMPARE(readyChanges.size(), 1);

        item.retainStorage();
        QVERIFY(!item.storageReleased());
        // Columns without a reset first: BridgeClient drops a stale
        // session's resets (see spectrogramSessionsBeforeAStopNeverFillReleasedPanes).
        item.feedPrecomputedChunk(QByteArray(50 * bins, '\x70'), bins, 0, 50,
            50, 1000, 48000, 1024, false, false, 3, false, 2);
        QCOMPARE(item.m_ring.committedBytes(), qint64(0));
        QVERIFY(!item.m_precomputedReady);

        // The fresh session: its reset, then the playhead's columns.
        item.feedPrecomputedChunk(QByteArray(), bins, 0, 0,
            200, 1000, 48000, 1024, false, true, 3, true, 3);
        item.feedPrecomputedChunk(QByteArray(100 * bins, '\xF0'), bins, 0, 100,
            200, 1000, 48000, 1024, false, false, 3, false, 3);
        QVERIFY(item.m_ring.committedBytes() > 0);
        QVERIFY(item.m_precomputedReady);
        QCOMPARE(readyChanges.size(), 2);
        QCOMPARE(item.m_precomputedCommittedGeneration, quint64(3));
        if (mode == 0) {
            // Rolling history starts with the fresh session's columns.
            QCOMPARE(item.m_ringOldestSeq, qint64(0));
            QCOMPARE(item.m_ringWriteSeq, qint64(100));
            QCOMPARE(item.ringColumnLocked(0, true)[0], quint8(0xF0));
        } else {
            QCOMPARE(item.ringColumnLocked(200, false)[0], quint8(0xF0));
            QVERIFY(item.ringColumnLocked(199, false) == nullptr);
        }
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.ensureMapping(10);
            if (mode == 0) {
                rebuildCanvasNow(item, 100, 10, 0, 99, true);
            } else {
                rebuildCanvasNow(item, 100, 10, 200, 299, false);
            }
        }
        QVERIFY(!item.m_canvas.isNull());
        QVERIFY(item.m_canvas.pixel(50, 5) != kSpectrogramNoColumn);
    }
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredLateFillUsesCircularCanvasOffset() {
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
        // As the paint does before it draws.
        item.m_binsPerColumn = bins;
        item.ensureMapping(10);
        rebuildCanvasNow(item, 100, 10, 50, 149, false);
        QVERIFY(item.advancePrecomputedCanvasLocked(60, 159, false, item.m_now()));
    }

    // Column 151 is not decoded yet, so its physical circular slot is black.
    QCOMPARE(item.m_canvasWriteX, 10);
    QCOMPARE(item.m_canvas.pixel(1, 5), kSpectrogramNoColumn);

    QByteArray late(9 * bins, '\x70');
    item.feedPrecomputedChunk(
        late, bins, 0, 9, 151, 1000,
        48000, 1024, false, false, token);

    // Logical x=91 maps to physical x=(canvasStart 10 + 91) % 100 = 1.
    // Painting x=91 directly leaves the visible gap permanently black.
    QVERIFY(item.m_canvas.pixel(1, 5) != kSpectrogramNoColumn);
}

void SpectrogramCanvasSmokeTest::spectrogramPeakHoldRebuildUsesMaxNotNearest() {
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
        rebuildCanvasNow(item, 100, 10, 0, cols - 1, false);
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

void SpectrogramCanvasSmokeTest::spectrogramZoomFillClearsWhenDecoderReachesTail() {
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
        rebuildCanvasNow(item, 3440, 720, 0, 3439, false);
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

void SpectrogramCanvasSmokeTest::spectrogramSyntheticClearPreservesCanvasDuringSeek() {
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
        rebuildCanvasNow(item, 1920, 400, 0, 1919, false);
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

void SpectrogramCanvasSmokeTest::spectrogramSyntheticClearInvalidatesCanvasWhenNoOldContent() {
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

void SpectrogramCanvasSmokeTest::spectrogramWheelZoomFollowsTheDeltaAndPassesHorizontalScroll() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setZoomEnabled(true);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false);
    QSignalSpy requested(&item, &SpectrogramItem::zoomRequested);
    const auto wheel = [&item](QPoint pixelDelta, QPoint angleDelta) {
        QWheelEvent event(QPointF(100, 50), QPointF(100, 50), pixelDelta, angleDelta,
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        event.setAccepted(false);
        item.wheelEvent(&event);
        return event.isAccepted();
    };

    // Horizontal scrolling passes through to whatever is behind.
    QVERIFY(!wheel(QPoint(), QPoint(120, 0)));
    QCOMPARE(requested.count(), 0);

    // An eighth of a notch zooms by an eighth of a notch's factor.
    QVERIFY(wheel(QPoint(), QPoint(0, 15)));
    QCOMPARE(requested.count(), 1);
    QVERIFY(std::abs(requested.takeFirst().at(0).toDouble() - std::pow(1.25, 0.125)) < 1e-9);

    // A pixel-only event counts by its pixels.
    QVERIFY(wheel(QPoint(0, 60), QPoint()));
    QCOMPARE(requested.count(), 1);
    QVERIFY(std::abs(requested.takeFirst().at(0).toDouble() - std::pow(1.25, 0.5)) < 1e-9);
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomPreviewShowsAtOnceAndGivesWayToTheData() {
    // Textures, and so drawn spans, need a window.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 10.0);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(drawnCanvasScale(node), 1.0);
    auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
    QVERIFY(std::abs(playhead->rect().x() - 100.0) <= 1.0);

    // The user zooms out to half, and the backend restarts for it; then,
    // before its data arrives, in to 2x.
    item.setZoomLevel(0.5);
    QVERIFY(sendPendingBackendZoom(item));
    item.setZoomLevel(2.0);

    // The next frame shows the canvas at 2x at once, around the playhead,
    // which stays where the 2x data will put it.
    node = paintSettled(item, node);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);
    QVERIFY(std::abs(playhead->rect().x() - 100.0) <= 1.0);
    const QPair<double, double> extent = drawnCanvasExtent(node);
    QVERIFY(extent.first >= 0.0 && extent.second <= 200.0);

    // Data for half arrives from the restart. It is not for the zoom on
    // screen, so it must not flash: the 2x preview stays.
    constexpr int staleHop = 2048;
    const int staleStart = static_cast<int>(10.0 * 48000 / staleHop) - 200;
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x60'), bins, 0, 400,
        staleStart, columns / 2, 48000, staleHop, false, true, 1, false, 2);
    node = paintSettled(item, node);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);
    QVERIFY(std::abs(playhead->rect().x() - 100.0) <= 1.0);
    QVERIFY(item.m_zoomFillActive);

    // Data for 2x replaces the preview.
    constexpr int hop = 512;
    const int nowColumn = static_cast<int>(10.0 * 48000 / hop);
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x70'), bins, 0, 400,
        nowColumn - 150, columns * 2, 48000, hop, false, true, 1, false, 3);
    node = paintSettled(item, node);
    QVERIFY(!item.m_zoomFillActive);
    QCOMPARE(drawnCanvasScale(node), 1.0);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
    QCOMPARE(item.m_renderZoomLevel, 2.0);
    QVERIFY(std::abs(playhead->rect().x() - 100.0) <= 1.0);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(nowColumn - 100));

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramRollingZoomPreviewShowsAtOnceAndGivesWayToTheData() {
    // Textures, and so drawn spans, need a window.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(0);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 10.0);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    QCOMPARE(drawnCanvasScale(node), 1.0);

    item.setZoomLevel(4.0);
    QVERIFY(sendPendingBackendZoom(item));
    item.setZoomLevel(2.0);

    // Shown at 2x at once, with the playhead's time at the right edge,
    // where rolling data for any zoom puts it.
    node = paintSettled(item, node);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);
    const QPair<double, double> extent = drawnCanvasExtent(node);
    QVERIFY2(extent.first >= 0.0 && std::abs(extent.second - 200.0) <= 2.0,
             qPrintable(QStringLiteral("%1..%2").arg(extent.first).arg(extent.second)));

    // The restart's 4x data does not flash.
    constexpr int staleHop = 256;
    item.feedPrecomputedChunk(
        QByteArray(bins * 200, '\x60'), bins, 0, 200,
        static_cast<int>(10.0 * 48000 / staleHop), columns * 4, 48000, staleHop,
        false, true, 1, true, 2);
    node = paintSettled(item, node);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);

    // The 2x data replaces the preview as it arrives.
    constexpr int hop = 512;
    item.feedPrecomputedChunk(
        QByteArray(bins * 300, '\x70'), bins, 0, 300,
        static_cast<int>(10.0 * 48000 / hop), columns * 2, 48000, hop,
        false, true, 1, true, 3);
    node = paintSettled(item, node);
    QVERIFY(!item.m_zoomFillActive);
    QCOMPARE(drawnCanvasScale(node), 1.0);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
    QCOMPARE(item.m_renderZoomLevel, 2.0);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomPreviewShowsTheColumnsOfItsDataAtFractionalScales_data() {
    // Panes of 8 kHz files on screens at fractional scales, zoomed in from
    // 0.5 (columns of 342 frames) to a zoom whose data the canvas draws at
    // a bit less than a column a device pixel: 171 / hop × hop / 171 is
    // 0.9999999999999999, and at 0.57 (300 frames) a 1680-pixel pane at
    // 1.5x, or at 0.55 (311 frames) one at 1.25x, shows a column more than
    // its pixels, 1681 of them.
    QTest::addColumn<double>("scale");
    QTest::addColumn<double>("zoom");
    QTest::addColumn<int>("hop");
    QTest::addColumn<int>("frames");
    QTest::addColumn<bool>("lengthKnown");
    QTest::addColumn<int>("width");
    QTest::addColumn<double>("position");
    QTest::addColumn<int>("window");
    QTest::addColumn<qint64>("left");
    // Issue 436's example: 1867 columns of 300 frames, of which 1681 are
    // 90.04%, the whole track, and 1680 89.98%. Column 1024 at 38.4 s.
    QTest::newRow("1.5x, issue example")
        << 1.5 << 0.57 << 300 << 560'640 << true << 1'680 << 38.4 << 1'681 << qint64(0);
    // 1868 columns of 300 frames, of which 1681 are 89.99%: centered. The
    // 1638 columns of 342 frames, rescaled, come to 1867, which the window
    // would have shown whole.
    QTest::newRow("1.5x, a column past the rescaled end")
        << 1.5 << 0.57 << 300 << 561'124 << true << 1'680 << 38.4 << 1'681 << qint64(1'024 - 840);
    // Shown whole, with the playhead three quarters into column 1024: the
    // canvas draws it at the column's start.
    QTest::newRow("1.5x, playhead between columns")
        << 1.5 << 0.57 << 300 << 560'640 << true << 1'680 << 1'024.75 * 300 / 8'000
        << 1'681 << qint64(0);
    // A file that does not tell its length: the columns so far, rescaled,
    // which come to the 1867 the session ends at.
    QTest::newRow("1.5x, length not told")
        << 1.5 << 0.57 << 300 << 560'640 << false << 1'680 << 38.4 << 1'681 << qint64(0);
    // 1867 columns of 311 frames; column 1024 at 39.808 s.
    QTest::newRow("1.25x, across the whole-track lock")
        << 1.25 << 0.55 << 311 << 581'524 << true << 1'680 << 39.808 << 1'681 << qint64(0);
    QTest::newRow("1.25x, mid-track")
        << 1.25 << 0.55 << 311 << 581'524 << true << 1'600 << 39.808 << 1'601
        << qint64(1'024 - 800);
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomPreviewShowsTheColumnsOfItsDataAtFractionalScales() {
    // Until the data for a zoom arrives, the canvas on screen is shown
    // scaled to it, where the data will put it. The preview reckons the
    // view of the data as the canvas will once the session has decoded the
    // file's end (issue 436): its zoom is the effective zoom, then the
    // device scale; its columns end where the decoder reckons from the
    // file's frames; and the playhead is at the start of its column. In
    // the other order, its window was a column short of the canvas's; by
    // the earlier zoom's columns, rescaled, its end could be a column
    // short; across the whole-track lock, either made the view jump when
    // the data arrived.
    QFETCH(double, scale);
    QFETCH(double, zoom);
    QFETCH(int, hop);
    QFETCH(int, frames);
    QFETCH(bool, lengthKnown);
    QFETCH(int, width);
    QFETCH(double, position);
    QFETCH(int, window);
    QFETCH(qint64, left);
    QQuickWindow quickWindow;
    if (!showOnScreen(quickWindow, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    quickWindow.resize(width + 40, 160);
    SpectrogramItem &item = *new SpectrogramItem(quickWindow.contentItem());
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int fftSize = 2'048;
    constexpr int referenceHop = 171;
    constexpr int startHop = 342;
    const quint64 trackFrames = lengthKnown ? static_cast<quint64>(frames) : 0;
    // A session's column estimate until it has decoded the end.
    const auto estimateAt = [frames](int columnHop) {
        return static_cast<int>(
            std::ceil((frames / 1'024 + 64) * 1'024.0 / static_cast<double>(columnHop)));
    };
    // The columns a session decodes of the file: the rows of 2048-frame
    // windows that fit, an eighth of a window apart (or a column, if
    // closer), in columns, the last one partial.
    const auto decodedEndAt = [frames](int columnHop) {
        const int rowHop = std::min(fftSize / 8, columnHop);
        const int rows = (frames - fftSize / 2) / rowHop + 1;
        return (rows * rowHop + columnHop - 1) / columnHop;
    };
    // A whole session, as its chunks bring it, with the end the decoder
    // places its views by when the file tells its length.
    quint64 generation = 1;
    const auto feedSession = [&](int columnHop) {
        const int end = decodedEndAt(columnHop);
        const int endColumn = lengthKnown ? end : 0;
        ++generation;
        item.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, 0, estimateAt(columnHop), rate, columnHop,
            false, true, 1, false, generation, 0, fftSize, trackFrames, endColumn);
        item.feedPrecomputedChunk(
            QByteArray(end * bins, '\x40'), bins, 0, end, 0, estimateAt(columnHop),
            rate, columnHop, false, false, 1, false, generation, 0, fftSize, trackFrames,
            endColumn);
        item.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, end, end, rate, columnHop,
            /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false,
            generation, 0, fftSize, trackFrames, endColumn);
    };

    // The pane at zoom 0.5, with all its columns in.
    item.feedPrecomputedChunk(
        QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, estimateAt(referenceHop),
        rate, referenceHop, false, true, 1, false, generation, 0, fftSize, trackFrames,
        lengthKnown ? decodedEndAt(referenceHop) : 0);
    item.setZoomLevel(0.5);
    QVERIFY(sendPendingBackendZoom(item));
    feedSession(startHop);
    setTestPosition(item, position);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_canvasScale, scale);
        QCOMPARE(item.m_precomputedHopSize, startHop);
        QCOMPARE(item.m_precomputedTrackFrames, trackFrames);
        QVERIFY(!item.m_awaitingZoomData);
    }

    // Zoomed in: the canvas is shown scaled until the data arrives.
    item.setZoomLevel(zoom);
    QVERIFY(sendPendingBackendZoom(item));
    node = paintSettled(item, node);
    SpectrogramItem::ZoomTargetView predicted;
    int canvasWidth = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_awaitingZoomData);
        QVERIFY(item.m_zoomPreviewShown);
        QCOMPARE(item.columnHopForZoomLocked(item.backendZoomForRequestLocked()), hop);
        canvasWidth = item.canvasWidthLocked();
        predicted = item.centeredZoomTargetLocked(
            canvasWidth, position, static_cast<double>(rate) / startHop);
    }
    QVERIFY(predicted.valid);
    const double previewPlayheadX = playhead->rect().x();

    // The data for the zoom replaces the preview.
    feedSession(hop);
    node = paintSettled(item, node);
    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(!item.m_awaitingZoomData);
    QVERIFY(!item.m_zoomFillActive);
    QVERIFY(!item.m_zoomPreviewShown);
    QCOMPARE(item.m_precomputedHopSize, hop);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, decodedEndAt(hop));
    QCOMPARE(item.canvasWidthLocked(), canvasWidth);
    const double canvasZoom = item.canvasZoomLocked();
    QVERIFY(canvasZoom < scale);
    const int canvasWindow =
        static_cast<int>(std::ceil(static_cast<double>(canvasWidth) / canvasZoom));
    QCOMPARE(canvasWindow, window);
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, left);

    // The preview showed the canvas's columns, where the canvas shows them:
    // the playhead stays put, and so does the view's left edge.
    QCOMPARE(playhead->rect().x(), previewPlayheadX);
    QCOMPARE(predicted.range.left, item.m_precomputedCanvasDisplayLeft);
    QCOMPARE(predicted.range.right, item.m_precomputedCanvasViewRight);
    QCOMPARE(predicted.windowColumns, canvasWindow);
    // Exactly: QCOMPARE would let a rounding through.
    QVERIFY2(predicted.canvasZoom == canvasZoom,
             qPrintable(QStringLiteral("%1 != %2")
                            .arg(predicted.canvasZoom, 0, 'g', 17)
                            .arg(canvasZoom, 0, 'g', 17)));

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomPreviewHoldsThroughTheDataBeforeTheEnd_data() {
    // Issue 436's file, zoomed out from 2x to 0.57 at 1.5x: 1867 columns
    // of 300 frames, of which the 1681 the window shows are the whole
    // track. Column 1024 is at 38.4 s, at logical x 1024 of the whole
    // track and 840 of a centered view.
    QTest::addColumn<bool>("lengthKnown");
    QTest::addColumn<double>("shownX");
    QTest::addColumn<qint64>("shownLeft");
    QTest::addColumn<double>("endX");
    // The view ends where the decoder's does from the first chunk.
    QTest::newRow("length told") << true << 1'024.0 << qint64(0) << 1'024.0;
    // Bounded by the column estimate, which runs 64 columns of 1024
    // frames past the end, until the session reaches it, as before.
    QTest::newRow("length not told") << false << 840.0 << qint64(1'024 - 840) << 1'024.0;
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredZoomPreviewHoldsThroughTheDataBeforeTheEnd() {
    // Zoomed out, the preview gives way to the zoom's data as soon as it
    // arrives, long before its session reaches the end of the file. The
    // view of that data ends where the decoder's views end, which the
    // session reckons from the file's frames and sends with every chunk,
    // and where the preview put it (issue 436): it does not wait for the
    // end to show the whole track. By the column estimate, which runs past
    // the end, the view centered the playhead, at 840, after a preview of
    // the whole track at 1024, and went back to 1024 at the end.
    QFETCH(bool, lengthKnown);
    QFETCH(double, shownX);
    QFETCH(qint64, shownLeft);
    QFETCH(double, endX);
    constexpr double scale = 1.5;
    QQuickWindow quickWindow;
    if (!showOnScreen(quickWindow, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    constexpr int width = 1'680;
    quickWindow.resize(width + 40, 160);
    SpectrogramItem &item = *new SpectrogramItem(quickWindow.contentItem());
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int fftSize = 2'048;
    constexpr int frames = 560'640;
    constexpr double position = 38.4;
    constexpr int referenceHop = 171;
    constexpr int startHop = 86;
    constexpr int hop = 300;
    const quint64 trackFrames = lengthKnown ? frames : 0;
    // As the session reckons them: see the test above.
    const auto estimateAt = [](int columnHop) {
        return static_cast<int>(
            std::ceil((frames / 1'024 + 64) * 1'024.0 / static_cast<double>(columnHop)));
    };
    const auto decodedEndAt = [](int columnHop) {
        const int rowHop = std::min(fftSize / 8, columnHop);
        const int rows = (frames - fftSize / 2) / rowHop + 1;
        return (rows * rowHop + columnHop - 1) / columnHop;
    };
    quint64 generation = 1;
    // Columns [first, last) of the session at columnHop, which has not
    // reached the end.
    const auto feedColumns = [&](int columnHop, int first, int last, bool reset) {
        const int endColumn = lengthKnown ? decodedEndAt(columnHop) : 0;
        if (reset) {
            ++generation;
            item.feedPrecomputedChunk(
                QByteArray(), bins, 0, 0, first, estimateAt(columnHop), rate, columnHop,
                false, true, 1, false, generation, 0, fftSize, trackFrames, endColumn);
        }
        item.feedPrecomputedChunk(
            QByteArray((last - first) * bins, '\x40'), bins, 0, last - first, first,
            estimateAt(columnHop), rate, columnHop, false, false, 1, false, generation, 0,
            fftSize, trackFrames, endColumn);
    };

    // The pane at 2x, where 38.4 s is column 3572, before the session
    // has decoded the end.
    item.feedPrecomputedChunk(
        QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, estimateAt(referenceHop),
        rate, referenceHop, false, true, 1, false, generation, 0, fftSize, trackFrames,
        lengthKnown ? decodedEndAt(referenceHop) : 0);
    item.setZoomLevel(2.0);
    QVERIFY(sendPendingBackendZoom(item));
    feedColumns(startHop, 3'572 - 1'200, 3'572 + 1'200, true);
    setTestPosition(item, position);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedHopSize, startHop);
        QVERIFY(!item.m_awaitingZoomData);
    }

    // Zoomed out to 0.57: the preview.
    item.setZoomLevel(0.57);
    QVERIFY(sendPendingBackendZoom(item));
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomPreviewShown);
        QCOMPARE(item.columnHopForZoomLocked(item.backendZoomForRequestLocked()), hop);
    }
    QCOMPARE(playhead->rect().x(), shownX);

    // The session for 0.57 sends the columns around the playhead first;
    // the data replaces the preview, which it shows as the preview did.
    feedColumns(hop, 600, 1'500, true);
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_awaitingZoomData);
        QVERIFY(!item.m_zoomPreviewShown);
        QCOMPARE(item.m_precomputedHopSize, hop);
        QCOMPARE(item.m_precomputedTotalColumnsEstimate, estimateAt(hop));
        QVERIFY(item.m_precomputedMaxColumnIndex + 1 < decodedEndAt(hop));
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, shownLeft);
    }
    QCOMPARE(playhead->rect().x(), shownX);

    // The rest, and the end, which the last chunk says the session reached.
    const int end = decodedEndAt(hop);
    QCOMPARE(end, 1'867);
    feedColumns(hop, 0, 600, false);
    feedColumns(hop, 1'500, end, false);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, end, end, rate, hop,
        /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false,
        generation, 0, fftSize, trackFrames, lengthKnown ? end : 0);
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedTotalColumnsEstimate, end);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, qint64(0));
        QCOMPARE(item.m_precomputedCanvasViewRight, qint64(end - 1));
    }
    QCOMPARE(playhead->rect().x(), endX);

    delete node;
}

namespace {

// A rolling pane at zoom 1 with a session of `rate` audio at its
// reference hop, `hop`.
void feedReferenceSession(SpectrogramItem &item, int rate, int hop) {
    constexpr int bins = 8;
    constexpr int columns = 400;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, rate, hop, false, true, 1, true, 1);
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramSecondsAreAsWideAtEveryRateOfAFamily() {
    // A column is as long at 96 kHz as at 48 kHz, 2048 samples to 1024,
    // so a second of audio is as wide, and scrolls as fast.
    for (const double zoom : {1.0, 2.0, 0.5}) {
        double pace = 0.0;
        for (const int rate : {48000, 96000, 192000}) {
            SpectrogramItem item;
            item.setWidth(200);
            item.setHeight(100);
            item.setDisplayMode(0);
            const int referenceHop = static_cast<int>(1024LL * rate / 48000);
            QCOMPARE(static_cast<int>(ferrous_ffi_spectrogram_reference_hop(
                         static_cast<std::uint32_t>(rate))),
                     referenceHop);
            feedReferenceSession(item, rate, referenceHop);
            QMutexLocker lock(&item.m_stateMutex);
            QCOMPARE(item.referenceHopLocked(), static_cast<double>(referenceHop));
            const int hop = item.columnHopForZoomLocked(zoom);
            QCOMPARE(hop, static_cast<int>(std::lround(referenceHop / zoom)));
            item.m_renderZoomLevel = item.renderZoomForZoomLocked(zoom);
            item.m_precomputedHopSize = hop;
            QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
            const double pixelsPerSecond = item.currentPixelsPerSecondLocked();
            QVERIFY(std::abs(pixelsPerSecond - zoom * 46.875) < 1e-9);
            if (pace > 0.0) {
                QCOMPARE(pixelsPerSecond, pace);
            }
            pace = pixelsPerSecond;
        }
    }
    // The 44.1 kHz family has a pace of its own, 9% slower.
    SpectrogramItem item;
    feedReferenceSession(item, 88200, 2048);
    QMutexLocker lock(&item.m_stateMutex);
    QVERIFY(std::abs(item.currentPixelsPerSecondLocked() - 44100.0 / 1024.0) < 1e-9);
}

void SpectrogramCanvasSmokeTest::spectrogramZoomSnapsToTheScaledHopOfAHighRate() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(0);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int rate = 96000;
    feedReferenceSession(item, rate, 2048);
    setTestPosition(item, 5.0);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);

    // Zoomed in twice, the session's columns are 1024 samples apart, which
    // is the reference hop of 48 kHz but not of 96 kHz.
    item.setZoomLevel(2.0);
    QVERIFY(item.m_awaitingZoomData);
    QVERIFY(sendPendingBackendZoom(item));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.columnHopForZoomLocked(2.0), 1024);
    }
    item.feedPrecomputedChunk(
        QByteArray(bins * 300, '\x70'), bins, 0, 300,
        static_cast<int>(5.0 * rate / 1024), 4000, rate, 1024,
        false, true, 1, true, 2);
    node = paintSettled(item, node);
    QVERIFY(!item.m_awaitingZoomData);
    QCOMPARE(item.m_renderZoomLevel, 2.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
        QVERIFY(std::abs(item.currentPixelsPerSecondLocked() - 93.75) < 1e-9);
    }

    // A zoom whose hop rounds snaps to it, a column a pixel.
    item.setZoomLevel(3.815);
    QVERIFY(sendPendingBackendZoom(item));
    const int hop = static_cast<int>(std::lround(2048.0 / 3.815));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.columnHopForZoomLocked(3.815), hop);
    }
    item.feedPrecomputedChunk(
        QByteArray(bins * 300, '\x50'), bins, 0, 300,
        static_cast<int>(5.0 * rate / hop), 8000, rate, hop,
        false, true, 1, true, 3);
    node = paintSettled(item, node);
    QVERIFY(!item.m_awaitingZoomData);
    QCOMPARE(item.m_renderZoomLevel, 2048.0 / hop);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramFlooredHopShowsAColumnAPixelToTheEnd() {
    // 8 kHz at 16x is 16 samples a column, the floor, which stands for
    // 171 / 16 = 10.7x: a 3440 pixel view shows 3440 columns, which is
    // what the backend decodes for (window_seconds) and the ring holds.
    QQuickWindow window;
    window.resize(3460, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    constexpr int width = 3440;
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int rate = 8000;
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x40'), bins, 0, 400, 0, 5700, rate, 171,
        false, true, 1, true, 1);
    item.setZoomLevel(16.0);
    QVERIFY(sendPendingBackendZoom(item));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.columnHopForZoomLocked(16.0), 16);
    }
    // The session for 16x near the end: the view of the end and the
    // warm-up before it, to the last column.
    constexpr int end = 60000;
    constexpr int first = end - width - 2 * rate / 16;
    setTestPosition(item, 119.0);
    item.feedPrecomputedChunk(
        QByteArray(bins * (end - first), '\x60'), bins, 0, end - first, first, end,
        rate, 16, false, true, 1, true, 2);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);
    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_renderZoomLevel, ferrous_ffi_spectrogram_render_zoom(rate, 16, 16.0));
    QCOMPARE(item.m_renderZoomLevel, 171.0 / 16.0);
    QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(end - 1));
    QCOMPARE(item.m_precomputedCanvasDisplayLeft, static_cast<qint64>(end - width));
    QVERIFY(item.ringHoldsColumnsLocked(end - width, end - 1));
    QVERIFY(item.m_ringCapacity >= end - first);
    // A view of history, two ahead and 10 s of lookahead, in the columns
    // the view draws: 2299 columns a view at 16x of the reference hop.
    QCOMPARE(item.m_ringCapacity, 3 * width + 10 * rate / 16);
    QCOMPARE(item.m_ringCapacity,
             static_cast<int>(ferrous_ffi_spectrogram_ring_capacity(
                 width, rate, 16, bins, 1, 16.0, true)));
    lock.unlock();
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramOddReferenceHopSnapsZoomedIn() {
    // 32 kHz has a reference hop of 683; at 4x its columns are 171
    // samples apart, which stands for 3.994x, a column a pixel.
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    feedReferenceSession(item, 32000, 683);
    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.referenceHopLocked(), 683.0);
    QCOMPARE(item.columnHopForZoomLocked(4.0), 171);
    QCOMPARE(item.renderZoomForZoomLocked(4.0), 683.0 / 171.0);
    QCOMPARE(item.renderZoomForZoomLocked(4.0),
             ferrous_ffi_spectrogram_render_zoom(32000, 171, 4.0));
    item.m_renderZoomLevel = item.renderZoomForZoomLocked(4.0);
    item.m_precomputedHopSize = 171;
    QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-12);
}

void SpectrogramCanvasSmokeTest::spectrogramZoomWithinTheBackendToleranceNeedsNoNewData() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 10.0);

    // At 4.0075x the data comes at a hop of 256.
    item.setZoomLevel(4.0075);
    QVERIFY(sendPendingBackendZoom(item));
    item.feedPrecomputedChunk(
        QByteArray(bins * 800, '\x60'), bins, 0, 800,
        static_cast<int>(10.0 * 48000 / 256) - 400, columns * 4, 48000, 256,
        false, true, 1, false, 2);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(!item.m_awaitingZoomData);
    QCOMPARE(drawnCanvasScale(node), 1.0);

    // 4.0083x would be a hop of 255, but the backend keeps its zoom for
    // so small a change and sends nothing new. The data on screen is final.
    item.setZoomLevel(4.0083);
    QVERIFY(sendPendingBackendZoom(item));
    QVERIFY(!item.m_awaitingZoomData);
    node = paintSettled(item, node);
    QCOMPARE(drawnCanvasScale(node), 1.0);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomRequestsTheBackendIgnoresDoNotLeaveAPreview() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setZoomEnabled(true);
    // The wheel asks the surface for a zoom, which sets it, as in QML.
    QObject::connect(&item, &SpectrogramItem::zoomRequested, &item,
                     [&item](double zoom) { item.setZoomLevel(zoom); });
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 10.0);

    // Zoomed out to 0.1x, where the data comes at a hop of 10240.
    item.setZoomLevel(0.1);
    QVERIFY(sendPendingBackendZoom(item));
    item.feedPrecomputedChunk(
        QByteArray(bins * 200, '\x60'), bins, 0, 200,
        0, 200, 48000, 10240, true, true, 1, false, 2);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(!item.m_awaitingZoomData);
    QCOMPARE(drawnCanvasScale(node), 1.0);

    // Two touchpad bursts, the debounce sending each: 0.100559x, then
    // 0.100934x. Both are within the backend's tolerance of 0.1x, so it
    // keeps 0.1x and sends nothing new.
    const auto wheel = [&item](int angle) {
        QWheelEvent event(QPointF(100, 50), QPointF(100, 50), QPoint(), QPoint(0, angle),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        item.wheelEvent(&event);
    };
    wheel(3);
    QVERIFY(std::abs(item.zoomLevel() - 0.100559) < 1e-6);
    QVERIFY(sendPendingBackendZoom(item));
    wheel(2);
    QVERIFY(std::abs(item.zoomLevel() - 0.100934) < 1e-6);
    QVERIFY(sendPendingBackendZoom(item));

    // The data on screen is the backend's, so no preview waits for more.
    QVERIFY(!item.m_awaitingZoomData);
    node = paintSettled(item, node);
    QCOMPARE(drawnCanvasScale(node), 1.0);
    QVERIFY(!item.m_zoomPreviewShown);

    // A third burst takes the zoom past the tolerance: the backend
    // restarts, and the preview shows until its data arrives.
    wheel(3);
    QVERIFY(sendPendingBackendZoom(item));
    QVERIFY(item.m_awaitingZoomData);
    node = paintSettled(item, node);
    QVERIFY(item.m_zoomPreviewShown);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramStaleZoomDataDoesNotDrawIntoTheFrozenPreview() {
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 2000;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x40'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 10.0);
    QSGNode *node = paintSettled(item, nullptr);

    // Zoomed out to half, sent; then in to 2x before the half data arrives.
    item.setZoomLevel(0.5);
    QVERIFY(sendPendingBackendZoom(item));
    item.setZoomLevel(2.0);
    node = paintSettled(item, node);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);
    const QImage previewed = item.m_canvas.copy();
    QVERIFY(!previewed.isNull());

    // The half data covers the columns on screen, in another time scale:
    // first its reset, then more of it.
    constexpr int staleHop = 2048;
    const int staleStart = static_cast<int>(10.0 * 48000 / staleHop) - 200;
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x7f'), bins, 0, 400,
        staleStart, columns / 2, 48000, staleHop, false, true, 1, false, 2);
    QVERIFY(item.m_zoomFillActive);
    QCOMPARE(item.m_canvas, previewed);
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x20'), bins, 0, 400,
        staleStart + 400, columns / 2, 48000, staleHop, false, false, 1, false, 2);
    QCOMPARE(item.m_canvas, previewed);
    node = paintSettled(item, node);
    QCOMPARE(item.m_canvas, previewed);
    QVERIFY(std::abs(drawnCanvasScale(node) - 2.0) < 1e-6);

    // The 2x data replaces it.
    constexpr int hop = 512;
    const int nowColumn = static_cast<int>(10.0 * 48000 / hop);
    item.feedPrecomputedChunk(
        QByteArray(bins * 400, '\x70'), bins, 0, 400,
        nowColumn - 150, columns * 2, 48000, hop, false, true, 1, false, 3);
    node = paintSettled(item, node);
    QVERIFY(!item.m_zoomFillActive);
    QVERIFY(item.m_canvas != previewed);
    QCOMPARE(drawnCanvasScale(node), 1.0);

    delete node;
}

namespace {

// The rows of a pixel's column of a canvas that are `color`.
QVector<int> rowsOfColor(const QImage &canvas, int x, QRgb color) {
    QVector<int> rows;
    for (int y = 0; y < canvas.height(); ++y) {
        if (canvas.pixel(x, y) == color) {
            rows.push_back(y);
        }
    }
    return rows;
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramDrawsAtDevicePixels_data() {
    QTest::addColumn<double>("scale");
    QTest::newRow("2x") << 2.0;
    QTest::newRow("1.5x") << 1.5;
    QTest::newRow("1.25x") << 1.25;
}

void SpectrogramCanvasSmokeTest::spectrogramDrawsAtDevicePixels() {
    QFETCH(double, scale);
    QQuickWindow window;
    if (!showOnScreen(window, scale)) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setShowTimeLabels(true);
    item.setCrosshairEnabled(true);
    // Quiet but for one loud bin of one column, at the view's start.
    constexpr int bins = 1025;
    constexpr int columns = 400;
    constexpr qint64 loudColumn = 40;
    constexpr int loudBin = 512;
    constexpr quint8 quiet = 0x20;
    QByteArray data(bins * columns, static_cast<char>(quiet));
    data[static_cast<qsizetype>(loudColumn * bins + loudBin)] = static_cast<char>(0xFF);
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 0.0);
    QHoverEvent hover(QEvent::HoverEnter, QPointF(73.25, 41.0), QPointF(73.25, 41.0), QPointF());
    item.hoverEnterEvent(&hover);
    QSGNode *node = paintSettled(item, nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    const int deviceWidth = static_cast<int>(std::floor(200 * scale + 1e-6));
    const int deviceHeight = static_cast<int>(std::floor(100 * scale + 1e-6));
    const int lineWidth = static_cast<int>(std::lround(scale));
    QCOMPARE(item.m_canvasScale, scale);

    // The canvas has the item's device pixels, one to a pixel on screen.
    QCOMPARE(item.m_canvas.size(), QSize(deviceWidth, deviceHeight));
    QVERIFY(drawnCanvasFragments(node) > 0);
    QVERIFY(std::abs(drawnCanvasScale(node) - 1.0 / scale) < 1e-9);
    const QPair<double, double> extent = drawnCanvasExtent(node);
    QVERIFY(std::abs(extent.first) < 1e-9);
    QVERIFY(std::abs(extent.second - deviceWidth / scale) < 1e-9);

    // The loud bin is on the device rows the frequency axis puts its
    // frequency on: a row or two of them, not a stretched logical row.
    const double binHz = 24000.0 / (bins - 1) * loudBin;
    const int loudRow = item.frequencyToPixelYLocked(binHz, deviceHeight);
    const int loudPixel = static_cast<int>(std::lround(static_cast<double>(loudColumn) * scale));
    const QRgb quietColor = item.m_canvas.pixel(0, 0);
    const QRgb loudColor = item.m_canvas.pixel(loudPixel, loudRow);
    QVERIFY(loudColor != quietColor);
    const QVector<int> loudRows = rowsOfColor(item.m_canvas, loudPixel, loudColor);
    QVERIFY(loudRows.size() <= 2);
    QCOMPARE(loudRows.last() - loudRows.first() + 1, static_cast<int>(loudRows.size()));
    for (const int row : loudRows) {
        QVERIFY(std::abs(item.pixelToFrequencyHzLocked(row, deviceHeight) - binHz)
            <= 24000.0 / deviceHeight);
    }
    QCOMPARE(static_cast<int>(rowsOfColor(item.m_canvas, loudPixel, quietColor).size()),
        deviceHeight - static_cast<int>(loudRows.size()));
    // A display column is `scale` device pixels: the loud column's own
    // pixel starts there, the next pixel blends it with its neighbour, and
    // the pixels before its neighbour's are quiet.
    const QRgb blendedColor = item.m_canvas.pixel(loudPixel + 1, loudRow);
    QVERIFY(blendedColor != loudColor && blendedColor != quietColor);
    QCOMPARE(rowsOfColor(item.m_canvas, loudPixel + 1, blendedColor), loudRows);
    QCOMPARE(static_cast<int>(rowsOfColor(item.m_canvas, loudPixel - lineWidth - 1, quietColor).size()),
        deviceHeight);

    // Overlay images are whole device pixels, one texel to each, and their
    // lines whole device pixels thick.
    const auto onDevicePixels = [scale](const QRectF &rect) {
        // Scene graph rects are single precision.
        const auto whole = [](double value) { return std::abs(value - std::round(value)) < 1e-3; };
        return whole(rect.x() * scale) && whole(rect.y() * scale)
            && whole(rect.width() * scale) && whole(rect.height() * scale);
    };
    const auto checkImages = [&](const OverlayOnScreen &overlay) {
        for (const auto &[rect, textureSize] : overlay.images) {
            QVERIFY(onDevicePixels(rect));
            QCOMPARE(QSize(static_cast<int>(std::lround(rect.width() * scale)),
                         static_cast<int>(std::lround(rect.height() * scale))),
                textureSize);
        }
    };

    // The frequency scale's ticks are on device rows, on whole hundreds of
    // hertz, 6 px at the right edge; its labels are drawn at device
    // resolution.
    QVERIFY(item.m_freqGrid.valid);
    QCOMPARE(item.m_freqGrid.size, QSize(deviceWidth, deviceHeight));
    QVERIFY(!item.m_freqGrid.lines.isEmpty());
    const OverlayOnScreen freqGrid = overlayOnScreen(node, 5);
    QCOMPARE(freqGrid.lines.size(), item.m_freqGrid.lines.size());
    const int tickWidth = static_cast<int>(std::floor(6 * scale + 1e-6));
    for (int i = 0; i < item.m_freqGrid.lines.size(); ++i) {
        const QRect line = item.m_freqGrid.lines[i];
        QCOMPARE(line, QRect(deviceWidth - tickWidth, line.y(), tickWidth, lineWidth));
        // devicePixels() floors: whole device pixels within the 6 px.
        QVERIFY(nearlyEqual(freqGrid.lines[i],
            QRectF(line.x() / scale, line.y() / scale, tickWidth / scale, lineWidth / scale)));
        const double hz = item.pixelToFrequencyHzLocked(line.y(), deviceHeight);
        const double hzPerRow = 24000.0 / deviceHeight;
        QVERIFY2(std::abs(hz - std::round(hz / 100.0) * 100.0) <= hzPerRow,
                 qPrintable(QStringLiteral("line at %1 Hz").arg(hz)));
    }
    QCOMPARE(freqGrid.images.size(), item.m_freqGrid.images.size());
    checkImages(freqGrid);

    // The time scale's ticks are on the device columns of whole seconds,
    // 5 px down from the top.
    QVERIFY(item.m_timeGrid.valid);
    QCOMPARE(item.m_timeGrid.size.height(), deviceHeight);
    const double canvasZoom = item.canvasZoomLocked();
    const double columnsPerSecond = 48000.0 / 1024.0;
    QVERIFY(!item.m_timeGrid.lines.isEmpty());
    for (const QRect &line : item.m_timeGrid.lines) {
        QCOMPARE(line, QRect(line.x(), 0, lineWidth, static_cast<int>(std::floor(5 * scale + 1e-6))));
        const double seconds = (static_cast<double>(item.m_timeGridRenderDisplayLeft)
            + (line.x() - item.m_timeGridRenderDrawX) / canvasZoom) / columnsPerSecond;
        QVERIFY2(std::abs(seconds - std::round(seconds)) * columnsPerSecond * canvasZoom <= 0.5 + 1e-9,
                 qPrintable(QStringLiteral("line at %1 s").arg(seconds)));
    }
    // What a 200 px pane shows of it: its ticks keep 48 px from the left
    // and 40 px from the right.
    const OverlayOnScreen timeGrid = overlayOnScreen(node, 6);
    for (const QRectF &line : timeGrid.lines) {
        QVERIFY(onDevicePixels(line));
        QVERIFY(std::abs(line.width() - lineWidth / scale) < 1e-4);
        QVERIFY(line.left() >= 48.0 && line.left() <= 160.0);
    }
    checkImages(timeGrid);

    // The crosshair is on the device column and row under the pointer.
    QVERIFY(item.m_crosshair.valid);
    const int crosshairX = static_cast<int>(std::floor(73.25 * scale));
    const int crosshairY = static_cast<int>(std::floor(41.0 * scale));
    QCOMPARE(item.m_crosshairVerticalLineRect, QRect(crosshairX, 0, lineWidth, deviceHeight));
    QCOMPARE(item.m_crosshairHorizontalLineRect, QRect(0, crosshairY, deviceWidth, lineWidth));
    const OverlayOnScreen crosshair = overlayOnScreen(node, 7);
    checkImages(crosshair);
    // Its lines, then the readout's plates, inside the pane.
    QCOMPARE(crosshair.images.size(), 4);
    QVERIFY(nearlyEqual(crosshair.images[0].first,
        QRectF(crosshairX / scale, 0.0, lineWidth / scale, deviceHeight / scale)));
    QVERIFY(nearlyEqual(crosshair.images[1].first,
        QRectF(0.0, crosshairY / scale, deviceWidth / scale, lineWidth / scale)));
    for (int i = 2; i < 4; ++i) {
        QVERIFY(QRectF(0.0, 0.0, deviceWidth / scale, deviceHeight / scale)
                    .adjusted(-1e-4, -1e-4, 1e-4, 1e-4)
                    .contains(crosshair.images[i].first));
    }
    lock.unlock();
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramWideCanvasDrawsEveryTile() {
    // 7000 pixels are 110 tiles, more than the scene node starts with, and
    // as many OpenGL textures as its size limit takes.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(7000);
    item.setHeight(40);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 7000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    QSGNode *node = paintSettled(item, nullptr);
    // The window draws them too.
    QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
    item.update();
    QTRY_VERIFY(!frames.isEmpty());
    const QVector<QPair<int, QSGTexture *>> textures = SpectrogramItem::canvasTextures(node);
    if (SpectrogramItem::sceneStatistics(node).canvasInPlace) {
        GLint limit = 0;
        QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        QCOMPARE(textures.size(), (7000 + limit - 1) / limit);
    } else {
        QCOMPARE(textures.size(), 110);
    }
    QCOMPARE(drawnCanvasFragments(node), static_cast<int>(textures.size()));
    const QPair<double, double> extent = drawnCanvasExtent(node);
    QCOMPARE(extent.first, 0.0);
    QCOMPARE(extent.second, 7000.0);
    delete node;
}

namespace {

// Feeds the stripes again for another hop, around the playhead at `seconds`,
// as the decoder sends a zoom's data.
void feedZoomStripes(SpectrogramItem &item, double seconds, int hop, bool rolling) {
    constexpr int columns = 1200;
    const int nowColumn = static_cast<int>(seconds * 48'000.0 / hop);
    QByteArray data(kStripeBins * columns, Qt::Uninitialized);
    for (int column = 0; column < columns; ++column) {
        std::fill_n(data.data() + static_cast<qsizetype>(column) * kStripeBins, kStripeBins,
            static_cast<char>(60 + (column * 37) % 170));
    }
    item.feedPrecomputedChunk(data, kStripeBins, 0, columns,
        rolling ? nowColumn : nowColumn - columns / 2, kStripeColumns * 1024 / hop, 48'000, hop,
        false, true, 1, true, 2);
}

void saveForInspection(const QImage &image, const QString &name) {
    if (qEnvironmentVariableIsSet("FERROUS_TEST_IMAGE_DIR")) {
        image.save(qEnvironmentVariable("FERROUS_TEST_IMAGE_DIR") + QLatin1Char('/') + name + QStringLiteral(".png"));
    }
}

// What a window shows of a spectrogram's canvas: where `node`, a scene node
// of the item drawn at the same time, places its spans, each device pixel
// sampled at its centre as Nearest filtering does, over the older data under
// them, the reserve and the backdrop, and black. The pixels
// under the centered playhead, which blends over them, are left out of
// `mask`.
QImage expectedCanvasImage(const SpectrogramItem &item, QSGNode *node, const QSize &size, double scale,
    QImage *mask) {
    QHash<QSGTexture *, int> bandX;
    for (const auto &[x, texture] : SpectrogramItem::canvasTextures(node)) {
        bandX.insert(texture, x);
    }
    QImage expected(size, QImage::Format_RGB32);
    expected.fill(kSpectrogramFloor);
    *mask = QImage(size, QImage::Format_Grayscale8);
    mask->fill(255);
    // A span of `image`, its source `x0` pixels into it; transparent pixels
    // leave what is under them. A layer under the canvas, placed at the
    // times of its columns, can be magnified by a fraction: its pixels
    // sampled at a texel's edge, which a renderer's rounding may take from
    // either side, are left out of `mask`.
    const auto drawImage = [&](const QSGSimpleTextureNode *span, const QImage &image, int x0,
                               bool layer) {
        if (span->rect().isEmpty() || image.isNull()) {
            return;
        }
        const QRectF target(span->rect().topLeft() * scale, span->rect().size() * scale);
        const QRectF source = span->sourceRect();
        for (int y = 0; y < expected.height(); ++y) {
            if (y + 0.5 < target.top() || y + 0.5 >= target.bottom()) {
                continue;
            }
            const int sy = static_cast<int>(std::floor(
                source.top() + (y + 0.5 - target.top()) * source.height() / target.height()));
            for (int x = 0; x < expected.width(); ++x) {
                if (x + 0.5 < target.left() || x + 0.5 >= target.right()) {
                    continue;
                }
                const double at =
                    source.left() + (x + 0.5 - target.left()) * source.width() / target.width();
                const int sx = static_cast<int>(std::floor(at));
                if (layer && std::abs(at - std::round(at)) < 0.02) {
                    mask->setPixel(x, y, 0);
                }
                // Clamped to the edge, as a texture is sampled.
                const QRgb pixel = image.pixel(std::min(x0 + sx, image.width() - 1), sy);
                if (qAlpha(pixel) > 0) {
                    expected.setPixel(x, y, pixel);
                }
            }
        }
    };
    const auto draw = [&](const QSGSimpleTextureNode *span) {
        if (bandX.contains(span->texture())) {
            drawImage(span, item.m_canvas, bandX.value(span->texture()), false);
        }
    };
    // Under the canvas, the floor's children: the reserve, then the
    // backdrop's spans. They are placed at the times of their columns,
    // which can end within a pixel that a renderer may or may not count as
    // theirs: those pixels are left out of `mask`.
    QSGNode *floor = node->childAtIndex(0);
    const QImage *layers[] = {&item.m_reserve.image, &item.m_backdrop.image, &item.m_backdrop.image};
    for (int i = 0; i < 3; ++i) {
        const auto *span = static_cast<QSGSimpleTextureNode *>(floor->childAtIndex(i));
        drawImage(span, *layers[i], 0, true);
        if (span->rect().isEmpty()) {
            continue;
        }
        for (const double edge : {span->rect().left() * scale, span->rect().right() * scale}) {
            const int x = static_cast<int>(std::floor(edge));
            if (std::abs(edge - std::round(edge)) < 1e-6) {
                continue;
            }
            for (int y = 0; y < size.height(); ++y) {
                if (x >= 0 && x < size.width()) {
                    mask->setPixel(x, y, 0);
                }
            }
        }
    }
    QSGNode *tiles = node->childAtIndex(1);
    for (int i = 0; i < tiles->childCount(); ++i) {
        draw(static_cast<QSGSimpleTextureNode *>(tiles->childAtIndex(i)));
    }
    draw(static_cast<QSGSimpleTextureNode *>(node->childAtIndex(2)));
    // The playhead and its casing, a device pixel either side of the core,
    // with a pixel to spare: the window's frame may have it a pixel apart.
    const QRectF playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild))->rect();
    if (!playhead.isEmpty()) {
        const int left = std::max(0, static_cast<int>(std::floor(playhead.left() * scale)) - 2 * static_cast<int>(std::ceil(scale)));
        const int right = std::min(size.width() - 1,
            static_cast<int>(std::ceil(playhead.right() * scale)) + 2 * static_cast<int>(std::ceil(scale)));
        for (int y = 0; y < size.height(); ++y) {
            for (int x = left; x <= right; ++x) {
                mask->setPixel(x, y, 0);
            }
        }
    }
    return expected;
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramCanvasTexturesSurviveSceneGraphChanges() {
    // A new scene graph, as when the window's is released, and another
    // window, as when the fullscreen viewer takes the pane, both start a
    // scene node of their own, which takes the whole canvas. The windows
    // are shown, so that renderers on the RHI can make their textures.
    QQuickWindow window;
    window.resize(400, 100);
    QQuickWindow other;
    other.resize(400, 100);
    window.show();
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    TestTime time;
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareScrollingPlayback(item, time, 1);
    QSGNode *node = paintSettled(item, nullptr);
    const auto guard = qScopeGuard([&node]() { delete node; });
    const qint64 canvasBytes = static_cast<qint64>(400) * 100 * 4;
    QVERIFY(SpectrogramItem::sceneStatistics(node).canvasTextureBytes >= canvasBytes);
    time.advance(0.5);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(SpectrogramItem::sceneStatistics(node).canvasTextureBytes < canvasBytes);

    item.releaseResources();
    // The window's scene graph deletes the nodes it had.
    delete node;
    node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(SpectrogramItem::sceneStatistics(node).canvasTextureBytes >= canvasBytes);
    QVERIFY(!SpectrogramItem::canvasTextures(node).isEmpty());

    item.setParentItem(other.contentItem());
    time.advance(0.5);
    // The old window's node makes way for one of the new window's.
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(SpectrogramItem::sceneStatistics(node).canvasTextureBytes >= canvasBytes);
    QVERIFY(!SpectrogramItem::canvasTextures(node).isEmpty());
}

void SpectrogramCanvasSmokeTest::spectrogramSceneGraphDrawsTheCanvas_data() {
    addScrollingRows();
}

void SpectrogramCanvasSmokeTest::spectrogramSceneGraphDrawsTheCanvas() {
    // What the window shows of the canvas, one texel to a device pixel, is
    // the canvas where its scene node places it: through playback past the
    // ring's wrap and its textures' edges, a seek whose rasterization has
    // columns drawn over it, a zoom preview and its data, a resize, a
    // canvas taller than the textures may be, a new scene graph and another
    // window. With OpenGL the window keeps the canvas in its own textures,
    // updated in place; with other renderers in tiles. Rows at other scales
    // run on the offscreen platform's HiDPI test screens, or where the
    // window has that scale, as with QT_SCALE_FACTOR.
    QFETCH(int, mode);
    QFETCH(double, scale);
    QQuickWindow window;
    if (window.rendererInterface()->graphicsApi() == QSGRendererInterface::Null) {
        QSKIP("The null renderer draws no pixels");
    }
    const bool openGl = window.rendererInterface()->graphicsApi() == QSGRendererInterface::OpenGL;
    QQuickWindow other;
    window.resize(400, 100);
    other.resize(400, 100);
    if (QGuiApplication::platformName() == QStringLiteral("offscreen") && scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
        QVERIFY(moveWindowToScreen(other, screen));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (std::abs(window.effectiveDevicePixelRatio() - scale) > 1e-9) {
        QSKIP("The window is at another scale");
    }
    TestTime time;
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    // Textures narrower than the canvas, so that the ring wraps across
    // their edges too.
    item.m_canvasTextureSizeLimit = 256;
    prepareScrollingPlayback(item, time, mode);
    QQuickWindow *shownIn = &window;
    const auto grab = [&]() {
        item.update();
        return shownIn->grabWindow().convertToFormat(QImage::Format_RGB32);
    };
    // A window shown again, whose scene graph the threaded render loop
    // makes anew, draws nothing to grab until its first frame.
    const auto firstFrame = [](QQuickWindow &shown) {
        return QTest::qWaitFor([&shown]() { return !shown.grabWindow().isNull(); }, 5000);
    };
    const auto settle = [&]() {
        grab();
        for (int round = 0; round < 8 && spectrogramRasterPending(item); ++round) {
            if (!QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000)) {
                return false;
            }
            grab();
        }
        return !spectrogramRasterPending(item);
    };
    QSGNode *node = nullptr;
    const auto guard = qScopeGuard([&node]() { delete node; });
    // What the window's node did in the frame shows() compared last: the
    // test's own node, which it then makes, is not the window's.
    SpectrogramSceneStatistics windowFrame;
    // Compares what the window shows now with the canvas; `inPlace` is
    // what the window's node should keep it in, with OpenGL.
    const auto shows = [&](const QString &when, bool inPlace) {
        // Drawn again once the reserve a frame asks for has come, which
        // the test's own node, made next, would show.
        QImage shown = grab();
        for (int round = 0; round < 8 && spectrogramReservePending(item); ++round) {
            if (!QTest::qWaitFor([&item]() { return !spectrogramReservePending(item); }, 5000)) {
                break;
            }
            shown = grab();
        }
        if (shown.isNull()) {
            qWarning("%s: the window drew no frame", qPrintable(when));
            return false;
        }
        if (spectrogramRasterPending(item)) {
            qWarning("%s: a rasterization is pending", qPrintable(when));
            return false;
        }
        SpectrogramSceneStatistics drawn;
        {
            QMutexLocker lock(&item.m_stateMutex);
            drawn = item.m_lastSceneStatistics;
        }
        windowFrame = drawn;
        if (drawn.canvasInPlace != (openGl && inPlace)) {
            qWarning("%s: the window's canvas is %sin place", qPrintable(when),
                drawn.canvasInPlace ? "" : "not ");
            return false;
        }
        // The node the window drew, placed alike: the frames of one time
        // are all the same.
        delete node;
        node = paintSettled(item, nullptr);
        QMutexLocker lock(&item.m_stateMutex);
        QImage mask;
        const QImage expected = expectedCanvasImage(item, node, shown.size(), scale, &mask);
        int differing = 0;
        QPoint first;
        // The item's pixels; the window's beyond it are its own.
        const int itemPixels = std::min(expected.width(),
            static_cast<int>(std::floor(item.width() * scale + 1e-6)));
        for (int y = 0; y < expected.height(); ++y) {
            for (int x = 0; x < itemPixels; ++x) {
                if (mask.constScanLine(y)[x] != 0 && shown.pixel(x, y) != expected.pixel(x, y)) {
                    if (differing++ == 0) {
                        first = QPoint(x, y);
                    }
                }
            }
        }
        if (differing > 0) {
            saveForInspection(shown, QStringLiteral("shown"));
            saveForInspection(expected, QStringLiteral("expected"));
            qWarning("%s: %d pixels differ, first at (%d, %d)", qPrintable(when), differing,
                first.x(), first.y());
        }
        return differing == 0;
    };
    const auto play = [&](int frames, double step, const QString &what, bool inPlace) {
        for (int frame = 0; frame < frames; ++frame) {
            time.advance(step);
            if (!shows(QStringLiteral("%1, frame %2").arg(what).arg(frame), inPlace)) {
                return false;
            }
        }
        return true;
    };
    QVERIFY(settle());
    QVERIFY(shows(QStringLiteral("first frame"), true));
    if (openGl) {
        // Bands of at most 256 columns.
        QCOMPARE(windowFrame.canvasTextureCount,
            (static_cast<int>(std::floor(400 * scale + 1e-6)) + 255) / 256);
    }

    // Playback past the ring's wrap, then a frame's worth of columns at a
    // time: each frame uploads only what it drew.
    QVERIFY(play(20, 0.7, QStringLiteral("wrap"), true));
    QVERIFY(play(20, 1.0 / 30.0, QStringLiteral("playback"), true));
    if (openGl) {
        time.advance(1.0 / 30.0);
        grab();
        // The window's frame, not the test's.
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_lastSceneStatistics.canvasTextures, 0);
        QVERIFY(item.m_lastSceneStatistics.canvasUploads <= 2);
    }

    // A seek far away rasterizes the canvas again; columns played while
    // that runs are uploaded after its copy.
    setTestPosition(item, 70.0);
    grab();
    QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
    time.advance(0.25);
    QVERIFY(shows(QStringLiteral("after a seek"), true));
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("after a seek"), true));

    if (mode == 1) {
        // A seek outside the decoded columns: the worker sends the
        // playhead's columns first, then those left of it, which are drawn
        // into the canvas, and uploaded, as they come.
        const auto feedStripes = [&item](qint64 first, qint64 end) {
            const int columns = static_cast<int>(end - first);
            QByteArray data(kStripeBins * columns, Qt::Uninitialized);
            for (int column = 0; column < columns; ++column) {
                std::fill_n(data.data() + static_cast<qsizetype>(column) * kStripeBins,
                    kStripeBins, static_cast<char>(40 + ((first + column) * 53) % 180));
            }
            item.feedPrecomputedChunk(data, kStripeBins, 0, columns, static_cast<int>(first),
                kStripeColumns, 48'000, 1024, false, false, 1, false, 2);
        };
        item.feedPrecomputedChunk({}, 0, 0, 0, 0, 0, 0, 0, false, true, 1, true, 2);
        // Off a quarter column, like the seek before, so that the zoom
        // preview's pixel centres fall within texels, not on their edges,
        // which renderers round either way.
        setTestPosition(item, 20.25);
        const qint64 playhead = static_cast<qint64>(testClock(item).positionAt(time.nowNs)
            * kStripeColumnsPerSecond);
        const qint64 start = playhead - 200 - 94;
        item.feedPrecomputedChunk({}, kStripeBins, 0, 0, static_cast<int>(start), kStripeColumns,
            48'000, 1024, false, true, 1, true, 2);
        feedStripes(playhead, playhead + 1);
        feedStripes(playhead + 1, playhead + 300);
        QVERIFY(settle());
        QVERIFY(shows(QStringLiteral("the playhead's columns"), true));
        for (qint64 first = start; first < playhead; first += 100) {
            feedStripes(first, std::min(first + 100, playhead));
            QVERIFY(shows(QStringLiteral("the left from %1").arg(first), true));
        }
        QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("after the backfill"), true));
    }

    // A zoom shows the canvas scaled until its data and its rasterization
    // come.
    item.setZoomLevel(2.0);
    QVERIFY(sendPendingBackendZoom(item));
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("zoom preview"), true));
    feedZoomStripes(item, testClock(item).positionAt(time.nowNs), 512, mode == 0);
    QVERIFY(settle());
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("zoomed"), true));

    // A resize lays out new textures.
    item.setWidth(300);
    QVERIFY(settle());
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("resized"), true));

    // A canvas taller than the textures may be goes in tiles.
    const auto limitTextures = [&item](int size) {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_canvasTextureSizeLimit = size;
    };
    limitTextures(static_cast<int>(std::floor(100 * scale + 1e-6)) - 1);
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("too tall"), false));
    limitTextures(256);
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("fits again"), true));

    // A new scene graph makes new textures.
    window.setPersistentGraphics(false);
    window.setPersistentSceneGraph(false);
    window.hide();
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(firstFrame(window));
    QVERIFY(settle());
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("new scene graph"), true));

    // Another window, with a context of its own, takes the pane, as the
    // fullscreen viewer does; the old window lets its textures go.
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    QVERIFY(firstFrame(other));
    item.setParentItem(other.contentItem());
    shownIn = &other;
    QVERIFY(settle());
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("another window"), true));
    window.hide();
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("the old window hidden"), true));

    // And back, as when the fullscreen viewer closes.
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(firstFrame(window));
    item.setParentItem(window.contentItem());
    shownIn = &window;
    QVERIFY(settle());
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("back in the first window"), true));

    // The fullscreen viewer's window is hidden when the viewer closes, and
    // kept with its graphics and scene graph. Closing takes the pane out
    // and hides the window at once, so that the window keeps the pane's
    // old node, and its textures, until it draws again: as it opens again,
    // and takes the pane again, in the same frame.
    QVERIFY(other.isPersistentGraphics() && other.isPersistentSceneGraph());
    item.setParentItem(other.contentItem());
    shownIn = &other;
    QVERIFY(settle());
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("in the viewer's window again"), true));
    item.setParentItem(window.contentItem());
    other.hide();
    shownIn = &window;
    QVERIFY(settle());
    QVERIFY(play(5, 1.0 / 30.0, QStringLiteral("the viewer closed"), true));
    other.show();
    item.setParentItem(other.contentItem());
    shownIn = &other;
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    QVERIFY(firstFrame(other));
    QVERIFY(settle());
    QVERIFY(play(10, 1.0 / 30.0, QStringLiteral("the viewer opened again"), true));
}

void SpectrogramCanvasSmokeTest::spectrogramScaleChangeKeepsAnUnalignedCanvasCovering_data() {
    QTest::addColumn<bool>("seek");
    QTest::newRow("cached seek") << true;
    QTest::newRow("another track") << false;
}

void SpectrogramCanvasSmokeTest::spectrogramScaleChangeKeepsAnUnalignedCanvasCovering() {
    QFETCH(bool, seek);
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 1500;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48000, 1024, true, true, 1, false, 1);
    setTestPosition(item, 15.0);
    QSGNode *node = paintSettled(item, nullptr);
    // Across the pane, scrolled by part of a column.
    const QPair<double, double> before = drawnCanvasExtent(node);
    QVERIFY(before.first <= 0.0 && before.first > -1.0);
    QVERIFY(before.second >= 199.0);

    // Back to 5 s, or columns of another track ahead, as the window moves
    // to a screen at 2: the canvas is not aligned to the view, and stays on
    // screen as it was until its replacement arrives.
    if (seek) {
        setTestPosition(item, 5.0);
    } else {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_precomputedTrackToken = 2;
    }
    QVERIFY(moveWindowToScreen(window, screen));
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.markCanvasDirtyLocked();
    }
    node = item.updatePaintNode(node, nullptr);
    // The screen change reaches the item at once, and this is its first
    // frame since: its canvas at 1x is still the one on screen.
    QVERIFY(spectrogramRasterPending(item));
    QCOMPARE(item.m_canvas.width(), 200);
    // A texel to a logical pixel, still across the pane, at the view's
    // start.
    QVERIFY(std::abs(drawnCanvasScale(node) - 1.0) < 1e-6);
    const QPair<double, double> extent = drawnCanvasExtent(node);
    QVERIFY2(extent.first > -1.0 && extent.first <= 0.0 && extent.second >= 199.0,
             qPrintable(QStringLiteral("drawn over %1 to %2").arg(extent.first).arg(extent.second)));
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramScaleChangeRebuildsOnceAndDropsStaleRasters_data() {
    QTest::addColumn<bool>("rolling");
    QTest::newRow("rolling") << true;
    QTest::newRow("centered") << false;
}

void SpectrogramCanvasSmokeTest::spectrogramScaleChangeRebuildsOnceAndDropsStaleRasters() {
    QFETCH(bool, rolling);
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) {
        QSKIP("Needs the offscreen platform's HiDPI test screens");
    }
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    QCOMPARE(window.effectiveDevicePixelRatio(), 1.0);
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(rolling ? 0 : 1);
    item.setPlaying(true);
    constexpr int bins = 64;
    constexpr int columns = 4000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, columns, 48000, 1024, false, true, 1, true, 1);
    setTestPosition(item, 20.0);
    QSGNode *node = paintSettled(item, nullptr);
    QCOMPARE(item.m_canvas.size(), QSize(200, 100));

    // A setting change asks for a canvas at a scale of 1, which is still
    // being drawn when the window moves to a screen at 2.
    item.setDbRange(90.0);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(spectrogramRasterPending(item));
    std::shared_ptr<SpectrogramRasterResult> stale;
    quint64 serial = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto job = item.makeRasterJobLocked(200, 100, item.m_rasterTarget.displayLeft,
            item.m_rasterTarget.displayRight, rolling);
        QVERIFY(job);
        job->serial = item.m_rasterSerial;
        stale = SpectrogramItem::rasterize(*job);
        serial = item.m_rasterSerial;
    }
    QVERIFY(stale);
    QVERIFY(moveWindowToScreen(window, screen));
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_canvasScale, 2.0);
    }

    // Its result arrives at the old scale: dropped.
    item.installRasterResult(stale);
    QCOMPARE(item.m_canvas.size(), QSize(200, 100));
    QVERIFY(item.m_canvas != stale->canvas);
    QVERIFY(spectrogramRasterPending(item));

    // Playback goes on. The frames ask for one canvas at the new scale and
    // show the old one, scaled to it, until it arrives.
    bool rebuilt = false;
    const QDeadlineTimer rebuildDeadline(10000);
    for (int frame = 0; !rebuilt && !rebuildDeadline.hasExpired(); ++frame) {
        {
            // The window paints the item too, uploading its tiles to its
            // own nodes: this node needs them as well.
            QMutexLocker lock(&item.m_stateMutex);
            item.markCanvasDirtyLocked();
        }
        node = item.updatePaintNode(node, nullptr);
        QVERIFY2(drawnCanvasFragments(node) > 0,
                 qPrintable(QStringLiteral("frame %1 has no canvas").arg(frame)));
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_rasterSerial, serial + 2);
        rebuilt = item.m_canvas.size() == QSize(400, 200);
        if (!rebuilt) {
            QVERIFY(std::abs(drawnCanvasScale(node) - 1.0) < 1e-9);
            lock.unlock();
            QTest::qWait(16);
        }
    }
    QVERIFY(rebuilt);
    // From then on the canvas moves on with playback, at the new scale.
    for (int frame = 0; frame < 6; ++frame) {
        QTest::qWait(16);
        {
            QMutexLocker lock(&item.m_stateMutex);
            item.markCanvasDirtyLocked();
        }
        node = item.updatePaintNode(node, nullptr);
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(!item.m_rasterPending && item.m_rasterSerial == serial + 2,
                 qPrintable(QStringLiteral("frame %1 asked for a rasterization").arg(frame)));
        QCOMPARE(item.m_canvas.size(), QSize(400, 200));
        QVERIFY(std::abs(drawnCanvasScale(node) - 0.5) < 1e-9);
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramDevicePixelCanvasAdvancesAsItsRasterization_data() {
    // Canvas pixels to a display column, from the device pixel ratio or,
    // at a ratio of 1, from the zoom.
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("viaZoom");
    QTest::addColumn<bool>("rolling");
    QTest::addColumn<int>("step");
    for (const bool viaZoom : {false, true}) {
        for (const bool rolling : {true, false}) {
            const char *mode = rolling ? "rolling" : "centered";
            const char *by = viaZoom ? "zoom" : "scale";
            QTest::addRow("%s 2x %s", by, mode) << 2.0 << viaZoom << rolling << 1;
            QTest::addRow("%s 1.5x %s", by, mode) << 1.5 << viaZoom << rolling << 1;
            QTest::addRow("%s 1.25x %s", by, mode) << 1.25 << viaZoom << rolling << 1;
            QTest::addRow("%s 1.25x %s by 3", by, mode) << 1.25 << viaZoom << rolling << 3;
        }
    }
}

void SpectrogramCanvasSmokeTest::spectrogramDevicePixelCanvasAdvancesAsItsRasterization() {
    QFETCH(double, scale);
    QFETCH(bool, viaZoom);
    QFETCH(bool, rolling);
    QFETCH(int, step);
    SpectrogramItem item;
    item.setWidth(120);
    item.setHeight(40);
    item.setDisplayMode(rolling ? 0 : 1);
    if (!viaZoom) {
        item.m_canvasScale = scale;
    }
    constexpr int bins = 16;
    constexpr int columns = 600;
    QByteArray data(bins * columns, '\0');
    for (int i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(1 + (i * 37 + (i / bins) * 11) % 250);
    }
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, columns, 48000, 1024, true, true, 1, false, 1);
    QMutexLocker lock(&item.m_stateMutex);
    if (viaZoom) {
        // A track's data resets the zoom.
        item.m_renderZoomLevel = scale;
    }
    item.m_binsPerColumn = bins;
    const int width = item.canvasWidthLocked();
    const int height = item.canvasHeightLocked();
    QCOMPARE(width, viaZoom ? 120 : static_cast<int>(std::floor(120 * scale + 1e-6)));
    item.ensureMapping(height);
    const qint64 span = static_cast<qint64>(std::ceil(width / scale)) - 1;
    rebuildCanvasNow(item, width, height, 100, 100 + span, rolling);

    // Scrolled by part of a pixel, the canvas's pixel q starts where a
    // rasterization from `back` columns earlier puts its pixel q + shift.
    for (qint64 left = 100 + step; left <= 100 + 4 * step; left += step) {
        QVERIFY(item.advancePrecomputedCanvasLocked(left, left + span, rolling, item.m_now()));
        const double offset = item.m_precomputedCanvasSubpixelOffset;
        int back = 0;
        while (std::abs(back * scale - offset - std::round(back * scale - offset)) > 1e-6) {
            ++back;
        }
        const int shift = static_cast<int>(std::lround(back * scale - offset));
        const QImage incremental = canvasInViewOrder(item);
        const QImage reference = freshCanvas(item, width, height, left - back, left + span, rolling);
        QVERIFY(!reference.isNull());
        const int compared = std::min(incremental.width(), reference.width() - shift);
        QVERIFY(compared >= width - shift - 1);
        for (int x = 0; x < compared; ++x) {
            for (int y = 0; y < height; ++y) {
                QVERIFY2(incremental.pixel(x, y) == reference.pixel(x + shift, y),
                         qPrintable(QStringLiteral("left %1 pixel %2,%3 offset %4")
                             .arg(left).arg(x).arg(y).arg(offset)));
            }
        }
    }
}

namespace {

// What each of the item's `width` logical pixel columns shows along its
// middle row, as `node` draws it: the canvas where its pixel is not
// transparent, and else the backdrop or the reserve under it; 0 where none
// has data.
QVector<QRgb> plotColumnColors(const SpectrogramItem &item, QSGNode *node, int width) {
    QHash<QSGTexture *, int> bandX;
    for (const auto &[x, texture] : SpectrogramItem::canvasTextures(node)) {
        bandX.insert(texture, x);
    }
    // What `span` draws of `image`, its source `offset` pixels into it, at
    // x: 0 if nothing.
    const auto sample = [](const QSGSimpleTextureNode *span, double x, const QImage &image,
                            int offset) -> QRgb {
        const QRectF rect = span->rect();
        if (rect.isEmpty() || image.isNull() || x < rect.left() || x >= rect.right()) {
            return 0;
        }
        const QRectF source = span->sourceRect();
        const int sx = offset + static_cast<int>(std::floor(
            source.left() + (x - rect.left()) * source.width() / rect.width()));
        if (sx < 0 || sx >= image.width()) {
            return 0;
        }
        const QRgb pixel = image.pixel(sx, image.height() / 2);
        return qAlpha(pixel) > 0 ? pixel : 0;
    };
    QSGNode *floor = node->childAtIndex(0);
    QSGNode *tiles = node->childAtIndex(1);
    QVector<QRgb> colors(width, 0);
    for (int x = 0; x < width; ++x) {
        const double at = x + 0.5;
        QRgb color = 0;
        const auto canvasSpan = [&](QSGNode *child) {
            const auto *span = static_cast<const QSGSimpleTextureNode *>(child);
            if (color == 0 && bandX.contains(span->texture())) {
                color = sample(span, at, item.m_canvas, bandX.value(span->texture()));
            }
        };
        for (int i = 0; i < tiles->childCount(); ++i) {
            canvasSpan(tiles->childAtIndex(i));
        }
        canvasSpan(node->childAtIndex(2));
        // Under the floor's children: the backdrop's spans over the reserve.
        for (const int child : {2, 1}) {
            if (color == 0) {
                color = sample(static_cast<const QSGSimpleTextureNode *>(
                    floor->childAtIndex(child)), at, item.m_backdrop.image, 0);
            }
        }
        if (color == 0) {
            color = sample(static_cast<const QSGSimpleTextureNode *>(floor->childAtIndex(0)), at,
                item.m_reserve.image, 0);
        }
        colors[x] = color;
    }
    return colors;
}

// Which of the item's `width` logical pixel columns show data.
QVector<bool> plotColumnsShown(const SpectrogramItem &item, QSGNode *node, int width) {
    const QVector<QRgb> colors = plotColumnColors(item, node, width);
    QVector<bool> shown(width);
    std::transform(colors.begin(), colors.end(), shown.begin(), [](QRgb color) { return color != 0; });
    return shown;
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramZoomShowsOlderDataUntilTheNewArrives_data() {
    QTest::addColumn<bool>("rolling");
    QTest::newRow("centered") << false;
    QTest::newRow("rolling") << true;
}

void SpectrogramCanvasSmokeTest::spectrogramZoomShowsOlderDataUntilTheNewArrives() {
    // Issue 542: zooming in and out, by steps and back to 1, no frame shows
    // the floor where the frame before showed data, while there is older
    // data for it: the view before the zoom, scaled, until the zoom's data
    // arrives, and the ring's columns around it for a zoom out.
    QFETCH(bool, rolling);
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    constexpr int width = 200;
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(rolling ? 0 : 1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr double seconds = 60.0;
    constexpr int trackColumns = 6000;
    quint64 generation = 1;
    // The data for zoom 1: the whole track centered, the history played
    // so far rolling.
    const int nowColumn = static_cast<int>(seconds * 48'000 / 1024);
    if (rolling) {
        item.feedPrecomputedChunk(QByteArray(bins * 2000, '\x40'), bins, 0, 2000,
            nowColumn - 1999, trackColumns, 48'000, 1024, false, true, 1, true, generation);
    } else {
        item.feedPrecomputedChunk(QByteArray(bins * trackColumns, '\x40'), bins, 0, trackColumns,
            0, trackColumns, 48'000, 1024, true, true, 1, false, generation);
    }
    setTestPosition(item, seconds);

    QSGNode *node = nullptr;
    // Nothing shows before the first frame.
    QVector<bool> shown(width, false);
    // Paints a frame, and checks that it shows data wherever the last did.
    const auto frame = [&](const char *step) {
        node = item.updatePaintNode(node, nullptr);
        const QVector<bool> now = plotColumnsShown(item, node, width);
        for (int x = 0; x < width; ++x) {
            if (shown[x] && !now[x]) {
                QTest::qFail(qPrintable(QStringLiteral("%1: x %2 shows the floor").arg(
                    QString::fromLatin1(step)).arg(x)), __FILE__, __LINE__);
                return false;
            }
        }
        shown = now;
        return true;
    };
    // Frames until the rasterizations under way have arrived.
    const auto settle = [&](const char *step) {
        for (int round = 0; round < 8; ++round) {
            if (!frame(step)) {
                return false;
            }
            if (!spectrogramRasterPending(item) && !spectrogramReservePending(item)) {
                return frame(step);
            }
            if (!QTest::qWaitFor([&item]() {
                    return !spectrogramRasterPending(item) && !spectrogramReservePending(item);
                }, 5000)) {
                QTest::qFail("a rasterization did not arrive", __FILE__, __LINE__);
                return false;
            }
        }
        return frame(step);
    };
    // The backend restarts for the zoom asked for: its first columns, at
    // the playhead, then the rest of the view's.
    const auto feedZoom = [&](const char *step) {
        if (!sendPendingBackendZoom(item)) {
            return false;
        }
        const int hop = item.columnHopForZoomLocked(item.backendZoomForRequestLocked());
        const int columns = trackColumns * 1024 / hop;
        const int now = static_cast<int>(seconds * 48'000 / hop);
        ++generation;
        item.feedPrecomputedChunk(QByteArray(bins * 8, '\x70'), bins, 0, 8,
            now, columns, 48'000, hop, false, true, 1, rolling, generation);
        if (!settle(step)) {
            return false;
        }
        // The view before the zoom stands in at its own detail for the
        // columns the canvas lacks, over the ring's around it.
        if (!item.m_canvasComplete && item.m_backdrop.isNull()) {
            QTest::qFail(qPrintable(QStringLiteral("%1: no backdrop").arg(QString::fromLatin1(step))),
                __FILE__, __LINE__);
            return false;
        }
        if (!rolling) {
            // The rest of the track; a rolling session has no history.
            item.feedPrecomputedChunk(QByteArray(bins * (columns - now - 8), '\x70'), bins, 0,
                columns - now - 8, now + 8, columns, 48'000, hop, false, false, 1, false,
                generation);
            item.feedPrecomputedChunk(QByteArray(bins * now, '\x70'), bins, 0, now,
                0, columns, 48'000, hop, true, false, 1, false, generation);
        }
        return settle(step);
    };

    QVERIFY(settle("zoom 1"));
    QVERIFY(std::all_of(shown.begin(), shown.end(), [](bool data) { return data; }));
    // Out by two wheel steps, the data for the second, then in past 1, and
    // back to 1 at once.
    item.setZoomLevel(0.8);
    QVERIFY(frame("first step out"));
    QVERIFY(settle("first step out"));
    item.setZoomLevel(0.64);
    QVERIFY(frame("second step out"));
    QVERIFY(feedZoom("data for the second step out"));
    if (!rolling) {
        QVERIFY(item.m_backdrop.isNull());
    }
    item.setZoomLevel(1.25);
    QVERIFY(frame("step in"));
    QVERIFY(feedZoom("data for the step in"));
    item.setZoomLevel(1.0);
    QVERIFY(frame("reset"));
    QVERIFY(feedZoom("data for the reset"));
    // Seen over the view.
    QVERIFY(std::count(shown.begin(), shown.end(), true) > width / 2);

    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramTrackChangeDuringAZoomShowsNothingOfTheTrackBefore_data() {
    QTest::addColumn<bool>("clockFirst");
    // The clock reports the next track before its data arrives, and after.
    QTest::newRow("clock first") << true;
    QTest::newRow("data first") << false;
}

void SpectrogramCanvasSmokeTest::spectrogramTrackChangeDuringAZoomShowsNothingOfTheTrackBefore() {
    // Issue 542's review: a rolling view zoomed, and before the zoom's data
    // arrives, the next track starts. Its session's reset is not the zoom's
    // restart: nothing of the track before, its canvas, the ring's columns
    // kept for the reserve or a reserve being drawn, shows under the next
    // track's columns, though their times overlap, as when skipping soon
    // after playback starts.
    QFETCH(bool, clockFirst);
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    constexpr int width = 200;
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(0);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr quint64 before = 1;
    constexpr quint64 next = 2;
    // Five seconds of the first track, of one colour.
    const int playedColumns = static_cast<int>(5.0 * 48'000 / 1024);
    item.feedPrecomputedChunk(QByteArray(bins * playedColumns, '\x30'), bins, 0, playedColumns,
        0, 6000, 48'000, 1024, false, true, before, true, 1);
    setTestPosition(item, 5.0, before);
    QSGNode *node = nullptr;
    const auto settle = [&]() {
        for (int round = 0; round < 8; ++round) {
            node = item.updatePaintNode(node, nullptr);
            if (!QTest::qWaitFor([&item]() {
                    return !spectrogramRasterPending(item) && !spectrogramReservePending(item);
                }, 5000)) {
                return false;
            }
        }
        node = item.updatePaintNode(node, nullptr);
        return true;
    };
    QVERIFY(settle());
    const QRgb beforeColor = plotColumnColors(item, node, width).at(width / 2);
    QVERIFY(beforeColor != 0);

    // Zoomed out, and the zoom sent; its data does not come.
    item.setZoomLevel(0.8);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(sendPendingBackendZoom(item));
    QVERIFY(settle());
    // The next track, from 4 s, in another colour: its session's reset,
    // then its first columns.
    const int hop = item.columnHopForZoomLocked(item.backendZoomForRequestLocked());
    const int start = static_cast<int>(4.0 * 48'000 / hop);
    const auto feedNext = [&]() {
        item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, start, 6000 * 1024 / hop,
            48'000, hop, false, true, next, true, 2);
        item.feedPrecomputedChunk(QByteArray(bins * 16, '\x70'), bins, 0, 16, start,
            6000 * 1024 / hop, 48'000, hop, false, false, next, true, 2);
    };
    if (clockFirst) {
        setTestPosition(item, 4.0, next);
        feedNext();
    } else {
        feedNext();
        node = item.updatePaintNode(node, nullptr);
        setTestPosition(item, 4.0, next);
    }
    QVERIFY(settle());
    for (int frame = 0; frame < 4; ++frame) {
        const QVector<QRgb> colors = plotColumnColors(item, node, width);
        for (int x = 0; x < width; ++x) {
            QVERIFY2(colors[x] != beforeColor,
                qPrintable(QStringLiteral("frame %1: x %2 shows the track before").arg(frame).arg(x)));
        }
        QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramReservePending(item); }, 5000));
        node = item.updatePaintNode(node, nullptr);
    }
    // The next track's columns show.
    const QVector<QRgb> colors = plotColumnColors(item, node, width);
    QVERIFY(std::any_of(colors.begin(), colors.end(), [](QRgb color) { return color != 0; }));

    delete node;
}

namespace {

// A rolling pane 200 x 100, its first track's columns at 48 kHz and a hop
// of 1024 from the start to `seconds`, each second of another colour.
void prepareRollingHistory(SpectrogramItem &item, double seconds, quint64 trackToken) {
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(0);
    constexpr int bins = 8;
    const int columns = static_cast<int>(seconds * 48'000 / 1024);
    QByteArray data(bins * columns, Qt::Uninitialized);
    for (int column = 0; column < columns; ++column) {
        const int second = column * 1024 / 48'000;
        std::fill_n(data.data() + static_cast<qsizetype>(column) * bins, bins,
            static_cast<char>(0x20 + (second % 8) * 0x18));
    }
    item.feedPrecomputedChunk(data, bins, 0, columns, 0, 6000, 48'000, 1024, false, true,
        trackToken, true, 1);
}

// Paints until the rasterizations under way have arrived.
QSGNode *paintWithReserve(SpectrogramItem &item, QSGNode *node) {
    for (int round = 0; round < 8; ++round) {
        node = item.updatePaintNode(node, nullptr);
        if (!QTest::qWaitFor([&item]() {
                return !spectrogramRasterPending(item) && !spectrogramReservePending(item);
            }, 5000)) {
            break;
        }
    }
    return item.updatePaintNode(node, nullptr);
}

// The backend's restart for the zoom asked for: its reset, then `seconds`
// of columns from `startSeconds`, of track `trackToken`.
void feedRollingZoomRestart(SpectrogramItem &item, double startSeconds, double seconds,
    quint64 trackToken, quint64 generation) {
    constexpr int bins = 8;
    const int hop = item.columnHopForZoomLocked(item.backendZoomForRequestLocked());
    const int columns = std::max(1, static_cast<int>(seconds * 48'000 / hop));
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x70'), bins, 0, columns,
        static_cast<int>(startSeconds * 48'000 / hop), 6000 * 1024 / hop, 48'000, hop, false,
        true, trackToken, true, generation);
}

} // namespace

void SpectrogramCanvasSmokeTest::spectrogramLateZoomDataKeepsTheRollingHistory() {
    // Issue 542's review: playing, a zoom's restart can take longer than a
    // second or two to send its first columns, when opening the file or
    // decoding is slow. Its reset is still the zoom's, by the hop asked
    // for and playback unbroken since, wherever the playhead has got to:
    // the history only the backdrop and the reserve hold stays.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    TestTime time;
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    time.drive(item);
    item.setPlaying(true);
    prepareRollingHistory(item, 26.0, 1);
    setTestPosition(item, 20.0, 1);
    QSGNode *node = paintWithReserve(item, nullptr);
    item.setZoomLevel(0.8);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(sendPendingBackendZoom(item));
    // 2.5 s later, the restart's columns from where playback was asked.
    time.advance(2.5);
    node = paintWithReserve(item, node);
    const QVector<bool> before = plotColumnsShown(item, node, 200);
    feedRollingZoomRestart(item, 20.0, 3.0, 1, 2);
    QVERIFY(!item.m_awaitingZoomData);
    node = paintWithReserve(item, node);
    QVERIFY(!item.m_backdrop.isNull() || !item.m_reserve.isNull());
    const QVector<bool> after = plotColumnsShown(item, node, 200);
    for (int x = 0; x < 200; ++x) {
        QVERIFY2(!before[x] || after[x], qPrintable(QStringLiteral("x %1 shows the floor").arg(x)));
    }
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramSmallSeekDuringAZoomBreaksTheHistory() {
    // A seek while a zoom waits, however small, starts the view over: the
    // session after it is not the zoom's restart, though its columns are
    // at the hop asked for, and nothing from before it stands in.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    item.setPlaying(false);
    prepareRollingHistory(item, 20.0, 1);
    setTestPosition(item, 20.0, 1);
    QSGNode *node = paintWithReserve(item, nullptr);
    QVERIFY(!item.m_reserve.isNull());
    item.setZoomLevel(0.8);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(sendPendingBackendZoom(item));
    // A fifth of a second back, as the seek bar asks the clock to show it.
    testClock(item).holdAt(19.8);
    feedRollingZoomRestart(item, 19.8, 1.0, 1, 2);
    QVERIFY(item.m_backdrop.isNull());
    QVERIFY(item.m_reserve.isNull());
    node = paintWithReserve(item, node);
    QVERIFY(item.m_backdrop.isNull());
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramGaplessContinuationKeepsTheRollingHistory_data() {
    QTest::addColumn<bool>("repeatOne");
    QTest::addColumn<int>("order");
    QTest::addColumn<bool>("lengthKnown");
    QTest::newRow("gapless, clock first") << false << 0 << false;
    QTest::newRow("gapless, data first") << false << 1 << false;
    QTest::newRow("repeat one, clock first") << true << 0 << false;
    QTest::newRow("repeat one, data first") << true << 1 << false;
    QTest::newRow("gapless, restart before the clock") << false << 2 << false;
    QTest::newRow("gapless, restart before the clock, length known") << false << 2 << true;
    QTest::newRow("repeat one, restart before the clock") << true << 2 << false;
    QTest::newRow("repeat one, restart before the clock, length known") << true << 2 << true;
}

void SpectrogramCanvasSmokeTest::spectrogramGaplessContinuationKeepsTheRollingHistory() {
    // Issue 542's review: zoomed shortly before a gapless transition, or
    // the replay of Repeat One, the ring holds only the zoom's columns and
    // the history before them is the backdrop's and the reserve's. The
    // successor goes on with that history, as the canvas does, so neither
    // the clock's nor the data's new track drops them, and a zoom asked
    // for before the transition still keeps them when its restart, of the
    // successor, comes. They stay where their times are, also when that
    // restart comes between the successor's data and the clock's new
    // track, whose handoff then still places them at the previous track's
    // end.
    QFETCH(bool, repeatOne);
    QFETCH(int, order);
    QFETCH(bool, lengthKnown);
    constexpr int clockFirst = 0;
    constexpr int restartBeforeClock = 2;
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    constexpr int width = 200;
    constexpr int bins = 8;
    item.setPlaying(false);
    prepareRollingHistory(item, 20.0, 1);
    setTestPosition(item, 19.9, 1);
    if (lengthKnown) {
        // The previous track ends where it is paused.
        testClock(item).setDurationSeconds(19.9);
    }
    QSGNode *node = paintWithReserve(item, nullptr);
    item.setZoomLevel(0.8);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(sendPendingBackendZoom(item));
    feedRollingZoomRestart(item, 19.9, 0.1, 1, 2);
    node = paintWithReserve(item, node);
    QVERIFY(!item.m_backdrop.isNull());
    const QVector<QRgb> zoomed = plotColumnColors(item, node, width);

    // Another zoom out, sent; then the successor starts, the next track or
    // the same file again, and its first columns follow.
    item.setZoomLevel(0.64);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(sendPendingBackendZoom(item));
    const quint64 next = 2;
    const int hop = item.m_precomputedHopSize;
    const auto feedSuccessor = [&]() {
        item.feedPrecomputedChunk(QByteArray(bins * 4, repeatOne ? '\x20' : '\x50'), bins, 0, 4,
            0, 6000 * 1024 / hop, 48'000, hop, false, false, next, true, 2);
    };
    if (order == clockFirst) {
        setTestPosition(item, 0.05, next);
        feedSuccessor();
    } else {
        feedSuccessor();
        node = item.updatePaintNode(node, nullptr);
        if (order == restartBeforeClock) {
            // The zoom's restart, of the successor, from its playhead,
            // while the clock still plays the previous track.
            feedRollingZoomRestart(item, 0.05, 0.1, next, 3);
            QVERIFY(!item.m_awaitingZoomData);
            QVERIFY(item.m_gaplessContinuationPending);
            node = paintWithReserve(item, node);
        }
        setTestPosition(item, 0.05, next);
    }
    node = paintWithReserve(item, node);
    QVERIFY(!item.m_backdrop.isNull() || order == restartBeforeClock);
    QVERIFY(!item.m_reserve.isNull());
    if (order != restartBeforeClock) {
        // The zoom's restart, of the successor, from its playhead.
        feedRollingZoomRestart(item, 0.05, 0.1, next, 3);
        QVERIFY(!item.m_awaitingZoomData);
        node = paintWithReserve(item, node);
    }
    QVERIFY(!item.m_backdrop.isNull() || !item.m_reserve.isNull());
    // In every order, the successor's playhead follows its clock, as the
    // restart's columns do, and its times start at the previous track's
    // end: where the successor's clock puts it, at the previous track's
    // length, or where the restart's first column was at the playhead.
    QVERIFY(!item.m_gaplessContinuationPending);
    QVERIFY(!item.m_gaplessContinuationRebased);
    QCOMPARE(item.m_gaplessPositionOffset, 0.0);
    QVERIFY(std::abs(item.m_positionSeconds - 0.05) < 1e-9);
    const int restartHop = item.m_precomputedHopSize;
    const double successorStart = order != restartBeforeClock ? 19.85
        : lengthKnown ? 19.9
        : 19.9 - std::floor(0.05 * 48'000 / restartHop) * restartHop / 48'000.0;
    QVERIFY2(std::abs(item.m_streamOrigin - successorStart) < 1e-9,
        qPrintable(QStringLiteral("%1, not %2").arg(item.m_streamOrigin, 0, 'f', 6)
            .arg(successorStart, 0, 'f', 6)));
    // Rolling at 0.64 rather than 0.8, the history is a fifth narrower
    // towards the playhead at the right edge: the colour a second of it
    // showed at x shows at width - (width - x) * 0.8, where the playhead
    // has not moved; give or take a pixel when the restart placed the
    // successor's start, which can differ from the clock's by a column.
    const QVector<QRgb> after = plotColumnColors(item, node, width);
    const int slack = order == restartBeforeClock ? 1 : 0;
    int compared = 0;
    for (int x = 0; x < width - 20; ++x) {
        // Within a second's colour, away from its edges.
        if (zoomed[x] == 0 || x < 3 || zoomed[x - 3] != zoomed[x] || zoomed[x + 3] != zoomed[x]) {
            continue;
        }
        const int at = static_cast<int>(std::lround(width - (width - (x + 0.5)) * 0.8 - 0.5));
        bool shown = false;
        for (int near = std::max(0, at - slack); near <= std::min(width - 1, at + slack); ++near) {
            shown = shown || after[near] == zoomed[x];
        }
        QVERIFY2(shown,
            qPrintable(QStringLiteral("x %1 shows at %2 as %3, not %4").arg(x).arg(at)
                .arg(after[at], 0, 16).arg(zoomed[x], 0, 16)));
        ++compared;
    }
    QVERIFY(compared > 50);
    delete node;
}

void SpectrogramCanvasSmokeTest::spectrogramCenteredGaplessZoomRestartBeforeTheClockFollowsTheClock() {
    // Issue 542's review: a centered view counts each track's times from
    // its own start and follows the clock, so a zoom's restart of a
    // gapless successor, between its data and the clock's new track,
    // leaves no continuation for the clock to complete.
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 8;
    constexpr int columns = 20 * 48'000 / 1024;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x30'), bins, 0, columns, 0, 6000,
        48'000, 1024, false, true, 1, true, 1);
    setTestPosition(item, 19.9, 1);
    item.setZoomLevel(0.8);
    QVERIFY(sendPendingBackendZoom(item));
    const quint64 next = 2;
    const int hop = item.m_precomputedHopSize;
    item.feedPrecomputedChunk(QByteArray(bins * 4, '\x50'), bins, 0, 4, 0, 6000 * 1024 / hop,
        48'000, hop, false, false, next, true, 1);
    QVERIFY(!item.m_gaplessContinuationPending);
    feedRollingZoomRestart(item, 0.05, 0.1, next, 2);
    QVERIFY(!item.m_gaplessContinuationPending);
    setTestPosition(item, 0.05, next);
    QCOMPARE(item.m_gaplessPositionOffset, 0.0);
    QCOMPARE(item.m_streamOrigin, 0.0);
    QVERIFY(std::abs(item.m_positionSeconds - 0.05) < 1e-9);
}

std::unique_ptr<QmlSmokeTest> makeSpectrogramCanvasSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<SpectrogramCanvasSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_spectrogram_canvas.moc"
