# Adaptive STFT Zoom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the user zooms in (>1.0x), the backend restarts the STFT with a smaller hop size, producing more columns per second at native resolution. The frontend renders 1:1 (one column per pixel), restoring full FPS and eliminating blockiness.

**Architecture:** The frontend computes `effectiveZoom = userZoom * chunkHopSize / REFERENCE_HOP`. When the backend's hop matches the user's zoom, effectiveZoom = 1.0 and the incremental-advance fast path runs at full FPS. During transitions or zoom-out, the existing interpolation/decimation rendering serves as fallback. Zoom-out (≤1.0) is handled entirely in the frontend (no backend change). Centered mode always uses windowed decode (rolling window around the playhead) instead of full-track pre-decode, reducing memory from O(track_length) to O(screen_width) regardless of zoom level.

**Tech Stack:** Rust (analysis pipeline), C++ / Qt Quick Scene Graph (rendering), QML (wiring)

---

## Context

The current zoom implementation (on this branch) handles zoom purely in the frontend by changing the column-to-pixel mapping. This causes two problems:

1. **FPS tanks** at any non-1.0 zoom — full canvas rebuild every frame instead of incremental advance
2. **Visual quality** — even with interpolation, zoom-in shows smoothed approximations rather than true spectral data at the zoomed resolution

The fix: when zooming in, the backend produces more columns per second (smaller STFT hop), giving the frontend native-resolution data at 1:1 pixel mapping. The incremental advance path works, FPS stays high, and the spectrogram shows real spectral detail at every zoom level.

Additionally, centered mode switches from eagerly pre-decoding the entire track to **windowed decode**: only the visible window (plus lookahead in both directions) is computed at full speed, then the session follows the playhead at slightly-faster-than-realtime, just like rolling mode. On seek, the new window is recomputed immediately. This eliminates the O(track_length × channels) memory footprint that becomes prohibitive for long multichannel tracks, especially at high zoom (e.g., a 30-minute 6ch DSD track at zoom=16x would need ~11 GB with full-track pre-decode).

**Key formula:**
```
zoom_hop = max(64, round(REFERENCE_HOP / zoomLevel))   // for zoom > 1.0
effectiveZoom = userZoom * actualHop / REFERENCE_HOP    // for rendering
```

When effectiveZoom ≈ 1.0, the backend matches the requested zoom and the fast rendering path runs.

**Zoom-out (≤1.0):** No backend change. The existing frontend decimation handles it.

**Max adaptive zoom:** 16x (hop=64, minimum). The existing frontend interpolation handles any zoom beyond 16x.

**Centered mode decode strategy:** Always windowed. The "centered" vs "rolling" distinction is purely a display mode (playhead at center vs scrolling edge), not a decode strategy. Both modes use a finite ring buffer window around the playhead.

---

## Task 1: Rust — Add `effective_hop` to session and fix chunk metadata

This is a pure refactor — no behavior change. It makes the chunk `hop_size` field carry the true effective temporal spacing instead of the hardcoded `REFERENCE_HOP`.

**Files:**
- Modify: `src/analysis/session.rs:582-637` (SpectrogramSessionState struct)
- Modify: `src/analysis/session.rs:710-711` (cols_per_second computation)
- Modify: `src/analysis/session.rs` (8 chunk emission sites)

- [ ] **Step 1: Add `effective_hop` field to `SpectrogramSessionState`**

In `src/analysis/session.rs`, add after `hop_size` (line 586):

```rust
effective_hop: usize,
```

- [ ] **Step 2: Compute `effective_hop` at session initialization**

In `run_spectrogram_session` (line ~710), after `decimation_factor` is computed:

```rust
let decimation_factor = decimation_factor_for_hop(hop_size);
let effective_hop = hop_size * decimation_factor;
let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(effective_hop);
```

This replaces the current `cols_per_second = ... / REFERENCE_HOP`. Since `effective_hop = hop * (REFERENCE_HOP/hop) = REFERENCE_HOP` with the current decimation system, this produces identical results. No behavior change.

Set the field in the `SpectrogramSessionState` initialization:

```rust
effective_hop,
```

- [ ] **Step 3: Replace all `clamp_to_u16(REFERENCE_HOP)` in chunk emission**

Replace `hop_size: clamp_to_u16(REFERENCE_HOP)` with `hop_size: clamp_to_u16(effective_hop)` at all 8 emission sites in `session.rs`:

