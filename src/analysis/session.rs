// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc, Mutex, PoisonError,
};
use std::time::Duration;

use crossbeam_channel::{unbounded, Receiver, Sender};

use super::cache::WaveformSourceStamp;
use super::decoders::{
    open_audio_file, u64_to_u32_saturating, AudioFrameSource, AudioFrames, AudioRead, AudioTimeline,
};
use super::fft::{PeakHoldResampler, StftComputer};
use super::output::{AnalysisEventOutput, AnalysisOutputs};
use super::quantize::append_quantized_row;
use super::retention::{lookahead_seconds, SpectrogramBufferConfig};
use super::track_io::{source_stamp, TrackFormat};
use super::{
    f64_to_u64_saturating, usize_to_u64, AnalysisEvent, PrecomputedSpectrogramChunk,
    SpectrogramDisplayMode, SpectrogramViewMode, REFERENCE_HOP,
};
use crate::playback::{remap_path, PathRenames};
use crate::worker_thread::{spawn_joinable, JoinableThread};

#[cfg(feature = "profiling-logs")]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {
        eprintln!($($arg)*);
    };
}

#[cfg(not(feature = "profiling-logs"))]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {};
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Keep default 2048-FFT stereo centered chunks at <=64 columns, about 128 KiB.
const SPECTROGRAM_CHUNK_BYTE_BUDGET: usize = 64 * 1_025 * 2;

// Gapless staging is a prefix, not a whole-track cache. Its receiver is not
// consumed until handoff, so channel backpressure would prevent cancellation.
const CENTERED_STAGING_BYTE_BUDGET: usize = 8 * 1024 * 1024;

fn staging_column_limit(bins: usize, channels: usize) -> u64 {
    usize_to_u64(CENTERED_STAGING_BYTE_BUDGET / bins.saturating_mul(channels).max(1))
}

fn clamp_to_u8(v: usize) -> u8 {
    u8::try_from(v).unwrap_or(u8::MAX)
}

fn clamp_to_u16(v: usize) -> u16 {
    u16::try_from(v).unwrap_or(u16::MAX)
}

/// Saturating conversion from `f64` to `u32`, clamping to `[0, u32::MAX]`.
#[allow(dead_code)]
fn f64_to_u32_saturating(v: f64) -> u32 {
    if v <= 0.0 {
        return 0;
    }
    if v >= f64::from(u32::MAX) {
        return u32::MAX;
    }
    // Value is within u32 range after clamping above.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let r = v as u32;
    r
}

/// Lossless `usize`-to-`f64` on platforms where `usize` may be wider than
/// `f64`'s 52-bit mantissa. Precision loss is acceptable for column-index
/// arithmetic that only needs approximate floating-point division.
fn usize_to_f64_approx(v: usize) -> f64 {
    // On 64-bit targets clippy warns about precision loss; suppress because
    // this is used for spectrogram column estimation where exact precision
    // is unnecessary.
    #[allow(clippy::cast_precision_loss)]
    let r = v as f64;
    r
}

fn output_interval_for_hop(hop: usize, zoom_level: f32) -> f64 {
    if hop == 0 {
        return 1.0;
    }
    // Continuous fractional interval: target_effective_hop / hop.
    // Round to the sample hop advertised over the bridge, then retain the
    // fractional row interval. This prevents long-track timing drift.
    // At zoom=0.8 with hop=1024, the interval remains 1.25 (not 1).
    let zoom = f64::from(zoom_level.clamp(0.001, 1.0));
    // REFERENCE_HOP and hop are small compile-time/session constants;
    // precision loss from usize→f64 is negligible.
    #[allow(clippy::cast_precision_loss)]
    let target_effective_hop = (REFERENCE_HOP as f64 / zoom).round();
    #[allow(clippy::cast_precision_loss)]
    let interval = target_effective_hop / hop as f64;
    interval.max(1.0)
}

/// The output row interval for an STFT hop at `zoom_level`.
fn output_interval_for_zoom(hop_size: usize, zoom_level: f32) -> f64 {
    if zoom_level > 1.0 {
        1.0 // Bypass decimation -- keep all STFT rows for fine temporal resolution
    } else {
        output_interval_for_hop(hop_size, zoom_level)
    }
}

/// The column hop that a session with STFT hop `hop_size` reports at
/// `zoom_level`.
pub(super) fn effective_hop_for_zoom(hop_size: usize, zoom_level: f32) -> usize {
    effective_hop_from_interval(hop_size, output_interval_for_zoom(hop_size, zoom_level))
}

/// Locate the first input FFT window contributing to an absolute output column.
fn stft_seek_grid(
    position_seconds: f64,
    rate: u32,
    hop: usize,
    effective_hop: usize,
) -> (u64, u64) {
    let column = f64_to_u64_saturating(
        position_seconds * f64::from(rate) / usize_to_f64_approx(effective_hop),
    );
    let output_frame = column.saturating_mul(usize_to_u64(effective_hop));
    let input_hop = usize_to_u64(hop.max(1));
    (column, output_frame / input_hop * input_hop)
}

/// Where to start reading audio for an STFT grid whose first window belongs
/// to the column at `grid_frame`. Each window is centred on its column's
/// time, so that a sound shows in the column of the time it plays, and
/// starts half an FFT before it; before the start of the file it reads
/// silence. Returns the frame to read from and the frames of silence that
/// come first.
fn centred_window_start(grid_frame: u64, fft_size: usize) -> (u64, usize) {
    let half = usize_to_u64(fft_size / 2);
    let start = grid_frame.saturating_sub(half);
    let silence = half - (grid_frame - start);
    (start, usize::try_from(silence).unwrap_or(0))
}

/// Feed `frames` of silence to each STFT before its audio; see
/// [`centred_window_start`].
fn enqueue_leading_silence(stfts: &mut [StftComputer], frames: usize) {
    if frames == 0 {
        return;
    }
    for stft in stfts {
        stft.enqueue_with(frames, |pending, first| {
            pending.resize(pending.len() + frames - first, 0.0);
        });
    }
}

/// The most audio an STFT may hold beyond one window: a decoded batch, of
/// which the decode loops drain every row before reading the next.
const MAX_PENDING_FRAMES: usize = 1 << 20;

/// Feed a decoded batch to the per-channel STFTs, copying it straight into
/// their queues. Every STFT gets one sample per frame, whatever the batch's
/// channel count, so their windows stay in step; see
/// [`AudioFrames::extend_analysis_channel`]. Fails if an STFT holds more
/// audio than a batch, which would grow without producing columns.
fn enqueue_audio(
    stfts: &mut [StftComputer],
    audio: &AudioFrames,
    view_mode: SpectrogramViewMode,
) -> Result<(), String> {
    let frames = audio.analysis_frames();
    for (channel, stft) in stfts.iter_mut().enumerate() {
        stft.enqueue_with(frames, |pending, first| {
            audio.extend_analysis_channel(pending, channel, view_mode, first);
        });
        if stft.pending_len() > stft.fft_size() + MAX_PENDING_FRAMES {
            return Err(format!(
                "its analysis fell behind the decoded audio ({} frames queued)",
                stft.pending_len()
            ));
        }
    }
    Ok(())
}

/// The total-columns estimate of a file continued at column `start_column`
/// of a session: the file's own estimate, `total_columns` reference-hop
/// columns, after the columns before it.
fn continued_columns_estimate(start_column: u64, total_columns: u32, effective_hop: usize) -> u32 {
    let file_columns = u64::from(scale_columns_estimate(total_columns, effective_hop));
    u64_to_u32_saturating(start_column.saturating_add(file_columns))
}

/// Compute the effective hop size from a fractional output interval.
/// `hop_size` is a small positive usize; precision loss is negligible.
fn effective_hop_from_interval(hop_size: usize, output_interval: f64) -> usize {
    #[allow(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        clippy::cast_precision_loss
    )]
    let hop = (hop_size as f64 * output_interval).round() as usize;
    hop
}

/// Rescale a base `total_columns` estimate for a given `effective_hop`.
/// When the effective hop differs from `REFERENCE_HOP`, the output
/// column count changes proportionally.
fn scale_columns_estimate(total_columns: u32, effective_hop: usize) -> u32 {
    if effective_hop != REFERENCE_HOP && effective_hop > 0 {
        // REFERENCE_HOP and effective_hop are small constants; precision loss is negligible.
        #[allow(clippy::cast_precision_loss)]
        let ratio = REFERENCE_HOP as f64 / effective_hop as f64;
        let scaled = (f64::from(total_columns) * ratio).ceil();
        // Ceil result is non-negative and clamped via try_from fallback.
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let val = scaled as u64;
        u32::try_from(val).unwrap_or(u32::MAX)
    } else {
        total_columns
    }
}

fn seconds_from_frames(frames: u64, sample_rate_hz: u64) -> f32 {
    if sample_rate_hz == 0 {
        return 0.0;
    }

    let secs = frames / sample_rate_hz;
    let remainder = frames % sample_rate_hz;
    let nanos = (u128::from(remainder) * 1_000_000_000) / u128::from(sample_rate_hz);
    let nanos = u32::try_from(nanos).unwrap_or(u32::MAX);
    Duration::new(secs, nanos).as_secs_f32()
}

// ---------------------------------------------------------------------------
// SpectrogramWorkerCommand / SpectrogramWorkerHandles
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub(super) enum SpectrogramWorkerCommand {
    NewTrack {
        track_token: u64,
        generation: u64,
        path: PathBuf,
        fft_size: usize,
        hop_size: usize,
        zoom_level: f32,
        widget_width: u32,
        channel_count: usize,
        start_seconds: f64,
        target_position_seconds: f64,
        emit_initial_reset: bool,
        clear_history_on_reset: bool,
        view_mode: SpectrogramViewMode,
        display_mode: SpectrogramDisplayMode,
        handoff: SessionHandoff,
    },
    PositionUpdate {
        position_seconds: f64,
    },
    Seek {
        position_seconds: f64,
    },
    /// Continue the current decode session with a new file.  Preserves
    /// STFT, decimator, rate-limiter, and column counter state so the
    /// spectrogram scrolls seamlessly across a gapless track boundary.
    ContinueWithFile {
        path: PathBuf,
        track_token: u64,
    },
    /// Update the track token on the running session.  If a
    /// `pending_continue` exists (continuation not yet consumed at EOF),
    /// only the pending token is updated so old-track tail columns keep
    /// the old token.  If the continuation was already consumed, the
    /// session token is updated directly.
    UpdateTrackToken {
        track_token: u64,
    },
    /// Clear any pending continuation that has not yet been consumed at
    /// EOF.  Sent when the gapless prediction is cancelled.
    CancelPendingContinue,
    /// Grow the running session's widget-width basis so the centered-mode
    /// lookahead park threshold covers a newly-enlarged display (e.g.
    /// fullscreen toggle without session restart).  The worker updates
    /// `session.widget_width` and recomputes `session.lookahead_columns`
    /// so the decoder produces enough cols to fill the bigger window.
    UpdateWidgetWidth {
        widget_width: u32,
    },
    SetDisplayMode(SpectrogramDisplayMode),
    /// Release the current decoder and wait for another session. The worker
    /// itself lives until its command channel disconnects.
    StopSession,
    /// Files were renamed on disk. A continuation or a session that the
    /// worker has not opened yet opens its file at the new path; the file
    /// it reads stays open.
    RemapPaths(Arc<PathRenames>),
}

/// Output the analysis thread hands to the worker with a session, so that
/// the worker is the only writer of the spectral stream: what the UI
/// receives stays in order, and nothing waits on the analysis thread.
#[derive(Debug, Clone, Default)]
pub(super) struct SessionHandoff {
    /// Before switching, publish the decoded extent of the session this one
    /// replaces, for its track: a centered gapless switch shows the new
    /// track's columns next.
    pub(super) finalize_previous: bool,
    /// Chunks to emit before the session opens its file. They carry the
    /// session's track token and generation.
    pub(super) prelude: Vec<PrecomputedSpectrogramChunk>,
}

/// What the worker tells the analysis thread about its sessions: the
/// format of each file it opens, read from the decoder rather than probed,
/// the continuations it cannot make, and the files it cannot read.
#[derive(Debug, Clone, PartialEq)]
pub(super) enum SessionReport {
    /// The session of `generation` opened `path`, for `track_token`.
    Opened {
        generation: u64,
        track_token: u64,
        path: PathBuf,
        stamp: Option<WaveformSourceStamp>,
        format: TrackFormat,
    },
    /// The worker did not continue its timeline with `path`, and nothing
    /// decodes the file: its `format` does not fit the session, the file
    /// cannot be opened (`None`), or no session is running.
    ContinuationRejected {
        generation: u64,
        path: PathBuf,
        stamp: Option<WaveformSourceStamp>,
        format: Option<TrackFormat>,
    },
    /// The session could not read the audio of `path`, which it could
    /// open, and ended after resetting the spectral stream.  A file that
    /// cannot be opened at all is not reported: playback reports it.
    Failed {
        generation: u64,
        track_token: u64,
        path: PathBuf,
        error: String,
    },
}

impl SessionReport {
    pub(super) fn generation(&self) -> u64 {
        match self {
            Self::Opened { generation, .. }
            | Self::ContinuationRejected { generation, .. }
            | Self::Failed { generation, .. } => *generation,
        }
    }
}

/// Point a session that has not started at its file's new path.
fn remap_new_track_path(cmd: &mut SpectrogramWorkerCommand, renames: &PathRenames) {
    if let SpectrogramWorkerCommand::NewTrack { path, .. } = cmd {
        remap_path(path, renames);
    }
}

/// Point a continuation that has not been opened at its file's new path.
fn remap_pending_continue(session: &mut SpectrogramSessionState, renames: &PathRenames) {
    if let Some((path, _)) = session.pending_continue.as_mut() {
        remap_path(path, renames);
    }
}

pub(super) struct SpectrogramWorkerHandles {
    pub(super) cmd_tx: Sender<SpectrogramWorkerCommand>,
    pub(super) decode_generation: Arc<AtomicU64>,
    pub(super) columns_produced: Arc<AtomicU64>,
    /// Track duration (milliseconds) published by the active worker
    /// session after its file probe.  0 = unknown.  Read by the analysis
    /// thread to model the centered display's visible window (full-track
    /// lock / EOF detach) when choosing session decode windows.
    pub(super) track_duration_ms: Arc<AtomicU64>,
    /// Whether the worker is still there to take commands.
    pub(super) status: SpectrogramWorkerStatus,
    /// What the worker reports about its sessions.
    pub(super) reports: Receiver<SessionReport>,
}

// ---------------------------------------------------------------------------
// Staging chunk accumulation
// ---------------------------------------------------------------------------

/// State for the staging decode thread's chunk accumulation.
/// Bundles both mutable accumulation state and immutable session
/// parameters so that helper functions stay under the argument limit.
struct StagingChunkState {
    // Immutable session parameters.
    fft_size: usize,
    bins_per_column: usize,
    channel_count: usize,
    effective_rate: u32,
    effective_hop: usize,
    total_columns_estimate: u32,
    max_columns: u64,
    // Mutable accumulation state.
    columns_produced: u64,
    total_covered_samples: u64,
    chunk_buf: Vec<u8>,
    chunk_columns: u16,
    chunk_start_index: u64,
    target_chunk_columns: u16,
}

impl StagingChunkState {
    /// Build a partial-flush chunk from the current accumulation state
    /// without resetting counters.  Returns `None` if no columns are
    /// buffered.
    fn take_partial_chunk(&mut self) -> Option<PrecomputedSpectrogramChunk> {
        if self.chunk_columns == 0 {
            return None;
        }
        let coverage =
            seconds_from_frames(self.total_covered_samples, u64::from(self.effective_rate));
        Some(PrecomputedSpectrogramChunk {
            track_token: 0,
            generation: 0,
            columns_u8: std::mem::take(&mut self.chunk_buf),
            bins_per_column: clamp_to_u16(self.bins_per_column),
            column_count: self.chunk_columns,
            channel_count: clamp_to_u8(self.channel_count),
            start_column_index: u64_to_u32_saturating(self.chunk_start_index),
            total_columns_estimate: self.total_columns_estimate,
            sample_rate_hz: self.effective_rate,
            hop_size: clamp_to_u16(self.effective_hop),
            coverage_seconds: coverage,
            complete: false,
            buffer_reset: false,
            clear_history: false,
        })
    }
}

// ---------------------------------------------------------------------------
// Staging decode (centered-mode gapless pre-decode)
// ---------------------------------------------------------------------------

/// Consume one synchronized row across channels directly into the byte batch.
/// Returns None when more audio is needed, or whether resampling emitted a column.
fn append_stft_column(
    stfts: &mut [StftComputer],
    resamplers: &mut [PeakHoldResampler],
    chunk: &mut Vec<u8>,
    bins: usize,
    fft_size: usize,
) -> Option<bool> {
    if stfts.is_empty() || !stfts.iter().all(StftComputer::has_row) {
        return None;
    }
    let start = chunk.len();
    let mut emitted = true;
    for (channel, stft) in stfts.iter_mut().enumerate() {
        let row = stft.take_row().expect("all channel windows checked above");
        let output = resamplers
            .get_mut(channel)
            .map_or(Some(row), |resampler| resampler.push(row));
        if let Some(output) = output {
            append_quantized_row(chunk, output, bins, fft_size);
        } else {
            emitted = false;
        }
    }
    if !emitted {
        chunk.truncate(start);
    }
    Some(emitted)
}

/// Drain STFT rows from the staging pipeline, quantize, and emit
/// chunks via the channel.  Extracted from `centered_staging_decode`
/// to stay within clippy's line limit.
/// Returns `false` once the prefix budget is exhausted or the receiver drops.
fn staging_drain_stft_rows(
    stfts: &mut [StftComputer],
    resamplers: &mut [PeakHoldResampler],
    state: &mut StagingChunkState,
    tx: &Sender<PrecomputedSpectrogramChunk>,
) -> bool {
    if state.columns_produced >= state.max_columns {
        return false;
    }
    while let Some(emitted) = append_stft_column(
        stfts,
        resamplers,
        &mut state.chunk_buf,
        state.bins_per_column,
        state.fft_size,
    ) {
        if !emitted {
            continue;
        }
        state.chunk_columns += 1;
        state.columns_produced += 1;

        let prefix_complete = state.columns_produced >= state.max_columns;
        if state.chunk_columns >= state.target_chunk_columns || prefix_complete {
            let coverage =
                seconds_from_frames(state.total_covered_samples, u64::from(state.effective_rate));
            let chunk = PrecomputedSpectrogramChunk {
                track_token: 0,
                generation: 0,
                columns_u8: std::mem::take(&mut state.chunk_buf),
                bins_per_column: clamp_to_u16(state.bins_per_column),
                column_count: state.chunk_columns,
                channel_count: clamp_to_u8(state.channel_count),
                start_column_index: u64_to_u32_saturating(state.chunk_start_index),
                total_columns_estimate: state.total_columns_estimate,
                sample_rate_hz: state.effective_rate,
                hop_size: clamp_to_u16(state.effective_hop),
                coverage_seconds: coverage,
                complete: false,
                buffer_reset: false,
                clear_history: false,
            };
            if tx.send(chunk).is_err() {
                return false; // receiver dropped
            }
            state.chunk_start_index = state.columns_produced;
            state.chunk_columns = 0;
            state.target_chunk_columns = next_target_chunk_columns_for_payload(
                state.target_chunk_columns,
                SpectrogramDisplayMode::Centered,
                state.bins_per_column,
                state.channel_count,
            );
        }
        if prefix_complete {
            return false;
        }
    }
    true // ok, continue decoding
}

fn centered_staging_decode(
    path: &Path,
    fft_size: usize,
    hop_size: usize,
    zoom_level: f32,
    view_mode: SpectrogramViewMode,
    stop: &AtomicBool,
    tx: &Sender<PrecomputedSpectrogramChunk>,
) {
    if stop.load(Ordering::Relaxed) {
        return;
    }
    let Some((mut source, native_sample_rate, native_channels, total_columns)) =
        open_audio_file(path)
    else {
        profile_eprintln!("[staging] failed to open {}", path.display());
        return;
    };

    let effective_rate = u32::try_from(native_sample_rate).unwrap_or(48_000);
    let channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let bins_per_column = (fft_size / 2) + 1;
    let output_interval = output_interval_for_zoom(hop_size, zoom_level);

    let mut stfts: Vec<StftComputer> = (0..channel_count)
        .map(|_| StftComputer::new(fft_size, hop_size))
        .collect();
    let mut resamplers: Vec<PeakHoldResampler> = (0..channel_count)
        .map(|_| PeakHoldResampler::new(output_interval))
        .collect();

    let effective_hop = effective_hop_from_interval(hop_size, output_interval);
    let total_columns_estimate = scale_columns_estimate(total_columns, effective_hop);

    let mut state = StagingChunkState {
        fft_size,
        bins_per_column,
        channel_count,
        effective_rate,
        effective_hop,
        total_columns_estimate,
        max_columns: staging_column_limit(bins_per_column, channel_count),
        columns_produced: 0,
        total_covered_samples: 0,
        chunk_buf: Vec::new(),
        chunk_columns: 0,
        chunk_start_index: 0,
        target_chunk_columns: 1,
    };

    profile_eprintln!(
        "[staging] started for {} sr={native_sample_rate} ch={native_channels}",
        path.file_name().unwrap_or_default().to_string_lossy(),
    );

    let (_, leading_silence) = centred_window_start(0, fft_size);
    enqueue_leading_silence(&mut stfts, leading_silence);
    let mut timeline = AudioTimeline::new(0);
    loop {
        if stop.load(Ordering::Relaxed) {
            // Flush any partial chunk so the drain captures as much data as possible.
            if let Some(chunk) = state.take_partial_chunk() {
                let _ = tx.send(chunk);
            }
            profile_eprintln!("[staging] stopped after {} columns", state.columns_produced);
            return;
        }

        let audio = match timeline.read(|| source.next_frames()) {
            Ok(AudioRead::Pending) => continue,
            Ok(AudioRead::Frames(audio)) => audio,
            Ok(AudioRead::Eof) => break,
            Err(error) => {
                eprintln!("[analysis] staging decode failed: {error}");
                return;
            }
        };

        state.total_covered_samples += usize_to_u64(audio.frames);
        if let Err(error) = enqueue_audio(&mut stfts, &audio, view_mode) {
            eprintln!("[analysis] staging decode failed: {error}");
            return;
        }

        if !staging_drain_stft_rows(&mut stfts, &mut resamplers, &mut state, tx) {
            return; // prefix budget exhausted or receiver dropped
        }
    }

    // Flush remaining partial chunk at EOF.
    if let Some(chunk) = state.take_partial_chunk() {
        let _ = tx.send(chunk);
    }

    profile_eprintln!(
        "[staging] completed {} columns for {}",
        state.columns_produced,
        path.file_name().unwrap_or_default().to_string_lossy(),
    );
}

