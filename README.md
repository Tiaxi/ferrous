# Ferrous

Ferrous is a Linux desktop audio player prototype built with a Rust backend and a Qt6/Kirigami frontend. It is aimed at fast local-library playback, responsive queue workflows, and rich playback visualization.

## Highlights

- Linux-first desktop UI built with Qt6/QML and KDE Kirigami
- Real playback through GStreamer, including seeking, repeat, shuffle, volume control, and gapless queue handoff
- Local library indexing from one or more folders, backed by SQLite
- Folder-first library browsing with artist, album, and track grouping
- Global search across artists, albums, and tracks
- Queue workflows for opening files, adding folders, importing playlists, reordering tracks, and restoring the previous session
- Embedded cover art extraction plus live waveform and spectrogram views
- KDE/Plasma integration through MPRIS media controls, media keys, and single-instance file opening

## Supported Content

Ferrous currently targets local audio playback on Linux. The UI and desktop integration are wired for common audio and playlist formats including:

- MP3
- FLAC
- M4A / AAC / MP4 audio
- Ogg Vorbis and Opus
- WAV
- AC-3 and DTS
- M3U / M3U8 playlists

Actual playback support depends on the GStreamer plugins installed on the host system.

## Status

Ferrous is usable today, but it should still be treated as a prototype rather than a polished end-user release.

What is already in place:

- Playback, queue management, library indexing, global search, and visualization
- Persistent settings and queue/session restore
- Desktop-file and MIME integration for installed builds
- Local RPM packaging for Fedora-like systems

What is still in progress:

- ReplayGain and preamp behavior
- Crossfade and output-device tuning
- Deeper visualization customization
- General polish expected from a mature public release

## Installation

Prebuilt packages are not published yet. Right now Ferrous is best installed from source or from a locally built RPM.

### Quick Start From Source

Install these prerequisites for your distro:

- Rust toolchain
- `zsh`
- CMake and Ninja
- A C++20-capable compiler
- Qt 6.6+ development packages, including Quick Controls 2
- KDE Frameworks 6 Kirigami development packages
- GStreamer runtime and development packages
- GStreamer codec plugins for the formats you want to play

Then run:

```bash
git clone <your-repo-url>
cd ferrous
./scripts/run-ui.sh
```

That script builds the Rust backend, configures the Qt UI, builds the app, and launches Ferrous.

### Local RPM Build

For Fedora-like systems, the repository includes local RPM packaging:

```bash
./scripts/build-rpm.sh
./scripts/build-rpm.sh --install
```

## Development And Docs

- [Installation guide](docs/INSTALL.md)
- [Development guide](docs/DEVELOPMENT.md)
- [UI-specific notes](ui/README.md)
- [Roadmap](docs/ROADMAP.md)

## Tech Stack

- Rust for playback, metadata, library, search, and analysis
- Qt6/QML + Kirigami for the desktop UI
- GStreamer for playback
- SQLite for library and waveform cache persistence

## License

Licensing is not finalized yet. This repository does not currently ship a public open-source license.
