# Ferrous UI (Qt6/QML + Kirigami)

This directory contains the KDE frontend scaffold for Ferrous.

Runtime mode uses an in-process Rust backend via C FFI.

## Build

```bash
cd ui
cmake -B build -G Ninja
cmake --build build
```

## Fast Dev Launch

From repo root:

```bash
./scripts/run-ui.sh
```

This script builds Rust artifacts first, then launches the UI.

Launch from a clean Ferrous state (library DB + thumbnail cache):

```bash
./scripts/run-ui.sh --nuke-all
```

Build-only (no GUI launch):

```bash
./scripts/run-ui.sh --no-run
```

Cleanup-only utility mode:

```bash
./scripts/run-ui.sh --nuke-thumbnails --no-configure --no-build --no-run
```

## Tests

```bash
cmake -S ui -B ui/build
cmake --build ui/build
ctest --test-dir ui/build --output-on-failure
```

## Scope (Milestone A)

- UI shell window and menu/footer scaffolding
- Top transport controls wired to backend bridge
- Live bridge status/snapshot display
- Placeholder panes for playlist/library/spectrogram until later milestones

## Scope (Milestone B progress)

- Split layout matches Ferrous/DeaDBeeF structure with placeholder content
- Centralized playback actions are shared by toolbar + menu + shortcuts
- Seek and volume sliders are wired to bridge commands
