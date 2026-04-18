# Backend-Owned Playback Clock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move playback-clock policy fully into the backend so the spectrogram stays visually synced to audio without post-seek cadence bursts or long trailing lag.

**Architecture:** Extend the backend smoothed playback clock with explicit seek-phase ownership and export one authoritative `snapshot.position`. Simplify `PlaybackController` so it interpolates between backend heartbeats but no longer performs independent timing policy such as seek reacquire, trim, or bleed. Keep existing diagnostics and add backend clock-mode logging so behavior is verifiable from logs.

**Tech Stack:** Rust (`gstreamer`, backend playback runtime), C++/QML (Qt6/QML controller tests), diagnostics logs, QtTest, Rust unit tests

---

## File Map

| File | Responsibility |
|------|----------------|
| `src/playback/backend_gst.rs` | Authoritative visual playback clock, seek hold/reacquire state, backend diagnostics, Rust tests |
| `src/playback/mod.rs` | Snapshot semantics only if a new clock-state enum must surface beyond backend logs |
| `ui/qml/controllers/PlaybackController.qml` | Timer interpolation only, no independent seek/trim/bleed policy |
| `ui/tests/tst_qml_smoke.cpp` | UI regressions for post-seek sync and interpolation behavior |
| `docs/superpowers/specs/2026-04-18-backend-owned-playback-clock-design.md` | Accepted design reference |

## Task 1: Strengthen Backend Clock State Model

**Files:**
- Modify: `src/playback/backend_gst.rs`
- Test: `src/playback/backend_gst.rs`

- [ ] **Step 1: Write failing backend tests for seek-phase ownership**

Add Rust tests near the existing `SmoothedPlaybackClock` tests covering:

```rust
#[test]
fn seek_hold_advances_visible_position_from_target() {
    let base = Instant::now();
    let mut clock = SmoothedPlaybackClock::new(base);
    clock.reset(Duration::from_secs_f64(12.0), base);

    let visible = clock.current_position(base + Duration::from_millis(120));
    assert!(visible > Duration::from_secs_f64(12.10));
}

#[test]
fn post_seek_reacquire_stays_close_to_raw_sample() {
    let base = Instant::now();
    let target = Duration::from_secs_f64(12.0);
    let mut clock = SmoothedPlaybackClock::new(base);
    clock.reset(target, base);

    let raw = Duration::from_secs_f64(12.26);
    let modeled = release_seek_hold_sample(
        &mut clock,
        target,
        base + Duration::from_millis(220),
        raw,
        base + Duration::from_millis(240),
    );

    assert!((raw.as_secs_f64() - modeled.as_secs_f64()) < 0.05);
}
```

- [ ] **Step 2: Run Rust tests to confirm red where behavior is still missing**

Run: `./scripts/run-tests.sh --rust-only`

Expected: if the tests expose missing behavior, the new tests fail with seek-phase mismatch assertions.

- [ ] **Step 3: Introduce explicit backend clock mode**

