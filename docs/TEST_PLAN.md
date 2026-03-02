# Ferrous Test Coverage Plan

This plan tracks test coverage additions for safe optimization/refactoring.

## Goals

- Catch behavioral regressions early in backend and bridge code.
- Add a fast smoke layer for native UI loading and basic runtime integrity.
- Establish CI-friendly commands for repeatable verification.

## Test Matrix

### Layer 1: Backend Unit Tests (Rust)

- Scope:
  - Settings parse/format/persistence helpers.
  - Queue mutation/state transition logic.
  - Snapshot/analysis delta encoding helpers.
- Speed target: fast (`cargo test` scale), no media files required.
- Status:
  - Phase 1 implemented:
    - `src/frontend_bridge/mod.rs` tests for settings + queue state logic.
    - Queue-state coverage includes replace/autoplay, append (empty/non-empty), move, remove, select, and out-of-bounds play-at behavior.
    - `src/frontend_bridge/ffi.rs` tests for command parsing + snapshot/analysis encoding contract.
  - Phase 2 implemented:
    - `src/analysis/mod.rs` tests for waveform cache roundtrip, peak blob roundtrip, STFT row generation, spectrogram decimation, and snapshot emission gating.
    - `src/library/mod.rs` tests for supported-extension detection, indexed scan behavior, and stale/deleted track cleanup.
    - `src/app/mod.rs` tests for settings parse/format roundtrip and FFT/db-range/log-scale normalization.

### Layer 2: FFI Contract Tests (Rust)

- Scope:
  - C ABI command parsing/error handling behavior.
  - Event payload shape contract (`snapshot`, `error`, `stopped`).
  - Binary analysis frame envelope validation.
- Status:
  - Phase 1 implemented in `src/frontend_bridge/ffi.rs` (unit-level contract checks).
  - Phase 2 implemented:
    - End-to-end tests now drive exported `ferrous_ffi_bridge_*` functions for snapshot/error/stopped flow.
  - Phase 3 implemented:
    - `src/bin/native_frontend.rs` parity tests confirm process-style command parsing path and in-process FFI path produce matching outcomes for:
      - queue replacement via album paths
      - queue transition sequence (`select_queue`, `move_queue`, `remove_at`) including resulting queue order
      - successful seek command path preserving queue/current playback invariants
      - playback state transition sequence (`pause`/`play`/`next`/`prev`) preserving queue/current playback parity
      - invalid seek command error payload parity
  - Future: broaden parity checks to stop/restart and explicit play-at edge transitions.

### Layer 3: Native UI Smoke Tests (Qt)

- Scope:
  - Main QML loads headlessly and instantiates root object.
  - Bridge fallback path remains loadable.
- Status:
  - Phase 1 implemented:
    - `native_ui/tests/tst_qml_smoke.cpp`.

### Layer 4: Integration/Playback Regression Tests (Rust + UI)

- Scope:
  - Queue transitions, seek behavior, gapless handoff, no early metadata/waveform switch.
  - In-process bridge mode and process fallback mode parity checks.
- Status:
  - Phase 1 partially implemented:
    - Bridge queue roundtrip integration test (`FrontendBridgeHandle` + snapshot assertions).
    - Process-vs-FFI queue replacement parity assertion in `src/bin/native_frontend.rs`.
    - Bridge integration regression test for queue/play-at + seek clamp + remove transition flow in `src/frontend_bridge/mod.rs`.
    - Playback unit regression test for seek boundary behavior before/at track end in `src/playback/mod.rs`.
    - Playback unit regression test proving natural handoff emits `TrackChanged::Natural` at boundary in `src/playback/mod.rs`.
    - Bridge regression tests proving:
      - `Seeked` events do not trigger early waveform-track switch side effects
      - `TrackChanged` does not swap metadata until metadata events arrive
      in `src/frontend_bridge/mod.rs`.
  - Further gapless and metadata transition cases are still planned.

## Execution Commands

- Rust tests:
  - `cargo test --features gst`
  - `cargo clippy --features gst -- -D clippy::pedantic`
  - `cargo audit`
- Native UI smoke tests:
  - `cmake -S native_ui -B native_ui/build`
  - `cmake --build native_ui/build`
  - `ctest --test-dir native_ui/build --output-on-failure`
- One-shot verification script:
  - `./scripts/run-tests.sh`

## Next Coverage Steps

1. Add no-early-next-track metadata transition regression tests.
2. Add deterministic integration tests for gapless handoff behavior.
3. Expand process-vs-in-process parity tests for stop/restart and play-at edge transitions.
4. Add performance regression harness for bridge/event throughput and UI frame pacing.
