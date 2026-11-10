# Development

Run commands from the repository root. See [installation](INSTALL.md) for build dependencies.

## Build and run

```bash
./scripts/run-ui.sh
./scripts/run-ui.sh --no-run
```

The launcher configures CMake, builds the Rust static library and the app, and launches the app unless `--no-run` is given. It builds only the `ferrous` target, not the tests; `./scripts/run-tests.sh` builds those in the same build directory. Set `CMAKE_BUILD_PARALLEL_LEVEL` to limit the build jobs. CMake builds the Rust library with `--locked` and the `cargo` it finds at configure time: `$CARGO` when set, otherwise the first `cargo` on the path, and `~/.cargo/bin/cargo` only when the path has none. Set the `FERROUS_CARGO` cache variable to choose another.

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

The QML smoke tests are one binary, `ferrous_qml_smoke_tests`, with its tests grouped by area. Each test class is in a file of its own, `ui/tests/tst_qml_smoke_<name>.cpp`, with the helpers only its tests use; `ui/tests/QmlSmokeTest.h` has the base class and the helpers that more than one class uses, and `ui/tests/tst_qml_smoke.cpp` has `main()` and the classes of each area. The files compile in parallel, and a change to one recompiles only that file. An area has one or more classes: `spectrogram` has three, and `waveform` and `transport_input` two each. Add a new test to the class of its area in the file whose topic fits, and a new class to `kSmokeTestAreas`. CTest runs each area as `ferrous_qml_smoke_<area>`, in a process of its own and with a timeout, so a crash or hang costs only that area's results; `ctest --test-dir ui/build -R ferrous_qml_smoke_waveform` runs one. Run the binary with `-area <name>` for one area, or with test function names as any Qt test. Offscreen, the spectrogram draws its canvas in tiles, as it does with any renderer but OpenGL; CI also runs the spectrogram's pixel comparisons natively with OpenGL, where its canvas is in textures updated in place. `ferrous_qml_module_tests` loads the QML module that the app links, so it fails for a QML file missing from `qt_add_qml_module` in `ui/CMakeLists.txt`; `ferrous_qml_cache_linked` checks that the app has the module's compiled QML. The QML compiles to bytecode only, not to C++: compiled bindings call Qt's private API, whose symbols carry the exact Qt minor version, so the package would need a rebuild for each Qt update. `<binary>_no_private_qt_api` fails for any executable of the build that refers to Qt's private API; use public API instead. Qt test binaries draw offscreen unless `QT_QPA_PLATFORM` or `-platform` chooses another platform, so running one directly opens no windows. `ferrous_ui_motion_tests` runs Qt Quick's basic render loop whatever `QSG_RENDER_LOOP` says: its list-transition tests step animation time with an animation driver of their own, which Qt takes only while the render loop has installed none.

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

### Stress runs

Timing-sensitive tests fail only now and then, mostly on a loaded machine. `./scripts/stress-tests.sh` runs the suites again and again to find them: the Rust tests with the GStreamer backend (`gst`) and with the mock backend (`mock`), and the CTest tests (`ctest`), 30 times each by default. It builds first unless given `--no-build`. Run *i* of a suite uses the *i*-th value of its parallelism list, cycling: `--test-threads` 1, libtest's default and 32 for the mock backend; 4, the default and 32 for GStreamer, whose tests play their fixtures in real time and take about 20 minutes on one thread; and `ctest -j` 1, 4 and the number of CPUs. Every third run is under load, from `stress-ng --cpu` with twice as many workers as CPUs, or busy shell loops without `stress-ng`; the load ends with the run. The whole loop takes about two hours on a machine with 16 CPU threads.

```bash
./scripts/stress-tests.sh                             # 30 runs of each suite
./scripts/stress-tests.sh --suite mock --runs 10      # one suite
./scripts/stress-tests.sh --suite ctest --runs 5 --first 6 --no-build
```

`--first` numbers the runs from another index, so a long loop can be split into several invocations that keep the cycle of parallelism and load. Each run writes its log to `target/stress/<suite>-<i>.log` and appends a line to `target/stress/summary.txt` with its parallelism, whether it was loaded, the load average at its start and end, and its duration; a failed run adds the tests that failed. The runs use throwaway user and temporary directories, as `run-tests.sh` does, and the script exits with a failure when any run failed. `--help` lists the other options.

