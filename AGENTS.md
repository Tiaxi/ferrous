# Agent instructions

- Fix bugs where the wrong state originates. Do not hide them with timers, retries, delays, or flags that suppress updates.
- Keep the UI thread, the bridge thread (which also drives playback), and the analysis control thread free of blocking I/O and expensive work. Use workers with batching, cancellation, and generation checks.
- For playback, seek, or visualization changes, check rolling and centered modes, track transitions (including gapless and Repeat-One), and stale asynchronous results. Test playback behavior against the GStreamer backend; the mock backend has no about-to-finish phase.
- Keep Rust/C++ protocol changes synchronized, including command IDs, payload limits, buffer ownership, and cleanup.
- Write user files atomically (temporary file, then rename), preserve data the app does not edit, and never delete files the app did not create.
- Show errors that affect the user in the UI; logging alone is not error handling. Do not panic in event loops or across the FFI boundary; use `unwrap`/`expect` only for established invariants.
- Add a regression test for every bug fix and tests for every new feature. Cover both Rust and Qt behavior for changes that cross the boundary. Tests must use self-contained fixtures, need no network or personal media, and must not read or write the real config, data, or cache directories.
- Validate code changes with `./scripts/run-tests.sh`: `--rust-only` for backend-only changes, `--ui-only` for UI/QML-only changes, and no flags for cross-cutting changes or uncertainty. Keep Clippy and audit enabled; report any checks that could not run. See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for setup and additional test configurations.
- For documentation-only changes, verify commands and claims against their implementation, check local links, and run `git diff --check`.
- Introduce no warnings. Prefer refactoring over lint suppressions; justify unavoidable suppressions with a nearby comment. Justify every `unsafe` block with a `// SAFETY:` comment.
- Work on one issue per PR. Autonomous commits are allowed at coherent, validated checkpoints. Use lowercase conventional subjects (`fix: correct seek handling`) and branches of the form `<type>/<branch_name>`; do not use agent-specific branch types. Merge with `gh pr merge`.
- Keep this file limited to actionable instructions. Put user and development documentation in the linked guides; keep implementation details with the code.
