# UI Bootstrap (Qt6/QML + Kirigami)

This document tracks the KDE UI bootstrap and local build expectations.

## Goal

Build a Kirigami frontend on top of the existing Rust backend (playback, analysis, metadata, library).

## Current Bootstrap State

- A typed Rust bridge API exists in `src/frontend_bridge/mod.rs`.
- A second app entrypoint exists at `src/bin/native_frontend.rs`.
- `native_frontend` supports interactive CLI mode for backend debugging.
- The Kirigami shell in `ui/` uses the in-process Rust FFI bridge (binary protocol).

## Engineering Plans

- Test plan: `docs/TEST_PLAN.md`
- Optimization backlog: `docs/OPTIMIZATION_PLAN.md`

## Why this bootstrap exists

Before wiring Qt/QML bindings, we need:

- a stable command/event contract,
- backend state snapshots for UI consumption,
- a separate frontend target to avoid blocking ongoing development.

## Current Qt/Kirigami integration path

1. Keep `frontend_bridge` as backend orchestration layer.
2. UI links Rust backend in-process via C FFI bridge.
3. Bridge snapshots and commands use a binary protocol end-to-end.

## KDE dev prerequisites (target environment)

- Qt 6 development packages
- KDE Frameworks / Kirigami development packages
- CMake + Ninja
- Rust toolchain

Package names differ by distro. Install the Qt6 + Kirigami/KF6 development meta-packages appropriate for your KDE distro.

## Running current bootstrap

```bash
cargo run --release --bin native_frontend --features gst
```

Commands in bootstrap shell:

- `play`, `pause`, `stop`, `next`, `prev`
- `vol <0..1>`
- `seek <seconds>`
- `dbrange <50..120>`
- `log <0|1>`
- `snap`
- `quit`

## Running Kirigami shell scaffold

```bash
cd ui
cmake -B build -G Ninja
cmake --build build
./build/ferrous
```

One-command dev path from repo root:

```bash
./scripts/run-ui.sh
```

Build-only check:

```bash
./scripts/run-ui.sh --no-run
```

Notes:

- The UI shell runs against the in-process Rust bridge.
- `native_frontend` remains a CLI/debug tool; the UI does not launch it as a subprocess.
