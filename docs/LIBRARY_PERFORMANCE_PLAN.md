# Library Performance Plan (Remaining Phases)

## Status
- Completed:
  - Phase 0: dedicated FFI tree-frame channel, tree removed from binary snapshot payloads.
  - Phase 1: incremental `LibraryTreeModel` row updates with reset fallback.
- Remaining:
  - Phase 2: lazy artist-first library tree.
  - Phase 3: backend-driven global search.

This document only covers the remaining implementation work.

---

## Phase 2: Lazy Artist-First Tree

### Goal
Reduce tree payload size and parse cost during scans by sending only shallow rows by default, then hydrating deeper levels on demand.

### Scope
- Keep full snapshot transport for playback/queue/analysis unchanged.
- Keep dedicated tree-frame transport (already in place).
- Do not implement row-level deltas in this phase; continue sending complete tree frames, but with partial hydration.

### Design
1. Backend owns expansion state.
2. Initial tree frame contains:
   - root rows (when multi-root mode is active),
   - artist rows,
   - no album/section/track rows until expanded.
3. Artist expansion hydrates albums.
4. Album expansion hydrates tracks/sections.
5. Existing `child_count` continues to indicate whether a row has descendants, even when descendants are not yet emitted.

### Protocol Changes
1. Add binary command `CmdSetNodeExpanded = 34`.
2. Payload:
   - `[u16 key_len][key_bytes][u8 expanded]`
3. Rust command mapping:
   - add `BridgeLibraryCommand::SetNodeExpanded { key: String, expanded: bool }`.
4. Backward compatibility:
   - no snapshot header change needed,
   - existing tree row binary format stays unchanged in Phase 2.

### Key Stability Requirement
Before backend-driven expansion can be trusted, row keys must be globally unique and stable across roots.

Implementation rule:
1. Update tree key format to include root identity for non-track rows.
2. Keep track keys path-based (`track|<abs path>`), as they are already stable.

### Backend Work

#### [MODIFY] `src/frontend_bridge/mod.rs`
1. Add expansion state to bridge state:
   - `expanded_keys: HashSet<String>`
2. Handle `SetNodeExpanded` command:
   - insert/remove key in `expanded_keys`,
   - trigger tree rebuild and snapshot/tree emission.
3. On root/library mutations, prune stale `expanded_keys`.

#### [MODIFY] `src/frontend_bridge/ffi.rs`
1. Parse command id `34`.
2. Route to new `BridgeLibraryCommand::SetNodeExpanded`.
3. Add command parser tests for valid/invalid payloads.

#### [MODIFY] `src/frontend_bridge/library_tree.rs`
1. Extend tree builder input with expansion context:
   - `build_library_tree_flat_binary(library, sort_mode, expanded_keys)`
2. Emit only hydrated descendants:
   - artists always emitted,
   - albums emitted only if artist key is expanded,
   - tracks/sections emitted only if album key is expanded.
3. Keep `child_count` based on real descendant totals, not emitted row counts.
4. Update tests to cover lazy-hydration behavior and key stability.

### UI Work

#### [MODIFY] `ui/src/BinaryBridgeCodec.h` + `ui/src/BinaryBridgeCodec.cpp`
1. Add `CmdSetNodeExpanded = 34`.
2. Add encoder helper for `(key, expanded)` payload.

#### [MODIFY] `ui/src/BridgeClient.h` + `ui/src/BridgeClient.cpp`
1. Add bridge method to send node expansion changes.
2. Wire model expansion changes to bridge command sends.

#### [MODIFY] `ui/src/LibraryTreeModel.h` + `ui/src/LibraryTreeModel.cpp`
1. Add signal:
   - `nodeExpansionRequested(const QString &key, bool expanded)`
2. Emit signal from `toggleKey()` when expansion state changes.
3. Keep local expanded state for immediate arrow feedback while waiting for next tree frame.

#### [MODIFY] `ui/qml/Main.qml`
1. No major layout change required.
2. Ensure expand/collapse actions continue to call `toggleKey`.
3. Keep scroll anchoring based on `selectionKey` after tree applies.