/// Spawn a short-lived staging thread that pre-decodes a track for
/// centered-mode gapless.  Produces `PrecomputedSpectrogramChunk`s
/// with 0-based column indices, and `track_token: 0` and `generation: 0`
/// as placeholders for the session that shows them.
/// Returns the chunk receiver and the thread, or `None` if it cannot start.
///
/// The thread holds its chunk sender until it ends, so the receiver
/// disconnects when it does. `open_gate`, when given, is waited on before
/// the file is opened.
pub(super) fn spawn_centered_staging_worker(
    path: PathBuf,
    fft_size: usize,
    hop_size: usize,
    zoom_level: f32,
    view_mode: SpectrogramViewMode,
    stop: Arc<AtomicBool>,
    open_gate: Option<Receiver<()>>,
) -> Option<(Receiver<PrecomputedSpectrogramChunk>, JoinableThread)> {
    let (tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();
    let thread = crate::worker_thread::spawn_worker("ferrous-spectrogram-staging", move || {
        if let Some(gate) = open_gate {
            let _ = gate.recv();
        }
        centered_staging_decode(&path, fft_size, hop_size, zoom_level, view_mode, &stop, &tx);
    })?;
    Some((rx, thread))
}

// ---------------------------------------------------------------------------
// Spectrogram decode worker
// ---------------------------------------------------------------------------

/// Why the spectrogram decode worker is gone, once it is.
///
/// Commands to a gone worker fail without a word, and the other analysis
/// threads keep the bridge's event channel open, so the bridge cannot see
/// it either; the analysis worker reads this to tell the user.
#[derive(Clone, Default)]
pub(super) struct SpectrogramWorkerStatus(Arc<Mutex<Option<String>>>);

impl SpectrogramWorkerStatus {
    fn stop(&self, reason: String) {
        self.0
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get_or_insert(reason);
    }

    /// Why the worker stopped, if it has.
    pub(super) fn stopped(&self) -> Option<String> {
        self.0
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone()
    }
}

/// Marks the worker stopped when its thread ends, by a panic included.
struct StopOnExit(SpectrogramWorkerStatus);

impl Drop for StopOnExit {
    fn drop(&mut self) {
        self.0.stop("it stopped unexpectedly".to_string());
    }
}

const SPECTROGRAM_THREAD_NAME: &str = "ferrous-spectrogram-decode";

pub(super) fn spawn_spectrogram_decode_worker(
    cmd_rx: Receiver<SpectrogramWorkerCommand>,
    event_tx: AnalysisOutputs,
    generation: Arc<AtomicU64>,
    columns_produced: Arc<AtomicU64>,
    track_duration_ms: Arc<AtomicU64>,
    reports: Sender<SessionReport>,
    status: &SpectrogramWorkerStatus,
) -> Option<JoinableThread> {
    let thread_status = status.clone();
    let work = move || {
        let _stopped = StopOnExit(thread_status);
        spectrogram_worker_loop(
            &cmd_rx,
            &event_tx,
            &generation,
            &columns_produced,
            &track_duration_ms,
            &reports,
        );
    };
    #[cfg(test)]
    let spawned = if spawn_test_hooks::take_spawn_failure() {
        drop(work);
        Err(std::io::Error::other("injected spawn failure"))
    } else {
        spawn_joinable(SPECTROGRAM_THREAD_NAME, work)
    };
    #[cfg(not(test))]
    let spawned = spawn_joinable(SPECTROGRAM_THREAD_NAME, work);
    spawned
        .inspect_err(|error| {
            eprintln!("[ferrous] failed to start the spectrogram decode thread: {error}");
            status.stop(format!("its thread could not start ({error})"));
        })
        .ok()
}

/// Mark `status` stopped the way a spectrogram worker that panics does.
#[cfg(test)]
pub(super) fn stop_spectrogram_worker_for_test(status: &SpectrogramWorkerStatus) {
    let status = status.clone();
    let worker = std::thread::spawn(move || {
        let _stopped = StopOnExit(status);
        panic!("test panic in the spectrogram worker");
    });
    assert!(worker.join().is_err(), "the test worker panics");
}

/// Make starting the spectrogram decode thread fail.
#[cfg(test)]
pub(crate) mod spawn_test_hooks {
    use std::cell::Cell;

    thread_local! {
        static FAIL_NEXT_SPAWN: Cell<bool> = const { Cell::new(false) };
    }

    /// Make the next spectrogram decode thread started on this thread fail
    /// to start.
    pub(crate) fn fail_next_spawn() {
        FAIL_NEXT_SPAWN.with(|cell| cell.set(true));
    }

    pub(super) fn take_spawn_failure() -> bool {
        FAIL_NEXT_SPAWN.with(|cell| cell.replace(false))
    }
}

fn spectrogram_worker_loop(
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &dyn AnalysisEventOutput,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
    track_duration_ms_out: &AtomicU64,
    reports: &Sender<SessionReport>,
) {
    let mut next_cmd: Option<SpectrogramWorkerCommand> = None;
    loop {
        let cmd = match next_cmd.take() {
            Some(cmd) => cmd,
            None => match cmd_rx.recv() {
                Ok(cmd) => cmd,
                Err(_) => break,
            },
        };
        let Some(next) = coalesce_queued_new_track_command(cmd, cmd_rx, generation) else {
            continue;
        };

        match next.cmd {
            SpectrogramWorkerCommand::NewTrack { .. } => {
                next_cmd = run_spectrogram_session(
                    next,
                    cmd_rx,
                    event_tx,
                    generation,
                    columns_produced_out,
                    track_duration_ms_out,
                    reports,
                );
            }
            SpectrogramWorkerCommand::ContinueWithFile { path, .. } => {
                // No session to continue: the analysis thread starts one,
                // as only it creates generations, and positions it sends
                // for a continued timeline would misplace a session that
                // started here.
                let _ = reports.send(SessionReport::ContinuationRejected {
                    generation: generation.load(Ordering::Relaxed),
                    path,
                    stamp: None,
                    format: None,
                });
            }
            _ => {} // UpdateTrackToken, CancelPendingContinue, etc. — stale outside session
        }
    }
}

/// Take the commands queued behind `initial_cmd`, when it starts a session,
/// before the session starts.  A later `NewTrack` supersedes it and the
/// commands queued before that one: the analysis thread built it from its
/// state after them.  A `StopSession` supersedes all of them.  The session
/// that starts applies the others, in order, before it reads the channel;
/// only positions and seeks that later ones supersede are dropped.
fn coalesce_queued_new_track_command(
    initial_cmd: SpectrogramWorkerCommand,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    generation: &AtomicU64,
) -> Option<QueuedCommand> {
    let mut queued = VecDeque::new();
    if !matches!(initial_cmd, SpectrogramWorkerCommand::NewTrack { .. }) {
        return Some(QueuedCommand::alone(initial_cmd));
    }

    let mut latest_new_track = initial_cmd;
    while let Ok(cmd) = cmd_rx.try_recv() {
        match cmd {
            SpectrogramWorkerCommand::NewTrack { .. } => {
                latest_new_track = cmd;
                queued.clear();
            }
            SpectrogramWorkerCommand::StopSession => {
                return Some(QueuedCommand::alone(SpectrogramWorkerCommand::StopSession));
            }
            SpectrogramWorkerCommand::RemapPaths(renames) => {
                // The session opens its file after these renames.  The
                // continuations queued before them follow them too.
                remap_new_track_path(&mut latest_new_track, &renames);
                queue_session_command(&mut queued, SpectrogramWorkerCommand::RemapPaths(renames));
            }
            cmd => queue_session_command(&mut queued, cmd),
        }
    }

    let &SpectrogramWorkerCommand::NewTrack {
        generation: cmd_generation,
        ..
    } = &latest_new_track
    else {
        return Some(QueuedCommand::alone(latest_new_track));
    };
    // The commands queued behind a stale session are stale with it: the
    // session of the current generation starts from the state after them.
    (cmd_generation == generation.load(Ordering::Relaxed)).then_some(QueuedCommand {
        cmd: latest_new_track,
        behind: queued,
    })
}

/// A command for the worker, with the commands queued behind it for the
/// session it starts.
#[derive(Debug)]
struct QueuedCommand {
    cmd: SpectrogramWorkerCommand,
    behind: VecDeque<SpectrogramWorkerCommand>,
}

impl QueuedCommand {
    fn alone(cmd: SpectrogramWorkerCommand) -> Self {
        Self {
            cmd,
            behind: VecDeque::new(),
        }
    }
}

/// Queue `cmd` for a session that has not started, dropping the commands it
/// supersedes, as the session acts only on the latest of them: a position
/// supersedes earlier positions, and a seek earlier positions and seeks.
/// A seek storm so stays one command.
fn queue_session_command(
    queued: &mut VecDeque<SpectrogramWorkerCommand>,
    cmd: SpectrogramWorkerCommand,
) {
    match cmd {
        SpectrogramWorkerCommand::PositionUpdate { .. } => {
            queued.retain(|queued| {
                !matches!(queued, SpectrogramWorkerCommand::PositionUpdate { .. })
            });
        }
        SpectrogramWorkerCommand::Seek { .. } => {
            queued.retain(|queued| {
                !matches!(
                    queued,
                    SpectrogramWorkerCommand::PositionUpdate { .. }
                        | SpectrogramWorkerCommand::Seek { .. }
                )
            });
        }
        _ => {}
    }
    queued.push_back(cmd);
}

// ---------------------------------------------------------------------------
// SpectrogramSessionState
// ---------------------------------------------------------------------------

/// Holds decode state for a spectrogram session.
struct SpectrogramSessionState {
    track_token: u64,
    /// The file being decoded.
    path: PathBuf,
    gen: u64,
    fft_size: usize,
    hop_size: usize,
    effective_hop: usize,
    zoom_level: f32,
    widget_width: u32,
    view_mode: SpectrogramViewMode,
    display_mode: SpectrogramDisplayMode,
    channel_count: usize,
    bins_per_column: usize,
    total_columns_estimate: u32,
    effective_rate: u32,
    cols_per_second: f64,

    // Position tracking
    target_position_seconds: f64,
    /// Suppresses backward-seek detection for one command cycle after a
    /// `ContinueWithFile` file switch.  The offset-adjusted position
    /// can be a few columns behind the reset `session_start_column`
    /// due to timing discrepancy between the last position update and
    /// the decoder's actual position.
    suppress_backward_seek: bool,
    /// Absolute column index of the next column to be produced (monotonic within session).
    columns_produced: u64,
    /// Column index where the current decode segment started (after last seek/reset).
    session_start_column: u64,

    // Chunking / STFT state
    stfts: Vec<StftComputer>,
    resamplers: Vec<PeakHoldResampler>,
    packet_counter: usize,
    chunk_buf: Vec<u8>,
    chunk_columns: u16,
    chunk_start_index: u64,
    target_chunk_columns: u16,
    total_covered_samples: u64,

    // Rate throttling
    /// When and at which column the rolling rate limit started counting.
    /// Unset while the post-reset window decodes unthrottled, and set when
    /// the window ends, so the columns the window decoded ahead do not
    /// count against the realtime budget.
    throttle_origin: Option<(std::time::Instant, u64)>,
    /// Number of columns to decode immediately after a reset/handoff before
    /// re-enabling rolling-mode throttling and lookahead parking.
    post_reset_unthrottled_columns: u32,
    decode_rate_limit: f64,
    lookahead_columns: u64,
    /// Seconds-of-audio component of the lookahead park cap.  Stored so
    /// `UpdateWidgetWidth` can recompute `lookahead_columns` without
    /// re-reading the env var.
    lookahead_seconds: f64,
    /// True while parked after real source EOF; reset on seeks/continuations.
    source_reached_eof: bool,
    timeline: AudioTimeline,

    /// Whether a `GStreamer` duration re-query has already been attempted.
    /// Raw DTS/AC3 files often lack duration at pipeline start; a re-query
    /// after some data has been decoded may succeed.
    #[cfg(feature = "gst")]
    gst_duration_requeried: bool,

    /// Stored `ContinueWithFile` command to apply when the current file
    /// reaches EOF.  Set by command processing, consumed at EOF.
    /// Fields: `(path, track_token)`.
    pending_continue: Option<(PathBuf, u64)>,
    /// The file of a continuation rejected before the track switch: a
    /// switch to it ends the session, and the analysis thread restarts it.
    rejected_continuation: Option<PathBuf>,
    /// Why the decode failed, when it did.
    failure: Option<String>,
    /// Commands queued behind the session's `NewTrack`.  The session takes
    /// them before the channel's, so they apply in the order sent.
    queued_commands: VecDeque<SpectrogramWorkerCommand>,
}

/// Action returned by command processing in the session loop.
enum SessionAction {
    Continue,
    /// Token was updated directly on the session (continuation already
    /// consumed).  The caller must flush any partial chunk and emit a
    /// 0-column metadata chunk with the new token so the UI gapless
    /// handler fires immediately — not delayed until the next
    /// rate-limited data chunk.
    FlushToken,
    Stop,
    /// The track switched to a continuation the worker rejected; the
    /// session ends and the analysis thread restarts it.
    End,
    NewSession(SpectrogramWorkerCommand),
    SeekRequired {
        position_seconds: f64,
    },
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

/// Runs a spectrogram decode session for a single track.
/// Returns `Some(cmd)` if interrupted by a NewTrack/Stop, `None` if session ended naturally.
#[allow(clippy::too_many_lines)]
fn run_spectrogram_session(
    start: QueuedCommand,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &dyn AnalysisEventOutput,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
    track_duration_ms_out: &AtomicU64,
    reports: &Sender<SessionReport>,
) -> Option<SpectrogramWorkerCommand> {
    let QueuedCommand {
        cmd: mut initial_cmd,
        behind: queued_commands,
    } = start;
    let initial_cmd = &mut initial_cmd;
    let SpectrogramWorkerCommand::NewTrack { handoff, .. } = initial_cmd else {
        return None;
    };
    // Emitted before the file opens, which can take a while.
    for chunk in std::mem::take(&mut handoff.prelude) {
        let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(chunk));
    }
    let &mut SpectrogramWorkerCommand::NewTrack {
        track_token,
        generation: gen,
        ref path,
        fft_size,
        hop_size,
        zoom_level,
        widget_width,
        channel_count: requested_channel_count,
        start_seconds,
        target_position_seconds,
        emit_initial_reset,
        clear_history_on_reset,
        view_mode,
        display_mode,
        handoff: _,
    } = initial_cmd
    else {
        return None;
    };

    #[cfg(feature = "profiling-logs")]
    let session_started_at = std::time::Instant::now();
    profile_eprintln!(
        "[spect-worker] SESSION START path={} gen={gen} token={track_token} fft={fft_size} hop={hop_size} view={view_mode:?} display={display_mode:?} start_s={start_seconds:.2} target_s={target_position_seconds:.2}",
        path.file_name().unwrap_or_default().to_string_lossy(),
    );

    let bins_per_column = (fft_size / 2) + 1;
    let Some((mut source, native_sample_rate, native_channels, total_columns)) =
        open_audio_file(path)
    else {
        let cause = match std::fs::File::open(path) {
            Err(error) => FailureCause::Unopenable(error),
            Ok(_) => FailureCause::Analysis("its audio could not be read".to_string()),
        };
        fail_session(
            event_tx,
            reports,
            SessionFailure {
                generation: gen,
                track_token,
                channel_count: requested_channel_count,
                path: path.clone(),
                cause,
            },
        );
        return None;
    };

    profile_eprintln!(
        "[spect-worker] file opened in {:.2}ms sr={native_sample_rate} ch={native_channels} est_cols={total_columns}",
        session_started_at.elapsed().as_secs_f64() * 1000.0,
    );

    let effective_rate = u32::try_from(native_sample_rate).unwrap_or(48_000);

    publish_track_duration_ms(total_columns, effective_rate, track_duration_ms_out);
    let _ = reports.send(SessionReport::Opened {
        generation: gen,
        track_token,
        path: path.clone(),
        stamp: source_stamp(path),
        format: TrackFormat::Known {
            sample_rate: effective_rate,
            channels: native_channels,
        },
    });

    let actual_channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let output_interval = output_interval_for_zoom(hop_size, zoom_level);
    let effective_hop = effective_hop_from_interval(hop_size, output_interval);
    let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(effective_hop);
    let total_columns_estimate = scale_columns_estimate(total_columns, effective_hop);

    // Lookahead configuration: how far ahead of the play head the decode
    // worker runs before parking.
    let lookahead_seconds = lookahead_seconds();
    let lookahead_columns = SpectrogramBufferConfig {
        width: widget_width,
        sample_rate: effective_rate,
        hop: u32::try_from(effective_hop).unwrap_or(u32::MAX),
        bins: u32::try_from(bins_per_column).unwrap_or(u32::MAX),
        channels: u32::try_from(actual_channel_count).unwrap_or(u32::MAX),
        zoom: f64::from(zoom_level),
        centered: display_mode == SpectrogramDisplayMode::Centered,
    }
    .limits(lookahead_seconds)
    .lookahead;

    // Rate throttle: 2× realtime for rolling, unlimited for centered.
    let decode_rate_limit = if display_mode == SpectrogramDisplayMode::Rolling {
        std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(2.0)
    } else {
        f64::INFINITY
    };

    let (start_column, grid_frame) =
        stft_seek_grid(start_seconds, effective_rate, hop_size, effective_hop);
    let (seek_frame, leading_silence) = centred_window_start(grid_frame, fft_size);
    if seek_frame > 0 {
        if let Err(error) = source.seek_to_frame(seek_frame, effective_rate) {
            fail_session(
                event_tx,
                reports,
                SessionFailure {
                    generation: gen,
                    track_token,
                    channel_count: actual_channel_count,
                    path: path.clone(),
                    cause: FailureCause::Analysis(format!("seeking failed ({error})")),
                },
            );
            return None;
        }
    }

    let mut session = SpectrogramSessionState {
        track_token,
        path: path.clone(),
        gen,
        fft_size,
        hop_size,
        effective_hop,
        zoom_level,
        widget_width,
        view_mode,
        display_mode,
        channel_count: actual_channel_count,
        bins_per_column,
        total_columns_estimate,
        effective_rate,
        cols_per_second,
        target_position_seconds,
        suppress_backward_seek: false,
        columns_produced: start_column,
        session_start_column: start_column,
        stfts: (0..actual_channel_count)
            .map(|_| StftComputer::new(fft_size, hop_size))
            .collect(),
        resamplers: (0..actual_channel_count)
            .map(|_| PeakHoldResampler::new(output_interval))
            .collect(),
        packet_counter: 0,
        chunk_buf: Vec::new(),
        chunk_columns: 0,
        chunk_start_index: start_column,
        target_chunk_columns: 1, // Start with 1 for fastest first-pixel
        total_covered_samples: 0,
        throttle_origin: None,
        post_reset_unthrottled_columns: initial_unthrottled_columns(
            display_mode,
            start_column,
            target_position_seconds,
            cols_per_second,
        ),
        decode_rate_limit,
        lookahead_columns,
        lookahead_seconds,
        source_reached_eof: false,
        timeline: AudioTimeline::new(seek_frame),
        #[cfg(feature = "gst")]
        gst_duration_requeried: false,
        pending_continue: None,
        rejected_continuation: None,
        failure: None,
        queued_commands,
    };
    enqueue_leading_silence(&mut session.stfts, leading_silence);

    // Send initial metadata chunk (0 columns, carries estimates + transition semantics).
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token,
            generation: gen,
            columns_u8: Vec::new(),
            bins_per_column: clamp_to_u16(bins_per_column),
            column_count: 0,
            channel_count: clamp_to_u8(actual_channel_count),
            start_column_index: u64_to_u32_saturating(start_column),
            total_columns_estimate,
            sample_rate_hz: effective_rate,
            hop_size: clamp_to_u16(effective_hop),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: emit_initial_reset,
            clear_history: emit_initial_reset && clear_history_on_reset,
        },
    ));

    for resampler in &mut session.resamplers {
        resampler.reset_at_column(start_column, hop_size, effective_hop);
    }

    loop {
        let result = session_decode_loop(
            &mut session,
            &mut source,
            cmd_rx,
            event_tx,
            generation,
            columns_produced_out,
        );

        if let Some(SpectrogramWorkerCommand::Seek { position_seconds }) = result {
            if let Err(error) =
                handle_session_seek(&mut session, position_seconds, &mut source, event_tx)
            {
                session.fail(event_tx, reports, format!("seeking failed ({error})"));
                return None;
            }
            continue;
        }

        // Flush any partially accumulated chunk so the final columns are
        // not lost.
        session_flush_chunk(&mut session, event_tx, columns_produced_out);

        match result {
            Some(SpectrogramWorkerCommand::ContinueWithFile { path, track_token }) => {
                // Gapless continuation: open the next file but keep all
                // STFT / decimator / rate-limiter state intact.
                profile_eprintln!(
                    "[spect-worker] ContinueWithFile path={} token={track_token} cols_so_far={}",
                    path.file_name().unwrap_or_default().to_string_lossy(),
                    session.columns_produced.saturating_sub(start_column),
                );
                let opened = open_audio_file(&path);
                let stamp = source_stamp(&path);
                let format = opened.as_ref().map(|(_, sr, ch, _)| TrackFormat::Known {
                    sample_rate: u32::try_from(*sr).unwrap_or(u32::MAX),
                    channels: *ch,
                });
                let compatible = opened.as_ref().is_some_and(|(_, sr, ch, _)| {
                    let eff_ch = match session.view_mode {
                        SpectrogramViewMode::Downmix => 1,
                        SpectrogramViewMode::PerChannel => *ch,
                    };
                    *sr == u64::from(session.effective_rate) && eff_ch == session.channel_count
                });
                let (Some((new_source, _, _, new_est)), Some(format), true) =
                    (opened, format, compatible)
                else {
                    // The analysis thread starts the file afresh: the
                    // positions it sends for the continued timeline would
                    // misplace a session started here.  Before the track
                    // switch, the outgoing track stays parked at its end.
                    profile_eprintln!(
                        "[spect-worker] ContinueWithFile rejected path={}",
                        path.file_name().unwrap_or_default().to_string_lossy(),
                    );
                    let _ = reports.send(SessionReport::ContinuationRejected {
                        generation: session.gen,
                        path: path.clone(),
                        stamp,
                        format,
                    });
                    if track_token != session.track_token {
                        return None;
                    }
                    session.rejected_continuation = Some(path);
                    continue;
                };
                let _ = reports.send(SessionReport::Opened {
                    generation: session.gen,
                    track_token,
                    path: path.clone(),
                    stamp,
                    format,
                });
                source = new_source;
                session.source_reached_eof = false;
                session.rejected_continuation = None;
                session.timeline = AudioTimeline::new(0);
                session.track_token = track_token;
                session.path = path;
                // The session's columns go on from the outgoing track's.
                session.total_columns_estimate = continued_columns_estimate(
                    session.columns_produced,
                    new_est,
                    session.effective_hop,
                );
                // handle_track_change invalidates the outgoing duration
                // before dispatching ContinueWithFile.  A compatible
                // continuation stays inside this worker session, so it
                // must republish here; otherwise the duration remains
                // unknown for every later centered seek/zoom restart.
                publish_track_duration_ms(new_est, session.effective_rate, track_duration_ms_out);

                session.target_chunk_columns = 1;
                session.throttle_origin = None;
                session.session_start_column = session.columns_produced;
                session.post_reset_unthrottled_columns =
                    post_reset_unthrottled_columns(session.display_mode);
                session.suppress_backward_seek = true;
                profile_eprintln!(
                    "[spect-worker] file switch OK, continuing session unthrottled_cols={}",
                    session.post_reset_unthrottled_columns,
                );
            }
            Some(cmd) => {
                // Interrupted by NewTrack or Stop.
                let finalize = matches!(
                    &cmd,
                    SpectrogramWorkerCommand::NewTrack { handoff, .. } if handoff.finalize_previous
                );
                // A session parked at EOF has published its extent already.
                if finalize && !session.source_reached_eof && session.columns_produced > 0 {
                    session_emit_finalize_chunk(&session, event_tx);
                }
                profile_eprintln!(
                    "[spect-worker] SESSION END (interrupted) elapsed={:.1}ms cols_produced={}",
                    session_started_at.elapsed().as_secs_f64() * 1000.0,
                    session.columns_produced.saturating_sub(start_column),
                );
                return Some(cmd);
            }
            None => {
                if let Some(error) = session.failure.take() {
                    session.fail(event_tx, reports, error);
                    return None;
                }
                // EOF is published before parking; session exit must not
                // publish it again or turn a cancelled decode into completion.
                profile_eprintln!(
                    "[spect-worker] SESSION END ({}) elapsed={:.1}ms cols_produced={}",
                    if session.source_reached_eof {
                        "EOF"
                    } else {
                        "superseded"
                    },
                    session_started_at.elapsed().as_secs_f64() * 1000.0,
                    session.columns_produced.saturating_sub(start_column),
                );
                return None;
            }
        }
    }
}

