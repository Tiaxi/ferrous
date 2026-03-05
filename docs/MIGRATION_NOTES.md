# Ferrous Frontend Migration Notes

This document summarizes the current frontend migration state and what changed for users/developers.

## Summary

- Primary UI path is the KDE frontend in `ui/` (Qt6/QML + Kirigami).
- UI uses the in-process Rust FFI bridge (single-process runtime path).
- UI/backend command and snapshot transport is binary end-to-end.
- Legacy egui frontend has been removed from the repository.

## Default Run Path

Use the UI launcher script from repository root:

```bash
./scripts/run-ui.sh
```

By default this launches:

- `ferrous` (Qt/Kirigami UI)
- in-process Rust backend bridge (`ferrous_ffi_bridge_*`)

No long-lived bridge subprocess is started in this path.

## Legacy Frontend Status

- `eframe/egui` frontend modules (`src/main.rs`, `src/app/`, `src/ui/`) have been removed.

## Notes for Contributors

- Prefer validating changes against the UI launcher path first.
- Keep `src/bin/frontend_cli.rs` as CLI/debug tooling.