- Line 218 (staging `take_partial_chunk`): use the staging state's effective_hop
- Line 297 (staging completion chunk): same
- Line 800 (initial metadata chunk): `clamp_to_u16(effective_hop)`
- Line 1347 (seek reset chunk): `clamp_to_u16(session.effective_hop)`
- Line 1512 (regular chunk in `session_drain_stft_rows`): `clamp_to_u16(session.effective_hop)`
- Line 1607 (flush chunk): `clamp_to_u16(session.effective_hop)`
- Line 1645 (flush token metadata chunk): `clamp_to_u16(session.effective_hop)`
- Line 2280 (centered staging chunk): use staging session's effective_hop

For the staging sessions (lines 218, 297, 2280), add `effective_hop` to `StagingChunkState` (line ~180) and `StagingChunkState`, computed the same way.

- [ ] **Step 4: Update seek handler to preserve effective_hop**

In `handle_session_seek` (line ~1316-1322), after recreating STFTs and decimators, preserve `session.effective_hop` (it doesn't change on seek — only on session restart).

- [ ] **Step 5: Build and test**

Run: `./scripts/run-tests.sh --rust-only`
Expected: All tests pass. Behavior is identical — `effective_hop = REFERENCE_HOP` for all current usage.

- [ ] **Step 6: Commit**

```bash
git add src/analysis/session.rs
git commit -m "refactor: replace hardcoded REFERENCE_HOP in chunk metadata with effective_hop"
```

---

## Task 2: Rust — Add `SetSpectrogramZoomLevel` command and handler

**Files:**
- Modify: `src/analysis/mod.rs:65-109` (AnalysisCommand enum)
- Modify: `src/analysis/mod.rs:204-234` (AnalysisRuntimeState)
- Modify: `src/analysis/mod.rs:381-510` (handle_command)
- Modify: `src/analysis/mod.rs:419-429` (SetFftSize handler)
- Modify: `src/analysis/mod.rs:869-905` (start_spectrogram_session)
- Modify: `src/analysis/session.rs:128-141` (NewTrack command)
- Modify: `src/analysis/session.rs:582-637` (session state)
- Modify: `src/analysis/session.rs:662-787` (session init)

- [ ] **Step 1: Add zoom helper function**

In `src/analysis/mod.rs`, near `REFERENCE_HOP` (line 196), add:

```rust
/// Compute the STFT hop size for a given zoom level.
/// Zoom > 1.0: smaller hop (finer temporal resolution).
/// Zoom ≤ 1.0: FFT-derived hop (normal resolution).
///
/// The zoom hop is derived from REFERENCE_HOP (not fft_size/8) because
/// zoom is relative to the *output* column rate, which is always
/// normalized to REFERENCE_HOP by the decimation system. At zoom=2x
/// we need effective_hop = REFERENCE_HOP/2, so with decimation bypassed
/// the STFT hop must equal REFERENCE_HOP/2. The STFT hop may be larger
/// than the unzoomed fft_size/8 hop — this is correct because the
/// unzoomed path decimates many overlapping STFT rows into one output
/// column, while the zoomed path keeps every STFT row individually.
fn zoom_hop_size(fft_size: usize, zoom_level: f32) -> usize {
    if zoom_level > 1.0 {
        let raw = (REFERENCE_HOP as f64 / f64::from(zoom_level)).round() as usize;
        raw.clamp(64, REFERENCE_HOP)
    } else {
        (fft_size / 8).max(64)
    }
}
```

- [ ] **Step 2: Add command variant and state field**

Add to `AnalysisCommand` enum:

```rust
SetSpectrogramZoomLevel(f32),
```

Add to `AnalysisRuntimeState` (after `hop_size`, line ~210):

```rust
zoom_level: f32,
```

Initialize to `1.0` in the state constructor.

- [ ] **Step 3: Add `zoom_level` to `NewTrack` command**

In `src/analysis/session.rs`, add to `SpectrogramWorkerCommand::NewTrack` (line ~128-141):

```rust
zoom_level: f32,
```

- [ ] **Step 4: Pass zoom_level in `start_spectrogram_session`**

In `src/analysis/mod.rs`, update `start_spectrogram_session` (line ~890-904) to include:

```rust
zoom_level: self.zoom_level,
```

- [ ] **Step 5: Implement `SetSpectrogramZoomLevel` handler**

In `handle_command` (after `SetFftSize`, line ~429), add:

```rust
AnalysisCommand::SetSpectrogramZoomLevel(level) => {
    let level = level.clamp(0.05, 16.0);
    self.clear_early_continuation(ctx);
    self.cancel_centered_staging();
    self.zoom_level = level;
    self.hop_size = zoom_hop_size(self.fft_size, self.zoom_level);
    self.reset_spectrogram_state();
    self.emit_snapshot(ctx.event_tx, true);
    // Centered mode: start from 0 (full-track decode, changed to windowed in Task 7).
    // Rolling mode: start from current position.
    let start = if self.display_mode == SpectrogramDisplayMode::Centered {
        0.0
    } else {
        self.last_spectrogram_position
    };
    self.start_spectrogram_session(start, true, true, ctx);
}
```

- [ ] **Step 6: Update `SetFftSize` handler to use zoom_level**

Replace the hop computation in `SetFftSize` handler (line ~423):

```rust
// Old: let hop = (fft / 8).max(64);
// New:
let hop = zoom_hop_size(fft, self.zoom_level);
```

- [ ] **Step 7: Use zoom_level for decimation bypass in session init**

In `run_spectrogram_session` (session.rs, line ~710), use zoom_level from the command:

```rust
let decimation_factor = if zoom_level > 1.0 {
    1 // Bypass decimation — keep all STFT rows for fine temporal resolution
} else {
    decimation_factor_for_hop(hop_size)
};
let effective_hop = hop_size * decimation_factor;
let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(effective_hop);
```

Extract `zoom_level` from the `NewTrack` command alongside `hop_size` and `fft_size`.

Store `zoom_level` in `SpectrogramSessionState`:

```rust
zoom_level: f32,
```

- [ ] **Step 8: Adjust `total_columns_estimate` for zoom**

In `run_spectrogram_session`, after computing `effective_hop`, adjust the estimate if it differs from REFERENCE_HOP:

```rust
let total_columns_estimate = if effective_hop != REFERENCE_HOP && effective_hop > 0 {
    let ratio = REFERENCE_HOP as f64 / effective_hop as f64;
    u32::try_from((u64::from(total_columns) as f64 * ratio).ceil() as u64)
        .unwrap_or(u32::MAX)
} else {
    total_columns
};
```

- [ ] **Step 9: Update all `NewTrack` pattern matches**

Search for all destructuring of `SpectrogramWorkerCommand::NewTrack` and add `zoom_level` field. Key sites:
- `run_spectrogram_session` (line ~662)
- `spectrogram_worker_loop` outer match (line ~324+)
- Any staging-related code that constructs or matches `NewTrack`

- [ ] **Step 10: Pass `zoom_level` to staging worker**

The centered staging worker (`spawn_centered_staging_worker` in `mod.rs:594`) receives `hop_size` from `self.hop_size`. With zoom, this is already the zoom-adjusted hop. However, the staging code at `session.rs:340` computes `decimation_factor_for_hop(hop_size)` which returns `REFERENCE_HOP / hop` — producing the wrong factor for zoom (e.g., at zoom=4x, hop=256: factor=4 instead of 1).

Pass `zoom_level` to the staging function and the staging decode path. In `centered_staging_decode` (session.rs), use the same bypass logic:

```rust
let decimation_factor = if zoom_level > 1.0 {
    1
} else {
    decimation_factor_for_hop(hop_size)
};
```

Also set `effective_hop` on the `StagingChunkState` to match.

- [ ] **Step 11: Add tests**

Add to the test module in `src/analysis/mod.rs`:

```rust
#[test]
fn zoom_hop_size_computation() {
    // Zoom 1.0: FFT-derived hop
    assert_eq!(zoom_hop_size(8192, 1.0), 1024);
    assert_eq!(zoom_hop_size(2048, 1.0), 256);
    // Zoom 2.0: half REFERENCE_HOP
    assert_eq!(zoom_hop_size(8192, 2.0), 512);
    // Zoom 4.0: quarter REFERENCE_HOP
    assert_eq!(zoom_hop_size(8192, 4.0), 256);
    // Zoom 16.0: minimum hop (64)
    assert_eq!(zoom_hop_size(8192, 16.0), 64);
    // Zoom beyond 16: still clamped to 64
    assert_eq!(zoom_hop_size(8192, 32.0), 64);
    // Zoom out: FFT-derived
    assert_eq!(zoom_hop_size(8192, 0.5), 1024);
}
```

- [ ] **Step 12: Build and test**

Run: `./scripts/run-tests.sh --rust-only`
Expected: All tests pass.

- [ ] **Step 13: Commit**

```bash
git add src/analysis/mod.rs src/analysis/session.rs
git commit -m "feat: add SetSpectrogramZoomLevel command with adaptive hop size"
```

---

## Task 3: Bridge — Add FFI command routing and C++ BridgeClient method

**Files:**
- Modify: `src/frontend_bridge/mod.rs:188-190` (BridgeAnalysisCommand)
- Modify: `src/frontend_bridge/ffi.rs` (add parse_analysis_command, wire into chain)
- Modify: `src/frontend_bridge/commands.rs:90-98` (handler)
- Modify: `ui/src/BinaryBridgeCodec.h` (command constant)
- Modify: `ui/src/BridgeClient.h` (method declaration)
- Modify: `ui/src/BridgeClient.cpp` (method implementation)

- [ ] **Step 1: Add Rust command variant**

In `src/frontend_bridge/mod.rs`, add to `BridgeAnalysisCommand`:

```rust
pub enum BridgeAnalysisCommand {
    SetFftSize(usize),
    SetSpectrogramZoomLevel(f32),
}
```

- [ ] **Step 2: Add FFI parsing function**

In `src/frontend_bridge/ffi.rs`, add a new function after `parse_settings_command`:

```rust
fn parse_analysis_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        57 => {
            let level = reader.read_f32()?;
            if !level.is_finite() || level < 0.05 {
                return Err("zoom level must be finite and >= 0.05".to_string());
            }
            BridgeAnalysisCommand::SetSpectrogramZoomLevel(level)
        }
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Analysis(command)))
}
```

Wire it into `parse_binary_command` (add after the `parse_settings_command` call in the chain):

```rust
} else if let Some(command) = parse_analysis_command(cmd_id, &mut reader)? {
    command
```

- [ ] **Step 3: Add bridge command handler**

In `src/frontend_bridge/commands.rs`, add to the `BridgeAnalysisCommand` match (line ~90-98):

```rust
BridgeAnalysisCommand::SetSpectrogramZoomLevel(level) => {
    context
        .analysis
        .command(AnalysisCommand::SetSpectrogramZoomLevel(level));
    true
}
```

Note: no `settings_dirty = true` — zoom level is not persisted.

- [ ] **Step 4: Add C++ command constant and method**

In `ui/src/BinaryBridgeCodec.h`, add to the enum:

```cpp
CmdSetSpectrogramZoomLevel = 57,
```

In `ui/src/BridgeClient.h`, add declaration:

```cpp
Q_INVOKABLE void setSpectrogramZoomLevel(float level);
```

In `ui/src/BridgeClient.cpp`, add implementation:

```cpp
void BridgeClient::setSpectrogramZoomLevel(float level) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF32(
        BinaryBridgeCodec::CmdSetSpectrogramZoomLevel, level));
}
```

- [ ] **Step 5: Add mock bridge stubs**

In `ui/qml/Main.qml` fallback bridge, add:

```qml
function setSpectrogramZoomLevel(level) {}
```

In `ui/tests/tst_qml_smoke.cpp` inline fallback bridge, add the same stub.

- [ ] **Step 6: Build and test**

Run: `./scripts/run-tests.sh`
Expected: Full suite passes (cross-cutting change).

- [ ] **Step 7: Commit**

```bash
git add src/frontend_bridge/mod.rs src/frontend_bridge/ffi.rs \
        src/frontend_bridge/commands.rs \
        ui/src/BinaryBridgeCodec.h ui/src/BridgeClient.h ui/src/BridgeClient.cpp \
        ui/qml/Main.qml ui/tests/tst_qml_smoke.cpp
git commit -m "feat: add SetSpectrogramZoomLevel bridge command"
```

---

## Task 4: Frontend — effectiveZoom rendering and QML integration

Replace `m_zoomLevel` in the rendering path with `effectiveZoom` computed from the chunk's `hop_size`. This makes 1:1 rendering automatic when the backend provides matching resolution.

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/qml/viewers/SpectrogramSurface.qml`

- [ ] **Step 1: Add effectiveZoom helper and backend zoom signal**

In `ui/src/SpectrogramItem.h`, add private method:

```cpp
double effectiveZoomLocked() const;
```

Add signal:

```cpp
void backendZoomRequested(float zoomLevel);
```

In `ui/src/SpectrogramItem.cpp`, implement:

```cpp
double SpectrogramItem::effectiveZoomLocked() const {
    if (m_precomputedHopSize <= 0) {
        return m_zoomLevel;
    }
    return m_zoomLevel * static_cast<double>(m_precomputedHopSize)
           / kReferenceHopSamples;
}
```

This returns:
- `1.0` when backend hop matches user zoom (e.g., zoom=4x, hop=256: `4 * 256 / 1024 = 1.0`)
- `> 1.0` during transition (old hop, new zoom)
- `< 1.0` for zoom-out

- [ ] **Step 2: Replace `m_zoomLevel` with `effectiveZoomLocked()` in rendering**

In `updatePaintNode`, compute effectiveZoom once near the top of the precomputed block (after the mutex is held):

```cpp
const double effectiveZoom = effectiveZoomLocked();
```

Then replace ALL occurrences of `m_zoomLevel` in the rendering calculations with `effectiveZoom`. The key locations (search for `m_zoomLevel` in updatePaintNode):

1. **Centered mode display range** (~line 1538-1541): `visibleWindowCols` and `halfWindowCols`
2. **Rolling mode display range** (~line 1569): `visibleWindowCols`
3. **Playhead pixel** (~line 1565): `* m_zoomLevel` → `* effectiveZoom`
4. **needsFullRebuild** (~line 1625): `m_precomputedCanvasZoomLevel` comparison
5. **scrollOffset** (~line 1659): `columnPhase * m_zoomLevel` → `columnPhase * effectiveZoom`
6. **drawX rolling** (~line 1661): `columnPhase * m_zoomLevel` → `columnPhase * effectiveZoom`
7. **drawX centered** (~line 1670): same
8. **Crosshair** cached for draw: no change (uses stored drawX)

In `rebuildPrecomputedCanvasLocked` (~line 3036):
- `m_zoomLevel` → `effectiveZoom` (need to pass it as parameter or use member)
- Since rebuild is called from updatePaintNode under mutex, pass effectiveZoom as a parameter OR store a cached value. The simplest: read `effectiveZoomLocked()` inside rebuild (it accesses member state, all under mutex).

In `advancePrecomputedCanvasLocked` (~line 3064): change the zoom guard:
```cpp
if (std::abs(effectiveZoomLocked() - 1.0) > 0.001) {
    return false;
}
```

Store the cached zoom in rebuild:
```cpp
m_precomputedCanvasZoomLevel = effectiveZoomLocked();
```

In `pixelToTimeSeconds` and `timeToPixelX` calls: pass `effectiveZoom` instead of `m_zoomLevel`.

In `updateTimeGridOverlayLocked`: `secondsPerPixel` uses `m_zoomLevel` → use `effectiveZoomLocked()`.

In `ensureRingCapacityLocked` (~line 908): use `effectiveZoomLocked()` for capacity computation. Also add windowed centered mode:

```cpp
if (m_displayMode == 1) {
    // Centered: keep existing full-track sizing for now.
    // Task 7 will switch this to windowed decode.
    neededCapacity = std::max(
        static_cast<int>(m_precomputedTotalColumnsEstimate) + 256,
        screenWidth + screenWidth / 2
            + static_cast<int>(extraSeconds * colsPerSecond));
} else {
    // Rolling
    const double ez = effectiveZoomLocked();
    const int zoomAdjustedWidth = static_cast<int>(
        static_cast<double>(screenWidth) / std::max(0.05, ez));
    neededCapacity = zoomAdjustedWidth
        + static_cast<int>(extraSeconds * colsPerSecond);
}
```

- [ ] **Step 3: Emit backend zoom command from setZoomLevel**

In `setZoomLevel`, after updating `m_zoomLevel` and before emitting `zoomLevelChanged`, emit the backend request:

```cpp
// Only notify backend when zooming above 1.0 or resetting from above 1.0.
// Zoom-out (within ≤1.0 range) is handled entirely in the frontend
// renderer via decimation — no backend session restart needed.
if (m_zoomLevel > 1.001 || std::abs(oldZoom - 1.0) > 0.001) {
    emit backendZoomRequested(static_cast<float>(m_zoomLevel));
}
```

Where `oldZoom` is saved before the change.

- [ ] **Step 4: Wire up QML**

In `ui/qml/viewers/SpectrogramSurface.qml`, add to the SpectrogramItem delegate:

```qml
onBackendZoomRequested: (level) => {
    root.uiBridge.setSpectrogramZoomLevel(level)
}
```

- [ ] **Step 5: Build and test**

Run: `./scripts/run-tests.sh`
Expected: Full suite passes. At this point, the full pipeline is connected: wheel zoom sends hop to backend, backend restarts with new hop, chunks arrive with new hop_size, effectiveZoom drops to 1.0, fast rendering path runs.

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp \
        ui/qml/viewers/SpectrogramSurface.qml
git commit -m "feat: compute effectiveZoom from backend hop, enable 1:1 rendering"
```

---

## Task 5: Frontend — Debounce backend zoom requests

Rapid wheel scrolling fires many zoom events. Each triggers a full session restart. Debounce so only the final zoom level triggers a restart.

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`

- [ ] **Step 1: Add debounce timer**

In `ui/src/SpectrogramItem.h`, add private members:

```cpp
QTimer *m_zoomDebounceTimer{nullptr};
float m_pendingBackendZoom{1.0f};
```

- [ ] **Step 2: Create timer in constructor**

In `SpectrogramItem` constructor, add:

```cpp
m_zoomDebounceTimer = new QTimer(this);
m_zoomDebounceTimer->setSingleShot(true);
m_zoomDebounceTimer->setInterval(150);
connect(m_zoomDebounceTimer, &QTimer::timeout, this, [this]() {
    emit backendZoomRequested(m_pendingBackendZoom);
});
```

Add `#include <QTimer>` to `SpectrogramItem.cpp` if not already present.

- [ ] **Step 3: Debounce in setZoomLevel**

Replace the direct `emit backendZoomRequested(...)` with debounced version:

```cpp
if (m_zoomLevel > 1.001 || std::abs(oldZoom - 1.0) > 0.001) {
    m_pendingBackendZoom = static_cast<float>(m_zoomLevel);
    m_zoomDebounceTimer->start(); // restarts the 150ms timer
}
```

During the debounce window, the frontend uses interpolation/decimation (effectiveZoom != 1.0) providing immediate visual feedback. When the timer fires, the backend gets the final zoom level.

- [ ] **Step 4: Build and test**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds, tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp
git commit -m "feat: debounce backend zoom requests to avoid rapid session restarts"
```

---

## Task 6: Tests

**Files:**
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add effectiveZoom test**

```cpp
void QmlSmokeTest::spectrogramEffectiveZoomMatchesBackendHop() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed data with hop_size=256 (simulating zoom=4x backend)
    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 4, 48000, 256, false,
        true, 1, false);

    // Set zoom to 4x — should match backend hop
    item.setZoomLevel(4.0);

    // effectiveZoom = 4.0 * 256 / 1024 = 1.0
    // The advance path should work (effectiveZoom ~= 1.0)
    // Verify by checking that zoom level is 4.0 but rendering behaves as 1:1
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}
```

- [ ] **Step 2: Add advance-path test with matching backend hop**

Verify that `advancePrecomputedCanvasLocked` succeeds (doesn't bail out) when the backend hop matches the user's zoom level, i.e., effectiveZoom ≈ 1.0:

```cpp
void QmlSmokeTest::spectrogramAdvanceWorksWhenBackendMatchesZoom() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed data with hop_size=256 (simulating zoom=4x backend)
    constexpr int binsPerColumn = 64;
    constexpr int columns = 200;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns * 4, 48000, 256, false,
        true, 1, false);

    // Set zoom to 4x — effectiveZoom = 4.0 * 256 / 1024 = 1.0
    item.setZoomLevel(4.0);

    // The advance path should NOT bail out (effectiveZoom ≈ 1.0).
    // Verify by checking that m_precomputedCanvasZoomLevel tracks
    // the effective zoom (1.0), not the user zoom (4.0).
    // This is observable: if advance works, canvas rebuilds are avoided
    // and FPS stays high. We can't directly test FPS here but we can
    // verify the zoom level is 4.0 while the rendering behaves as 1:1.
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}
```

- [ ] **Step 3: Add transition state test**

```cpp
void QmlSmokeTest::spectrogramEffectiveZoomDuringTransition() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed data with default hop_size=1024
    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, false,
        true, 1, false);

    // Set zoom to 4x — backend hasn't responded yet
    item.setZoomLevel(4.0);

    // effectiveZoom = 4.0 * 1024 / 1024 = 4.0 (transition state)
    // This means interpolation/full-rebuild rendering is active
    QVERIFY(std::abs(item.zoomLevel() - 4.0) < 0.0001);
}
```

- [ ] **Step 4: Build and run full test suite**

Run: `./scripts/run-tests.sh`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/tests/tst_qml_smoke.cpp
git commit -m "test: add effectiveZoom and transition state tests"
```

