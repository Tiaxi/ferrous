# Installing Ferrous

Ferrous is a Linux desktop app built with Qt6/Kirigami and an in-process Rust library.

## Prebuilt packages

Check [GitHub Releases](https://github.com/Tiaxi/ferrous/releases) for RPM and deb assets. Choose a package built for your distribution version and architecture:

```bash
# Fedora
sudo dnf install ./ferrous-*.rpm

# Ubuntu/Debian
sudo apt install ./ferrous_*.deb
```

## Build dependencies

Install these tools and development libraries:

- Rust toolchain (`cargo`, `rustc`)
- Bash and `/bin/zsh` (CMake invokes Cargo through zsh)
- CMake 3.24 or newer, Ninja, pkg-config, and a C++20 compiler
- Qt 6.8 or newer development libraries: Core, DBus, Gui, Qml, Quick, QuickControls2, Network, Widgets, Concurrent, and Test
- KDE Frameworks 6 Kirigami development libraries and QML runtime modules
- GLib/GIO and GStreamer development libraries, including app, audio, and pbutils
- GStreamer runtime codec plugins for the formats you want to play

Although CMake declares a Qt 6.6 minimum, it unconditionally enables [QTP0004, introduced in Qt 6.8](https://doc.qt.io/qt-6/qt-cmake-policy-qtp0004.html), so source builds need Qt 6.8 or newer. See [UI CI](../.github/workflows/ci.yml) for the reference distribution and Rust toolchain.

For Fedora, the build package set used by CI is:

```bash
sudo dnf install git rust cargo zsh cmake ninja-build gcc-c++ \
  pkgconf-pkg-config glib2-devel gstreamer1-devel \
  gstreamer1-plugins-base-devel qt6-qtbase-devel \
  qt6-qtdeclarative-devel qt6-qtquickcontrols2-devel kf6-kirigami-devel
```

For Debian/Ubuntu package names, see [build dependencies](../packaging/debian/control) and the [release workflow](../.github/workflows/release.yml). Rust must also be installed. Older distributions may not provide the required Qt/Kirigami versions.

## Run from source

```bash
git clone https://github.com/Tiaxi/ferrous.git
cd ferrous
./scripts/run-ui.sh
```

The launcher loads optional `build.env` settings, configures CMake, builds Rust and Qt, and launches Ferrous. Use `./scripts/run-ui.sh --no-run` to build without launching. See [development](DEVELOPMENT.md) for Last.fm build credentials, tests, debugging, and profiling.

## Manual build and install

CMake builds the Rust static library with the `gst` feature automatically; no separate Cargo build is needed:

```bash
cmake -S ui -B ui/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ui/build
./ui/build/ferrous
```

To install the executable, desktop entry, and icon under a local prefix:

```bash
cmake --install ui/build --prefix "$HOME/.local"
```

Manual CMake and Cargo commands do not load `build.env`; export any build credentials in your shell first. There is no `cargo install` flow for the desktop app.

## Local package builds

On Fedora, install the build dependencies above plus `rpm-build` and `desktop-file-utils`, then run:

```bash
./scripts/build-rpm.sh
# Or build and install:
./scripts/build-rpm.sh --install
```

The helper packages the current working tree, runs package checks, and writes RPMs under `dist/rpm/RPMS/`.

On Debian/Ubuntu, install `dpkg-dev`, `debhelper`, Rust, and the dependencies listed in [packaging/debian/control](../packaging/debian/control), then:

```bash
cp -r packaging/debian .
dpkg-buildpackage -us -uc -b
```

The deb is written to the parent directory. This manual packaging path also requires build credentials to be exported if Last.fm support is wanted.

## Formats

See [supported formats](../README.md#supported-formats). Playback codec support depends on installed GStreamer plugins; file import and visualization support also depend on Ferrous's decoders.

## Data locations

| State | Location |
| --- | --- |
| Library database and waveform cache | `$XDG_DATA_HOME/ferrous/library.sqlite3` |
| Diagnostics log | `$XDG_DATA_HOME/ferrous/diagnostics.log` |
| Settings | `$XDG_CONFIG_HOME/ferrous/settings.txt` |
| Session restore | `$XDG_CONFIG_HOME/ferrous/session.json` |
| Embedded cover art | `$XDG_CACHE_HOME/ferrous/embedded_covers` |

Unset XDG variables fall back to `~/.local/share`, `~/.config`, and `~/.cache`. Last.fm session keys are stored in the system keyring.
