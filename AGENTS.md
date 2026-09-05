# Agent instructions

- Trace the relevant code and data flow before fixing a bug; identify the root cause rather than guessing.
- Keep the UI responsive. Move blocking or expensive work off the UI thread; use batching and cancellation where needed.
- For playback, seek, or visualization changes, check rolling and centered modes, track transitions, and stale asynchronous results.
- Keep Rust/C++ protocol changes synchronized, including buffer ownership and cleanup.
- Add a regression test for every bug fix and tests for every new feature. Cover both Rust and Qt behavior for changes that cross the boundary. Tests must use self-contained fixtures and require no network or personal media files.
- Validate code changes with `./scripts/run-tests.sh`: `--rust-only` for backend-only changes, `--ui-only` for UI/QML-only changes, and no flags for cross-cutting changes or uncertainty. Keep Clippy and audit enabled; report any checks that could not run. See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for setup and additional test configurations.
- For documentation-only changes, verify commands and claims against their implementation, check local links, and run `git diff --check`.
- Introduce no warnings. Prefer refactoring over lint suppressions; justify unavoidable suppressions with a nearby comment.
- Handle recoverable errors without panicking in event loops. Use `unwrap`/`expect` only for established invariants. Justify every `unsafe` block with a `// SAFETY:` comment.
- Follow the surrounding code's naming and style. Keep changes focused and avoid unrelated cleanup.
- Autonomous commits are allowed at coherent, validated checkpoints. Use lowercase conventional subjects (`fix: correct seek handling`) and branches of the form `<type>/<branch_name>`; do not use agent-specific branch types.
- Keep this file limited to actionable instructions. Put user and development documentation in the linked guides; keep implementation details with the code.
