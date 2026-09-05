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

The Rust path runs formatting, `cargo check`, strict Clippy (`-D clippy::pedantic`), tests, and cargo-audit, in that order. The default feature set is `gst`. The UI path configures and builds with CMake, then runs all registered Qt tests through CTest with offscreen rendering where configured. UI builds also rebuild the Rust static library. The script configures profiling logs off.

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

This optional benchmark reports full-canvas rebuild times and image checksums for two FFT sizes, two pane heights, and linear/logarithmic frequency scales at 3440 pixels wide. It excludes decoding, transport, and GPU uploads. Use the same optimized build and machine for comparisons; normal test runs skip it.

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

See [local package builds](INSTALL.md#local-package-builds) for RPM and deb instructions. `./scripts/install-rpm.sh` installs the newest local RPM without rebuilding it.
