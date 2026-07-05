# GPU Spectrogram Worktree Rescue List

Last reviewed: 2026-07-01

Source worktree: `.worktrees/gpu-spectrogram-overhaul`  
Source branch tip: `7d52c0b` (`fix: keep warmed zoom reveal fallback`)  
Main baseline at review: `81eae84` (`fix: keep content below menu bar`)

`git cherry -v main gpu-spectrogram-overhaul` reported all 203 source commits as
patch-unique relative to `main`; nothing below had landed verbatim at review time.

Use the `Status` column to track rescue work:

- `todo`: not yet brought over
- `adapted`: reimplemented or partially cherry-picked on `main`
- `picked`: clean cherry-pick or near-clean replay
- `skipped`: intentionally left behind

## Rescue Progress

### P0 Branch: `codex/rescue-p0-spectrogram-fixes`

Status: implemented and validated on 2026-06-27.

Validation:

- `./scripts/run-tests.sh` passed.
- The branch also updates `Cargo.lock` to resolve current `cargo audit`
  vulnerabilities in `quinn-proto` and `rustls-webpki`; the remaining `rand`
  advisory is reported by the script as an allowed warning.

Adaptation notes:

- No P0 commit was replayed verbatim. The source branch had diverged around the
  GPU renderer, so the useful behavior was adapted onto the current CPU/Qt
  spectrogram implementation.
- GPU-only retained-renderer freeze/cache hunks from `adc3a68` and `1b7cafb`
  were not suitable for current `main` and were intentionally left behind.
- The unrelated source-branch lockfile bump in `cd5afe5` was skipped; the branch
  carries only the current audit-required lockfile updates.

## P0: Rescue First

These are the most likely user-visible fixes or low-risk backend/analysis fixes.

| Status | Commit | Pick type | Why rescue it |
| --- | --- | --- | --- |
| adapted | `adc3a68` `fix: avoid spectrogram feed stalls and flac seek failures` | Partial | Brought over the FLAC seek fix: FLAC seeks use `FLUSH | ACCURATE`, with a Rust test. Retained-GPU freeze/cache hunks were skipped as not applicable to current `main`. |
| adapted | `44c23f8` `fix: keep seek hold at target until reacquired` | Direct/adapt | Backend playback clock now holds the requested target instead of visually running ahead before GStreamer reacquires position. |
| adapted | `cd5afe5` `fix: coalesce queued spectrogram restarts` | Adapt | Analysis worker keeps only the latest queued `NewTrack`; unrelated source-branch lockfile churn was skipped. |
| adapted | `6a91681` `fix: skip stale spectrogram restart generations` | Adapt | Idle worker drops stale restart generations before opening files or emitting reset chunks. |
| adapted | `ed3fdb1` `fix: reject stale spectrogram generations` | Adapt as group | Generation IDs now flow through analysis, FFI, bridge, QML, and `SpectrogramItem` so old same-track zoom chunks cannot overwrite newer data. |
| adapted | `5c1406d` `fix: preserve active spectrogram chunks` | Adapt with `ed3fdb1` | FFI queue drops stale generations but preserves active-generation precomputed chunks losslessly. |
| adapted | `1b7cafb` `fix: keep centered spectrogram restarts warm` | Partial | Centered seeks now restart closer to the visible left edge instead of decoding lots of off-screen margin first. Retained-GPU frame-freeze pieces were skipped. |
| adapted | `767ced0` `fix: reuse centered seek lookahead after restarts` | Adapt | Follow-up centered seeks after a fast restart reuse the replenishing decoded window instead of repeatedly restarting. |
| adapted | `3fd6d9a` `fix: cap centered spectrogram payloads` | Direct/adapt | Centered chunks are capped by byte budget to reduce UI dispatch spikes, with Rust coverage. |

### P1 Branch: `codex/rescue-p1-spectrogram-fixes`

Status: implemented, validated, and merged via PR #22 on 2026-06-30.

Validation so far:

- `./scripts/run-tests.sh --ui-only` passed after adapting the current P1 stack.

Adaptation notes:

- The source branch's older single-body GPU display image/cache has diverged
  from current `main`, which now renders the spectrogram body through a tiled
  CPU canvas. CPU-visible behavior was adapted to the tiled renderer instead of
  restoring the old GPU image fields.
- The rolling cache group is implemented as incremental pixel-range updates in
  `advancePrecomputedCanvasLocked`, including initial fill growth, fill-to-scroll
  handoff, and fractional-zoom scroll carry.
- Texture segmentation/body-texture bounding commits are skipped as obsolete for
  the current tiled renderer: tile count is bounded by widget width, and dirty
  tile uploads already avoid repeated full-body texture uploads.

### P2 Branch: `codex/rescue-p2-spectrogram-throughput`

Status: implemented and validated on 2026-07-01.

Validation:

- `./scripts/run-tests.sh --ui-only` passed.

Adaptation notes:

