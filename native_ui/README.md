# Ferrous Native UI (Qt6/QML + Kirigami)

This directory contains the KDE-native frontend scaffold for Ferrous.

Default runtime mode uses an in-process Rust backend via C FFI.

## Build

```bash
cd native_ui
cmake -B build -G Ninja
cmake --build build
```

## Fast Dev Launch

From repo root:

```bash
./scripts/run-native-ui.sh
```

This script builds Rust artifacts first, then launches the native UI.
By default, no bridge subprocess is spawned.

Build-only (no GUI launch):

```bash
./scripts/run-native-ui.sh --no-run
```

## Run (process bridge fallback)

```bash
FERROUS_BRIDGE_MODE=process \
FERROUS_BRIDGE_CMD='cargo run --release --bin native_frontend --features gst -- --json-bridge' \
./build/ferrous_kirigami_shell
```

If `FERROUS_BRIDGE_MODE=process` and `FERROUS_BRIDGE_CMD` is not set, the app auto-detects
`target/release/native_frontend --json-bridge`, then falls back to
`cargo run --release --bin native_frontend --features gst -- --json-bridge`.

## Scope (Milestone A)

- Native shell window and menu/footer scaffolding
- Top transport controls wired to backend bridge
- Live bridge status/snapshot display
- Placeholder panes for playlist/library/spectrogram until later milestones

## Scope (Milestone B progress)

- Split layout matches Ferrous/DeaDBeeF structure with placeholder content
- Centralized playback actions are shared by toolbar + menu + shortcuts
- Seek and volume sliders are wired to bridge commands
