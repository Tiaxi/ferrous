# Responsiveness And Reactive Bridge Plan

## Summary

This plan addresses concrete violations of the repository responsiveness rule:

- main-thread bridge polling and snapshot apply work
- synchronous iTunes artwork decode/normalize/file I/O on the GUI thread
- synchronous image metadata probing from QML interactions
- QML-side `O(n)` loops in global search scrolling and library type-ahead
- spectrogram and waveform rendering paths that do too much per frame or per update

The 16 ms Qt-side bridge poll is not a strict architectural requirement. It is a chosen wakeup policy on top of a bridge runtime that already lives on its own Rust thread. The target design is a hybrid model:

- immediate wake-driven delivery for user actions and backend state changes
- coarse heartbeat updates only where time continuity matters, mainly playback position and visible analysis progress
- local UI smoothing for seek/position display instead of high-rate backend snapshot spam

This should reduce idle UI-thread load, remove avoidable input latency, and improve command responsiveness because results no longer wait for the next fixed UI poll tick.

## Current Root Causes

### High-priority GUI-thread violations

- `BridgeClient` runs on the main thread and uses a fixed 16 ms `QTimer` to call `pollInProcessBridge()`, which drains FFI events and applies analysis, tree, search, and snapshot work in one GUI-thread turn.
- `shutdownBridgeGracefully()` busy-loops on the GUI thread and sleeps in the `BridgeClient` destructor path.
- iTunes artwork asset preparation performs image decode, crop/normalize, file writes, and metadata probing in the `QNetworkReply::finished` handler on the GUI thread.
- spectrogram rendering and ingestion still do too much work per frame and per batch, with full-texture recreation for incremental updates.

### High-priority UI-thread hot loops

- global search wheel scrolling computes row positions in QML with `O(result_count)` work and repeated `QVariantMap` marshaling
- library type-ahead scans the full visible tree on each typed character via repeated `rowDataForRow()` calls
- album-art viewer and iTunes dialog call `imageFileDetails()` synchronously during visible user interactions
- library thumbnail URL resolution still does filesystem metadata work in delegate bindings

## Implementation Plan

### Phase 1: Remove current GUI-thread blockers

Progress update (2026-03-15):

- Completed: `BridgeClient` now starts with a cheap file-browser heuristic, resolves the final name on a background worker, and no longer blocks shutdown with a GUI-thread sleep loop.
- Completed: iTunes artwork post-download processing now runs off the GUI thread; the reply handler only validates the reply, copies the payload, and dispatches background normalization work.
- Completed: async cached image metadata API added to `BridgeClient`:
  - `requestImageFileDetails(path)`
  - `cachedImageFileDetails(path)`
  - `imageFileDetailsChanged(path)`
- Completed: `ViewerController` and `ItunesArtworkDialog` now request metadata asynchronously and refresh from cache on `imageFileDetailsChanged(path)`.
- Completed: global search no longer overrides wheel scrolling with QML-side row accumulation; it now relies on native `ListView` scrolling while keeping keyboard selection visibility via `positionViewAtIndex()`.
- Completed: library type-ahead matching moved into `LibraryTreeModel` via `findArtistRowByPrefix(prefix, startRow)`, removing the QML-side full-model scan.
- Completed: library thumbnail source resolution now uses a cache-only helper path and no longer performs canonical-path and `mtime` filesystem work in delegate bindings.
- Validation: `./scripts/run-tests.sh --ui-only` passed after the Phase 1 changes.

#### Bridge client startup and shutdown

- Stop calling `detectFileBrowserName()` synchronously in the `BridgeClient` constructor.
- Initialize `fileBrowserName` from a cheap heuristic immediately.
- Resolve the actual file browser name on a background worker and apply it back to the main thread only if it changes.
- Remove the main-thread `shutdownBridgeGracefully()` loop and `QThread::msleep()` usage from the destructor path.
- Send shutdown opportunistically and let runtime teardown happen without blocking the GUI thread.

#### iTunes artwork processing

- Keep network fetch async but move post-download image work off the GUI thread.
- The reply handler should only validate the reply, copy the payload, and enqueue a worker job.
- The worker should perform:
  - image decode
  - crop/normalize decision
  - file writes
  - metadata extraction
- The worker returns a result object to the main thread that updates `itunesArtworkResults`.

#### Async image metadata API

- Replace synchronous UI use of `imageFileDetails(path)` with an async cached API on `BridgeClient`.
- Add:
  - `Q_INVOKABLE void requestImageFileDetails(const QString &path)`
  - `Q_INVOKABLE QVariantMap cachedImageFileDetails(const QString &path) const`
  - `signal imageFileDetailsChanged(const QString &path)`
- Update `ViewerController` and `ItunesArtworkDialog` to:
  - request metadata when needed
  - render cached metadata immediately if present
  - refresh when `imageFileDetailsChanged(path)` fires

#### QML hot-loop removal

