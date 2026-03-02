# UI Bootstrap (Qt6/QML + Kirigami)

This document tracks the KDE UI bootstrap and local build expectations.

## Goal

Build a Kirigami frontend on top of the existing Rust backend (playback, analysis, metadata, library).

## Current Bootstrap State

- A typed Rust bridge API exists in `src/frontend_bridge/mod.rs`.
- A second app entrypoint exists at `src/bin/native_frontend.rs`.
- `native_frontend` supports:
  - interactive CLI mode
  - JSON bridge mode (`--json-bridge`) for external UI clients.
- A Kirigami shell exists in `ui/` and uses the in-process Rust FFI bridge by default.

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
2. UI now links Rust backend in-process via C FFI bridge by default.
3. Keep JSON bridge mode from `native_frontend --json-bridge` as optional fallback/debug path.

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

## Running JSON bridge mode

```bash
cargo run --release --bin native_frontend --features gst -- --json-bridge
```

Input is line-delimited JSON commands, for example:

```json
{"cmd":"play"}
{"cmd":"set_volume","value":0.5}
{"cmd":"set_db_range","value":90}
{"cmd":"set_log_scale","value":1}
{"cmd":"seek","value":42.25}
{"cmd":"play_at","value":3}
{"cmd":"select_queue","value":3}
{"cmd":"remove_at","value":3}
{"cmd":"move_queue","from":3,"to":1}
{"cmd":"clear_queue"}
{"cmd":"replace_album","paths":["/music/album/01.flac","/music/album/02.flac"]}
{"cmd":"append_album","paths":["/music/album/03.flac"]}
{"cmd":"scan_root","path":"/home/user/Music"}
{"cmd":"request_snapshot"}
```

Output is line-delimited JSON events (`snapshot`, `error`, `stopped`).
For performance, `snapshot.analysis.waveform_peaks` and `snapshot.analysis.spectrogram_rows`
are delta-style payloads and may be `null` when unchanged.

## Running Kirigami shell scaffold

```bash
cd ui
cmake -B build -G Ninja
cmake --build build
./build/ferrous_kirigami_shell
```

One-command dev path from repo root:

```bash
./scripts/run-ui.sh
```

The script still builds `target/release/native_frontend` for CLI/debug tooling.
In default in-process mode, the UI does not launch a long-lived bridge subprocess.

Process bridge fallback:

- Set `FERROUS_BRIDGE_MODE=process` to force legacy process/stdout bridge mode.
- Or run `./scripts/run-ui.sh --process-bridge`.

Build-only check:

```bash
./scripts/run-ui.sh --no-run
```

Notes:

- The UI shell now runs against the in-process Rust bridge by default.
- Process bridge mode remains available for fallback/debugging only.