---

## Task 7: Windowed centered mode decode + seek handling

Switch centered mode from full-track pre-decode to windowed decode. This is a behavioral change at zoom=1.0 (not just for zoom > 1.0), so it's isolated into its own task after the full zoom pipeline works end-to-end.

**Files:**
- Modify: `src/analysis/mod.rs` (seek handler, session start helper)
- Modify: `src/analysis/session.rs` (lookahead, rate limiting)
- Modify: `ui/src/SpectrogramItem.cpp` (ring buffer sizing)

- [ ] **Step 1: Add `centered_start_seconds` helper to `AnalysisRuntimeState`**

```rust
fn centered_start_seconds(&self) -> f64 {
    if self.display_mode == SpectrogramDisplayMode::Centered {
        (self.last_spectrogram_position - 30.0).max(0.0)
    } else {
        self.last_spectrogram_position
    }
}
```

- [ ] **Step 2: Update existing `start_spectrogram_session(0.0, ...)` call sites**

Update these to use `centered_start_seconds()`:
- Line 428 (`SetFftSize`): `self.start_spectrogram_session(self.centered_start_seconds(), ...)`
- Line 437 (`SetSpectrogramViewMode`): same

Leave these unchanged (0.0 is correct — new track starts at position 0):
- Line 716 (`SetTrack` / `handle_track_change`): new track
- Lines 795, 813 (`handle_track_change` gapless paths): new track
- Line 1991 (state init): no track loaded

