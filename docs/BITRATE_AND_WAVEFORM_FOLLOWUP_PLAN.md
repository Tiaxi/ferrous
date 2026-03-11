# Bitrate And Waveform Follow-Up Plan

## Context

Ferrous had two related playback-analysis problems:

- showing a true rolling compressed bitrate without causing heavy network-share I/O on track change
- reducing first-build waveform cost for high-sample-rate files such as `24-bit / 96 kHz` FLAC

The old rolling bitrate path came from a full-file Symphonia packet walk in metadata loading. That produced a real per-second compressed bitrate timeline, but it also caused large synchronous reads on mounted network shares.

The waveform path already caches results, but first-build cost is still noticeable for high-rate files because Ferrous decodes and samples the whole file during cache generation.

## Findings

### Current playback architecture

- Playback is built around `playbin` in `src/playback/mod.rs`.
- Ferrous's custom analysis tap is attached after decode/resample and only sees PCM.
- That means the current playback pipeline does not expose compressed packet sizes/timestamps directly.

### Current metadata bitrate path

- The old `probe_stream_details()` path in `src/metadata/mod.rs` can compute a true per-second compressed bitrate timeline.
- It does that by walking packets across the whole file.
- On network shares that packet walk is too expensive to run synchronously on every track change.

### Current waveform path

- Waveforms are generated once and cached persistently in the analysis cache DB.
- The waveform builder still does full decode on first build.
- For high-rate files, sampling every frame is unnecessary for an overview waveform.

## Current Runtime Decisions

### Bitrate

- Ferrous currently uses static average bitrate as the safe default.
- Playback-time bitrate estimators based on the decoded side of the pipeline were rejected because they reported PCM-rate values, not compressed bitrate.
- GStreamer tag-message bitrate updates were also rejected as the main solution because they only produced startup/nominal values, not true rolling compressed bitrate.

### Waveform

- Ferrous now widens waveform sampling stride for exact `48 kHz` / `44.1 kHz` divisors only:
  - `96 kHz -> 48 kHz`
  - `88.2 kHz -> 44.1 kHz`
  - `192 kHz -> 48 kHz`
  - etc.
- Files at or below `48 kHz` are left untouched.

## Available Approaches For Rolling Compressed Bitrate

### 1. Keep Static Average Bitrate

Rationale:

- Fastest and simplest.
- No extra I/O during playback.
- No codec-specific work.

Pros:

- Zero additional network-share pressure.
- Minimal complexity.
- Works consistently across formats.

Cons:

- No rolling bitrate.
- Less informative for VBR/lossy formats.

### 2. Background Cached Bitrate Timeline

Rationale:

- Preserve true rolling compressed bitrate without synchronous track-change cost.
- Shift the full-file packet walk off the hot path and persist the result by `path + size + mtime`.

How it would work:

- show static average bitrate immediately
- schedule a low-priority background job to build per-second bitrate timeline
- persist the timeline in a cache table similar to waveform caching
- use cached timeline on later plays, and optionally update the current play once the job completes

Pros:

- True rolling compressed bitrate once cached.
- No synchronous network-share burst on track change.
- Reuses the same invalidation model as waveform cache.

Cons:

- First play of an uncached file still has no rolling bitrate immediately.
- Adds cache schema, invalidation, and storage complexity.
- Still requires full-file packet walk once per file version.

### 3. Custom GStreamer Pipeline With Pre-Decode Packet Stats

Rationale:

- A true live solution would tap compressed packets before decode instead of relying on metadata scans.

How it would work:

- replace or partially replace `playbin` with a more explicit pipeline
- attach pad probes or parser-side taps before decode
- derive rolling compressed bitrate from packet bytes over playback time

Pros:

- True live rolling bitrate during first play.
- No separate metadata packet walk required.
- Closest to how classic players and plugin-based players likely solved this.

Cons:

- Most invasive option.
- Loses some simplicity and convenience currently provided by `playbin`.
- Requires more GStreamer plumbing and per-format validation.
- Higher maintenance burden.

### 4. Codec-Specific Lightweight Parsers

Rationale:

- Some formats are much easier to parse for bitrate timeline than others.
- A targeted parser path could avoid a heavy generic solution.

Examples:

- MP3 frame-header parsing
- AAC/ADTS packet parsing
- Ogg/Opus/Vorbis page-level parsing

Pros:

- Can be more efficient than a generic full decoder walk.
- Best effort can focus on the formats where rolling bitrate matters most.

Cons:

- Per-codec implementation burden.
- Harder to keep behavior consistent across formats.
- Container and muxing edge cases increase complexity quickly.

### 5. Hybrid Strategy: Cache Only For Lossy/VBR Formats

Rationale:

- Rolling bitrate is more useful for MP3/AAC/Opus/Vorbis than for FLAC.
- Lossless formats can keep static average bitrate.

Pros:

- Best cost/benefit ratio.
- Avoids spending effort where rolling bitrate adds little value.
- Keeps FLAC/network-share path cheap.

Cons:

- Different behavior by format.
- More policy logic and user expectations to manage.

## Available Approaches For Waveform Cost Reduction

### 1. Keep Full Decode, Keep Exactness

Rationale:

- Exact waveform requires decoding the full file.

Pros:

- Highest fidelity.
- Simplest to reason about.

Cons:

- Slowest first-build path.
- Most noticeable on high-rate multichannel or lossless files.

### 2. Current High-Rate Sampling Reduction

