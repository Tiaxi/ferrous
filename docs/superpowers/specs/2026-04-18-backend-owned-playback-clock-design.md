# Backend-Owned Playback Clock

## Goal

Make the playback backend the sole owner of the visible playback clock so the spectrogram can keep smooth cadence while staying visually in sync with audio, including after repeated seeks.

## Problem

The current timing stack has two owners:

- `src/playback/backend_gst.rs` exports a smoothed `snapshot.position`
- `ui/qml/controllers/PlaybackController.qml` applies additional interpolation and correction policy on top of that exported position

This split ownership creates a correction loop:

- raw GStreamer `query_position()` is bursty after seeks
- backend smooths those bursts to protect cadence
- QML then trims or bleeds again to protect cadence locally
- one layer reduces visible jumps, but the other layer reintroduces lag or cadence modulation

The diagnostics now show that the system can remove the obvious post-seek jump, but it still leaves visible sync lag after some seeks because backend lag and QML lag stack.

## Decision

Choose a backend-owned playback clock.

The backend will export one authoritative playback position intended for direct display. QML may interpolate between backend heartbeats for frame rate, but it must not perform independent trim, bleed, ignore, or seek-reacquire policy decisions.

The system should bias toward sync over perfectly even cadence. Small visually invisible smoothing is acceptable. Persistent visible lag is not.

## Non-Goals

- Do not change audio playback timing.
- Do not make GStreamer polling perfectly smooth; raw polling is allowed to stay bursty.
- Do not redesign the binary snapshot protocol beyond the minimum needed for clock ownership.
- Do not change spectrogram rendering semantics in rolling vs centered mode.

## Ownership Model

### Backend Responsibilities

`src/playback/backend_gst.rs` owns the visible playback clock.

It is responsible for:

- deriving a continuous playback position from the current clock anchor
- deciding when raw GStreamer position is a small jitter sample, a seek reacquire sample, or a true discontinuity
- deciding when to ignore, trim, fast-reanchor, or snap
- exporting `PlaybackSnapshot.position` as the single display clock

### QML Responsibilities

`ui/qml/controllers/PlaybackController.qml` becomes a thin client.

It is responsible for:

- driving a local 60 fps timer for visual interpolation between backend heartbeats
- snapping immediately to backend position on explicit discontinuities already expressed by the backend
- keeping `displayedPositionSeconds` and `spectrogramPositionSeconds` aligned to the backend-owned clock

It is not responsible for:

- seek hold policy
- seek reacquire policy
- steady-state trim or bleed policy
- long-tail drift correction

## Backend Clock States

The backend clock needs explicit states instead of treating every position sample the same way.

### 1. Steady

Normal playback.

- Anchor from the last accepted visible position and wall-clock instant.
- Predict forward continuously at the current learned rate.
- Treat raw `query_position()` as measurement, not truth.
- Ignore very small error.
- Apply small bounded correction for moderate error.
- Snap only for large discontinuity.

### 2. Seek Hold

Immediately after a seek request, before the first stable post-seek sample.

- Anchor visible position to the requested seek target.
- Advance visible position locally from that target at `1.0x` during the hold.
- Do not freeze visible position at the exact target.

This avoids the current "pinned target then reacquire" behavior that creates visible lag after hold release.

### 3. Seek Reacquire

The first accepted raw samples after seek hold.

- Compare raw position against the locally advanced seek-phase prediction.
- If the error is small, accept the sample without visible discontinuity.
- If the error is moderate, re-anchor quickly in backend, favoring sync.
- If the error is large, snap in backend immediately.

This phase should be short-lived. It exists to absorb real post-seek stabilization, not to run for seconds.

### 4. Discontinuity

Used for:

- stop
- pause/resume if position continuity cannot be trusted
- track change
- large backward jump
- large forward jump outside the seek context

Behavior:

- snap immediately in backend
- reset learned rate and anchors

## Backend Clock Rules

### Anchor