- Remove the custom wheel-stepping path from global search and rely on native `ListView` scrolling.
- Keep selection visibility with `positionViewAtIndex()` rather than manual `rowTop()` accumulation.
- Move library type-ahead matching into `LibraryTreeModel`.
- Add `Q_INVOKABLE int findArtistRowByPrefix(const QString &prefix, int startRow) const`.
- Update `LibraryController` to call that model method instead of iterating over `rowDataForRow()`.

#### Library thumbnail source handling

- Stop doing canonical-path and `mtime` work in delegate bindings.
- Precompute thumbnail source strings when rows are built, or use a cache-only helper that does not hit the filesystem for every delegate evaluation.

### Phase 1.5: Adaptive polling as an interim step

Progress update (2026-03-15):

- Completed: the permanent repeating 16 ms Qt bridge timer has been replaced with a single-shot adaptive scheduler in `BridgeClient`.
- Completed: the current default poll tiers are now:
  - `0 ms` immediate re-arm when one drain run exhausts its wall-clock budget or saturates a per-category cap
  - `8 ms` while playback is active, a seek is pending, library scan work is active, or global-search work is in flight
  - `33 ms` while paused or when stopped with active track or queue context
  - `160 ms` while idle/stopped without active work
- Completed: `pollInProcessBridge()` now drains within both per-category caps and a wall-clock budget, then reschedules from the post-drain state instead of relying on a fixed repeating tick.
- Completed: bridge command sends now request an immediate single-shot poll so user actions do not wait for the idle tier.
- Validation: `./scripts/run-tests.sh --ui-only` passed after the Phase 1.5 changes.

- Replace the permanent repeating 16 ms Qt timer with a single-shot adaptive scheduler.
- Use these default poll tiers:
  - `0 ms` immediate re-arm when the previous drain saturated its work budget or FFI still has pending queues
  - `8 ms` while playback is active, a seek just happened, analysis backlog exists, or search results are in flight
  - `33 ms` while paused with active visible UI state
  - `100-250 ms` while idle/stopped with no pending work
- Change `pollInProcessBridge()` to use both:
  - per-category item caps
  - a wall-clock budget for one activation
- If work remains after the budget is exhausted, reschedule immediately instead of continuing in the same event-loop turn.

### Phase 2: Wake-driven bridge delivery

Progress update (2026-03-15):

- Completed: `FerrousFfiBridge` now owns a relay thread that blocks on bridge events, coalesces them off the Qt thread, and fills the existing pending FFI queues without relying on GUI-side polling.
- Completed: the FFI bridge now exposes a non-blocking wake pipe through:
  - `ferrous_ffi_bridge_wakeup_fd`
  - `ferrous_ffi_bridge_ack_wakeup`
- Completed: `BridgeClient` now consumes bridge readiness via `QSocketNotifier` instead of calling `ferrous_ffi_bridge_poll()` on a repeating timer.
- Completed: notifier activation now acknowledges the wake fd, drains pending work through the existing bounded apply path, and uses a `0 ms` single-shot continuation only when one drain run saturates its budget or per-category caps.
- Completed: direct command sends no longer force an immediate Qt poll; wake-driven delivery now propagates command results back to the UI.
- Completed: added wake-pipe coverage for readability, coalescing, and `ack_wakeup()` behavior, and updated `BridgeClient` tests to assert notifier installation and continuation scheduling behavior.
- Validation: `cargo fmt` and `./scripts/run-tests.sh` passed after the Phase 2 changes.

#### FFI bridge changes

- Keep the Rust bridge runtime on its existing thread.
- Add a relay thread inside `FerrousFfiBridge` that blocks on `FrontendBridgeHandle::recv_timeout(...)`, coalesces bridge events, and fills the existing pending FFI queues.
- Add a non-blocking wake pipe that becomes readable when pending queues transition from empty to non-empty.
- Extend the FFI API with:
  - `int ferrous_ffi_bridge_wakeup_fd(FerrousFfiBridge *handle);`
  - `void ferrous_ffi_bridge_ack_wakeup(FerrousFfiBridge *handle);`
- Keep `ferrous_ffi_bridge_poll()` temporarily for tests and compatibility, but stop using it in `BridgeClient`.

#### Qt consumer changes

- Replace the fixed poll timer in `BridgeClient` with a `QSocketNotifier` on the FFI wake fd.
- On notifier activation:
  - acknowledge the wake
  - drain pending binary/search/tree/analysis queues within the same bounded apply path
- If draining leaves queued work behind, schedule a `0 ms` continuation rather than monopolizing the notifier callback.

### Phase 3: Reactive snapshots with a coarse heartbeat

Progress update (2026-03-15):

- Completed: the bridge loop in `src/frontend_bridge/mod.rs` no longer treats snapshots as a fixed-rate default stream; it now wakes directly on commands, playback, metadata, library, search, queue-detail, album-art, and Last.fm events.
- Completed: snapshot emission is now urgency-based:
  - immediate for command-driven state changes, playback state/track changes, metadata changes, library progress/state changes, settings/Last.fm changes, queue-detail refreshes, and deferred tree rebuild completions
  - heartbeat-gated for continuous playback-position and analysis-progress updates
