# Public Qt GPU Spectrogram Body Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the active `full` spectrogram body path with a public `QSGMaterial` / `QSGMaterialShader` renderer so normal rendering no longer depends on CPU final-body rasterization.

**Architecture:** Keep `SpectrogramItem` as the owner of all display semantics, source/remap/palette image generation, and overlay geometry. Introduce a focused public shader-backed body node that samples the existing source/remap/palette textures directly, while keeping `texture-node` debug mode and temporary CPU display-image scaffolding for bring-up.

**Tech Stack:** Qt 6 public scene graph (`QSGGeometryNode`, `QSGMaterial`, `QSGMaterialShader`, `QSGTexture`), QSB-compiled shaders, existing `SpectrogramItem` caches/tests, CMake Qt shader resource integration.

---

## File Structure

### New files

- `ui/src/SpectrogramBodyMaterial.h`
  - Public declarations for the shader-backed body node helpers, material state, and uniform payload.
- `ui/src/SpectrogramBodyMaterial.cpp`
  - Public `QSGMaterial` / `QSGMaterialShader` implementation, body geometry setup, uniform upload, texture binding, and node sync helpers.
- `ui/src/shaders/SpectrogramBody.vert`
  - Vertex shader for the body quad.
- `ui/src/shaders/SpectrogramBody.frag`
  - Fragment shader for source sampling, dB remap, and palette lookup.

### Modified files

- `ui/CMakeLists.txt`
  - Add the new body material sources and public Qt shader compilation/resource wiring.
- `ui/src/SpectrogramItem.cpp`
  - Replace the active `full` body node path in `updatePaintNode()`, create/sync shader textures, keep `texture-node` debug mode, and stop drawing `full` mode from `m_gpuDisplayImage` after the shader path is proven.
- `ui/src/SpectrogramItem.h`
  - Add any small body-material-facing sync structs or helper declarations that do not belong in the material file.
- `ui/tests/tst_qml_smoke.cpp`
  - Add failing-then-passing tests for the material-backed node, grayscale stage, color stage, and the removal of active `full` dependence on the CPU display image.

### Existing files intentionally left alone in this phase

- `ui/src/SpectrogramRenderUtils.cpp`
  - Continue to own `DebugMode` parsing and column-span math.
- `ui/qml/viewers/SpectrogramSurface.qml`
  - No behavior changes expected.
- Overlay line code in `ui/src/SpectrogramItem.cpp`
  - Remains on public geometry nodes.

---

### Task 1: Add the Public Body Material Skeleton and Failing Node-Ownership Test

**Files:**
- Create: `ui/src/SpectrogramBodyMaterial.h`
- Create: `ui/src/SpectrogramBodyMaterial.cpp`
- Create: `ui/src/shaders/SpectrogramBody.vert`
- Create: `ui/src/shaders/SpectrogramBody.frag`
- Modify: `ui/CMakeLists.txt`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing body-node ownership test**

Add a new Qt smoke test next to the existing body-node tests in `ui/tests/tst_qml_smoke.cpp` that asserts normal `full` mode uses a material-backed geometry node instead of a simple texture node.

```cpp
void QmlSmokeTest::spectrogramFullModeUsesMaterialBackedBodyNode() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    feedCenteredPrecomputedChunk(item, 0, 128, 32, 48'000, 512);

    std::unique_ptr<QQuickWindow> window(makeOffscreenWindow());
    QVERIFY(window);
    item.setParentItem(window->contentItem());
    window->resize(320, 180);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    QCoreApplication::processEvents();

    QSGNode *root = item.updatePaintNode(nullptr, nullptr);
    QVERIFY(root != nullptr);

    auto *scene = static_cast<SpectrogramSceneNode *>(root);
    QVERIFY(scene != nullptr);
    QVERIFY(scene->spectrogramMaterialNode != nullptr);
    QVERIFY(scene->spectrogramTexture == nullptr || scene->spectrogramTexture->rect().isEmpty());
}
```

- [ ] **Step 2: Run the UI suite to verify the new test fails for the right reason**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- FAIL in `QmlSmokeTest::spectrogramFullModeUsesMaterialBackedBodyNode`
- Existing tests stay green
- Failure should show that the material-backed node or helper does not exist yet

- [ ] **Step 3: Add the material skeleton and build wiring**