Add a small backend enum and state field in `src/playback/backend_gst.rs`:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PlaybackClockMode {
    Steady,
    SeekHold,
    SeekReacquire,
    Discontinuity,
}
```

Store it in `SmoothedPlaybackClock` or `GstPlaybackRuntime`, whichever keeps ownership local to backend timing policy.

- [ ] **Step 4: Implement seek-hold local advancement**

Change the seek-hold path so visible position advances from the seek target during hold rather than freezing:

```rust
if now < until {
    let hold_position = self.position_clock.current_position(now);
    if self.snapshot.position != hold_position {
        self.snapshot.position = hold_position;
        snapshot_changed = true;
    }
    position_locked = true;
}
```

Keep the seek target as the anchor, but do not pin visible position to the exact target for the whole hold window.

- [ ] **Step 5: Implement seek-reacquire backend acceptance policy**

Update `release_seek_hold_sample()` and `update_playing_sample()` so the first post-seek sample is handled by backend policy, not generic steady-state trim:

```rust
fn release_seek_hold_sample(
    clock: &mut SmoothedPlaybackClock,
    _target: Duration,
    _released_at: Instant,
    raw_position: Duration,
    now: Instant,
) -> Duration {
    clock.update_playing_sample(raw_position, now)
}
```

Then add mode-aware handling in `update_playing_sample()` so `SeekReacquire` prefers sync over conservative lag:

```rust
match self.mode {
    PlaybackClockMode::SeekReacquire => {
        if error_seconds.abs() <= 0.03 {
            corrected = raw_position;
        } else if error_seconds.abs() <= 0.12 {
            corrected = add_signed_seconds(predicted, error_seconds * 0.6);
        } else {
            self.reset(raw_position, now);
            corrected = raw_position;
        }
    }
    PlaybackClockMode::Steady => { /* existing lighter smoothing */ }
    _ => { /* state-specific handling */ }
}
```

Use the repo’s existing naming and helper style when applying this.

- [ ] **Step 6: Run Rust tests to confirm green**

Run: `./scripts/run-tests.sh --rust-only`

Expected: Rust tests pass, including the new seek-phase tests.

- [ ] **Step 7: Commit**

```bash
git add src/playback/backend_gst.rs
git commit -m "fix: make backend own seek-phase playback clock"
```

## Task 2: Add Backend Clock Diagnostics

**Files:**
- Modify: `src/playback/backend_gst.rs`
- Test: `src/playback/backend_gst.rs`

- [ ] **Step 1: Write a failing trace-format test**

Add a small unit test for the backend trace helper:

```rust
#[test]
fn backend_clock_trace_reports_mode() {
    let mode = PlaybackClockMode::SeekReacquire;
    let text = format!("{mode:?}");
    assert!(text.contains("SeekReacquire"));
}
```

- [ ] **Step 2: Add mode to profiling logs**

Extend `[gst-pos]` logging so it includes backend clock mode and accepted visible position:

```rust
profile_eprintln!(
    "[gst-pos] ... mode={:?} accepted_ms={} ...",
    self.position_clock.mode(),
    duration_ms_i128(self.snapshot.position),
);
```

- [ ] **Step 3: Run Rust tests**

Run: `./scripts/run-tests.sh --rust-only`

Expected: all Rust tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/playback/backend_gst.rs
git commit -m "chore: log backend playback clock mode"
```

## Task 3: Remove QML Seek-Reacquire Policy

**Files:**
- Modify: `ui/qml/controllers/PlaybackController.qml`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write failing UI tests for direct backend follow after seek**

Adjust or add Qt smoke tests so the controller must follow the first advancing backend heartbeat directly:

```cpp
void QmlSmokeTest::playbackControllerPostSeekHeartbeatSnapsToBackendPosition() {
    // seek to 48.0, then inject backend heartbeat at 48.26
    // expected: displayedPositionSeconds ~= 48.26 immediately
}

void QmlSmokeTest::playbackControllerPostSeekHeartbeatDoesNotEnterBleed() {
    // enable profile logs, perform seek, inject first advancing heartbeat
    // expected: captured warnings do not contain "action=bleed"
}
```

- [ ] **Step 2: Run UI tests to verify red if the old logic remains**

Run: `./scripts/run-tests.sh --ui-only`

Expected: the new post-seek follow test fails if QML still uses bounded correction or bleed.

- [ ] **Step 3: Simplify seek reacquire in QML**

Change the `interpolationAwaitingSeekReacquire` branch so the first advancing heartbeat becomes the new truth:

```qml
} else if (root.interpolationAwaitingSeekReacquire) {
    const pinnedPosition = root.interpolationSeekPinnedPosition
    const movedFromPinned = Math.abs(incomingPosition - pinnedPosition)
        > root.interpolationSeekReacquireEpsilonSeconds
    if (!movedFromPinned) {
        root.interpolationActive = false
        root.displayedPositionSeconds = pinnedPosition
        root.spectrogramPositionSeconds = pinnedPosition
        root.resetInterpolationState(pinnedPosition, nowMs)
    } else {
        root.interpolationAwaitingSeekReacquire = false
        root.displayedPositionSeconds = incomingPosition
        root.spectrogramPositionSeconds = incomingPosition
        root.resetInterpolationState(incomingPosition, nowMs)
        root.interpolationActive = true
    }
}
```

- [ ] **Step 4: Remove steady-state correction policy that duplicates backend ownership**

Simplify `applyBoundedPlaybackCorrection()` so QML no longer owns trim/bleed policy during normal playback. The target shape is:

