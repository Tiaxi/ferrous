# GPU-Owned Source Texture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove `m_gpuSourceImage` entirely and make the active spectrogram body render from a persistent GPU-owned retained source texture.

**Architecture:** Keep `SpectrogramItem` responsible for ring semantics, visible-window math, dirty-span tracking, palette/remap generation, and row-LUT generation. Add a public Qt/OpenGL retained source texture path using `QNativeInterface::QSGOpenGLTexture::fromNative(...)`, then move `texture-node`, `shader-source-only`, and `full` mode onto that retained texture so scrolling and window motion become uniform changes rather than CPU-built source-image rebuilds.

**Tech Stack:** Qt 6 scene graph (`QSGMaterial`, `QSGTexture`, `QNativeInterface::QSGOpenGLTexture`), OpenGL (`QOpenGLContext`, `QOpenGLFunctions`), C++/QtTest, existing Ferrous spectrogram ring/state logic.

---

## File Structure

- Create: `ui/src/SpectrogramSourceTexture.h`
  - Public-Qt/OpenGL retained texture owner for spectrogram columns.
  - Owns native `GLuint`, wrapped `QSGTexture *`, size/capacity metadata, and upload/release helpers.
- Create: `ui/src/SpectrogramSourceTexture.cpp`
  - Implements `glTexImage2D`, `glTexSubImage2D`, `QNativeInterface::QSGOpenGLTexture::fromNative(...)`, and cleanup.
- Modify: `ui/src/SpectrogramItem.h`
  - Remove `m_gpuSourceImage` and related source-image bookkeeping.
  - Add retained-source metadata, dirty upload spans, row-LUT image/generation, and helper declarations.
- Modify: `ui/src/SpectrogramItem.cpp`
  - Replace `syncPrecomputedGpuImagesLocked(...)` and `paintPrecomputedGpuSourceColumnsLocked(...)` with retained-source upload planning.
  - Sync the new retained source texture in `updatePaintNode()`.
  - Retarget `texture-node`, `shader-source-only`, and `full` mode to the retained texture.
- Modify: `ui/src/SpectrogramBodyMaterial.h`
  - Extend sample-material state with row-LUT texture and view/window uniforms.
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
  - Bind row-LUT texture and expanded uniform state for both `full` and `shader-source-only`.
- Modify: `ui/src/shaders/SpectrogramSample.vert`
  - Pass through the same quad geometry; keep matrix-only vertex contract unless extra varyings are actually needed.
- Modify: `ui/src/shaders/SpectrogramSourceSample.frag`
  - Sample the retained source texture with visible-window uniforms instead of pre-rendered source-image UVs.
- Modify: `ui/src/shaders/SpectrogramBodySample.frag`
  - Sample retained columns, apply row-LUT, choose interpolation vs peak-hold, then remap + palette.
- Modify: `ui/CMakeLists.txt`
  - Add the new retained texture source files to the UI target.
- Modify: `ui/tests/tst_qml_smoke.cpp`
  - Replace `m_gpuSourceImage`-specific tests with retained-source texture/layout/upload invariants and live window-capture checks.

---

### Task 1: Extract Retained-Source Layout and Dirty-Span Planning

**Files:**
- Create: `ui/src/SpectrogramSourceTexture.h`
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramRetainedSourceLayoutTracksVisibleWindowIndependentlyOfViewportWidth() {
    SpectrogramRetainedSourceLayout layout{
        .capacityColumns = 1024,
        .binsPerColumn = 64,
        .displayLeft = 400,
        .displayRight = 527,
        .rollingMode = true,
    };

    const QRect visibleColumns = layout.visibleColumnRect();
    QCOMPARE(visibleColumns.x(), 400);
    QCOMPARE(visibleColumns.width(), 128);
    QCOMPARE(layout.capacityColumns, 1024);
}

void QmlSmokeTest::spectrogramRetainedSourceUploadPlannerMergesAdjacentDirtyColumns() {
    QVector<SpectrogramSourceUploadSpan> spans = {
        {10, 10},
        {11, 11},
        {14, 14},
        {15, 17},
    };

    const QVector<SpectrogramSourceUploadSpan> merged =
        mergeSpectrogramSourceUploadSpans(spans);

    QCOMPARE(merged.size(), 2);
    QCOMPARE(merged[0].xBegin, 10);
    QCOMPARE(merged[0].xEnd, 11);
    QCOMPARE(merged[1].xBegin, 14);
    QCOMPARE(merged[1].xEnd, 17);
}
```

- [ ] **Step 2: Run the UI suite to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL in `tst_qml_smoke` because `SpectrogramRetainedSourceLayout`, `SpectrogramSourceUploadSpan`, and `mergeSpectrogramSourceUploadSpans(...)` do not exist yet.

- [ ] **Step 3: Add the retained-source planning types and declarations**

```cpp
// ui/src/SpectrogramSourceTexture.h
struct SpectrogramSourceUploadSpan {
    int xBegin{0};
    int xEnd{0};
};