Create the public body-material files and wire them into CMake with public shader compilation. Keep the implementation minimal: a quad node, a material type, a shader class, and shader resources that return a visible grayscale output later.

```cpp
// ui/src/SpectrogramBodyMaterial.h
#pragma once

#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGTexture>

class SpectrogramBodyMaterial final : public QSGMaterial {
public:
    struct State {
        QSGTexture *sourceTexture{nullptr};
        QSGTexture *dbRemapTexture{nullptr};
        QSGTexture *paletteTexture{nullptr};
        QSize sourceSize;
        QSize targetSize;
        QPointF drawOffset;
        double effectiveZoom{1.0};
        double subpixelOffset{0.0};
        int debugStage{0};
    };

    SpectrogramBodyMaterial();
    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;

    State state;
};

QSGGeometryNode *createSpectrogramBodyMaterialNode();
void syncSpectrogramBodyMaterialNode(QSGGeometryNode *node, const QRectF &targetRect, const SpectrogramBodyMaterial::State &state);
```

```cmake
# ui/CMakeLists.txt
qt_add_shaders(ferrous "ferrous_spectrogram_body_shaders"
    PREFIX "/shaders"
    FILES
        src/shaders/SpectrogramBody.vert
        src/shaders/SpectrogramBody.frag
)

qt_add_shaders(ferrous_qml_smoke_tests "ferrous_qml_smoke_body_shaders"
    PREFIX "/shaders"
    FILES
        src/shaders/SpectrogramBody.vert
        src/shaders/SpectrogramBody.frag
)

qt_add_shaders(ferrous_bridge_client_tests "ferrous_bridge_body_shaders"
    PREFIX "/shaders"
    FILES
        src/shaders/SpectrogramBody.vert
        src/shaders/SpectrogramBody.frag
)
```

- [ ] **Step 4: Switch the active `full` body node shape without changing output semantics yet**

Update `SpectrogramSceneNode` in `ui/src/SpectrogramItem.cpp` so it owns a dedicated material-backed body node for normal `full` rendering, while `texture-node` debug mode keeps using the existing simple texture node.

```cpp
struct SpectrogramSceneNode final : public QSGNode {
    SpectrogramSceneNode() {
        background = new QSGSimpleRectNode();
        spectrogramMaterialNode = createSpectrogramBodyMaterialNode();
        spectrogramTexture = new QSGSimpleTextureNode();
        // ... existing overlay nodes ...
        appendChildNode(background);
        appendChildNode(spectrogramMaterialNode);
        appendChildNode(spectrogramTexture);
        // ...
    }

    QSGGeometryNode *spectrogramMaterialNode{nullptr};
    QSGSimpleTextureNode *spectrogramTexture{nullptr};
    // ...
};
```

- [ ] **Step 5: Run the UI suite to verify the new test passes and nothing regresses**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS for `spectrogramFullModeUsesMaterialBackedBodyNode`
- PASS for all existing UI tests

- [ ] **Step 6: Commit the material skeleton checkpoint**

```bash
git add ui/CMakeLists.txt ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBody.vert ui/src/shaders/SpectrogramBody.frag ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: add public spectrogram body material skeleton"
```

### Task 2: Bring Up the Public Shader Path in Grayscale

**Files:**
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramBody.vert`
- Modify: `ui/src/shaders/SpectrogramBody.frag`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing grayscale-output test**

Add a test that proves `full` mode can render visible spectrogram pixels through the material-backed body path while `texture-node` remains available as the raw-source control.

```cpp
void QmlSmokeTest::spectrogramMaterialBodyShowsVisibleGrayscalePixels() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    feedSyntheticGradientChunk(item, 0, 256, 64, 48'000, 512);

    auto window = makeOffscreenWindow();
    item.setParentItem(window->contentItem());
    window->resize(320, 180);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));

    const QImage capture = window->grabWindow();
    QVERIFY(!capture.isNull());
    QVERIFY(analyzeImageHasVisibleNonBlackPixels(capture));
}
```

- [ ] **Step 2: Run the UI suite to verify the test fails before shader sampling works**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- FAIL in `spectrogramMaterialBodyShowsVisibleGrayscalePixels`
- Failure should be missing or black body output, not unrelated test breakage

- [ ] **Step 3: Implement the grayscale vertex and fragment shaders**

Use the source texture only at this stage. Keep the shader contract intentionally small: quad coordinates in, UV mapping out, grayscale color out.

```glsl
// ui/src/shaders/SpectrogramBody.vert
#version 440
layout(location = 0) in vec4 aVertex;
layout(location = 1) in vec2 aTexCoord;
layout(location = 0) out vec2 vTexCoord;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} ubuf;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = ubuf.qt_Matrix * aVertex;
}
```

```glsl
// ui/src/shaders/SpectrogramBody.frag
#version 440
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D sourceTex;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} ubuf;

