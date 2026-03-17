# AGENTS.md

## Build/Tooling Rule
- For `cargo` commands that may require internet access (for example dependency/index fetches), run with an elevated prompt (`sandbox_permissions: require_escalated`) instead of sandbox-only execution.
- Use a concise justification tied to the exact cargo action.
- Prefer a reusable approval prefix for cargo workflows when appropriate.

## Validation Rule
- Use `./scripts/run-tests.sh` as the default validation entrypoint for this repository.
- Choose test scope based on the change surface:
  - Rust/backend-only changes: `./scripts/run-tests.sh --rust-only`
  - UI/QML-only changes: `./scripts/run-tests.sh --ui-only`
  - Cross-cutting changes (or uncertainty about impact): `./scripts/run-tests.sh`
- Keep strict checks enabled by default. Do not use `--no-clippy` or `--no-audit` unless explicitly justified.
- Use `--no-configure` / `--no-build` only when reusing a known-good UI build directory.
- Use `--coverage` only when a coverage gate is intentionally part of the task.

## Root Cause Rule
- When investigating bugs, regressions, or broken behavior, do not guess at likely fixes and stop at the first plausible explanation.
- Inspect the relevant code paths and data flow until there is a concrete, defensible root cause for the observed behavior.
- Only then implement the fix. If multiple plausible causes remain, keep investigating or explicitly state the remaining uncertainty instead of presenting a blind guess as the answer.

## UI Responsiveness Rule
- Always target buttery smooth, hitching-free, stutter-free, immediately responsive UI behavior.
- Do not put blocking or long-running work on the UI thread.
- Backend and frontend changes must preserve immediate reaction to user input, scrolling, animation, window interaction, and playback controls.
- Prefer asynchronous/background execution, incremental updates, batching, and cancellation over synchronous work that can stall rendering or input handling.
- Treat UI jank, visible hitching, delayed feedback, and blocked interaction as correctness issues, not polish-only issues.
- Apply this rule to both backend and frontend design and implementation work, especially when introducing I/O, parsing, image processing, model updates, or expensive recomputation.

## Test Rule
- Always add unit tests when feasible to lock in implementation details, behavioural logic, or bug fixes and prevent future regressions.
- Prefer testing the specific invariant or edge case that motivated the change.
- Tests should be self-contained and not depend on external files or network access.

## Commit Policy
- Autonomous commits are allowed in this repository.
- Commit when all of the following are true:
  - A coherent checkpoint is reached (feature slice, bugfix, refactor boundary, or roadmap sub-step).
  - Relevant formatting/build checks for touched code pass locally (at minimum `cargo fmt` + `cargo check`, and UI build when changed).
  - The tree is in a runnable/debuggable state (no known breakage introduced by the commit).
  - Commit message clearly states scope and intent.
- Prefer smaller, incremental commits over large mixed commits.
- Do not commit half-migrated or knowingly broken states unless explicitly requested by the user.
