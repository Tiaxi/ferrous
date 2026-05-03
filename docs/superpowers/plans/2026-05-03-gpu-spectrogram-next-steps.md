# GPU Spectrogram Next Steps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the OpenGL spectrogram renderer measurably faster than the CPU/display-image path by removing avoidable CPU work, adding comparable diagnostics, and then replacing the monolithic retained source texture with bounded retained source pages.

**Architecture:** Keep the current public Qt scene graph/OpenGL approach. Treat retained GPU source textures as the normal `full`, `texture-node`, and `shader-source-only` path when coverage is valid, and keep CPU display images only for software/unsupported rendering, texture-limit fallback, sparse overflow fallback, and explicit debug validation. Add page planning before expanding rendering so correctness is testable without requiring an OpenGL scene graph.

**Tech Stack:** Qt 6/QML scene graph, public `QSGTexture`/`QSGGeometryNode` APIs, C++ QtTest in `ui/tests/tst_qml_smoke.cpp`, Rust/C++ project validation through `./scripts/run-tests.sh`.

---

## Current Status

Last updated: 2026-05-03 on branch `gpu-spectrogram-overhaul`.

Implemented checkpoint:
- Commit: `0bcde06 fix: avoid retained spectrogram display warmup`
- Validation: `./scripts/run-tests.sh --ui-only`
- Result: 3/3 UI test targets passed, 0 failures.

Completed in this checkpoint:
- [x] Add comparable renderer telemetry fields for retained/full/fallback path selection.
- [x] Record display-image build time separately from total cache sync time.
- [x] Record retained upload time, upload spans, uploaded columns, and retained texture reset count.
- [x] Record fallback reason strings for diagnostics.
- [x] Add `scripts/compare-spectrogram-diagnostics.py` for A/B log summaries.
- [x] Stop normal OpenGL retained full mode from building `m_gpuDisplayImage` purely because centered playback is paused or in zoom-fill.
- [x] Remove the retained-valid warmed-display-image reveal in `full` mode; retained rendering reveals directly when retained coverage is valid.
- [x] Keep CPU display-image fallback for software scene graph, unsupported renderer, texture-limit fallback, and retained segment overflow.
- [x] Add Qt tests for retained full mode avoiding CPU display image on OpenGL.
- [x] Update max-zoom reveal test intent from warmed CPU image to direct retained full-mode reveal.
- [x] Add retained source page constants/state scaffolding.
- [x] Add pure retained page span planning across single-page, page-boundary, and rolling-wrap ranges.

Not completed yet:
- [ ] Upload retained source data into retained source pages.
- [ ] Replace the fixed two-segment retained render plan with a dynamic segment list.
- [ ] Pool dynamic retained segment nodes/materials.
- [ ] Retarget `full`, `texture-node`, and `shader-source-only` debug modes to page-backed source textures.
- [ ] Run live A/B logs against `main` on an actual OpenGL scene graph.

## Files And Responsibilities

- `ui/src/SpectrogramItem.h`: renderer state, retained render planning structs, future retained page structs, helper declarations.
- `ui/src/SpectrogramItem.cpp`: render-path selection, retained source sync, display-image fallback, render plan construction, scene graph node configuration, profile logging.
- `ui/src/SpectrogramSourceTexture.h`: retained source texture owner and upload span types. Future page container types may live here only if they are texture-owner-specific; otherwise keep page planning in `SpectrogramItem`.
- `ui/src/SpectrogramSourceTexture.cpp`: retained source texture lifecycle and OpenGL upload behavior. Avoid changing public Qt approach.
- `ui/tests/tst_qml_smoke.cpp`: QtTest coverage for retained planning, fallback behavior, page mapping, debug modes, and OpenGL-only scene graph binding tests.
- `scripts/compare-spectrogram-diagnostics.py`: log A/B summary for paint latency, frame gaps, event stalls, cache/display-image time, retained upload time, paths, fallbacks, CPU rebuilds, and retained reset/upload counters.

## Constraints

- Use public Qt APIs only. Do not add `Qt6::GuiPrivate`.
- Do not move to `QQuickRhiItem`.
- Do not add compute shaders, SSBOs, or protocol redesign in this phase.
- Keep RGBA source texture format for this phase.
- Preserve CPU display-image fallback in-tree.
- Validate with `./scripts/run-tests.sh --ui-only` for UI-only changes.
- Use full `./scripts/run-tests.sh` if touching both `src/` and `ui/`.

---

## Task 1: Telemetry First

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Create: `scripts/compare-spectrogram-diagnostics.py`
- Test: `ui/tests/tst_qml_smoke.cpp` where renderer behavior changes are observable