struct SpectrogramRetainedSourceLayout {
    int capacityColumns{0};
    int binsPerColumn{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
    bool rollingMode{false};

    [[nodiscard]] QRect visibleColumnRect() const {
        const int width = std::max(0, static_cast<int>(displayRight - displayLeft + 1));
        return QRect(static_cast<int>(displayLeft), 0, width, binsPerColumn);
    }
};

QVector<SpectrogramSourceUploadSpan> mergeSpectrogramSourceUploadSpans(
    const QVector<SpectrogramSourceUploadSpan> &spans);
```

```cpp
// ui/src/SpectrogramItem.h
QVector<SpectrogramSourceUploadSpan> collectRetainedSourceUploadSpansLocked(
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) const;

SpectrogramRetainedSourceLayout buildRetainedSourceLayoutLocked(
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) const;
```

- [ ] **Step 4: Implement the pure planning helpers**

```cpp
// ui/src/SpectrogramSourceTexture.cpp
QVector<SpectrogramSourceUploadSpan> mergeSpectrogramSourceUploadSpans(
    const QVector<SpectrogramSourceUploadSpan> &spans) {
    if (spans.isEmpty()) {
        return {};
    }

    QVector<SpectrogramSourceUploadSpan> sorted = spans;
    std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.xBegin < rhs.xBegin;
    });

    QVector<SpectrogramSourceUploadSpan> merged;
    merged.push_back(sorted.front());
    for (int i = 1; i < sorted.size(); ++i) {
        auto &tail = merged.back();
        if (sorted[i].xBegin <= tail.xEnd + 1) {
            tail.xEnd = std::max(tail.xEnd, sorted[i].xEnd);
        } else {
            merged.push_back(sorted[i]);
        }
    }
    return merged;
}
```

```cpp
// ui/src/SpectrogramItem.cpp
SpectrogramRetainedSourceLayout SpectrogramItem::buildRetainedSourceLayoutLocked(
    const qint64 displayLeft,
    const qint64 displayRight,
    const bool rollingMode) const {
    return SpectrogramRetainedSourceLayout{
        .capacityColumns = m_ringCapacity,
        .binsPerColumn = m_precomputedBinsPerColumn,
        .displayLeft = displayLeft,
        .displayRight = displayRight,
        .rollingMode = rollingMode,
    };
}
```

- [ ] **Step 5: Run the UI suite to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS; the new pure planning tests pass and no production behavior changes yet.

- [ ] **Step 6: Commit**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramSourceTexture.h \
  ui/src/SpectrogramSourceTexture.cpp \
  ui/src/SpectrogramItem.h \
  ui/src/SpectrogramItem.cpp \
  ui/tests/tst_qml_smoke.cpp
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "refactor: add retained source layout helpers"
```

---

### Task 2: Add the Public OpenGL Retained Source Texture Owner

**Files:**
- Modify: `ui/src/SpectrogramSourceTexture.h`
- Modify: `ui/src/SpectrogramSourceTexture.cpp`
- Modify: `ui/CMakeLists.txt`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramRetainedSourceTextureCreatesWrappedQsgTexture() {
    SpectrogramSourceTexture texture;
    QCOMPARE(texture.texture(), nullptr);
    QCOMPARE(texture.size(), QSize());
}

void QmlSmokeTest::spectrogramRetainedSourceTextureTracksUploadSpanWithoutCpuSourceImage() {
    SpectrogramSourceTexture texture;
    QCOMPARE(texture.pendingUploadSpanCount(), 0);
    texture.noteUploadedSpan({4, 7});
    QCOMPARE(texture.pendingUploadSpanCount(), 1);
}
```

- [ ] **Step 2: Run the UI suite to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because `SpectrogramSourceTexture` does not expose `texture()`, `size()`, or upload-span bookkeeping yet.

- [ ] **Step 3: Add the retained texture owner interface**

```cpp
// ui/src/SpectrogramSourceTexture.h
class SpectrogramSourceTexture {
public:
    ~SpectrogramSourceTexture();

