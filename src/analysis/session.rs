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
use super::centered_view::{self, CenteredView};
use super::column_hop::{column_hops, ColumnHops};
use super::decoders::{
    open_audio_file, u64_to_u32_saturating, AudioFrameSource, AudioFrames, AudioRead,
    AudioTimeline, TrackLength,
};
use super::fft::{PeakHoldResampler, StftComputer};
use super::frequency_cap::shown_bins;
use super::output::{AnalysisEventOutput, AnalysisOutputs};
use super::quantize::append_quantized_row;
use super::retention::{lookahead_seconds, SpectrogramBufferConfig, SpectrogramBufferLimits};
use super::stft_batch;
use super::track_io::{source_stamp, TrackFormat};
use super::{
    f64_to_u64_saturating, usize_to_u64, AnalysisEvent, PrecomputedSpectrogramChunk,
    SpectrogramDisplayMode, SpectrogramViewMode, ESTIMATE_COLUMN_FRAMES,
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
    (column, column_grid_frame(column, hop, effective_hop))
}

/// The frame of the first input FFT window contributing to the absolute
/// output column `column`.
fn column_grid_frame(column: u64, hop: usize, effective_hop: usize) -> u64 {
    let output_frame = column.saturating_mul(usize_to_u64(effective_hop));
    let input_hop = usize_to_u64(hop.max(1));
    output_frame / input_hop * input_hop
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

/// [`centred_window_start`] in a file that starts at `file_start_frame` of
/// the column grid: the frame of the file to read from, and the frames of
/// silence before it. The windows of a continued timeline that reach back
/// into the earlier file read silence there.
fn file_window_start(grid_frame: u64, fft_size: usize, file_start_frame: u64) -> (u64, usize) {
    let (start, silence) = centred_window_start(grid_frame, fft_size);
    if let Some(frame) = start.checked_sub(file_start_frame) {
        return (frame, silence);
    }
    let before = usize::try_from(file_start_frame - start).unwrap_or(usize::MAX);
    (0, silence.saturating_add(before))
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
/// which the decode loops drain every row before reading the next, or the
/// [`stft_batch::PARALLEL_BATCH_ROWS`] rows a session with helper threads
/// queues, at most 64 hops of 2048 frames.
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

/// Rescale a base `total_columns` estimate for a given `effective_hop`.
/// The estimate counts columns of [`ESTIMATE_COLUMN_FRAMES`] frames; a
/// session's columns of another hop are proportionally more or fewer.
fn scale_columns_estimate(total_columns: u32, effective_hop: usize) -> u32 {
    if effective_hop != ESTIMATE_COLUMN_FRAMES && effective_hop > 0 {
        // Both hops are small; precision loss is negligible.
        #[allow(clippy::cast_precision_loss)]
        let ratio = ESTIMATE_COLUMN_FRAMES as f64 / effective_hop as f64;
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
        /// Whether the session sends only the bins up to its rate family's
        /// frequency cap.
        frequency_cap: bool,
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
/// the continuations it cannot make, the files it cannot read, and the
/// sessions that end with no other to follow.
#[derive(Debug, Clone, PartialEq)]
pub(super) enum SessionReport {
    /// The session of `generation` opened `path`, for `track_token`.
    Opened {
        generation: u64,
        track_token: u64,
        path: PathBuf,
        stamp: Option<WaveformSourceStamp>,
        format: TrackFormat,
        /// For a gapless continuation, where the file starts on the
        /// session's timeline, in seconds: the time its first column shows.
        /// `None` for the file a session starts with, which is on its own
        /// timeline.
        continued_at: Option<f64>,
    },
    /// The session of `generation` seeked, and decodes from `start_seconds`
    /// on: what it decoded before is gone from the UI.
    Repositioned { generation: u64, start_seconds: f64 },
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
    /// The session of `generation` ended while it was current, with no
    /// other to follow it: it failed, whether it could open its file or
    /// not, or playback switched to a continuation it rejected. The worker
    /// waits for the next session, and takes no seek until then. A session
    /// that reaches the end of its file parks there instead, taking seeks.
    Ended { generation: u64 },
}

impl SessionReport {
    pub(super) fn generation(&self) -> u64 {
        match self {
            Self::Opened { generation, .. }
            | Self::Repositioned { generation, .. }
            | Self::ContinuationRejected { generation, .. }
            | Self::Failed { generation, .. }
            | Self::Ended { generation } => *generation,
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
    /// Where the analysis thread publishes its deactivations.
    pub(super) stops: Arc<super::SpectrogramStops>,
    pub(super) columns_produced: Arc<AtomicU64>,
    /// The [`PublishedTrackLength`] of the active worker session's file, after
    /// its file probe.  0 = unknown.  Read by the analysis thread to model
    /// the centered display's visible window (full-track lock / EOF
    /// detach) when choosing session decode windows.
    pub(super) track_length: Arc<PublishedTrackLength>,
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
            hop_size: u32::try_from(self.effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(self.fft_size).unwrap_or(u32::MAX),
            coverage_seconds: coverage,
            complete: false,
            buffer_reset: false,
            clear_history: false,
            widget_width: 0,
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
                hop_size: u32::try_from(state.effective_hop).unwrap_or(u32::MAX),
                fft_size: u32::try_from(state.fft_size).unwrap_or(u32::MAX),
                coverage_seconds: coverage,
                complete: false,
                buffer_reset: false,
                clear_history: false,
                widget_width: 0,
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
    frequency_cap: bool,
    zoom_level: f32,
    view_mode: SpectrogramViewMode,
    stop: &AtomicBool,
    tx: &Sender<PrecomputedSpectrogramChunk>,
) {
    if stop.load(Ordering::Relaxed) {
        return;
    }
    let Some((mut source, native_sample_rate, native_channels, length)) = open_audio_file(path)
    else {
        profile_eprintln!("[staging] failed to open {}", path.display());
        return;
    };

    let effective_rate = u32::try_from(native_sample_rate).unwrap_or(48_000);
    let channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let bins_per_column = shown_bins(fft_size, effective_rate, frequency_cap);
    let ColumnHops {
        stft_hop: hop_size,
        output_interval,
        effective_hop,
    } = column_hops(fft_size, zoom_level, effective_rate);

    let mut stfts: Vec<StftComputer> = (0..channel_count)
        .map(|_| StftComputer::new(fft_size, hop_size))
        .collect();
    let mut resamplers: Vec<PeakHoldResampler> = (0..channel_count)
        .map(|_| PeakHoldResampler::new(output_interval))
        .collect();

    let total_columns_estimate = scale_columns_estimate(length.columns, effective_hop);

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
    frequency_cap: bool,
    zoom_level: f32,
    view_mode: SpectrogramViewMode,
    stop: Arc<AtomicBool>,
    open_gate: Option<Receiver<()>>,
) -> Option<(Receiver<PrecomputedSpectrogramChunk>, JoinableThread)> {
    let (tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();
    let thread = crate::worker_thread::spawn_worker("ferrous-spectrogram-staging", move || {
        crate::worker_thread::lower_current_thread_priority();
        if let Some(gate) = open_gate {
            let _ = gate.recv();
        }
        centered_staging_decode(
            &path,
            fft_size,
            frequency_cap,
            zoom_level,
            view_mode,
            &stop,
            &tx,
        );
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
    track_length: Arc<PublishedTrackLength>,
    reports: Sender<SessionReport>,
    status: &SpectrogramWorkerStatus,
) -> Option<JoinableThread> {
    let thread_status = status.clone();
    let work = move || {
        let _stopped = StopOnExit(thread_status);
        crate::worker_thread::lower_current_thread_priority();
        spectrogram_worker_loop(
            &cmd_rx,
            &event_tx,
            &generation,
            &columns_produced,
            &track_length,
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

/// Make a session's decode of a file fail, or wait to open it.
#[cfg(test)]
pub(crate) mod decode_test_hooks {
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;

    use crossbeam_channel::{bounded, Receiver, Sender};

    /// Files whose next session waits before it opens them: it says it
    /// reached the file on the first channel, and waits on the second.
    #[allow(clippy::type_complexity)]
    static HELD_OPENS: Mutex<Vec<(PathBuf, Sender<()>, Receiver<()>)>> = Mutex::new(Vec::new());

    /// Make the next session of `path` wait before it opens the file, as
    /// one of a file on a slow network share does. It sends on the returned
    /// receiver once it waits, and goes on once the returned sender sends
    /// or is dropped.
    pub(crate) fn hold_open_of(path: &Path) -> (Receiver<()>, Sender<()>) {
        let (reached_tx, reached) = bounded(1);
        let (release, release_rx) = bounded(0);
        HELD_OPENS
            .lock()
            .expect("held opens")
            .push((path.to_path_buf(), reached_tx, release_rx));
        (reached, release)
    }

    /// Wait before opening `path` if a test holds it, once.
    pub(super) fn wait_to_open(path: &Path) {
        let held = {
            let mut held = HELD_OPENS.lock().expect("held opens");
            held.iter()
                .position(|(held, ..)| held == path)
                .map(|index| held.remove(index))
        };
        if let Some((_, reached, release)) = held {
            let _ = reached.send(());
            let _ = release.recv();
        }
    }

    /// Files whose next session fails to decode, after how many packets.
    static READ_FAILURES: Mutex<Vec<(PathBuf, usize)>> = Mutex::new(Vec::new());

    /// Make the next session of `path` fail to decode once it has read
    /// `after_packets` packets.
    pub(crate) fn fail_reads_of(path: &Path, after_packets: usize) {
        READ_FAILURES
            .lock()
            .expect("read failures")
            .push((path.to_path_buf(), after_packets));
    }

    /// Whether the read after `packets` packets of `path` fails, which it
    /// does only once.
    pub(super) fn take_read_failure(path: &Path, packets: usize) -> bool {
        let mut failures = READ_FAILURES.lock().expect("read failures");
        let Some(index) = failures
            .iter()
            .position(|(failing, after)| failing == path && packets >= *after)
        else {
            return false;
        };
        failures.remove(index);
        true
    }
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
    track_length_out: &PublishedTrackLength,
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
            SpectrogramWorkerCommand::NewTrack {
                generation: session_generation,
                ..
            } => {
                next_cmd = run_spectrogram_session(
                    next,
                    cmd_rx,
                    event_tx,
                    generation,
                    columns_produced_out,
                    track_length_out,
                    reports,
                );
                // A superseded session ends for the newer one; any other
                // end leaves the worker with no session.
                if next_cmd.is_none() && generation.load(Ordering::Relaxed) == session_generation {
                    let _ = reports.send(SessionReport::Ended {
                        generation: session_generation,
                    });
                }
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
    /// The frames of the file, as published to the analysis thread; `None`
    /// if unknown, or on the timeline of a continued file, where no track
    /// ends at the session's last column.
    track_frames: Option<u64>,
    effective_rate: u32,
    cols_per_second: f64,

    // Position tracking
    target_position_seconds: f64,
    /// Whose timeline the session's columns are on.
    timeline_of: ColumnTimeline,
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
    /// The left of a centered view, while the session decodes it after the
    /// columns at the playhead.
    backfill: Option<Backfill>,
    /// The column after the file's last, once the session has decoded to
    /// its end: the extent every later chunk carries as its estimate.
    end_column: Option<u64>,
}

/// Whose timeline a session's columns are on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ColumnTimeline {
    /// The playing file's: a position in it is the column's time.
    PlayingFile,
    /// An earlier file's, which a gapless continuation went on from. The
    /// file the session decodes starts at `file_start_frame` of the column
    /// grid, and the analysis thread adds its start to the positions in it.
    /// A seek, which is in the playing file, puts the session on that file's
    /// timeline.
    EarlierFile { file_start_frame: u64 },
}

impl ColumnTimeline {
    /// The frame of the column grid where the file the session decodes
    /// starts.
    fn file_start_frame(self) -> u64 {
        match self {
            Self::PlayingFile => 0,
            Self::EarlierFile { file_start_frame } => file_start_frame,
        }
    }
}

/// Where the file a session decodes starts on its timeline, in seconds.
fn file_start_seconds(session: &SpectrogramSessionState) -> f64 {
    frames_to_seconds(
        session.timeline_of.file_start_frame(),
        session.effective_rate,
    )
}

fn frames_to_seconds(frames: u64, rate: u32) -> f64 {
    // Frame counts are far below f64's exact-integer range.
    #[allow(clippy::cast_precision_loss)]
    let frames = frames as f64;
    frames / f64::from(rate.max(1))
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
    track_length_out: &PublishedTrackLength,
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
        frequency_cap,
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
        "[spect-worker] SESSION START path={} gen={gen} token={track_token} fft={fft_size} zoom={zoom_level} view={view_mode:?} display={display_mode:?} start_s={start_seconds:.2} target_s={target_position_seconds:.2}",
        path.file_name().unwrap_or_default().to_string_lossy(),
    );

    #[cfg(test)]
    decode_test_hooks::wait_to_open(path);
    let Some((mut source, native_sample_rate, native_channels, length)) = open_audio_file(path)
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
        "[spect-worker] file opened in {:.2}ms sr={native_sample_rate} ch={native_channels} est_cols={}",
        session_started_at.elapsed().as_secs_f64() * 1000.0,
        length.columns,
    );

    let effective_rate = u32::try_from(native_sample_rate).unwrap_or(48_000);
    let bins_per_column = shown_bins(fft_size, effective_rate, frequency_cap);

    publish_track_length(track_length_out, gen, path, length, effective_rate);
    let _ = reports.send(SessionReport::Opened {
        generation: gen,
        track_token,
        path: path.clone(),
        stamp: source_stamp(path),
        format: TrackFormat::Known {
            sample_rate: effective_rate,
            channels: native_channels,
        },
        continued_at: None,
    });

    let actual_channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let ColumnHops {
        stft_hop: hop_size,
        output_interval,
        effective_hop,
    } = column_hops(fft_size, zoom_level, effective_rate);
    let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(effective_hop);
    let total_columns_estimate = scale_columns_estimate(length.columns, effective_hop);

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

    let (start_column, _) = stft_seek_grid(start_seconds, effective_rate, hop_size, effective_hop);
    let (playhead_column, _) = stft_seek_grid(
        target_position_seconds,
        effective_rate,
        hop_size,
        effective_hop,
    );
    let backfill = plan_backfill(display_mode, start_column, playhead_column);
    // A centered session that starts left of its playhead decodes the
    // playhead's columns first.
    let first_column = backfill.map_or(start_column, |backfill| backfill.end);
    let (seek_frame, leading_silence) = centred_window_start(
        column_grid_frame(first_column, hop_size, effective_hop),
        fft_size,
    );
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
        track_frames: track_length_out.frames_of(path, effective_rate),
        effective_rate,
        cols_per_second,
        target_position_seconds,
        timeline_of: ColumnTimeline::PlayingFile,
        columns_produced: first_column,
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
        chunk_start_index: first_column,
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
        backfill,
        end_column: None,
    };
    enqueue_leading_silence(&mut session.stfts, leading_silence);

    // Send initial metadata chunk (0 columns, carries estimates + transition
    // semantics). It starts the session's columns at its first, which the
    // UI's ring counts the columns of a backfill from.
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
            hop_size: u32::try_from(effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(fft_size).unwrap_or(u32::MAX),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: emit_initial_reset,
            clear_history: emit_initial_reset && clear_history_on_reset,
            widget_width: session.widget_width,
        },
    ));

    for resampler in &mut session.resamplers {
        resampler.reset_at_column(first_column, hop_size, effective_hop);
    }

    loop {
        let result = session_decode_loop(
            &mut session,
            &mut source,
            cmd_rx,
            event_tx,
            generation,
            columns_produced_out,
            track_length_out,
        );

        if let Some(SpectrogramWorkerCommand::Seek { position_seconds }) = result {
            if let Err(error) = handle_session_seek(
                &mut session,
                position_seconds,
                &mut source,
                event_tx,
                reports,
            ) {
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
                let (Some((new_source, _, _, new_length)), Some(format), true) =
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
                // The file follows the outgoing one on the session's
                // timeline: its first frame comes after that file's last.
                let file_start_frame = session
                    .timeline_of
                    .file_start_frame()
                    .saturating_add(session.timeline.next_frame());
                session.timeline_of = ColumnTimeline::EarlierFile { file_start_frame };
                let _ = reports.send(SessionReport::Opened {
                    generation: session.gen,
                    track_token,
                    path: path.clone(),
                    stamp,
                    format,
                    continued_at: Some(file_start_seconds(&session)),
                });
                source = new_source;
                session.source_reached_eof = false;
                session.rejected_continuation = None;
                session.end_column = None;
                session.timeline = AudioTimeline::new(0);
                session.track_token = track_token;
                session.path = path;
                // The session's columns go on from the outgoing track's.
                session.total_columns_estimate = continued_columns_estimate(
                    session.columns_produced,
                    new_length.columns,
                    session.effective_hop,
                );
                // The outgoing file's length is not the continued file's.
                // A compatible continuation stays inside this worker
                // session, so it must publish here; otherwise the length
                // remains unknown for every later centered seek/zoom
                // restart.
                publish_track_length(
                    track_length_out,
                    session.gen,
                    &session.path,
                    new_length,
                    session.effective_rate,
                );
                session.track_frames = None;

                session.target_chunk_columns = 1;
                session.throttle_origin = None;
                session.session_start_column = session.columns_produced;
                session.post_reset_unthrottled_columns =
                    post_reset_unthrottled_columns(session.display_mode);
                profile_eprintln!(
                    "[spect-worker] file switch OK, continuing session at {:.2}s unthrottled_cols={}",
                    file_start_seconds(&session),
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
                if finalize && !session.source_reached_eof && decoded_frontier(&session) > 0 {
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
            fft_size: 0,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: true,
            widget_width: 0,
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

/// The length of the file a decode worker session opened last, as it
/// publishes it to the analysis thread: the file's frames, without the
/// allowance of its column estimate, at the rate they are at. The decoder
/// places its centered views by the columns it decodes of them (see
/// [`centered_view::decoded_end_column`]), which the UI's view ends at.
///
/// A publication is bound to the file it is of and to the generation of the
/// session that probed it. The probe can outlast the track: a session of
/// the outgoing track, whose file is slow to open, publishes after the
/// track has changed. A length is therefore only of the file whose path it
/// carries, and a session's publication does not replace a newer
/// session's. The file's sessions all publish its length, so the restart
/// of one finds the length another published.
#[derive(Debug, Default)]
pub(super) struct PublishedTrackLength {
    latest: Mutex<Option<TrackLengthPublication>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct TrackLengthPublication {
    generation: u64,
    path: PathBuf,
    frames: u64,
    rate: u32,
}

impl PublishedTrackLength {
    /// Publish that the file at `path`, which a session of `generation`
    /// opened, has `frames` frames at `rate`, unless a session of a later
    /// generation has published since.
    pub(super) fn publish(&self, generation: u64, path: &Path, frames: u64, rate: u32) {
        let mut latest = self.latest.lock().unwrap_or_else(PoisonError::into_inner);
        if latest
            .as_ref()
            .is_some_and(|published| published.generation > generation)
        {
            return;
        }
        *latest = Some(TrackLengthPublication {
            generation,
            path: path.to_path_buf(),
            frames,
            rate,
        });
    }

    /// The frames published of the file at `path`, if they are at `rate`;
    /// `None` for a length of another file, or of the file at another rate.
    pub(super) fn frames_of(&self, path: &Path, rate: u32) -> Option<u64> {
        self.latest
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .as_ref()
            .filter(|published| published.path == path && published.rate == rate)
            .map(|published| published.frames)
    }
}

/// Publish the length of a file that tells it. Without one there is
/// nothing to publish: a placeholder would put the end of the track, and
/// every view of it, where playback can pass it. What a decode of the same
/// file found out stays.
fn publish_track_length(
    out: &PublishedTrackLength,
    generation: u64,
    path: &Path,
    length: TrackLength,
    effective_rate: u32,
) {
    if let Some(frames) = length.frames {
        out.publish(generation, path, frames, effective_rate);
    }
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
    track_length_out: &PublishedTrackLength,
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

        // Between the right of a centered view and its left.
        if let Err(error) = step_backfill(session, source, event_tx, columns_produced_out) {
            session.failure = Some(format!("seeking failed ({error})"));
            return None;
        }
        if session.source_reached_eof {
            // A backfill after the end of the file: the session parks there.
            break;
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
        let helpers = may_use_helpers(source);
        if session_has_batch(session, helpers) {
            session_drain_stft_rows(session, event_tx, columns_produced_out, helpers);
            if session.failure.is_some() {
                return None;
            }
            continue;
        }

        #[cfg(test)]
        if decode_test_hooks::take_read_failure(&session.path, session.packet_counter) {
            session.failure = Some("decoding failed (injected)".to_string());
            return None;
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
                // The rows a session kept for a larger batch.
                if session_rows(session) > 0 {
                    let helpers = may_use_helpers(source);
                    session_drain_stft_rows(session, event_tx, columns_produced_out, helpers);
                    if session.failure.is_some() {
                        return None;
                    }
                    if session_rows(session) > 0 {
                        // It parks at its lookahead, and reads the end again
                        // once it has drained them.
                        continue;
                    }
                }
                if let Some(backfill) = session.backfill {
                    if backfill.resume.is_none() && session.pending_continue.is_none() {
                        // The right ends here. The chunks of the left carry
                        // where, and the end is published once they are
                        // sent: until then more columns come.
                        session_flush_chunk(session, event_tx, columns_produced_out);
                        record_end_column(session);
                        session.backfill = Some(Backfill {
                            right_ended: true,
                            ..backfill
                        });
                        continue;
                    }
                    // A continuation goes on from the right. Or the left
                    // reached the end of the file, before the playhead,
                    // which a padded duration estimate can put past the
                    // end: the right has no audio, whether it found none or
                    // went to the left before it read any, as it does for a
                    // playhead past the end the session knows. The file
                    // ends where the left did, not where the right started,
                    // and every column of the session is sent.
                    session.backfill = None;
                }
                // Real source EOF.  Publish the decoded extent while this
                // track is still current. Waiting until the parked worker
                // exits leaves the UI using a padded estimate.
                session.source_reached_eof = true;
                session_flush_chunk(session, event_tx, columns_produced_out);
                record_end_column(session);
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
        let helpers = may_use_helpers(source);
        if session_has_batch(session, helpers) {
            session_drain_stft_rows(session, event_tx, columns_produced_out, helpers);
            if session.failure.is_some() {
                return None;
            }
        }

        // 6. Update total_columns_estimate when the decoder produces more
        //    columns than initially estimated.  This happens for raw DTS/AC3
        //    files where GStreamer cannot query duration from the headerless
        //    bitstream.  Without this, the UI ring buffer is undersized and
        //    early columns get evicted, causing the spectrogram to go black.
        maybe_update_columns_estimate(session, source, generation, track_length_out);
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

    // Drain all pending commands, take the latest seek, and the latest
    // position after it.
    while let Ok(cmd) = next_session_command(session, || cmd_rx.try_recv()) {
        match cmd {
            SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
                latest_position = Some(position_seconds);
            }
            SpectrogramWorkerCommand::Seek { position_seconds } => {
                latest_seek = Some(position_seconds);
                latest_position = None;
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
        }
        if let Some(position_seconds) = latest_position {
            queue_session_command(
                &mut session.queued_commands,
                SpectrogramWorkerCommand::PositionUpdate { position_seconds },
            );
        }
        return Some(SessionAction::FlushToken);
    }

    if let Some(position_seconds) = latest_seek {
        let action = apply_seek(session, position_seconds);
        if let Some(position_seconds) = latest_position {
            if matches!(action, SessionAction::Continue) {
                return apply_position(session, position_seconds);
            }
            // The position played after the seek applies once it is made.
            queue_session_command(
                &mut session.queued_commands,
                SpectrogramWorkerCommand::PositionUpdate { position_seconds },
            );
        }
        return Some(action);
    }

    // Process position update — check if seek is needed.
    latest_position.and_then(|position_seconds| apply_position(session, position_seconds))
}

/// Where a seek for a playhead at `position_seconds` starts decoding. A
/// rolling view shows what has played since. A centered one shows the
/// columns around the playhead at once, so the session starts where its
/// analysis-thread restarts do, or the left of the view would stay empty.
fn session_seek_start_seconds(session: &SpectrogramSessionState, position_seconds: f64) -> f64 {
    if session.display_mode != SpectrogramDisplayMode::Centered {
        return position_seconds;
    }
    session_view(session).decode_start_seconds(position_seconds)
}

/// The UI's centered view of the session's columns. It ends where the file
/// does: once the session has decoded the end, at the end its chunks
/// carry; until then at the columns the session decodes of the file's
/// published frames, which for a raw stream are an estimate that can run
/// past the audio.
fn session_view(session: &SpectrogramSessionState) -> CenteredView {
    let end = match (session.timeline_of, session.end_column) {
        (ColumnTimeline::PlayingFile, Some(end)) => Some(end),
        (ColumnTimeline::PlayingFile, None) => session.track_frames.map(|frames| {
            centered_view::decoded_end_column(
                frames,
                session.fft_size,
                session.zoom_level,
                session.effective_rate,
            )
        }),
        (ColumnTimeline::EarlierFile { .. }, _) => None,
    };
    CenteredView::new(
        session.widget_width,
        session.zoom_level,
        session.effective_rate,
        end,
    )
}

/// Take a seek to `position_seconds`. A rolling view shows what plays from
/// there on, so the session always seeks. A centered view shows the
/// columns around the playhead at once: the session keeps decoding where it
/// is only if the columns it decoded cover the view, and otherwise seeks
/// where the view starts, to decode the playhead's columns first rather
/// than every column up to them.
///
/// The analysis thread seeks in the playing file, which the session decodes,
/// and adds no continuation's start to the position: from the seek on, the
/// columns are on that file's timeline.
fn apply_seek(session: &mut SpectrogramSessionState, position_seconds: f64) -> SessionAction {
    if decoded_columns_cover_view(session, position_seconds) {
        return apply_position(session, position_seconds).unwrap_or(SessionAction::Continue);
    }
    session.timeline_of = ColumnTimeline::PlayingFile;
    session.target_position_seconds = position_seconds;
    SessionAction::SeekRequired { position_seconds }
}

/// Whether the columns a centered session has decoded show its view with
/// the playhead at `position_seconds`: the view's left edge is in what the
/// session decodes and the UI's ring still holds, and the playhead's column
/// is decoded, or is about to be. The columns between them are then decoded
/// too, or are in the left of the view the session fills.
fn decoded_columns_cover_view(session: &SpectrogramSessionState, position_seconds: f64) -> bool {
    if session.display_mode != SpectrogramDisplayMode::Centered
        || session.timeline_of != ColumnTimeline::PlayingFile
    {
        return false;
    }
    let left_column = session_view(session).left_column(position_seconds);
    let frontier = decoded_frontier(session);
    let retained_left = session
        .session_start_column
        .max(frontier.saturating_sub(buffer_limits(session).capacity));
    if left_column < retained_left {
        return false;
    }
    let mut playhead = f64_to_u64_saturating(position_seconds * session.cols_per_second);
    if let Some(end) = session.end_column {
        // At the end of the file, the view shows its last column.
        playhead = playhead.min(end.saturating_sub(1));
    }
    // The columns decoded left of the playhead a session started at, and
    // from there to the right.
    let (left_decoded_end, right_start) = match session.backfill {
        Some(backfill) if backfill.resume.is_some() => (session.columns_produced, backfill.end),
        Some(backfill) => (backfill.next, backfill.end),
        None => (session.session_start_column, session.session_start_column),
    };
    let decoded = (session.session_start_column..left_decoded_end).contains(&playhead)
        || (right_start..frontier).contains(&playhead);
    let cursor = session.columns_produced;
    let decoded_next =
        (cursor..cursor.saturating_add(SEEK_DECODE_THROUGH_COLUMNS)).contains(&playhead);
    decoded || decoded_next
}

/// A centered seek to a playhead at most this many columns ahead of where
/// the session decodes goes on decoding from there: as many columns as a
/// backfill needs at least take less time than the seek.
const SEEK_DECODE_THROUGH_COLUMNS: u64 = MIN_BACKFILL_COLUMNS;

/// Take a new playback position. Returns the seek it needs, if the decoder
/// has to leave the columns it is decoding: a position in its file before
/// them, or a position that lies further ahead than the session would
/// decode. Playback moves the position on a little at a time, so a session
/// that is decoding columns ahead of it goes on with them.
///
/// On a continued timeline, a position before the file the session decodes
/// is in an earlier file, which the session decoded before it went on: the
/// outgoing track, still playing while the session decodes its successor.
/// Seeking the file it decodes cannot reach it, so it needs no seek. Nor
/// does a position in a successor that playback has switched to before the
/// session reached it: the session decodes the rest of its file first.
fn apply_position(
    session: &mut SpectrogramSessionState,
    position_seconds: f64,
) -> Option<SessionAction> {
    session.target_position_seconds = position_seconds;
    if position_seconds < file_start_seconds(session) {
        return None;
    }
    let target_col = f64_to_u64_saturating(position_seconds * session.cols_per_second);
    if target_col < session.session_start_column {
        // Backward seek needed.
        return Some(SessionAction::SeekRequired { position_seconds });
    }
    if playing_pending_continuation(session) {
        return None;
    }
    let start_col = f64_to_u64_saturating(
        session_seek_start_seconds(session, position_seconds) * session.cols_per_second,
    );
    // Far forward seek needed.
    (start_col.saturating_sub(decoded_frontier(session)) > session.lookahead_columns)
        .then_some(SessionAction::SeekRequired { position_seconds })
}

/// Whether playback has switched to the continuation the session goes on
/// with once its file ends. The analysis thread stages a continuation with
/// the outgoing track's token and gives it the successor's at the switch.
fn playing_pending_continuation(session: &SpectrogramSessionState) -> bool {
    session
        .pending_continue
        .as_ref()
        .is_some_and(|(_, token)| *token != session.track_token)
}

fn handle_single_command(
    session: &mut SpectrogramSessionState,
    cmd: SpectrogramWorkerCommand,
) -> SessionAction {
    match cmd {
        SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
            apply_position(session, position_seconds).unwrap_or(SessionAction::Continue)
        }
        SpectrogramWorkerCommand::Seek { position_seconds } => {
            apply_seek(session, position_seconds)
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
/// repositioning the file reader. `position_seconds` is on the session's
/// timeline: a seek the analysis thread sent has put the session on the
/// playing file's (see [`apply_seek`]), and one that a position needs on a
/// continued timeline stays on it, in the file the session decodes.
fn handle_session_seek(
    session: &mut SpectrogramSessionState,
    position_seconds: f64,
    source: &mut AudioFrameSource,
    event_tx: &dyn AnalysisEventOutput,
    reports: &Sender<SessionReport>,
) -> anyhow::Result<()> {
    // The columns before the file are an earlier file's.
    let start_seconds =
        session_seek_start_seconds(session, position_seconds).max(file_start_seconds(session));
    profile_eprintln!(
        "[spect-worker] SEEK to {start_seconds:.2}s for {position_seconds:.2}s file_start={:.2}s",
        file_start_seconds(session),
    );

    // Drop any partially accumulated pre-seek chunk. Emitting it after a seek
    // makes the UI paint old-timeline columns just before the reset arrives.
    session.chunk_buf.clear();
    session.chunk_columns = 0;

    let (new_column, _) = stft_seek_grid(
        start_seconds,
        session.effective_rate,
        session.hop_size,
        session.effective_hop,
    );
    let (playhead_column, _) = stft_seek_grid(
        position_seconds,
        session.effective_rate,
        session.hop_size,
        session.effective_hop,
    );
    let backfill = plan_backfill(session.display_mode, new_column, playhead_column);
    reposition_decoder(
        session,
        source,
        backfill.map_or(new_column, |backfill| backfill.end),
    )?;
    session.backfill = backfill;
    session.session_start_column = new_column;
    session.target_chunk_columns = 1; // Fast first-pixel after seek
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
            hop_size: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(session.fft_size).unwrap_or(u32::MAX),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: false,
            widget_width: session.widget_width,
        },
    ));
    let _ = reports.send(SessionReport::Repositioned {
        generation: session.gen,
        start_seconds,
    });

    Ok(())
}

// ---------------------------------------------------------------------------
// Playhead first: the left of a centered view after its right
// ---------------------------------------------------------------------------

/// A centered session decodes the columns left of its playhead after the
/// right only if there are at least this many; fewer take less time than
/// the seeks there and back.
const MIN_BACKFILL_COLUMNS: u64 = 64;

/// How far past the right edge of the view a centered session decodes
/// before it fills the left, and so how far playback goes on before the
/// session leaves the left for the right again.
const BACKFILL_RIGHT_MARGIN_SECONDS: f64 = 2.0;

/// The left of a centered view, which a session that starts left of its
/// playhead decodes after the playhead's columns: first from the playhead
/// to past the right edge of the view, then from the start of the session
/// up to the playhead, then on from the right. Every column lands on the
/// absolute grid, and the UI's ring takes each where it belongs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Backfill {
    /// The next column of the left to decode.
    next: u64,
    /// The column the session started at the playhead: the left ends
    /// before it.
    end: u64,
    /// While the session decodes the left, the column after the last it
    /// decoded on the right, where it goes on afterwards.
    resume: Option<u64>,
    /// The right reached the end of the file.
    right_ended: bool,
}

/// The backfill of a session in `display_mode` whose columns start at
/// `start_column`, with its playhead at `playhead_column`.
fn plan_backfill(
    display_mode: SpectrogramDisplayMode,
    start_column: u64,
    playhead_column: u64,
) -> Option<Backfill> {
    (display_mode == SpectrogramDisplayMode::Centered
        && playhead_column >= start_column.saturating_add(MIN_BACKFILL_COLUMNS))
    .then_some(Backfill {
        next: start_column,
        end: playhead_column,
        resume: None,
        right_ended: false,
    })
}

/// The column after the last the session decoded on the right, where the
/// UI's ring ends; while it decodes the left, that is not where it reads.
fn decoded_frontier(session: &SpectrogramSessionState) -> u64 {
    session
        .backfill
        .and_then(|backfill| backfill.resume)
        .unwrap_or(session.columns_produced)
}

/// The column at the right edge of the session's centered view of its
/// playback position.
fn view_right_column(session: &SpectrogramSessionState) -> u64 {
    let view = session_view(session);
    view.left_column(session.target_position_seconds) + view.window_columns()
}

/// Where a session decoding the right of its view leaves it for the left:
/// a margin past the view's right edge, or its lookahead if that is nearer.
fn backfill_right_end(session: &SpectrogramSessionState) -> u64 {
    let margin = f64_to_u64_saturating(BACKFILL_RIGHT_MARGIN_SECONDS * session.cols_per_second);
    let view_end = view_right_column(session).saturating_add(margin);
    if session.lookahead_columns == 0 {
        return view_end;
    }
    let target = f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
    view_end.min(target.saturating_add(session.lookahead_columns))
}

/// Whether the session has yet to decode its first column at the playhead,
/// which it computes as soon as it has the rows.
fn awaits_playhead_columns(session: &SpectrogramSessionState) -> bool {
    session.backfill.is_some_and(|backfill| {
        backfill.resume.is_none() && session.columns_produced == backfill.end
    })
}

/// Point the decoder at the absolute column `column`, which it decodes
/// next, dropping the rows and columns it has not sent.
fn reposition_decoder(
    session: &mut SpectrogramSessionState,
    source: &mut AudioFrameSource,
    column: u64,
) -> anyhow::Result<()> {
    let grid_frame = column_grid_frame(column, session.hop_size, session.effective_hop);
    let (frame, leading_silence) = file_window_start(
        grid_frame,
        session.fft_size,
        session.timeline_of.file_start_frame(),
    );
    source.seek_to_frame(frame, session.effective_rate)?;
    session.source_reached_eof = false;
    session.timeline = AudioTimeline::new(frame);
    for stft in &mut session.stfts {
        stft.reset();
    }
    enqueue_leading_silence(&mut session.stfts, leading_silence);
    for resampler in &mut session.resamplers {
        resampler.reset_at_column(column, session.hop_size, session.effective_hop);
    }
    session.columns_produced = column;
    session.chunk_buf.clear();
    session.chunk_columns = 0;
    session.chunk_start_index = column;
    session.total_covered_samples = 0;
    Ok(())
}

/// Move a session that decodes its playhead's columns first between the
/// right of its view and the left: to the left once the right reaches
/// [`backfill_right_end`] or the end of the file, back to the right when
/// playback gets near the right edge of what it decoded there, and on with
/// the right once the left is done. A view that stops being centered gets
/// the right only, as a rolling view shows columns in the order they come.
fn step_backfill(
    session: &mut SpectrogramSessionState,
    source: &mut AudioFrameSource,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) -> anyhow::Result<()> {
    let Some(backfill) = session.backfill else {
        return Ok(());
    };
    if session.display_mode != SpectrogramDisplayMode::Centered {
        return end_backfill(session, source, event_tx, columns_produced_out);
    }
    match backfill.resume {
        None if backfill.right_ended || session.columns_produced >= backfill_right_end(session) => {
            session_emit_pending_chunk(session, event_tx, columns_produced_out);
            let resume = session.columns_produced;
            profile_eprintln!(
                "[spect-worker] backfill {}..{} (right decoded to {resume})",
                backfill.next,
                backfill.end,
            );
            reposition_decoder(session, source, backfill.next)?;
            session.backfill = Some(Backfill {
                resume: Some(resume),
                ..backfill
            });
        }
        Some(_) if session.columns_produced >= backfill.end => {
            end_backfill(session, source, event_tx, columns_produced_out)?;
        }
        Some(resume) if !backfill.right_ended && resume < view_right_column(session) => {
            // Playback caught up with the right: decode it ahead again.
            session_emit_pending_chunk(session, event_tx, columns_produced_out);
            let next = session.columns_produced;
            reposition_decoder(session, source, resume)?;
            session.backfill = Some(Backfill {
                next,
                resume: None,
                ..backfill
            });
        }
        _ => {}
    }
    Ok(())
}

/// The session has decoded its file to the end: the column after its last
/// is the file's extent, which the chunks it sends from now on carry,
/// rather than an estimate that may be padded or a fallback.
fn record_end_column(session: &mut SpectrogramSessionState) {
    session.end_column = Some(session.columns_produced);
    session.total_columns_estimate = u64_to_u32_saturating(session.columns_produced);
}

/// Stop filling the left of the view, and go on with the right where the
/// session left it, or park at its end, publishing it.
fn end_backfill(
    session: &mut SpectrogramSessionState,
    source: &mut AudioFrameSource,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) -> anyhow::Result<()> {
    let Some(backfill) = session.backfill else {
        return Ok(());
    };
    let Some(resume) = backfill.resume else {
        session.backfill = None;
        return Ok(());
    };
    session_emit_pending_chunk(session, event_tx, columns_produced_out);
    session.backfill = None;
    if backfill.right_ended {
        // Every column is sent: publish the end, and park there.
        session.columns_produced = resume;
        session.chunk_start_index = resume;
        session.source_reached_eof = true;
        session_emit_finalize_chunk(session, event_tx);
        return Ok(());
    }
    reposition_decoder(session, source, resume)
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
    session.lookahead_columns = buffer_limits(session).lookahead;
}

/// How far the session decodes ahead of its playhead, and how many columns
/// the UI's ring holds, which the UI works out the same way.
fn buffer_limits(session: &SpectrogramSessionState) -> SpectrogramBufferLimits {
    SpectrogramBufferConfig {
        width: session.widget_width,
        sample_rate: session.effective_rate,
        hop: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
        bins: u32::try_from(session.bins_per_column).unwrap_or(u32::MAX),
        channels: u32::try_from(session.channel_count).unwrap_or(u32::MAX),
        zoom: f64::from(session.zoom_level),
        centered: session.display_mode == SpectrogramDisplayMode::Centered,
    }
    .limits(session.lookahead_seconds)
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
    // A tiny rate limit makes the wait too long for a `Duration`; the caller
    // caps each wait anyway.
    Some(
        Duration::try_from_secs_f64((expected_elapsed - elapsed).max(0.0)).unwrap_or(Duration::MAX),
    )
    .filter(|wait| !wait.is_zero())
}

// ---------------------------------------------------------------------------
// STFT row draining / chunk emission
// ---------------------------------------------------------------------------

/// Compute and send the columns of the rows the session has queued, up to
/// its lookahead, on helper threads too if `helpers`.
fn session_drain_stft_rows(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
    helpers: bool,
) {
    stft_batch::with_thread_batcher(|batcher| loop {
        let allowed = columns_before_park(session);
        let max_columns = usize::try_from(allowed)
            .unwrap_or(usize::MAX)
            .min(stft_batch::MAX_BATCH_COLUMNS);
        let columns = match batcher.compute(
            &mut session.stfts,
            &mut session.resamplers,
            max_columns,
            session.bins_per_column,
            session.fft_size,
            helpers,
        ) {
            Ok(0) => break,
            Ok(columns) => columns,
            Err(error) => {
                session.failure = Some(error);
                break;
            }
        };
        for column in 0..columns {
            batcher.append_column(column, &mut session.chunk_buf);
            session.chunk_columns += 1;
            session.columns_produced += 1;
            if session.chunk_columns >= session.target_chunk_columns {
                session_send_full_chunk(session, event_tx, columns_produced_out);
            }
        }
    });
}

/// The rows every channel of the session has queued.
fn session_rows(session: &SpectrogramSessionState) -> usize {
    session
        .stfts
        .iter()
        .map(StftComputer::available_rows)
        .min()
        .unwrap_or(0)
}

/// Whether a session reading `source` computes columns on helper threads
/// too. A source that decodes on its own thread already runs beside the
/// session's; the session keeps up with it alone, and batches of rows would
/// only make the two wait for each other.
fn may_use_helpers(source: &AudioFrameSource) -> bool {
    !source.decodes_on_its_own_thread()
}

/// Whether the session computes the rows it has queued now, rather than
/// decode more first. A session whose batches run on helper threads waits
/// for [`stft_batch::PARALLEL_BATCH_ROWS`] rows, as waking the helpers for
/// a packet's few rows costs more CPU than it saves, except in its
/// post-reset window, which is there to show columns soon. The end of the
/// file drains them.
fn session_has_batch(session: &SpectrogramSessionState, helpers: bool) -> bool {
    let rows = session_rows(session);
    rows > 0
        && (rows >= stft_batch::PARALLEL_BATCH_ROWS
            || post_reset_window_active(session)
            || awaits_playhead_columns(session)
            || !helpers
            || !stft_batch::with_thread_batcher(|batcher| {
                batcher.runs_in_parallel(session.stfts.len())
            }))
}

/// How many more columns the session decodes before it parks at its
/// lookahead: none once it leads the playback position by the lookahead,
/// unless it is still in its post-reset window.
fn columns_before_park(session: &SpectrogramSessionState) -> u64 {
    match session.backfill {
        // Filling the left of the view, up to the playhead's columns.
        Some(Backfill {
            resume: Some(_),
            end,
            ..
        }) => end.saturating_sub(session.columns_produced),
        // Decoding its right, up to where the left comes next.
        Some(_) => columns_before_lookahead_park(session)
            .min(backfill_right_end(session).saturating_sub(session.columns_produced)),
        None => columns_before_lookahead_park(session),
    }
}

/// [`columns_before_park`] for the lookahead.
fn columns_before_lookahead_park(session: &SpectrogramSessionState) -> u64 {
    let target = f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
    let lookahead_end = if session.lookahead_columns == 0 {
        0
    } else {
        target.saturating_add(session.lookahead_columns)
    };
    let window = u64::from(session.post_reset_unthrottled_columns);
    let window_end = if window == 0 {
        0
    } else {
        session.session_start_column.saturating_add(window)
    };
    lookahead_end
        .max(window_end)
        .saturating_sub(session.columns_produced)
}

/// Send the session's buffered columns, which have reached the target
/// chunk size, and grow the next chunk.
fn session_send_full_chunk(
    session: &mut SpectrogramSessionState,
    event_tx: &dyn AnalysisEventOutput,
    columns_produced_out: &AtomicU64,
) {
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
            hop_size: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(session.fft_size).unwrap_or(u32::MAX),
            coverage_seconds: coverage,
            complete: false,
            buffer_reset: false,
            clear_history: false,
            widget_width: session.widget_width,
        },
    ));
    session.chunk_start_index = session.columns_produced;
    session.chunk_columns = 0;
    columns_produced_out.store(decoded_frontier(session), Ordering::Relaxed);
    // Ramp up chunk size in rolling mode only to a latency-friendly cap.
    session.target_chunk_columns = next_target_chunk_columns_for_payload(
        session.target_chunk_columns,
        session.display_mode,
        session.bins_per_column,
        session.channel_count,
    );
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
/// The raw math (`frames / ESTIMATE_COLUMN_FRAMES`) counts estimate
/// columns — but `session.total_columns_estimate` is always stored in
/// the current session's `effective_hop` cols, so we must scale.
/// Without this scaling, sessions of longer columns
/// (`effective_hop > ESTIMATE_COLUMN_FRAMES`) see a figure that is
/// always larger than the correctly-scaled in-session estimate and
/// the estimate gets inflated by `effective_hop / ESTIMATE_COLUMN_FRAMES`,
/// breaking the zoom-out minZoom computation on the Qt side.
#[cfg(any(feature = "gst", test))]
fn duration_requery_estimate_for_session(effective_frames: u64, effective_hop: usize) -> u32 {
    scale_columns_estimate(
        super::decoders::columns_estimate(effective_frames),
        effective_hop,
    )
}

/// Tell the analysis thread, which starts every other session of the file,
/// the length a re-query found: each of them opens the file anew, without
/// it. The query can outlast the session, whose length then is not that of
/// the track that the analysis thread has moved on to.
#[cfg(any(feature = "gst", test))]
fn publish_requeried_duration(
    session: &mut SpectrogramSessionState,
    total_frames: u64,
    generation: &AtomicU64,
    track_length_out: &PublishedTrackLength,
) {
    if generation.load(Ordering::Relaxed) != session.gen {
        return;
    }
    track_length_out.publish(
        session.gen,
        &session.path,
        total_frames,
        session.effective_rate,
    );
    session.track_frames = Some(total_frames);
}

fn maybe_update_columns_estimate(
    session: &mut SpectrogramSessionState,
    source: &AudioFrameSource,
    generation: &AtomicU64,
    track_length_out: &PublishedTrackLength,
) {
    // The file's end, once decoded, is its extent.
    if session.end_column.is_some() {
        return;
    }
    let estimate = u64::from(session.total_columns_estimate);
    let produced = decoded_frontier(session);

    // Attempt a GStreamer duration re-query early in the session.
    #[cfg(feature = "gst")]
    if !session.gst_duration_requeried && session.packet_counter >= 20 {
        session.gst_duration_requeried = true;
        if let Some(ns) = source.query_duration_ns() {
            let rate = u64::from(session.effective_rate);
            let total_frames = ns * rate / 1_000_000_000;
            let effective = total_frames;
            publish_requeried_duration(session, effective, generation, track_length_out);
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

    // Suppress the unused-variable warnings when GStreamer is disabled.
    let _ = (source, generation, track_length_out);

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
    // inflated zoom-out estimates by 2× (effective_hop > ESTIMATE_COLUMN_FRAMES
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
    // The left of a view ends where its right began: a partial interval
    // there is not the end of anything.
    if session
        .backfill
        .is_some_and(|backfill| backfill.resume.is_some())
    {
        session_emit_pending_chunk(session, event_tx, columns_produced_out);
        return;
    }
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
                hop_size: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
                fft_size: u32::try_from(session.fft_size).unwrap_or(u32::MAX),
                coverage_seconds: coverage,
                complete: false,
                buffer_reset: false,
                clear_history: false,
                widget_width: session.widget_width,
            },
        ));
        session.chunk_columns = 0;
        session.chunk_start_index = session.columns_produced;
    }
    // Always publish the final columns_produced so the analysis runtime
    // has the exact boundary at EOF for staged-commit alignment.
    columns_produced_out.store(decoded_frontier(session), Ordering::Relaxed);
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
            start_column_index: u64_to_u32_saturating(decoded_frontier(session)),
            total_columns_estimate: session.total_columns_estimate,
            sample_rate_hz: session.effective_rate,
            hop_size: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(session.fft_size).unwrap_or(u32::MAX),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: false,
            clear_history: false,
            widget_width: session.widget_width,
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
    let final_cols = u64_to_u32_saturating(decoded_frontier(session));
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
            hop_size: u32::try_from(session.effective_hop).unwrap_or(u32::MAX),
            fft_size: u32::try_from(session.fft_size).unwrap_or(u32::MAX),
            coverage_seconds: 0.0,
            complete: true,
            buffer_reset: false,
            clear_history: false,
            widget_width: session.widget_width,
        },
    ));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::{unbounded, RecvTimeoutError};

    /// How long a test waits for a worker's output before it fails. The
    /// tests wait for the output itself; this bound matters only when it
    /// never comes, so a starved worker has time to produce it.
    const OUTPUT_WAIT: Duration = Duration::from_secs(20);

    /// Wait for the next chunk that satisfies `wanted`. `Err` names why
    /// none came: the worker ended, which closes its events, or it sent
    /// nothing for [`OUTPUT_WAIT`].
    fn wait_for_chunk(
        events: &Receiver<AnalysisEvent>,
        mut wanted: impl FnMut(&PrecomputedSpectrogramChunk) -> bool,
    ) -> Result<PrecomputedSpectrogramChunk, RecvTimeoutError> {
        loop {
            if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) =
                events.recv_timeout(OUTPUT_WAIT)?
            {
                if wanted(&chunk) {
                    return Ok(chunk);
                }
            }
        }
    }

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
                    &PublishedTrackLength::default(),
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
                        frequency_cap: false,
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
                // The worker holds the only event sender, so a worker that
                // stopped permanently closes the events.
                if let Err(error) = wait_for_chunk(&event_rx, |chunk| {
                    chunk.track_token == token && chunk.complete
                }) {
                    panic!("no EOF for track {token}: {error}");
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
    fn a_tiny_rate_limit_waits_as_long_as_possible() {
        // FERROUS_SPECTROGRAM_DECODE_RATE=1e-300 makes the wait infinite,
        // and 1e-30 too long for a `Duration`.
        for rate in [1e-300, 1e-30] {
            let mut session = make_test_session();
            session.post_reset_unthrottled_columns = 0;
            session.columns_produced = session.session_start_column;
            session.decode_rate_limit = rate;
            let start = std::time::Instant::now();
            assert_eq!(throttle_wait(&mut session, start), None);
            session.columns_produced += 1;
            assert_eq!(throttle_wait(&mut session, start), Some(Duration::MAX));
        }
    }

    #[test]
    fn a_session_that_ends_without_a_successor_is_reported_and_one_parked_at_its_end_is_not() {
        let fixture = include_bytes!("fixtures/seek-tone.mp3");
        let folder = crate::paths::test_root().join(format!("session-ends-{}", std::process::id()));
        std::fs::create_dir_all(&folder).expect("folder");
        let missing = folder.join("missing.mp3");
        let parked = folder.join("parked.mp3");
        let failing = folder.join("failing.mp3");
        std::fs::write(&parked, fixture).expect("fixture");
        std::fs::write(&failing, fixture).expect("fixture");
        decode_test_hooks::fail_reads_of(&failing, 2);
        let new_track = |generation, path: &Path| SpectrogramWorkerCommand::NewTrack {
            track_token: 1,
            generation,
            path: path.to_path_buf(),
            fft_size: 512,
            frequency_cap: false,
            zoom_level: 1.0,
            widget_width: 320,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            handoff: SessionHandoff::default(),
        };
        let (commands, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let (report_tx, reports) = unbounded();
        let generation = Arc::new(AtomicU64::new(1));
        let worker_generation = Arc::clone(&generation);
        let worker = std::thread::spawn(move || {
            spectrogram_worker_loop(
                &command_rx,
                &events,
                &worker_generation,
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
                &report_tx,
            );
        });
        let next_report = || reports.recv_timeout(OUTPUT_WAIT).expect("a report");

        // A file that cannot be opened: playback tells the user, and the
        // worker reports only that the session ended.
        commands.send(new_track(1, &missing)).expect("send");
        assert_eq!(next_report(), SessionReport::Ended { generation: 1 });

        // A session at the end of its file parks there, and takes a seek.
        generation.store(2, Ordering::Relaxed);
        commands.send(new_track(2, &parked)).expect("send");
        assert!(matches!(
            next_report(),
            SessionReport::Opened { generation: 2, .. }
        ));
        wait_for_chunk(&event_rx, |chunk| chunk.generation == 2 && chunk.complete)
            .expect("the end of the file");
        commands
            .send(SpectrogramWorkerCommand::Seek {
                position_seconds: 0.25,
            })
            .expect("send");
        assert!(matches!(
            next_report(),
            SessionReport::Repositioned { generation: 2, .. }
        ));

        // A session that cannot read its audio reports why, then its end.
        generation.store(3, Ordering::Relaxed);
        commands.send(new_track(3, &failing)).expect("send");
        assert!(matches!(
            next_report(),
            SessionReport::Opened { generation: 3, .. }
        ));
        assert!(matches!(
            next_report(),
            SessionReport::Failed { generation: 3, ref error, .. }
                if error == "decoding failed (injected)"
        ));
        assert_eq!(next_report(), SessionReport::Ended { generation: 3 });
        drop(commands);
        worker.join().expect("join worker");
        assert!(
            reports.try_iter().all(|report| report.generation() == 3),
            "nothing about the parked session"
        );
        std::fs::remove_dir_all(folder).expect("remove fixtures");
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
                    frequency_cap: false,
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
                    &PublishedTrackLength::default(),
                    &unbounded().0,
                )
            });
            // The worker parks at EOF until its next command, so an EOF
            // published only when the session ends would not arrive.
            let wait_for_eof = || wait_for_chunk(&event_rx, |chunk| chunk.complete);
            let first = wait_for_eof();
            commands
                .send(SpectrogramWorkerCommand::Seek {
                    position_seconds: 0.25,
                })
                .expect("backward seek");
            // A rolling view shows what plays from the seek on, so the
            // session decodes it again. A centered one has every column of
            // the short file, which covers the view: the session keeps it.
            let rolling = mode == SpectrogramDisplayMode::Rolling;
            let second = rolling.then(wait_for_eof);
            commands
                .send(SpectrogramWorkerCommand::StopSession)
                .expect("stop worker");
            worker.join().expect("join worker");
            std::fs::remove_file(path).expect("remove fixture");
            if !rolling {
                assert!(
                    !event_rx.try_iter().any(|event| matches!(
                        event,
                        AnalysisEvent::PrecomputedSpectrogramChunk(_)
                    )),
                    "a centered seek inside the decoded columns decoded again"
                );
            }
            for result in std::iter::once(first).chain(second) {
                let chunk = result.unwrap_or_else(|error| {
                    panic!("EOF must arrive without replacing or stopping the track: {error}")
                });
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
        let complete = wait_for_chunk(rx, |chunk| {
            if chunk.complete {
                return true;
            }
            let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                columns.insert(
                    chunk.start_column_index + u32::try_from(offset).unwrap(),
                    column.to_vec(),
                );
            }
            false
        });
        if let Err(error) = complete {
            panic!("no EOF from the worker: {error}");
        }
        columns
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
        for rate in [44_100, 48_000, 96_000, 192_000, 32_000] {
            for zoom in [0.05, 0.17, 0.77, 1.0, 1.5] {
                let hops = column_hops(1024, zoom, rate);
                let mut resampler = PeakHoldResampler::new(hops.output_interval);
                let count = (0..100_000)
                    .filter(|_| resampler.push(&[1.0]).is_some())
                    .count();
                assert_eq!(
                    count,
                    100_000 * hops.stft_hop / hops.effective_hop,
                    "{rate} Hz at zoom {zoom}"
                );
            }
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
                let (commands, command_rx) = unbounded();
                let (events, event_rx) = unbounded();
                let command = SpectrogramWorkerCommand::NewTrack {
                    track_token: 1,
                    generation: 1,
                    path: path.clone(),
                    fft_size: fft,
                    frequency_cap: false,
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
                        &PublishedTrackLength::default(),
                        &unbounded().0,
                    )
                });
                let reference = collect_session_columns(&event_rx);
                // A centered session keeps the columns of the short file
                // through a seek, as they cover its view; its seeks to a
                // view it has not decoded are on the grid in
                // `a_seek_in_a_centered_session_decodes_the_playhead_first_and_during_the_left_starts_over`.
                let explicit_seeks = if mode == SpectrogramDisplayMode::Rolling {
                    16
                } else {
                    0
                };
                for seek in [0.001, 0.231_234, 0.731_234, 0.031_234]
                    .into_iter()
                    .cycle()
                    .take(explicit_seeks)
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
                        &PublishedTrackLength::default(),
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

    /// A raw DTS stream whose sample rate changes mid-stream, as a DTS 96/24
    /// stream's does where its decoder falls back to the 48 kHz core of a
    /// damaged frame: a 440 Hz tone, 1.0 s at 48 kHz, then 1.02 s at 44.1
    /// kHz, then 1.0 s at 48 kHz again.
    #[cfg(feature = "gst")]
    fn write_rate_change_fixture() -> PathBuf {
        use crate::raw_audio::{TEST_DTS, TEST_DTS_44K};

        let path = crate::paths::test_root().join("session-rate-change.dts");
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        let stream = [
            TEST_DTS,
            TEST_DTS,
            TEST_DTS_44K,
            TEST_DTS_44K,
            TEST_DTS,
            TEST_DTS,
        ]
        .concat();
        std::fs::write(&path, stream).expect("write fixture");
        path
    }

    /// The columns of the session that sends `events`, up to its end, by
    /// index. Fails with the session's error if it fails.
    #[cfg(feature = "gst")]
    fn session_columns_to_end(
        events: &Receiver<AnalysisEvent>,
        reports: &Receiver<SessionReport>,
    ) -> std::collections::BTreeMap<u32, Vec<u8>> {
        let mut columns = std::collections::BTreeMap::new();
        let end = wait_for_chunk(events, |chunk| {
            // A failing session clears the view with a chunk of no bins.
            if chunk.complete || (chunk.buffer_reset && chunk.bins_per_column == 0) {
                return true;
            }
            let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                columns.insert(
                    chunk.start_column_index + u32::try_from(offset).unwrap(),
                    column.to_vec(),
                );
            }
            false
        })
        .unwrap_or_else(|error| panic!("no end of the session: {error}"));
        if !end.complete {
            let error =
                std::iter::from_fn(|| reports.recv_timeout(OUTPUT_WAIT).ok()).find_map(|report| {
                    match report {
                        SessionReport::Failed { error, .. } => Some(error),
                        _ => None,
                    }
                });
            panic!(
                "the session failed after {} columns: {error:?}",
                columns.len()
            );
        }
        columns
    }

    /// The bin of the loudest value of `column`.
    #[cfg(feature = "gst")]
    fn loudest_bin(column: &[u8]) -> usize {
        column
            .iter()
            .enumerate()
            .max_by_key(|&(_, value)| *value)
            .map(|(bin, _)| bin)
            .expect("bins")
    }

    /// The spectrogram of a stream whose sample rate changes mid-stream
    /// goes on over all of it, at the rate it started at, with its columns
    /// on the absolute grid: there is a column for every hop of the stream,
    /// the tone stays in its bin, also where the stream is at 44.1 kHz, and
    /// seeks into, past and back across that stretch land on their columns.
    #[cfg(feature = "gst")]
    #[test]
    fn a_stream_whose_rate_changes_mid_stream_gets_columns_over_all_of_it() {
        // The frames of the fixture at 48 kHz: 1.0 s at 48 kHz, 1.021 s at
        // 44.1 kHz, then 1.0 s at 48 kHz.
        const STREAM_FRAMES: usize = 145_296;
        let path = write_rate_change_fixture();
        let (fft, zoom) = (8_192, 1.0);
        let hop = column_hops(fft, zoom, 48_000).stft_hop;
        let command = SpectrogramWorkerCommand::NewTrack {
            track_token: 1,
            generation: 1,
            path: path.clone(),
            fft_size: fft,
            frequency_cap: false,
            zoom_level: zoom,
            widget_width: 320,
            channel_count: 1,
            start_seconds: 0.0,
            target_position_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::PerChannel,
            display_mode: SpectrogramDisplayMode::Centered,
            handoff: SessionHandoff::default(),
        };
        let (commands, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let (reports, report_rx) = unbounded();
        let worker = std::thread::spawn(move || {
            run_spectrogram_session(
                QueuedCommand::alone(command),
                &command_rx,
                &events,
                &AtomicU64::new(1),
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
                &reports,
            )
        });

        // A column for each hop whose window the stream fills, from the
        // first on, without a gap.
        let reference = session_columns_to_end(&event_rx, &report_rx);
        let count = u32::try_from(reference.len()).unwrap();
        assert!(reference.keys().copied().eq(0..count));
        let expected = (STREAM_FRAMES - fft / 2) / hop + 1;
        assert!(
            reference.len().abs_diff(expected) <= 1,
            "{} columns of {expected}",
            reference.len()
        );
        // The 440 Hz tone, at 44.1 kHz analysed as 48 kHz audio, would be
        // seven bins higher.
        let tone = loudest_bin(&reference[&10]);
        for (index, column) in &reference {
            assert!(
                loudest_bin(column).abs_diff(tone) <= 1,
                "column {index}: the tone is in bin {} of {tone}",
                loudest_bin(column)
            );
        }

        // Seeks start at the playhead when rolling.
        commands
            .send(SpectrogramWorkerCommand::SetDisplayMode(
                SpectrogramDisplayMode::Rolling,
            ))
            .unwrap();
        // The columns whose windows end before the stream changes rate.
        let before_change = stft_seek_grid(1.0, 48_000, hop, hop).0 - fft as u64 / 2 / hop as u64;
        for seek in [2.5, 1.5, 0.7] {
            commands
                .send(SpectrogramWorkerCommand::Seek {
                    position_seconds: seek,
                })
                .unwrap();
            let columns = session_columns_to_end(&event_rx, &report_rx);
            let first = u32::try_from(stft_seek_grid(seek, 48_000, hop, hop).0).unwrap();
            assert!(
                columns.keys().copied().eq(first..count),
                "seek {seek}: columns {:?} to {:?}",
                columns.keys().next(),
                columns.keys().last()
            );
            for (index, column) in &columns {
                if seek > 2.1 || u64::from(*index) < before_change {
                    // The audio the reading from the start decoded.
                    assert_eq!(column, &reference[index], "seek {seek}: column {index}");
                } else {
                    // The fixture's parser times a seek into its stretch at
                    // 44.1 kHz, whose frames are shorter, about 1 ms early,
                    // also without any conversion, and that till the end. A
                    // DTS 96/24 stream keeps the length of its frames.
                    assert!(
                        loudest_bin(column).abs_diff(tone) <= 1,
                        "seek {seek}: column {index}"
                    );
                }
            }
        }
        commands
            .send(SpectrogramWorkerCommand::StopSession)
            .unwrap();
        worker.join().unwrap();
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
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
            track_frames: None,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            target_position_seconds: 2.0,
            timeline_of: ColumnTimeline::PlayingFile,
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
            backfill: None,
            end_column: None,
        }
    }

    #[test]
    fn continued_file_republishes_track_duration() {
        // Compatible ContinueWithFile handoffs do not create a new worker
        // session.  Publishing the next file's length must therefore
        // replace the outgoing file's rather than leave centered restart
        // math in its unknown-duration fallback.
        let published = PublishedTrackLength::default();
        for (path, frames) in [("a.flac", 2_048_000), ("b.flac", 4_096_000)] {
            let length = TrackLength {
                frames: Some(frames),
                columns: super::super::decoders::columns_estimate(frames),
            };
            publish_track_length(&published, 1, Path::new(path), length, 48_000);
            assert_eq!(published.frames_of(Path::new(path), 48_000), Some(frames));
        }
        assert_eq!(published.frames_of(Path::new("a.flac"), 48_000), None);
    }

    #[test]
    fn a_length_a_file_does_not_tell_is_not_published_as_its_duration() {
        // The placeholder for a raw stream would end its view at 300 s.
        let published = PublishedTrackLength::default();
        let path = Path::new("raw.ac3");
        let placeholder = TrackLength {
            frames: None,
            columns: 14_126,
        };
        publish_track_length(&published, 1, path, placeholder, 48_000);
        assert_eq!(published.frames_of(path, 48_000), None);

        // Nor does it replace what a decode of the file found out.
        published.publish(1, path, 751 * 48_000, 48_000);
        publish_track_length(&published, 2, path, placeholder, 48_000);
        assert_eq!(published.frames_of(path, 48_000), Some(751 * 48_000));

        let known = TrackLength {
            frames: Some(2_048_000),
            columns: 2_064,
        };
        publish_track_length(&published, 2, path, known, 48_000);
        assert_eq!(published.frames_of(path, 48_000), Some(2_048_000));
    }

    #[test]
    fn a_published_length_is_the_tracks_frames_without_the_estimates_allowance() {
        // 320 s at 8 kHz, whose column estimate runs 64 columns of 1024
        // frames, 8.2 s, past the end. A view placed by that would reach
        // further left, once the UI pins it to the end, than its decode.
        let frames = 320 * 8_000;
        let length = TrackLength {
            frames: Some(frames),
            columns: super::super::decoders::columns_estimate(frames),
        };
        assert_eq!(length.columns, 2_500 + 64);
        let published = PublishedTrackLength::default();
        let path = Path::new("track.wav");
        publish_track_length(&published, 1, path, length, 8_000);
        assert_eq!(published.frames_of(path, 8_000), Some(frames));
        // A length at another rate, or of another file, is not this one's.
        assert_eq!(published.frames_of(path, 44_100), None);
        assert_eq!(published.frames_of(Path::new("other.wav"), 8_000), None);
    }

    #[test]
    fn a_session_publishes_no_length_over_a_later_sessions() {
        // The probe of the outgoing track's file ends after the new track's
        // session has published: the new track keeps its length.
        let published = PublishedTrackLength::default();
        let (outgoing, playing) = (Path::new("outgoing.flac"), Path::new("playing.flac"));
        published.publish(2, playing, 480_000, 8_000);
        published.publish(1, outgoing, 320_000, 8_000);
        assert_eq!(published.frames_of(playing, 8_000), Some(480_000));
        assert_eq!(published.frames_of(outgoing, 8_000), None);
        // Ending before it, it is of a file no longer playing: the playing
        // one's length is unknown until its session publishes it.
        let published = PublishedTrackLength::default();
        published.publish(1, outgoing, 320_000, 8_000);
        assert_eq!(published.frames_of(playing, 8_000), None);
        published.publish(2, playing, 480_000, 8_000);
        assert_eq!(published.frames_of(playing, 8_000), Some(480_000));
        // A session of the same generation, which went on with the next
        // file gaplessly, publishes that file's.
        published.publish(2, outgoing, 320_000, 8_000);
        assert_eq!(published.frames_of(outgoing, 8_000), Some(320_000));
    }

    #[test]
    fn a_requeried_duration_reaches_the_analysis_thread_and_the_session() {
        let mut session = make_test_session();
        let published = PublishedTrackLength::default();
        let generation = AtomicU64::new(session.gen);
        publish_requeried_duration(&mut session, 751 * 48_000, &generation, &published);
        // The track's frames, without the column estimate's allowance.
        assert_eq!(
            published.frames_of(&session.path, 48_000),
            Some(751 * 48_000)
        );
        assert_eq!(session.track_frames, Some(751 * 48_000));
    }

    #[test]
    fn a_superseded_session_does_not_publish_its_requeried_duration() {
        let mut session = make_test_session();
        let published = PublishedTrackLength::default();
        let generation = AtomicU64::new(session.gen + 1);
        publish_requeried_duration(&mut session, 751 * 48_000, &generation, &published);
        assert_eq!(published.frames_of(&session.path, 48_000), None);
    }

    /// A centered session decoding at 258 s, further behind
    /// `position_seconds` than its lookahead, in a track of unknown length.
    fn centered_session_behind(position_seconds: f64) -> SpectrogramSessionState {
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.widget_width = 1185;
        session.columns_produced = 12_112;
        session.session_start_column = 12_112;
        session.lookahead_columns = 2_730;
        session.target_position_seconds = position_seconds;
        session
    }

    #[test]
    fn a_centered_seek_starts_where_the_view_does() {
        let mut session = centered_session_behind(609.7);
        let action = apply_position(&mut session, 609.7);
        assert!(matches!(
            action,
            Some(SessionAction::SeekRequired { position_seconds }) if position_seconds == 609.7
        ));
        // 12.64 s of half a view, and 2 s of warm-up.
        let start = session_seek_start_seconds(&session, 609.7);
        assert!((start - 595.06).abs() < 0.01, "{start}");

        // Near the end of the track, the view starts a window before the
        // column after its last, 29062 of the 620 s.
        session.track_frames = Some(620 * 48_000);
        let start = session_seek_start_seconds(&session, 615.0);
        let expected = (29_062.0 - 1_185.0) / 46.875 - 2.0;
        assert!((start - expected).abs() < 1e-9, "{start}");
    }

    #[test]
    fn a_session_that_seeked_for_its_view_stays_there() {
        let mut session = centered_session_behind(609.7);
        let start = session_seek_start_seconds(&session, 609.7);
        let (column, _) = stft_seek_grid(start, 48_000, 256, 1_024);
        session.columns_produced = column;
        session.session_start_column = column;
        // Otherwise each position would seek again, and no column be decoded.
        assert!(apply_position(&mut session, 609.7).is_none());
        assert!(apply_position(&mut session, 609.74).is_none());
    }

    #[test]
    fn a_view_of_the_whole_track_does_not_seek_for_a_playhead_it_shows() {
        // Zoomed out to the whole track, a view starts at the track's start
        // wherever the playhead is, even further than the lookahead away.
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.columns_produced = 0;
        session.lookahead_columns = 1_920;
        session.track_frames = Some(45 * 48_000);
        session.widget_width = 1920;
        assert!(apply_position(&mut session, 44.0).is_none());
        assert!(session_seek_start_seconds(&session, 44.0).abs() < f64::EPSILON);
    }

    #[test]
    fn a_rolling_seek_starts_at_the_playhead() {
        let mut session = make_test_session();
        session.columns_produced = 0;
        assert!((session_seek_start_seconds(&session, 300.0) - 300.0).abs() < f64::EPSILON);
        // A gap of the lookahead does not seek; one column more does.
        assert!(apply_position(&mut session, 512.0 / 46.875).is_none());
        assert!(matches!(
            apply_position(&mut session, 513.5 / 46.875),
            Some(SessionAction::SeekRequired { .. })
        ));
    }

    #[test]
    fn a_seek_tells_the_analysis_thread_where_the_session_starts() {
        let path =
            super::super::waveform_window::tests::write_test_wave(&vec![0; 1_440_000], 48_000, 1);
        let (mut source, ..) = open_audio_file(&path).expect("open the fixture");
        let mut session = centered_session_behind(20.0);
        let (events, event_rx) = unbounded();
        let (reports, report_rx) = unbounded();
        handle_session_seek(&mut session, 20.0, &mut source, &events, &reports)
            .expect("the seek applies");
        drop(source);
        std::fs::remove_file(path).expect("remove fixture");

        let Ok(SessionReport::Repositioned {
            generation,
            start_seconds,
        }) = report_rx.try_recv()
        else {
            panic!("no report of the new start");
        };
        assert_eq!(generation, session.gen);
        assert!((start_seconds - 5.36).abs() < 0.01, "{start_seconds}");
        assert_eq!(
            session.session_start_column,
            f64_to_u64_saturating(start_seconds * session.cols_per_second)
        );
        assert!((session.target_position_seconds - 20.0).abs() < f64::EPSILON);
        let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(reset)) = event_rx.try_recv() else {
            panic!("no reset");
        };
        assert!(reset.buffer_reset);
        assert_eq!(
            u64::from(reset.start_column_index),
            session.session_start_column
        );
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
            frequency_cap: false,
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
            frequency_cap: false,
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
            frequency_cap: false,
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
            frequency_cap: false,
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
            frequency_cap: false,
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
            frequency_cap: false,
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
        let reference_est =
            duration_requery_estimate_for_session(effective_frames, ESTIMATE_COLUMN_FRAMES);
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
            frequency_cap: false,
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
            session_drain_stft_rows(&mut session, &tx, &produced, true);
            assert_eq!(session.columns_produced, 3);
            assert!(session.stfts[0].has_row());
            while session.stfts[0].has_row() {
                session.target_position_seconds =
                    usize_to_f64_approx(usize::try_from(session.columns_produced).unwrap())
                        / session.cols_per_second;
                session_drain_stft_rows(&mut session, &tx, &produced, true);
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

        let generation = AtomicU64::new(session.gen);
        maybe_update_columns_estimate(
            &mut session,
            &source,
            &generation,
            &PublishedTrackLength::default(),
        );
        assert_eq!(
            session.total_columns_estimate, 1000,
            "should not grow below estimate"
        );

        session.columns_produced = 750;
        let generation = AtomicU64::new(session.gen);
        maybe_update_columns_estimate(
            &mut session,
            &source,
            &generation,
            &PublishedTrackLength::default(),
        );
        assert_eq!(
            session.total_columns_estimate, 1000,
            "75% threshold no longer triggers doubling"
        );

        session.columns_produced = 1000;
        let generation = AtomicU64::new(session.gen);
        maybe_update_columns_estimate(
            &mut session,
            &source,
            &generation,
            &PublishedTrackLength::default(),
        );
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
        let generation = AtomicU64::new(session.gen);
        maybe_update_columns_estimate(
            &mut session,
            &source,
            &generation,
            &PublishedTrackLength::default(),
        );
        assert_eq!(
            session.total_columns_estimate, 2000,
            "overshoot by 1 col must double"
        );

        // Cross the next estimate too — safety net keeps firing until
        // it catches up with the real length.
        session.columns_produced = 2500;
        let generation = AtomicU64::new(session.gen);
        maybe_update_columns_estimate(
            &mut session,
            &source,
            &generation,
            &PublishedTrackLength::default(),
        );
        assert_eq!(session.total_columns_estimate, 4000);
    }

    #[test]
    fn output_interval_is_continuous() {
        let i1 = column_hops(8192, 1.0, 48_000).output_interval;
        assert!((i1 - 1.0).abs() < 0.001);

        let i2 = column_hops(8192, 0.8, 48_000).output_interval;
        assert!((i2 - 1.25).abs() < 0.001);
        assert!(
            i2 > i1,
            "zoom=0.8 must produce a different interval than zoom=1.0"
        );

        let i3 = column_hops(8192, 0.5, 48_000).output_interval;
        assert!((i3 - 2.0).abs() < 0.001);

        let i4 = column_hops(1024, 0.7, 48_000).output_interval;
        assert!((i4 - 11.43).abs() < 0.1);
        // At 96 kHz the reference hop is twice as long, and so is the
        // interval.
        let i5 = column_hops(8192, 0.8, 96_000).output_interval;
        assert!((i5 - 2.5).abs() < 0.001);
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

    /// Write a 16-bit WAV of `channels` channels, each a square wave of a
    /// period of its own, and return its path.
    fn write_surround_fixture(seconds: u32, rate: u32, channels: u16) -> PathBuf {
        let samples: Vec<i16> = (0..seconds * rate)
            .flat_map(|frame| {
                (0..u32::from(channels)).map(move |channel| {
                    let period = 12 * (channel + 1);
                    if frame % period < period / 2 {
                        6_000
                    } else {
                        -6_000
                    }
                })
            })
            .collect();
        super::super::waveform_window::tests::write_test_wave(&samples, rate, channels)
    }

    /// A per-channel session of `path`'s six channels at the default FFT.
    fn surround_session_on(
        path: &Path,
        track_token: u64,
        generation: u64,
        display_mode: SpectrogramDisplayMode,
    ) -> SpectrogramWorkerCommand {
        let mut command = test_new_track(&path.to_string_lossy(), generation);
        if let SpectrogramWorkerCommand::NewTrack {
            track_token: token,
            channel_count,
            view_mode,
            display_mode: mode,
            ..
        } = &mut command
        {
            *token = track_token;
            *channel_count = 6;
            *view_mode = SpectrogramViewMode::PerChannel;
            *mode = display_mode;
        }
        command
    }

    /// What a chunk carries, to compare the chunks of two runs.
    fn chunk_content(chunk: &PrecomputedSpectrogramChunk) -> (u32, u16, u8, bool, bool, Vec<u8>) {
        (
            chunk.start_column_index,
            chunk.column_count,
            chunk.channel_count,
            chunk.buffer_reset,
            chunk.complete,
            chunk.columns_u8.clone(),
        )
    }

    /// Start a worker whose batches run on at most `max_groups` threads.
    fn spawn_worker_on_threads(
        max_groups: usize,
        generation: Arc<AtomicU64>,
    ) -> (
        Sender<SpectrogramWorkerCommand>,
        Receiver<AnalysisEvent>,
        std::thread::JoinHandle<()>,
    ) {
        let (commands, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let worker = std::thread::spawn(move || {
            stft_batch::with_thread_batcher(|batcher| {
                *batcher = stft_batch::ColumnBatcher::new(max_groups);
            });
            spectrogram_worker_loop(
                &command_rx,
                &events,
                &generation,
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
                &unbounded().0,
            );
        });
        (commands, event_rx, worker)
    }

    /// Every chunk of a centered session of `path` on at most `max_groups`
    /// threads, up to its end.
    fn surround_session_chunks(
        path: &Path,
        max_groups: usize,
    ) -> Vec<(u32, u16, u8, bool, bool, Vec<u8>)> {
        let (commands, events, worker) =
            spawn_worker_on_threads(max_groups, Arc::new(AtomicU64::new(1)));
        commands
            .send(surround_session_on(
                path,
                1,
                1,
                SpectrogramDisplayMode::Centered,
            ))
            .unwrap();
        let mut chunks = Vec::new();
        loop {
            let chunk = wait_for_chunk(&events, |_| true).expect("a chunk");
            chunks.push(chunk_content(&chunk));
            if chunk.complete {
                break;
            }
        }
        drop(commands);
        worker.join().expect("worker");
        chunks
    }

    #[test]
    fn a_surround_session_sends_the_same_chunks_on_any_number_of_threads() {
        let path = write_surround_fixture(4, 22_050, 6);
        let serial = surround_session_chunks(&path, 1);
        let parallel = surround_session_chunks(&path, 3);
        std::fs::remove_file(&path).unwrap();
        let columns: u32 = serial.iter().map(|chunk| u32::from(chunk.1)).sum();
        assert!(columns > 50, "{columns} columns");
        let bins = 2_048 / 2 + 1;
        let first = serial.iter().find(|chunk| chunk.1 > 0).expect("columns");
        assert_eq!(first.2, 6);
        assert_ne!(
            first.5[..bins],
            first.5[bins..2 * bins],
            "the channels differ"
        );
        // Each chunk starts where the one before it ended.
        let mut next = 0;
        for chunk in serial.iter().filter(|chunk| chunk.1 > 0) {
            assert_eq!(chunk.0, next);
            next += u32::from(chunk.1);
        }
        assert_eq!(serial.len(), parallel.len());
        for (index, (serial, parallel)) in serial.iter().zip(&parallel).enumerate() {
            assert!(
                serial == parallel,
                "chunk {index} differs: {:?} against {:?}",
                (serial.0, serial.1, serial.2, serial.3, serial.4),
                (parallel.0, parallel.1, parallel.2, parallel.3, parallel.4),
            );
        }
    }

    #[test]
    fn a_surround_session_ends_mid_decode_when_superseded() {
        let long = write_surround_fixture(60, 8_000, 6);
        let short = write_surround_fixture(3, 22_050, 6);
        let expected = surround_session_chunks(&short, 1);
        let generation = Arc::new(AtomicU64::new(1));
        let (commands, events, worker) = spawn_worker_on_threads(3, Arc::clone(&generation));
        // Rolling sessions decode at twice the playback rate after their
        // first 127 columns, 16 s here, so this one is still decoding when
        // the next starts.
        commands
            .send(surround_session_on(
                &long,
                1,
                1,
                SpectrogramDisplayMode::Rolling,
            ))
            .unwrap();
        wait_for_chunk(&events, |chunk| {
            chunk.track_token == 1 && chunk.column_count > 0
        })
        .expect("the first session's columns");
        generation.store(2, Ordering::Relaxed);
        commands
            .send(surround_session_on(
                &short,
                2,
                2,
                SpectrogramDisplayMode::Centered,
            ))
            .unwrap();
        let mut chunks = Vec::new();
        loop {
            let chunk = wait_for_chunk(&events, |_| true).expect("a chunk");
            if chunk.track_token == 1 {
                assert!(chunks.is_empty(), "the first session went on");
                assert!(!chunk.complete, "the first session decoded to its end");
                continue;
            }
            assert_eq!(chunk.generation, 2);
            chunks.push(chunk_content(&chunk));
            if chunk.complete {
                break;
            }
        }
        drop(commands);
        worker.join().expect("worker");
        std::fs::remove_file(&long).unwrap();
        std::fs::remove_file(&short).unwrap();
        // The helpers carry nothing from the session that ended.
        assert!(chunks == expected, "the second session's chunks differ");
    }

    /// Decode `path`'s six channels with batches on at most `max_groups`
    /// threads and a lookahead of 36 columns from the start, then move the
    /// playback position to where it parked, and return every chunk.
    fn chunks_parked_near_the_end(
        path: &Path,
        max_groups: usize,
    ) -> Vec<(u32, u16, u8, bool, bool, Vec<u8>)> {
        let path = path.to_path_buf();
        let (commands, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let worker = std::thread::spawn(move || {
            stft_batch::with_thread_batcher(|batcher| {
                *batcher = stft_batch::ColumnBatcher::new(max_groups);
            });
            let (mut source, rate, channels, _) = open_audio_file(&path).expect("fixture");
            assert_eq!(channels, 6);
            let mut session = make_test_session();
            session.display_mode = SpectrogramDisplayMode::Centered;
            session.view_mode = SpectrogramViewMode::PerChannel;
            session.channel_count = 6;
            assert_eq!(rate, 22_050);
            session.effective_rate = 22_050;
            session.cols_per_second = 22_050.0 / 1_024.0;
            session.target_position_seconds = 0.0;
            session.columns_produced = 0;
            session.chunk_start_index = 0;
            session.lookahead_columns = 36;
            session.stfts = (0..6).map(|_| StftComputer::new(2_048, 256)).collect();
            session.resamplers = (0..6).map(|_| PeakHoldResampler::new(4.0)).collect();
            session_decode_loop(
                &mut session,
                &mut source,
                &command_rx,
                &events,
                &AtomicU64::new(1),
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
            );
        });
        let mut chunks = Vec::new();
        let mut columns = 0;
        loop {
            let chunk = wait_for_chunk(&event_rx, |_| true).expect("a chunk");
            columns += u32::from(chunk.column_count);
            chunks.push(chunk_content(&chunk));
            if chunk.complete {
                break;
            }
            if columns == 36 {
                // Parked: move the lookahead past the end.
                commands
                    .send(SpectrogramWorkerCommand::PositionUpdate {
                        position_seconds: 1.5,
                    })
                    .unwrap();
            }
        }
        drop(commands);
        worker.join().expect("worker");
        chunks
    }

    #[test]
    fn a_batched_session_parked_in_its_last_rows_finishes_once_resumed() {
        // 165 rows a channel: batches of 64 rows, 32 columns in two, then
        // 37 rows at the end of the file, of which the lookahead takes 16
        // before the session parks.
        let path = write_surround_fixture(2, 22_050, 6);
        let serial = chunks_parked_near_the_end(&path, 1);
        let batched = chunks_parked_near_the_end(&path, 3);
        std::fs::remove_file(&path).unwrap();
        let columns: u32 = batched.iter().map(|chunk| u32::from(chunk.1)).sum();
        // 41 whole intervals of four rows, and the last row's flushed.
        assert_eq!(columns, 42);
        assert!(
            batched
                .iter()
                .any(|chunk| u32::from(chunk.1) + chunk.0 == 36 && !chunk.4),
            "it parked at its lookahead"
        );
        assert!(batched.last().is_some_and(|chunk| chunk.4));
        assert!(serial == batched, "the chunks differ");
    }

    fn session_on(path: &Path, track_token: u64, generation: u64) -> SpectrogramWorkerCommand {
        let mut command = test_new_track(&path.to_string_lossy(), generation);
        if let SpectrogramWorkerCommand::NewTrack {
            track_token: token,
            fft_size,
            display_mode,
            ..
        } = &mut command
        {
            *token = track_token;
            *fft_size = 512;
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
        // Symphonia cannot open the missing file, so the worker tries
        // GStreamer, whose first initialisation builds its registry.  With
        // a fresh cache, as the test script gives each run, that takes
        // seconds, and it is not what the waits below are for.
        #[cfg(feature = "gst")]
        gstreamer::init().expect("gstreamer initialization");
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
                    &PublishedTrackLength::default(),
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
                &PublishedTrackLength::default(),
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
                    &PublishedTrackLength::default(),
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
                &PublishedTrackLength::default(),
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
            fft_size: 512,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: true,
            widget_width: 0,
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
                    &PublishedTrackLength::default(),
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
            zoom_level,
            display_mode: mode,
            ..
        } = &mut command
        {
            // Above zoom 1 the columns are the STFT rows: 64 frames.
            #[allow(clippy::cast_precision_loss)]
            let zoom = super::super::column_hop::reference_hop(16_000) as f32 / 64.0;
            assert_eq!(column_hops(512, zoom, 16_000).stft_hop, 64);
            *zoom_level = zoom;
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
        // 40 s in, on the grid of 8 kHz columns.
        let expected = stft_seek_grid(40.0, 8_000, 1, column_hops(512, 1.0, 8_000).effective_hop).0;
        assert_eq!(
            u64::from(chunks.last().expect("chunks").start_column_index),
            expected
        );
        worker.finish();
        std::fs::remove_file(path).expect("remove fixture");
    }

    /// The chunks' columns follow each other without a gap or a reset; the
    /// column after the last.
    fn assert_contiguous(chunks: &[PrecomputedSpectrogramChunk], case: &str) -> u64 {
        let mut next: Option<u64> = None;
        for chunk in chunks {
            assert!(
                !chunk.buffer_reset || next.is_none(),
                "{case}: reset {chunk:?}"
            );
            let start = u64::from(chunk.start_column_index);
            if let Some(next) = next {
                assert_eq!(start, next, "{case}: {chunk:?}");
            }
            next = Some(start + u64::from(chunk.column_count));
        }
        next.unwrap_or(0)
    }

    #[test]
    fn positions_that_step_back_after_a_continuation_keep_it_decoding_its_file() {
        // Repeat-One, and another track. The worker reaches the end of the
        // outgoing track first and goes on with the next file; the playback
        // positions after the switch step back below where it went on.
        let first = write_tone_fixture("continued-outgoing", 2, 16_000);
        let other = write_tone_fixture("continued-other", 1, 16_000);
        for next in [first.clone(), other.clone()] {
            let case = next.file_name().unwrap().to_string_lossy().into_owned();
            let worker = QueuedWorker::start(vec![
                fast_session(&first, SpectrogramDisplayMode::Rolling),
                SpectrogramWorkerCommand::ContinueWithFile {
                    path: next.clone(),
                    track_token: 1,
                },
            ]);
            let start = loop {
                match worker.reports.recv_timeout(Duration::from_secs(10)) {
                    Ok(SessionReport::Opened {
                        continued_at: Some(start),
                        path,
                        ..
                    }) if path == next => break start,
                    Ok(_) => {}
                    Err(error) => panic!("{case}: no continuation: {error}"),
                }
            };
            // Where the outgoing file ends.
            assert!((start - 2.0).abs() < 1e-9, "{case}: {start}");
            // Both files decoded; the session parks at the end of the next.
            let mut completed = 0;
            let decoded = worker.chunks_until(|chunk| {
                completed += usize::from(chunk.complete);
                completed == 2
            });
            let end = assert_contiguous(&decoded, &case);
            let file_columns = if next == first { 500 } else { 250 };
            assert!(end.abs_diff(500 + file_columns) <= 4, "{case}: {end}");

            // The outgoing track plays on, then playback switches to the
            // next file, with positions as the analysis thread sent them
            // when it counted the outgoing track as ending at its last
            // position: they reach the start, then step back below it.
            let send = |command| worker.commands.send(command).unwrap();
            send(SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: 1.9,
            });
            send(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 });
            for position in [0.0, 0.0, 0.04, 0.23, 0.09, 0.19] {
                send(SpectrogramWorkerCommand::PositionUpdate {
                    position_seconds: start - 0.22 + position,
                });
            }
            for position in [0.3, 0.6, 0.95] {
                send(SpectrogramWorkerCommand::PositionUpdate {
                    position_seconds: start + position,
                });
            }
            // A token that shows the session handled them all.
            send(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 3 });
            let after = worker.chunks_until(|chunk| chunk.track_token == 3);
            assert!(
                after
                    .iter()
                    .all(|chunk| !chunk.buffer_reset && chunk.column_count == 0),
                "{case}: {after:?}"
            );
            let reports = worker.finish();
            assert!(
                reports
                    .iter()
                    .all(|report| !matches!(report, SessionReport::Failed { .. })),
                "{case}: {reports:?}"
            );
        }
        std::fs::remove_file(first).expect("remove fixture");
        std::fs::remove_file(other).expect("remove fixture");
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
    fn every_chunk_carries_the_width_its_columns_were_decoded_for() {
        // The UI sizes its ring for the width a chunk carries, so a column
        // decoded ahead for a wider view must come in a chunk that says so,
        // and a later session for a narrower view carries that view's
        // width. At zoom 64 a 1920-pixel centered view
        // parks 138 columns ahead, and a 3840-pixel one 198.
        let path = write_tone_fixture("chunk-width", 30, 8_000);
        let end = |chunk: &PrecomputedSpectrogramChunk| {
            chunk.start_column_index + u32::from(chunk.column_count)
        };
        let worker = QueuedWorker::start(vec![with_new_track(
            session_on(&path, 1, 1),
            |zoom, width, mode| {
                *zoom = 64.0;
                *width = 1_920;
                *mode = SpectrogramDisplayMode::Centered;
            },
        )]);
        let narrow = worker.chunks_until(|chunk| end(chunk) >= 130);
        assert!(narrow.iter().all(|chunk| chunk.widget_width == 1_920));

        worker
            .commands
            .send(SpectrogramWorkerCommand::UpdateWidgetWidth {
                widget_width: 3_840,
            })
            .unwrap();
        let wide = worker.chunks_until(|chunk| end(chunk) >= 190);
        assert!(
            wide.windows(2)
                .all(|pair| pair[0].widget_width <= pair[1].widget_width),
            "the width of a session only grows"
        );
        for chunk in &wide {
            assert!(
                matches!(chunk.widget_width, 1_920 | 3_840),
                "{}",
                chunk.widget_width
            );
            if end(chunk) > 138 {
                assert_eq!(
                    chunk.widget_width, 3_840,
                    "columns past the narrow lookahead"
                );
            }
        }

        // The next session is for a narrower view again. (The worker's
        // generation counter stays at 1 here, so the track token tells the
        // sessions apart.)
        worker
            .commands
            .send(with_new_track(
                session_on(&path, 2, 1),
                |zoom, width, mode| {
                    *zoom = 64.0;
                    *width = 1_920;
                    *mode = SpectrogramDisplayMode::Centered;
                },
            ))
            .unwrap();
        let next = worker.chunks_until(|chunk| chunk.track_token == 2 && end(chunk) >= 130);
        let next: Vec<_> = next.iter().filter(|chunk| chunk.track_token == 2).collect();
        assert!(!next.is_empty());
        assert!(next.iter().all(|chunk| chunk.widget_width == 1_920));
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

    /// A session of `path` with `fft_size` and `zoom_level`.
    fn timing_session(
        path: &Path,
        fft_size: usize,
        zoom_level: f32,
        display_mode: SpectrogramDisplayMode,
        start_seconds: f64,
    ) -> SpectrogramWorkerCommand {
        let mut command = session_on(path, 1, 1);
        if let SpectrogramWorkerCommand::NewTrack {
            fft_size: fft,
            zoom_level: zoom,
            display_mode: mode,
            start_seconds: start,
            target_position_seconds,
            ..
        } = &mut command
        {
            *fft = fft_size;
            *zoom = zoom_level;
            *mode = display_mode;
            *start = start_seconds;
            *target_position_seconds = start_seconds;
        }
        command
    }

    #[test]
    fn sessions_of_every_rate_of_a_family_make_columns_at_its_pace() {
        // Two seconds of each rate, at zoom 1 and zoomed in four times. The
        // windows are a number of samples, which is a shorter time at a
        // higher rate; a short window keeps the columns its end loses to
        // under one at every rate.
        for (rates, pace) in [
            (&[44_100, 88_200, 176_400][..], 44_100.0 / 1024.0),
            (&[48_000, 96_000, 192_000][..], 46.875),
        ] {
            for zoom in [1.0_f32, 4.0] {
                let mut counts = Vec::new();
                for &rate in rates {
                    let path = write_tone_fixture(&format!("pace-{rate}-{zoom}"), 2, rate);
                    let columns = columns_until_eof(vec![timing_session(
                        &path,
                        512,
                        zoom,
                        SpectrogramDisplayMode::Rolling,
                        0.0,
                    )]);
                    std::fs::remove_file(path).expect("remove fixture");
                    let count = columns.keys().next_back().map_or(0, |last| last + 1);
                    assert_eq!(usize::try_from(count).unwrap(), columns.len(), "{rate} Hz");
                    counts.push(count);
                }
                // A column for each hop of the two seconds, give or take
                // the window at the end of the file.
                let expected = 2.0 * pace * f64::from(zoom);
                for (&rate, &count) in rates.iter().zip(&counts) {
                    assert!(
                        (f64::from(count) - expected).abs() <= 2.0,
                        "{rate} Hz at zoom {zoom}: {count} columns, {expected:.1} expected"
                    );
                }
                assert!(
                    counts.iter().max().unwrap() - counts.iter().min().unwrap() <= 1,
                    "zoom {zoom}: {counts:?}"
                );
            }
        }
    }

    #[test]
    fn the_seek_grid_puts_a_time_in_one_column_at_every_rate_of_a_family() {
        for fft in [512, 8_192] {
            for zoom in [1.0_f32, 4.0, 0.25] {
                for seconds in [0.0, 1.0, 12.34, 300.0] {
                    let grid = |rate: u32| {
                        let hops = column_hops(fft, zoom, rate);
                        let (column, frame) =
                            stft_seek_grid(seconds, rate, hops.stft_hop, hops.effective_hop);
                        #[allow(clippy::cast_precision_loss)]
                        let (frame_seconds, stft_hop_seconds) = (
                            frame as f64 / f64::from(rate),
                            hops.stft_hop as f64 / f64::from(rate),
                        );
                        (column, frame_seconds, stft_hop_seconds)
                    };
                    let (column, frame_seconds, _) = grid(48_000);
                    for rate in [96_000, 192_000, 384_000] {
                        let (other, other_seconds, stft_hop_seconds) = grid(rate);
                        assert_eq!(other, column, "{rate} Hz, {seconds} s at zoom {zoom}");
                        // The first window of the column starts at the same
                        // time, to within an STFT hop of either rate.
                        assert!(
                            (other_seconds - frame_seconds).abs()
                                <= stft_hop_seconds.max(grid(48_000).2),
                            "{rate} Hz, {seconds} s at zoom {zoom}: {other_seconds} s"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn a_click_shows_in_the_column_of_the_time_it_plays() {
        // 22.05 kHz clicks, each in the middle of a 1024-frame column at
        // zoom 0.5 and on a 64-frame column at zoom 8, all after the half
        // second that seeks start at.
        let clicks = [12_800_u32, 20_992, 29_184];
        let path = write_click_track("timing", 22_050, 2, &clicks);
        // (FFT size, zoom, column hop)
        for (fft_size, zoom, column_hop) in [(512, 8.0, 64), (2_048, 0.5, 1_024)] {
            assert_eq!(
                column_hops(fft_size, zoom, 22_050).effective_hop,
                usize::try_from(column_hop).unwrap()
            );
            for mode in [
                SpectrogramDisplayMode::Rolling,
                SpectrogramDisplayMode::Centered,
            ] {
                // From the start, and from a seek at the start and in the
                // session, which start the grid half a second in.
                let start = |seconds| timing_session(&path, fft_size, zoom, mode, seconds);
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
        let path = write_click_track("staged", 22_050, 2, &clicks);
        let (tx, rx) = unbounded();
        centered_staging_decode(
            &path,
            512,
            false,
            8.0,
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

    /// The columns of `chunks`, by absolute column, after checking that
    /// each chunk with columns has `bins` bins of a `fft_size` spectrum for
    /// `channels` channels.
    fn columns_of_shape(
        chunks: &[PrecomputedSpectrogramChunk],
        bins: usize,
        fft_size: u32,
        channels: usize,
    ) -> std::collections::BTreeMap<u32, Vec<u8>> {
        let mut columns = std::collections::BTreeMap::new();
        for chunk in chunks.iter().filter(|chunk| chunk.bins_per_column > 0) {
            assert_eq!(usize::from(chunk.bins_per_column), bins);
            assert_eq!(chunk.fft_size, fft_size);
            assert_eq!(usize::from(chunk.channel_count), channels);
            let width = bins * channels;
            assert_eq!(
                chunk.columns_u8.len(),
                usize::from(chunk.column_count) * width
            );
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                columns.insert(
                    chunk.start_column_index + u32::try_from(offset).unwrap(),
                    column.to_vec(),
                );
            }
        }
        columns
    }

    #[test]
    fn a_capped_session_sends_the_bins_up_to_its_rate_familys_cap() {
        // (rate, bins of a 512-point spectrum with the cap): a bin is
        // rate / 512 Hz, so 96 kHz stops at bin 128 (24 kHz) and 192 kHz
        // at bin 64; 44.1 kHz is below its cap and keeps all 257.
        for (rate, capped_bins) in [(96_000, 129), (192_000, 65), (44_100, 257)] {
            let path = write_surround_fixture(1, rate, 6);
            for view_mode in [
                SpectrogramViewMode::PerChannel,
                SpectrogramViewMode::Downmix,
            ] {
                let channels = if view_mode == SpectrogramViewMode::PerChannel {
                    6
                } else {
                    1
                };
                let run = |frequency_cap: bool| {
                    let mut command = session_on(&path, 1, 1);
                    if let SpectrogramWorkerCommand::NewTrack {
                        frequency_cap: cap,
                        view_mode: mode,
                        channel_count,
                        ..
                    } = &mut command
                    {
                        *cap = frequency_cap;
                        *mode = view_mode;
                        *channel_count = channels;
                    }
                    let worker = QueuedWorker::start(vec![command]);
                    let chunks = worker.chunks_until(|chunk| chunk.complete);
                    worker.finish();
                    chunks
                };
                let full = columns_of_shape(&run(false), 257, 512, channels);
                let capped = columns_of_shape(&run(true), capped_bins, 512, channels);
                assert!(!full.is_empty());
                assert_eq!(
                    full.keys().collect::<Vec<_>>(),
                    capped.keys().collect::<Vec<_>>(),
                    "{rate} Hz {view_mode:?}"
                );
                // The same spectra, each channel's cut at the cap.
                for (column, bytes) in &full {
                    let expected: Vec<u8> = bytes
                        .chunks_exact(257)
                        .flat_map(|channel| channel[..capped_bins].iter().copied())
                        .collect();
                    assert_eq!(
                        capped[column], expected,
                        "{rate} Hz {view_mode:?} column {column}"
                    );
                }
            }

            // A centered gapless successor's staged columns are cut too.
            let (tx, rx) = unbounded();
            centered_staging_decode(
                &path,
                512,
                true,
                1.0,
                SpectrogramViewMode::PerChannel,
                &AtomicBool::new(false),
                &tx,
            );
            drop(tx);
            let staged: Vec<_> = rx.try_iter().collect();
            assert!(!columns_of_shape(&staged, capped_bins, 512, 6).is_empty());
            std::fs::remove_file(path).expect("remove fixture");
        }
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
            let columns = columns_until_eof(vec![timing_session(&path, 512, 16.0, mode, 0.0)]);
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
            &PublishedTrackLength::default(),
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
            .map(|index| write_tone_fixture(&format!("estimate-{index}"), 1, 16_000))
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

        // Each file is 250 columns of 16 kHz audio at a 64-frame hop, and is
        // estimated from its header as 1,264, with the usual allowance.
        let mut track_start = 0;
        for token in 1..=4 {
            let estimates: std::collections::BTreeSet<u32> = chunks
                .iter()
                .filter(|chunk| chunk.track_token == token && !chunk.complete)
                .map(|chunk| chunk.total_columns_estimate)
                .collect();
            assert_eq!(
                estimates.into_iter().collect::<Vec<_>>(),
                [track_start + 1_264],
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
                let ColumnHops {
                    stft_hop: hop,
                    output_interval: interval,
                    ..
                } = column_hops(fft_size, zoom, 48_000);
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
            let interval = column_hops(fft_size, zoom, 48_000).output_interval;
            assert_eq!(column_hops(fft_size, zoom, 48_000).stft_hop, hop);
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

    // -----------------------------------------------------------------------
    // Playhead first: the left of a centered view after its right
    // -----------------------------------------------------------------------

    /// Seconds of the backfill fixtures, at [`BACKFILL_RATE`].
    const BACKFILL_SECONDS: u32 = 120;
    /// A low rate keeps the fixtures small.
    const BACKFILL_RATE: u32 = 8_000;
    /// The zoom of the backfill sessions: a sixth, at which their columns
    /// are 1024 frames apart (see `backfill_sessions_have_long_columns`),
    /// 7.8 a second, so a view of [`BACKFILL_WIDTH`] columns shows 41 s.
    const BACKFILL_ZOOM: f32 = 171.0 / 1024.0;
    const BACKFILL_WIDTH: u32 = 320;
    /// Where the playhead is.
    const BACKFILL_PLAYHEAD: f64 = 60.0;

    /// A 16-bit WAV of `channels` channels of noise, every channel its
    /// own, named for the test.
    fn write_noise_fixture(name: &str, channels: u16) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "ferrous-backfill-{name}-{channels}-{}.wav",
            std::process::id()
        ));
        let data_bytes = BACKFILL_SECONDS * BACKFILL_RATE * u32::from(channels) * 2;
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&(36 + data_bytes).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0");
        bytes.extend_from_slice(&channels.to_le_bytes());
        bytes.extend_from_slice(&BACKFILL_RATE.to_le_bytes());
        bytes.extend_from_slice(&(BACKFILL_RATE * u32::from(channels) * 2).to_le_bytes());
        bytes.extend_from_slice(&(channels * 2).to_le_bytes());
        bytes.extend_from_slice(b"\x10\0data");
        bytes.extend_from_slice(&data_bytes.to_le_bytes());
        let mut random = 0x9e37_79b9_u32 ^ u32::from(channels);
        for _ in 0..data_bytes / 2 {
            random = random.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let sample = i16::try_from(random >> 18).unwrap() - 8_192;
            bytes.extend_from_slice(&sample.to_le_bytes());
        }
        std::fs::write(&path, bytes).unwrap();
        path
    }

    /// A per-channel centered session of `path` with its playhead at
    /// `target`, starting where a restart for it does.
    fn backfill_session_on(path: &Path, channels: usize, target: f64) -> SpectrogramWorkerCommand {
        let mut command = test_new_track(&path.to_string_lossy(), 1);
        if let SpectrogramWorkerCommand::NewTrack {
            channel_count,
            view_mode,
            display_mode,
            widget_width,
            zoom_level,
            start_seconds,
            target_position_seconds,
            ..
        } = &mut command
        {
            *channel_count = channels;
            *zoom_level = BACKFILL_ZOOM;
            *view_mode = SpectrogramViewMode::PerChannel;
            *display_mode = SpectrogramDisplayMode::Centered;
            *widget_width = BACKFILL_WIDTH;
            *start_seconds = backfill_start_seconds(target);
            *target_position_seconds = target;
        }
        command
    }

    /// Where a centered session for a playhead at `target` starts: its
    /// view's left edge, less the warm-up.
    fn backfill_start_seconds(target: f64) -> f64 {
        CenteredView::new(BACKFILL_WIDTH, BACKFILL_ZOOM, BACKFILL_RATE, None)
            .decode_start_seconds(target)
    }

    fn backfill_column(seconds: f64) -> u64 {
        stft_seek_grid(
            seconds,
            BACKFILL_RATE,
            256,
            column_hops(2048, BACKFILL_ZOOM, BACKFILL_RATE).effective_hop,
        )
        .0
    }

    #[test]
    fn backfill_sessions_have_long_columns() {
        assert_eq!(
            column_hops(2048, BACKFILL_ZOOM, BACKFILL_RATE).effective_hop,
            1024
        );
        let window = centered_view::window_seconds(BACKFILL_WIDTH, BACKFILL_ZOOM, BACKFILL_RATE);
        assert!((window - 40.96).abs() < 0.01, "{window}");
    }

    /// Sends what a session sends, and does `on_chunk` with each chunk as
    /// the worker sends it, so that a test acts at a point of the session.
    struct WatchedOutput<F: Fn(&PrecomputedSpectrogramChunk) + Send + Sync> {
        events: Sender<AnalysisEvent>,
        on_chunk: F,
    }

    impl<F: Fn(&PrecomputedSpectrogramChunk) + Send + Sync> AnalysisEventOutput for WatchedOutput<F> {
        fn send(&self, event: AnalysisEvent) -> Result<(), ()> {
            if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = &event {
                (self.on_chunk)(chunk);
            }
            self.events.send(event).map_err(|_| ())
        }
    }

    /// Run `command` as a session on at most `max_groups` threads, its
    /// output watched by `on_chunk`, until the session ends; then return
    /// every chunk it sent.
    fn run_watched_session(
        command: SpectrogramWorkerCommand,
        max_groups: usize,
        commands: (
            Sender<SpectrogramWorkerCommand>,
            Receiver<SpectrogramWorkerCommand>,
        ),
        generation: &Arc<AtomicU64>,
        on_chunk: impl Fn(&PrecomputedSpectrogramChunk) + Send + Sync + 'static,
        until: impl Fn(&[PrecomputedSpectrogramChunk]) -> bool,
    ) -> Vec<PrecomputedSpectrogramChunk> {
        let (events, event_rx) = unbounded();
        let (command_tx, command_rx) = commands;
        let generation_for_worker = Arc::clone(generation);
        let worker = std::thread::spawn(move || {
            stft_batch::with_thread_batcher(|batcher| {
                *batcher = stft_batch::ColumnBatcher::new(max_groups);
            });
            run_spectrogram_session(
                QueuedCommand::alone(command),
                &command_rx,
                &WatchedOutput { events, on_chunk },
                &generation_for_worker,
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
                &unbounded().0,
            )
        });
        let mut chunks = Vec::new();
        while !until(&chunks) {
            match event_rx.recv_timeout(OUTPUT_WAIT) {
                Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) => chunks.push(chunk),
                Ok(_) => {}
                Err(_) => break,
            }
        }
        let _ = command_tx.send(SpectrogramWorkerCommand::StopSession);
        worker.join().expect("worker");
        chunks.extend(event_rx.try_iter().filter_map(|event| match event {
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => Some(chunk),
            _ => None,
        }));
        chunks
    }

    fn ends_complete(chunks: &[PrecomputedSpectrogramChunk]) -> bool {
        chunks.last().is_some_and(|chunk| chunk.complete)
    }

    /// The columns of `chunks` by their absolute index. Fails for a column
    /// sent twice.
    fn columns_of(
        chunks: &[PrecomputedSpectrogramChunk],
    ) -> std::collections::BTreeMap<u64, Vec<u8>> {
        let mut columns = std::collections::BTreeMap::new();
        for chunk in chunks.iter().filter(|chunk| chunk.column_count > 0) {
            let width = usize::from(chunk.bins_per_column) * usize::from(chunk.channel_count);
            for (offset, column) in chunk.columns_u8.chunks_exact(width).enumerate() {
                let index = u64::from(chunk.start_column_index) + usize_to_u64(offset);
                assert!(
                    columns.insert(index, column.to_vec()).is_none(),
                    "column {index} sent twice"
                );
            }
        }
        columns
    }

    /// The column spans of `chunks` with data, merged where they follow
    /// each other, in the order sent.
    fn spans_of(chunks: &[PrecomputedSpectrogramChunk]) -> Vec<(u64, u64)> {
        let mut spans: Vec<(u64, u64)> = Vec::new();
        for chunk in chunks.iter().filter(|chunk| chunk.column_count > 0) {
            let start = u64::from(chunk.start_column_index);
            let end = start + u64::from(chunk.column_count);
            match spans.last_mut() {
                Some(last) if last.1 == start => last.1 = end,
                _ => spans.push((start, end)),
            }
        }
        spans
    }

    /// The columns of a session that decodes from `start` on, in order.
    fn continuous_reference(
        path: &Path,
        channels: usize,
        max_groups: usize,
    ) -> std::collections::BTreeMap<u64, Vec<u8>> {
        let mut command = backfill_session_on(path, channels, BACKFILL_PLAYHEAD);
        if let SpectrogramWorkerCommand::NewTrack {
            target_position_seconds,
            start_seconds,
            ..
        } = &mut command
        {
            // A session at the start of the file, which has nothing left of it.
            *start_seconds = 0.0;
            *target_position_seconds = 0.0;
        }
        let chunks = run_watched_session(
            command,
            max_groups,
            unbounded(),
            &Arc::new(AtomicU64::new(1)),
            |_| {},
            ends_complete,
        );
        assert_eq!(
            spans_of(&chunks).len(),
            1,
            "a session from the start decodes in order"
        );
        columns_of(&chunks)
    }

    #[test]
    fn a_centered_session_decodes_the_playhead_first_then_the_left_on_the_grid() {
        // Stereo on one thread, and six channels whose batches the worker
        // shares with helper threads.
        for (channels, max_groups) in [(2_u16, 1), (6, 3)] {
            let path = write_noise_fixture("order", channels);
            let channel_count = usize::from(channels);
            let reference = continuous_reference(&path, channel_count, max_groups);
            let start = backfill_column(backfill_start_seconds(BACKFILL_PLAYHEAD));
            let playhead = backfill_column(BACKFILL_PLAYHEAD);
            assert!(
                playhead >= start + MIN_BACKFILL_COLUMNS,
                "{start} {playhead}"
            );

            let chunks = run_watched_session(
                backfill_session_on(&path, channel_count, BACKFILL_PLAYHEAD),
                max_groups,
                unbounded(),
                &Arc::new(AtomicU64::new(1)),
                |_| {},
                ends_complete,
            );
            // The reset starts the session's columns at its first, left of
            // the playhead, for the UI's ring to count from.
            let reset = &chunks[0];
            assert!(reset.buffer_reset && reset.column_count == 0);
            assert_eq!(u64::from(reset.start_column_index), start);
            // The playhead's column comes first, alone.
            let first = chunks.iter().find(|chunk| chunk.column_count > 0).unwrap();
            assert_eq!(u64::from(first.start_column_index), playhead);
            assert_eq!(first.column_count, 1);
            // Then the right of the view, the left up to the playhead, and
            // the right on from where it was left.
            let spans = spans_of(&chunks);
            assert_eq!(spans.len(), 3, "{channels} channels: {spans:?}");
            let (right, left, rest) = (spans[0], spans[1], spans[2]);
            assert_eq!(right.0, playhead);
            let view_right = backfill_column(BACKFILL_PLAYHEAD) + u64::from(BACKFILL_WIDTH) / 2;
            assert!(right.1 >= view_right, "{spans:?}");
            assert_eq!(left, (start, playhead));
            assert_eq!(rest.0, right.1);
            // Every column is the one a session decoding in order makes.
            let columns = columns_of(&chunks);
            assert_eq!(columns.len(), usize::try_from(rest.1 - start).unwrap());
            for (index, bytes) in &columns {
                assert_eq!(
                    Some(bytes),
                    reference.get(index),
                    "{channels} channels: column {index}"
                );
            }
            // The end is published once, after the last column.
            assert_eq!(chunks.iter().filter(|chunk| chunk.complete).count(), 1);
            assert_eq!(
                u64::from(chunks.last().unwrap().total_columns_estimate),
                rest.1
            );
            std::fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn a_seek_in_a_centered_session_decodes_the_playhead_first_and_during_the_left_starts_over() {
        let path = write_noise_fixture("seek", 2);
        let reference = continuous_reference(&path, 2, 1);
        let first_target = BACKFILL_PLAYHEAD;
        let second_target = 90.0;
        let first_playhead = backfill_column(first_target);
        // A seek arrives while the session decodes the left of its view.
        let (commands, command_rx) = unbounded();
        let seek = commands.clone();
        let sent = AtomicBool::new(false);
        let chunks = run_watched_session(
            backfill_session_on(&path, 2, first_target),
            1,
            (commands, command_rx),
            &Arc::new(AtomicU64::new(1)),
            move |chunk| {
                if chunk.column_count > 0
                    && u64::from(chunk.start_column_index) < first_playhead
                    && !sent.swap(true, Ordering::Relaxed)
                {
                    seek.send(SpectrogramWorkerCommand::Seek {
                        position_seconds: second_target,
                    })
                    .unwrap();
                }
            },
            ends_complete,
        );
        let reset = chunks
            .iter()
            .rposition(|chunk| chunk.buffer_reset)
            .expect("the seek's reset");
        assert!(reset > 0, "the seek follows the session's own reset");
        let after = &chunks[reset..];
        let start = backfill_column(backfill_start_seconds(second_target));
        let playhead = backfill_column(second_target);
        assert_eq!(u64::from(after[0].start_column_index), start);
        // Nothing of the left before the seek follows its reset.
        let spans = spans_of(after);
        assert_eq!(spans.len(), 3, "{spans:?}");
        assert_eq!(spans[0].0, playhead);
        assert_eq!(spans[1], (start, playhead));
        assert_eq!(spans[2].0, spans[0].1);
        for (index, bytes) in columns_of(after) {
            assert_eq!(Some(&bytes), reference.get(&index), "column {index}");
        }
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn a_superseded_centered_session_does_not_decode_the_left_of_its_view() {
        let path = write_noise_fixture("superseded", 2);
        let playhead = backfill_column(BACKFILL_PLAYHEAD);
        let generation = Arc::new(AtomicU64::new(1));
        let superseding = Arc::clone(&generation);
        // Another session takes over once the playhead's column is out.
        let chunks = run_watched_session(
            backfill_session_on(&path, 2, BACKFILL_PLAYHEAD),
            1,
            unbounded(),
            &generation,
            move |chunk| {
                if chunk.column_count > 0 {
                    superseding.store(2, Ordering::Relaxed);
                }
            },
            |_| false,
        );
        assert!(chunks.iter().any(|chunk| chunk.column_count > 0));
        for chunk in chunks.iter().filter(|chunk| chunk.column_count > 0) {
            assert!(
                u64::from(chunk.start_column_index) >= playhead,
                "a column of the left at {}",
                chunk.start_column_index
            );
        }
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn a_view_that_stops_being_centered_gets_the_right_of_its_view_only() {
        let path = write_noise_fixture("rolling", 2);
        let reference = continuous_reference(&path, 2, 1);
        let playhead = backfill_column(BACKFILL_PLAYHEAD);
        let (commands, command_rx) = unbounded();
        let switch = commands.clone();
        let sent = AtomicBool::new(false);
        let chunks = run_watched_session(
            backfill_session_on(&path, 2, BACKFILL_PLAYHEAD),
            1,
            (commands, command_rx),
            &Arc::new(AtomicU64::new(1)),
            move |chunk| {
                if chunk.column_count > 0
                    && u64::from(chunk.start_column_index) < playhead
                    && !sent.swap(true, Ordering::Relaxed)
                {
                    switch
                        .send(SpectrogramWorkerCommand::SetDisplayMode(
                            SpectrogramDisplayMode::Rolling,
                        ))
                        .unwrap();
                    // Playback goes on, so that the rolling session's
                    // lookahead, smaller than the centered one's, holds
                    // more than a chunk past what it decoded.
                    switch
                        .send(SpectrogramWorkerCommand::PositionUpdate {
                            position_seconds: BACKFILL_PLAYHEAD + 20.0,
                        })
                        .unwrap();
                }
            },
            |chunks| spans_of(chunks).len() >= 3,
        );
        let spans = spans_of(&chunks);
        assert_eq!(spans.len(), 3, "{spans:?}");
        // The left stopped where the switch found it, and the right went on
        // from where it was.
        assert_eq!(
            spans[1].0,
            backfill_column(backfill_start_seconds(BACKFILL_PLAYHEAD))
        );
        assert!(spans[1].1 < playhead, "{spans:?}");
        assert_eq!(spans[2].0, spans[0].1);
        for (index, bytes) in columns_of(&chunks) {
            assert_eq!(Some(&bytes), reference.get(&index), "column {index}");
        }
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn a_session_whose_right_reaches_the_end_publishes_it_after_its_left() {
        // Near the end of the file the right of the view reaches it before
        // the left is decoded. The left's chunks carry the file's extent,
        // not the padded estimate, and the end is published once they are
        // sent: a zoom waiting for the whole view waits for it.
        let path = write_noise_fixture("end", 2);
        let target = 110.0;
        let start = backfill_column(backfill_start_seconds(target));
        let playhead = backfill_column(target);
        let chunks = run_watched_session(
            backfill_session_on(&path, 2, target),
            1,
            unbounded(),
            &Arc::new(AtomicU64::new(1)),
            |_| {},
            ends_complete,
        );
        let spans = spans_of(&chunks);
        assert_eq!(spans.len(), 2, "{spans:?}");
        assert_eq!(spans[0].0, playhead);
        assert_eq!(spans[1], (start, playhead));
        let end = spans[0].1;
        let first_left = chunks
            .iter()
            .position(|chunk| {
                chunk.column_count > 0 && u64::from(chunk.start_column_index) < playhead
            })
            .unwrap();
        // Before the end the estimate is the file's, with its padding.
        assert!(
            u64::from(chunks[1].total_columns_estimate) > end,
            "{} {end}",
            chunks[1].total_columns_estimate
        );
        for chunk in &chunks[first_left..] {
            assert_eq!(u64::from(chunk.total_columns_estimate), end);
        }
        let finals: Vec<_> = chunks
            .iter()
            .enumerate()
            .filter(|(_, chunk)| chunk.complete)
            .collect();
        assert_eq!(finals.len(), 1);
        assert_eq!(
            finals[0].0,
            chunks.len() - 1,
            "the end comes after every column"
        );
        assert_eq!(u64::from(finals[0].1.total_columns_estimate), end);
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn a_gapless_switch_during_the_left_after_the_end_publishes_the_end() {
        let path = write_noise_fixture("switch", 2);
        let target = 110.0;
        let playhead = backfill_column(target);
        let command = backfill_session_on(&path, 2, target);
        let mut successor = command.clone();
        if let SpectrogramWorkerCommand::NewTrack {
            track_token,
            handoff,
            ..
        } = &mut successor
        {
            *track_token += 1;
            handoff.finalize_previous = true;
        }
        let (commands, command_rx) = unbounded();
        let switch = commands.clone();
        let sent = AtomicBool::new(false);
        let chunks = run_watched_session(
            command,
            1,
            (commands, command_rx),
            &Arc::new(AtomicU64::new(1)),
            move |chunk| {
                if chunk.column_count > 0
                    && u64::from(chunk.start_column_index) < playhead
                    && !sent.swap(true, Ordering::Relaxed)
                {
                    switch.send(successor.clone()).unwrap();
                }
            },
            |_| false,
        );
        let end = spans_of(&chunks)[0].1;
        let finals: Vec<_> = chunks.iter().filter(|chunk| chunk.complete).collect();
        assert_eq!(finals.len(), 1, "the end is published once");
        assert_eq!(u64::from(finals[0].total_columns_estimate), end);
        assert!(chunks.last().unwrap().complete);
        std::fs::remove_file(path).unwrap();
    }

    /// The rate of the raw AC-3 tone, whose columns at the reference hop
    /// come 46.875 a second.
    #[cfg(feature = "gst")]
    const PADDED_RATE: u32 = 48_000;

    /// A raw AC-3 file: 20.48 s of a tone at 32 kbit/s, then 10 s worth of
    /// bytes that hold no frame. Its duration, estimated from the bitrate of
    /// its first frame and the length of its frames, takes the padding in,
    /// so a playhead can land past the end of its audio.
    #[cfg(feature = "gst")]
    fn write_padded_ac3_fixture(name: &str) -> PathBuf {
        use crate::raw_audio::TEST_AC3;

        let path =
            std::env::temp_dir().join(format!("ferrous-padded-{name}-{}.ac3", std::process::id()));
        let mut bytes = TEST_AC3.repeat(20);
        bytes.resize(bytes.len() + 10 * 32_000 / 8, 0);
        std::fs::write(&path, bytes).unwrap();
        path
    }

    #[cfg(feature = "gst")]
    fn padded_column(seconds: f64) -> u64 {
        stft_seek_grid(
            seconds,
            PADDED_RATE,
            256,
            column_hops(2048, 1.0, PADDED_RATE).effective_hop,
        )
        .0
    }

    /// Where a centered session of the padded file for a playhead at
    /// `target` starts, in a track whose columns end at `end`, if known.
    #[cfg(feature = "gst")]
    fn padded_start_seconds(target: f64, end: Option<u64>) -> f64 {
        CenteredView::new(BACKFILL_WIDTH, 1.0, PADDED_RATE, end).decode_start_seconds(target)
    }

    /// A centered session of `path` as `track_token`, with its playhead at
    /// `target`, starting where a restart for it does.
    #[cfg(feature = "gst")]
    fn padded_session_on(path: &Path, track_token: u64, target: f64) -> SpectrogramWorkerCommand {
        let mut command = test_new_track(&path.to_string_lossy(), 1);
        if let SpectrogramWorkerCommand::NewTrack {
            track_token: token,
            display_mode,
            widget_width,
            start_seconds,
            target_position_seconds,
            ..
        } = &mut command
        {
            *token = track_token;
            *display_mode = SpectrogramDisplayMode::Centered;
            *widget_width = BACKFILL_WIDTH;
            *start_seconds = padded_start_seconds(target, None);
            *target_position_seconds = target;
        }
        command
    }

    /// The chunks a worker sends up to the first that satisfies `last`,
    /// that one included.
    #[cfg(feature = "gst")]
    fn chunks_through(
        events: &Receiver<AnalysisEvent>,
        last: impl Fn(&PrecomputedSpectrogramChunk) -> bool,
    ) -> Vec<PrecomputedSpectrogramChunk> {
        let mut chunks = Vec::new();
        while !chunks.last().is_some_and(&last) {
            chunks.push(wait_for_chunk(events, |_| true).expect("a chunk"));
        }
        chunks
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_centered_playhead_past_the_audio_of_a_padded_estimate_ends_where_the_audio_does() {
        // A seek into the padding puts the playhead past the end of the
        // audio: the right of the view finds none, and the left reaches the
        // end of the file. The session ends there, as a session reading the
        // file from its start does, not at the playhead: the UI would keep
        // the columns between them pending, and show the track that much
        // longer than its audio.
        let path = write_padded_ac3_fixture("end");
        let reference = run_watched_session(
            padded_session_on(&path, 30, 0.0),
            1,
            unbounded(),
            &Arc::new(AtomicU64::new(1)),
            |_| {},
            ends_complete,
        );
        let end = u64::from(reference.last().unwrap().total_columns_estimate);
        assert_eq!(spans_of(&reference), vec![(0, end)]);

        let target = 22.0;
        let start = padded_column(padded_start_seconds(target, None));
        let playhead = padded_column(target);
        assert!(
            start < end && end + MIN_BACKFILL_COLUMNS < playhead,
            "{start} {end} {playhead}"
        );

        let generation = Arc::new(AtomicU64::new(1));
        let (commands, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let worker_generation = Arc::clone(&generation);
        let worker = std::thread::spawn(move || {
            spectrogram_worker_loop(
                &command_rx,
                &events,
                &worker_generation,
                &AtomicU64::new(0),
                &PublishedTrackLength::default(),
                &unbounded().0,
            );
        });
        commands.send(padded_session_on(&path, 30, target)).unwrap();
        let chunks = chunks_through(&event_rx, |chunk| chunk.complete);
        // The session starts with the estimate, padding and all.
        assert!(chunks[0].buffer_reset);
        assert!(
            u64::from(chunks[0].total_columns_estimate) > playhead,
            "{}",
            chunks[0].total_columns_estimate
        );
        // The left of the view, up to the end of the audio, the last,
        // partial column included, is all there is.
        assert_eq!(spans_of(&chunks), vec![(start, end)]);
        let finalize = chunks.last().unwrap();
        assert_eq!(finalize.column_count, 0);
        assert_eq!(u64::from(finalize.total_columns_estimate), end);
        assert_eq!(u64::from(finalize.start_column_index), end);

        // Once the UI has the end, its view of a playhead near it ends there,
        // a view's width after its left edge, which the session did not
        // decode. A seek keeps the session's columns only if they cover that
        // view, and otherwise starts before it: whether its playhead is in
        // the audio the session decoded, further into the padding, or left
        // of what it decoded. The padded duration would put the view's left
        // edge in the columns decoded, and leave the UI's view without the
        // columns left of them.
        let view_left = end - u64::from(BACKFILL_WIDTH);
        assert!(view_left < start, "{view_left} {start}");
        for (session, seek) in [20.1, 25.0, 18.0].into_iter().enumerate() {
            if session > 0 {
                commands.send(padded_session_on(&path, 30, target)).unwrap();
                let chunks = chunks_through(&event_rx, |chunk| chunk.complete);
                assert_eq!(spans_of(&chunks), vec![(start, end)], "{seek}");
            }
            commands
                .send(SpectrogramWorkerCommand::Seek {
                    position_seconds: seek,
                })
                .unwrap();
            let chunks = chunks_through(&event_rx, |chunk| chunk.complete);
            assert!(chunks[0].buffer_reset, "{seek}");
            let seek_start = padded_column(padded_start_seconds(seek, Some(end)));
            assert_eq!(
                u64::from(chunks[0].start_column_index),
                seek_start,
                "{seek}"
            );
            assert!(seek_start <= view_left, "{seek}: {seek_start}");
            // The playhead's columns first, if it is in the audio, then the
            // left of the view, up to the same end.
            let seek_playhead = padded_column(seek);
            let spans = if seek_playhead < end {
                vec![(seek_playhead, end), (seek_start, seek_playhead)]
            } else {
                vec![(seek_start, end)]
            };
            assert_eq!(spans_of(&chunks), spans, "{seek}");
            assert_eq!(chunks.iter().filter(|chunk| chunk.complete).count(), 1);
            assert_eq!(
                u64::from(chunks.last().unwrap().total_columns_estimate),
                end,
                "{seek}"
            );
        }

        // A gapless switch after the end publishes no other end for it.
        let mut successor = padded_session_on(&path, 31, 0.0);
        if let SpectrogramWorkerCommand::NewTrack { handoff, .. } = &mut successor {
            handoff.finalize_previous = true;
        }
        commands.send(successor).unwrap();
        let chunks = chunks_through(&event_rx, |chunk| chunk.track_token == 31);
        assert!(
            chunks.iter().all(|chunk| chunk.track_token == 31),
            "{chunks:?}"
        );
        drop(commands);
        worker.join().expect("worker");
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn only_a_centered_session_left_of_its_playhead_backfills() {
        let centered = SpectrogramDisplayMode::Centered;
        assert_eq!(
            plan_backfill(centered, 100, 100 + MIN_BACKFILL_COLUMNS),
            Some(Backfill {
                next: 100,
                end: 100 + MIN_BACKFILL_COLUMNS,
                resume: None,
                right_ended: false,
            })
        );
        // Rolling views show history in the order it arrives.
        assert_eq!(
            plan_backfill(SpectrogramDisplayMode::Rolling, 0, 5_000),
            None
        );
        // A track's start, or a few columns, cost less than the seeks.
        assert_eq!(plan_backfill(centered, 0, 0), None);
        assert_eq!(
            plan_backfill(centered, 100, 100 + MIN_BACKFILL_COLUMNS - 1),
            None
        );
    }

    #[test]
    fn a_session_filling_its_left_reports_the_right_it_decoded() {
        // What the UI's ring ends at, which the analysis thread judges
        // seeks by, and a finalize for a gapless switch publishes.
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.columns_produced = 300;
        session.backfill = Some(Backfill {
            next: 250,
            end: 400,
            resume: Some(900),
            right_ended: false,
        });
        assert_eq!(decoded_frontier(&session), 900);
        let (events, event_rx) = unbounded();
        session_emit_finalize_chunk(&session, &events);
        let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event_rx.try_recv().unwrap() else {
            panic!("a chunk");
        };
        assert!(chunk.complete);
        assert_eq!(chunk.total_columns_estimate, 900);
        // A position the right covers needs no seek, though the session
        // reads further left.
        session.target_position_seconds = 15.0;
        session.session_start_column = 250;
        session.lookahead_columns = 512;
        assert!(apply_position(&mut session, 15.0).is_none());
        // Its partial interval at the end of the left is not a column.
        let produced = AtomicU64::new(0);
        session.resamplers = vec![PeakHoldResampler::new(1.0)];
        session_flush_chunk(&mut session, &events, &produced);
        assert_eq!(session.columns_produced, 300);
        assert_eq!(produced.load(Ordering::Relaxed), 900);
    }

    // ---------------------------------------------------------------
    // Explicit seeks in a centered session
    // ---------------------------------------------------------------

    /// A centered session for a 1185-pixel view, 25.28 s wide, that has
    /// decoded from `start` up to `frontier`, with its playhead at
    /// `position_seconds`.
    fn centered_session_decoded(
        start: u64,
        frontier: u64,
        position_seconds: f64,
    ) -> SpectrogramSessionState {
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Centered;
        session.decode_rate_limit = f64::INFINITY;
        session.widget_width = 1185;
        session.session_start_column = start;
        session.columns_produced = frontier;
        session.chunk_start_index = frontier;
        session.target_position_seconds = position_seconds;
        refresh_lookahead(&mut session);
        session
    }

    fn column_at(seconds: f64) -> u64 {
        stft_seek_grid(seconds, 48_000, 256, 1_024).0
    }

    fn seeks(action: &SessionAction, position: f64) -> bool {
        matches!(action, SessionAction::SeekRequired { position_seconds } if *position_seconds == position)
    }

    #[test]
    fn a_centered_seek_past_the_decoded_columns_seeks_to_its_view() {
        // A session restarted for 522 s decodes the playhead first: 8 s
        // of it so far, and the left of its view not yet.
        let start = column_at(507.36);
        let playhead = column_at(522.0);
        let restarted = || {
            let mut session = centered_session_decoded(start, column_at(530.0), 522.0);
            session.backfill = plan_backfill(session.display_mode, start, playhead);
            assert!(session.backfill.is_some());
            session
        };
        // 610 s is within the session's lookahead: a position there does
        // not seek, and the session would decode the 65 s up to the view
        // first.
        let mut session = restarted();
        let gap = column_at(595.36) - decoded_frontier(&session);
        assert!(gap <= session.lookahead_columns, "{gap}");
        assert!(apply_position(&mut session, 610.0).is_none());
        // A seek there goes to the view.
        let mut session = restarted();
        let action = apply_seek(&mut session, 610.0);
        assert!(seeks(&action, 610.0));
        assert!((session.target_position_seconds - 610.0).abs() < f64::EPSILON);
        // Back before the session's start, too.
        let mut session = centered_session_decoded(start, column_at(560.0), 522.0);
        assert!(seeks(&apply_seek(&mut session, 505.0), 505.0));
    }

    #[test]
    fn a_centered_seek_inside_the_decoded_columns_keeps_them() {
        // Decoded from 507 s to 600 s, without a left to fill.
        let start = column_at(507.36);
        for target in [540.0, 522.0, 580.0, 599.0] {
            let mut session = centered_session_decoded(start, column_at(600.0), 530.0);
            let action = apply_seek(&mut session, target);
            assert!(matches!(action, SessionAction::Continue), "{target}");
            assert!((session.target_position_seconds - target).abs() < f64::EPSILON);
        }
        // A playhead just ahead of the decoder is decoded next.
        let frontier = column_at(600.0);
        let mut session = centered_session_decoded(start, frontier, 530.0);
        let next = (frontier + SEEK_DECODE_THROUGH_COLUMNS - 1) as f64 / 46.875 + 0.001;
        assert!(matches!(
            apply_seek(&mut session, next),
            SessionAction::Continue
        ));
        let mut session = centered_session_decoded(start, frontier, 530.0);
        let beyond = (frontier + SEEK_DECODE_THROUGH_COLUMNS) as f64 / 46.875 + 0.001;
        assert!(seeks(&apply_seek(&mut session, beyond), beyond));
        // A view whose left the UI's ring no longer holds is not covered,
        // though the session decoded it.
        let mut session = centered_session_decoded(0, column_at(600.0), 590.0);
        let dropped = session.columns_produced - buffer_limits(&session).capacity;
        let left_dropped = (dropped as f64 - 10.0) / 46.875 + 12.64;
        assert!(seeks(&apply_seek(&mut session, left_dropped), left_dropped));
        // The file's end shows its last column.
        let mut session = centered_session_decoded(start, column_at(600.0), 590.0);
        session.end_column = Some(session.columns_produced);
        session.track_frames = Some(600 * 48_000);
        assert!(matches!(
            apply_seek(&mut session, 600.0),
            SessionAction::Continue
        ));
    }

    #[test]
    fn a_centered_seek_while_the_left_fills_keeps_what_it_covers() {
        // Started at 480 s for a playhead at 522 s, it decoded the right
        // to 560 s, and the left from 480 s to 500 s.
        let start = column_at(480.0);
        let playhead = column_at(522.0);
        let filling = || {
            let mut session = centered_session_decoded(start, column_at(500.0), 522.0);
            session.backfill = Some(Backfill {
                next: start,
                end: playhead,
                resume: Some(column_at(560.0)),
                right_ended: false,
            });
            session
        };
        // On the right, and in the left already decoded, the view is
        // there or about to be.
        for target in [540.0, 523.0, 499.0] {
            let mut session = filling();
            assert!(
                matches!(apply_seek(&mut session, target), SessionAction::Continue),
                "{target}"
            );
        }
        // In the left still to decode, past the right, and before the
        // session's start, it is not.
        for target in [510.0, 565.0, 485.0] {
            let mut session = filling();
            assert!(seeks(&apply_seek(&mut session, target), target), "{target}");
        }
        // Back on the right, the decoded left ends where the session left it.
        let on_the_right = || {
            let mut session = filling();
            session.columns_produced = column_at(560.0);
            session.backfill = Some(Backfill {
                next: column_at(500.0),
                end: playhead,
                resume: None,
                right_ended: false,
            });
            session
        };
        assert!(matches!(
            apply_seek(&mut on_the_right(), 499.0),
            SessionAction::Continue
        ));
        assert!(seeks(&apply_seek(&mut on_the_right(), 510.0), 510.0));
    }

    #[test]
    fn rolling_and_continued_sessions_always_seek() {
        let start = column_at(507.36);
        let mut session = centered_session_decoded(start, column_at(600.0), 530.0);
        apply_display_mode(&mut session, SpectrogramDisplayMode::Rolling);
        assert!(seeks(&apply_seek(&mut session, 540.0), 540.0));
        // A continued timeline is not the playing file's.
        let mut session = centered_session_decoded(start, column_at(600.0), 530.0);
        session.timeline_of = ColumnTimeline::EarlierFile {
            file_start_frame: u64::from(session.effective_rate) * 500,
        };
        assert!(seeks(&apply_seek(&mut session, 540.0), 540.0));
    }

    #[test]
    fn a_position_before_a_continued_file_needs_no_seek() {
        // The outgoing track ended 2 s into the session's timeline, where it
        // went on with the next file.
        let mut session = make_test_session();
        session.timeline_of = ColumnTimeline::EarlierFile {
            file_start_frame: 96_000,
        };
        session.session_start_column = column_at(1.98);
        session.columns_produced = column_at(2.0) + 100;
        // The outgoing track still plays; then the next file, with a clock
        // correction back below its start after reaching it.
        for position in [1.5, 1.95, 2.0, 2.01, 1.92, 2.1] {
            assert!(
                apply_position(&mut session, position).is_none(),
                "{position}"
            );
        }
        // Playback switched to a continuation the session has not reached:
        // the session decodes the rest of its file first.
        let mut session = make_test_session();
        session.columns_produced = column_at(100.0);
        session.pending_continue = Some((PathBuf::from("/music/next.flac"), 2));
        assert!(apply_position(&mut session, 200.0).is_none());
        session.pending_continue = Some((PathBuf::from("/music/next.flac"), 1));
        assert!(seeks(&apply_position(&mut session, 200.0).unwrap(), 200.0));
    }

    #[test]
    fn a_seek_on_a_continued_timeline_reads_the_playing_file_at_its_own_time() {
        // The outgoing track ended 2 s into the session's timeline, and the
        // playing file goes on from there: the same file played again, and
        // a shorter one.
        let file_start_frame = 96_000;
        for file_seconds in [2, 1] {
            let path = super::super::waveform_window::tests::write_test_wave(
                &vec![0; 48_000 * file_seconds],
                48_000,
                1,
            );
            let (mut source, ..) = open_audio_file(&path).expect("open the fixture");
            let mut session = make_test_session();
            session.timeline_of = ColumnTimeline::EarlierFile { file_start_frame };
            // Decoding from 0.8 s into the file on, after a seek.
            session.session_start_column = column_at(2.8);
            session.columns_produced = column_at(2.9);
            // Playback 0.5 s into the file needs the columns before.
            let action = apply_position(&mut session, 2.5).expect("a seek");
            assert!(seeks(&action, 2.5), "{file_seconds} s");
            let (events, event_rx) = unbounded();
            let (reports, _report_rx) = unbounded();
            handle_session_seek(&mut session, 2.5, &mut source, &events, &reports)
                .expect("the seek applies");
            // The file reads from 0.5 s on, less half an FFT before the
            // column, and its columns stay on the session's timeline.
            let grid_frame = column_grid_frame(column_at(2.5), 256, 1_024);
            assert_eq!(
                session.timeline.next_frame(),
                grid_frame - 1_024 - file_start_frame,
                "{file_seconds} s"
            );
            let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(reset)) = event_rx.try_recv() else {
                panic!("no reset");
            };
            assert!(reset.buffer_reset);
            assert_eq!(u64::from(reset.start_column_index), column_at(2.5));
            assert_eq!(
                session.timeline_of,
                ColumnTimeline::EarlierFile { file_start_frame }
            );

            // A seek the analysis thread sends is in the playing file, and
            // puts the session on its timeline.
            assert!(seeks(&apply_seek(&mut session, 0.25), 0.25));
            assert_eq!(session.timeline_of, ColumnTimeline::PlayingFile);
            handle_session_seek(&mut session, 0.25, &mut source, &events, &reports)
                .expect("the seek applies");
            let grid_frame = column_grid_frame(column_at(0.25), 256, 1_024);
            assert_eq!(session.timeline.next_frame(), grid_frame - 1_024);
            drop(source);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    #[test]
    fn playback_inside_the_decoded_columns_never_seeks() {
        // Playback, with a clock correction back now and then, through
        // what a centered session decoded, and on past it.
        let start = column_at(507.36);
        let mut session = centered_session_decoded(start, column_at(600.0), 522.0);
        let mut position = 522.0;
        for step in 0..2_000 {
            position += if step % 50 == 49 { -0.12 } else { 0.04 };
            assert!(
                apply_position(&mut session, position).is_none(),
                "{position}"
            );
            // The session decodes ahead as playback goes on.
            session.columns_produced = session.columns_produced.max(column_at(position + 60.0));
        }
    }

    #[test]
    fn a_position_after_a_seek_applies_once_the_seek_is_made() {
        let start = column_at(507.36);
        let (commands, command_rx) = unbounded();
        // Inside the decoded columns, the position applies at once.
        let mut session = centered_session_decoded(start, column_at(600.0), 522.0);
        for command in [
            SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: 523.0,
            },
            SpectrogramWorkerCommand::Seek {
                position_seconds: 540.0,
            },
            SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: 540.04,
            },
        ] {
            commands.send(command).unwrap();
        }
        let action = process_session_commands(&mut session, &command_rx);
        assert!(action.is_none(), "{:?}", action.map(|_| ()));
        assert!((session.target_position_seconds - 540.04).abs() < f64::EPSILON);
        // Past them, it follows the seek.
        let mut session = centered_session_decoded(start, column_at(530.0), 522.0);
        for command in [
            SpectrogramWorkerCommand::Seek {
                position_seconds: 610.0,
            },
            SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: 610.04,
            },
        ] {
            commands.send(command).unwrap();
        }
        let action = process_session_commands(&mut session, &command_rx).expect("a seek");
        assert!(seeks(&action, 610.0));
        assert!(matches!(
            session.queued_commands.make_contiguous(),
            [SpectrogramWorkerCommand::PositionUpdate { position_seconds }] if *position_seconds == 610.04
        ));
    }

    /// The analysis thread restarts a centered session of the noise fixture
    /// for 30 s, and the playhead's first column is out when a seek to 100
    /// s comes: inside the window the restart is for, but 47 s past what
    /// the session has decoded.
    #[test]
    fn a_seek_past_what_a_restarted_centered_session_decoded_skips_the_gap() {
        use super::super::{AnalysisContext, AnalysisRuntimeState, WaveformDecodeJob};

        let path = write_noise_fixture("seek-gap", 2);
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(path.clone());
        state.active_track_token = 1;
        state.active_session_effective_rate = BACKFILL_RATE;
        state.active_session_channel_count = 2;
        state.spectrogram_view_mode = SpectrogramViewMode::PerChannel;
        state.fft_size = 2_048;
        state.zoom_level = BACKFILL_ZOOM;
        state.spectrogram_widget_width = BACKFILL_WIDTH;
        state.spectrogram_session_widget_width = BACKFILL_WIDTH;

        let (command_tx, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let (reports, report_rx) = unbounded();
        let (waveform_job_tx, _waveform_jobs) = unbounded::<WaveformDecodeJob>();
        let (control_events, _control_event_rx) = unbounded::<AnalysisEvent>();
        let generation = AtomicU64::new(0);
        let columns_produced = AtomicU64::new(0);
        let track_length = PublishedTrackLength::default();
        let waveform_token = AtomicU64::new(0);
        // The worker waits at the playhead's first column until the
        // analysis thread has sent the seek.
        let (at_playhead, reached) = crossbeam_channel::bounded::<()>(0);
        let (go_on, resume) = crossbeam_channel::bounded::<()>(0);
        let held = AtomicBool::new(false);
        let output = WatchedOutput {
            events,
            on_chunk: |chunk: &PrecomputedSpectrogramChunk| {
                if chunk.column_count > 0 && !held.swap(true, Ordering::Relaxed) {
                    let _ = at_playhead.send(());
                    let _ = resume.recv();
                }
            },
        };
        let first_target = 30.0;
        let target = 100.0;
        let chunks = std::thread::scope(|scope| {
            scope.spawn(|| {
                spectrogram_worker_loop(
                    &command_rx,
                    &output,
                    &generation,
                    &columns_produced,
                    &track_length,
                    &reports,
                );
            });
            let command_tx = command_tx;
            let ctx = AnalysisContext {
                event_tx: &control_events,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_token,
                spectrogram_cmd_tx: &command_tx,
                spectrogram_decode_generation: &generation,
                spectrogram_decode_columns_produced: &columns_produced,
                spectrogram_track_length: &track_length,
            };
            state.refill_centered_spectrogram(first_target, &ctx);
            reached
                .recv_timeout(OUTPUT_WAIT)
                .expect("the playhead's first column");
            // As the engine reports a seek: the seek, the position it
            // landed at, and the next heartbeat.
            state.seek_spectrogram_position(target, &ctx);
            state.update_spectrogram_position(target, &ctx);
            state.update_spectrogram_position(target + 0.04, &ctx);
            go_on.send(()).unwrap();

            // Until the session has decoded to the end, and sent its left.
            let mut chunks = Vec::new();
            while !ends_complete(&chunks) {
                let Ok(event) = event_rx.recv_timeout(OUTPUT_WAIT) else {
                    break;
                };
                if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event {
                    chunks.push(chunk);
                }
            }
            // Playback goes on from there, with a clock correction: the
            // session keeps its columns.
            for position in [100.2, 100.6, 100.5, 101.0, 105.0] {
                state.update_spectrogram_position(position, &ctx);
            }
            command_tx
                .send(SpectrogramWorkerCommand::StopSession)
                .unwrap();
            // The worker ends once its commands do.
            drop(command_tx);
            chunks
        });
        let later: Vec<_> = event_rx
            .try_iter()
            .filter_map(|event| match event {
                AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => Some(chunk),
                _ => None,
            })
            .collect();
        std::fs::remove_file(&path).unwrap();
        assert_eq!(generation.load(Ordering::Relaxed), 1, "the seek restarted");
        assert!(
            later.iter().all(|chunk| !chunk.buffer_reset),
            "playback after the seek seeked again"
        );

        // The session seeked to where the view of 100 s starts, by the
        // length it published, which the analysis thread judged it by too.
        let end = track_length.frames_of(&path, BACKFILL_RATE).map(|frames| {
            centered_view::decoded_end_column(frames, 2_048, BACKFILL_ZOOM, BACKFILL_RATE)
        });
        let start_seconds = CenteredView::new(BACKFILL_WIDTH, BACKFILL_ZOOM, BACKFILL_RATE, end)
            .decode_start_seconds(target);
        let repositioned: Vec<_> = report_rx
            .try_iter()
            .filter_map(|report| match report {
                SessionReport::Repositioned { start_seconds, .. } => Some(start_seconds),
                _ => None,
            })
            .collect();
        assert!(
            matches!(repositioned.as_slice(), [start] if (start - start_seconds).abs() < 1e-6),
            "{repositioned:?}"
        );
        let reset = chunks
            .iter()
            .rposition(|chunk| chunk.buffer_reset)
            .expect("the seek's reset");
        let start = backfill_column(start_seconds);
        let playhead = backfill_column(target);
        assert_eq!(u64::from(chunks[reset].start_column_index), start);
        // The playhead's columns come first after it, then the left.
        let spans = spans_of(&chunks[reset..]);
        assert_eq!(spans[0].0, playhead, "{spans:?}");
        assert!(spans.contains(&(start, playhead)), "{spans:?}");
        // Nothing between the columns decoded for 30 s and the view of 100
        // s was decoded.
        let gap = backfill_column(first_target + 10.0)..start;
        for (from, to) in spans_of(&chunks) {
            assert!(
                to <= gap.start || from >= gap.end,
                "decoded {from}..{to} in the gap {gap:?}"
            );
        }
    }

    // -----------------------------------------------------------------------
    // The end of a centered view, by the track's real length
    // -----------------------------------------------------------------------

    /// The columns the UI's centered view shows of a track decoded to the
    /// column before `end`, which its chunks then carry as their estimate,
    /// with the playhead at column `now` and a window of `window` columns:
    /// `centeredDisplayRange` in `ui/src/SpectrogramItem.cpp`. The view
    /// shows the whole track once the window covers 90% of its columns,
    /// and otherwise centers the playhead; once the rest of the track is
    /// shorter than half a window, it ends at the last column, a window
    /// before it.
    fn ui_centered_view(now: u64, window: u64, end: u64) -> std::ops::Range<u64> {
        let half = window / 2;
        let (mut left, mut right) = if window * 100 / end >= 90 {
            (0, end - 1)
        } else {
            let right = (end - 1).min(now.saturating_sub(half) + window - 1);
            ((right + 1).saturating_sub(window), right)
        };
        if end - 1 < now + half {
            right = right.min(end - 1);
            left = (right + 1).saturating_sub(window);
        }
        left..right + 1
    }

    /// A mono 16-bit WAV of `frames` frames of noise at `rate`.
    fn write_frames_fixture(name: &str, rate: u32, frames: u32) -> PathBuf {
        let folder = crate::paths::test_root();
        std::fs::create_dir_all(&folder).unwrap();
        let path = folder.join(format!("analysis-end-{name}-{rate}-{frames}.wav"));
        let data_bytes = frames * 2;
        let mut bytes = b"RIFF".to_vec();
        bytes.extend_from_slice(&(36 + data_bytes).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt \x10\0\0\0\x01\0\x01\0");
        bytes.extend_from_slice(&rate.to_le_bytes());
        bytes.extend_from_slice(&(rate * 2).to_le_bytes());
        bytes.extend_from_slice(b"\x02\0\x10\0data");
        bytes.extend_from_slice(&data_bytes.to_le_bytes());
        let mut random = 0x2545_f491_u32 ^ rate ^ frames;
        for _ in 0..frames {
            random = random.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let sample = i16::try_from(random >> 18).unwrap() - 8_192;
            bytes.extend_from_slice(&sample.to_le_bytes());
        }
        std::fs::write(&path, bytes).unwrap();
        path
    }

    /// Where the view of a track ends, and what its window is a share of,
    /// is where sessions of it end: the decoder places its views by it
    /// before any session has decoded the end. Sessions that start at
    /// other columns end at the same one, at every rate, window and zoom,
    /// whether a column's rows end on its last or part of the way through.
    #[test]
    fn the_decoded_end_column_is_where_sessions_of_the_track_end() {
        for (rate, frames) in [
            (8_000, 48_001),
            (44_100, 264_601),
            (48_000, 288_517),
            (96_000, 576_111),
        ] {
            let path = write_frames_fixture("predicted", rate, frames);
            let seconds = f64::from(frames) / f64::from(rate);
            for fft in [512, 2_048, 8_192] {
                for zoom in [0.3, 1.0, 1.7, 4.0] {
                    let expected =
                        centered_view::decoded_end_column(u64::from(frames), fft, zoom, rate);
                    for start in [seconds - 1.5, seconds * 0.5] {
                        let mut command = test_new_track(&path.to_string_lossy(), 1);
                        if let SpectrogramWorkerCommand::NewTrack {
                            fft_size,
                            zoom_level,
                            display_mode,
                            start_seconds,
                            target_position_seconds,
                            ..
                        } = &mut command
                        {
                            *fft_size = fft;
                            *zoom_level = zoom;
                            *display_mode = SpectrogramDisplayMode::Centered;
                            *start_seconds = start;
                            *target_position_seconds = start;
                        }
                        let chunks = run_watched_session(
                            command,
                            1,
                            unbounded(),
                            &Arc::new(AtomicU64::new(1)),
                            |_| {},
                            ends_complete,
                        );
                        let last = chunks.last().unwrap();
                        assert!(last.complete, "{rate} Hz {fft} {zoom} from {start} s");
                        assert_eq!(
                            u64::from(last.total_columns_estimate),
                            expected,
                            "{rate} Hz, {frames} frames, FFT {fft}, zoom {zoom}, from {start} s"
                        );
                    }
                }
            }
            std::fs::remove_file(path).unwrap();
        }
    }

    /// Releases a worker that a test holds at a column, and ends it, when
    /// dropped: as the test ends, or as a failed assertion unwinds it, which
    /// would otherwise wait for the worker while the worker waits for it.
    struct ReleasedWorker {
        commands: Option<Sender<SpectrogramWorkerCommand>>,
        /// Dropped, they fail the worker's reports that it reached where
        /// it waits.
        reached: Vec<Receiver<()>>,
        /// Dropped, they let the worker go on from where it waits.
        go_on: Vec<Sender<()>>,
    }

    impl Drop for ReleasedWorker {
        fn drop(&mut self) {
            self.reached.clear();
            self.go_on.clear();
            if let Some(commands) = self.commands.take() {
                let _ = commands.send(SpectrogramWorkerCommand::StopSession);
            }
        }
    }

    /// How a seek reaches the session: as a restart of it, or as a seek
    /// the analysis thread hands it, if it judges it can take it.
    #[derive(Debug, Clone, Copy)]
    enum EndSeek {
        Restart,
        Seek,
    }

    /// The columns of `path`, of `frames` frames at `rate`, that the UI
    /// holds, and the column after the last, once a centered session at
    /// `zoom` for a view `width` pixels
    /// wide, started at the track's start, has been sent a seek to
    /// `target` as `how` and has decoded to the end of the file.
    fn columns_after_a_centered_seek(
        (path, frames): (&Path, u32),
        rate: u32,
        (zoom, width): (f32, u32),
        target: f64,
        how: EndSeek,
    ) -> (std::collections::BTreeMap<u64, Vec<u8>>, u64) {
        use super::super::{AnalysisContext, AnalysisRuntimeState, WaveformDecodeJob};

        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(path.to_path_buf());
        state.active_track_token = 1;
        state.active_session_effective_rate = rate;
        state.active_session_channel_count = 1;
        state.spectrogram_view_mode = SpectrogramViewMode::Downmix;
        state.fft_size = 2_048;
        state.zoom_level = zoom;
        state.spectrogram_widget_width = width;
        state.spectrogram_session_widget_width = width;

        let (command_tx, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let (reports, _report_rx) = unbounded();
        let (waveform_job_tx, _waveform_jobs) = unbounded::<WaveformDecodeJob>();
        let (control_events, _control_event_rx) = unbounded::<AnalysisEvent>();
        let generation = AtomicU64::new(0);
        let columns_produced = AtomicU64::new(0);
        let track_length = PublishedTrackLength::default();
        let waveform_token = AtomicU64::new(0);
        // The worker waits at the first column of the track's start until
        // the analysis thread has sent the seek.
        let (at_first_column, reached) = crossbeam_channel::bounded::<()>(0);
        let (go_on, resume) = crossbeam_channel::bounded::<()>(0);
        let held = AtomicBool::new(false);
        let output = WatchedOutput {
            events,
            on_chunk: |chunk: &PrecomputedSpectrogramChunk| {
                if chunk.column_count > 0 && !held.swap(true, Ordering::Relaxed) {
                    let _ = at_first_column.send(());
                    let _ = resume.recv();
                }
            },
        };
        let chunks = std::thread::scope(|scope| {
            scope.spawn(|| {
                spectrogram_worker_loop(
                    &command_rx,
                    &output,
                    &generation,
                    &columns_produced,
                    &track_length,
                    &reports,
                );
            });
            // Declared before the context that borrows it, it is dropped
            // after it, and before the scope waits for the worker.
            let worker = ReleasedWorker {
                commands: Some(command_tx),
                reached: vec![reached],
                go_on: vec![go_on],
            };
            let ctx = AnalysisContext {
                event_tx: &control_events,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_token,
                spectrogram_cmd_tx: worker.commands.as_ref().unwrap(),
                spectrogram_decode_generation: &generation,
                spectrogram_decode_columns_produced: &columns_produced,
                spectrogram_track_length: &track_length,
            };
            state.refill_centered_spectrogram(0.0, &ctx);
            worker.reached[0]
                .recv_timeout(OUTPUT_WAIT)
                .expect("the first column");
            // The session published the length of the file when it opened
            // it, which the seek is placed by.
            assert_eq!(track_length.frames_of(path, rate), Some(u64::from(frames)));
            match how {
                EndSeek::Restart => state.refill_centered_spectrogram(target, &ctx),
                EndSeek::Seek => state.seek_spectrogram_position(target, &ctx),
            }
            state.update_spectrogram_position(target, &ctx);
            worker.go_on[0].send(()).unwrap();

            // Until the session for the seek has decoded to the end, and
            // sent the left of its view.
            let mut chunks = Vec::new();
            while !chunks
                .last()
                .is_some_and(|chunk: &PrecomputedSpectrogramChunk| {
                    chunk.complete && chunk.generation == generation.load(Ordering::Relaxed)
                })
            {
                let event = event_rx.recv_timeout(OUTPUT_WAIT).expect("the end");
                if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event {
                    chunks.push(chunk);
                }
            }
            chunks
        });
        // The UI holds the columns of the last session since its last
        // reset.
        let last_generation = chunks.last().unwrap().generation;
        let session: Vec<_> = chunks
            .into_iter()
            .filter(|chunk| chunk.generation == last_generation)
            .collect();
        let reset = session
            .iter()
            .rposition(|chunk| chunk.buffer_reset)
            .expect("the seek's reset");
        let end = u64::from(session.last().unwrap().total_columns_estimate);
        (columns_of(&session[reset..]), end)
    }

    /// A centered view of a file that tells its length, with a seek a few
    /// seconds before its end. Once the session has decoded the end, the UI
    /// ends the view there, a window before it: every column of that view
    /// is decoded. And a view that covers 90% of the track's columns shows
    /// the whole of it, from its start, while the playhead is more than half
    /// a window before the end. Placed by the column estimate, whose
    /// allowance runs 64 columns of 1024 frames past the end (1.5 s at 44.1
    /// kHz, 8.2 s at 8 kHz), the seek decoded from further right: at 8 kHz
    /// in every case, at 44.1 kHz, whose allowance the 2 s of warm-up hides
    /// at the end, in the view of the whole track. Placed by the seconds of
    /// the file, not the columns decoded of it, a view of 90% to 90.4% of
    /// the columns did not show the whole track; nor did a view that the
    /// rounding of Qt's zoom gives a column more than its pixels.
    #[test]
    fn a_centered_view_by_the_end_of_a_track_is_decoded_to_its_left_edge() {
        // 40 s at 8 kHz and at 44.1 kHz, and 70.08 s at 8 kHz.
        let (short, short_cd, long) = (320_000, 1_764_000, 560_640);
        // (rate, frames, (zoom, width), playhead, whether the UI shows the
        // whole track): views of 64% of the track, as wide as at zoom 1 and
        // as one zoomed in, and of 91%, which shows the whole track, and
        // would not by the estimate. Views at the edge of the whole track,
        // of 1866 columns at 8 kHz and 1722 at 44.1 kHz: of 89.98% and
        // 89.95%, and of 90.03% and 90.01%, which are 35.91 s and 35.99 s,
        // less than 90% of the 40 s, and of a column more. And at zoom 0.57,
        // 300 samples a column, which Qt draws at 171 / 300 × 300 / 171, a
        // bit below 1: 1680 pixels show 1681 of the 1867 columns, 90.04%,
        // and 1679 pixels 1680, 89.98%.
        let cases = [
            (8_000, short, (1.0, 1_200), 37.0, Some(false)),
            (8_000, short, (2.0, 1_200), 37.0, Some(false)),
            (8_000, short, (1.0, 1_710), 37.0, None),
            (8_000, short, (1.0, 1_710), 21.3, Some(true)),
            (8_000, short, (1.0, 1_679), 21.9, Some(false)),
            (8_000, short, (1.0, 1_680), 21.9, Some(true)),
            (8_000, short, (1.0, 1_681), 21.9, Some(true)),
            (8_000, long, (0.57, 1_679), 38.4, Some(false)),
            (8_000, long, (0.57, 1_680), 38.4, Some(true)),
            (44_100, short_cd, (1.0, 1_100), 37.0, Some(false)),
            (44_100, short_cd, (2.0, 2_200), 37.0, Some(false)),
            (44_100, short_cd, (1.0, 1_568), 37.0, None),
            (44_100, short_cd, (1.0, 1_568), 21.5, Some(true)),
            (44_100, short_cd, (1.0, 1_549), 21.5, Some(false)),
            (44_100, short_cd, (1.0, 1_550), 21.5, Some(true)),
            (44_100, short_cd, (1.0, 1_551), 21.5, Some(true)),
        ];
        for file in [(8_000, short), (8_000, long), (44_100, short_cd)] {
            let (rate, frames) = file;
            let path = write_frames_fixture("pinned", rate, frames);
            for (_, _, view, target, whole) in cases
                .iter()
                .filter(|(case_rate, case_frames, ..)| (*case_rate, *case_frames) == file)
            {
                let (zoom, width) = *view;
                // The view's window and playhead column, as Qt's
                // centeredViewLocked reckons them at a device pixel ratio
                // of 1, with the zoom its effectiveZoomLocked draws at.
                let hop = super::super::column_hop::column_hop(rate, zoom);
                let drawn = super::super::column_hop::effective_zoom(
                    rate,
                    hop,
                    super::super::column_hop::render_zoom(rate, hop, f64::from(zoom)),
                );
                let window = f64_to_u64_saturating((f64::from(width) / drawn).ceil());
                let columns_per_second = f64::from(rate) / usize_to_f64_approx(hop);
                let now = f64_to_u64_saturating((target * columns_per_second).floor());
                for how in [EndSeek::Restart, EndSeek::Seek] {
                    let (columns, end) =
                        columns_after_a_centered_seek((&path, frames), rate, *view, *target, how);
                    // The columns the decoder expects of the file.
                    assert_eq!(
                        end,
                        centered_view::decoded_end_column(u64::from(frames), 2_048, zoom, rate),
                        "{rate} Hz {view:?} {how:?}"
                    );
                    let shown = ui_centered_view(now, window, end);
                    if let Some(whole) = whole {
                        assert_eq!(
                            shown == (0..end),
                            *whole,
                            "{rate} Hz {view:?} at {target} s: the view {shown:?} of {end}"
                        );
                    }
                    let missing: Vec<u64> = shown
                        .clone()
                        .filter(|column| !columns.contains_key(column))
                        .collect();
                    assert!(
                        missing.is_empty(),
                        "{rate} Hz {view:?} at {target} s {how:?}: the view {shown:?} misses {} \
                         columns, {:?}..{:?}",
                        missing.len(),
                        missing.first(),
                        missing.last(),
                    );
                }
            }
            std::fs::remove_file(path).unwrap();
        }
    }
    /// The session of the outgoing track opens its file slowly, as one on a
    /// network share can, and publishes the file's length after the track
    /// has changed. A seek in the new track before the new track's session
    /// has published its own must not place the view by the outgoing
    /// file's length: the new track's length is unknown until then, and
    /// the view stays centered on the playhead.
    #[test]
    fn a_probe_that_ends_after_the_track_changed_does_not_place_the_new_tracks_view() {
        use super::super::{AnalysisContext, AnalysisRuntimeState, WaveformDecodeJob};

        // 40 s, and 60 s, at 8 kHz. By the outgoing file's 1866 columns, a
        // view of 1200 at 37 s ends at them, 22.2 s left of where the
        // playhead centers it in the 60 s.
        let rate = 8_000;
        let outgoing = write_frames_fixture("probe-outgoing", rate, 320_000);
        let playing = write_frames_fixture("probe-playing", rate, 480_000);
        let (width, target) = (1_200, 37.0);
        let unknown = CenteredView::new(width, 1.0, rate, None).decode_start_seconds(target);
        let outgoing_end = centered_view::decoded_end_column(320_000, 2_048, 1.0, rate);
        let by_outgoing =
            CenteredView::new(width, 1.0, rate, Some(outgoing_end)).decode_start_seconds(target);
        let playing_end = centered_view::decoded_end_column(480_000, 2_048, 1.0, rate);
        let by_playing =
            CenteredView::new(width, 1.0, rate, Some(playing_end)).decode_start_seconds(target);
        assert!(by_outgoing < unknown - 5.0, "{by_outgoing} {unknown}");
        assert!(
            (by_playing - unknown).abs() < 1e-9,
            "{by_playing} {unknown}"
        );

        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(outgoing.clone());
        state.active_track_token = 1;
        state.active_session_effective_rate = rate;
        state.active_session_channel_count = 1;
        state.spectrogram_view_mode = SpectrogramViewMode::Downmix;
        state.fft_size = 2_048;
        state.zoom_level = 1.0;
        state.spectrogram_widget_width = width;
        state.spectrogram_session_widget_width = width;

        let (command_tx, command_rx) = unbounded();
        let (events, event_rx) = unbounded();
        let (reports, report_rx) = unbounded();
        let (waveform_job_tx, _waveform_jobs) = unbounded::<WaveformDecodeJob>();
        let (control_events, _control_event_rx) = unbounded::<AnalysisEvent>();
        let generation = AtomicU64::new(0);
        let columns_produced = AtomicU64::new(0);
        let track_length = PublishedTrackLength::default();
        let waveform_token = AtomicU64::new(0);
        let output = WatchedOutput {
            events,
            on_chunk: |_: &PrecomputedSpectrogramChunk| {},
        };
        let (outgoing_reached, outgoing_release) = decode_test_hooks::hold_open_of(&outgoing);
        let (playing_reached, playing_release) = decode_test_hooks::hold_open_of(&playing);
        let first_chunk = std::thread::scope(|scope| {
            scope.spawn(|| {
                spectrogram_worker_loop(
                    &command_rx,
                    &output,
                    &generation,
                    &columns_produced,
                    &track_length,
                    &reports,
                );
            });
            // Declared before the context that borrows it, it is dropped
            // after it, and before the scope waits for the worker.
            let mut worker = ReleasedWorker {
                commands: Some(command_tx),
                reached: vec![outgoing_reached, playing_reached],
                go_on: vec![outgoing_release, playing_release],
            };
            let ctx = AnalysisContext {
                event_tx: &control_events,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_token,
                spectrogram_cmd_tx: worker.commands.as_ref().unwrap(),
                spectrogram_decode_generation: &generation,
                spectrogram_decode_columns_produced: &columns_produced,
                spectrogram_track_length: &track_length,
            };
            // The outgoing track's session waits to open its file.
            state.refill_centered_spectrogram(0.0, &ctx);
            worker.reached[0]
                .recv_timeout(OUTPUT_WAIT)
                .expect("the outgoing session at its file");
            state.handle_track_change(playing.clone(), true, false, 2, &ctx);
            // The probe ends after the track change, and the outgoing
            // session publishes the file's length as it reports it opened.
            drop(worker.go_on.remove(0));
            loop {
                match report_rx
                    .recv_timeout(OUTPUT_WAIT)
                    .expect("the outgoing file opened")
                {
                    SessionReport::Opened { path, .. } if path == outgoing => break,
                    _ => {}
                }
            }
            assert_eq!(
                track_length.frames_of(&outgoing, rate),
                Some(320_000),
                "the outgoing session published its file's length"
            );
            // A seek in the new track, before its session has opened the
            // file, restarts the view of it.
            let restart_start = state.spectrogram_restart_start_seconds(target, &ctx);
            assert!(
                (restart_start - unknown).abs() < 1e-9,
                "the restart starts at {restart_start} s, not {unknown} s (by the outgoing \
                 file's length, {by_outgoing} s)"
            );
            state.refill_centered_spectrogram(target, &ctx);
            let restart_generation = generation.load(Ordering::Relaxed);
            drop(worker.go_on.remove(0));
            let first_chunk = loop {
                let event = event_rx.recv_timeout(OUTPUT_WAIT).expect("the restart");
                // The session's first chunk, after the reset the worker
                // sends before it opens the file, which has no bins.
                if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event {
                    if chunk.generation == restart_generation && chunk.bins_per_column > 0 {
                        break chunk;
                    }
                }
            };
            // Once the new track's session has published, its length
            // places the view.
            assert_eq!(track_length.frames_of(&playing, rate), Some(480_000));
            assert_eq!(track_length.frames_of(&outgoing, rate), None);
            first_chunk
        });
        std::fs::remove_file(outgoing).unwrap();
        std::fs::remove_file(playing).unwrap();
        let hops = column_hops(2_048, 1.0, rate);
        let (start_column, _) = stft_seek_grid(unknown, rate, hops.stft_hop, hops.effective_hop);
        assert_eq!(u64::from(first_chunk.start_column_index), start_column);
    }
}
