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
- CMake 3.24 or newer, Ninja, pkg-config, and a C++20 compiler; the tests need CMake 3.29 or newer
- Qt 6.8 or newer development libraries: Core, DBus, Gui, Qml, Quick, QuickControls2, Network, Widgets, Concurrent, and Test
- KDE Frameworks 6 Kirigami development libraries and QML runtime modules
- KDE Frameworks 6 KI18n 6.8 or newer, which translates the interface
- GNU gettext (`xgettext`, `msgen`, `msgfilter`, `msgfmt`) for the tests and the translation template, and the `en_US.UTF-8` locale for the translation tests
- `flock` from util-linux, for `scripts/build-slot.sh` and its test
- GLib/GIO and GStreamer development libraries, including app, audio, and pbutils
- The D-Bus development library (libdbus), which the Last.fm keyring support links
- GStreamer runtime codec plugins for the formats you want to play
- For the GStreamer backend tests, the good and bad-free GStreamer plugins (`gstreamer1-plugins-good` and `gstreamer1-plugins-bad-free` on Fedora); the AC-3 and DTS tests also need `gstreamer1-plugin-libav`

To run Ferrous, also install the KDE `qqc2-desktop-style` QML module (`kf6-qqc2-desktop-style` on Fedora). Ferrous uses its `org.kde.desktop` controls style and does not start without it, and the UI tests use it too, with the Breeze icon theme (`breeze-icon-theme`). Plasma desktops include both.

CMake refuses a Qt older than 6.8. See [UI CI](../.github/workflows/ci.yml) for the reference distribution and Rust toolchain.

For Fedora, the build package set used by CI is:

```bash
sudo dnf install git rust cargo cmake ninja-build gcc-c++ \
  pkgconf-pkg-config dbus-devel glib2-devel gstreamer1-devel \
  gstreamer1-plugins-base-devel qt6-qtbase-devel \
  qt6-qtdeclarative-devel qt6-qtquickcontrols2-devel kf6-kirigami-devel \
  kf6-ki18n-devel kf6-qqc2-desktop-style breeze-icon-theme gettext \
  glibc-langpack-en util-linux-core
```

### Optional developer tools

The [UI capture harness](../tools/ui-capture/README.md), which takes and measures screenshots of the interface, needs more tools on top of the build dependencies. Neither the build nor the tests use it:

- Python 3 with Pillow, numpy, mutagen, and dbus-python
- ImageMagick and ffmpeg (with the FLAC, MP3, and Opus encoders), which generate its fixtures
- KWin (`kwin_wayland`, run nested and headless), `dbus-run-session`, the KDE platform theme, the Breeze colour schemes, and Noto Sans
- `unshare` (with unprivileged user namespaces), `flock`, `rsync`, `bc`, and `nm`
- The KF6 IconThemes library, which one of its probes links
- GStreamer's `fakeaudiosink` (bad-free plugins) and `gst-launch-1.0`: the harness plays every track to that sink and refuses to start without it, since the app would otherwise fall back to a real audio device

On Fedora:

```bash
sudo dnf install python3-pillow python3-numpy python3-mutagen python3-dbus \
  ImageMagick ffmpeg-free kwin plasma-integration plasma-breeze-common \
  google-noto-sans-fonts kf6-kiconthemes rsync bc binutils util-linux-core \
  gstreamer1 gstreamer1-plugins-bad-free
```

With RPM Fusion's `ffmpeg` installed, leave out `ffmpeg-free`; either has the encoders.

## Run from source

```bash
git clone https://github.com/Tiaxi/ferrous.git
cd ferrous
./scripts/run-ui.sh
```

The launcher loads optional `build.env` settings, configures CMake, builds the Rust library and the app (not the tests), and launches Ferrous. Use `./scripts/run-ui.sh --no-run` to build without launching. See [development](DEVELOPMENT.md) for Last.fm build credentials, tests, debugging, and profiling.

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
| Settings, including the window's size, position, pane sizes, visualization mode, whether the queue follows the current track, the widths of the queue columns you resized, and the keyboard shortcuts you changed | `$XDG_CONFIG_HOME/ferrous/settings.txt` |
| Pending Last.fm scrobbles | `$XDG_CONFIG_HOME/ferrous/lastfm_queue.json`; an unreadable file is kept as `lastfm_queue.json.corrupt` |
| Session restore: the queue, the library view, repeat, shuffle, and where the current track was paused or playing | `$XDG_CONFIG_HOME/ferrous/session.json` |
| Embedded cover art | `$XDG_CACHE_HOME/ferrous/embedded_covers` |
| Album art links for desktop media controls (MPRIS) | `$XDG_CACHE_HOME/ferrous/mpris_art` |

Unset XDG variables fall back to `~/.local/share`, `~/.config`, and `~/.cache`. Last.fm session keys are stored in the system keyring.

The embedded cover cache keeps one file per distinct embedded picture. Files that the library database no longer references are removed after library scans.

The media controls folder holds symbolic links to the current track's cover, one per revision of the image, so that desktop shells show a cover replaced at the same path. Ferrous keeps the four newest links and removes older ones; the cover files themselves are not changed.
