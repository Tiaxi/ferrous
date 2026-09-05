# Visualization architecture

## Playback and analysis

Playback owns audio output and publishes transport position and track identity. It does not forward unused PCM to the visualizations. Analysis opens independent decoders, preserving native sample rate and timestamped frame positions. Exact seeks decode codec preroll and trim to the requested frame. Spectrogram sessions align their FFT windows to an absolute sample grid; timestamp gaps become bounded silence batches and overlaps are trimmed. Waveform detail preserves missing coverage separately from actual silence.

The analysis control thread owns track identity, display settings, generation changes, and gapless staging. A spectrogram worker owns its decoder, FFT scratch storage, peak-hold state, and partial output batch. Seeking resets these objects without recursive decode loops or rebuilding FFT plans. Rolling continuation uses a continuous column timeline; centered transitions use the new track's timeline.

## Ownership and backpressure

Spectral batches travel through a dedicated bounded channel to an FFI relay. Snapshot/control delivery remains independent. The relay serializes outside the UI queue lock and waits for queue space. Generation changes discard obsolete output and interrupt blocked sends; shutdown wakes the relay explicitly. Rust and Qt must agree on command IDs, payloads, and FFI buffer ownership.

| Storage | Policy |
| --- | --- |
| GStreamer analysis appsink | Bounded queue; producers wait instead of dropping audio |
| Spectral output channel | Four batches |
| Serialized FFI spectral queue | 16 MiB |
| Centered next-track staging | At most an 8 MiB prefix |
| Speculative spectral lookahead | 64 MiB across channels, subject to the minimum visible window |
| Waveform PCM cache | 32 MiB LRU, 16,384-frame tiles |
| Waveform summary cache | Separate 16 MiB LRU |

The lookahead policy is shared by Rust and Qt through `ferrous_ffi_spectrogram_ring_capacity`. It preserves one viewport of history in centered mode and two in rolling mode, in addition to lookahead. Visible-window requirements can exceed the speculative budget; this is not a hard process-memory ceiling. Decoder storage, render images, metadata, and allocations in flight are additional costs.

## Waveform queries

Detail requests run on Qt worker threads. A shared Rust service serializes access to a persistent decoder and caches PCM and min/max summaries. File identity includes path, length, and modification time. Cancellation also applies while waiting for the service; incomplete cancelled tiles are not cached. The summary pyramid starts at 64-frame blocks and supports coarser power-of-two queries without decoding again. Clipped endpoints use PCM to avoid including samples outside the requested interval.

Native channels and the arithmetic downmix have separate extrema: averaging channel extrema would destroy phase cancellation. The versioned WVF2 payload carries both. The compact whole-track overview remains a separate background job with a persistent cache, partial-coverage metadata, and peak-preserving reduction.

## Qt rendering and visibility

Each spectrogram item owns its raw ring and rendered canvas under its state mutex. Matching panes share immutable frequency-bin projection ranges and color lookup tables through weak references. Render passes retain their projection; image updates preserve previously retained `QImage` copies. This shares reusable rendering calculations without introducing a second global owner for mutable timeline state.

`BridgeClient` coalesces item/window visibility changes and enables spectral analysis when at least one registered item is visible in a visible, non-minimized window. Hiding all views stops decoding, cancels staging, and advances the generation. Transport and track changes still update analysis state. Showing a view starts a fresh generation at the latest position, including a viewport of rolling history. The non-Qt analysis API defaults to enabled.

A hidden pane skips animation ticks and canvas writes while retaining batches needed by another visible pane. Showing it invalidates the canvas. Hidden waveform surfaces clear their detail source, cancelling detail queries; the one-off overview cache job remains independent.

## Validation and profiling

Run the checks in [DEVELOPMENT.md](DEVELOPMENT.md). Regression fixtures are synthetic or embedded and require no personal media or network access. Tests cover rolling and centered seeks, codec preroll, packet boundaries, timestamp gaps, track transitions, EOF, cancellation, stale generations, memory budgets, and Rust/Qt waveform payloads.

The optional [spectrogram benchmark](DEVELOPMENT.md#profiling) measures CPU canvas rebuilding with resident data. Its image checksums permit output comparisons. It does not measure decoder throughput, transport latency, GPU uploads, or end-to-end frame pacing; those require an instrumented application run on the target hardware.