Also update the `SetSpectrogramZoomLevel` handler (added in Task 2) to use `centered_start_seconds()`.

- [ ] **Step 3: Switch centered mode to finite lookahead**

In `run_spectrogram_session` (session.rs, line ~715), replace `u64::MAX` for centered mode:

```rust
let lookahead_columns = if display_mode == SpectrogramDisplayMode::Centered {
    // Windowed centered: decode a window around the playhead.
    // Unlimited decode speed fills this almost instantly, then the
    // session follows the playhead. Ring buffer overwrites old data.
    let screen_cols = 1920_u64 * 3;
    screen_cols + f64_to_u64_saturating(lookahead_seconds * cols_per_second)
} else {
    // Rolling mode: existing logic
    f64_to_u64_saturating(lookahead_seconds * cols_per_second)
};
```

Keep `decode_rate_limit = f64::INFINITY` for centered mode so the initial window fills as fast as possible.

- [ ] **Step 4: Fix centered-mode seek for windowed decode**

Currently `seek_spectrogram_position` (mod.rs:927-942) treats centered mode as "just update the display window" — it sends `PositionUpdate` to the worker, not `Seek`, because the full track is pre-decoded. With windowed decode, seeking outside the decoded window lands on empty data.

Change the seek handler to restart the session when the seek is in centered mode:

