# Ferrous Optimization Plan

This plan tracks pending and potential performance optimizations after in-process bridge migration.

## Principles

- Preserve behavior parity while optimizing internals.
- Prefer measurable wins (latency, frame pacing, CPU/memory).
- Keep high-rate and low-rate paths explicitly separated.

## Backlog

## P0: Low-Rate Transport Internals (In-Process)

- Status: pending
- Priority: high
- Tasks:
  - Replace low-rate JSON payloads in in-process mode with typed C ABI structs/events.
  - Emit low-rate state as field-level deltas (avoid snapshot-shaped full object churn).
  - Remove duplicated encode/parse logic between process bridge and FFI bridge.

## P1: Memory and Allocation Pressure

- Status: pending
- Priority: high
- Tasks:
  - Reuse buffers for JSON/event/frame queues and analysis frame construction.
  - Reduce temporary `QStringList`/`QVariantList` rebuilds when library digest is unchanged.
  - Add targeted arena/reuse strategy for hot spectrogram row paths where feasible.

## P2: UI Main-Thread Work Reduction

- Status: pending
- Priority: high
- Tasks:
  - Avoid full library tree rebuilds on minor state updates.
  - Cache album/cover map transforms and update incrementally.
  - Keep high-frequency work fully out of UI-thread JSON parsing path.

## P3: Concurrency and Scheduling

- Status: pending
- Priority: medium
- Tasks:
  - Evaluate lock contention in FFI runtime polling path.
  - Tighten poll cadence and batching strategy to reduce wakeups.
  - Verify no starvation between playback control and analysis updates.

## P4: Process Fallback Hygiene

- Status: pending
- Priority: medium
- Tasks:
  - Keep subprocess bridge working as debug fallback with parity tests.
  - Minimize maintenance overhead by sharing core delta/serialization code.

## Instrumentation/Verification

- Add/update profiling counters for:
  - low-rate events per second
  - high-rate frame queue depth and drops
  - UI update/apply time
  - end-to-end control latency (command send -> reflected state)
- Run before/after comparison for each optimization slice.