void main() {
    float value = texture(sourceTex, vTexCoord).r;
    fragColor = vec4(value, value, value, qt_Opacity);
}
```

- [ ] **Step 4: Sync the material node from `SpectrogramItem` in normal `full` mode**

Populate the material state from the already-working source cache and draw-window state, and use the material node only for `SpectrogramGpu::DebugMode::Full`.

```cpp
SpectrogramBodyMaterial::State state;
state.sourceTexture = ensureWindowTextureForImage(node, m_gpuSourceImage, &node->spectrogramSourceTextureResource);
state.sourceSize = m_gpuSourceImage.size();
state.targetSize = QSize(drawPixels, h);
state.drawOffset = QPointF(drawX, 0.0);
state.effectiveZoom = effectiveZoom;
state.subpixelOffset = rollingMode ? m_gpuDisplaySubpixelOffset : 0.0;
state.debugStage = 0;
syncSpectrogramBodyMaterialNode(node->spectrogramMaterialNode, spectrogramTargetRect, state);
```

- [ ] **Step 5: Keep `texture-node` debug mode unchanged**

Do not route `texture-node` through the material path. Preserve the current raw-source control path so live-backend debugging still has a known-good baseline.

```cpp
if (gpuDebugMode == SpectrogramGpu::DebugMode::TextureNode) {
    configureTextureNode(scene->spectrogramTexture, scene->spectrogramTextureResource, targetRect, sourceRect, scene->placeholderTexture);
    scene->spectrogramMaterialNode->setFlag(QSGNode::OwnedByParent, true);
    scene->spectrogramMaterialNode->setVisible(false);
} else {
    syncSpectrogramBodyMaterialNode(scene->spectrogramMaterialNode, targetRect, state);
    scene->spectrogramTexture->setRect(QRectF());
}
```

- [ ] **Step 6: Run the UI suite to verify grayscale rendering works**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS for the new grayscale test
- PASS for existing `texture-node` debug-mode tests
- PASS for the rest of the UI suite

- [ ] **Step 7: Commit the grayscale body checkpoint**

```bash
git add ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBody.vert ui/src/shaders/SpectrogramBody.frag ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
 git commit -m "feat: render spectrogram body through public shader path"
```

### Task 3: Add dB Remap and Palette Lookup to the Shader Path

**Files:**
- Modify: `ui/src/SpectrogramBodyMaterial.h`
- Modify: `ui/src/SpectrogramBodyMaterial.cpp`
- Modify: `ui/src/shaders/SpectrogramBody.frag`
- Modify: `ui/src/SpectrogramItem.cpp`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing colorized-body parity test**

Add a test that proves `full` mode body output is colorized rather than grayscale and still produces visible spectrogram pixels under the current palette/remap behavior.

```cpp
void QmlSmokeTest::spectrogramMaterialBodyAppliesDbRemapAndPalette() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setDbRange(96.0);

    feedSyntheticGradientChunk(item, 0, 256, 64, 48'000, 512);

    auto window = makeOffscreenWindow();
    item.setParentItem(window->contentItem());
    window->resize(320, 180);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));

    const QImage capture = window->grabWindow();
    QVERIFY(!capture.isNull());
    QVERIFY(imageContainsNonGraySpectrogramPixels(capture));
}
```

- [ ] **Step 2: Run the UI suite to verify the color test fails first**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- FAIL in `spectrogramMaterialBodyAppliesDbRemapAndPalette`
- Failure should indicate grayscale or incorrect body colors

- [ ] **Step 3: Extend the material state to bind remap and palette textures**

Add texture slots and any small uniform fields needed for the lookup stage.

```cpp
struct State {
    QSGTexture *sourceTexture{nullptr};
    QSGTexture *dbRemapTexture{nullptr};
    QSGTexture *paletteTexture{nullptr};
    QSize sourceSize;
    QSize targetSize;
    QPointF drawOffset;
    double effectiveZoom{1.0};
    double subpixelOffset{0.0};
    int debugStage{0};
};
```

- [ ] **Step 4: Update the fragment shader to apply remap and palette lookup**

```glsl
layout(binding = 1) uniform sampler2D sourceTex;
layout(binding = 2) uniform sampler2D dbRemapTex;
layout(binding = 3) uniform sampler2D paletteTex;