- Spectrogram chunks now bypass QML payload dispatch when live
  `SpectrogramItem` instances are registered. `BridgeClient` still emits the
  legacy full-payload signal as a fallback when no direct routes exist.
- The no-copy split avoidance from `f30628a` was folded into the direct-routing
  adaptation: registered items receive a raw-data view of the interleaved frame
  plus their channel index, and `SpectrogramItem` extracts the active channel
  from that view.
- Precomputed spectrogram frames are counted in bridge-poll work accounting so
  the poll loop can immediately continue when it saturates the per-pass cap.

### P2 Branch: `codex/rescue-p2-diagnostics-cleanup`

Status: implemented and validated on 2026-07-01.

Validation:

- `./scripts/run-tests.sh` passed.

Adaptation notes:

- The diagnostic/profile logging cleanup was replayed onto the current tiled
  CPU renderer rather than the source branch's retained GPU renderer.
- `c9be021` was skipped as obsolete: current `main` already lacks the
  zoom-fill payload peak scan that commit removed. Pulling its hunk forward
  would have reintroduced older GPU-specific zoom-fill gate code.
- The retained-GPU upload-budget portion of `ae3cb4c` was left behind; the
  relevant seek-profile duplicate suppression was adapted and covered by the
  existing Qt smoke profiling test.

## P1: CPU Spectrogram Behavior And Performance

This is a coherent `SpectrogramItem` block. Replay in order if current `main`
still has the same CPU image/QSG texture cache structure.

| Status | Commit | Pick type | Why rescue it |
| --- | --- | --- | --- |
| adapted | `0f8f648` `fix: avoid centered zoom-out freeze on coarser restart` | Adapt | Prevents centered zoom-out from looking stuck on the previous fine-hop image. |
| adapted | `84be591` `fix: refresh spectrogram body texture after resize` | Adapt | Ensures resize invalidates the body image generation so texture upload refreshes. |
| adapted | `0d0c1c2` `fix: preserve top-bin coverage in linear spectrograms` | Direct/adapt | Fixes linear-scale bin mapping so the highest FFT bins remain visible at tall heights. |
| adapted | `2cae6f5` `fix: grow rolling spectrogram cache incrementally` | Group/adapt | Avoids full rolling canvas rebuilds during initial fill. |
| adapted | `240d49c` `fix: smooth rolling fill-to-scroll transition` | Group/adapt | Preserves overlap when rolling fill turns into leftward motion. |
| adapted | `f7ea825` `fix: trim redundant spectrogram cache clears` | Group/adapt | Current tiled canvas avoids the old hot-path GPU cache clears while keeping dirty-tile invalidation. |
| skipped | `b7886c9` `fix: warm zoom-fill cache behind frozen frame` | Obsolete/cautious | Old frozen GPU image cache does not map cleanly to the current single tiled canvas without a second retained canvas. Existing zoom-fill freeze remains intact. |
| adapted | `12dc487` `fix: keep rolling prefill on a fixed-width cache` | Group/adapt | Keeps rolling prefill anchored to a fixed-width tiled canvas while visible content grows. |
| skipped | `9b092fc` `fix: segment rolling spectrogram texture uploads` | Obsolete | Current renderer already segments the body into bounded dirty tiles instead of one full rolling texture. |
| adapted | `b766402` `fix: keep rolling cache incremental at fractional zoom` | Group/adapt | Preserves incremental scroll at non-integer zoom by tracking subpixel offset. |
| adapted | `7a8d88b` `fix: add rolling ring headroom beyond lookahead` | Group/adapt | Adds an extra viewport of rolling ring slack so decoder lookahead does not evict live-window data. |
| skipped | `45d1f7b` `fix: bound rolling spectrogram body textures` | Obsolete | Current tiled renderer bounds body texture nodes by canvas tile count, so long rolling scrolls do not grow body texture resources. |
| adapted | `219707f` `fix: smooth rolling fill completion` | Group/adapt | Keeps transition from partially filled rolling viewport to full scrolling incremental. |
| adapted | `b949c6b` `fix: reuse crosshair label overlay buffers` | Direct/adapt | Reduces crosshair overlay allocation churn by clearing only old/new label rectangles. |

## P1: Seek And UI Behavior

These are not GPU-specific and are likely user-visible.

| Status | Commit | Pick type | Why rescue it |
| --- | --- | --- | --- |
| picked | `f11b1ae` `fix: keep spectrogram seek targets current` | Adapt | Optimistically publishes seek target in `BridgeClient` and fixes stale centered click-to-seek mapping. |
| adapted | `5a97bc7` `fix: keep centered seeks ahead of lagging decode` | Adapt | Prevents lagging decoded tail from being treated as EOF during far centered seeks. |
| adapted | `a992480` `fix: clear centered spectrogram on track reset` | Adapt | Stops old centered frames from lingering on non-gapless track changes. |
| adapted | `7d55997` `fix: let visual seek clock own reacquire` | Pair with `f9e07fa` | Final visual-seek-clock approach; supersedes earlier centered seek freeze experiments. |
| picked | `f9e07fa` `fix: preserve visual seek clock through target echo` | Pair with `7d55997` | Keeps the visual seek clock active when backend echoes the target or lags slightly behind it. |
| picked | `2b416d9` `fix: reset playback clock on playlist restart` | Adapt | Clears pending seek and visual clock when restarting a queue item. |

