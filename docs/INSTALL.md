# Installing Ferrous

Ferrous is a Linux desktop app built with Qt6/Kirigami and an in-process Rust library.

## Prebuilt packages

Check [GitHub Releases](https://github.com/Tiaxi/ferrous/releases) for Fedora RPM assets. Choose a package built for your Fedora version and architecture:

```bash
sudo dnf install ./ferrous-*.rpm
```

Each release also has a `SHA256SUMS` file. To check the packages you downloaded, run this in the directory that holds them and `SHA256SUMS`:

```bash
sha256sum --check --ignore-missing SHA256SUMS
```

## Build dependencies

Install these tools and development libraries:

- Rust toolchain (`cargo`, `rustc`)
- Bash, for the repository scripts
- CMake 3.24 or newer, Ninja, pkg-config, and a C++20 compiler
- Qt 6.8 or newer development libraries: Core, DBus, Gui, Qml, Quick, QuickControls2, Network, Widgets, Concurrent, and Test
- KDE Frameworks 6 Kirigami development libraries and QML runtime modules
- GLib/GIO and GStreamer development libraries, including app, audio, and pbutils
- The D-Bus development library (libdbus), which the Last.fm keyring support links
- GStreamer runtime codec plugins for the formats you want to play

To run Ferrous, also install the KDE `qqc2-desktop-style` QML module (`kf6-qqc2-desktop-style` on Fedora). Ferrous uses its `org.kde.desktop` controls style and does not start without it. Plasma desktops include it.

Although CMake declares a Qt 6.6 minimum, it unconditionally enables [QTP0004, introduced in Qt 6.8](https://doc.qt.io/qt-6/qt-cmake-policy-qtp0004.html), so source builds need Qt 6.8 or newer. See [UI CI](../.github/workflows/ci.yml) for the reference distribution and Rust toolchain.

For Fedora, the build package set used by CI is:

```bash
sudo dnf install git rust cargo cmake ninja-build gcc-c++ \
  pkgconf-pkg-config dbus-devel glib2-devel gstreamer1-devel \
  gstreamer1-plugins-base-devel qt6-qtbase-devel \
  qt6-qtdeclarative-devel qt6-qtquickcontrols2-devel kf6-kirigami-devel
```

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

On Fedora, install the packaging tools and the spec's build dependencies, then run:

```bash
sudo dnf install rpm-build appstream desktop-file-utils
sudo dnf builddep packaging/rpm/ferrous.spec
./scripts/build-rpm.sh
# Or build and install:
./scripts/build-rpm.sh --install
```

The helper packages the current working tree, vendors the crates that `Cargo.lock` lists into a second source tarball, validates the desktop entry and AppStream metadata, runs package checks, and writes RPMs under `dist/rpm/RPMS/`. Vendoring needs network access unless Cargo has already downloaded the crates; the package build itself runs offline. Package builds do not treat compiler warnings as errors.

rpmbuild checks that Fedora's `rust` and `cargo` packages are installed, but it does not choose the compiler: CMake runs `$CARGO` when set, otherwise the first `cargo` on the path, and `~/.cargo/bin/cargo` only when the path has none. Release packages are built in a clean Fedora container, where Fedora's toolchain is the only one. On a machine with rustup's `~/.cargo/bin` on the path, that `cargo` is rustup's proxy, so a local package build uses the rustup toolchain unless you link Fedora's toolchain into rustup and select it:

```bash
rustup toolchain link fedora /usr
RUSTUP_TOOLCHAIN=fedora ./scripts/build-rpm.sh
```

## Formats

See [supported formats](../README.md#supported-formats). Playback codec support depends on installed GStreamer plugins; file import and visualization support also depend on Ferrous's decoders.

## Data locations

| State | Location |
| --- | --- |
| Library database and waveform cache | `$XDG_DATA_HOME/ferrous/library.sqlite3`, with SQLite's `-wal` and `-shm` files while Ferrous runs; a damaged database is kept in `library.sqlite3.damaged-<UTC time>/` |
| Diagnostics log | `$XDG_DATA_HOME/ferrous/diagnostics.log` |
| Settings, including the window's size, position, pane sizes, visualization mode, whether the queue follows the current track, and the widths of the queue columns you resized | `$XDG_CONFIG_HOME/ferrous/settings.txt` |
| Pending Last.fm scrobbles | `$XDG_CONFIG_HOME/ferrous/lastfm_queue.json`; an unreadable file is kept as `lastfm_queue.json.corrupt` |
| Session restore: the queue, the library view, repeat, shuffle, and where the current track was paused or playing | `$XDG_CONFIG_HOME/ferrous/session.json` |
| Embedded cover art | `$XDG_CACHE_HOME/ferrous/embedded_covers` |
| Album art links for desktop media controls (MPRIS) | `$XDG_CACHE_HOME/ferrous/mpris_art` |

Unset XDG variables fall back to `~/.local/share`, `~/.config`, and `~/.cache`. Last.fm session keys are stored in the system keyring.

The embedded cover cache keeps one file per distinct embedded picture. Files that the library database no longer references are removed after library scans.

The media controls folder holds symbolic links to the current track's cover, one per revision of the image, so that desktop shells show a cover replaced at the same path. Ferrous keeps the four newest links and removes older ones; the cover files themselves are not changed.
