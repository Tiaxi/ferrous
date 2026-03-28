# Installing Ferrous

Ferrous is currently a Linux-first project. The supported runtime path today is the Qt6/Kirigami desktop app in `ui/`, backed by the in-process Rust library.

## Packaging Status

- Prebuilt RPM and deb packages are attached to [GitHub Releases](https://github.com/Tiaxi/ferrous/releases).
- The repository also supports building from source or building packages locally.

## Requirements

Install the equivalent of these tools and development packages for your distro:

- Rust toolchain (`cargo`, `rustc`)
- `zsh`
- CMake
- Ninja
- `pkg-config`
- A C++20-capable compiler
- Qt 6.6 or newer development packages
- Qt Quick Controls 2 development packages
- KDE Frameworks 6 Kirigami development packages
- GStreamer runtime packages
- GStreamer development packages
- GStreamer codec plugins for the formats you want to play

GStreamer is required for the current UI build and default test path. There is no separate `cargo install` application flow at the moment.

For Fedora-like systems, the package names typically map closely to:

- `rust`
- `cargo`
- `cmake`
- `ninja-build`
- `gcc-c++`
- `qt6-qtbase-devel`
- `qt6-qtdeclarative-devel`
- `qt6-qtquickcontrols2-devel`
- `kf6-kirigami-devel`
- `gstreamer1-devel`
- `gstreamer1-plugins-base-devel`

Depending on the codecs you need, you may also want the distro-specific GStreamer plugin packages that provide MP3, AAC, Opus, and similar formats.

## Option 1: Run From Source

This is the simplest path today.

```bash
git clone https://github.com/Tiaxi/ferrous.git
cd ferrous
./scripts/run-ui.sh
```

`./scripts/run-ui.sh` will:

- load optional local build settings from `build.env`
- configure the Qt UI build
- build the Rust static library used by the UI
- build the UI executable
- launch Ferrous

The current UI build invokes Cargo through `/bin/zsh`, so `zsh` needs to be present even if your login shell is different.

Useful variants:

```bash
./scripts/run-ui.sh --no-run
./scripts/run-ui.sh --nuke-all
```

## Option 2: Build Manually

If you want the explicit build steps:

```bash
cargo build --release --features gst --lib
cmake -S ui -B ui/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ui/build
./ui/build/ferrous
```

To install into a local prefix instead of running from the build tree:

```bash
cmake --install ui/build --prefix "$HOME/.local"
```

That install path also places the desktop file and icon under the prefix.

## Option 3: Build A Local RPM

On Fedora-like systems:

```bash
./scripts/build-rpm.sh
```

To build and install immediately:

```bash
./scripts/build-rpm.sh --install
```

The generated RPM is written under `dist/rpm/RPMS/`.

## Option 4: Build A Local deb

On Debian/Ubuntu-like systems:

```bash
cp -r packaging/debian .
dpkg-buildpackage -us -uc -b
```

The generated `.deb` is written to the parent directory.

## Supported Formats

Ferrous currently handles common local audio and playlist formats, including:

- MP3
- FLAC
- M4A / AAC / MP4 audio
- Ogg Vorbis and Opus
- WAV
- AC-3 and DTS
- M3U / M3U8 playlists

Format handling is a combination of:

- file import support in the app
- MIME declarations in the desktop file
- the GStreamer plugins available on your machine

## Data Locations

Ferrous stores its local state under standard XDG locations:

- library database: `$XDG_DATA_HOME/ferrous/library.sqlite3`
- settings: `$XDG_CONFIG_HOME/ferrous/settings.txt`
- session restore: `$XDG_CONFIG_HOME/ferrous/session.json`
- cached cover art: `$XDG_CACHE_HOME/ferrous/embedded_covers`

If the XDG variables are unset, the usual `~/.local/share`, `~/.config`, and `~/.cache` fallbacks are used.