    [[nodiscard]] QSize size() const;
    [[nodiscard]] QSGTexture *texture() const;
    [[nodiscard]] int pendingUploadSpanCount() const;

    bool ensureCreated(QQuickWindow *window, int width, int height);
    bool uploadRgbaColumns(
        QQuickWindow *window,
        int xOffset,
        int width,
        int height,
        const QByteArray &rgbaBytes);
    void noteUploadedSpan(const SpectrogramSourceUploadSpan &span);
    void clearPendingSpans();
    void release();

private:
    GLuint m_textureId{0};
    QSize m_size;
    QSGTexture *m_texture{nullptr};
    QVector<SpectrogramSourceUploadSpan> m_pendingUploadedSpans;
};
```

- [ ] **Step 4: Implement creation, upload, and release with public Qt/OpenGL APIs**

```cpp
// ui/src/SpectrogramSourceTexture.cpp
bool SpectrogramSourceTexture::ensureCreated(QQuickWindow *window, const int width, const int height) {
    if (window == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (m_texture != nullptr && m_size == QSize(width, height)) {
        return true;
    }

    release();

    auto *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    functions->glGenTextures(1, &m_textureId);
    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    m_texture = QNativeInterface::QSGOpenGLTexture::fromNative(
        m_textureId, window, QSize(width, height), {});
    m_size = QSize(width, height);
    return m_texture != nullptr;
}
```

```cpp
bool SpectrogramSourceTexture::uploadRgbaColumns(
    QQuickWindow *window,
    const int xOffset,
    const int width,
    const int height,
    const QByteArray &rgbaBytes) {
    if (!ensureCreated(window, m_size.width(), m_size.height())) {
        return false;
    }
    auto *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return false;
    }
    QOpenGLFunctions *functions = context->functions();
    functions->glBindTexture(GL_TEXTURE_2D, m_textureId);
    functions->glTexSubImage2D(
        GL_TEXTURE_2D, 0, xOffset, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaBytes.constData());
    return true;
}
```

- [ ] **Step 5: Run the UI suite to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS; the retained-texture owner compiles, the tests pass, and no renderer path uses it yet.

- [ ] **Step 6: Commit**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramSourceTexture.h \
  ui/src/SpectrogramSourceTexture.cpp \
  ui/CMakeLists.txt \
  ui/tests/tst_qml_smoke.cpp
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "feat: add retained spectrogram source texture"
```

---

### Task 3: Move `texture-node` and `shader-source-only` onto the Retained Source Texture

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramSourceSample.frag`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramGpuTextureNodeDebugUsesRetainedSourceTextureResource() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    SpectrogramItem item;
    // ... existing item/window setup ...
    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    QVERIFY(node != nullptr);
    QVERIFY2(
        spectrogramBodyTextureNode(node.get())->texture() != nullptr,
        "texture-node debug mode should bind the retained source texture, not a CPU-built source image");
    qunsetenv("FERROUS_GPU_DEBUG_MODE");
}

void QmlSmokeTest::spectrogramGpuSourceOnlyDebugRendersWithoutGpuSourceImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "shader-source-only");
    SpectrogramItem item;
    // ... existing item/window setup ...
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_gpuSourceImage = QImage();
    }
    const QImage frame = window.grabWindow();
    QVERIFY2(countVisiblePixelsInRect(frame, frame.rect(), 8) > 100,
        "shader-source-only should stay visible when the old CPU source image is absent");
    qunsetenv("FERROUS_GPU_DEBUG_MODE");
}
```

- [ ] **Step 2: Run the UI suite to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because the active debug paths still depend on `m_gpuSourceImage` and the test uses the old state assumptions.

- [ ] **Step 3: Add retained-source metadata and upload staging in `SpectrogramItem`**

```cpp
// ui/src/SpectrogramItem.h
SpectrogramSourceTexture m_gpuSourceTexture;
QByteArray m_gpuSourceUploadRgba;
QVector<SpectrogramSourceUploadSpan> m_gpuSourceUploadSpans;
qint64 m_gpuSourceTextureDisplayLeft{0};
qint64 m_gpuSourceTextureDisplayRight{-1};
bool m_gpuSourceTextureRolling{false};
quint64 m_gpuSourceTextureTrackToken{0};
```

