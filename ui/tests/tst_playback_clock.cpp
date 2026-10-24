// SPDX-License-Identifier: GPL-3.0-or-later

#include <QSignalSpy>
#include <QtTest/QtTest>

#include <cmath>
#include <cstdint>

#include "PlaybackClock.h"

namespace {

constexpr qint64 kMs = 1'000'000;
// Engine time stamps are large; start far from zero like a real system.
constexpr qint64 kStart = 5'000'000 * kMs;

PlaybackSample sample(double position, qint64 sampledAtNs, bool advancing,
                      quint64 token = 1, double duration = 600.0) {
    PlaybackSample out;
    out.positionSeconds = position;
    out.sampledAtNs = sampledAtNs;
    out.advancing = advancing;
    out.durationSeconds = duration;
    out.trackToken = token;
    return out;
}

double seconds(qint64 nanos) {
    return static_cast<double>(nanos) / 1e9;
}

} // namespace

class PlaybackClockTest : public QObject {
    Q_OBJECT

private:
    qint64 m_now{kStart};

    void attach(PlaybackClock &clock) {
        m_now = kStart;
        clock.setTimeSource([this]() { return m_now; });
    }

private slots:
    void monotonicClockIsTheSteadyClock();
    void steadyPlaybackAdvancesInRealTime();
    void lateDeliveryDoesNotDelayThePosition();
    void pauseHoldsAndResumeStartsFromTheNewSample();
    void seekHoldsTheTargetUntilTheEngineLands();
    void seekLandingElsewhereStartsOverFromTheLanding();
    void trackChangeStartsOverWithoutSmoothing();
    void gaplessSuccessorWaitsForItsStart();
    void jitterNeverMovesThePositionBackwards();
    void quantizedSinkPositionsDoNotBiasTheClock();
    void stallOfThePipelineStartsOverFromItsSample();
    void positionStopsAtTheEndOfTheTrack();
    void framesTickOnlyWhileAdvancing();
};

void PlaybackClockTest::monotonicClockIsTheSteadyClock() {
    // The engine stamps samples with CLOCK_MONOTONIC; render code converts
    // steady_clock time points. Both must read the same clock.
    const qint64 before = monotonicNowNs();
    const qint64 steady = monotonicNs(std::chrono::steady_clock::now());
    const qint64 after = monotonicNowNs();
    QVERIFY2(steady >= before && steady <= after,
             qPrintable(QStringLiteral("steady %1 outside [%2, %3]")
                            .arg(steady).arg(before).arg(after)));
}

void PlaybackClockTest::steadyPlaybackAdvancesInRealTime() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(12.0, kStart, true), true);
    for (int tick = 1; tick <= 50; ++tick) {
        const qint64 sampledAt = kStart + tick * 40 * kMs;
        m_now = sampledAt + 3 * kMs;
        clock.applySample(sample(12.0 + tick * 0.040, sampledAt, true), false);
        QCOMPARE(clock.state().rate, 1.0);
        QVERIFY(std::abs(clock.positionAt(m_now) - (12.0 + seconds(m_now - kStart))) < 1e-9);
    }
    // Between samples, the position follows real time.
    m_now += 25 * kMs;
    clock.refresh();
    QVERIFY(std::abs(clock.positionSeconds() - (12.0 + seconds(m_now - kStart))) < 1e-9);
    QCOMPARE(clock.state().epoch, quint64{1});
}

void PlaybackClockTest::lateDeliveryDoesNotDelayThePosition() {
    // The bridge delivers samples late and irregularly; the time stamp tells
    // how old each one is.
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(30.0, kStart, true), true);
    // Delivered in order, so the arrival times do not go back.
    const qint64 delays[] = {2, 45, 30, 80, 50, 45, 120, 90};
    qint64 sampledAt = kStart;
    for (const qint64 delay : delays) {
        sampledAt += 40 * kMs;
        m_now = sampledAt + delay * kMs;
        clock.applySample(sample(30.0 + seconds(sampledAt - kStart), sampledAt, true), false);
        QVERIFY(std::abs(clock.positionAt(m_now) - (30.0 + seconds(m_now - kStart))) < 1e-9);
        QCOMPARE(clock.state().rate, 1.0);
    }
}

