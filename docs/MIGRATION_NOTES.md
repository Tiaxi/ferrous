# Ferrous Frontend Migration Notes

This document summarizes the current frontend migration state and what changed for users/developers.

## Summary

- Primary UI path is now the KDE frontend in `ui/` (Qt6/QML + Kirigami).
- UI uses the in-process Rust FFI bridge by default (single-process runtime path).
- Legacy process/stdout JSON bridge remains available for fallback/debug use.
- Legacy egui frontend has been removed from the repository.

## Default Run Path

Use the UI launcher script from repository root:

```bash
./scripts/run-ui.sh
```

By default this launches:

- `ferrous` (Qt/Kirigami UI)
- in-process Rust backend bridge (`ferrous_ffi_bridge_*`)

No long-lived `native_frontend --json-bridge` subprocess is started in this default mode.

## Bridge Modes

### In-process mode (default)

- Selected automatically.
- Lowest overhead path.
- No stdio/pipe backpressure exposure in steady-state UI runtime.

### Process bridge fallback

Use when debugging transport/fallback behavior:

```bash
./scripts/run-ui.sh --process-bridge
```

Equivalent environment override:

```bash
FERROUS_BRIDGE_MODE=process ./scripts/run-ui.sh
```

Optional custom bridge command:

```bash
FERROUS_BRIDGE_MODE=process \
FERROUS_BRIDGE_CMD='cargo run --release --bin native_frontend --features gst -- --json-bridge' \
./scripts/run-ui.sh
```

## Legacy Frontend Status

- `eframe/egui` frontend modules (`src/main.rs`, `src/app/`, `src/ui/`) have been removed.

## Notes for Contributors

- Prefer validating changes against the UI launcher path first.
- Use process bridge mode only for targeted debugging/regression checks.
- Keep `src/bin/native_frontend.rs` as fallback/debug tooling, not steady-state production path.
