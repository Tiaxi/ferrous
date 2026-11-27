# Ferrous backlog

These are open ideas, not a release schedule. Implemented features are described in the [feature guide](FEATURES.md).

## Playback

- Level raw AC-3 and DTS files with the ReplayGain tags in their APEv2 tags ([#806](https://github.com/Tiaxi/ferrous/issues/806)).
- Add output device selection with persistence.
- Add optional crossfade and buffer tuning controls.
- Add an equalizer.
- Add Stop After Current Track.
- Play WavPack, AIFF and Monkey's Audio (APE) files; the library and the queue take only the [supported formats](FEATURES.md#supported-formats) today.
- Play the tracks of a single-file album image from its CUE sheet.

## Queue and playlists

- Mark tracks to [play next](FEATURES.md#play-next) from the library and search too, adding them to the queue; the queue's rows can be marked today.
- Save the queue as an M3U playlist; playlists can only be imported today.

## Library and metadata

- Add richer sorting, grouping, filtering, and extended metadata fields.
- Keep play counts and the time each track was last played, and add ratings.
- Show a track's lyrics from its tags; search finds them, but nothing shows them.

## Visualization

- Add a spectrogram color-stop editor, presets, and reset-to-default controls.
- Add waveform density and style options.
- Continue reducing synchronization lag after repeated seeks and track transitions.
- Decode each track once for the overview, waveform detail, and spectrogram, which each open their own decoder today.
- Keep the spectrograms of the last one or two tracks in memory, so that replaying a track or going back to the previous one shows its spectrogram at once.
- Show where the view went on Follow Playhead, seek-bar clicks and zoom, instead of cutting in one frame ([#756](https://github.com/Tiaxi/ferrous/issues/756)).
- Fade the crosshair's level line and readout while panning ([#766](https://github.com/Tiaxi/ferrous/issues/766)).
- Draw spectrogram silence on the plot's indigo floor rather than pure black ([#762](https://github.com/Tiaxi/ferrous/issues/762)).

## Desktop and interface

- Export the menus to Plasma's Global Menu ([#783](https://github.com/Tiaxi/ferrous/issues/783)).
- Give a short window at a large font a layout of its own ([#761](https://github.com/Tiaxi/ferrous/issues/761)).
- Say how many rows the queue's Move Up and Move Down act on ([#763](https://github.com/Tiaxi/ferrous/issues/763)).

## Packaging

- Build `.deb` packages again, for Debian-based KDE distributions ([#782](https://github.com/Tiaxi/ferrous/issues/782)).

## Translations

- Translate the Rust backend's messages, such as playback, scan, and tag errors: the backend would send a code and its arguments, for the UI to translate. See [translations](DEVELOPMENT.md#translations).

## Maintenance

- Add structured telemetry counters for performance regression tracking.
- Upgrade Rust from edition 2021 to edition 2024, with full validation.
- Refuse network loads by the QML engine, and fetch iTunes previews in C++ ([#728](https://github.com/Tiaxi/ferrous/issues/728)).
- Run the menu window checks under the UI capture harness's KWin ([#667](https://github.com/Tiaxi/ferrous/issues/667)).