```cpp
// ui/src/SpectrogramItem.cpp
void SpectrogramItem::syncRetainedSourceTextureLocked(
    QQuickWindow *window,
    qint64 displayLeft,
    qint64 displayRight,
    bool rollingMode) {
    const auto layout = buildRetainedSourceLayoutLocked(displayLeft, displayRight, rollingMode);
    m_gpuSourceTexture.ensureCreated(window, layout.capacityColumns, layout.binsPerColumn);
    m_gpuSourceUploadSpans = mergeSpectrogramSourceUploadSpans(
        collectRetainedSourceUploadSpansLocked(displayLeft, displayRight, rollingMode));
}
```

- [ ] **Step 4: Retarget the source-only and texture-node body paths to the retained texture**

```cpp
// ui/src/SpectrogramItem.cpp, inside updatePaintNode()
syncRetainedSourceTextureLocked(currentWindow, displayLeft, displayRight, rollingMode);

QSGTexture *retainedSourceTexture = m_gpuSourceTexture.texture();

syncSpectrogramSourceSampleMaterialNode(
    node->spectrogramSourceSampleNode,
    showSpectrogram && useSourceOnlySampleBody ? baseTargetRect : QRectF(),
    retainedSourceTexture,
    QRectF(baseSourceRect),
    node->placeholderTexture);

configureTextureNode(
    node->spectrogramTexture,
    useTextureNodeBody ? retainedSourceTexture : nullptr,
    useTextureNodeBody ? baseTargetRect : QRectF(),
    baseSourceRect,
    node->placeholderTexture);
```

- [ ] **Step 5: Run the UI suite to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS; `texture-node` and `shader-source-only` both render from the retained texture path and the tests no longer require `m_gpuSourceImage`.

- [ ] **Step 6: Commit**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramItem.h \
  ui/src/SpectrogramItem.cpp \
  ui/src/SpectrogramBodyMaterial.h \
  ui/src/SpectrogramBodyMaterial.cpp \
  ui/src/shaders/SpectrogramSourceSample.frag \
  ui/tests/tst_qml_smoke.cpp
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "feat: route debug spectrogram bodies through retained source texture"
```

---

### Task 4: Add Row-LUT Texture and Move `full` Mode Sampling onto the Retained Source Texture

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramBodySample.frag`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramGpuFullModeRendersWithoutCpuSourceImage() {
    SpectrogramItem item;
    // ... existing item/window setup ...
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_gpuSourceImage = QImage();
    }
    const QImage frame = window.grabWindow();
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(40, 15, 160, 80), 8) > 100,
        "full mode should render from the retained source texture even when the old CPU source image is absent");
}

void QmlSmokeTest::spectrogramGpuFullModeKeepsTopBinVisibleWithRowLutSampling() {
    SpectrogramItem item;
    item.setLogScale(false);
    // feed only the highest bins
    // ... existing item/window setup ...
    const QImage frame = window.grabWindow();
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(40, 0, 160, 8), 8) > 50,
        "full mode should preserve top-bin visibility through the row-LUT shader path");
}
```

- [ ] **Step 2: Run the UI suite to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because `full` mode still depends on `m_gpuSourceImage` and the fragment shader has no row-LUT path yet.

- [ ] **Step 3: Add row-LUT state and bind it into the body material**

```cpp
// ui/src/SpectrogramItem.h
QImage m_gpuRowLutImage;
quint64 m_gpuRowLutGeneration{0};
int m_gpuRowLutHeight{0};
int m_gpuRowLutBins{0};

QImage buildRowLutImageLocked(int height) const;
void syncGpuRowLutImageLocked(int height);
```

```cpp
// ui/src/SpectrogramBodyMaterial.h
struct State {
    QSGTexture *sourceTexture{nullptr};
    QSGTexture *dbRemapTexture{nullptr};
    QSGTexture *paletteTexture{nullptr};
    QSGTexture *rowLutTexture{nullptr};
    QRectF sourceRect;
    QPointF drawOffset;
    QSize sourceTextureSize;
    float effectiveZoom{1.0f};
    float sourceColumnOffset{0.0f};
};
```

- [ ] **Step 4: Implement row-LUT generation and retained-texture `full` sampling**

```cpp
// ui/src/SpectrogramItem.cpp
QImage SpectrogramItem::buildRowLutImageLocked(const int height) const {
    QImage image(1, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const int bin = std::clamp(m_iToBin.at(static_cast<size_t>(y)), 0, m_binsPerColumn - 1);
        uchar *pixel = image.scanLine(y);
        pixel[0] = static_cast<uchar>(bin & 0xff);
        pixel[1] = static_cast<uchar>((bin >> 8) & 0xff);
        pixel[2] = 0;
        pixel[3] = 255;
    }
    return image;
}
```

```glsl
// ui/src/shaders/SpectrogramBodySample.frag
layout(binding = 0) uniform sampler2D sourceTex;
layout(binding = 1) uniform sampler2D dbRemapTex;
layout(binding = 2) uniform sampler2D paletteTex;
layout(binding = 3) uniform sampler2D rowLutTex;