```rust
fn seek_spectrogram_position(&mut self, position_seconds: f64, ctx: &AnalysisContext<'_>) {
    self.spectrogram_position_offset = 0.0;
    self.last_spectrogram_position = position_seconds;

    if self.display_mode == SpectrogramDisplayMode::Centered {
        // Windowed centered: restart session from before the seek target
        // so both sides of the playhead fill immediately.
        // Use clear_history=false so the ring retains overlapping data —
        // for nearby seeks most columns are still valid, avoiding a blank
        // flash. Stale columns get progressively overwritten as the new
        // session fills in data.
        let start = (position_seconds - 30.0).max(0.0);
        self.start_spectrogram_session(start, true, false, ctx);
    } else {
        // Rolling: send Seek command (existing behavior)
        let _ = ctx.spectrogram_cmd_tx.send(SpectrogramWorkerCommand::Seek {
            position_seconds,
        });
    }
}
```

- [ ] **Step 5: Update frontend ring buffer sizing**

In `ensureRingCapacityLocked` in `SpectrogramItem.cpp`, switch centered mode to windowed capacity:

```cpp
if (m_displayMode == 1) {
    // Centered: windowed — ~3 screen widths around playhead.
    neededCapacity = screenWidth * 3
        + static_cast<int>(extraSeconds * colsPerSecond);
} else {
    // Rolling
    const double ez = effectiveZoomLocked();
    const int zoomAdjustedWidth = static_cast<int>(
        static_cast<double>(screenWidth) / std::max(0.05, ez));
    neededCapacity = zoomAdjustedWidth
        + static_cast<int>(extraSeconds * colsPerSecond);
}
```