Do not rescue these intermediate commits alone; they were superseded by
`7d55997` and `f9e07fa`:

- `efacbb5` `fix: hold centered seek target during reacquire`
- `db9236a` `fix: freeze early centered seeks before data ready`
- `c686f1a` `fix: keep centered seek freeze through local ticks`
- `e409228` `fix: shorten centered seek freeze window`

## P2: Bridge/QML Throughput

Useful if precomputed spectrogram dispatch still shows up as a UI hot path.

| Status | Commit | Pick type | Why rescue it |
| --- | --- | --- | --- |
| adapted | `ade12ee` `fix: route spectrogram chunks outside qml` | Adapt | Routes chunk frames directly from `BridgeClient` to registered `SpectrogramItem`s. |
| adapted | `f30628a` `fix: avoid spectrogram chunk split copies` | Folded into `ade12ee` adaptation | Avoids per-channel split-copying by passing raw chunk data and channel index. |
| picked | `13c13ca` `fix: reserve spectrogram ring column map` | Direct/adapt | Small hot-path allocation reduction while feeding precomputed columns. |

## P2: Diagnostics And Profiling Cleanup

Only worth rescuing if profiling logs are still noisy or causing stalls.

| Status | Commit | Pick type | Why rescue it |
| --- | --- | --- | --- |
| adapted | `c2e7b7c` `fix: gate hot spectrogram trace logs behind opt-in` | Direct/adapt | Keeps detailed spectrogram trace output behind `FERROUS_PROFILE_SPECTROGRAM_TRACE`. |
| adapted | `fd9ad83` `fix: suppress minor playback heartbeat profile spam` | Direct/adapt | Reduces QML heartbeat log noise for tiny follow corrections. Current `main` already carried the later 0.1s threshold, so that value was preserved. |
| adapted | `f534fd7` `fix: gate heartbeat profile trace behind opt-in` | Direct/adapt | Adds Rust-side opt-in gate for heartbeat trace logging. |
| picked | `fab8fd7` `fix: route ui profile diagnostics off the main thread` | Direct/adapt | Sends UI profile diagnostics to stderr instead of the UI-thread disk queue. |
| skipped | `c9be021` `fix: remove hot-path zoom fill payload scan` | Obsolete | Current `main` already lacks the expensive payload peak scan; the conflicting source hunk was tied to older GPU zoom-fill gating. |
| picked | `ed48f59` `fix: avoid repeated seek profile summaries` | Adapt | Prevents duplicate seek profile summaries for the same trace generation. |
| adapted | `ae3cb4c` `fix: reduce spectrogram profiling churn` | Partial | Globally gates duplicate seek profile logging. Retained-GPU upload-budget churn was skipped as obsolete for the current tiled renderer. |

## P3: Optional Or Cautious

| Status | Commit | Pick type | Why it is lower priority |
| --- | --- | --- | --- |
| todo | `1115017` `fix: defer bridge startup until first frame` | Cautious | May improve startup responsiveness by starting the Rust bridge after first frame, but changes startup sequencing. |
| todo | `2b75a06` `fix: queue commands during deferred bridge startup` | Depends on `1115017` | Required if bridge startup is deferred so early commands are not dropped. |
| todo | `d73b54c` `fix: oversample centered zoom-out spectrograms` | Cautious | Oversamples zoomed-out data for GPU peak-hold. It may increase CPU-path work without clear benefit. |

## Probably Skip For Main

Most commits after the public shader/retained-texture pivot are GPU-specific:
RHI/material renderer, retained source textures/pages, shader sampling, GPU LOD
rendering, and retained zoom-freeze work. Leave these behind unless GPU
rendering is revived.

Representative skip categories:

- RHI/public shader renderer setup and shader material commits.
- Retained source texture ownership/cleanup/page upload commits.
- Retained zoom preview/freeze/handoff commits.
- Spectrogram LOD protocol/rendering commits, unless long-track overview LOD
  becomes a CPU renderer goal.
- GPU plan/spec docs, unless preserving implementation notes is useful.

## Validation Notes

For any rescued code, follow the project validation policy:

- Rust/backend-only changes: `./scripts/run-tests.sh --rust-only`
- UI/QML-only changes: `./scripts/run-tests.sh --ui-only`
- Cross-stack changes, especially generation protocol or bridge routing:
  `./scripts/run-tests.sh`

Cross-stack rescues should carry both Rust tests and Qt/QML tests where the
original branch had them.