```qml
function applyPlaybackHeartbeat(incomingPosition, nowMs) {
    root.displayedPositionSeconds = incomingPosition
    root.spectrogramPositionSeconds = incomingPosition
    root.resetInterpolationState(incomingPosition, nowMs)
    root.interpolationActive = true
}
```

Preserve only minimal timer interpolation between heartbeats. Remove or neutralize:

- `interpolationCorrectionDebtSeconds`
- steady-state `trim`
- `bleed`
- learned QML playback-rate correction

Do this in the smallest coherent steps that keep tests green.

- [ ] **Step 5: Run UI tests to confirm green**

Run: `./scripts/run-tests.sh --ui-only`

Expected: UI tests pass, including post-seek follow tests and existing interpolation coverage.

- [ ] **Step 6: Commit**

```bash
git add ui/qml/controllers/PlaybackController.qml ui/tests/tst_qml_smoke.cpp
git commit -m "fix: make qml follow backend playback clock after seek"
```

## Task 4: Verify Steady Playback Smoothness With Thin QML

**Files:**
- Modify: `ui/tests/tst_qml_smoke.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add regression coverage for backend-owned steady playback**

Add a smoke test that confirms interpolation stays smooth between two backend heartbeats without QML trim/bleed:

```cpp
void QmlSmokeTest::playbackControllerInterpolatesBetweenBackendHeartbeatsOnly() {
    // initialize, deliver heartbeat at 12.0, wait one frame, ensure position advanced
    // deliver heartbeat at 12.04, ensure no correction warning path is entered
}
```

- [ ] **Step 2: Run UI tests**

Run: `./scripts/run-tests.sh --ui-only`

Expected: the new interpolation test passes and existing cadence tests remain green.

- [ ] **Step 3: Commit**

```bash
git add ui/tests/tst_qml_smoke.cpp
git commit -m "test: cover backend-owned playback interpolation"
```

## Task 5: End-to-End Diagnostics Validation

**Files:**
- Modify: `src/playback/backend_gst.rs` if trace shape still needs adjustment
- Modify: `ui/qml/controllers/PlaybackController.qml` only if logs prove a remaining ownership leak

- [ ] **Step 1: Run full suite**

Run: `./scripts/run-tests.sh`

Expected: Rust checks, clippy, audit, UI build, and UI tests all pass.

- [ ] **Step 2: Collect a real diagnostics run**

Run:

```bash
./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
```

Reproduce the old failure pattern:

1. start playback at innermost zoom
2. seek backward repeatedly in the same song
3. stop seeking and watch post-seek recovery

- [ ] **Step 3: Verify the diagnostics against the design**

Inspect `/home/tuomas/.local/share/ferrous/diagnostics.log` and confirm:

- `[bridge-pos]` stays close to `[gst-pos]` after seek reacquire
- `[qml-playback-profile]` no longer shows long-lived post-seek `trim` or `bleed`
- no visible `bridge-pos delta_ms=6` style artificial lag release

- [ ] **Step 4: If needed, make one bounded backend threshold adjustment**

Only if diagnostics still show backend-owned lag after seek, tune backend thresholds in `SmoothedPlaybackClock` and rerun:

```rust
const IGNORE_ERROR_SECONDS: f64 = 0.02;
const MAX_TRIM_SECONDS: f64 = 0.006;
```

Adjust one threshold at a time and rerun `./scripts/run-tests.sh`.

- [ ] **Step 5: Commit**

```bash
git add src/playback/backend_gst.rs ui/qml/controllers/PlaybackController.qml ui/tests/tst_qml_smoke.cpp
git commit -m "fix: consolidate playback clock ownership in backend"
```

## Self-Review

- Spec coverage:
  - backend-owned clock: Tasks 1-2
  - thin QML client: Tasks 3-4
  - diagnostics validation: Task 5
- Placeholder scan:
  - no `TODO` or `TBD` markers remain
  - each task names exact files and commands
- Type consistency:
  - uses existing `SmoothedPlaybackClock`, `release_seek_hold_sample`, `PlaybackController`, and `displayedPositionSeconds` names from the current codebase

Plan complete and saved to `docs/superpowers/plans/2026-04-18-backend-owned-playback-clock.md`. Two execution options:

1. Subagent-Driven (recommended) - I dispatch a fresh subagent per task, review between tasks, fast iteration

2. Inline Execution - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
