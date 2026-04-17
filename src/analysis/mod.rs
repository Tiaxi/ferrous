// SPDX-License-Identifier: GPL-3.0-or-later

mod cache;
mod decoders;
mod fft;
#[cfg(feature = "gst")]
mod gst_waveform;
mod session;

use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc,
};
use std::time::{Duration, UNIX_EPOCH};

use crossbeam_channel::{select, unbounded, Receiver, Sender};
use rusqlite::Connection;
use symphonia::core::audio::SampleBuffer;

use cache::{
    insert_waveform_cache_entry, load_waveform_from_db, open_waveform_cache_db,
    persist_waveform_to_db, prune_persistent_waveform_cache, touch_waveform_cache_lru,
    usize_to_u64, WaveformCacheEntry, WaveformSourceStamp, PERSISTENT_WAVEFORM_CACHE_MAX_ROWS,
    PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL,
};
use decoders::{open_symphonia_file, SymphoniaFile};
use fft::{
    ensure_sample_buffer, peak_across_channels, waveform_sample_rate_divisor, WaveformAccumulator,
};
#[cfg(feature = "gst")]
use gst_waveform::decode_waveform_peaks_stream_gst;
use session::{
    spawn_centered_staging_worker, spawn_spectrogram_decode_worker, SpectrogramWorkerCommand,
    SpectrogramWorkerHandles,
};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;
#[cfg(feature = "gst")]
use crate::raw_audio::same_surround_extension;

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

