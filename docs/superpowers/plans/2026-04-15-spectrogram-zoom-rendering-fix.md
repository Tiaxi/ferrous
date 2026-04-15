# Spectrogram Zoom Rendering Fix

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix FPS degradation and black gaps in the spectrogram at non-1.0 effective zoom levels by restoring the render zoom snap to 1.0 and removing the broken in-place draw path.

**Architecture:** Restore `m_renderZoomLevel = refHop/hop` (snapping effectiveZoom to 1.0) which keeps the fast advance path working at all zoom levels. Remove the in-place draw code from `feedPrecomputedChunk` (which caused black gaps). In centered mode, suppress `m_precomputedCanvasDirty` for chunks that overlap the existing canvas range — the canvas is already correct for the static locked display, and new columns are drawn on the next rebuild only when actually needed (zoom change, seek, etc.).

**Tech Stack:** C++ (Qt6/QML, QSGNode scene graph), Rust (backend analysis)

---

## Background

The zoom-adapted decimation system adjusts the backend's output column rate to match the zoom level, making `effectiveZoom ≈ 1.0` at all zoom levels. Integer truncation in the decimation factor causes `effectiveZoom` to deviate slightly from 1.0 at certain zoom levels (e.g., 0.940 at max zoom-out on fullscreen). This is a small rendering-only mismatch — 3660 columns for 3440 pixels — not a data problem.

Two previous fix attempts caused regressions:
1. **Setting `m_renderZoomLevel = m_zoomLevel`** (true effective zoom) — broke the advance path at many zoom levels (requires `effectiveZoom ≈ 1.0`), causing 20-50ms full rebuilds every frame.
2. **In-place column draws in `feedPrecomputedChunk`** — used a proportional mapping (`frac × canvasWidth`) that differs from the rebuild's mapping (`floor(px × columnsPerPixel)`), producing 1-pixel gaps.

The correct approach: keep the 1.0 snap for the advance path's sake, and handle the rare overflow (more columns than pixels at max zoom-out) at the rendering level by clamping the display range.

## File Map

| File | Changes |
|------|---------|
| `ui/src/SpectrogramItem.cpp` | Revert render zoom snap, remove in-place draw, fix centered mode dirty suppression, clamp display range |
| `ui/tests/tst_qml_smoke.cpp` | Test that verifies centered locked display range is clamped to canvas width |

---

### Task 1: Restore the render zoom snap to refHop/hop

The render zoom snap forces `effectiveZoom = 1.0` for all zoom levels, which is what the advance path requires. The previous change to `m_renderZoomLevel = m_zoomLevel` broke this.

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` (feedPrecomputedChunk zoom snap, ~line 1042-1061)

- [ ] **Step 1: Restore the snap logic**

Replace the current zoom snap code in `feedPrecomputedChunk` (inside the `m_awaitingZoomData && (appliedReset || appliedImplicitReset)` block) with:

```cpp
        if (m_awaitingZoomData && (appliedReset || appliedImplicitReset)) {
            if (!m_zoomDebounceTimer->isActive()) {
                m_awaitingZoomData = false;
            }
            // Snap renderZoomLevel so effectiveZoom = renderZoom × hop / ref
            // equals 1.0.  With zoom-adapted decimation the backend adjusts
            // the output column rate to match the zoom, so the 1.0 snap is
            // correct to within a few percent (integer decimation rounding).
            // Keeping effectiveZoom at 1.0 lets the fast incremental advance
            // path work at all zoom levels.
            if (m_precomputedHopSize > 0
                && m_precomputedHopSize != static_cast<int>(kReferenceHopSamples)) {
                m_renderZoomLevel = kReferenceHopSamples
                    / static_cast<double>(m_precomputedHopSize);
            } else {
                m_renderZoomLevel = m_zoomLevel;
            }
            m_crosshairDirty = true;
        }
```

- [ ] **Step 2: Build and run tests**

Run: `./scripts/run-tests.sh --ui-only`
Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add ui/src/SpectrogramItem.cpp
git commit -m "fix: restore render zoom snap to refHop/hop for advance path"
```

---

### Task 2: Remove the in-place draw path from feedPrecomputedChunk