void main() {
    float value = texture(sourceTex, vTexCoord).r;
    float mapped = texture(dbRemapTex, vec2(value, 0.5)).r;
    vec4 paletteColor = texture(paletteTex, vec2(mapped, 0.5));
    fragColor = vec4(paletteColor.rgb, paletteColor.a * qt_Opacity);
}
```

- [ ] **Step 5: Ensure `SpectrogramItem` binds current lookup textures before syncing the body node**

Use the already-existing lookup image generation path and make texture creation part of the body sync path, not the old CPU display-image draw path.

```cpp
syncPrecomputedGpuLookupImagesLocked();
state.dbRemapTexture = ensureWindowTextureForImage(node, m_gpuDbRemapImage, &node->spectrogramDbRemapTextureResource);
state.paletteTexture = ensureWindowTextureForImage(node, m_gpuPaletteImage, &node->spectrogramPaletteTextureResource);
```

- [ ] **Step 6: Run the UI suite to verify the colorized path passes**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS for the new color test
- PASS for existing top-bin and visual capture tests
- PASS for the rest of the UI suite

- [ ] **Step 7: Commit the colorized-body checkpoint**

```bash
git add ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBody.frag ui/src/SpectrogramItem.cpp ui/tests/tst_qml_smoke.cpp
git commit -m "feat: apply palette lookup in public spectrogram shader"
```

### Task 4: Remove Active `full` Dependence on `m_gpuDisplayImage`

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp`
- Modify: `ui/src/SpectrogramItem.h`
- Test: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Write the failing active-path dependency test**

Add a test that proves normal `full` rendering still shows visible pixels even when the CPU final display image is cleared after source/remap/palette inputs are prepared.

```cpp
void QmlSmokeTest::spectrogramFullMaterialPathDoesNotDependOnCpuDisplayImage() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);

    feedCenteredPrecomputedChunk(item, 0, 256, 64, 48'000, 512);

    {
        QMutexLocker lock(&item.m_stateMutex);
        item.syncPrecomputedGpuImagesLocked(320, 180, 0, 255, false, 1.0, 8192);
        item.syncPrecomputedGpuLookupImagesLocked();
        item.m_gpuDisplayImage = QImage();
        item.bumpImageGeneration(item.m_gpuDisplayGeneration);
    }

    auto window = makeOffscreenWindow();
    item.setParentItem(window->contentItem());
    window->resize(320, 180);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));

    const QImage capture = window->grabWindow();
    QVERIFY(!capture.isNull());
    QVERIFY(analyzeImageHasVisibleNonBlackPixels(capture));
}
```

- [ ] **Step 2: Run the UI suite to verify the dependency test fails first**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- FAIL in `spectrogramFullMaterialPathDoesNotDependOnCpuDisplayImage`
- Failure should indicate that `full` mode still falls back to the CPU display image somewhere in the active path

- [ ] **Step 3: Remove normal `full`-mode reliance on `m_gpuDisplayImage` in `updatePaintNode()`**

Restrict `m_gpuDisplayImage` usage to explicit debug/validation cases only. Normal `full` rendering should be fully driven by source/remap/palette textures and body-material uniforms.

```cpp
const bool useTextureNodeDebug = gpuDebugMode == SpectrogramGpu::DebugMode::TextureNode;
if (useTextureNodeDebug) {
    spectrogramNodeImage = m_gpuSourceImage;
    spectrogramImageGeneration = m_gpuSourceGeneration;
    showTextureDebugBody = true;
} else {
    showMaterialBody = !m_gpuSourceImage.isNull()
        && !m_gpuDbRemapImage.isNull()
        && !m_gpuPaletteImage.isNull();
}
```

