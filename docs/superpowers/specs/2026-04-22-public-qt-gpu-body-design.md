# Public Qt GPU Spectrogram Body Design

Date: 2026-04-22
Branch: `gpu-spectrogram-overhaul`
Status: approved for implementation

## Goal

Continue from the current public-Qt scene-graph baseline and replace the active spectrogram body renderer in normal `full` mode with a true GPU shader path built only on public Qt APIs.

This phase does not redesign spectrogram semantics. It removes CPU final-body rasterization from the active `full` render path while keeping the branch distributable with zero Qt private API usage.

## Current Baseline

The branch currently has:

- public scene-graph overlay lines rendered via `QSGGeometryNode`
- a public texture-node body path that displays CPU-produced images
- strong rolling/fullscreen performance fixes already validated on the live backend
- retained debug/profiling instrumentation that is still useful for the next phase

The body renderer in `full` mode still depends on CPU final-image generation in `SpectrogramItem`:

- `syncPrecomputedDisplayImageLocked(...)`
- `paintPrecomputedDisplayColumnsLocked(...)`
- `m_gpuDisplayImage`

That means the branch is on a public, shippable renderer architecture, but it has not yet achieved the original “true GPU spectrogram body” goal.

## Non-Negotiable Constraints

The end state for this phase must preserve:

- zero Qt private API usage in the UI build
- existing rolling vs centered semantics
- existing zoom / hop behavior
- existing top-bin / frequency-axis correctness
- the current smooth fullscreen rolling performance behavior

The phase must not introduce:

- `Qt6::GuiPrivate`
- `QQuickRhiItem`
- private QRhi headers
- SSBO or compute-based preprocessing
- text-rendering redesign

## Scope

This phase covers:

- the active spectrogram body renderer in normal `full` mode
- public shader/material integration in `SpectrogramItem::updatePaintNode()`
- reuse of existing source/remap/palette texture generation
- tests needed to prove the active body path has moved from CPU final image to public GPU shader rendering

This phase does not cover:

- overlay line redesign
- crosshair/time/frequency text label redesign
- compute/max-pyramid acceleration
- source upload model redesign
- deleting all CPU body scaffolding immediately

## Recommended Architecture

### High-level structure

Keep `SpectrogramItem` as the single owner of:

- playback and display semantics
- rolling vs centered state
- ring buffer and token gating
- draw-window computation
- source/remap/palette image generation
- overlay line generation
- text-label overlays

The only architectural change in this phase is the body node used for `full` rendering.

### Active body renderer

Replace the normal `full` body node with a public custom material node:

- `QSGGeometryNode` for the body quad
- `QSGMaterial` / `QSGMaterialShader` for sampling and color mapping

The body shader samples GPU textures directly and outputs final spectrogram colors.

### Debug and temporary paths

Keep these paths during bring-up:

- `texture-node` debug mode as the known-good raw source-texture control
- the current CPU display-image path in-tree as temporary scaffolding for validation and comparison

But the active `full` path should move to the material-backed node as soon as the first shader stage is working.

## Data Model

### Source texture

Reuse `m_gpuSourceImage` as the first shader input.

This is the right first-step input because:

- it already exists
- `texture-node` debug mode proves it uploads and displays correctly on the live backend
- it avoids expanding this phase into a new raw ring-to-GPU upload design

### Lookup textures

Reuse:

- `m_gpuDbRemapImage`
- `m_gpuPaletteImage`

These remain CPU-generated lookup images uploaded as textures.

### Removed active dependency

`m_gpuDisplayImage` should stop being the active draw source in normal `full` mode.

It may remain temporarily in-tree for validation, debug, or fallback staging while the new shader path is proven, but it is no longer the intended active renderer output.

## Rendering Contract

### CPU responsibilities

`SpectrogramItem` continues to compute and synchronize:

- `displayLeft`
- `displayRight`
- rolling vs centered mode
- `drawPixels`
- `drawX`
- effective zoom
- subpixel offset
- texture generations and dirty state for source/remap/palette images

The CPU remains responsible for “what should be shown,” not for producing the final colored spectrogram image.

### GPU responsibilities

The shader body is responsible for:

- sampling the source intensity texture
- applying dB remap lookup
- applying palette lookup
- mapping body pixel coordinates to source texture coordinates using the current draw-window state
- producing final colored output for the body quad

## Minimum Viable Shader Behavior

The first working shader body should:

- render a quad covering the current spectrogram target rect
- sample the uploaded source texture correctly on the live backend
- support current `drawX` / draw width behavior
- support current vertical mapping without clipping the high-frequency end
- apply dB remap and palette lookup to match current colors closely enough for behavioral parity

It should explicitly avoid trying to solve future problems in this phase:

- no compute shader preprocessing
- no SSBO path
- no new source texture format design
- no overlay line migration
- no text changes

## Implementation Staging

### Stage 1: Material-backed grayscale control

Replace the active `full` body node with a material-backed node that renders source texture intensity in grayscale.

Purpose:

- prove the public material path itself renders correctly on the live backend
- isolate the body shader path from palette/remap complexity

### Stage 2: dB remap + palette

Add shader lookup stages so the material path renders the same colorized spectrogram content as the current body renderer.

Purpose:

- move active `full` rendering off CPU final-body rasterization
- preserve current visual semantics

### Stage 3: remove active `full` dependence on CPU display image

Once the shader path is validated on the live backend and the tests pass, stop using `m_gpuDisplayImage` for normal `full` drawing.

Temporary scaffolding may remain only where explicitly justified for debug or validation.

## Testing Strategy

The test strategy should stay narrow and staged.

### First tests

Add tests that prove:

- the active `full` body node is no longer a simple texture node
- the new body node is material-backed
- `texture-node` debug mode still exists as a raw-source control

### Behavior guardrails to retain

Keep existing tests covering:

- rolling fullscreen smoothness behavior
- fill-complete rolling transition behavior
- top-bin visibility and frequency-axis correctness
- centered/rolling semantic invariants

These are critical because the new body path must not regress the fixes that made the current public renderer stable.

### Live-backend validation

Offscreen tests are necessary but not sufficient.

After each shader stage, validate manually on the real session for:

- visible rendering correctness
- rolling fullscreen smoothness
- crosshair and overlay alignment
- no black/smeared output regressions

## Alternatives Considered

### Option 1: public `QSGMaterial` body renderer while keeping current public overlays

Recommended.

This is the smallest architectural move that advances the original GPU-renderer goal while staying within the public-Qt boundary.

### Option 2: shader body only for a narrower subset, such as steady-state rolling mode first

Rejected for now.

It reduces initial risk but would prolong split architecture complexity and slow down convergence on a clean active renderer.

### Option 3: immediate full deletion of CPU display-image scaffolding

Rejected.

Given the branch history, that is too risky. The live backend must prove the public shader path first.

## Risks

### Risk: public material path fails on the live backend

Mitigation:

- bring up the shader path in grayscale first
- keep `texture-node` debug mode alive as a control
- validate manually before deleting active CPU-body dependence

### Risk: horizontal mapping regressions reintroduce rolling hitching

Mitigation:

- reuse current draw-window state rather than inventing a new mapping model
- keep the current rolling performance regressions intact

### Risk: vertical mapping regresses top-frequency coverage

Mitigation:

- port current row-mapping semantics carefully
- keep the top-bin visibility regression tests active during the migration

### Risk: CPU body scaffolding lingers too long

Mitigation:

- define the milestone explicitly: normal `full` mode no longer depends on `m_gpuDisplayImage`
- treat remaining CPU-body code as temporary scaffolding, not the target architecture

## Success Criteria

This phase is successful when:

- the active `full` spectrogram body path is material-backed and GPU-colored
- normal `full` rendering no longer depends on CPU final-body rasterization
- the UI build still uses zero Qt private APIs
- `./scripts/run-tests.sh --ui-only` passes
- the live backend renders correctly without smearing/black output
- the current rolling/fullscreen smoothness baseline is preserved

## Follow-up Work

After this phase succeeds, the branch can decide whether to pursue a later performance phase such as:

- public-shader-side zoom-out optimization
- max-reduction pyramid or similar acceleration
- further cleanup of temporary CPU-body scaffolding

Those are separate decisions. This phase only needs to establish the true public-Qt GPU body renderer first.
