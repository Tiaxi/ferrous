# Spectrogram Multires Zoom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a multiresolution spectrogram data path so zoomed-out scrolling, max zoom-out, and zoom transitions use stable pre-aggregated overview data instead of live per-pixel pooling over a single resolution.

**Architecture:** Keep `SpectrogramItem` as the UI state owner and preserve existing rolling/centered semantics. Add explicit spectrogram levels of detail (LOD) alongside the current level-0 stream, first in UI-side structures and then in backend/bridge protocol chunks; the renderer selects and blends levels based on current screen columns-per-pixel.

**Tech Stack:** Rust analysis worker and FFI protocol, Qt/C++ bridge and `SpectrogramItem`, public Qt `QSGMaterial` shader path, QtTest UI smoke tests, Rust unit tests.

---

## Current Baseline

The branch now has a public Qt GPU body renderer in `full` mode:

- Level-0 source columns are decoded by `src/analysis/session.rs`.
- Chunks are carried as `PrecomputedSpectrogramChunk` in `src/analysis/mod.rs`.
- The FFI frame is encoded in `src/frontend_bridge/ffi.rs` with magic `0xA2`.
- `BridgeClient::parsePrecomputedSpectrogramFrame()` decodes and routes chunks.
- `SpectrogramItem::feedPrecomputedChunk()` stores the active source data in the ring and page-backed retained textures.
- `ui/src/shaders/SpectrogramBodySample.frag` samples columns and currently smooths zoomed-out peak-hold by blending adjacent windows.

This is fast, but zoom-out still derives its visible result from one source resolution. At lower zooms, pixels cover multiple source columns, and live max-pooling can shimmer while strict integer windowing can look choppy.

## Target Behavior

- At normal and zoomed-in levels, level 0 remains the source of truth.
- At zoomed-out levels, the renderer samples a pre-aggregated LOD level whose column density is near screen density.
- Adjacent LOD levels cross-fade during zoom to avoid visible popping.
- The most zoomed-out view can fit the full track when a coarse full-track overview exists.
- Visible-first decode behavior remains: the current viewport fills first, then overview data extends ahead/behind and eventually across the full track.
- Rolling and centered semantics remain separate:
  - Rolling keeps write-order continuity and gapless history.
  - Centered uses full-track column indices and random-access pages.

## File Map

- Modify `src/analysis/mod.rs`: extend public spectrogram chunk metadata with explicit LOD fields.
- Modify `src/analysis/session.rs`: build and emit coarse LOD chunks from decoded level-0 columns.
- Modify `src/frontend_bridge/ffi.rs`: add binary protocol support for LOD frames while preserving old `0xA2` frames.
- Modify `ui/src/BridgeClient.h` and `ui/src/BridgeClient.cpp`: parse and route LOD chunk frames.
- Modify `ui/src/SpectrogramItem.h` and `ui/src/SpectrogramItem.cpp`: store LOD pages, select render levels, and update max zoom-out logic.
- Modify `ui/src/SpectrogramBodyMaterial.h` and `ui/src/SpectrogramBodyMaterial.cpp`: allow binding primary and secondary LOD textures plus blend factors.
- Modify `ui/src/shaders/SpectrogramBodySample.frag`: sample peak/mean LOD data and cross-fade levels.
- Modify `ui/tests/tst_qml_smoke.cpp`: add UI storage, selection, shader, and max-fit tests.
- Modify `src/frontend_bridge/ffi.rs` tests and `src/analysis/session.rs` tests: cover protocol and LOD aggregation.

## LOD Data Model

Each LOD level has:

- `level`: `0` for current source columns, `1` for 2-column aggregates, `2` for 4-column aggregates, etc.
- `factor`: `1 << level`.
- `start_column_index`: level-local column index.
- `source_start_column_index`: original level-0 column index for alignment.
- `source_column_span`: number of level-0 columns represented by each LOD column.
- `total_columns_estimate`: level-local estimate.
- `sample_rate_hz`.
- `effective_hop_size`: `level0_hop_size * factor`.
- `planes`: `peak` and `mean` for LOD levels greater than 0.

Recommended packed data for LOD levels greater than 0:

```text
column_count × channel_count × bins_per_column × plane_count bytes
```

Plane order:

```text
plane 0 = peak
plane 1 = mean
```

Level 0 can continue using the existing single-plane source format until the shader path supports both layouts.

## LOD Selection Rule

For the current render frame:

```cpp
const double columnsPerPixel = 1.0 / std::max(0.001, effectiveZoom);
const double desiredFactor = std::max(1.0, columnsPerPixel);
const int lowerLevel = std::clamp(
    static_cast<int>(std::floor(std::log2(desiredFactor))),
    0,
    maxAvailableLevel);
const int upperLevel = std::min(lowerLevel + 1, maxAvailableLevel);
const double lowerFactor = static_cast<double>(1 << lowerLevel);
const double upperFactor = static_cast<double>(1 << upperLevel);
const double lodBlend =
    upperLevel == lowerLevel
        ? 0.0
        : std::clamp(
              (desiredFactor - lowerFactor) / (upperFactor - lowerFactor),
              0.0,
              1.0);
```

Level-local zoom:

```cpp
const double selectedEffectiveZoom =
    effectiveZoom * static_cast<double>(1 << selectedLevel);
```

The renderer should prefer a level where `selectedEffectiveZoom` is between `0.75` and `1.5` pixels per LOD column. That avoids both excessive live pooling and excessive stretching.

---

### Task 1: Add UI-only LOD Types And Selection Tests

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

Add a private-test helper test in `ui/tests/tst_qml_smoke.cpp`:

```cpp
void QmlSmokeTest::spectrogramLodSelectionChoosesLevelNearScreenDensity() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(240);
    item.setDisplayMode(1);

    const auto levelAtHalfPixel =
        item.selectSpectrogramLodLevelForTests(0.5, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtHalfPixel.primaryLevel, 1);
    QCOMPARE(levelAtHalfPixel.secondaryLevel, 2);
    QVERIFY(levelAtHalfPixel.blend >= 0.0);
    QVERIFY(levelAtHalfPixel.blend <= 1.0);

    const auto levelAtOnePixel =
        item.selectSpectrogramLodLevelForTests(1.0, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtOnePixel.primaryLevel, 0);
    QCOMPARE(levelAtOnePixel.secondaryLevel, 1);
    QVERIFY(levelAtOnePixel.blend < 0.001);

    const auto levelAtFarZoom =
        item.selectSpectrogramLodLevelForTests(0.125, QVector<int>{0, 1, 2, 3});
    QCOMPARE(levelAtFarZoom.primaryLevel, 3);
    QCOMPARE(levelAtFarZoom.secondaryLevel, 3);
}
```

Declare the test in the `QmlSmokeTest` private slots.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: `spectrogramLodSelectionChoosesLevelNearScreenDensity` fails because `selectSpectrogramLodLevelForTests` does not exist.

- [ ] **Step 3: Add the minimal UI LOD selection type**

Add to `ui/src/SpectrogramItem.h` inside the test-visible/private section:

```cpp
struct SpectrogramLodSelection {
    int primaryLevel{0};
    int secondaryLevel{0};
    double blend{0.0};
};

SpectrogramLodSelection selectSpectrogramLodLevelForTests(
    double effectiveZoom,
    const QVector<int> &availableLevels) const;
```

Add to `ui/src/SpectrogramItem.cpp`:

```cpp
SpectrogramItem::SpectrogramLodSelection SpectrogramItem::selectSpectrogramLodLevelForTests(
    const double effectiveZoom,
    const QVector<int> &availableLevels) const {
    if (availableLevels.isEmpty()) {
        return {};
    }
    const double columnsPerPixel = 1.0 / std::max(0.001, effectiveZoom);
    const double desiredFactor = std::max(1.0, columnsPerPixel);
    int lowerLevel = static_cast<int>(std::floor(std::log2(desiredFactor)));
    lowerLevel = std::clamp(lowerLevel, availableLevels.front(), availableLevels.back());
    int upperLevel = std::min(lowerLevel + 1, availableLevels.back());
    if (!availableLevels.contains(lowerLevel)) {
        lowerLevel = availableLevels.front();
        for (const int level : availableLevels) {
            if (level <= static_cast<int>(std::floor(std::log2(desiredFactor)))) {
                lowerLevel = level;
            }
        }
    }
    if (!availableLevels.contains(upperLevel)) {
        upperLevel = lowerLevel;
        for (const int level : availableLevels) {
            if (level > lowerLevel) {
                upperLevel = level;
                break;
            }
        }
    }
    const double lowerFactor = static_cast<double>(1 << lowerLevel);
    const double upperFactor = static_cast<double>(1 << upperLevel);
    const double blend = upperLevel == lowerLevel
        ? 0.0
        : std::clamp((desiredFactor - lowerFactor) / (upperFactor - lowerFactor), 0.0, 1.0);
    return SpectrogramLodSelection{
        .primaryLevel = lowerLevel,
        .secondaryLevel = upperLevel,
        .blend = blend,
    };
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: all UI tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "test: add spectrogram lod selection contract"
```

