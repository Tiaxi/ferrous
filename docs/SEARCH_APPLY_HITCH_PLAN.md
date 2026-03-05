# Plan: Eliminate Global Search Apply Hitches (UI Freeze) in the UI<->Backend Bridge

## Summary
Global search query compute time is now good, but the UI still freezes briefly when results are applied.
The root issue is not only search compute; it is synchronous result transport + decode + `QVariant`/QML model rebuild work on the UI thread.

This plan moves heavy apply work off the UI thread, adds backpressure/coalescing, and replaces JS-array rebuilds with a C++ list model.
A dedicated FFI channel is treated as an optional final step, not the first step, because the biggest wins are in UI-thread workload reduction.

## Background and Rationale
Current pipeline (simplified):

1. Rust search worker computes rows quickly.
2. FFI runtime queues encoded search frames.
3. `BridgeClient::pollInProcessBridge()` (UI thread) pops and decodes frames.
4. `processSearchResultsFrame()` builds large `QVariantMap` lists.
5. QML rebuilds `globalSearchDisplayRows` JS array and ListView delegates.

The observable freezes happen mainly in steps 3-5 (UI-thread apply path), especially on broad results.
So even if query execution is fast, UI can hitch at completion.

### Why this plan helps performance
- Offloading decode/materialization removes expensive per-row work from UI thread.
- Coalescing keeps only newest result when typing quickly (prevents wasted applies).
- C++ model update is cheaper and more predictable than full JS array rebuild.
- Analysis/spectrogram updates and playback controls stay responsive because UI event loop is less blocked.

## Goals and Success Criteria
### User-visible goals
- No perceptible multi-hundred-ms hitch when search results appear.
- Spectrogram/seekbar/user controls remain smooth during search updates.

### Performance targets (baseline library ~24k tracks)
- Search apply hitch (UI-thread blocked span): `p95 < 40 ms`, `max < 80 ms`.
- End-to-end search latency for normal queries unchanged or better.
- No regressions to search correctness/ranking/keyboard navigation/context menus.

## Non-Goals
- Changing search ranking semantics.
- Changing search result schema/content (except internal transport/model representation).
- Replacing in-memory search algorithm in this phase.

## Implementation Plan
### Phase 1: Instrumentation and Apply-Path Timing (No behavior changes)
#### Changes
- Add per-stage diagnostics timestamps:
  - `ffi_pop_ms`
  - `decode_ms`
  - `materialize_ms`
  - `model_apply_ms`
  - `qml_frame_ms` (coarse)
- Add counters:
  - `search_frames_received`
  - `search_frames_applied`
  - `search_frames_dropped_stale`
  - `search_frames_coalesced`

#### Rationale
Need stage-level attribution to verify where hitches remain after each phase.

#### Expected impact
No direct speedup, but enables precise verification and tuning.

### Phase 2: Move Search Decode/Materialization Off UI Thread
#### Changes
- In `BridgeClient::pollInProcessBridge()`:
  - Pop raw search frame bytes quickly.
  - Enqueue to a dedicated search-apply worker (`QThread`/worker object).
- Worker responsibilities:
  - Decode search payload.
  - Build typed row structs (not `QVariantMap` yet).
  - Coalesce to latest seq (drop obsolete pending work).
- UI-thread responsibilities:
  - Receive prepared latest frame via queued signal.
  - Apply frame in one light operation.

#### Rationale
Most hitch risk is synchronous decode + row conversion on UI thread.

#### Expected impact
Major reduction in UI stalls during result arrival.

### Phase 3: Replace QML JS Rebuild with C++ `QAbstractListModel`
#### Changes
- Introduce `GlobalSearchResultsModel` (C++) with roles for all existing columns/metadata.
- BridgeClient exposes:
  - `Q_PROPERTY(QAbstractItemModel* globalSearchModel READ globalSearchModel CONSTANT)`