- [x] **Step 1: Add per-paint path labels**

Use path names:
- `cpu-display`
- `retained-full`
- `retained-texture-node`
- `fallback-display`
- `frozen-display`
- `empty`

Implemented in `SpectrogramItem::updatePaintNode()` as `profilePaintPath`.

- [x] **Step 2: Add timing fields**

Add profile fields:
- `cache_ms`
- `display_ms`
- `retained_upload_ms`
- `overlay_build_ms`
- `tex_ms`
- `overlay_tex_ms`
- total paint `ms`

Implemented in `[ui-spectrogram] paint_spike ...` log lines.

- [x] **Step 3: Add counters and fallback reasons**

Add fields:
- `cpu_rebuild`
- `retained_reset`
- `retained_spans`
- `retained_cols`
- `fallback`

Implemented in `[ui-spectrogram] paint_spike ...` log lines.

- [x] **Step 4: Add log comparison script**

Run:

```bash
scripts/compare-spectrogram-diagnostics.py /tmp/main.log /tmp/gpu.log
```

Expected output includes:
- paint samples, avg, p90, p99, max
- cache/display/retained-upload/texture-upload averages
- frame gap and event stall counts
- CPU rebuild count
- retained reset count
- retained uploaded columns
- render path counts
- fallback counts

- [x] **Step 5: Validate**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Observed on 2026-05-03:

```text
100% tests passed, 0 tests failed out of 3
```

---

## Task 2: Eliminate Unnecessary CPU Display Work

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [x] **Step 1: Add retained-full OpenGL no-CPU-image test**

Test name:

```cpp
void QmlSmokeTest::spectrogramGpuFullModeUsesRetainedSourceWithoutCpuDisplayImage()
```

Invariant:
- `FERROUS_GPU_DEBUG_MODE=full`
- OpenGL scene graph
- centered retained source coverage complete
- body texture/material uses `m_gpuSourceTexture.texture()`
- `m_gpuDisplayImage.isNull()`

- [x] **Step 2: Stop retained-capable OpenGL from maintaining CPU image by default**

Change `maintainCpuDisplayImage` so normal retained OpenGL rendering only maintains CPU pixels when retained rendering is unavailable:

```cpp
const bool maintainCpuDisplayImage =
    currentWindow == nullptr
    || !canUseRetainedSourceTextureBody;
```

- [x] **Step 3: Remove warmed-display reveal when retained coverage is valid**

Retained full mode should not use the one-frame CPU display reveal when `retainedTextureCoversVisibleRange` is true:

```cpp
const bool revealViaWarmedDisplayImage =
    freezeCanRevealFinalFrame
    && gpuDebugMode == SpectrogramGpu::DebugMode::Full
    && !retainedTextureCoversVisibleRange
    && !m_gpuDisplayImage.isNull();
```

- [x] **Step 4: Preserve retained freeze without forcing CPU snapshot**

`captureZoomFillFrozenCacheLocked()` should only build a CPU snapshot when retained freeze metadata is unavailable:

```cpp
if (!m_zoomFillFrozenRetainedAvailable || m_gpuSourceTexture.texture() == nullptr) {
    ensureZoomFillSnapshotImageLocked();
}
```

- [x] **Step 5: Keep explicit CPU fallbacks**

Confirmed retained segment overflow and non-retained render paths can still call `syncPrecomputedDisplayImageLocked(..., true)`.

- [x] **Step 6: Validate**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Observed on 2026-05-03:

```text
100% tests passed, 0 tests failed out of 3
```

---

## Task 3: Introduce Retained Source Pages

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [x] **Step 1: Add page constants and structs**

Add to `SpectrogramItem` private section:

```cpp
static constexpr int kRetainedSourcePageWidth = 2048;

struct RetainedSourcePageKey {
    quint64 trackToken{0};
    bool rollingMode{false};
    qint64 pageIndex{0};
};

struct RetainedSourcePageState {
    RetainedSourcePageKey key;
    SpectrogramSourceTexture texture;
    int width{0};
    int binsPerColumn{0};
    qint64 uploadedDisplayLeft{0};
    qint64 uploadedDisplayRight{-1};
    QVector<SpectrogramSourceUploadSpan> dirtySpans;
};
```

Use an owning container such as:

```cpp
QVector<std::unique_ptr<RetainedSourcePageState>> m_gpuSourcePages;
```

If `std::unique_ptr` is used, include `<memory>` in `ui/src/SpectrogramItem.h`.