Rationale:

- Overview waveforms do not need all high-rate samples.
- If the source rate divides cleanly to `48 kHz` or `44.1 kHz`, widened sampling gives cheaper first-build waveform generation with limited visual impact.

Pros:

- Simple.
- Reduces work for common high-rate masters.
- Leaves `44.1/48 kHz` and lower unchanged.

Cons:

- Still an approximation.
- Not peak-preserving across skipped samples.
- Does not reduce decode cost itself, only waveform sampling work.

### 3. Block-Peak Decimation To Target Rate

Rationale:

- Better approximation than taking every Nth sample.
- Collapse each N-sample block to its max absolute amplitude before feeding the accumulator.

Pros:

- Less likely to miss short transients.
- Better visual fidelity than naïve sample skipping.

Cons:

- More CPU than simple skipping.
- Still approximate unless aligned carefully with final bucket boundaries.

### 4. Lower Target Rate Such As `24 kHz` / `22.05 kHz`

Rationale:

- The final seekbar waveform is heavily reduced anyway, so a lower effective rate may still be visually sufficient.

Pros:

- Larger reduction in first-build waveform work.
- Likely still acceptable for overview waveforms.

Cons:

- Bigger fidelity tradeoff.
- More likely to smooth or miss brief transient peaks.

### 5. Build Waveform Progressively From Playback PCM

Rationale:

- Playback already decodes PCM.
- Waveform could fill in opportunistically as the user listens.

Pros:

- No extra first-play decode pass.
- Extremely cheap during later playback.

Cons:

- Incomplete waveform until enough of the track has been played.
- Poor fit for immediate full-track waveform expectations.
- More state complexity.

### 6. Format-Specific Waveform Shortcuts

Rationale:

- Some codecs may expose frame-level amplitude or easier packet structures.

Pros:

- Potentially much faster for specific formats.

Cons:

- High implementation complexity.
- Format-specific maintenance burden.
- Hard to generalize cleanly.

## Recommendations

### Bitrate

Recommended near-term direction:

- keep static average bitrate as the default runtime behavior
- if rolling bitrate is revisited later, implement a cached bitrate timeline
- only consider a custom GStreamer pipeline if true first-play live rolling bitrate becomes a priority worth architectural cost

Recommended scope if cache is implemented:

- prioritize lossy/VBR formats first
- leave FLAC/lossless on static average unless there is a strong product need

### Waveform

### Reassessment After Inspecting Current Implementation

The current implementation makes one important tradeoff explicit:

- on waveform-cache miss, Ferrous immediately launches a dedicated full-file waveform decode job on track change
- that job opens the file separately and walks packets until EOF
- the current sample-stride reduction happens after decode, so it reduces PCM-side work but does not materially reduce network-share read volume

That means the current near-term recommendation in this document is too narrow for the specific pain point of large surround FLACs on network shares. The main bottleneck is the extra first-play read pass, not just the per-sample waveform math.

### Recommended Direction

Recommended default behavior:

- stop doing eager full-file linear waveform generation on cache miss during track change
- build a fast whole-track preview waveform by sparsely probing the file at seek intervals
- keep the persistent waveform cache, and optionally refine the preview later if a full-quality offline build is still desired

Why this is the best mitigation:

- it keeps the core product promise: a mostly complete full-track waveform is available early enough to guide seeking
- it avoids the current worst case of reading the entire file sequentially on first play
- on FLAC specifically, Ferrous's Symphonia stack already parses `SEEKTABLE` metadata and exposes coarse seek support, which makes sparse sampling a much better fit than a full linear pass
- the amount of decoded audio can be bounded to a tiny window per probe instead of the entire track

Tradeoff:

- the first-pass waveform is approximate rather than exact
- some formats or files without good seek indexes may need fallback behavior
- sparse seeks trade bandwidth for latency, so probe count needs to stay intentionally low

### Recommended Implementation Order

1. Replace the current cache-miss full scan with a sparse preview builder.
   - If a cached waveform exists, use it immediately.
   - If not, build a low-resolution whole-track preview instead of a full-file decode.

2. Implement seek-sampled probing for seek-friendly formats first.
   - For FLAC, use Symphonia seeks against target timestamps spread across the track.
   - Decode only a short window around each target point and collapse it to one bin or a small number of bins.
   - Aggregate peaks across all channels, not just the first channel.

3. Persist the preview waveform as its own cache product.
   - Treat it as good enough for seek guidance.
   - Optionally tag cache rows by quality level if Ferrous later adds a refined builder.

4. Keep refinement optional and off the hot path.
   - If a more exact waveform is still wanted, run that only when playback is idle, on local files, or behind an explicit setting.
   - Do not make exact refinement a requirement for first-play usability.

5. Use playback-derived accumulation only as a secondary refinement path.
   - Playback PCM can still help fill or validate cache quality later.
   - It should not be the only uncached-path strategy if immediate seek guidance is required.

### Secondary Speedups If An Offline Builder Remains

If Ferrous keeps a separate waveform builder for non-default/background use, the next improvements should focus on fidelity-per-cost rather than assuming they solve the network problem:

- replace first-sample skipping with block-peak decimation
- collapse channels by max absolute amplitude instead of sampling only the first channel
- optionally test lower fixed analysis targets such as `24 kHz` / `22.05 kHz`

These changes can make the builder faster and more representative, especially for multichannel material, but they do not remove the fundamental extra-read cost on network shares.
