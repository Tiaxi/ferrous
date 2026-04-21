# Public Qt Spectrogram Pivot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove all Qt private API usage from the UI build while preserving the branch's current spectrogram rendering behavior using only public Qt scene-graph APIs.

**Architecture:** `SpectrogramItem` becomes the sole active renderer integration point again. The spectrogram body stays texture-backed through public `QSGSimpleTextureNode`/`QSGTexture`, line overlays move to public `QSGGeometryNode`s, and text labels remain on the existing overlay-image path. The private `SpectrogramRhiItem` boundary, QRhi plumbing, and `Qt6::GuiPrivate` build dependency are removed completely.

**Tech Stack:** Qt 6 public scene-graph APIs (`QQuickItem`, `QSGNode`, `QSGGeometryNode`, `QSGFlatColorMaterial`, `QSGSimpleTextureNode`), existing `SpectrogramItem` cache/state machine, QtTest smoke tests, CMake UI build, `./scripts/run-tests.sh --ui-only`.

---

### Task 1: Remove the private renderer boundary from the build

**Files:**
- Delete: `ui/src/SpectrogramRhiItem.h`
- Delete: `ui/src/SpectrogramRhiItem.cpp`
- Delete: `ui/src/shaders/SpectrogramRhi.vert`
- Delete: `ui/src/shaders/SpectrogramRhi.frag`
- Delete: `ui/src/shaders/SpectrogramRhiLine.vert`
- Delete: `ui/src/shaders/SpectrogramRhiLine.frag`
- Modify: `ui/CMakeLists.txt`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramDoesNotCreatePrivateRhiChild() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1);

    QCOMPARE(item.findChild<QQuickItem *>(QString(), Qt::FindDirectChildrenOnly), nullptr);
}

