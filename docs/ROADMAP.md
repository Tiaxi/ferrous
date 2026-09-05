# Ferrous backlog

These are open ideas, not a release schedule. Implemented features are described in the [README](../README.md).

## Playback

- Consolidate playback position and post-seek synchronization around the backend clock; the UI still maintains local interpolation and a visual seek clock.
- Add ReplayGain and preamp/volume dB options.
- Add output device selection with persistence.
- Add optional crossfade and buffer tuning controls.

## Library and metadata

- Add richer sorting, grouping, filtering, and extended metadata fields.
- Define an explicit database schema versioning and migration policy.

## Visualization

- Add a spectrogram color-stop editor, presets, and reset-to-default controls.
- Add waveform density and style options.
- Continue reducing synchronization lag after repeated seeks and track transitions.

## Maintenance

- Add structured telemetry counters for performance regression tracking.
- Upgrade Rust from edition 2021 to edition 2024, with full validation.
