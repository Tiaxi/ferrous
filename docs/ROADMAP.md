# Ferrous backlog

These are open ideas, not a release schedule. Implemented features are described in the [README](../README.md).

## Playback

- Add ReplayGain, from the track and album gain tags, and preamp/volume dB options.
- Add output device selection with persistence.
- Add optional crossfade and buffer tuning controls.
- Add an equalizer.
- Add Stop After Current Track.
- Play WavPack, AIFF and Monkey's Audio (APE) files; the library and the queue take only the [supported formats](../README.md#supported-formats) today.
- Play the tracks of a single-file album image from its CUE sheet.

## Queue and playlists

- Add a Play Next action that puts tracks after the current one; today they go to the end of the queue unless dragged into place.
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

## Translations

- Translate the Rust backend's messages, such as playback, scan, and tag errors: the backend would send a code and its arguments, for the UI to translate. See [translations](DEVELOPMENT.md#translations).

## Maintenance

- Add structured telemetry counters for performance regression tracking.
- Upgrade Rust from edition 2021 to edition 2024, with full validation.
- Run the menu window checks under the UI capture harness's KWin ([#667](https://github.com/Tiaxi/ferrous/issues/667)).