- [x] **Step 2: Write page span planning tests**

Add tests in `ui/tests/tst_qml_smoke.cpp`:

```cpp
void QmlSmokeTest::spectrogramRetainedPagePlanSplitsSinglePageRange();
void QmlSmokeTest::spectrogramRetainedPagePlanSplitsAcrossPageBoundary();
void QmlSmokeTest::spectrogramRetainedPagePlanSplitsRollingWrap();
```

Expected invariants:
- display `0..2047` maps to one page span
- display `2040..2055` maps to two page spans
- rolling wrap maps to two or more page spans without forcing CPU fallback

- [x] **Step 3: Run tests and verify RED**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected before implementation:

```text
FAIL: page planning helper/test symbols are missing or return empty plans
```

- [x] **Step 4: Implement pure page planner**

Add a helper that does not require an OpenGL scene graph:

```cpp
struct RetainedPageSpan {
    qint64 pageIndex{0};
    int pageXBegin{0};
    int pageXEnd{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
};

QVector<RetainedPageSpan> buildRetainedPageSpansLocked(
    qint64 displayLeft,
    qint64 displayRight,
    qint64 renderableRight,
    bool rollingMode) const;
```

The helper must:
- stop at `min(displayRight, renderableRight)`
- skip missing ring slots
- split whenever page index changes
- split whenever ring slots are non-contiguous within a page
- preserve `qint64` display indices

- [x] **Step 5: Run tests and verify GREEN**

Run:

```bash
./scripts/run-tests.sh --ui-only
```

Expected:

```text
100% tests passed, 0 tests failed out of 3
```

- [x] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: plan retained spectrogram source pages"
```

---

## Task 4: Upload Dirty Spans Into Retained Pages

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add upload planner tests**

Add tests:

```cpp
void QmlSmokeTest::spectrogramRetainedPageUploadPlannerUploadsOnlyDirtyPageSpans();
void QmlSmokeTest::spectrogramRetainedPageUploadPlannerResetsPageOnTokenChange();
void QmlSmokeTest::spectrogramRetainedPageUploadPlannerUsesGpuLimitForPageWidth();
```

Expected invariants:
- clean visible pages upload no spans
- newly visible columns upload only affected page spans
- token or rolling-mode change resets affected pages
- page width is `min(2048, GL_MAX_TEXTURE_SIZE)` and never exceeds GPU limit

- [ ] **Step 2: Run tests and verify RED**

```bash
./scripts/run-tests.sh --ui-only
```

- [ ] **Step 3: Replace monolithic source texture sync with page sync**

Replace `syncRetainedSourceTextureLocked()` internals with page-oriented logic:
- build visible page spans
- find or create page state by `(trackToken, rollingMode, pageIndex)`
- call `SpectrogramSourceTexture::ensureCreated(window, pageWidth, bins)`
- upload only dirty page-local spans
- update page uploaded range
- retire pages that do not match current token/rolling mode or are no longer needed

Keep the existing monolithic helper available only until all render routing has moved to pages.

- [ ] **Step 4: Update telemetry**

Increment existing fields:
- `retained_reset` when a page texture is created/recreated
- `retained_spans` by number of uploaded page spans
- `retained_cols` by uploaded page-local column count

- [ ] **Step 5: Run tests and verify GREEN**

```bash
./scripts/run-tests.sh --ui-only
```

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: upload retained spectrogram source pages"
```

---

## Task 5: Render From Dynamic Page Segments

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Replace fixed retained segment array**

Replace:

```cpp
std::array<RetainedRenderSegment, 2>
```

with:

```cpp
QVector<RetainedRenderSegment>
```

Update `RetainedRenderPlan`:

```cpp
struct RetainedRenderPlan {
    QVector<RetainedRenderSegment> segments;
    bool haveSegments{false};
    bool overflowed{false};
    bool coversRange{false};
};
```

- [ ] **Step 2: Add dynamic segment tests**

Add tests:

```cpp
void QmlSmokeTest::spectrogramRetainedRenderPlanCoversOnePage();
void QmlSmokeTest::spectrogramRetainedRenderPlanCoversTwoPages();
void QmlSmokeTest::spectrogramRetainedRenderPlanCoversThreePages();
void QmlSmokeTest::spectrogramRetainedRenderPlanStillFallsBackForSparseOverflow();
```

Expected:
- one-page fullscreen window produces one segment
- crossing one page boundary produces two segments
- crossing two page boundaries produces three segments
- sparse holes still set `coversRange=false`; CPU fallback remains visible

- [ ] **Step 3: Run tests and verify RED**