---

### Task 2: Add Rust LOD Aggregation Helpers

**Files:**
- Modify: `src/analysis/session.rs`

- [ ] **Step 1: Write failing Rust tests for peak and mean aggregation**

Add tests to the existing `#[cfg(test)]` module in `src/analysis/session.rs`:

```rust
#[test]
fn lod_aggregation_preserves_peak_and_mean_per_bin() {
    let input = vec![
        10, 20, 30, 40, // column 0, bins 0..3
        30, 10, 90, 50, // column 1
        70, 80, 20, 10, // column 2
        90, 60, 40, 30, // column 3
    ];

    let aggregated = aggregate_lod_columns_peak_mean(&input, 4, 1, 2);

    assert_eq!(aggregated.column_count, 2);
    assert_eq!(aggregated.plane_count, 2);
    assert_eq!(
        aggregated.bytes,
        vec![
            30, 20, 90, 50, // output column 0 peak
            20, 15, 60, 45, // output column 0 mean
            90, 80, 40, 30, // output column 1 peak
            80, 70, 30, 20, // output column 1 mean
        ]
    );
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
./scripts/run-tests.sh --rust-only
```

Expected: compile failure because `aggregate_lod_columns_peak_mean` does not exist.

- [ ] **Step 3: Implement the aggregation helper**

Add near the precomputed chunk helper code in `src/analysis/session.rs`:

```rust
#[derive(Debug, Clone, PartialEq, Eq)]
struct AggregatedLodColumns {
    bytes: Vec<u8>,
    column_count: u16,
    plane_count: u8,
}

fn aggregate_lod_columns_peak_mean(
    input: &[u8],
    bins_per_column: usize,
    channel_count: usize,
    source_span_columns: usize,
) -> AggregatedLodColumns {
    if bins_per_column == 0 || channel_count == 0 || source_span_columns == 0 {
        return AggregatedLodColumns {
            bytes: Vec::new(),
            column_count: 0,
            plane_count: 2,
        };
    }
    let bytes_per_column = bins_per_column * channel_count;
    let input_columns = input.len() / bytes_per_column;
    let output_columns = input_columns / source_span_columns;
    let mut bytes = Vec::with_capacity(output_columns * bytes_per_column * 2);
    for out_col in 0..output_columns {
        let source_start = out_col * source_span_columns;
        for plane in 0..2 {
            for channel in 0..channel_count {
                for bin in 0..bins_per_column {
                    let mut peak = 0u8;
                    let mut sum = 0u32;
                    for source_delta in 0..source_span_columns {
                        let source_col = source_start + source_delta;
                        let idx = source_col * bytes_per_column + channel * bins_per_column + bin;
                        let value = input[idx];
                        peak = peak.max(value);
                        sum += u32::from(value);
                    }
                    let mean = ((sum + u32::try_from(source_span_columns / 2).unwrap_or(0))
                        / u32::try_from(source_span_columns).unwrap_or(1)) as u8;
                    bytes.push(if plane == 0 { peak } else { mean });
                }
            }
        }
    }
    AggregatedLodColumns {
        bytes,
        column_count: u16::try_from(output_columns).unwrap_or(u16::MAX),
        plane_count: 2,
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
./scripts/run-tests.sh --rust-only
```

