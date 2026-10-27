# Development

Run commands from the repository root. See [installation](INSTALL.md) for build dependencies.

## Build and run

```bash
./scripts/run-ui.sh
./scripts/run-ui.sh --no-run
```

The launcher configures CMake, builds the Rust static library and Qt executable, and launches the app unless `--no-run` is given. CMake builds the Rust library with `--locked` and the `cargo` it finds at configure time: `$CARGO` when set, otherwise the first `cargo` on the path, and `~/.cargo/bin/cargo` only when the path has none. Set the `FERROUS_CARGO` cache variable to choose another.

Repository scripts load optional `build.env` settings. Start with `cp build.env.example build.env` and replace the placeholder values. Last.fm builds need `FERROUS_LASTFM_API_KEY` and `FERROUS_LASTFM_SHARED_SECRET` at compile time; they are optional for other features. `FERROUS_DNF_CMD` overrides the local RPM installation command. Keep credentials out of version control.

Set `FERROUS_UI_BUILD_DIR` to use another UI build directory. Each UI build directory builds the Rust archive in its own Cargo target directory, `cargo/` inside it, so builds with and without profiling never link each other's archive; `CARGO_TARGET_DIR` does not move it, and the `FERROUS_CARGO_TARGET_DIR` cache variable does. The launcher also accepts `CMAKE_BUILD_TYPE` and `CMAKE_GENERATOR`.

Cargo's default features are `gst`, the GStreamer playback backend, and `cli`, the command-line frontend, so plain `cargo run`, `cargo test` and `cargo build` use GStreamer. Build with `--no-default-features --features cli` for the mock backend. `profiling-logs` adds the profiling output described below. The CMake build selects its own features. Release builds keep Rust line tables, so backtraces, coredumps and debuginfo packages show Rust files and line numbers.

## Validation

Install rustfmt, Clippy, and cargo-audit before running the checks:

```bash
rustup component add rustfmt clippy
cargo install cargo-audit
```

Use the repository script for validation:

| Change | Command |
| --- | --- |
| Rust/backend only | `./scripts/run-tests.sh --rust-only` |
| UI/QML only | `./scripts/run-tests.sh --ui-only` |
| Both stacks, build configuration, or uncertain scope | `./scripts/run-tests.sh` |

The Rust path runs formatting, `cargo check`, strict Clippy (`--all-targets -- -D warnings -D clippy::pedantic`; test code is exempt from the pedantic group only), tests, and cargo-audit with the ignore list in `.cargo/audit.toml`, in that order. CI runs the same Clippy and audit checks, and builds with `--locked`. The script builds without Cargo's default features and with `cli` and the features in `FERROUS_TEST_FEATURES`, `gst` unless set. The UI path configures and builds with CMake, then runs all registered Qt tests through CTest with offscreen rendering where configured. UI builds also rebuild the Rust static library. The script configures profiling logs off. Tests run with temporary `TMPDIR`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_CACHE_HOME` directories that the script removes afterwards; Rust unit tests also resolve the XDG directories under a per-process temporary root regardless of the environment.

The QML smoke tests are one binary, `ferrous_qml_smoke_tests`, with its tests grouped by area. CTest runs each area as `ferrous_qml_smoke_<area>`, in a process of its own and with a timeout, so a crash or hang costs only that area's results; `ctest --test-dir ui/build -R ferrous_qml_smoke_waveform` runs one. Run the binary with `-area <name>` for one area, or with test function names as any Qt test. `ferrous_qml_module_tests` loads the QML module that the app links, so it fails for a QML file missing from `qt_add_qml_module` in `ui/CMakeLists.txt`; `ferrous_qml_cache_linked` checks that the app has the module's compiled QML. Qt test binaries draw offscreen unless `QT_QPA_PLATFORM` or `-platform` chooses another platform, so running one directly opens no windows.

Keep Clippy and audit enabled. Fix failures and new warnings; report checks that could not run. For documentation-only changes, verify commands and claims against the source, check relative links, and run `git diff --check`.

CI also tests the mock playback backend. To exercise that path locally through the script:

```bash
FERROUS_TEST_FEATURES=' ' ./scripts/run-tests.sh --rust-only
```

The single space deliberately selects no backend feature; an empty value falls back to `gst` in the script. Add this run when changing shared playback behavior or mock-only tests. Without the script, `cargo test --no-default-features --features cli` tests the mock backend.

Optional coverage gate (default minimum line coverage: 35%, overridden by `FERROUS_COVERAGE_MIN`):

```bash
rustup component add llvm-tools-preview
cargo install cargo-llvm-cov
./scripts/run-tests.sh --rust-only --coverage
```

## Debugging

The backend CLI uses the in-process bridge without Qt:

```bash
cargo run --bin frontend_cli
```

It prints its commands at startup and reads them from standard input until `quit` or the end of the input, so it also takes commands from a pipe. These cover playback, seeking, volume, display settings, and snapshots; the UI does not launch this CLI as a subprocess.

