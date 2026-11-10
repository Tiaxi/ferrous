// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The waveform editor's scene graph, its playback tiles and frames, and
// fast scrolling.
class WaveformSceneGraphSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void waveformEditorFrameReadsTheClockOnce();
    void waveformEditorFullscreenSampleCurveRequestsRawFramesEarly();
    void waveformEditorPausedZoomRebuildsCacheAtSamplePresentationBoundaries();
    void waveformEditorBuildsPlaybackTilesWithinFrameBudget();
    void waveformEditorKeepsPlaybackTilesAcrossDetailHandoffs();
    void waveformEditorLateDetailPreservesPartialPlaybackTiles();
    void waveformEditorPausedDetailReplacesOverviewCache();
    void waveformEditorSampleViewDoesNotFillLaterChannels();
    void waveformEditorUsesSceneGraphAndNativeFrameInterpolation();
    void waveformSceneGraphReusesTextures();
    void waveformEditorDestroyedInItsWindowRunsNoHandlersOfItsOwn();
    void waveformEditorHiddenDrawsNothing();
    void waveformEditorMadeInAWindowSchedulesPlaybackFrames();
    void waveformEditorParentedByQmlBindsItsWindowOnce();
    void waveformEditorDrawsMarkersByLogicalSpacing();
    void waveformEditorMakesPlaybackTilesOnWorkers();
    void waveformSceneGraphKeepsGeometryNodesWithinShortIndices();
    void waveformSceneGraphDrawsCloseSamplesWithFewerVertices_data();
    void waveformSceneGraphDrawsCloseSamplesWithFewerVertices();
    void waveformSceneGraphCloseSamplesLookAsBefore_data();
    void waveformSceneGraphCloseSamplesLookAsBefore();
    void waveformSceneGraphStraightSegmentsKeepTheirWidth_data();
    void waveformSceneGraphStraightSegmentsKeepTheirWidth();
    void waveformSceneGraphStrokeWeightHoldsAcrossTheThreshold_data();
    void waveformSceneGraphStrokeWeightHoldsAcrossTheThreshold();
    void waveformSceneGraphDrawsEveryLaneAtDeepZoom();
    void waveformSceneGraphScrollsSampleChunks_data();
    void waveformSceneGraphScrollsSampleChunks();
    void waveformSceneGraphSampleChunkBoundariesDoNotShow_data();
    void waveformSceneGraphSampleChunkBoundariesDoNotShow();
    void waveformSceneGraphRebuildsSampleChunksOnChange_data();
    void waveformSceneGraphRebuildsSampleChunksOnChange();
    void waveformSceneGraphKeepsSampleChunksAcrossDetailWindows();
    void waveformSceneGraphSplitsLargeSampleChunks();
    void waveformSceneGraphClipsSampleChunksToFastViews_data();
    void waveformSceneGraphClipsSampleChunksToFastViews();
    void waveformSceneGraphSampleChunkClipKeepsItsSide();
    void waveformEditorDetailStaysAheadOfSamplePlayback_data();
    void waveformEditorDetailStaysAheadOfSamplePlayback();
    void waveformEditorDeferredZoomOutLandsWhilePlaying();
    void waveformEditorZoomOutWhilePlayingHoldsTheTiledView();
    void waveformEditorLateDetailDrawsNoRasterEachFrame_data();
    void waveformEditorLateDetailDrawsNoRasterEachFrame();
    void waveformEditorPauseRedrawsTheRasterPlaybackLeft_data();
    void waveformEditorPauseRedrawsTheRasterPlaybackLeft();
    void waveformSceneGraphMatchesRaster_data();
    void waveformSceneGraphMatchesRaster();
    void waveformSceneGraphBenchmark();
    void waveformEditorPlaybackBenchmark();
    void waveformEditorDeepZoomPlaybackBenchmark();
    void waveformSceneGraphLaysOutEachLabelOnce();
    void waveformEditorIdleViewWaitsForThePlayheadsNextPixel();
    void waveformEditorNoOneSeesAsksForNoPlaybackFrames();
    void waveformEditorOffScreenDecodesNoDetail();
    void waveformEditorMovedBetweenWindowsFollowsTheirPresence();
    void waveformEditorReadsItsFileOnlyWhileShown();
    void waveformEditorLateFrameCallbackDrawsAtOnce();
    void waveformEditorCapsOnlyFastScrolling();
    void waveformEditorCapsAtAWholeNumberOfRefreshes_data();
    void waveformEditorCapsAtAWholeNumberOfRefreshes();
    void waveformEditorFastScrollRedrawsChangesAtOnce();
    void waveformEditorFastScrollShowsArrivalsWithItsNextFrame();
    void waveformEditorFastScrollFollowsTheScreensRefreshRate();
    void waveformEditorIdleViewDrawsOnlyFramesThatDiffer();
    void waveformEditorFollowingViewDrawsOnlyAtPixelSteps_data();
    void waveformEditorFollowingViewDrawsOnlyAtPixelSteps();
    void waveformEditorFollowingViewShowsJumpsAndPausesAtOnce();
    void waveformEditorInteractionDrawsAtOnceWhileFollowing();
};

void WaveformSceneGraphSmokeTest::waveformEditorFrameReadsTheClockOnce() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setHeight(120);
    item.setDurationSeconds(60.0);
    // 50 ms in view: a millisecond of playback is 6.4 pixels.
    item.setZoomLevel(1'200.0);
    item.setPlaying(true);
    // Each reading of the clock is a millisecond after the last.
    qint64 now = 1'000'000'000;
    int readings = 0;
    item.m_nowNs = [&now, &readings]() {
        ++readings;
        now += 1'000'000;
        return now;
    };
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_clockState.anchorSeconds = 20.0;
        item.m_clockState.anchorNs = now;
        item.m_clockState.rate = 1.0;
        item.m_clockState.durationSeconds = 60.0;
    }
    for (int frame = 0; frame < 3; ++frame) {
        QImage canvas(320, 120, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        readings = 0;
        QPainter painter(&canvas);
        item.paint(&painter);
        painter.end();
        QCOMPARE(readings, 1);
        // The view follows the playhead, which stays in the middle of it,
        // on column 160: a dash of it starts at the top.
        QVector<int> playhead;
        for (int x = 0; x < canvas.width(); ++x) {
            if (QColor(canvas.pixel(x, 1)).red() > 60) playhead.push_back(x);
        }
        QCOMPARE(playhead, QVector<int>{160});
    }
}

void WaveformSceneGraphSmokeTest::waveformEditorFullscreenSampleCurveRequestsRawFramesEarly() {
    WaveformEditorItem item;
    item.setWidth(3'440);
    item.setDurationSeconds(257.133333);
    setTestPosition(item, 8.9);
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

void WaveformSceneGraphSmokeTest::waveformEditorPausedZoomRebuildsCacheAtSamplePresentationBoundaries() {
    WaveformEditorItem item;
    item.setWidth(400);
    item.setHeight(180);
    item.setDurationSeconds(1.0);
    setTestPosition(item, 0.5);
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

void WaveformSceneGraphSmokeTest::waveformEditorBuildsPlaybackTilesWithinFrameBudget() {
    WaveformEditorItem item;
    // Tiles made at once, as the test asks for them.
    item.m_playbackTilesInline = true;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    item.m_clockState.anchorSeconds = 5.0;
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
        visibleStart, visibleEnd, 320, 180);
    QVERIFY(!paints.empty());
    for (const auto &paint : paints) {
        QCOMPARE(paint.image.width(), 64);
        QCOMPARE(paint.image.height(), 180);
    }

    // A view that moved on by more than its width, as one does in a frame
    // at a deep zoom, makes all of its tiles in one paint, however many.
    const double shiftedStart = visibleStart + 6.0 * tileDuration;
    const double shiftedEnd = visibleEnd + 6.0 * tileDuration;
    const qint64 firstShifted = static_cast<qint64>(std::floor(shiftedStart / tileDuration + 1.0e-9));
    const qint64 lastShifted = static_cast<qint64>(std::floor((shiftedEnd - 1.0e-12) / tileDuration));
    for (qint64 tile = firstShifted; tile <= lastShifted; ++tile) {
        QVERIFY(!item.m_playbackTiles.contains(tile));
    }
    const int catchUpBudget = item.playbackTileRenderBudgetLocked(
        shiftedStart, shiftedEnd);
    QCOMPARE(catchUpBudget, static_cast<int>(lastShifted - firstShifted + 1));
    QCOMPARE(item.renderMissingPlaybackTilesLocked(
        shiftedStart, shiftedEnd, 180, catchUpBudget), catchUpBudget);
    QVERIFY(item.playbackTilesCoverLocked(shiftedStart, shiftedEnd));
}

void WaveformSceneGraphSmokeTest::waveformEditorKeepsPlaybackTilesAcrossDetailHandoffs() {
    WaveformEditorItem item;
    // Tiles made at once, as the test asks for them.
    item.m_playbackTilesInline = true;
    item.setWidth(320);
    item.setHeight(180);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
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
            current->second.strips.front().cacheKey(),
            originalTile.strips.front().cacheKey());
    }

    item.m_cacheDirty = false;
    item.m_cacheStartSeconds = 0.0;
    item.m_cacheEndSeconds = 1.0;
    item.m_detail = {};
    QVERIFY(item.playbackTilesEligibleLocked(visibleStart, visibleEnd));
    setTestPosition(item, 5.01);
    QVERIFY(!item.m_cacheDirty);
    QVERIFY(!item.m_playbackTiles.empty());
}

void WaveformSceneGraphSmokeTest::waveformEditorLateDetailPreservesPartialPlaybackTiles() {
    for (int viewMode : {0, 1}) {
        WaveformEditorItem item;
        item.m_playbackTilesInline = true;
        item.setWidth(3440);
        item.setHeight(1440);
        item.setDurationSeconds(225.413333);
        setTestPosition(item, 70.0);
        item.setViewMode(viewMode);
        item.m_sampleRateHz = 44100;
        item.m_channelCount = 2;
        item.setZoomLevel(std::pow(1.25, 23));
        const auto [start, end] = item.visibleRangeLocked();
        const auto [requestStart, requestEnd] = item.requestRangeLocked(start, end);
        item.m_detail.sampleRateHz = 44100;
        item.m_detail.channelCount = 2;
        item.m_detail.framesPerPoint = 1;
        item.m_detail.pointCount = static_cast<int>((requestEnd - requestStart) * 44100);
        item.m_detail.startSeconds = requestStart;
        item.m_detail.endSeconds = requestEnd;
        item.m_detail.extrema.assign(item.m_detail.pointCount * 4U, 0.25F);
        item.m_playing = true;
        item.preparePlaybackTilesLocked(start, end, 1440);
        while (!item.playbackTilesCoverLocked(start, end)) {
            QVERIFY(item.renderMissingPlaybackTilesLocked(start, end, 1440, 8) > 0);
        }
        const auto originalTiles = item.m_playbackTiles;
        item.m_cacheDirty = false;
        item.m_cacheStartSeconds = requestStart;
        item.m_cacheEndSeconds = requestEnd;
        // A late result lets playback overtake both the detail and raster edge.
        item.m_clockState.anchorSeconds += 0.1;
        const auto [nextStart, nextEnd] = item.visibleRangeLocked();
        QVERIFY(!item.detailCoversRangeLocked(nextStart, nextEnd));
        QVERIFY(!item.playbackTilesCoverLocked(nextStart, nextEnd));
        item.handleWindowFrameSwapped();
        QVERIFY(!item.m_cacheDirty);
        QCOMPARE(item.m_playbackTiles.size(), originalTiles.size());
        for (const auto &[index, tile] : originalTiles) {
            QCOMPARE(item.m_playbackTiles.at(index).strips.front().cacheKey(), tile.strips.front().cacheKey());
        }
        // The replacement shares the same sample grid and keeps resident tiles.
        item.m_detail.startSeconds += 0.1;
        item.m_detail.endSeconds += 0.1;
        item.preparePlaybackTilesLocked(nextStart, nextEnd, 1440);
        const int rendered = item.renderMissingPlaybackTilesLocked(nextStart, nextEnd, 1440, 8);
        QVERIFY(rendered > 0 && rendered <= 8);
        QVERIFY(item.playbackTilesCoverLocked(nextStart, nextEnd));
        // A distant seek has no reusable overlap and must refresh its fallback.
        testClock(item).holdAt(150.0);
        QVERIFY(item.m_cacheDirty);
        QVERIFY(item.m_playbackTiles.empty());
        // Track changes still discard all presentation resources and data.
        item.setSourcePath(QStringLiteral("/fixture/next.wav"));
        QVERIFY(item.m_playbackTiles.empty());
        QVERIFY(item.m_detail.extrema.empty());
    }
}

void WaveformSceneGraphSmokeTest::waveformEditorPausedDetailReplacesOverviewCache() {
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

void WaveformSceneGraphSmokeTest::waveformEditorSampleViewDoesNotFillLaterChannels() {
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

void WaveformSceneGraphSmokeTest::waveformSceneGraphReusesTextures() {
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(3440);
    item.setHeight(180);
    item.setGridEnabled(true);
    item.setDurationSeconds(60.0);
    setTestPosition(item, 30.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    const auto initial = WaveformSceneGraph::statistics(node);
    QVERIFY(initial.uploadedBytes >= 3440 * 180 * 4);
    for (int frame = 0; frame < 20; ++frame) {
        setTestPosition(item, 30.0 + frame * 0.01);
        node = item.updatePaintNode(node, nullptr);
        QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, quint64{0});
    }
    item.setGridEnabled(false);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(WaveformSceneGraph::statistics(node).residentBytes < initial.residentBytes);
    item.setHeight(1440);
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(WaveformSceneGraph::statistics(node).uploadedBytes >= 3440 * 1440 * 4);
    item.setOverviewData(QByteArray(8192, static_cast<char>(32)));
    node = item.updatePaintNode(node, nullptr);
    QVERIFY(WaveformSceneGraph::statistics(node).textureUploads > 0);
    const auto resident = WaveformSceneGraph::statistics(node).residentBytes;
    // Rebuilding the scene graph (e.g. another fullscreen window) recreates
    // render resources without retaining textures from the previous window.
    delete node;
    node = item.updatePaintNode(nullptr, nullptr);
    QCOMPARE(WaveformSceneGraph::statistics(node).residentBytes, resident);
    delete node;

    // Oversized source caches must be divided into bounded textures. A pan
    // inside their resident tiles must only update source coordinates.
    QImage wideCache(9000, 180, QImage::Format_RGB32);
    wideCache.fill(Qt::green);
    double sourceX = 2500;
    node = nullptr;
    auto pan = [&]() {
        node = WaveformSceneGraph::render(node, &window, QSize(3440, 180), [&](QPainter *painter) {
            painter->drawImage(QRectF(0, 0, 3440, 180), wideCache, QRectF(sourceX, 0, 3440, 180));
        });
    };
    pan();
    const bool native = window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
    QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, native ? quint64{2} : quint64{1});
    sourceX += 0.5;
    pan();
    QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, quint64{0});
    sourceX = 4500;
    pan();
    QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, native ? quint64{1} : quint64{0});
    if (native) QCOMPARE(WaveformSceneGraph::statistics(node).residentBytes, quint64{4096 * 180 * 4});
    delete node;
}

void WaveformSceneGraphSmokeTest::waveformEditorHiddenDrawsNothing() {
    // Qt asks a hidden item for its node too, whenever it is made dirty or
    // enters a window: the waveform that the spectrogram replaces, for one,
    // as it moves into the fullscreen viewer with it, where it drew a whole
    // screen of pixels into the viewer's first frame. Hidden, the editor
    // draws nothing and keeps the node it has; shown again, it asks for a
    // frame, which draws it as it is then.
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *item = new WaveformEditorItem;
    item->setParentItem(window.contentItem());
    item->setWidth(400);
    item->setHeight(100);
    item->setDurationSeconds(60.0);
    item->setOverviewComplete(true);
    item->setOverviewData(QByteArray(8192, static_cast<char>(128)));
    item->setVisible(false);
    QCOMPARE(item->updatePaintNode(nullptr, nullptr), nullptr);

    item->setVisible(true);
    QSGNode *node = item->updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    const auto guard = qScopeGuard([&node]() { delete node; });
    QVERIFY(WaveformSceneGraph::statistics(node).uploadedBytes >= 400 * 100 * 4);
    node = item->updatePaintNode(node, nullptr);
    QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, quint64{0});

    // Hidden, a change draws nothing.
    item->setVisible(false);
    item->setOverviewData(QByteArray(8192, static_cast<char>(32)));
    QSGNode *const kept = node;
    node = item->updatePaintNode(node, nullptr);
    QCOMPARE(node, kept);
    QCOMPARE(WaveformSceneGraph::statistics(node).textureUploads, quint64{0});

    // Shown, it asks for a frame, which draws the change.
    const quint64 requests = item->m_playbackFrameRequests;
    item->setVisible(true);
    QVERIFY(item->m_playbackFrameRequests > requests);
    node = item->updatePaintNode(node, nullptr);
    QVERIFY(WaveformSceneGraph::statistics(node).textureUploads > 0);
}