Expected: all Rust checks and tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/analysis/session.rs
git commit -m "feat: add spectrogram lod aggregation helper"
```

---

### Task 3: Add A Versioned LOD Chunk Protocol

**Files:**
- Modify: `src/analysis/mod.rs`
- Modify: `src/frontend_bridge/ffi.rs`
- Modify: `ui/src/BridgeClient.h`
- Modify: `ui/src/BridgeClient.cpp`
- Test: `src/frontend_bridge/ffi.rs`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write Rust protocol test**

Add a test in `src/frontend_bridge/ffi.rs`:

```rust
#[test]
fn precomputed_lod_frame_includes_level_span_and_planes() {
    let chunk = PrecomputedSpectrogramChunk {
        track_token: 7,
        generation: 11,
        columns_u8: vec![1, 2, 3, 4, 5, 6, 7, 8],
        bins_per_column: 2,
        column_count: 2,
        channel_count: 1,
        start_column_index: 4,
        total_columns_estimate: 100,
        sample_rate_hz: 48_000,
        hop_size: 2048,
        coverage_seconds: 0.1,
        complete: false,
        buffer_reset: false,
        clear_history: false,
        lod_level: 1,
        lod_source_span: 2,
        lod_plane_count: 2,
    };

    let frame = encode_precomputed_spectrogram_chunk(&chunk);
    assert_eq!(frame[4], PRECOMPUTED_SPECTROGRAM_LOD_MAGIC);
    assert_eq!(frame[47], 1);
    assert_eq!(u16::from_le_bytes([frame[48], frame[49]]), 2);
    assert_eq!(frame[50], 2);
}
```

- [ ] **Step 2: Run Rust test to verify it fails**

Run:

```bash
./scripts/run-tests.sh --rust-only
```

Expected: compile failure for missing LOD fields and magic.

- [ ] **Step 3: Extend Rust chunk metadata and encoder**

Modify `PrecomputedSpectrogramChunk` in `src/analysis/mod.rs`:

```rust
pub lod_level: u8,
pub lod_source_span: u16,
pub lod_plane_count: u8,
```

Set defaults at every construction site:

```rust
lod_level: 0,
lod_source_span: 1,
lod_plane_count: 1,
```

In `src/frontend_bridge/ffi.rs`, keep `0xA2` for level 0 and add:

```rust
const PRECOMPUTED_SPECTROGRAM_LOD_MAGIC: u8 = 0xA3;
```

Append these bytes before `column_data` for `0xA3` only:

```text
[43]    u8  lod_level
[44..46] u16 lod_source_span
[46]    u8  lod_plane_count
[47..]  column_data
```

Keep old `0xA2` layout byte-for-byte for level 0 so existing parser tests still prove backward compatibility.

- [ ] **Step 4: Write C++ parser test**

Add a QtTest helper in `ui/tests/tst_bridge_client.cpp` that constructs a raw `0xA3` frame and verifies:

```cpp
QCOMPARE(stats.lodLevel, 2);
QCOMPARE(stats.lodSourceSpan, 4);
QCOMPARE(stats.lodPlaneCount, 2);
QVERIFY(stats.valid);
```

- [ ] **Step 5: Implement C++ parser fields**

Add fields to `BridgeClient::PrecomputedDispatchStats` in `ui/src/BridgeClient.h`:

```cpp
int lodLevel{0};
int lodSourceSpan{1};
int lodPlaneCount{1};
```

Modify `BridgeClient::parsePrecomputedSpectrogramFrame()`:

```cpp
const bool isLegacy = d[base] == 0xA2;
const bool isLod = d[base] == 0xA3;
if (!isLegacy && !isLod) {
    return stats;
}
const int metadataOffset = isLod ? base + 43 : -1;
const int dataOffset = isLod ? 51 : 47;
stats.lodLevel = isLod ? d[metadataOffset] : 0;
stats.lodSourceSpan = isLod ? readU16(metadataOffset + 1) : 1;
stats.lodPlaneCount = isLod ? d[metadataOffset + 3] : 1;
```

Update expected data length:

```cpp
const qint64 expectedDataLen =
    static_cast<qint64>(columns) * channelCount * bins * stats.lodPlaneCount;
```

- [ ] **Step 6: Route legacy and LOD chunks separately**

In `BridgeClient::routePrecomputedSpectrogramFrame()`:

```cpp
if (stats.lodLevel == 0) {
    item->feedPrecomputedChunk(...);
} else {
    item->feedPrecomputedLodChunk(
        rawView,
        stats.bins,
        route.channelIndex,
        stats.columns,
        stats.startIndex,
        stats.totalEstimate,
        stats.sampleRate,
        stats.hopSize,
        stats.trackToken,
        stats.generation,
        stats.lodLevel,
        stats.lodSourceSpan,
        stats.lodPlaneCount,
        stats.complete);
}
```

Add a stub `SpectrogramItem::feedPrecomputedLodChunk(...)` that records metadata but does not affect rendering yet.

- [ ] **Step 7: Run tests**

Run:

```bash
./scripts/run-tests.sh
```

Expected: full suite passes.

- [ ] **Step 8: Commit**

```bash
git add src/analysis/mod.rs src/analysis/session.rs src/frontend_bridge/ffi.rs ui/src/BridgeClient.h ui/src/BridgeClient.cpp ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: add spectrogram lod chunk protocol"
```

---

### Task 4: Store LOD Pages In SpectrogramItem

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write failing storage tests**

Add tests:

```cpp
void QmlSmokeTest::spectrogramStoresLodChunksSeparatelyFromLevelZero() {
    SpectrogramItem item;
    item.setWidth(400);
    item.setHeight(120);

    QByteArray level0(8, '\x10');
    item.feedPrecomputedChunk(level0, 4, 0, 2, 0, 64, 48000, 1024, false, true, 9, true, 1);

    QByteArray level1(16, '\x20'); // 2 cols × 1 channel × 4 bins × 2 planes
    item.feedPrecomputedLodChunk(level1, 4, 0, 2, 0, 32, 48000, 2048, 9, 1, 1, 2, 2, false);

    QCOMPARE(item.availableSpectrogramLodLevelsForTests(), QVector<int>({0, 1}));
    QCOMPARE(item.spectrogramLodPlaneCountForTests(1), 2);
}
```

- [ ] **Step 2: Run UI tests to verify failure**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: compile failure for missing test helpers or runtime failure because LOD storage is empty.

- [ ] **Step 3: Add LOD page state**

Add to `SpectrogramItem.h`:

```cpp
struct SpectrogramLodPage {
    quint64 trackToken{0};
    quint64 generation{0};
    int level{0};
    int binsPerColumn{0};
    int planeCount{1};
    int sourceSpan{1};
    qint64 startColumn{0};
    qint64 endColumn{-1};
    QByteArray bytes;
};