### Acceptance Criteria
1. Initial scan/startup tree frame size is substantially lower than current full tree frames.
2. Seekbar + spectrogram stay smooth during scan tree updates (no long freezes).
3. Expanding/collapsing artists/albums is responsive and deterministic.
4. Multi-root libraries do not suffer key collisions.

---

## Phase 3: Backend-Driven Global Search

### Goal
Support global search without requiring all tracks to be present in the UI model at once.

### Scope
- Search query evaluated in backend.
- UI sends query changes and receives result-set updates.
- Search mode coexists with lazy tree mode from Phase 2.

### Design
1. Add searchable index in SQLite (FTS5).
2. Debounced query updates from UI.
3. Backend returns bounded result sets (for example top N matches).
4. Clearing query returns UI to normal lazy tree browsing.

### Database Changes

#### [MODIFY] `src/library/mod.rs`
1. Add FTS table and sync strategy:
   - `tracks_fts(path, title, artist, album)` with `path` as stable identifier.
2. Populate/update FTS entries during track upserts.
3. Delete stale FTS rows when tracks are removed.
4. Add query API returning ordered matching paths with limit.

### Bridge and Protocol Changes

#### [MODIFY] `src/frontend_bridge/mod.rs`
1. Add bridge search state:
   - active query string,
   - query token/version to drop stale results.
2. Add command handling:
   - `BridgeLibraryCommand::SetSearchQuery { query: String }`
   - `BridgeLibraryCommand::ClearSearch`
3. When search is active, tree frames should represent search results instead of hierarchical browse rows.

#### [MODIFY] `src/frontend_bridge/ffi.rs`
1. Add command ids:
   - `CmdSetSearchQuery = 35`
   - `CmdClearSearch = 36`
2. Parse payloads and map to bridge commands.
3. Add parser tests for edge cases and malformed payloads.

#### [MODIFY] `ui/src/BinaryBridgeCodec.h` + `ui/src/BinaryBridgeCodec.cpp`
1. Add command ids `35` and `36`.
2. Add encoder helpers for query string command.

### UI Work

#### [MODIFY] `ui/src/BridgeClient.h` + `ui/src/BridgeClient.cpp`
1. Add debounced query send path (reuse existing UI search box input).
2. Send `CmdSetSearchQuery` when non-empty query changes.
3. Send `CmdClearSearch` when query is cleared.

#### [MODIFY] `ui/src/LibraryTreeModel.cpp`
1. Support search-result rendering mode:
   - track rows only (flat list), or grouped minimally if needed.
2. Keep selection behavior consistent with normal browsing mode.

#### [MODIFY] `ui/qml/Main.qml`
1. Keep existing search field, but switch behavior:
   - local filter only when backend search disabled,
   - backend query mode when enabled.
2. Add explicit empty-state text for backend search: `No matches`.

### Acceptance Criteria
1. Query updates remain responsive while playback and analysis continue smoothly.
2. Large libraries can be searched without loading full hydrated track trees.
3. Clearing query restores previous browse context (including expansions where possible).

---

## Verification Plan (Phases 2-3)

### Automated
1. Rust:
   - `cargo test --features gst`
   - new tests for command parsing ids `34-36`,
   - new lazy-tree builder tests (collapsed/expanded outputs),
   - new FTS search tests (insert/update/delete/query behavior).
2. UI:
   - `cmake --build ui/build -j`
   - `ctest --test-dir ui/build --output-on-failure`
   - add/extend QML smoke coverage for expansion + search-mode flows.

### Manual
1. Startup:
   - library should appear quickly with artist-level rows.
2. Scan:
   - playback controls, seekbar, and spectrogram remain smooth during scan/rescan.
3. Expand:
   - expanding artist/album loads deeper rows without full-view hitch.
4. Search:
   - query returns relevant tracks quickly,
   - clearing query returns to lazy browse tree cleanly.

---

## Implementation Order
1. Phase 2 key-stability changes.
2. Phase 2 expansion command + backend expansion-state rebuilds.
3. Phase 2 UI expansion command wiring.
4. Phase 2 performance validation.
5. Phase 3 FTS schema + index maintenance.
6. Phase 3 command path + backend query state.
7. Phase 3 UI wiring and search-mode rendering.
8. End-to-end validation and tuning.