void PlaybackClockTest::pauseHoldsAndResumeStartsFromTheNewSample() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(10.0, kStart, true), true);
    m_now = kStart + 500 * kMs;
    QVERIFY(std::abs(clock.positionAt(m_now) - 10.5) < 1e-9);

    // Paused where the engine says, slightly before the extrapolation.
    clock.applySample(sample(10.48, kStart + 480 * kMs, false), false);
    QVERIFY(!clock.advancing());
    QVERIFY(std::abs(clock.positionAt(m_now) - 10.48) < 1e-9);
    m_now += 5'000 * kMs;
    QVERIFY(std::abs(clock.positionAt(m_now) - 10.48) < 1e-9);

    // Resumed: playback is still at 10.48 until the pipeline plays again.
    const qint64 resumedAt = m_now + 30 * kMs;
    clock.applySample(sample(10.48, resumedAt, true), false);
    QVERIFY(clock.advancing());
    QCOMPARE(clock.state().rate, 1.0);
    QVERIFY(std::abs(clock.positionAt(m_now) - 10.48) < 1e-9);
    m_now = resumedAt + 100 * kMs;
    QVERIFY(std::abs(clock.positionAt(m_now) - 10.58) < 1e-9);
    QCOMPARE(clock.state().epoch, quint64{1});
}

void PlaybackClockTest::seekHoldsTheTargetUntilTheEngineLands() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(10.0, kStart, true), true);
    m_now = kStart + 200 * kMs;
    clock.holdAt(95.0);
    QCOMPARE(clock.state().epoch, quint64{2});
    QVERIFY(!clock.advancing());
    m_now += 150 * kMs;
    QCOMPARE(clock.positionAt(m_now), 95.0);

    // The engine landed on the target, and plays from there.
    clock.applySample(sample(95.0, m_now - 10 * kMs, false), true);
    QCOMPARE(clock.positionAt(m_now), 95.0);
    QCOMPARE(clock.state().epoch, quint64{2});
    const qint64 playingAt = m_now + 20 * kMs;
    m_now = playingAt + 5 * kMs;
    clock.applySample(sample(95.0, playingAt, true), false);
    QVERIFY(std::abs(clock.positionAt(m_now) - 95.005) < 1e-9);
    QCOMPARE(clock.state().epoch, quint64{2});
}

void PlaybackClockTest::seekLandingElsewhereStartsOverFromTheLanding() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(10.0, kStart, true), true);
    clock.holdAt(60.0);
    m_now += 100 * kMs;
    // Landed on a key frame before the target, already playing.
    clock.applySample(sample(58.5, m_now - 20 * kMs, true), true);
    QCOMPARE(clock.state().epoch, quint64{3});
    QVERIFY(std::abs(clock.positionAt(m_now) - 58.52) < 1e-9);
    QCOMPARE(clock.state().rate, 1.0);
}

void PlaybackClockTest::trackChangeStartsOverWithoutSmoothing() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(170.0, kStart, true, 4), true);
    m_now = kStart + 40 * kMs;
    // A new track starts near zero; no slewing across the jump, even
    // though the engine did not mark it.
    clock.applySample(sample(0.01, m_now - 2 * kMs, true, 5, 200.0), false);
    QCOMPARE(clock.state().trackToken, quint64{5});
    QCOMPARE(clock.state().durationSeconds, 200.0);
    QCOMPARE(clock.state().epoch, quint64{2});
    QCOMPARE(clock.state().rate, 1.0);
    QVERIFY(std::abs(clock.positionAt(m_now) - 0.012) < 1e-9);

    // A replay of the same file is a new track as well.
    m_now += 3'000 * kMs;
    clock.applySample(sample(0.0, m_now, true, 6, 200.0), false);
    QCOMPARE(clock.state().epoch, quint64{3});
    QCOMPARE(clock.positionAt(m_now), 0.0);
}

void PlaybackClockTest::gaplessSuccessorWaitsForItsStart() {
    // The engine stamps a gapless successor with the time the previous
    // track ends, which can be after the sample arrives.
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(3.8, kStart, true, 1, 4.0), true);
    m_now = kStart + 20 * kMs;
    const qint64 startsAt = kStart + 200 * kMs;
    clock.applySample(sample(0.0, startsAt, true, 2, 4.5), false);
    QCOMPARE(clock.positionAt(m_now), 0.0);
    m_now = startsAt - kMs;
    QCOMPARE(clock.positionAt(m_now), 0.0);
    m_now = startsAt + 50 * kMs;
    QVERIFY(std::abs(clock.positionAt(m_now) - 0.05) < 1e-9);
    // The next sample agrees.
    clock.applySample(sample(0.045, startsAt + 45 * kMs, true, 2, 4.5), false);
    QVERIFY(std::abs(clock.positionAt(m_now) - 0.05) < 1e-9);
    QCOMPARE(clock.state().rate, 1.0);
}