QVector<SpectrogramLodPage> m_spectrogramLodPages;
```

Add helpers:

```cpp
QVector<int> availableSpectrogramLodLevelsForTests() const;
int spectrogramLodPlaneCountForTests(int level) const;
```

- [ ] **Step 4: Implement `feedPrecomputedLodChunk`**

In `SpectrogramItem.cpp`, make it token/generation gated like `feedPrecomputedChunk()`:

```cpp
void SpectrogramItem::feedPrecomputedLodChunk(
    const QByteArray &data,
    int bins,
    int channelIndex,
    int columns,
    int startIndex,
    int totalEstimate,
    int sampleRate,
    int hopSize,
    quint64 trackToken,
    quint64 generation,
    int lodLevel,
    int lodSourceSpan,
    int lodPlaneCount,
    bool complete) {
    Q_UNUSED(channelIndex);
    Q_UNUSED(totalEstimate);
    Q_UNUSED(sampleRate);
    Q_UNUSED(hopSize);
    Q_UNUSED(complete);
    QMutexLocker lock(&m_stateMutex);
    if (lodLevel <= 0 || columns <= 0 || bins <= 0 || lodPlaneCount <= 0) {
        return;
    }
    if (m_precomputedTrackToken != 0 && trackToken != m_precomputedTrackToken) {
        return;
    }
    if (m_precomputedSessionGeneration != 0 && generation < m_precomputedSessionGeneration) {
        return;
    }
    m_spectrogramLodPages.push_back(SpectrogramLodPage{
        .trackToken = trackToken,
        .generation = generation,
        .level = lodLevel,
        .binsPerColumn = bins,
        .planeCount = lodPlaneCount,
        .sourceSpan = std::max(1, lodSourceSpan),
        .startColumn = startIndex,
        .endColumn = static_cast<qint64>(startIndex) + columns - 1,
        .bytes = data,
    });
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: all UI tests pass.

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: store spectrogram lod pages"
```

---

### Task 5: Emit Coarse LOD Chunks From The Analysis Worker

**Files:**
- Modify: `src/analysis/session.rs`
- Modify: `src/analysis/mod.rs`
- Test: `src/analysis/session.rs`

- [ ] **Step 1: Write failing worker emission test**

Add a test in `src/analysis/session.rs` that feeds enough level-0 columns into the session chunk path and asserts an emitted chunk has:

```rust
assert_eq!(chunk.lod_level, 1);
assert_eq!(chunk.lod_source_span, 2);
assert_eq!(chunk.lod_plane_count, 2);
assert_eq!(chunk.hop_size, level0_hop_size * 2);
```

Use a local `crossbeam_channel::unbounded()` event receiver, drive the smallest existing worker helper that emits precomputed chunks, and collect events until one chunk has `lod_level == 1`.

- [ ] **Step 2: Run Rust tests to verify failure**

Run:

```bash
./scripts/run-tests.sh --rust-only
```

Expected: test fails because the worker emits only level-0 chunks.

- [ ] **Step 3: Add per-session LOD builder state**

Add to `SpectrogramSessionState` in `src/analysis/session.rs`:

```rust
lod_pending_columns: Vec<u8>,
lod_max_level: u8,
```

Initialize:

```rust
lod_pending_columns: Vec::new(),
lod_max_level: 6,
```

When a level-0 output chunk is emitted, append its packed bytes to `lod_pending_columns`. For each level from 1 through `lod_max_level`, emit complete aggregate chunks when at least `1 << level` source columns are available.

- [ ] **Step 4: Emit LOD chunks without delaying visible level-0 chunks**

After sending the level-0 `PrecomputedSpectrogramChunk`, send LOD chunks opportunistically:

```rust
let lod = aggregate_lod_columns_peak_mean(
    &source_window,
    usize::from(chunk.bins_per_column),
    usize::from(chunk.channel_count),
    1usize << level,
);
let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
    PrecomputedSpectrogramChunk {
        track_token: chunk.track_token,
        generation: chunk.generation,
        columns_u8: lod.bytes,
        bins_per_column: chunk.bins_per_column,
        column_count: lod.column_count,
        channel_count: chunk.channel_count,
        start_column_index: chunk.start_column_index >> level,
        total_columns_estimate: (chunk.total_columns_estimate + ((1u32 << level) - 1)) >> level,
        sample_rate_hz: chunk.sample_rate_hz,
        hop_size: chunk.hop_size.saturating_mul(1u16 << level),
        coverage_seconds: chunk.coverage_seconds,
        complete: false,
        buffer_reset: false,
        clear_history: false,
        lod_level: level,
        lod_source_span: 1u16 << level,
        lod_plane_count: 2,
    }
));
```

Keep LOD emission bounded:

- Do not emit LOD chunks larger than the existing chunk budget.
- Do not emit levels whose level-local column count is zero.
- Do not emit LOD chunks before the corresponding level-0 reset has been sent.

- [ ] **Step 5: Run Rust tests**

Run:

```bash
./scripts/run-tests.sh --rust-only
```

Expected: all Rust checks and tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/analysis/mod.rs src/analysis/session.rs src/frontend_bridge/ffi.rs
git commit -m "feat: emit spectrogram lod chunks"
```

