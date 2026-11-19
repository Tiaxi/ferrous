// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramViewFollow.h"

#include <QMutexLocker>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

qint64 steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool sameClockState(const PlaybackClockState &a, const PlaybackClockState &b) {
    return a.anchorSeconds == b.anchorSeconds && a.anchorNs == b.anchorNs && a.rate == b.rate
        && a.durationSeconds == b.durationSeconds && a.trackToken == b.trackToken
        && a.epoch == b.epoch;
}

} // namespace

SpectrogramViewFollow::SpectrogramViewFollow(QObject *parent)
    : QObject(parent) {
    m_anchorTimer.setSingleShot(true);
    connect(&m_anchorTimer, &QTimer::timeout, this, &SpectrogramViewFollow::sendBackendAnchor);
}

SpectrogramViewFollow::Snapshot SpectrogramViewFollow::snapshot() const {
    QMutexLocker lock(&m_mutex);
    Snapshot snapshot;
    snapshot.state = m_state;
    snapshot.anchorSeconds = m_anchorSeconds;
    snapshot.anchorFraction = m_anchorFraction;
    if (m_state.mode == ViewFollow::Mode::Detached && m_reattachArmed && !m_panning) {
        snapshot.reattachSeconds = reattachPointLocked();
    }
    return snapshot;
}

ViewFollow::State SpectrogramViewFollow::stateAt(const Snapshot &snapshot, double position) {
    return ViewFollow::advance(snapshot.state, position, snapshot.reattachSeconds);
}

void SpectrogramViewFollow::detach(double anchorSeconds, double fraction) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_state.mode != ViewFollow::Mode::Detached) {
            m_ownSeekSeconds = std::numeric_limits<double>::quiet_NaN();
            m_reattachArmed = false;
        }
        m_state = {ViewFollow::Mode::Detached, 0.0};
        m_anchorSeconds = std::max(0.0, anchorSeconds);
        m_anchorFraction = std::clamp(fraction, 0.0, 1.0);
    }
    emit changed();
    publish();
}

void SpectrogramViewFollow::setPanning(bool panning) {
    QMutexLocker lock(&m_mutex);
    m_panning = panning;
    if (panning) {
        // Nothing glides back until the user lets go.
        m_reattachArmed = false;
    }
}

bool SpectrogramViewFollow::panning() const {
    QMutexLocker lock(&m_mutex);
    return m_panning;
}

void SpectrogramViewFollow::setGeometry(
    double spanSeconds, double endSeconds, bool wholeTrack, double position) {
    {
        QMutexLocker lock(&m_mutex);
        if (!wholeTrack && spanSeconds == m_spanSeconds && endSeconds == m_endSeconds) {
            return;
        }
        m_spanSeconds = spanSeconds;
        m_endSeconds = endSeconds;
        if (m_state.mode == ViewFollow::Mode::Playhead) {
            return;
        }
        if (wholeTrack) {
            // A view of the whole track has nothing to follow.
            reattachLocked();
        } else {
            // A playhead the new width puts past the re-attach point stays.
            armLocked(position);
        }
    }
    emit changed();
    publish();
}

void SpectrogramViewFollow::arm(double position) {
    {
        QMutexLocker lock(&m_mutex);
        armLocked(position);
    }
    emit changed();
    publish();
}

void SpectrogramViewFollow::armLocked(double position) {
    // A view the user still drags stays where it is put.
    if (m_state.mode != ViewFollow::Mode::Detached || m_panning) {
        return;
    }
    // A playhead past the point already, or past the view, leaves the view
    // where it was put.
    m_reattachArmed = position < reattachPointLocked();
}

void SpectrogramViewFollow::settle(double position) {
    {
        QMutexLocker lock(&m_mutex);
        if (m_state.mode == ViewFollow::Mode::Playhead) {
            return;
        }
        const double reattach = m_state.mode == ViewFollow::Mode::Detached && m_reattachArmed
                && !m_panning
            ? reattachPointLocked()
            : std::numeric_limits<double>::infinity();
        const ViewFollow::State next = ViewFollow::advance(m_state, position, reattach);
        if (next.mode == m_state.mode) {
            return;
        }
        if (next.mode == ViewFollow::Mode::Playhead) {
            reattachLocked();
        } else {
            m_state = next;
            m_ownSeekSeconds = std::numeric_limits<double>::quiet_NaN();
        }
    }
    emit changed();
    publish();
}

void SpectrogramViewFollow::noteOwnSeek(double seconds) {
    QMutexLocker lock(&m_mutex);
    if (m_state.mode == ViewFollow::Mode::Detached) {
        m_ownSeekSeconds = seconds;
        m_ownSeekLanded = false;
    }
}