For crash investigation, `./scripts/run-ui.sh --coredump` enables core dumps and prints `coredumpctl` commands.

The launcher also provides cleanup options that delete local app state:

| Option | Deletes |
| --- | --- |
| `--nuke-db` | Library database, including waveform cache, and SQLite WAL/SHM files |
| `--nuke-session` | Saved queue/session |
| `--nuke-thumbnails` | Library thumbnail cache that older versions kept under `$XDG_CACHE_HOME/ferrous` |
| `--nuke-all` | All three of the above; settings and embedded cover cache remain |
| `--clear-diagnostics-log` | Current diagnostics log |

The launcher lists what these options would delete and asks before deleting it. Pass `--yes` to delete without asking; without a terminal, the options need it. See [data locations](INSTALL.md#data-locations). Use `./scripts/run-ui.sh --help` for the complete launcher interface.

A GStreamer test that times out reports its pipeline's state, the elements still changing state, and its latest bus messages. Starving the tests of CPU makes stalls more likely, for example with `taskset -c 0,1 cargo test --features gst --lib -- --test-threads=4`. With `FERROUS_TEST_THREAD_DUMPS` set to a directory, the report also names a file there with the `gdb` stacks of every thread, taken while the pipeline stalls. This needs `gdb` and a `ptrace` scope that lets a process trace its parent (`kernel.yama.ptrace_scope` 0).

## Profiling

See [visualization architecture](VISUALIZATION_ARCHITECTURE.md) for decoder ownership, cache budgets, cancellation, and visibility scheduling.

Profiling logs are disabled at compile time by default. To build and run with UI instrumentation:

```bash
./scripts/run-ui.sh --profile-logs
```

This enables `FERROUS_ENABLE_PROFILE_LOGS` and launches with `FERROUS_PROFILE_UI=1` unless already set. For Rust backend profiling:

```bash
FERROUS_PROFILE=1 cargo run --bin frontend_cli --features profiling-logs
```

With a profiling build, these runtime variables select additional output:

| Variable | Purpose |
| --- | --- |
| `FERROUS_PROFILE_UI=1` | UI timing and seek diagnostics |
| `FERROUS_PROFILE=1` | Backend timing logs and UI profiling |
| `FERROUS_PROFILE_WAVEFORM=1` | Waveform editor timing logs |
| `FERROUS_SEARCH_PROFILE=1` | Rust search timing |
| `FERROUS_PROFILE_SPECTROGRAM_TRACE=1` | Detailed UI spectrogram traces |
| `FERROUS_PROFILE_HEARTBEAT_TRACE=1` | Detailed Rust heartbeat traces |

The desktop app copies console output to the diagnostics log available through **Help > Diagnostics**. Waveform editor entries use the `[ui-waveform-editor]` prefix.

To measure spectrogram CPU projection with synthetic resident data after a UI test build:

```bash
FERROUS_BENCHMARK_SPECTROGRAM=1 QT_QPA_PLATFORM=offscreen \
  ./ui/build/ferrous_qml_smoke_tests spectrogramRebuildBenchmark
```

This optional benchmark covers two FFT sizes, two pane heights, and linear/logarithmic frequency scales at 3440 pixels wide. For each, it reports the time to rasterize a full canvas on a worker (`raster_ms`), the render sync's time to hand that work to a worker (`request_ms`), the UI thread's time to put the result in place (`install_ms`), the render sync's time to pass its tiles on for upload (`handoff_ms`), and an image checksum. It excludes decoding, transport, and GPU uploads. Use the same optimized build and machine for comparisons; normal test runs skip it.

The canvas is drawn at device pixels. Add `FERROUS_BENCHMARK_SPECTROGRAM_SCALE=2` (or `1.5`, `1.25`) to measure it at that device pixel ratio: the panes keep their logical size and the canvas gets that many device pixels to a logical one. Under `QT_QPA_PLATFORM=offscreen`, the smoke tests add offscreen screens at 2, 1.5 and 1.25 beside the default one, which the HiDPI tests and the windowed parts of this benchmark move their windows to; they skip without them.

`spectrogramOverlayBenchmark` takes the same two variables. It measures the render sync's share of a 3440×719 pane's grid and crosshair overlays, with both grids and their labels shown: an ordinary frame, a time-grid refresh, a refresh of both grids, a crosshair move and a resize frame, as `mean_ms` and `worst_ms`.

`spectrogramPlaybackBenchmark` takes them too. It plays a 1720×719 pane rolling, centered and scrolling, centered with the whole track in view, and centered with both grids, time labels and the pointer over it. For each, it reports the frames the window draws a second of real playback (`frames_per_s`) and the renderer's mean time for them, then, over 120 frames 1/60 s apart, the canvas and overlay textures made a frame and their size, the texture nodes changed a frame, and the render sync's time.

To compare waveform CPU painting with scene graph preparation using synthetic resident data:

```bash
FERROUS_BENCHMARK_WAVEFORM=1 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=rhi \
  QSG_RHI_BACKEND=opengl QSG_RENDER_LOOP=basic \
  ./ui/build/ferrous_qml_smoke_tests waveformSceneGraphBenchmark
```

This optional benchmark uses 1200×180 and 3440×1440 viewports at overview, intermediate, and sample zoom levels, without and with the grid and its labels. It reports CPU milliseconds per frame and texture creation requests/payload bytes after warmup. It excludes decoding, GPU submission, presentation, and display refresh limits; it does not measure displayed FPS. Use the same optimized build, machine, and renderer for comparisons. The native command requires an X display (Xvfb works for automated checks); use `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software` to measure the software fallback.

With the same variable, `waveformEditorPlaybackBenchmark` reports the frames a window draws a second while a four-minute track plays, over the whole track and zoomed in.

The waveform editor retains image tiles in the scene graph and moves their source coordinates during playback. Native sample curves use antialiased geometry; labels use a bounded raster cache and are laid out only when first drawn. The software renderer keeps a single raster pass at sample zoom. Texture resources belong to the render node and are recreated with the window's scene graph. CI compares the native output with the shared raster reference at 1× and 2× scaling, including rulers, crosshairs, split channels, and texture tile boundaries.

Other useful runtime controls:

| Variable | Purpose / default |
| --- | --- |
| `FERROUS_UI_SHOW_FPS=1` | Show spectrogram FPS on startup |
| `FERROUS_UI_PAINT_FBO=1` | Request framebuffer rendering for the waveform overview |
| `FERROUS_UI_SEARCH_DEBOUNCE_MS` | UI search debounce, 40 ms |
| `FERROUS_BRIDGE_PLAYING_HEARTBEAT_MS` | Playback heartbeat, 40 ms |
| `FERROUS_BRIDGE_PAUSED_HEARTBEAT_MS` | Paused heartbeat, 333 ms |
| `FERROUS_BRIDGE_ANALYSIS_SNAPSHOT_MS` | Analysis snapshot interval, 16 ms |

## Translations

Ferrous translates its interface with [KI18n](https://invent.kde.org/frameworks/ki18n), from the `ferrous` gettext domain. There are no translations yet; the template and the code paths are in place.

- QML marks a string with the `I18n` singleton of `FerrousUi 1.0`, with one of KI18n's semantic context markers: `I18n.i18nc("@action:button", "Save")`. Counts use the plural forms, `I18n.i18ncp("@info:status", "%1 track", "%1 tracks", count)`, never a hand-built plural. JavaScript libraries import the module: `.import FerrousUi 1.0 as FerrousUi`.
- C++ uses `i18nc()` and `i18ncp()` from `<KLocalizedString>`; CMake sets `TRANSLATION_DOMAIN` for every target.
- Logic never compares or parses shown text. Pass codes, enums, or properties, and turn them into text only for display.
- Messages from the Rust backend, such as playback, scan, and tag errors, are still English. Translating them is a follow-up: the backend would send a code and its arguments, for the UI to translate.

To write the translation template, `po/ferrous.pot`, which is not committed:

```bash
./scripts/extract-messages.sh
# Or into ui/build/po/ferrous.pot:
cmake --build ui/build --target extract-messages
```

The script runs `ui/Messages.sh`, the extraction script that KDE's translation tooling looks for, with `xgettext` and KI18n's keywords. The `ferrous_translation_template` test checks the template, and `ferrous_i18n_tests` runs the interface under `x-test`, a pseudo-translation made at build time that wraps every message in `⟦` and `⟧`.

## Packaging

See [local package builds](INSTALL.md#local-package-builds) for RPM instructions. `./scripts/install-rpm.sh` installs the newest local RPM without rebuilding it. `./scripts/smoke-test-rpm.sh [rpm-path]` installs the newest local RPM, or the given one, in a fresh Fedora container with podman and network access, then validates the installed files and checks that Ferrous starts.

`./scripts/bump-version.sh <version>` updates the version in `Cargo.toml`'s `[package]` table, `Cargo.lock`, the RPM spec, and the AppStream metadata in `ui/io.github.tiaxi.ferrous.metainfo.xml`, where it adds a release dated today. The UI tests fail when the newest AppStream release is not the `Cargo.toml` version, and they validate the metadata and desktop entry when `appstreamcli` and `desktop-file-validate` are installed. CMake configures again when `Cargo.toml` changes, so the next build uses the new version. The RPM changelog has one entry, generated from the version and the packaged commit's date. The UI tests also run `scripts/tests/`, which test the repository scripts on scratch copies.

Pushing a `vX.Y.Z` tag runs the [release workflow](../.github/workflows/release.yml). It first runs `./scripts/check-release-version.sh <tag>`, which fails unless the tag matches the version in `Cargo.toml`, the RPM spec, and the newest AppStream release; run it locally before tagging. The workflow then runs the CI workflow on the tagged commit and builds the RPMs for each Fedora release with `%check` enabled. It publishes the release, with a `SHA256SUMS` file for the packages, only when CI and every required package build pass. A Fedora release that is not out yet is built too, but its failure does not block the release.
