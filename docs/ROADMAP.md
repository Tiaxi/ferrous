# Ferrous Roadmap

This document tracks upcoming work for Ferrous.
Reference baseline for UX: DeaDBeeF screenshot (`assets/deadbeef_reference.png`).

## Product Direction

- Frontend: `Qt6/QML + Kirigami`.
- Backend: Rust playback/analysis/library services.
- Keep business logic in Rust and keep UI state orchestration thin and explicit.
- Performance and responsiveness are hard requirements.

## Current Baseline

- Gapless playback works.
- Repeat/shuffle modes work.
- Waveform cache persists in SQLite.
- Spectrogram rendering is in C++ with packed binary analysis transport.
- Library roots configuration, folder-first tree browsing, and queue workflows are in place.
- Global search is backend-driven with dedicated binary result frames and stale-frame dropping.
- In-process FFI runtime path is the production default.
- Seek-drag floating timestamp overlay is implemented.

## Supporting Plans

- Test coverage plan: `docs/TEST_PLAN.md`
- Optimization backlog: `docs/OPTIMIZATION_PLAN.md`
- Library performance plan: `docs/LIBRARY_PERFORMANCE_PLAN.md`
- Spectrogram timing analysis: `docs/DEADBEEF_SPECTROGRAM_SYNC_ANALYSIS.md`

## Active Priorities (2026-03-05)

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
- [ ] Add Last.fm scrobbling support.
- [ ] Plan and execute Rust edition upgrade to `2024` (`cargo fix --edition`, cleanup, full verification).

## Backlog by Area

### Core UX

- [x] Add seek-drag time overlay (floating timestamp near seekbar while dragging).

### Playback

- [ ] Add ReplayGain support.
- [ ] Add preamp/volume dB behavior options.
- [ ] Add output device selector persistence.
- [ ] Add optional crossfade and buffer tuning controls.
- [ ] Add Last.fm scrobbling support.

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

## Working Rules

- Keep items concrete and testable.
- Keep this file current as work lands or priorities change.
