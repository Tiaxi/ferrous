# Spectrogram Smoothness Baseline

This document marks the current spectrogram implementation as the non-regression baseline for smoothness.

## Golden Standard

Baseline commit series:

- `a96131b` `Fix spectrogram reset handoff and scene graph rendering`
- `4e3b05c` `Fix scene graph spectrogram startup crash`
- `bdce95f` `Fix scene graph spectrogram texture slicing`
- `9bb0d28` `Preserve spectrogram history and stop draining fresh PCM`
- `19bebc0` `Seed spectrogram bursts as history at startup`
- `b5ef2d5` `Limit spectrogram history seeding to reset bursts`
- `cd6c59b` `Reset spectrogram on stopped track switches`
- `e1fddb2` `Reset spectrogram on resume after stopped track switch`

Current post-refactor golden baseline:

- wake-driven bridge delivery and reactive snapshot emission remain enabled
- the QML spectrogram handoff stays on the deferred packed-batch flush path
- frame-cadenced `SpectrogramItem` draining and wrapped fragment composition remain in place
- profiling-only hitch/smoothness instrumentation is available behind `FERROUS_ENABLE_PROFILE_LOGS`, but production builds compile it out entirely
- `~/.local/share/ferrous/diagnostics.log` is the canonical diagnostics path on Linux

Observed behavior to preserve:

- normal widget playback scrolls smoothly with no visible hitching
- playback start does not visibly stall or catch up
- seeking does not clear history and does not visibly speed up before settling
- fullscreen spectrogram remains smooth at high FPS
- stopping playback freezes spectrogram motion immediately
- stop, switch tracks, then play does not let old-track spectrogram content nudge forward before the new track takes over

## Locked-In Invariants

The current implementation depends on these rules:

- Scene-graph rendering stays on the stable texture-node path in `ui/src/SpectrogramItem.cpp`.
- Seek does not hard-reset spectrogram history in `src/frontend_bridge/mod.rs`.
- Analysis reset does not drain queued fresh PCM in `src/analysis/mod.rs`; stale PCM is filtered by track token instead.
- Only the first post-reset spectrogram burst is seeded into history immediately.
- If a reset burst is delivered in multiple UI chunks, only the first chunk may trigger a visual reset, but every chunk that belongs to that burst must still be seeded into history.
- Steady-state row appends remain animation-driven and must not synchronously absorb large batches into history on the UI thread.
- Live row draining must stay frame-cadenced; do not chain immediate queued drain passes that can consume multiple recovery chunks inside one catch-up window.
- Wrapped scene-graph composition must allocate visible fragment nodes independently of source tile ids; the ring-buffer wrap can require the same source tile to appear at both screen edges in one frame.
- Scroll cadence uses the backend visual hop cadence (`sampleRate / 1024`) rather than burst-size-derived startup estimates.
- Stopped track switches must clear any pending bridge-side spectrogram delta before the next track resumes.
- The stopped-track-switch reset must be enforced on the immediate `playbackChanged` path, not only the delayed `snapshotChanged` path, because quick stop-switch-play sequences can outrun the snapshot coalescing timer.

## Guardrail For Future Sync Work

Any spectrogram latency/sync change must preserve this baseline.

Minimum acceptance bar before merging:

- widget smoothness unchanged during steady playback
- no startup hitch or seek catch-up sequence
- fullscreen smoothness unchanged
- profiling builds still emit the seek/smoothness guardrail markers when `./scripts/run-ui.sh --profile-logs` is used
- `./scripts/run-tests.sh --ui-only` passes
- `ui/tests/tst_qml_smoke.cpp` spectrogram burst-handling tests still pass
- `ui/tests/tst_qml_smoke.cpp` diagnostics path and profiling tests still pass
- `ui/tests/tst_bridge_client.cpp` stopped-track-switch delta clearing test still passes
- `ui/tests/tst_qml_smoke.cpp` stopped-track-switch resume predicate test still passes

If a latency improvement conflicts with these guarantees, treat it as an architectural problem and redesign the handoff instead of weakening the pacing rules.
