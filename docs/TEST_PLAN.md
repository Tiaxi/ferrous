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
    - `src/frontend_bridge/ffi.rs` tests for command parsing + snapshot/analysis encoding contract.

### Layer 2: FFI Contract Tests (Rust)

- Scope:
  - C ABI command parsing/error handling behavior.
  - Event payload shape contract (`snapshot`, `error`, `stopped`).
  - Binary analysis frame envelope validation.
- Status:
  - Phase 1 implemented in `src/frontend_bridge/ffi.rs` (unit-level contract checks).
  - Phase 2 implemented:
    - End-to-end tests now drive exported `ferrous_ffi_bridge_*` functions for snapshot/error/stopped flow.
  - Future: add process-vs-in-process parity integration checks for key command flows.

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
  - Further queue/seek/gapless and metadata transition cases still planned.

## Execution Commands

- Rust tests:
  - `cargo test --features gst`
- Native UI smoke tests:
  - `cmake -S native_ui -B native_ui/build`
  - `cmake --build native_ui/build`
  - `ctest --test-dir native_ui/build --output-on-failure`

## Next Coverage Steps

1. Add process-vs-in-process bridge parity integration tests.
2. Expand queue/playback integration tests with deterministic fixtures.
3. Add seek/gapless/metadata transition regression tests.
4. Add performance regression harness for bridge/event throughput and UI frame pacing.