/// A session that cannot read its file.
struct SessionFailure {
    generation: u64,
    track_token: u64,
    channel_count: usize,
    path: PathBuf,
    cause: FailureCause,
}

/// Why a session cannot read its file.
enum FailureCause {
    /// The file cannot be opened at all, which playback reports itself.
    Unopenable(std::io::Error),
    /// The spectrogram cannot read audio that playback may play.
    Analysis(String),
}

/// End a session that cannot read its file: clear what the UI shows of it,
/// as a far seek does, and tell the analysis thread, which tells the user
/// unless playback does. The worker waits for the next session, which may
/// read its file.
fn fail_session(
    event_tx: &dyn AnalysisEventOutput,
    reports: &Sender<SessionReport>,
    failure: SessionFailure,
) {
    let error = match failure.cause {
        FailureCause::Unopenable(error) => {
            eprintln!(
                "[analysis] spectrogram cannot open {}: {error}",
                failure.path.display()
            );
            None
        }
        FailureCause::Analysis(error) => {
            eprintln!(
                "[analysis] spectrogram of {} failed: {error}",
                failure.path.display()
            );
            Some(error)
        }
    };
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: failure.track_token,
            generation: failure.generation,
            columns_u8: Vec::new(),
            bins_per_column: 0,
            column_count: 0,
            channel_count: clamp_to_u8(failure.channel_count.max(1)),
            start_column_index: 0,
            total_columns_estimate: 0,
            sample_rate_hz: 0,
            hop_size: 0,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: true,
        },
    ));
    if let Some(error) = error {
        let _ = reports.send(SessionReport::Failed {
            generation: failure.generation,
            track_token: failure.track_token,
            path: failure.path,
            error,
        });
    }
}

impl SpectrogramSessionState {
    fn fail(
        &self,
        event_tx: &dyn AnalysisEventOutput,
        reports: &Sender<SessionReport>,
        error: String,
    ) {
        fail_session(
            event_tx,
            reports,
            SessionFailure {
                generation: self.gen,
                track_token: self.track_token,
                channel_count: self.channel_count,
                path: self.path.clone(),
                cause: FailureCause::Analysis(error),
            },
        );
    }
}

/// Publish the file-metadata duration represented by a reference-hop column
/// estimate.  `total_columns` includes the same +64 STFT-edge allowance used
/// by Qt's estimate, so retaining it keeps full-track-lock and EOF-detach math
/// aligned across the worker and analysis threads.
fn publish_track_duration_ms(
    total_columns: u32,
    effective_rate: u32,
    track_duration_ms_out: &AtomicU64,
) {
    let duration_ms = if effective_rate > 0 {
        f64_to_u64_saturating(
            f64::from(total_columns) * usize_to_f64_approx(REFERENCE_HOP)
                / f64::from(effective_rate)
                * 1000.0,
        )
    } else {
        0
    };
    track_duration_ms_out.store(duration_ms, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// Session decode loop
// ---------------------------------------------------------------------------

/// Inner decode loop. Returns `Some(cmd)` if interrupted, `None` on EOF.
#[allow(clippy::too_many_lines)]
fn session_decode_loop(
    session: &mut SpectrogramSessionState,
    source: &mut AudioFrameSource,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &dyn AnalysisEventOutput,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    // Only check generation for staleness. Generation is incremented whenever
    // analysis starts a fresh precomputed session for a new track or reset.
    let session_gen = session.gen;
    let is_stale = || -> bool { generation.load(Ordering::Relaxed) != session_gen };

    loop {
        if is_stale() {
            return None;
        }
        // A session parked at its end after rejecting a continuation has
        // published its end already.
        if session.source_reached_eof {
            break;
        }

        // 1. Check for commands via try_recv (drain pending, take latest position).
        if let Some(action) = process_session_commands(session, cmd_rx) {
            match action {
                SessionAction::Continue => {}
                SessionAction::FlushToken => {
                    session_flush_token(session, event_tx, columns_produced_out);
                    // Then the position that waited for the token chunk.
                    continue;
                }
                SessionAction::Stop => return Some(SpectrogramWorkerCommand::StopSession),
                SessionAction::End => return None,
                SessionAction::NewSession(cmd) => return Some(cmd),
                SessionAction::SeekRequired { position_seconds } => {
                    return Some(SpectrogramWorkerCommand::Seek { position_seconds });
                }
            }
        }

        // 2. Check lead — park if sufficiently ahead (unless gapless pending).
        let target_column =
            f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
        let lead = session.columns_produced.saturating_sub(target_column);

        if lead >= session.lookahead_columns && !post_reset_window_active(session) {
            // In centered mode the parked rows can become immediately visible,
            // so flush them before sleeping. Rolling mode only needs data at
            // the playback head, so keep partial lookahead chunks buffered to
            // avoid re-fragmenting into heartbeat-sized UI updates.
            flush_chunk_before_lookahead_park(session, event_tx, columns_produced_out);
            // Park: block until a command arrives.
            match next_session_command(session, || cmd_rx.recv()) {
                Ok(cmd) => match handle_single_command(session, cmd) {
                    SessionAction::Continue => continue,
                    SessionAction::FlushToken => {
                        session_flush_token(session, event_tx, columns_produced_out);
                        continue;
                    }
                    SessionAction::Stop => {
                        return Some(SpectrogramWorkerCommand::StopSession);
                    }
                    SessionAction::End => return None,
                    SessionAction::NewSession(cmd) => return Some(cmd),
                    SessionAction::SeekRequired { position_seconds } => {
                        return Some(SpectrogramWorkerCommand::Seek { position_seconds });
                    }
                },
                Err(_) => return None,
            }
        }

        // 3. Rate throttle (rolling mode only, after the post-reset window).
        if let Some(wait) = throttle_wait(session, std::time::Instant::now()) {
            match next_session_command(session, || {
                cmd_rx.recv_timeout(wait.min(Duration::from_millis(500)))
            }) {
                Ok(cmd) => match handle_single_command(session, cmd) {
                    SessionAction::Continue => continue,
                    SessionAction::FlushToken => {
                        session_flush_token(session, event_tx, columns_produced_out);
                        continue;
                    }
                    SessionAction::Stop => {
                        return Some(SpectrogramWorkerCommand::StopSession);
                    }
                    SessionAction::End => return None,
                    SessionAction::NewSession(cmd) => {
                        return Some(cmd);
                    }
                    SessionAction::SeekRequired { position_seconds } => {
                        return Some(SpectrogramWorkerCommand::Seek { position_seconds });
                    }
                },
                Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
                Err(_) => return None,
            }
        }

        // A packet may contain more FFT rows than the lookahead budget.
        // Drain retained rows before reading another packet after resuming.
        if !session.stfts.is_empty() && session.stfts.iter().all(StftComputer::has_row) {
            session_drain_stft_rows(session, event_tx, columns_produced_out);
            continue;
        }

        // 4. Decode next batch of audio frames.
        let audio = match session.timeline.read(|| source.next_frames()) {
            Ok(AudioRead::Pending) => continue,
            Ok(AudioRead::Frames(audio)) => audio,
            Err(error) => {
                session.failure = Some(format!("decoding failed ({error})"));
                return None;
            }
            Ok(AudioRead::Eof) => {
                // Real source EOF.  Publish the decoded extent while this
                // track is still current. Waiting until the parked worker
                // exits leaves the UI using a padded estimate.
                session.source_reached_eof = true;
                session_flush_chunk(session, event_tx, columns_produced_out);
                session_emit_finalize_chunk(session, event_tx);
                break;
            }
        };
        session.packet_counter += 1;

        session.total_covered_samples += usize_to_u64(audio.frames);
        if let Err(error) = enqueue_audio(&mut session.stfts, &audio, session.view_mode) {
            session.failure = Some(error);
            return None;
        }

        // 5. Drain STFT rows and emit chunks.
        session_drain_stft_rows(session, event_tx, columns_produced_out);

        // 6. Update total_columns_estimate when the decoder produces more
        //    columns than initially estimated.  This happens for raw DTS/AC3
        //    files where GStreamer cannot query duration from the headerless
        //    bitstream.  Without this, the UI ring buffer is undersized and
        //    early columns get evicted, causing the spectrogram to go black.
        maybe_update_columns_estimate(session, source);

        // Yield periodically to avoid starving UI.
        if session.packet_counter.is_multiple_of(64) {
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    if let Some((path, track_token)) = session.pending_continue.take() {
        return Some(SpectrogramWorkerCommand::ContinueWithFile { path, track_token });
    }
    // Otherwise park and keep handling commands so backward seeks
    // still work after the decoder has consumed the entire file.
    loop {
        let session_gen = session.gen;
        if generation.load(Ordering::Relaxed) != session_gen {
            return None;
        }
        match next_session_command(session, || cmd_rx.recv()) {
            Ok(cmd) => match handle_single_command(session, cmd) {
                SessionAction::Continue => {
                    if let Some((path, token)) = session.pending_continue.take() {
                        return Some(SpectrogramWorkerCommand::ContinueWithFile {
                            path,
                            track_token: token,
                        });
                    }
                }
                SessionAction::FlushToken => {
                    session_flush_token(session, event_tx, columns_produced_out);
                }
                SessionAction::Stop => return Some(SpectrogramWorkerCommand::StopSession),
                SessionAction::End => return None,
                SessionAction::NewSession(cmd) => return Some(cmd),
                SessionAction::SeekRequired { position_seconds } => {
                    return Some(SpectrogramWorkerCommand::Seek { position_seconds });
                }
            },
            Err(_) => return None,
        }
    }
}

// ---------------------------------------------------------------------------
// Command processing
// ---------------------------------------------------------------------------

/// The session's next command: those queued behind its `NewTrack` come
/// before the channel's.
fn next_session_command<E>(
    session: &mut SpectrogramSessionState,
    recv: impl FnOnce() -> Result<SpectrogramWorkerCommand, E>,
) -> Result<SpectrogramWorkerCommand, E> {
    session.queued_commands.pop_front().map_or_else(recv, Ok)
}

fn process_session_commands(
    session: &mut SpectrogramSessionState,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
) -> Option<SessionAction> {
    let mut latest_position: Option<f64> = None;
    let mut latest_seek: Option<f64> = None;
    let mut needs_flush_token = false;

    // Drain all pending commands, take the latest seek/position update.
    while let Ok(cmd) = next_session_command(session, || cmd_rx.try_recv()) {
        match cmd {
            SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
                latest_position = Some(position_seconds);
            }
            SpectrogramWorkerCommand::Seek { position_seconds } => {
                latest_seek = Some(position_seconds);
            }
            SpectrogramWorkerCommand::NewTrack { .. } => {
                // The commands queued behind it are for the session it
                // starts, which takes them with it.
                return Some(SessionAction::NewSession(cmd));
            }
            SpectrogramWorkerCommand::SetDisplayMode(mode) => {
                apply_display_mode(session, mode);
            }
            SpectrogramWorkerCommand::ContinueWithFile { path, track_token } => {
                session.pending_continue = Some((path, track_token));
            }
            SpectrogramWorkerCommand::UpdateTrackToken { track_token } => {
                match update_track_token(session, track_token) {
                    SessionAction::FlushToken => needs_flush_token = true,
                    SessionAction::End => return Some(SessionAction::End),
                    _ => {}
                }
            }
            SpectrogramWorkerCommand::CancelPendingContinue => {
                session.pending_continue = None;
            }
            SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width } => {
                if widget_width > session.widget_width {
                    session.widget_width = widget_width;
                    refresh_lookahead(session);
                }
            }
            SpectrogramWorkerCommand::StopSession => {
                return Some(SessionAction::Stop);
            }
            SpectrogramWorkerCommand::RemapPaths(renames) => {
                remap_pending_continue(session, &renames);
            }
        }
    }

    // FlushToken takes priority over position-only results — the UI
    // needs the metadata chunk before the next position update lands.
    // The seek or position then follows it.
    if needs_flush_token {
        if let Some(position_seconds) = latest_seek {
            queue_session_command(
                &mut session.queued_commands,
                SpectrogramWorkerCommand::Seek { position_seconds },
            );
        } else if let Some(position_seconds) = latest_position {
            queue_session_command(
                &mut session.queued_commands,
                SpectrogramWorkerCommand::PositionUpdate { position_seconds },
            );
        }
        return Some(SessionAction::FlushToken);
    }

    if let Some(position_seconds) = latest_seek {
        session.target_position_seconds = position_seconds;
        return Some(SessionAction::SeekRequired { position_seconds });
    }

    // Process position update — check if seek is needed.
    if let Some(position_seconds) = latest_position {
        session.target_position_seconds = position_seconds;

        let target_col = f64_to_u64_saturating(position_seconds * session.cols_per_second);
        if session.suppress_backward_seek {
            // Keep suppressing until a position update lands at or past
            // session_start_column.  The offset-adjusted position can
            // lag a few columns behind for multiple heartbeats.
            if target_col >= session.session_start_column {
                session.suppress_backward_seek = false;
            }
        } else if target_col < session.session_start_column {
            // Backward seek needed.
            return Some(SessionAction::SeekRequired { position_seconds });
        }
        let forward_gap = target_col.saturating_sub(session.columns_produced);
        let far_forward_threshold = session.lookahead_columns;
        if forward_gap > far_forward_threshold {
            // Far forward seek needed.
            return Some(SessionAction::SeekRequired { position_seconds });
        }
    }

    None
}

fn handle_single_command(
    session: &mut SpectrogramSessionState,
    cmd: SpectrogramWorkerCommand,
) -> SessionAction {
    match cmd {
        SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
            session.target_position_seconds = position_seconds;
            let target_col = f64_to_u64_saturating(position_seconds * session.cols_per_second);
            if session.suppress_backward_seek {
                if target_col >= session.session_start_column {
                    session.suppress_backward_seek = false;
                }
            } else if target_col < session.session_start_column {
                return SessionAction::SeekRequired { position_seconds };
            }
            let forward_gap = target_col.saturating_sub(session.columns_produced);
            if forward_gap > session.lookahead_columns {
                return SessionAction::SeekRequired { position_seconds };
            }
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::Seek { position_seconds } => {
            session.target_position_seconds = position_seconds;
            SessionAction::SeekRequired { position_seconds }
        }
        SpectrogramWorkerCommand::NewTrack { .. } => SessionAction::NewSession(cmd),
        SpectrogramWorkerCommand::SetDisplayMode(mode) => {
            apply_display_mode(session, mode);
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::ContinueWithFile { path, track_token } => {
            session.pending_continue = Some((path, track_token));
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::UpdateTrackToken { track_token } => {
            update_track_token(session, track_token)
        }
        SpectrogramWorkerCommand::CancelPendingContinue => {
            session.pending_continue = None;
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width } => {
            if widget_width > session.widget_width {
                session.widget_width = widget_width;
                refresh_lookahead(session);
            }
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::StopSession => SessionAction::Stop,
        SpectrogramWorkerCommand::RemapPaths(renames) => {
            remap_pending_continue(session, &renames);
            SessionAction::Continue
        }
    }
}

/// The track switched to the session's continuation.
fn update_track_token(session: &mut SpectrogramSessionState, track_token: u64) -> SessionAction {
    if let Some((_, ref mut pending_token)) = session.pending_continue {
        // Continuation not yet consumed — update pending token only.
        // Old-track columns keep the old token until EOF.
        *pending_token = track_token;
        SessionAction::Continue
    } else if session.rejected_continuation.is_some() {
        // The worker could not continue with the file now playing, and the
        // analysis thread, told so, starts it afresh.  Positions on the
        // timeline it switched to do not fit this session.
        SessionAction::End
    } else {
        // Continuation already consumed — worker is on the new file.
        // FlushToken tells the caller to emit a 0-column metadata
        // chunk immediately so the UI gapless handler fires without
        // waiting for the next rate-limited data chunk.
        session.track_token = track_token;
        SessionAction::FlushToken
    }
}

// ---------------------------------------------------------------------------
// Seek handling
// ---------------------------------------------------------------------------

/// Handles a seek within the current session by resetting STFT state and
/// repositioning the file reader.
fn handle_session_seek(
    session: &mut SpectrogramSessionState,
    position_seconds: f64,
    source: &mut AudioFrameSource,
    event_tx: &dyn AnalysisEventOutput,
) -> anyhow::Result<()> {
    profile_eprintln!("[spect-worker] SEEK to {position_seconds:.2}s");

    // Drop any partially accumulated pre-seek chunk. Emitting it after a seek
    // makes the UI paint old-timeline columns just before the reset arrives.
    session.chunk_buf.clear();
    session.chunk_columns = 0;

    let (new_column, grid_frame) = stft_seek_grid(
        position_seconds,
        session.effective_rate,
        session.hop_size,
        session.effective_hop,
    );
    let (frame, leading_silence) = centred_window_start(grid_frame, session.fft_size);
    source.seek_to_frame(frame, session.effective_rate)?;
    session.source_reached_eof = false;
    session.timeline = AudioTimeline::new(frame);
    for stft in &mut session.stfts {
        stft.reset();
    }
    enqueue_leading_silence(&mut session.stfts, leading_silence);
    for resampler in &mut session.resamplers {
        resampler.reset_at_column(new_column, session.hop_size, session.effective_hop);
    }

    session.columns_produced = new_column;
    session.session_start_column = new_column;
    session.chunk_buf.clear();
    session.chunk_columns = 0;
    session.chunk_start_index = new_column;
    session.target_chunk_columns = 1; // Fast first-pixel after seek
    session.total_covered_samples = 0;
    session.throttle_origin = None;
    session.post_reset_unthrottled_columns = post_reset_unthrottled_columns(session.display_mode);
    session.target_position_seconds = position_seconds;

    // Emit buffer_reset chunk.
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: session.track_token,
            generation: session.gen,
            columns_u8: Vec::new(),
            bins_per_column: clamp_to_u16(session.bins_per_column),
            column_count: 0,
            channel_count: clamp_to_u8(session.channel_count),
            start_column_index: u64_to_u32_saturating(new_column),
            total_columns_estimate: session.total_columns_estimate,
            sample_rate_hz: session.effective_rate,
            hop_size: clamp_to_u16(session.effective_hop),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: false,
        },
    ));

    Ok(())
}

// ---------------------------------------------------------------------------
// Display mode / chunk helpers
// ---------------------------------------------------------------------------

fn max_target_chunk_columns(display_mode: SpectrogramDisplayMode) -> u16 {
    match display_mode {
        // Rolling mode is latency-sensitive and the chunk payload is copied
        // through the UI bridge on the GUI thread. Capping the ramp here keeps
        // post-transition updates smooth instead of reintroducing 256-column bursts.
        SpectrogramDisplayMode::Rolling => 64,
        SpectrogramDisplayMode::Centered => 256,
    }
}

fn next_target_chunk_columns(current: u16, display_mode: SpectrogramDisplayMode) -> u16 {
    current
        .saturating_mul(2)
        .min(max_target_chunk_columns(display_mode))
}

fn next_target_chunk_columns_for_payload(
    current: u16,
    display_mode: SpectrogramDisplayMode,
    bins_per_column: usize,
    channel_count: usize,
) -> u16 {
    let column_cap = next_target_chunk_columns(current, display_mode);
    let bytes_per_column = bins_per_column.saturating_mul(channel_count).max(1);
    let payload_cap = SPECTROGRAM_CHUNK_BYTE_BUDGET / bytes_per_column;
    column_cap.min(clamp_to_u16(payload_cap.max(1)))
}

/// Apply a display-mode change to a live session: update rate limit
/// and lookahead for the new mode.
fn apply_display_mode(session: &mut SpectrogramSessionState, mode: SpectrogramDisplayMode) {
    session.display_mode = mode;
    // Columns decoded at the centered mode's unlimited rate do not count
    // against the rolling limit.
    session.throttle_origin = None;
    if mode == SpectrogramDisplayMode::Rolling {
        session.decode_rate_limit = std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(2.0);
    } else {
        session.decode_rate_limit = f64::INFINITY;
    }
    refresh_lookahead(session);
}

fn refresh_lookahead(session: &mut SpectrogramSessionState) {
    session.lookahead_columns = SpectrogramBufferConfig {
        width: session.widget_width,
        sample_rate: session.effective_rate,
        hop: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
        bins: u32::try_from(session.bins_per_column).unwrap_or(u32::MAX),
        channels: u32::try_from(session.channel_count).unwrap_or(u32::MAX),
        zoom: f64::from(session.zoom_level),
        centered: session.display_mode == SpectrogramDisplayMode::Centered,
    }
    .limits(session.lookahead_seconds)
    .lookahead;
}

fn initial_unthrottled_columns(
    mode: SpectrogramDisplayMode,
    start: u64,
    target: f64,
    columns_per_second: f64,
) -> u32 {
    let history = if mode == SpectrogramDisplayMode::Rolling {
        u64_to_u32_saturating(
            f64_to_u64_saturating(target * columns_per_second).saturating_sub(start),
        )
    } else {
        0
    };
    post_reset_unthrottled_columns(mode).saturating_add(history)
}

fn post_reset_unthrottled_columns(display_mode: SpectrogramDisplayMode) -> u32 {
    match display_mode {
        // Keep rolling mode unthrottled until it has emitted the first full
        // 64-column chunk after the 1/2/4/8/16/32 ramp. Stopping earlier lets
        // the display catch the write head before that first full chunk lands.
        SpectrogramDisplayMode::Rolling => 1 + 2 + 4 + 8 + 16 + 32 + 64,
        SpectrogramDisplayMode::Centered => 0,
    }
}

fn post_reset_window_active(session: &SpectrogramSessionState) -> bool {
    session
        .columns_produced
        .saturating_sub(session.session_start_column)
        < u64::from(session.post_reset_unthrottled_columns)
}

/// How long a rolling session waits before decoding more, to stay within
/// its rate limit; `None` when it may decode now.
///
/// The limit counts from where the post-reset window ended: the window
/// decodes the history a reset needs, and the playback position that
/// history ends at, at full speed. Counting those columns too would make
/// the session wait for the time they would have taken at the limit,
/// stalling the display after a rolling view is shown again.
fn throttle_wait(
    session: &mut SpectrogramSessionState,
    now: std::time::Instant,
) -> Option<Duration> {
    if post_reset_window_active(session) || !session.decode_rate_limit.is_finite() {
        return None;
    }
    let max_cols_per_wall_sec = session.decode_rate_limit * session.cols_per_second;
    if max_cols_per_wall_sec <= 0.0 {
        return None;
    }
    let (origin_time, origin_column) = *session
        .throttle_origin
        .get_or_insert((now, session.columns_produced));
    let elapsed = now.saturating_duration_since(origin_time).as_secs_f64();
    let columns = session.columns_produced.saturating_sub(origin_column);
    #[allow(clippy::cast_precision_loss)]
    let expected_elapsed = columns as f64 / max_cols_per_wall_sec;
    Some(Duration::from_secs_f64(
        (expected_elapsed - elapsed).max(0.0),
    ))
    .filter(|wait| !wait.is_zero())
}

// ---------------------------------------------------------------------------
// STFT row draining / chunk emission
// ---------------------------------------------------------------------------

fn session_drain_stft_rows(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
    loop {
        let target =
            f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
        if session.columns_produced.saturating_sub(target) >= session.lookahead_columns
            && !post_reset_window_active(session)
        {
            break;
        }

        let Some(emitted) = append_stft_column(
            &mut session.stfts,
            &mut session.resamplers,
            &mut session.chunk_buf,
            session.bins_per_column,
            session.fft_size,
        ) else {
            break;
        };
        if !emitted {
            continue;
        }
        session.chunk_columns += 1;
        session.columns_produced += 1;

        if session.chunk_columns >= session.target_chunk_columns {
            let coverage = seconds_from_frames(
                session.total_covered_samples,
                u64::from(session.effective_rate),
            );
            let start_col_u32 = u64_to_u32_saturating(session.chunk_start_index);
            let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
                PrecomputedSpectrogramChunk {
                    track_token: session.track_token,
                    generation: session.gen,
                    columns_u8: std::mem::take(&mut session.chunk_buf),
                    bins_per_column: clamp_to_u16(session.bins_per_column),
                    column_count: session.chunk_columns,
                    channel_count: clamp_to_u8(session.channel_count),
                    start_column_index: start_col_u32,
                    total_columns_estimate: session.total_columns_estimate,
                    sample_rate_hz: session.effective_rate,
                    hop_size: clamp_to_u16(session.effective_hop),
                    coverage_seconds: coverage,
                    complete: false,
                    buffer_reset: false,
                    clear_history: false,
                },
            ));
            session.chunk_start_index = session.columns_produced;
            session.chunk_columns = 0;
            columns_produced_out.store(session.columns_produced, Ordering::Relaxed);
            // Ramp up chunk size in rolling mode only to a latency-friendly cap.
            session.target_chunk_columns = next_target_chunk_columns_for_payload(
                session.target_chunk_columns,
                session.display_mode,
                session.bins_per_column,
                session.channel_count,
            );
        }
    }
}

