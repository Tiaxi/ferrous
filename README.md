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

Backend CLI/debug shell:

```bash
cargo run --bin native_frontend --features gst
```

Force legacy process bridge mode for debugging:

```bash
./scripts/run-ui.sh --process-bridge
```

Run project tests (Rust + UI smoke test):

```bash
./scripts/run-tests.sh
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
- `FERROUS_FFI_JSON_SNAPSHOT_MS`: throttles JSON snapshot delivery to the Qt side in in-process mode (default `100`, range `16..1000`).
  - Lower values update playback/library text state more frequently with higher JSON/UI overhead.
- `FERROUS_UI_PAINT_IMAGE=1`: force `QQuickPaintedItem` image target (default uses framebuffer object target).
- `FERROUS_UI_SHOW_FPS=1`: show spectrogram FPS overlay.
- `FERROUS_PROFILE_UI=1`: print per-second UI paint cost counters (`[ui-spectrogram]`, `[ui-waveform]`).

Roadmap and engineering plans live under `docs/`:

- `docs/ROADMAP.md`
- `docs/MIGRATION_NOTES.md`
- `docs/TEST_PLAN.md`
- `docs/OPTIMIZATION_PLAN.md`

## Project layout

- `ui/`: Qt6/QML + Kirigami frontend (primary UI path)
- `src/bin/native_frontend.rs`: backend CLI/debug entrypoint + JSON bridge fallback
- `src/playback/`: playback engine command/event model (`gst` + stub backends)
- `src/analysis/`: waveform/spectrogram worker
- `src/metadata/`: track metadata + cover art extraction
- `src/frontend_bridge/`: typed bridge orchestration + FFI boundary