void main() {
    float sourceX = /* derive from visible-window uniforms */;
    vec2 rowLut = texture(rowLutTex, vec2(0.5, vTexCoord.y)).xy;
    float sourceBin = (rowLut.x * 255.0 + rowLut.y * 65280.0) / max(1.0, sourceTextureSize.y - 1.0);
    float value = sampleSourceIntensity(sourceX, sourceBin);
    float mapped = texture(dbRemapTex, vec2(value, 0.5)).x;
    fragColor = texture(paletteTex, vec2(1.0 - mapped, 0.5));
}
```

- [ ] **Step 5: Run the UI suite to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS; `full` mode renders from the retained source texture, top-bin visibility stays correct, and the CPU source image is no longer required for active drawing.

- [ ] **Step 6: Commit**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramItem.h \
  ui/src/SpectrogramItem.cpp \
  ui/src/SpectrogramBodyMaterial.h \
  ui/src/SpectrogramBodyMaterial.cpp \
  ui/src/shaders/SpectrogramBodySample.frag \
  ui/tests/tst_qml_smoke.cpp
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "feat: render full spectrogram body from retained source texture"
```

---

### Task 5: Delete `m_gpuSourceImage` and the Old Source-Image Build Path

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
void QmlSmokeTest::spectrogramGpuFullModeStaysVisibleAfterMultipleRollingUpdatesWithoutCpuSourceImage() {
    SpectrogramItem item;
    // ... rolling setup ...
    for (int step = 0; step < 64; ++step) {
        // feed one column, advance position, update node
    }
    const QImage frame = window.grabWindow();
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(20, 16, 200, 88), 8) > 100,
        "rolling full mode should stay visible after repeated retained-source updates without any CPU source image path");
}

void QmlSmokeTest::spectrogramGpuTextureNodeDebugStaysVisibleAfterRepeatedRollingUpdatesWithoutCpuSourceImage() {
    qputenv("FERROUS_GPU_DEBUG_MODE", "texture-node");
    SpectrogramItem item;
    // ... rolling setup ...
    for (int step = 0; step < 64; ++step) {
        // feed one column, advance position, update node
    }
    const QImage frame = window.grabWindow();
    QVERIFY2(
        countVisiblePixelsInRect(frame, QRect(20, 16, 200, 88), 8) > 100,
        "texture-node debug mode should keep rendering from the retained source texture after repeated rolling updates");
    qunsetenv("FERROUS_GPU_DEBUG_MODE");
}
```

- [ ] **Step 2: Run the UI suite to verify it fails**

Run: `./scripts/run-tests.sh --ui-only`
Expected: FAIL because `m_gpuSourceImage`, `paintPrecomputedGpuSourceColumnsLocked(...)`, and `syncPrecomputedGpuImagesLocked(...)` still exist.

- [ ] **Step 3: Remove the old CPU source-image members and helpers**

```cpp
// ui/src/SpectrogramItem.h
-    QImage m_gpuSourceImage;
-    quint64 m_gpuSourceGeneration{0};
-    qint64 m_gpuSourceDisplayLeft{0};
-    qint64 m_gpuSourceDisplayRight{-1};
-    bool m_gpuSourceRolling{false};
-    int m_gpuSourceActualColumns{0};
-    qint64 m_gpuSourceRingWriteSeq{-1};
-    qint64 m_gpuSourceRingOldestSeq{-1};
-    qint32 m_gpuSourceMaxColumnIndex{-1};
-    quint64 m_gpuSourceTrackToken{0};
-    bool m_gpuSourceLastUpdateWasIncremental{false};
-    int m_gpuSourceLastShiftColumns{0};
-    int m_gpuSourceLastUpdatedColumns{0};
-    void paintPrecomputedGpuSourceColumnsLocked(...);
-    void syncPrecomputedGpuImagesLocked(...);
```

- [ ] **Step 4: Remove the old code path and retarget the remaining tests**

```cpp
// ui/src/SpectrogramItem.cpp
-void SpectrogramItem::paintPrecomputedGpuSourceColumnsLocked(...) const { ... }
-void SpectrogramItem::syncPrecomputedGpuImagesLocked(...) { ... }

