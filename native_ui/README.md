# Ferrous Native UI (Qt6/QML + Kirigami)

This directory contains the KDE-native frontend scaffold for Ferrous.

## Build

```bash
cd native_ui
cmake -B build -G Ninja
cmake --build build
```

## Run (dev mode)

```bash
FERROUS_BRIDGE_CMD='cargo run --bin native_frontend --features gst -- --json-bridge' ./build/ferrous_kirigami_shell
```

If `FERROUS_BRIDGE_CMD` is not set, the app uses this same command as default.

## Scope (Milestone A)

- Native shell window and menu/footer scaffolding
- Top transport controls wired to backend bridge
- Live bridge status/snapshot display
- Placeholder panes for playlist/library/spectrogram until later milestones

## Scope (Milestone B progress)

- Split layout matches Ferrous/DeaDBeeF structure with placeholder content
- Centralized playback actions are shared by toolbar + menu + shortcuts
- Seek and volume sliders are wired to bridge commands