- Completed: the default heartbeat tiers now match the plan intent:
  - `100 ms` while playing
  - `333 ms` while paused with an active track
  - no stopped/idle snapshot heartbeat
- Completed: playback polling is no longer treated as an idle bridge ticker; it now polls at `40 ms` while playing, `333 ms` while paused with an active track, and not at all while stopped.
- Completed: pending snapshot/search retries, queue-detail revalidation, deferred tree rebuilds, and settings/session persistence now participate in the wake scheduler instead of depending on a permanent default tick.
- Completed: added deterministic Phase 3 tests covering immediate metadata snapshots from event wakes and coarse heartbeat gating for playback-position updates.
- Validation: `cargo fmt` and `./scripts/run-tests.sh` passed after the Phase 3 changes.

- In `src/frontend_bridge/mod.rs`, stop treating snapshots as a fixed-rate default update stream.
- Emit snapshots immediately for:
  - user commands
  - queue changes
  - playback state changes
  - track changes and metadata changes
  - library progress/stats changes
  - settings changes
  - last.fm state changes
  - errors
- Keep a low-rate heartbeat only for time-continuous UI state:
  - default `100 ms` while playing
  - `250-500 ms` while paused only if needed
  - no idle heartbeat when nothing is changing
- Keep local position smoothing in `PlaybackController`; do not raise snapshot frequency just to animate the seek bar smoothly.

### Phase 4: Spectrogram and waveform cleanup

Progress update (2026-03-15):

- Completed: spectrogram delta delivery is now bounded per UI turn. `BridgeClient::takeSpectrogramRowsDeltaPacked(maxRowsPerChannel)` drains packed rows in small chunks, preserves ordering, and re-queues `analysisChanged()` while backlog remains so a seek-induced burst no longer lands in one GUI-thread turn.
- Completed: `Main.qml` now consumes the per-delta `reset` flag instead of re-reading a sticky bridge property, so partial draining after a reset only clears the surface once.
- Completed: `SpectrogramSurface` no longer flushes packed deltas synchronously on the `analysisChanged` handler stack; it schedules the flush to the next turn.
- Remaining: `SpectrogramItem` still recreates the full texture image for incremental updates, so the texture-upload part of the Phase 4 spectrogram work is still outstanding.

#### Spectrogram

- Stop flushing whole delta bursts synchronously from QML in one event-loop turn.
- Queue pending deltas in `SpectrogramSurface` and flush bounded chunks via `Qt.callLater` or a `0 ms` timer.
- Reduce GUI-thread work in the frame callback to presentation-only state changes.
- Replace full-canvas texture recreation for incremental updates with a changed-region or ring-buffer texture strategy.

#### Waveform

- Stop repainting the full waveform width for each progressive update.
- Use a cached raster image and update only the newly available range, or throttle progressive visual updates to a fixed UI rate such as 30 Hz while preserving final correctness.

## Public API And Interface Changes

### `BridgeClient`

- Add async image detail methods:
  - `requestImageFileDetails(path)`
  - `cachedImageFileDetails(path)`
  - `imageFileDetailsChanged(path)`
- Remove GUI use of synchronous `imageFileDetails(path)` once migrated.
- Replace Qt timer polling with a wake-fd consumer.

### FFI bridge

- Add:
  - `ferrous_ffi_bridge_wakeup_fd`
  - `ferrous_ffi_bridge_ack_wakeup`
- Keep `ferrous_ffi_bridge_poll` during migration only.

### `LibraryTreeModel`

- Add `findArtistRowByPrefix(prefix, startRow)`.

### Global search UI

- Remove the custom wheel-stepping interface entirely.

## Test Plan

### Automated coverage

- Add FFI wakeup contract tests:
  - wake fd becomes readable when snapshot/search/error/stopped work is queued
  - repeated events coalesce without unbounded wake writes
  - `ack_wakeup` clears readiness correctly
- Add `BridgeClient` tests for:
  - non-blocking startup file-browser detection
  - no blocking shutdown sleep loop
  - async image-file-detail request/cache/update behavior
  - iTunes artwork normalization still yields correct preview/apply metadata
- Add QML tests for:
  - album-art viewer and iTunes dialog metadata updates via async cache/signal flow
  - global search scrolling without custom wheel stepping
  - library type-ahead behavior on large models

### Manual verification

- app startup and shutdown
- play, pause, seek, and queue commands
- global search scrolling with large result sets
- library type-ahead on large libraries
- iTunes dialog open, preview, and apply flow on large artwork files
- fullscreen spectrogram and waveform behavior during active playback

## Defaults And Assumptions

- The target design is hybrid reactive plus coarse heartbeat, not pure event-only snapshots.
- Linux/POSIX assumptions are acceptable for the wake-fd design; use a non-blocking pipe.
- User-initiated commands should wake the UI immediately and must not wait for a fixed poll interval.
- Phase 1 and 1.5 should ship before the wake-driven bridge rewrite so the worst GUI-thread violations are removed early.
- Spectrogram and waveform cleanup remains part of the same responsiveness initiative, but bridge wakeup and GUI-thread blocker removal have priority.
