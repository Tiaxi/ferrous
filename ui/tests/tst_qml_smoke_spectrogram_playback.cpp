// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Spectrogram seeking, track transitions, sample rates and playback
// frames.
class SpectrogramPlaybackSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void spectrogramMetadataOnlyResetWaitsForDataChunk();
    void spectrogramRollingSeekKeepsHistoryContinuous();
    void spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch();
    void spectrogramLargePositionJumpWaitsForResetHandoff();
    void spectrogramCenteredSmallSeekMovesAtOnce();
    void spectrogramCenteredSeekFollowsLocalVisualClock();
    void spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady();
    void visualizationsDrawTheSharedPlaybackClock();
    void spectrogramRollingGaplessContinuesWhereThePreviousTrackEnds_data();
    void spectrogramRollingGaplessContinuesWhereThePreviousTrackEnds();
    void spectrogramGaplessTrackChangePreservesRollingHistory();
    void spectrogramRollingRepeatOneDrawsTheReplay();
    void spectrogramNaturalTrackResetPreservesRollingHistory();
    void spectrogramManualTrackResetClearsRollingHistory();
    void spectrogramRollingZoomResetAnchorsEpochToResetStart();
    void spectrogramSeekResetAnchorsPlaybackClockToChunkStart();
    void spectrogramSeekProfileFlagsStalledPostSeekWindow();
    void spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling();
    void spectrogramSmoothnessProfileFlagsGapHeavyWindow();
    void spectrogramSmoothnessProfileTracksAdvanceFallbackSignals();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
    void spectrogramStaleTokenChunksAreDropped();
    void spectrogramGaplessTokenChunksPassFilter();
    void spectrogramEvictingOldTokenKeepsActiveTokenIteratorValid();
    void spectrogramFreshWidgetAcceptsDataWithImplicitReset();
    void spectrogramCenteredGaplessPreStagedFill();
    void spectrogramCenteredGaplessFollowsTheClock();
    void spectrogramCenteredSeekRestartRebuildsEarlierWindow();
    void spectrogramFullscreenRefillAfterSeekCoversLeftEdge();
    void spectrogramCenteredFinalizeChunkShrinksTotalEstimate();
    void spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate_data();
    void spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate();
    void spectrogramCenteredViewEndsAtTheSessionsEndBeforeDecodingIt();
    void spectrogramCenteredViewShowsTheWholeTrackAtNinetyPercentOfTheDecodedColumns_data();
    void spectrogramCenteredViewShowsTheWholeTrackAtNinetyPercentOfTheDecodedColumns();
    void spectrogramSeekFreezeEndsWithTheSession();
    void spectrogramCenteredFinalizeChunkIgnoredForStaleToken();
    void spectrogramCenteredClampsRightEdgeToMaxColNearEof();
    void spectrogramCenteredEofDetachmentDisablesSubpixelScrolling();
    void spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof();
    void spectrogramRollingGaplessTrackChangePreservesZoom();
    void spectrogramCenteredGaplessTrackChangeResetsZoom();
    void spectrogramRollingResetTrackChangeResetsZoom();
    void spectrogramTrackChangeMetadataResetClearsOldCenteredFrame();
    void spectrogramFreshInstanceResyncsBackendZoomOnTrackChange();
    void spectrogramFreshInstanceSeekRestartDoesNotResetZoom();
    void spectrogramTrackChangeCancelsPendingZoomDebounce();
    void spectrogramShortTrackDefaultZoomFillsTheView_data();
    void spectrogramShortTrackDefaultZoomFillsTheView();
    void spectrogramShortTracksLengthArrivingLaterMovesTheDefault();
    void spectrogramGaplessLongToShortTrackStretchesTheView_data();
    void spectrogramGaplessLongToShortTrackStretchesTheView();
    void spectrogramRepeatOneOfAStretchedTrackKeepsItsZoom();
    void spectrogramStretchSettingMovesTheDefaultWhilePlaying();
    void spectrogramKeptRollingZoomComesWithinTheLongTracksLimit_data();
    void spectrogramKeptRollingZoomComesWithinTheLongTracksLimit();
    void spectrogramRenderLoopStopsWhenNotPlaying();
    void spectrogramStillFramesLeaveTextureNodesAlone();
    void spectrogramIdleCenteredViewWaitsForThePlayheadsNextPixel_data();
    void spectrogramIdleCenteredViewWaitsForThePlayheadsNextPixel();
    void spectrogramLateFrameCallbackDrawsAtOnce();
    void spectrogramScrollingViewWaitsForTheNextPixel_data();
    void spectrogramScrollingViewWaitsForTheNextPixel();
    void spectrogramScrollingFramesMoveAPixelAtATime_data();
    void spectrogramScrollingFramesMoveAPixelAtATime();
    void spectrogramDataAheadOfTheShownFrameAsksForNoFrame();
    void spectrogramFrameSchedulerWakesDueFramesTogether();
    void spectrogramFrameSchedulerWakesEveryFrameDueByALateTimer();
    void spectrogramDecodedEndMovesTheCenteredViewAtOnce();
    void spectrogramPaintedCenteredViewFollowsASmallEndCorrection_data();
    void spectrogramPaintedCenteredViewFollowsASmallEndCorrection();
    void spectrogramPanesShareTheirWindowsFrameSchedule();
    void spectrogramPaneNoOneSeesAsksForNoPlaybackFrames();
    void spectrogramClockCourseCorrectionWaitsButJumpsShowAtOnce();
    void spectrogramIdleViewDrawsOnlyFramesThatDiffer();
    void spectrogramPlaybackBenchmark();
    void spectrogramSampleRateSyncsFromPrecomputedChunks();
    void spectrogramPreservesNativeRateAcrossModesAndTrackTransitions();
    void spectrogramPlaybackRateSwitchKeepsTheSessionRate();
    void spectrogramSessionRateChangeRemapsTheSpectrogram();
    void spectrogramSignalsWithoutItsStateLocked();
    void spectrogramCenteredViewMatchesTheDecodersModel();
    void spectrogramPlaybackTravelCountsOnlyContinuousPlayback();
    void spectrogramHighRatePlaybackAdvancesWithoutRebuilds_data();
    void spectrogramHighRatePlaybackAdvancesWithoutRebuilds();
    void spectrogramCenteredModeUsesWindowedCapacity();
    void spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead();
    void spectrogramTrackChangeToAHighRateKeepsZoomAtItsReferenceHop();
};

