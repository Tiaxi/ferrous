# Public Qt Spectrogram Pivot Design

Date: 2026-04-21
Branch: `gpu-spectrogram-overhaul`
Status: approved for implementation

## Goal

Pivot the experimental spectrogram renderer away from Qt private rendering APIs so the UI build uses only public Qt APIs, while preserving the branch's current spectrogram behavior, overlay behavior, and performance-sensitive cache/state work.

This pivot explicitly ends with zero Qt private API usage anywhere in the UI build.

## Problem Statement

The current branch moved the active spectrogram path toward a `QQuickRhiItem` renderer backed by private Qt RHI APIs:

- `Qt6::GuiPrivate`
- `rhi/qrhi.h`
- `rhi/qshader.h`
- private-Qt shader/resource plumbing in the UI build

That gives more low-level control, but it creates a packaging and maintenance burden:

- distro packages need private Qt development packages
- builds become tightly coupled to exact Qt private build versions
- rebuild discipline around Qt updates becomes stricter than desired

That tradeoff is not acceptable for the intended packaging model.

## Non-Negotiable Constraint

The end state must have:

- no `Qt6::GuiPrivate` dependency
- no private Qt headers in UI code
- no `QQuickRhiItem`-based private rendering implementation
- no build-time dependency on private Qt shader/RHI internals

This is a hard architectural boundary, not a soft preference.

## Scope

This pivot covers:

- the active spectrogram body rendering path
- line overlays:
  - time grid
  - frequency grid
  - crosshair lines
  - playhead line
- build system cleanup for private Qt removal
- retargeting tests that currently depend on `SpectrogramRhiItem`

This pivot does not cover:

- redesigning spectrogram semantics
- text label rendering changes
- compute/SSBO work
- a new public custom-shader effort beyond what is needed for the pivot

## Recommended Architecture

### High-level structure

Keep `SpectrogramItem` as the active rendering owner and interaction surface.

`SpectrogramItem` continues to own:

- rolling vs centered display semantics
- playback position / interpolation
- token gating and reset semantics
- source/cache derivation
- overlay line positions
- text-label generation

`updatePaintNode()` becomes the sole active rendering integration point again.

### Active scene-graph subtree

The active subtree under `SpectrogramItem` should be composed of public scene-graph nodes only:

- background node
- spectrogram body texture node
- overlay line geometry node(s)
- existing text/image overlay nodes for labels and FPS, where still needed

The key point is that the renderer becomes a public scene-graph renderer, not a private-RHI renderer.

## Rendering Strategy

### Spectrogram body

The first public-API pivot should keep the already-working texture-backed body path instead of attempting a fresh public shader experiment immediately.

That means:

- keep the existing source/display cache logic in `SpectrogramItem`
- keep the public texture-node based spectrogram body as the active body renderer
- remove only the private-RHI body path first

This is deliberately pragmatic. The branch already has working body texture upload/caching behavior and strong tests around it. Reusing that path reduces risk during the architectural pivot.

Once the private-Qt dependency is removed and the public path is stable again, a later phase can reconsider whether a public `QSGMaterial`-based shader path is worth attempting.

### Overlay lines

Line overlays should not go back to CPU overlay images if avoidable.

Instead:

- frequency grid lines
- time grid lines
- crosshair lines
- playhead line

should be rendered as public `QSGGeometryNode` line geometry owned by the main scene-graph subtree.

This preserves the useful work already done in the branch:

- line positions are already derived cleanly in `SpectrogramItem`
- overlay semantics are already covered by tests

### Text labels

Text labels stay on the existing path for now:

- time labels
- frequency labels
- crosshair labels
- FPS overlay text

This keeps the pivot focused on removing private Qt usage rather than expanding into custom text rendering.

## Migration Plan

### Phase 1: Remove private renderer boundary

Delete the private renderer architecture from the active build:

- remove `SpectrogramRhiItem` from the active path
- remove private QRhi includes and code
- remove `Qt6::GuiPrivate` from the UI build
- remove the RHI shader resource build path

The goal of this phase is not visual improvement. It is architectural cleanup to restore a public-API-only build.

### Phase 2: Make public scene-graph overlays active

Replace the private-RHI line overlay rendering with public geometry nodes in `SpectrogramItem::updatePaintNode()`.

The body remains texture-backed. The overlay lines become geometry-backed. Labels remain on their current text/image path.

### Phase 3: Retarget tests

Replace tests that assert the existence or state of `SpectrogramRhiItem` with tests that assert public active-path behavior instead.

Examples:

- no private renderer child exists
- overlay line geometry is populated as expected
- spectrogram body node still renders correctly
- existing spectrogram semantics remain unchanged

### Phase 4: Cleanup

Remove dead code, including:

- RHI sync structs that only exist for the private path
- shader files only used by the private path
- CMake logic for private shader baking/resources
- any stale renderer-specific caches that no longer serve the public active path

## Reused Work

The pivot should preserve and reuse:

- the current `SpectrogramItem` state machine
- rolling/centered cache logic
- source image derivation
- overlay line derivation
- semantic smoke tests
- the texture-node checkpoint work already proven on the live backend

The pivot should discard or rewrite:

- `SpectrogramRhiItem`
- private QRhi resource management
- private shader plumbing in CMake
- tests whose only purpose is validating the private renderer boundary

## Alternatives Considered

### Option 1: Keep the private-RHI renderer

Rejected because it conflicts with the packaging and maintenance constraints.

### Option 2: Public scene-graph pivot with texture-backed body and geometry-backed lines

Recommended.

It removes private Qt usage with the least disruption to the current branch and preserves the working parts of the current implementation.

### Option 3: Rebuild immediately around a public custom material/shader body path

Deferred.

This is closer to the original “true GPU renderer” goal, but it carries higher live-backend risk and would slow down the private-Qt removal pivot. It should only be revisited after the public-API build is stable.

## Risks

### Risk: performance regression

If the pivot accidentally reintroduces older CPU-heavy overlay image rebuilding or breaks incremental texture reuse, performance may drop.

Mitigation:

- keep the current cache logic
- keep existing performance-sensitive tests
- validate manually on the live session after the pivot

### Risk: live-backend rendering regressions

The earlier shader experiment already showed that offscreen tests are not enough.

Mitigation:

- use the existing public texture-node path as the base body renderer
- validate manually on the real backend as soon as the pivot is runnable

### Risk: partial cleanup leaving hidden private dependencies

Mitigation:

- explicitly remove `Qt6::GuiPrivate`
- remove private headers from source files
- remove private shader build wiring from CMake

## Success Criteria

The pivot is successful when all of the following are true:

- the UI build uses zero Qt private APIs
- `./scripts/run-tests.sh --ui-only` passes
- the app renders correctly on the live session
- line overlays remain aligned
- text labels still work
- packaging no longer requires private Qt development packages

## Follow-up Work

After the public-API pivot is complete, the branch can decide whether to attempt a second-stage public shader/material body renderer.

That should be treated as a separate performance/architecture project, not part of the private-Qt removal itself.
