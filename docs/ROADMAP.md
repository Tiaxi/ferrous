# Ferrous Roadmap

This document tracks upcoming work for Ferrous.

## Product Direction

- Frontend: `Qt6/QML + Kirigami`.
- Backend: Rust playback/analysis/library services.
- Keep business logic in Rust and keep UI state orchestration thin and explicit.
- Performance and responsiveness are hard requirements.

## Current Baseline

- Gapless playback with repeat and shuffle modes.
- Waveform cache persisted in SQLite.
- Live spectrogram rendering with binary analysis transport.
- Configurable library roots, folder-first tree browsing, and queue workflows.
- Global search across artists, albums, and tracks.
- Last.fm scrobbling with desktop authentication.
- Seek-drag floating timestamp overlay.

## Active Priorities

### P0

- [ ] Reduce end-to-end spectrogram sync latency toward DeaDBeeF behavior (start with no-ticker analysis consumption path).

### P1

- [ ] Add ReplayGain support and preamp/volume dB behavior options.
- [ ] Add output device selector persistence.
- [ ] Expand spectrogram/waveform customization:
  - spectrogram color-stop editor
  - preset/save/reset-to-default flow
  - waveform density/style options
- [ ] Add DB schema versioning/evolution policy.

### P2

- [ ] Add optional crossfade and buffer tuning controls.
- [ ] Plan and execute Rust edition upgrade to `2024` (`cargo fix --edition`, cleanup, full verification).

## Backlog by Area

### Core UX

- [x] Add seek-drag time overlay (floating timestamp near seekbar while dragging).

### Playback

- [ ] Add ReplayGain support.
- [ ] Add preamp/volume dB behavior options.
- [ ] Add output device selector persistence.
- [x] Add Last.fm scrobbling support.
- [ ] Add optional crossfade and buffer tuning controls.

### Library/Metadata

- [ ] Add richer sort/group/filter modes and extended metadata fields.
- [ ] Add DB schema versioning/evolution policy.

### Spectrogram/Waveform

- [ ] Add spectrogram color-stop editor.
- [ ] Add spectrogram presets and reset-to-default flow.
- [ ] Add waveform density/style options.
- [ ] Reduce end-to-end spectrogram sync latency toward DeaDBeeF behavior.

### Quality/Performance

- [ ] Promote profiling logs to structured, regression-friendly telemetry counters.
- [ ] Plan and execute Rust edition upgrade to `2024` (`cargo fix --edition`, cleanup, full verification).