void QmlSmokeTest::spectrogramUpdatePaintNodeStillUsesPublicTextureBody() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(1.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(node->childCount() >= 2);
    QCOMPARE(node->childAtIndex(1)->type(), QSGNode::GeometryNodeType);
    delete node;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because `SpectrogramRhiItem` is still created or referenced by the active path and the private renderer files are still in the build.

- [ ] **Step 3: Write minimal implementation**

```cmake
# ui/CMakeLists.txt
find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Gui Qml Quick QuickControls2 Network Widgets Concurrent)

qt_add_executable(ferrous
    src/AppInstanceController.cpp
    src/main.cpp
    src/CoverImageProvider.cpp
    src/DiagnosticsLog.cpp
    src/BinaryBridgeCodec.cpp
    src/BridgeClient.cpp
    src/GlobalSearchResultsModel.cpp
    src/LibraryTreeModel.cpp
    src/MprisController.cpp
    src/SpectrogramGpuRenderer.cpp
    src/TagEditorController.cpp
    src/TagEditorTableModel.cpp
    src/SpectrogramItem.cpp
    src/WindowActivation.cpp
    src/WaveformItem.cpp
)
```

```cpp
// ui/src/SpectrogramItem.h
// Remove:
// #include "SpectrogramRhiItem.h"
// SpectrogramRhiItem *m_rhiItem{nullptr};
```

```cpp
// ui/src/SpectrogramItem.cpp
// Constructor: remove child creation for SpectrogramRhiItem.
// updatePaintNode(): remove useRhiSpectrogramBody/useRhiOverlayLines gating.
// Keep the public texture-node spectrogram body path active.
```

- [ ] **Step 4: Delete private renderer files**

```text
ui/src/SpectrogramRhiItem.h
ui/src/SpectrogramRhiItem.cpp
ui/src/shaders/SpectrogramRhi.vert
ui/src/shaders/SpectrogramRhi.frag
ui/src/shaders/SpectrogramRhiLine.vert
ui/src/shaders/SpectrogramRhiLine.frag
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for the new tests, and no references to `Qt6::GuiPrivate`, `QQuickRhiItem`, `qrhi.h`, or `qshader.h` remain in the UI build.

- [ ] **Step 6: Commit**

```bash
git add ui/CMakeLists.txt ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git add -u ui/src/SpectrogramRhiItem.h ui/src/SpectrogramRhiItem.cpp ui/src/shaders
git commit -m "refactor: remove private qt spectrogram renderer"
```

### Task 2: Replace RHI line rendering with public scene-graph geometry nodes

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramPublicOverlayLineNodeTracksPlayhead() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);
    item.setPlaying(false);

    constexpr int bins = 8;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);
    item.setPositionSeconds(1.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *sceneNode = static_cast<SpectrogramSceneNode *>(node);
    QVERIFY(sceneNode->overlayLines != nullptr);
    QVERIFY(sceneNode->overlayLines->isSubtreeBlocked() == false);
    delete node;
}

void QmlSmokeTest::spectrogramPublicOverlayLineNodeTracksGridAndCrosshair() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(120);
    item.setDisplayMode(1);
    item.setPlaying(false);
    item.setGridEnabled(true);
    item.setCrosshairEnabled(true);
    item.setShowTimeLabels(true);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_hoverActive = true;
        item.m_hoverPosition = QPointF(60.0, 40.0);
    }

    constexpr int bins = 64;
    constexpr int totalColumns = 240;
    QByteArray chunk(bins * totalColumns, '\x40');
    item.feedPrecomputedChunk(
        chunk, bins, 0, totalColumns,
        0, totalColumns, 48000, 1024, true, true, 1, false);
    item.setPositionSeconds(2.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *sceneNode = static_cast<SpectrogramSceneNode *>(node);
    QVERIFY(sceneNode->overlayLines != nullptr);
    QVERIFY(sceneNode->overlayLineVertexCount > 0);
    delete node;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because there is no public line-geometry node yet.

- [ ] **Step 3: Write minimal implementation**

```cpp
// ui/src/SpectrogramItem.cpp
struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        background = new QSGSimpleRectNode();
        spectrogramTexture = new QSGSimpleTextureNode();
        spectrogramTailTexture = new QSGSimpleTextureNode();
        overlayLines = new QSGGeometryNode();
        playhead = new QSGSimpleRectNode();
        freqGridOverlay = new QSGSimpleTextureNode();
        timeGridOverlay = new QSGSimpleTextureNode();
        crosshairOverlay = new QSGSimpleTextureNode();
        overlay = new QSGSimpleTextureNode();
        appendChildNode(background);
        appendChildNode(spectrogramTexture);
        appendChildNode(spectrogramTailTexture);
        appendChildNode(overlayLines);
        appendChildNode(playhead);
        appendChildNode(freqGridOverlay);
        appendChildNode(timeGridOverlay);
        appendChildNode(crosshairOverlay);
        appendChildNode(overlay);
    }

    QSGGeometryNode *overlayLines{nullptr};
    int overlayLineVertexCount{0};
};
```

```cpp
// ui/src/SpectrogramItem.cpp
static void configureLineNode(
    QSGGeometryNode *node,
    const QVector<QLineF> &freqLines,
    const QVector<QLineF> &timeLines,
    const std::optional<QLineF> &playheadLine,
    const std::optional<QLineF> &crosshairX,
    const std::optional<QLineF> &crosshairY);
```

```cpp
// ui/src/SpectrogramItem.cpp
// updatePaintNode():
// - derive line positions with buildFreqGridLinesLocked(...)
// - derive time-grid positions with buildTimeGridLinesLocked(...)
// - derive crosshair/playhead lines
// - feed them into configureLineNode(...)
// - suppress CPU playhead rect when geometry lines are active
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for the new public-overlay-line tests and existing overlay semantics.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp
git commit -m "feat: render spectrogram overlay lines with public scene graph"
```

### Task 3: Retarget the body renderer to the public active path only

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramActiveRendererUsesPublicBodyTextureOnly() {
    SpectrogramItem item;
    item.setWidth(200);
    item.setHeight(100);
    item.setDisplayMode(1);

    constexpr int bins = 64;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x80');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(1.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);

    auto *sceneNode = static_cast<SpectrogramSceneNode *>(node);
    QVERIFY(sceneNode->spectrogramTexture != nullptr);
    QVERIFY(sceneNode->spectrogramTexture->rect().width() > 0.0);
    delete node;
}

void QmlSmokeTest::spectrogramGpuWindowCaptureStillShowsVisiblePixelsAfterPublicPivot() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(160);
    item.setDisplayMode(1);

    constexpr int bins = 64;
    QByteArray chunk(bins * 320, '\x60');
    item.feedPrecomputedChunk(chunk, bins, 0, 320, 0, 320, 48000, 1024, true, true, 1, false);

    QImage frame = captureItemFrame(item);
    QVERIFY(hasNonBlackPixels(frame));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL if any active body path still depends on the removed private renderer structures.

- [ ] **Step 3: Write minimal implementation**

```cpp
// ui/src/SpectrogramItem.cpp
const SpectrogramGpu::DebugMode gpuDebugMode = SpectrogramGpu::currentDebugMode();