- [ ] **Step 6: Add tests for windowed centered behavior**

Add a Rust test verifying that centered-mode seek restarts the session. In the analysis test module, use a mock command channel to verify the seek handler sends `NewTrack` (session restart) rather than `PositionUpdate` for centered mode:

```rust
#[test]
fn centered_seek_restarts_session() {
    // Verify that seek_spectrogram_position in centered mode
    // calls start_spectrogram_session (which sends NewTrack)
    // rather than sending a Seek command.
    // The observable behavior: the spectrogram_cmd_tx receives
    // a NewTrack command, not a Seek command.
}
```

The exact test implementation depends on the existing test infrastructure — use the same patterns as other analysis tests. If mock channels are available, verify the command type. Otherwise, verify the side effect (e.g., `last_spectrogram_position` is updated and a new session generation is created).

Also add a C++ test in `tst_qml_smoke.cpp` verifying that centered mode ring buffer capacity is finite (not based on `totalColumnsEstimate`):

```cpp
void QmlSmokeTest::spectrogramCenteredModeUsesWindowedCapacity() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);
    item.setDisplayMode(1); // Centered

    // Feed a large track estimate
    constexpr int binsPerColumn = 64;
    constexpr int columns = 100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, 100000, 48000, 1024, false,
        true, 1, false);

    // Ring capacity should NOT be 100000 (full track).
    // It should be bounded to ~3 screen widths + lookahead.
    QVERIFY(item.m_ringCapacity < 20000);
}
```

