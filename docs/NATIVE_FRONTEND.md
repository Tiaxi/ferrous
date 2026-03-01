# Native Frontend Bootstrap (Qt6/QML + Kirigami)

This document tracks the KDE-native frontend bootstrap and local build expectations.

## Goal

Build a Kirigami frontend on top of the existing Rust backend (playback, analysis, metadata, library).

## Current Bootstrap State

- A typed Rust bridge API exists in `src/frontend_bridge/mod.rs`.
- A second app entrypoint exists at `src/bin/native_frontend.rs`.
- `native_frontend` supports:
  - interactive CLI mode
  - JSON bridge mode (`--json-bridge`) for external UI clients.
- A Kirigami shell scaffold exists in `native_ui/` and talks to the JSON bridge.
- Existing egui frontend (`src/main.rs`) remains buildable.

## Why this bootstrap exists

Before wiring Qt/QML bindings, we need:

- a stable command/event contract,
- backend state snapshots for UI consumption,
- a separate frontend target to avoid blocking ongoing development.

## Current Qt/Kirigami integration path

1. Keep `frontend_bridge` as backend orchestration layer.
2. Use JSON bridge mode from `native_frontend` during early development.
3. Replace transport layer with direct CXX-Qt bridge once shell behavior stabilizes.

## KDE dev prerequisites (target environment)

- Qt 6 development packages
- KDE Frameworks / Kirigami development packages
- CMake + Ninja
- Rust toolchain

Package names differ by distro. Install the Qt6 + Kirigami/KF6 development meta-packages appropriate for your KDE distro.

## Running current bootstrap

```bash
cargo run --bin native_frontend --features gst
```

Commands in bootstrap shell:

- `play`, `pause`, `stop`, `next`, `prev`
- `vol <0..1>`
- `seek <seconds>`
- `snap`
- `quit`

## Running JSON bridge mode

```bash
cargo run --bin native_frontend --features gst -- --json-bridge
```

Input is line-delimited JSON commands, for example:

```json
{"cmd":"play"}
{"cmd":"set_volume","value":0.5}
{"cmd":"seek","value":42.25}
{"cmd":"request_snapshot"}
```

Output is line-delimited JSON events (`snapshot`, `error`, `stopped`).

## Running Kirigami shell scaffold

```bash
cd native_ui
cmake -B build -G Ninja
cmake --build build
FERROUS_BRIDGE_CMD='cargo run --bin native_frontend --features gst -- --json-bridge' ./build/ferrous_kirigami_shell
```

Notes:

- The shell is currently Milestone A scaffolding (layout + control wiring + status/footer).
- Playlist/library/spectrogram widgets in QML are placeholders pending Milestones C-E.
