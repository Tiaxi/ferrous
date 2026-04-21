# QQuickRhiItem Spectrogram Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the active spectrogram texture-node renderer with a `QQuickRhiItem`-owned GPU renderer that draws the spectrogram body and line overlays while preserving existing spectrogram semantics.

**Architecture:** Keep `SpectrogramItem` as the state owner and interaction surface, but delegate active rendering to a dedicated `SpectrogramRhiItem` child backed by `QQuickRhiItemRenderer`. The renderer consumes texture-backed source data plus overlay line state, while text labels stay on the existing Qt-side path until phase 2.

**Tech Stack:** Qt 6 `QQuickRhiItem`, QRhi pipelines and buffers, existing `SpectrogramItem` state machine, QtTest smoke tests, CMake UI build.

---

### Task 1: Introduce the RHI item boundary

**Files:**
- Create: `ui/src/SpectrogramRhiItem.h`
- Create: `ui/src/SpectrogramRhiItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/CMakeLists.txt`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
void QmlSmokeTest::spectrogramCreatesRhiChildForActiveRenderer() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDisplayMode(1);

    auto *rhiChild = item.findChild<QQuickRhiItem *>();
    QVERIFY2(rhiChild != nullptr, "active spectrogram path should create a QQuickRhiItem child");
    QCOMPARE(rhiChild->parentItem(), &item);
    QVERIFY(rhiChild->isVisible());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL in `spectrogramCreatesRhiChildForActiveRenderer`

- [ ] **Step 3: Write minimal implementation**

```cpp
class SpectrogramRhiItem final : public QQuickRhiItem {
    Q_OBJECT
public:
    explicit SpectrogramRhiItem(QQuickItem *parent = nullptr);
protected:
    QQuickRhiItemRenderer *createRenderer() override;
};

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_rhiItem(new SpectrogramRhiItem(this)) {
    m_rhiItem->setParentItem(this);
    m_rhiItem->setZ(0);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for the new child-creation test

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramRhiItem.h ui/src/SpectrogramRhiItem.cpp ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/CMakeLists.txt ui/tests/tst_qml_smoke.cpp
git commit -m "feat: add spectrogram rhi item boundary"
```

### Task 2: Move active spectrogram body rendering into the RHI renderer

