# AGENTS.md

## Project Overview

Ferrous is a desktop music player. Rust backend (playback, analysis, library, metadata) linked as a static library into a Qt6/QML C++ frontend. GStreamer for audio. Symphonia for format probing. SQLite for library and waveform cache.

## Architecture

### Threading Model

Named threads communicate via `crossbeam_channel` (Rust) and a wakeup pipe (Rust→Qt). No shared mutable state outside channels and explicit atomics.

| Thread | Role |
|--------|------|
| Qt main | Event loop, rendering, QML |
| `ferrous-bridge` | Bridge loop: routes commands, emits snapshots |
| `ferrous-playback-gst` | GStreamer pipeline, bus messages, gapless queue |
| `ferrous-analysis` | Spectrogram/waveform state machine, PCM processing |
| `ferrous-spectrogram-decode` | FFT session: decode → STFT → rate-limited column output |
| `ferrous-spectrogram-staging` | Short-lived: pre-decodes next track for gapless |
| `ferrous-waveform-decode` | Symphonia peak extraction |
| `ferrous-library` | SQLite scan, FTS indexing |
| `ferrous-metadata` | Tag extraction, cover art, bitrate timeline |
| `ferrous-lastfm` | HTTP scrobbling |

### FFI Boundary (Rust ↔ C++)

- Rust exports `extern "C"` functions in `src/frontend_bridge/ffi.rs`.
- Qt sends binary-encoded commands via `ferrous_ffi_bridge_send_binary()`.
- Qt polls Rust via timer + wakeup pipe fd; drains binary event buffers.
- Rust allocates response buffers (`Vec::into_boxed_slice`); C++ frees via paired free function.
- Binary protocol defined in `ui/src/BinaryBridgeCodec.h` (command IDs) and `src/frontend_bridge/ffi.rs` (encoding/decoding).

### Spectrogram Display Modes

Rolling and centered modes have fundamentally different semantics:

- **Rolling**: Continuous write-order ring, `ContinueWithFile` for gapless STFT continuity, rate-limited at ~2× realtime, seeks restart the worker session.
- **Centered**: Full-track random-access ring, `NewTrack` for gapless (0-based indices), unlimited decode speed, seeks just move the display window.

Always check if behavior must differ by display mode when touching spectrogram, seek, or transition code.

### Playback Gapless Model

Two-phase: `about-to-finish` pre-arms the next URI (same-format) or sets `pending_eos_track_switch` (cross-format). `maybe_emit_natural_handoff()` confirms the real switch. `cancel_pending_gapless_advance()` reverts on seek near EOF.

### Binary Snapshot Protocol

12-byte header (magic + section bitmask), followed by present sections (playback, queue, library, metadata, settings, lastfm, error). Sections are length-prefixed. Queue inclusion is opt-in (only on `RequestSnapshot`). Heartbeat snapshots omit queue.

## Build & Validation

- `./scripts/run-tests.sh` is the default validation entrypoint.
  - Rust-only: `--rust-only`
  - UI-only: `--ui-only`
  - Cross-cutting (or uncertain): no flag
- Keep `--no-clippy` and `--no-audit` disabled unless explicitly justified.
- `cargo` commands that fetch from the network require elevated sandbox permissions.
- The Rust library builds as a static archive linked into the Qt executable via CMake.

## Code Quality Rules

### Root Cause Rule
- Do not guess at fixes. Trace the actual code paths and data flow to a concrete, defensible root cause before implementing a fix.
- If multiple plausible causes remain, keep investigating or state the uncertainty explicitly.

### UI Responsiveness Rule
- Target buttery smooth, hitching-free, immediately responsive UI.
- No blocking or long-running work on the UI thread.
- Treat jank, stutter, delayed feedback, and blocked interaction as correctness bugs, not polish.
- Prefer async/background execution, incremental updates, batching, and cancellation.

### Test Rule
- Add unit tests to lock in behavior, logic, and bug fixes.
- Test the specific invariant or edge case that motivated the change.
- Tests must be self-contained — no external files or network.
- Feature-gated tests: `#[cfg(not(feature = "gst"))]` for mock-playback tests, `#[cfg(feature = "gst")]` for GStreamer integration tests.

### Clippy Suppression Rule
- Do not add `#[allow(clippy::...)]` unless genuinely unavoidable (FFI constraints, intentional numeric casts, exact float comparison with known source).
- Every suppression needs a justification comment on the line above.
- Prefer refactoring: extract helpers for `too_many_lines`, use context structs for `too_many_arguments`, explicit conversions for cast warnings.

### Commit Policy
- Autonomous commits are allowed.
- Commit at coherent checkpoints when formatting/build checks pass and the tree is runnable.
- Prefer smaller, incremental commits over large mixed ones.
- Do not commit half-migrated or knowingly broken states.

## Naming Conventions

- Rust: `snake_case` functions, `UPPER_CASE` constants, `PascalCase` types.
- C++: `m_` prefix for member variables, `camelCase` methods.
- Threads: `ferrous-` prefix (e.g., `ferrous-spectrogram-decode`).
- Channels: `*_tx` / `*_rx` for sender/receiver pairs.
- Playback commands: `PlaybackCommand::Verb` (e.g., `PlayAt`, `Seek`, `Stop`).
- Analysis commands: `AnalysisCommand::Verb` or `SetNoun` (e.g., `SetTrack`, `SeekPosition`).
- Worker commands: `SpectrogramWorkerCommand::Verb` (e.g., `NewTrack`, `ContinueWithFile`).

## Error Handling

- `anyhow::Result<T>` for fallible operations (library, metadata, FFI prep).
- `.unwrap()` / `.expect()` only for invariants (thread spawn, channel send to known-live thread).
- No panics in event loops — log errors and preserve state.
- Bridge-layer errors surface as `BridgeEvent::Error(String)`.

## Unsafe

- Confined to FFI (`src/frontend_bridge/ffi.rs`): pipe setup, wakeup signaling, buffer handoff.
- Every `unsafe` block has a `// SAFETY:` comment.
- No unsafe for data races — use `Mutex`, `AtomicU64`, `AtomicBool`, channels.

## Key File Map

| Area | Files |
|------|-------|
| Bridge / state machine | `src/frontend_bridge/mod.rs` |
| C FFI exports | `src/frontend_bridge/ffi.rs` |
| Binary protocol (Rust) | `src/frontend_bridge/ffi.rs` (encode/decode) |
| Binary protocol (C++) | `ui/src/BinaryBridgeCodec.h`, `ui/src/BinaryBridgeCodec.cpp` |
| Playback engine | `src/playback/mod.rs` |
| Spectrogram / waveform | `src/analysis/mod.rs` |
| Library indexing | `src/library/mod.rs` |
| Metadata extraction | `src/metadata/mod.rs` |
| Spectrogram rendering | `ui/src/SpectrogramItem.h`, `ui/src/SpectrogramItem.cpp` |
| Bridge client (Qt) | `ui/src/BridgeClient.h`, `ui/src/BridgeClient.cpp` |
| Settings persistence | `src/frontend_bridge/mod.rs` (`format_settings_text`, `load_settings_into`) |
| QML entry | `ui/qml/Main.qml` |
| Test script | `scripts/run-tests.sh` |
