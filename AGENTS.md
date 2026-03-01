# AGENTS.md

## Build/Tooling Rule
- For `cargo` commands that may require internet access (for example dependency/index fetches), run with an elevated prompt (`sandbox_permissions: require_escalated`) instead of sandbox-only execution.
- Use a concise justification tied to the exact cargo action.
- Prefer a reusable approval prefix for cargo workflows when appropriate.

## Commit Policy
- Autonomous commits are allowed in this repository.
- Commit when all of the following are true:
  - A coherent checkpoint is reached (feature slice, bugfix, refactor boundary, or roadmap sub-step).
  - Relevant formatting/build checks for touched code pass locally (at minimum `cargo fmt` + `cargo check`, and native UI build when changed).
  - The tree is in a runnable/debuggable state (no known breakage introduced by the commit).
  - Commit message clearly states scope and intent.
- Prefer smaller, incremental commits over large mixed commits.
- Do not commit half-migrated or knowingly broken states unless explicitly requested by the user.
