# AGENTS.md

## Build/Tooling Rule
- For `cargo` commands that may require internet access (for example dependency/index fetches), run with an elevated prompt (`sandbox_permissions: require_escalated`) instead of sandbox-only execution.
- Use a concise justification tied to the exact cargo action.
- Prefer a reusable approval prefix for cargo workflows when appropriate.