void WaveformSceneGraphSmokeTest::waveformEditorDestroyedInItsWindowRunsNoHandlersOfItsOwn() {
    // ~QQuickItem takes an item out of its window once the item's own
    // members are destroyed, and emits windowChanged. The item's handler
    // for it once ran then, on its destroyed window connections and state.
    QQuickWindow window;
    window.resize(240, 160);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    // Parented after construction, as QML does, so it binds the window.
    auto *item = new WaveformEditorItem;
    item->setParentItem(window.contentItem());
    item->setWidth(200);
    item->setHeight(100);
    item->setDurationSeconds(10.0);
    item->setOverviewComplete(true);
    item->setOverviewData(QByteArray(1024, static_cast<char>(128)));
    QVERIFY(item->m_frameSwappedConnection);
    QVERIFY(item->m_windowVisibilityConnection);

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

namespace {

// Counts the item connections to a window's presented frames and visibility.
class ListenerCountingWindow final : public QQuickWindow {
public:
    int frameSwappedListeners() const { return receivers(SIGNAL(frameSwapped())); }
    int visibleChangedListeners() const { return receivers(SIGNAL(visibleChanged(bool))); }
};

// A track that the editor, zoomed in, follows: 12.8 pixels a second.
void playFollowingThePlayhead(WaveformEditorItem &item) {
    item.setWidth(400);
    item.setHeight(100);
    item.setDurationSeconds(250.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    item.setZoomLevel(8.0);
    setTestPosition(item, 100.0);
    item.setPlaying(true);
    setTestPosition(item, 100.0);
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformEditorMadeInAWindowSchedulesPlaybackFrames() {
    // windowChanged fires inside QQuickItem's constructor, before the
    // editor's handler for it is connected.
    ListenerCountingWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    // The window's presence, which its views share, follows it already.
    WindowPresence::of(&window);
    const int frameListeners = window.frameSwappedListeners();
    const int visibilityListeners = window.visibleChangedListeners();
    auto *item = new WaveformEditorItem(window.contentItem());
    playFollowingThePlayhead(*item);
    const quint64 requested = item->m_playbackFrameRequests;
    QTRY_VERIFY_WITH_TIMEOUT(item->m_playbackFrameRequests >= requested + 5, 2000);
    QVERIFY(item->m_frameSwappedConnection);
    QVERIFY(item->m_windowVisibilityConnection);
    QCOMPARE(window.frameSwappedListeners(), frameListeners + 1);
    QCOMPARE(window.visibleChangedListeners(), visibilityListeners + 1);

    // Moved within the window, or told of the same window again, it keeps
    // one connection of each.
    QQuickItem sibling(window.contentItem());
    item->setParentItem(&sibling);
    emit item->windowChanged(&window);
    QCOMPARE(window.frameSwappedListeners(), frameListeners + 1);
    QCOMPARE(window.visibleChangedListeners(), visibilityListeners + 1);

    delete item;
    QCOMPARE(window.frameSwappedListeners(), frameListeners);
    QCOMPARE(window.visibleChangedListeners(), visibilityListeners);
}

void WaveformSceneGraphSmokeTest::waveformEditorParentedByQmlBindsItsWindowOnce() {
    ListenerCountingWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    // The window's presence, which its views share, follows it already.
    WindowPresence::of(&window);
    const int frameListeners = window.frameSwappedListeners();
    const int visibilityListeners = window.visibleChangedListeners();
    QQmlEngine engine;
    QString errors;
    std::unique_ptr<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(
        "import QtQuick\n"
        "import FerrousUi 1.0\n"
        "Item { WaveformEditorItem { objectName: \"editor\" } }\n"),
        QUrl::fromLocalFile(
            QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/WaveformEditorParent.qml")),
        &errors));
    QVERIFY2(root, qPrintable(errors));
    auto *item = root->findChild<WaveformEditorItem *>(QStringLiteral("editor"));
    QVERIFY(item != nullptr);
    QVERIFY(!item->m_frameSwappedConnection);
    qobject_cast<QQuickItem *>(root.get())->setParentItem(window.contentItem());
    QCOMPARE(window.frameSwappedListeners(), frameListeners + 1);
    QCOMPARE(window.visibleChangedListeners(), visibilityListeners + 1);
    playFollowingThePlayhead(*item);
    const quint64 requested = item->m_playbackFrameRequests;
    QTRY_VERIFY_WITH_TIMEOUT(item->m_playbackFrameRequests >= requested + 5, 2000);
    QCOMPARE(window.frameSwappedListeners(), frameListeners + 1);

    root.reset();
    QCOMPARE(window.frameSwappedListeners(), frameListeners);
    QCOMPARE(window.visibleChangedListeners(), visibilityListeners);
}

namespace {

// A detail window as the decoder sends one: `channels` channels at `rate`
// over [start, end], raw samples when `maxPoints` holds them all.
QByteArray sampleDetailWindow(int rate, int channels, double start, double end, int maxPoints) {
    // As src/analysis/waveform_window.rs takes frames of the times.
    const qint64 firstFrame = static_cast<qint64>(std::floor(start * rate));
    const qint64 frames = std::max<qint64>(1, static_cast<qint64>(std::floor(end * rate)) - firstFrame);
    quint32 framesPerPoint = 1;
    while (frames > static_cast<qint64>(framesPerPoint) * maxPoints) framesPerPoint *= 2;
    const auto points = static_cast<quint32>((frames + framesPerPoint - 1) / framesPerPoint);
    QByteArray bytes("WVF2", 4);
    appendLe<quint32>(bytes, static_cast<quint32>(rate));
    appendLe<quint16>(bytes, static_cast<quint16>(channels));
    appendLe<quint16>(bytes, 0);
    const double windowStart = static_cast<double>(firstFrame) / rate;
    const double windowEnd = static_cast<double>(firstFrame + qint64{points} * framesPerPoint) / rate;
    for (const double seconds : {windowStart, windowEnd}) {
        quint64 bits = 0;
        std::memcpy(&bits, &seconds, sizeof(bits));
        appendLe<quint64>(bytes, bits);
    }
    appendLe<quint32>(bytes, framesPerPoint);
    appendLe<quint32>(bytes, points);
    // Written in place: a window of six channels holds megabytes, which an
    // unoptimized build takes long to append a value at a time.
    const qsizetype header = bytes.size();
    const auto values = static_cast<qsizetype>(points) * (channels + 1) * 2;
    bytes.resize(header + values * 4);
    auto *out = reinterpret_cast<uchar *>(bytes.data() + header);
    const auto put = [&out](float value) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        qToLittleEndian(bits, out);
        out += 4;
    };
    for (quint32 point = 0; point < points; ++point) {
        const double frame = static_cast<double>(firstFrame + qint64{point} * framesPerPoint);
        for (int channel = 0; channel < channels; ++channel) {
            const auto value = static_cast<float>(0.8 * std::sin(frame * 0.05 + channel));
            put(value);
            put(value);
        }
    }
    for (quint32 point = 0; point < points; ++point) {
        put(-0.5F);
        put(0.5F);
    }
    return bytes;
}

// Whether a pixel shows the waveform: its color, or an edge of it.
bool isWaveformPixel(QRgb pixel) {
    return qGreen(pixel) >= 70 && qGreen(pixel) >= qRed(pixel) + 40;
}

// Gives `item` raw samples as giveSampleDetail does, of a signal with more
// in it than one sine: a slow wave with quicker, quieter ones on it, as
// music has, whose curve between samples differs from straight segments.
void giveRichSampleDetail(WaveformEditorItem &item, int rate, int channels, double start, double end,
    bool sameSource = false) {
    giveSampleDetail(item, rate, channels, start, end, sameSource);
    auto &detail = item.m_detail;
    const qint64 firstFrame = std::llround(start * rate);
    for (int point = 0; point < detail.pointCount; ++point) {
        const double frame = static_cast<double>(firstFrame + point);
        for (int channel = 0; channel < channels; ++channel) {
            // Off the lane's center line, which is drawn over the waveform.
            const auto value = static_cast<float>(0.5 + 0.25 * std::sin(frame * 0.05 + channel)
                + 0.12 * std::sin(frame * 0.31 + 2.0 * channel) + 0.05 * std::sin(frame * 1.3 + channel));
            const std::size_t index = (static_cast<std::size_t>(point) * channels + channel) * 2;
            detail.extrema[index] = value;
            detail.extrema[index + 1] = value;
        }
    }
}

// Gives `item` raw samples that alternate between -amplitude and
// amplitude: the steepest lines a waveform draws.
void giveZigzagSampleDetail(WaveformEditorItem &item, int rate, int channels, double start, double end,
    double amplitude) {
    giveSampleDetail(item, rate, channels, start, end);
    auto &detail = item.m_detail;
    const qint64 firstFrame = std::llround(start * rate);
    for (int point = 0; point < detail.pointCount; ++point) {
        const auto value = static_cast<float>((firstFrame + point) % 2 == 0 ? -amplitude : amplitude);
        for (int channel = 0; channel < channels; ++channel) {
            const std::size_t index = (static_cast<std::size_t>(point) * channels + channel) * 2;
            detail.extrema[index] = value;
            detail.extrema[index + 1] = value;
        }
    }
}

// Calls `visit` with each node under `node` that draws, in the order a
// renderer draws them, and the translation of its coordinates in the
// scene's: chunks of geometry are under transform nodes that translate.
template <typename Visit>
void visitDrawnNodes(const QSGNode *node, QPointF offset, Visit &&visit) {
    for (const QSGNode *child = node->firstChild(); child; child = child->nextSibling()) {
        if (child->type() == QSGNode::TransformNodeType) {
            const QMatrix4x4 matrix = static_cast<const QSGTransformNode *>(child)->matrix();
            visitDrawnNodes(child, offset + QPointF(matrix(0, 3), matrix(1, 3)), visit);
        } else if (child->type() == QSGNode::BasicNodeType) {
            visitDrawnNodes(child, offset, visit);
        } else {
            visit(child, offset);
        }
    }
}

// The scene's vertex-colored geometry and rectangles drawn as a GPU draws
// them without multisampling: a pixel takes a triangle's color, interpolated
// across it, where its center is inside, blended premultiplied over what is
// there. Offscreen, no renderer draws the geometry a native one would.
QImage rasterizeSceneGeometry(const QSGNode *scene, QSize size, double scale, QColor background) {
    QImage image((QSizeF(size) * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    const int width = image.width();
    const int height = image.height();
    const auto blend = [&image](int x, int y, double r, double g, double b, double a) {
        auto *pixel = reinterpret_cast<QRgb *>(image.scanLine(y)) + x;
        const double keep = 1.0 - a;
        *pixel = qRgba(static_cast<int>(std::lround(r + qRed(*pixel) * keep)),
            static_cast<int>(std::lround(g + qGreen(*pixel) * keep)),
            static_cast<int>(std::lround(b + qBlue(*pixel) * keep)),
            static_cast<int>(std::lround(a * 255.0 + qAlpha(*pixel) * keep)));
    };
    visitDrawnNodes(scene, QPointF(), [&](const QSGNode *child, QPointF offset) {
        if (const auto *rectangle = dynamic_cast<const QSGRectangleNode *>(child)) {
            const QRectF rect = rectangle->rect().translated(offset);
            const QColor color = rectangle->color();
            const double alpha = color.alphaF();
            for (int y = std::max(0, static_cast<int>(std::lround(rect.top() * scale)));
                 y < std::min(height, static_cast<int>(std::lround(rect.bottom() * scale))); ++y) {
                for (int x = std::max(0, static_cast<int>(std::lround(rect.left() * scale)));
                     x < std::min(width, static_cast<int>(std::lround(rect.right() * scale))); ++x) {
                    blend(x, y, color.red() * alpha, color.green() * alpha, color.blue() * alpha, alpha);
                }
            }
            return;
        }
        // Images, labels and the playhead, are left out.
        if (child->type() != QSGNode::GeometryNodeType || dynamic_cast<const QSGImageNode *>(child)) {
            return;
        }
        const QSGGeometry *geometry = static_cast<const QSGGeometryNode *>(child)->geometry();
        if (geometry == nullptr
            || geometry->sizeOfVertex() != static_cast<int>(sizeof(QSGGeometry::ColoredPoint2D))
            || geometry->drawingMode() != QSGGeometry::DrawTriangles) {
            return;
        }
        const auto *vertices = geometry->vertexDataAsColoredPoint2D();
        for (int first = 0; first + 2 < geometry->vertexCount(); first += 3) {
            const QSGGeometry::ColoredPoint2D *corner[3] = {
                &vertices[first], &vertices[first + 1], &vertices[first + 2]};
            double px[3];
            double py[3];
            for (int i = 0; i < 3; ++i) {
                px[i] = (corner[i]->x + offset.x()) * scale;
                py[i] = (corner[i]->y + offset.y()) * scale;
            }
            const double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
            if (area == 0.0) continue;
            const double sign = area > 0 ? 1.0 : -1.0;
            // Whether edge i (from corner i+1 to corner i+2) owns centers on
            // it: its top or left edges, as GPUs rasterize.
            bool owns[3];
            for (int i = 0; i < 3; ++i) {
                const double dx = (px[(i + 2) % 3] - px[(i + 1) % 3]) * sign;
                const double dy = (py[(i + 2) % 3] - py[(i + 1) % 3]) * sign;
                owns[i] = dy > 0 || (dy == 0 && dx < 0);
            }
            const int left = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))));
            const int right = std::min(width - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
            const int top = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))));
            const int bottom = std::min(height - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
            for (int y = top; y <= bottom; ++y) {
                const double cy = y + 0.5;
                for (int x = left; x <= right; ++x) {
                    const double cx = x + 0.5;
                    double weight[3];
                    bool inside = true;
                    for (int i = 0; i < 3 && inside; ++i) {
                        const int a = (i + 1) % 3;
                        const int b = (i + 2) % 3;
                        weight[i] = ((px[b] - px[a]) * (cy - py[a]) - (py[b] - py[a]) * (cx - px[a])) * sign;
                        inside = weight[i] > 0 || (weight[i] == 0 && owns[i]);
                    }
                    if (!inside) continue;
                    const double total = weight[0] + weight[1] + weight[2];
                    double rgba[4] = {0, 0, 0, 0};
                    for (int i = 0; i < 3; ++i) {
                        const double w = weight[i] / total;
                        rgba[0] += corner[i]->r * w;
                        rgba[1] += corner[i]->g * w;
                        rgba[2] += corner[i]->b * w;
                        rgba[3] += corner[i]->a * w;
                    }
                    blend(x, y, rgba[0], rgba[1], rgba[2], rgba[3] / 255.0);
                }
            }
        }
    });
    return image.convertToFormat(QImage::Format_RGB32);
}

// How far geometry drawn as a GPU draws it may differ from QPainter's
// raster reference, as the mean difference a channel and the most pixels
// in `share` that differ by more than 45. Antialiased edges differ a little.
// Where a zigzag's strokes turn back at a peak, they overlap for many rows:
// QPainter fills their union, while the GPU blends two antialiased edges,
// which leaves those pixels up to a sixth darker.
struct RasterTolerance {
    double mean;
    int share;
};

constexpr RasterTolerance kRasterTolerance{2.0, 40};

constexpr RasterTolerance kZigzagRasterTolerance{4.0, 8};

// The pixels of two images of a size that differ by more than `threshold`
// (the sum over red, green and blue), and their mean difference a channel.
std::pair<int, double> imageDifference(const QImage &a, const QImage &b, int threshold) {
    int different = 0;
    quint64 error = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *rowA = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *rowB = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int delta = std::abs(qRed(rowA[x]) - qRed(rowB[x]))
                + std::abs(qGreen(rowA[x]) - qGreen(rowB[x])) + std::abs(qBlue(rowA[x]) - qBlue(rowB[x]));
            error += static_cast<quint64>(delta);
            if (delta > threshold) ++different;
        }
    }
    return {different, static_cast<double>(error) / (3.0 * a.width() * a.height())};
}

// The green of each pixel of `image`, averaged over the (2 radius + 1)
// pixels square around it: the detail an eye resolves from further away.
std::vector<int> blurredGreen(const QImage &image, int radius) {
    const int width = image.width();
    const int height = image.height();
    std::vector<int> rows(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            int sum = 0;
            int count = 0;
            for (int i = std::max(0, x - radius); i <= std::min(width - 1, x + radius); ++i) {
                sum += qGreen(line[i]);
                ++count;
            }
            rows[static_cast<std::size_t>(y) * width + x] = sum / count;
        }
    }
    std::vector<int> result(rows.size());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sum = 0;
            int count = 0;
            for (int i = std::max(0, y - radius); i <= std::min(height - 1, y + radius); ++i) {
                sum += rows[static_cast<std::size_t>(i) * width + x];
                ++count;
            }
            result[static_cast<std::size_t>(y) * width + x] = sum / count;
        }
    }
    return result;
}

// The visible samples a lane of `item` draws, those within the margin
// drawDetailSlice draws curves into, of 8 pixels or three samples and 3
// pixels: their positions in logical pixels, and their points of the
// detail.
QList<std::pair<double, int>> visibleSamples(WaveformEditorItem &item, int width) {
    QMutexLocker lock(&item.m_stateMutex);
    const auto [start, end] = item.visibleRangeLocked();
    const double secondsPerPixel = (end - start) / width;
    const double spacing = WaveformEditorItem::sampleSpacingPixels(item.m_detail, width, start, end);
    const double margin = std::max(8.0, std::ceil(3.0 * spacing + 3.0));
    QList<std::pair<double, int>> samples;
    for (int point = 0; point < item.m_detail.pointCount; ++point) {
        const double time = WaveformEditorItem::detailPointTime(item.m_detail, point);
        const double position = std::round((time - start) / secondsPerPixel * 256.0) / 256.0;
        if (position >= -margin && position < width + margin) samples.push_back({position, point});
    }
    return samples;
}

QList<double> visibleSamplePositions(WaveformEditorItem &item, int width) {
    QList<double> positions;
    for (const auto &[position, point] : visibleSamples(item, width)) {
        Q_UNUSED(point);
        positions.push_back(position);
    }
    return positions;
}

// The stroke's weight across each segment between two samples of a lane
// of `image`, in device pixels: the waveform's color in a row, as a share of
// a pixel, summed over the interval the segment crosses that row in, and
// averaged over the rows of the middle tenth of its height. The markers at
// its ends are far from there. Only segments at least 8 device pixels tall,
// within the image, count.
std::vector<double> segmentWeights(const QImage &image, const QList<QPointF> &points, double scale) {
    std::vector<double> weights;
    for (qsizetype i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i] * scale;
        const QPointF b = points[i + 1] * scale;
        const double spacing = b.x() - a.x();
        if (a.x() < 4 || b.x() > image.width() - 4 || std::abs(b.y() - a.y()) < 8) continue;
        const double top = std::min(a.y(), b.y());
        const double span = std::abs(b.y() - a.y());
        double total = 0;
        int rows = 0;
        for (auto y = static_cast<int>(std::floor(top + 0.45 * span));
             y <= static_cast<int>(std::floor(top + 0.55 * span)); ++y) {
            const double t = (y + 0.5 - a.y()) / (b.y() - a.y());
            const double middle = a.x() + t * spacing;
            const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            double ink = 0;
            for (auto x = std::max(0, static_cast<int>(std::floor(middle - spacing / 2)));
                 x <= std::min(image.width() - 1, static_cast<int>(std::floor(middle + spacing / 2))); ++x) {
                if (std::abs(x + 0.5 - middle) > spacing / 2) continue;
                ink += std::clamp((qGreen(row[x]) - 9) / 216.0, 0.0, 1.0);
            }
            total += ink;
            ++rows;
        }
        if (rows > 0) weights.push_back(total / rows);
    }
    return weights;
}

// The polyline of a lane of `item` drawn directly, in logical pixels, from
// the lowest value of each sample, as drawDetailSlice draws it.
QList<QPointF> lanePolyline(WaveformEditorItem &item, int width, int height, int lanes, int lane) {
    const double top = static_cast<double>(lane) * height / lanes;
    const double bottom = static_cast<double>(lane + 1) * height / lanes;
    const double center = (top + bottom) * 0.5;
    const double half = (bottom - top) * 0.5 - 1.0;
    QList<QPointF> points;
    for (const auto &[position, point] : visibleSamples(item, width)) {
        const std::size_t index = (static_cast<std::size_t>(point) * item.m_detail.channelCount + lane) * 2;
        points.push_back({position, center - static_cast<double>(item.m_detail.extrema[index]) * half});
    }
    return points;
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformEditorDrawsMarkersByLogicalSpacing() {
    // At 2x, samples 1.6 logical pixels apart are 3.2 device pixels apart.
    // Whether markers show, and with them whether samples are drawn
    // directly, follows the logical spacing: the playback tiles that draw
    // the view otherwise draw no markers, which would cost several times
    // as much to rasterize each frame.
    QQuickWindow window;
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) QSKIP("Needs the offscreen platform's HiDPI test screens");
    QVERIFY(moveWindowToScreen(window, screen));
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.m_playbackTilesInline = true;
    item.setWidth(400);
    item.setHeight(120);
    item.setDurationSeconds(60.0);
    giveSampleDetail(item, 48'000, 1, 29.0, 31.0);
    setTestPosition(item, 30.0);
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    const auto zoomForSpacing = [](double logicalPixels) {
        return logicalPixels * 60.0 * 48'000.0 / 400.0;
    };
    const auto markerPixels = [&item]() {
        // A tile of the view, and the same tile with no markers drawn: the
        // markers' squares add pixels to the curve's.
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        item.preparePlaybackTilesLocked(start, end, item.renderPixelHeightLocked());
        const double tileDuration = item.m_playbackTileSecondsPerPixel * 64.0;
        const double tileStart = std::floor(start / tileDuration + 1.0) * tileDuration;
        WaveformEditorItem::LaneStyle style;
        const QImage tile = WaveformEditorItem::renderPlaybackTile(item.m_detail, style,
            240, tileStart, tileStart + tileDuration, 1, 0, 64, 2.0).strips.front();
        // Spacing in device pixels below a marker's, at the same scale.
        const QImage bare = WaveformEditorItem::renderPlaybackTile(item.m_detail, style,
            240, tileStart, tileStart + tileDuration * 2.0, 1, 0, 64, 2.0).strips.front();
        const auto green = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) count += isWaveformPixel(image.pixel(x, y));
            }
            return count;
        };
        return std::pair{green(tile), green(bare)};
    };

    item.setZoomLevel(zoomForSpacing(1.6));
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(!item.samplePointsVisibleLocked());
        QVERIFY(!item.renderDetailDirectlyLocked(start, end));
        QVERIFY(item.playbackTilesEligibleLocked(start, end));
    }
    // Without markers, a tile of samples 3.2 device pixels apart has about
    // as many pixels as one of samples 1.6 apart.
    const auto [atSpacing, halfSpacing] = markerPixels();
    QVERIFY2(atSpacing < halfSpacing * 1.5,
        qPrintable(QStringLiteral("%1 pixels at 3.2, %2 at 1.6").arg(atSpacing).arg(halfSpacing)));

    item.setZoomLevel(zoomForSpacing(2.1));
    QMutexLocker lock(&item.m_stateMutex);
    const auto [start, end] = item.visibleRangeLocked();
    QVERIFY(item.samplePointsVisibleLocked());
    QVERIFY(item.renderDetailDirectlyLocked(start, end));
    QVERIFY(!item.playbackTilesEligibleLocked(start, end));
}