// Always drive the active full-mode body through the public texture-node path.
const bool showPublicSpectrogramBody =
    gpuDebugMode == SpectrogramGpu::DebugMode::Full
    ? !m_gpuDisplayImage.isNull()
    : !m_gpuSourceImage.isNull();
```

```cpp
// ui/src/SpectrogramItem.cpp
// Remove:
// - SpectrogramRhiSyncState usage
// - RHI lookup image sync
// - RHI body/line gating
// Keep:
// - syncPrecomputedGpuImagesLocked(...)
// - syncPrecomputedDisplayImageLocked(...)
// - texture upload reuse
// - rolling incremental tail updates
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS and no references to the deleted private renderer remain.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp
git commit -m "refactor: restore public spectrogram body renderer"
```

### Task 4: Remove dead private-renderer state and retarget tests cleanly

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
void QmlSmokeTest::spectrogramBuildContainsNoPrivateRendererStateAssumptions() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(120);
    item.setDisplayMode(1);

    constexpr int bins = 32;
    constexpr int columns = 240;
    QByteArray data(bins * columns, '\x40');
    item.feedPrecomputedChunk(
        data, bins, 0, columns, 0, columns,
        48000, 1024, false, true, 1, false);
    item.setPositionSeconds(1.0);

    QSGNode *node = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(node != nullptr);
    QVERIFY(item.findChild<QObject *>("spectrogram-rhi-item") == nullptr);
    delete node;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL if any `SpectrogramRhiItem`-specific symbols, helper structs, or assumptions still remain in tests or production code.

- [ ] **Step 3: Write minimal cleanup implementation**

```cpp
// ui/src/SpectrogramItem.h
// Remove any remaining members used only by the deleted private renderer,
// for example:
// - lookup texture generations
// - private sync structs
// - temporary bridge-only helpers that no longer feed the public path
```

```cpp
// ui/tests/tst_qml_smoke.cpp
// Replace:
// - spectrogramCreatesRhiChildForActiveRenderer
// - spectrogramRhiRendererConsumesViewSyncState
// - spectrogramRhiQuadUsesDrawOffsetAndFlipsUv
// - spectrogramRhiRendererTracksPlayheadLinePosition
// - spectrogramRhiRendererTracksGridAndCrosshairLineState
// - spectrogramRhiRendererTracksPaletteAndDbRemapState
// with public-path assertions from Tasks 1-3.
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS with a clean public-API-only UI build and no stale private-renderer tests.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp
git commit -m "test: retarget spectrogram coverage to public renderer"
```

### Task 5: Verify packaging-facing cleanup and live-manual parity

**Files:**
- Modify: `ui/CMakeLists.txt`
- Modify: `docs/superpowers/specs/2026-04-21-public-qt-spectrogram-pivot-design.md` (only if implementation changed the agreed architecture)
- Test: `./scripts/run-tests.sh --ui-only`

- [ ] **Step 1: Write the failing check**

```bash
rg -n "GuiPrivate|qrhi.h|qshader.h|QQuickRhiItem|SpectrogramRhiItem" ui/CMakeLists.txt ui/src ui/tests
```

Expected: at least one match before final cleanup.

- [ ] **Step 2: Remove remaining private-Qt references**

```cmake
# ui/CMakeLists.txt
# Ensure there is no:
# - find_package(Qt6 ... GuiPrivate)
# - target_link_libraries(... Qt6::GuiPrivate)
# - qsb-generated private-RHI shader resources
```

```bash
rg -n "GuiPrivate|qrhi.h|qshader.h|QQuickRhiItem|SpectrogramRhiItem" ui/CMakeLists.txt ui/src ui/tests
```

Expected: no matches.

- [ ] **Step 3: Run full UI verification**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS

- [ ] **Step 4: Manual live-session verification**

Run: `./scripts/run-ui.sh --profile-logs --clear-diagnostics-log`

Check:
- spectrogram body visible
- no black screen or smear
- overlay lines align
- colors still plausible
- no private-Qt build warnings from `GuiPrivate`

- [ ] **Step 5: Commit**

```bash
git add ui/CMakeLists.txt ui/src ui/tests
git commit -m "refactor: finish public qt spectrogram pivot"
```