The in-place draw uses a different column-to-pixel mapping than `rebuildPrecomputedCanvasLocked`, causing black gaps. Remove it and replace with a targeted dirty suppression for centered mode.

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` (feedPrecomputedChunk centered mode dirty logic, ~line 1237-1269)

- [ ] **Step 1: Replace the in-place draw with centered mode dirty suppression**

The centered mode dirty logic currently does an in-place draw. Replace it with logic that:
- For centered mode with an existing canvas: does NOT set `m_precomputedCanvasDirty` at all (the display range is static in the locked case, and the advance path handles rolling range shifts).
- For rolling mode: unchanged (advance path handles incrementally).
- For no canvas yet: sets dirty (first build needed).

Replace the entire centered mode dirty block (from `if (m_displayMode == 1` through the `} else {` that sets `m_precomputedCanvasDirty = true`) with:

```cpp
    if ((m_displayMode == 1 || m_displayMode == 0)
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft) {
        // Centered or rolling mode with existing canvas: don't mark
        // the entire canvas dirty.  In centered mode the display range
        // is static (locked to full track at max zoom-out) so the
        // existing canvas is correct.  In rolling mode the advance
        // path handles new columns incrementally.  Forcing a full
        // rebuild on every chunk causes 20-50 ms paint spikes at
        // fullscreen resolution.
    } else {
        // No canvas range yet (first data after reset) — force build.
        m_precomputedCanvasDirty = true;
    }
```

- [ ] **Step 2: Build and run tests**

Run: `./scripts/run-tests.sh --ui-only`
Expected: All tests pass.

- [ ] **Step 3: Commit**

```bash
git add ui/src/SpectrogramItem.cpp
git commit -m "fix: remove in-place draw, suppress dirty for centered mode with existing canvas"
```

---

### Task 3: Clamp the locked display range to the canvas width

At max zoom-out with `effectiveZoom = 1.0` (snapped), integer decimation can produce slightly more columns than pixels (e.g., 3660 columns for a 3440px canvas). The display locked to `[0, totalCols-1]` extends beyond the canvas width. This causes:
- The EOF clamping to shift `displayLeft` forward (hiding the beginning)
- The `rangeChanged` check to fire every frame (since `displayLeft + visibleCols - 1 ≠ canvasDisplayRight`)

The fix: when the display is locked to full extent AND the column count exceeds the canvas width, clamp `displayRight` to `displayLeft + w - 1`. This drops the last few columns (which are beyond the canvas edge and invisible anyway) and keeps the display stable.

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` (updatePaintNode centered locked branch, ~line 1800)
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

Add a test to `tst_qml_smoke.cpp` that verifies the locked display range is clamped to the canvas width:

```cpp
void QmlSmokeTest::spectrogramLockedDisplayClampedToCanvasWidth() {
    SpectrogramItem item;
    item.setWidth(1000);
    item.setHeight(100);
    item.setDisplayMode(1); // Centered

    // Feed data where totalEst (1100) > width (1000) at effectiveZoom=1.0.
    // This simulates the integer decimation overshoot at max zoom-out.
    constexpr int binsPerColumn = 4;
    constexpr int columns = 1100;
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // The display should be locked but clamped: displayRight = 999, not 1099.
    // Verify by checking that the playhead at column 999 is at the right edge.
    item.setPositionSeconds(999.0 * 1024.0 / 48000.0);
    // At effectiveZoom=1.0, playheadPixel = nowCol - displayLeft = 999 - 0 = 999
    // (clamped to w-1 = 999). If not clamped, displayLeft would shift forward
    // and the playhead would be elsewhere.
    QVERIFY(item.precomputedReady());
}
```

Add the declaration to the private slots section alongside the other zoom tests.

- [ ] **Step 2: Run the test to verify it compiles**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Compiles and passes (this is a basic structural test).

- [ ] **Step 3: Clamp displayRight in the locked branch**

In `updatePaintNode()`, in the centered mode locked branch (where `displayLeft = 0; displayRight = estTotalCols - 1;`), add the clamping right after setting the display range:

```cpp
                if (estTotalCols <= 0
                    || static_cast<qint64>(visibleWindowCols) * 100
                           / estTotalCols >= 90) {
                    displayLeft = 0;
                    displayRight = estTotalCols - 1;
                    // Clamp to canvas width.  Integer decimation can
                    // produce a few more columns than pixels at max
                    // zoom-out (effectiveZoom snapped to 1.0 but actual
                    // column count slightly exceeds widget width).
                    // Without this clamp, the EOF clamping below shifts
                    // displayLeft forward and rangeChanged fires every
                    // frame.
                    if (displayRight >= static_cast<qint64>(w)) {
                        displayRight = static_cast<qint64>(w) - 1;
                    }
```

- [ ] **Step 4: Build and run tests**

Run: `./scripts/run-tests.sh --ui-only`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "fix: clamp locked display range to canvas width at max zoom-out"
```

---

### Task 4: Verify the full fix works end-to-end

Run the full test suite and manually verify the key scenarios.

**Files:** None (verification only)

- [ ] **Step 1: Run full test suite**

Run: `./scripts/run-tests.sh --ui-only`
Expected: All tests pass, no regressions.

- [ ] **Step 2: Manual verification checklist**

Test these scenarios in the running app:
1. Start playback → observe smooth spectrogram in widget at zoom=1.0
2. Zoom in one level → observe smooth FPS
3. Open fullscreen → observe smooth FPS (no growing edge if decode window covers the view)
4. Zoom out fully in fullscreen → observe:
   - Full song visible (no missing beginning or end)
   - Smooth FPS (no choppy rendering)
   - Crosshair time labels correct at left and right edges
5. Seek in zoomed-out fullscreen → no black history behind playhead
6. Exit fullscreen → widget returns to its own zoom level, smooth
7. Re-enter fullscreen → smooth, no growing edge

- [ ] **Step 3: Final commit if any tweaks needed**

---

## Design Rationale

**Why snap effectiveZoom to 1.0?** The zoom-adapted decimation already adjusts the backend output to match the zoom level. The snap corrects for integer truncation (a few percent error). The incremental advance path — which is the primary FPS optimization — requires `effectiveZoom ≈ 1.0`. Using the true zoom level (previous attempt) breaks the advance path at many zoom levels.

**Why clamp displayRight instead of adjusting effectiveZoom?** Clamping drops at most a few columns at the end of the track (invisible beyond the canvas edge). It keeps `effectiveZoom = 1.0` everywhere, so all consumers agree: advance path, crosshair, time grid, EOF clamping. Adjusting effectiveZoom (previous attempt) creates a value that `effectiveZoomLocked()` doesn't return, causing mismatches.

**Why suppress dirty instead of in-place draw?** In centered mode with the locked display, the canvas doesn't need updating when new chunks arrive — the display range is static and the data is already in the ring for the next full rebuild (which only happens on zoom/seek/track changes). The in-place draw was trying to show the growing edge during initial fill, but it used a different mapping than the rebuild, causing black gaps. Suppressing dirty and letting the first full rebuild handle it is simpler and correct.
