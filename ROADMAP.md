# Ferrous Roadmap

This document tracks upcoming work with a KDE-first target.
Reference baseline for UX: DeaDBeeF screenshot (`deadbeef_reference.png`).

## Product Direction

- Frontend strategy selected: `Qt6/QML + Kirigami` (KDE-native).
- Existing `eframe/egui` UI is now considered a legacy frontend during migration.
- Rust playback/analysis/library services remain the core backend.

## Migration Goal

Ship a Kirigami frontend that reaches current Ferrous behavior parity, then continue DeaDBeeF parity and feature expansion on top of it.

## Current Backend Baseline

- Gapless playback works.
- Spectrogram pipeline is near DeaDBeeF behavior.
- Waveform cache exists and persists to SQLite.
- Library indexing, metadata loading, queue management, and playback control are in place.

## Migration Principles

- Keep business logic in Rust backend; UI layer should stay thin.
- Avoid feature freeze on backend improvements, but avoid new large egui-only UX work.
- Migrate screen-by-screen with runnable checkpoints.
- Keep one source of truth for queue/playback/library state (no duplicated state machines in QML).
- Performance target: minimum 60 FPS rendering, with design choices favoring display refresh-rate rendering when feasible.
- Performance is a hard requirement (no degraded responsiveness accepted for feature parity).

## Critical Path: Performance Architecture Rework (Started)

Current issue summary:
- Rust backend processing is healthy under load.
- UI stalls come from high-rate JSON/stdout transport + Qt main-thread parsing + QML `Canvas` heavy per-pixel JS work.

Decision:
- Keep Rust backend.
- Replace high-rate JSON data path with native/binary UI data flow.
- Keep JSON/command bridge only for low-rate control/state until in-process FFI path is in place.

### Phase P0: Stabilize Current Path During Development

- [x] Bound bridge queues and drop stale snapshots under backpressure.
- [x] Remove heavyweight metadata cloning from per-frame snapshots.
- [x] Add bridge profiling counters (`sent/drop`, payload size, write latency).
- [x] Throttle Qt `snapshotChanged` notifications.
- [x] Reduce spectrogram UI paint cadence and cap history width.
- [x] Add backend snapshot emission pacing (fixed UI rate instead of per-tick flood).
- [x] Smooth playback control path (single-shot seek on release, volume ramping to avoid zipper noise/pops).
- [x] Revert temporary spectrogram quality caps after native C++ path stabilization (higher bin/row throughput restored).

Acceptance criteria:
- App remains interactive during playback (no 40s+ UI lockups).
- Memory usage remains bounded during long playback sessions.

### Phase P1: Native Spectrogram Render Path (C++ Item)

- [x] Replace QML `Canvas` spectrogram with C++ render item (`QQuickPaintedItem` baseline landed).
- [ ] Move palette mapping + bin projection to C++ (no per-frame JS loops).
- [ ] Keep DeaDBeeF-like color mapping and dB behavior parity.
- [ ] Keep rolling history and seek behavior parity.

Acceptance criteria:
- Spectrogram rendering no longer causes observable UI hitching.
- Playlist/library interactions remain responsive while spectrogram is active.

### Phase P2: Split Transport by Data Rate

- [ ] Low-rate channel (JSON/properties): playback state, queue, library, settings.
- [ ] High-rate channel (binary ring/shared memory): spectrogram rows + waveform peaks.
- [ ] Explicit frame sequencing and drop policy for high-rate visuals.
- [ ] Eliminate repeated parse/alloc on UI thread for high-rate analysis data.

Acceptance criteria:
- High-rate analysis visuals do not block command/control responsiveness.
- Predictable CPU usage and bounded latency at target refresh rates.

### Phase P3: In-Process Integration (Bridge Replacement)

- [ ] Introduce in-process Rust backend integration for native UI (FFI boundary).
- [ ] Remove stdout JSON process bridge from steady-state runtime for both control and analysis paths.
- [ ] Keep CLI/debug bridge as optional developer fallback tool.

Acceptance criteria:
- No pipe/stdio backpressure risk in production UI path.
- Native frontend + Rust backend run as one process with explicit threading model.

## Milestone A: Frontend Foundation (QML/Kirigami bootstrap)