#[derive(Debug, Clone)]
pub enum AnalysisCommand {
    SetTrack {
        path: PathBuf,
        reset_spectrogram: bool,
        track_token: u64,
        /// When true (same-format gapless), skip PCM label re-init so the
        /// spectrogram/channel state stays continuous.
        gapless: bool,
    },
    SetTrackToken(u64),
    ResetSpectrogram,
    SetSampleRate(u32),
    SetFftSize(usize),
    SetSpectrogramZoomLevel(f32),
    SetSpectrogramWidgetWidth(u32),
    SetSpectrogramViewMode(SpectrogramViewMode),
    SetSpectrogramDisplayMode(SpectrogramDisplayMode),
    RestartCurrentTrack {
        position_seconds: f64,
        clear_history: bool,
    },
    PositionUpdate(f64),
    SeekPosition(f64),
    WaveformProgress {
        track_token: u64,
        peaks: Vec<f32>,
        coverage_seconds: f32,
        complete: bool,
        done: bool,
    },
    /// Prepare staged spectrogram data for a likely gapless successor.
    /// Sent from about-to-finish (~2 s before track end, same-format only).
    /// The analysis runtime checks format compatibility and spawns an
    /// off-screen staging thread if the candidate matches the active session.
    PrepareGaplessContinuation {
        path: PathBuf,
    },
    /// Cancel any in-progress staged continuation and restart the
    /// spectrogram session.  Used when the current track stays playing
    /// but the gapless prediction is invalid (seek near EOF, queue
    /// mutation).  The restart recovers from possible wrong-file decode.
    CancelStagedContinuation,
    /// Clear any in-progress staged continuation without restarting.
    /// Used when a `SetTrack` or stop follows immediately, superseding
    /// the worker session via generation or stopping playback.
    ClearStagedContinuation,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SpectrogramViewMode {
    #[default]
    Downmix,
    PerChannel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SpectrogramDisplayMode {
    #[default]
    Rolling,
    Centered,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SpectrogramChannelLabel {
    #[default]
    Mono,
    FrontLeft,
    FrontRight,
    FrontCenter,
    Lfe,
    SideLeft,
    SideRight,
    RearLeft,
    RearRight,
    RearCenter,
    Unknown,
}

#[derive(Debug, Clone, Default)]
pub struct AnalysisPcmChunk {
    pub samples: Vec<f32>,
    pub channel_labels: Vec<SpectrogramChannelLabel>,
    pub track_token: u64,
}

#[derive(Debug, Clone, Default)]
pub struct AnalysisSpectrogramChannel {
    pub label: SpectrogramChannelLabel,
    pub rows: Vec<Vec<f32>>,
}

#[derive(Debug, Clone, Default)]
pub struct AnalysisSnapshot {
    pub waveform_peaks: Vec<f32>,
    pub waveform_coverage_seconds: f32,
    pub waveform_complete: bool,
    pub sample_rate_hz: u32,
}

#[derive(Debug, Clone)]
pub struct PrecomputedSpectrogramChunk {
    pub track_token: u64,
    /// Packed column data: `column_count` × `channel_count` × `bins_per_column` bytes.
    /// Within each column, all channels are contiguous.
    pub columns_u8: Vec<u8>,
    pub bins_per_column: u16,
    pub column_count: u16,
    pub channel_count: u8,
    pub start_column_index: u32,
    pub total_columns_estimate: u32,
    pub sample_rate_hz: u32,
    pub hop_size: u16,
    pub coverage_seconds: f32,
    pub complete: bool,
    /// When true, the C++ ring buffer should be cleared and the epoch reset.
    /// Emitted after a hard seek outside the buffered range.
    pub buffer_reset: bool,
    /// When true, the UI should discard previous-track history instead of
    /// preserving rolling continuity across the reset handoff.
    pub clear_history: bool,
}

#[derive(Debug, Clone)]
pub enum AnalysisEvent {
    Snapshot(AnalysisSnapshot),
    PrecomputedSpectrogramChunk(PrecomputedSpectrogramChunk),
}

pub struct AnalysisEngine {
    tx: Sender<AnalysisCommand>,
    pcm_tx: Sender<AnalysisPcmChunk>,
}

const REFERENCE_HOP: usize = 1024;

/// Compute the STFT hop size for a given zoom level.
/// Zoom > 1.0: smaller hop (finer temporal resolution).
/// Zoom <= 1.0: FFT-derived hop (normal resolution).
///
/// The zoom hop is derived from `REFERENCE_HOP` (not `fft_size/8`) because
/// zoom is relative to the *output* column rate, which is always
/// normalized to `REFERENCE_HOP` by the decimation system.  At zoom=2x
/// we need `effective_hop = REFERENCE_HOP/2`, so with decimation bypassed
/// the STFT hop must equal `REFERENCE_HOP/2`.  The STFT hop may be larger
/// than the unzoomed `fft_size/8` hop -- this is correct because the
/// unzoomed path decimates many overlapping STFT rows into one output
/// column, while the zoomed path keeps every STFT row individually.
fn zoom_hop_size(fft_size: usize, zoom_level: f32) -> usize {
    if zoom_level > 1.0 {
        // REFERENCE_HOP is 1024, well within f64 precision.
        #[allow(clippy::cast_precision_loss)]
        let hop_f64 = REFERENCE_HOP as f64;
        // Result is clamped to [64, 1024] below, so truncation and sign loss are safe.
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
        let raw = (hop_f64 / f64::from(zoom_level)).round() as usize;
        raw.clamp(64, REFERENCE_HOP)
    } else {
        (fft_size / 8).max(64)
    }
}

#[derive(Debug, Clone)]
struct WaveformDecodeJob {
    track_token: u64,
    path: PathBuf,
}

// The bools in this struct are independent flags with distinct semantics;
// collapsing them into enums or a bitset would reduce clarity without benefit.
#[allow(clippy::struct_excessive_bools)]
struct AnalysisRuntimeState {
    snapshot: AnalysisSnapshot,
    pending_channels: Vec<AnalysisSpectrogramChannel>,
    waveform_dirty: bool,
    last_emit: std::time::Instant,
    fft_size: usize,
    hop_size: usize,
    zoom_level: f32,
    /// Actual pixel width of the spectrogram widget, used to compute
    /// the decode margin and lookahead dynamically.  Updated from the
    /// frontend on resize.
    spectrogram_widget_width: u32,
    /// Maximum widget width observed so far.  The lookahead park
    /// threshold is sized against this rather than the current width so
    /// a fullscreen toggle that skips the session restart (because the
    /// zoom level hasn't changed) still has enough decoded columns to
    /// fill the larger display.  Cheap memory trade: at max zoom a 4K
    /// lookahead costs ~12 MB extra vs a 1080p lookahead.
    spectrogram_max_widget_width: u32,
    spectrogram_view_mode: SpectrogramViewMode,
    active_track_token: u64,
    active_track_path: Option<PathBuf>,
    active_track_stamp: Option<WaveformSourceStamp>,
    /// Display mode (Rolling/Centered) — forwarded to decode worker for rate throttle.
    display_mode: SpectrogramDisplayMode,
    waveform_cache: HashMap<PathBuf, WaveformCacheEntry>,
    waveform_cache_lru: VecDeque<PathBuf>,
    waveform_db: Option<Connection>,
    waveform_db_writes_since_prune: usize,
    pcm_fifo: VecDeque<f32>,
    pcm_labels: Vec<SpectrogramChannelLabel>,
    /// Set on track changes; disables transient channel-reduction
    /// suppression for the first label change so that legitimate
    /// cross-track format changes (e.g. 5.1 → stereo) are accepted.
    pcm_labels_pending_init: bool,
    active_pcm_track_token: u64,
    /// Cumulative offset added to playback positions before forwarding
    /// to the spectrogram worker.  Translates the new track's position
    /// (which resets to 0) into the worker's continuous coordinate
    /// space so `PositionUpdate(0.0)` doesn't trigger a backward seek.
    spectrogram_position_offset: f64,
    /// Last raw position forwarded to the spectrogram worker, used to
    /// compute the offset at gapless transitions.
    last_spectrogram_position: f64,
    /// Start position (seconds) of the current spectrogram decode session.
    /// Used to determine whether a seek falls within the already-decoded
    /// window, avoiding unnecessary session restarts.
    spectrogram_session_start: f64,
    /// The margin (in seconds) that was used when the current session
    /// started.  Used in the seek-within-window check so that widget
    /// width changes (e.g. entering/exiting fullscreen) don't shrink
    /// the effective window below the actual decode extent.
    spectrogram_session_margin: f64,
    /// Suppresses the next `PositionUpdate` forwarded to the spectrogram
    /// worker after a centered-mode seek.  Without this, the position
    /// update from the playback snapshot can reach the worker before the
    /// `NewTrack` command, causing the old session to seek to the new
    /// position and produce a brief flash of content at the playhead
    /// before the new session replaces it.
    suppress_next_spectrogram_position_update: bool,
    /// Compatibility params for the active spectrogram worker session.
    /// Updated on every session start and track change so the staging
    /// preflight can compare candidate files against the live session.
    active_session_effective_rate: u32,
    active_session_channel_count: usize,
    active_session_divisor: usize,
    /// Path of the next track for which an early `ContinueWithFile` was
    /// sent to the worker.  Consumed at commit time (`handle_track_change`).
    staged_continuation_path: Option<PathBuf>,
    /// Receiver for pre-decoded centered-mode chunks from the staging
    /// thread.  Each chunk has `track_token: 0` (placeholder).
    staged_centered_rx: Option<Receiver<PrecomputedSpectrogramChunk>>,
    /// Stop flag shared with the staging thread.
    staged_centered_stop: Option<Arc<AtomicBool>>,
    /// Join handle for the staging thread -- joined before draining
    /// to guarantee all flushed output is in the channel.
    staged_centered_handle: Option<std::thread::JoinHandle<()>>,
    /// Path of the file being pre-decoded for centered gapless.
    staged_centered_path: Option<PathBuf>,
    profile_enabled: bool,
    prof_last: std::time::Instant,
    prof_pcm: usize,
    prof_rows: usize,
    prof_ticks: usize,
    prof_in_samples: usize,
    prof_out_samples: usize,
}

impl AnalysisEngine {
    #[must_use]
    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    pub fn new() -> (Self, Receiver<AnalysisEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<AnalysisCommand>();
        // Bounded PCM queue to prevent unbounded backlog under decode bursts.
        let (pcm_tx, pcm_rx) = crossbeam_channel::bounded::<AnalysisPcmChunk>(12);
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();

        let waveform_tx = cmd_tx.clone();
        let (waveform_job_tx, waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = Arc::new(AtomicU64::new(0));
        spawn_waveform_decode_worker(
            waveform_job_rx,
            waveform_tx,
            Arc::clone(&waveform_decode_active_token),
        );

        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let spectrogram_decode_generation = Arc::new(AtomicU64::new(0));
        let worker_columns_produced = Arc::new(AtomicU64::new(0));
        spawn_spectrogram_decode_worker(
            spectrogram_cmd_rx,
            event_tx.clone(),
            Arc::clone(&waveform_decode_active_token),
            Arc::clone(&spectrogram_decode_generation),
            Arc::clone(&worker_columns_produced),
        );

        spawn_analysis_worker(
            cmd_rx,
            pcm_rx,
            event_tx,
            waveform_job_tx,
            waveform_decode_active_token,
            SpectrogramWorkerHandles {
                cmd_tx: spectrogram_cmd_tx,
                decode_generation: spectrogram_decode_generation,
                columns_produced: worker_columns_produced,
            },
        );

        (Self { tx: cmd_tx, pcm_tx }, event_rx)
    }

    pub fn command(&self, cmd: AnalysisCommand) {
        let _ = self.tx.send(cmd);
    }

    #[must_use]
    pub fn sender(&self) -> Sender<AnalysisCommand> {
        self.tx.clone()
    }

    #[must_use]
    pub fn pcm_sender(&self) -> Sender<AnalysisPcmChunk> {
        self.pcm_tx.clone()
    }
}

struct AnalysisContext<'a> {
    event_tx: &'a Sender<AnalysisEvent>,
    waveform_job_tx: &'a Sender<WaveformDecodeJob>,
    waveform_decode_active_token: &'a AtomicU64,
    spectrogram_cmd_tx: &'a Sender<SpectrogramWorkerCommand>,
    spectrogram_decode_generation: &'a AtomicU64,
    /// Absolute column count the spectrogram decode worker has produced
    /// for its current session.  Read by the analysis thread when a
    /// gapless transition commits so it can emit a finalize chunk for
    /// the outgoing track with the true decoded extent.
    spectrogram_decode_columns_produced: &'a AtomicU64,
}

impl AnalysisRuntimeState {
    fn new() -> Self {
        Self {
            snapshot: AnalysisSnapshot {
                sample_rate_hz: 48_000,
                ..AnalysisSnapshot::default()
            },
            pending_channels: Vec::new(),
            waveform_dirty: false,
            last_emit: std::time::Instant::now(),
            fft_size: 8192,
            hop_size: 1024,
            zoom_level: 1.0,
            spectrogram_widget_width: 1920,
            spectrogram_max_widget_width: 1920,
            spectrogram_view_mode: SpectrogramViewMode::Downmix,
            active_track_token: 0,
            active_track_path: None,
            active_track_stamp: None,
            display_mode: SpectrogramDisplayMode::Rolling,
            waveform_cache: HashMap::new(),
            waveform_cache_lru: VecDeque::new(),
            waveform_db: open_waveform_cache_db().ok(),
            waveform_db_writes_since_prune: 0,
            pcm_fifo: VecDeque::with_capacity(48_000),
            pcm_labels: vec![SpectrogramChannelLabel::Mono],
            pcm_labels_pending_init: true,
            active_pcm_track_token: 0,
            profile_enabled: cfg!(feature = "profiling-logs")
                && std::env::var_os("FERROUS_PROFILE").is_some(),
            prof_last: std::time::Instant::now(),
            prof_pcm: 0,
            prof_rows: 0,
            prof_ticks: 0,
            prof_in_samples: 0,
            prof_out_samples: 0,
            spectrogram_position_offset: 0.0,
            last_spectrogram_position: 0.0,
            spectrogram_session_start: 0.0,
            spectrogram_session_margin: 30.0,
            suppress_next_spectrogram_position_update: false,
            active_session_effective_rate: 0,
            active_session_channel_count: 0,
            active_session_divisor: 1,
            staged_continuation_path: None,
            staged_centered_rx: None,
            staged_centered_stop: None,
            staged_centered_handle: None,
            staged_centered_path: None,
        }
    }

    #[allow(clippy::too_many_lines)]
    fn handle_command(&mut self, cmd: AnalysisCommand, ctx: &AnalysisContext<'_>) {
        match cmd {
            AnalysisCommand::SetTrack {
                ref path,
                reset_spectrogram,
                track_token,
                gapless,
            } => {
                profile_eprintln!(
                    "[analysis] SetTrack path={} token={track_token} gapless={gapless} reset_spec={reset_spectrogram}",
                    path.file_name().unwrap_or_default().to_string_lossy(),
                );
                self.handle_track_change(
                    path.clone(),
                    reset_spectrogram,
                    gapless,
                    track_token,
                    ctx,
                );
            }
            AnalysisCommand::SetTrackToken(track_token) => {
                self.active_pcm_track_token = track_token;
                // Don't set pcm_labels_pending_init here — the subsequent
                // SetTrack command will set it when appropriate (skipped
                // for gapless transitions to keep channel state continuous).
            }
            AnalysisCommand::ResetSpectrogram => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
            }
            AnalysisCommand::SetSampleRate(rate) => {
                if rate > 0 {
                    self.snapshot.sample_rate_hz = rate;
                    self.emit_snapshot(ctx.event_tx, true);
                }
            }
            AnalysisCommand::SetFftSize(size) => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                let fft = size.clamp(512, 8192).next_power_of_two();
                let hop = zoom_hop_size(fft, self.zoom_level);
                self.fft_size = fft;
                self.hop_size = hop;
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(self.centered_start_seconds(), true, true, ctx);
            }
            AnalysisCommand::SetSpectrogramWidgetWidth(width) => {
                let w = width.max(320);
                self.spectrogram_widget_width = w;
                if w > self.spectrogram_max_widget_width {
                    self.spectrogram_max_widget_width = w;
                    // Propagate to the running session so its
                    // centered-mode lookahead park threshold grows to
                    // fill the enlarged display.  Without this, a
                    // fullscreen toggle that skips the session restart
                    // (zoom unchanged) leaves the decoder parked at the
                    // old lookahead and the right portion of the new
                    // window stays black until playback catches up.
                    let _ = ctx
                        .spectrogram_cmd_tx
                        .send(SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: w });
                }
            }
            AnalysisCommand::SetSpectrogramZoomLevel(level) => {
                let level = level.clamp(0.05, 16.0);
                // The Qt side fires this on width changes too (via the
                // widthSettleTimer after a fullscreen toggle) with the
                // unchanged zoom.  Restarting the session in that case
                // wipes the ring and flashes black for ~100 ms while
                // the decoder catches up — but the existing session's
                // data is still valid at the same hop, and the Qt-side
                // ring realloc + canvas rebuild handle the width change
                // on their own.  Skip the restart when the zoom level
                // hasn't actually changed.
                if (self.zoom_level - level).abs() < 0.001 {
                    return;
                }
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                self.zoom_level = level;
                self.hop_size = zoom_hop_size(self.fft_size, self.zoom_level);
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                let start = self.centered_start_seconds();
                self.start_spectrogram_session(start, true, true, ctx);
            }
            AnalysisCommand::SetSpectrogramViewMode(view_mode) => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                profile_eprintln!("[analysis] SetSpectrogramViewMode({view_mode:?})");
                self.spectrogram_view_mode = view_mode;
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(self.centered_start_seconds(), true, true, ctx);
            }
            AnalysisCommand::SetSpectrogramDisplayMode(mode) => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                profile_eprintln!("[analysis] SetSpectrogramDisplayMode({mode:?})");
                self.display_mode = mode;
                let _ = ctx
                    .spectrogram_cmd_tx
                    .send(SpectrogramWorkerCommand::SetDisplayMode(mode));
            }
            AnalysisCommand::RestartCurrentTrack {
                position_seconds,
                clear_history,
            } => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                profile_eprintln!(
                    "[analysis] RestartCurrentTrack pos={position_seconds:.2} clear_history={clear_history}"
                );
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(position_seconds, true, clear_history, ctx);
            }
            AnalysisCommand::PositionUpdate(position_seconds) => {
                profile_eprintln!("[analysis] PositionUpdate pos={position_seconds:.2}");
                self.update_spectrogram_position(position_seconds, ctx);
            }
            AnalysisCommand::SeekPosition(position_seconds) => {
                profile_eprintln!("[analysis] SeekPosition pos={position_seconds:.2}");
                self.seek_spectrogram_position(position_seconds, ctx);
            }
            AnalysisCommand::WaveformProgress {
                track_token,
                peaks,
                coverage_seconds,
                complete,
                done,
            } => self.handle_waveform_progress(
                track_token,
                peaks,
                coverage_seconds,
                complete,
                done,
                ctx.event_tx,
            ),
            AnalysisCommand::PrepareGaplessContinuation { path } => {
                self.handle_prepare_gapless_continuation(path, ctx);
            }
            AnalysisCommand::CancelStagedContinuation => {
                self.cancel_centered_staging();
                if self.staged_continuation_path.take().is_some() {
                    let _ = ctx
                        .spectrogram_cmd_tx
                        .send(SpectrogramWorkerCommand::CancelPendingContinue);
                    // Restart — the worker may have consumed the
                    // continuation and be decoding the wrong file.
                    self.spectrogram_position_offset = 0.0;
                    self.start_spectrogram_session(
                        self.last_spectrogram_position,
                        true,  // emit_initial_reset — triggers UI truncation
                        false, // clear_history — preserve rolling history
                        ctx,
                    );
                }
            }
            AnalysisCommand::ClearStagedContinuation => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
            }
        }
    }

    /// Send an early `ContinueWithFile` to the live worker so it writes
    /// next-track columns directly into the ring — zero gap.  Rolling
    /// mode only; centered mode needs fresh `NewTrack` with 0-based
    /// indices at commit time.  Called from `about-to-finish`.
    fn handle_prepare_gapless_continuation(&mut self, path: PathBuf, ctx: &AnalysisContext<'_>) {
        // Cancel any prior pending work for both modes.
        self.clear_early_continuation(ctx);
        self.cancel_centered_staging();

        // Format compatibility check (shared between both modes).
        // For raw surround files (AC3/DTS), Symphonia can't probe the
        // format and a GStreamer pipeline is too expensive.  Same-extension
        // surround transitions are virtually always compatible (same rate
        // and channel layout), so proceed optimistically — the worker
        // validates and falls back to NewTrack if wrong.
        #[cfg(feature = "gst")]
        let surround_optimistic = is_raw_surround_file(&path)
            && self
                .active_track_path
                .as_deref()
                .is_some_and(|active| same_surround_extension(active, &path));
        #[cfg(not(feature = "gst"))]
        let surround_optimistic = false;

        if !surround_optimistic {
            let Some(SymphoniaFile {
                native_sample_rate: native_sr,
                native_channels: native_ch,
                ..
            }) = open_symphonia_file(&path)
            else {
                profile_eprintln!(
                    "[analysis] staged: cannot open candidate {}",
                    path.display(),
                );
                return;
            };

            let divisor = usize::try_from(waveform_sample_rate_divisor(native_sr)).unwrap_or(1);
            let divisor_u64 = u64::try_from(divisor).unwrap_or(1);
            let cand_effective_rate =
                u32::try_from(native_sr / divisor_u64.max(1)).unwrap_or(48_000);
            let cand_channel_count = match self.spectrogram_view_mode {
                SpectrogramViewMode::Downmix => 1,
                SpectrogramViewMode::PerChannel => native_ch,
            };

            if cand_effective_rate != self.active_session_effective_rate
                || cand_channel_count != self.active_session_channel_count
            {
                profile_eprintln!(
                    "[analysis] staged: incompatible (rate {}!={} ch {}!={}), skipping",
                    cand_effective_rate,
                    self.active_session_effective_rate,
                    cand_channel_count,
                    self.active_session_channel_count,
                );
                return;
            }
        }

        if self.display_mode == SpectrogramDisplayMode::Rolling {
            // Rolling mode: send early ContinueWithFile to live worker.
            profile_eprintln!(
                "[analysis] early ContinueWithFile for {} optimistic={surround_optimistic}",
                path.display(),
            );
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: path.clone(),
                    track_token: self.active_track_token,
                });
            self.staged_continuation_path = Some(path);
        } else {
            // Centered mode: spawn staging decode thread.
            profile_eprintln!(
                "[analysis] centered staging for {} optimistic={surround_optimistic}",
                path.display(),
            );
            let stop = Arc::new(AtomicBool::new(false));
            let (rx, handle) = spawn_centered_staging_worker(
                path.clone(),
                self.fft_size,
                self.hop_size,
                self.zoom_level,
                self.spectrogram_view_mode,
                Arc::clone(&stop),
            );
            self.staged_centered_rx = Some(rx);
            self.staged_centered_stop = Some(stop);
            self.staged_centered_handle = Some(handle);
            self.staged_centered_path = Some(path);
        }
    }

    /// Clear any early continuation, sending `CancelPendingContinue` to
    /// the worker if one was in flight.  Does NOT restart the session.
    fn clear_early_continuation(&mut self, ctx: &AnalysisContext<'_>) {
        if self.staged_continuation_path.take().is_some() {
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::CancelPendingContinue);
        }
    }

    /// Signal the staging thread to stop, drain all buffered chunks,
    /// rewrite their track token, and emit them to the bridge.  Returns
    /// the number of columns emitted.
    ///
    /// Does NOT join the staging thread — the chunks have been accumulating
    /// in the channel for ~2 seconds and `try_iter()` grabs them all.
    /// The staging thread exits naturally after seeing the stop flag;
    /// `cancel_centered_staging` joins it if needed for cleanup.
    ///
    /// Chunks are emitted individually (no consolidation) so the first
    /// chunk (carrying the buffer reset) reaches the bridge immediately
    /// without waiting for a costly memcpy pass over the full dataset.
    /// Emit a 0-column finalize chunk for the outgoing track so Qt can
    /// shrink its `total_columns_estimate` to the actual decoded extent
    /// reported by the worker.  This is the signal the centered-mode
    /// paint code uses to detach the playhead from center as playback
    /// approaches the real end of the track.
    fn emit_outgoing_track_finalize(&self, outgoing_track_token: u64, ctx: &AnalysisContext<'_>) {
        if outgoing_track_token == 0 {
            return;
        }
        let cols_produced = ctx
            .spectrogram_decode_columns_produced
            .load(Ordering::Relaxed);
        if cols_produced == 0 {
            return;
        }
        let final_cols = u32::try_from(cols_produced).unwrap_or(u32::MAX);
        let bins_per_column = u16::try_from((self.fft_size / 2) + 1).unwrap_or(u16::MAX);
        let channel_count =
            u8::try_from(self.active_session_channel_count.max(1).min(255)).unwrap_or(u8::MAX);
        let _ = ctx
            .event_tx
            .send(AnalysisEvent::PrecomputedSpectrogramChunk(
                PrecomputedSpectrogramChunk {
                    track_token: outgoing_track_token,
                    columns_u8: Vec::new(),
                    bins_per_column,
                    column_count: 0,
                    channel_count,
                    start_column_index: final_cols,
                    total_columns_estimate: final_cols,
                    sample_rate_hz: self.active_session_effective_rate,
                    hop_size: 0,
                    coverage_seconds: 0.0,
                    complete: true,
                    buffer_reset: false,
                    clear_history: false,
                },
            ));
    }

    fn drain_staged_centered_chunks(&mut self, track_token: u64, ctx: &AnalysisContext<'_>) -> u32 {
        // Signal staging thread to stop (non-blocking).
        if let Some(stop) = self.staged_centered_stop.take() {
            stop.store(true, Ordering::Release);
        }
        // Detach the handle so cancel_centered_staging (called next)
        // won't block on join.  The thread exits naturally after
        // seeing the stop flag; dropping the handle detaches it.
        self.staged_centered_handle.take();
        let Some(rx) = self.staged_centered_rx.take() else {
            return 0;
        };

        let mut total_columns = 0u32;
        let mut first = true;
        for mut chunk in rx.try_iter() {
            total_columns += u32::from(chunk.column_count);
            chunk.track_token = track_token;
            if first {
                chunk.buffer_reset = true;
                chunk.clear_history = true;
                first = false;
            }
            let _ = ctx
                .event_tx
                .send(AnalysisEvent::PrecomputedSpectrogramChunk(chunk));
        }
        total_columns
    }

    /// Cancel any in-progress centered-mode staging thread and discard
    /// its buffered chunks.  Joins the thread to ensure clean shutdown.
    fn cancel_centered_staging(&mut self) {
        if let Some(stop) = self.staged_centered_stop.take() {
            stop.store(true, Ordering::Release);
        }
        if let Some(handle) = self.staged_centered_handle.take() {
            let _ = handle.join();
        }
        self.staged_centered_rx = None;
        self.staged_centered_path = None;
    }

    /// Rolling mode gapless: continue the existing decode session if
    /// format-compatible, or reset and start fresh when the format changed
    /// (e.g. 48 kHz/6ch → 44.1 kHz/2ch).
    fn handle_rolling_gapless(
        &mut self,
        path: &Path,
        track_token: u64,
        format_compatible: bool,
        ctx: &AnalysisContext<'_>,
    ) {
        if format_compatible {
            // Accumulate position offset: GStreamer's position resets to 0
            // for the new track, but the worker's coordinate space is
            // continuous.
            self.spectrogram_position_offset += self.last_spectrogram_position;

            if self.staged_continuation_path.take() == Some(path.to_path_buf()) {
                profile_eprintln!(
                    "[analysis] handle_track_change: UpdateTrackToken (early continue matched) offset={:.2}",
                    self.spectrogram_position_offset,
                );
                let _ = ctx
                    .spectrogram_cmd_tx
                    .send(SpectrogramWorkerCommand::UpdateTrackToken { track_token });
            } else {
                profile_eprintln!(
                    "[analysis] handle_track_change: dispatching ContinueWithFile offset={:.2}",
                    self.spectrogram_position_offset,
                );
                let _ = ctx
                    .spectrogram_cmd_tx
                    .send(SpectrogramWorkerCommand::ContinueWithFile {
                        path: path.to_path_buf(),
                        track_token,
                    });
            }
        } else {
            // Format changed — ContinueWithFile would be rejected by the
            // worker and fall back to NewTrack internally, but the analysis
            // would still have the accumulated offset, sending the worker
            // insane position updates past EOF.  Reset and start fresh.
            self.clear_early_continuation(ctx);
            self.spectrogram_position_offset = 0.0;
            profile_eprintln!(
                "[analysis] handle_track_change: rolling gapless format mismatch → fresh NewTrack",
            );
            self.start_spectrogram_session(0.0, true, false, ctx);
        }
    }

    fn handle_track_change(
        &mut self,
        path: PathBuf,
        reset_spectrogram: bool,
        gapless: bool,
        track_token: u64,
        ctx: &AnalysisContext<'_>,
    ) {
        #[cfg(feature = "profiling-logs")]
        let _track_change_start = std::time::Instant::now();

        // Snapshot the outgoing track's identity before any updates so
        // the centered-gapless path can emit a finalize chunk for it
        // before the new track's staged chunks flood the event stream.
        let outgoing_track_token = self.active_track_token;

        // Save previous compat params so the rolling gapless path can
        // detect incompatible transitions (e.g. 48 kHz/6ch → 44.1 kHz/2ch)
        // that would cause ContinueWithFile to fall back inside the worker.
        let prev_effective_rate = self.active_session_effective_rate;
        let prev_channel_count = self.active_session_channel_count;

        // Always update compat params from the new track so the staging
        // preflight has correct values.  This also covers same-extension
        // transitions where the worker internally falls back from
        // ContinueWithFile to NewTrack (different sample rate).
        self.update_session_compat_params(&path);
        let format_compatible = self.active_session_effective_rate == prev_effective_rate
            && self.active_session_channel_count == prev_channel_count;

        self.active_track_token = track_token;
        // For gapless transitions the PCM stream is continuous — the
        // playback module did NOT update the shared PCM tap atomic, so
        // chunks still arrive with the old token.  Keep active_pcm_track_token
        // unchanged so they are accepted without a gap.
        if !gapless {
            self.active_pcm_track_token = track_token;
            self.pcm_labels_pending_init = true;
        }
        ctx.waveform_decode_active_token
            .store(track_token, Ordering::Relaxed);
        self.active_track_stamp = source_stamp(&path);
        self.active_track_path = Some(path.clone());

        self.snapshot.waveform_peaks.clear();
        self.snapshot.waveform_coverage_seconds = 0.0;
        self.snapshot.waveform_complete = false;
        self.waveform_dirty = true;
        if reset_spectrogram {
            self.reset_spectrogram_state();
        }
        self.emit_snapshot(ctx.event_tx, true);

        if gapless && !reset_spectrogram && self.display_mode == SpectrogramDisplayMode::Rolling {
            self.handle_rolling_gapless(&path, track_token, format_compatible, ctx);
        } else if gapless && !reset_spectrogram {
            // Centered mode gapless: check for pre-staged data.
            self.clear_early_continuation(ctx);
            self.spectrogram_position_offset = 0.0;

            // Emit a finalize chunk for the outgoing track BEFORE the
            // staged chunks so Qt can shrink the old-track estimate to
            // the actual decoded extent in the short window before the
            // new track's buffer_reset lands.  Without this, the
            // centered-mode playhead stays pinned at center through
            // the transition because the file-metadata estimate
            // overshoots the true playback end by up to several
            // seconds (especially visible at max zoom).
            self.emit_outgoing_track_finalize(outgoing_track_token, ctx);

            let chunk_count = if self.staged_centered_path.as_ref() == Some(&path) {
                self.drain_staged_centered_chunks(track_token, ctx)
            } else {
                0
            };
            self.cancel_centered_staging();

            if chunk_count > 0 {
                profile_eprintln!(
                    "[analysis] handle_track_change: centered gapless → emitted {chunk_count} staged chunks",
                );
                // Start main worker from 0 without initial reset.  The
                // staged chunks already performed the reset.  The worker
                // re-decodes from the beginning; its identical columns
                // overwrite staged data in the ring.
                self.start_spectrogram_session_no_reset(0.0, ctx);
            } else {
                profile_eprintln!(
                    "[analysis] handle_track_change: centered gapless → fresh NewTrack",
                );
                self.start_spectrogram_session(0.0, true, true, ctx);
            }
        } else {
            // Non-gapless: cancel any stale staged continuation.
            self.clear_early_continuation(ctx);
            self.cancel_centered_staging();
            // Non-gapless: reset the offset since a new session starts
            // with fresh coordinates.
            self.spectrogram_position_offset = 0.0;
            // Start a fresh precomputed session. Emit a reset marker for
            // every non-gapless transition so the UI can distinguish a
            // true reset from a gapless handoff. Manual track changes clear
            // history; natural advances keep the already visible history.
            let emit_initial_reset = true;
            let clear_history_on_reset = reset_spectrogram;
            profile_eprintln!(
                "[analysis] handle_track_change: dispatching NewTrack from 0.0 emit_reset={emit_initial_reset} clear_history={clear_history_on_reset} gapless={gapless}",
            );
            self.start_spectrogram_session(0.0, emit_initial_reset, clear_history_on_reset, ctx);
        }

        if let Some(peaks) = self.load_cached_waveform(&path) {
            self.snapshot.waveform_peaks = peaks;
            self.snapshot.waveform_coverage_seconds = 0.0;
            self.snapshot.waveform_complete = true;
            self.waveform_dirty = true;
            self.emit_snapshot(ctx.event_tx, true);
            return;
        }

        let _ = ctx
            .waveform_job_tx
            .send(WaveformDecodeJob { track_token, path });

        profile_eprintln!(
            "[analysis] handle_track_change: completed in {:.2}ms",
            _track_change_start.elapsed().as_secs_f64() * 1000.0,
        );
    }

    /// Update the analysis-owned compatibility params from the given file
    /// so staging preflight can compare candidate files against the live
    /// worker session.  Called from `start_spectrogram_session` (covers all
    /// restart paths) and from `handle_track_change` (covers gapless where
    /// the worker may internally fall back from `ContinueWithFile` to `NewTrack`).
    fn update_session_compat_params(&mut self, path: &Path) {
        // Only probe via Symphonia (instant, ~0.05 ms).  For raw surround
        // files (AC3/DTS), skip entirely — Symphonia can't decode them and
        // its probe wastes ~200 ms per attempt on network filesystems
        // trying every format before failing.  The worker determines the
        // real format from its own file open.
        #[cfg(feature = "gst")]
        if is_raw_surround_file(path) {
            return;
        }
        let Some(SymphoniaFile {
            native_sample_rate: native_sr,
            native_channels: native_ch,
            ..
        }) = open_symphonia_file(path)
        else {
            return;
        };
        let divisor = usize::try_from(waveform_sample_rate_divisor(native_sr)).unwrap_or(1);
        let divisor_u64 = u64::try_from(divisor).unwrap_or(1);
        self.active_session_effective_rate =
            u32::try_from(native_sr / divisor_u64.max(1)).unwrap_or(48_000);
        self.active_session_channel_count = match self.spectrogram_view_mode {
            SpectrogramViewMode::Downmix => 1,
            SpectrogramViewMode::PerChannel => native_ch,
        };
        self.active_session_divisor = divisor;
    }

    fn start_spectrogram_session(
        &mut self,
        start_seconds: f64,
        emit_initial_reset: bool,
        clear_history_on_reset: bool,
        ctx: &AnalysisContext<'_>,
    ) {
        let Some(path) = self.active_track_path.clone() else {
            profile_eprintln!(
                "[analysis] start_spectrogram_session: no active_track_path, skipping"
            );
            return;
        };
        self.spectrogram_session_start = start_seconds;
        self.spectrogram_session_margin = self.centered_margin_seconds();
        // update_session_compat_params is already called by
        // handle_track_change before start_spectrogram_session; skip the
        // redundant probe here to avoid a second CIFS roundtrip.
        let path = &path;
        let gen = ctx
            .spectrogram_decode_generation
            .fetch_add(1, Ordering::Relaxed)
            + 1;
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::NewTrack {
                track_token: self.active_track_token,
                generation: gen,
                path: path.clone(),
                fft_size: self.fft_size,
                hop_size: self.hop_size,
                zoom_level: self.zoom_level,
                widget_width: self
                    .spectrogram_widget_width
                    .max(self.spectrogram_max_widget_width),
                channel_count: self.active_session_channel_count.max(1),
                start_seconds,
                emit_initial_reset,
                clear_history_on_reset,
                view_mode: self.spectrogram_view_mode,
                display_mode: self.display_mode,
            });
    }

    /// Return the decode start position for the current display mode.
    /// Centered mode starts just before the visible left edge so data
    /// around the playhead appears as quickly as possible.  Rolling mode
    /// starts at the playhead.
    fn centered_start_seconds(&self) -> f64 {
        if self.display_mode == SpectrogramDisplayMode::Centered {
            (self.last_spectrogram_position - self.centered_margin_seconds()).max(0.0)
        } else {
            self.last_spectrogram_position
        }
    }

    /// Compute the pre-decode margin for centered mode: how many seconds
    /// before the playhead to start decoding.  Based on the actual widget
    /// width, sample rate, zoom level, plus a small buffer.
    fn centered_margin_seconds(&self) -> f64 {
        let effective_hop = if self.zoom_level > 1.0 {
            self.hop_size
        } else {
            // Match the zoom-adapted decimation: at sub-1.0 zoom, columns
            // are coarser, so effective_hop = base_hop * decimation_factor.
            // Using REFERENCE_HOP here would underestimate the visible time
            // span by the decimation factor, causing seeks within the
            // already-decoded region to trigger unnecessary session restarts.
            let base_hop = (self.fft_size / 8).max(64);
            let zoom = f64::from(self.zoom_level.max(0.001));
            // Continuous fractional interval matching output_interval_for_hop.
            // REFERENCE_HOP and base_hop are small compile-time/session constants;
            // precision loss from usize→f64 is negligible.
            #[allow(clippy::cast_precision_loss)]
            let target_effective = REFERENCE_HOP as f64 / zoom;
            #[allow(clippy::cast_precision_loss)]
            let interval = (target_effective / base_hop as f64).max(1.0);
            // interval >= 1.0 and base_hop is small, so the product fits in usize.
            #[allow(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                clippy::cast_precision_loss
            )]
            let hop = (base_hop as f64 * interval).round() as usize;
            hop
        };
        let rate = if self.active_session_effective_rate > 0 {
            self.active_session_effective_rate
        } else if self.snapshot.sample_rate_hz > 0 {
            let sample_rate = self.snapshot.sample_rate_hz;
            let divisor = waveform_sample_rate_divisor(u64::from(sample_rate)).max(1);
            u32::try_from(u64::from(sample_rate) / divisor).unwrap_or(sample_rate)
        } else {
            0
        };
        if rate > 0 && effective_hop > 0 {
            // effective_hop and REFERENCE_HOP are hop sizes (small usize);
            // precision loss from usize→f64 is negligible for these values.
            #[allow(clippy::cast_precision_loss)]
            let cols_per_second = f64::from(rate) / effective_hop as f64;
            // Keep the seek-window math aligned with the live worker session:
            // both the worker lookahead and the Qt ring are sized from the
            // largest width seen so far, so using only the current width here
            // can misclassify in-window seeks as restart-worthy after a
            // narrow-pane layout change.
            let width = f64::from(
                self.spectrogram_widget_width
                    .max(self.spectrogram_max_widget_width),
            );
            #[allow(clippy::cast_precision_loss)]
            let effective_zoom =
                f64::from(self.zoom_level) * effective_hop as f64 / REFERENCE_HOP as f64;
            let visible_cols = width / effective_zoom.max(0.01);
            // Full screen width in seconds plus 2 s for STFT warmup.
            // A full width (not half) is needed because near the end of
            // a track the playhead detaches from center and moves right,
            // making the visible left edge up to one full screen width
            // behind the playhead position.
            let full_screen = visible_cols / cols_per_second;
            full_screen + 2.0
        } else {
            // No rate info yet (first track load) — use generous fallback.
            30.0
        }
    }

    /// Start a spectrogram session without emitting an initial reset.
    /// Used when staged chunks have already been emitted with reset flags.
    fn start_spectrogram_session_no_reset(
        &mut self,
        start_seconds: f64,
        ctx: &AnalysisContext<'_>,
    ) {
        self.start_spectrogram_session(start_seconds, false, false, ctx);
    }

    fn update_spectrogram_position(&mut self, position_seconds: f64, ctx: &AnalysisContext<'_>) {
        self.last_spectrogram_position = position_seconds;
        if self.suppress_next_spectrogram_position_update {
            self.suppress_next_spectrogram_position_update = false;
            return;
        }
        let adjusted = position_seconds + self.spectrogram_position_offset;
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::PositionUpdate {
                position_seconds: adjusted,
            });
    }

    fn seek_spectrogram_position(&mut self, position_seconds: f64, ctx: &AnalysisContext<'_>) {
        self.spectrogram_position_offset = 0.0;
        self.last_spectrogram_position = position_seconds;

        if self.display_mode == SpectrogramDisplayMode::Centered {
            // Windowed centered: check if the seek target is within the
            // already-decoded window before restarting the session.  The
            // window extends from the session start for approximately
            // 2× the visible screen width plus lookahead (~10 s).  If
            // the seek is inside, the data is already in the ring buffer
            // — just send a PositionUpdate so the display shifts without
            // a costly session restart.
            // Use the larger of the current margin and the session's
            // original margin.  The widget width can change between
            // session start and seek (e.g. fullscreen toggle), which
            // changes centered_margin_seconds().  Using only the current
            // margin would flag seeks within the decoded extent as
            // "outside window" after width shrinks, causing unnecessary
            // session restarts and data loss.
            let current_margin = self.centered_margin_seconds();
            let margin = current_margin.max(self.spectrogram_session_margin);
            let window_seconds = margin * 2.0 + 10.0;
            let window_start = self.spectrogram_session_start;
            let window_end = window_start + window_seconds;

            // The visible left edge can be up to `margin` seconds before
            // the playhead (full screen width when playhead is at track end).
            // Check that the entire visible range fits within the window.
            let visible_left = (position_seconds - margin).max(0.0);
            if visible_left >= window_start && position_seconds <= window_end {
                // Seek within decoded window — cheap position update.
                let adjusted = position_seconds + self.spectrogram_position_offset;
                let _ = ctx
                    .spectrogram_cmd_tx
                    .send(SpectrogramWorkerCommand::PositionUpdate {
                        position_seconds: adjusted,
                    });
            } else {
                // Seek outside decoded window — restart session and clear
                // the ring buffer.
                //
                // Emit an immediate reset chunk from the analysis thread so
                // the frontend clears the ring BEFORE the next render frame.
                // Without this, there's a 3-5 ms gap between the position
                // property jumping and the worker's reset chunk arriving,
                // during which old ring data can be briefly visible at the
                // new playhead position.
                let synth_channel_count =
                    u8::try_from(self.active_session_channel_count.max(1)).unwrap_or(u8::MAX);
                let _ = ctx
                    .event_tx
                    .send(AnalysisEvent::PrecomputedSpectrogramChunk(
                        PrecomputedSpectrogramChunk {
                            track_token: self.active_track_token,
                            columns_u8: Vec::new(),
                            bins_per_column: 0,
                            column_count: 0,
                            channel_count: synth_channel_count,
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
                // Suppress the next PositionUpdate to prevent a race: the
                // playback snapshot may send a PositionUpdate at the new
                // position before the worker processes our NewTrack.
                self.suppress_next_spectrogram_position_update = true;
                let margin = self.centered_margin_seconds();
                let start = (position_seconds - margin).max(0.0);
                self.start_spectrogram_session(start, true, true, ctx);
            }
        } else {
            // Rolling mode: an explicit seek breaks the continuous gapless
            // timeline.  Send a Seek command (existing behavior).
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::Seek { position_seconds });
        }
    }

    fn load_cached_waveform(&mut self, path: &Path) -> Option<Vec<f32>> {
        let cache_hit = self
            .waveform_cache
            .get(path)
            .filter(|entry| entry.stamp == self.active_track_stamp)
            .map(|entry| entry.peaks.clone())
            .filter(|peaks| !peaks.is_empty());
        if let Some(peaks) = cache_hit {
            touch_waveform_cache_lru(&mut self.waveform_cache_lru, path);
            return Some(peaks);
        }

        let (Some(conn), Some(stamp)) = (self.waveform_db.as_ref(), self.active_track_stamp) else {
            return None;
        };
        let disk_hit = load_waveform_from_db(conn, path, stamp);
        if let Some(peaks) = disk_hit.as_ref() {
            insert_waveform_cache_entry(
                &mut self.waveform_cache,
                &mut self.waveform_cache_lru,
                path,
                WaveformCacheEntry {
                    stamp: Some(stamp),
                    peaks: peaks.clone(),
                },
            );
        }
        disk_hit
    }

    fn handle_waveform_progress(
        &mut self,
        track_token: u64,
        peaks: Vec<f32>,
        coverage_seconds: f32,
        complete: bool,
        done: bool,
        event_tx: &Sender<AnalysisEvent>,
    ) {
        if track_token != self.active_track_token || peaks.is_empty() {
            return;
        }

        self.snapshot.waveform_peaks = peaks;
        self.snapshot.waveform_coverage_seconds = coverage_seconds;
        self.snapshot.waveform_complete = complete;
        if done {
            self.persist_active_waveform();
        }
        self.waveform_dirty = true;
        if done || self.snapshot.waveform_peaks.len() >= 24 {
            self.emit_snapshot(event_tx, true);
        }
    }

    fn persist_active_waveform(&mut self) {
        let Some(path) = self.active_track_path.as_ref() else {
            return;
        };

        let cached_peaks = self.snapshot.waveform_peaks.clone();
        insert_waveform_cache_entry(
            &mut self.waveform_cache,
            &mut self.waveform_cache_lru,
            path,
            WaveformCacheEntry {
                stamp: self.active_track_stamp,
                peaks: cached_peaks.clone(),
            },
        );

        let (Some(conn), Some(stamp)) = (self.waveform_db.as_mut(), self.active_track_stamp) else {
            return;
        };
        let _ = persist_waveform_to_db(conn, path, stamp, &cached_peaks);
        self.waveform_db_writes_since_prune = self.waveform_db_writes_since_prune.saturating_add(1);
        if self.waveform_db_writes_since_prune >= PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL {
            let _ = prune_persistent_waveform_cache(conn, PERSISTENT_WAVEFORM_CACHE_MAX_ROWS);
            self.waveform_db_writes_since_prune = 0;
        }
    }

    fn reset_spectrogram_state(&mut self) {
        self.pending_channels.clear();
        self.pcm_fifo.clear();
        // Clear labels so the first chunk from the new track
        // unconditionally sets them.  Keeping stale labels from the
        // previous track would cause the transient-suppression logic in
        // push_pcm_chunk to misidentify a legitimate channel-count
        // reduction (e.g. 5.1 → stereo) as a decoder startup transient,
        // permanently blocking all incoming audio.
        self.pcm_labels.clear();
    }

    fn handle_pcm_ready(
        &mut self,
        first_chunk: AnalysisPcmChunk,
        pcm_rx: &Receiver<AnalysisPcmChunk>,
        event_tx: &Sender<AnalysisEvent>,
    ) {
        self.prof_ticks += 1;
        self.push_pcm_chunk(first_chunk);
        self.pull_pcm_chunks(pcm_rx);

        let channel_count = self.pcm_labels.len().max(1);
        self.trim_pcm_fifo(channel_count);
        self.emit_snapshot(event_tx, false);
        self.maybe_log_profile(channel_count);
    }

    fn pull_pcm_chunks(&mut self, pcm_rx: &Receiver<AnalysisPcmChunk>) {
        for _ in 0..64 {
            let Ok(chunk) = pcm_rx.try_recv() else {
                break;
            };
            self.push_pcm_chunk(chunk);
        }
    }

    fn push_pcm_chunk(&mut self, chunk: AnalysisPcmChunk) {
        if chunk.samples.is_empty() {
            return;
        }
        if chunk.track_token != self.active_pcm_track_token {
            return;
        }
        self.prof_pcm += 1;
        self.prof_in_samples += chunk.samples.len();
        let chunk_labels = if chunk.channel_labels.is_empty() {
            vec![SpectrogramChannelLabel::Mono]
        } else {
            chunk.channel_labels.clone()
        };
        if chunk_labels == self.pcm_labels {
            // Labels match.  Do NOT clear pcm_labels_pending_init here —
            // during cross-format gapless transitions, residual buffers
            // from the old decoder (still in GStreamer's queues) can arrive
            // tagged with the new track token but carrying the old format.
            // Clearing the flag on these would re-enable suppression before
            // the real new-format buffers arrive.  The flag is only cleared
            // in the != branch when a genuine label change is accepted.
        } else {
            // GStreamer decoders (especially AC3/DTS) may initially report
            // fewer channels during startup before settling on the real
            // layout.  Suppress transient channel-count reductions to avoid
            // a brief spectrogram layout flicker.  Once the FIFO has enough
            // data the startup window has passed and we accept any change.
            //
            // Skip suppression when pcm_labels_pending_init is set — the
            // first label change after a track change is always legitimate
            // (a real format difference, not a decoder transient).
            let is_startup_reduction = !self.pcm_labels_pending_init
                && chunk_labels.len() < self.pcm_labels.len()
                && self.pcm_fifo.len() < self.pcm_labels.len() * 4096;
            if is_startup_reduction {
                return;
            }
            self.pcm_labels_pending_init = false;
            self.pcm_labels.clone_from(&chunk_labels);
            self.pcm_fifo.clear();
            self.pending_channels.clear();
        }
        self.pcm_fifo.extend(chunk.samples);
    }

    fn trim_pcm_fifo(&mut self, channel_count: usize) {
        let fifo_max_frames = (u32_to_usize(self.snapshot.sample_rate_hz) / 2).max(4096);
        while (self.pcm_fifo.len() / channel_count) > fifo_max_frames {
            for _ in 0..channel_count {
                let _ = self.pcm_fifo.pop_front();
            }
        }
    }

    fn maybe_log_profile(&mut self, _channel_count: usize) {
        if !self.profile_enabled || self.prof_last.elapsed() < Duration::from_secs(1) {
            return;
        }

        profile_eprintln!(
            "[analysis] wakes/s={} pcm_chunks/s={} in_samples/s={} out_samples/s={} rows/s={} pending_samples={} fifo_frames={} fft={} hop={} channels={}",
            self.prof_ticks,
            self.prof_pcm,
            self.prof_in_samples,
            self.prof_out_samples,
            self.prof_rows,
            0,
            self.pcm_fifo.len() / _channel_count,
            self.fft_size,
            self.hop_size,
            self.pcm_labels.len()
        );
        self.prof_last = std::time::Instant::now();
        self.prof_pcm = 0;
        self.prof_in_samples = 0;
        self.prof_out_samples = 0;
        self.prof_rows = 0;
        self.prof_ticks = 0;
    }

    fn emit_snapshot(&mut self, event_tx: &Sender<AnalysisEvent>, force: bool) {
        emit_snapshot(
            event_tx,
            &self.snapshot,
            &mut self.pending_channels,
            &mut self.waveform_dirty,
            &mut self.last_emit,
            force,
        );
    }
}

fn spawn_waveform_decode_worker(
    waveform_job_rx: Receiver<WaveformDecodeJob>,
    waveform_tx: Sender<AnalysisCommand>,
    waveform_decode_active_token: Arc<AtomicU64>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-waveform-decode".to_string())
        .spawn(move || {
            while let Ok(mut job) = waveform_job_rx.recv() {
                while let Ok(next_job) = waveform_job_rx.try_recv() {
                    job = next_job;
                }
                let track_token = job.track_token;
                let _ = decode_waveform_peaks_stream(
                    &job.path,
                    1024,
                    |peaks, coverage_seconds, done| {
                        if waveform_decode_active_token.load(Ordering::Relaxed) != track_token {
                            return false;
                        }
                        let _ = waveform_tx.send(AnalysisCommand::WaveformProgress {
                            track_token,
                            peaks,
                            coverage_seconds,
                            complete: done,
                            done,
                        });
                        true
                    },
                    || waveform_decode_active_token.load(Ordering::Relaxed) != track_token,
                );
            }
        });
}

/// Saturating conversion from `f64` to `u64`, clamping to `[0, u64::MAX]`.
fn f64_to_u64_saturating(v: f64) -> u64 {
    if v <= 0.0 {
        return 0;
    }
    // u64::MAX is not exactly representable in f64, so any f64 >= 2^63 is
    // close enough to saturate.
    if v >= 9_223_372_036_854_775_808.0 {
        return u64::MAX;
    }
    // Value is non-negative and below the saturation threshold.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let r = v as u64;
    r
}

/// Lossless `usize`-to-`f32` is impossible on 32/64-bit targets. Precision
/// loss is acceptable here (channel-count reciprocal, typically <=8).
fn usize_to_f32_approx(v: usize) -> f32 {
    #[allow(clippy::cast_precision_loss)]
    let r = v as f32;
    r
}

fn spawn_analysis_worker(
    cmd_rx: Receiver<AnalysisCommand>,
    pcm_rx: Receiver<AnalysisPcmChunk>,
    event_tx: Sender<AnalysisEvent>,
    waveform_job_tx: Sender<WaveformDecodeJob>,
    waveform_decode_active_token: Arc<AtomicU64>,
    spectrogram: SpectrogramWorkerHandles,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-analysis".to_string())
        .spawn(move || {
            let mut state = AnalysisRuntimeState::new();
            loop {
                select! {
                    recv(cmd_rx) -> msg => {
                        let Ok(cmd) = msg else { break; };
                        let ctx = AnalysisContext {
                            event_tx: &event_tx,
                            waveform_job_tx: &waveform_job_tx,
                            waveform_decode_active_token: waveform_decode_active_token.as_ref(),
                            spectrogram_cmd_tx: &spectrogram.cmd_tx,
                            spectrogram_decode_generation: spectrogram.decode_generation.as_ref(),
                            spectrogram_decode_columns_produced:
                                spectrogram.columns_produced.as_ref(),
                        };
                        state.handle_command(cmd, &ctx);
                    }
                    recv(pcm_rx) -> msg => {
                        let Ok(chunk) = msg else { break; };
                        state.handle_pcm_ready(chunk, &pcm_rx, &event_tx);
                    }
                }
            }
        });
}

fn source_stamp(path: &Path) -> Option<WaveformSourceStamp> {
    let meta = std::fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?;
    let since_epoch = modified.duration_since(UNIX_EPOCH).ok()?;
    Some(WaveformSourceStamp {
        size_bytes: meta.len(),
        modified_secs: since_epoch.as_secs(),
        modified_nanos: since_epoch.subsec_nanos(),
    })
}

fn u32_to_usize(value: u32) -> usize {
    usize::try_from(value).unwrap_or(usize::MAX)
}

fn small_usize_to_f32(value: usize) -> f32 {
    f32::from(u16::try_from(value).expect("value fits into u16"))
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

fn emit_snapshot(
    event_tx: &Sender<AnalysisEvent>,
    snapshot: &AnalysisSnapshot,
    pending_channels: &mut Vec<AnalysisSpectrogramChannel>,
    waveform_dirty: &mut bool,
    last_emit: &mut std::time::Instant,
    force: bool,
) {
    if !*waveform_dirty && !force {
        return;
    }
    if !force && last_emit.elapsed() < std::time::Duration::from_millis(16) {
        return;
    }

    pending_channels.clear();
    let out = AnalysisSnapshot {
        waveform_peaks: if *waveform_dirty {
            snapshot.waveform_peaks.clone()
        } else {
            Vec::new()
        },
        waveform_coverage_seconds: snapshot.waveform_coverage_seconds,
        waveform_complete: snapshot.waveform_complete,
        sample_rate_hz: snapshot.sample_rate_hz,
    };
    let _ = event_tx.send(AnalysisEvent::Snapshot(out));
    *waveform_dirty = false;
    *last_emit = std::time::Instant::now();
}

fn decode_waveform_peaks_stream<F, C>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, bool) -> bool,
    C: FnMut() -> bool,
{
    #[cfg(feature = "gst")]
    if is_raw_surround_file(path) {
        return decode_waveform_peaks_stream_gst(path, max_points, on_update, is_cancelled);
    }

    if is_cancelled() {
        return Ok(());
    }
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }

    let file = File::open(path)?;
    let mss = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());

    let mut format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )?
        .format;

    let track = format
        .default_track()
        .ok_or_else(|| anyhow::anyhow!("no default track"))?;
    let track_id = track.id;

    let mut audio_decoder =
        symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default())?;
    let sample_rate_hz = u64::from(track.codec_params.sample_rate.unwrap_or(48_000));
    let estimated_frames = track.codec_params.n_frames.unwrap_or(sample_rate_hz * 240);

    let mut sample_buf: Option<SampleBuffer<f32>> = None;
    let mut waveform = WaveformAccumulator::new(max_points, estimated_frames, sample_rate_hz);

    let mut packet_counter = 0usize;
    loop {
        if is_cancelled() {
            return Ok(());
        }
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(SymphoniaError::ResetRequired | _) => break,
        };

        if packet.track_id() != track_id {
            continue;
        }
        packet_counter += 1;

        let decoded_audio = match audio_decoder.decode(&packet) {
            Ok(decoded_audio) => decoded_audio,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(SymphoniaError::DecodeError(_)) => continue,
            Err(_) => break,
        };

        let spec = *decoded_audio.spec();
        let channels = spec.channels.count().max(1);
        let base_sample_stride = if channels >= 2 { 8usize } else { 4usize };
        let sample_rate_divisor =
            usize::try_from(waveform_sample_rate_divisor(sample_rate_hz)).unwrap_or(1);
        let sample_stride = base_sample_stride.saturating_mul(sample_rate_divisor);
        let decoded_capacity = decoded_audio.capacity();
        let buf = ensure_sample_buffer(&mut sample_buf, decoded_capacity, spec);
        buf.copy_interleaved_ref(decoded_audio);

        let samples = buf.samples();
        let frame_width = channels.saturating_mul(sample_stride).max(1);
        for base in (0..samples.len()).step_by(frame_width) {
            if base.is_multiple_of(4096) && is_cancelled() {
                return Ok(());
            }
            let peak = peak_across_channels(samples, base, channels);
            if !waveform.push_sample(peak, sample_stride, &mut on_update) {
                return Ok(());
            }
        }

        // Keep this worker from starving UI/render threads on heavy FLAC decode.
        if packet_counter.is_multiple_of(64) {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }

    if !is_cancelled() {
        let _ = on_update(waveform.finish(), 0.0, true);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;

    #[test]
    fn emit_snapshot_respects_force_and_waveform_dirty() {
        let (tx, rx) = unbounded::<AnalysisEvent>();
        let snapshot = AnalysisSnapshot {
            waveform_peaks: vec![0.1, 0.2],
            waveform_coverage_seconds: 0.0,
            waveform_complete: true,
            sample_rate_hz: 48_000,
        };
        let mut pending_channels = Vec::<AnalysisSpectrogramChannel>::new();
        let mut waveform_dirty = true;
        let mut last_emit = std::time::Instant::now() - Duration::from_secs(1);

        emit_snapshot(
            &tx,
            &snapshot,
            &mut pending_channels,
            &mut waveform_dirty,
            &mut last_emit,
            true,
        );
        let evt = rx.try_recv().expect("snapshot event");
        match evt {
            AnalysisEvent::Snapshot(s) => assert_eq!(s.waveform_peaks, vec![0.1, 0.2]),
            _ => panic!("unexpected event variant"),
        }
    }

    #[test]
    fn push_pcm_chunk_accepts_stereo_after_surround_track_change() {
        let mut state = AnalysisRuntimeState::new();
        let token = 1;
        state.active_pcm_track_token = token;

        let surround_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
            SpectrogramChannelLabel::FrontCenter,
            SpectrogramChannelLabel::Lfe,
            SpectrogramChannelLabel::RearLeft,
            SpectrogramChannelLabel::RearRight,
        ];

        // Simulate playing a 5.1 track: push enough data to exit the
        // startup window.
        state.pcm_labels = surround_labels.clone();
        let surround_samples: Vec<f32> = vec![0.1; 6 * 5000];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: surround_samples,
            channel_labels: surround_labels,
            track_token: token,
        });
        assert!(
            !state.pcm_fifo.is_empty(),
            "surround data should be in FIFO"
        );

        // Switch to a new stereo track.
        let token2 = 2;
        state.active_pcm_track_token = token2;
        state.reset_spectrogram_state();

        // Push stereo chunk for the new track.
        let stereo_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
        ];
        let stereo_samples: Vec<f32> = vec![0.2; 2 * 1024];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: stereo_samples,
            channel_labels: stereo_labels.clone(),
            track_token: token2,
        });

        // The stereo data must be accepted, not suppressed.
        assert_eq!(state.pcm_labels, stereo_labels);
        assert!(
            !state.pcm_fifo.is_empty(),
            "stereo data must be accepted after track change, not suppressed"
        );
    }

    #[test]
    fn push_pcm_chunk_suppresses_transient_channel_reduction_during_startup() {
        let mut state = AnalysisRuntimeState::new();
        let token = 1;
        state.active_pcm_track_token = token;

        // First chunk arrives with the real surround layout.
        let surround_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
            SpectrogramChannelLabel::FrontCenter,
            SpectrogramChannelLabel::Lfe,
            SpectrogramChannelLabel::RearLeft,
            SpectrogramChannelLabel::RearRight,
        ];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: vec![0.1; 6 * 100],
            channel_labels: surround_labels.clone(),
            track_token: token,
        });
        assert_eq!(state.pcm_labels, surround_labels);
        let fifo_after_surround = state.pcm_fifo.len();

        // Decoder transiently reports fewer channels during startup.
        // This should be suppressed (data dropped, labels unchanged).
        let stereo_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
        ];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: vec![0.2; 2 * 100],
            channel_labels: stereo_labels,
            track_token: token,
        });

        // Labels should NOT have changed — the transient was suppressed.
        assert_eq!(state.pcm_labels, surround_labels);
        assert_eq!(
            state.pcm_fifo.len(),
            fifo_after_surround,
            "transient stereo data should be dropped during startup"
        );
    }

    #[test]
    fn push_pcm_chunk_accepts_stereo_after_gapless_surround_transition() {
        // Gapless (Natural) transitions do NOT call reset_spectrogram_state.
        // The pcm_labels_pending_init flag must still allow the format change.
        let mut state = AnalysisRuntimeState::new();
        let token = 1;
        state.active_pcm_track_token = token;
        state.pcm_labels_pending_init = false;

        let surround_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
            SpectrogramChannelLabel::FrontCenter,
            SpectrogramChannelLabel::Lfe,
            SpectrogramChannelLabel::RearLeft,
            SpectrogramChannelLabel::RearRight,
        ];
        state.pcm_labels = surround_labels;

        // Fill FIFO with surround data, then drain most of it to simulate
        // spectrogram processing having consumed the buffer.
        state.pcm_fifo.extend(vec![0.1f32; 6 * 500]);

        // Gapless transition: only token changes + pcm_labels_pending_init
        // is set (mirrors SetTrackToken handler). NO reset_spectrogram_state.
        let token2 = 2;
        state.active_pcm_track_token = token2;
        state.pcm_labels_pending_init = true;

        // Push stereo chunk — must NOT be suppressed.
        let stereo_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
        ];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: vec![0.2; 2 * 1024],
            channel_labels: stereo_labels.clone(),
            track_token: token2,
        });

        assert_eq!(state.pcm_labels, stereo_labels);
        assert!(
            !state.pcm_fifo.is_empty(),
            "stereo data must be accepted during gapless transition"
        );
        assert!(
            !state.pcm_labels_pending_init,
            "init flag should be cleared after first label set"
        );
    }

    #[test]
    fn push_pcm_chunk_survives_residual_old_format_buffers_during_gapless() {
        // During cross-format gapless, residual buffers from the old
        // decoder (still in GStreamer's queues) arrive tagged with the
        // new token but carrying the old format.  These must NOT clear
        // pcm_labels_pending_init, or the subsequent real format change
        // will be suppressed.
        let mut state = AnalysisRuntimeState::new();
        let token = 1;
        state.active_pcm_track_token = token;
        state.pcm_labels_pending_init = false;

        let surround_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
            SpectrogramChannelLabel::FrontCenter,
            SpectrogramChannelLabel::Lfe,
            SpectrogramChannelLabel::RearLeft,
            SpectrogramChannelLabel::RearRight,
        ];
        state.pcm_labels = surround_labels.clone();
        state.pcm_fifo.extend(vec![0.1f32; 6 * 500]);

        // Gapless transition: token changes, flag set.
        let token2 = 2;
        state.active_pcm_track_token = token2;
        state.pcm_labels_pending_init = true;

        // Residual 5.1 buffer arrives with the NEW token but OLD format.
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: vec![0.1; 6 * 256],
            channel_labels: surround_labels,
            track_token: token2,
        });
        // Flag must still be set — the residual buffer must not clear it.
        assert!(
            state.pcm_labels_pending_init,
            "residual old-format buffer must not clear pending_init flag"
        );

        // Now the real stereo buffers arrive — must be accepted.
        let stereo_labels = vec![
            SpectrogramChannelLabel::FrontLeft,
            SpectrogramChannelLabel::FrontRight,
        ];
        state.push_pcm_chunk(AnalysisPcmChunk {
            samples: vec![0.2; 2 * 1024],
            channel_labels: stereo_labels.clone(),
            track_token: token2,
        });

        assert_eq!(state.pcm_labels, stereo_labels);
        assert!(!state.pcm_labels_pending_init);
    }

    #[test]
    fn gapless_track_change_starts_seamless_new_session_immediately() {
        let mut state = AnalysisRuntimeState::new();
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Gapless track change now sends ContinueWithFile, not NewTrack.
        state.handle_track_change(PathBuf::from("/tmp/next.flac"), false, true, 9, &ctx);

        let cmd = spectrogram_cmd_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("spectrogram command");
        match cmd {
            SpectrogramWorkerCommand::ContinueWithFile {
                track_token, path, ..
            } => {
                assert_eq!(track_token, 9);
                assert_eq!(path, PathBuf::from("/tmp/next.flac"));
            }
            other => panic!("expected ContinueWithFile, got {other:?}"),
        }
        // Generation must NOT have been incremented.
        assert_eq!(spectrogram_decode_generation.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn non_gapless_track_change_sends_new_track() {
        let mut state = AnalysisRuntimeState::new();
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Non-gapless (gapless=false) must send NewTrack.
        state.handle_track_change(PathBuf::from("/tmp/track.flac"), false, false, 5, &ctx);

        let cmd = spectrogram_cmd_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("spectrogram command");
        match cmd {
            SpectrogramWorkerCommand::NewTrack {
                emit_initial_reset,
                clear_history_on_reset,
                ..
            } => {
                assert!(emit_initial_reset);
                assert!(!clear_history_on_reset);
            }
            other => panic!("expected NewTrack, got {other:?}"),
        }
        // Generation must have been incremented.
        assert_eq!(spectrogram_decode_generation.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn clear_early_continuation_sends_cancel_and_clears_path() {
        let mut state = AnalysisRuntimeState::new();
        state.staged_continuation_path = Some(PathBuf::from("/tmp/next.flac"));

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.clear_early_continuation(&ctx);
        assert!(state.staged_continuation_path.is_none());
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(matches!(
            cmd,
            SpectrogramWorkerCommand::CancelPendingContinue
        ));
    }

    #[test]
    fn clear_early_continuation_noop_when_no_path() {
        let mut state = AnalysisRuntimeState::new();
        assert!(state.staged_continuation_path.is_none());

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.clear_early_continuation(&ctx);
        // No command should be sent.
        assert!(spectrogram_cmd_rx.try_recv().is_err());
    }

    #[test]
    fn centered_gapless_emits_outgoing_finalize_before_staged_chunks() {
        // Regression for zoom-dependent playhead detachment at track end:
        // at high zoom the file-metadata total_columns_estimate can
        // overshoot the actual playable extent by ~1 second worth of
        // columns, which at hop=64 is ~700 columns — far more than the
        // Qt-side EOF-clamp tolerance.  The analysis thread emits a
        // finalize chunk for the outgoing token carrying the worker's
        // cols_produced so Qt can shrink its estimate and detach the
        // centered-mode playhead before the new track's staged chunks
        // commit the transition.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track_a.flac"));
        state.active_track_token = 7;
        state.active_session_effective_rate = 44_100;
        state.active_session_channel_count = 2;

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        // Simulate the worker having produced 159_980 cols for the
        // outgoing track before the transition committed (matches the
        // max-zoom scenario in diagnostics).
        let spectrogram_decode_columns_produced = AtomicU64::new(159_980);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/track_b.flac"),
            false, // reset_spectrogram
            true,  // gapless
            8,     // new track_token
            &ctx,
        );

        // Scan emitted events for a finalize chunk tagged with the
        // outgoing token and the expected cols count.
        let mut finalize_seen = false;
        while let Ok(ev) = event_rx.try_recv() {
            if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = ev {
                if chunk.complete && chunk.column_count == 0 && chunk.track_token == 7 {
                    assert_eq!(
                        chunk.total_columns_estimate, 159_980,
                        "finalize estimate must equal the worker's cols_produced"
                    );
                    assert!(!chunk.buffer_reset);
                    assert!(!chunk.clear_history);
                    finalize_seen = true;
                    break;
                }
            }
        }
        assert!(
            finalize_seen,
            "analysis thread must emit a finalize chunk for the outgoing track on centered gapless transition"
        );
    }

    #[test]
    fn centered_gapless_skips_finalize_when_no_columns_produced() {
        // When the worker has not produced any columns yet (fresh start,
        // immediate track-change), there is nothing to finalize and the
        // chunk must not be emitted.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track_a.flac"));
        state.active_track_token = 11;
        state.active_session_effective_rate = 44_100;
        state.active_session_channel_count = 2;

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(PathBuf::from("/tmp/track_b.flac"), false, true, 12, &ctx);

        // No event should be a finalize chunk.
        while let Ok(ev) = event_rx.try_recv() {
            if let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = ev {
                assert!(
                    !(chunk.complete && chunk.column_count == 0),
                    "no finalize chunk should be emitted when cols_produced is 0"
                );
            }
        }
    }

    #[test]
    fn centered_gapless_dispatches_new_track_with_reset() {
        // In centered mode, gapless transitions should start a fresh
        // NewTrack session (not ContinueWithFile) with buffer_reset so
        // the UI gets 0-based column indices and a clean ring.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track_a.flac"));
        state.active_track_token = 1;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/track_b.flac"),
            false, // reset_spectrogram
            true,  // gapless
            2,     // track_token
            &ctx,
        );

        // Should dispatch NewTrack (not ContinueWithFile) because
        // centered mode needs 0-based column indices.
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(
                cmd,
                SpectrogramWorkerCommand::NewTrack {
                    emit_initial_reset: true,
                    clear_history_on_reset: true,
                    ..
                }
            ),
            "centered gapless should dispatch NewTrack with reset, got {cmd:?}"
        );

        // Position offset should be reset for fresh coordinate space.
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_within_window_sends_position_update() {
        // Seeking within the decoded window should send a cheap
        // PositionUpdate, not restart the session.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.spectrogram_session_start = 0.0;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Seek to 60s — within the window [0, 100].
        state.seek_spectrogram_position(60.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::PositionUpdate { .. }),
            "centered seek within window should send PositionUpdate, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_uses_max_widget_width_for_zoomed_out_window() {
        // Regression for 6ch AC3 max-zoom-out seeks: the worker session
        // and Qt ring are sized from the largest seen width, but the
        // seek-window check used only the current pane width. On a narrow
        // pane this misclassified an in-window seek as "outside" and
        // restarted decoding from the tail, smearing the display.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.ac3"));
        state.active_session_effective_rate = 48_000;
        state.zoom_level = 1024.0 / 14_088.0;
        state.spectrogram_widget_width = 120;
        state.spectrogram_max_widget_width = 1_920;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(266.791, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::PositionUpdate { .. }),
            "seek should stay within the already-decoded max-width window, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_uses_playback_rate_when_raw_surround_probe_skips() {
        // Regression for raw surround (AC3/DTS): update_session_compat_params
        // skips the Symphonia probe, so active_session_effective_rate can stay
        // zero even while playback and worker chunks already know the real
        // 48 kHz rate. Falling back to the hard-coded 30 s margin makes
        // whole-track max-zoom-out seeks restart from the tail.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.ac3"));
        state.snapshot.sample_rate_hz = 48_000;
        state.zoom_level = 1024.0 / 14_088.0;
        state.hop_size = 14_088;
        state.spectrogram_widget_width = 1_200;
        state.spectrogram_max_widget_width = 1_920;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(217.777, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::PositionUpdate { .. }),
            "seek should stay within the already-decoded raw-surround window, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_outside_window_restarts_session() {
        // Seeking outside the decoded window should restart the session.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.spectrogram_session_start = 0.0;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Seek to 200s — outside the decoded window, so the session
        // should restart from one centered margin before the target.
        let expected_start = (200.0 - state.centered_margin_seconds()).max(0.0);
        state.seek_spectrogram_position(200.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::NewTrack { start_seconds, clear_history_on_reset, .. }
                if (start_seconds - expected_start).abs() < 0.01 && clear_history_on_reset),
            "centered seek outside window should restart with clear_history=true, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn rolling_seek_sends_seek_command() {
        // In rolling mode, seeks must restart the worker session.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(60.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "rolling seek should send Seek, got {cmd:?}"
        );
    }

    #[test]
    fn rolling_gapless_dispatches_continue_with_file() {
        // In rolling mode, gapless transitions use ContinueWithFile
        // for seamless scrolling continuity.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/track_a.flac"));
        state.active_track_token = 1;
        state.last_spectrogram_position = 200.0;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/track_b.flac"),
            false, // reset_spectrogram
            true,  // gapless
            2,     // track_token
            &ctx,
        );

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::ContinueWithFile { .. }),
            "rolling gapless should dispatch ContinueWithFile, got {cmd:?}"
        );

        // Position offset should accumulate.
        assert!((state.spectrogram_position_offset - 200.0).abs() < 0.01);
    }

    #[test]
    fn set_display_mode_command_updates_runtime_and_worker() {
        let mut state = AnalysisRuntimeState::new();
        assert_eq!(state.display_mode, SpectrogramDisplayMode::Rolling);

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_command(
            AnalysisCommand::SetSpectrogramDisplayMode(SpectrogramDisplayMode::Centered),
            &ctx,
        );

        assert_eq!(state.display_mode, SpectrogramDisplayMode::Centered);
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(matches!(
            cmd,
            SpectrogramWorkerCommand::SetDisplayMode(SpectrogramDisplayMode::Centered)
        ));
    }

    #[test]
    fn start_spectrogram_session_uses_file_derived_channel_count() {
        // Verify that start_spectrogram_session uses
        // active_session_channel_count (from the file) rather than
        // the stale spectrogram.pipelines.len() from the previous track.
        let mut state = AnalysisRuntimeState::new();
        // Simulate: previous track was 6-channel, pipelines has 6 entries.
        // New track is stereo → active_session_channel_count should be 2.
        state.active_session_channel_count = 2;
        state.active_track_path = Some(PathBuf::from("/tmp/stereo.flac"));
        state.active_track_token = 1;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.start_spectrogram_session(0.0, true, true, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        match cmd {
            SpectrogramWorkerCommand::NewTrack { channel_count, .. } => {
                // open_symphonia_file may fail for /tmp/stereo.flac (doesn't exist),
                // which leaves active_session_channel_count at 2.
                // The fallback path uses spectrogram.pipelines.len().max(1) = 1.
                // Either 2 (file-derived) or 1 (fallback) is acceptable; NOT 6.
                assert!(
                    channel_count <= 2,
                    "channel_count should use file-derived count, got {channel_count}"
                );
            }
            _ => panic!("expected NewTrack, got {cmd:?}"),
        }
    }

    #[test]
    fn rolling_gapless_uses_continue_with_file_not_new_track() {
        // Verify rolling mode gapless sends ContinueWithFile (not NewTrack)
        // and accumulates the position offset.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/a.flac"));
        state.active_track_token = 1;
        state.last_spectrogram_position = 200.0;
        state.active_session_channel_count = 2;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/b.flac"),
            false,
            true, // gapless
            2,
            &ctx,
        );

        // Rolling gapless must use ContinueWithFile.
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::ContinueWithFile { .. }),
            "rolling gapless should use ContinueWithFile, got {cmd:?}"
        );
        // Position offset accumulates old track duration.
        assert!((state.spectrogram_position_offset - 200.0).abs() < 0.01);
    }

    #[test]
    fn centered_gapless_uses_new_track_with_reset() {
        // Verify centered mode gapless sends NewTrack with reset
        // (not ContinueWithFile) and resets the position offset.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/a.flac"));
        state.active_track_token = 1;
        state.last_spectrogram_position = 200.0;
        state.spectrogram_position_offset = 100.0;
        state.active_session_channel_count = 2;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/b.flac"),
            false,
            true, // gapless
            2,
            &ctx,
        );

        // Centered gapless must use NewTrack with reset.
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(
                cmd,
                SpectrogramWorkerCommand::NewTrack {
                    emit_initial_reset: true,
                    clear_history_on_reset: true,
                    ..
                }
            ),
            "centered gapless should use NewTrack with reset, got {cmd:?}"
        );
        // Position offset is reset for fresh coordinate space.
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn prepare_gapless_is_noop_in_centered_mode() {
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_prepare_gapless_continuation(PathBuf::from("/tmp/next.flac"), &ctx);

        // No commands should be sent (centered mode skips early ContinueWithFile).
        assert!(spectrogram_cmd_rx.try_recv().is_err());
        assert!(state.staged_continuation_path.is_none());
    }

    #[test]
    fn gapless_track_change_with_early_continue_sends_update_token() {
        // When staged_continuation_path matches the incoming path,
        // handle_track_change should send UpdateTrackToken (not ContinueWithFile).
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/a.flac"));
        state.active_track_token = 1;
        state.last_spectrogram_position = 200.0;
        state.staged_continuation_path = Some(PathBuf::from("/tmp/b.flac"));

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_track_change(
            PathBuf::from("/tmp/b.flac"),
            false,
            true, // gapless
            2,
            &ctx,
        );

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(
                cmd,
                SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 }
            ),
            "expected UpdateTrackToken, got {cmd:?}"
        );
        // staged_continuation_path consumed.
        assert!(state.staged_continuation_path.is_none());
        assert!((state.spectrogram_position_offset - 200.0).abs() < 0.01);
    }

    #[test]
    fn cancel_staged_continuation_restarts_session() {
        let mut state = AnalysisRuntimeState::new();
        state.staged_continuation_path = Some(PathBuf::from("/tmp/next.flac"));
        state.spectrogram_position_offset = 200.0;
        state.last_spectrogram_position = 50.0;
        state.active_track_path = Some(PathBuf::from("/tmp/current.flac"));

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let gen_before = spectrogram_decode_generation.load(Ordering::Relaxed);
        state.handle_command(AnalysisCommand::CancelStagedContinuation, &ctx);

        // Path should be cleared.
        assert!(state.staged_continuation_path.is_none());
        // CancelPendingContinue sent to worker.
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(matches!(
            cmd,
            SpectrogramWorkerCommand::CancelPendingContinue
        ));
        // Session restarted — generation incremented.
        let gen_after = spectrogram_decode_generation.load(Ordering::Relaxed);
        assert!(gen_after > gen_before);
        // Position offset reset.
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn clear_staged_continuation_does_not_restart() {
        let mut state = AnalysisRuntimeState::new();
        state.staged_continuation_path = Some(PathBuf::from("/tmp/next.flac"));
        state.spectrogram_position_offset = 200.0;

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let gen_before = spectrogram_decode_generation.load(Ordering::Relaxed);
        state.handle_command(AnalysisCommand::ClearStagedContinuation, &ctx);

        // Path should be cleared.
        assert!(state.staged_continuation_path.is_none());
        // CancelPendingContinue sent to worker.
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(matches!(
            cmd,
            SpectrogramWorkerCommand::CancelPendingContinue
        ));
        // Session NOT restarted — generation unchanged.
        let gen_after = spectrogram_decode_generation.load(Ordering::Relaxed);
        assert_eq!(gen_after, gen_before);
        // Position offset preserved.
        assert!((state.spectrogram_position_offset - 200.0).abs() < 0.01);
    }

    #[test]
    fn cancel_staged_noop_when_no_path() {
        let mut state = AnalysisRuntimeState::new();
        assert!(state.staged_continuation_path.is_none());

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_command(AnalysisCommand::CancelStagedContinuation, &ctx);

        // No commands should be sent when no early continuation active.
        assert!(spectrogram_cmd_rx.try_recv().is_err());
    }

    #[cfg(feature = "gst")]
    #[test]
    fn prepare_gapless_surround_sends_optimistic_continue() {
        // When the active track and candidate are both .dts files,
        // the surround-optimistic path sends ContinueWithFile without
        // needing to open the file via Symphonia.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/current.dts"));
        state.active_track_token = 7;

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_prepare_gapless_continuation(PathBuf::from("/tmp/next.dts"), &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(
                cmd,
                SpectrogramWorkerCommand::ContinueWithFile {
                    ref path,
                    track_token: 7,
                } if path == &PathBuf::from("/tmp/next.dts")
            ),
            "expected ContinueWithFile for /tmp/next.dts with token 7, got {cmd:?}"
        );
        assert_eq!(
            state.staged_continuation_path,
            Some(PathBuf::from("/tmp/next.dts"))
        );
    }

    #[cfg(feature = "gst")]
    #[test]
    fn prepare_gapless_surround_rejects_mixed_extensions() {
        // AC3 active + DTS candidate: surround_optimistic is false,
        // falls through to open_symphonia_file which fails for the
        // nonexistent file, so no ContinueWithFile is sent.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.active_track_path = Some(PathBuf::from("/tmp/current.ac3"));

        let (event_tx, _) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_prepare_gapless_continuation(PathBuf::from("/tmp/next.dts"), &ctx);

        // No ContinueWithFile — surround_optimistic is false and
        // open_symphonia_file fails for the nonexistent path.
        assert!(spectrogram_cmd_rx.try_recv().is_err());
        assert!(state.staged_continuation_path.is_none());
    }

    #[test]
    fn cancel_centered_staging_sets_stop_and_clears_state() {
        let stop = Arc::new(AtomicBool::new(false));
        let (_tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();

        let mut state = AnalysisRuntimeState::new();
        state.staged_centered_rx = Some(rx);
        state.staged_centered_stop = Some(Arc::clone(&stop));
        state.staged_centered_path = Some(PathBuf::from("/test/track.flac"));

        state.cancel_centered_staging();

        assert!(state.staged_centered_rx.is_none());
        assert!(state.staged_centered_stop.is_none());
        assert!(state.staged_centered_handle.is_none());
        assert!(state.staged_centered_path.is_none());
        assert!(stop.load(Ordering::Relaxed));
    }

    #[test]
    fn stop_join_drain_captures_all_staged_output() {
        // Simulate a staging thread that produces chunks and flushes on stop.
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();
        let stop_clone = Arc::clone(&stop);

        let handle = std::thread::Builder::new()
            .name("test-staging".to_string())
            .spawn(move || {
                for i in 0..5u32 {
                    let _ = tx.send(PrecomputedSpectrogramChunk {
                        track_token: 0,
                        columns_u8: vec![0u8; 4],
                        bins_per_column: 4,
                        column_count: 1,
                        channel_count: 1,
                        start_column_index: i,
                        total_columns_estimate: 100,
                        sample_rate_hz: 48_000,
                        hop_size: 1024,
                        coverage_seconds: 0.0,
                        complete: false,
                        buffer_reset: false,
                        clear_history: false,
                    });
                    std::thread::sleep(Duration::from_millis(1));
                }
                // Wait for stop signal, then produce one more (flush-on-stop).
                while !stop_clone.load(Ordering::Acquire) {
                    std::thread::sleep(Duration::from_millis(1));
                }
                let _ = tx.send(PrecomputedSpectrogramChunk {
                    track_token: 0,
                    columns_u8: vec![0u8; 4],
                    bins_per_column: 4,
                    column_count: 1,
                    channel_count: 1,
                    start_column_index: 5,
                    total_columns_estimate: 100,
                    sample_rate_hz: 48_000,
                    hop_size: 1024,
                    coverage_seconds: 0.0,
                    complete: false,
                    buffer_reset: false,
                    clear_history: false,
                });
            })
            .unwrap();

        // Give the thread time to produce initial chunks.
        std::thread::sleep(Duration::from_millis(20));

        // Signal stop, join, then drain — should get all 6 chunks.
        stop.store(true, Ordering::Release);
        let _ = handle.join();
        let chunks: Vec<_> = rx.try_iter().collect();

        assert_eq!(chunks.len(), 6);
        assert_eq!(chunks[5].start_column_index, 5);
    }

    #[test]
    fn staged_chunks_get_token_rewritten_and_first_gets_reset_flags() {
        let (tx, rx) = unbounded::<PrecomputedSpectrogramChunk>();

        for i in 0..3u32 {
            let _ = tx.send(PrecomputedSpectrogramChunk {
                track_token: 0,
                columns_u8: vec![0u8; 12],
                bins_per_column: 4,
                column_count: 3,
                channel_count: 1,
                start_column_index: i * 3,
                total_columns_estimate: 1000,
                sample_rate_hz: 48_000,
                hop_size: 1024,
                coverage_seconds: 0.0,
                complete: false,
                buffer_reset: false,
                clear_history: false,
            });
        }
        drop(tx);

        let real_token: u64 = 42;
        let mut first = true;
        let mut rewritten: Vec<PrecomputedSpectrogramChunk> = Vec::new();
        for mut chunk in rx.try_iter() {
            chunk.track_token = real_token;
            if first {
                chunk.buffer_reset = true;
                chunk.clear_history = true;
                first = false;
            }
            rewritten.push(chunk);
        }

        assert_eq!(rewritten.len(), 3);
        assert_eq!(rewritten[0].track_token, 42);
        assert!(rewritten[0].buffer_reset);
        assert!(rewritten[0].clear_history);
        assert_eq!(rewritten[0].start_column_index, 0);
        assert_eq!(rewritten[1].track_token, 42);
        assert!(!rewritten[1].buffer_reset);
        assert!(!rewritten[1].clear_history);
        assert_eq!(rewritten[2].track_token, 42);
        assert!(!rewritten[2].buffer_reset);
    }

    #[test]
    fn zoom_hop_size_computation() {
        assert_eq!(zoom_hop_size(8192, 1.0), 1024);
        assert_eq!(zoom_hop_size(2048, 1.0), 256);
        assert_eq!(zoom_hop_size(8192, 2.0), 512);
        assert_eq!(zoom_hop_size(8192, 4.0), 256);
        assert_eq!(zoom_hop_size(8192, 16.0), 64);
        assert_eq!(zoom_hop_size(8192, 32.0), 64);
        assert_eq!(zoom_hop_size(8192, 0.5), 1024);
    }

    #[test]
    fn set_widget_width_growth_notifies_worker_to_extend_lookahead() {
        // Regression for the fullscreen-regression-after-unchanged-zoom
        // skip: a widget-width increase (e.g. fullscreen toggle) must
        // propagate to the running decoder so its centered-mode
        // lookahead park threshold grows to fill the larger display.
        // Without this, the decoder stays parked at the old window's
        // lookahead and the right side of the new fullscreen view
        // shows only the slowly-advancing decode edge.
        let mut state = AnalysisRuntimeState::new();
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_track_token = 1;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Default starts at 1920; first shrink to 1000 simulates the
        // windowed layout.  No UpdateWidgetWidth should flow because the
        // max has not grown.
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(1000), &ctx);
        assert_eq!(state.spectrogram_widget_width, 1000);
        assert_eq!(state.spectrogram_max_widget_width, 1920);
        assert!(
            spectrogram_cmd_rx.try_recv().is_err(),
            "shrinking the widget must not refresh the worker's lookahead"
        );

        // Growing past the previous max (fullscreen on a wider display)
        // must dispatch an UpdateWidgetWidth carrying the new value.
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(3840), &ctx);
        assert_eq!(state.spectrogram_widget_width, 3840);
        assert_eq!(state.spectrogram_max_widget_width, 3840);
        let cmd = spectrogram_cmd_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("widget-width growth must send a worker command");
        match cmd {
            SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width } => {
                assert_eq!(widget_width, 3840);
            }
            other => panic!("expected UpdateWidgetWidth, got {other:?}"),
        }

        // Shrinking back does not refresh again (would just waste work).
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(1200), &ctx);
        assert_eq!(state.spectrogram_widget_width, 1200);
        assert_eq!(state.spectrogram_max_widget_width, 3840);
        assert!(spectrogram_cmd_rx.try_recv().is_err());
    }

    #[test]
    fn set_zoom_level_with_unchanged_value_skips_session_restart() {
        // Regression: fullscreen toggle fires widthSettleTimer which
        // calls SetSpectrogramZoomLevel(currentZoom) with an unchanged
        // zoom.  Restarting the session in that case wipes the ring
        // and flashes the canvas black for ~100 ms while the decoder
        // catches up.  The existing session's data is still valid at
        // the same hop — the restart must be skipped.
        let mut state = AnalysisRuntimeState::new();
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_track_token = 1;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // Default zoom is 1.0 — set the same again.
        state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(1.0), &ctx);

        // No session restart command should have been sent.
        assert!(
            spectrogram_cmd_rx.try_recv().is_err(),
            "unchanged zoom must not restart the spectrogram session"
        );
        assert_eq!(spectrogram_decode_generation.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn set_zoom_level_with_changed_value_restarts_session() {
        // Counterpart to the skip test: verify a real zoom change
        // still triggers the session restart.
        let mut state = AnalysisRuntimeState::new();
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_track_token = 1;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(2.0), &ctx);

        // A real zoom change must send a NewTrack to restart decoding
        // at the new hop.
        let cmd = spectrogram_cmd_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("zoom change must send a spectrogram command");
        match cmd {
            SpectrogramWorkerCommand::NewTrack { .. } => {}
            other => panic!("expected NewTrack for real zoom change, got {other:?}"),
        }
        assert!(
            (state.zoom_level - 2.0).abs() < 0.001,
            "zoom_level should have been updated to the new value"
        );
    }
}