---

### Task 6: Upload And Render A Selected LOD Texture

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramBodySample.frag`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write failing material-state test**

Add:

```cpp
void QmlSmokeTest::spectrogramFullModeBindsSelectedLodTextureWhenZoomedOut() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "full");
    struct Guard { ~Guard() { qunsetenv("FERROUS_GPU_DEBUG_MODE"); } } guard;

    QQuickWindow window;
    window.resize(320, 120);

    SpectrogramItem item;
    item.setParentItem(window.contentItem());
    item.setWidth(320);
    item.setHeight(120);
    item.setDisplayMode(1);
    item.setZoomLevel(0.25);

    QByteArray level0(64 * 256, '\x30');
    item.feedPrecomputedChunk(level0, 64, 0, 256, 0, 256, 48000, 1024, false, true, 3, true, 1);
    QByteArray level2(64 * 64 * 2, '\x50');
    item.feedPrecomputedLodChunk(level2, 64, 0, 64, 0, 64, 48000, 4096, 3, 1, 2, 4, 2, false);

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window, 5000));

    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    auto *materialNode = spectrogramBodySampleMaterialNode(node.get());
    QVERIFY(materialNode != nullptr);
    auto *material = dynamic_cast<SpectrogramBodySampleMaterial *>(materialNode->material());
    QVERIFY(material != nullptr);
    QCOMPARE(material->state.primaryLodLevel, 2);
}
```

- [ ] **Step 2: Run UI tests to verify failure**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: compile failure for missing material fields or runtime failure because the level-0 texture is still selected.

- [ ] **Step 3: Extend material state**

Add to `SpectrogramBodyMaterial.h`:

```cpp
QSGTexture *secondarySourceTexture{nullptr};
QRectF secondarySourceRect;
QSize secondarySourceTextureSize;
float lodBlend{0.0F};
int primaryLodLevel{0};
int secondaryLodLevel{0};
int primaryPlaneCount{1};
int secondaryPlaneCount{1};
```

Update `compare()` and uniform data in `SpectrogramBodyMaterial.cpp` so these fields mark the material dirty.

- [ ] **Step 4: Add LOD texture upload path**

Add a small page texture resource per selected LOD level. Reuse the existing retained source page upload pattern rather than adding a separate renderer. The initial implementation can upload a contiguous selected visible range into one texture; page-backed retained LOD uploads can follow after visual validation.

The selected LOD source rect must be level-local:

```cpp
const qint64 lodDisplayLeft = displayLeft >> selectedLevel;
const qint64 lodDisplayRight = displayRight >> selectedLevel;
```

The target rect remains in widget pixels. The selected level's effective zoom is:

```cpp
const double lodEffectiveZoom =
    effectiveZoom * static_cast<double>(1 << selectedLevel);
