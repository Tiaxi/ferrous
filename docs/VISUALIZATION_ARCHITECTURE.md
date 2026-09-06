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

## Channel positions

Channel positions are probed on the metadata worker using the same decoder selection as visualization analysis. The metadata protocol carries labels in native PCM channel order. Recognized speaker sets restore the status layout name and icon; unknown layouts retain the channel count. Waveform and spectrogram labels use the reported positions, falling back to numbers for unknown surround positions. Late layout metadata updates spectrogram text without replacing panes or clearing their history. The level meter remains unlabelled.

## Level meter

The horizontal meter occupies a fixed 20 logical pixels below the embedded visualization. Settings → Display → “Show level meter below the main visualization” controls its visibility and is persisted as `show_level_meter` (enabled by default). Both fullscreen viewer modes always hide the meter and give its space to the visualization. Hiding the meter clears its source and cancels pending queries. In the main view, its space comes from the playlist's initial preferred height; disabling it returns that space to the playlist. Stereo uses two 9-pixel bars with a 2-pixel gap; additional channels divide the same fixed height without labels.

The meter reads source levels before listening volume and channel mute/solo. It reuses the waveform service through the existing WVF2 interface, requesting at most 2,048 extrema points over two seconds on a Qt worker thread. One request may be in flight, with one current window and one staged replacement. Cancellation and generation checks discard results after seeks, track changes, pause, or hiding the view; failed reads retry after a delay. Only bins whose end has reached the transport playhead contribute to the display.

The scale runs from −60 to 0 dBFS with a subdued green/yellow/orange gradient. Each channel has immediate attack, 60 dB/s release (full scale to the floor in one second), and an independent one-physical-pixel-wide peak marker held for 1.5 seconds before falling at 12 dB/s. Elapsed-time ballistics include each bin's age so changing display refresh rate does not change hold or decay. The scene graph retains one shared gradient texture and updates bar and marker geometry on window presentation, with no fixed FPS timer. Pause lets the bars and held peaks decay; stop and transport discontinuities clear them.

## Qt rendering and visibility

Each spectrogram item owns its raw ring and rendered canvas under its state mutex. Matching panes share immutable frequency-bin projection ranges and color lookup tables through weak references. Render passes retain their projection; image updates preserve previously retained `QImage` copies. This shares reusable rendering calculations without introducing a second global owner for mutable timeline state.

`BridgeClient` coalesces item/window visibility changes and enables spectral analysis when at least one registered item is visible in a visible, non-minimized window. Hiding all views stops decoding, cancels staging, and advances the generation. Transport and track changes still update analysis state. Showing a view starts a fresh generation at the latest position, including a viewport of rolling history. The non-Qt analysis API defaults to enabled.

A hidden pane skips animation ticks and canvas writes while retaining batches needed by another visible pane. Showing it invalidates the canvas. Hidden waveform surfaces clear their detail source, cancelling detail queries; the one-off overview cache job remains independent.

## Validation and profiling

Run the checks in [DEVELOPMENT.md](DEVELOPMENT.md). Regression fixtures are synthetic or embedded and require no personal media or network access. Tests cover rolling and centered seeks, codec preroll, packet boundaries, timestamp gaps, track transitions, EOF, cancellation, stale generations, memory budgets, and Rust/Qt waveform payloads.

The optional [spectrogram benchmark](DEVELOPMENT.md#profiling) measures CPU canvas rebuilding with resident data. Its image checksums permit output comparisons. It does not measure decoder throughput, transport latency, GPU uploads, or end-to-end frame pacing; those require an instrumented application run on the target hardware.