void SpectrogramPlaybackSmokeTest::spectrogramMetadataOnlyResetWaitsForDataChunk() {
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

void SpectrogramPlaybackSmokeTest::spectrogramRollingSeekKeepsHistoryContinuous() {
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
    setTestPosition(item, (8.0 * 1024.0) / 48'000.0);

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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredToRollingAtMaxZoomReanchorsEpoch() {
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
    setTestPosition(item, positionSeconds);

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

void SpectrogramPlaybackSmokeTest::spectrogramLargePositionJumpWaitsForResetHandoff() {
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
    setTestPosition(item, 1.0);
    item.setPlaying(true);

    // A seek far away: the rolling history keeps scrolling from where it
    // was until the worker's data for the target arrives.
    const double expectedSeconds = (512.0 * 1024.0) / 48000.0;
    setTestPosition(item, expectedSeconds);
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

    QVERIFY(!item.m_positionJumpHoldActive);
    // The playhead follows the clock again, which has advanced since.
    QVERIFY(item.m_positionSeconds >= expectedSeconds);
    QVERIFY(item.m_positionSeconds < expectedSeconds + 0.5);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredSmallSeekMovesAtOnce() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    setTestPosition(item, 10.0);
    item.setPlaying(true);

    testClock(item).holdAt(10.08);

    QVERIFY2(
        std::abs(item.m_positionSeconds - 10.08) < 0.0001,
        qPrintable(QStringLiteral(
            "centered small seek should publish target, got position %1")
            .arg(item.m_positionSeconds, 0, 'f', 4)));
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredSeekFollowsLocalVisualClock() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    item.m_precomputedReady = true;
    setTestPosition(item, 0.5);
    item.setPlaying(true);

    constexpr double seekTarget = 134.577;
    setTestPosition(item, seekTarget);
    setTestPosition(item, seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "centered seek should follow the visual clock, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
}

void SpectrogramPlaybackSmokeTest::spectrogramEarlyCenteredSeekFollowsLocalVisualClockBeforePrecomputedReady() {
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered

    setTestPosition(item, 0.6);
    item.setPlaying(true);

    constexpr double seekTarget = 117.079;
    setTestPosition(item, seekTarget);
    setTestPosition(item, seekTarget + 0.08);

    const double displayed =
        item.currentRenderPositionSecondsLocked(std::chrono::steady_clock::now());
    QVERIFY2(
        displayed > seekTarget + 0.06,
        qPrintable(QStringLiteral(
            "early centered seek should follow the visual clock before precomputed ready, target=%1 displayed=%2")
            .arg(seekTarget, 0, 'f', 3)
            .arg(displayed, 0, 'f', 3)));
}

void SpectrogramPlaybackSmokeTest::visualizationsDrawTheSharedPlaybackClock() {
    // Rolling and centered spectrograms and the waveform read one clock,
    // each at its own frame time, instead of extrapolating on their own.
    PlaybackClock clock;
    SpectrogramItem rolling;
    rolling.setDisplayMode(0);
    SpectrogramItem centered;
    centered.setDisplayMode(1);
    WaveformEditorItem waveform;
    waveform.setDurationSeconds(600.0);
    for (SpectrogramItem *item : {&rolling, &centered}) {
        item->setPlaying(true);
        item->setClock(&clock);
    }
    waveform.setPlaying(true);
    waveform.setClock(&clock);

    PlaybackSample sample;
    sample.positionSeconds = 20.0;
    sample.sampledAtNs = monotonicNowNs() - 1'000'000'000;
    sample.advancing = true;
    sample.durationSeconds = 600.0;
    sample.trackToken = 1;
    clock.applySample(sample, true);

    using std::chrono::nanoseconds;
    using std::chrono::steady_clock;
    for (const qint64 offset : {0LL, 16'666'667LL, 500'000'000LL}) {
        const qint64 frameNs = monotonicNowNs() + offset;
        const auto frame = steady_clock::time_point(nanoseconds(frameNs));
        const double expected = clock.positionAt(frameNs);
        QVERIFY(expected >= 21.0);
        QCOMPARE(rolling.currentRenderPositionSecondsLocked(frame), expected);
        QCOMPARE(centered.currentRenderPositionSecondsLocked(frame), expected);
    }
    const double before = clock.positionAt(monotonicNowNs());
    const double drawn = waveform.displayedPositionSecondsLocked();
    const double after = clock.positionAt(monotonicNowNs());
    QVERIFY(drawn >= before && drawn <= after);

    // A seek moves them together.
    clock.holdAt(300.0);
    QCOMPARE(rolling.positionSeconds(), 300.0);
    QCOMPARE(centered.positionSeconds(), 300.0);
    QCOMPARE(waveform.positionSeconds(), 300.0);
}

void SpectrogramPlaybackSmokeTest::spectrogramRollingGaplessContinuesWhereThePreviousTrackEnds_data() {
    QTest::addColumn<bool>("clockFirst");
    QTest::newRow("clock-first") << true;
    QTest::newRow("data-first") << false;
}

void SpectrogramPlaybackSmokeTest::spectrogramRollingGaplessContinuesWhereThePreviousTrackEnds() {
    // A gapless successor's columns follow the previous track's in the
    // rolling history. The engine reports the successor before the end of
    // the previous track has played, stamped with the time that track
    // ends; the playhead goes on with the previous track until then, and
    // shows the successor after it.
    QFETCH(bool, clockFirst);
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(true);
    constexpr int bins = 8;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 1024, 48'000, 1'024, false, true, 1, true);
    item.feedPrecomputedChunk(
        QByteArray(64 * bins, '\x20'), bins, 0, 64, 0, 1024, 48'000, 1'024, false, false, 1);
    item.m_precomputedReady = true;

    PlaybackClock &clock = testClock(item);
    const qint64 now = monotonicNowNs();
    PlaybackSample previous;
    previous.positionSeconds = 3.8;
    previous.sampledAtNs = now;
    previous.advancing = true;
    previous.durationSeconds = 4.0;
    previous.trackToken = 1;
    clock.applySample(previous, true);
    PlaybackSample successor = previous;
    successor.positionSeconds = 0.0;
    successor.sampledAtNs = now + 200'000'000;
    successor.durationSeconds = 4.5;
    successor.trackToken = 2;

    const auto feedSuccessor = [&]() {
        item.feedPrecomputedChunk(
            QByteArray(8 * bins, '\x60'), bins, 0, 8, 0, 1024, 48'000, 1'024, false, false, 2);
    };
    if (clockFirst) {
        clock.applySample(successor, false);
        // The history still shows the previous track.
        QVERIFY(item.m_positionJumpHoldActive);
        feedSuccessor();
    } else {
        feedSuccessor();
        QVERIFY(item.m_gaplessContinuationPending);
        clock.applySample(successor, false);
    }
    QVERIFY(!item.m_gaplessContinuationPending);
    QCOMPARE(item.m_gaplessPositionOffset, 4.0);
    QCOMPARE(item.m_positionJumpHoldUntilNs, successor.sampledAtNs);

    using std::chrono::nanoseconds;
    using std::chrono::steady_clock;
    const auto at = [](qint64 nanos) { return steady_clock::time_point(nanoseconds(nanos)); };
    QVERIFY(std::abs(item.currentRenderPositionSecondsLocked(at(now + 100'000'000)) - 3.9) < 1e-6);
    QVERIFY(std::abs(item.currentRenderPositionSecondsLocked(at(now + 199'000'000)) - 3.999) < 1e-6);
    QVERIFY(std::abs(item.currentRenderPositionSecondsLocked(at(now + 300'000'000)) - 4.1) < 1e-6);
}

void SpectrogramPlaybackSmokeTest::spectrogramRollingRepeatOneDrawsTheReplay() {
    // Repeat-One in a rolling pane: the worker goes on with the same file
    // after the end of its first play, on the session's continuous
    // timeline, and playback switches to the replay with a new token. The
    // replay's columns come after the first play's, and the pane draws them
    // as the replay plays.
    TestTime time;
    SpectrogramItem item;
    time.drive(item);
    item.setWidth(320);
    item.setHeight(100);
    item.setDisplayMode(0); // Rolling
    item.setPlaying(true);
    constexpr int bins = 8;
    constexpr int rate = 48'000;
    constexpr int hop = 1'024;
    constexpr double columnsPerSecond = static_cast<double>(rate) / hop;
    constexpr int fileColumns = 768;
    constexpr double duration = fileColumns / columnsPerSecond;
    constexpr int aheadColumns = 64;
    const auto columns = [](int count, char value) { return QByteArray(count * bins, value); };

    // The first play.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, fileColumns, rate, hop, false, true, 1, true, 1);
    item.feedPrecomputedChunk(columns(fileColumns, '\x30'), bins, 0, fileColumns, 0, fileColumns,
        rate, hop, false, false, 1, false, 1);
    item.m_precomputedReady = true;
    PlaybackClock &clock = testClock(item);
    PlaybackSample first;
    first.positionSeconds = duration - 0.3;
    first.sampledAtNs = time.nowNs;
    first.advancing = true;
    first.durationSeconds = duration;
    first.trackToken = 1;
    clock.applySample(first, true);
    // At its end the worker goes on with the file, decoding the replay's
    // first columns ahead of playback with the outgoing token.
    item.feedPrecomputedChunk(columns(aheadColumns, '\x60'), bins, 0, aheadColumns, fileColumns,
        2 * fileColumns, rate, hop, false, false, 1, false, 1);
    // Playback switches to the replay, stamped with the end of the first
    // play; the worker's token chunk and the rest of the replay follow.
    const qint64 replayStartNs = time.nowNs + 300'000'000;
    PlaybackSample replay = first;
    replay.positionSeconds = 0.0;
    replay.sampledAtNs = replayStartNs;
    replay.trackToken = 2;
    clock.applySample(replay, false);
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, fileColumns + aheadColumns,
        2 * fileColumns, rate, hop, false, false, 2, false, 1);
    item.feedPrecomputedChunk(columns(fileColumns - aheadColumns, '\x60'), bins, 0,
        fileColumns - aheadColumns, fileColumns + aheadColumns, 2 * fileColumns, rate, hop,
        true, false, 2, false, 1);
    QVERIFY(!item.m_gaplessContinuationPending);
    QCOMPARE(item.m_gaplessPositionOffset, duration);

    QSGNode *node = nullptr;
    for (const double into : {3.0, 8.0, 14.0}) {
        time.nowNs = replayStartNs + static_cast<qint64>(into * 1e9);
        qint64 shownSeq = 0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            const double position = item.currentRenderPositionSecondsLocked(item.m_now());
            QVERIFY2(std::abs(position - (duration + into)) < 1e-6, qPrintable(QString::number(position)));
            // The playhead's column is the replay's, `into` into the file.
            shownSeq = item.m_rollingEpoch
                + static_cast<qint64>(std::floor(position * columnsPerSecond));
            QVERIFY(shownSeq < item.m_ringWriteSeq);
            QVERIFY(shownSeq >= item.m_ringOldestSeq);
            const int slot = static_cast<int>(shownSeq % item.m_ringCapacity);
            QCOMPARE(item.m_ringSequenceId[slot], shownSeq);
            QCOMPARE(item.m_ringColumnId[slot],
                fileColumns + static_cast<int>(std::floor(into * columnsPerSecond)));
            QCOMPARE(item.m_ringTrackToken[slot], 2ULL);
        }
        // And the pane draws it, at its right edge, as the replay's.
        node = paintSettled(item, node);
        const QImage shown = canvasInViewOrder(item);
        QCOMPARE(shown.width(), 320);
        const QRgb replayColor = shown.pixel(shown.width() - 1, shown.height() / 2);
        QVERIFY(replayColor != kSpectrogramNoColumn);
        const QRgb leftColor = shown.pixel(0, shown.height() / 2);
        // Further back than the pane's width, the first play is gone from
        // view by 14 s; before that its end shows on the left.
        if (into * columnsPerSecond < 320.0) {
            QVERIFY(leftColor != replayColor);
        } else {
            QCOMPARE(leftColor, replayColor);
        }
    }
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramGaplessTrackChangePreservesRollingHistory() {
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

void SpectrogramPlaybackSmokeTest::spectrogramNaturalTrackResetPreservesRollingHistory() {
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

void SpectrogramPlaybackSmokeTest::spectrogramManualTrackResetClearsRollingHistory() {
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

void SpectrogramPlaybackSmokeTest::spectrogramRollingZoomResetAnchorsEpochToResetStart() {
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

    // The worker restarts the zoom at the playhead.
    setTestPosition(
        item,
        (static_cast<double>(resetStart) + 0.25) * newHop / static_cast<double>(sampleRate));
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
        item.m_positionSeconds * colsPerSecond));
    QCOMPARE(anchoredCol, static_cast<qint64>(resetStart));
    QCOMPARE(item.m_rollingEpoch + anchoredCol, static_cast<qint64>(0));
}

void SpectrogramPlaybackSmokeTest::spectrogramSeekResetAnchorsPlaybackClockToChunkStart() {
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

    // The clock does not show the seek yet: the playhead starts from the
    // data and waits for the clock there.
    const double expectedSeconds = (256.0 * 1024.0) / 48000.0;
    QVERIFY(item.m_positionJumpHoldActive);
    QVERIFY(std::abs(item.m_positionHoldTrajectory.anchorSeconds - expectedSeconds) < 0.0001);
    QVERIFY(std::abs(item.m_positionSeconds - expectedSeconds) < 0.0001);
    setTestPosition(item, expectedSeconds + 0.01);
    QVERIFY(!item.m_positionJumpHoldActive);
}

void SpectrogramPlaybackSmokeTest::spectrogramSeekProfileFlagsStalledPostSeekWindow() {
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

void SpectrogramPlaybackSmokeTest::spectrogramSeekProfileDoesNotRestartSameTraceAfterSettling() {
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

void SpectrogramPlaybackSmokeTest::spectrogramSmoothnessProfileFlagsGapHeavyWindow() {
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

void SpectrogramPlaybackSmokeTest::spectrogramSmoothnessProfileTracksAdvanceFallbackSignals() {
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
    }

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
        // A jump past the whole canvas goes to a worker.
        QVERIFY(!item.advancePrecomputedCanvasLocked(400, 410, false, item.m_now()));
        state = item.debugSmoothnessProfileStateLocked();
    }

    qunsetenv("FERROUS_PROFILE_UI");
    QVERIFY(!state.isEmpty());
    QCOMPARE(state.value("advanceFallbackFrames").toInt(), 1);
    QCOMPARE(state.value("nonUnityAdvanceFallbackFrames").toInt(), 1);
    QVERIFY(state.value("maxAdvanceFallbackZoomDelta").toDouble() > 0.9);
#else
    QSKIP("Smoothness profiling instrumentation is compiled out");
#endif
}

void SpectrogramPlaybackSmokeTest::stoppedTrackSwitchRequiresSpectrogramResetOnResume() {
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

void SpectrogramPlaybackSmokeTest::spectrogramStaleTokenChunksAreDropped() {
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

void SpectrogramPlaybackSmokeTest::spectrogramGaplessTokenChunksPassFilter() {
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

void SpectrogramPlaybackSmokeTest::spectrogramEvictingOldTokenKeepsActiveTokenIteratorValid() {
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

void SpectrogramPlaybackSmokeTest::spectrogramFreshWidgetAcceptsDataWithImplicitReset() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredGaplessPreStagedFill() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredSeekRestartRebuildsEarlierWindow() {
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
    item.setZoomLevel(16.0);

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

    setTestPosition(item, 3.554);
    QSGNode *node = paintSettled(item, nullptr);
    QVERIFY(node != nullptr);

    QMutexLocker lock(&item.m_stateMutex);
    const auto tokenMap = item.m_trackColumnToSeqByToken.value(token);
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayLeft)));
    QVERIFY(tokenMap.contains(static_cast<qint32>(item.m_precomputedCanvasDisplayRight)));
}

void SpectrogramPlaybackSmokeTest::spectrogramFullscreenRefillAfterSeekCoversLeftEdge() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int rate = 48000;
    constexpr int hop = 1024;
    constexpr int total = 14063;
    constexpr quint64 token = 1;
    const auto feed = [&](int start, int count, bool reset, quint64 generation) {
        item.feedPrecomputedChunk(QByteArray(count * bins, '\x50'), bins, 0,
            count, start, total, rate, hop, false, reset, token, reset, generation);
    };

    // First playback, followed by a seek to 120 s at the default 1920 px
    // retention floor. This session never decoded the future fullscreen left.
    feed(0, 1000, true, 1);
    testClock(item).holdAt(120.0);
    feed(4571, 2500, true, 2);
    QScopedPointer<QSGNode> node(paintSettled(item, nullptr));
    QVERIFY(node);
    item.setWidth(3840);
    paintSettled(item, node.data());
    const qint64 left = item.m_precomputedCanvasDisplayLeft;
    QVERIFY(left > 0);
    QVERIFY(left < 4571);
    QVERIFY(item.ringSlotForDisplayIndexLocked(left, false) < 0);

    // The backend requests the missing history through the existing seek
    // reset protocol. Queued old data must not defeat the refill.
    item.feedPrecomputedChunk(QByteArray(), 0, 0, 0, 0, 0,
        0, 0, false, true, token, true);
    feed(7071, 100, false, 2);
    QCOMPARE(item.m_ringWriteSeq, qint64(0));
    feed(3611, 0, true, 3);
    feed(3611, 5500, false, 3);
    const qint64 writeSeq = item.m_ringWriteSeq;
    feed(4571, 2500, true, 2);
    QCOMPARE(item.m_ringWriteSeq, writeSeq);
    QCOMPARE(item.m_precomputedSessionGeneration, quint64(3));
    paintSettled(item, node.data());

    QCOMPARE(item.m_precomputedCanvasDisplayLeft, left);
    QVERIFY(!item.m_zoomFillActive);
    for (qint64 col = item.m_precomputedCanvasDisplayLeft;
         col <= item.m_precomputedCanvasDisplayRight; ++col) {
        QVERIFY(item.ringSlotForDisplayIndexLocked(col, false) >= 0);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramSeekFreezeEndsWithTheSession() {
    // A seek past the end of the audio, whose estimate overshot: the session
    // decodes the view up to the end, and the playhead's column, which a
    // seek's freeze waits for, never comes. The end of the session, sent
    // once all its columns are, must end the freeze, or the image of the
    // position before the seek stays on screen while playback goes on.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int estimate = 5000;
    constexpr int end = 4900;
    constexpr quint64 token = 7;
    item.feedPrecomputedChunk(QByteArray(640 * bins, '\x30'), bins, 0, 640,
        0, estimate, 44100, 1024, false, true, token, true, 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(180);
        rebuildCanvasNow(item, 320, 180, 0, 319, false);
    }
    QVERIFY(!item.m_canvas.isNull());
    testClock(item).holdAt(4950.0 * 1024.0 / 44100.0);

    item.feedPrecomputedChunk({}, 0, 0, 0, 0, 0, 0, 0,
        false, true, token, true, 2);
    QVERIFY(item.m_zoomFillActive);
    QVERIFY(item.m_seekFillActive);
    item.feedPrecomputedChunk({}, bins, 0, 0, 4700, estimate, 44100, 1024,
        false, true, token, true, 2);
    item.feedPrecomputedChunk(QByteArray(200 * bins, '\x60'), bins, 0, 200,
        4700, estimate, 44100, 1024, false, false, token, false, 2);
    QVERIFY2(item.m_zoomFillActive, "the playhead's column may still arrive");

    // Another session's end says nothing of this one's.
    item.feedPrecomputedChunk({}, bins, 0, 0, end, end, 44100, 1024,
        true, false, token - 1, false, 2);
    QVERIFY(item.m_zoomFillActive);

    // Nothing arrives after this session's end.
    item.feedPrecomputedChunk({}, bins, 0, 0, end, end, 44100, 1024,
        true, false, token, false, 2);
    QVERIFY2(!item.m_zoomFillActive, "nothing more arrives to end the freeze");
    QVERIFY(!item.m_seekFillActive);
    QVERIFY(item.m_precomputedCanvasDirty);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, end);

    // A zoom waits for the session of its own hop.
    item.m_zoomFillActive = true;
    item.m_awaitingZoomData = true;
    item.feedPrecomputedChunk({}, bins, 0, 0, end, end, 44100, 1024,
        true, false, token, false, 2);
    QVERIFY(item.m_zoomFillActive);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredFinalizeChunkShrinksTotalEstimate() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate_data() {
    // The column estimate a session sends until it has decoded the end of
    // a 120 s file at 8 kHz, in columns of 171 frames (the reference hop
    // at 8 kHz): the file's length in columns of 1024 frames, or the 300 s
    // the decoder assumes for a file that does not tell it, and 64 more,
    // 8.2 s, which only size buffers.
    const auto estimate = [](qint64 frames) {
        return static_cast<int>(std::ceil(
            static_cast<double>(frames / 1024 + 64) * 1024.0 / 171.0));
    };
    QTest::addColumn<int>("estimate");
    QTest::newRow("known length") << estimate(120 * 8'000);
    QTest::newRow("unknown length") << estimate(300 * 8'000);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate() {
    // The decoder places a centered view, its seeks and restarts, by the
    // columns a session decodes of the track (issue 421), not by its
    // column estimate, which runs past the end. The view here keeps
    // bounding itself
    // by the estimate until the session has decoded the end, and then
    // shows the whole track once its window covers 90% of the columns
    // decoded, and ends where the decode did, a window before it, once the
    // rest of the track is shorter than half a window: the full-track lock
    // and the end of the view come from the decoded end, for files that
    // tell their length and for files that do not.
    QFETCH(int, estimate);
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(100);
    item.setDisplayMode(1);

    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int hop = 171;
    constexpr double columnsPerSecond = static_cast<double>(rate) / hop;
    // The columns of 2048-sample windows centered on each column that fit
    // in 120 s.
    constexpr int decodedEnd = (120 * rate - 1'024) / hop + 1;
    QCOMPARE(decodedEnd, 5'609);
    QVERIFY(estimate > decodedEnd);
    constexpr int narrowWindow = 1'200;
    // 91% of the columns decoded, and less of the estimate.
    constexpr int wideWindow = 5'100;
    QVERIFY(static_cast<qint64>(wideWindow) * 100 / estimate < 90);
    QVERIFY(static_cast<qint64>(wideWindow) * 100 / decodedEnd >= 90);
    // 3 s before the end, and 60 s, where the wide window's right edge is
    // short of the end.
    constexpr double nearEnd = 117.0;
    constexpr double midTrack = 60.0;
    const auto column = [&](double seconds) {
        return static_cast<qint64>(std::floor(seconds * columnsPerSecond));
    };
    QVERIFY(column(midTrack) + wideWindow / 2 < decodedEnd - 1);
    // The view of a pane as many pixels wide as the window's columns, at
    // the canvas width and zoom the paint reckons it with.
    const auto view = [&](double seconds, int window) {
        item.setWidth(window);
        QMutexLocker lock(&item.m_stateMutex);
        return item.centeredViewLocked(seconds, item.canvasWidthLocked(),
            item.canvasHeightLocked(), item.canvasZoomLocked(), columnsPerSecond);
    };

    // Every column is in, but the session has yet to say it reached the
    // end: the chunks carry the estimate.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, estimate, rate, hop, false, true, 1);
    item.feedPrecomputedChunk(
        QByteArray(decodedEnd * bins, '\x40'), bins, 0, decodedEnd, 0, estimate,
        rate, hop, false, false, 1);
    {
        const SpectrogramItem::CenteredView narrow = view(nearEnd, narrowWindow);
        QVERIFY(!narrow.decodedTailLooksFinal);
        const qint64 right = std::min<qint64>(
            estimate - 1, column(nearEnd) - narrowWindow / 2 + narrowWindow - 1);
        QCOMPARE(narrow.right, right);
        QCOMPARE(narrow.left, right - narrowWindow + 1);
        const SpectrogramItem::CenteredView wide = view(midTrack, wideWindow);
        QVERIFY(!wide.decodedTailLooksFinal);
        QCOMPARE(wide.left, column(midTrack) - wideWindow / 2);
    }

    // The session's last chunk carries the decoded end.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, decodedEnd, decodedEnd, rate, hop,
        /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, decodedEnd);
    const SpectrogramItem::CenteredView narrow = view(nearEnd, narrowWindow);
    QVERIFY(narrow.decodedTailLooksFinal);
    QCOMPARE(narrow.right, qint64(decodedEnd - 1));
    QCOMPARE(narrow.left, qint64(decodedEnd - narrowWindow));
    const SpectrogramItem::CenteredView wide = view(midTrack, wideWindow);
    QCOMPARE(wide.left, qint64(0));
    QCOMPARE(wide.right, qint64(decodedEnd - 1));
    // Near the end, the whole track's view ends there too.
    const SpectrogramItem::CenteredView wideAtEnd = view(nearEnd, wideWindow);
    QCOMPARE(wideAtEnd.right, qint64(decodedEnd - 1));
    QCOMPARE(wideAtEnd.left, qint64(decodedEnd - wideWindow));

    // The decoder reckons the view by the same end, the same window, and
    // the same whole columns: near the end its left edge is the view's, and
    // mid-track it shows the whole track as the view does. By the
    // estimate, which runs 8.2 s past the end, a decode started right of
    // the view's left edge, 2 s of warm-up and all, and did not show the
    // whole track.
    QCOMPARE(qint64(decodedEnd - narrowWindow), narrow.left);
    QVERIFY(static_cast<qint64>(wideWindow) * 100 / decodedEnd >= 90);
    constexpr double warmUp = 2.0;
    const double estimatedEnd = 120.0 + 65'536.0 / rate;
    QVERIFY(column(estimatedEnd - narrowWindow / columnsPerSecond - warmUp) > narrow.left);
    QVERIFY(wideWindow / columnsPerSecond < 0.9 * estimatedEnd);
    QVERIFY(column(midTrack - wideWindow / columnsPerSecond / 2.0 - warmUp) > wide.left);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewEndsAtTheSessionsEndBeforeDecodingIt() {
    // A session that knows the file's length sends, with every chunk, the
    // end the decoder places its views by (issue 436). The view ends there
    // from the first chunk, not at the column estimate: near the end the
    // playhead leaves the middle, and the view shows the whole track at 90%
    // of those columns. Where the decoding has not reached, the view is as
    // it would be with every column in: the end-of-decode clamp is for the
    // estimate's overshoot, not for a known end.
    SpectrogramItem item;
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int hop = 171;
    constexpr double columnsPerSecond = static_cast<double>(rate) / hop;
    // 120 s: see spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate.
    constexpr int end = 5'609;
    constexpr int estimate = (120 * rate / 1'024 + 64) * 1'024 / hop + 1;
    QVERIFY(estimate > end + 300);
    const auto column = [](double seconds) {
        return static_cast<qint64>(std::floor(seconds * columnsPerSecond));
    };
    const auto view = [&](double seconds, int window) {
        item.setWidth(window);
        QMutexLocker lock(&item.m_stateMutex);
        return item.centeredViewLocked(seconds, item.canvasWidthLocked(),
            item.canvasHeightLocked(), item.canvasZoomLocked(), columnsPerSecond);
    };
    // The session decoded 60 s to 110 s so far.
    const int first = static_cast<int>(column(60.0));
    const int last = static_cast<int>(column(110.0));
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, first, estimate, rate, hop, false, true, 1, false, 1, 0,
        2'048, 120 * rate, end);
    item.feedPrecomputedChunk(
        QByteArray((last - first) * bins, '\x40'), bins, 0, last - first, first, estimate,
        rate, hop, false, false, 1, false, 1, 0, 2'048, 120 * rate, end);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedEndColumn, end);
        QCOMPARE(item.m_precomputedTotalColumnsEstimate, estimate);
    }

    // 3 s before the end: the view ends there, a window of 1200 columns
    // before it, and does not scroll; by the estimate it started further
    // right, past the end.
    const SpectrogramItem::CenteredView nearEnd = view(117.0, 1'200);
    QCOMPARE(nearEnd.right, qint64(end - 1));
    QCOMPARE(nearEnd.left, qint64(end - 1'200));
    QVERIFY(!nearEnd.scrolls);
    QVERIFY(std::min<qint64>(estimate - 1, column(117.0) + 599) - 1'199 > nearEnd.left);
    // Mid-track, centered: the decoded tail, at 110 s, does not pull the
    // right edge in.
    const SpectrogramItem::CenteredView midTrack = view(105.0, 1'200);
    QCOMPARE(midTrack.left, column(105.0) - 600);
    QCOMPARE(midTrack.right, column(105.0) + 599);
    QVERIFY(midTrack.right > last);
    QVERIFY(midTrack.scrolls);
    // 5100 columns are 91% of the end's and less of the estimate's.
    const SpectrogramItem::CenteredView whole = view(60.0, 5'100);
    QCOMPARE(whole.left, qint64(0));
    QCOMPARE(whole.right, qint64(end - 1));

    // The end the session reached replaces the one it reckoned.
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, end - 2, end - 2, rate, hop,
        /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false, 1, 0,
        2'048, 120 * rate, end - 2);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedEndColumn, end - 2);
    }
    QCOMPARE(view(117.0, 1'200).right, qint64(end - 3));
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewShowsTheWholeTrackAtNinetyPercentOfTheDecodedColumns_data() {
    // Files at 8 kHz decoded with 2048-sample windows, and views of them as
    // wide as a pane's pixels, at a device pixel ratio of 1. The decoder's
    // model of the view is tested on the same cases (src/analysis/
    // centered_view.rs and session.rs).
    QTest::addColumn<double>("zoom");
    QTest::addColumn<int>("hop");
    QTest::addColumn<int>("frames");
    QTest::addColumn<int>("decodedEnd");
    QTest::addColumn<int>("width");
    QTest::addColumn<double>("position");
    QTest::addColumn<qint64>("window");
    QTest::addColumn<qint64>("left");
    // 40 s at zoom 1, in 1866 columns of 171 frames: 1680 of them are
    // 90.03%, which the view shows the whole track at, though they are
    // 35.91 s, less than 90% of the 40 s. Centered on column 1024 at 21.9 s.
    QTest::newRow("zoom 1, 89.98%")
        << 1.0 << 171 << 320'000 << 1'866 << 1'679 << 21.9 << qint64(1'679)
        << qint64(1'024 - 1'679 / 2);
    QTest::newRow("zoom 1, 90.03%")
        << 1.0 << 171 << 320'000 << 1'866 << 1'680 << 21.9 << qint64(1'680) << qint64(0);
    QTest::newRow("zoom 1, 90.08%")
        << 1.0 << 171 << 320'000 << 1'866 << 1'681 << 21.9 << qint64(1'681) << qint64(0);
    // 70.08 s at zoom 0.57, in 1867 columns of 300 frames, which the view
    // draws at 171 / 300 × 300 / 171, a bit below 1: 1680 pixels show 1681
    // columns, 90.04%, and 1679 pixels 1680, 89.98%. Centered on column
    // 1024 at 38.4 s.
    QTest::newRow("zoom 0.57, 89.98%")
        << 0.57 << 300 << 560'640 << 1'867 << 1'679 << 38.4 << qint64(1'680)
        << qint64(1'024 - 1'680 / 2);
    QTest::newRow("zoom 0.57, 90.04%")
        << 0.57 << 300 << 560'640 << 1'867 << 1'680 << 38.4 << qint64(1'681) << qint64(0);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewShowsTheWholeTrackAtNinetyPercentOfTheDecodedColumns() {
    // The decoder starts a seek's decode at the view's left edge by the
    // same columns, the same zoom (ferrous_ffi_spectrogram_effective_zoom)
    // and the same integer share (issue 421): placed by seconds, the
    // 90.03% view started its decode at 1.95 s and left columns 0 to 89
    // black; placed by a column a pixel, the 90.04% one left 0 to 129.
    QFETCH(double, zoom);
    QFETCH(int, hop);
    QFETCH(int, frames);
    QFETCH(int, decodedEnd);
    QFETCH(int, width);
    QFETCH(double, position);
    QFETCH(qint64, window);
    QFETCH(qint64, left);
    SpectrogramItem item;
    item.setWidth(width);
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int referenceHop = 171;
    // The column estimate a session sends: the file in columns of 1024
    // frames, and 64 more, in its own columns.
    const auto estimateAt = [frames](int columnHop) {
        return static_cast<int>(
            std::ceil((frames / 1024 + 64) * 1024.0 / static_cast<double>(columnHop)));
    };
    if (hop != referenceHop) {
        // Zoomed from a view at zoom 1, as playback zooms: the backend
        // restarts at the zoom's hop, and the view draws its data at the
        // zoom the backend decodes at.
        item.feedPrecomputedChunk(
            QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, estimateAt(referenceHop),
            rate, referenceHop, false, true, 1);
        item.setZoomLevel(zoom);
        item.m_zoomDebounceTimer->stop();
        QVERIFY(item.m_awaitingZoomData);
    }
    const int estimate = estimateAt(hop);
    QVERIFY(estimate > decodedEnd);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, estimate, rate, hop, false, true, 1);
    item.feedPrecomputedChunk(
        QByteArray(decodedEnd * bins, '\x40'), bins, 0, decodedEnd, 0, estimate,
        rate, hop, false, false, 1);
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, decodedEnd, decodedEnd, rate, hop,
        /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false);
    QMutexLocker lock(&item.m_stateMutex);
    QCOMPARE(item.m_precomputedHopSize, hop);
    QCOMPARE(item.m_precomputedTotalColumnsEstimate, decodedEnd);
    const double canvasZoom = item.canvasZoomLocked();
    const int canvasWidth = item.canvasWidthLocked();
    QCOMPARE(canvasWidth, width);
    if (hop != referenceHop) {
        QVERIFY(!item.m_awaitingZoomData);
        QCOMPARE(item.m_renderZoomLevel, static_cast<double>(referenceHop) / hop);
        QVERIFY(canvasZoom < 1.0);
    }
    QCOMPARE(static_cast<qint64>(std::ceil(static_cast<double>(canvasWidth) / canvasZoom)),
        window);
    const double columnsPerSecond = static_cast<double>(rate) / hop;
    const SpectrogramItem::CenteredView view = item.centeredViewLocked(
        position, canvasWidth, item.canvasHeightLocked(), canvasZoom, columnsPerSecond);
    QCOMPARE(view.left, left);
    QCOMPARE(view.right, left == 0 ? qint64(decodedEnd - 1) : left + window - 1);
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredFinalizeChunkIgnoredForStaleToken() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredClampsRightEdgeToMaxColNearEof() {
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
    setTestPosition(item, 11.25);

    QSGNode *node = paintSettled(item, nullptr);
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredEofDetachmentDisablesSubpixelScrolling() {
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

    setTestPosition(item, 11.25);

    QSGNode *node = paintSettled(item, nullptr);
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredDisplayRangeIgnoresLaggingDecodedTailBeforeEof() {
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
    setTestPosition(item, 103.0);

    QSGNode *node = paintSettled(item, nullptr);
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

void SpectrogramPlaybackSmokeTest::spectrogramRollingGaplessTrackChangePreservesZoom() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredGaplessTrackChangeResetsZoom() {
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

void SpectrogramPlaybackSmokeTest::spectrogramRollingResetTrackChangeResetsZoom() {
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

void SpectrogramPlaybackSmokeTest::spectrogramTrackChangeMetadataResetClearsOldCenteredFrame() {
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
    setTestPosition(item, 72.0);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.ensureMapping(180);
        rebuildCanvasNow(item, 320, 180, 0, 319, false);
    }
    QVERIFY(item.m_precomputedReady);
    QVERIFY(!item.m_canvas.isNull());
    QCOMPARE(item.m_precomputedCanvasDisplayRight, static_cast<qint64>(319));
    QVERIFY(item.m_positionSeconds > 70.0);

    // The clock reports the new track, then its metadata reset arrives.
    setTestPosition(item, 0.0, newToken);
    QVERIFY(item.m_positionSeconds < 0.01);
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
    QVERIFY(item.m_positionSeconds < 0.01);
}

void SpectrogramPlaybackSmokeTest::spectrogramFreshInstanceResyncsBackendZoomOnTrackChange() {
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

void SpectrogramPlaybackSmokeTest::spectrogramFreshInstanceSeekRestartDoesNotResetZoom() {
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

void SpectrogramPlaybackSmokeTest::spectrogramTrackChangeCancelsPendingZoomDebounce() {
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

void SpectrogramPlaybackSmokeTest::spectrogramCenteredGaplessFollowsTheClock() {
    // In centered mode, the playhead moves to the start of a gapless
    // successor as soon as the clock reports it, and stays with the clock
    // when the successor's data arrives.
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1); // Centered
    item.setPlaying(true);

    constexpr int bins = 8;
    constexpr int total = 1024;
    constexpr quint64 oldToken = 5;
    constexpr quint64 newToken = 6;

    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, total, 48000, 1024, false, true, oldToken);
    QByteArray oldData(100 * bins, '\x10');
    item.feedPrecomputedChunk(
        oldData, bins, 0, 100, 0, total, 48000, 1024, false, false, oldToken);
    setTestPosition(item, 428.0, oldToken);
    QVERIFY(std::abs(item.m_positionSeconds - 428.0) < 0.1);

    setTestPosition(item, 0.04, newToken);
    QVERIFY(!item.m_positionJumpHoldActive);
    QVERIFY(item.m_positionSeconds < 0.1);

    QByteArray newData(50 * bins, '\x40');
    item.feedPrecomputedChunk(
        newData, bins, 0, 50, 0, total, 48000, 1024, false, false, newToken);
    QCOMPARE(item.m_precomputedTrackToken, newToken);
    QVERIFY(!item.m_positionJumpHoldActive);
    QCOMPARE(item.m_gaplessPositionOffset, 0.0);

    setTestPosition(item, 0.08, newToken);
    QVERIFY(!item.m_positionJumpHoldActive);
    QVERIFY(item.m_positionSeconds < 1.0);
}

namespace {

// A file at 48 kHz with 8192-sample windows, as the backend's sessions
// decode it: 1 s is a short track, a minute a long one.
constexpr int kStretchRate = 48'000;
constexpr int kStretchFft = 8'192;
constexpr int kStretchBins = 4;
constexpr quint64 kShortFrames = 48'000;
constexpr quint64 kLongFrames = 60 * 48'000;

// The column after the last that a session at `zoom` decodes of the file.
int stretchEndColumn(quint64 frames, double zoom) {
    return static_cast<int>(ferrous_ffi_spectrogram_decoded_end_column(
        frames, kStretchFft, static_cast<float>(zoom), kStretchRate));
}

int stretchHop(double zoom) {
    return static_cast<int>(ferrous_ffi_spectrogram_column_hop(
        kStretchRate, static_cast<float>(zoom)));
}

// A session's start of the track `token` at `zoom`: its reset, then
// `columns` columns of it (all of them if negative). `frames` is what the
// session tells of the file: 0 for a length it does not know.
void feedStretchSession(
    SpectrogramItem &item, quint64 token, double zoom, quint64 frames,
    quint64 trackFrames, int columns = -1) {
    const int hop = stretchHop(zoom);
    const int end = stretchEndColumn(frames, zoom);
    const int count = columns < 0 ? end : std::min(columns, end);
    const int knownEnd = trackFrames > 0 ? end : 0;
    item.feedPrecomputedChunk(
        QByteArray(), kStretchBins, 0, 0, 0, end, kStretchRate, hop, false, true, token, true,
        0, 0, kStretchFft, trackFrames, knownEnd);
    item.feedPrecomputedChunk(
        QByteArray(count * kStretchBins, '\x40'), kStretchBins, 0, count, 0, end, kStretchRate,
        hop, count == end, false, token, false, 0, 0, kStretchFft, trackFrames, knownEnd);
}

// Pixels the track's columns take at the zoom the view draws them at.
double trackPixels(SpectrogramItem &item, quint64 frames) {
    QMutexLocker lock(&item.m_stateMutex);
    return stretchEndColumn(frames, item.m_zoomLevel) * item.effectiveZoomLocked();
}

} // namespace

void SpectrogramPlaybackSmokeTest::spectrogramShortTrackDefaultZoomFillsTheView_data() {
    QTest::addColumn<int>("displayMode");
    QTest::addColumn<bool>("stretch");
    QTest::addColumn<bool>("shortTrack");
    for (const int mode : {0, 1}) {
        for (const bool stretch : {true, false}) {
            for (const bool shortTrack : {true, false}) {
                QTest::addRow("%s, %s, %s", mode == 0 ? "rolling" : "centered",
                              stretch ? "stretched" : "not stretched",
                              shortTrack ? "1 s" : "a minute")
                    << mode << stretch << shortTrack;
            }
        }
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramShortTrackDefaultZoomFillsTheView() {
    QFETCH(int, displayMode);
    QFETCH(bool, stretch);
    QFETCH(bool, shortTrack);
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(200);
    item.setDisplayMode(displayMode);
    item.setStretchShortTracks(stretch);
    QSignalSpy backendZoom(&item, &SpectrogramItem::backendZoomRequested);
    QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);
    const quint64 frames = shortTrack ? kShortFrames : kLongFrames;

    // The backend starts the track at zoom 1.
    feedStretchSession(item, 5, 1.0, frames, frames, 32);

    if (!stretch || !shortTrack) {
        // Today's scale: zoom 1, a column a pixel at the reference hop.
        QCOMPARE(item.defaultZoomLevel(), 1.0);
        QCOMPARE(item.zoomLevel(), 1.0);
        QCOMPARE(item.maximumZoomLevel(), 16.0);
        QCOMPARE(backendZoom.count(), 0);
        QCOMPARE(zoomReset.count(), 0);
        if (shortTrack) {
            // A sliver of the view: 43 columns of 1000 pixels.
            QCOMPARE(trackPixels(item, frames), 43.0);
        } else {
            QVERIFY(trackPixels(item, frames) > 1000.0);
        }
        return;
    }

    // The default zoom is the one at which the second fills the view, and
    // the view goes there at once, asking the backend for it.
    const double fit = ferrous_ffi_spectrogram_fit_zoom(kShortFrames, kStretchFft, kStretchRate, 1000);
    QCOMPARE(stretchHop(fit), 44);
    QCOMPARE(item.defaultZoomLevel(), fit);
    QCOMPARE(item.zoomLevel(), fit);
    QCOMPARE(zoomReset.count(), 1);
    QCOMPARE(backendZoom.count(), 1);
    QCOMPARE(backendZoom.at(0).at(0).toFloat(), static_cast<float>(fit));
    // The wheel zooms in no further than the default.
    QCOMPARE(item.maximumZoomLevel(), fit);

    // The backend's session at the default zoom: the track's 998 columns,
    // a pixel each, fill the 1000-pixel view.
    feedStretchSession(item, 5, fit, frames, frames);
    QCOMPARE(backendZoom.count(), 1);
    QCOMPARE(zoomReset.count(), 1);
    QCOMPARE(item.zoomLevel(), fit);
    const double pixels = trackPixels(item, frames);
    QVERIFY2(pixels >= 995.0 && pixels <= 1000.0, qPrintable(QString::number(pixels)));
    if (displayMode == 1) {
        // The whole track is in view wherever the playhead is.
        QMutexLocker lock(&item.m_stateMutex);
        for (const double position : {0.0, 0.5, 0.9}) {
            const auto view = item.centeredViewLocked(
                position, item.canvasWidthLocked(), item.canvasHeightLocked(),
                item.canvasZoomLocked(), static_cast<double>(kStretchRate) / 44.0);
            QCOMPARE(view.left, qint64(0));
            QVERIFY2(view.right >= qint64(997), qPrintable(QString::number(view.right)));
        }
    }

    // A track that fills the view gets zoom 1 again.
    backendZoom.clear();
    feedStretchSession(item, 6, fit, kLongFrames, kLongFrames, 32);
    QCOMPARE(item.defaultZoomLevel(), 1.0);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(backendZoom.count(), 1);
    QCOMPARE(backendZoom.at(0).at(0).toFloat(), 1.0f);
    QCOMPARE(item.maximumZoomLevel(), 16.0);
}

void SpectrogramPlaybackSmokeTest::spectrogramShortTracksLengthArrivingLaterMovesTheDefault() {
    // A session that does not know the file's length yet: the default
    // stays 1 until it does, then moves to the stretched zoom, as a zoom
    // of the wheel does, with the data on screen kept until the zoom's
    // arrives.
    for (const int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(1000);
        item.setHeight(200);
        item.setDisplayMode(mode);
        QSignalSpy defaultChanged(&item, &SpectrogramItem::defaultZoomLevelChanged);
        QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);
        feedStretchSession(item, 5, 1.0, kShortFrames, 0, 16);
        QCOMPARE(item.defaultZoomLevel(), 1.0);
        QCOMPARE(item.zoomLevel(), 1.0);
        QCOMPARE(defaultChanged.count(), 0);
        const bool ready = item.m_precomputedReady;

        // The length arrives with the next columns.
        const int end = stretchEndColumn(kShortFrames, 1.0);
        item.feedPrecomputedChunk(
            QByteArray(16 * kStretchBins, '\x40'), kStretchBins, 0, 16, 16, end, kStretchRate,
            1024, false, false, 5, false, 0, 0, kStretchFft, kShortFrames, end);
        const double fit = ferrous_ffi_spectrogram_fit_zoom(kShortFrames, kStretchFft, kStretchRate, 1000);
        QCOMPARE(defaultChanged.count(), 1);
        QCOMPARE(item.defaultZoomLevel(), fit);
        // Nothing resets: the view's owner follows the default.
        QCOMPARE(zoomReset.count(), 0);
        QCOMPARE(item.zoomLevel(), 1.0);
        QCOMPARE(item.m_precomputedReady, ready);
        item.setZoomLevel(item.defaultZoomLevel());
        QCOMPARE(item.zoomLevel(), fit);
        QVERIFY(item.m_awaitingZoomData);
        QCOMPARE(item.m_precomputedReady, ready);
        QCOMPARE(zoomReset.count(), 0);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramGaplessLongToShortTrackStretchesTheView_data() {
    QTest::addColumn<int>("displayMode");
    QTest::newRow("rolling") << 0;
    QTest::newRow("centered") << 1;
}

void SpectrogramPlaybackSmokeTest::spectrogramGaplessLongToShortTrackStretchesTheView() {
    QFETCH(int, displayMode);
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(200);
    item.setDisplayMode(displayMode);
    item.setPlaying(true);
    constexpr quint64 longToken = 5;
    constexpr quint64 shortToken = 6;
    setTestPosition(item, 59.0, longToken);
    feedStretchSession(item, longToken, 1.0, kLongFrames, kLongFrames, 64);
    QCOMPARE(item.defaultZoomLevel(), 1.0);
    QSignalSpy backendZoom(&item, &SpectrogramItem::backendZoomRequested);
    QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);
    QSignalSpy defaultChanged(&item, &SpectrogramItem::defaultZoomLevelChanged);
    const double fit = ferrous_ffi_spectrogram_fit_zoom(kShortFrames, kStretchFft, kStretchRate, 1000);

    if (displayMode == 1) {
        // Centered: the short track's session tells its length, and the
        // view resets to its default.
        const int end = stretchEndColumn(kShortFrames, 1.0);
        item.feedPrecomputedChunk(
            QByteArray(16 * kStretchBins, '\x40'), kStretchBins, 0, 16, 0, end, kStretchRate,
            1024, false, false, shortToken, false, 0, 0, kStretchFft, kShortFrames, end);
        QCOMPARE(item.defaultZoomLevel(), fit);
        QCOMPARE(item.zoomLevel(), fit);
        QCOMPARE(zoomReset.count(), 1);
        QCOMPARE(backendZoom.count(), 1);
        QCOMPARE(backendZoom.at(0).at(0).toFloat(), static_cast<float>(fit));
        return;
    }

    // Rolling: the session goes on into the short track without telling its
    // length, and the view goes on at its zoom.
    item.feedPrecomputedChunk(
        QByteArray(16 * kStretchBins, '\x40'), kStretchBins, 0, 16, 64, 2853, kStretchRate,
        1024, false, false, shortToken, false, 0, 0, kStretchFft, 0, 0);
    QCOMPARE(item.defaultZoomLevel(), 1.0);
    QCOMPARE(item.zoomLevel(), 1.0);
    QCOMPARE(zoomReset.count(), 0);
    QCOMPARE(backendZoom.count(), 0);
    // The clock's length of the short track tells the default, which the
    // view's owner then follows.
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = 0.0;
    sample.sampledAtNs = clock.nowNs();
    sample.advancing = true;
    sample.durationSeconds = 1.0;
    sample.trackToken = shortToken;
    clock.applySample(sample, false);
    QCOMPARE(defaultChanged.count(), 1);
    QCOMPARE(item.defaultZoomLevel(), fit);
    QCOMPARE(zoomReset.count(), 0);
}

void SpectrogramPlaybackSmokeTest::spectrogramRepeatOneOfAStretchedTrackKeepsItsZoom() {
    // Repeat-One plays the track again gaplessly as a new token: the view
    // at the stretched default stays there, without a restart.
    for (const int mode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(1000);
        item.setHeight(200);
        item.setDisplayMode(mode);
        feedStretchSession(item, 5, 1.0, kShortFrames, kShortFrames, 16);
        const double fit = item.defaultZoomLevel();
        QVERIFY(fit > 16.0);
        feedStretchSession(item, 5, fit, kShortFrames, kShortFrames);
        QCOMPARE(item.zoomLevel(), fit);
        QSignalSpy backendZoom(&item, &SpectrogramItem::backendZoomRequested);
        QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);
        QSignalSpy defaultChanged(&item, &SpectrogramItem::defaultZoomLevelChanged);
        const int hop = stretchHop(fit);
        const int end = stretchEndColumn(kShortFrames, fit);
        item.feedPrecomputedChunk(
            QByteArray(16 * kStretchBins, '\x40'), kStretchBins, 0, 16, mode == 0 ? end : 0,
            mode == 0 ? 2 * end : end, kStretchRate, hop, false, false, 6, false, 0, 0,
            kStretchFft, mode == 0 ? 0 : kShortFrames, mode == 0 ? 0 : end);
        QCOMPARE(item.zoomLevel(), fit);
        QCOMPARE(item.defaultZoomLevel(), fit);
        QCOMPARE(backendZoom.count(), 0);
        QCOMPARE(zoomReset.count(), 0);
        QCOMPARE(defaultChanged.count(), 0);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramStretchSettingMovesTheDefaultWhilePlaying() {
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(200);
    item.setDisplayMode(1);
    feedStretchSession(item, 5, 1.0, kShortFrames, kShortFrames, 16);
    const double fit = item.defaultZoomLevel();
    QVERIFY(fit > 1.0);
    QSignalSpy defaultChanged(&item, &SpectrogramItem::defaultZoomLevelChanged);
    QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);
    item.setStretchShortTracks(false);
    QCOMPARE(defaultChanged.count(), 1);
    QCOMPARE(item.defaultZoomLevel(), 1.0);
    QCOMPARE(item.maximumZoomLevel(), 16.0);
    item.setStretchShortTracks(true);
    QCOMPARE(defaultChanged.count(), 2);
    QCOMPARE(item.defaultZoomLevel(), fit);
    // A zoom deeper than 16x comes to 16x when the setting goes off, and
    // stays there when it comes back on.
    item.setZoomLevel(20.0);
    item.setStretchShortTracks(false);
    QCOMPARE(item.zoomLevel(), 16.0);
    item.setStretchShortTracks(true);
    QCOMPARE(item.zoomLevel(), 16.0);
    QCOMPARE(defaultChanged.count(), 4);
    // A wider view fills at a deeper zoom; a narrower one at a shallower.
    item.setWidth(1500);
    QCOMPARE(defaultChanged.count(), 5);
    QVERIFY(item.defaultZoomLevel() > fit);
    item.setWidth(1000);
    QCOMPARE(item.defaultZoomLevel(), fit);
    // The item leaves its zoom to its owner.
    QCOMPARE(zoomReset.count(), 0);
}

void SpectrogramPlaybackSmokeTest::spectrogramKeptRollingZoomComesWithinTheLongTracksLimit_data() {
    QTest::addColumn<bool>("clockFirst");
    QTest::addColumn<double>("chosenZoom");
    QTest::newRow("data first, at the clip's 64x") << false << 64.0;
    QTest::newRow("clock first, at the clip's 64x") << true << 64.0;
    QTest::newRow("data first, within the limit") << false << 8.0;
    QTest::newRow("clock first, within the limit") << true << 8.0;
}

void SpectrogramPlaybackSmokeTest::spectrogramKeptRollingZoomComesWithinTheLongTracksLimit() {
    // Regression: a zoom of a short clip, deeper than 16x, kept through a
    // rolling gapless transition to a long track stayed above the long
    // track's limit for good once the clock told its length, as the
    // continuation's data does not.
    QFETCH(bool, clockFirst);
    QFETCH(double, chosenZoom);
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(200);
    item.setDisplayMode(0);
    item.setPlaying(true);
    constexpr quint64 clipToken = 5;
    constexpr quint64 longToken = 6;
    // A fifth of a second fills the view only at the deepest zoom.
    constexpr quint64 clipFrames = 9'600;
    setTestPosition(item, 0.1, clipToken);
    feedStretchSession(item, clipToken, 1.0, clipFrames, clipFrames, 4);
    QCOMPARE(item.defaultZoomLevel(), 64.0);
    feedStretchSession(item, clipToken, 64.0, clipFrames, clipFrames);
    // The user's zoom: at the default, or out from it with the wheel, and
    // the backend's session at it.
    item.setZoomLevel(chosenZoom);
    QCOMPARE(item.zoomLevel(), chosenZoom);
    feedStretchSession(item, clipToken, chosenZoom, clipFrames, clipFrames);
    QVERIFY(!item.m_awaitingZoomData);
    QSignalSpy maximumChanged(&item, &SpectrogramItem::maximumZoomLevelChanged);
    QSignalSpy zoomReset(&item, &SpectrogramItem::zoomResetRequested);

    const auto clockOnLongTrack = [&item]() {
        PlaybackClock &clock = testClock(item);
        PlaybackSample sample;
        sample.sampledAtNs = clock.nowNs();
        sample.advancing = true;
        sample.durationSeconds = 60.0;
        sample.trackToken = longToken;
        clock.applySample(sample, false);
    };
    const auto continuationData = [&item, chosenZoom]() {
        const int hop = stretchHop(chosenZoom);
        const int start = stretchEndColumn(clipFrames, chosenZoom);
        // The session goes on into the long track without its length.
        item.feedPrecomputedChunk(
            QByteArray(16 * kStretchBins, '\x40'), kStretchBins, 0, 16, start, start + 4096,
            kStretchRate, hop, false, false, longToken, false, 0, 0, kStretchFft, 0, 0);
    };
    if (clockFirst) {
        clockOnLongTrack();
        continuationData();
    } else {
        continuationData();
        // Until the clock tells the long track's length, the clip's
        // default stands.
        QCOMPARE(item.defaultZoomLevel(), 64.0);
        QCOMPARE(item.zoomLevel(), chosenZoom);
        clockOnLongTrack();
    }
    QCOMPARE(item.defaultZoomLevel(), 1.0);
    QCOMPARE(item.maximumZoomLevel(), 16.0);
    QVERIFY(maximumChanged.count() >= 1);
    // A zoom within the long track's limit stays; a deeper one comes to it.
    QCOMPARE(item.zoomLevel(), std::min(chosenZoom, 16.0));
    // The rolling view goes on: nothing resets it.
    QCOMPARE(zoomReset.count(), 0);
    if (chosenZoom > 16.0) {
        // Its data is asked for at the limit.
        QVERIFY(item.m_awaitingZoomData);
        QCOMPARE(item.m_pendingBackendZoom, 16.0f);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramRenderLoopStopsWhenNotPlaying() {
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

namespace {

constexpr double kWholeTrackColumnsPerSecond = 48'000.0 / 20'480.0;

// How long until the item's next playback frame is due, in milliseconds
// from `nowNs`.
double playbackFrameDueInMs(const SpectrogramItem &item, qint64 nowNs = monotonicNowNs()) {
    return static_cast<double>(item.m_frameScheduler->dueNs(&item) - nowNs) / 1e6;
}

} // namespace

void SpectrogramPlaybackSmokeTest::spectrogramStillFramesLeaveTextureNodesAlone() {
    // The scene graph redoes each node it is told has changed: its geometry,
    // its material, and the batches it is in. A frame that shows what the
    // last one did, or moves only the playhead, changes no texture node.
    for (const double scale : {1.0, 2.0}) {
        QQuickWindow window;
        window.resize(400, 100);
        if (scale != 1.0) {
            QScreen *screen = hiDpiTestScreen(scale);
            if (screen == nullptr) {
                qInfo("No offscreen HiDPI test screen at %g", scale);
                continue;
            }
            QVERIFY(moveWindowToScreen(window, screen));
        }
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
        prepareWholeTrackPlayback(item, 50.0);
        item.setPlaying(false);
        item.setGridEnabled(true);
        item.setShowTimeLabels(true);
        QSGNode *node = paintSettled(item, nullptr);
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(drawnCanvasFragments(node) > 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).textureNodeChanges, 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasTextures, 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasUploads, 0);
        auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
        const QRectF before = playhead->rect();
        setTestPosition(item, 50.0 + 1.5 / kWholeTrackColumnsPerSecond);
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(playhead->rect().left() > before.left());
        QCOMPARE(SpectrogramItem::sceneStatistics(node).textureNodeChanges, 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasTextures, 0);
        QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasUploads, 0);
        delete node;
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramIdleCenteredViewWaitsForThePlayheadsNextPixel_data() {
    QTest::addColumn<double>("scale");
    QTest::newRow("1x") << 1.0;
    QTest::newRow("2x") << 2.0;
}

void SpectrogramPlaybackSmokeTest::spectrogramIdleCenteredViewWaitsForThePlayheadsNextPixel() {
    QFETCH(double, scale);
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
    }
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    // The test's time, so that how long painting and the timer take on a
    // loaded machine moves nothing.
    TestTime time;
    time.drive(item);
    // The window does not draw it; the test does.
    prepareWholeTrackPlayback(item, 50.0);
    QSGNode *node = paintSettled(item, nullptr);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_canvasScale, scale);
        QVERIFY(item.m_idleView.valid);
        // Column 117 is under the playhead; the next one moves it.
        QCOMPARE(item.m_idleView.changeSeconds, 118.0 / kWholeTrackColumnsPerSecond);
    }
    const quint64 requested = item.m_playbackFrameRequests;
    item.handleWindowAfterAnimating();
    // No frame until then: it would be the same.
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(playbackFrameScheduled(item));
    QCOMPARE(item.m_frameScheduler->parent(), &window);
    // Due at the column's change, rounded up to its millisecond.
    const double untilChangeMs = (118.0 / kWholeTrackColumnsPerSecond
        - testClock(item).positionAt(time.nowNs)) * 1000.0;
    const double dueInMs = playbackFrameDueInMs(item, time.nowNs);
    QVERIFY2(dueInMs >= untilChangeMs && dueInMs - untilChangeMs < 1.0,
        qPrintable(QStringLiteral("%1 ms for %2 ms").arg(dueInMs).arg(untilChangeMs)));
    // The window's scheduler wakes it then, with the playhead on its next
    // pixel, and the next frame waits for the column after.
    time.nowNs = item.m_frameScheduler->dueNs(&item);
    QTRY_COMPARE(item.m_playbackFrameRequests, requested + 1);
    auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
    const double before = playhead->rect().left();
    node = item.updatePaintNode(node, nullptr);
    QCOMPARE(playhead->rect().left(), before + 1.0);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_idleView.valid);
        QCOMPARE(item.m_idleView.changeSeconds, 119.0 / kWholeTrackColumnsPerSecond);
    }
    item.handleWindowAfterAnimating();
    QCOMPARE(item.m_playbackFrameRequests, requested + 1);
    QVERIFY(playbackFrameDueInMs(item, time.nowNs) > 300);
    // Paused, it waits for the clock.
    item.setPlaying(false);
    QVERIFY(!playbackFrameScheduled(item));
    QVERIFY(!item.m_frameScheduler->timerActive());
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramLateFrameCallbackDrawsAtOnce() {
    // The frame painted at 50 s shows column 117 until column 118 at
    // 50.35 s. Its frame-swapped callback arrives after that: the next
    // frame is due at once.
    SpectrogramItem item;
    prepareWholeTrackPlayback(item, 50.0);
    QSGNode *node = paintSettled(item, nullptr);
    PlaybackClock &clock = testClock(item);
    QVERIFY(QTest::qWaitFor([&clock]() {
        return clock.positionAt(monotonicNowNs()) >= 118.2 / kWholeTrackColumnsPerSecond;
    }, 2000));
    const quint64 requested = item.m_playbackFrameRequests;
    item.handleWindowAfterAnimating();
    QCOMPARE(item.m_playbackFrameRequests, requested + 1);
    QVERIFY(!playbackFrameScheduled(item));
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramScrollingViewWaitsForTheNextPixel_data() {
    addScrollingRows();
}

void SpectrogramPlaybackSmokeTest::spectrogramScrollingViewWaitsForTheNextPixel() {
    // A view that scrolls moves its canvas by whole device pixels, `scale`
    // of them a column. Its frame changes only when the scroll reaches the
    // next pixel or playback the next column, and it asks for no frame
    // before then: a frame drawn before is the same, and changes no node.
    QFETCH(int, mode);
    QFETCH(double, scale);
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
    }
    TestTime time;
    // The window does not draw it; the test does.
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareScrollingPlayback(item, time, mode);
    QSGNode *node = paintSettled(item, nullptr);
    node = item.updatePaintNode(node, nullptr);
    auto *playhead = static_cast<QSGSimpleRectNode *>(node->childAtIndex(kPlayheadChild));
    const auto canvasOnWholePixels = [&]() {
        QSGNode *tiles = node->childAtIndex(1);
        for (int i = 0; i < tiles->childCount(); ++i) {
            const QRectF rect = static_cast<QSGSimpleTextureNode *>(tiles->childAtIndex(i))->rect();
            if (!rect.isEmpty()
                && (std::abs(rect.left() * scale - std::round(rect.left() * scale)) > 1e-6
                    || std::abs(rect.right() * scale - std::round(rect.right() * scale)) > 1e-6)) {
                return false;
            }
        }
        return true;
    };
    QVERIFY(canvasOnWholePixels());
    PlaybackClock &clock = testClock(item);
    const double start = clock.positionAt(time.nowNs);
    int frames = 0;
    while (clock.positionAt(time.nowNs) < start + 1.0) {
        double changeSeconds = 0.0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            QVERIFY(item.m_idleView.valid);
            changeSeconds = item.m_idleView.changeSeconds;
        }
        const double position = clock.positionAt(time.nowNs);
        QVERIFY(changeSeconds > position);
        // At most a pixel's scroll and a column ahead.
        QVERIFY((changeSeconds - position) * kStripeColumnsPerSecond * scale <= 1.0 + 1e-6);
        QVERIFY((changeSeconds - position) * kStripeColumnsPerSecond <= 1.0 + 1e-6);
        const quint64 requested = item.m_playbackFrameRequests;
        item.handleWindowAfterAnimating();
        QCOMPARE(item.m_playbackFrameRequests, requested);
        QVERIFY(playbackFrameScheduled(item));
        const qint64 changeNs = *clock.state().reachesAt(changeSeconds);
        const qint64 dueNs = item.m_frameScheduler->dueNs(&item);
        QVERIFY(dueNs >= changeNs && dueNs < changeNs + 1'000'000);
        // Just before then, the frame is this one.
        const QRectF shownPlayhead = playhead->rect();
        if (changeNs - 50'000 > time.nowNs) {
            time.nowNs = changeNs - 50'000;
            node = item.updatePaintNode(node, nullptr);
            QCOMPARE(SpectrogramItem::sceneStatistics(node).textureNodeChanges, 0);
            QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasTextures, 0);
            QCOMPARE(SpectrogramItem::sceneStatistics(node).canvasUploads, 0);
            QCOMPARE(playhead->rect(), shownPlayhead);
        }
        // Then it is another.
        time.nowNs = dueNs;
        node = item.updatePaintNode(node, nullptr);
        ++frames;
        QVERIFY(SpectrogramItem::sceneStatistics(node).textureNodeChanges > 0
            || SpectrogramItem::sceneStatistics(node).canvasTextures > 0
            || SpectrogramItem::sceneStatistics(node).canvasUploads > 0);
        QVERIFY(canvasOnWholePixels());
    }
    // About a frame for each pixel the view scrolls, and one for each
    // column that does not start on a pixel.
    const double pixels = kStripeColumnsPerSecond * std::max(scale, 1.0);
    QVERIFY2(frames >= pixels - 2.0 && frames <= kStripeColumnsPerSecond * (scale + 1.0) + 2.0,
        qPrintable(QStringLiteral("%1 frames for %2 pixels").arg(frames).arg(pixels)));
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramScrollingFramesMoveAPixelAtATime_data() {
    addScrollingRows();
}

void SpectrogramPlaybackSmokeTest::spectrogramScrollingFramesMoveAPixelAtATime() {
    // In a window: what the window shows of a view that scrolls stays the
    // same until the frame the item asks for, which moves it on by no more
    // than a device pixel. Over a second those frames move it as far as
    // playback goes: no step is skipped.
    QFETCH(int, mode);
    QFETCH(double, scale);
    QQuickWindow window;
    if (window.rendererInterface()->graphicsApi() == QSGRendererInterface::Null) {
        QSKIP("The null renderer draws no pixels");
    }
    window.resize(400, 100);
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    TestTime time;
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareScrollingPlayback(item, time, mode);
    const auto grab = [&]() {
        item.update();
        return window.grabWindow().convertToFormat(QImage::Format_RGB32);
    };
    QImage shown = grab();
    for (int round = 0; round < 8 && spectrogramRasterPending(item); ++round) {
        QVERIFY(QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000));
        shown = grab();
    }
    QVERIFY(!spectrogramRasterPending(item));
    const int width = shown.width();
    QCOMPARE(width, static_cast<int>(std::round(400 * scale)));
    // The centered playhead stays in the middle, over what scrolls; the
    // newest column comes in at the right edge.
    const int playheadLeft = width / 2 - 2;
    const int playheadRight = width / 2 + static_cast<int>(std::ceil(2.0 * scale)) + 1;
    const int edge = width - static_cast<int>(std::ceil(scale)) - 2;
    const auto shiftedBy = [&](const QImage &next, int shift) {
        for (int x = 0; x + shift < edge; ++x) {
            if (mode == 1 && x + shift >= playheadLeft && x <= playheadRight) {
                continue;
            }
            for (int y = 0; y < next.height(); ++y) {
                if (next.pixel(x, y) != shown.pixel(x + shift, y)) {
                    return false;
                }
            }
        }
        return true;
    };
    PlaybackClock &clock = testClock(item);
    const double start = clock.positionAt(time.nowNs);
    int shifts = 0;
    int frames = 0;
    while (clock.positionAt(time.nowNs) < start + 1.0) {
        item.handleWindowAfterAnimating();
        QVERIFY(playbackFrameScheduled(item));
        const qint64 dueNs = item.m_frameScheduler->dueNs(&item);
        // Frames drawn before then would be the same.
        const qint64 shownAt = time.nowNs;
        for (const double part : {0.3, 0.8}) {
            const qint64 at = shownAt + static_cast<qint64>(
                static_cast<double>(dueNs - 1'000'000 - shownAt) * part);
            if (at > time.nowNs) {
                time.nowNs = at;
                QVERIFY(grab() == shown);
            }
        }
        time.nowNs = dueNs;
        const QImage next = grab();
        const bool still = shiftedBy(next, 0);
        const bool moved = shiftedBy(next, 1);
        QVERIFY2(still != moved,
            qPrintable(QStringLiteral("frame %1 at %2 s").arg(frames).arg(clock.positionAt(time.nowNs))));
        shifts += moved ? 1 : 0;
        ++frames;
        shown = next;
    }
    const double travelled = (clock.positionAt(time.nowNs) - start) * kStripeColumnsPerSecond * scale;
    QVERIFY2(std::abs(shifts - travelled) <= 1.0,
        qPrintable(QStringLiteral("%1 steps for %2 pixels").arg(shifts).arg(travelled)));
}

void SpectrogramPlaybackSmokeTest::spectrogramDataAheadOfTheShownFrameAsksForNoFrame() {
    // The decoder sends columns ahead of the playhead. While a view plays
    // and has asked for its next frame, columns after the ones it shows
    // change nothing on screen until playback reaches them, in frames it
    // asks for anyway. Columns it shows, and any while paused, ask for one.
    for (const int mode : {0, 1}) {
        TestTime time;
        SpectrogramItem item;
        time.drive(item);
        item.setWidth(400);
        item.setHeight(100);
        item.setDisplayMode(mode);
        constexpr int bins = 64;
        const auto feed = [&item](int start, int columns, char value, bool reset = false) {
            item.feedPrecomputedChunk(QByteArray(bins * columns, value), bins, 0, columns,
                start, 4000, 48'000, 1024, false, reset, 1, reset, 1);
        };
        feed(0, 3000, '\x50', true);
        setTestPosition(item, 40.0);
        item.setPlaying(true);
        setTestPosition(item, 40.0);
        QSGNode *node = paintSettled(item, nullptr);
        node = item.updatePaintNode(node, nullptr);
        {
            QMutexLocker lock(&item.m_stateMutex);
            QVERIFY(item.m_idleView.valid);
            QVERIFY(item.m_idleView.shownRight < item.m_ringWriteSeq);
        }
        quint64 requested = item.m_dataFrameRequests;
        feed(3000, 100, '\x60');
        QCOMPARE(item.m_dataFrameRequests, requested);
        if (mode == 1) {
            // A column under the playhead.
            feed(1880, 1, '\x70');
            QCOMPARE(item.m_dataFrameRequests, ++requested);
        }
        item.setPlaying(false);
        feed(3100, 100, '\x60');
        QCOMPARE(item.m_dataFrameRequests, ++requested);
        delete node;
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramFrameSchedulerWakesDueFramesTogether() {
    // Frames due within the same millisecond come together, from one
    // timer; a later one waits for its own.
    PlaybackFrameScheduler scheduler;
    SpectrogramItem first;
    SpectrogramItem second;
    SpectrogramItem third;
    const qint64 nowNs = monotonicNowNs();
    const qint64 baseMs = nowNs / 1'000'000 + 10;
    qint64 schedulerNowNs = nowNs;
    scheduler.m_nowNs = [&schedulerNowNs]() { return schedulerNowNs; };
    scheduler.schedule(&first, baseMs * 1'000'000 + 200'000, nowNs);
    scheduler.schedule(&second, baseMs * 1'000'000 + 900'000, nowNs);
    scheduler.schedule(&third, baseMs * 1'000'000 + 1'000'001, nowNs);
    QCOMPARE(scheduler.dueNs(&first), (baseMs + 1) * 1'000'000);
    QCOMPARE(scheduler.dueNs(&second), (baseMs + 1) * 1'000'000);
    QCOMPARE(scheduler.dueNs(&third), (baseMs + 2) * 1'000'000);
    QVERIFY(scheduler.timerActive());
    QCOMPARE(scheduler.m_armedMs, baseMs + 1);
    schedulerNowNs = (baseMs + 1) * 1'000'000 + 100'000;
    scheduler.fire();
    QCOMPARE(first.m_playbackFrameRequests, quint64(1));
    QCOMPARE(second.m_playbackFrameRequests, quint64(1));
    QCOMPARE(third.m_playbackFrameRequests, quint64(0));
    QCOMPARE(scheduler.dueNs(&first), qint64(-1));
    QVERIFY(scheduler.timerActive());
    QCOMPARE(scheduler.m_armedMs, baseMs + 2);
    // Cancelled, nothing is left to wake.
    scheduler.cancel(&third);
    QVERIFY(!scheduler.timerActive());
    // An item that goes away takes its frame with it.
    {
        SpectrogramItem gone;
        gone.m_frameScheduler = &scheduler;
        scheduler.schedule(&gone, nowNs + 5'000'000, nowNs);
        QVERIFY(scheduler.dueNs(&gone) >= 0);
    }
    QVERIFY(scheduler.m_dueMs.isEmpty());
    QVERIFY(!scheduler.timerActive());
}

void SpectrogramPlaybackSmokeTest::spectrogramFrameSchedulerWakesEveryFrameDueByALateTimer() {
    // Frames due at 100 and 110 ms, and a timer the event loop delivers at
    // 125 ms: both are due, and come in one frame of the window. One due
    // at 140 ms waits for its own, 15 ms from then.
    PlaybackFrameScheduler scheduler;
    const qint64 startNs = monotonicNowNs() / 1'000'000 * 1'000'000;
    qint64 nowNs = startNs;
    scheduler.m_nowNs = [&nowNs]() { return nowNs; };
    SpectrogramItem first;
    SpectrogramItem second;
    SpectrogramItem third;
    scheduler.schedule(&first, startNs + 100'000'000, nowNs);
    scheduler.schedule(&second, startNs + 110'000'000, nowNs);
    scheduler.schedule(&third, startNs + 140'000'000, nowNs);
    QCOMPARE(scheduler.m_armedMs, startNs / 1'000'000 + 100);
    nowNs = startNs + 125'000'000;
    scheduler.fire();
    QCOMPARE(first.m_playbackFrameRequests, quint64(1));
    QCOMPARE(second.m_playbackFrameRequests, quint64(1));
    QCOMPARE(third.m_playbackFrameRequests, quint64(0));
    QCOMPARE(scheduler.m_armedMs, startNs / 1'000'000 + 140);
    QVERIFY(scheduler.timerActive());
    QVERIFY(scheduler.m_timer.remainingTime() <= 15);
    scheduler.cancel(&third);
}

void SpectrogramPlaybackSmokeTest::spectrogramDecodedEndMovesTheCenteredViewAtOnce() {
    // Near the end of a track, a centered view ends at the last decoded
    // column. With 3906 of an estimated 4000 columns decoded and the
    // playhead at column 3900, a 400-column view shows columns 3506 to
    // 3905; each chunk the decoder sends after them moves it on, at once.
    TestTime time;
    SpectrogramItem item;
    time.drive(item);
    item.setWidth(400);
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 64;
    const auto feed = [&item](int start, int columns, bool reset = false) {
        item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
            start, 4000, 48'000, 1024, false, reset, 1, reset, 1);
    };
    feed(0, 3906, true);
    const double position = 3900.2 / kStripeColumnsPerSecond;
    setTestPosition(item, position);
    item.setPlaying(true);
    setTestPosition(item, position);
    QSGNode *node = paintSettled(item, nullptr);
    node = item.updatePaintNode(node, nullptr);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_idleView.valid);
        QCOMPARE(item.m_idleView.shownLeft, qint64(3506));
        QCOMPARE(item.m_idleView.shownRight, qint64(3905));
    }
    qint64 right = 3905;
    for (int chunk = 0; chunk < 2; ++chunk) {
        const quint64 requested = item.m_dataFrameRequests;
        feed(static_cast<int>(right) + 1, 32);
        QCOMPARE(item.m_dataFrameRequests, requested + 1);
        node = paintSettled(item, node);
        right += 32;
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_idleView.valid);
        QCOMPARE(item.m_idleView.shownRight, right);
        QCOMPARE(item.m_idleView.shownLeft, right - 399);
    }
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramPaintedCenteredViewFollowsASmallEndCorrection_data() {
    QTest::addColumn<int>("correction");
    QTest::newRow("a column") << 1;
    QTest::newRow("two columns") << 2;
}

void SpectrogramPlaybackSmokeTest::spectrogramPaintedCenteredViewFollowsASmallEndCorrection() {
    // Near the end, a centered view ends where the session's views end,
    // which its chunks reckon from the file's frames until its last chunk
    // says where its decode ended (issue 441). A painted view moves with
    // that correction at once, however small: a view does not step back a
    // column or two as the playhead's position wobbles, but one ending at
    // the end does not follow the playhead, and held, it showed the columns
    // past the new end blank at its right edge.
    QFETCH(int, correction);
    SpectrogramItem item;
    item.setWidth(1'200);
    item.setHeight(16);
    item.setDisplayMode(1);
    item.setPlaying(false);
    constexpr int bins = 4;
    constexpr int rate = 8'000;
    constexpr int hop = 171;
    constexpr int fftSize = 2'048;
    constexpr quint64 frames = 120 * rate;
    constexpr qint64 window = 1'200;
    // 120 s: see spectrogramCenteredViewEndsAtTheDecodedEndNotTheEstimate.
    constexpr int predictedEnd = 5'609;
    constexpr int estimate = (120 * rate / 1'024 + 64) * 1'024 / hop + 1;
    const int end = predictedEnd - correction;
    // 3 s before the end: the view ends at the end.
    constexpr double position = 117.0;
    QVERIFY(static_cast<qint64>(position * rate / hop) + window / 2 > predictedEnd);

    // Every column the session decodes is in; its chunks carry the end
    // reckoned from the file's frames.
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, 0, estimate, rate, hop,
        false, true, 1, false, 1, 0, fftSize, frames, predictedEnd);
    item.feedPrecomputedChunk(QByteArray(qsizetype(end) * bins, '\x80'), bins, 0, end, 0,
        estimate, rate, hop, false, false, 1, false, 1, 0, fftSize, frames, predictedEnd);
    setTestPosition(item, position);
    QSGNode *node = paintSettled(item, nullptr);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, predictedEnd - window);
        QCOMPARE(item.m_precomputedCanvasDisplayRight, qint64(predictedEnd - 1));
    }

    // The session's last chunk: its decode ended a column or two short.
    item.feedPrecomputedChunk(QByteArray(), bins, 0, 0, end, end, rate, hop,
        /*complete=*/true, /*bufferReset=*/false, 1, /*clearHistoryOnReset=*/false, 1, 0,
        fftSize, frames, end);
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedEndColumn, end);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, end - window);
        QCOMPARE(item.m_precomputedCanvasDisplayRight, qint64(end - 1));
        for (qint64 column = end - window; column < end; ++column) {
            QVERIFY(item.ringSlotForDisplayIndexLocked(column, false) >= 0);
        }
    }
    // None of the canvas's columns is blank, at its right edge or elsewhere.
    const QImage shown = canvasInViewOrder(item);
    QCOMPARE(shown.width(), static_cast<int>(window));
    for (int x = 0; x < shown.width(); ++x) {
        QVERIFY2(shown.pixel(x, shown.height() / 2) != kSpectrogramNoColumn,
                 qPrintable(QStringLiteral("x %1 is blank").arg(x)));
    }

    // Mid-track, where the view follows the playhead, it still does not
    // step back a column with it.
    constexpr double midTrack = 60.0;
    setTestPosition(item, midTrack);
    node = paintSettled(item, node);
    qint64 midLeft = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        midLeft = item.m_precomputedCanvasDisplayLeft;
        QCOMPARE(midLeft, static_cast<qint64>(midTrack * rate / hop) - window / 2);
    }
    setTestPosition(item, midTrack - 1.0 * hop / rate);
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_precomputedCanvasDisplayLeft, midLeft);
    }
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramPanesShareTheirWindowsFrameSchedule() {
    // The channels' panes of a window scroll alike: they wait for the same
    // frame, which one timer asks for for both. A pane in another window,
    // such as the fullscreen viewer, waits on that window's schedule.
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    TestTime time;
    std::array<SpectrogramItem *, 2> panes{};
    for (int i = 0; i < 2; ++i) {
        panes[i] = new SpectrogramItem(window.contentItem());
        panes[i]->setY(50.0 * i);
        prepareScrollingPlayback(*panes[i], time, 1, 50.0);
    }
    std::array<QSGNode *, 2> nodes{};
    for (int i = 0; i < 2; ++i) {
        nodes[i] = paintSettled(*panes[i], nullptr);
        nodes[i] = panes[i]->updatePaintNode(nodes[i], nullptr);
    }
    for (SpectrogramItem *pane : panes) {
        pane->handleWindowAfterAnimating();
    }
    PlaybackFrameScheduler *scheduler = panes[0]->m_frameScheduler;
    QVERIFY(scheduler != nullptr);
    QCOMPARE(scheduler->parent(), &window);
    QCOMPARE(panes[1]->m_frameScheduler.data(), scheduler);
    QVERIFY(scheduler->dueNs(panes[0]) >= 0);
    QCOMPARE(scheduler->dueNs(panes[1]), scheduler->dueNs(panes[0]));
    const quint64 first = panes[0]->m_playbackFrameRequests;
    const quint64 second = panes[1]->m_playbackFrameRequests;
    QTRY_VERIFY_WITH_TIMEOUT(panes[0]->m_playbackFrameRequests > first, 1000);
    QCOMPARE(panes[0]->m_playbackFrameRequests, first + 1);
    QCOMPARE(panes[1]->m_playbackFrameRequests, second + 1);

    QQuickWindow viewer;
    viewer.resize(400, 100);
    presentWithoutDrawing(viewer);
    panes[1]->handleWindowAfterAnimating();
    QVERIFY(scheduler->dueNs(panes[1]) >= 0);
    panes[1]->setParentItem(viewer.contentItem());
    QCOMPARE(scheduler->dueNs(panes[1]), qint64(-1));
    delete nodes[1];
    nodes[1] = paintSettled(*panes[1], nullptr);
    nodes[1] = panes[1]->updatePaintNode(nodes[1], nullptr);
    panes[1]->handleWindowAfterAnimating();
    QVERIFY(panes[1]->m_frameScheduler != scheduler);
    QCOMPARE(panes[1]->m_frameScheduler->parent(), &viewer);
    QVERIFY(panes[1]->m_frameScheduler->dueNs(panes[1]) >= 0);
    delete panes[1];
    delete nodes[0];
    delete nodes[1];
}

void SpectrogramPlaybackSmokeTest::spectrogramPaneNoOneSeesAsksForNoPlaybackFrames() {
    // A pane no one sees, such as the spectrogram behind the waveform view
    // or one in a window off screen, follows the clock but asks for no
    // frames. Shown again, it asks for one, which shows playback where it
    // is then; its frames follow from there.
    QQuickWindow window;
    window.resize(400, 100);
    presentWithoutDrawing(window);
    TestTime time;
    // The window does not draw it; the test does.
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    prepareScrollingPlayback(item, time, 1);
    QSGNode *node = paintSettled(item, nullptr);
    node = item.updatePaintNode(node, nullptr);
    item.handleWindowAfterAnimating();
    QVERIFY(playbackFrameScheduled(item));
    PlaybackFrameScheduler *scheduler = item.m_frameScheduler;
    QVERIFY(scheduler->timerActive());
    PlaybackClock &clock = testClock(item);
    // Half a second of the engine's samples, which correct the clock's
    // course, while the window draws frames for its other items; then a
    // seek.
    const auto playOn = [&]() {
        PlaybackSample sample;
        sample.advancing = true;
        sample.trackToken = clock.state().trackToken;
        for (int i = 0; i < 12; ++i) {
            time.advance(0.04);
            sample.positionSeconds = clock.positionAt(time.nowNs) + 0.001;
            sample.sampledAtNs = time.nowNs;
            clock.applySample(sample, false);
            item.handleWindowAfterAnimating();
        }
        sample.positionSeconds = 50.0;
        clock.applySample(sample, true);
    };
    const auto showsPlaybackNow = [&]() {
        QMutexLocker lock(&item.m_stateMutex);
        return std::abs(item.m_positionSeconds - clock.positionAt(time.nowNs)) < 1e-9;
    };

    item.setVisible(false);
    QVERIFY(!playbackFrameScheduled(item));
    QVERIFY(!scheduler->timerActive());
    quint64 requested = item.m_playbackFrameRequests;
    playOn();
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(!playbackFrameScheduled(item));
    QVERIFY(!scheduler->timerActive());
    item.setVisible(true);
    QCOMPARE(item.m_playbackFrameRequests, requested + 1);
    QVERIFY(showsPlaybackNow());
    {
        // Its canvas is drawn again from the data it kept.
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(item.m_precomputedCanvasDirty);
    }
    delete node;
    node = paintSettled(item, nullptr);
    node = item.updatePaintNode(node, nullptr);
    item.handleWindowAfterAnimating();
    QVERIFY(playbackFrameScheduled(item));

    // Its window off screen: a brief unexposure changes nothing; once its
    // grace ends, the pane stops asking for frames.
    setTestExposed(window, false);
    QVERIFY(WindowPresence::of(&window)->shown());
    QVERIFY(playbackFrameScheduled(item));
    endTestUnexposedGrace(window);
    QVERIFY(!WindowPresence::of(&window)->shown());
    QVERIFY(!playbackFrameScheduled(item));
    QVERIFY(!scheduler->timerActive());
    requested = item.m_playbackFrameRequests;
    playOn();
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(!scheduler->timerActive());
    setTestExposed(window, true);
    QVERIFY(WindowPresence::of(&window)->shown());
    QCOMPARE(item.m_playbackFrameRequests, requested + 1);
    QVERIFY(showsPlaybackNow());
    node = paintSettled(item, node);
    node = item.updatePaintNode(node, nullptr);
    item.handleWindowAfterAnimating();
    QVERIFY(playbackFrameScheduled(item));
    delete &item;
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramClockCourseCorrectionWaitsButJumpsShowAtOnce() {
    SpectrogramItem item;
    prepareWholeTrackPlayback(item, 50.0);
    QSGNode *node = paintSettled(item, nullptr);
    item.handleWindowAfterAnimating();
    QVERIFY(playbackFrameScheduled(item));
    quint64 requested = item.m_playbackFrameRequests;
    PlaybackClock &clock = testClock(item);
    const auto sampleAt = [&clock](double position, quint64 token, bool advancing) {
        PlaybackSample sample;
        sample.positionSeconds = position;
        sample.sampledAtNs = monotonicNowNs();
        sample.advancing = advancing;
        sample.trackToken = token;
        return sample;
    };
    // A sample a little ahead corrects the clock's course: the next frame
    // still waits for the playhead's next pixel, a little sooner.
    const double now = clock.positionAt(monotonicNowNs());
    clock.applySample(sampleAt(now + 0.02, clock.state().trackToken, true), false);
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(playbackFrameScheduled(item));
    // A seek shows at once.
    clock.applySample(sampleAt(80.0, clock.state().trackToken, true), true);
    QCOMPARE(item.m_playbackFrameRequests, ++requested);
    // So does another track, such as a gapless successor or Repeat-One's
    // next round.
    clock.applySample(sampleAt(0.0, clock.state().trackToken + 1, true), false);
    QCOMPARE(item.m_playbackFrameRequests, ++requested);
    node = item.updatePaintNode(node, nullptr);
    // And a pause.
    clock.applySample(sampleAt(0.5, clock.state().trackToken, false), false);
    QCOMPARE(item.m_playbackFrameRequests, ++requested);
    node = item.updatePaintNode(node, nullptr);
    // Paused, nothing moves until the clock does.
    item.handleWindowAfterAnimating();
    QCOMPARE(item.m_playbackFrameRequests, requested);
    QVERIFY(!playbackFrameScheduled(item));
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramIdleViewDrawsOnlyFramesThatDiffer() {
    // In a window: the whole track in view moves its playhead a pixel
    // 2.34 times a second, and the window draws about that often. A view
    // that scrolls draws a frame for each pixel it scrolls.
    QQuickWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int frames = 0;
    connect(&window, &QQuickWindow::frameSwapped, &window, [&frames]() { ++frames; });
    auto *idle = new SpectrogramItem(window.contentItem());
    prepareWholeTrackPlayback(*idle, 50.0);
    QTest::qWait(300);
    frames = 0;
    QTest::qWait(1000);
    QVERIFY2(frames >= 1 && frames <= 5, qPrintable(QString::number(frames)));
    const int idleFrames = frames;
    delete idle;

    auto *scrolling = new SpectrogramItem(window.contentItem());
    scrolling->setWidth(400);
    scrolling->setHeight(100);
    scrolling->setDisplayMode(1);
    constexpr int bins = 64;
    constexpr int columns = 4000;
    scrolling->feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48'000, 1024, true, true, 1, true, 1);
    setTestPosition(*scrolling, 40.0);
    scrolling->setPlaying(true);
    setTestPosition(*scrolling, 40.0);
    QTest::qWait(300);
    frames = 0;
    QTest::qWait(1000);
    // It scrolls a pixel with each of its 46.875 columns a second, and
    // draws a frame for each.
    QVERIFY2(frames >= std::max(4 * idleFrames, 20) && frames <= 60,
        qPrintable(QString::number(frames)));
}

void SpectrogramPlaybackSmokeTest::spectrogramPlaybackBenchmark() {
    if (qEnvironmentVariableIntValue("FERROUS_BENCHMARK_SPECTROGRAM") != 1) {
        QSKIP("Set FERROUS_BENCHMARK_SPECTROGRAM=1 to measure playback frames");
    }
    // Steady playback of a 1720 x 719 pane in a window at
    // FERROUS_BENCHMARK_SPECTROGRAM_SCALE (default 1): rolling, centered
    // and scrolling, and centered with the whole track in view, as when
    // zoomed out. First the frames the window draws in two seconds of
    // playback, then the render sync's share of 120 frames 1/60 s apart.
    const double scale = qEnvironmentVariableIsSet("FERROUS_BENCHMARK_SPECTROGRAM_SCALE")
        ? qEnvironmentVariable("FERROUS_BENCHMARK_SPECTROGRAM_SCALE").toDouble()
        : 1.0;
    QQuickWindow window;
    window.resize(1720, 719);
    if (scale != 1.0 && QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(window, screen));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));
    struct Scenario {
        const char *name;
        int mode;
        int hop;
        int columns;
        double position;
        // With both grids and time labels, and the pointer over the pane.
        bool hovered;
    };
    // 48 kHz; a hop of 20480 is the backend's at a zoom of 0.05, drawn a
    // column to a logical pixel.
    const Scenario scenarios[] = {
        {"rolling", 0, 1024, 6000, 60.0, false},
        {"centered", 1, 1024, 20'000, 100.0, false},
        {"centered_whole_track", 1, 20'480, 1700, 300.0, false},
        {"centered_hovered", 1, 1024, 20'000, 100.0, true},
    };
    constexpr int bins = 1025;
    for (const Scenario &scenario : scenarios) {
        auto *item = new SpectrogramItem(window.contentItem());
        item->setWidth(1720);
        item->setHeight(719);
        item->setDisplayMode(scenario.mode);
        if (scenario.hovered) {
            item->setGridEnabled(true);
            item->setShowTimeLabels(true);
            item->setCrosshairEnabled(true);
            QHoverEvent hover(QEvent::HoverEnter, QPointF(500.0, 300.0), QPointF(500.0, 300.0), QPointF());
            item->hoverEnterEvent(&hover);
        }
        item->feedPrecomputedChunk(QByteArray(bins * scenario.columns, '\x50'), bins, 0,
            scenario.columns, 0, scenario.columns, 48000, scenario.hop, true, true, 1, true, 1);
        {
            QMutexLocker lock(&item->m_stateMutex);
            item->m_renderZoomLevel = 1024.0 / scenario.hop;
        }
        // There first, so that the rolling history does not wait for data
        // after a jump.
        setTestPosition(*item, scenario.position);
        item->setPlaying(true);
        setTestPosition(*item, scenario.position);
        QTest::qWait(500);
        int frames = 0;
        const auto connection = connect(&window, &QQuickWindow::frameSwapped, &window,
            [&frames]() { ++frames; });
        // The renderer's time for each frame, on the render thread.
        QMutex renderMutex;
        QElapsedTimer renderTimer;
        std::vector<double> renderMs;
        const auto renderStarted = connect(&window, &QQuickWindow::beforeRendering, &window,
            [&]() { QMutexLocker lock(&renderMutex); renderTimer.start(); }, Qt::DirectConnection);
        const auto renderEnded = connect(&window, &QQuickWindow::afterRendering, &window,
            [&]() {
                QMutexLocker lock(&renderMutex);
                renderMs.push_back(static_cast<double>(renderTimer.nsecsElapsed()) / 1e6);
            }, Qt::DirectConnection);
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::qWait(2000);
        const double framesPerSecond = frames / (static_cast<double>(elapsed.nsecsElapsed()) / 1e9);
        disconnect(connection);
        disconnect(renderStarted);
        disconnect(renderEnded);
        double renderMean = 0.0;
        {
            QMutexLocker lock(&renderMutex);
            renderMean = renderMs.empty() ? 0.0
                : std::accumulate(renderMs.begin(), renderMs.end(), 0.0) / renderMs.size();
        }
        item->setPlaying(false);
        QTest::qWait(100);

        QSGNode *node = paintSettled(*item, nullptr);
        constexpr int iterations = 120;
        std::vector<double> ms;
        qint64 textures = 0;
        qint64 uploads = 0;
        qint64 bytes = 0;
        qint64 nodeChanges = 0;
        qint64 overlayTextures = 0;
        qint64 overlayBytes = 0;
        for (int i = 0; i < iterations; ++i) {
            {
                QMutexLocker lock(&item->m_stateMutex);
                item->m_clockState.anchorSeconds = scenario.position + i / 60.0;
                item->m_clockState.anchorNs = monotonicNowNs();
                item->m_clockState.rate = 0.0;
            }
            elapsed.restart();
            node = item->updatePaintNode(node, nullptr);
            ms.push_back(static_cast<double>(elapsed.nsecsElapsed()) / 1e6);
            const SpectrogramSceneStatistics frame = SpectrogramItem::sceneStatistics(node);
            textures += frame.canvasTextures;
            uploads += frame.canvasUploads;
            bytes += frame.canvasTextureBytes;
            nodeChanges += frame.textureNodeChanges;
            overlayTextures += frame.overlayTextures;
            overlayBytes += frame.overlayTextureBytes;
            QVERIFY(QTest::qWaitFor([item]() { return !spectrogramRasterPending(*item); }, 5000));
        }
        qInfo("playback %s scale=%g frames_per_s=%.1f render_mean_ms=%.3f textures_per_frame=%.2f "
              "uploads_per_frame=%.2f texture_kib_per_frame=%.1f overlay_textures_per_frame=%.2f "
              "overlay_kib_per_frame=%.2f node_changes_per_frame=%.1f sync_mean_ms=%.3f "
              "sync_worst_ms=%.3f",
            scenario.name, window.effectiveDevicePixelRatio(), framesPerSecond, renderMean,
            static_cast<double>(textures) / iterations,
            static_cast<double>(uploads) / iterations,
            static_cast<double>(bytes) / 1024.0 / iterations,
            static_cast<double>(overlayTextures) / iterations,
            static_cast<double>(overlayBytes) / 1024.0 / iterations,
            static_cast<double>(nodeChanges) / iterations,
            std::accumulate(ms.begin(), ms.end(), 0.0) / iterations,
            *std::max_element(ms.begin(), ms.end()));
        delete node;
        delete item;
        QTest::qWait(100);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramSampleRateSyncsFromPrecomputedChunks() {
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

void SpectrogramPlaybackSmokeTest::spectrogramPreservesNativeRateAcrossModesAndTrackTransitions() {
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
            const int hop = static_cast<int>(
                ferrous_ffi_spectrogram_reference_hop(static_cast<std::uint32_t>(rate)));
            item.feedPrecomputedChunk(data, bins, 0, 1, 0, 1000, rate, hop,
                false, true, generation, true, generation);
            const double nyquist = item.pixelToFrequencyHz(0, 100);
            QVERIFY(std::abs(nyquist - rate / 2.0) < rate * 0.02);
            QCOMPARE(item.m_precomputedSampleRateHz, rate);
            ++generation;
        }
        item.feedPrecomputedChunk(data, bins, 0, 1, 0, 1000, 96000, 2048,
            false, true, 1, true, 1);
        QCOMPARE(item.m_precomputedSampleRateHz, 44100);
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramPlaybackRateSwitchKeepsTheSessionRate() {
    // A decoder that falls back to another rate for single frames has
    // playback report the switch and the switch back. The columns keep
    // their analysis session's rate: nothing is mapped, rasterized or
    // uploaded again, and the frequency grid stays as it was.
    for (const int mode : {1, 0}) {
        QQuickWindow window;
        window.resize(400, 100);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
        item.setSampleRateHz(48'000);
        prepareWholeTrackPlayback(item, 50.0);
        item.setDisplayMode(mode);
        item.setPlaying(false);
        item.setGridEnabled(true);
        QSGNode *node = paintSettled(item, nullptr);
        node = item.updatePaintNode(node, nullptr);
        QVERIFY(drawnCanvasFragments(node) > 0);
        int mappingHeight = 0;
        {
            QMutexLocker lock(&item.m_stateMutex);
            mappingHeight = item.m_mappingHeight;
            QVERIFY(mappingHeight > 0);
            QCOMPARE(item.m_freqGridCachedSampleRateHz, 48'000);
        }
        QSignalSpy rateChanged(&item, &SpectrogramItem::sampleRateHzChanged);
        for (const int playbackRate : {44'100, 48'000, 96'000, 48'000}) {
            item.setSampleRateHz(playbackRate);
            {
                QMutexLocker lock(&item.m_stateMutex);
                QCOMPARE(item.m_sampleRateHz, 48'000);
                QCOMPARE(item.m_mappingHeight, mappingHeight);
                QVERIFY(!item.m_precomputedCanvasDirty);
                QVERIFY(!item.m_freqGridDirty);
            }
            node = item.updatePaintNode(node, nullptr);
            QVERIFY(!spectrogramRasterPending(item));
            const SpectrogramSceneStatistics statistics =
                SpectrogramItem::sceneStatistics(node);
            QCOMPARE(statistics.canvasUploads, 0);
            QCOMPARE(statistics.canvasTextures, 0);
            QCOMPARE(statistics.overlayTextures, 0);
        }
        QCOMPARE(rateChanged.count(), 0);
        QCOMPARE(item.sampleRateHz(), 48'000);

        // A mode switch while playback reports the fallback rate keeps the
        // session's rate too.
        item.setSampleRateHz(44'100);
        item.setDisplayMode(mode == 0 ? 1 : 0);
        node = paintSettled(item, node);
        QCOMPARE(item.sampleRateHz(), 48'000);
        QVERIFY(std::abs(item.pixelToFrequencyHz(0, mappingHeight) - 24'000.0) < 500.0);
        delete node;
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramSessionRateChangeRemapsTheSpectrogram() {
    QQuickWindow window;
    window.resize(400, 100);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    SpectrogramItem &item = *new SpectrogramItem(window.contentItem());
    // Before any session, as for the first track after startup, playback's
    // rate is the spectrogram's.
    item.setSampleRateHz(96'000);
    QCOMPARE(item.sampleRateHz(), 96'000);
    item.setSampleRateHz(48'000);
    QCOMPARE(item.sampleRateHz(), 48'000);
    prepareWholeTrackPlayback(item, 50.0);
    item.setPlaying(false);
    item.setGridEnabled(true);
    QSGNode *node = paintSettled(item, nullptr);
    node = item.updatePaintNode(node, nullptr);
    int mappingHeight = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        mappingHeight = item.m_mappingHeight;
        QCOMPARE(item.m_freqGridCachedSampleRateHz, 48'000);
    }
    QVERIFY(std::abs(item.pixelToFrequencyHz(0, mappingHeight) - 24'000.0) < 500.0);

    // The next track plays at 96 kHz. Playback reports it before the new
    // session's columns arrive; until then the shown columns keep theirs.
    item.setSampleRateHz(96'000);
    QCOMPARE(item.sampleRateHz(), 48'000);
    // The track change resets the zoom: the new session's reset, then its
    // columns at the reference hop.
    constexpr int bins = 64;
    constexpr int columns = 380;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        0, 10'000, 96'000, 2048, false, true, 2, true, 2);
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x60'), bins, 0, columns,
        columns, 10'000, 96'000, 2048, false, false, 2, true, 2);
    node = paintSettled(item, node);
    {
        QMutexLocker lock(&item.m_stateMutex);
        QCOMPARE(item.m_sampleRateHz, 96'000);
        QCOMPARE(item.m_mappingHeight, mappingHeight);
        QCOMPARE(item.m_freqGridCachedSampleRateHz, 96'000);
    }
    QCOMPARE(item.sampleRateHz(), 96'000);
    QVERIFY(std::abs(item.pixelToFrequencyHz(0, mappingHeight) - 48'000.0) < 1'000.0);

    // Without the session's columns, playback's rate stands in again.
    item.setSampleRateHz(44'100);
    QCOMPARE(item.sampleRateHz(), 96'000);
    QSignalSpy rateChanged(&item, &SpectrogramItem::sampleRateHzChanged);
    item.clearPrecomputed();
    QCOMPARE(rateChanged.count(), 1);
    QCOMPARE(item.sampleRateHz(), 44'100);
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramSignalsWithoutItsStateLocked() {
    SpectrogramItem item;
    StateMutexProbe probe(&item, &item.m_stateMutex);
    item.setWidth(320);
    item.setHeight(180);
    item.setDbRange(90.0);
    item.setLogScale(true);
    item.setSampleRateHz(44'100);
    item.setMaxColumns(1'024);
    item.setShowFpsOverlay(true);
    item.setCrosshairEnabled(true);
    item.setGridEnabled(true);
    item.setShowTimeLabels(true);
    item.setCrosshairSharedX(20.0);
    item.setChannelMuted(true);
    item.setZoomEnabled(true);
    item.setDisplayMode(1);
    item.setPlaying(true);
    constexpr int bins = 8;
    constexpr int columns = 2'000;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48'000, 1'024, true, true, 1, true, 1);
    QVERIFY(item.precomputedReady());
    setTestPosition(item, 10.0);
    item.setZoomLevel(2.0);
    QVERIFY(sendPendingBackendZoom(item));
    QHoverEvent enter(QEvent::HoverEnter, QPointF(60.0, 40.0), QPointF(60.0, 40.0), QPointF());
    item.hoverEnterEvent(&enter);
    QHoverEvent move(QEvent::HoverMove, QPointF(70.0, 40.0), QPointF(70.0, 40.0), QPointF());
    item.hoverMoveEvent(&move);
    QMouseEvent seek(QEvent::MouseButtonPress, QPointF(70.0, 40.0), QPointF(70.0, 40.0),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    item.mousePressEvent(&seek);
    QWheelEvent wheel(QPointF(70.0, 40.0), QPointF(70.0, 40.0), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    item.wheelEvent(&wheel);
    QMouseEvent reset(QEvent::MouseButtonPress, QPointF(70.0, 40.0), QPointF(70.0, 40.0),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    item.mousePressEvent(&reset);
    QHoverEvent leave(QEvent::HoverLeave, QPointF(), QPointF(), QPointF());
    item.hoverLeaveEvent(&leave);
    item.setWidth(400);
    item.setPlaying(false);
    item.clearPrecomputed();
    QVERIFY(!item.precomputedReady());
    QVERIFY(probe.deliveries >= 20);
    QVERIFY2(probe.heldSignals.isEmpty(), qPrintable(probe.heldSignals.join(QLatin1String(", "))));
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredViewMatchesTheDecodersModel() {
    // The cases the decoder's model of the view is tested against too.
    QFile file(QStringLiteral(FERROUS_UI_SOURCE_DIR "/../src/analysis/fixtures/centered-view.txt"));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    constexpr double columnsPerSecond = 100.0;
    int checked = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QStringList values = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(values.size(), 4);
        const auto columnsOf = [&values](int index) {
            return static_cast<qint64>(std::llround(values.at(index).toDouble() * columnsPerSecond));
        };
        const qint64 total = columnsOf(2);
        // The whole track decoded, as the model assumes.
        const SpectrogramItem::CenteredDisplayRange range = SpectrogramItem::centeredDisplayRange(
            columnsOf(0), static_cast<int>(columnsOf(1)), total, total, columnsPerSecond);
        QVERIFY2(range.left == columnsOf(3),
            qPrintable(QStringLiteral("%1: starts at column %2").arg(line).arg(range.left)));
        ++checked;
    }
    QVERIFY(checked >= 9);
}

void SpectrogramPlaybackSmokeTest::spectrogramPlaybackTravelCountsOnlyContinuousPlayback() {
    TestTime time;
    SpectrogramItem item;
    time.drive(item);
    item.setWidth(200);
    item.setHeight(50);
    item.setDisplayMode(1);
    item.setPlaying(true);
    setTestPosition(item, 10.0);
    const auto travel = [&item, &time]() {
        QMutexLocker lock(&item.m_stateMutex);
        return item.playbackTravelSecondsLocked(
            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(time.nowNs)));
    };
    const double start = travel();

    // Two seconds of playback, with a course correction on the way: the
    // second runs 5% fast to make up a tenth of a second.
    time.advance(1.0);
    PlaybackClock &clock = testClock(item);
    PlaybackSample sample;
    sample.positionSeconds = clock.positionAt(clock.nowNs()) + 0.1;
    sample.sampledAtNs = clock.nowNs();
    sample.advancing = true;
    sample.trackToken = clock.state().trackToken;
    clock.applySample(sample, false);
    time.advance(1.0);
    const double played = travel() - start;
    QVERIFY2(std::abs(played - 2.05) < 1e-6, qPrintable(QString::number(played)));

    // A pause adds nothing, however long; nor do seeks either way.
    item.setPlaying(false);
    setTestPosition(item, clock.positionAt(clock.nowNs()));
    time.advance(100.0);
    QVERIFY(std::abs(travel() - start - played) < 1e-6);
    item.setPlaying(true);
    setTestPosition(item, 80.0);
    QVERIFY(std::abs(travel() - start - played) < 1e-6);
    time.advance(0.5);
    setTestPosition(item, 20.0);
    time.advance(0.5);
    const double resumed = travel() - start - played;
    QVERIFY2(std::abs(resumed - 1.0) < 1e-6, qPrintable(QString::number(resumed)));
}

void SpectrogramPlaybackSmokeTest::spectrogramHighRatePlaybackAdvancesWithoutRebuilds_data() {
    QTest::addColumn<bool>("rolling");
    // Device pixels to a logical pixel.
    QTest::addColumn<double>("scale");
    const std::pair<const char *, double> scales[] = {{"1x", 1.0}, {"2x", 2.0}, {"1.5x", 1.5}};
    for (const auto &[name, scale] : scales) {
        QTest::addRow("rolling %s", name) << true << scale;
        QTest::addRow("centered %s", name) << false << scale;
    }
}

void SpectrogramPlaybackSmokeTest::spectrogramHighRatePlaybackAdvancesWithoutRebuilds() {
    QFETCH(bool, rolling);
    QFETCH(double, scale);
    constexpr int logicalWidth = 1000;
    const int width = static_cast<int>(std::floor(logicalWidth * scale + 1e-6));
    TestTime time;
    SpectrogramItem item;
    time.drive(item);
    item.m_canvasScale = scale;
    prepareHighRatePlayback(item, logicalWidth, 50, 8, rolling);
    QSGNode *node = paintSettled(item, nullptr);
    quint64 serial = 0;
    {
        QMutexLocker lock(&item.m_stateMutex);
        serial = item.m_rasterSerial;
    }

    // Painting at about 30 fps, playback moves the view about 200 pixels a
    // frame: the paint draws them in, and asks a worker for nothing.
    for (int frame = 0; frame < 10; ++frame) {
        time.advance(0.033);
        node = item.updatePaintNode(node, nullptr);
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY2(!item.m_rasterPending && item.m_rasterSerial == serial,
                 qPrintable(QStringLiteral("frame %1 asked for a rasterization").arg(frame)));
        // The canvas is the view's, to its right edge, or as far as its
        // part of a pixel scrolled leaves it.
        QCOMPARE(item.m_canvasFilledCols, width);
        if (rolling) {
            QVERIFY(item.m_precomputedCanvasDisplayRight
                >= item.m_precomputedLastDisplaySeq - (item.m_precomputedCanvasSubpixelOffset > 0.0 ? 1 : 0));
            QCOMPARE(item.m_precomputedCanvasViewRight, item.m_precomputedLastDisplaySeq);
        } else {
            QVERIFY(item.m_precomputedCanvasDisplayLeft <= item.m_precomputedLastDisplaySeq);
            QVERIFY(item.m_precomputedCanvasDisplayRight >= item.m_precomputedLastDisplaySeq);
        }
    }
    QVERIFY2(item.m_precomputedLastDisplaySeq >= 3000 + 10 * 33 * 6 - 1,
             qPrintable(QString::number(item.m_precomputedLastDisplaySeq)));
    delete node;
}

void SpectrogramPlaybackSmokeTest::spectrogramCenteredModeUsesWindowedCapacity() {
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

void SpectrogramPlaybackSmokeTest::spectrogramRollingModeKeepsViewportHeadroomBeyondLookahead() {
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

void SpectrogramPlaybackSmokeTest::spectrogramTrackChangeToAHighRateKeepsZoomAtItsReferenceHop() {
    // A 96 kHz track at zoom 1 is 2048 samples a column: its reference
    // hop, so a track change to it is no reason to reset the zoom.
    for (const int displayMode : {0, 1}) {
        SpectrogramItem item;
        item.setWidth(1200);
        item.setHeight(200);
        item.setDisplayMode(displayMode);
        QSignalSpy backendZoomSpy(&item, &SpectrogramItem::backendZoomRequested);
        QSignalSpy zoomResetSpy(&item, &SpectrogramItem::zoomResetRequested);
        constexpr int bins = 4;
        item.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, 0, 10064, 48000, 1024, false, true, 3);
        item.feedPrecomputedChunk(
            QByteArray(4 * bins, '\x10'), bins, 0, 4, 0, 10064,
            48000, 1024, false, false, 3);
        item.feedPrecomputedChunk(
            QByteArray(), bins, 0, 0, 0, 10064, 96000, 2048, false, true, 7);
        item.feedPrecomputedChunk(
            QByteArray(16 * bins, '\x40'), bins, 0, 16, 0, 10064,
            96000, 2048, false, false, 7);
        QCOMPARE(backendZoomSpy.count(), 0);
        QCOMPARE(zoomResetSpy.count(), 0);
        QCOMPARE(item.m_renderZoomLevel, 1.0);
        QMutexLocker lock(&item.m_stateMutex);
        QVERIFY(std::abs(item.effectiveZoomLocked() - 1.0) < 1e-9);
    }
}

std::unique_ptr<QmlSmokeTest> makeSpectrogramPlaybackSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<SpectrogramPlaybackSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_spectrogram_playback.moc"
