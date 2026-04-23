# GPU-Owned Source Texture Design

Date: 2026-04-23
Branch: `gpu-spectrogram-overhaul`
Status: approved for planning

## Goal

Continue from the current public Qt GPU body checkpoint and remove `m_gpuSourceImage` from the renderer completely.

The active `full` renderer should no longer depend on any CPU-built source image. Instead, it should render from a persistent GPU-owned source texture that stores retained spectrogram columns directly.

This phase keeps the existing Rust/backend protocol and spectrogram semantics intact. It changes only how the frontend stores and renders already-received spectrogram data.

## Current Baseline

The branch now has:

- public Qt API only
- a working public `QSGMaterial` / `QSGMaterialShader` body renderer in normal `full` mode
- public `QSGGeometryNode` overlay lines
- live-backend validation for:
  - `full` mode
  - `shader-source-only`
  - `texture-node`
  - `stock-material`
- good rolling/fullscreen performance and smooth behavior on the live backend

The remaining architectural gap is that the body shader still consumes `m_gpuSourceImage`, which is CPU-prepared in `SpectrogramItem`.

So the renderer is currently:

- GPU final pass
- CPU-prepared source texture

The target state for this phase is:

- GPU final pass
- GPU-retained source texture
- no active CPU source image path

## Non-Negotiable Constraints

This phase must preserve:

- zero Qt private API usage in the UI build
- current rolling vs centered semantics
- current zoom / hop coupling
- current gapless continuity behavior
- current high-frequency / top-bin correctness
- current fullscreen rolling smoothness
- current public overlay-line behavior

This phase must not introduce:

- Rust/frontend bridge protocol changes
- `Qt6::GuiPrivate`
- `QQuickRhiItem`
- SSBOs
- compute shaders
- text-rendering redesign

## Scope

This phase covers:

- replacing `m_gpuSourceImage` in the active renderer
- adding a persistent GPU-owned source texture for retained spectrogram columns
- adding a small row-mapping LUT texture for vertical bin/span correctness
- moving active horizontal body sampling semantics fully into the shader
- removing `m_gpuSourceImage` from all active and scaffolding paths

This phase does not cover:

- protocol redesign
- compute/max-pyramid preprocessing
- overlay text changes
- palette/remap redesign
- backend chunk format changes

## Explicit Non-Goal: Protocol Redesign

This phase assumes the existing Rust→Qt protocol remains the source of truth.

A protocol redesign is not planned by default. It should only be considered later if profiling shows that:

- frontend-only GPU source uploads are still too expensive, or
- the current chunk semantics materially block future compute/preprocessing work

So the order is:

1. prove the frontend-only persistent GPU source texture path
2. profile it
3. redesign the protocol later only if justified

## Recommended Architecture

### High-level structure

Keep `SpectrogramItem` as the single owner of:

- ring/index/token semantics
- rolling vs centered visible-window state
- zoom and draw-phase state
- dB remap lookup generation
- palette lookup generation
- row-mapping LUT generation
- upload bookkeeping for changed source-column spans
- overlay geometry and text overlays

The only major renderer change is the active source-data storage model.

### New active source model

Replace `m_gpuSourceImage` with a persistent source texture owned by the scene-graph node.

That texture stores retained spectrogram columns directly, not a pre-rendered visible image.

The body shader then samples from that texture using current visible-window uniforms and lookup textures.

### Texture set

The active body renderer should use:

- one persistent source texture for retained spectrogram data
- one dB remap texture
- one palette texture
- one row-mapping LUT texture

The body node stays a single quad plus the existing tail/base structure already proven on the live backend where needed for scrolling.

## Texture Model

### Source texture

The source texture represents retained spectrogram columns, not viewport pixels.

Recommended dimensions:

- width = retained source-column capacity for the current mode/capacity budget
- height = `bins_per_column`

Format:

- use the smallest format that is proven reliable on the current public Qt live backend path
- prefer single-channel 8-bit only if it is confirmed robust under the custom material path
- otherwise use the smallest stable sampled format already known to work

The implementation should optimize for correctness and backend reliability before theoretical compactness.

### Lookup textures

Keep:

- `m_gpuDbRemapImage` → uploaded lookup texture
- `m_gpuPaletteImage` → uploaded lookup texture

Add:

- row-mapping LUT texture that preserves current vertical row/span semantics

### Persistent ownership

The scene-graph node should retain the source texture across frames and update only changed spans.

Visible scrolling and window movement should not cause source texture reallocation or full image rebuilds.

## Mode-Specific Data Usage

### Rolling mode

Rolling mode should treat the source texture as the retained rolling ring.

Only newly written columns are uploaded.

The visible window should move by uniform changes and source-coordinate interpretation, not by rebuilding the visible source image.

### Centered mode

Centered mode should treat the source texture as retained decoded track data.

Only newly decoded or replaced columns are uploaded.

Window movement in centered mode should also mostly be a uniform change unless genuinely new decode data arrived.

## CPU / GPU Split

### CPU responsibilities

CPU continues to own:

- column validity and retention semantics
- rolling vs centered index math
- write-head / oldest-retained / max-column tracking
- visible-window and draw-phase computation
- source upload span detection
- dB remap image generation
- palette image generation
- row-LUT generation

CPU does not build a visible source image anymore.

### GPU responsibilities

GPU owns:

- retained source texture contents
- body sampling across source columns
- vertical row/bin span interpretation
- interpolation vs peak-hold behavior
- dB remap lookup
- palette lookup
- final body color output

## Shader Contract

The body shader should consume retained data, not pre-rendered source pixels.

### Horizontal mapping

For each fragment:

- determine the contributing source-column range from visible-window uniforms
- if `effectiveZoom >= 1.0`:
  - interpolate between adjacent columns
- if `effectiveZoom < 1.0`:
  - perform peak-hold over the contributing source-column span

Rolling vs centered should be represented by uniforms and coordinate interpretation, not by different shader programs.

### Vertical mapping

Do not revert to naive `bin = y * bins / height`.

The earlier top-frequency clipping bug was caused by insufficient vertical coverage. This phase must preserve the corrected row/span behavior by using a row-mapping LUT equivalent to the current CPU mapping.

### Color mapping

After source intensity selection:

- sample dB remap lookup
- sample palette lookup
- output final color

Palette direction must remain consistent with the currently verified public GPU body path.

## Update Strategy

### Uploads

Texture uploads should occur only when data changed:

- new columns arrived
- retained columns were replaced
- texture capacity had to change because mode or ring budget changed

Scrolling, window movement, and draw-phase changes alone should not rebuild source image data.

### Capacity changes

If the retained-capacity budget changes materially:

- allocate a new persistent source texture
- repopulate it from the current retained ring data
- switch atomically to the new resource

This should be a rare event tied to geometry/mode/capacity changes, not normal frame cadence.

## Migration Stages

### Stage 1: Persistent source texture resource

Add a retained source texture resource and change tracking for dirty upload spans.

At this stage, the renderer may still temporarily mirror old source behavior internally for validation, but the goal is to establish the retained GPU-owned source texture contract.

### Stage 2: Row-LUT-backed vertical mapping

Introduce the row-mapping LUT texture and feed the body shader with correct vertical row/span information.

This stage protects the already-fixed top-bin and axis-alignment behavior.

### Stage 3: Full horizontal source sampling in shader

Make the body shader responsible for:

- interpolation when zoomed in
- peak-hold span evaluation when zoomed out

At this point the shader consumes retained source data directly rather than a CPU-prepared visible image.

### Stage 4: Delete `m_gpuSourceImage`

Remove:

- `m_gpuSourceImage`
- source-image rebuild logic
- any debug or scaffolding path still depending on CPU-built source images

At the end of this stage, there should be no active or fallback dependency on CPU source-image generation.

## Risks

### Risk 1: Vertical correctness regression

The current CPU path already fixed top-frequency clipping. Reimplementing that logic loosely in shader math is error-prone.

Mitigation:

- use a row-mapping LUT texture
- keep the existing top-bin coverage tests

### Risk 2: Rolling smoothness regression

Changing the source-storage model could accidentally reintroduce scroll/fill hitches.

Mitigation:

- keep the current rolling/fullscreen tests
- preserve the bounded scene-graph texture strategy already proven on the live backend
- treat any new hitch as a correctness regression

### Risk 3: Texture-capacity churn

If texture size changes too often, the new path could lose the benefit of persistence.

Mitigation:

- tie reallocation only to real geometry/capacity changes
- keep steady-state motion uniform-driven

### Risk 4: Public Qt shader/backend quirks

The branch already showed that public custom sampled materials can be backend-sensitive.

Mitigation:

- keep `texture-node` and `stock-material` debug controls as controls during bring-up
- validate manually on the live backend after each stage

## Testing Strategy

This phase must be test-driven and preserve the current renderer guardrails.

### New tests

Add tests for:

- persistent source texture upload span tracking
- row-LUT generation correctness
- shader-facing source-window mapping invariants
- deletion of `m_gpuSourceImage` from active/full paths

### Existing tests to retain

Keep tests covering:

- rolling fullscreen smoothness invariants
- fill-complete transition smoothness
- top-bin visibility
- centered/rolling semantic behavior
- active public shader body rendering

### Live validation

Manual validation remains required for:

- `full` mode correctness on the live backend
- rolling fullscreen smoothness
- centered zoom/window behavior
- debug controls still behaving as expected

## Success Criteria

This phase is complete when all of the following are true:

- `full` mode no longer builds or depends on `m_gpuSourceImage`
- retained spectrogram source data lives in a persistent GPU-owned texture
- active body rendering still matches current vertical frequency coverage
- rolling/fullscreen remains smooth on the live backend
- `./scripts/run-tests.sh --ui-only` passes
- live manual validation shows normal `full` rendering still works correctly

## Recommended Next Step

Write an implementation plan for:

1. retained source-texture resource and dirty-span upload tracking
2. row-LUT integration
3. shader horizontal sampling over retained source data
4. removal of `m_gpuSourceImage`

That is the next meaningful renderer phase from the current checkpoint.