// updatePaintNode()
-    if (needsSourceCache) {
-        syncPrecomputedGpuImagesLocked(...);
-    }
-    spectrogramNodeImage = m_gpuSourceImage;
-    spectrogramImageGeneration = m_gpuSourceGeneration;
```

```cpp
// ui/tests/tst_qml_smoke.cpp
// Replace old m_gpuSourceImage assertions with retained-texture assertions:
QVERIFY2(
    spectrogramBodySampleMaterialNode(node.get()) != nullptr,
    "full mode should keep the retained-texture body material node active");
QVERIFY2(
    spectrogramBodySampleMaterialNode(node.get())->material() != nullptr,
    "retained-texture full mode should still expose a bound material after repeated rolling updates");
```

- [ ] **Step 5: Run the UI suite to verify it passes**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS; the old CPU source-image path is gone, retained-source tests pass, and the active renderer remains visible.

- [ ] **Step 6: Commit**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramItem.h \
  ui/src/SpectrogramItem.cpp \
  ui/tests/tst_qml_smoke.cpp
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "refactor: remove cpu spectrogram source image path"
```

---

### Task 6: Final Validation and Branch Checkpoint

**Files:**
- Modify: `ui/tests/tst_qml_smoke.cpp` (only if any last test rename or expectation drift remains)
- Modify: `docs/superpowers/specs/2026-04-23-gpu-owned-source-texture-design.md` (only if implementation constraints discovered during Tasks 1-5 changed reality)

- [ ] **Step 1: Run the full UI validation on the final retained-source branch state**

Run: `./scripts/run-tests.sh --ui-only`
Expected: PASS with all UI tests green and no new warnings.

- [ ] **Step 2: Manually validate the live renderer checkpoints**

Run:

```bash
./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
FERROUS_GPU_DEBUG_MODE=texture-node ./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
FERROUS_GPU_DEBUG_MODE=shader-source-only ./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
```

Expected:
- `full`: upright colored spectrogram
- `texture-node`: upright raw-source debug view from the retained texture
- `shader-source-only`: upright grayscale view from the retained texture
- no return of the old black-body/sampler issues

- [ ] **Step 3: Commit the completed phase**

```bash
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul add \
  ui/src/SpectrogramSourceTexture.h \
  ui/src/SpectrogramSourceTexture.cpp \
  ui/src/SpectrogramItem.h \
  ui/src/SpectrogramItem.cpp \
  ui/src/SpectrogramBodyMaterial.h \
  ui/src/SpectrogramBodyMaterial.cpp \
  ui/src/shaders/SpectrogramSample.vert \
  ui/src/shaders/SpectrogramSourceSample.frag \
  ui/src/shaders/SpectrogramBodySample.frag \
  ui/CMakeLists.txt \
  ui/tests/tst_qml_smoke.cpp \
  docs/superpowers/specs/2026-04-23-gpu-owned-source-texture-design.md
git -C /home/tuomas/gdrive/Code/github/ferrous/.worktrees/gpu-spectrogram-overhaul commit -m "feat: retain spectrogram source texture on gpu"
```

---

## Self-Review

### Spec coverage

- Persistent GPU-owned source texture: covered by Tasks 2-5.
- No `m_gpuSourceImage` in active or scaffolding paths: covered by Tasks 3-5, with explicit deletion in Task 5.
- Row-LUT vertical correctness: covered by Task 4.
- Preserve rolling/fullscreen smoothness and centered behavior: covered by Tasks 3-5 with existing rolling/full window-capture tests retained and retargeted.
- No protocol redesign / no private Qt API: respected throughout; the plan uses public `QNativeInterface::QSGOpenGLTexture::fromNative(...)`.

### Placeholder scan

- Removed generic “add tests” / “handle edge cases” wording; each task contains named tests, concrete files, and exact commands.
- No `TODO`, `TBD`, or “similar to Task N” placeholders remain.

### Type consistency

- The plan consistently uses:
  - `SpectrogramSourceTexture`
  - `SpectrogramSourceUploadSpan`
  - `SpectrogramRetainedSourceLayout`
  - `mergeSpectrogramSourceUploadSpans(...)`
  - `syncRetainedSourceTextureLocked(...)`
- These names are reused consistently across Tasks 1-5.
