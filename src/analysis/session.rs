// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc,
};
use std::time::Duration;

use crossbeam_channel::{unbounded, Receiver, Sender};

use super::decoders::{
    deinterleave_samples, open_audio_file, u64_to_u32_saturating, AudioFrameSource,
};
use super::fft::{waveform_sample_rate_divisor, SpectrogramDecimator, StftComputer};
use super::{
    f64_to_u64_saturating, AnalysisEvent, PrecomputedSpectrogramChunk, SpectrogramDisplayMode,
    SpectrogramViewMode, REFERENCE_HOP,
};

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

fn decimation_factor_for_hop(hop: usize) -> usize {
    if hop == 0 {
        return 1;
    }
    (REFERENCE_HOP / hop).max(1)
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

const PRECOMPUTED_DB_RANGE: f32 = 132.0;

/// Compute the expected peak power dB for a full-scale sine in a
/// Blackman-Harris-windowed FFT of the given size.  This is used to
/// normalise the STFT output so that a full-scale signal maps to u8≈255
/// regardless of FFT size.
///
/// For BH4 window: sum(w) = N × a0 (a0 = 0.35875).
/// Peak bin magnitude = sum(w) / 2, power = (N × a0 / 2)².
#[allow(clippy::cast_precision_loss)]
fn stft_peak_power_db(fft_size: usize) -> f64 {
    20.0 * (fft_size as f64 * 0.35875 / 2.0).log10()
}

fn precomputed_to_u8_spectrum(v: f32, fft_size: usize) -> u8 {
    let range = f64::from(PRECOMPUTED_DB_RANGE);
    let db = if v > 0.0 {
        (10.0 / std::f64::consts::LN_10) * f64::from(v).ln()
    } else {
        -200.0
    };
    // Normalise for FFT size: anchor at the BH4 peak power so that a
    // full-scale signal maps to u8≈255 for any FFT size.
    let peak_db = stft_peak_power_db(fft_size);
    let xdb = (db + range - peak_db).clamp(0.0, range);
    let scaled = (xdb / range) * 255.0;
    // Value is clamped to 0.0..=255.0, so truncation and sign loss are impossible.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let result = scaled.round().clamp(0.0, 255.0) as u8;
    result
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
        channel_count: usize,
        start_seconds: f64,
        emit_initial_reset: bool,
        clear_history_on_reset: bool,
        view_mode: SpectrogramViewMode,
        display_mode: SpectrogramDisplayMode,
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
    #[allow(dead_code)]
    SetDisplayMode(SpectrogramDisplayMode),
    Stop,
}

pub(super) struct SpectrogramWorkerHandles {
    pub(super) cmd_tx: Sender<SpectrogramWorkerCommand>,
    pub(super) decode_generation: Arc<AtomicU64>,
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

/// Drain STFT rows from the staging pipeline, quantize, and emit
/// chunks via the channel.  Extracted from `centered_staging_decode`
/// to stay within clippy's line limit.
/// Returns `true` to continue decoding, `false` if the receiver dropped.
fn staging_drain_stft_rows(
    stfts: &mut [StftComputer],
    decimators: &mut [SpectrogramDecimator],
    state: &mut StagingChunkState,
    tx: &Sender<PrecomputedSpectrogramChunk>,
) -> bool {
    loop {
        let mut rows: Vec<Vec<f32>> = Vec::with_capacity(state.channel_count);
        let mut all_have_row = true;
        for stft in stfts.iter_mut() {
            let row = stft.take_rows(1);
            if row.is_empty() {
                all_have_row = false;
                break;
            }
            rows.push(row.into_iter().next().unwrap());
        }
        if !all_have_row {
            break;
        }

        let mut decimated_rows: Vec<Option<Vec<f32>>> = Vec::with_capacity(state.channel_count);
        for (ch, row) in rows.into_iter().enumerate() {
            if let Some(dec) = decimators.get_mut(ch) {
                decimated_rows.push(dec.push(row));
            } else {
                decimated_rows.push(Some(row));
            }
        }

        if !decimated_rows.iter().all(Option::is_some) {
            continue;
        }

        for maybe_row in &decimated_rows {
            let row = maybe_row.as_ref().unwrap();
            for &v in row.iter().take(state.bins_per_column) {
                state
                    .chunk_buf
                    .push(precomputed_to_u8_spectrum(v, state.fft_size));
            }
            if row.len() < state.bins_per_column {
                state
                    .chunk_buf
                    .extend(std::iter::repeat_n(0u8, state.bins_per_column - row.len()));
            }
        }
        state.chunk_columns += 1;
        state.columns_produced += 1;

        if state.chunk_columns >= state.target_chunk_columns {
            let coverage =
                seconds_from_frames(state.total_covered_samples, u64::from(state.effective_rate));
            let chunk = PrecomputedSpectrogramChunk {
                track_token: 0,
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
            state.target_chunk_columns = next_target_chunk_columns(
                state.target_chunk_columns,
                SpectrogramDisplayMode::Centered,
            );
        }
    }
    true // ok, continue decoding
}

fn centered_staging_decode(
    path: &Path,
    fft_size: usize,
    hop_size: usize,
    view_mode: SpectrogramViewMode,
    stop: &AtomicBool,
    tx: &Sender<PrecomputedSpectrogramChunk>,
) {
    let Some((mut source, native_sample_rate, native_channels, total_columns_estimate)) =
        open_audio_file(path)
    else {
        profile_eprintln!("[staging] failed to open {}", path.display());
        return;
    };

    let divisor = usize::try_from(waveform_sample_rate_divisor(native_sample_rate)).unwrap_or(1);
    let divisor_u64 = u64::try_from(divisor).unwrap_or(1);
    let effective_rate = u32::try_from(native_sample_rate / divisor_u64.max(1)).unwrap_or(48_000);
    let channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let bins_per_column = (fft_size / 2) + 1;
    let decimation_factor = decimation_factor_for_hop(hop_size);

    let mut stfts: Vec<StftComputer> = (0..channel_count)
        .map(|_| StftComputer::new(fft_size, hop_size))
        .collect();
    let mut decimators: Vec<SpectrogramDecimator> = (0..channel_count)
        .map(|_| SpectrogramDecimator::new(decimation_factor))
        .collect();

    let mut state = StagingChunkState {
        fft_size,
        bins_per_column,
        channel_count,
        effective_rate,
        effective_hop: hop_size * decimation_factor,
        total_columns_estimate,
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

    loop {
        if stop.load(Ordering::Relaxed) {
            // Flush any partial chunk so the drain captures as much data as possible.
            if let Some(chunk) = state.take_partial_chunk() {
                let _ = tx.send(chunk);
            }
            profile_eprintln!("[staging] stopped after {} columns", state.columns_produced);
            return;
        }

        let audio = match source.next_frames() {
            Some(af) if af.frames == 0 => continue,
            Some(af) => af,
            None => break, // EOF
        };

        let effective_frames = audio.frames / divisor;
        let per_channel = deinterleave_samples(
            &audio.samples,
            audio.frames,
            audio.channels,
            channel_count,
            divisor,
            effective_frames,
            view_mode,
        );

        #[allow(clippy::cast_possible_truncation)]
        {
            state.total_covered_samples += effective_frames as u64;
        }

        for (ch, channel_samples) in per_channel.iter().enumerate() {
            if let Some(stft) = stfts.get_mut(ch) {
                stft.enqueue_samples(channel_samples, effective_rate);
            }
        }

        if !staging_drain_stft_rows(&mut stfts, &mut decimators, &mut state, tx) {
            return; // receiver dropped
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
/// with 0-based column indices and `track_token: 0` (placeholder).
/// Returns the chunk receiver and the thread's join handle.
// Callers are added in a subsequent task (handle_prepare_gapless_continuation).
#[allow(dead_code)]
pub(super) fn spawn_centered_staging_worker(
    path: PathBuf,
    fft_size: usize,
    hop_size: usize,
    view_mode: SpectrogramViewMode,
    stop: Arc<AtomicBool>,
) -> (
    Receiver<PrecomputedSpectrogramChunk>,
    std::thread::JoinHandle<()>,
) {
    let (tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();
    let handle = std::thread::Builder::new()
        .name("ferrous-spectrogram-staging".to_string())
        .spawn(move || {
            centered_staging_decode(&path, fft_size, hop_size, view_mode, &stop, &tx);
        })
        .expect("failed to spawn staging thread");
    (rx, handle)
}

// ---------------------------------------------------------------------------
// Spectrogram decode worker
// ---------------------------------------------------------------------------

pub(super) fn spawn_spectrogram_decode_worker(
    cmd_rx: Receiver<SpectrogramWorkerCommand>,
    event_tx: Sender<AnalysisEvent>,
    active_token: Arc<AtomicU64>,
    generation: Arc<AtomicU64>,
    columns_produced: Arc<AtomicU64>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-spectrogram-decode".to_string())
        .spawn(move || {
            spectrogram_worker_loop(
                &cmd_rx,
                &event_tx,
                &active_token,
                &generation,
                &columns_produced,
            );
        });
}

/// Session parameters cached by the worker loop so that a
/// `ContinueWithFile` arriving outside a live session can be
/// converted to a `NewTrack` fallback.
struct LastSessionParams {
    fft_size: usize,
    hop_size: usize,
    channel_count: usize,
    view_mode: SpectrogramViewMode,
    display_mode: SpectrogramDisplayMode,
}

fn spectrogram_worker_loop(
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
) {
    let mut next_cmd: Option<SpectrogramWorkerCommand> = None;
    let mut last_params: Option<LastSessionParams> = None;
    loop {
        let cmd = match next_cmd.take() {
            Some(cmd) => cmd,
            None => match cmd_rx.recv() {
                Ok(cmd) => cmd,
                Err(_) => break,
            },
        };

        match cmd {
            SpectrogramWorkerCommand::NewTrack {
                fft_size,
                hop_size,
                channel_count,
                view_mode,
                display_mode,
                ..
            } => {
                last_params = Some(LastSessionParams {
                    fft_size,
                    hop_size,
                    channel_count,
                    view_mode,
                    display_mode,
                });
                next_cmd = run_spectrogram_session(
                    &cmd,
                    cmd_rx,
                    event_tx,
                    active_token,
                    generation,
                    columns_produced_out,
                );
                if matches!(next_cmd, Some(SpectrogramWorkerCommand::Stop)) {
                    break;
                }
            }
            SpectrogramWorkerCommand::ContinueWithFile { path, track_token } => {
                // No active session — convert to NewTrack fallback.
                if let Some(params) = &last_params {
                    let gen = generation.fetch_add(1, Ordering::Relaxed) + 1;
                    let fallback = SpectrogramWorkerCommand::NewTrack {
                        track_token,
                        generation: gen,
                        path,
                        fft_size: params.fft_size,
                        hop_size: params.hop_size,
                        channel_count: params.channel_count,
                        start_seconds: 0.0,
                        emit_initial_reset: false,
                        clear_history_on_reset: false,
                        view_mode: params.view_mode,
                        display_mode: params.display_mode,
                    };
                    last_params = Some(LastSessionParams {
                        fft_size: params.fft_size,
                        hop_size: params.hop_size,
                        channel_count: params.channel_count,
                        view_mode: params.view_mode,
                        display_mode: params.display_mode,
                    });
                    next_cmd = run_spectrogram_session(
                        &fallback,
                        cmd_rx,
                        event_tx,
                        active_token,
                        generation,
                        columns_produced_out,
                    );
                    if matches!(next_cmd, Some(SpectrogramWorkerCommand::Stop)) {
                        break;
                    }
                } else {
                    eprintln!(
                        "[ferrous] ContinueWithFile outside session with no prior params — ignoring"
                    );
                }
            }
            SpectrogramWorkerCommand::Stop => break,
            _ => {} // UpdateTrackToken, CancelPendingContinue, etc. — stale outside session
        }
    }
}

// ---------------------------------------------------------------------------
// SpectrogramSessionState
// ---------------------------------------------------------------------------

/// Holds decode state for a spectrogram session.
struct SpectrogramSessionState {
    track_token: u64,
    gen: u64,
    fft_size: usize,
    hop_size: usize,
    effective_hop: usize,
    view_mode: SpectrogramViewMode,
    display_mode: SpectrogramDisplayMode,
    channel_count: usize,
    bins_per_column: usize,
    total_columns_estimate: u32,
    effective_rate: u32,
    cols_per_second: f64,
    divisor: usize,

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
    decimators: Vec<SpectrogramDecimator>,
    packet_counter: usize,
    chunk_buf: Vec<u8>,
    chunk_columns: u16,
    chunk_start_index: u64,
    target_chunk_columns: u16,
    total_covered_samples: u64,

    // Rate throttling
    session_start_time: std::time::Instant,
    /// Number of columns to decode immediately after a reset/handoff before
    /// re-enabling rolling-mode throttling and lookahead parking.
    post_reset_unthrottled_columns: u32,
    decode_rate_limit: f64,
    lookahead_columns: u64,

    /// Whether a `GStreamer` duration re-query has already been attempted.
    /// Raw DTS/AC3 files often lack duration at pipeline start; a re-query
    /// after some data has been decoded may succeed.
    #[cfg(feature = "gst")]
    gst_duration_requeried: bool,

    /// Stored `ContinueWithFile` command to apply when the current file
    /// reaches EOF.  Set by command processing, consumed at EOF.
    /// Fields: `(path, track_token)`.
    pending_continue: Option<(PathBuf, u64)>,
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
    initial_cmd: &SpectrogramWorkerCommand,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    let &SpectrogramWorkerCommand::NewTrack {
        track_token,
        generation: gen,
        ref path,
        fft_size,
        hop_size,
        channel_count: _channel_count,
        start_seconds,
        emit_initial_reset,
        clear_history_on_reset,
        view_mode,
        display_mode,
    } = initial_cmd
    else {
        return None;
    };

    let _start = std::time::Instant::now();
    profile_eprintln!(
        "[spect-worker] SESSION START path={} gen={gen} token={track_token} fft={fft_size} hop={hop_size} ch={channel_count} view={view_mode:?} display={display_mode:?} start_s={start_seconds:.2}",
        path.file_name().unwrap_or_default().to_string_lossy(),
        channel_count = _channel_count,
    );

    let bins_per_column = (fft_size / 2) + 1;
    let (mut source, native_sample_rate, native_channels, total_columns_estimate) =
        open_audio_file(path)?;

    profile_eprintln!(
        "[spect-worker] file opened in {:.2}ms sr={native_sample_rate} ch={native_channels} est_cols={total_columns_estimate}",
        _start.elapsed().as_secs_f64() * 1000.0,
    );

    let divisor = usize::try_from(waveform_sample_rate_divisor(native_sample_rate)).unwrap_or(1);
    let divisor_u64 = u64::try_from(divisor).unwrap_or(1);
    let effective_rate = u32::try_from(native_sample_rate / divisor_u64.max(1)).unwrap_or(48_000);
    let actual_channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let decimation_factor = decimation_factor_for_hop(hop_size);
    let effective_hop = hop_size * decimation_factor;
    let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(effective_hop);

    // Lookahead configuration: rolling mode parks the decode ~10 s ahead
    // of the play head; centered mode decodes the entire track.
    let lookahead_columns = if display_mode == SpectrogramDisplayMode::Centered {
        u64::MAX
    } else {
        let lookahead_seconds = std::env::var("FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(10.0);
        f64_to_u64_saturating(lookahead_seconds * cols_per_second)
    };

    // Rate throttle: 2× realtime for rolling, unlimited for centered.
    let decode_rate_limit = if display_mode == SpectrogramDisplayMode::Rolling {
        std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(2.0)
    } else {
        f64::INFINITY
    };

    // Pre-seek warmup: seek fft_size samples earlier so the STFT produces
    // its first output at exactly the requested position.
    let fft_warmup_seconds = usize_to_f64_approx(fft_size) / f64::from(effective_rate);
    let actual_seek_seconds = (start_seconds - fft_warmup_seconds).max(0.0);
    let warmup_columns = if start_seconds > fft_warmup_seconds {
        f64_to_u64_saturating((start_seconds - actual_seek_seconds) * cols_per_second)
    } else {
        0
    };

    if actual_seek_seconds > 0.0 {
        source.seek(actual_seek_seconds, native_sample_rate);
    }

    let start_column = f64_to_u64_saturating((start_seconds * cols_per_second).floor());

    let mut session = SpectrogramSessionState {
        track_token,
        gen,
        fft_size,
        hop_size,
        effective_hop,
        view_mode,
        display_mode,
        channel_count: actual_channel_count,
        bins_per_column,
        total_columns_estimate,
        effective_rate,
        cols_per_second,
        divisor,
        target_position_seconds: start_seconds,
        suppress_backward_seek: false,
        columns_produced: start_column,
        session_start_column: start_column,
        stfts: (0..actual_channel_count)
            .map(|_| StftComputer::new(fft_size, hop_size))
            .collect(),
        decimators: (0..actual_channel_count)
            .map(|_| SpectrogramDecimator::new(decimation_factor))
            .collect(),
        packet_counter: 0,
        chunk_buf: Vec::new(),
        chunk_columns: 0,
        chunk_start_index: start_column,
        target_chunk_columns: 1, // Start with 1 for fastest first-pixel
        total_covered_samples: 0,
        session_start_time: std::time::Instant::now(),
        post_reset_unthrottled_columns: post_reset_unthrottled_columns(display_mode),
        decode_rate_limit,
        lookahead_columns,
        #[cfg(feature = "gst")]
        gst_duration_requeried: false,
        pending_continue: None,
    };

    // Send initial metadata chunk (0 columns, carries estimates + transition semantics).
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token,
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

    let mut warmup_remaining = warmup_columns;

    loop {
        let result = session_decode_loop(
            &mut session,
            &mut source,
            &mut warmup_remaining,
            cmd_rx,
            event_tx,
            active_token,
            generation,
            columns_produced_out,
        );

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
                let compatible = opened.as_ref().is_some_and(|(_, sr, ch, _)| {
                    let eff_ch = match session.view_mode {
                        SpectrogramViewMode::Downmix => 1,
                        SpectrogramViewMode::PerChannel => *ch,
                    };
                    let divisor_u64 = u64::try_from(session.divisor).unwrap_or(1);
                    *sr == u64::from(session.effective_rate) * divisor_u64
                        && eff_ch == session.channel_count
                });
                if compatible {
                    let (new_source, _, _, new_est) = opened.unwrap();
                    source = new_source;
                    session.track_token = track_token;
                    session.total_columns_estimate = new_est;
                    warmup_remaining = 0;
                    session.target_chunk_columns = 1;
                    session.session_start_time = std::time::Instant::now();
                    session.session_start_column = session.columns_produced;
                    session.post_reset_unthrottled_columns =
                        post_reset_unthrottled_columns(session.display_mode);
                    session.suppress_backward_seek = true;
                    profile_eprintln!(
                        "[spect-worker] file switch OK, continuing session unthrottled_cols={}",
                        session.post_reset_unthrottled_columns,
                    );
                    continue; // re-enter session_decode_loop
                }
                // Incompatible or open failed — fall back to NewTrack.
                profile_eprintln!(
                    "[spect-worker] ContinueWithFile incompatible, falling back to NewTrack"
                );
                let gen = generation.fetch_add(1, Ordering::Relaxed) + 1;
                return Some(SpectrogramWorkerCommand::NewTrack {
                    track_token,
                    generation: gen,
                    path,
                    fft_size: session.fft_size,
                    hop_size: session.hop_size,
                    channel_count: session.channel_count,
                    start_seconds: 0.0,
                    emit_initial_reset: false,
                    clear_history_on_reset: false,
                    view_mode: session.view_mode,
                    display_mode: session.display_mode,
                });
            }
            Some(cmd) => {
                // Interrupted by NewTrack or Stop.
                profile_eprintln!(
                    "[spect-worker] SESSION END (interrupted) elapsed={:.1}ms cols_produced={}",
                    _start.elapsed().as_secs_f64() * 1000.0,
                    session.columns_produced.saturating_sub(start_column),
                );
                return Some(cmd);
            }
            None => {
                // Natural EOF with no pending continuation.
                profile_eprintln!(
                    "[spect-worker] SESSION END (EOF) elapsed={:.1}ms cols_produced={}",
                    _start.elapsed().as_secs_f64() * 1000.0,
                    session.columns_produced.saturating_sub(start_column),
                );
                return None;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Session decode loop
// ---------------------------------------------------------------------------

/// Inner decode loop. Returns `Some(cmd)` if interrupted, `None` on EOF.
#[allow(clippy::too_many_arguments, clippy::too_many_lines)]
fn session_decode_loop(
    session: &mut SpectrogramSessionState,
    source: &mut AudioFrameSource,
    warmup_remaining: &mut u64,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
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

        // 1. Check for commands via try_recv (drain pending, take latest position).
        if let Some(action) = process_session_commands(session, cmd_rx) {
            match action {
                SessionAction::Continue => {}
                SessionAction::FlushToken => {
                    session_flush_token(session, event_tx, columns_produced_out);
                }
                SessionAction::Stop => return Some(SpectrogramWorkerCommand::Stop),
                SessionAction::NewSession(cmd) => return Some(cmd),
                SessionAction::SeekRequired { position_seconds } => {
                    return handle_session_seek(
                        session,
                        position_seconds,
                        source,
                        warmup_remaining,
                        cmd_rx,
                        event_tx,
                        active_token,
                        generation,
                        columns_produced_out,
                    );
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
            match cmd_rx.recv() {
                Ok(cmd) => match handle_single_command(session, cmd) {
                    SessionAction::Continue => continue,
                    SessionAction::FlushToken => {
                        session_flush_token(session, event_tx, columns_produced_out);
                        continue;
                    }
                    SessionAction::Stop => {
                        return Some(SpectrogramWorkerCommand::Stop);
                    }
                    SessionAction::NewSession(cmd) => return Some(cmd),
                    SessionAction::SeekRequired { position_seconds } => {
                        return handle_session_seek(
                            session,
                            position_seconds,
                            source,
                            warmup_remaining,
                            cmd_rx,
                            event_tx,
                            active_token,
                            generation,
                            columns_produced_out,
                        );
                    }
                },
                Err(_) => return None,
            }
        }

        // 3. Rate throttle (rolling mode only, after the post-reset window).
        if !post_reset_window_active(session) && session.decode_rate_limit.is_finite() {
            let max_cols_per_wall_sec = session.decode_rate_limit * session.cols_per_second;
            if max_cols_per_wall_sec > 0.0 {
                let elapsed = session.session_start_time.elapsed().as_secs_f64();
                let cols_since_start = session
                    .columns_produced
                    .saturating_sub(session.session_start_column);
                #[allow(clippy::cast_precision_loss)]
                let expected_elapsed = cols_since_start as f64 / max_cols_per_wall_sec;
                if expected_elapsed > elapsed {
                    let sleep_dur = Duration::from_secs_f64((expected_elapsed - elapsed).min(0.5));
                    match cmd_rx.recv_timeout(sleep_dur) {
                        Ok(cmd) => match handle_single_command(session, cmd) {
                            SessionAction::Continue => continue,
                            SessionAction::FlushToken => {
                                session_flush_token(session, event_tx, columns_produced_out);
                                continue;
                            }
                            SessionAction::Stop => {
                                return Some(SpectrogramWorkerCommand::Stop);
                            }
                            SessionAction::NewSession(cmd) => {
                                return Some(cmd);
                            }
                            SessionAction::SeekRequired { position_seconds } => {
                                return handle_session_seek(
                                    session,
                                    position_seconds,
                                    source,
                                    warmup_remaining,
                                    cmd_rx,
                                    event_tx,
                                    active_token,
                                    generation,
                                    columns_produced_out,
                                );
                            }
                        },
                        Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
                        Err(_) => return None,
                    }
                }
            }
        }

        // 4. Decode next batch of audio frames.
        let audio = match source.next_frames() {
            Some(af) if af.frames == 0 => continue, // GStreamer timeout, no data yet
            Some(af) => af,
            None => break, // EOF
        };
        session.packet_counter += 1;

        let frames = audio.frames;
        let effective_frames = frames / session.divisor;

        let per_channel = deinterleave_samples(
            &audio.samples,
            frames,
            audio.channels,
            session.channel_count,
            session.divisor,
            effective_frames,
            session.view_mode,
        );

        #[allow(clippy::cast_possible_truncation)]
        {
            session.total_covered_samples += effective_frames as u64;
        }

        for (ch, channel_samples) in per_channel.iter().enumerate() {
            if let Some(stft) = session.stfts.get_mut(ch) {
                stft.enqueue_samples(channel_samples, session.effective_rate);
            }
        }

        // 5. Drain STFT rows and emit chunks.
        session_drain_stft_rows(session, warmup_remaining, event_tx, columns_produced_out);

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

    // EOF reached — flush remaining columns so the UI has all decoded
    // data.  If a ContinueWithFile is pending, return it immediately
    // so run_spectrogram_session can switch to the next file.
    session_flush_chunk(session, event_tx, columns_produced_out);
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
        match cmd_rx.recv() {
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
                SessionAction::Stop => return Some(SpectrogramWorkerCommand::Stop),
                SessionAction::NewSession(cmd) => return Some(cmd),
                SessionAction::SeekRequired { position_seconds } => {
                    return handle_session_seek(
                        session,
                        position_seconds,
                        source,
                        warmup_remaining,
                        cmd_rx,
                        event_tx,
                        active_token,
                        generation,
                        columns_produced_out,
                    );
                }
            },
            Err(_) => return None,
        }
    }
}

// ---------------------------------------------------------------------------
// Command processing
// ---------------------------------------------------------------------------

fn process_session_commands(
    session: &mut SpectrogramSessionState,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
) -> Option<SessionAction> {
    let mut latest_position: Option<f64> = None;
    let mut latest_seek: Option<f64> = None;
    let mut needs_flush_token = false;

    // Drain all pending commands, take the latest seek/position update.
    while let Ok(cmd) = cmd_rx.try_recv() {
        match cmd {
            SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
                latest_position = Some(position_seconds);
            }
            SpectrogramWorkerCommand::Seek { position_seconds } => {
                latest_seek = Some(position_seconds);
            }
            SpectrogramWorkerCommand::NewTrack { .. } => {
                return Some(SessionAction::NewSession(cmd));
            }
            SpectrogramWorkerCommand::SetDisplayMode(mode) => {
                apply_display_mode(session, mode);
            }
            SpectrogramWorkerCommand::ContinueWithFile { path, track_token } => {
                session.pending_continue = Some((path, track_token));
            }
            SpectrogramWorkerCommand::UpdateTrackToken { track_token } => {
                if let Some((_, ref mut pending_token)) = session.pending_continue {
                    *pending_token = track_token;
                } else {
                    session.track_token = track_token;
                    needs_flush_token = true;
                }
            }
            SpectrogramWorkerCommand::CancelPendingContinue => {
                session.pending_continue = None;
            }
            SpectrogramWorkerCommand::Stop => {
                return Some(SessionAction::Stop);
            }
        }
    }

    // FlushToken takes priority over position-only results — the UI
    // needs the metadata chunk before the next position update lands.
    if needs_flush_token {
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
            if let Some((_, ref mut pending_token)) = session.pending_continue {
                // Continuation not yet consumed — update pending token only.
                // Old-track columns keep the old token until EOF.
                *pending_token = track_token;
                SessionAction::Continue
            } else {
                // Continuation already consumed — worker is on the new file.
                // FlushToken tells the caller to emit a 0-column metadata
                // chunk immediately so the UI gapless handler fires without
                // waiting for the next rate-limited data chunk.
                session.track_token = track_token;
                SessionAction::FlushToken
            }
        }
        SpectrogramWorkerCommand::CancelPendingContinue => {
            session.pending_continue = None;
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::Stop => SessionAction::Stop,
    }
}

// ---------------------------------------------------------------------------
// Seek handling
// ---------------------------------------------------------------------------

/// Handles a seek within the current session by resetting STFT state and
/// repositioning the file reader.
#[allow(clippy::too_many_arguments)]
fn handle_session_seek(
    session: &mut SpectrogramSessionState,
    position_seconds: f64,
    source: &mut AudioFrameSource,
    warmup_remaining: &mut u64,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
    columns_produced_out: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    profile_eprintln!("[spect-worker] SEEK to {position_seconds:.2}s");

    // Drop any partially accumulated pre-seek chunk. Emitting it after a seek
    // makes the UI paint old-timeline columns just before the reset arrives.
    session.chunk_buf.clear();
    session.chunk_columns = 0;

    // Pre-seek warmup.
    let fft_warmup_seconds =
        usize_to_f64_approx(session.fft_size) / f64::from(session.effective_rate);
    let actual_seek_seconds = (position_seconds - fft_warmup_seconds).max(0.0);

    // Compute native rate from effective rate × divisor.
    let native_rate =
        u64::from(session.effective_rate) * u64::try_from(session.divisor).unwrap_or(1);
    source.seek(actual_seek_seconds, native_rate);

    // Reset STFT state.
    let decimation_factor = decimation_factor_for_hop(session.hop_size);
    session.stfts = (0..session.channel_count)
        .map(|_| StftComputer::new(session.fft_size, session.hop_size))
        .collect();
    session.decimators = (0..session.channel_count)
        .map(|_| SpectrogramDecimator::new(decimation_factor))
        .collect();

    let new_column = f64_to_u64_saturating((position_seconds * session.cols_per_second).floor());
    session.columns_produced = new_column;
    session.session_start_column = new_column;
    session.chunk_buf.clear();
    session.chunk_columns = 0;
    session.chunk_start_index = new_column;
    session.target_chunk_columns = 1; // Fast first-pixel after seek
    session.total_covered_samples = 0;
    session.session_start_time = std::time::Instant::now();
    session.post_reset_unthrottled_columns = post_reset_unthrottled_columns(session.display_mode);
    session.target_position_seconds = position_seconds;

    // Emit buffer_reset chunk.
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: session.track_token,
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

    *warmup_remaining = if position_seconds > fft_warmup_seconds {
        f64_to_u64_saturating((position_seconds - actual_seek_seconds) * session.cols_per_second)
    } else {
        0
    };

    // Continue decode loop from new position.
    session_decode_loop(
        session,
        source,
        warmup_remaining,
        cmd_rx,
        event_tx,
        active_token,
        generation,
        columns_produced_out,
    )
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

/// Apply a display-mode change to a live session: update rate limit
/// and lookahead so centered mode decodes the full track immediately.
fn apply_display_mode(session: &mut SpectrogramSessionState, mode: SpectrogramDisplayMode) {
    session.display_mode = mode;
    if mode == SpectrogramDisplayMode::Rolling {
        session.decode_rate_limit = std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(2.0);
        let lookahead_seconds = std::env::var("FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS")
            .ok()
            .and_then(|s| s.parse::<f64>().ok())
            .unwrap_or(10.0);
        session.lookahead_columns =
            f64_to_u64_saturating(lookahead_seconds * session.cols_per_second);
    } else {
        session.decode_rate_limit = f64::INFINITY;
        session.lookahead_columns = u64::MAX;
    }
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

// ---------------------------------------------------------------------------
// STFT row draining / chunk emission
// ---------------------------------------------------------------------------

fn session_drain_stft_rows(
    session: &mut SpectrogramSessionState,
    warmup_remaining: &mut u64,
    event_tx: &Sender<AnalysisEvent>,
    columns_produced_out: &AtomicU64,
) {
    loop {
        let mut rows: Vec<Vec<f32>> = Vec::with_capacity(session.channel_count);
        let mut all_have_row = true;
        for stft in &mut session.stfts {
            let row = stft.take_rows(1);
            if row.is_empty() {
                all_have_row = false;
                break;
            }
            rows.push(row.into_iter().next().unwrap());
        }
        if !all_have_row {
            break;
        }

        // Push through decimators.
        let mut decimated_rows: Vec<Option<Vec<f32>>> = Vec::with_capacity(session.channel_count);
        for (ch, row) in rows.into_iter().enumerate() {
            if let Some(dec) = session.decimators.get_mut(ch) {
                decimated_rows.push(dec.push(row));
            } else {
                decimated_rows.push(Some(row));
            }
        }

        let all_decimated = decimated_rows.iter().all(Option::is_some);
        if !all_decimated {
            continue;
        }

        // Skip warmup columns (pre-seek: feed STFT without emitting).
        if *warmup_remaining > 0 {
            *warmup_remaining -= 1;
            continue;
        }

        // Quantize and append to chunk buffer.
        for maybe_row in &decimated_rows {
            let row = maybe_row.as_ref().unwrap();
            for &v in row.iter().take(session.bins_per_column) {
                session
                    .chunk_buf
                    .push(precomputed_to_u8_spectrum(v, session.fft_size));
            }
            if row.len() < session.bins_per_column {
                session.chunk_buf.extend(std::iter::repeat_n(
                    0u8,
                    session.bins_per_column - row.len(),
                ));
            }
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
            session.target_chunk_columns =
                next_target_chunk_columns(session.target_chunk_columns, session.display_mode);
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
fn maybe_update_columns_estimate(session: &mut SpectrogramSessionState, source: &AudioFrameSource) {
    let estimate = u64::from(session.total_columns_estimate);
    let produced = session.columns_produced;

    // Attempt a GStreamer duration re-query early in the session.
    #[cfg(feature = "gst")]
    if !session.gst_duration_requeried && session.packet_counter >= 20 {
        session.gst_duration_requeried = true;
        if let Some(ns) = source.query_duration_ns() {
            let rate =
                u64::from(session.effective_rate) * u64::try_from(session.divisor).unwrap_or(1);
            let total_frames = ns * rate / 1_000_000_000;
            let divisor = waveform_sample_rate_divisor(rate);
            let effective = total_frames / divisor;
            let new_est =
                u32::try_from(((effective / (REFERENCE_HOP as u64)) + 64).min(u64::from(u32::MAX)))
                    .unwrap_or(u32::MAX);
            if new_est > session.total_columns_estimate {
                profile_eprintln!(
                    "[spect-worker] duration re-query OK, est_cols {} → {}",
                    session.total_columns_estimate,
                    new_est,
                );
                session.total_columns_estimate = new_est;
                return;
            }
        }
    }

    // Suppress the unused-variable warning when GStreamer is disabled.
    let _ = source;

    // Safety net: if we're within 25% of the estimate, double it.
    // This ensures the UI ring buffer grows before columns get evicted.
    let threshold = estimate.saturating_sub(estimate / 4);
    if produced >= threshold && estimate < u64::from(u32::MAX / 2) {
        let new_est = session.total_columns_estimate.saturating_mul(2);
        profile_eprintln!(
            "[spect-worker] columns approaching estimate, est_cols {} → {} (produced={})",
            session.total_columns_estimate,
            new_est,
            produced,
        );
        session.total_columns_estimate = new_est;
    }
}

fn session_flush_chunk(
    session: &mut SpectrogramSessionState,
    event_tx: &Sender<AnalysisEvent>,
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
    event_tx: &Sender<AnalysisEvent>,
    columns_produced_out: &AtomicU64,
) {
    // Flush any partially accumulated data so it carries the new token.
    session_flush_chunk(session, event_tx, columns_produced_out);

    // Emit a 0-column metadata chunk with the new token.
    let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
        PrecomputedSpectrogramChunk {
            track_token: session.track_token,
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
    event_tx: &Sender<AnalysisEvent>,
    columns_produced_out: &AtomicU64,
) {
    if session.display_mode == SpectrogramDisplayMode::Rolling {
        return;
    }
    session_flush_chunk(session, event_tx, columns_produced_out);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;
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

    #[test]
    fn explicit_seek_command_forces_seek_even_inside_lookahead() {
        let mut session = SpectrogramSessionState {
            track_token: 1,
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 256,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
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
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 400,
            session_start_column: 400,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 400,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: post_reset_unthrottled_columns(
                SpectrogramDisplayMode::Rolling,
            ),
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
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
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
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
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 64,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
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
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Centered,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 320,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: vec![1, 2, 3, 4],
            chunk_columns: 1,
            chunk_start_index: 256,
            target_chunk_columns: 64,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: f64::INFINITY,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
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

    /// Helper: creates a minimal `SpectrogramSessionState` for command tests.
    fn make_test_session() -> SpectrogramSessionState {
        SpectrogramSessionState {
            track_token: 1,
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            effective_hop: 1_024,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            suppress_backward_seek: false,
            columns_produced: 256,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_unthrottled_columns: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            #[cfg(feature = "gst")]
            gst_duration_requeried: false,
            pending_continue: None,
        }
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
            channel_count: 1,
            start_seconds: 0.0,
            emit_initial_reset: true,
            clear_history_on_reset: true,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
        })
        .unwrap();

        let action = process_session_commands(&mut session, &rx);
        // NewTrack must take priority.
        assert!(matches!(action, Some(SessionAction::NewSession(_))));
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

    // ---------------------------------------------------------------
    // Centered mode behavior tests
    // ---------------------------------------------------------------

    #[test]
    fn apply_display_mode_centered_sets_unlimited_rate_and_lookahead() {
        let mut session = make_test_session();
        session.display_mode = SpectrogramDisplayMode::Rolling;
        session.decode_rate_limit = 2.0;
        session.lookahead_columns = 512;

        assert!(session.decode_rate_limit.is_finite());
        assert_eq!(session.lookahead_columns, 512);

        apply_display_mode(&mut session, SpectrogramDisplayMode::Centered);

        assert!(session.decode_rate_limit.is_infinite());
        assert_eq!(session.lookahead_columns, u64::MAX);
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
    fn centered_staging_chunk_indices_are_zero_based_and_monotonic() {
        // Exercises the same STFT -> decimator -> chunk indexing logic used
        // by centered_staging_decode, verifying 0-based start indices and
        // placeholder token 0.
        let fft_size = 512;
        let hop_size = 128;
        let bins_per_column = (fft_size / 2) + 1;
        let decimation_factor = decimation_factor_for_hop(hop_size);

        let mut stft = StftComputer::new(fft_size, hop_size);
        let mut decimator = SpectrogramDecimator::new(decimation_factor);

        // Feed a 440 Hz sine wave, enough for several output columns.
        let sample_rate = 48_000u32;
        let samples: Vec<f32> = (0u32..32_768)
            .map(|i| (2.0 * std::f32::consts::PI * 440.0 * (i as f32 / sample_rate as f32)).sin())
            .collect();
        stft.enqueue_samples(&samples, sample_rate);

        let mut columns_produced: u64 = 0;
        let mut chunk_start_index: u64 = 0;
        let mut chunk_columns: u16 = 0;
        let mut target_chunk_columns: u16 = 1;
        let mut chunks: Vec<PrecomputedSpectrogramChunk> = Vec::new();

        loop {
            let row = stft.take_rows(1);
            if row.is_empty() {
                break;
            }
            let row = row.into_iter().next().unwrap();
            let maybe_dec = decimator.push(row);
            if maybe_dec.is_none() {
                continue;
            }

            chunk_columns += 1;
            columns_produced += 1;

            if chunk_columns >= target_chunk_columns {
                chunks.push(PrecomputedSpectrogramChunk {
                    track_token: 0,
                    columns_u8: vec![0u8; usize::from(chunk_columns) * bins_per_column],
                    bins_per_column: clamp_to_u16(bins_per_column),
                    column_count: chunk_columns,
                    channel_count: 1,
                    start_column_index: u64_to_u32_saturating(chunk_start_index),
                    total_columns_estimate: 1000,
                    sample_rate_hz: sample_rate,
                    hop_size: clamp_to_u16(hop_size * decimation_factor),
                    coverage_seconds: 0.0,
                    complete: false,
                    buffer_reset: false,
                    clear_history: false,
                });
                chunk_start_index = columns_produced;
                chunk_columns = 0;
                target_chunk_columns = next_target_chunk_columns(
                    target_chunk_columns,
                    SpectrogramDisplayMode::Centered,
                );
            }
        }

        assert!(chunks.len() >= 2, "should produce multiple chunks");
        // First chunk starts at index 0.
        assert_eq!(chunks[0].start_column_index, 0);
        assert_eq!(chunks[0].track_token, 0);
        // Indices are monotonically increasing.
        for i in 1..chunks.len() {
            assert!(chunks[i].start_column_index > chunks[i - 1].start_column_index);
            assert_eq!(chunks[i].track_token, 0);
        }
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
            sample_buf: None,
        }
    }

    #[test]
    #[cfg(not(feature = "gst"))]
    fn columns_estimate_doubles_when_approaching_limit() {
        let mut session = make_test_session();
        session.total_columns_estimate = 1000;
        session.columns_produced = 600; // below 75% threshold
        let source = make_dummy_source();

        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(
            session.total_columns_estimate, 1000,
            "should not grow below 75%"
        );

        session.columns_produced = 750; // exactly at 75% threshold
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(session.total_columns_estimate, 2000, "should double at 75%");

        // After doubling, 750 is well below the new 75% threshold (1500).
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(session.total_columns_estimate, 2000, "should stay stable");
    }

    #[test]
    #[cfg(not(feature = "gst"))]
    fn columns_estimate_doubles_again_when_still_producing() {
        let mut session = make_test_session();
        session.total_columns_estimate = 1000;
        let source = make_dummy_source();

        // Cross first threshold.
        session.columns_produced = 800;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(session.total_columns_estimate, 2000);

        // Cross second threshold (75% of 2000 = 1500).
        session.columns_produced = 1600;
        maybe_update_columns_estimate(&mut session, &source);
        assert_eq!(session.total_columns_estimate, 4000);
    }
}
