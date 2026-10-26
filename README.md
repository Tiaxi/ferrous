# Ferrous

A fast, KDE-native desktop music player built with Rust and Qt6.

Ferrous is a personal project built for my own daily listening. It pairs a Rust backend for playback, analysis, and library management with a Qt6/QML + KDE Kirigami frontend. Developed with KDE Plasma in mind, it targets responsive local-library workflows, gapless playback, and rich real-time visualization. Ferrous is opinionated about library organization — your folder structure *is* your library.

![Ferrous screenshot](docs/assets/screenshot.png)

## Features

**Playback** — GStreamer-backed engine with seeking, gapless queue transitions, repeat, shuffle, and volume control.

**Library** — Folder-first browsing that mirrors your filesystem. Add one or more root directories, and Ferrous indexes them into a navigable tree with artist, album, and track grouping. It watches the roots, so files added, changed, or removed by other programs appear without a rescan, and it rescans them at startup. Roots on network or FUSE file systems, network or FUSE mounts inside a root, and roots with more folders than `fs.inotify.max_user_watches` allows are rescanned every 30 minutes instead, and Preferences > Library marks them. Watching can be turned off there. Removing a root there asks first and says how many tracks leave the library; the files stay on disk. [Global search](docs/SEARCH.md) spans the entire library, with ranked results and artist/album/track filters.

**Queue** — Open files, add folders, import M3U playlists, drag-to-reorder, and automatic session restore on launch. Clearing the queue, removing tracks, and replacing the queue can be undone until the queue changes again; see [keyboard and mouse](docs/SHORTCUTS.md#library-and-queue).

**Tag editor** — Multi-file tag editing with bulk operations and auto-numbering. Renaming files from their tags lists each new name, and any that cannot be used, before any file is renamed. Confirming renames only what the list showed, and nothing if the files changed since.

**Album art** — Folder and embedded cover art display with the option to fetch high-resolution artwork from the iTunes catalog. A folder's cover is its `cover`, `folder`, or `front` image, in that order and in any letter case, or else its first JPEG or PNG image by name. The library and the now-playing view both show the folder's cover before a track's embedded picture. Covers follow images that are added, renamed, removed, or replaced at the next scan, or at once while the library watches its folders. Applied artwork replaces that named image in its current format, or adds a `cover.jpg` or `cover.png` and leaves other images, such as back scans, untouched. In folders without such images, the artwork is embedded in every track there with the same album tag, and no track is changed unless all of them can be updated. Ferrous keeps no copy of the art it replaces, so it asks before replacing a track's existing art. Suggestions go to the album they were searched for, even if playback moves on before they are applied, and nothing is written if that track's file was moved, renamed, deleted or changed since the search.

**Visualization** — Live spectrogram and waveform displays rendered alongside embedded cover art. The spectrogram is inspired by Adobe Audition's spectral view — something I've spent countless hours using to analyze songs. It supports mono downmix or per-channel display, fullscreen viewing, logarithmic or linear frequency scale, adjustable dB range, configurable FFT window size, and centered or rolling display modes. Rolling mode scrolls continuously; centered mode shows a zoomable window around playback, with the view clamped at track boundaries. The waveform viewer supports zooming and per-channel inspection.

**Desktop integration** — MPRIS media controls, media key support, single-instance file opening, and `.desktop`/MIME registration.

**Scrobbling** — Last.fm integration with desktop authentication and batched submissions.

## Design priorities

Ferrous treats UI responsiveness as a correctness requirement. Playback, analysis, metadata extraction, library indexing, and search use background workers. Smooth visualization and immediately responsive interaction are development goals; if something feels sluggish, it's a bug.

## Supported Formats

MP3, FLAC, M4A/AAC, Ogg Vorbis, Opus, WAV, AC-3, DTS, and M3U/M3U8 playlists. Actual codec support depends on the GStreamer plugins installed on the host system.

On Fedora, the base, good, and bad-free GStreamer plugins that the RPM requires play every format above except AC-3 and DTS. Those need `gstreamer1-plugin-libav`, which the RPM recommends, so dnf installs it unless weak dependencies are turned off. Fedora builds that plugin against `ffmpeg-free`, which leaves out some codecs, such as E-AC-3. Replacing `ffmpeg-free` with the full FFmpeg from [RPM Fusion](https://rpmfusion.org/Howto/Multimedia) (`sudo dnf swap ffmpeg-free ffmpeg --allowerasing`) adds them. Ferrous does not require RPM Fusion.

## Installation

Prebuilt Fedora RPM packages are available from [GitHub Releases](https://github.com/Tiaxi/ferrous/releases).

To build from source, install the prerequisites in the [installation guide](docs/INSTALL.md), then:

```bash
git clone https://github.com/Tiaxi/ferrous.git
cd ferrous
./scripts/run-ui.sh
```

See the [installation guide](docs/INSTALL.md) for full dependency lists, manual build steps, and local package builds.

## Status

Ferrous is a personal project tailored for my specific needs and workflow. It is usable for daily listening but still under active development. Contributions and feedback are welcome, though the project direction follows my own priorities.

See the [backlog](docs/ROADMAP.md) for open ideas.

## Tech Stack

| Layer | Technology |
|-------|------------|
| Backend | Rust (playback, metadata, library, search, analysis) |
| Frontend | Qt6/QML + KDE Kirigami |
| Audio | GStreamer |
| Storage | SQLite |

## Documentation

- [Installation guide](docs/INSTALL.md) — dependencies, build options, data locations
- [Keyboard and mouse](docs/SHORTCUTS.md) — shortcuts, scrolling, and zooming
- [Development guide](docs/DEVELOPMENT.md) — building, testing, debugging, profiling
- [Backlog](docs/ROADMAP.md) — open feature and maintenance ideas
- [Contributing](CONTRIBUTING.md) — proposing and submitting changes

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
