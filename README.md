# Ferrous

A high-performance Linux audio player prototype in Rust, inspired by Foobar2000/DeaDBeeF.

## Current status

This is a runnable architecture scaffold with:

- `eframe/egui` desktop UI shell
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

Install GTK/file-dialog runtime dependencies as needed for your distro (for `eframe`/`rfd`).

When enabling real playback with GStreamer later, install:

- GStreamer runtime + development packages
- plugins base/good (and likely `ugly` for MP3 in many distros)

## Commands

```bash
source "$HOME/.cargo/env"
cargo run
```

If you implement and gate GStreamer code behind the feature:

```bash
cargo run --features gst
```

Native KDE frontend (Qt6/QML + Kirigami) dev launcher:

```bash
./scripts/run-native-ui.sh
```

Run project tests (Rust + native UI smoke test):

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

Roadmap and engineering plans live under `docs/`:

- `docs/ROADMAP.md`
- `docs/TEST_PLAN.md`
- `docs/OPTIMIZATION_PLAN.md`

## Project layout

- `src/main.rs`: app entrypoint
- `src/app/`: app coordinator and event loop wiring
- `src/playback/`: playback engine command/event model (`gst` + stub backends)
- `src/analysis/`: waveform/spectrogram worker
- `src/metadata/`: track metadata + cover art extraction
- `src/ui/`: `egui` panels and visual rendering
