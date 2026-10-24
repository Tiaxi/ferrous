// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlaybackClock.h"

#include <QChar>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace {

constexpr double kNanosPerSecond = 1'000'000'000.0;

} // namespace

qint64 monotonicNowNs() {
    timespec now{};
    // CLOCK_MONOTONIC is always available on Linux; a failure would leave
    // the reading at zero, which only makes samples look old.
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<qint64>(now.tv_sec) * 1'000'000'000LL + static_cast<qint64>(now.tv_nsec);
}

qint64 monotonicNs(std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

double PlaybackClockState::positionAt(qint64 nowNs) const {
    const double elapsed = nowNs > anchorNs
        ? static_cast<double>(nowNs - anchorNs) / kNanosPerSecond
        : 0.0;
    double position = std::max(0.0, anchorSeconds + elapsed * rate);
    if (durationSeconds > 0.0) {
        position = std::min(position, durationSeconds);
    }
    return position;
}

PlaybackClock::PlaybackClock(QObject *parent)
    : QObject(parent),
      m_timeSource(monotonicNowNs) {
    m_frameTimer.setInterval(kFrameIntervalMs);
    m_frameTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_frameTimer, &QTimer::timeout, this, &PlaybackClock::refresh);
}

void PlaybackClock::setTimeSource(TimeSource source) {
    m_timeSource = source ? std::move(source) : TimeSource(monotonicNowNs);
    refresh();
}

qint64 PlaybackClock::nowNs() const {
    return m_timeSource();
}

void PlaybackClock::applySample(const PlaybackSample &sample, bool jump) {
    const qint64 now = nowNs();
    PlaybackClockState sampled = m_state;
    sampled.durationSeconds = std::isfinite(sample.durationSeconds)
        ? std::max(0.0, sample.durationSeconds)
        : 0.0;
    sampled.trackToken = sample.trackToken;
    sampled.anchorSeconds = std::isfinite(sample.positionSeconds)
        ? std::max(0.0, sample.positionSeconds)
        : 0.0;
    sampled.anchorNs = sample.sampledAtNs > 0 ? sample.sampledAtNs : now;
    sampled.rate = sample.advancing ? 1.0 : 0.0;

    const bool trackChanged = sample.trackToken != m_state.trackToken;
    const double displayed = m_state.positionAt(now);
    const double error = sampled.positionAt(now) - displayed;
    if (!jump && !trackChanged && sample.advancing && m_state.advancing()
        && m_state.anchorNs > now && error <= 0.0) {
        // The position waits for a start that has not come, such as a
        // gapless successor's, and the sample does not show playback past
        // that start yet, such as a repeat of the first sample. Starting to
        // advance now would run ahead of the audio.
        PlaybackClockState scheduled = m_state;
        scheduled.durationSeconds = sampled.durationSeconds;
        setState(scheduled);
        return;
    }
    if (!jump && !trackChanged && sample.advancing && m_state.advancing()
        && std::abs(error) <= kMaxSlewErrorSeconds) {
        // Make the difference up by the next samples, from where the
        // position is shown now, so that it neither steps nor goes back.
        PlaybackClockState slewed = sampled;
        slewed.anchorSeconds = displayed;
        slewed.anchorNs = now;
        slewed.rate = 1.0 + std::clamp(
            error / kSlewHorizonSeconds, -kMaxRateOffset, kMaxRateOffset);
        slewed.epoch = m_state.epoch;
        setState(slewed);
        return;
    }
    const bool discontinuity = trackChanged
        || (jump && std::abs(error) >= kJumpThresholdSeconds)
        || std::abs(error) > kMaxSlewErrorSeconds;
    sampled.epoch = m_state.epoch + (discontinuity ? 1 : 0);
    setState(sampled);
}

void PlaybackClock::holdAt(double positionSeconds) {
    PlaybackClockState held = m_state;
    held.anchorSeconds = std::isfinite(positionSeconds) ? std::max(0.0, positionSeconds) : 0.0;
    held.anchorNs = nowNs();
    held.rate = 0.0;
    held.epoch = m_state.epoch + 1;
    setState(held);
}

void PlaybackClock::setDurationSeconds(double durationSeconds) {
    const double duration = std::isfinite(durationSeconds) ? std::max(0.0, durationSeconds) : 0.0;
    if (duration == m_state.durationSeconds) {
        return;
    }
    PlaybackClockState next = m_state;
    next.durationSeconds = duration;
    setState(next);
}

void PlaybackClock::setState(const PlaybackClockState &state) {
    m_state = state;
    if (m_state.advancing()) {
        if (!m_frameTimer.isActive()) {
            m_frameTimer.start();
        }
    } else {
        m_frameTimer.stop();
    }
    emit stateChanged();
    refresh();
}

void PlaybackClock::refresh() {
    const double position = m_state.positionAt(nowNs());
    if (position != m_positionSeconds) {
        m_positionSeconds = position;
        emit positionChanged();
    }
    QString text = formatSeconds(position);
    if (text != m_positionText) {
        m_positionText = std::move(text);
        emit positionTextChanged();
    }
}

QString PlaybackClock::formatSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}
