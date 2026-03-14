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

Observed behavior to preserve:

- normal widget playback scrolls smoothly with no visible hitching
- playback start does not visibly stall or catch up
- seeking does not clear history and does not visibly speed up before settling
- fullscreen spectrogram remains smooth at high FPS

## Locked-In Invariants

The current implementation depends on these rules:

- Scene-graph rendering stays on the stable texture-node path in `ui/src/SpectrogramItem.cpp`.
- Seek does not hard-reset spectrogram history in `src/frontend_bridge/mod.rs`.
- Analysis reset does not drain queued fresh PCM in `src/analysis/mod.rs`; stale PCM is filtered by track token instead.
- Only the first post-reset spectrogram burst is seeded into history immediately.
- Steady-state row appends remain animation-driven and must not synchronously absorb large batches into history on the UI thread.
- Scroll cadence uses the backend visual hop cadence (`sampleRate / 1024`) rather than burst-size-derived startup estimates.

## Guardrail For Future Sync Work

Any spectrogram latency/sync change must preserve this baseline.

Minimum acceptance bar before merging:

- widget smoothness unchanged during steady playback
- no startup hitch or seek catch-up sequence
- fullscreen smoothness unchanged
- `./scripts/run-tests.sh --ui-only` passes
- `ui/tests/tst_qml_smoke.cpp` spectrogram burst-handling tests still pass

If a latency improvement conflicts with these guarantees, treat it as an architectural problem and redesign the handoff instead of weakening the pacing rules.
