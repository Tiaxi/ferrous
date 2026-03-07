# Development

This page keeps day-to-day development, validation, and debugging details out of the main repository README.

## Run The App

Preferred development path:

```bash
./scripts/run-ui.sh
```

The current UI build expects `/bin/zsh` because the CMake bridge target invokes Cargo through it.

Useful variants:

```bash
./scripts/run-ui.sh --no-run
./scripts/run-ui.sh --nuke-db
./scripts/run-ui.sh --nuke-session
./scripts/run-ui.sh --nuke-thumbnails
./scripts/run-ui.sh --nuke-all
./scripts/run-ui.sh --coredump
```

## Validation

Default validation entrypoint:

```bash
./scripts/run-tests.sh
```

Scope-specific variants:

```bash
./scripts/run-tests.sh --rust-only
./scripts/run-tests.sh --ui-only
```

By default the script runs:

- `cargo fmt --check`
- `cargo check --features gst`
- `cargo clippy --features gst -- -D clippy::pedantic`
- `cargo test --features gst`
- `cargo audit`
- UI configure/build/test via CMake and CTest

Optional coverage gate:

```bash
rustup component add llvm-tools-preview
cargo install cargo-llvm-cov
./scripts/run-tests.sh --rust-only --coverage
```

Coverage threshold is controlled by `FERROUS_COVERAGE_MIN`.

## Backend Debug CLI

For backend-oriented debugging without the Qt UI:

```bash
cargo run --bin frontend_cli --features gst
```

The CLI exposes playback and settings commands such as:

- `play`, `pause`, `stop`, `next`, `prev`
- `vol <0..1>`
- `seek <seconds>`
- `repeat <0|1|2>`
- `shuffle <0|1>`
- `dbrange <50..120>`
- `log <0|1>`
- `snap`

See `docs/FRONTEND_CLI.md` for the bootstrap context around this entrypoint.

## Local Build Configuration

Repository scripts load `build.env` if it exists. Start from:

```bash
cp build.env.example build.env
```

Current optional values include:

- `FERROUS_LASTFM_API_KEY`
- `FERROUS_LASTFM_SHARED_SECRET`
- `FERROUS_DNF_CMD`

Last.fm support is intended for local experimentation at this stage, not as a polished published feature.

## Runtime Knobs

Useful environment variables during local development:

- `FERROUS_BRIDGE_SNAPSHOT_MS`: bridge snapshot cadence, default `16`
- `FERROUS_UI_PAINT_FBO=1`: force framebuffer-backed painted items
- `FERROUS_UI_SHOW_FPS=1`: show the spectrogram FPS overlay on startup
- `FERROUS_UI_SEARCH_DEBOUNCE_MS=<ms>`: override search debounce timing

## Profiling Logs

Profiling prints are compile-time gated and are disabled by default.

Enable them in the UI build:

```bash
cmake -S ui -B ui/build -G Ninja -DFERROUS_ENABLE_PROFILE_LOGS=ON
cmake --build ui/build
```

Enable them for direct Rust CLI runs:

```bash
cargo run --bin frontend_cli --features "gst profiling-logs"
```

With a profile-enabled build, these runtime toggles become active:

- `FERROUS_PROFILE_UI=1`
- `FERROUS_PROFILE=1`
- `FERROUS_SEARCH_PROFILE=1`

## Packaging

Build a local RPM:

```bash
./scripts/build-rpm.sh
```

Install the newest local RPM:

```bash
./scripts/install-rpm.sh
```

Or build and install in one step:

```bash
./scripts/build-rpm.sh --install
```
