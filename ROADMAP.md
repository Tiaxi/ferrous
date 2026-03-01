# Ferrous Roadmap

This document tracks upcoming work, with an emphasis on DeaDBeeF parity.
Reference baseline: current DeaDBeeF layout/behavior in your screenshot.

## Goal

Reach practical daily-driver parity with DeaDBeeF for local-library playback, then iterate beyond parity.

## Parity Baseline (Current)

- Layout, split panes, transport controls, library tree, playlist, and spectrogram are in place.
- Gapless playback works.
- Spectrogram pipeline is close to DeaDBeeF behavior.
- Waveform cache exists in-memory and persists to SQLite.

## Milestone 1: Core UX Parity

- [x] Replace text transport buttons with icon-style toolbar controls (Open/Add/Prev/Next/Play/Pause/Stop).
- [x] Make waveform seekbar consume remaining horizontal space in top transport row.
- [x] Remove volume numeric box and keep slider-only volume control.
- [x] Move playback info text into bottom footer/status bar.
- [x] Remove redundant "Library" title above album art.
- [x] Tighten UI density (reduced extra spacing/padding between panes/widgets).
- [x] Standardize zero-padding separators and apply margins only per-pane/per-widget where explicitly needed.
- [x] Simplify to a single playlist model (intentional UX choice vs DeaDBeeF multi-playlist workflow).
- [ ] Implement real top menu actions (`File/Edit/View/Playback/Help`) instead of static labels.
- [ ] Add playlist context menus (track/playlist) for common actions.
- [ ] Add library context menus (play, add, add all from album/artist, rescan folder).
- [ ] Add drag-and-drop from library tree into playlist.
- [ ] Add multi-select in playlist and library lists.
- [ ] Add keyboard shortcuts for common actions (`Space`, `Ctrl+O`, `Delete`, `Ctrl+F`, `Ctrl+Tab`, etc.).
- [ ] Add double-click/enter behavior consistency across all list/tree rows.
- [ ] Add status bar parity items (selection counts, queue duration, playback mode indicators).

## Milestone 2: Playback Feature Parity

- [ ] Add playback modes: repeat off/all/one and shuffle (track/album).
- [ ] Add ReplayGain support (track/album mode, preamp, clipping prevention).
- [ ] Add preamp/volume behavior parity with dB scale options.
- [ ] Add output device selector and remember selected output device.
- [ ] Add configurable prebuffer/buffer sizes for gapless stability tuning.
- [ ] Add optional crossfade with sane defaults and disable rules (pause/seek/manual track switch).
- [ ] Add robust stream error handling with skip-to-next policies.

## Milestone 3: Library/Metadata Parity

- [ ] Add persistent playlist save/load on app restart.
- [ ] Add autoplay last playlist/last track restore.
- [ ] Add file system watcher for incremental library updates.
- [ ] Add configurable library roots UI (`Folders`/`Configure`) parity dialog.
- [ ] Add richer sort/group modes in library view (artist/album/year/genre).
- [ ] Add fast filter modes (artist-only, album-only, title-only).
- [ ] Add missing metadata fields in UI (year, genre, codec/container, sample format details).

## Milestone 4: Spectrogram/Waveform Controls Parity

- [ ] Add spectrogram properties UI (dB range, log scale, number of colors, gradient stops).
- [ ] Add persisted spectrogram presets and quick reset to DeaDBeeF-like defaults.
- [ ] Add waveform seekbar style options and density tuning.
- [ ] Add analysis quality presets (CPU vs detail).

## Milestone 5: Quality and Performance

- [ ] Add integration tests for queue transitions, gapless handoff, and seek behavior.
- [ ] Add regression tests for "no early next-track waveform/metadata switch".
- [ ] Add DB migration/versioning strategy for library + waveform cache schema changes.
- [ ] Add profiling toggles + telemetry for decode/analyze/render timings.
- [ ] Add startup performance budget and benchmarks for large libraries.

## Nice-to-Have (Post-Parity)

- [ ] Global media keys and optional desktop notifications.
- [ ] Theming and compact/normal density modes.
- [ ] Plugin-style visualization architecture.
- [ ] Embedded lyrics and external metadata provider hooks.
- [ ] Smart playlists and search query language.

## Working Rules for This Roadmap

- Keep items concrete and testable.
- Prefer parity-first behavior over new custom UX until Milestone 2 is mostly complete.
- When adding a new item, include acceptance criteria in the related PR/commit.