**Files:**
- Modify: `ui/src/SpectrogramRhiItem.h`
- Modify: `ui/src/SpectrogramRhiItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
void QmlSmokeTest::spectrogramRhiRendererConsumesSourceTextureState() {
    SpectrogramItem item;
    item.setWidth(240);
    item.setHeight(120);
    item.setDisplayMode(1);

    constexpr int bins = 8;
    QByteArray chunk(bins * 240, '\x40');
    item.feedPrecomputedChunk(chunk, bins, 0, 240, 0, 240, 48000, 1024, true, true, 1, false);
    item.setPositionSeconds(1.0);

    item.update();
    QCoreApplication::processEvents();

    auto *rhiChild = item.findChild<SpectrogramRhiItem *>();
    QVERIFY(rhiChild != nullptr);
    QVERIFY(rhiChild->debugLastSyncState().sourceColumns > 0);
    QVERIFY(rhiChild->debugLastSyncState().bins > 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because no source sync state exists yet

- [ ] **Step 3: Write minimal implementation**

```cpp
struct SpectrogramRhiSyncState {
    QImage sourceImage;
    int sourceColumns{0};
    int bins{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
    double effectiveZoom{1.0};
};

void SpectrogramItem::syncRhiStateLocked(...) {
    syncPrecomputedGpuImagesLocked(...);
    m_rhiItem->setSyncState(SpectrogramRhiSyncState{
        .sourceImage = m_gpuSourceImage,
        .sourceColumns = int(displayRight - displayLeft + 1),
        .bins = m_precomputedBinsPerColumn,
        .displayLeft = displayLeft,
        .displayRight = displayRight,
        .effectiveZoom = effectiveZoom,
    });
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for source-sync test

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramRhiItem.h ui/src/SpectrogramRhiItem.cpp ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: feed source texture state into spectrogram rhi renderer"
```

### Task 3: Render spectrogram intensity in the RHI pass

**Files:**
- Create: `ui/src/shaders/SpectrogramRhi.vert`
- Create: `ui/src/shaders/SpectrogramRhi.frag`
- Modify: `ui/src/SpectrogramRhiItem.cpp`
- Modify: `ui/CMakeLists.txt`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
void QmlSmokeTest::spectrogramRhiWindowCaptureShowsSpectrogramPixels() {
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
Expected: FAIL because the RHI renderer does not draw source data yet

- [ ] **Step 3: Write minimal implementation**

```cpp
void SpectrogramRhiRenderer::initialize(QRhiCommandBuffer *cb) {
    ensureQuadBuffers();
    ensureSpectrogramPipeline();
}

void SpectrogramRhiRenderer::render(QRhiCommandBuffer *cb) {
    uploadSourceTextureIfNeeded(cb);
    beginSpectrogramPass(cb);
    cb->setGraphicsPipeline(m_spectrogramPipeline.get());
    cb->setShaderResources(m_spectrogramBindings.get());
    cb->draw(4);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for the RHI window-capture spectrogram test

- [ ] **Step 5: Commit**

```bash
git add ui/src/shaders/SpectrogramRhi.vert ui/src/shaders/SpectrogramRhi.frag ui/src/SpectrogramRhiItem.cpp ui/CMakeLists.txt ui/tests/tst_qml_smoke.cpp
git commit -m "feat: render spectrogram body with qquickrhiitem"
```

### Task 4: Move line overlays into the RHI renderer

**Files:**
- Modify: `ui/src/SpectrogramRhiItem.h`
- Modify: `ui/src/SpectrogramRhiItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramRhiRendererTracksPlayheadLinePosition();
void QmlSmokeTest::spectrogramRhiRendererTracksCrosshairAndGridLinePositions();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because overlay line state is not rendered by the RHI path yet

- [ ] **Step 3: Write minimal implementation**

```cpp
struct SpectrogramOverlayLines {
    QVector<QLineF> timeGrid;
    QVector<QLineF> freqGrid;
    std::optional<QLineF> playhead;
    std::optional<QLineF> crosshairX;
    std::optional<QLineF> crosshairY;
};

void SpectrogramRhiRenderer::render(QRhiCommandBuffer *cb) {
    renderSpectrogram(cb);
    renderOverlayLines(cb);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for overlay-line tests and existing spectrogram behavior tests

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramRhiItem.h ui/src/SpectrogramRhiItem.cpp ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: move spectrogram line overlays into rhi renderer"
```

### Task 5: Remove the active texture-node spectrogram path

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramActiveRendererNoLongerCreatesTextureNodePath();
void QmlSmokeTest::spectrogramCenteredAndRollingSemanticsRemainIntactUnderRhiRenderer();
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because the texture-node path still exists as active code

- [ ] **Step 3: Write minimal implementation**

```cpp
// Remove spectrogram QSGSimpleTextureNode ownership from updatePaintNode(),
// keep only label/text-node responsibilities, and route render state through
// the SpectrogramRhiItem.
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS for the full UI suite

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp
git commit -m "refactor: replace active spectrogram texture nodes with rhi renderer"
```

### Task 6: Verify on the live backend and trim debug scaffolding

**Files:**
- Modify: `ui/src/main.cpp`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramRhiItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add or keep only the diagnostics needed to validate the RHI path**

```cpp
// Keep renderer-backend logging and compact RHI sync/render diagnostics,
// remove texture-node-only profiling once the new path is proven.
```

- [ ] **Step 2: Run verification**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS

- [ ] **Step 3: Manual backend validation**

Run: `./scripts/run-ui.sh --profile-logs --clear-diagnostics-log`
Expected: Live app renders correctly on the target backend and diagnostics show the RHI path active

- [ ] **Step 4: Commit**

```bash
git add ui/src/main.cpp ui/src/SpectrogramItem.cpp ui/src/SpectrogramRhiItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "chore: validate qquickrhi spectrogram renderer"
```
