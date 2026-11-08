# Ferrous backlog

These are open ideas, not a release schedule. Implemented features are described in the [README](../README.md).

## Playback

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
- Decode each track once for the overview, waveform detail, and spectrogram, which each open their own decoder today.
- Keep the spectrograms of the last one or two tracks in memory, so that replaying a track or going back to the previous one shows its spectrogram at once.

## Maintenance

- Add structured telemetry counters for performance regression tracking.
- Upgrade Rust from edition 2021 to edition 2024, with full validation.
