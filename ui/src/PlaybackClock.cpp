// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlaybackClock.h"

#include <QChar>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>

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

std::optional<qint64> PlaybackClockState::reachesAt(double position) const {
    if (positionAt(anchorNs) >= position) {
        return std::numeric_limits<qint64>::min();
    }
    if (!advancing() || (durationSeconds > 0.0 && position > durationSeconds)) {
        return std::nullopt;
    }
    const double untilNs = std::ceil((position - anchorSeconds) / rate * kNanosPerSecond);
    if (untilNs >= static_cast<double>(std::numeric_limits<qint64>::max() - anchorNs)) {
        return std::nullopt;
    }
    return anchorNs + static_cast<qint64>(untilNs);
}

PlaybackClock::PlaybackClock(QObject *parent)
    : QObject(parent),
      m_timeSource(monotonicNowNs) {
    m_tickTimer.setSingleShot(true);
    m_tickTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_tickTimer, &QTimer::timeout, this, &PlaybackClock::tick);
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
    const PlaybackClockState previous = m_state;
    m_state = state;
    emit stateChanged();
    const bool jumped = state.epoch != previous.epoch || state.trackToken != previous.trackToken;
    // A correction of the advancing position moves positionSeconds by its
    // steps; anything else shows where the position is now.
    update(jumped
           || state.advancing() != previous.advancing()
           || state.durationSeconds != previous.durationSeconds);
    if (jumped) {
        emit positionJumped();
    }
}

void PlaybackClock::setFrameUpdatesEnabled(bool enabled) {
    if (m_frameUpdatesEnabled == enabled) {
        return;
    }
    m_frameUpdatesEnabled = enabled;
    emit frameUpdatesEnabledChanged();
    // The position catches up with the samples taken without frames.
    update(true);
}

void PlaybackClock::setPositionStepSeconds(double stepSeconds) {
    const double step = std::isfinite(stepSeconds) ? std::max(0.0, stepSeconds) : 0.0;
    if (step == m_positionStepSeconds) {
        return;
    }
    m_positionStepSeconds = step;
    emit positionStepSecondsChanged();
    update(true);
}

void PlaybackClock::refresh() {
    update(true);
}

void PlaybackClock::tick() {
    update(false);
}

bool PlaybackClock::showsMove(double shown, double position) const {
    if (m_positionStepSeconds <= 0.0) {
        return position != shown;
    }
    // The steps the position is nearest to, as a view rounds it to them.
    return std::floor(position / m_positionStepSeconds + 0.5)
        != std::floor(shown / m_positionStepSeconds + 0.5);
}

void PlaybackClock::update(bool exact) {
    const qint64 now = nowNs();
    const double position = m_state.positionAt(now);
    if (position != m_positionSeconds && (exact || showsMove(m_positionSeconds, position))) {
        m_positionSeconds = position;
        emit positionChanged();
    }
    QString text = formatSeconds(position);
    if (text != m_positionText) {
        m_positionText = std::move(text);
        m_positionSecond = wholeSecond(position);
        emit positionTextChanged();
    }
    scheduleTick(now, position);
}

void PlaybackClock::scheduleTick(qint64 now, double position) {
    if (!m_state.advancing() || !m_frameUpdatesEnabled) {
        m_tickTimer.stop();
        m_nextTickNs.reset();
        return;
    }
    std::optional<qint64> next;
    const auto consider = [&next](std::optional<qint64> at) {
        if (at.has_value() && (!next.has_value() || *at < *next)) {
            next = at;
        }
    };
    // Just past a boundary, so that rounding cannot leave the position
    // short of it then.
    constexpr double kPastBoundarySeconds = 1e-6;
    // formatSeconds rounds to whole seconds, so the text changes at halves.
    consider(m_state.reachesAt(std::floor(position + 0.5) + 0.5 + kPastBoundarySeconds));
    if (m_positionStepSeconds > 0.0) {
        const double step = m_positionStepSeconds;
        consider(m_state.reachesAt(
            (std::floor(position / step + 0.5) + 0.5) * step + kPastBoundarySeconds));
    } else {
        consider(now + static_cast<qint64>(kFrameIntervalMs) * 1'000'000);
    }
    if (!next.has_value()) {
        // The position stops at the end of the track before either changes.
        m_tickTimer.stop();
        m_nextTickNs.reset();
        return;
    }
    // Rounded up to whole milliseconds, and at least one, so that the clock
    // cannot wake in a loop.
    constexpr qint64 kMaxIntervalMs = std::numeric_limits<int>::max();
    const qint64 untilNs = *next > now ? *next - now : 0;
    const qint64 intervalMs = std::clamp<qint64>((untilNs + 999'999) / 1'000'000, 1, kMaxIntervalMs);
    m_tickTimer.start(static_cast<int>(intervalMs));
    m_nextTickNs = now + intervalMs * 1'000'000;
}

QString PlaybackClock::formatSeconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = wholeSecond(seconds);
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

int PlaybackClock::wholeSecond(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return 0;
    }
    return static_cast<int>(std::min(seconds + 0.5, static_cast<double>(std::numeric_limits<int>::max())));
}