- Remove full JS `rebuildGlobalSearchDisplayRows()` path.
- Keep existing UX behaviors (Tab reveal, arrows, page up/down, enter, context menu) against model indices.
- Keep section/header rows in model as explicit row kinds (`section`, `columns`, `item`) to preserve current rendering semantics.

#### Rationale
QML JS array rebuilding and delegate churn are expensive and unpredictable under load.

#### Expected impact
Lower GC/churn, faster and steadier frame apply.

### Phase 4: Backpressure and Apply Coalescing (Bridge + UI)
#### Changes
- FFI queue behavior for search results: keep newest, drop superseded frames aggressively.
- UI apply gate:
  - Apply at most one search frame per event-loop turn.
  - If multiple pending, keep latest only.
- Maintain existing stale-seq checks as safety.

#### Rationale
Typing bursts should never force UI to process obsolete frames.

#### Expected impact
Improved responsiveness during rapid typing and lower worst-case hitch time.

### Phase 5 (Optional, Only If Needed): Dedicated FFI Search Channel
#### Decision
Defer by default. Implement only if Phase 2-4 metrics still miss targets.

#### Changes
- Split search frame queue/lock path from snapshot/analysis path in FFI runtime.
- Optional separate poll/pop functions for search queue.

#### Rationale
A dedicated channel helps lock contention/fairness, but alone does not fix UI-thread apply cost.
Do it only after primary bottlenecks are removed.

#### Expected impact
Incremental improvement, mostly under concurrent heavy bridge traffic.

## Public API / Interface / Type Changes
### C++/QML bridge
- Add: `globalSearchModel` (`QAbstractItemModel*`) in `BridgeClient`.
- Deprecate internal use (can keep temporarily for compatibility):
  - `globalSearchArtistResults`
  - `globalSearchAlbumResults`
  - `globalSearchTrackResults`
- Add internal type: `GlobalSearchResultsModel` row struct + roles.

### Rust FFI (optional depending on Phase 4 design)
- Keep existing functions compatible.
- May add internal/latest-only behavior for pending search result queue.
- No external command payload schema changes required.

## Test Plan and Scenarios
### Functional correctness
1. Search results identical (content/order) before vs after for representative queries.
2. Tab reveal / Enter play / Queue / context menus unchanged.
3. Empty query clears results correctly.
4. Stale frame handling still correct during rapid typing.

### Performance scenarios
1. Broad query (`"a"`) on ~24k tracks: verify no visible freeze; measure stage timings.
2. Rapid typing sequence (`por` -> `porcupine` -> clear): confirm coalescing and low dropped-frame overhead.
3. Concurrent playback + spectrogram + searching: verify seekbar/spectrogram remain smooth.

### Regression checks
- Existing QML smoke tests.
- Add targeted unit tests:
  - search frame coalescing logic
  - model row-kind generation and role mapping
  - stale-seq drop logic

## Rollout Strategy
1. Ship Phase 1 instrumentation behind current diagnostics logging.
2. Ship Phase 2 + 4 together (largest responsiveness gain).
3. Ship Phase 3 model migration (keep old properties temporarily for fallback).
4. Re-measure; only then decide on Phase 5 dedicated channel.

## Risks and Mitigations
- Risk: Thread-safety/ordering bugs between worker and UI.
  - Mitigation: seq-based monotonic apply + queued signals + latest-only queue.
- Risk: QML behavior regressions during model migration.
  - Mitigation: preserve row-kind semantics and keyboard/context tests.
- Risk: Hard-to-compare before/after.
  - Mitigation: mandatory per-stage timing logs from Phase 1.

## Assumptions and Defaults
- Keep in-memory search strategy and current ranking semantics.
- Keep current result caps/debounce defaults unless instrumentation suggests otherwise.
- Keep current global search UI layout and interactions unchanged.
- Dedicated FFI search channel is not default; it is conditional on post-Phase-4 metrics.
