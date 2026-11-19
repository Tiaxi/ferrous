// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

// How a zoomed plot's view is placed against the playhead: it follows the
// playhead, stays where a middle-drag moved it, or glides back to the
// playhead from there. The waveform editor and the centered spectrogram
// share this arithmetic; each reckons from its own view where a detached
// view's playhead starts the glide back (its re-attach point).
namespace ViewFollow {

enum class Mode { Playhead, Detached, Gliding };

struct State {
    Mode mode{Mode::Playhead};
    // Gliding: where the playhead was as the glide began.
    double glideFrom{0.0};
};

// Where in a detached view's width the playhead, moving through it, starts
// the glide back to following it.
inline constexpr double kReattachViewFraction = 0.85;
// The playback, in seconds of the track, over which the glide brings the
// playhead from kReattachViewFraction to the middle: about 2 s at normal
// speed.
inline constexpr double kReattachGlideSeconds = 2.0;
// How near the time the plot asked to seek to a jump of the playhead lands
// for it to be that seek, which keeps a detached view.
inline constexpr double kOwnSeekToleranceSeconds = 0.5;

// The glide's ease, from 0 to 1 as `progress` goes from 0 to 1, and its
// slope: smooth at both ends, so that the view takes up the playhead at
// kReattachViewFraction at playback's speed and hands it over at the middle
// at the same.
inline double glideEase(double progress) {
    return progress * progress * (3.0 - 2.0 * progress);
}

inline double glideEaseSlope(double progress) {
    return 6.0 * progress * (1.0 - progress);
}

// How far a glide begun with the playhead at `from` has come with the
// playhead at `position`, from 0 to 1.
inline double glideProgress(double position, double from) {
    return std::clamp((position - from) / kReattachGlideSeconds, 0.0, 1.0);
}

// Where in the view's width the playhead is while the view glides back:
// from kReattachViewFraction to the middle, so that it never leaves the
// view, however deep the zoom.
inline double glidePlayheadFraction(double position, double from) {
    return kReattachViewFraction
        + (0.5 - kReattachViewFraction) * glideEase(glideProgress(position, from));
}

// How fast a view `span` seconds wide moves while it glides back, over
// playback's speed: faster than playback, as the playhead moves to the
// middle, and at playback's at both ends.
inline double glideViewSpeed(double span, double position, double from) {
    return 1.0 + (kReattachViewFraction - 0.5) * span
        * glideEaseSlope(glideProgress(position, from)) / kReattachGlideSeconds;
}

// How a view in `state` is placed with the playhead at `position`, purely
// from the playhead: a detached view whose playhead has reached `reattach`,
// its re-attach point (infinity for none), glides back from there, and a
// glide that has taken kReattachGlideSeconds of playback follows. The
// frame that finds the playhead there glides, whenever the state takes
// that in.
inline State advance(State state, double position, double reattach) {
    if (state.mode == Mode::Detached && position >= reattach) {
        state = {Mode::Gliding, reattach};
    }
    if (state.mode == Mode::Gliding && position >= state.glideFrom + kReattachGlideSeconds) {
        state = {Mode::Playhead, 0.0};
    }
    return state;
}

} // namespace ViewFollow