void WaveformSceneGraphSmokeTest::waveformEditorMakesPlaybackTilesOnWorkers() {
    // The fullscreen viewer at 2x, playing six lanes of 96 kHz samples 1.7
    // logical pixels apart, just after a seek: a paint asks the workers for
    // the view's tiles and makes none itself, and the frame shows the
    // detail's envelope in their place.
    QQuickWindow window;
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) QSKIP("Needs the offscreen platform's HiDPI test screens");
    QVERIFY(moveWindowToScreen(window, screen));
    TestTime time;
    auto item = std::make_unique<WaveformEditorItem>();
    item->setParentItem(window.contentItem());
    time.drive(*item);
    item->setWidth(3440);
    item->setHeight(1412);
    item->setDurationSeconds(60.0);
    item->setViewMode(1);
    item->setChannelCountHint(6);
    giveSampleDetail(*item, 96'000, 6, 29.0, 31.5);
    item->setZoomLevel(item->maximumZoomLevel() / std::pow(1.25, 7));
    setTestPosition(*item, 30.0);
    item->setPlaying(true);
    setTestPosition(*item, 30.0);
    QImage image(QSize(6880, 2824), QImage::Format_RGB32);
    image.setDevicePixelRatio(2.0);
    const auto draw = [&]() {
        QPainter painter(&image);
        item->paint(&painter);
    };
    draw();
    std::size_t asked = 0;
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto [start, end] = item->visibleRangeLocked();
        QVERIFY(item->playbackTilesEligibleLocked(start, end));
        asked = item->m_playbackTilesPending.size() + item->m_playbackTiles.size();
        // The view's 108 tiles and those playback reaches next.
        QVERIFY2(asked >= 108, qPrintable(QString::number(asked)));
    }
    QVERIFY(lanesWithEmptyColumns(image, 6, 3438, 3442).isEmpty());
    // They come from the workers.
    const auto tilesCoverTheView = [&item]() {
        QMutexLocker lock(&item->m_stateMutex);
        const auto range = item->visibleRangeLocked();
        return item->playbackTilesCoverLocked(range.first, range.second);
    };
    QTRY_VERIFY_WITH_TIMEOUT(tilesCoverTheView(), kSlowBuildTimeoutMs);
    {
        // In strips the scene graph takes as textures without copying them.
        QMutexLocker lock(&item->m_stateMutex);
        for (const auto &[index, tile] : item->m_playbackTiles) {
            Q_UNUSED(index);
            QCOMPARE(tile.strips.size(), 2U);
            QCOMPARE(tile.strips[0].height(), WaveformSceneGraph::kTextureSize);
            QCOMPARE(tile.strips[1].height(), 2824 - WaveformSceneGraph::kTextureSize);
        }
    }
    draw();
    QVERIFY(lanesWithEmptyColumns(image, 6, 3438, 3442).isEmpty());

    // After a seek, the tiles of the new view come the same way.
    giveSampleDetail(*item, 96'000, 6, 39.0, 41.5);
    time.advance(0.5);
    setTestPosition(*item, 40.0);
    draw();
    QVERIFY(lanesWithEmptyColumns(image, 6, 3438, 3442).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(tilesCoverTheView(), kSlowBuildTimeoutMs);

    // Muting a channel drops the tiles: those the workers are still making
    // in the old colors are not taken. The workers are held busy until then.
    QThreadPool *pool = WaveformEditorItem::playbackTileThreadPool();
    QSemaphore hold;
    for (int i = 0; i < pool->maxThreadCount(); ++i) {
        pool->start([&hold]() { hold.acquire(); });
    }
    time.advance(0.5);
    draw();
    quint64 oldGeneration = 0;
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY(!item->m_playbackTilesPending.empty());
        oldGeneration = item->m_playbackTileGeneration;
    }
    item->setMutedChannelsMask(1);
    hold.release(pool->maxThreadCount());
    QVERIFY(pool->waitForDone(kSlowBuildTimeoutMs));
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY(item->m_playbackTileGeneration != oldGeneration);
        QVERIFY(item->m_playbackTiles.empty());
        QVERIFY(item->m_playbackTilesPending.empty());
    }
    item->deliverPlaybackTile(oldGeneration, 0,
        WaveformEditorItem::PlaybackTile{{QImage(64, 2824, QImage::Format_RGB32)}});
    {
        QMutexLocker lock(&item->m_stateMutex);
        QVERIFY(item->m_playbackTiles.empty());
    }
    // Heard again, every lane shows in the waveform's color.
    item->setMutedChannelsMask(0);
    draw();
    QVERIFY(lanesWithEmptyColumns(image, 6, 3438, 3442).isEmpty());
    // An item gone while its tiles are being made takes none.
    item.reset();
    QVERIFY(WaveformEditorItem::playbackTileThreadPool()->waitForDone(kSlowBuildTimeoutMs));
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphKeepsGeometryNodesWithinShortIndices() {
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    // The fullscreen viewer at its deepest zoom: six lanes of sample curves
    // and markers, far more vertices than 16-bit indices reach.
    WaveformEditorItem item;
    item.setWidth(3440);
    item.setHeight(1412);
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(6);
    giveSampleDetail(item, 96'000, 6, 29.5, 30.5);
    item.setZoomLevel(item.maximumZoomLevel());
    setTestPosition(item, 30.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.renderDetailDirectlyLocked(start, end));
    }
    QSGNode *node = WaveformSceneGraph::render(nullptr, &window, QSize(3440, 1412),
        [&](QPainter *painter) { item.paint(painter); }, false,
        WaveformSceneGraph::Primitives::Geometry);
    QVERIFY(node != nullptr);
    const auto stats = WaveformSceneGraph::statistics(node);
    // Qt's renderer merges such nodes with 16-bit indices, which start over
    // past 65,535 vertices: the vertices after them are not drawn.
    QVERIFY2(stats.geometryVertices > 65'535, qPrintable(QString::number(stats.geometryVertices)));
    QVERIFY2(stats.largestGeometryVertices <= 65'534,
        qPrintable(QString::number(stats.largestGeometryVertices)));
    QVERIFY(stats.largestGeometryVertices > 0);
    QCOMPARE(stats.largestGeometryVertices % 3, 0);
    // A frame with fewer vertices keeps the nodes it needs and drops the rest.
    item.setZoomLevel(item.maximumZoomLevel() / 1.25);
    node = WaveformSceneGraph::render(node, &window, QSize(3440, 1412),
        [&](QPainter *painter) { item.paint(painter); }, false,
        WaveformSceneGraph::Primitives::Geometry);
    QVERIFY(WaveformSceneGraph::statistics(node).largestGeometryVertices <= 65'534);
    // Closer samples, joined by straight segments with a quad a marker, are
    // still more than one node holds, and split the same way: whole
    // triangles, within 65,532 vertices a node.
    for (const int stepsOut : {3, 4, 5, 6}) {
        item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
        QVERIFY(item.sampleSpacingPixelsLocked() < WaveformEditorItem::kStraightSamplePixelsPerSample);
        node = WaveformSceneGraph::render(node, &window, QSize(3440, 1412),
            [&](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        const auto close = WaveformSceneGraph::statistics(node);
        QVERIFY2(close.geometryVertices > 65'535, qPrintable(QString::number(close.geometryVertices)));
        QVERIFY2(close.largestGeometryVertices <= 65'532,
            qPrintable(QString::number(close.largestGeometryVertices)));
        QCOMPARE(close.largestGeometryVertices % 3, 0);
    }
    delete node;
}

namespace {

// The opaque, axis-aligned squares of the scene's geometry, in logical
// pixels: the markers of samples, or for antialiased ones their insides.
// The waveform's primitives are quads, (a, b, c, c, b, d), and triangles.
QList<QRectF> sceneSquares(const QSGNode *scene, QColor color) {
    // The vertices of consecutive geometry nodes, as one, in the scene's
    // coordinates: a geometry too large for a node is split between
    // triangles, which may be a quad's.
    std::vector<QSGGeometry::ColoredPoint2D> vertices;
    visitDrawnNodes(scene, QPointF(), [&vertices](const QSGNode *child, QPointF offset) {
        // The software renderer's image and rectangle nodes hold no real
        // geometry.
        if (child->type() != QSGNode::GeometryNodeType || dynamic_cast<const QSGImageNode *>(child)
            || dynamic_cast<const QSGRectangleNode *>(child)) {
            return;
        }
        const QSGGeometry *geometry = static_cast<const QSGGeometryNode *>(child)->geometry();
        if (geometry == nullptr
            || geometry->sizeOfVertex() != static_cast<int>(sizeof(QSGGeometry::ColoredPoint2D))) {
            return;
        }
        const auto *v = geometry->vertexDataAsColoredPoint2D();
        for (int i = 0; i < geometry->vertexCount(); ++i) {
            QSGGeometry::ColoredPoint2D point = v[i];
            point.x = static_cast<float>(point.x + offset.x());
            point.y = static_cast<float>(point.y + offset.y());
            vertices.push_back(point);
        }
    });
    QList<QRectF> squares;
    {
        // Quads start at any triangle: bevelled joins are three triangles.
        for (std::size_t first = 0; first + 5 < vertices.size(); first += 3) {
            const auto *q = vertices.data() + first;
            bool solid = q[2].x == q[3].x && q[2].y == q[3].y && q[1].x == q[4].x && q[1].y == q[4].y;
            for (int i = 0; i < 6 && solid; ++i) {
                solid = q[i].a == 255 && q[i].r == color.red() && q[i].g == color.green()
                    && q[i].b == color.blue();
            }
            const QRectF rect(QPointF(q[0].x, q[0].y), QPointF(q[5].x, q[5].y));
            if (!solid || q[0].y != q[1].y || q[0].x != q[2].x || q[1].x != q[5].x
                || q[2].y != q[5].y || rect.width() <= 0.5
                || std::abs(rect.width() - rect.height()) > 1e-3) {
                continue;
            }
            squares.push_back(rect);
            first += 3;
        }
    }
    return squares;
}

// The samples of the view, at `positions` in each of `lanes` lanes of a
// view of `size`, that have no marker, or more than one: a square centred
// within a device pixel of it.
QString samplesWithoutOneMarker(const QList<QRectF> &squares, const QList<double> &positions, QSize size,
    int lanes, double scale) {
    const double laneHeight = static_cast<double>(size.height()) / lanes;
    QString result;
    int wrong = 0;
    for (int lane = 0; lane < lanes; ++lane) {
        std::vector<double> centres;
        for (const QRectF &square : squares) {
            if (static_cast<int>(square.center().y() / laneHeight) == lane) centres.push_back(square.center().x());
        }
        std::sort(centres.begin(), centres.end());
        for (const double position : positions) {
            if (position < 0.0 || position >= size.width()) continue;
            const auto from = std::lower_bound(centres.begin(), centres.end(), position - 1.0 / scale);
            const auto to = std::upper_bound(centres.begin(), centres.end(), position + 1.0 / scale);
            const auto count = to - from;
            if (count != 1 && ++wrong <= 8) {
                result += QStringLiteral("lane %1: %2 markers at %3; ").arg(lane).arg(count).arg(position);
            }
        }
    }
    if (wrong > 8) result += QStringLiteral("%1 in all").arg(wrong);
    return result;
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformSceneGraphDrawsCloseSamplesWithFewerVertices_data() {
    QTest::addColumn<double>("spacing");
    QTest::addColumn<double>("scale");
    // Samples alternating at full scale: every bend is bevelled.
    QTest::addColumn<bool>("zigzag");
    for (const double scale : {1.0, 2.0}) {
        for (const double spacing : {2.0, 3.0, 4.199, 4.2, 4.201, 5.0, 8.0}) {
            QTest::addRow("%.3fpx-%.0fx", spacing, scale) << spacing << scale << false;
        }
        for (const double spacing : {3.0, 5.0}) {
            QTest::addRow("zigzag-%.3fpx-%.0fx", spacing, scale) << spacing << scale << true;
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphDrawsCloseSamplesWithFewerVertices() {
    // Samples closer than 4.2 logical pixels are joined by straight segments,
    // 18 vertices each, and their markers are a quad each, 6 vertices,
    // rather than flattened curves and antialiased squares of 30.
    QFETCH(double, spacing);
    QFETCH(double, scale);
    QFETCH(bool, zigzag);
    QQuickWindow window;
    if (scale != 1.0) {
        if (!showOnScreen(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    } else {
        window.resize(64, 64);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }
    constexpr int kWidth = 400;
    constexpr int kHeight = 200;
    constexpr int kLanes = 2;
    constexpr int kRate = 1'000;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    item.setWidth(kWidth);
    item.setHeight(kHeight);
    item.setDurationSeconds(10.0);
    item.setViewMode(1);
    item.setChannelCountHint(kLanes);
    if (zigzag) giveZigzagSampleDetail(item, kRate, kLanes, 4.0, 6.0, 1.0);
    else giveSampleDetail(item, kRate, kLanes, 4.0, 6.0);
    item.setZoomLevel(spacing * 10.0 * kRate / kWidth);
    setTestPosition(item, 5.0);
    QVERIFY(qFuzzyCompare(item.sampleSpacingPixelsLocked(), spacing));
    const auto samples = static_cast<int>(visibleSamplePositions(item, kWidth).size());
    QVERIFY(samples > 40);
    QSGNode *node = WaveformSceneGraph::render(nullptr, &window, QSize(kWidth, kHeight),
        [&item](QPainter *painter) {
            const auto [start, end] = item.visibleRangeLocked();
            item.drawDetailLocked(*painter, kWidth, kHeight, start, end, kLanes, 1.0,
                item.renderScaleLocked());
        }, false, WaveformSceneGraph::Primitives::Geometry);
    QVERIFY(node != nullptr);
    const int vertices = WaveformSceneGraph::statistics(node).geometryVertices;
    const QList<QRectF> squares = sceneSquares(node, QColor(54, 225, 161));
    delete node;
    // A square a sample in each lane: the marker, or an antialiased one's
    // inside.
    QCOMPARE(squares.size(), samples * kLanes);
    for (int lane = 0; lane < kLanes; ++lane) {
        const auto inLane = std::count_if(squares.begin(), squares.end(), [lane](const QRectF &square) {
            return static_cast<int>(square.center().y() / (kHeight / kLanes)) == lane;
        });
        QCOMPARE(static_cast<int>(inLane), samples);
    }
    const int segments = samples - 1;
    if (spacing < WaveformEditorItem::kStraightSamplePixelsPerSample) {
        // Each lane: a straight segment between each two samples, with a
        // bevel (a triangle and a quad) at each of a zigzag's sharp bends
        // and none at a sine's gentle ones, and a quad a marker, as large as
        // the pen and fill of an antialiased one in whole device pixels.
        const int bevels = zigzag ? segments - 1 : 0;
        QCOMPARE(vertices, kLanes * (18 * segments + 9 * bevels + 6 * samples));
        const double size = std::round((std::clamp(spacing * 0.75, 1.5, 3.0) + 1.2) * scale) / scale;
        for (const QRectF &square : squares) {
            QCOMPARE(square.width(), size);
            QCOMPARE(square.left() * scale, std::round(square.left() * scale));
            QCOMPARE(square.top() * scale, std::round(square.top() * scale));
        }
    } else {
        // As before: flattened curves, 18 vertices a part, and antialiased
        // markers of 30.
        const int curves = vertices - kLanes * 30 * samples;
        QVERIFY2(curves >= kLanes * 18 * segments, qPrintable(QString::number(vertices)));
        QCOMPARE(curves % 18, 0);
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphCloseSamplesLookAsBefore_data() {
    QTest::addColumn<int>("stepsOut");
    QTest::addColumn<double>("scale");
    // The deepest zoom, 8 logical pixels a sample, and steps out to 2.1:
    // 5.12 and 4.10 either side of the threshold.
    for (const double scale : {1.0, 2.0}) {
        for (const int stepsOut : {0, 2, 3, 4, 5, 6}) {
            QTest::addRow("%dout-%.0fx", stepsOut, scale) << stepsOut << scale;
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphCloseSamplesLookAsBefore() {
    // The scene's geometry drawn as a GPU would: the same picture as with
    // curves and antialiased markers from 4.2 logical pixels a sample, and
    // closer, the same seen from a normal distance, at which a logical
    // pixel's detail blurs: markers move by up to half a device pixel and
    // lose their soft edges. Every lane is drawn across the view, with a
    // marker at each sample. With FERROUS_WAVEFORM_GRAB_DIR set, the
    // pictures are saved there.
    QFETCH(int, stepsOut);
    QFETCH(double, scale);
    QQuickWindow window;
    if (scale != 1.0) {
        if (!showOnScreen(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    } else {
        window.resize(64, 64);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }
    // The main window's pane, with six lanes.
    const QSize size(1720, 600);
    constexpr int kLanes = 6;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(kLanes);
    giveRichSampleDetail(item, 96'000, kLanes, 29.5, 30.5);
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    setTestPosition(item, 30.0);
    const double spacing = item.sampleSpacingPixelsLocked();
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.renderDetailDirectlyLocked(start, end));
    }
    const auto draw = [&](double straightBelow, QList<QRectF> *squares) {
        item.m_straightSamplePixelsPerSample = straightBelow;
        QSGNode *node = WaveformSceneGraph::render(nullptr, &window, size,
            [&item](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        if (node == nullptr) return QImage();
        *squares = sceneSquares(node, QColor(54, 225, 161));
        QImage image = rasterizeSceneGeometry(node, size, scale, QColor(5, 9, 7));
        delete node;
        return image;
    };
    QList<QRectF> beforeSquares;
    QList<QRectF> afterSquares;
    const QImage before = draw(0.0, &beforeSquares);
    const QImage after = draw(WaveformEditorItem::kStraightSamplePixelsPerSample, &afterSquares);
    QVERIFY(!before.isNull() && !after.isNull());
    if (const QString dir = qEnvironmentVariable("FERROUS_WAVEFORM_GRAB_DIR"); !dir.isEmpty()) {
        const QString name = QStringLiteral("%1/%2x-%3out-%4px-").arg(dir).arg(scale)
            .arg(stepsOut).arg(spacing, 0, 'f', 2);
        before.save(name + QStringLiteral("before.png"));
        after.save(name + QStringLiteral("after.png"));
    }
    // But for the playhead's columns.
    const int playheadX = static_cast<int>(std::floor(size.width() * scale / 2.0));
    const int playheadMargin = static_cast<int>(std::ceil(scale)) + 1;
    const QString empty = lanesWithEmptyColumns(
        after, kLanes, playheadX - playheadMargin, playheadX + playheadMargin);
    QVERIFY2(empty.isEmpty(), qPrintable(empty));
    // A marker at each sample of every lane.
    QVERIFY2(afterSquares.size() == beforeSquares.size(), qPrintable(QStringLiteral("%1 squares, %2 before")
        .arg(afterSquares.size()).arg(beforeSquares.size())));
    const QString unmarked = samplesWithoutOneMarker(
        afterSquares, visibleSamplePositions(item, size.width()), size, kLanes, scale);
    QVERIFY2(unmarked.isEmpty(), qPrintable(unmarked));
    const auto [different, mean] = imageDifference(before, after, 45);
    const int pixels = after.width() * after.height();
    const auto radius = static_cast<int>(std::lround(scale));
    const std::vector<int> blurredBefore = blurredGreen(before, radius);
    const std::vector<int> blurredAfter = blurredGreen(after, radius);
    int blurredDifferent = 0;
    qint64 inkBefore = 0;
    qint64 inkAfter = 0;
    for (std::size_t i = 0; i < blurredAfter.size(); ++i) {
        blurredDifferent += std::abs(blurredAfter[i] - blurredBefore[i]) > 45;
        inkBefore += blurredBefore[i] - 9;
        inkAfter += blurredAfter[i] - 9;
    }
    const double ink = static_cast<double>(inkAfter) / static_cast<double>(inkBefore);
    qInfo("%.0fx %d steps out, %.2f px a sample: %d of %d pixels differ, mean %.3f; "
          "blurred, %d differ; ink %.3f",
        scale, stepsOut, spacing, different, pixels, mean, blurredDifferent, ink);
    if (spacing >= WaveformEditorItem::kStraightSamplePixelsPerSample) {
        QCOMPARE(different, 0);
        QCOMPARE(mean, 0.0);
    } else {
        QVERIFY2(different > 0, "closer samples are drawn with other primitives");
        // Blurred over a logical pixel, the pictures differ in few places,
        // and hold about as much of the waveform's color.
        QVERIFY2(mean < 1.5 && blurredDifferent < pixels / 200 && ink > 0.85 && ink < 1.15,
            qPrintable(QStringLiteral("mean %1, %2 differ blurred, ink %3")
                .arg(mean).arg(blurredDifferent).arg(ink)));
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphDrawsEveryLaneAtDeepZoom() {
    // CI runs this with a native renderer too, where the scene's geometry
    // is drawn by the GPU; offscreen, the software renderer draws it.
    QQuickWindow window;
    QSize size(3440, 1412);
    size.setWidth(std::min(size.width(), window.screen()->availableGeometry().width()));
    size.setHeight(std::min(size.height(), window.screen()->availableGeometry().height()));
    window.resize(size);
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(6);
    giveSampleDetail(item, 96'000, 6, 29.5, 30.5);
    // Five steps out from the deepest zoom, where samples are still drawn
    // directly, and the most of them are.
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, 5));
    setTestPosition(item, 30.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.renderDetailDirectlyLocked(start, end));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QImage grab = window.grabWindow();
    QVERIFY(!grab.isNull());
    const qreal scale = window.effectiveDevicePixelRatio();
    const int playheadX = static_cast<int>(std::floor(size.width() * scale / 2.0));
    const QString empty = lanesWithEmptyColumns(
        grab, 6, playheadX - static_cast<int>(std::ceil(scale)) - 1,
        playheadX + static_cast<int>(std::ceil(scale)) + 1);
    if (!empty.isEmpty()) grab.save(QDir::tempPath() + QStringLiteral("/waveform-deep-zoom-grab.png"));
    QVERIFY2(empty.isEmpty(), qPrintable(empty));
}

namespace {

// The layer of chunks of a waveform scene, or none.
const QSGNode *sampleChunkLayer(const QSGNode *scene) {
    for (const QSGNode *child = scene != nullptr ? scene->firstChild() : nullptr; child;
         child = child->nextSibling()) {
        if (child->type() == QSGNode::BasicNodeType) return child;
    }
    return nullptr;
}

// A chunk's node as a frame left it: what its geometry holds, and where.
struct SampleChunkNode {
    size_t hash{0};
    int vertices{0};
    double x{0.0};
};

// The nodes of a waveform scene's chunks that hold geometry.
QHash<const QSGNode *, SampleChunkNode> sampleChunkNodes(const QSGNode *scene) {
    QHash<const QSGNode *, SampleChunkNode> result;
    const QSGNode *layer = sampleChunkLayer(scene);
    for (const QSGNode *slot = layer != nullptr ? layer->firstChild() : nullptr; slot;
         slot = slot->nextSibling()) {
        SampleChunkNode state;
        state.x = static_cast<const QSGTransformNode *>(slot)->matrix()(0, 3);
        for (const QSGNode *child = slot->firstChild(); child; child = child->nextSibling()) {
            const QSGGeometry *geometry = static_cast<const QSGGeometryNode *>(child)->geometry();
            state.hash = qHashBits(geometry->vertexData(),
                static_cast<size_t>(geometry->vertexCount()) * static_cast<size_t>(geometry->sizeOfVertex()),
                state.hash);
            state.vertices += geometry->vertexCount();
        }
        if (state.vertices > 0) result.insert(slot, state);
    }
    return result;
}

// `item`'s samples, drawn as the scene graph draws a frame of them: in
// chunks, kept in `node` from frame to frame, or, with `chunked` false,
// the whole frame drawn again into a new scene.
QSGNode *renderSamples(WaveformEditorItem &item, QQuickWindow &window, QSGNode *node, bool chunked,
    bool clipToView = false) {
    const QSize size(static_cast<int>(item.width()), static_cast<int>(item.height()));
    return WaveformSceneGraph::render(node, &window, size, [&item, size, chunked, clipToView](QPainter *painter) {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        const int lanes = item.displayedChannelCountLocked();
        painter->fillRect(QRect(QPoint(), size), QColor(5, 9, 7));
        const auto chunks = chunked
            ? item.sampleChunksLocked(start, size.width(), size.height(), lanes, clipToView)
            : std::optional<WaveformSceneGraph::Chunks>();
        if (!chunks || !WaveformSceneGraph::drawChunks(*painter, *chunks)) {
            item.drawDetailLocked(*painter, size.width(), size.height(), start, end, lanes, 1.0,
                item.renderScaleLocked());
        }
    }, false, WaveformSceneGraph::Primitives::Geometry);
}

// A frame of `item`'s samples drawn in chunks, which `chunks` keeps from
// frame to frame, and the same frame drawn whole, as a GPU draws them: the
// pixels that differ, and by how much at most, the three channels' sum.
struct SampleFrames {
    QImage chunked;
    QImage whole;
    WaveformSceneGraph::Statistics stats;
    QList<QRectF> squares;
    QList<QRectF> wholeSquares;
    double scale{1.0};
    int different{0};
    int largest{0};
    // The most that pixels differ by where neither frame has a marker
    // within a device pixel.
    int largestStroke{0};
};

SampleFrames compareSampleFrames(WaveformEditorItem &item, QQuickWindow &window, QSGNode *&chunks,
    bool clipToView = false) {
    SampleFrames frames;
    const QSize size(static_cast<int>(item.width()), static_cast<int>(item.height()));
    const double scale = window.effectiveDevicePixelRatio();
    chunks = renderSamples(item, window, chunks, true, clipToView);
    QSGNode *whole = renderSamples(item, window, nullptr, false);
    if (chunks == nullptr || whole == nullptr) {
        delete whole;
        return frames;
    }
    frames.stats = WaveformSceneGraph::statistics(chunks);
    frames.scale = scale;
    frames.squares = sceneSquares(chunks, QColor(54, 225, 161));
    frames.wholeSquares = sceneSquares(whole, QColor(54, 225, 161));
    frames.chunked = rasterizeSceneGeometry(chunks, size, scale, QColor(5, 9, 7));
    frames.whole = rasterizeSceneGeometry(whole, size, scale, QColor(5, 9, 7));
    delete whole;
    // The device pixels of either frame's markers, and one around them.
    QImage markers(frames.chunked.size(), QImage::Format_Grayscale8);
    markers.fill(0);
    for (const QList<QRectF> *list : {&frames.squares, &frames.wholeSquares}) {
        for (const QRectF &square : *list) {
            const QRect pixels = QRectF(square.topLeft() * scale, square.size() * scale).toAlignedRect()
                .adjusted(-1, -1, 1, 1).intersected(markers.rect());
            for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
                std::fill_n(markers.scanLine(y) + pixels.left(), pixels.width(), uchar(1));
            }
        }
    }
    for (int y = 0; y < frames.chunked.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(frames.chunked.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(frames.whole.constScanLine(y));
        const uchar *marked = markers.constScanLine(y);
        for (int x = 0; x < frames.chunked.width(); ++x) {
            const int delta = std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x]))
                + std::abs(qBlue(a[x]) - qBlue(b[x]));
            frames.different += delta > 0;
            frames.largest = std::max(frames.largest, delta);
            if (marked[x] == 0) frames.largestStroke = std::max(frames.largestStroke, delta);
        }
    }
    return frames;
}

// Whether a frame drawn in chunks looks as one drawn whole. A chunk's
// vertices are in its own coordinates, rounded to floats there, so that
// antialiased edges may round a level apart in a channel; a gap, a seam or
// an edge drawn twice differs by tens of levels. At a scale that is not a
// whole number, a chunk places samples on the 1/256 pixel from its own
// start, which is a fraction of that from the view's: strokes may differ
// by a few levels, and a marker, snapped to device pixels, by a pixel.
QString sampleFramesDiffer(const SampleFrames &frames) {
    if (frames.chunked.isNull() || frames.whole.isNull()) return QStringLiteral("no frame");
    if (frames.scale == std::round(frames.scale)) {
        if (frames.largest <= 3 && frames.different * 100 <= frames.chunked.width() * frames.chunked.height()) {
            return {};
        }
        return QStringLiteral("%1 pixels differ, by up to %2").arg(frames.different).arg(frames.largest);
    }
    // The markers in the view, which both frames draw.
    const double width = frames.chunked.width() / frames.scale;
    std::vector<QPointF> chunked;
    std::vector<QPointF> whole;
    for (const QRectF &square : frames.squares) {
        if (square.right() > 0.0 && square.left() < width) chunked.push_back(square.center());
    }
    for (const QRectF &square : frames.wholeSquares) {
        if (square.right() > 0.0 && square.left() < width) whole.push_back(square.center());
    }
    if (chunked.size() != whole.size()) {
        return QStringLiteral("%1 markers, %2 drawn whole").arg(chunked.size()).arg(whole.size());
    }
    const auto byPosition = [](QPointF a, QPointF b) { return a.y() < b.y() || (a.y() == b.y() && a.x() < b.x()); };
    std::sort(chunked.begin(), chunked.end(), byPosition);
    std::sort(whole.begin(), whole.end(), byPosition);
    for (std::size_t i = 0; i < chunked.size(); ++i) {
        const QPointF moved = (chunked[i] - whole[i]) * frames.scale;
        if (std::abs(moved.x()) > 1.0 + 1e-3 || std::abs(moved.y()) > 1e-3) {
            return QStringLiteral("a marker at %1, %2 drawn whole at %3, %4").arg(chunked[i].x()).arg(chunked[i].y())
                .arg(whole[i].x()).arg(whole[i].y());
        }
    }
    if (frames.largestStroke <= 12) return {};
    return QStringLiteral("strokes differ by up to %1").arg(frames.largestStroke);
}

// A window shown at `scale`, or false without a screen of it.
bool showSampleWindow(QQuickWindow &window, double scale) {
    if (scale != 1.0) return showOnScreen(window, scale);
    window.resize(64, 64);
    window.show();
    return QTest::qWaitForWindowExposed(&window);
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformSceneGraphScrollsSampleChunks_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("stepsOut");
    // At the deepest zoom, curves; 5 steps out, straight segments.
    for (const double scale : {1.0, 2.0}) {
        for (const int stepsOut : {0, 5}) QTest::addRow("%dout-%.0fx", stepsOut, scale) << scale << stepsOut;
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphScrollsSampleChunks() {
    // Playing at 360 frames a second, six lanes of 48 kHz samples: each
    // frame builds the chunks that come into view, and writes no other
    // geometry; the others keep theirs, and move with the view by whole
    // device pixels, as their translation alone.
    QFETCH(double, scale);
    QFETCH(int, stepsOut);
    QQuickWindow window;
    if (!showSampleWindow(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    const QSize size(1720, 300);
    TestTime time;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    time.drive(item);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(6);
    giveSampleDetail(item, 48'000, 6, 29.5, 30.5);
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    setTestPosition(item, 30.0);
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    QSGNode *node = nullptr;
    QHash<const QSGNode *, SampleChunkNode> previous;
    qint64 previousFirst = 0;
    qint64 previousLast = -1;
    int kept = 0;
    for (int frame = 0; frame < 24; ++frame) {
        if (frame > 0) time.advance(1.0 / 360.0);
        qint64 first = 0;
        qint64 last = -1;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            QVERIFY(item.renderDetailDirectlyLocked(start, end));
            const auto chunks = item.sampleChunksLocked(start, size.width(), size.height(), 6);
            QVERIFY(chunks.has_value());
            first = chunks->first;
            last = chunks->last;
        }
        node = WaveformSceneGraph::render(node, &window, size,
            [&item](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        QVERIFY(node != nullptr);
        const auto stats = WaveformSceneGraph::statistics(node);
        qint64 entered = 0;
        for (qint64 index = first; index <= last; ++index) entered += index < previousFirst || index > previousLast;
        QCOMPARE(stats.chunks, static_cast<int>(last - first + 1));
        QCOMPARE(stats.chunksBuilt, static_cast<int>(entered));
        // The geometry written: the new chunks', and none other.
        const auto current = sampleChunkNodes(node);
        int rebuilt = 0;
        int rebuiltVertices = 0;
        QList<double> moves;
        for (auto it = current.cbegin(); it != current.cend(); ++it) {
            const auto before = previous.constFind(it.key());
            if (before == previous.cend() || before->hash != it->hash) {
                ++rebuilt;
                rebuiltVertices += it->vertices;
            } else {
                moves.push_back(it->x - before->x);
            }
        }
        QCOMPARE(rebuilt, static_cast<int>(entered));
        QCOMPARE(stats.uploadedVertices, rebuiltVertices);
        QCOMPARE(static_cast<int>(current.size()), stats.chunks);
        for (const double move : moves) {
            QCOMPARE(move, moves.front());
            QVERIFY(move < 0.0);
            QCOMPARE(move * scale, std::round(move * scale));
        }
        QVERIFY(stats.chunksMoved >= moves.size());
        kept += static_cast<int>(moves.size());
        previous = current;
        previousFirst = first;
        previousLast = last;
    }
    QVERIFY2(kept > 0, "no chunk was kept from a frame to the next");
    delete node;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphSampleChunkBoundariesDoNotShow_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("zigzag");
    QTest::addColumn<int>("stepsOut");
    QTest::addColumn<int>("place");
    // Curves at the deepest zoom and straight segments 4 steps out, with a
    // bevel at every zigzag's bend: at the track's start, in its middle,
    // and at its end, where the view stops a fraction of a column past the
    // grid.
    const char *const places[] = {"start", "middle", "end"};
    for (const double scale : {1.0, 1.5, 1.25, 2.0}) {
        for (const bool zigzag : {false, true}) {
            for (const int stepsOut : {0, 4}) {
                for (int place = 0; place < 3; ++place) {
                    QTest::addRow("%s-%dout-%s-%gx", zigzag ? "zigzag" : "smooth", stepsOut, places[place],
                        scale) << scale << zigzag << stepsOut << place;
                }
            }
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphSampleChunkBoundariesDoNotShow() {
    // Playing across chunks' boundaries, a frame drawn in chunks, as a GPU
    // draws it, looks as the frame drawn whole: no gaps, seams or doubled
    // edges where they meet, every lane drawn across the view, and one
    // marker for each sample. With FERROUS_WAVEFORM_GRAB_DIR set, the
    // frames are saved there, with the boundaries' device columns.
    QFETCH(double, scale);
    QFETCH(bool, zigzag);
    QFETCH(int, stepsOut);
    QFETCH(int, place);
    QQuickWindow window;
    if (!showSampleWindow(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    const QSize size(600, 240);
    constexpr int kLanes = 3;
    constexpr int kRate = 96'000;
    TestTime time;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    time.drive(item);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(kLanes);
    const double detailStart[] = {0.0, 29.9, 59.88};
    const double position[] = {0.0002, 30.0, 59.9985};
    if (zigzag) giveZigzagSampleDetail(item, kRate, kLanes, detailStart[place], detailStart[place] + 0.12, 0.8);
    else giveRichSampleDetail(item, kRate, kLanes, detailStart[place], detailStart[place] + 0.12);
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    setTestPosition(item, position[place]);
    item.setPlaying(true);
    setTestPosition(item, position[place]);
    QSGNode *chunks = nullptr;
    int boundaries = 0;
    const QString grabs = qEnvironmentVariable("FERROUS_WAVEFORM_GRAB_DIR");
    // Each frame moves the view on by hundreds of pixels, to other places
    // of the chunks' boundaries in it.
    for (int frame = 0; frame < 3; ++frame) {
        if (frame > 0) time.advance(1.0 / 360.0);
        double spacing = 0.0;
        QStringList columns;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            QVERIFY(item.renderDetailDirectlyLocked(start, end));
            const auto layout = item.sampleChunksLocked(start, size.width(), size.height(), kLanes);
            QVERIFY(layout.has_value());
            // The chunks' boundaries inside the view.
            for (qint64 index = layout->first + 1; index <= layout->last; ++index) {
                const double x = layout->offset(index);
                if (x > 0.0 && x < size.width()) {
                    ++boundaries;
                    columns << QString::number(std::lround(x * scale));
                }
            }
            spacing = item.sampleSpacingPixelsLocked();
        }
        const SampleFrames frames = compareSampleFrames(item, window, chunks);
        QVERIFY(frames.stats.chunks > 0);
        if (!grabs.isEmpty()) {
            const QString name = QStringLiteral("%1/%2-frame%3-").arg(grabs,
                QString::fromLatin1(QTest::currentDataTag())).arg(frame);
            frames.chunked.save(name + QStringLiteral("chunked.png"));
            frames.whole.save(name + QStringLiteral("whole.png"));
            QFile file(name + QStringLiteral("boundaries.txt"));
            if (file.open(QIODevice::WriteOnly)) file.write(columns.join(QLatin1Char(' ')).toUtf8());
        }
        const QString differ = sampleFramesDiffer(frames);
        if (!differ.isEmpty()) {
            frames.chunked.save(QDir::tempPath() + QStringLiteral("/waveform-chunked.png"));
            frames.whole.save(QDir::tempPath() + QStringLiteral("/waveform-whole.png"));
        }
        QVERIFY2(differ.isEmpty(), qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(differ)));
        // After the track's last sample, the view shows nothing.
        const int lastColumn = place == 2
            ? frames.chunked.width() - static_cast<int>(std::ceil(spacing * scale)) - 1
            : frames.chunked.width();
        const QString empty = lanesWithEmptyColumns(frames.chunked, kLanes, lastColumn, frames.chunked.width());
        QVERIFY2(empty.isEmpty(), qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(empty)));
        const QString unmarked = samplesWithoutOneMarker(
            frames.squares, visibleSamplePositions(item, size.width()), size, kLanes, scale);
        QVERIFY2(unmarked.isEmpty(), qPrintable(QStringLiteral("frame %1: %2").arg(frame).arg(unmarked)));
    }
    QVERIFY(boundaries > 0);
    delete chunks;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphRebuildsSampleChunksOnChange_data() {
    QTest::addColumn<QString>("change");
    for (const char *change : {"zoom", "scale", "width", "height", "channels", "view-mode", "mute", "solo",
             "track", "gapless", "repeat-one", "seek", "detail"}) {
        QTest::newRow(change) << QString::fromLatin1(change);
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphRebuildsSampleChunksOnChange() {
    // A still view keeps its chunks; whatever changes what they show
    // builds them again, and the frame then looks as one drawn whole. A
    // new detail window of the same file builds again only the chunks it
    // holds more of.
    QFETCH(QString, change);
    QQuickWindow window;
    QVERIFY(showSampleWindow(window, 1.0));
    constexpr int kRate = 96'000;
    TestTime time;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    time.drive(item);
    item.setWidth(1720);
    item.setHeight(240);
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(3);
    item.setSourcePath(QStringLiteral("/fixture/first.wav"));
    // At the deepest zoom, the view spans 1720 / 8 samples.
    const bool repeatOne = change == QLatin1String("repeat-one");
    const double position = repeatOne ? 59.999 : 30.0;
    const double samples = 1720.0 / 8.0 / kRate;
    if (change == QLatin1String("detail")) {
        // Just over the view: the chunks at its edges take shape from
        // samples it does not hold.
        giveRichSampleDetail(item, kRate, 3, position - 0.6 * samples, position + 0.6 * samples);
    } else {
        giveRichSampleDetail(item, kRate, 3, position - 0.05, std::min(60.0, position + 0.05));
    }
    item.setZoomLevel(item.maximumZoomLevel());
    setTestPosition(item, position);
    QSGNode *chunks = nullptr;
    SampleFrames frames = compareSampleFrames(item, window, chunks);
    QVERIFY(frames.stats.chunks > 0);
    QCOMPARE(frames.stats.chunksBuilt, frames.stats.chunks);
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));
    frames = compareSampleFrames(item, window, chunks);
    QCOMPARE(frames.stats.chunksBuilt, 0);
    QCOMPARE(frames.stats.chunksMoved, 0);
    QCOMPARE(frames.stats.uploadedVertices, 0);
    // The chunks a new window holds more of.
    int partial = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        const auto layout = item.sampleChunksLocked(start, 1720, 240, 3);
        for (qint64 index = layout->first; index <= layout->last; ++index) partial += std::get<0>(layout->stamp(index)) != 0;
    }
    if (change == QLatin1String("zoom")) {
        item.setZoomLevel(item.maximumZoomLevel() / 1.25);
    } else if (change == QLatin1String("scale")) {
        QScreen *screen = hiDpiTestScreen(2.0);
        if (screen == nullptr) QSKIP("Needs the offscreen platform's HiDPI test screens");
        QVERIFY(moveWindowToScreen(window, screen));
    } else if (change == QLatin1String("width")) {
        item.setWidth(1500);
        item.setZoomLevel(item.maximumZoomLevel());
    } else if (change == QLatin1String("height")) {
        item.setHeight(300);
    } else if (change == QLatin1String("channels")) {
        giveRichSampleDetail(item, kRate, 2, position - 0.05, position + 0.05);
    } else if (change == QLatin1String("view-mode")) {
        item.setViewMode(0);
    } else if (change == QLatin1String("mute")) {
        item.setMutedChannelsMask(0b010);
    } else if (change == QLatin1String("solo")) {
        item.setSoloedChannelsMask(0b001);
    } else if (change == QLatin1String("track")) {
        item.setSourcePath(QStringLiteral("/fixture/second.wav"));
        giveZigzagSampleDetail(item, kRate, 3, position - 0.05, position + 0.05, 0.6);
        item.setZoomLevel(item.maximumZoomLevel());
    } else if (change == QLatin1String("gapless")) {
        // The next track takes over where the clock says, with another
        // token, as at a gapless commit.
        item.setSourcePath(QStringLiteral("/fixture/second.wav"));
        setTestPosition(item, position, 7);
        giveZigzagSampleDetail(item, kRate, 3, position - 0.05, position + 0.05, 0.6);
        item.setZoomLevel(item.maximumZoomLevel());
    } else if (repeatOne) {
        // The same track again from its start: the same file.
        setTestPosition(item, 0.0001);
        giveRichSampleDetail(item, kRate, 3, 0.0, 0.1, true);
    } else if (change == QLatin1String("seek")) {
        // A jump the clock starts over at, within the window.
        setTestPosition(item, position + 0.04);
    } else if (change == QLatin1String("detail")) {
        giveRichSampleDetail(item, kRate, 3, position - 0.05, position + 0.05, true);
    }
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.renderDetailDirectlyLocked(start, end));
    }
    frames = compareSampleFrames(item, window, chunks);
    QVERIFY(frames.stats.chunks > 0);
    if (change == QLatin1String("detail")) {
        QVERIFY(partial > 0 && partial < frames.stats.chunks);
        QCOMPARE(frames.stats.chunksBuilt, partial);
    } else {
        QCOMPARE(frames.stats.chunksBuilt, frames.stats.chunks);
    }
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));
    delete chunks;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphKeepsSampleChunksAcrossDetailWindows() {
    // Through the decoder: a new window of the same file keeps the chunks
    // the old one held whole, and a window that arrives for a view the
    // item has left is not drawn; the chunks of the new view are built
    // from its own window.
    QQuickWindow window;
    QVERIFY(showSampleWindow(window, 1.0));
    constexpr int kRate = 96'000;
    constexpr int kLanes = 3;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(1720);
    item.setHeight(240);
    item.setDurationSeconds(120.0);
    item.setViewMode(1);
    item.setChannelCountHint(kLanes);
    auto gate = std::make_shared<QSemaphore>();
    auto held = std::make_shared<std::atomic_bool>(false);
    item.m_decodeWindow = [gate, held](const QString &, double start, double end, int points,
                                       const std::shared_ptr<std::atomic_bool> &) {
        if (held->load()) gate->tryAcquire(1, 5000);
        return sampleDetailWindow(kRate, kLanes, start, end, points);
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/chunks.wav"));
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), kRate, kSlowBuildTimeoutMs);
    item.setZoomLevel(item.maximumZoomLevel());
    const auto viewHasSamples = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && item.renderDetailDirectlyLocked(start, end);
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    QSGNode *chunks = nullptr;
    SampleFrames frames = compareSampleFrames(item, window, chunks);
    QVERIFY(frames.stats.chunks > 0);
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));

    // The next window of the file.
    const auto generation = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.m_detail.generation;
    };
    const quint64 first = generation();
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.invalidateDetailRequestLocked();
        item.clearPendingRequestLocked();
    }
    item.requestDetailWindow();
    QTRY_VERIFY_WITH_TIMEOUT(generation() != first && !item.m_decodeActive, kSlowBuildTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    frames = compareSampleFrames(item, window, chunks);
    QCOMPARE(frames.stats.chunksBuilt, 0);
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));

    // A window asked for here arrives after a seek elsewhere: stale.
    held->store(true);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.invalidateDetailRequestLocked();
        item.clearPendingRequestLocked();
    }
    item.requestDetailWindow();
    QVERIFY(item.m_decodeActive);
    const quint64 second = generation();
    setTestPosition(item, 90.0);
    const auto paint = [&]() {
        QSGNode *node = WaveformSceneGraph::render(chunks, &window, QSize(1720, 240),
            [&item](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        chunks = node;
        return WaveformSceneGraph::statistics(node);
    };
    // Until the view's own window comes, it shows no chunks.
    QCOMPARE(paint().chunks, 0);
    QCOMPARE(sampleChunkLayer(chunks), nullptr);
    held->store(false);
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    QVERIFY(generation() != second);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_detail.startSeconds > 80.0);
    }
    frames = compareSampleFrames(item, window, chunks);
    QVERIFY(frames.stats.chunks > 0);
    QCOMPARE(frames.stats.chunksBuilt, frames.stats.chunks);
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));
    delete chunks;
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphClipsSampleChunksToFastViews_data() {
    QTest::addColumn<double>("scale");
    for (const double scale : {1.0, 2.0}) QTest::addRow("%.0fx", scale) << scale;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphClipsSampleChunksToFastViews() {
    // Frames slower than the view can be followed in, as at the deepest
    // zoom of a 96 kHz track: a frame that moved on by the view's width or
    // more since the last builds its chunks for the samples the view shows
    // alone, no more geometry than the frame drawn whole, as no later
    // frame shows them. The next frame that moves on by less builds them
    // whole again, and the frames look as drawn whole throughout.
    QFETCH(double, scale);
    QQuickWindow window;
    if (!showSampleWindow(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    const QSize size(1720, 300);
    TestTime time;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    time.drive(item);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(6);
    giveSampleDetail(item, 96'000, 6, 29.5, 30.5);
    item.setZoomLevel(item.maximumZoomLevel());
    setTestPosition(item, 30.0);
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    // 1/120 s moves the view by 6400 px a frame, 1/1200 s by 640.
    const double steps[] = {0.0, 1.0 / 120, 1.0 / 120, 1.0 / 1200, 1.0 / 1200, 1.0 / 120, 1.0 / 1200};
    QSGNode *painted = nullptr;
    QSGNode *chunks = nullptr;
    bool previousClipped = false;
    int clippedFrames = 0;
    for (const double step : steps) {
        time.advance(step);
        bool clip = false;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            QVERIFY(item.renderDetailDirectlyLocked(start, end));
            clip = item.clipSampleChunksLocked(item.sampleChunkColumnLocked(start));
        }
        QCOMPARE(clip, step > 1.0 / 360);
        // As the window draws it.
        painted = WaveformSceneGraph::render(painted, &window, size,
            [&item](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        QVERIFY(painted != nullptr);
        const auto stats = WaveformSceneGraph::statistics(painted);
        QSGNode *whole = renderSamples(item, window, nullptr, false);
        QVERIFY(whole != nullptr);
        const int wholeVertices = WaveformSceneGraph::statistics(whole).geometryVertices;
        delete whole;
        if (clip) {
            ++clippedFrames;
            QCOMPARE(stats.chunksBuilt, stats.chunks);
            QVERIFY2(stats.uploadedVertices <= wholeVertices, qPrintable(QStringLiteral("%1 vertices, %2 whole")
                .arg(stats.uploadedVertices).arg(wholeVertices)));
        } else if (previousClipped) {
            // The chunks the last view's edges cut are built whole; those
            // it showed whole own the same frames, and are kept.
            QVERIFY(stats.chunksBuilt > 0 && stats.chunksBuilt <= stats.chunks);
        }
        // The same frame's chunks, drawn the same way, look as drawn whole.
        const SampleFrames frames = compareSampleFrames(item, window, chunks, clip);
        QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));
        const QString unmarked = samplesWithoutOneMarker(
            frames.squares, visibleSamplePositions(item, size.width()), size, 6, scale);
        QVERIFY2(unmarked.isEmpty(), qPrintable(unmarked));
        previousClipped = clip;
    }
    QCOMPARE(clippedFrames, 3);
    delete painted;
    delete chunks;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphSplitsLargeSampleChunks() {
    // Thirty-two lanes at the deepest zoom: a chunk holds more vertices than
    // a node may, and is split between nodes of whole triangles, within
    // 65,532 vertices each, and looks as the frame drawn whole.
    QQuickWindow window;
    QVERIFY(showSampleWindow(window, 1.0));
    constexpr int kLanes = 32;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    item.setWidth(1720);
    item.setHeight(1600);
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(kLanes);
    giveSampleDetail(item, 96'000, kLanes, 29.9, 30.1);
    item.setZoomLevel(item.maximumZoomLevel());
    setTestPosition(item, 30.0);
    QSGNode *chunks = nullptr;
    const SampleFrames frames = compareSampleFrames(item, window, chunks);
    QVERIFY(frames.stats.chunks > 0);
    int largestChunk = 0;
    for (const SampleChunkNode &chunk : sampleChunkNodes(chunks)) largestChunk = std::max(largestChunk, chunk.vertices);
    QVERIFY2(largestChunk > 65'535, qPrintable(QString::number(largestChunk)));
    QVERIFY2(frames.stats.largestGeometryVertices <= 65'532,
        qPrintable(QString::number(frames.stats.largestGeometryVertices)));
    QCOMPARE(frames.stats.largestGeometryVertices % 3, 0);
    QVERIFY2(sampleFramesDiffer(frames).isEmpty(), qPrintable(sampleFramesDiffer(frames)));
    const QString empty = lanesWithEmptyColumns(frames.chunked, kLanes);
    QVERIFY2(empty.isEmpty(), qPrintable(empty));
    delete chunks;
}

namespace {

// A window at `scale`, shown, or false where the offscreen platform has no
// screen at that scale.
bool showTestWindow(QQuickWindow &window, double scale) {
    if (scale != 1.0) return showOnScreen(window, scale);
    window.resize(64, 64);
    window.show();
    return QTest::qWaitForWindowExposed(&window);
}

// Two 100-pixel lanes of a 400-pixel wide item of samples `spacing`
// logical pixels apart, alternating between -amplitude and amplitude.
void showZigzag(WaveformEditorItem &item, QQuickWindow &window, double amplitude, double spacing) {
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    item.setWidth(400);
    item.setHeight(200);
    item.setDurationSeconds(10.0);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    giveZigzagSampleDetail(item, 1'000, 2, 4.0, 6.0, amplitude);
    item.setZoomLevel(spacing * 10.0 * 1'000 / 400);
    setTestPosition(item, 5.0);
}

// The samples of `item` drawn as the scene graph's geometry, drawn as a
// GPU would, and as QPainter draws them: the software renderer's picture.
std::pair<QImage, QImage> zigzagPictures(WaveformEditorItem &item, QQuickWindow &window, double scale) {
    const QSize size(400, 200);
    const auto draw = [&item, scale](QPainter *painter) {
        const auto [start, end] = item.visibleRangeLocked();
        painter->fillRect(QRect(0, 0, 400, 200), QColor(5, 9, 7));
        item.drawDetailLocked(*painter, 400, 200, start, end, 2, 1.0, scale);
    };
    QSGNode *node = WaveformSceneGraph::render(nullptr, &window, size, draw, false,
        WaveformSceneGraph::Primitives::Geometry);
    if (node == nullptr) return {};
    QImage geometry = rasterizeSceneGeometry(node, size, scale, QColor(5, 9, 7));
    delete node;
    QImage raster((QSizeF(size) * scale).toSize(), QImage::Format_RGB32);
    raster.setDevicePixelRatio(scale);
    QPainter painter(&raster);
    draw(&painter);
    painter.end();
    raster.setDevicePixelRatio(1.0);
    return {geometry, raster};
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformSceneGraphStraightSegmentsKeepTheirWidth_data() {
    QTest::addColumn<double>("amplitude");
    QTest::addColumn<double>("spacing");
    QTest::addColumn<double>("scale");
    for (const double scale : {1.0, 2.0}) {
        for (const double amplitude : {0.8, 1.0}) {
            for (const double spacing : {2.0, 3.0, 4.1}) {
                QTest::addRow("%.1f-%.1fpx-%.0fx", amplitude, spacing, scale) << amplitude << spacing << scale;
            }
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphStraightSegmentsKeepTheirWidth() {
    // Samples alternating between -amplitude and amplitude make nearly
    // vertical segments, which bend sharply at each sample. Each keeps the
    // pen's width, as QPainter draws the same polyline: a stroke offset
    // along its neighbours' level chord would be a twentieth as thick.
    QFETCH(double, amplitude);
    QFETCH(double, spacing);
    QFETCH(double, scale);
    QQuickWindow window;
    if (!showTestWindow(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    WaveformEditorItem item;
    showZigzag(item, window, amplitude, spacing);
    QVERIFY(item.sampleSpacingPixelsLocked() < WaveformEditorItem::kStraightSamplePixelsPerSample);
    const auto [geometry, raster] = zigzagPictures(item, window, scale);
    QVERIFY(!geometry.isNull());
    if (const QString dir = qEnvironmentVariable("FERROUS_WAVEFORM_GRAB_DIR"); !dir.isEmpty()) {
        const QString name = QStringLiteral("%1/zigzag-%2-%3x-%4px-").arg(dir).arg(amplitude)
            .arg(scale).arg(spacing, 0, 'f', 1);
        geometry.save(name + QStringLiteral("geometry.png"));
        raster.save(name + QStringLiteral("qpainter.png"));
    }
    for (int lane = 0; lane < 2; ++lane) {
        const QList<QPointF> points = lanePolyline(item, 400, 200, 2, lane);
        const std::vector<double> drawn = segmentWeights(geometry, points, scale);
        const std::vector<double> painted = segmentWeights(raster, points, scale);
        QCOMPARE(drawn.size(), painted.size());
        QVERIFY(drawn.size() > 50);
        // The pen, 1.2 logical pixels.
        const double pen = 1.2 * scale;
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            QVERIFY2(drawn[i] > 0.75 * painted[i] && drawn[i] < 1.33 * painted[i]
                    && drawn[i] > 0.7 * pen && drawn[i] < 1.4 * pen,
                qPrintable(QStringLiteral("lane %1, segment %2: %3 device pixels thick, QPainter %4")
                    .arg(lane).arg(i).arg(drawn[i]).arg(painted[i])));
        }
        const double ratio = median(drawn) / median(painted);
        QVERIFY2(ratio > 0.9 && ratio < 1.12, qPrintable(QString::number(ratio)));
    }
    // As CI's comparison of a native renderer with the raster reference.
    const auto [different, mean] = imageDifference(geometry, raster, 45);
    const int pixels = geometry.width() * geometry.height();
    QVERIFY2(mean < kZigzagRasterTolerance.mean && different < pixels / kZigzagRasterTolerance.share,
        qPrintable(QStringLiteral("%1 pixels differ, mean %2").arg(different).arg(mean)));
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphStrokeWeightHoldsAcrossTheThreshold_data() {
    QTest::addColumn<double>("amplitude");
    QTest::addColumn<double>("scale");
    for (const double scale : {1.0, 2.0}) {
        for (const double amplitude : {0.3, 0.8, 1.0}) {
            QTest::addRow("%.1f-%.0fx", amplitude, scale) << amplitude << scale;
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphStrokeWeightHoldsAcrossTheThreshold() {
    // Just closer than the threshold, samples are joined by straight
    // segments; just further apart, by curves. Their strokes weigh the same.
    QFETCH(double, amplitude);
    QFETCH(double, scale);
    QQuickWindow window;
    if (!showTestWindow(window, scale)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    const auto weight = [&](double spacing) {
        WaveformEditorItem item;
        showZigzag(item, window, amplitude, spacing);
        const auto [geometry, raster] = zigzagPictures(item, window, scale);
        Q_UNUSED(raster);
        std::vector<double> weights;
        for (int lane = 0; lane < 2; ++lane) {
            const std::vector<double> laneWeights = segmentWeights(
                geometry, lanePolyline(item, 400, 200, 2, lane), scale);
            weights.insert(weights.end(), laneWeights.begin(), laneWeights.end());
        }
        item.setParentItem(nullptr);
        return median(weights);
    };
    const double below = weight(WaveformEditorItem::kStraightSamplePixelsPerSample - 0.1);
    const double above = weight(WaveformEditorItem::kStraightSamplePixelsPerSample + 0.1);
    qInfo("%.1fx, amplitude %.1f: %.3f device pixels below the threshold, %.3f above",
        scale, amplitude, below, above);
    QVERIFY(above > 0.7 * 1.2 * scale);
    QVERIFY2(below > 0.88 * above && below < 1.12 * above,
        qPrintable(QStringLiteral("%1 below, %2 above").arg(below).arg(above)));
}

void WaveformSceneGraphSmokeTest::waveformEditorDetailStaysAheadOfSamplePlayback_data() {
    QTest::addColumn<int>("rate");
    QTest::addColumn<int>("stepsOut");
    QTest::addColumn<int>("replyMs");
    // How long a detail window takes to arrive: longer than the frames
    // between requests, as a 6-channel window of a compressed surround
    // file can, and more at 48 kHz, whose windows span twice as long.
    for (const int stepsOut : {0, 6}) {
        QTest::addRow("96k-%dout", stepsOut) << 96'000 << stepsOut << 250;
        QTest::addRow("48k-%dout", stepsOut) << 48'000 << stepsOut << 450;
    }
}

void WaveformSceneGraphSmokeTest::waveformEditorDetailStaysAheadOfSamplePlayback() {
    QFETCH(int, rate);
    QFETCH(int, stepsOut);
    QFETCH(int, replyMs);
    constexpr int kChannels = 6;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(2000);
    item.setHeight(400);
    item.setDurationSeconds(120.0);
    item.setViewMode(1);
    item.setChannelCountHint(kChannels);
    auto gate = std::make_shared<QSemaphore>();
    // Windows arrive at once until playback starts.
    auto held = std::make_shared<std::atomic_bool>(false);
    item.m_decodeWindow = [gate, held, rate](const QString &, double start, double end, int points,
                                             const std::shared_ptr<std::atomic_bool> &) {
        if (held->load()) gate->tryAcquire(1, 5000);
        return sampleDetailWindow(rate, kChannels, start, end, points);
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/surround.dts"));
    // The first window tells the sample rate; the zoom then asks for samples.
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), rate, kSlowBuildTimeoutMs);
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    const auto viewHasSamples = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && item.detailCoversRangeLocked(start, end)
            && item.detailResolutionCoversLocked(start, end) && item.m_detail.framesPerPoint == 1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    held->store(true);

    item.setPlaying(true);
    setTestPosition(item, 30.0);
    // Four seconds of playback at 60 frames a second, with a seek back
    // halfway. Each window arrives replyMs after it is asked for, as the
    // frames go on.
    const qint64 replyNs = qint64{replyMs} * 1'000'000;
    constexpr int kSeekFrame = 120;
    // After the seek, the window in flight for the old position arrives,
    // then the one for the new position.
    const int seekFrames = static_cast<int>(std::ceil(2.0 * replyMs / (1000.0 / 60.0))) + 2;
    qint64 askedAtNs = -1;
    int requests = 0;
    QStringList uncovered;
    for (int frame = 0; frame < 240; ++frame) {
        time.advance(1.0 / 60.0);
        if (frame == kSeekFrame) setTestPosition(item, 25.0);
        item.handleWindowFrameSwapped();
        // The request it asks for at once, if it does.
        QCoreApplication::processEvents();
        if (item.m_decodeActive && askedAtNs < 0) {
            askedAtNs = time.nowNs;
            ++requests;
        }
        if (askedAtNs >= 0 && time.nowNs - askedAtNs >= replyNs) {
            gate->release();
            QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
            askedAtNs = -1;
        }
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        if (frame == kSeekFrame) QVERIFY(start < 25.1);
        const bool seeking = frame >= kSeekFrame && frame < kSeekFrame + seekFrames;
        if (!seeking && (!item.detailCoversRangeLocked(start, end)
            || !item.detailResolutionCoversLocked(start, end))) {
            uncovered << QStringLiteral("%1 (view %2..%3, detail %4..%5 at %6 frames a point)")
                .arg(frame).arg(start, 0, 'f', 3).arg(end, 0, 'f', 3)
                .arg(item.m_detail.startSeconds, 0, 'f', 3).arg(item.m_detail.endSeconds, 0, 'f', 3)
                .arg(item.m_detail.framesPerPoint);
        }
    }
    QVERIFY(requests >= 2);
    QVERIFY2(uncovered.isEmpty(), qPrintable(QStringLiteral("frames without detail: ")
        + uncovered.mid(0, 6).join(QStringLiteral(", "))));
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformEditorDeferredZoomOutLandsWhilePlaying() {
    // A zoom out while playing at a deep zoom waits for the detail of the
    // wider view, holding the deeper one, and must show once that detail
    // comes, whatever playback does meanwhile. Here frames come too slowly
    // for a raster of the view to be drawn, a part a frame, before
    // playback leaves it: as in a wide view, on a slow machine, or with a
    // busy render thread.
    constexpr int kRate = 96'000;
    constexpr int kChannels = 6;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.m_playbackTilesInline = true;
    item.setWidth(1000);
    item.setHeight(240);
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(kChannels);
    auto gate = std::make_shared<QSemaphore>();
    auto held = std::make_shared<std::atomic_bool>(false);
    item.m_decodeWindow = [gate, held](const QString &, double start, double end, int points,
                                       const std::shared_ptr<std::atomic_bool> &) {
        if (held->load()) gate->tryAcquire(1, 5000);
        return sampleDetailWindow(kRate, kChannels, start, end, points);
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/surround.flac"));
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), kRate, kSlowBuildTimeoutMs);
    // Eight steps out from the deepest zoom, drawn in playback tiles while
    // playing and in a raster while paused.
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, 8));
    const auto viewHasSamples = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && item.detailCoversRangeLocked(start, end)
            && item.detailResolutionCoversLocked(start, end) && item.m_detail.framesPerPoint == 1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    // Detail of little more than the view, as when playback has nearly
    // used up its window, so that the wider view has none.
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        giveSampleDetail(item, kRate, kChannels, start - 0.001, end + 0.001, true);
    }
    QImage canvas(1000, 240, QImage::Format_RGB32);
    const auto paintFrame = [&item, &canvas]() {
        QPainter painter(&canvas);
        item.paint(&painter);
    };
    // Paused, the view is drawn in a raster, which then holds it while
    // the wider zoom's detail is decoded.
    paintFrame();
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    held->store(true);
    const double deeperZoom = item.zoomLevel();
    // Two steps out: the view moves on by about two thirds of its width a
    // frame, and a raster of three views' width takes 24 frames to draw.
    item.setZoomLevel(deeperZoom / std::pow(1.25, 2));
    const double target = item.zoomLevel();
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomOutHandoffPending);
        QCOMPARE(item.m_presentedZoomLevel, deeperZoom);
    }
    // Frames 1/120 s apart. The wider zoom's window comes in the second.
    int landedAt = -1;
    for (int frame = 0; frame < 60 && landedAt < 0; ++frame) {
        time.advance(1.0 / 120.0);
        item.handleWindowFrameSwapped();
        QCoreApplication::processEvents();
        if (frame == 1) {
            QVERIFY(item.m_decodeActive);
            held->store(false);
            gate->release();
            QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
        }
        paintFrame();
        QMutexLocker lock(&item.m_stateMutex);
        if (!item.m_zoomOutHandoffPending && item.m_presentedZoomLevel == target) landedAt = frame;
    }
    QVERIFY2(landedAt >= 0, "the zoom out never showed");
    // Shown in the frame its detail came for.
    QCOMPARE(landedAt, 1);
    // And it stays shown as playback goes on.
    for (int frame = 0; frame < 30; ++frame) {
        time.advance(1.0 / 120.0);
        item.handleWindowFrameSwapped();
        QCoreApplication::processEvents();
        paintFrame();
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_presentedZoomLevel, target);
    }
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformEditorZoomOutWhilePlayingHoldsTheTiledView() {
    // Issue 542: while playing, a zoomed view is drawn in playback tiles,
    // which leave no cache to hold it on. A zoom out then showed at once,
    // empty past the detail the view had until the wider view's came. It
    // holds the view in its tiles instead, as a cached view is held.
    constexpr int kRate = 48'000;
    constexpr int kChannels = 2;
    constexpr int kWidth = 800;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.m_playbackTilesInline = true;
    item.setWidth(kWidth);
    item.setHeight(200);
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(kChannels);
    auto gate = std::make_shared<QSemaphore>();
    auto held = std::make_shared<std::atomic_bool>(false);
    item.m_decodeWindow = [gate, held](const QString &, double start, double end, int points,
                                       const std::shared_ptr<std::atomic_bool> &) {
        if (held->load()) gate->tryAcquire(1, 5000);
        return sampleDetailWindow(kRate, kChannels, start, end, points);
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/stereo.flac"));
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), kRate, kSlowBuildTimeoutMs);
    item.setZoomLevel(8.0);
    const auto viewReady = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && item.detailCoversRangeLocked(start, end)
            && item.detailResolutionCoversLocked(start, end);
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewReady(), kSlowBuildTimeoutMs);
    QImage canvas(kWidth, 200, QImage::Format_RGB32);
    const auto paintFrame = [&item, &canvas]() {
        QPainter painter(&canvas);
        item.paint(&painter);
    };
    // Playing, drawn in tiles.
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    for (int frame = 0; frame < 8; ++frame) {
        paintFrame();
    }
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.playbackTilesEligibleLocked(start, end));
        QVERIFY(item.m_cacheDirty);
        // Detail of little more than the view, as when playback has nearly
        // used up its window.
        giveSampleDetail(item, kRate, kChannels, start - 0.001, end + 0.001, true);
    }
    held->store(true);
    item.setZoomLevel(8.0 / 1.25);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_zoomOutHandoffPending);
        QCOMPARE(item.m_presentedZoomLevel, 8.0);
    }
    paintFrame();
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        const int playhead = static_cast<int>((item.displayedPositionSecondsLocked() - start)
            / (end - start) * kWidth);
        const QString empty = lanesWithEmptyColumns(canvas, kChannels, playhead - 3, playhead + 3);
        QVERIFY2(empty.isEmpty(), qPrintable(empty));
    }
    // The wider view's detail comes, and the zoom shows.
    held->store(false);
    gate->release();
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
    for (int frame = 0; frame < 60; ++frame) {
        time.advance(1.0 / 120.0);
        item.handleWindowFrameSwapped();
        QCoreApplication::processEvents();
        paintFrame();
        QMutexLocker lock(&item.m_stateMutex);
        if (!item.m_zoomOutHandoffPending) {
            break;
        }
    }
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_zoomOutHandoffPending);
        QCOMPARE(item.m_presentedZoomLevel, 8.0 / 1.25);
    }
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformEditorLateDetailDrawsNoRasterEachFrame_data() {
    QTest::addColumn<int>("stepsOut");
    QTest::addColumn<int>("viewMode");
    // Samples with markers, drawn directly, and samples in playback tiles,
    // in a lane each and mixed into one over the track's overview.
    QTest::addRow("5out-split") << 5 << 1;
    QTest::addRow("8out-split") << 8 << 1;
    QTest::addRow("8out-mixed") << 8 << 0;
}

void WaveformSceneGraphSmokeTest::waveformEditorLateDetailDrawsNoRasterEachFrame() {
    // Playing at a deep zoom, the view moves on by more than its width a
    // frame. When detail windows take longer to come than playback takes
    // to use one up, the view has none for a while: it shows what it has
    // until the next comes, rather than drawing a raster of the view for
    // every frame, which the scene graph would upload as new textures.
    QFETCH(int, stepsOut);
    QFETCH(int, viewMode);
    constexpr int kRate = 96'000;
    constexpr int kChannels = 6;
    // A window comes this long after it is asked for: longer than playback
    // takes to use up the margin ahead of the view.
    constexpr qint64 kReplyNs = 800'000'000;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.m_playbackTilesInline = true;
    item.setWidth(1000);
    item.setHeight(240);
    item.setDurationSeconds(60.0);
    item.setViewMode(viewMode);
    item.setChannelCountHint(kChannels);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(160)));
    auto gate = std::make_shared<QSemaphore>();
    auto held = std::make_shared<std::atomic_bool>(false);
    item.m_decodeWindow = [gate, held](const QString &, double start, double end, int points,
                                       const std::shared_ptr<std::atomic_bool> &) {
        if (held->load()) gate->tryAcquire(1, 5000);
        return sampleDetailWindow(kRate, kChannels, start, end, points);
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/surround.flac"));
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), kRate, kSlowBuildTimeoutMs);
    item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
    const auto viewHasSamples = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && item.detailCoversRangeLocked(start, end)
            && item.detailResolutionCoversLocked(start, end) && item.m_detail.framesPerPoint == 1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(viewHasSamples(), kSlowBuildTimeoutMs);
    held->store(true);
    item.setPlaying(true);
    setTestPosition(item, 30.0);

    QImage canvas(1000, 240, QImage::Format_RGB32);
    // Three seconds at 120 frames a second.
    qint64 askedAtNs = -1;
    int arrivals = 0;
    int framesWithoutDetail = 0;
    // The rasters drawn since the last window came, and the most of them
    // between two windows.
    int rasters = 0;
    int mostRasters = 0;
    QStringList misplacedRequests;
    for (int frame = 0; frame < 360; ++frame) {
        time.advance(1.0 / 120.0);
        item.handleWindowFrameSwapped();
        QCoreApplication::processEvents();
        if (item.m_decodeActive && askedAtNs < 0) {
            askedAtNs = time.nowNs;
            // The window asked for holds the playhead, and what playback
            // reaches next.
            QMutexLocker lock(&item.m_stateMutex);
            const double position = item.displayedPositionSecondsLocked();
            if (position < item.m_requestedStartSeconds || position >= item.m_requestedEndSeconds) {
                misplacedRequests << QStringLiteral("%1 at %2..%3").arg(position, 0, 'f', 3)
                    .arg(item.m_requestedStartSeconds, 0, 'f', 3).arg(item.m_requestedEndSeconds, 0, 'f', 3);
            }
        }
        if (askedAtNs >= 0 && time.nowNs - askedAtNs >= kReplyNs) {
            gate->release();
            QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
            askedAtNs = -1;
            ++arrivals;
            rasters = 0;
        }
        qint64 rasterBefore = 0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            rasterBefore = item.m_cache.cacheKey();
            const auto [start, end] = item.visibleRangeLocked();
            if (!item.detailCoversRangeLocked(start, end)) ++framesWithoutDetail;
        }
        {
            QPainter painter(&canvas);
            item.paint(&painter);
        }
        QMutexLocker lock(&item.m_stateMutex);
        if (item.m_cache.cacheKey() != rasterBefore) mostRasters = std::max(mostRasters, ++rasters);
    }
    // The case this is about: windows came, and the view went without
    // detail between them.
    QVERIFY(arrivals >= 2);
    QVERIFY(framesWithoutDetail >= 30);
    QVERIFY2(misplacedRequests.isEmpty(), qPrintable(misplacedRequests.join(QStringLiteral(", "))));
    QVERIFY2(mostRasters <= 1, qPrintable(QStringLiteral("%1 rasters between two windows").arg(mostRasters)));
    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformEditorPauseRedrawsTheRasterPlaybackLeft_data() {
    QTest::addColumn<bool>("requestFails");
    QTest::addRow("no-window-comes") << false;
    QTest::addRow("request-fails") << true;
}

void WaveformSceneGraphSmokeTest::waveformEditorPauseRedrawsTheRasterPlaybackLeft() {
    // While playing without detail, playback may move the view past the
    // raster that stands in for it, which is drawn again only once another
    // window comes. Pausing there must draw it again at once, for the view
    // where it stopped: from the overview, as no detail comes, rather than
    // leaving the view blank.
    QFETCH(bool, requestFails);
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(320);
    item.setHeight(120);
    item.setDurationSeconds(60.0);
    item.setViewMode(0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(4096, static_cast<char>(255)));
    auto gate = std::make_shared<QSemaphore>();
    item.m_decodeWindow = [gate, requestFails](const QString &, double, double, int,
                                               const std::shared_ptr<std::atomic_bool> &) {
        // A request that fails replies with nothing; one whose window
        // never comes waits for the end of the test.
        if (!requestFails) gate->tryAcquire(1, 30'000);
        return QByteArray();
    };
    setTestPosition(item, 30.0);
    item.setSourcePath(QStringLiteral("/fixture/stereo.flac"));
    // A tenth of a second in view.
    item.setZoomLevel(600.0);
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    // A request goes out, and fails or stays in flight.
    item.handleWindowFrameSwapped();
    QTRY_VERIFY_WITH_TIMEOUT(item.m_decodeActive || item.m_detailArrivals > 0, kSlowBuildTimeoutMs);
    if (requestFails) QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);

    QImage canvas(320, 120, QImage::Format_RGB32);
    const auto paintFrame = [&item, &canvas]() {
        QPainter painter(&canvas);
        item.paint(&painter);
    };
    // Rows away from the center line, which is drawn over the waveform.
    const auto waveformShows = [&canvas]() {
        int pixels = 0;
        for (int y = 10; y < 50; ++y) {
            for (int x = 0; x < canvas.width(); ++x) {
                if (canvas.pixelColor(x, y) == QColor(54, 225, 161)) ++pixels;
            }
        }
        return pixels > canvas.width() * 20;
    };
    paintFrame();
    QVERIFY(waveformShows());
    // Playback moves the view past the raster, with no window coming
    // between: the view shows what the raster still covers, nothing here.
    time.advance(1.0);
    paintFrame();
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(!item.cacheCoversRangeLocked(start, end));
    }
    QVERIFY(!waveformShows());

    // Paused there, the view is drawn again where it stopped.
    item.setPlaying(false);
    paintFrame();
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QVERIFY(item.cacheCoversRangeLocked(start, end));
    }
    QVERIFY(waveformShows());

    // Played on and paused by the engine, which holds the clock while
    // the item still plays: the same.
    item.setPlaying(true);
    setTestPosition(item, item.positionSeconds());
    time.advance(1.0);
    paintFrame();
    QVERIFY(!waveformShows());
    testClock(item).holdAt(item.positionSeconds());
    paintFrame();
    QVERIFY(waveformShows());

    item.setSourcePath(QString());
    gate->release(4);
    QTRY_VERIFY_WITH_TIMEOUT(!item.m_decodeActive, kSlowBuildTimeoutMs);
}

void WaveformSceneGraphSmokeTest::waveformEditorDeepZoomPlaybackBenchmark() {
    // The time a frame of the fullscreen viewer spends drawing the waveform
    // while the GUI thread waits, at 2x: 3440 x 1412 logical pixels, six
    // lanes of 96 kHz samples, steps of the wheel out from the deepest zoom.
    // Each run seeks to where no tiles are, then plays 120 frames 1/120 s
    // apart in real time, so that workers have the time they would, and
    // again at 360 frames a second, 1/360 s apart.
    if (!qEnvironmentVariableIsSet("FERROUS_BENCHMARK_WAVEFORM")) QSKIP("optional render benchmark");
    QQuickWindow window;
    if (!showOnScreen(window, 2.0)) QSKIP("Needs the offscreen platform's HiDPI test screens");
    const QSize size(3440, 1412);
    for (const int rate : {120, 360})
    for (const int stepsOut : {0, 3, 4, 5, 6, 7, 8, 10, 12}) {
        TestTime time;
        WaveformEditorItem item;
        item.setParentItem(window.contentItem());
        // The test draws it, not the window.
        item.setVisible(false);
        time.drive(item);
        item.setWidth(size.width());
        item.setHeight(size.height());
        item.setDurationSeconds(60.0);
        item.setViewMode(1);
        item.setChannelCountHint(6);
        giveSampleDetail(item, 96'000, 6, 29.0, 32.5);
        item.setZoomLevel(item.maximumZoomLevel() / std::pow(1.25, stepsOut));
        setTestPosition(item, 30.0);
        item.setPlaying(true);
        setTestPosition(item, 30.0);
        QSGNode *node = nullptr;
        std::vector<double> frameMs;
        int complete = 0;
        double vertices = 0.0;
        double uploadedBytes = 0.0;
        double chunksBuilt = 0.0;
        constexpr int frames = 120;
        for (int frame = 0; frame < frames; ++frame) {
            QElapsedTimer pace;
            pace.start();
            if (frame > 0) time.advance(1.0 / rate);
            QElapsedTimer timer;
            timer.start();
            node = WaveformSceneGraph::render(node, &window, size,
                [&item](QPainter *painter) { item.paint(painter); }, false,
                WaveformSceneGraph::Primitives::Geometry);
            frameMs.push_back(timer.nsecsElapsed() / 1.0e6);
            // The geometry drawn, and that written: the frame's own, and
            // that of the chunks it builds.
            const auto stats = WaveformSceneGraph::statistics(node);
            vertices += stats.geometryVertices;
            uploadedBytes += static_cast<double>(stats.uploadedBytes)
                + static_cast<double>(stats.uploadedVertices) * sizeof(QSGGeometry::ColoredPoint2D);
            chunksBuilt += stats.chunksBuilt;
            {
                QMutexLocker lock(&item.m_stateMutex);
                const auto [start, end] = item.visibleRangeLocked();
                const bool tiled = item.playbackTilesEligibleLocked(start, end);
                if (!tiled || item.playbackTilesCoverLocked(start, end)) ++complete;
            }
            item.handleWindowFrameSwapped();
            QCoreApplication::processEvents();
            const qint64 left = 1'000'000'000 / rate - pace.nsecsElapsed();
            if (left > 0) QThread::usleep(static_cast<unsigned long>(left / 1000));
        }
        std::vector<double> sorted = frameMs;
        std::sort(sorted.begin(), sorted.end());
        double mean = 0.0;
        for (const double ms : frameMs) mean += ms;
        mean /= frameMs.size();
        bool direct = false;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            direct = item.renderDetailDirectlyLocked(start, end);
        }
        const auto slowest = std::max_element(frameMs.begin(), frameMs.end()) - frameMs.begin();
        qInfo("deep-zoom 2x %dx%d hz=%d steps=%d px/sample=%.2f direct=%d first_ms=%.2f mean_ms=%.2f p95_ms=%.2f max_ms=%.2f (frame %d) complete_frames=%d/%d vertices_per_frame=%.0f upload_kib_per_frame=%.0f chunks_built_per_frame=%.2f",
            size.width(), size.height(), rate, stepsOut, 8.0 / std::pow(1.25, stepsOut), direct ? 1 : 0,
            frameMs.front(), mean, sorted[sorted.size() * 95 / 100], sorted.back(),
            static_cast<int>(slowest), complete, frames, vertices / frames,
            uploadedBytes / frames / 1024.0, chunksBuilt / frames);
        delete node;
        item.setParentItem(nullptr);
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphMatchesRaster_data() {
    QTest::addColumn<bool>("samples");
    QTest::addColumn<bool>("splitChannels");
    QTest::addColumn<QSize>("size");
    // The zoom of sample rows: 100 is 6.4 logical pixels a sample at 640
    // wide, 47 is 3.0, joined by straight segments with a quad a marker.
    QTest::addColumn<double>("sampleZoom");
    // Samples alternating between -0.8 and 0.8: nearly vertical segments.
    QTest::addColumn<bool>("zigzag");
    QTest::newRow("overview") << false << false << QSize(640, 240) << 100.0 << false;
    QTest::newRow("channel-rulers") << false << true << QSize(640, 240) << 100.0 << false;
    QTest::newRow("sample-curves") << true << false << QSize(640, 240) << 100.0 << false;
    QTest::newRow("muted-sample-curves") << true << true << QSize(640, 240) << 100.0 << false;
    QTest::newRow("close-samples") << true << true << QSize(640, 240) << 47.0 << false;
    QTest::newRow("close-zigzag") << true << true << QSize(640, 240) << 47.0 << true;
    QTest::newRow("wide-overview") << false << true << QSize(3440, 360) << 100.0 << false;
    QTest::newRow("wide-samples") << true << true << QSize(3440, 360) << 100.0 << false;
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphMatchesRaster() {
    QFETCH(bool, samples);
    QFETCH(bool, splitChannels);
    QFETCH(QSize, size);
    QFETCH(double, sampleZoom);
    QFETCH(bool, zigzag);
    QQuickWindow window;
    // Window managers may clamp a 3440-logical-pixel window at 2x scale.
    // Fit the desktop while keeping the source cache wider than the pane.
    size.setWidth(std::min(size.width(), window.screen()->availableGeometry().width()));
    window.resize(size);
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setOverviewComplete(true);
    QByteArray overview(8192, Qt::Uninitialized);
    for (int i = 0; i < overview.size(); ++i) overview[i] = static_cast<char>(80 + 60 * std::sin(i * 0.03));
    item.setOverviewData(overview);
    item.setGridEnabled(true);
    item.setViewMode(splitChannels ? 1 : 0);
    item.setChannelCountHint(splitChannels ? 2 : 1);
    item.setMutedChannelsMask(splitChannels ? 2 : 0);
    if (size.width() > 640 && !samples) item.setZoomLevel(2.0);
    if (samples) {
        item.m_sampleRateHz = 1000;
        item.m_detail.sampleRateHz = 1000;
        item.m_detail.channelCount = splitChannels ? 2 : 1;
        item.m_detail.startSeconds = 4.0;
        item.m_detail.endSeconds = 6.0;
        item.m_detail.framesPerPoint = 1;
        item.m_detail.pointCount = 2000;
        item.m_detail.extrema.resize(4000 * item.m_detail.channelCount);
        for (int point = 0; point < 2000; ++point) {
            for (int channel = 0; channel < item.m_detail.channelCount; ++channel) {
                const float value = zigzag ? (point % 2 == 0 ? -0.8F : 0.8F)
                    : static_cast<float>(std::sin(point * 0.18 + channel) * 0.8);
                const int index = (point * item.m_detail.channelCount + channel) * 2;
                item.m_detail.extrema[index] = value;
                item.m_detail.extrema[index + 1] = value;
            }
        }
        item.setZoomLevel(sampleZoom);
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    item.setHoverPosition(220, 100, true);
    QImage actual = window.grabWindow();
    QVERIFY(!actual.isNull());
    const qreal scale = window.effectiveDevicePixelRatio();
    QImage expected((QSizeF(size) * scale).toSize(), QImage::Format_RGB32);
    expected.setDevicePixelRatio(scale);
    QPainter painter(&expected); item.paint(&painter); painter.end();
    actual = actual.convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(), expected.size());
    quint64 error = 0;
    int different = 0;
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const QColor a = actual.pixelColor(x, y);
            const QColor b = expected.pixelColor(x, y);
            const int delta = std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
            error += delta;
            if (delta > 45) ++different;
        }
    }
    const int pixels = expected.width() * expected.height();
    const double mean = static_cast<double>(error) / (pixels * 3);
    // Geometry AA can differ from QPainter at curve edges, but not in the
    // waveform shape, ruler positions, clipping, labels or channel colors.
    const RasterTolerance tolerance = zigzag ? kZigzagRasterTolerance : kRasterTolerance;
    if (mean >= tolerance.mean || different >= pixels / tolerance.share) {
        actual.save(QStringLiteral("/tmp/ferrous-waveform-actual.png"));
        expected.save(QStringLiteral("/tmp/ferrous-waveform-expected.png"));
    }
    QVERIFY2(mean < tolerance.mean && different < pixels / tolerance.share,
        qPrintable(QStringLiteral("mean=%1 differing_pixels=%2").arg(mean).arg(different)));
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphBenchmark() {
    if (!qEnvironmentVariableIsSet("FERROUS_BENCHMARK_WAVEFORM")) QSKIP("optional render benchmark");
    QQuickWindow window;
    window.resize(64, 64); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const QSize size : {QSize(1200, 180), QSize(3440, 1440)}) {
        for (const bool grid : {false, true}) {
        for (const double zoom : {1.0, 8.0, 4000.0}) {
            WaveformEditorItem item;
            item.setWidth(size.width()); item.setHeight(size.height());
            // The grid adds rulers and their labels.
            item.setGridEnabled(grid);
            item.setDurationSeconds(60.0); setTestPosition(item, 30.0);
            item.setOverviewComplete(true);
            item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
            item.m_sampleRateHz = 48000;
            if (zoom > 100) {
                item.m_detail.sampleRateHz = 48000; item.m_detail.channelCount = 1;
                item.m_detail.startSeconds = 29.5; item.m_detail.endSeconds = 30.5;
                item.m_detail.framesPerPoint = 1; item.m_detail.pointCount = 48000;
                item.m_detail.extrema.resize(96000);
                for (int i = 0; i < 48000; ++i) {
                    const float value = static_cast<float>(std::sin(i * 0.04) * 0.8);
                    item.m_detail.extrema[i * 2] = value; item.m_detail.extrema[i * 2 + 1] = value;
                }
            }
            item.setZoomLevel(zoom);
            QImage canvas(size, QImage::Format_RGB32);
            QPainter raster(&canvas);
            for (int i = 0; i < 10; ++i) item.paint(&raster);
            constexpr int frames = 180;
            QElapsedTimer timer; timer.start();
            for (int i = 0; i < frames; ++i) {
                item.m_clockState.anchorSeconds = 30.0 + i * 0.00001;
                item.paint(&raster);
            }
            const double rasterMs = timer.nsecsElapsed() / 1.0e6 / frames;
            QSGNode *node = nullptr;
            auto prepare = [&]() {
                const auto [start, end] = item.visibleRangeLocked();
                const bool rasterize = window.rendererInterface()->graphicsApi() == QSGRendererInterface::Software
                    && item.renderDetailDirectlyLocked(start, end);
                node = WaveformSceneGraph::render(node, &window, size, [&](QPainter *p) { item.paint(p); }, rasterize);
            };
            for (int i = 0; i < 10; ++i) prepare();
            quint64 uploads = 0; quint64 bytes = 0;
            timer.restart();
            for (int i = 0; i < frames; ++i) {
                item.m_clockState.anchorSeconds = 30.0 + i * 0.00001; prepare();
                const auto stats = WaveformSceneGraph::statistics(node);
                uploads += stats.textureUploads; bytes += stats.uploadedBytes;
            }
            const double sceneMs = timer.nsecsElapsed() / 1.0e6 / frames;
            qInfo("waveform %dx%d grid=%d zoom=%.0f raster_ms=%.4f scene_ms=%.4f uploads=%llu bytes=%llu",
                size.width(), size.height(), grid ? 1 : 0, zoom, rasterMs, sceneMs,
                static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(bytes));
            delete node;
        }
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphLaysOutEachLabelOnce() {
    QQuickWindow window;
    window.resize(64, 64);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    WaveformEditorItem item;
    item.setWidth(1200);
    item.setHeight(180);
    item.setGridEnabled(true);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    item.setDurationSeconds(60.0);
    setTestPosition(item, 30.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    const auto render = [&](QSGNode *node) {
        return WaveformSceneGraph::render(node, &window, QSize(1200, 180),
            [&](QPainter *painter) { item.paint(painter); });
    };
    QSGNode *node = render(nullptr);
    QVERIFY(node != nullptr);
    // Both channels' rulers and the time ruler.
    QVERIFY(WaveformSceneGraph::statistics(node).labelRasters >= 20);
    // The labels of the next frames are the same ones.
    for (int frame = 0; frame < 3; ++frame) {
        node = render(node);
        QCOMPARE(WaveformSceneGraph::statistics(node).labelRasters, 0);
    }
    delete node;
}

void WaveformSceneGraphSmokeTest::waveformEditorIdleViewWaitsForThePlayheadsNextPixel() {
    // 100 s over 1000 pixels: the playhead moves a pixel every 0.1 s. On
    // the test's time, so that a loaded machine moves nothing.
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(1000);
    item.setHeight(100);
    item.setDurationSeconds(100.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    // The window paints, then hears that the frame is on screen.
    QImage canvas(1000, 100, QImage::Format_RGB32);
    const auto paint = [&item, &canvas]() {
        QPainter painter(&canvas);
        item.paint(&painter);
    };
    setTestPosition(item, 10.02);
    item.setPlaying(true);
    setTestPosition(item, 10.02);
    const quint64 requested = item.m_playbackFrameRequests;
    paint();
    item.handleWindowFrameSwapped();
    // The whole track is in view: no frame until pixel 101 at 10.1 s.
    QCOMPARE(item.m_playbackFrameRequests, requested);
    // Due in 80 ms, rounded up to whole milliseconds; the timer then wakes it.
    const auto dueInMs = [&item, &time]() {
        const qint64 due = item.m_frameScheduler != nullptr
            ? item.m_frameScheduler->dueNs(&item) : -1;
        return due >= 0 ? (due - time.nowNs + 999'999) / 1'000'000 : -1;
    };
    QVERIFY2(dueInMs() == 80 || dueInMs() == 81, qPrintable(QString::number(dueInMs())));
    QVERIFY(item.m_frameScheduler->timerActive());
    QTRY_COMPARE(item.m_playbackFrameRequests, requested + 1);

    // Zoomed in, the view follows the playhead a device pixel at a time:
    // 12.5 s over 1000 pixels, the next in 12.5 ms.
    item.setZoomLevel(8.0);
    setTestPosition(item, 50.0);
    const quint64 zoomed = item.m_playbackFrameRequests;
    paint();
    item.handleWindowFrameSwapped();
    QCOMPARE(item.m_playbackFrameRequests, zoomed);
    // Rounded up to the scheduler's milliseconds, from the test's time.
    QVERIFY2(dueInMs() == 13 || dueInMs() == 14, qPrintable(QString::number(dueInMs())));
    // Until the playhead reaches the middle of a view that starts at the
    // track's start, the view stays where it is.
    setTestPosition(item, 6.2);
    const quint64 atStart = item.m_playbackFrameRequests;
    paint();
    item.handleWindowFrameSwapped();
    QCOMPARE(item.m_playbackFrameRequests, atStart);
    QVERIFY(dueInMs() >= 0 && dueInMs() <= 14);
    // A course correction reschedules without a frame; a seek shows at once.
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = clock.positionAt(time.nowNs) + 0.001;
    sample.sampledAtNs = time.nowNs;
    sample.advancing = true;
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, false);
    QCOMPARE(item.m_playbackFrameRequests, atStart);
    QVERIFY(dueInMs() >= 0);
    sample.positionSeconds = 1.0;
    clock.applySample(sample, true);
    QCOMPARE(item.m_playbackFrameRequests, atStart + 1);
    QCOMPARE(dueInMs(), -1);
    paint();
    item.handleWindowFrameSwapped();
    QVERIFY(dueInMs() >= 0);
    item.setPlaying(false);
    QCOMPARE(dueInMs(), -1);
    QVERIFY(!item.m_frameScheduler->timerActive());
}

void WaveformSceneGraphSmokeTest::waveformEditorNoOneSeesAsksForNoPlaybackFrames() {
    // A waveform no one sees, such as the one behind the spectrogram view
    // or one in a window off screen, follows the clock but asks for no
    // frames. Shown again, it asks for one; its frames follow from there.
    // Zoomed in, the view follows the playhead a device pixel at a time.
    QQuickWindow window;
    window.resize(1000, 100);
    presentWithoutDrawing(window);
    TestTime time;
    auto *item = new WaveformEditorItem(window.contentItem());
    time.drive(*item);
    item->setWidth(1000);
    item->setHeight(100);
    item->setDurationSeconds(100.0);
    item->setOverviewComplete(true);
    item->setOverviewData(QByteArray(8192, static_cast<char>(128)));
    item->setZoomLevel(8.0);
    setTestPosition(*item, 50.0);
    item->setPlaying(true);
    setTestPosition(*item, 50.0);
    // The window does not draw it; the test does.
    QImage canvas(1000, 100, QImage::Format_RGB32);
    const auto paint = [item, &canvas]() {
        QPainter painter(&canvas);
        item->paint(&painter);
    };
    const auto scheduled = [item]() {
        return item->m_frameScheduler != nullptr && item->m_frameScheduler->dueNs(item) >= 0;
    };
    paint();
    item->handleWindowFrameSwapped();
    QVERIFY(scheduled());
    PlaybackFrameScheduler *scheduler = item->m_frameScheduler;
    PlaybackClock &clock = testClock(*item);
    // Half a second of the engine's samples, which correct the clock's
    // course, while the window draws frames for its other items; then a
    // seek and another track.
    const auto playOn = [&]() {
        PlaybackSample sample;
        sample.advancing = true;
        sample.trackToken = clock.state().trackToken;
        for (int i = 0; i < 12; ++i) {
            time.advance(0.04);
            sample.positionSeconds = clock.positionAt(time.nowNs) + 0.001;
            sample.sampledAtNs = time.nowNs;
            clock.applySample(sample, false);
            item->handleWindowFrameSwapped();
        }
        sample.positionSeconds = 70.0;
        clock.applySample(sample, true);
        sample.trackToken += 1;
        sample.positionSeconds = 60.0;
        clock.applySample(sample, false);
    };

    item->setVisible(false);
    QVERIFY(!scheduled());
    QVERIFY(!scheduler->timerActive());
    quint64 requested = item->m_playbackFrameRequests;
    playOn();
    QCOMPARE(item->m_playbackFrameRequests, requested);
    QVERIFY(!scheduled());
    QVERIFY(!scheduler->timerActive());
    item->setVisible(true);
    QCOMPARE(item->m_playbackFrameRequests, requested + 1);
    {
        // The frame shows the view at the playhead now.
        QMutexLocker lock(&item->m_stateMutex);
        const auto [start, end] = item->visibleRangeLocked();
        const double position = clock.positionAt(time.nowNs);
        QVERIFY(start <= position && position <= end);
    }
    paint();
    item->handleWindowFrameSwapped();
    QVERIFY(scheduled());

    // Its window off screen: a brief unexposure changes nothing; once its
    // grace ends, the view stops asking for frames.
    setTestExposed(window, false);
    QVERIFY(scheduled());
    endTestUnexposedGrace(window);
    QVERIFY(!scheduled());
    QVERIFY(!scheduler->timerActive());
    requested = item->m_playbackFrameRequests;
    playOn();
    QCOMPARE(item->m_playbackFrameRequests, requested);
    QVERIFY(!scheduler->timerActive());
    setTestExposed(window, true);
    QCOMPARE(item->m_playbackFrameRequests, requested + 1);
    paint();
    item->handleWindowFrameSwapped();
    QVERIFY(scheduled());
    delete item;
}

namespace {

// Ten seconds of a stereo 48 kHz sawtooth, as a 16-bit WAV file.
QByteArray syntheticTenSecondWav() {
    constexpr quint32 kFrames = 480'000;
    QByteArray wav("RIFF", 4);
    appendLe<quint32>(wav, 36 + kFrames * 4);
    wav.append("WAVEfmt ", 8);
    appendLe<quint32>(wav, 16);
    appendLe<quint16>(wav, 1);
    appendLe<quint16>(wav, 2);
    appendLe<quint32>(wav, 48'000);
    appendLe<quint32>(wav, 48'000 * 4);
    appendLe<quint16>(wav, 4);
    appendLe<quint16>(wav, 16);
    wav.append("data", 4);
    appendLe<quint32>(wav, kFrames * 4);
    for (quint32 frame = 0; frame < kFrames; ++frame) {
        const qint16 sample = static_cast<qint16>((frame % 48) * 512);
        appendLe<qint16>(wav, sample);
        appendLe<qint16>(wav, -sample);
    }
    return wav;
}

// The real decoder for `item`, each decode held until the test releases
// `gate`; `started` counts them.
void holdTestDecodes(WaveformEditorItem &item, const std::shared_ptr<QSemaphore> &gate,
                     const std::shared_ptr<std::atomic_int> &started) {
    item.m_decodeWindow = [gate, started](const QString &path, double start, double end,
                                          int points,
                                          const std::shared_ptr<std::atomic_bool> &cancelled) {
        ++*started;
        gate->tryAcquire(1, 5000);
        return WaveformEditorItem::decodeWindow(path, start, end, points, cancelled);
    };
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformEditorOffScreenDecodesNoDetail() {
    // A zoomed waveform in a window off screen decodes no detail: the
    // request in flight is cancelled, and the engine's samples, a seek
    // among them, start none. The detail it has stays; back on screen, it
    // asks for the detail of where playback is then.
    constexpr quint32 kFrames = 480'000;
    const QByteArray wav = syntheticTenSecondWav();
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(wav), wav.size());
    QVERIFY(file.flush());

    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    TestTime time;
    auto *item = new WaveformEditorItem(window.contentItem());
    time.drive(*item);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    holdTestDecodes(*item, gate, started);
    item->setWidth(400);
    item->setHeight(100);
    item->setDurationSeconds(static_cast<double>(kFrames) / 48'000.0);
    item->setZoomLevel(8.0);
    setTestPosition(*item, 3.0);
    item->setPlaying(true);
    setTestPosition(*item, 3.0);
    item->setSourcePath(file.fileName());
    const auto viewHasDetail = [item]() {
        QMutexLocker lock(&item->m_stateMutex);
        const auto [start, end] = item->detailRequestVisibleRangeLocked();
        return item->detailCoversRangeLocked(start, end);
    };
    QTRY_COMPARE(started->load(), 1);
    gate->release();
    QTRY_VERIFY(!item->m_decodeActive);
    QVERIFY(viewHasDetail());
    const double detailStart = item->m_detail.startSeconds;

    // A seek out of it asks for more, which is in flight when the window
    // leaves the screen.
    PlaybackClock &clock = testClock(*item);
    PlaybackSample sample;
    sample.advancing = true;
    sample.trackToken = clock.state().trackToken;
    sample.durationSeconds = item->durationSeconds();
    sample.positionSeconds = 7.0;
    sample.sampledAtNs = time.nowNs;
    clock.applySample(sample, true);
    QTRY_COMPARE(started->load(), 2);
    const std::shared_ptr<std::atomic_bool> inFlight = item->m_decodeCancelled;
    QVERIFY(inFlight && !inFlight->load());
    setTestExposed(window, false);
    // Within the grace, it goes on.
    QVERIFY(!inFlight->load());
    endTestUnexposedGrace(window);
    QVERIFY(inFlight->load());
    QVERIFY(!item->m_requestInFlight);
    QVERIFY(!item->m_requestTimer.isActive());
    gate->release();
    QTRY_VERIFY(!item->m_decodeActive);
    // The engine's samples go on, with another seek; nothing decodes.
    for (int i = 0; i < 12; ++i) {
        time.advance(0.04);
        sample.positionSeconds = clock.positionAt(time.nowNs) + 0.001;
        sample.sampledAtNs = time.nowNs;
        clock.applySample(sample, false);
        item->handleWindowFrameSwapped();
    }
    sample.positionSeconds = 8.5;
    clock.applySample(sample, true);
    QTest::qWait(150);
    QCOMPARE(started->load(), 2);
    QVERIFY(!item->m_requestTimer.isActive());
    QVERIFY(!item->m_decodeActive);
    // What it decoded before stays.
    QVERIFY(item->m_detail.pointCount > 0);
    QCOMPARE(item->m_detail.startSeconds, detailStart);

    // Back on screen, the detail of where playback is now.
    setTestExposed(window, true);
    QTRY_COMPARE(started->load(), 3);
    gate->release();
    QTRY_VERIFY(!item->m_decodeActive);
    QVERIFY(viewHasDetail());
    QVERIFY(item->m_detail.startSeconds <= 8.5 && item->m_detail.endSeconds >= 8.5);
    gate->release(8);
    delete item;
}

void WaveformSceneGraphSmokeTest::waveformEditorMovedBetweenWindowsFollowsTheirPresence() {
    // A paused, zoomed waveform moved into a window already off screen
    // cancels its detail request in flight; moved back into one on screen,
    // it asks for the detail it misses. The windows and the view keep
    // their size, so only the move tells it.
    const QByteArray wav = syntheticTenSecondWav();
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write(wav), wav.size());
    QVERIFY(file.flush());
    QQuickWindow shown;
    shown.resize(400, 100);
    presentWithoutDrawing(shown);
    QQuickWindow offScreen;
    offScreen.resize(400, 100);
    presentWithoutDrawing(offScreen);
    setTestExposed(offScreen, false);
    endTestUnexposedGrace(offScreen);
    QVERIFY(WindowPresence::of(&shown)->shown());
    QVERIFY(!WindowPresence::of(&offScreen)->shown());

    TestTime time;
    auto *item = new WaveformEditorItem(shown.contentItem());
    time.drive(*item);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    holdTestDecodes(*item, gate, started);
    item->setWidth(400);
    item->setHeight(100);
    item->setDurationSeconds(10.0);
    item->setZoomLevel(8.0);
    setTestPosition(*item, 3.0);
    item->setSourcePath(file.fileName());
    QTRY_COMPARE(started->load(), 1);
    const std::shared_ptr<std::atomic_bool> inFlight = item->m_decodeCancelled;
    QVERIFY(inFlight && !inFlight->load());

    item->setParentItem(offScreen.contentItem());
    QCOMPARE(item->window(), &offScreen);
    QVERIFY(inFlight->load());
    QVERIFY(!item->m_requestInFlight);
    QVERIFY(!item->m_requestTimer.isActive());
    gate->release();
    QTRY_VERIFY(!item->m_decodeActive);
    QTest::qWait(100);
    QCOMPARE(started->load(), 1);
    QCOMPARE(item->m_detail.pointCount, 0);

    item->setParentItem(shown.contentItem());
    QCOMPARE(item->window(), &shown);
    QTRY_COMPARE(started->load(), 2);
    gate->release();
    QTRY_VERIFY(!item->m_decodeActive);
    {
        QMutexLocker lock(&item->m_stateMutex);
        const auto [start, end] = item->detailRequestVisibleRangeLocked();
        QVERIFY(item->detailCoversRangeLocked(start, end));
    }
    gate->release(8);
    delete item;
}

void WaveformSceneGraphSmokeTest::waveformEditorReadsItsFileOnlyWhileShown() {
    // The waveform detail service keeps what a view reads of its file only
    // while the view is shown in a window on screen: hidden, as in the
    // spectrogram mode, or in a window off screen, as minimized, it frees
    // the file's fine caches and decoder. A view moving to the next track,
    // or into another window, retains the new before it releases the old.
    WaveformReaderLog log;
    QQuickWindow shown;
    shown.resize(400, 100);
    presentWithoutDrawing(shown);
    QQuickWindow other;
    other.resize(400, 100);
    presentWithoutDrawing(other);
    TestTime time;
    auto *item = new WaveformEditorItem(shown.contentItem());
    time.drive(*item);
    item->m_decodeWindow = [](const QString &, double, double, int,
                              const std::shared_ptr<std::atomic_bool> &) { return QByteArray(); };
    item->setWidth(400);
    item->setHeight(100);
    item->setDurationSeconds(10.0);
    QCOMPARE(log.held(), QStringList());
    item->setSourcePath(QStringLiteral("first.wav"));
    QCOMPARE(log.held(), QStringList{QStringLiteral("first.wav view")});

    WaveformReaderLog::order().clear();
    item->setSourcePath(QStringLiteral("second.wav"));
    QCOMPARE(WaveformReaderLog::order(),
             (QStringList{QStringLiteral("+second.wav view"), QStringLiteral("-first.wav view")}));
    QTRY_VERIFY(WaveformReaderLog::releasesUnread().load() >= 1);

    item->setVisible(false);
    QCOMPARE(log.held(), QStringList());
    item->setVisible(true);
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav view")});

    // Off screen once its grace ends, not at once.
    setTestExposed(shown, false);
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav view")});
    endTestUnexposedGrace(shown);
    QCOMPARE(log.held(), QStringList());
    setTestExposed(shown, true);
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav view")});

    // A second view of the file, as the fullscreen viewer's, holds it too.
    auto *viewer = new WaveformEditorItem(other.contentItem());
    time.drive(*viewer);
    viewer->m_decodeWindow = item->m_decodeWindow;
    viewer->setSourcePath(QStringLiteral("second.wav"));
    QCOMPARE(log.held(),
             (QStringList{QStringLiteral("second.wav view"), QStringLiteral("second.wav view")}));
    delete viewer;
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav view")});

    // Moved into a window off screen, and back.
    setTestExposed(other, false);
    endTestUnexposedGrace(other);
    item->setParentItem(other.contentItem());
    QCOMPARE(log.held(), QStringList());
    item->setParentItem(shown.contentItem());
    QCOMPARE(log.held(), QStringList{QStringLiteral("second.wav view")});

    const int passes = WaveformReaderLog::releasesUnread().load();
    delete item;
    QCOMPARE(log.held(), QStringList());
    QTRY_VERIFY(WaveformReaderLog::releasesUnread().load() > passes);
}

void WaveformSceneGraphSmokeTest::waveformEditorLateFrameCallbackDrawsAtOnce() {
    // 100 s over 100 pixels: a pixel a second. The frame painted at 10.4 s
    // shows pixel 10 until 11 s. Its frame-swapped callback arrives late,
    // after playback has passed that: the next frame is due at once, not
    // at the boundary after.
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(100);
    item.setHeight(50);
    item.setDurationSeconds(100.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    setTestPosition(item, 10.4);
    item.setPlaying(true);
    setTestPosition(item, 10.4);
    QImage canvas(100, 50, QImage::Format_RGB32);
    {
        QPainter painter(&canvas);
        item.paint(&painter);
    }
    PlaybackClock &clock = testClock(item);
    time.advance(0.62);
    QCOMPARE(clock.positionAt(clock.nowNs()), 11.02);
    const quint64 requested = item.m_playbackFrameRequests;
    item.handleWindowFrameSwapped();
    QCOMPARE(item.m_playbackFrameRequests, requested + 1);
    QVERIFY(item.m_frameScheduler == nullptr || item.m_frameScheduler->dueNs(&item) < 0);
    // A course correction then does not postpone it either.
    {
        QPainter painter(&canvas);
        item.paint(&painter);
    }
    time.advance(1.0);
    PlaybackSample sample;
    sample.positionSeconds = clock.positionAt(clock.nowNs()) + 0.001;
    sample.sampledAtNs = clock.nowNs();
    sample.advancing = true;
    sample.trackToken = clock.state().trackToken;
    const quint64 corrected = item.m_playbackFrameRequests;
    clock.applySample(sample, false);
    QCOMPARE(item.m_playbackFrameRequests, corrected + 1);
}

namespace {

// A 1000-pixel view of a 10 s track of 1000 Hz samples, playing from 5 s,
// on the test's time and a display of `refreshRateHz`: at zoom z it
// scrolls by 100 z logical pixels a second, and its samples are drawn
// directly from zoom 20.
struct FastScrollView {
    TestTime time;
    WaveformEditorItem item;
    QImage canvas{1000, 100, QImage::Format_RGB32};

    explicit FastScrollView(double refreshRateHz) {
        time.drive(item);
        item.setWidth(1000);
        item.setHeight(100);
        item.setDurationSeconds(10.0);
        giveSampleDetail(item, 1000, 1, 0.0, 10.0);
        item.m_refreshRateHz = refreshRateHz;
        setTestPosition(item, 5.0);
        item.setPlaying(true);
        setTestPosition(item, 5.0);
    }
    // A frame as the window draws it: painted, then on screen.
    void frame() {
        {
            QPainter painter(&canvas);
            item.paint(&painter);
        }
        item.handleWindowFrameSwapped();
    }
    // How long until the frame the item asked for, rounded up to the
    // scheduler's milliseconds; -1 for none.
    qint64 dueInNs() const {
        const qint64 due = item.m_frameScheduler != nullptr ? item.m_frameScheduler->dueNs(&item) : -1;
        return due >= 0 ? due - time.nowNs : -1;
    }
    bool capped() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.m_fastScrollCapped;
    }
    qint64 nextCappedFrameNs() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.m_nextCappedFrameNs;
    }
    // Whether the next frame is asked for when playback reaches the next
    // device column, as on every refresh: at once, or that soon, rounded
    // up to the scheduler's milliseconds.
    bool dueAtNextColumn(quint64 requestedBefore) const {
        const qint64 column = std::llround(1e9 * 10.0 / item.zoomLevel() / 1000.0);
        return item.m_playbackFrameRequests > requestedBefore
            || (dueInNs() >= 0 && dueInNs() <= column + 1'000'000);
    }
};

// The time from one capped frame to the next on a display of `rate`.
qint64 cappedIntervalNs(double rate) {
    return std::llround(WaveformEditorItem::fastScrollRefreshDivisor(rate) * 1e9 / rate);
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformEditorCapsOnlyFastScrolling() {
    // On a 360 Hz display, a view that scrolls faster than 4,000 logical
    // pixels a second is drawn on every 3rd refresh; one slower, whenever
    // playback moves it on, up to every refresh. The cap comes on above
    // 4,000 px/s and goes off below 3,200 px/s, so a zoom between them
    // keeps what it had.
    FastScrollView view(360.0);
    struct Step {
        double zoom;
        bool capped;
    };
    const Step steps[] = {
        {30.0, false}, {39.0, false}, {41.0, true}, {35.0, true}, {33.0, true},
        {31.0, false}, {35.0, false}, {39.0, false}, {41.0, true}, {80.0, true},
    };
    const qint64 interval = cappedIntervalNs(360.0);
    QCOMPARE(interval, 8'333'333);
    for (const Step &step : steps) {
        view.item.setZoomLevel(step.zoom);
        QCOMPARE(view.item.zoomLevel(), step.zoom);
        view.time.advance(0.02);
        const quint64 requested = view.item.m_playbackFrameRequests;
        view.frame();
        const QString where = QStringLiteral("zoom %1, %2 px/s, due in %3 ns")
            .arg(step.zoom).arg(step.zoom * 100.0).arg(view.dueInNs());
        QVERIFY2(view.capped() == step.capped, qPrintable(where));
        if (step.capped) {
            // 1/120 s from the frame, rounded up to whole milliseconds.
            QVERIFY2(view.item.m_playbackFrameRequests == requested, qPrintable(where));
            QVERIFY2(view.dueInNs() >= interval && view.dueInNs() < interval + 1'000'000, qPrintable(where));
        } else {
            QVERIFY2(view.dueAtNextColumn(requested), qPrintable(where));
        }
    }
    // Paused, nothing is capped; playing again, the first frame shows at
    // once and the cap holds from it.
    view.item.setPlaying(false);
    PlaybackClock &clock = testClock(view.item);
    PlaybackSample sample;
    sample.positionSeconds = clock.positionAt(view.time.nowNs);
    sample.sampledAtNs = view.time.nowNs;
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, false);
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), 0);
    QCOMPARE(view.dueInNs(), -1);
    {
        QMutexLocker lock(&view.item.m_stateMutex);
        QCOMPARE(view.item.cappedFrameIntervalNsLocked(), 0);
    }
    view.item.setPlaying(true);
    const quint64 resumed = view.item.m_playbackFrameRequests;
    sample.advancing = true;
    clock.applySample(sample, false);
    QCOMPARE(view.item.m_playbackFrameRequests, resumed + 1);
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), view.time.nowNs + interval);
}

void WaveformSceneGraphSmokeTest::waveformEditorCapsAtAWholeNumberOfRefreshes_data() {
    QTest::addColumn<double>("rate");
    QTest::addColumn<int>("divisor");
    QTest::newRow("480 Hz") << 480.0 << 4;
    QTest::newRow("360 Hz") << 360.0 << 3;
    QTest::newRow("359.98 Hz") << 359.98 << 3;
    QTest::newRow("300 Hz") << 300.0 << 3;
    QTest::newRow("240 Hz") << 240.0 << 2;
    QTest::newRow("165 Hz") << 165.0 << 1;
    QTest::newRow("144 Hz") << 144.0 << 1;
    QTest::newRow("120 Hz") << 120.0 << 1;
    QTest::newRow("60 Hz") << 60.0 << 1;
    QTest::newRow("unknown") << 0.0 << 1;
}

void WaveformSceneGraphSmokeTest::waveformEditorCapsAtAWholeNumberOfRefreshes() {
    // The cap draws every n-th refresh, the whole number nearest 120
    // frames a second, so that the frames are evenly spaced. Frames keep to
    // a grid of that interval from the first: one the timer wakes a little
    // late for does not move the ones after it.
    QFETCH(double, rate);
    QFETCH(int, divisor);
    QCOMPARE(WaveformEditorItem::fastScrollRefreshDivisor(rate), divisor);
    FastScrollView view(rate);
    view.item.setZoomLevel(60.0);
    const quint64 requested = view.item.m_playbackFrameRequests;
    view.frame();
    QVERIFY(view.capped());
    if (divisor == 1) {
        // Every refresh: when playback reaches the next device column.
        QCOMPARE(view.nextCappedFrameNs(), 0);
        QVERIFY(view.dueAtNextColumn(requested));
        return;
    }
    const qint64 interval = std::llround(divisor * 1e9 / rate);
    const qint64 refresh = std::llround(1e9 / rate);
    const qint64 first = view.time.nowNs;
    QCOMPARE(view.nextCappedFrameNs(), first + interval);
    QCOMPARE(view.item.m_playbackFrameRequests, requested);
    QVERIFY(view.dueInNs() >= interval && view.dueInNs() < interval + 1'000'000);
    // On time, and a little under a refresh late: the grid holds.
    view.time.nowNs = first + interval;
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), first + 2 * interval);
    view.time.nowNs = first + 2 * interval + refresh - 100'000;
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), first + 3 * interval);
    // The frame due next waits for its place on the grid.
    const qint64 wait = first + 3 * interval - view.time.nowNs;
    QVERIFY(view.dueInNs() >= wait && view.dueInNs() < wait + 1'000'000);
    // A refresh or more late, it starts again from the frame.
    view.time.nowNs = first + 3 * interval + refresh;
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), view.time.nowNs + interval);
}

void WaveformSceneGraphSmokeTest::waveformEditorFastScrollRedrawsChangesAtOnce() {
    // Capped, a pause, a resume, a seek, a jump back to the start as
    // Repeat-One makes, and a gapless change to the next track each ask
    // for a frame at once rather than waiting for the next capped one. A
    // zoom, a resize or an edit is drawn in the window's next frame, and
    // the capped frames start again from it.
    FastScrollView view(360.0);
    const qint64 interval = cappedIntervalNs(360.0);
    view.item.setZoomLevel(60.0);
    view.frame();
    QVERIFY(view.capped());
    QVERIFY(view.dueInNs() >= interval);
    PlaybackClock &clock = testClock(view.item);
    const auto sampleAt = [&view, &clock](double seconds, bool advancing, quint64 trackToken) {
        PlaybackSample sample;
        sample.positionSeconds = seconds;
        sample.sampledAtNs = view.time.nowNs;
        sample.advancing = advancing;
        sample.trackToken = trackToken;
        return sample;
    };
    // A course correction waits with the frames it corrects.
    view.time.advance(0.002);
    quint64 requested = view.item.m_playbackFrameRequests;
    clock.applySample(sampleAt(clock.positionAt(view.time.nowNs) + 0.001, true, clock.state().trackToken), false);
    QCOMPARE(view.item.m_playbackFrameRequests, requested);
    QVERIFY(view.dueInNs() > 0 && view.dueInNs() <= interval);
    const auto checkAtOnce = [&view, &requested, interval](const char *what) {
        QVERIFY2(view.item.m_playbackFrameRequests == requested + 1, what);
        QVERIFY2(view.dueInNs() < 0, what);
        // The frame drawn for it starts the capped frames again.
        view.frame();
        QVERIFY2(view.nextCappedFrameNs() == view.time.nowNs + interval, what);
        requested = view.item.m_playbackFrameRequests;
        view.time.advance(0.002);
    };
    clock.applySample(sampleAt(2.0, true, clock.state().trackToken), true);
    checkAtOnce("seek");
    // Repeat-One: the same track again from its start.
    clock.applySample(sampleAt(0.0, true, clock.state().trackToken), true);
    checkAtOnce("repeat one");
    clock.applySample(sampleAt(0.0, true, clock.state().trackToken + 1), false);
    checkAtOnce("gapless track change");
    view.item.setPlaying(false);
    QCOMPARE(view.dueInNs(), -1);
    clock.applySample(sampleAt(clock.positionAt(view.time.nowNs), false, clock.state().trackToken), false);
    QCOMPARE(view.item.m_playbackFrameRequests, requested + 1);
    view.frame();
    QCOMPARE(view.nextCappedFrameNs(), 0);
    view.item.setPlaying(true);
    requested = view.item.m_playbackFrameRequests;
    clock.applySample(sampleAt(clock.positionAt(view.time.nowNs), true, clock.state().trackToken), false);
    checkAtOnce("resume");
    // The window draws a zoom, a resize or an edit in its next frame,
    // whenever the capped frames are due; the capped frames follow it.
    const auto drawnAtOnce = [&view, interval](const char *what) {
        view.frame();
        QVERIFY2(view.capped(), what);
        QVERIFY2(view.nextCappedFrameNs() == view.time.nowNs + interval, what);
        view.time.advance(0.002);
    };
    view.item.setZoomLevel(70.0);
    drawnAtOnce("zoom");
    view.item.setWidth(900.0);
    drawnAtOnce("resize");
    view.item.setViewMode(1);
    drawnAtOnce("view mode");
    view.item.setMutedChannels({true});
    drawnAtOnce("mute");
}

void WaveformSceneGraphSmokeTest::waveformEditorFastScrollShowsArrivalsWithItsNextFrame() {
    // Capped, a playback tile or detail window that comes between frames
    // shows with the next capped frame, which draws the view again anyway,
    // rather than in a frame of its own; uncapped, it shows at once.
    constexpr int kRate = 1000;
    TestTime time;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(1000);
    item.setHeight(100);
    item.setDurationSeconds(10.0);
    item.m_refreshRateHz = 360.0;
    item.m_decodeWindow = [](const QString &, double start, double end, int points,
                             const std::shared_ptr<std::atomic_bool> &) {
        return sampleDetailWindow(kRate, 1, start, end, points);
    };
    setTestPosition(item, 5.0);
    item.setSourcePath(QStringLiteral("/fixture/fast.flac"));
    QTRY_COMPARE_WITH_TIMEOUT(item.sampleRateHz(), kRate, kSlowBuildTimeoutMs);
    item.setZoomLevel(60.0);
    const auto idle = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        return !item.m_decodeActive && !item.m_requestTimer.isActive()
            && item.detailCoversRangeLocked(start, end) && item.m_detail.framesPerPoint == 1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(idle(), kSlowBuildTimeoutMs);
    item.setPlaying(true);
    setTestPosition(item, 5.0);
    QImage canvas(1000, 100, QImage::Format_RGB32);
    const auto frame = [&item, &canvas]() {
        {
            QPainter painter(&canvas);
            item.paint(&painter);
        }
        item.handleWindowFrameSwapped();
    };
    const auto nextCapped = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.m_nextCappedFrameNs;
    };
    // The capped frame's millisecond, as the scheduler holds it.
    const auto dueMs = [](qint64 ns) { return (ns + 999'999) / 1'000'000 * 1'000'000; };
    frame();
    // The detail playback asks for ahead of the view, if any, has come.
    QTRY_VERIFY_WITH_TIMEOUT(idle(), kSlowBuildTimeoutMs);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_fastScrollCapped);
    }
    PlaybackFrameScheduler *scheduler = item.m_frameScheduler;
    QVERIFY(scheduler != nullptr);
    // With no frame asked for, as when nothing playback shows would change.
    time.advance(0.002);
    item.cancelPlaybackFrame();
    const quint64 requested = item.m_playbackFrameRequests;
    item.requestArrivalFrame();
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QCOMPARE(scheduler->dueNs(&item), dueMs(nextCapped()));
    // A frame already asked for by then shows it.
    item.requestArrivalFrame();
    QCOMPARE(scheduler->dueNs(&item), dueMs(nextCapped()));

    // The next capped frame far away, so that the scheduler does not wake
    // it while the test waits for what comes.
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_nextCappedFrameNs = time.nowNs + 60'000'000'000;
    }
    // A detail window for where playback goes, the same file's.
    item.cancelPlaybackFrame();
    const quint64 arrivals = item.m_detailArrivals;
    item.requestDetailWindow();
    QTRY_VERIFY_WITH_TIMEOUT(item.m_detailArrivals > arrivals, kSlowBuildTimeoutMs);
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QCOMPARE(scheduler->dueNs(&item), dueMs(nextCapped()));

    // A playback tile, from the workers.
    item.cancelPlaybackFrame();
    WaveformEditorItem::PlaybackTile tile;
    tile.strips.emplace_back(8, 8, QImage::Format_ARGB32_Premultiplied);
    quint64 generation = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        generation = item.m_playbackTileGeneration;
    }
    item.deliverPlaybackTile(generation, 0, std::move(tile));
    QTRY_COMPARE(scheduler->dueNs(&item), dueMs(nextCapped()));
    QCOMPARE(item.m_playbackFrameRequests, requested);

    // Uncapped, an arrival asks for no capped frame: it is drawn at once.
    item.setZoomLevel(20.0);
    frame();
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(!item.m_fastScrollCapped);
        QCOMPARE(item.cappedFrameDelayNsLocked(time.nowNs), 0);
    }
    item.cancelPlaybackFrame();
    item.requestArrivalFrame();
    QCOMPARE(scheduler->dueNs(&item), -1);
}

void WaveformSceneGraphSmokeTest::waveformEditorFastScrollFollowsTheScreensRefreshRate() {
    // The cap counts refreshes of the screen the window is on: moved to
    // another screen, or the screen's rate changing, it follows.
    QScreen *other = hiDpiTestScreen(2.0);
    if (other == nullptr) QSKIP("Needs the offscreen platform's HiDPI test screens");
    QQuickWindow window;
    window.resize(64, 64);
    auto rates = std::make_shared<QHash<QString, double>>();
    rates->insert(other->name(), 240.0);
    WaveformEditorItem item;
    item.m_screenRefreshRate = [rates](const QScreen &screen) {
        return rates->value(screen.name(), 360.0);
    };
    item.setParentItem(window.contentItem());
    const auto rate = [&item]() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.m_refreshRateHz;
    };
    QVERIFY(window.screen() != other);
    QCOMPARE(rate(), 360.0);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(moveWindowToScreen(window, other));
    QTRY_COMPARE(rate(), 240.0);
    QCOMPARE(WaveformEditorItem::fastScrollRefreshDivisor(rate()), 2);
    rates->insert(other->name(), 144.0);
    emit other->refreshRateChanged(144.0);
    QCOMPARE(rate(), 144.0);
    QCOMPARE(WaveformEditorItem::fastScrollRefreshDivisor(rate()), 1);
    // Out of the window, it counts none.
    item.setParentItem(nullptr);
    QCOMPARE(rate(), 0.0);
    emit other->refreshRateChanged(144.0);
    QCOMPARE(rate(), 0.0);
}

void WaveformSceneGraphSmokeTest::waveformSceneGraphSampleChunkClipKeepsItsSide() {
    // A frame that moves on by the view's width or more since the last
    // clips its chunks to the view; the frames after keep clipping them
    // until one moves on by less than 80% of the width, and then keep
    // building them whole until one moves on by the whole width again. A
    // view that moves on by about its width a frame does not switch from
    // frame to frame.
    QQuickWindow window;
    QVERIFY(showSampleWindow(window, 1.0));
    const QSize size(1720, 300);
    TestTime time;
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setVisible(false);
    time.drive(item);
    item.setWidth(size.width());
    item.setHeight(size.height());
    item.setDurationSeconds(60.0);
    item.setViewMode(1);
    item.setChannelCountHint(2);
    giveSampleDetail(item, 96'000, 2, 29.9, 30.1);
    item.setZoomLevel(item.maximumZoomLevel());
    setTestPosition(item, 30.0);
    item.setPlaying(true);
    setTestPosition(item, 30.0);
    double span = 0.0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        span = item.visibleSpanForZoomLocked(item.m_presentedZoomLevel);
    }
    struct Step {
        double views;
        bool clipped;
    };
    const Step steps[] = {
        {0.0, false}, {1.05, true}, {0.95, true}, {0.85, true}, {0.75, false},
        {0.95, false}, {0.85, false}, {1.05, true}, {0.7, false},
    };
    QSGNode *painted = nullptr;
    for (const Step &step : steps) {
        time.advance(step.views * span);
        bool clip = false;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const auto [start, end] = item.visibleRangeLocked();
            QVERIFY(item.renderDetailDirectlyLocked(start, end));
            clip = item.clipSampleChunksLocked(item.sampleChunkColumnLocked(start));
        }
        QVERIFY2(clip == step.clipped, qPrintable(QStringLiteral("%1 views").arg(step.views)));
        painted = WaveformSceneGraph::render(painted, &window, size,
            [&item](QPainter *painter) { item.paint(painter); }, false,
            WaveformSceneGraph::Primitives::Geometry);
        QVERIFY(painted != nullptr);
        {
            QMutexLocker lock(&item.m_stateMutex);
            QCOMPARE(item.m_sampleChunksClipped, step.clipped);
        }
        if (step.clipped) {
            const auto stats = WaveformSceneGraph::statistics(painted);
            QCOMPARE(stats.chunksBuilt, stats.chunks);
        }
    }
    delete painted;
}

void WaveformSceneGraphSmokeTest::waveformEditorIdleViewDrawsOnlyFramesThatDiffer() {
    QQuickWindow window;
    window.resize(1001, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int frames = 0;
    connect(&window, &QQuickWindow::frameSwapped, &window, [&frames]() { ++frames; });
    WaveformEditorItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(1001);
    item.setHeight(100);
    item.setDurationSeconds(250.0);
    item.setOverviewComplete(true);
    item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
    setTestPosition(item, 100.0);
    item.setPlaying(true);
    setTestPosition(item, 100.0);
    // The whole track in view: a pixel every 0.25 s.
    QTest::qWait(300);
    frames = 0;
    QTest::qWait(1000);
    QVERIFY2(frames >= 2 && frames <= 7, qPrintable(QString::number(frames)));
    const int idleFrames = frames;
    // Zoomed in, the view scrolls a pixel at a time, 32 a second, and
    // draws a frame for each, not one for every frame the window could.
    item.setZoomLevel(8.0);
    QTest::qWait(300);
    frames = 0;
    QTest::qWait(1000);
    QVERIFY2(frames >= 2 * std::max(idleFrames, 5) && frames <= 40,
        qPrintable(QString::number(frames)));
}

namespace {

// The frames of a window for a waveform editor that plays on the test's
// time: it draws a frame whenever the editor asks for one, then tells the
// editor that the frame is on screen, as the window's frameSwapped does.
struct WaveformFrameLoop {
    WaveformEditorItem &item;
    TestTime &time;
    double scale{1.0};
    QImage shown;
    quint64 seen{0};
    int frames{0};
    // Set when the editor kept asking for frames without end.
    bool runaway{false};

    WaveformFrameLoop(WaveformEditorItem &editor, TestTime &testTime)
        : item(editor), time(testTime) {
        scale = editor.window() != nullptr ? editor.window()->effectiveDevicePixelRatio() : 1.0;
        item.frameScheduler()->m_nowNs = [&testTime]() { return testTime.nowNs; };
        seen = item.m_playbackFrameRequests - 1;
    }

    QImage draw() const {
        const int width = static_cast<int>(std::floor(item.width()));
        const int height = static_cast<int>(std::floor(item.height()));
        QImage image(QSize(static_cast<int>(std::lround(width * scale)),
                         static_cast<int>(std::lround(height * scale))),
            QImage::Format_RGB32);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::black);
        QPainter painter(&image);
        item.paint(&painter);
        return image;
    }

    // Wakes the frame due by now, if any, and draws the frames asked for.
    void settle() {
        PlaybackFrameScheduler *scheduler = item.m_frameScheduler;
        if (scheduler != nullptr) {
            const qint64 due = scheduler->dueNs(&item);
            if (due >= 0 && due <= time.nowNs) {
                scheduler->fire();
            }
        }
        for (int i = 0; item.m_playbackFrameRequests != seen; ++i) {
            if (i >= 64) {
                runaway = true;
                return;
            }
            seen = item.m_playbackFrameRequests;
            shown = draw();
            ++frames;
            item.handleWindowFrameSwapped();
        }
    }

    // Plays `seconds` a millisecond at a time. Each frame must move the
    // view on by a device column, with the playhead on the view's middle
    // one, `middle`, and between frames the view must be the one shown.
    // Returns an error, or none.
    QString play(double seconds, int middle, int *framesDrawn = nullptr) {
        const int before = frames;
        QImage previous = shown;
        for (int ms = 1; ms <= std::lround(seconds * 1000.0); ++ms) {
            time.advance(0.001);
            const int drawn = frames;
            settle();
            if (runaway) {
                return QStringLiteral("frames without end at %1 ms").arg(ms);
            }
            if (frames > drawn + 1) {
                return QStringLiteral("%1 frames at %2 ms").arg(frames - drawn).arg(ms);
            }
            if (frames > drawn && middle >= 0 && !movedOneColumn(previous, shown, middle)) {
                return QStringLiteral("the frame at %1 ms did not move the view by a column").arg(ms);
            }
            previous = shown;
            if (draw() != shown) {
                return QStringLiteral("the view changed without a frame at %1 ms").arg(ms);
            }
        }
        if (framesDrawn != nullptr) {
            *framesDrawn = frames - before;
        }
        return QString();
    }

    // Whether `after` is `before` moved left by a device column, but for
    // the columns of the playhead, at `middle`, and next to it, and the
    // dashed centre line of the one channel, which stays with the view.
    bool movedOneColumn(const QImage &before, const QImage &after, int middle) const {
        const int margin = 3 * static_cast<int>(std::ceil(scale));
        const int centre = static_cast<int>(std::floor(item.height())) / 2;
        const int centreTop = static_cast<int>(std::floor(centre * scale));
        const int centreBottom = static_cast<int>(std::ceil((centre + 1) * scale));
        for (int x = 0; x + 1 < after.width(); ++x) {
            if (std::abs(x - middle) <= margin || std::abs(x + 1 - middle) <= margin) {
                continue;
            }
            for (int y = 0; y < after.height(); ++y) {
                if (y >= centreTop && y < centreBottom) {
                    continue;
                }
                if (after.pixel(x, y) != before.pixel(x + 1, y)) {
                    return false;
                }
            }
        }
        return before != after;
    }
};

// An overview that varies along the track, so that a view moved by a
// pixel differs.
QByteArray varyingOverview() {
    QByteArray overview(8192, Qt::Uninitialized);
    for (int i = 0; i < overview.size(); ++i) {
        overview[i] = static_cast<char>(128 + 100 * std::sin(i * 0.05));
    }
    return overview;
}

// Detail for a 250 s mono track at 44.1 kHz, 256 frames a point, which a
// 31.25 s view draws with playback tiles.
void giveWholeTrackDetail(WaveformEditorItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    auto &detail = item.m_detail;
    detail.sampleRateHz = 44'100;
    detail.channelCount = 1;
    detail.startSeconds = 0.0;
    detail.framesPerPoint = 256;
    detail.pointCount = 43'067;
    detail.endSeconds = detail.pointCount * 256.0 / 44'100.0;
    detail.extrema.resize(static_cast<std::size_t>(detail.pointCount) * 2);
    for (int point = 0; point < detail.pointCount; ++point) {
        const float peak = 0.5F + 0.4F * static_cast<float>(std::sin(point * 0.013));
        detail.extrema[static_cast<std::size_t>(point) * 2] = -peak;
        detail.extrema[static_cast<std::size_t>(point) * 2 + 1] = peak * 0.8F;
    }
    detail.downmixExtrema = detail.extrema;
    item.m_channelCount = 1;
    item.m_sampleRateHz = 44'100;
}

} // namespace

void WaveformSceneGraphSmokeTest::waveformEditorFollowingViewDrawsOnlyAtPixelSteps_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("tiles");
    QTest::newRow("1x overview") << 1.0 << 400 << false;
    QTest::newRow("1x odd width") << 1.0 << 401 << false;
    QTest::newRow("1x tiles") << 1.0 << 400 << true;
    QTest::newRow("1.25x tiles") << 1.25 << 400 << true;
    QTest::newRow("1.5x overview") << 1.5 << 400 << false;
    QTest::newRow("1.5x tiles") << 1.5 << 400 << true;
    QTest::newRow("2x tiles") << 2.0 << 400 << true;
    // 401 logical pixels end in a fraction of a device pixel at 1.25 and
    // 1.5: the raster's pixels are still the view's device columns.
    for (const double oddScale : {1.25, 1.5, 2.0}) {
        for (const bool oddTiles : {false, true}) {
            const QByteArray name = QByteArray::number(oddScale) + "x odd width "
                + (oddTiles ? "tiles" : "overview");
            QTest::newRow(name.constData()) << oddScale << 401 << oddTiles;
        }
    }
}

void WaveformSceneGraphSmokeTest::waveformEditorFollowingViewDrawsOnlyAtPixelSteps() {
    // A 250 s track zoomed in 8 times: 31.25 s over the view's device
    // columns, which follows the playhead from 100 s. It asks for a frame
    // only when the playhead reaches its next column; each frame moves the
    // view on by exactly a column, and the playhead stays on the view's
    // middle column. In between, a frame would be the same.
    QFETCH(double, scale);
    QFETCH(int, width);
    QFETCH(bool, tiles);
    TestTime time;
    time.nowNs = time.nowNs / 1'000'000 * 1'000'000;
    QQuickWindow window;
    std::unique_ptr<WaveformEditorItem> standalone;
    if (scale != 1.0) {
        if (!showOnScreen(window, scale)) {
            QSKIP("Needs the offscreen platform's HiDPI test screens");
        }
    } else {
        standalone = std::make_unique<WaveformEditorItem>();
    }
    WaveformEditorItem &item =
        standalone ? *standalone : *new WaveformEditorItem(window.contentItem());
    time.drive(item);
    item.setWidth(width);
    item.setHeight(100);
    item.setDurationSeconds(250.0);
    item.setOverviewComplete(true);
    item.setOverviewData(varyingOverview());
    item.setZoomLevel(8.0);
    setTestPosition(item, 100.0);
    item.setPlaying(true);
    setTestPosition(item, 100.0);
    if (tiles) {
        giveWholeTrackDetail(item);
        // Tiles made in the frame: a tile from a worker, which replaces the
        // envelope drawn in its place, changes the view between the steps.
        item.m_playbackTilesInline = true;
    }
    WaveformFrameLoop loop(item, time);
    loop.settle();
    QVERIFY(!loop.runaway);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const auto [start, end] = item.visibleRangeLocked();
        QCOMPARE(item.playbackTilesEligibleLocked(start, end), tiles);
    }
    const double deviceColumns = width * scale;
    const double column = 31.25 / deviceColumns;
    const int middle = static_cast<int>(std::floor(deviceColumns / 2.0));
    PlaybackClock &clock = testClock(item);
    const auto playheadColumn = [&]() {
        return static_cast<int>(std::floor(clock.positionAt(time.nowNs) / column + 1e-6));
    };
    const int firstColumn = playheadColumn();
    int frames = 0;
    const QString error = loop.play(1.0, middle, &frames);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // 12.8 to 25.6 columns a second: one frame for each.
    QCOMPARE(frames, playheadColumn() - firstColumn);
    QVERIFY(frames >= 12);

    // On a column's edge, the view is where one centred on the playhead
    // is, as before it moved a column at a time.
    const double onEdge = (std::floor(clock.positionAt(time.nowNs) / column) + 1.0 + 1e-4) * column;
    setTestPosition(item, onEdge);
    loop.settle();
    QMutexLocker lock(&item.m_stateMutex);
    const double start = item.visibleRangeLocked().first;
    const double centred = onEdge - 31.25 * (middle / deviceColumns);
    QVERIFY2(std::abs(start - centred) < column * 1e-3,
        qPrintable(QStringLiteral("start=%1 centred=%2").arg(start, 0, 'f', 9).arg(centred, 0, 'f', 9)));
}

void WaveformSceneGraphSmokeTest::waveformEditorFollowingViewShowsJumpsAndPausesAtOnce() {
    // 31.25 s over 400 pixels: a column every 78.125 ms. A course
    // correction only moves the next frame; a pause, a resume, a seek and a
    // track change, gapless or Repeat-One, show at once, and the frames
    // after them again come a column at a time.
    TestTime time;
    time.nowNs = time.nowNs / 1'000'000 * 1'000'000;
    WaveformEditorItem item;
    time.drive(item);
    item.setWidth(400);
    item.setHeight(100);
    item.setDurationSeconds(250.0);
    item.setOverviewComplete(true);
    item.setOverviewData(varyingOverview());
    item.setZoomLevel(8.0);
    setTestPosition(item, 100.0);
    item.setPlaying(true);
    setTestPosition(item, 100.0);
    WaveformFrameLoop loop(item, time);
    loop.settle();
    PlaybackClock &clock = testClock(item);
    constexpr double kColumn = 31.25 / 400.0;
    const auto playheadColumn = [&]() {
        return static_cast<int>(std::floor(clock.positionAt(time.nowNs) / kColumn + 1e-6));
    };
    // Plays on, checking that its frames come a column at a time.
    const auto playColumns = [&](double seconds, int middle) {
        const int first = playheadColumn();
        int frames = 0;
        const QString error = loop.play(seconds, middle, &frames);
        if (!error.isEmpty()) {
            return error;
        }
        const int columns = playheadColumn() - first;
        return frames == columns
            ? QString()
            : QStringLiteral("%1 frames for %2 columns").arg(frames).arg(columns);
    };
    const auto sample = [&](double position, bool advancing, quint64 token = 0) {
        PlaybackSample s;
        s.positionSeconds = position;
        s.sampledAtNs = time.nowNs;
        s.advancing = advancing;
        s.trackToken = token != 0 ? token : clock.state().trackToken;
        return s;
    };
    QString error = playColumns(0.3, 200);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A correction of 4 ms: no frame now, the next where the playhead's
    // column changes on the new course.
    const quint64 requested = item.m_playbackFrameRequests;
    clock.applySample(sample(clock.positionAt(time.nowNs) + 0.004, true), false);
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(item.m_frameScheduler->dueNs(&item) >= 0);
    error = playColumns(0.3, 200);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A pause shows at once, then nothing moves.
    int frames = loop.frames;
    item.setPlaying(false);
    clock.applySample(sample(clock.positionAt(time.nowNs), false), false);
    loop.settle();
    QVERIFY(loop.frames > frames);
    QCOMPARE(item.m_frameScheduler->dueNs(&item), qint64(-1));
    frames = loop.frames;
    error = loop.play(0.5, -1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loop.frames, frames);
    // A resume too, and playback goes on a column at a time.
    item.setPlaying(true);
    clock.applySample(sample(clock.positionAt(time.nowNs), true), false);
    loop.settle();
    QVERIFY(loop.frames > frames);
    error = playColumns(0.3, 200);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A seek.
    frames = loop.frames;
    clock.applySample(sample(30.0, true), true);
    loop.settle();
    QCOMPARE(loop.frames, frames + 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        const double start = item.visibleRangeLocked().first;
        QVERIFY(std::abs(start - (30.0 - 15.625)) <= kColumn);
    }
    error = playColumns(0.3, 200);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // A gapless successor: another track, from its start, where the view
    // holds still and the playhead moves a column at a time.
    frames = loop.frames;
    const quint64 nextTrack = clock.state().trackToken + 1;
    clock.applySample(sample(0.01, true, nextTrack), false);
    loop.settle();
    QCOMPARE(loop.frames, frames + 1);
    error = playColumns(0.3, -1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Repeat-One: the same track again, also from its start, here after
    // it has played on to the middle of the view.
    clock.applySample(sample(40.0, true), true);
    loop.settle();
    error = playColumns(0.2, 200);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    frames = loop.frames;
    clock.applySample(sample(0.0, true, nextTrack + 1), false);
    loop.settle();
    QCOMPARE(loop.frames, frames + 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.visibleRangeLocked().first, 0.0);
    }
    error = playColumns(0.3, -1);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void WaveformSceneGraphSmokeTest::waveformEditorInteractionDrawsAtOnceWhileFollowing() {
    // 10000 s over 400 pixels, zoomed in twice and following the playhead:
    // the next pixel is 25 s away, so any frame in the meantime is one an
    // interaction asked for. Each draws at once.
    QQuickWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int frames = 0;
    connect(&window, &QQuickWindow::frameSwapped, &window, [&frames]() { ++frames; });
    auto *item = new WaveformEditorItem(window.contentItem());
    item->setWidth(400);
    item->setHeight(100);
    item->setDurationSeconds(20'000.0);
    item->setOverviewComplete(true);
    item->setOverviewData(varyingOverview());
    item->setZoomLevel(2.0);
    item->setCrosshairEnabled(true);
    setTestPosition(*item, 10'000.0);
    item->setPlaying(true);
    setTestPosition(*item, 10'000.0);
    const auto settlesWithoutFrames = [&]() {
        QTest::qWait(200);
        frames = 0;
        QTest::qWait(300);
        return frames == 0;
    };
    const auto drawsAtOnce = [&](const std::function<void()> &interact) {
        QTest::qWait(100);
        frames = 0;
        interact();
        return QTest::qWaitFor([&frames]() { return frames > 0; }, 500);
    };
    QVERIFY(settlesWithoutFrames());
    QVERIFY(item->m_frameScheduler != nullptr);
    QCOMPARE(item->m_frameScheduler->parent(), &window);
    QVERIFY(item->m_frameScheduler->dueNs(item) >= 0);
    QVERIFY(drawsAtOnce([item]() { item->setHoverPosition(120.0, 40.0, true); }));
    QVERIFY(drawsAtOnce([item]() { item->setHoverPosition(121.0, 41.0, true); }));
    QVERIFY(drawsAtOnce([item]() { item->setHoverPosition(121.0, 41.0, false); }));
    QVERIFY(drawsAtOnce([item]() { item->setZoomLevel(4.0); }));
    QVERIFY(settlesWithoutFrames());
    QVERIFY(drawsAtOnce([item]() { setTestPositionAgo(*item, 5'000.0, 0.0); }));
    QVERIFY(drawsAtOnce([item]() { item->setWidth(380); }));
    QVERIFY(settlesWithoutFrames());

    // In another window, such as the fullscreen viewer, it waits on that
    // window's schedule.
    QQuickWindow viewer;
    viewer.resize(400, 100);
    viewer.show();
    QVERIFY(QTest::qWaitForWindowExposed(&viewer));
    PlaybackFrameScheduler *scheduler = item->m_frameScheduler;
    item->setParentItem(viewer.contentItem());
    QCOMPARE(scheduler->dueNs(item), qint64(-1));
    QTRY_VERIFY(item->m_frameScheduler != nullptr && item->m_frameScheduler->dueNs(item) >= 0);
    QCOMPARE(item->m_frameScheduler->parent(), &viewer);
    delete item;
    QCOMPARE(scheduler->dueNs(item), qint64(-1));
}

void WaveformSceneGraphSmokeTest::waveformEditorPlaybackBenchmark() {
    if (!qEnvironmentVariableIsSet("FERROUS_BENCHMARK_WAVEFORM")) QSKIP("optional render benchmark");
    // The frames a window draws in two seconds of playback of a four-minute
    // track, with the grid, over the whole track and zoomed in.
    QQuickWindow window;
    window.resize(1200, 180);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const double zoom : {1.0, 8.0}) {
        WaveformEditorItem item;
        item.setParentItem(window.contentItem());
        item.setWidth(1200);
        item.setHeight(180);
        item.setGridEnabled(true);
        item.setDurationSeconds(240.0);
        item.setOverviewComplete(true);
        item.setOverviewData(QByteArray(8192, static_cast<char>(128)));
        item.setZoomLevel(zoom);
        setTestPosition(item, 100.0);
        item.setPlaying(true);
        setTestPosition(item, 100.0);
        QTest::qWait(500);
        int frames = 0;
        const auto connection = connect(&window, &QQuickWindow::frameSwapped, &window,
            [&frames]() { ++frames; });
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::qWait(2000);
        disconnect(connection);
        qInfo("waveform playback zoom=%.0f scale=%g frames_per_s=%.1f", zoom,
            window.effectiveDevicePixelRatio(),
            frames / (static_cast<double>(elapsed.nsecsElapsed()) / 1e9));
        item.setPlaying(false);
        item.setParentItem(nullptr);
        QTest::qWait(100);
    }
}

void WaveformSceneGraphSmokeTest::waveformEditorUsesSceneGraphAndNativeFrameInterpolation() {
    WaveformEditorItem item;
    item.setWidth(320);
    item.setDurationSeconds(10.0);
    setTestPosition(item, 5.0);
    item.setZoomLevel(2.0);

    QVERIFY(item.flags().testFlag(QQuickItem::ItemHasContents));

    // The view moves with the clock between its samples.
    const double before = item.visibleRangeLocked().first;
    item.setPlaying(true);
    setTestPositionAgo(item, 5.0, 0.05);
    const double after = item.visibleRangeLocked().first;
    QVERIFY(after > before + 0.04);
}

std::unique_ptr<QmlSmokeTest> makeWaveformSceneGraphSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<WaveformSceneGraphSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_waveform_scene_graph.moc"
