# Add `run-ui.sh` Clean-Slate Flags (`--nuke-db`, `--nuke-thumbnails`, `--nuke-all`)

## Summary
Add development-focused cleanup arguments to `scripts/run-ui.sh` so UI runs can start from a cold state for performance testing. Cleanup runs immediately after argument parsing (before configure/build/run), works with `--no-run`, and only targets Ferrous-owned cache/index files.

## CLI Changes
1. Add flags:
   1. `--nuke-db`: delete Ferrous SQLite DB file(s).
   2. `--nuke-thumbnails`: delete Ferrous library thumbnail cache directory.
   3. `--nuke-all`: equivalent to both flags.
2. Extend `--help` text with flag descriptions and exact target paths.
3. Keep all existing behavior unchanged when no new flag is provided.

## Target Paths and Deletion Rules
1. DB target path resolution (match Rust code):
   1. `${XDG_DATA_HOME:-$HOME/.local/share}/ferrous/library.sqlite3`
2. Also remove sidecars if present:
   1. `library.sqlite3-wal`
   2. `library.sqlite3-shm`
3. Thumbnail target path resolution:
   1. Primary: `${XDG_CACHE_HOME:-$HOME/.cache}/ferrous/thumbnails/library`
   2. Fallback candidate: `/tmp/ferrous/thumbnails/library` (matches UI fallback path behavior)
4. Safety constraints:
   1. Delete only explicit file/dir targets above.
   2. Never recurse on broad parent directories like `${XDG_DATA_HOME}` or `${XDG_CACHE_HOME}`.
   3. Emit clear log lines for each removed/missing target.

## Script Flow
1. Parse args and set cleanup booleans.
2. Run cleanup step immediately.
3. Continue existing configure/build/run flow unchanged.
4. Cleanup should still execute even if `--no-configure --no-build --no-run` is used (so script can be used as a clean-state utility).

## Docs Updates
1. Update usage references where appropriate:
   1. `README.md`
   2. `ui/README.md`
2. Mention examples:
   1. `./scripts/run-ui.sh --nuke-all`
   2. `./scripts/run-ui.sh --nuke-db --no-run`

## Important Behavioral Note
- `--nuke-db` clears both library index and waveform cache because both are stored in the same `library.sqlite3` file today.

## Test Scenarios
1. `./scripts/run-ui.sh --help` shows new options.
2. `./scripts/run-ui.sh --nuke-db --no-run --no-configure --no-build` removes DB targets only.
3. `./scripts/run-ui.sh --nuke-thumbnails --no-run --no-configure --no-build` removes thumbnail cache only.
4. `./scripts/run-ui.sh --nuke-all --no-run --no-configure --no-build` removes both sets.
5. Re-run with already-missing targets logs "not found" and exits successfully.
6. Normal run without nuke flags behaves exactly as before.

## Assumptions and Defaults
1. Cleanup scope is limited to Ferrous local DB/cache, not music source folders.
2. No changes are made to `/mnt/nassikka/Musiikki/Albumit/` or any original media files/folders.