```

- [ ] **Step 5: Update shader to sample LOD peak/mean**

In `SpectrogramBodySample.frag`, add uniforms for primary/secondary level metadata. For a two-plane LOD texture, derive rows as:

```glsl
float peakValue = sampleSourceValue(sourceColumn, sourceBin);
float meanValue = sampleSourceValue(sourceColumn, sourceBin + sourceRectHeight);
float stableValue = mix(meanValue, peakValue, 0.65);
```

For level 0, keep the existing single-plane behavior.

- [ ] **Step 6: Run UI tests**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: all UI tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBodySample.frag ui/tests/tst_qml_smoke.cpp
git commit -m "feat: render zoomed-out spectrogram lod textures"
```

---

### Task 7: Cross-Fade Adjacent LOD Levels During Zoom

**Files:**
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramBodySample.frag`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write failing cross-fade test**

Add a test that feeds level 1 and level 2 textures, sets `effectiveZoom` so `blend` is between `0.25` and `0.75`, and asserts:

```cpp
QVERIFY(material->state.sourceTexture != nullptr);
QVERIFY(material->state.secondarySourceTexture != nullptr);
QVERIFY(material->state.lodBlend > 0.25F);
QVERIFY(material->state.lodBlend < 0.75F);
QCOMPARE(material->state.primaryLodLevel, 1);
QCOMPARE(material->state.secondaryLodLevel, 2);
```

- [ ] **Step 2: Run UI tests to verify failure**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: failure because the material binds only one LOD.

- [ ] **Step 3: Bind primary and secondary textures**

In `SpectrogramItem::updatePaintNode()`, call the selection helper and resolve both level pages. If the secondary level is not available or does not cover the visible range, set `secondarySourceTexture = sourceTexture` and `lodBlend = 0.0F`.

- [ ] **Step 4: Blend in shader**

Add a helper:

```glsl
vec4 colorForLod(float value) {
    float mapped = texture(dbRemapTex, vec2(value, 0.5)).x;
    return texture(paletteTex, vec2(1.0 - mapped, 0.5));
}
```

Sample both LODs, map both through dB/palette, then:

```glsl
fragColor = mix(primaryColor, secondaryColor, lodBlend);
```

- [ ] **Step 5: Run UI tests**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: all UI tests pass.

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBodySample.frag ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: crossfade spectrogram lod levels"
```

---

### Task 8: Use Full-Track Overview For Max Zoom-Out

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `src/analysis/mod.rs`
- Modify: `src/analysis/session.rs`
- Test: `ui/tests/tst_qml_smoke.cpp`
- Test: `src/analysis/mod.rs`

- [ ] **Step 1: Write failing max-fit test**

Add a UI test:

```cpp
void QmlSmokeTest::spectrogramMaxZoomOutUsesOverviewToFitLongTrack() {
    SpectrogramItem item;
    item.setWidth(1200);
    item.setHeight(240);
    item.setDisplayMode(1);

    QByteArray level0(64 * 2000, '\x20');
    item.feedPrecomputedChunk(level0, 64, 0, 2000, 0, 30000, 48000, 1024, false, true, 5, true, 1);

    QByteArray level5(64 * 938 * 2, '\x20');
    item.feedPrecomputedLodChunk(level5, 64, 0, 938, 0, 938, 48000, 32768, 5, 1, 5, 32, 2, false);

    QVERIFY(item.minimumZoomLevel() < 0.05);
    item.setZoomLevel(item.minimumZoomLevel());
    QCOMPARE(item.centeredVisibleCoveragePercentForTests(), 100);
}
```

- [ ] **Step 2: Run UI tests to verify failure**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected: failure because `minimumZoomLevel()` is still clamped by the old backend minimum and the test helper does not exist.

- [ ] **Step 3: Make max zoom-out overview-aware**

Change `minimumZoomLevelLocked()` so it can return below `0.05` when a coarse LOD level covers the estimated full track:

```cpp
if (hasFullTrackOverviewLocked()) {
    const double fitZoom =
        static_cast<double>(width()) / referenceTrackColumnsAtLevelZeroLocked();
    return std::max(0.005, fitZoom);
}
return std::max(trackFit, kBackendMinZoom);
```

Do not ask the backend to restart at every sub-`0.05` zoom; render from the overview level when below backend minimum.

- [ ] **Step 4: Keep backend zoom clamp stable**

In `src/analysis/mod.rs`, preserve the current worker minimum for level-0 sessions. Add a comment and test proving UI-only overview zoom does not require a new worker restart below backend minimum.

- [ ] **Step 5: Run full suite**

Run:

```bash
./scripts/run-tests.sh
```

Expected: full suite passes.

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp src/analysis/mod.rs src/analysis/session.rs
git commit -m "feat: fit long tracks with spectrogram overview lod"
```

---

### Task 9: Add Diagnostics And Performance Guards

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/BridgeClient.cpp`
- Modify: `scripts/compare-spectrogram-diagnostics.py`
- Create: `scripts/test-compare-spectrogram-diagnostics.py`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add diagnostics fields**