/// Update `total_columns_estimate` when the decoder is approaching or
/// exceeding the current estimate.  This is critical for raw DTS/AC3 files
/// where `GStreamer` cannot determine the stream duration from headerless
/// bitstreams, causing the initial estimate to fall back to 300 seconds.
///
/// Two strategies:
/// 1. Re-query `GStreamer` pipeline duration once some data has been decoded;
///    `GStreamer` may determine the duration after processing the bitstream.
/// 2. If the re-query fails and we're within 25% of the estimate, double it
///    so the UI ring buffer grows before columns are evicted.
///
/// Convert an audio-frame count from a duration re-query into a
/// total-columns estimate in the session's `effective_hop` units.
///
/// The raw math (`frames / REFERENCE_HOP`) gives a reference-hop
/// count — but `session.total_columns_estimate` is always stored in
/// the current session's `effective_hop` cols, so we must scale.
/// Without this scaling, zoom-out sessions
/// (`effective_hop > REFERENCE_HOP`) see a ref-hop figure that is
/// always larger than the correctly-scaled in-session estimate and
/// the estimate gets inflated by `effective_hop / REFERENCE_HOP`,
/// breaking the zoom-out minZoom computation on the Qt side.
#[cfg(any(feature = "gst", test))]
fn duration_requery_estimate_for_session(effective_frames: u64, effective_hop: usize) -> u32 {
    let new_est_ref =
        u32::try_from(((effective_frames / (REFERENCE_HOP as u64)) + 64).min(u64::from(u32::MAX)))
            .unwrap_or(u32::MAX);
    scale_columns_estimate(new_est_ref, effective_hop)
}

fn maybe_update_columns_estimate(session: &mut SpectrogramSessionState, source: &AudioFrameSource) {
    let estimate = u64::from(session.total_columns_estimate);
    let produced = session.columns_produced;

    // Attempt a GStreamer duration re-query early in the session.
    #[cfg(feature = "gst")]
    if !session.gst_duration_requeried && session.packet_counter >= 20 {
        session.gst_duration_requeried = true;
        if let Some(ns) = source.query_duration_ns() {
            let rate = u64::from(session.effective_rate);
            let total_frames = ns * rate / 1_000_000_000;
            let effective = total_frames;
            let new_est = duration_requery_estimate_for_session(effective, session.effective_hop);
            if new_est > session.total_columns_estimate {
                profile_eprintln!(
                    "[spect-worker] duration re-query OK, est_cols {} → {} (effective_hop={})",
                    session.total_columns_estimate,
                    new_est,
                    session.effective_hop,
                );
                session.total_columns_estimate = new_est;
                return;
            }
        }
    }

    // Suppress the unused-variable warning when GStreamer is disabled.
    let _ = source;

    // Safety net for sources without a trustworthy duration (the
    // 300-second fallback in decoders.rs): if the decoder has actually
    // produced MORE cols than the estimate, the estimate was wrong and
    // we need to double it so the UI ring/range don't clamp behind the
    // growing column count.
    //
    // Fire strictly when produced exceeds the estimate rather than on
    // a 75% threshold.  For bounded (file) sources with a real
    // duration, the estimate already includes +64 cols of STFT tail
    // padding, so produced tops out AT or slightly BELOW the estimate
    // and the doubling never fires.  An early-threshold doubling here
    // inflated zoom-out estimates by 2× (effective_hop > REFERENCE_HOP
    // means cols_produced climbs fast relative to the already-small
    // estimate) and left Qt with an estimate that didn't match the
    // actual decoded extent — post-seek displays showed mostly blank
    // because displayRight clamped to the inflated estimate but data
    // only covered the actual decoded cols.
    if produced > estimate && estimate < u64::from(u32::MAX / 2) {
        let new_est = session.total_columns_estimate.saturating_mul(2);
        profile_eprintln!(
            "[spect-worker] columns overshot estimate, est_cols {} → {} (produced={})",
            session.total_columns_estimate,
            new_est,
            produced,
        );
        session.total_columns_estimate = new_est;
    }
}

fn session_flush_chunk(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
    // Drain any partial resampler window at track end.
    // All channels must flush together — if any channel has no
    // partial data, skip the flush (same synchronization as the
    // drain loop).
    let flushed: Vec<Option<&[f32]>> = session
        .resamplers
        .iter_mut()
        .map(PeakHoldResampler::flush)
        .collect();
    if !flushed.is_empty() && flushed.iter().all(Option::is_some) {
        for maybe_row in &flushed {
            let row = maybe_row.as_ref().unwrap();
            append_quantized_row(
                &mut session.chunk_buf,
                row,
                session.bins_per_column,
                session.fft_size,
            );
        }
        session.chunk_columns += 1;
        session.columns_produced += 1;
    }

    session_emit_pending_chunk(session, event_tx, columns_produced_out);
}

/// Publish already-complete columns without finalizing the resampler's
/// partial output interval. Temporary lookahead parks and token handoffs must
/// preserve that interval so column indices remain derived from audio frames.
fn session_emit_pending_chunk(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
    if session.chunk_columns > 0 {
        let coverage = seconds_from_frames(
            session.total_covered_samples,
            u64::from(session.effective_rate),
        );
        let start_col_u32 = u64_to_u32_saturating(session.chunk_start_index);
        let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
            PrecomputedSpectrogramChunk {
                track_token: session.track_token,
                generation: session.gen,
                columns_u8: std::mem::take(&mut session.chunk_buf),
                bins_per_column: clamp_to_u16(session.bins_per_column),
                column_count: session.chunk_columns,
                channel_count: clamp_to_u8(session.channel_count),
                start_column_index: start_col_u32,
                total_columns_estimate: session.total_columns_estimate,
                sample_rate_hz: session.effective_rate,
                hop_size: clamp_to_u16(session.effective_hop),
                coverage_seconds: coverage,
                complete: false,
                buffer_reset: false,
                clear_history: false,
            },
        ));
        session.chunk_columns = 0;
        session.chunk_start_index = session.columns_produced;
    }
    // Always publish the final columns_produced so the analysis runtime
    // has the exact boundary at EOF for staged-commit alignment.
    columns_produced_out.store(session.columns_produced, Ordering::Relaxed);
}

/// Flush any partial chunk and emit a 0-column metadata chunk carrying
/// the session's current token.  This makes the UI's gapless handler fire
/// immediately on `UpdateTrackToken` instead of waiting for the next
/// rate-limited data chunk (~0.4–0.7 s later).
fn session_flush_token(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
    // Publish complete buffered columns so they carry the updated token.
    // Preserve the partial resampler interval across the token handoff.
    session_emit_pending_chunk(session, event_tx, columns_produced_out);

    // Emit a 0-column metadata chunk with the new token.
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: session.track_token,
            generation: session.gen,
            columns_u8: Vec::new(),
            bins_per_column: clamp_to_u16(session.bins_per_column),
            column_count: 0,
            channel_count: clamp_to_u8(session.channel_count),
            start_column_index: u64_to_u32_saturating(session.columns_produced),
            total_columns_estimate: session.total_columns_estimate,
            sample_rate_hz: session.effective_rate,
            hop_size: clamp_to_u16(session.effective_hop),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: false,
            clear_history: false,
        },
    ));
}

fn flush_chunk_before_lookahead_park(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
    if session.display_mode == SpectrogramDisplayMode::Rolling {
        return;
    }
    session_emit_pending_chunk(session, event_tx, columns_produced_out);
}