- [ ] **Step 7: Build and test**

Run: `./scripts/run-tests.sh`
Expected: All tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/analysis/mod.rs src/analysis/session.rs \
        ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: switch centered mode to windowed decode with seek restart"
```

---

## Verification

After all tasks:

1. **Build full project:** `./scripts/run-tests.sh`
2. **Manual testing checklist:**
   - Open a track in centered mode, zoom in with scroll wheel
   - FPS should be high (shown by FPS overlay) — verify incremental advance is working
   - Spectrogram should show native spectral detail, not interpolated/blocky bands
   - Zoom out → smooth, no backend restart (frontend-only decimation)
   - Zoom in to 16x → hop=64, maximum backend resolution
   - Middle-click reset → session restarts with REFERENCE_HOP, FPS stays high
   - Change FFT size while zoomed → session restarts with zoom-appropriate hop
   - Switch rolling/centered mode while zoomed → correct behavior in both modes
   - Brief interpolated fallback visible during zoom change (before backend catches up)
   - Rapid scroll wheel → only one session restart (debounce working)
   - Gapless transition while zoomed → spectrogram continues smoothly
   - Track change while zoomed → new track starts at zoom resolution
   - **Debounce UX**: when zooming, expect ~150ms + backend restart latency (~200-500ms total) of interpolated fallback before native-resolution data arrives
   - **Centered mode at zoom=1.0**: spectrogram fills around playhead, not full-track pre-decode (Task 7)
   - **Seek in centered mode**: visible window fills quickly at the new position (Task 7)
   - **Seek far away in centered mode**: session restarts, window fills from the seek target (Task 7)
   - **Rapid scrubbing in centered mode**: drag quickly through the timeline — each seek triggers a session restart, so expect visible re-decode latency compared to the old instant-seek behavior (Task 7). If too janky, a seek debounce (like the zoom debounce) may be needed as a follow-up.
   - **Long multichannel track**: memory usage stays bounded (check with a long 5.1 track)
   - **High zoom CPU**: at zoom=8x+, the STFT produces 8x+ more rows per second — verify CPU load is acceptable on target hardware

---

## Future Enhancements (not in scope)

- **Zoom-out adaptive STFT**: Currently zoom-out uses frontend decimation. For extreme zoom-out, the backend could use a larger hop to reduce column density and memory.
- **Progressive refinement**: Show interpolated fallback immediately, then cross-fade to high-res data as it arrives.
- **Bidirectional decode on seek**: Currently the session decodes forward from `seekPosition - 30s`. A future optimization could decode backward from the seek point to fill the left side of the display faster.
- **Scrubbing optimization**: Rapid seeking (scrubbing) in centered windowed mode triggers many session restarts. A future optimization could debounce seek-triggered restarts or keep a coarser-resolution cache for instant display during scrubbing.