- [x] Select and implement initial Rust↔frontend bridge approach (fallback bridge bootstrap now in place; CXX-Qt binding integration pending).
- [x] Add new app target/entrypoint for native frontend (keep egui target buildable during migration).
- [x] Define typed bridge API for:
  - playback controls/events
  - queue queries/mutations
  - library queries/selections
  - analysis snapshots (waveform/spectrogram)
  - settings read/write
- [x] Add minimal Kirigami app shell scaffold with native window, menu bar, and status/footer area.
- [x] Add build documentation for KDE dev environment and runtime dependencies.

Acceptance criteria:
- Kirigami shell launches and can call Rust backend (`Play/Pause/Stop` roundtrip works).
- Legacy egui build still compiles.

## Milestone B: Native Main Layout Skeleton

- [x] Implement split layout in QML matching current Ferrous/DeaDBeeF structure:
  - top controls row
  - left library pane
  - right playlist pane
  - bottom spectrogram pane
  - footer status line
- [x] Recreate top control semantics with native KDE look/behavior.
- [x] Implement centralized action/shortcut map in native shell (`Space`, media controls, etc.).

Acceptance criteria:
- [x] Layout parity exists with placeholder/static content.
- [x] Native menu/shortcuts are wired and functional.

## Milestone C: Playlist + Playback UI Migration

- [x] Implement native playlist table (header + rows + selection + double-click play).
- [x] Wire queue reordering, remove, clear, and play-at operations.
- [x] Implement waveform seekbar in native frontend with current behavior.
- [x] Implement volume control UX in native frontend.

Acceptance criteria:
- [x] Day-to-day playback can be driven fully from Kirigami UI without egui.

## Milestone D: Library Pane Migration

- [ ] Implement library tree/grouping UI (artist/album/track hierarchy).
- [ ] Wire search/filter and indexed roots display.
- [x] Implement album interactions (on current album list view):
  - double-click = replace playlist + play
  - context menu append option
- [ ] Implement library album-art thumbnails and cover panel.

Acceptance criteria:
- Library browsing and enqueue/play workflows match current behavior.

## Milestone E: Spectrogram + Analysis View Migration

- [x] Port spectrogram widget rendering path to native frontend.
- [ ] Preserve rolling behavior across seek and track transitions.
- [ ] Port dB/log-scale controls and settings persistence.
- [ ] Ensure performance parity with current implementation.

Acceptance criteria:
- Spectrogram and waveform behavior are functionally on par with current frontend.

## Milestone F: Cutover and Cleanup

- [ ] Make Kirigami frontend the default build/run path.
- [ ] Remove or archive egui-specific UI modules after migration sign-off.
- [ ] Update CI to test native frontend build and backend integration.
- [ ] Write migration notes/changelog for users.

Acceptance criteria:
- Native frontend is the primary supported UI with no functional regressions vs pre-cutover baseline.

## Post-Migration Parity/Feature Backlog

### Core UX / Interaction
- [ ] Implement full native top menu actions (`File/Edit/View/Playback/Help`).
- [ ] Add playlist and library context menus for common actions.
- [ ] Add drag-and-drop from library to playlist.
- [ ] Add multi-select in playlist and library views.
- [ ] Add status bar parity items (selection counts, queue duration, mode indicators).

### Playback Features
- [ ] Add repeat/shuffle modes.
- [ ] Add ReplayGain support.
- [ ] Add preamp/volume dB behavior options.
- [ ] Add output device selector persistence.
- [ ] Add optional crossfade and buffer tuning controls.

### Library/Metadata
- [ ] Add persistent playlist/session restore.
- [ ] Add incremental library updates via filesystem watcher.
- [ ] Add configurable library roots/config dialog parity.
- [ ] Add richer sort/group/filter modes and extended metadata fields.

### Spectrogram/Waveform Controls
- [ ] Add spectrogram properties UI (dB range, log scale, color stops).
- [ ] Add presets and reset to DeaDBeeF-like defaults.
- [ ] Add waveform density/style options.

### Quality/Performance
- [ ] Add integration tests for queue transitions, gapless handoff, seek behavior.
- [ ] Add regression tests for no early next-track waveform/metadata switch.
- [ ] Add DB migration/versioning strategy.
- [ ] Add profiling/telemetry for decode/analyze/render timing.

## Working Rules

- Keep items concrete and testable.
- Frontend migration tasks should include explicit acceptance criteria before moving to next milestone.
- Keep `ROADMAP.md` updated continuously as tasks land or are reprioritized.