/// Emit a finalize chunk carrying the actual decoded column count as
/// `total_columns_estimate`.  Qt uses this to shrink its
/// `m_precomputedTotalColumnsEstimate` so the centered-mode EOF clamp
/// fires at the true end of decoded audio instead of the file-metadata
/// estimate (which may overshoot by seconds at high zoom).
fn session_emit_finalize_chunk(
    session: &SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
) {
    let final_cols = u64_to_u32_saturating(session.columns_produced);
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: session.track_token,
            generation: session.gen,
            columns_u8: Vec::new(),
            bins_per_column: clamp_to_u16(session.bins_per_column),
            column_count: 0,
            channel_count: clamp_to_u8(session.channel_count),
            start_column_index: final_cols,
            total_columns_estimate: final_cols,
            sample_rate_hz: session.effective_rate,
            hop_size: clamp_to_u16(session.effective_hop),
            coverage_seconds: 0.0,
            complete: true,
            buffer_reset: false,
            clear_history: false,
        },
    ));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;

    #[test]
    fn stopping_a_session_keeps_the_worker_alive_until_disconnect() {
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let path = std::env::temp_dir().join(format!(
                "ferrous-worker-lifetime-{}-{mode:?}.mp3",
                std::process::id()
            ));
            std::fs::write(&path, include_bytes!("fixtures/seek-tone.mp3")).unwrap();
            let (commands, command_rx) = unbounded();
            let (events, event_rx) = unbounded();
            let worker = std::thread::spawn(move || {
                spectrogram_worker_loop(
                    &command_rx,
                    &events,
                    &AtomicU64::new(1),
                    &AtomicU64::new(0),
                    &AtomicU64::new(0),
                    &unbounded().0,
                )
            });
            for token in 1..=3 {
                // First stop is idle; subsequent stops interrupt an EOF-parked
                // session. Queue the restart immediately to cover coalescing.
                commands
                    .send(SpectrogramWorkerCommand::StopSession)
                    .unwrap();
                commands
                    .send(SpectrogramWorkerCommand::NewTrack {
                        track_token: token,
                        generation: 1,
                        path: path.clone(),
                        fft_size: 512,
                        hop_size: 1024,
                        zoom_level: 1.0,
                        widget_width: 320,
                        channel_count: 1,
                        start_seconds: 0.0,
                        target_position_seconds: 0.0,
                        emit_initial_reset: true,
                        clear_history_on_reset: true,
                        view_mode: SpectrogramViewMode::Downmix,
                        display_mode: mode,
                        handoff: SessionHandoff::default(),
                    })
                    .unwrap();
                let deadline = std::time::Instant::now() + Duration::from_secs(2);
                loop {
                    assert!(
                        std::time::Instant::now() < deadline,
                        "worker stopped permanently"
                    );
                    if let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) =
                        event_rx.recv_timeout(Duration::from_millis(20))
                    {
                        if chunk.track_token == token && chunk.complete {
                            break;
                        }
                    }
                }
            }
            drop(commands);
            worker.join().unwrap();
            std::fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn resumed_rolling_session_fills_history_before_throttling() {
        assert_eq!(
            initial_unthrottled_columns(SpectrogramDisplayMode::Rolling, 100, 5.0, 100.0),
            527
        );
        assert_eq!(
            initial_unthrottled_columns(SpectrogramDisplayMode::Rolling, 0, 0.0, 100.0),
            127
        );
        assert_eq!(
            initial_unthrottled_columns(SpectrogramDisplayMode::Centered, 100, 5.0, 100.0),
            0
        );
    }

    /// Decode a rolling session's columns as the decode loop does, on a
    /// simulated clock, while playback advances from where the session was
    /// resumed; returns the least lead of the decoded right edge over the
    /// playhead after the refill, and the longest the session waited at once.
    fn simulate_rolling_resume(session: &mut SpectrogramSessionState, seconds: f64) -> (i64, f64) {
        let start = std::time::Instant::now();
        let resumed_at = session.target_position_seconds;
        let step = 0.002;
        let mut least_lead = i64::MAX;
        let mut longest_wait = 0.0_f64;
        let mut now = 0.0;
        while now < seconds {
            session.target_position_seconds = resumed_at + now;
            let playhead =
                f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
            let lead = session.columns_produced.saturating_sub(playhead);
            let at = start + Duration::from_secs_f64(now);
            if lead >= session.lookahead_columns && !post_reset_window_active(session) {
                now += step;
            } else if let Some(wait) = throttle_wait(session, at) {
                longest_wait = longest_wait.max(wait.as_secs_f64());
                now += wait.as_secs_f64().min(0.5);
                // The clock the wait is measured on counts nanoseconds.
                now += 1e-9;
            } else {
                // Decoding takes no time on the simulated clock.
                session.columns_produced += 1;
            }
            // The refill decodes the history up to the playhead first.
            if !post_reset_window_active(session) {
                #[allow(clippy::cast_possible_wrap)]
                let lead = session.columns_produced as i64 - playhead as i64;
                least_lead = least_lead.min(lead);
            }
        }
        (least_lead, longest_wait)
    }

    #[test]
    fn a_resumed_rolling_session_keeps_its_right_edge_ahead_of_playback() {
        // Shown again 30 s into a track: the session restarts a viewport
        // back and refills that history at full speed.
        let mut session = make_test_session();
        let (start_column, _) = stft_seek_grid(10.0, session.effective_rate, 256, 1_024);
        session.columns_produced = start_column;
        session.session_start_column = start_column;
        session.target_position_seconds = 30.0;
        session.post_reset_unthrottled_columns = initial_unthrottled_columns(
            SpectrogramDisplayMode::Rolling,
            start_column,
            30.0,
            session.cols_per_second,
        );
        let (least_lead, longest_wait) = simulate_rolling_resume(&mut session, 20.0);
        // Within the rate limit, it does not wait for the time the refill
        // would have taken at the limit, and stays ahead of playback.
        assert!(longest_wait < 0.05, "waited {longest_wait} s");
        assert!(
            least_lead > 0,
            "playback overtook the decoded columns by {least_lead}"
        );
        // It still decodes at twice the playback rate, up to its lookahead.
        assert!(session.columns_produced > start_column + 1_400);

        // A centered session that switches to rolling does not count its
        // unlimited decode against the limit either.
        let mut session = make_test_session();
        session.target_position_seconds = 30.0;
        apply_display_mode(&mut session, SpectrogramDisplayMode::Centered);
        session.columns_produced = 1_406 + 300;
        assert_eq!(throttle_wait(&mut session, std::time::Instant::now()), None);
        apply_display_mode(&mut session, SpectrogramDisplayMode::Rolling);
        let (least_lead, longest_wait) = simulate_rolling_resume(&mut session, 5.0);
        assert!(longest_wait < 0.05, "waited {longest_wait} s");
        assert!(
            least_lead > 0,
            "playback overtook the decoded columns by {least_lead}"
        );
    }

    #[test]
    fn rolling_sessions_decode_at_the_rate_limit_after_their_window() {
        let mut session = make_test_session();
        session.post_reset_unthrottled_columns = 0;
        session.columns_produced = session.session_start_column;
        let start = std::time::Instant::now();
        assert_eq!(throttle_wait(&mut session, start), None);
        // 94 columns are two seconds of audio, a second at twice realtime.
        session.columns_produced += 94;
        let wait = throttle_wait(&mut session, start + Duration::from_millis(250))
            .expect("ahead of the limit");
        assert!((wait.as_secs_f64() - 0.752).abs() < 0.01, "{wait:?}");
        assert_eq!(
            throttle_wait(&mut session, start + Duration::from_millis(1_010)),
            None
        );
    }

    #[test]
    fn worker_publishes_eof_before_parking_and_after_a_backward_seek() {
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let path = std::env::temp_dir()
                .join(format!("ferrous-eof-{}-{mode:?}.mp3", std::process::id()));
            std::fs::write(&path, include_bytes!("fixtures/seek-tone.mp3")).expect("fixture");
            let (commands, command_rx) = unbounded();
            let (events, event_rx) = unbounded();
            let file = path.clone();
            let worker = std::thread::spawn(move || {
                let command = SpectrogramWorkerCommand::NewTrack {
                    track_token: 1,
                    generation: 1,
                    path: file,
                    fft_size: 512,
                    hop_size: 64,
                    zoom_level: 1.0,
                    widget_width: 320,
                    channel_count: 1,
                    start_seconds: 0.0,
                    target_position_seconds: 0.0,
                    emit_initial_reset: true,
                    clear_history_on_reset: true,
                    view_mode: SpectrogramViewMode::Downmix,
                    display_mode: mode,
                    handoff: SessionHandoff::default(),
                };
                run_spectrogram_session(
                    QueuedCommand::alone(command),
                    &command_rx,
                    &events,
                    &AtomicU64::new(1),
                    &AtomicU64::new(0),
                    &AtomicU64::new(0),
                    &unbounded().0,
                )
            });
            let wait_for_eof = || {
                let deadline = std::time::Instant::now() + Duration::from_secs(2);
                while std::time::Instant::now() < deadline {
                    if let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) =
                        event_rx.recv_timeout(Duration::from_millis(20))
                    {
                        if chunk.complete {
                            return Some(chunk);
                        }
                    }
                }
                None
            };
            let first = wait_for_eof();
            commands
                .send(SpectrogramWorkerCommand::Seek {
                    position_seconds: 0.25,
                })
                .expect("backward seek");
            let second = wait_for_eof();
            commands
                .send(SpectrogramWorkerCommand::StopSession)
                .expect("stop worker");
            worker.join().expect("join worker");
            std::fs::remove_file(path).expect("remove fixture");
            for result in [first, second] {
                let chunk =
                    result.expect("EOF must arrive without replacing or stopping the track");
                assert_eq!(chunk.track_token, 1);
                assert_eq!(chunk.generation, 1);
                assert_eq!(chunk.column_count, 0);
                assert!(chunk.total_columns_estimate > 0 && chunk.total_columns_estimate < 60);
            }
            assert!(!event_rx.try_iter().any(|event| matches!(event,
                AnalysisEvent::PrecomputedSpectrogramChunk(chunk) if chunk.complete)));
        }
    }
    use std::sync::atomic::AtomicU64;

    #[cfg(not(feature = "gst"))]
    use super::super::decoders::AudioFrameSource;
    #[cfg(not(feature = "gst"))]
    use symphonia::core::codecs::DecoderOptions;
    #[cfg(not(feature = "gst"))]
    use symphonia::core::formats::FormatOptions;
    #[cfg(not(feature = "gst"))]
    use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
    #[cfg(not(feature = "gst"))]
    use symphonia::core::meta::MetadataOptions;
    #[cfg(not(feature = "gst"))]
    use symphonia::core::probe::Hint;

    fn collect_session_columns(
        rx: &Receiver<AnalysisEvent>,
    ) -> std::collections::BTreeMap<u32, Vec<u8>> {
        let mut columns = std::collections::BTreeMap::new();
        loop {
            let event = rx
                .recv_timeout(Duration::from_secs(2))
                .expect("worker output before timeout");
            if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event {
                if chunk.complete {
                    return columns;
                }
                let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
                for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                    columns.insert(
                        chunk.start_column_index + u32::try_from(offset).unwrap(),
                        column.to_vec(),
                    );
                }
            }
        }
    }

    fn write_seek_grid_fixture() -> PathBuf {
        let path =
            std::env::temp_dir().join(format!("ferrous-seek-grid-{}.wav", std::process::id()));
        let data_size = 48_000u32 * 4;
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&(36 + data_size).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0\x02\0");
        bytes.extend_from_slice(&48_000u32.to_le_bytes());
        bytes.extend_from_slice(&192_000u32.to_le_bytes());
        bytes.extend_from_slice(b"\x04\0\x10\0data");
        bytes.extend_from_slice(&data_size.to_le_bytes());
        let mut random = 0x1234_5678u32;
        for _ in 0..96_000 {
            random = random.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            bytes.extend_from_slice(&u16::try_from(random >> 16).unwrap().to_le_bytes());
        }
        std::fs::write(&path, bytes).unwrap();
        path
    }

    #[test]
    fn fractional_zoom_column_rate_matches_the_published_integer_hop() {
        for zoom in [0.05, 0.17, 0.77, 1.0] {
            let hop = 1_024;
            let interval = output_interval_for_hop(hop, zoom);
            let output_hop = effective_hop_from_interval(hop, interval);
            let mut resampler = PeakHoldResampler::new(interval);
            let count = (0..100_000)
                .filter(|_| resampler.push(&[1.0]).is_some())
                .count();
            assert_eq!(count, 100_000 * hop / output_hop, "zoom {zoom}");
        }
    }

    #[test]
    fn explicit_and_initial_seeks_match_continuous_absolute_columns() {
        let path = write_seek_grid_fixture();
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            for (fft, zoom) in [(512, 1.1), (2_048, 0.77), (8_192, 0.73)] {
                let hop = super::super::zoom_hop_size(fft, zoom);
                let (commands, command_rx) = unbounded();
                let (events, event_rx) = unbounded();
                let command = SpectrogramWorkerCommand::NewTrack {
                    track_token: 1,
                    generation: 1,
                    path: path.clone(),
                    fft_size: fft,
                    hop_size: hop,
                    zoom_level: zoom,
                    widget_width: 320,
                    channel_count: 2,
                    start_seconds: 0.0,
                    target_position_seconds: 0.0,
                    emit_initial_reset: true,
                    clear_history_on_reset: true,
                    view_mode: SpectrogramViewMode::PerChannel,
                    display_mode: mode,
                    handoff: SessionHandoff::default(),
                };
                let initial = command.clone();
                let worker = std::thread::spawn(move || {
                    run_spectrogram_session(
                        QueuedCommand::alone(command),
                        &command_rx,
                        &events,
                        &AtomicU64::new(1),
                        &AtomicU64::new(0),
                        &AtomicU64::new(0),
                        &unbounded().0,
                    )
                });
                let reference = collect_session_columns(&event_rx);
                for seek in [0.001, 0.231_234, 0.731_234, 0.031_234]
                    .into_iter()
                    .cycle()
                    .take(16)
                {
                    commands
                        .send(SpectrogramWorkerCommand::Seek {
                            position_seconds: seek,
                        })
                        .unwrap();
                    let actual = collect_session_columns(&event_rx);
                    assert!(!actual.is_empty());
                    for (index, bytes) in actual {
                        assert_eq!(
                            Some(&bytes),
                            reference.get(&index),
                            "{mode:?} fft {fft} zoom {zoom} seek {seek} column {index}"
                        );
                    }
                }
                commands
                    .send(SpectrogramWorkerCommand::StopSession)
                    .unwrap();
                worker.join().unwrap();
                let mut initial = initial;
                if let SpectrogramWorkerCommand::NewTrack {
                    start_seconds,
                    target_position_seconds,
                    ..
                } = &mut initial
                {
                    *start_seconds = 0.231_234;
                    *target_position_seconds = 0.231_234;
                }
                let (commands, command_rx) = unbounded();
                let (events, event_rx) = unbounded();
                let worker = std::thread::spawn(move || {
                    run_spectrogram_session(
                        QueuedCommand::alone(initial),
                        &command_rx,
                        &events,
                        &AtomicU64::new(1),
                        &AtomicU64::new(0),
                        &AtomicU64::new(0),
                        &unbounded().0,
                    )
                });
                let actual = collect_session_columns(&event_rx);
                commands
                    .send(SpectrogramWorkerCommand::StopSession)
                    .unwrap();
                worker.join().unwrap();
                assert!(!actual.is_empty());
                for (index, bytes) in actual {
                    assert_eq!(Some(&bytes), reference.get(&index));
                }
            }
        }
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn synchronized_stft_does_not_consume_a_channel_before_all_are_ready() {
        let mut stfts = vec![StftComputer::new(512, 1_024), StftComputer::new(512, 1_024)];
        let mut resamplers = vec![PeakHoldResampler::new(1.0), PeakHoldResampler::new(1.0)];
        let mut bytes = Vec::new();
        stfts[0].enqueue_samples(&[0.25; 512]);
        assert_eq!(
            append_stft_column(&mut stfts, &mut resamplers, &mut bytes, 257, 512),
            None
        );
        assert!(stfts[0].has_row());
        stfts[1].enqueue_samples(&[0.25; 512]);
        assert_eq!(
            append_stft_column(&mut stfts, &mut resamplers, &mut bytes, 257, 512),
            Some(true)
        );
        assert_eq!(bytes.len(), 514);
        assert_eq!(bytes[..257], bytes[257..]);
        assert!(!stfts.iter().any(StftComputer::has_row));
    }

    #[test]
    fn explicit_seek_command_forces_seek_even_inside_lookahead() {
        let mut session = SpectrogramSessionState {
            track_token: 1,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 256,
            session_start_column: 0,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        };

        match handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::Seek {
                position_seconds: 3.0,
            },
        ) {
            SessionAction::SeekRequired { position_seconds } => {
                assert_eq!(position_seconds, 3.0);
            }
            _ => panic!("expected explicit seek to force a seek"),
        }
    }

    #[test]
    fn rolling_mode_caps_chunk_growth_for_ui_smoothness() {
        assert_eq!(
            next_target_chunk_columns(1, SpectrogramDisplayMode::Rolling),
            2
        );
        assert_eq!(
            next_target_chunk_columns(32, SpectrogramDisplayMode::Rolling),
            64
        );
        assert_eq!(
            next_target_chunk_columns(64, SpectrogramDisplayMode::Rolling),
            64
        );
        assert_eq!(
            next_target_chunk_columns(128, SpectrogramDisplayMode::Centered),
            256
        );
        assert_eq!(
            next_target_chunk_columns(256, SpectrogramDisplayMode::Centered),
            256
        );
    }

    #[test]
    fn centered_mode_caps_chunk_growth_by_payload_bytes() {
        // 256 centered columns at 1025 bins and 2 channels is about
        // 512 KiB, which shows up as UI-thread dispatch spikes.
        assert_eq!(
            next_target_chunk_columns_for_payload(128, SpectrogramDisplayMode::Centered, 1_025, 2),
            64
        );
        assert_eq!(
            next_target_chunk_columns_for_payload(32, SpectrogramDisplayMode::Centered, 1_025, 2),
            64
        );
        assert_eq!(
            next_target_chunk_columns_for_payload(32, SpectrogramDisplayMode::Rolling, 1_025, 2),
            64
        );
    }

    #[test]
    fn post_reset_unthrottled_columns_cover_first_rolling_full_chunk() {
        assert_eq!(
            post_reset_unthrottled_columns(SpectrogramDisplayMode::Rolling),
            127
        );
        assert_eq!(
            post_reset_unthrottled_columns(SpectrogramDisplayMode::Centered),
            0
        );
    }

    #[test]
    fn post_reset_window_tracks_decoded_columns() {
        let mut session = SpectrogramSessionState {
            track_token: 7,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 400,
            session_start_column: 400,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 400,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: post_reset_unthrottled_columns(
                SpectrogramDisplayMode::Rolling,
            ),
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        };

        assert!(post_reset_window_active(&session));
        session.columns_produced = session.session_start_column + 126;
        assert!(post_reset_window_active(&session));
        session.columns_produced = session.session_start_column + 127;
        assert!(!post_reset_window_active(&session));
    }

    #[test]
    fn session_flush_chunk_advances_start_index_for_following_chunk() {
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let mut session = SpectrogramSessionState {
            track_token: 7,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        };

        let cols_out = AtomicU64::new(0);
        session_flush_chunk(&mut session, &event_tx, &cols_out);

        let chunk = event_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("flushed chunk");
        match chunk {
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                assert_eq!(chunk.start_column_index, 256);
                assert_eq!(chunk.column_count, 1);
            }
            other => panic!("expected precomputed chunk, got {other:?}"),
        }
        assert_eq!(session.chunk_columns, 0);
        assert_eq!(session.chunk_start_index, 320);
    }

    #[test]
    fn rolling_mode_keeps_partial_chunk_buffered_while_parked() {
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let mut session = SpectrogramSessionState {
            track_token: 7,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 64,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        };

        let cols_out = AtomicU64::new(0);
        flush_chunk_before_lookahead_park(&mut session, &event_tx, &cols_out);

        assert!(event_rx.try_recv().is_err());
        assert_eq!(session.chunk_columns, 1);
        assert_eq!(session.chunk_start_index, 256);
        assert_eq!(session.chunk_buf, vec![1, 2, 3, 4]);
    }

    #[test]
    fn centered_mode_flushes_partial_chunk_before_park() {
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let mut session = SpectrogramSessionState {
            track_token: 7,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 64,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: f64::INFINITY,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        };

        let cols_out = AtomicU64::new(0);
        flush_chunk_before_lookahead_park(&mut session, &event_tx, &cols_out);

        let chunk = event_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("flushed centered chunk");
        match chunk {
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                assert_eq!(chunk.start_column_index, 256);
                assert_eq!(chunk.column_count, 1);
            }
            other => panic!("expected precomputed chunk, got {other:?}"),
        }
        assert_eq!(session.chunk_columns, 0);
        assert_eq!(session.chunk_start_index, 320);
        assert!(session.chunk_buf.is_empty());
    }

    #[test]
    fn centered_mode_park_preserves_partial_resampler_interval() {
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.resamplers = vec![PeakHoldResampler::new(4.0)];

        let spectrum = [1.0_f32];
        for _ in 0..3 {
            assert!(session.resamplers[0].push(&spectrum).is_none());
        }

        let cols_out = AtomicU64::new(0);
        flush_chunk_before_lookahead_park(&mut session, &event_tx, &cols_out);

        assert!(event_rx.try_recv().is_err());
        assert_eq!(session.columns_produced, 256);
        assert!(
            session.resamplers[0].push(&spectrum).is_some(),
            "parking must not turn a partial four-row interval into an extra column"
        );
    }

    /// Helper: creates a minimal `SpectrogramSessionState` for command tests.
    fn make_test_session() -> SpectrogramSessionState {
        SpectrogramSessionState {
            track_token: 1,
            path: PathBuf::new(),
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            zoom_level: 1.0,
            widget_width: 1920,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 256,
            session_start_column: 0,
            stfts: Vec::new(),
            resamplers: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            throttle_origin: None,
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            lookahead_seconds: 10.0,
            source_reached_eof: false,
            timeline: AudioTimeline::new(0),
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
            rejected_continuation: None,
            failure: None,
            queued_commands: VecDeque::new(),
        }
    }

    #[test]
    fn continued_file_republishes_track_duration() {
        // Compatible ContinueWithFile handoffs do not create a new worker
        // session.  Publishing the next file's estimate must therefore
        // replace the zero written by handle_track_change rather than leave
        // centered restart math in its unknown-duration fallback.
        let published_duration_ms = AtomicU64::new(0);

        publish_track_duration_ms(2_000, 48_000, &published_duration_ms);
        assert_eq!(published_duration_ms.load(Ordering::Relaxed), 42_666);

        publish_track_duration_ms(4_000, 48_000, &published_duration_ms);
        assert_eq!(published_duration_ms.load(Ordering::Relaxed), 85_333);
    }

    #[test]
    fn continue_file_stored_mid_session() {
        let mut session = make_test_session();

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::ContinueWithFile {
                path: PathBuf::from("/tmp/next.flac"),
                track_token: 42,
            },
        );

        assert!(matches!(action, SessionAction::Continue));
        assert!(session.pending_continue.is_some());
        let (path, token) = session.pending_continue.unwrap();
        assert_eq!(path, PathBuf::from("/tmp/next.flac"));
        assert_eq!(token, 42);
    }

    #[test]
    fn new_track_supersedes_pending_continue() {
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/old.flac"), 10));

        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        // Send ContinueWithFile then NewTrack.
        tx.send(SpectrogramWorkerCommand::ContinueWithFile {
            path: PathBuf::from("/tmp/next.flac"),
            track_token: 20,
        })
        .unwrap();
        tx.send(SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 2,
            path: PathBuf::from("/tmp/manual.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 1.0,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            handoff: SessionHandoff::default(),
        })
        .unwrap();

        let action = process_session_commands(&mut session, &rx);
        // NewTrack must take priority.
        assert!(matches!(action, Some(SessionAction::NewSession(_))));
    }

    #[test]
    fn process_session_commands_coalesces_queued_new_tracks_to_latest() {
        let mut session = make_test_session();

        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 2,
            path: PathBuf::from("/tmp/intermediate.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 0.5,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        })
        .unwrap();
        tx.send(SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 3,
            path: PathBuf::from("/tmp/final.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 0.1,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        })
        .unwrap();

        // The session ends at the first; the worker then takes the latest.
        let Some(SessionAction::NewSession(first)) = process_session_commands(&mut session, &rx)
        else {
            panic!("expected the first queued NewTrack");
        };
        let Some(QueuedCommand {
            cmd:
                SpectrogramWorkerCommand::NewTrack {
                    generation,
                    zoom_level,
                    ..
                },
            behind: queued,
        }) = coalesce_queued_new_track_command(first, &rx, &AtomicU64::new(3))
        else {
            panic!("expected latest queued NewTrack");
        };
        assert_eq!(generation, 3);
        assert!((zoom_level - 0.1).abs() < f32::EPSILON);
        assert!(queued.is_empty(), "{queued:?}");
    }

    #[test]
    fn idle_worker_coalesces_queued_new_tracks_before_starting_session() {
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        let generation_state = AtomicU64::new(3);
        let initial = SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 2,
            path: PathBuf::from("/tmp/intermediate.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 0.5,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        };
        tx.send(SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 3,
            path: PathBuf::from("/tmp/final.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 0.1,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        })
        .unwrap();

        let QueuedCommand { cmd, .. } =
            coalesce_queued_new_track_command(initial, &rx, &generation_state)
                .expect("latest queued generation should still be live");
        let SpectrogramWorkerCommand::NewTrack {
            generation,
            zoom_level,
            ..
        } = cmd
        else {
            panic!("expected latest queued NewTrack");
        };
        assert_eq!(generation, 3);
        assert!((zoom_level - 0.1).abs() < f32::EPSILON);
    }

    #[test]
    fn idle_worker_drops_stale_new_track_before_starting_session() {
        let (_tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        let generation_state = AtomicU64::new(3);
        let initial = SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation: 2,
            path: PathBuf::from("/tmp/intermediate.flac"),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 0.5,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        };

        let cmd = coalesce_queued_new_track_command(initial, &rx, &generation_state);
        assert!(
            cmd.is_none(),
            "stale NewTrack generations should not open files or emit reset chunks"
        );
    }

    // ---------------------------------------------------------------
    // UpdateTrackToken / CancelPendingContinue worker command tests
    // ---------------------------------------------------------------

    #[test]
    fn update_track_token_before_eof_updates_pending_only() {
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/next.flac"), 10));

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::UpdateTrackToken { track_token: 20 },
        );

        assert!(matches!(action, SessionAction::Continue));
        // Session token must be unchanged (still old token).
        assert_eq!(session.track_token, 1);
        // Pending token must be updated.
        let (_, token) = session.pending_continue.unwrap();
        assert_eq!(token, 20);
    }

    #[test]
    fn update_track_token_after_eof_updates_session_and_returns_flush() {
        let mut session = make_test_session();
        // No pending_continue — continuation already consumed.

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::UpdateTrackToken { track_token: 20 },
        );

        assert!(
            matches!(action, SessionAction::FlushToken),
            "expected FlushToken when pending_continue is None"
        );
        assert_eq!(session.track_token, 20);
    }

    #[test]
    fn update_widget_width_grows_centered_lookahead() {
        // Growing the widget width (e.g. fullscreen) must lift the
        // centered-mode lookahead so the decoder doesn't stay parked at
        // the old window's threshold and leave the right edge black.
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.widget_width = 1000;
        session.cols_per_second = 43.07;
        session.lookahead_seconds = 10.0;
        session.lookahead_columns = 2000 + 431; // 1000*2 + 10*43.07

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: 3840 },
        );

        assert!(matches!(action, SessionAction::Continue));
        assert_eq!(session.widget_width, 3840);
        // new lookahead = 3840*2 + 10*43.07 ≈ 8111.
        assert!(
            session.lookahead_columns > 8_000 && session.lookahead_columns < 8_200,
            "lookahead did not grow to cover the new width: {}",
            session.lookahead_columns
        );
    }

    #[test]
    fn update_widget_width_ignored_when_shrinking() {
        // Shrinking the widget must not shrink the running lookahead —
        // the existing buffer is fine and we don't want to discard
        // already-decoded cols.
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.widget_width = 3840;
        session.lookahead_columns = 8_111;

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: 1200 },
        );

        assert!(matches!(action, SessionAction::Continue));
        assert_eq!(session.widget_width, 3840);
        assert_eq!(session.lookahead_columns, 8_111);
    }

    #[test]
    fn update_widget_width_remembers_rolling_width_for_mode_switch() {
        // Record rolling resizes so a later centered mode switch uses the
        // same viewport width as the UI.
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Rolling;
        session.widget_width = 1000;
        session.lookahead_columns = 430;

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: 3840 },
        );

        assert!(matches!(action, SessionAction::Continue));
        assert_eq!(session.widget_width, 3840);
        assert_eq!(session.lookahead_columns, 468);
    }

    #[test]
    fn duration_requery_estimate_scales_to_effective_hop() {
        // Regression: the GStreamer duration re-query computed new_est
        // as effective_frames/REFERENCE_HOP + 64 and compared it to
        // session.total_columns_estimate (stored in effective_hop
        // cols).  For zoom-out sessions (effective_hop > REFERENCE_HOP)
        // the ref-hop figure was always larger than the correctly-
        // scaled in-session estimate, so the estimate got inflated by
        // effective_hop/REFERENCE_HOP — AC3 at zoom ≈ 0.17 (effective
        // hop ≈ 6104) turned a correct ~2370-col estimate into a bogus
        // 16273, which then broke Qt's minimumZoomLevel calc and
        // trapped the user at a fixed zoom across repeated zoom-out
        // attempts.  Pipe new_est through scale_columns_estimate.
        //
        // Numbers mirror the diagnostics: a ~5-minute AC3 track at
        // 48 kHz stereo-after-downmix (divisor 1) has
        // effective_frames = 5 * 60 * 48_000 = 14_400_000.
        let effective_frames: u64 = 14_400_000;
        let reference_est = duration_requery_estimate_for_session(effective_frames, REFERENCE_HOP);
        // Reference-hop gives the raw ~14125 figure + 64 padding.
        assert!(
            (14_000..=14_200).contains(&reference_est),
            "reference-hop est out of expected range: {}",
            reference_est,
        );

        // Zoom-out: effective_hop ≈ 6104 → est in effective-hop cols
        // must be strictly smaller than the reference-hop count, in the
        // same ratio scale_columns_estimate uses for a fresh session.
        let zoom_out_est = duration_requery_estimate_for_session(effective_frames, 6_104);
        assert!(
            zoom_out_est < reference_est,
            "zoom-out est ({}) should scale below reference-hop est ({})",
            zoom_out_est,
            reference_est,
        );
        // Compare against what scale_columns_estimate(initial_open_file_cols, 6104)
        // would produce for the same input.  Without the fix the
        // zoom-out est would be ≈ reference_est (i.e. inflated).
        let expected_scaled = scale_columns_estimate(reference_est, 6_104);
        assert_eq!(zoom_out_est, expected_scaled);
    }

    #[test]
    fn session_emit_finalize_chunk_carries_cols_produced_and_complete_flag() {
        // Regression: the finalize chunk must carry the session's
        // columns_produced as total_columns_estimate and complete=true
        // so the Qt-side handler recognises it and shrinks its estimate
        // only for a session that actually consumed the source.
        let mut session = make_test_session();
        session.track_token = 42;
        session.columns_produced = 7_777;
        session.bins_per_column = 1_025;
        session.channel_count = 2;
        session.effective_rate = 44_100;
        session.effective_hop = 64;

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        session_emit_finalize_chunk(&session, &event_tx);
        let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) =
            event_rx.try_recv().expect("finalize event")
        else {
            panic!("expected a PrecomputedSpectrogramChunk event")
        };
        assert!(chunk.complete);
        assert_eq!(chunk.column_count, 0);
        assert_eq!(chunk.track_token, 42);
        assert_eq!(chunk.total_columns_estimate, 7_777);
        assert_eq!(chunk.start_column_index, 7_777);
        assert!(!chunk.buffer_reset);
        assert!(!chunk.clear_history);
    }

    #[test]
    fn source_reached_eof_defaults_to_false() {
        // Regression guard: a freshly-built session must not claim to
        // have reached source EOF, otherwise a stale-generation
        // termination (None return before decoding finishes the file)
        // would be treated as real EOF and emit a spurious finalize
        // chunk clobbering the next session's total_columns_estimate.
        let session = make_test_session();
        assert!(!session.source_reached_eof);
    }

    #[test]
    fn cancel_pending_continue_with_pending() {
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/next.flac"), 10));

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::CancelPendingContinue,
        );

        assert!(matches!(action, SessionAction::Continue));
        assert!(session.pending_continue.is_none());
    }

    #[test]
    fn cancel_pending_continue_without_pending_is_noop() {
        let mut session = make_test_session();
        assert!(session.pending_continue.is_none());

        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::CancelPendingContinue,
        );

        assert!(matches!(action, SessionAction::Continue));
        assert!(session.pending_continue.is_none());
    }

    #[test]
    fn process_session_commands_update_track_token_before_eof() {
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/next.flac"), 10));

        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 20 })
            .unwrap();
        drop(tx);

        let action = process_session_commands(&mut session, &rx);
        assert!(action.is_none()); // No seek/stop/new-session triggered.
        assert_eq!(session.track_token, 1); // Unchanged.
        let (_, token) = session.pending_continue.unwrap();
        assert_eq!(token, 20);
    }

    #[test]
    fn process_session_commands_cancel_pending_continue() {
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/next.flac"), 10));

        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::CancelPendingContinue)
            .unwrap();
        drop(tx);

        let action = process_session_commands(&mut session, &rx);
        assert!(action.is_none());
        assert!(session.pending_continue.is_none());
    }

    #[test]
    fn seek_preserves_pending_continue() {
        // An intra-track seek should NOT clear pending_continue.
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/tmp/next.flac"), 42));

        // Position update within lookahead should not touch pending_continue.
        let action = handle_single_command(
            &mut session,
            SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: 3.0,
            },
        );
        assert!(matches!(action, SessionAction::Continue));
        assert!(session.pending_continue.is_some());
    }

    fn test_new_track(path: &str, generation: u64) -> SpectrogramWorkerCommand {
        SpectrogramWorkerCommand::NewTrack {
            track_token: 30,
            generation,
            path: PathBuf::from(path),
            fft_size: 2_048,
            hop_size: 256,
            zoom_level: 1.0,
            widget_width: 1920,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            handoff: SessionHandoff::default(),
        }
    }

    fn new_track_path(cmd: &SpectrogramWorkerCommand) -> &Path {
        match cmd {
            SpectrogramWorkerCommand::NewTrack { path, .. } => path,
            other => panic!("expected NewTrack, got {other:?}"),
        }
    }

    #[test]
    fn renames_reach_files_the_worker_has_not_opened_yet() {
        let renames = |pairs: &[(&str, &str)]| {
            SpectrogramWorkerCommand::RemapPaths(Arc::new(
                pairs
                    .iter()
                    .map(|(old, new)| (PathBuf::from(old), PathBuf::from(new)))
                    .collect(),
            ))
        };

        // A parked session opens its continuation at the new path.
        let mut session = make_test_session();
        session.pending_continue = Some((PathBuf::from("/music/next.flac"), 42));
        let action = handle_single_command(
            &mut session,
            renames(&[("/music/next.flac", "/music/02.flac")]),
        );
        assert!(matches!(action, SessionAction::Continue));
        assert_eq!(
            session.pending_continue,
            Some((PathBuf::from("/music/02.flac"), 42))
        );

        // A session queued before the renames starts at the new path, one
        // queued after them at the path it names. In this swap, renaming
        // the later one would give it the other file.
        let swap = [
            ("/music/a.flac", "/music/b.flac"),
            ("/music/b.flac", "/music/a.flac"),
        ];
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(test_new_track("/music/a.flac", 1)).unwrap();
        tx.send(renames(&swap)).unwrap();
        let Some(SessionAction::NewSession(cmd)) = process_session_commands(&mut session, &rx)
        else {
            panic!("expected the queued session");
        };
        let QueuedCommand { cmd, .. } =
            coalesce_queued_new_track_command(cmd, &rx, &AtomicU64::new(1))
                .expect("current session");
        assert_eq!(new_track_path(&cmd), Path::new("/music/b.flac"));
        tx.send(renames(&swap)).unwrap();
        tx.send(test_new_track("/music/a.flac", 2)).unwrap();
        let Some(SessionAction::NewSession(cmd)) = process_session_commands(&mut session, &rx)
        else {
            panic!("expected the queued session");
        };
        assert_eq!(new_track_path(&cmd), Path::new("/music/a.flac"));

        // So does a session waiting for an idle worker.
        let generation = AtomicU64::new(3);
        tx.send(renames(&swap)).unwrap();
        let QueuedCommand { cmd, .. } =
            coalesce_queued_new_track_command(test_new_track("/music/a.flac", 3), &rx, &generation)
                .expect("current session");
        assert_eq!(new_track_path(&cmd), Path::new("/music/b.flac"));
    }

    // ---------------------------------------------------------------
    // Centered mode behavior tests
    // ---------------------------------------------------------------

    #[test]
    fn apply_display_mode_centered_sets_unlimited_rate_and_windowed_lookahead() {
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Rolling;
        session.decode_rate_limit = 2.0;
        session.lookahead_columns = 512;

        assert!(session.decode_rate_limit.is_finite());
        assert_eq!(session.lookahead_columns, 512);

        apply_display_mode(&mut session, SpectrogramDisplayMode::Centered);

        assert!(session.decode_rate_limit.is_infinite());
        // Windowed centered: ~3 screen widths + lookahead, not u64::MAX.
        assert!(session.lookahead_columns > 512);
        assert!(session.lookahead_columns < u64::MAX);
        assert_eq!(session.display_mode, SpectrogramDisplayMode::Centered);
    }

    #[test]
    fn apply_display_mode_rolling_restores_finite_rate_and_lookahead() {
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.decode_rate_limit = f64::INFINITY;
        session.lookahead_columns = u64::MAX;

        apply_display_mode(&mut session, SpectrogramDisplayMode::Rolling);

        assert!(session.decode_rate_limit.is_finite());
        assert!(session.lookahead_columns < u64::MAX);
        assert_eq!(session.display_mode, SpectrogramDisplayMode::Rolling);
    }

    #[test]
    fn timestamped_gaps_and_overlaps_preserve_absolute_spectral_columns() {
        let mut packets = vec![
            AudioFrames {
                samples: vec![0.5; 1024],
                first_frame: 0,
                frames: 1024,
                channels: 1,
            },
            AudioFrames {
                samples: vec![-0.5; 1024],
                first_frame: 4096,
                frames: 1024,
                channels: 1,
            },
            AudioFrames {
                samples: vec![0.25; 1024],
                first_frame: 4608,
                frames: 1024,
                channels: 1,
            },
        ]
        .into_iter();
        let mut timeline = AudioTimeline::new(0);
        let mut actual = StftComputer::new(512, 128);
        loop {
            match timeline
                .read(|| Ok(packets.next().map_or(AudioRead::Eof, AudioRead::Frames)))
                .unwrap()
            {
                AudioRead::Frames(frames) => actual.enqueue_samples(&frames.samples),
                AudioRead::Pending => {}
                AudioRead::Eof => break,
            }
        }
        let mut expected = StftComputer::new(512, 128);
        let samples: Vec<_> = std::iter::repeat_n(0.5, 1024)
            .chain(std::iter::repeat_n(0.0, 3072))
            .chain(std::iter::repeat_n(-0.5, 1024))
            .chain(std::iter::repeat_n(0.25, 512))
            .collect();
        expected.enqueue_samples(&samples);
        let mut columns = 0;
        while let Some(row) = expected.take_row() {
            assert_eq!(actual.take_row(), Some(row));
            columns += 1;
        }
        assert_eq!(columns, 41);
        assert!(!actual.has_row());
    }

    #[test]
    fn packet_drain_stops_at_lookahead_and_resumes_without_losing_rows() {
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let samples: Vec<_> = (0..10_000_u32).map(|i| (i as f32 * 0.07).sin()).collect();
            let mut reference = StftComputer::new(512, 128);
            reference.enqueue_samples(&samples);
            let mut expected = Vec::new();
            while let Some(row) = reference.take_row() {
                append_quantized_row(&mut expected, row, 257, 512);
            }
            let mut session = make_test_session();
            session.display_mode = mode;
            session.fft_size = 512;
            session.hop_size = 128;
            session.effective_hop = 128;
            session.bins_per_column = 257;
            session.target_position_seconds = 0.0;
            session.columns_produced = 0;
            session.chunk_start_index = 0;
            session.lookahead_columns = 3;
            session.stfts = vec![StftComputer::new(512, 128)];
            session.stfts[0].enqueue_samples(&samples);
            session.resamplers = vec![PeakHoldResampler::new(1.0)];
            let (tx, rx) = unbounded();
            let produced = AtomicU64::new(0);
            session_drain_stft_rows(&mut session, &tx, &produced);
            assert_eq!(session.columns_produced, 3);
            assert!(session.stfts[0].has_row());
            while session.stfts[0].has_row() {
                session.target_position_seconds =
                    usize_to_f64_approx(usize::try_from(session.columns_produced).unwrap())
                        / session.cols_per_second;
                session_drain_stft_rows(&mut session, &tx, &produced);
            }
            session_flush_chunk(&mut session, &tx, &produced);
            let actual: Vec<_> = rx
                .try_iter()
                .filter_map(|event| match event {
                    AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => Some(chunk.columns_u8),
                    _ => None,
                })
                .flatten()
                .collect();
            assert_eq!(actual, expected);
        }
    }

    #[test]
    fn worker_columns_produced_updated_on_flush() {
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let cols_out = AtomicU64::new(0);
        let mut session = make_test_session();
        session.track_token = 7;
        session.columns_produced = 320;
        session.session_start_column = 0;
        session.chunk_buf = vec![0u8; 1_025];
        session.chunk_columns = 1;
        session.chunk_start_index = 319;

        assert_eq!(cols_out.load(Ordering::Relaxed), 0);
        session_flush_chunk(&mut session, &event_tx, &cols_out);
        // After flush, the atomic should reflect columns_produced.
        assert_eq!(cols_out.load(Ordering::Relaxed), 320);
    }

    #[test]
    fn centered_staging_stops_at_byte_budget_with_partial_batch() {
        let fft_size = 512;
        let bins = fft_size / 2 + 1;
        // A limit of three ends inside the normal 1, 2, 4... batching sequence
        // when starting with a four-column batch. Keep the receiver unread to
        // reproduce predecode waiting for a gapless handoff.
        for (limit, target) in [(3, 4), (9, 1)] {
            let mut stfts: Vec<_> = (0..2)
                .map(|_| {
                    let mut stft = StftComputer::new(fft_size, 128);
                    stft.enqueue_samples(&vec![0.25; 32_768]);
                    stft
                })
                .collect();
            let mut resamplers: Vec<_> = (0..2).map(|_| PeakHoldResampler::new(1.0)).collect();
            let mut state = StagingChunkState {
                fft_size,
                bins_per_column: bins,
                channel_count: 2,
                effective_rate: 48_000,
                effective_hop: 128,
                total_columns_estimate: 1_000,
                max_columns: limit,
                columns_produced: 0,
                total_covered_samples: 32_768,
                chunk_buf: Vec::new(),
                chunk_columns: 0,
                chunk_start_index: 0,
                target_chunk_columns: target,
            };
            let (tx, rx) = unbounded();
            assert!(!staging_drain_stft_rows(
                &mut stfts,
                &mut resamplers,
                &mut state,
                &tx,
            ));
            assert!(stfts.iter().all(StftComputer::has_row));
            assert!(state.take_partial_chunk().is_none());
            assert!(!staging_drain_stft_rows(
                &mut stfts,
                &mut resamplers,
                &mut state,
                &tx,
            ));
            let mut next = 0;
            let mut bytes = 0;
            for chunk in rx.try_iter() {
                assert_eq!(u64::from(chunk.start_column_index), next);
                assert_eq!(chunk.track_token, 0);
                assert!(!chunk.complete, "a staged prefix is not EOF");
                assert_eq!(
                    chunk.columns_u8.len(),
                    usize::from(chunk.column_count) * bins * 2
                );
                next += u64::from(chunk.column_count);
                bytes += chunk.columns_u8.len();
            }
            assert_eq!(next, limit);
            assert_eq!(usize_to_u64(bytes), limit * usize_to_u64(bins * 2));
        }
    }

    #[test]
    fn centered_staging_budget_accounts_for_fft_and_all_channels() {
        for bins in [257, 1_025, 4_097] {
            for channels in [1, 2, 8, 255] {
                let limit = staging_column_limit(bins, channels);
                let bytes_per_column = usize_to_u64(bins * channels);
                let budget = usize_to_u64(CENTERED_STAGING_BYTE_BUDGET);
                assert!(limit > 0);
                assert!(limit * bytes_per_column <= budget);
                assert!((limit + 1) * bytes_per_column > budget);
            }
        }
        assert_eq!(staging_column_limit(usize::MAX, 2), 0);
    }

    /// Simulates a Symphonia source for tests that need `maybe_update_columns_estimate`.
    #[cfg(not(feature = "gst"))]
    fn make_dummy_source() -> AudioFrameSource {
        let hint = Hint::new();
        // We never call next_frames(), so a dummy Symphonia variant is fine.
        // Build the minimum valid state — all fields will be unused.
        AudioFrameSource::Symphonia {
            format: {
                // Create a minimal in-memory source that Symphonia can probe.
                // Since we never actually decode, we just need a valid instance.
                // Use a short WAV header for a silent 1-sample file.
                let wav_header: &[u8] = &[
                    0x52, 0x49, 0x46, 0x46, // "RIFF"
                    0x2E, 0x00, 0x00, 0x00, // chunk size (46 bytes after this)
                    0x57, 0x41, 0x56, 0x45, // "WAVE"
                    0x66, 0x6D, 0x74, 0x20, // "fmt "
                    0x10, 0x00, 0x00, 0x00, // subchunk1 size (16)
                    0x01, 0x00, // PCM format
                    0x01, 0x00, // 1 channel
                    0x80, 0xBB, 0x00, 0x00, // 48000 Hz
                    0x00, 0x77, 0x01, 0x00, // byte rate
                    0x02, 0x00, // block align
                    0x10, 0x00, // 16 bits/sample
                    0x64, 0x61, 0x74, 0x61, // "data"
                    0x02, 0x00, 0x00, 0x00, // data size (2 bytes = 1 sample)
                    0x00, 0x00, // one silent sample
                ];
                let cursor = std::io::Cursor::new(wav_header.to_vec());
                let mss =
                    MediaSourceStream::new(Box::new(cursor), MediaSourceStreamOptions::default());
                symphonia::default::get_probe()
                    .format(
                        &hint,
                        mss,
                        &FormatOptions::default(),
                        &MetadataOptions::default(),
                    )
                    .unwrap()
                    .format
            },
            decoder: {
                let wav_header2: &[u8] = &[
                    0x52, 0x49, 0x46, 0x46, 0x2E, 0x00, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66,
                    0x6D, 0x74, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x80, 0xBB,
                    0x00, 0x00, 0x00, 0x77, 0x01, 0x00, 0x02, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74,
                    0x61, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                ];
                let cursor2 = std::io::Cursor::new(wav_header2.to_vec());
                let mss2 =
                    MediaSourceStream::new(Box::new(cursor2), MediaSourceStreamOptions::default());
                let probed = symphonia::default::get_probe()
                    .format(
                        &Hint::new(),
                        mss2,
                        &FormatOptions::default(),
                        &MetadataOptions::default(),
                    )
                    .unwrap();
                let track = probed.format.default_track().unwrap();
                symphonia::default::get_codecs()
                    .make(&track.codec_params, &DecoderOptions::default())
                    .unwrap()
            },
            track_id: 0,
            seek_frame: 0,
        }
    }

    #[test]
    #[cfg(not(feature = "gst"))]
    fn columns_estimate_holds_when_approaching_without_overshoot() {
        // Regression for zoom-out blank-post-seek: the doubling used
        // to fire at the 75% threshold, which inflated a correctly-
        // scaled zoom-out estimate as the decoder finished the file
        // (produced climbs naturally toward the real total).  For
        // bounded sources the estimate already includes STFT tail
        // padding, so doubling while still within the estimate is
        // always spurious.
        let mut session = make_test_session();
        session.total_columns_estimate = 1000;
        session.columns_produced = 600;
        let source = make_dummy_source();

        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(
            session.total_columns_estimate, 1000,
            "should not grow below estimate"
        );

        session.columns_produced = 750;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(
            session.total_columns_estimate, 1000,
            "75% threshold no longer triggers doubling"
        );

        session.columns_produced = 1000;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(
            session.total_columns_estimate, 1000,
            "AT the estimate must not trigger doubling — bounded \
             sources legitimately stop here"
        );
    }

    #[test]
    #[cfg(not(feature = "gst"))]
    fn columns_estimate_doubles_when_overshooting() {
        // Unbounded sources (the decoders.rs 300-second fallback for
        // missing-duration metadata) legitimately produce more cols
        // than the estimate.  The safety net must still fire when
        // produced strictly exceeds the estimate.
        let mut session = make_test_session();
        session.total_columns_estimate = 1000;
        let source = make_dummy_source();

        session.columns_produced = 1001;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(
            session.total_columns_estimate, 2000,
            "overshoot by 1 col must double"
        );

        // Cross the next estimate too — safety net keeps firing until
        // it catches up with the real length.
        session.columns_produced = 2500;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(session.total_columns_estimate, 4000);
    }

    #[test]
    fn output_interval_is_continuous() {
        let i1 = output_interval_for_hop(1024, 1.0);
        assert!((i1 - 1.0).abs() < 0.001);

        let i2 = output_interval_for_hop(1024, 0.8);
        assert!((i2 - 1.25).abs() < 0.001);
        assert!(
            i2 > i1,
            "zoom=0.8 must produce a different interval than zoom=1.0"
        );

        let i3 = output_interval_for_hop(1024, 0.5);
        assert!((i3 - 2.0).abs() < 0.001);

        let i4 = output_interval_for_hop(128, 0.7);
        assert!((i4 - 11.43).abs() < 0.1);
    }

    /// Write a mono 16-bit square-wave WAV and return its path.
    fn write_tone_fixture(name: &str, seconds: u32, rate: u32) -> PathBuf {
        let path =
            std::env::temp_dir().join(format!("ferrous-session-{name}-{}.wav", std::process::id()));
        let frames = seconds * rate;
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&(36 + frames * 2).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0\x01\0");
        bytes.extend_from_slice(&rate.to_le_bytes());
        bytes.extend_from_slice(&(rate * 2).to_le_bytes());
        bytes.extend_from_slice(b"\x02\0\x10\0data");
        bytes.extend_from_slice(&(frames * 2).to_le_bytes());
        for frame in 0..frames {
            let sample: i16 = if frame % 64 < 32 { 8_000 } else { -8_000 };
            bytes.extend_from_slice(&sample.to_le_bytes());
        }
        std::fs::write(&path, bytes).expect("write fixture");
        path
    }

    fn session_on(path: &Path, track_token: u64, generation: u64) -> SpectrogramWorkerCommand {
        let mut command = test_new_track(&path.to_string_lossy(), generation);
        if let SpectrogramWorkerCommand::NewTrack {
            track_token: token,
            fft_size,
            hop_size,
            display_mode,
            ..
        } = &mut command
        {
            *token = track_token;
            *fft_size = 512;
            *hop_size = 1024;
            *display_mode = SpectrogramDisplayMode::Centered;
        }
        command
    }

    fn next_chunk(events: &Receiver<AnalysisEvent>) -> PrecomputedSpectrogramChunk {
        match events.recv_timeout(Duration::from_secs(10)) {
            Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) => chunk,
            other => panic!("expected a chunk, got {other:?}"),
        }
    }

    #[test]
    fn a_continuation_without_a_session_is_rejected_and_starts_nothing() {
        let path = write_tone_fixture("fallback", 1, 8_000);
        let missing = path.with_extension("missing.wav");
        // The continuation's file cannot be opened, and it arrives while the
        // session has ended and the worker is idle, or while it is parked
        // at EOF.  The analysis thread starts any session; the worker
        // starts none of its own.
        for ended_first in [true, false] {
            let generation = Arc::new(AtomicU64::new(1));
            let (commands, command_rx) = unbounded();
            let (events, event_rx) = unbounded();
            let (reports, report_rx) = unbounded();
            let worker_generation = Arc::clone(&generation);
            let worker = std::thread::spawn(move || {
                spectrogram_worker_loop(
                    &command_rx,
                    &events,
                    &worker_generation,
                    &AtomicU64::new(0),
                    &AtomicU64::new(0),
                    &reports,
                );
            });
            commands.send(session_on(&path, 1, 1)).unwrap();
            while !next_chunk(&event_rx).complete {}

            // The analysis thread seeks: a new generation, then its session.
            generation.store(2, Ordering::Relaxed);
            if ended_first {
                // Ends the stale session.
                commands
                    .send(SpectrogramWorkerCommand::PositionUpdate {
                        position_seconds: 0.5,
                    })
                    .unwrap();
            }
            commands
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: missing.clone(),
                    track_token: 2,
                })
                .unwrap();
            commands.send(session_on(&path, 3, 2)).unwrap();

            let seek_session = loop {
                let chunk = next_chunk(&event_rx);
                assert_ne!(chunk.track_token, 2, "ended_first={ended_first}");
                if chunk.track_token == 3 {
                    break chunk;
                }
            };
            assert_eq!(seek_session.generation, 2, "ended_first={ended_first}");
            assert!(seek_session.buffer_reset, "ended_first={ended_first}");
            assert_eq!(generation.load(Ordering::Relaxed), 2);
            drop(commands);
            worker.join().expect("worker");
            let rejected: Vec<_> = report_rx
                .try_iter()
                .filter(|report| matches!(report, SessionReport::ContinuationRejected { .. }))
                .collect();
            assert!(
                matches!(rejected.as_slice(), [SessionReport::ContinuationRejected {
                    path, format: None, ..
                }] if *path == missing),
                "ended_first={ended_first}: {rejected:?}"
            );
            assert!(
                event_rx.try_iter().all(|chunk| !matches!(chunk,
                    AnalysisEvent::PrecomputedSpectrogramChunk(chunk) if chunk.track_token == 2)),
                "ended_first={ended_first}"
            );
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_continuation_that_does_not_fit_is_reported_with_its_format() {
        let path = write_tone_fixture("continuation", 1, 8_000);
        let faster = write_tone_fixture("continuation-faster", 1, 16_000);
        let missing = path.with_extension("missing.wav");
        let cases = [
            (missing.clone(), None),
            (
                faster.clone(),
                Some(TrackFormat::Known {
                    sample_rate: 16_000,
                    channels: 1,
                }),
            ),
        ];
        for (next, format) in cases {
            // After the track switch, the session ends: positions on the
            // continued timeline do not fit a session of the new file.
            let generation = AtomicU64::new(4);
            let (commands, command_rx) = unbounded();
            let (events, event_rx) = unbounded();
            let (reports, report_rx) = unbounded();
            commands
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: next.clone(),
                    track_token: 2,
                })
                .unwrap();
            let session = session_on(&path, 1, 4);
            let ended = run_spectrogram_session(
                QueuedCommand::alone(session),
                &command_rx,
                &events,
                &generation,
                &AtomicU64::new(0),
                &AtomicU64::new(0),
                &reports,
            );
            assert!(ended.is_none(), "{ended:?}");
            assert_eq!(generation.load(Ordering::Relaxed), 4);
            let reports: Vec<_> = report_rx.try_iter().collect();
            assert!(
                matches!(reports.as_slice(), [
                    SessionReport::Opened { generation: 4, track_token: 1, .. },
                    SessionReport::ContinuationRejected { generation: 4, path: rejected, format: found, .. },
                ] if *rejected == next && *found == format),
                "{reports:?}"
            );
            assert!(event_rx.try_iter().all(|chunk| !matches!(chunk,
                AnalysisEvent::PrecomputedSpectrogramChunk(chunk) if chunk.track_token == 2)));

            // Before the switch, the outgoing track stays parked at its end
            // for seeks; a switch to the rejected file then ends it.
            let (commands, command_rx) = unbounded();
            let (events, event_rx) = unbounded();
            let (reports, report_rx) = unbounded();
            commands
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: next.clone(),
                    track_token: 1,
                })
                .unwrap();
            let session = session_on(&path, 1, 4);
            let session = std::thread::spawn(move || {
                run_spectrogram_session(
                    QueuedCommand::alone(session),
                    &command_rx,
                    &events,
                    &AtomicU64::new(4),
                    &AtomicU64::new(0),
                    &AtomicU64::new(0),
                    &reports,
                )
            });
            loop {
                match report_rx.recv_timeout(Duration::from_secs(10)) {
                    Ok(SessionReport::ContinuationRejected { path, .. }) if path == next => break,
                    Ok(_) => {}
                    Err(error) => panic!("no rejection: {error}"),
                }
            }
            commands
                .send(SpectrogramWorkerCommand::Seek {
                    position_seconds: 0.5,
                })
                .unwrap();
            loop {
                let chunk = next_chunk(&event_rx);
                if chunk.buffer_reset {
                    assert_eq!(chunk.track_token, 1);
                    break;
                }
            }
            commands
                .send(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 })
                .unwrap();
            assert!(session.join().expect("session").is_none());
            assert!(event_rx.try_iter().all(|chunk| !matches!(chunk,
                AnalysisEvent::PrecomputedSpectrogramChunk(chunk) if chunk.track_token == 2)));
        }
        std::fs::remove_file(path).expect("remove fixture");
        std::fs::remove_file(faster).expect("remove fixture");
    }

    #[test]
    fn a_centered_gapless_switch_finalizes_the_outgoing_track_before_its_prelude() {
        let path = write_tone_fixture("handoff", 30, 8_000);
        let generation = Arc::new(AtomicU64::new(1));
        let (commands, command_rx) = unbounded();
        // One slot, so the session cannot run ahead of this test.
        let (events, event_rx) = crossbeam_channel::bounded(1);
        let worker_generation = Arc::clone(&generation);
        let worker = std::thread::spawn(move || {
            spectrogram_worker_loop(
                &command_rx,
                &events,
                &worker_generation,
                &AtomicU64::new(0),
                &AtomicU64::new(0),
                &unbounded().0,
            );
        });
        commands.send(session_on(&path, 7, 1)).unwrap();
        let mut outgoing = vec![next_chunk(&event_rx)];
        while outgoing.iter().all(|chunk| chunk.column_count == 0) {
            outgoing.push(next_chunk(&event_rx));
        }

        let mut switch = session_on(&path, 8, 1);
        let staged = PrecomputedSpectrogramChunk {
            track_token: 8,
            generation: 1,
            columns_u8: vec![0; 257],
            bins_per_column: 257,
            column_count: 1,
            channel_count: 1,
            start_column_index: 0,
            total_columns_estimate: 300,
            sample_rate_hz: 8_000,
            hop_size: 1024,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: true,
        };
        if let SpectrogramWorkerCommand::NewTrack {
            emit_initial_reset,
            handoff,
            ..
        } = &mut switch
        {
            *emit_initial_reset = false;
            handoff.finalize_previous = true;
            handoff.prelude = vec![staged];
        }
        commands.send(switch).unwrap();

        let prelude = loop {
            let chunk = next_chunk(&event_rx);
            if chunk.track_token == 8 {
                break chunk;
            }
            outgoing.push(chunk);
        };
        assert!(prelude.buffer_reset && prelude.column_count == 1);
        let finalize = outgoing.last().expect("outgoing chunks");
        assert!(
            finalize.complete
                && finalize.column_count == 0
                && finalize.track_token == 7
                && finalize.generation == 1,
            "{finalize:?}"
        );
        let decoded: u32 = outgoing
            .iter()
            .map(|chunk| u32::from(chunk.column_count))
            .sum();
        assert_eq!(finalize.total_columns_estimate, decoded);
        // The new session's own chunks follow, without a reset.
        let next = next_chunk(&event_rx);
        assert!(next.track_token == 8 && !next.buffer_reset, "{next:?}");
        drop(commands);
        drop(event_rx);
        worker.join().expect("worker");
        std::fs::remove_file(path).expect("remove fixture");
    }

    // ---------------------------------------------------------------
    // Commands queued behind a NewTrack
    // ---------------------------------------------------------------

    /// A worker that finds `commands` queued when it reads its first, as
    /// when they queue behind a `NewTrack` while it is busy.
    struct QueuedWorker {
        commands: Sender<SpectrogramWorkerCommand>,
        events: Receiver<AnalysisEvent>,
        reports: Receiver<SessionReport>,
        thread: std::thread::JoinHandle<()>,
    }

    impl QueuedWorker {
        fn start(commands: Vec<SpectrogramWorkerCommand>) -> Self {
            let (command_tx, command_rx) = unbounded();
            for command in commands {
                command_tx.send(command).unwrap();
            }
            let (events, event_rx) = unbounded();
            let (reports, report_rx) = unbounded();
            let thread = std::thread::spawn(move || {
                spectrogram_worker_loop(
                    &command_rx,
                    &events,
                    &AtomicU64::new(1),
                    &AtomicU64::new(0),
                    &AtomicU64::new(0),
                    &reports,
                );
            });
            Self {
                commands: command_tx,
                events: event_rx,
                reports: report_rx,
                thread,
            }
        }

        /// The chunks up to and including the first that `stop` accepts.
        fn chunks_until(
            &self,
            mut stop: impl FnMut(&PrecomputedSpectrogramChunk) -> bool,
        ) -> Vec<PrecomputedSpectrogramChunk> {
            let mut chunks = Vec::new();
            loop {
                let chunk = next_chunk(&self.events);
                let done = stop(&chunk);
                chunks.push(chunk);
                if done {
                    return chunks;
                }
            }
        }

        /// End the worker once it parks, and return what it reported.
        fn finish(self) -> Vec<SessionReport> {
            drop(self.commands);
            self.thread.join().expect("worker");
            self.reports.try_iter().collect()
        }
    }

    /// A session of `path` at 250 columns per second, for a 16 kHz file.
    fn fast_session(path: &Path, display_mode: SpectrogramDisplayMode) -> SpectrogramWorkerCommand {
        let mut command = session_on(path, 1, 1);
        if let SpectrogramWorkerCommand::NewTrack {
            hop_size,
            zoom_level,
            display_mode: mode,
            ..
        } = &mut command
        {
            // Above zoom 1 the columns are the STFT rows.
            *hop_size = 64;
            *zoom_level = 2.0;
            *mode = display_mode;
        }
        command
    }

    fn with_new_track(
        mut command: SpectrogramWorkerCommand,
        edit: impl FnOnce(&mut f32, &mut u32, &mut SpectrogramDisplayMode),
    ) -> SpectrogramWorkerCommand {
        if let SpectrogramWorkerCommand::NewTrack {
            zoom_level,
            widget_width,
            display_mode,
            ..
        } = &mut command
        {
            edit(zoom_level, widget_width, display_mode);
        }
        command
    }

    #[test]
    fn a_display_mode_change_queued_behind_a_new_track_takes_effect() {
        // Rolling chunks stop growing at 64 columns; centered ones grow on.
        let path = write_tone_fixture("queued-display-mode", 4, 16_000);
        for (started, queued) in [
            (
                SpectrogramDisplayMode::Centered,
                SpectrogramDisplayMode::Rolling,
            ),
            (
                SpectrogramDisplayMode::Rolling,
                SpectrogramDisplayMode::Centered,
            ),
        ] {
            let worker = QueuedWorker::start(vec![
                fast_session(&path, started),
                SpectrogramWorkerCommand::SetDisplayMode(queued),
            ]);
            let mut columns = 0;
            let chunks = worker.chunks_until(|chunk| {
                columns += u32::from(chunk.column_count);
                columns >= 600 || chunk.complete
            });
            let largest = chunks.iter().map(|chunk| chunk.column_count).max();
            match queued {
                SpectrogramDisplayMode::Rolling => {
                    assert!(largest <= Some(64), "{started:?} → rolling: {largest:?}");
                }
                SpectrogramDisplayMode::Centered => {
                    assert!(largest > Some(64), "{started:?} → centered: {largest:?}");
                }
            }
            worker.finish();
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_seek_queued_behind_a_new_track_takes_effect() {
        let path = write_tone_fixture("queued-seek", 4, 16_000);
        let worker = QueuedWorker::start(vec![
            fast_session(&path, SpectrogramDisplayMode::Rolling),
            SpectrogramWorkerCommand::Seek {
                position_seconds: 2.0,
            },
        ]);
        let chunks = worker.chunks_until(|chunk| {
            (chunk.buffer_reset && chunk.start_column_index > 0) || chunk.complete
        });
        let reset = chunks.last().expect("chunks");
        assert!(
            reset.buffer_reset && reset.start_column_index == 500,
            "{reset:?}"
        );
        worker.finish();
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_position_storm_behind_a_new_track_coalesces_to_the_last_position() {
        let positions = |range: std::ops::RangeInclusive<u32>| {
            range.map(|step| SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: f64::from(step) * 0.04,
            })
        };
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        for command in positions(1..=1_000) {
            tx.send(command).unwrap();
        }
        let QueuedCommand {
            behind: mut queued, ..
        } = coalesce_queued_new_track_command(
            test_new_track("/music/a.flac", 1),
            &rx,
            &AtomicU64::new(1),
        )
        .expect("current session");
        assert!(
            matches!(queued.make_contiguous(), [SpectrogramWorkerCommand::PositionUpdate { position_seconds }]
                if (*position_seconds - 40.0).abs() < 1e-9),
            "{queued:?}"
        );

        // A seek supersedes the positions and seeks before it, not the
        // positions after it.
        for command in positions(1..=10) {
            tx.send(command).unwrap();
        }
        for step in 1..=10 {
            tx.send(SpectrogramWorkerCommand::Seek {
                position_seconds: f64::from(step),
            })
            .unwrap();
        }
        tx.send(SpectrogramWorkerCommand::UpdateWidgetWidth {
            widget_width: 2_560,
        })
        .unwrap();
        for command in positions(251..=260) {
            tx.send(command).unwrap();
        }
        let QueuedCommand {
            behind: mut queued, ..
        } = coalesce_queued_new_track_command(
            test_new_track("/music/a.flac", 1),
            &rx,
            &AtomicU64::new(1),
        )
        .expect("current session");
        assert!(
            matches!(
                queued.make_contiguous(),
                [
                    SpectrogramWorkerCommand::Seek { position_seconds: seek },
                    SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: 2_560 },
                    SpectrogramWorkerCommand::PositionUpdate { position_seconds: position },
                ] if (*seek - 10.0).abs() < 1e-9 && (*position - 10.4).abs() < 1e-9
            ),
            "{queued:?}"
        );

        // The worker seeks once, to the last position, past its lookahead.
        let path = write_tone_fixture("queued-positions", 45, 8_000);
        let mut commands = vec![with_new_track(session_on(&path, 1, 1), |_, _, mode| {
            *mode = SpectrogramDisplayMode::Rolling;
        })];
        commands.extend(positions(1..=1_000));
        let worker = QueuedWorker::start(commands);
        let chunks =
            worker.chunks_until(|chunk| chunk.buffer_reset && chunk.start_column_index > 0);
        assert_eq!(chunks.last().expect("chunks").start_column_index, 312);
        worker.finish();
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_continuation_queued_behind_a_new_track_takes_effect() {
        let first = write_tone_fixture("queued-continue-first", 1, 8_000);
        let next = write_tone_fixture("queued-continue-next", 1, 8_000);
        let rolling = |path: &Path| {
            with_new_track(session_on(path, 1, 1), |_, _, mode| {
                *mode = SpectrogramDisplayMode::Rolling;
            })
        };

        // The track switches before the first file ends.
        let worker = QueuedWorker::start(vec![
            rolling(&first),
            SpectrogramWorkerCommand::ContinueWithFile {
                path: next.clone(),
                track_token: 1,
            },
            SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 },
        ]);
        let chunks = worker.chunks_until(|chunk| chunk.track_token == 2 && chunk.complete);
        assert!(chunks
            .iter()
            .any(|chunk| chunk.track_token == 1 && chunk.complete));
        let reports = worker.finish();
        assert!(
            reports.iter().any(|report| matches!(report,
                SessionReport::Opened { path, track_token: 2, generation: 1, .. } if *path == next)),
            "{reports:?}"
        );

        // A cancelled continuation is not opened.
        let worker = QueuedWorker::start(vec![
            rolling(&first),
            SpectrogramWorkerCommand::ContinueWithFile {
                path: next.clone(),
                track_token: 2,
            },
            SpectrogramWorkerCommand::CancelPendingContinue,
        ]);
        worker.chunks_until(|chunk| chunk.complete);
        let reports = worker.finish();
        assert!(
            reports.iter().all(|report| !matches!(report,
                SessionReport::Opened { path, .. } if *path == next)),
            "{reports:?}"
        );
        std::fs::remove_file(first).expect("remove fixture");
        std::fs::remove_file(next).expect("remove fixture");
    }

    #[test]
    fn a_track_token_queued_behind_a_new_track_takes_effect() {
        let path = write_tone_fixture("queued-token", 1, 8_000);
        let worker = QueuedWorker::start(vec![
            session_on(&path, 1, 1),
            SpectrogramWorkerCommand::UpdateTrackToken { track_token: 5 },
        ]);
        let chunks = worker.chunks_until(|chunk| chunk.complete);
        assert_eq!(chunks.last().expect("chunks").track_token, 5);
        worker.finish();
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_widget_width_queued_behind_a_new_track_takes_effect() {
        // At zoom 64 a 1920-pixel centered view parks 138 columns ahead,
        // and a 3840-pixel one 198.
        let path = write_tone_fixture("queued-width", 30, 8_000);
        let worker = QueuedWorker::start(vec![
            with_new_track(session_on(&path, 1, 1), |zoom, width, _| {
                *zoom = 64.0;
                *width = 1_920;
            }),
            SpectrogramWorkerCommand::UpdateWidgetWidth {
                widget_width: 3_840,
            },
        ]);
        worker
            .chunks_until(|chunk| chunk.start_column_index + u32::from(chunk.column_count) >= 190);
        worker.finish();
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_continuation_queued_behind_a_new_track_follows_later_renames() {
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::ContinueWithFile {
            path: PathBuf::from("/music/next.flac"),
            track_token: 2,
        })
        .unwrap();
        tx.send(SpectrogramWorkerCommand::RemapPaths(Arc::new(
            [(
                PathBuf::from("/music/next.flac"),
                PathBuf::from("/music/02.flac"),
            )]
            .into_iter()
            .collect(),
        )))
        .unwrap();
        let QueuedCommand { behind: queued, .. } = coalesce_queued_new_track_command(
            test_new_track("/music/a.flac", 1),
            &rx,
            &AtomicU64::new(1),
        )
        .expect("current session");
        let mut session = make_test_session();
        session.queued_commands = queued;
        assert!(process_session_commands(&mut session, &rx).is_none());
        assert_eq!(
            session.pending_continue,
            Some((PathBuf::from("/music/02.flac"), 2))
        );
    }

    #[test]
    fn only_later_sessions_and_stops_supersede_queued_commands() {
        // A later session starts from the analysis thread's state after
        // the commands queued before it.
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::SetDisplayMode(
            SpectrogramDisplayMode::Centered,
        ))
        .unwrap();
        tx.send(test_new_track("/music/b.flac", 1)).unwrap();
        tx.send(SpectrogramWorkerCommand::CancelPendingContinue)
            .unwrap();
        let QueuedCommand {
            cmd,
            behind: mut queued,
        } = coalesce_queued_new_track_command(
            test_new_track("/music/a.flac", 1),
            &rx,
            &AtomicU64::new(1),
        )
        .expect("current session");
        assert_eq!(new_track_path(&cmd), Path::new("/music/b.flac"));
        assert!(
            matches!(
                queued.make_contiguous(),
                [SpectrogramWorkerCommand::CancelPendingContinue]
            ),
            "{queued:?}"
        );

        // A stop ends both, and the commands after it stay queued.
        tx.send(SpectrogramWorkerCommand::SetDisplayMode(
            SpectrogramDisplayMode::Centered,
        ))
        .unwrap();
        tx.send(SpectrogramWorkerCommand::StopSession).unwrap();
        tx.send(test_new_track("/music/c.flac", 1)).unwrap();
        let stopped = coalesce_queued_new_track_command(
            test_new_track("/music/a.flac", 1),
            &rx,
            &AtomicU64::new(1),
        );
        assert!(
            matches!(&stopped, Some(QueuedCommand { cmd: SpectrogramWorkerCommand::StopSession, behind })
                if behind.is_empty()),
            "{stopped:?}"
        );
        assert_eq!(
            new_track_path(&rx.try_recv().expect("the next session")),
            Path::new("/music/c.flac")
        );
    }

    #[test]
    fn a_running_session_leaves_the_commands_behind_a_new_track_queued() {
        let mut session = make_test_session();
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(test_new_track("/music/b.flac", 2)).unwrap();
        tx.send(SpectrogramWorkerCommand::SetDisplayMode(
            SpectrogramDisplayMode::Centered,
        ))
        .unwrap();
        assert!(matches!(
            process_session_commands(&mut session, &rx),
            Some(SessionAction::NewSession(_))
        ));
        assert_eq!(session.display_mode, SpectrogramDisplayMode::Rolling);
        assert!(matches!(
            rx.try_recv(),
            Ok(SpectrogramWorkerCommand::SetDisplayMode(
                SpectrogramDisplayMode::Centered
            ))
        ));
    }

    #[test]
    fn a_seek_drained_with_a_track_token_follows_the_token_chunk() {
        let mut session = make_test_session();
        let (tx, rx) = unbounded::<SpectrogramWorkerCommand>();
        tx.send(SpectrogramWorkerCommand::Seek {
            position_seconds: 5.0,
        })
        .unwrap();
        tx.send(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 9 })
            .unwrap();
        assert!(matches!(
            process_session_commands(&mut session, &rx),
            Some(SessionAction::FlushToken)
        ));
        assert_eq!(session.track_token, 9);
        assert!(
            matches!(
                process_session_commands(&mut session, &rx),
                Some(SessionAction::SeekRequired { position_seconds }) if (position_seconds - 5.0).abs() < 1e-9
            ),
            "the seek was dropped"
        );
    }

    // ---------------------------------------------------------------
    // Column timing
    // ---------------------------------------------------------------

    /// A mono 16-bit WAV of `seconds` at `rate`, silent but for one-sample
    /// clicks at `clicks`.
    fn write_click_track(name: &str, rate: u32, seconds: u32, clicks: &[u32]) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "ferrous-session-clicks-{name}-{}.wav",
            std::process::id()
        ));
        let frames = rate * seconds;
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&(36 + frames * 2).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0\x01\0");
        bytes.extend_from_slice(&rate.to_le_bytes());
        bytes.extend_from_slice(&(rate * 2).to_le_bytes());
        bytes.extend_from_slice(b"\x02\0\x10\0data");
        bytes.extend_from_slice(&(frames * 2).to_le_bytes());
        for frame in 0..frames {
            let sample: i16 = if clicks.contains(&frame) { 30_000 } else { 0 };
            bytes.extend_from_slice(&sample.to_le_bytes());
        }
        std::fs::write(&path, bytes).expect("write fixture");
        path
    }

    /// The columns a worker publishes for `commands` until its file ends,
    /// by absolute column; a seek's columns replace those before it.
    fn columns_until_eof(
        commands: Vec<SpectrogramWorkerCommand>,
    ) -> std::collections::BTreeMap<u32, Vec<u8>> {
        let worker = QueuedWorker::start(commands);
        let mut columns = std::collections::BTreeMap::new();
        for chunk in worker.chunks_until(|chunk| chunk.complete) {
            let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
            if width == 0 {
                continue;
            }
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                columns.insert(
                    chunk.start_column_index + u32::try_from(offset).unwrap(),
                    column.to_vec(),
                );
            }
        }
        worker.finish();
        columns
    }

    /// The loudest column within `reach` columns of `near`.
    fn loudest_column(
        columns: &std::collections::BTreeMap<u32, Vec<u8>>,
        near: u32,
        reach: u32,
    ) -> u32 {
        columns
            .range(near.saturating_sub(reach)..=near + reach)
            .max_by_key(|(_, bins)| bins.iter().map(|&bin| u32::from(bin)).sum::<u32>())
            .map(|(&column, _)| column)
            .expect("columns near the click")
    }

    /// A session of `path` with `fft_size`, `hop_size` and `zoom_level`.
    fn timing_session(
        path: &Path,
        fft_size: usize,
        hop_size: usize,
        zoom_level: f32,
        display_mode: SpectrogramDisplayMode,
        start_seconds: f64,
    ) -> SpectrogramWorkerCommand {
        let mut command = session_on(path, 1, 1);
        if let SpectrogramWorkerCommand::NewTrack {
            fft_size: fft,
            hop_size: hop,
            zoom_level: zoom,
            display_mode: mode,
            start_seconds: start,
            target_position_seconds,
            ..
        } = &mut command
        {
            *fft = fft_size;
            *hop = hop_size;
            *zoom = zoom_level;
            *mode = display_mode;
            *start = start_seconds;
            *target_position_seconds = start_seconds;
        }
        command
    }

    #[test]
    fn a_click_shows_in_the_column_of_the_time_it_plays() {
        // 16 kHz clicks, each in the middle of a 1024-frame column at zoom 1
        // and on a 64-frame column of the faster grid.
        let clicks = [8_704_u32, 16_896, 25_088];
        let path = write_click_track("timing", 16_000, 2, &clicks);
        // (FFT size, STFT hop, zoom, column hop)
        for (fft_size, hop_size, zoom, column_hop) in [(512, 64, 2.0, 64), (2_048, 256, 1.0, 1_024)]
        {
            for mode in [
                SpectrogramDisplayMode::Rolling,
                SpectrogramDisplayMode::Centered,
            ] {
                // From the start, and from a seek at the start and in the
                // session, which start the grid half a second in.
                let start =
                    |seconds| timing_session(&path, fft_size, hop_size, zoom, mode, seconds);
                for (case, commands) in [
                    ("start", vec![start(0.0)]),
                    ("started at 0.5 s", vec![start(0.5)]),
                    (
                        "seek to 0.5 s",
                        vec![
                            start(0.0),
                            SpectrogramWorkerCommand::Seek {
                                position_seconds: 0.5,
                            },
                        ],
                    ),
                ] {
                    let columns = columns_until_eof(commands);
                    for click in clicks {
                        // A quarter of the clicks' spacing.
                        let expected = click / column_hop;
                        let reach = 2_048 / column_hop;
                        assert_eq!(
                            loudest_column(&columns, expected, reach),
                            expected,
                            "FFT {fft_size} at zoom {zoom}, {mode:?}, {case}: click at {click}"
                        );
                    }
                }
            }
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_click_shows_in_its_column_in_a_staged_centered_successor() {
        let clicks = [8_704_u32, 16_896];
        let path = write_click_track("staged", 16_000, 2, &clicks);
        let (tx, rx) = unbounded();
        centered_staging_decode(
            &path,
            512,
            64,
            2.0,
            SpectrogramViewMode::Downmix,
            &AtomicBool::new(false),
            &tx,
        );
        drop(tx);
        let mut columns = std::collections::BTreeMap::new();
        for chunk in rx.try_iter() {
            let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                columns.insert(
                    chunk.start_column_index + u32::try_from(offset).unwrap(),
                    column.to_vec(),
                );
            }
        }
        for click in clicks {
            assert_eq!(loudest_column(&columns, click / 64, 32), click / 64);
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn an_mp3_click_shows_at_its_playback_time_after_the_encoder_delay_is_trimmed() {
        // A click at 0.2 s of a 0.4 s 44.1 kHz MP3 with a LAME tag, which
        // gives its encoder delay; GStreamer plays it at 0.2 s.
        let path = std::env::temp_dir().join(format!(
            "ferrous-session-gapless-click-{}.mp3",
            std::process::id()
        ));
        std::fs::write(&path, include_bytes!("fixtures/gapless-click.mp3")).expect("fixture");
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let columns = columns_until_eof(vec![timing_session(&path, 512, 64, 2.0, mode, 0.0)]);
            let column = loudest_column(&columns, 8_820 / 64, 40);
            let time = i64::from(column) * 64;
            assert!(
                (time - 8_820).abs() <= 32,
                "{mode:?}: the click shows at frame {time}"
            );
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    // ---------------------------------------------------------------
    // Channel-count changes
    // ---------------------------------------------------------------

    /// The decoder of `channel-change.mp3`, 0.25 s of stereo and then 0.25 s
    /// of mono 22.05 kHz MP3 frames, changes its caps from two channels to
    /// one. Every
    /// frame of both parts is analysed, in step on both channels.
    #[cfg(feature = "gst")]
    #[test]
    fn a_channel_count_change_mid_stream_neither_stalls_nor_grows() {
        let path = std::env::temp_dir().join(format!(
            "ferrous-session-channel-change-{}.mp3",
            std::process::id()
        ));
        std::fs::write(&path, include_bytes!("fixtures/channel-change.mp3")).expect("fixture");
        let (mut source, rate, channels, _) =
            super::super::decoders::open_gstreamer_file(&path).expect("GStreamer opens it");
        assert_eq!((rate, channels), (22_050, 2));

        let mut session = make_test_session();
        session.fft_size = 512;
        session.hop_size = 64;
        session.effective_hop = 64;
        session.zoom_level = 2.0;
        session.view_mode = SpectrogramViewMode::PerChannel;
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.decode_rate_limit = f64::INFINITY;
        session.channel_count = 2;
        session.bins_per_column = 257;
        session.effective_rate = 22_050;
        session.cols_per_second = 22_050.0 / 64.0;
        session.target_position_seconds = 0.0;
        session.columns_produced = 0;
        session.chunk_start_index = 0;
        session.lookahead_columns = u64::MAX;
        session.stfts = (0..2).map(|_| StftComputer::new(512, 64)).collect();
        session.resamplers = (0..2).map(|_| PeakHoldResampler::new(1.0)).collect();
        // No commands: the session ends when it parks at the end.
        let command_rx = unbounded::<SpectrogramWorkerCommand>().1;
        let (events, event_rx) = unbounded();

        let result = session_decode_loop(
            &mut session,
            &mut source,
            &command_rx,
            &events,
            &AtomicU64::new(1),
            &AtomicU64::new(0),
        );

        assert!(result.is_none(), "{result:?}");
        assert_eq!(session.failure, None);
        assert!(session.source_reached_eof, "the decode reached the end");
        // GStreamer decodes 24 MPEG-2 frames of 576 samples.
        assert_eq!(session.total_covered_samples, 13_824);
        let analysed = session.columns_produced * 64;
        assert!(
            (13_824 - 512..=13_824).contains(&analysed),
            "analysed {analysed} frames"
        );
        for stft in &session.stfts {
            assert!(stft.pending_len() < 512, "{} queued", stft.pending_len());
        }
        drop(event_rx);
        std::fs::remove_file(path).expect("remove fixture");
    }

    // ---------------------------------------------------------------
    // Column estimate across gapless continuations
    // ---------------------------------------------------------------

    #[test]
    fn the_column_estimate_stays_put_over_gapless_continuations() {
        let files: Vec<PathBuf> = (0..4)
            .map(|index| write_tone_fixture(&format!("estimate-{index}"), 1, 8_000))
            .collect();
        let worker = QueuedWorker::start(vec![fast_session(
            &files[0],
            SpectrogramDisplayMode::Rolling,
        )]);
        let mut chunks = Vec::new();
        for (token, next) in (1..).zip(&files[1..]) {
            chunks
                .extend(worker.chunks_until(|chunk| chunk.track_token == token && chunk.complete));
            worker
                .commands
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: next.clone(),
                    track_token: token + 1,
                })
                .unwrap();
        }
        chunks.extend(worker.chunks_until(|chunk| chunk.track_token == 4 && chunk.complete));
        worker.finish();

        // Each file is 125 columns of 8 kHz audio at a 64-frame hop, and is
        // estimated from its header as 1,136, with the usual allowance.
        let mut track_start = 0;
        for token in 1..=4 {
            let estimates: std::collections::BTreeSet<u32> = chunks
                .iter()
                .filter(|chunk| chunk.track_token == token && !chunk.complete)
                .map(|chunk| chunk.total_columns_estimate)
                .collect();
            assert_eq!(
                estimates.into_iter().collect::<Vec<_>>(),
                [track_start + 1_136],
                "track {token}"
            );
            let end = chunks
                .iter()
                .find(|chunk| chunk.track_token == token && chunk.complete)
                .expect("finalize chunk");
            assert!(end.total_columns_estimate > track_start);
            track_start = end.total_columns_estimate;
        }
        for file in files {
            std::fs::remove_file(file).expect("remove fixture");
        }
    }

    // -----------------------------------------------------------------------
    // Hot path: the STFT input and byte levels match the implementation
    // before the copies and logarithms were taken out of it.
    // -----------------------------------------------------------------------

    use super::super::decoders::reference_deinterleave_samples;
    use super::super::quantize::reference_level;

    /// A deterministic white noise source in -1..1.
    struct Noise(u32);

    impl Noise {
        fn next(&mut self) -> f32 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 17;
            self.0 ^= self.0 << 5;
            f32::from(u16::try_from(self.0 >> 16).unwrap()) / 32_768.0 - 1.0
        }
    }

    /// Tones, white noise, silence, impulses, full-scale clipping and noise
    /// near the bottom of the range, `frames` long.
    fn synthetic_signals(frames: usize) -> Vec<Vec<f32>> {
        let time = |i: usize| super::super::usize_to_f32_approx(i) / 48_000.0;
        let tau = std::f32::consts::TAU;
        let mut noise = Noise(0x9e37_79b9);
        let mut quiet = Noise(0x0bad_cafe);
        vec![
            (0..frames)
                .map(|i| {
                    0.5 * (tau * 1_000.0 * time(i)).sin() + 0.01 * (tau * 15_000.0 * time(i)).sin()
                })
                .collect(),
            (0..frames).map(|_| noise.next()).collect(),
            vec![0.0; frames],
            (0..frames)
                .map(|i| if i % 3_001 == 7 { 1.0 } else { 0.0 })
                .collect(),
            (0..frames)
                .map(|i| (4.0 * (tau * 440.0 * time(i)).sin()).clamp(-1.0, 1.0))
                .collect(),
            (0..frames).map(|_| quiet.next() * 1.0e-7).collect(),
        ]
    }

    /// Interleave `channels` of the signals, each channel starting at a
    /// different one.
    fn interleave_signals(signals: &[Vec<f32>], first: usize, channels: usize) -> Vec<f32> {
        let frames = signals[0].len();
        (0..frames)
            .flat_map(|frame| {
                (0..channels).map(move |channel| signals[(first + channel) % signals.len()][frame])
            })
            .collect()
    }

    /// Packet sizes that straddle window and hop boundaries.
    const PACKET_FRAMES: [usize; 7] = [1, 7, 333, 1_152, 4_099, 576, 2_048];

    fn packets(samples: &[f32], channels: usize) -> Vec<AudioFrames> {
        let mut packets = Vec::new();
        let mut frame = 0;
        let total = samples.len() / channels;
        for size in PACKET_FRAMES.iter().cycle() {
            if frame >= total {
                break;
            }
            let frames = (*size).min(total - frame);
            packets.push(AudioFrames {
                samples: samples[frame * channels..(frame + frames) * channels].to_vec(),
                first_frame: usize_to_u64(frame),
                frames,
                channels,
            });
            frame += frames;
        }
        packets
    }

    /// The decode path before it de-interleaved straight into the STFTs and
    /// quantized without logarithms.
    struct ReferencePipeline {
        stfts: Vec<StftComputer>,
        resamplers: Vec<PeakHoldResampler>,
        fft_size: usize,
        bins: usize,
        bytes: Vec<u8>,
    }

    impl ReferencePipeline {
        fn new(channels: usize, fft_size: usize, hop: usize, interval: f64) -> Self {
            Self {
                stfts: (0..channels)
                    .map(|_| StftComputer::new(fft_size, hop))
                    .collect(),
                resamplers: (0..channels)
                    .map(|_| PeakHoldResampler::new(interval))
                    .collect(),
                fft_size,
                bins: fft_size / 2 + 1,
                bytes: Vec::new(),
            }
        }

        fn enqueue(&mut self, audio: &AudioFrames, view_mode: SpectrogramViewMode) {
            let per_channel = reference_deinterleave_samples(
                &audio.samples,
                audio.frames,
                audio.channels,
                self.stfts.len(),
                view_mode,
            );
            for (stft, samples) in self.stfts.iter_mut().zip(&per_channel) {
                stft.enqueue_samples(samples);
            }
        }

        fn append_row(&mut self, row: &[f32]) {
            self.bytes.extend(
                row.iter()
                    .take(self.bins)
                    .map(|&power| reference_level(power, self.fft_size)),
            );
            self.bytes
                .extend(std::iter::repeat_n(0, self.bins.saturating_sub(row.len())));
        }

        fn drain(&mut self) {
            while self.stfts.iter().all(StftComputer::has_row) {
                let start = self.bytes.len();
                let mut emitted = true;
                for channel in 0..self.stfts.len() {
                    let row = self.stfts[channel].take_row().unwrap().to_vec();
                    match self.resamplers[channel].push(&row).map(<[f32]>::to_vec) {
                        Some(output) => self.append_row(&output),
                        None => emitted = false,
                    }
                }
                if !emitted {
                    self.bytes.truncate(start);
                }
            }
        }

        fn flush(&mut self) {
            let flushed: Vec<_> = self
                .resamplers
                .iter_mut()
                .map(|resampler| resampler.flush().map(<[f32]>::to_vec))
                .collect();
            if flushed.iter().all(Option::is_some) {
                for row in flushed.into_iter().flatten() {
                    self.append_row(&row);
                }
            }
        }
    }

    /// Feed packets through the session's path and the reference path, and
    /// check that every STFT's queued input and every byte level match.
    #[allow(clippy::too_many_arguments)]
    fn assert_paths_match(
        packets: &[AudioFrames],
        channels: usize,
        fft_size: usize,
        hop: usize,
        interval: f64,
        view_mode: SpectrogramViewMode,
        leading_silence: usize,
        context: &str,
    ) -> Vec<u8> {
        let mut reference = ReferencePipeline::new(channels, fft_size, hop, interval);
        let mut stfts: Vec<_> = (0..channels)
            .map(|_| StftComputer::new(fft_size, hop))
            .collect();
        let mut resamplers: Vec<_> = (0..channels)
            .map(|_| PeakHoldResampler::new(interval))
            .collect();
        let bins = fft_size / 2 + 1;
        let mut bytes = Vec::new();
        for stft in &mut reference.stfts {
            stft.enqueue_samples(&vec![0.0; leading_silence]);
        }
        enqueue_leading_silence(&mut stfts, leading_silence);
        for (index, packet) in packets.iter().enumerate() {
            reference.enqueue(packet, view_mode);
            enqueue_audio(&mut stfts, packet, view_mode).unwrap();
            for (channel, (actual, expected)) in stfts.iter().zip(&reference.stfts).enumerate() {
                assert!(
                    actual
                        .pending_samples()
                        .iter()
                        .map(|sample| sample.to_bits())
                        .eq(expected
                            .pending_samples()
                            .iter()
                            .map(|sample| sample.to_bits())),
                    "{context}: STFT input of channel {channel} after packet {index}"
                );
            }
            reference.drain();
            while append_stft_column(&mut stfts, &mut resamplers, &mut bytes, bins, fft_size)
                .is_some()
            {}
            assert_eq!(
                bytes, reference.bytes,
                "{context}: levels after packet {index}"
            );
        }
        reference.flush();
        let flushed: Vec<_> = resamplers
            .iter_mut()
            .map(|resampler| resampler.flush().map(<[f32]>::to_vec))
            .collect();
        if flushed.iter().all(Option::is_some) {
            for row in flushed.iter().flatten() {
                append_quantized_row(&mut bytes, row, bins, fft_size);
            }
        }
        assert_eq!(bytes, reference.bytes, "{context}: levels after the flush");
        bytes
    }

    #[test]
    fn levels_of_synthetic_signals_match_the_logarithmic_encoding() {
        let mut seen = [false; 256];
        for fft_size in [512, 1_024, 2_048, 4_096, 8_192] {
            for zoom in [0.05, 0.5, 1.0, 2.0, 16.0] {
                let hop = super::super::zoom_hop_size(fft_size, zoom);
                let interval = output_interval_for_zoom(hop, zoom);
                // Enough audio for a few columns at every zoom.
                let rows = usize::try_from(f64_to_u64_saturating(interval.ceil())).unwrap();
                let frames = fft_size + hop * rows * 6;
                let signals = synthetic_signals(frames);
                for channels in [1, 2, 6] {
                    let samples = interleave_signals(&signals, fft_size, channels);
                    let context = format!("FFT {fft_size}, zoom {zoom}, {channels} channels");
                    let produced = assert_paths_match(
                        &packets(&samples, channels),
                        channels,
                        fft_size,
                        hop,
                        interval,
                        SpectrogramViewMode::PerChannel,
                        fft_size / 2,
                        &context,
                    );
                    assert!(!produced.is_empty(), "{context}: no columns");
                    for level in produced {
                        seen[usize::from(level)] = true;
                    }
                }
            }
        }
        // The signals reach from silence to full scale.
        assert!(seen[0] && seen[255]);
        assert!(seen.iter().filter(|&&seen| seen).count() > 200);
    }

    #[test]
    fn levels_of_every_signal_alone_match_the_logarithmic_encoding() {
        let signals = synthetic_signals(2_048 + 256 * 16);
        for (index, signal) in signals.iter().enumerate() {
            let produced = assert_paths_match(
                &packets(signal, 1),
                1,
                2_048,
                256,
                1.0,
                SpectrogramViewMode::PerChannel,
                1_024,
                &format!("signal {index}"),
            );
            assert!(!produced.is_empty());
        }
    }

    #[test]
    fn stft_input_matches_the_copying_path_for_any_decoded_layout() {
        let signals = synthetic_signals(9_000);
        // (decoded channels, analysis channels, view mode): matching
        // layouts, a mono stream filling every channel, missing and extra
        // channels, and downmixes.
        let layouts = [
            (1, 1, SpectrogramViewMode::PerChannel),
            (2, 2, SpectrogramViewMode::PerChannel),
            (6, 6, SpectrogramViewMode::PerChannel),
            (1, 2, SpectrogramViewMode::PerChannel),
            (2, 6, SpectrogramViewMode::PerChannel),
            (6, 2, SpectrogramViewMode::PerChannel),
            (1, 1, SpectrogramViewMode::Downmix),
            (2, 1, SpectrogramViewMode::Downmix),
            (6, 1, SpectrogramViewMode::Downmix),
        ];
        for (decoded, channels, view_mode) in layouts {
            let samples = interleave_signals(&signals, decoded, decoded);
            // Hops shorter than, equal to, and longer than the window: a
            // longer hop carries a skip across packets.
            for (fft_size, hop) in [(512, 128), (512, 512), (512, 1_024), (2_048, 4_095)] {
                assert_paths_match(
                    &packets(&samples, decoded),
                    channels,
                    fft_size,
                    hop,
                    1.0,
                    view_mode,
                    fft_size / 2,
                    &format!("{decoded} into {channels} {view_mode:?}, FFT {fft_size} hop {hop}"),
                );
            }
        }
    }

    #[test]
    fn stft_input_matches_the_copying_path_over_a_gapless_continuation() {
        let signals = synthetic_signals(6_000);
        // A stereo file continued by a mono file and then a six-channel
        // one, whose timelines each start again at frame 0, on STFTs that
        // keep their windows over the switches.
        let mut packets = Vec::new();
        for decoded in [2, 1, 6] {
            let samples = interleave_signals(&signals, decoded, decoded);
            let mut timeline = AudioTimeline::new(0);
            let mut file = self::packets(&samples, decoded).into_iter();
            loop {
                match timeline
                    .read(|| Ok(file.next().map_or(AudioRead::Eof, AudioRead::Frames)))
                    .unwrap()
                {
                    AudioRead::Frames(audio) => packets.push(audio),
                    AudioRead::Pending => {}
                    AudioRead::Eof => break,
                }
            }
        }
        for (fft_size, hop, zoom) in [(1_024, 128, 1.0), (2_048, 64, 16.0), (512, 64, 0.5)] {
            let interval = output_interval_for_zoom(hop, zoom);
            for (channels, view_mode) in [
                (2, SpectrogramViewMode::PerChannel),
                (1, SpectrogramViewMode::Downmix),
            ] {
                let produced = assert_paths_match(
                    &packets,
                    channels,
                    fft_size,
                    hop,
                    interval,
                    view_mode,
                    fft_size / 2,
                    &format!("continuation, FFT {fft_size} hop {hop}, {view_mode:?}"),
                );
                assert!(!produced.is_empty());
            }
        }
    }

    /// Before/after timings of the hot path. Run with
    /// `cargo test --release --lib -- --ignored --nocapture spectrogram_hot_path_benchmark`.
    #[test]
    #[ignore = "benchmark; run in a release build"]
    fn spectrogram_hot_path_benchmark() {
        use std::hint::black_box;
        use std::time::Instant;
        use symphonia::core::audio::{AsAudioBufferRef, AudioBuffer, SampleBuffer, Signal};
        use symphonia::core::conv::FromSample;

        fn per_item(start: Instant, items: usize) -> f64 {
            start.elapsed().as_secs_f64() * 1.0e9 / usize_to_f64_approx(items)
        }

        // Quantization: rows of an FFT of 8192, noise across the whole range.
        let fft_size = 8_192;
        let bins = fft_size / 2 + 1;
        let mut noise = Noise(0x1234_5678);
        let row: Vec<f32> = (0..bins)
            .map(|_| 10.0_f32.powf(noise.next() * 9.0 + 2.0))
            .collect();
        let rows = 2_000;
        let mut bytes = Vec::with_capacity(bins);
        let start = Instant::now();
        for _ in 0..rows {
            bytes.clear();
            bytes.extend(row.iter().map(|&power| reference_level(power, fft_size)));
            black_box(&bytes);
        }
        let before = per_item(start, rows * bins);
        let start = Instant::now();
        for _ in 0..rows {
            bytes.clear();
            append_quantized_row(&mut bytes, black_box(&row), bins, fft_size);
            black_box(&bytes);
        }
        let after = per_item(start, rows * bins);
        println!("quantization: {before:.2} ns/bin before, {after:.2} ns/bin after");

        // Packets: 16-bit stereo, 4096 frames, as Symphonia decodes FLAC,
        // into two STFTs. Everything from the decoded buffer to the queue.
        let frames = 4_096;
        let spec = symphonia::core::audio::SignalSpec::new(
            48_000,
            symphonia::core::audio::Channels::FRONT_LEFT
                | symphonia::core::audio::Channels::FRONT_RIGHT,
        );
        let mut decoded = AudioBuffer::<i16>::new(usize_to_u64(frames), spec);
        decoded.render_reserved(Some(frames));
        for channel in 0..2 {
            for sample in decoded.chan_mut(channel) {
                *sample = i16::from_sample(noise.next());
            }
        }
        let view_mode = SpectrogramViewMode::PerChannel;
        let packets = 4_000;
        let mut stfts = vec![StftComputer::new(2_048, 256), StftComputer::new(2_048, 256)];
        let mut sample_buf = None;
        let start = Instant::now();
        for _ in 0..packets {
            let audio = decoded.as_audio_buffer_ref();
            let buffer: &mut SampleBuffer<f32> =
                super::super::fft::ensure_sample_buffer(&mut sample_buf, audio.capacity(), spec);
            buffer.copy_interleaved_ref(audio);
            let samples = buffer.samples().to_vec();
            let per_channel =
                reference_deinterleave_samples(&samples, frames, 2, stfts.len(), view_mode);
            for (stft, samples) in stfts.iter_mut().zip(&per_channel) {
                stft.enqueue_samples(samples);
                stft.reset();
            }
        }
        let before = per_item(start, packets * frames);
        let start = Instant::now();
        for _ in 0..packets {
            let audio = AudioFrames {
                samples: super::super::decoders::interleave_f32(&decoded.as_audio_buffer_ref()),
                first_frame: 0,
                frames,
                channels: 2,
            };
            enqueue_audio(&mut stfts, &audio, view_mode).unwrap();
            for stft in &mut stfts {
                stft.reset();
            }
        }
        let after = per_item(start, packets * frames);
        println!("packets: {before:.2} ns/frame before, {after:.2} ns/frame after");
    }
}