Fix a test that fails there where its timing assumption is: wait for the signal, state or message it needs instead of a fixed time, drive time the test sets (`TestTime` and the clocks' time sources in the Qt tests) instead of the wall clock, and isolate tests that share global state. A longer timeout alone does not fix it.

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

Recent Issues shows only errors that get in the user's way, such as a track that can't be played, a setting that wasn't saved, or Last.fm needing to be reconnected, and errors in Ferrous's own logic. `BridgeClient::reportError` writes each of them to the diagnostics log too, as `[<category>] error: <message> (<file>)` or `warning:`. What the user need not act on, such as a temporary Last.fm failure that is retried or a fallback that works, goes to the log only: the backend writes it to standard error with `log_warning` or `log_info` (`src/frontend_bridge/errors.rs`), as `[ferrous] [<category>] warning: <message>`. Routine activity worth tracing is logged with `log_info` as `[ferrous] [<category>] info: <message>`: each Last.fm Now Playing update and scrobble batch, with the tracks and Last.fm's answer, but never a key, secret, or request signature. An error that never reaches the UI, because the channel to it is closed, it gave way to the count of earlier errors, or the app quit before the UI took it, is logged by the backend in the same form, so each error is logged once.

The log reads as a trace of what the app did, one line per event and never one per frame, tick or snapshot; the profiling logs above cover timing. At info level it has:

- **Startup and shutdown:** the UI's `[app] info: Ferrous <version> started: …` line, with the Qt and KDE Frameworks versions, the desktop, Qt platform, style, icon theme and locale, and the backend's `[ferrous] [system] info: Backend started: …` line, with the compiler and GStreamer versions and the config, data and cache folders. A clean quit ends with `Backend stopped cleanly: …` and `[app] info: Ferrous quit cleanly.`
- **Playback:** each track start and how it began (`user`, `gapless`, `next in queue`, `Repeat-One`, `restored session`), playing, pausing and resuming with the position, stopping, each seek that lands, and repeat and shuffle changes. Playback errors are logged as errors. Each MPRIS command a client sends, such as a media key's, is an `[mpris] info:` line. Ferrous plays to the audio server's default output and leaves the output device to it, so device changes are not logged.
- **Library:** each scan's start and end, with its root, the files it added, changed and removed, and how long it took; what asked for it, when not the user: the start of watching, the 30-minute check of roots that are not watched, mount changes, or more file changes than the watches can follow. A scan of the files the watches saw change logs as `Scanning <n> changed paths in "<root>"`. Roots that become unavailable or available again, and roots that cannot be watched. Root changes and rescans the user asks for.
- **Tags:** each tag save, with its fields and files, each rename, and each file that failed.
- **Analysis:** a waveform decode that stopped at a read error, once per play. A file that is cut off is shown in Recent Issues, so it is logged as a warning.
- **Last.fm:** authentication changes, besides the Now Playing and scrobble lines above.
- **Settings:** each save of the settings logs the keys it changed with their new values, such as `Settings changed: fft_size=4096.` The Last.fm user name is logged only as changed.

Log lines quote paths in double quotes. `FERROUS_LOG=debug` adds `debug:` lines: each command the UI sends the backend, cut to 400 characters, and the UI's screens and platform theme at startup. The log keeps full paths, since it stays on the user's machine; **Copy Log Without Paths** in the Diagnostics dialog copies it with each path in a library root replaced by `<library N>/…`, any other path by `<path>`, and a quoted file name by `<file>`, keeping their extensions, for a bug report (`DiagnosticsLog::withoutPaths`). It keeps Ferrous's own folders, with the home folder as `~`, and system paths. It removes only paths and file names: the artists and titles in the Last.fm lines stay. Log lines quote a path in double quotes with its `"` and `\` escaped by a backslash (`quoted_path` in Rust, `DiagnosticsLog::quotedPath` in C++), as Rust's `{:?}` does; a quoted path with other quotes after it on its line, which a path an older build logged with unescaped quotes in it would look like, is redacted to the end of the line, unless it is one of Ferrous's own folders, so a line that quotes several library paths keeps only the first one's placeholder. The Last.fm account name is never logged. The UI writes its log lines, and reads the log for the Diagnostics dialog, on a thread of its own (`DiagnosticsLog::Writer`), so neither logging nor opening the dialog waits for the disk.

To measure spectrogram CPU projection with synthetic resident data after a UI test build:

```bash
FERROUS_BENCHMARK_SPECTROGRAM=1 QT_QPA_PLATFORM=offscreen \
  ./ui/build/ferrous_qml_smoke_tests spectrogramRebuildBenchmark
```

This optional benchmark covers two FFT sizes, two pane heights, and linear/logarithmic frequency scales at 3440 pixels wide. For each, it reports the time to rasterize a full canvas on a worker (`raster_ms`), the render sync's time to hand that work to a worker (`request_ms`), the UI thread's time to put the result in place (`install_ms`), the render sync's time to pass its tiles on for upload (`handoff_ms`), and an image checksum. It excludes decoding, transport, and GPU uploads. Use the same optimized build and machine for comparisons; normal test runs skip it.

The canvas is drawn at device pixels. Add `FERROUS_BENCHMARK_SPECTROGRAM_SCALE=2` (or `1.5`, `1.25`) to measure it at that device pixel ratio: the panes keep their logical size and the canvas gets that many device pixels to a logical one. Under `QT_QPA_PLATFORM=offscreen`, the smoke tests add offscreen screens at 2, 1.5 and 1.25 beside the default one, which the HiDPI tests and the windowed parts of this benchmark move their windows to; they skip without them.

`spectrogramOverlayBenchmark` takes the same two variables. It measures the render sync's share of a 3440×719 pane's grid and crosshair overlays, with both grids and their labels shown: an ordinary frame, a time-grid refresh, a refresh of both grids, a crosshair move and a resize frame, as `mean_ms` and `worst_ms`.

`spectrogramPlaybackBenchmark` takes them too. It plays a 1720×719 pane rolling, centered and scrolling, centered with the whole track in view, and centered with both grids, time labels and the pointer over it. For each, it reports the frames the window draws a second of real playback (`frames_per_s`) and the renderer's mean time for them, then, over 120 frames 1/60 s apart, the canvas textures made a frame (`textures_per_frame`), the uploads into canvas textures kept from frame to frame (`uploads_per_frame`), the canvas pixels passed on for both (`texture_kib_per_frame`), the overlay textures made and their size, the texture nodes changed a frame, and the render sync's time. With OpenGL, the canvas is in textures updated in place, which needs a native platform (`QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=rhi QSG_RHI_BACKEND=opengl`); with other renderers, including the offscreen platform's, in tiles made again when they change.

`spectrogramRingChangeBenchmark` times the first data of a track change and of an FFT size switch for a 3440-pixel stereo pane, rolling and centered, each making room for a new ring at the lookahead budget, as `first_chunk_ms` and `fft_switch_ms` (median and worst).

To compare waveform CPU painting with scene graph preparation using synthetic resident data:

```bash
FERROUS_BENCHMARK_WAVEFORM=1 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=rhi \
  QSG_RHI_BACKEND=opengl QSG_RENDER_LOOP=basic \
  ./ui/build/ferrous_qml_smoke_tests waveformSceneGraphBenchmark
```

This optional benchmark uses 1200×180 and 3440×1440 viewports at overview, intermediate, and sample zoom levels, without and with the grid and its labels. It reports CPU milliseconds per frame and texture creation requests/payload bytes after warmup. It excludes decoding, GPU submission, presentation, and display refresh limits; it does not measure displayed FPS. Use the same optimized build, machine, and renderer for comparisons. The native command requires an X display (Xvfb works for automated checks); use `QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software` to measure the software fallback.

With the same variable, `waveformEditorPlaybackBenchmark` reports the frames a window draws a second while a four-minute track plays, over the whole track and zoomed in.

The waveform editor retains image tiles in the scene graph and moves their source coordinates during playback. While playing, a pool of worker threads makes every playback tile the view lacks, and those playback reaches in the next 50 ms, from a copy of the detail they show; a deep zoom moves the view on by more than its width a frame. Until a tile arrives, the frame draws the detail's envelope, its lowest and highest value in each column, in its place, so no tile is rasterized while the GUI thread waits for the frame and the view is never empty. Tiles come in strips no taller than 2048 device pixels, which the scene graph takes as textures without copying them. Samples 2 logical pixels apart or more, which show markers, are drawn directly as geometry at any scale rather than in tiles: rasterizing their markers again each frame costs more. That geometry is kept from frame to frame in chunks of 512 device columns along the track, each built once in coordinates of its own and moved into the view by a translation, the view moving whole device columns; a frame builds only the chunks that come into view and drops those that leave it. A frame that moved on by the view's width or more since the last, as frames slower than the view can be followed in do at the deepest zoom of a 96 kHz track, builds each chunk for the samples the view shows alone, as no later frame shows them; the frames after it do so until one moves on by less than 80% of the width, so that a view moving on by about its width a frame does not switch between the two and build every chunk again each time. A chunk draws the markers of the samples it owns, by frame of the track, and the segments from them, with three samples either side as context for their joins and curves, so that chunks meet as one stroke. Chunks are built again when anything they show changes: the zoom, scale, size, lanes, view mode, muted or soloed channels, the file or its sample rate, a jump of the playback clock, or a new detail window holding more of the samples a chunk takes shape from. Each chunk is in a transform node that Qt's renderer makes a batch root once it moves, so that moving it changes a matrix; these nodes are reused for later chunks rather than added and removed, which would rebuild the renderer's batches. `waveformEditorDeepZoomPlaybackBenchmark` reports the time a frame of a 3440×1412 view at 2x spends drawing the waveform at several zooms while playing, after a seek, at 120 and 360 frames a second, with the variable above, and the geometry vertices it draws, the bytes it uploads, and the chunks it builds a frame. Native sample curves and markers use antialiased geometry, in nodes of at most 65,532 vertices: Qt's renderer merges such nodes with 16-bit indices, so the vertices of a larger one past 65,535 would not be drawn. Samples closer than 4.2 logical pixels are joined by straight segments rather than curves, and their markers are squares in whole device pixels, one quad each rather than 30 antialiased vertices: there a marker is as wide as the interval to the next sample, so the curve's bends do not show. Each segment's stroke takes its own normal, so that steep segments keep the pen's width; gentle bends are mitred and sharper ones bevelled, as QPainter joins them. `waveformSceneGraphCloseSamplesLookAsBefore` draws the geometry as a GPU would and compares it with the curves and antialiased markers; with `FERROUS_WAVEFORM_GRAB_DIR` set to a directory, it saves both pictures there. `waveformSceneGraphSampleChunkBoundariesDoNotShow` compares frames drawn in chunks with the same frames drawn whole, and saves both there too. Labels use a bounded raster cache and are laid out only when first drawn. The software renderer keeps a single raster pass at sample zoom. Texture resources belong to the render node and are recreated with the window's scene graph. CI compares the native output with the shared raster reference at 1× and 2× scaling, including rulers, crosshairs, split channels, and texture tile boundaries.

For backend measurements (analysis throughput per file, seek latency, waveform detail windows, and the app's per-thread CPU and wakeups), see the harness in [`scripts/perf`](../scripts/perf/README.md).

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

The script runs `ui/Messages.sh`, the extraction script that KDE's translation tooling looks for, with `xgettext` and KI18n's keywords. The `ferrous_translation_template` test checks the template, and `ferrous_i18n_tests` runs the interface under `x-test`, a pseudo-translation made at build time that wraps every message in `⟦` and `⟧`. Gettext translates nothing under the C locale, so that test runs in `en_US.UTF-8` (`glibc-langpack-en` on Fedora).

## Packaging

See [local package builds](INSTALL.md#local-package-builds) for RPM instructions. `./scripts/install-rpm.sh` installs the newest local RPM without rebuilding it. `./scripts/smoke-test-rpm.sh [rpm-path]` installs the newest local RPM, or the given one, in a fresh Fedora container with podman and network access, then validates the installed files and checks that Ferrous starts.

`./scripts/bump-version.sh <version>` updates the version in `Cargo.toml`'s `[package]` table, `Cargo.lock`, the RPM spec, and the AppStream metadata in `ui/io.github.tiaxi.ferrous.metainfo.xml`, where it adds a release dated today. The UI tests fail when the newest AppStream release is not the `Cargo.toml` version, and they validate the metadata and desktop entry when `appstreamcli` and `desktop-file-validate` are installed. CMake configures again when `Cargo.toml` changes, so the next build uses the new version. The RPM changelog has one entry, generated from the version and the packaged commit's date. The UI tests also run `scripts/tests/`, which test the repository scripts on scratch copies.

The app icon is one hand-drawn SVG per size in `ui/assets/icons/hicolor/<size>/apps/ferrous.svg`: 16, 22, 32, 48, 64, 128 and 256 px, and a scalable drawing for the sizes between. Edit each size on its own pixel grid rather than scaling another. The build installs them into the hicolor icon theme and builds them in, for the window icon and the queue's first-run image. `ferrous_app_icon_tests` draws each size and checks it, then checks a `cmake --install` of the `desktop_integration` component (the desktop file, the AppStream metadata and the icons) into the build directory, and that the hicolor theme gives each size its own drawing.

Pushing a `vX.Y.Z` tag runs the [release workflow](../.github/workflows/release.yml). It first runs `./scripts/check-release-version.sh <tag>`, which fails unless the tag matches the version in `Cargo.toml`, the RPM spec, and the newest AppStream release; run it locally before tagging. The workflow then runs the CI workflow on the tagged commit and builds the RPMs for each Fedora release with `%check` enabled. It publishes the release, with a `SHA256SUMS` file for the packages, only when CI and every required package build pass. A Fedora release that is not out yet is built too, but its failure does not block the release. `scripts/collect-release-assets.sh` gathers the downloaded packages, fails without a package from each required build or with two packages of the same name, and writes `SHA256SUMS`.
