# Development

Run commands from the repository root. See [installation](INSTALL.md) for build dependencies.

## Build and run

```bash
./scripts/run-ui.sh
./scripts/run-ui.sh --no-run
```

The launcher configures CMake, builds the Rust static library and Qt executable, and launches the app unless `--no-run` is given. CMake invokes Cargo through `/bin/zsh`.

Repository scripts load optional `build.env` settings. Start with `cp build.env.example build.env` and replace the placeholder values. Last.fm builds need `FERROUS_LASTFM_API_KEY` and `FERROUS_LASTFM_SHARED_SECRET` at compile time; they are optional for other features. `FERROUS_DNF_CMD` overrides the local RPM installation command. Keep credentials out of version control.

Set `FERROUS_UI_BUILD_DIR` to use another UI build directory. The Rust archive still builds under `target/release`. The launcher also accepts `CMAKE_BUILD_TYPE` and `CMAKE_GENERATOR`.

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

The Rust path runs formatting, `cargo check`, strict Clippy (`--all-targets -- -D warnings -D clippy::pedantic`; test code is exempt from the pedantic group only), tests, and cargo-audit with the ignore list in `.cargo/audit.toml`, in that order. CI runs the same Clippy and audit checks, and builds with `--locked`. The default feature set is `gst`. The UI path configures and builds with CMake, then runs all registered Qt tests through CTest with offscreen rendering where configured. UI builds also rebuild the Rust static library. The script configures profiling logs off. Tests run with temporary `TMPDIR`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_CACHE_HOME` directories that the script removes afterwards; Rust unit tests also resolve the XDG directories under a per-process temporary root regardless of the environment.

Keep Clippy and audit enabled. Fix failures and new warnings; report checks that could not run. For documentation-only changes, verify commands and claims against the source, check relative links, and run `git diff --check`.

CI also tests the mock playback backend. To exercise that path locally through the script:

```bash
FERROUS_TEST_FEATURES=' ' ./scripts/run-tests.sh --rust-only
```

The single space deliberately supplies no Cargo features; an empty value falls back to `gst` in the script. Add this run when changing shared playback behavior or mock-only tests.

Optional coverage gate (default minimum line coverage: 35%, overridden by `FERROUS_COVERAGE_MIN`):

```bash
rustup component add llvm-tools-preview
cargo install cargo-llvm-cov
./scripts/run-tests.sh --rust-only --coverage
```

## Debugging

The backend CLI uses the in-process bridge without Qt:

```bash
cargo run --bin frontend_cli --features gst
```

It prints its commands at startup. These cover playback, seeking, volume, display settings, and snapshots; the UI does not launch this CLI as a subprocess.

For crash investigation, `./scripts/run-ui.sh --coredump` enables core dumps and prints `coredumpctl` commands.

The launcher also provides cleanup options that delete local app state:

| Option | Deletes |
| --- | --- |
| `--nuke-db` | Library database, including waveform cache, and SQLite WAL/SHM files |
| `--nuke-session` | Saved queue/session |
| `--nuke-thumbnails` | Library thumbnail cache, including its temporary fallback |
| `--nuke-all` | All three of the above; settings and embedded cover cache remain |
| `--clear-diagnostics-log` | Current diagnostics log |

See [data locations](INSTALL.md#data-locations). Use `./scripts/run-ui.sh --help` for the complete launcher interface.

A GStreamer test that times out reports its pipeline's state, the elements still changing state, and its latest bus messages. Starving the tests of CPU makes stalls more likely, for example with `taskset -c 0,1 cargo test --features gst --lib -- --test-threads=4`. With `FERROUS_TEST_THREAD_DUMPS` set to a directory, the report also names a file there with the `gdb` stacks of every thread, taken while the pipeline stalls. This needs `gdb` and a `ptrace` scope that lets a process trace its parent (`kernel.yama.ptrace_scope` 0).

## Profiling

See [visualization architecture](VISUALIZATION_ARCHITECTURE.md) for decoder ownership, cache budgets, cancellation, and visibility scheduling.

Profiling logs are disabled at compile time by default. To build and run with UI instrumentation:

```bash
./scripts/run-ui.sh --profile-logs
```

This enables `FERROUS_ENABLE_PROFILE_LOGS` and launches with `FERROUS_PROFILE_UI=1` unless already set. For Rust backend profiling:

```bash
FERROUS_PROFILE=1 cargo run --bin frontend_cli --features 'gst profiling-logs'
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

## Packaging

See [local package builds](INSTALL.md#local-package-builds) for RPM instructions. `./scripts/install-rpm.sh` installs the newest local RPM without rebuilding it. `./scripts/smoke-test-rpm.sh [rpm-path]` installs the newest local RPM, or the given one, in a fresh Fedora container with podman and network access, then validates the installed files and checks that Ferrous starts.

`./scripts/bump-version.sh <version>` updates the version in `Cargo.toml`, `Cargo.lock`, the RPM spec, and the AppStream metadata in `ui/io.github.tiaxi.ferrous.metainfo.xml`, where it adds a release dated today. The UI tests fail when the newest AppStream release is not the `Cargo.toml` version, and they validate the metadata and desktop entry when `appstreamcli` and `desktop-file-validate` are installed. The RPM changelog has one entry, generated from the version and the packaged commit's date.

Pushing a `vX.Y.Z` tag runs the [release workflow](../.github/workflows/release.yml). It first runs `./scripts/check-release-version.sh <tag>`, which fails unless the tag matches the version in `Cargo.toml`, the RPM spec, and the newest AppStream release; run it locally before tagging. The workflow then runs the CI workflow on the tagged commit and builds the RPMs for each Fedora release with `%check` enabled. It publishes the release, with a `SHA256SUMS` file for the packages, only when CI and every required package build pass. A Fedora release that is not out yet is built too, but its failure does not block the release.