- [ ] **Step 4: Keep CPU display-image generation only where explicitly justified**

Do not delete the CPU display-image code in this task. Only stop using it for the active `full` path so debug and validation scaffolding stay available while the shader renderer proves itself.

```cpp
if (gpuDebugMode == SpectrogramGpu::DebugMode::TextureNode) {
    spectrogramNodeImage = m_gpuSourceImage;
    spectrogramImageGeneration = m_gpuSourceGeneration;
    showTextureDebugBody = true;
} else {
    showMaterialBody = !m_gpuSourceImage.isNull()
        && !m_gpuDbRemapImage.isNull()
        && !m_gpuPaletteImage.isNull();
}
```

- [ ] **Step 5: Run the UI suite to verify active `full` mode is independent of the CPU display image**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS for `spectrogramFullMaterialPathDoesNotDependOnCpuDisplayImage`
- PASS for all existing UI tests

- [ ] **Step 6: Commit the active-path transition checkpoint**

```bash
git add ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp
git commit -m "refactor: move active spectrogram body off cpu display image"
```

### Task 5: Live Validation and Focused Cleanup

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` (only if validation reveals defects)
- Modify: `ui/src/SpectrogramBodyMaterial.cpp` (only if validation reveals defects)
- Modify: `ui/tests/tst_qml_smoke.cpp` (only for defect-driven regressions)

- [ ] **Step 1: Run the full UI verification suite before live validation**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS with no new failures

- [ ] **Step 2: Run the real application without debug mode and validate the active body path**

Run:
```bash
./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
```

Expected manual checks:
- spectrogram body appears correctly
- colors look plausible and consistent with the current branch behavior
- rolling fullscreen remains smooth
- no black output or smearing
- overlays remain aligned with the body

- [ ] **Step 3: Run the raw-source control path and compare behavior**

Run:
```bash
FERROUS_GPU_DEBUG_MODE=texture-node ./scripts/run-ui.sh --profile-logs --clear-diagnostics-log
```

Expected manual checks:
- raw source texture still appears
- debug control path remains usable for future diagnosis
- any discrepancy between `full` and `texture-node` points to shader mapping, not source upload

- [ ] **Step 4: If live validation exposed defects, write one regression test per confirmed defect before changing code**

Use the same TDD loop as above. Do not batch speculative fixes.

```cpp
void QmlSmokeTest::spectrogramMaterialBodyPreservesTopBinCoverageAfterLiveFix() {
    // defect-specific regression goes here
}
```

- [ ] **Step 5: Re-run verification after any validation-driven fixes**

Run:
```bash
./scripts/run-tests.sh --ui-only
```

Expected:
- PASS for the whole UI suite
- any new defect regression tests pass

- [ ] **Step 6: Commit the validated public GPU-body milestone**

```bash
git add ui/src/SpectrogramBodyMaterial.h ui/src/SpectrogramBodyMaterial.cpp ui/src/shaders/SpectrogramBody.vert ui/src/shaders/SpectrogramBody.frag ui/src/SpectrogramItem.cpp ui/src/SpectrogramItem.h ui/tests/tst_qml_smoke.cpp ui/CMakeLists.txt
git commit -m "feat: move active spectrogram body to public gpu shader"
```

---

## Self-Review

### Spec coverage

This plan covers the approved spec requirements:

- active `full` body moves to a public material-backed node: Tasks 1-4
- source/remap/palette textures are reused: Tasks 2-3
- `texture-node` debug mode remains intact: Tasks 2 and 5
- CPU final-body rasterization is removed from the active `full` path: Task 4
- live-backend validation is explicit: Task 5

No spec requirement is left without a task.

### Placeholder scan

The plan contains:

- exact file paths
- explicit commands
- concrete test names
- concrete code snippets for each code-changing step

No `TBD`, `TODO`, or “similar to above” placeholders remain.

### Type consistency

The plan consistently uses:

- `SpectrogramBodyMaterial`
- `QSGGeometryNode` for the body node
- `QSGMaterial` / `QSGMaterialShader` for the shader path
- `m_gpuSourceImage`, `m_gpuDbRemapImage`, `m_gpuPaletteImage`
- `texture-node` as the debug control mode

The terminology matches the current codebase and approved design.