```bash
./scripts/run-tests.sh --ui-only
```

- [ ] **Step 4: Pool scene graph body/tail nodes**

Replace fixed base/tail retained rendering with a node pool:
- one node per retained segment
- reuse nodes across frames
- hide unused nodes with empty geometry/source rects
- do not allocate/free segment nodes every frame

Keep separate handling for:
- `texture-node`
- `shader-source-only`
- `full`

- [ ] **Step 5: Route retained debug modes through page-backed segments**

Ensure:
- `FERROUS_GPU_DEBUG_MODE=texture-node` binds page textures directly
- `FERROUS_GPU_DEBUG_MODE=shader-source-only` samples page source texture directly
- `FERROUS_GPU_DEBUG_MODE=full` uses source texture + db remap + palette + row LUT

- [ ] **Step 6: Run tests and verify GREEN**

```bash
./scripts/run-tests.sh --ui-only
```

- [ ] **Step 7: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: render spectrogram from retained pages"
```

---

## Task 6: Live A/B Validation Against Main

**Files:**
- Use: `scripts/run-ui.sh`
- Use: `scripts/compare-spectrogram-diagnostics.py`

- [ ] **Step 1: Build and run `main` with profile logs**

Use the same track and interaction script/manual workflow as the GPU branch.

Example:

```bash
FERROUS_PROFILE_UI=1 ./scripts/run-ui.sh --spectrogram-instrumentation 2> /tmp/ferrous-main.log
```

Scenarios:
- centered max zoom in fullscreen
- one zoom level out and back in
- repeated seeks
- rolling steady-state

- [ ] **Step 2: Build and run GPU branch with profile logs**

```bash
FERROUS_PROFILE_UI=1 ./scripts/run-ui.sh --spectrogram-instrumentation 2> /tmp/ferrous-gpu.log
```

Repeat the exact same scenarios.

- [ ] **Step 3: Run debug-mode live checks**

```bash
FERROUS_GPU_DEBUG_MODE=texture-node FERROUS_PROFILE_UI=1 ./scripts/run-ui.sh --spectrogram-instrumentation 2> /tmp/ferrous-gpu-texture-node.log
FERROUS_GPU_DEBUG_MODE=shader-source-only FERROUS_PROFILE_UI=1 ./scripts/run-ui.sh --spectrogram-instrumentation 2> /tmp/ferrous-gpu-source-only.log
```

Expected:
- spectrogram remains visible
- no blank zoom-fill reveal
- no unexpected `fallback-display` path during ordinary retained-covered operation

- [ ] **Step 4: Compare logs**

```bash
scripts/compare-spectrogram-diagnostics.py /tmp/ferrous-main.log /tmp/ferrous-gpu.log
```

Acceptance target:
- GPU branch does not have worse p95/p99 paint/cache time than `main`
- GPU branch does not show more frame gaps than `main`
- ordinary retained-covered operation shows `retained-full` or `retained-texture-node`, not repeated `fallback-display`
- CPU rebuild count is zero or explainable during retained-covered OpenGL `full` operation

- [ ] **Step 5: Decide next action**

If GPU p95/p99 paint/cache time is worse than `main`, pause page-renderer expansion and investigate the measured bottleneck.

If GPU p95/p99 paint/cache time matches or beats `main`, continue with retained source pages.

---

## Recommendation Before Next Phase

Run one short live OpenGL validation before starting retained pages. The automated UI gate runs under the offscreen software scene graph on this machine, so it verifies structure and fallbacks but skips the OpenGL-only visibility assertions. A live run now is useful because Task 2 changed the reveal path: it should confirm that retained `full` mode remains visible without the warmed CPU display image.

Minimum live check before Task 3:

```bash
FERROUS_PROFILE_UI=1 FERROUS_GPU_DEBUG_MODE=full ./scripts/run-ui.sh --spectrogram-instrumentation 2> /tmp/ferrous-gpu-full.log
```

Exercise:
- centered max zoom in fullscreen
- zoom one level out and back in
- repeated seeks

Then inspect:

```bash
scripts/compare-spectrogram-diagnostics.py /tmp/ferrous-gpu-full.log /tmp/ferrous-gpu-full.log
```

Look for:
- `paths=retained-full:...`
- no repeated `fallback-display` during normal retained-covered operation
- `cpu rebuilds=0` during retained-covered OpenGL `full` operation
- no visible blank frame during zoom-fill reveal

If that passes visually and diagnostically, continue to Task 3. If it shows blanks or repeated CPU rebuilds, investigate before implementing pages.