void SpectrogramViewFollow::clockChanged(const PlaybackClockState &next, qint64 nowNs) {
    bool placementChanged = false;
    {
        QMutexLocker lock(&m_mutex);
        if (m_clockKnown && sameClockState(next, m_clockState)) {
            // Another lane told it already.
            return;
        }
        const PlaybackClockState previous = m_clockState;
        const bool known = m_clockKnown;
        m_clockState = next;
        m_clockKnown = true;
        if (!known || m_state.mode == ViewFollow::Mode::Playhead) {
            return;
        }
        const bool trackChanged = next.trackToken != previous.trackToken;
        const bool explicitSeek = next.epoch != previous.epoch || trackChanged;
        const double previousPosition = previous.positionAt(nowNs);
        const double position = next.positionAt(nowNs);
        const auto near = [this](double seconds) {
            return std::abs(seconds - m_ownSeekSeconds) <= ViewFollow::kOwnSeekToleranceSeconds;
        };
        const bool ownSeek = !std::isnan(m_ownSeekSeconds) && near(position)
            && (!explicitSeek || !m_ownSeekLanded || near(previousPosition));
        if (explicitSeek) {
            // The plot's own seek, as it lands and as the engine confirms
            // it, keeps a detached view where it is. Any other jump, such
            // as a seek from the seek bar, the keyboard or MPRIS, a stop or
            // a track change, including a gapless successor and Repeat-One,
            // brings it back to the playhead.
            if (ownSeek && !trackChanged && m_state.mode == ViewFollow::Mode::Detached) {
                m_ownSeekLanded = true;
                armLocked(position);
            } else {
                reattachLocked();
            }
            placementChanged = true;
        } else if (!ownSeek) {
            // Playback has moved on from it: a later seek there is another's.
            m_ownSeekSeconds = std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (placementChanged) {
        emit changed();
    }
    publish();
}

void SpectrogramViewFollow::reattach() {
    {
        QMutexLocker lock(&m_mutex);
        if (m_state.mode == ViewFollow::Mode::Playhead) {
            return;
        }
        reattachLocked();
    }
    emit changed();
    publish();
}

void SpectrogramViewFollow::reattachLocked() {
    m_state = {ViewFollow::Mode::Playhead, 0.0};
    m_reattachArmed = false;
    m_panning = false;
    m_ownSeekSeconds = std::numeric_limits<double>::quiet_NaN();
}

double SpectrogramViewFollow::detachedLeftLocked() const {
    const double left = m_anchorSeconds - m_anchorFraction * m_spanSeconds;
    const double lastLeft = m_endSeconds > 0.0
        ? std::max(0.0, m_endSeconds - m_spanSeconds)
        : std::numeric_limits<double>::infinity();
    return std::clamp(left, 0.0, lastLeft);
}

double SpectrogramViewFollow::reattachPointLocked() const {
    if (m_spanSeconds <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return detachedLeftLocked() + ViewFollow::kReattachViewFraction * m_spanSeconds;
}

void SpectrogramViewFollow::publish() {
    bool detached = false;
    {
        QMutexLocker lock(&m_mutex);
        detached = m_state.mode == ViewFollow::Mode::Detached;
    }
    if (detached != m_detached) {
        m_detached = detached;
        emit detachedChanged();
    }
    sendBackendAnchor();
}

void SpectrogramViewFollow::sendBackendAnchor() {
    quint64 token = 0;
    double center = std::numeric_limits<double>::quiet_NaN();
    {
        QMutexLocker lock(&m_mutex);
        token = m_clockState.trackToken;
        if (m_state.mode == ViewFollow::Mode::Detached && m_spanSeconds > 0.0) {
            center = detachedLeftLocked() + 0.5 * m_spanSeconds;
        }
    }
    const bool follows = std::isnan(center);
    if (follows && std::isnan(m_sentCenter)) {
        // The analysis follows the playhead of whichever track it decodes.
        m_anchorTimer.stop();
        return;
    }
    if (!follows && token == m_sentToken && center == m_sentCenter) {
        return;
    }
    const qint64 nowMs = steadyMs();
    const qint64 sinceMs = nowMs - m_sentAtMs;
    if (!follows && token == m_sentToken && !std::isnan(m_sentCenter)
        && sinceMs < kBackendAnchorIntervalMs) {
        // A drag: the latest anchor goes once the interval is over.
        if (!m_anchorTimer.isActive()) {
            m_anchorTimer.start(static_cast<int>(kBackendAnchorIntervalMs - sinceMs));
        }
        return;
    }
    m_anchorTimer.stop();
    m_sentToken = token;
    m_sentCenter = center;
    m_sentAtMs = nowMs;
    emit backendAnchorChanged(token, center);
}