void PlaybackClockTest::jitterNeverMovesThePositionBackwards() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(20.0, kStart, true), true);
    // Samples that scatter around the true position by up to 15 ms, and
    // frames between them.
    const int jitterMs[] = {0, 12, -9, 15, -15, 3, -12, 14, -2, 9, -14, 0, 11, -8};
    double previous = clock.positionAt(m_now);
    double maximumError = 0.0;
    for (int tick = 1; tick <= 200; ++tick) {
        const qint64 sampledAt = kStart + tick * 40 * kMs;
        const double truth = 20.0 + seconds(sampledAt - kStart);
        const int jitter = jitterMs[tick % (sizeof(jitterMs) / sizeof(jitterMs[0]))];
        for (qint64 frame = sampledAt - 40 * kMs; frame < sampledAt; frame += 8 * kMs) {
            m_now = frame;
            const double position = clock.positionAt(m_now);
            QVERIFY2(position >= previous,
                     qPrintable(QStringLiteral("went back from %1 to %2")
                                    .arg(previous, 0, 'f', 6).arg(position, 0, 'f', 6)));
            previous = position;
        }
        m_now = sampledAt;
        clock.applySample(sample(truth + jitter / 1000.0, sampledAt, true), false);
        const double position = clock.positionAt(m_now);
        QVERIFY(position >= previous);
        previous = position;
        QVERIFY(clock.state().rate >= 1.0 - PlaybackClock::kMaxRateOffset);
        maximumError = std::max(maximumError, std::abs(position - truth));
    }
    QCOMPARE(clock.state().epoch, quint64{1});
    QVERIFY2(maximumError < 0.03,
             qPrintable(QStringLiteral("strayed %1 s from playback").arg(maximumError)));
}

void PlaybackClockTest::quantizedSinkPositionsDoNotBiasTheClock() {
    // A 100 ms cadence sees four or five 1024-frame buffers at 44.1 kHz.
    // The media clock advances at exactly 1x; only the reported position
    // is quantized to the last buffer edge.
    PlaybackClock clock;
    attach(clock);
    constexpr std::int64_t kRate = 44'100;
    constexpr std::int64_t kBufferFrames = 1'024;
    clock.applySample(sample(0.0, kStart, true), true);
    for (std::int64_t tick = 1; tick <= 200; ++tick) {
        const std::int64_t elapsedNanos = tick * 100'000'000;
        const std::int64_t frames = elapsedNanos * kRate / 1'000'000'000;
        const std::int64_t reported = frames / kBufferFrames * kBufferFrames;
        m_now = kStart + elapsedNanos;
        clock.applySample(
            sample(static_cast<double>(reported) / static_cast<double>(kRate), m_now, true), false);
    }
    const double lag = 20.0 - clock.positionAt(m_now);
    QVERIFY2(std::abs(lag) < 0.025, qPrintable(QStringLiteral("lag %1 s").arg(lag)));
    QCOMPARE(clock.state().epoch, quint64{1});
}

void PlaybackClockTest::stallOfThePipelineStartsOverFromItsSample() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(50.0, kStart, true), true);
    // The pipeline held its position for a second.
    m_now = kStart + 1'000 * kMs;
    clock.applySample(sample(50.0, m_now, true), false);
    QCOMPARE(clock.state().epoch, quint64{2});
    QCOMPARE(clock.positionAt(m_now), 50.0);
}

void PlaybackClockTest::positionStopsAtTheEndOfTheTrack() {
    PlaybackClock clock;
    attach(clock);
    clock.applySample(sample(3.9, kStart, true, 1, 4.0), true);
    m_now = kStart + 500 * kMs;
    QCOMPARE(clock.positionAt(m_now), 4.0);
    clock.setDurationSeconds(10.0);
    QVERIFY(std::abs(clock.positionAt(m_now) - 4.4) < 1e-9);
}

void PlaybackClockTest::framesTickOnlyWhileAdvancing() {
    PlaybackClock clock;
    QSignalSpy position(&clock, &PlaybackClock::positionChanged);
    QSignalSpy text(&clock, &PlaybackClock::positionTextChanged);
    const qint64 now = monotonicNowNs();
    clock.applySample(sample(61.0, now, false), true);
    QCOMPARE(clock.positionSeconds(), 61.0);
    QCOMPARE(clock.positionText(), QStringLiteral("01:01"));
    QCOMPARE(text.count(), 1);
    // Paused: no frames.
    QVERIFY(!clock.m_frameTimer.isActive());
    position.clear();
    clock.applySample(sample(61.0, monotonicNowNs(), true), false);
    QVERIFY(clock.m_frameTimer.isActive());
    QTRY_VERIFY(position.count() >= 2);
    QVERIFY(clock.positionSeconds() > 61.0);
}

QTEST_MAIN(PlaybackClockTest)
#include "tst_playback_clock.moc"
