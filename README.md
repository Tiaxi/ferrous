# Ferrous

A high-performance Linux audio player prototype in Rust, inspired by Foobar2000/DeaDBeeF.

## Current status

This is a runnable architecture scaffold with:

- KDE Qt6/QML (Kirigami) frontend (`ui/`) as the primary UI path
- playback worker thread with queue/state/seek commands
- metadata worker using `lofty` (title/artist/album + embedded cover art extraction)
- analysis worker with live waveform accumulation + STFT spectrogram
- transport controls and seekbar wired through command/event channels
- queue view with Open/Add files, Prev/Next, and click-to-play track selection

With `--features gst`, playback uses a real GStreamer `playbin` backend with:

- MP3/FLAC playback through installed GStreamer plugins
- gapless queue handoff via `about-to-finish`
- PCM tap (`appsink`) feeding analysis data for waveform/spectrogram visuals

Without `--features gst`, playback remains a simulated backend for development.

## Build prerequisites (Linux)

Install Rust via `rustup` (user-local):

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source "$HOME/.cargo/env"
```

Install Qt6 + Kirigami/KF6 development/runtime packages for your distro (primary UI path).

When enabling real playback with GStreamer later, install:

- GStreamer runtime + development packages
- plugins base/good (and likely `ugly` for MP3 in many distros)

## Commands

Primary app run path (Kirigami UI + in-process Rust backend):

```bash
./scripts/run-ui.sh
```

Start from a clean local Ferrous state (library DB + thumbnail cache):

```bash
./scripts/run-ui.sh --nuke-all
```

Cleanup-only utility mode (no configure/build/run):

```bash
./scripts/run-ui.sh --nuke-db --no-configure --no-build --no-run
```

Backend CLI/debug shell:

```bash
cargo run --bin frontend_cli --features gst
```

Run project tests (Rust + UI smoke test):

```bash
./scripts/run-tests.sh
```

Build a local RPM for quick deployment testing:

```bash
./scripts/build-rpm.sh
```

Build and install the resulting RPM locally via `dnf`:

```bash
./scripts/build-rpm.sh --install
```

The RPM path is also reusable later through:

```bash
./scripts/install-rpm.sh
```

Rust verification now also includes strict lint/security checks by default:

- `cargo clippy --features gst -- -D clippy::pedantic`
- `cargo audit` (requires `cargo-audit` installed)

Install `cargo-audit` once:

```bash
cargo install cargo-audit
```

Use `./scripts/run-tests.sh --no-clippy --no-audit` to temporarily skip them.

Optional coverage gate (line coverage threshold via `cargo llvm-cov`):

- enable in script: `./scripts/run-tests.sh --coverage`
- configure threshold: `FERROUS_COVERAGE_MIN=35 ./scripts/run-tests.sh --rust-only --coverage`

Install coverage tooling once:

```bash
rustup component add llvm-tools-preview
cargo install cargo-llvm-cov
```

Optional runtime tuning:

- `FERROUS_BRIDGE_SNAPSHOT_MS`: controls bridge snapshot cadence (default `16`, range `8..1000`).
- `FERROUS_UI_PAINT_FBO=1`: force `QQuickPaintedItem` framebuffer target (default uses image target).
- `FERROUS_UI_SHOW_FPS=1`: show spectrogram FPS overlay.
- `FERROUS_UI_SEARCH_DEBOUNCE_MS`: search debounce in milliseconds (default `90`).

### Profiling logs (compile-time gated)

Profiling prints are compiled out by default. Runtime env vars such as
`FERROUS_PROFILE_UI`, `FERROUS_PROFILE`, and `FERROUS_SEARCH_PROFILE`
only produce output when profiling logs are compiled in.

Enable profiling logs for the UI build:

```bash
cmake -S ui -B ui/build -G Ninja -DFERROUS_ENABLE_PROFILE_LOGS=ON
cmake --build ui/build -j
```

Enable profiling logs for direct Rust runs:

```bash
cargo run --bin frontend_cli --features "gst profiling-logs"
```

With a profile-enabled build, typical runtime toggles are:

- `FERROUS_PROFILE_UI=1`: per-second UI paint counters (`[ui-spectrogram]`, `[ui-waveform]`).
- `FERROUS_PROFILE=1`: bridge/playback/analysis profiling logs.
- `FERROUS_SEARCH_PROFILE=1`: search-worker and search-apply profiling logs.

Diagnostics log UI no longer live-rebinds large text while closed; use the
Diagnostics dialog `Reload` button to refresh text from disk.

Roadmap and engineering plans live under `docs/`:

- `docs/ROADMAP.md`
- `docs/MIGRATION_NOTES.md`
- `docs/TEST_PLAN.md`
- `docs/OPTIMIZATION_PLAN.md`

## Project layout

- `ui/`: Qt6/QML + Kirigami frontend (primary UI path)
- `src/bin/frontend_cli.rs`: backend CLI/debug entrypoint
- `src/playback/`: playback engine command/event model (`gst` + stub backends)
- `src/analysis/`: waveform/spectrogram worker
- `src/metadata/`: track metadata + cover art extraction
- `src/frontend_bridge/`: typed bridge orchestration + FFI boundary