Log these fields in the existing spectrogram paint/profile summary:

```text
lod_primary=...
lod_secondary=...
lod_blend=...
lod_tex_upload_ms=...
lod_available=...
lod_missing_reason=...
```

Add bridge frame diagnostics:

```text
lod_level=...
lod_span=...
lod_planes=...
```

- [ ] **Step 2: Update comparison script**

Add parsing in `scripts/compare-spectrogram-diagnostics.py` for:

```text
lod_primary
lod_secondary
lod_blend
lod_tex_upload_ms
```

Print a compact section:

```text
LOD:
  primary levels: ...
  blended frames: ...
  missing overview frames: ...
  upload avg/p90/p99: ...
```

- [ ] **Step 3: Add a diagnostics script parser test**

Create `scripts/test-compare-spectrogram-diagnostics.py`:

```python
#!/usr/bin/env python3
import importlib.util
import pathlib
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("compare-spectrogram-diagnostics.py")


class SpectrogramDiagnosticsLodTest(unittest.TestCase):
    def test_lod_fields_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "lod.log"
            path.write_text(
                "[ui-spectrogram] paint path=retained-full "
                "lod_primary=2 lod_secondary=3 lod_blend=0.375 lod_tex_upload_ms=0.120\n",
                encoding="utf-8",
            )
            spec = importlib.util.spec_from_file_location("compare_spectrogram", SCRIPT)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            summary = module.parse_log(path)
            self.assertEqual(summary["lod_primary_levels"][2], 1)
            self.assertEqual(summary["lod_secondary_levels"][3], 1)
            self.assertAlmostEqual(summary["lod_upload_ms"].max_value, 0.120)


if __name__ == "__main__":
    unittest.main()
```

If `compare-spectrogram-diagnostics.py` does not expose `parse_log()`, extract its current parsing logic into that function first and keep the command-line output unchanged.

- [ ] **Step 4: Run validation**

Run:

```bash
./scripts/run-tests.sh
python3 scripts/compare-spectrogram-diagnostics.py /home/tuomas/storage/ferrous/main.log /home/tuomas/.local/share/ferrous/diagnostics.log
python3 scripts/test-compare-spectrogram-diagnostics.py
```

Expected: tests pass and the comparison script prints the LOD section when LOD fields exist.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/BridgeClient.cpp scripts/compare-spectrogram-diagnostics.py scripts/test-compare-spectrogram-diagnostics.py
git commit -m "chore: add spectrogram lod diagnostics"
```

---

## Rollout Strategy

1. Keep the current level-0 renderer path working throughout.
2. Parse and store LOD data before rendering from it.
3. Render one selected LOD before cross-fading between two.
4. Enable overview-based max zoom-out only after full-track coarse coverage is available.
5. Compare before/after logs with `scripts/compare-spectrogram-diagnostics.py`.
6. Manually validate:
   - rolling mode at several zoomed-out levels
   - centered mode at several zoomed-out levels
   - max zoom-out on a 10+ minute song
   - seeking after startup
   - natural track transition
   - same-track restart from playlist double-click

## Risks And Mitigations

- **Risk:** LOD frames increase bridge/UI dispatch load.
  **Mitigation:** emit bounded chunks, cap levels, and track `lod_tex_upload_ms` plus bridge frame sizes.

- **Risk:** peak/mean blending softens transients too much.
  **Mitigation:** keep peak as a separate plane and tune shader blend; preserve level-0 at zoom-in.

- **Risk:** centered and rolling alignment diverge.
  **Mitigation:** store both level-local and source-level column indices; test centered fit and rolling continuity separately.

- **Risk:** protocol changes break old frames.
  **Mitigation:** keep `0xA2` legacy frames unchanged and add `0xA3` only for LOD frames.

- **Risk:** memory grows for long multi-channel tracks.
  **Mitigation:** use coarse levels for full-track overview, page high-detail data, and cap retained LOD pages by token/generation/viewport relevance.

## Completion Criteria

- Full test suite passes via `./scripts/run-tests.sh`.
- Logs show no new bridge dispatch spikes from LOD frames.
- Zoomed-out scrolling no longer has obvious low-FPS stepping.
- Shimmer is lower than the current live peak-hold smoothing path.
- Max zoom-out can fit the entire `3 Years Older`-class 10-minute track when the overview is complete.
- If overview is missing, the UI falls back to the current level-0 behavior without black frames or resets.
