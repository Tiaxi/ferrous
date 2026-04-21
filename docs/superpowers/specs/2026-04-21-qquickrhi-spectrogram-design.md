# QQuickRhiItem Spectrogram Renderer Design

Date: 2026-04-21
Branch: `gpu-spectrogram-overhaul`
Status: approved for implementation

## Goal

Replace the active spectrogram rendering path with a true GPU-owned renderer built on `QQuickRhiItem`.

This design explicitly replaces the current texture-node fallback as the active path. The committed texture-node renderer remains valuable as a working baseline and checkpoint, but it is not the intended end state for this branch.

## Scope

The first `QQuickRhiItem` version owns:

- spectrogram raster
- time grid lines
- frequency grid lines
- crosshair lines and marker
- playhead line

The first version does not own:

- text labels for time/frequency/crosshair overlays
- unrelated scene-graph content outside the spectrogram item
- compute-based preprocessing such as max pyramids

Text labels stay on the existing Qt scene-graph/text path in this first pass because custom GPU text handling would expand the scope into glyph atlas, shaping, DPI, and fallback-font behavior. That is a separate problem from fixing the spectrogram renderer.

## Non-Goals

- No SSBO-first architecture in the first pass
- No compute shader preprocessing in the first pass
- No runtime fallback switch between texture-node and RHI paths
- No attempt to preserve the old custom `QSGGeometryNode` shader experiment

Compute-style preprocessing is explicitly deferred to a later phase once the basic RHI renderer is stable and verified on the live backend.

## Renderer Architecture

### High-level structure

`SpectrogramItem` remains the state-owning UI item. It continues to own:

- playback position and interpolation state
- centered vs rolling display semantics
- ring buffer metadata and token filtering
- seek/reset/transition rules
- overlay state derivation
- label text generation

The new renderer is a dedicated `QQuickRhiItem`-based child or embedded rendering surface responsible only for drawing GPU primitives from already-derived state.

The key separation is:

- `SpectrogramItem`: derives what should be shown
- `SpectrogramRhiItem` / renderer: draws it

### Data passed to the renderer

The renderer receives a compact sync snapshot each frame containing:

- visible display range: `displayLeft`, `displayRight`
- effective zoom
- rolling vs centered mode
- current track token
- playhead x position
- crosshair x/y state
- grid line positions
- palette / dB range settings
- source texture generation and dirty span information

The renderer does not re-derive playback or session semantics internally.

## GPU Data Model

### Spectrogram source

The source spectrogram remains texture-backed in the first pass.

The CPU side maintains a source texture image representing the visible spectrogram column field in a GPU-friendly format. The renderer uploads that into an RHI texture and samples it directly in the fragment shader.

This first version intentionally avoids SSBO storage and compute preprocessing because:

- texture sampling is the most direct, portable first step
- we already know the spectrogram payload is a dense 8-bit column field
- it reduces moving parts while replacing the active path

### Display strategy

The RHI renderer does not receive a pre-colored final display image. That is the main architectural difference from the texture-node checkpoint.

Instead:

- fragment shader samples raw intensity data from the source texture
- shader performs the same zoom-dependent column-span logic used today
- shader applies dB remap and palette mapping
- the rendered result is produced entirely in the GPU pass

This removes CPU final-image rasterization from the active path.

### Overlay strategy

Overlay lines are rendered as GPU geometry in dedicated passes or batched draws:

- time grid: vertical lines
- frequency grid: horizontal lines
- crosshair: vertical/horizontal lines plus marker
- playhead: vertical line

These are driven by CPU-derived positions but no longer use CPU-built overlay images.

Text labels remain outside this renderer and continue using existing Qt text content.

## Rendering Passes

The first implementation should stay simple:

1. spectrogram pass
2. line-overlay pass

If blending/order issues make this awkward, the renderer may internally split line overlays into multiple draws, but the public design remains “spectrogram plus overlay lines”.

The spectrogram pass uses:

- fullscreen quad or rect geometry
- source texture sampling
- shader-side column-span resolution for zoom-in / zoom-out behavior
- shader-side dB remap and palette lookup

The overlay pass uses:

- simple line geometry
- uniform-driven colors and transforms

## Integration Plan

### Phase 1 replacement

Replace the active spectrogram body rendering path with the new RHI renderer. The current texture-node code remains in git history and can stay in-tree temporarily while the migration is in flight, but it should not remain the default runtime path once the RHI path is working.

### State migration

Keep these semantics unchanged:

- rolling mode history continuity
- centered mode full-track behavior
- gapless transition handling
- seek reset semantics
- EOF clamping and detachment behavior
- token-based stale chunk rejection
- zoom / backend hop coupling

The renderer migration must not redefine any of those rules.

## Testing Strategy

The first failing tests should focus on behavior and ownership boundaries, not raw performance claims.

Add tests for:

- active path now uses the RHI renderer rather than the texture-node display path
- centered mode preserves visible content through offscreen decode growth
- rolling mode preserves continuity through incremental advances
- overlay line positions match existing playhead/crosshair/grid semantics
- no CPU final display image rebuild is required for normal steady-state rendering

Existing behavior tests in `ui/tests/tst_qml_smoke.cpp` remain the semantic guardrails and should be updated only where they are explicitly coupled to the texture-node implementation.

## Success Criteria

The first RHI implementation is successful when all of the following are true:

- it renders correctly on the real live backend where the earlier custom shader path failed
- it preserves the existing centered and rolling semantics
- it keeps overlays aligned
- it passes `./scripts/run-tests.sh --ui-only`
- it matches or beats the current smooth 360 Hz steady-state baseline on the user’s machine

## Follow-up Phase

Once the basic RHI renderer is stable, phase 2 can add compute-style preprocessing:

- max-reduction pyramid or similar zoom-out acceleration
- possibly SSBO or compute-driven resource layouts if profiling justifies it

That phase is intentionally deferred until the basic renderer is proven correct and robust.