Keep:

- `anchor_position`
- `anchor_instant`
- `rate`
- `last_raw_position`
- `last_raw_instant`
- `mode`

### Prediction

Visible position at `now` is:

- `anchor_position + elapsed_since_anchor * rate`

### Measurement Handling

When a raw position sample arrives:

1. predict the current visible position
2. compute error against raw sample
3. choose policy based on mode and error size
4. update anchor and rate once in backend
5. export the resulting position directly as `snapshot.position`

### Sync Bias

If there is a tradeoff between smoothing and lag:

- prefer the correction policy that keeps visible position close to raw playback
- avoid long-lived lag windows larger than what is visually acceptable
- accept a single backend-side snap over seconds of trailing correction

## QML Behavior After This Change

`PlaybackController` should keep only:

- local timer-based interpolation from the last backend anchor
- immediate snap to backend position on seek request initiated from the UI
- immediate snap to the next backend heartbeat if the backend has already decided a discontinuity occurred

The following QML concepts should be removed or reduced to no-ops:

- `interpolationAwaitingSeekReacquire`
- steady-state `trim`
- `bleed`
- post-seek bounded correction policy
- learned playback-rate correction independent from backend

QML should stop trying to repair timing policy that the backend already owns.

## Data Flow

### Current

GStreamer raw position -> backend smoothing -> snapshot.position -> bridge -> QML smoothing -> spectrogram

### Target

GStreamer raw position -> backend-owned visual clock -> snapshot.position -> bridge -> QML interpolation only -> spectrogram

This removes one full policy layer.

## Seek Behavior

Desired visible behavior for a seek:

1. user seeks to `T`
2. UI snaps immediately to `T`
3. backend enters `seek_hold` and advances locally from `T`
4. first post-seek backend sample is compared to the local prediction
5. backend exports the corrected visible position
6. QML follows that exported position directly

There should be:

- no frozen post-seek window
- no QML-side reacquire bleed
- no long trailing correction after the first accepted backend sample

## Rolling and Centered Spectrogram Modes

This design does not change display-mode semantics.

- rolling mode still uses the same continuous playback position semantics it already has
- centered mode still uses the same random-access display semantics it already has

The change is only about who owns the playback clock that drives those modes.

## Instrumentation

Keep the current diagnostics and add one backend field for clock state.

Add to backend logs:

- clock mode: `steady`, `seek_hold`, `seek_reacquire`, `discontinuity`
- predicted position
- accepted visible position
- raw position
- correction type

This should make it obvious whether lag or snapping originates in backend policy rather than UI policy.

## Testing

### Rust

Add backend unit tests that prove:

- seek hold advances visible position from the requested target during the hold
- the first post-seek raw sample does not get artificially held behind for multiple heartbeats
- steady-state bursty samples do not produce large visible velocity spikes
- true discontinuities still snap

### UI

Add Qt smoke tests that prove:

- after a seek, the first advancing backend heartbeat is reflected immediately by the controller
- QML no longer enters post-seek trim or bleed
- steady playback still interpolates smoothly between backend heartbeats
- rolling spectrogram cadence remains stable at max zoom

## Risks

- If backend thresholds are too aggressive, visible snaps may return after seek.
- If backend thresholds are too conservative, sync lag may remain.
- Removing too much QML logic at once could regress transport-bar smoothness if backend interpolation is not ready first.

## Migration Strategy

1. strengthen the backend clock so it fully owns seek-phase behavior
2. prove that backend-exported position is visually usable through Rust diagnostics and tests
3. simplify QML correction logic until it is interpolation-only
4. keep instrumentation on through the transition

## Success Criteria

This design is successful when:

- repeated backward seeks do not create visible scroll-speed bursts
- repeated backward seeks do not leave the spectrogram visibly trailing the audio
- diagnostics show backend-exported position staying close to raw playback after seek reacquire
- QML logs no longer show long-lived post-seek trim/bleed behavior
