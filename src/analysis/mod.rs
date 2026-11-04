// SPDX-License-Identifier: GPL-3.0-or-later

mod cache;
mod centered_view;
mod channel_layout;
pub(crate) use channel_layout::probe_source_channel_labels;
mod decoders;
mod fft;
mod output;
mod quantize;
pub(crate) mod retention;
use output::{resets_spectral_stream, AnalysisEventOutput, AnalysisOutputs};
#[cfg(feature = "gst")]
mod gst_lifecycle;
#[cfg(feature = "gst")]
mod gst_waveform;
mod session;
mod stft_batch;
mod track_io;
mod waveform_prefill;
mod waveform_pyramid;
mod waveform_service;
mod waveform_window;

pub(crate) use waveform_service::{release_reader, release_unread, retain_reader, WaveformReader};
pub(crate) use waveform_window::{decode_waveform_window_cancellable, WaveformWindow};

use std::cell::Cell;
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    Arc,
};
use std::time::{Duration, Instant};

use crossbeam_channel::{unbounded, Receiver, Sender, TryRecvError};
use symphonia::core::audio::SampleBuffer;

use cache::{usize_to_u64, WaveformSourceStamp};
use decoders::{analysis_format_options, packet_first_frame, symphonia_track_shape};
use fft::{ensure_sample_buffer, OverviewProgress, WaveformAccumulator, MIN_PREVIEW_PEAKS};
#[cfg(feature = "gst")]
use gst_waveform::decode_waveform_peaks_stream_gst;
use session::{
    spawn_centered_staging_worker, spawn_spectrogram_decode_worker, SessionHandoff, SessionReport,
    SpectrogramWorkerCommand, SpectrogramWorkerHandles, SpectrogramWorkerStatus,
};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::io::{MediaSource, MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;
use track_io::{
    format_from_name, spawn_track_io, stamp_of, FormatCache, TrackFormat, TrackIoReply,
    TrackIoRequest,
};
use waveform_prefill::PrefillSlot;

use crate::playback::{remap_path, PathRenames};
use crate::profile_logging::heartbeat_trace_enabled;
#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;
#[cfg(feature = "gst")]
use crate::raw_audio::same_surround_extension;
use crate::worker_thread::JoinableThread;

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

/// An update of the overview waveform decode of the active track.
#[derive(Debug, Clone)]
pub struct WaveformUpdate {
    peaks: Vec<f32>,
    coverage_seconds: f32,
    /// Whether the decode reached the end of the file.
    complete: bool,
    /// Whether this is the decode's last update.
    done: bool,
    /// The stamp of the file the decode read, which may be another than the
    /// one at the path when the track was loaded; `None` if not known, and
    /// then the waveform is not saved.
    source: Option<WaveformSourceStamp>,
}

impl WaveformUpdate {
    /// The update of an overview decode of the file of stamp `source`.
    fn of(
        peaks: Vec<f32>,
        coverage_seconds: f32,
        progress: OverviewProgress,
        source: Option<WaveformSourceStamp>,
    ) -> Self {
        Self {
            peaks,
            coverage_seconds,
            complete: progress == OverviewProgress::Complete,
            done: progress != OverviewProgress::Partial,
            source,
        }
    }
}

#[derive(Debug, Clone)]
pub enum AnalysisCommand {
    SetTrack {
        path: PathBuf,
        reset_spectrogram: bool,
        track_token: u64,
        /// Preserve continuous spectrogram history on compatible gapless transitions.
        gapless: bool,
    },
    ResetSpectrogram,
    SetSampleRate(u32),
    SetFftSize(usize),
    SetSpectrogramZoomLevel(f32),
    SetSpectrogramWidgetWidth(u32),
    SetSpectrogramActive(bool),
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
        update: WaveformUpdate,
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
    /// Files were renamed on disk: each old path maps to its new one. The
    /// audio is the same, so the analysis shown stays, and later decodes
    /// open the files at their new paths. The whole map applies at once, so
    /// a swap moves each path once.
    RemapPaths(Arc<PathRenames>),
    /// Stop the analysis threads. Only the engine sends this, when it is
    /// dropped; commands sent after it are ignored.
    Shutdown,
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
    pub generation: u64,
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
    /// The widget width the session decodes for, in logical pixels (0 if
    /// unknown). The UI sizes its ring for at least this width, so the ring
    /// holds the session's lookahead even in a narrower view.
    pub widget_width: u32,
}

#[derive(Debug, Clone)]
pub enum AnalysisEvent {
    Snapshot(AnalysisSnapshot),
    PrecomputedSpectrogramChunk(PrecomputedSpectrogramChunk),
    /// Part of the analysis stopped working; the message is for the user.
    WorkerFailed(String),
}

pub(crate) struct SpectrogramOutputRoute {
    pub sender: Sender<AnalysisEvent>,
    pub generation: Arc<AtomicU64>,
    pub stops: Arc<SpectrogramStops>,
}

/// The spectrogram deactivations the analysis thread has carried out, and
/// the decode generation the latest one made. A spectral chunk that reaches
/// the frontend after the deactivation and is not newer than that
/// generation is from before it; showing a view again starts a newer one.
/// The frontend reads this to tell the two apart without a field in the
/// chunks.
#[derive(Debug, Default)]
pub struct SpectrogramStops {
    count: AtomicU64,
    generation: AtomicU64,
}

impl SpectrogramStops {
    fn record(&self, generation: u64) {
        self.generation.store(generation, Ordering::Relaxed);
        // Publishes the generation with the count.
        self.count.fetch_add(1, Ordering::Release);
    }

    /// How many deactivations there have been, and the generation the
    /// latest made.
    pub fn load(&self) -> (u64, u64) {
        let count = self.count.load(Ordering::Acquire);
        (count, self.generation.load(Ordering::Relaxed))
    }
}

pub struct AnalysisEngine {
    tx: Sender<AnalysisCommand>,
    pending_resets: Arc<AtomicUsize>,
    /// Tells the threads to stop waiting on their output and decodes.
    closed: Arc<AtomicBool>,
    spectrogram_decode_generation: Arc<AtomicU64>,
    /// The analysis, file helper, waveform decode, and spectrogram decode
    /// threads.
    threads: Vec<JoinableThread>,
}

/// How long dropping the engine waits for its threads. Each checks for
/// shutdown between decoded chunks, which take milliseconds; the rest
/// covers closing a `GStreamer` pipeline and the waveform database.
pub(crate) const ANALYSIS_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);

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

/// The column hop that spectrogram sessions report at `zoom_level`, which
/// tells Qt whether data is for the zoom it asked for.
#[must_use]
pub fn spectrogram_column_hop(fft_size: usize, zoom_level: f32) -> usize {
    let zoom_level = zoom_level.clamp(0.05, 16.0);
    session::effective_hop_for_zoom(zoom_hop_size(fft_size, zoom_level), zoom_level)
}

/// The zoom the spectrogram moves to when `requested` is asked of it at
/// `current`, or `None` when it keeps `current`. Qt mirrors this through
/// the FFI to know which data to wait for.
#[must_use]
pub fn accepted_spectrogram_zoom(current: f32, requested: f32) -> Option<f32> {
    let requested = requested.clamp(0.05, 16.0);
    // Qt re-sends the zoom on width changes too (via the widthSettleTimer
    // after a fullscreen toggle) with the unchanged zoom.  Restarting the
    // session in that case wipes the ring and flashes black for ~100 ms
    // while the decoder catches up — but the existing session's data is
    // still valid at the same hop. Width commands already refill missing
    // history or extend the worker's lookahead.
    if (current - requested).abs() < 0.001 {
        None
    } else {
        Some(requested)
    }
}

#[derive(Debug, Clone)]
struct WaveformDecodeJob {
    track_token: u64,
    path: PathBuf,
    /// Whether to decode the overview, which a saved one makes needless.
    decode: bool,
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
    /// The width the live worker session decodes for: the widget width
    /// when it started, raised while it runs when the widget grows, as a
    /// fullscreen toggle that keeps the zoom level does without a restart.
    /// It sizes the session's lookahead, and every chunk of the session
    /// carries it, so the UI's ring holds what the session sends. A
    /// narrower widget gets a narrower lookahead from its next session.
    spectrogram_session_widget_width: u32,
    spectrogram_view_mode: SpectrogramViewMode,
    spectrogram_active: bool,
    /// Where deactivations are published for the frontend.
    spectrogram_stops: Arc<SpectrogramStops>,
    active_track_token: u64,
    active_track_path: Option<PathBuf>,
    /// Display mode (Rolling/Centered) — forwarded to decode worker for rate throttle.
    display_mode: SpectrogramDisplayMode,
    /// Does file and database work, so this thread never waits on it.
    track_io: Sender<TrackIoRequest>,
    /// The requests this state sent when it was created for a test.
    #[cfg(test)]
    track_io_requests: Receiver<TrackIoRequest>,
    /// Whether the file helper has stopped; its work is done without it.
    track_io_lost: bool,
    /// The track whose stamp and saved waveform the helper is reading.
    pending_track_load: Option<u64>,
    /// Formats the helper found, or the spectrogram worker read from the
    /// files it opened, by file.
    formats: FormatCache,
    /// The track whose format the spectrogram worker has reported. Its
    /// decoder knows the format better than the file helper's probe.
    worker_format_track: Option<u64>,
    /// A rolling gapless switch to this track that continued the worker's
    /// timeline on a format known before the switch, with the session's
    /// effective rate and channel count before it.  The file may have
    /// changed since; the helper's reply for the track tells.
    unconfirmed_continuation: Option<(u64, u32, usize)>,
    /// The gapless successor whose format the helper is probing, with the
    /// request's number.
    pending_candidate: Option<(u64, PathBuf)>,
    next_candidate_request: u64,
    /// Cumulative offset added to playback positions before forwarding
    /// to the spectrogram worker.  Translates the new track's position
    /// (which resets to 0) into the worker's continuous coordinate
    /// space so `PositionUpdate(0.0)` doesn't trigger a backward seek.
    spectrogram_position_offset: f64,
    /// Where the worker's timeline reaches the staged rolling continuation,
    /// in seconds, when the worker opened it before the track switch.
    staged_continuation_start: Option<(PathBuf, f64)>,
    /// A rolling switch to a continuation the worker had not opened yet.
    /// Until it reports where the file starts on its timeline, the position
    /// offset counts the outgoing track as ending at its last position.
    provisional_continuation: Option<PathBuf>,
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
    /// Path of the next track for which an early `ContinueWithFile` was
    /// sent to the worker.  Consumed at commit time (`handle_track_change`).
    staged_continuation_path: Option<PathBuf>,
    /// Receiver for pre-decoded centered-mode chunks from the staging
    /// thread.  Each chunk has `track_token: 0` (placeholder).
    staged_centered_rx: Option<Receiver<PrecomputedSpectrogramChunk>>,
    /// Stop flag shared with the staging thread.
    staged_centered_stop: Option<Arc<AtomicBool>>,
    /// The staging thread.
    staged_centered_thread: Option<JoinableThread>,
    /// Path of the file being pre-decoded for centered gapless.
    staged_centered_path: Option<PathBuf>,
    /// Staging threads told to stop, which may still be opening their file.
    /// Each chunk receiver disconnects when its thread ends; they are joined
    /// only at shutdown.
    retired_staging: Vec<(JoinableThread, Receiver<PrecomputedSpectrogramChunk>)>,
    /// Holds staging threads before they open their file, for tests.
    #[cfg(test)]
    staging_open_gate: Option<Receiver<()>>,
}

/// How a spectrogram session starts.
#[derive(Default)]
struct SessionStart {
    start_seconds: f64,
    target_position_seconds: f64,
    emit_initial_reset: bool,
    clear_history_on_reset: bool,
    /// Continue the current generation instead of superseding it, for a
    /// gapless switch whose outgoing session finishes its stream first.
    continue_generation: bool,
    handoff: SessionHandoff,
}

/// The analysis thread's channels to the file helper.
struct TrackIoChannels {
    requests: Sender<TrackIoRequest>,
    replies: Receiver<TrackIoReply>,
}

impl AnalysisEngine {
    #[must_use]
    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    pub fn new() -> (Self, Receiver<AnalysisEvent>) {
        Self::new_with_spectrogram_output(None)
    }

    pub(crate) fn new_with_spectrogram_output(
        spectral_route: Option<SpectrogramOutputRoute>,
    ) -> (Self, Receiver<AnalysisEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<AnalysisCommand>();
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();

        let waveform_tx = cmd_tx.clone();
        let (waveform_job_tx, waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = Arc::new(AtomicU64::new(0));
        let closed = Arc::new(AtomicBool::new(false));
        let mut threads = Vec::with_capacity(4);
        let (track_io_tx, track_io_rx) = unbounded::<TrackIoRequest>();
        let (track_io_reply_tx, track_io_reply_rx) = unbounded::<TrackIoReply>();
        threads.extend(spawn_track_io(track_io_rx, track_io_reply_tx));
        threads.extend(spawn_waveform_decode_worker(
            waveform_job_rx,
            waveform_tx,
            Arc::clone(&waveform_decode_active_token),
            Arc::clone(&closed),
        ));

        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let route = spectral_route.unwrap_or_else(|| SpectrogramOutputRoute {
            sender: event_tx.clone(),
            generation: Arc::new(AtomicU64::new(0)),
            stops: Arc::default(),
        });
        let spectrogram_decode_generation = route.generation;
        let spectrogram_stops = route.stops;
        let pending_resets = Arc::new(AtomicUsize::new(0));
        let event_tx = AnalysisOutputs {
            spectra: route.sender,
            snapshots: event_tx,
            generation: Arc::clone(&spectrogram_decode_generation),
            pending_resets: Arc::clone(&pending_resets),
            closed: Arc::clone(&closed),
        };
        let worker_columns_produced = Arc::new(AtomicU64::new(0));
        let worker_track_duration_ms = Arc::new(AtomicU64::new(0));
        let spectrogram_status = SpectrogramWorkerStatus::default();
        let (session_report_tx, session_report_rx) = unbounded::<SessionReport>();
        threads.extend(spawn_spectrogram_decode_worker(
            spectrogram_cmd_rx,
            event_tx.clone(),
            Arc::clone(&spectrogram_decode_generation),
            Arc::clone(&worker_columns_produced),
            Arc::clone(&worker_track_duration_ms),
            session_report_tx,
            &spectrogram_status,
        ));

        threads.extend(spawn_analysis_worker(
            cmd_rx,
            event_tx,
            waveform_job_tx,
            waveform_decode_active_token,
            TrackIoChannels {
                requests: track_io_tx,
                replies: track_io_reply_rx,
            },
            SpectrogramWorkerHandles {
                cmd_tx: spectrogram_cmd_tx,
                decode_generation: Arc::clone(&spectrogram_decode_generation),
                stops: spectrogram_stops,
                columns_produced: worker_columns_produced,
                track_duration_ms: worker_track_duration_ms,
                status: spectrogram_status,
                reports: session_report_rx,
            },
        ));

        (
            Self {
                tx: cmd_tx,
                pending_resets,
                closed,
                spectrogram_decode_generation,
                threads,
            },
            event_rx,
        )
    }

    pub fn command(&self, cmd: AnalysisCommand) {
        let resets = resets_spectral_stream(&cmd);
        if resets {
            self.pending_resets.fetch_add(1, Ordering::Relaxed);
        }
        if self.tx.send(cmd).is_err() && resets {
            self.pending_resets.fetch_sub(1, Ordering::Relaxed);
        }
    }

    #[must_use]
    pub fn sender(&self) -> Sender<AnalysisCommand> {
        self.tx.clone()
    }

    /// Stop the threads and wait for them, up to a bound. Returns how many
    /// are still running.
    ///
    /// Other senders, such as the playback engine, keep the command channel
    /// open, and the threads hold each other's channels, so none of them
    /// would see a disconnect. The analysis thread stops on `Shutdown`,
    /// which closes the waveform and spectrogram workers' channels.
    fn stop_threads(&mut self) -> usize {
        // Free a send blocked on a full spectral stream, and end the
        // current decodes, before the analysis thread gets to `Shutdown`.
        self.closed.store(true, Ordering::Relaxed);
        self.spectrogram_decode_generation
            .fetch_add(1, Ordering::Relaxed);
        let _ = self.tx.send(AnalysisCommand::Shutdown);
        let deadline = std::time::Instant::now() + ANALYSIS_SHUTDOWN_TIMEOUT;
        self.threads
            .drain(..)
            .map(|thread| thread.join_by(deadline))
            .filter(|joined| !joined)
            .count()
    }

    /// An engine whose commands go to `tx` instead of analysis threads.
    #[cfg(test)]
    pub(crate) fn from_command_sender(tx: Sender<AnalysisCommand>) -> Self {
        Self {
            tx,
            pending_resets: Arc::new(AtomicUsize::new(0)),
            closed: Arc::new(AtomicBool::new(false)),
            spectrogram_decode_generation: Arc::new(AtomicU64::new(0)),
            threads: Vec::new(),
        }
    }

    /// How many of the engine's threads are still running.
    #[cfg(test)]
    fn running_threads(&self) -> usize {
        self.threads
            .iter()
            .filter(|thread| thread.is_running())
            .count()
    }
}

impl Drop for AnalysisEngine {
    fn drop(&mut self) {
        let running = self.stop_threads();
        if running > 0 {
            eprintln!(
                "[ferrous] {running} analysis thread(s) did not stop within {} s",
                ANALYSIS_SHUTDOWN_TIMEOUT.as_secs()
            );
        }
    }
}

struct AnalysisContext<'a> {
    event_tx: &'a dyn AnalysisEventOutput,
    waveform_job_tx: &'a Sender<WaveformDecodeJob>,
    waveform_decode_active_token: &'a AtomicU64,
    spectrogram_cmd_tx: &'a Sender<SpectrogramWorkerCommand>,
    spectrogram_decode_generation: &'a AtomicU64,
    /// Absolute column count the spectrogram decode worker has produced
    /// for its current session.  Read by the analysis thread when a
    /// gapless transition commits so it can emit a finalize chunk for
    /// the outgoing track with the true decoded extent.
    spectrogram_decode_columns_produced: &'a AtomicU64,
    /// Track duration (milliseconds) published by the active worker
    /// session after its file probe; 0 = unknown.  Read by the analysis
    /// thread to model the centered display's visible left edge
    /// (full-track lock / EOF detach) when sizing decode windows.
    spectrogram_track_duration_ms: &'a AtomicU64,
}

impl AnalysisRuntimeState {
    /// A state whose file helper requests can be read from
    /// `track_io_requests`.
    #[cfg(test)]
    fn new() -> Self {
        let (track_io, track_io_requests) = unbounded();
        let mut state = Self::with_track_io(track_io);
        state.track_io_requests = track_io_requests;
        state
    }

    fn with_track_io(track_io: Sender<TrackIoRequest>) -> Self {
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
            spectrogram_session_widget_width: 1920,
            spectrogram_view_mode: SpectrogramViewMode::Downmix,
            spectrogram_active: true,
            spectrogram_stops: Arc::default(),
            active_track_token: 0,
            active_track_path: None,
            display_mode: SpectrogramDisplayMode::Rolling,
            track_io,
            #[cfg(test)]
            track_io_requests: crossbeam_channel::never(),
            track_io_lost: false,
            pending_track_load: None,
            formats: FormatCache::default(),
            worker_format_track: None,
            unconfirmed_continuation: None,
            pending_candidate: None,
            next_candidate_request: 0,
            spectrogram_position_offset: 0.0,
            staged_continuation_start: None,
            provisional_continuation: None,
            last_spectrogram_position: 0.0,
            spectrogram_session_start: 0.0,
            spectrogram_session_margin: 30.0,
            suppress_next_spectrogram_position_update: false,
            active_session_effective_rate: 0,
            active_session_channel_count: 0,
            staged_continuation_path: None,
            staged_centered_rx: None,
            staged_centered_stop: None,
            staged_centered_thread: None,
            staged_centered_path: None,
            retired_staging: Vec::new(),
            #[cfg(test)]
            staging_open_gate: None,
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
                let target = self.last_spectrogram_position;
                let start = self.spectrogram_restart_start_seconds(target, ctx);
                self.start_spectrogram_session_with_target(start, target, true, true, ctx);
            }
            AnalysisCommand::SetSpectrogramActive(active) => {
                self.set_spectrogram_active(active, ctx);
            }
            AnalysisCommand::SetSpectrogramWidgetWidth(width) => {
                let retained_start = self.centered_retained_start_seconds(ctx);
                let w = width.max(320);
                self.spectrogram_widget_width = w;
                if w > self.spectrogram_session_widget_width {
                    let target = self.last_spectrogram_position;
                    let visible_left = Self::centered_visible_left_edge_seconds(
                        target,
                        self.centered_margin_seconds(),
                        ctx,
                    );
                    // A wider ring cannot restore columns skipped by an earlier
                    // seek or already evicted at the OLD capacity. Refill only
                    // when the enlarged centered viewport exposes that history.
                    if self.spectrogram_active
                        && self.active_track_path.is_some()
                        && self.display_mode == SpectrogramDisplayMode::Centered
                        && visible_left < retained_start
                    {
                        self.clear_early_continuation(ctx);
                        self.cancel_centered_staging();
                        self.refill_centered_spectrogram(target, ctx);
                    } else {
                        // Existing history covers the left edge; let the worker
                        // extend its lookahead without clearing the Qt ring.
                        // The session now decodes for the wider view, and
                        // its window reaches as far.
                        self.spectrogram_session_widget_width = w;
                        self.spectrogram_session_margin = self
                            .spectrogram_session_margin
                            .max(self.centered_margin_seconds());
                        let _ = ctx
                            .spectrogram_cmd_tx
                            .send(SpectrogramWorkerCommand::UpdateWidgetWidth { widget_width: w });
                    }
                }
            }
            AnalysisCommand::SetSpectrogramZoomLevel(level) => {
                // Skip the restart when the zoom level hasn't actually changed.
                let Some(level) = accepted_spectrogram_zoom(self.zoom_level, level) else {
                    return;
                };
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                self.zoom_level = level;
                self.hop_size = zoom_hop_size(self.fft_size, self.zoom_level);
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                let target = self.last_spectrogram_position;
                let start = self.spectrogram_restart_start_seconds(target, ctx);
                self.start_spectrogram_session_with_target(start, target, true, true, ctx);
            }
            AnalysisCommand::SetSpectrogramViewMode(view_mode) => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
                profile_eprintln!("[analysis] SetSpectrogramViewMode({view_mode:?})");
                self.spectrogram_view_mode = view_mode;
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                let target = self.last_spectrogram_position;
                let start = self.spectrogram_restart_start_seconds(target, ctx);
                self.start_spectrogram_session_with_target(start, target, true, true, ctx);
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
                // RestartCurrentTrack is also the authoritative transport
                // re-anchor used when playback stops.  Keep the canonical
                // position in sync so a following zoom/view restart does not
                // resurrect the pre-stop playhead and decode an unrelated
                // window near EOF while Qt is rendering position 0.
                self.last_spectrogram_position = position_seconds;
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                let start = self.spectrogram_restart_start_seconds(position_seconds, ctx);
                self.start_spectrogram_session_with_target(
                    start,
                    position_seconds,
                    true,
                    clear_history,
                    ctx,
                );
            }
            AnalysisCommand::PositionUpdate(position_seconds) => {
                if heartbeat_trace_enabled() {
                    profile_eprintln!("[analysis] PositionUpdate pos={position_seconds:.2}");
                }
                self.update_spectrogram_position(position_seconds, ctx);
            }
            AnalysisCommand::SeekPosition(position_seconds) => {
                profile_eprintln!("[analysis] SeekPosition pos={position_seconds:.2}");
                self.seek_spectrogram_position(position_seconds, ctx);
            }
            AnalysisCommand::WaveformProgress {
                track_token,
                update,
            } => self.handle_waveform_progress(track_token, update, ctx.event_tx),
            AnalysisCommand::PrepareGaplessContinuation { path } => {
                self.handle_prepare_gapless_continuation(path, ctx);
            }
            AnalysisCommand::CancelStagedContinuation => {
                self.cancel_centered_staging();
                self.staged_continuation_start = None;
                if self.staged_continuation_path.take().is_some() {
                    let _ = ctx
                        .spectrogram_cmd_tx
                        .send(SpectrogramWorkerCommand::CancelPendingContinue);
                    // Restart — the worker may have consumed the
                    // continuation and be decoding the wrong file.
                    self.reset_position_offset();
                    let target = self.last_spectrogram_position;
                    self.start_spectrogram_session_with_target(
                        target, target, true,  // emit_initial_reset — triggers UI truncation
                        false, // clear_history — preserve rolling history
                        ctx,
                    );
                }
            }
            AnalysisCommand::ClearStagedContinuation => {
                self.clear_early_continuation(ctx);
                self.cancel_centered_staging();
            }
            AnalysisCommand::RemapPaths(renames) => self.remap_paths(renames, ctx),
            AnalysisCommand::Shutdown => {
                // Ends any decode session a command before this one started.
                ctx.spectrogram_decode_generation
                    .fetch_add(1, Ordering::Relaxed);
                self.cancel_centered_staging();
                // Leave time for the engine, which waits for this thread.
                let deadline = Instant::now() + ANALYSIS_SHUTDOWN_TIMEOUT / 2;
                for (thread, _) in self.retired_staging.drain(..) {
                    thread.join_by(deadline);
                }
            }
        }
    }

    /// Follow files renamed on disk without restarting anything: a later
    /// restart of the current track, or a staged successor, opens the file
    /// at its new path, and cached waveforms stay with their files.
    fn remap_paths(&mut self, renames: Arc<PathRenames>, ctx: &AnalysisContext<'_>) {
        for path in [
            &mut self.active_track_path,
            &mut self.staged_continuation_path,
            &mut self.staged_centered_path,
            &mut self.provisional_continuation,
        ]
        .into_iter()
        .flatten()
        {
            remap_path(path, &renames);
        }
        if let Some((_, path)) = self.pending_candidate.as_mut() {
            remap_path(path, &renames);
        }
        // A start the worker reported stays with its file, which the switch
        // names by its new path.
        if let Some((path, _)) = self.staged_continuation_start.as_mut() {
            remap_path(path, &renames);
        }
        self.formats.remap(&renames);
        // The helper keeps the decoded waveforms, and the worker opens a
        // gapless continuation, or a session it has not started yet, later.
        let _ = self
            .track_io
            .send(TrackIoRequest::RemapPaths(Arc::clone(&renames)));
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::RemapPaths(renames));
    }

    fn set_spectrogram_active(&mut self, active: bool, ctx: &AnalysisContext<'_>) {
        if self.spectrogram_active == active {
            return;
        }
        self.spectrogram_active = active;
        self.suppress_next_spectrogram_position_update = false;
        if active {
            let target = self.last_spectrogram_position;
            let start = if self.display_mode == SpectrogramDisplayMode::Rolling {
                // Restore a viewport of history when a hidden rolling view returns.
                (target - (self.centered_margin_seconds() - 2.0).max(0.0)).max(0.0)
            } else {
                self.spectrogram_restart_start_seconds(target, ctx)
            };
            self.start_spectrogram_session_with_target(start, target, true, true, ctx);
        } else {
            let generation = ctx
                .spectrogram_decode_generation
                .fetch_add(1, Ordering::Relaxed)
                + 1;
            // No session starts while inactive, so every chunk from now on
            // that is not newer is stale.
            self.spectrogram_stops.record(generation);
            self.clear_early_continuation(ctx);
            self.cancel_centered_staging();
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::StopSession);
        }
    }

    /// Send an early `ContinueWithFile` to the live worker so it writes
    /// next-track columns directly into the ring — zero gap.  Rolling
    /// mode only; centered mode needs fresh `NewTrack` with 0-based
    /// indices at commit time.  Called from `about-to-finish`.
    ///
    /// The successor must match the live session's format. A format the
    /// file helper found before is used at once; otherwise the helper
    /// probes the file, and the staging continues when it answers.
    fn handle_prepare_gapless_continuation(&mut self, path: PathBuf, ctx: &AnalysisContext<'_>) {
        if !self.spectrogram_active {
            return;
        }
        // Cancel any prior pending work for both modes.
        self.clear_early_continuation(ctx);
        self.cancel_centered_staging();

        let cached = self.formats.get(&path).map(|probed| probed.format);
        // A format the worker read when it decoded the file before.
        if let Some(known @ TrackFormat::Known { .. }) = cached {
            self.stage_successor_if_compatible(path, known, ctx);
            return;
        }

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
        if surround_optimistic {
            self.stage_successor(path, true, ctx);
            return;
        }

        if let Some(format) = format_from_name(&path).or(cached) {
            self.stage_successor_if_compatible(path, format, ctx);
            return;
        }
        self.next_candidate_request += 1;
        let request = self.next_candidate_request;
        let probe = TrackIoRequest::ProbeCandidate {
            request,
            path: path.clone(),
        };
        if self.track_io.send(probe).is_ok() {
            self.pending_candidate = Some((request, path));
        }
    }

    fn handle_candidate_probed(
        &mut self,
        request: u64,
        stamp: Option<WaveformSourceStamp>,
        format: TrackFormat,
        ctx: &AnalysisContext<'_>,
    ) {
        // Anything that dropped the staging since has dropped the request.
        let Some((_, path)) = self
            .pending_candidate
            .take_if(|(pending, _)| *pending == request)
        else {
            return;
        };
        self.formats.insert(path.clone(), format, stamp);
        if self.spectrogram_active {
            self.stage_successor_if_compatible(path, format, ctx);
        }
    }

    fn stage_successor_if_compatible(
        &mut self,
        path: PathBuf,
        format: TrackFormat,
        ctx: &AnalysisContext<'_>,
    ) {
        let TrackFormat::Known {
            sample_rate,
            channels,
        } = format
        else {
            profile_eprintln!(
                "[analysis] staged: cannot read candidate {}",
                path.display(),
            );
            return;
        };
        let cand_channel_count = match self.spectrogram_view_mode {
            SpectrogramViewMode::Downmix => 1,
            SpectrogramViewMode::PerChannel => channels,
        };
        if sample_rate != self.active_session_effective_rate
            || cand_channel_count != self.active_session_channel_count
        {
            profile_eprintln!(
                "[analysis] staged: incompatible (rate {}!={} ch {}!={}), skipping",
                sample_rate,
                self.active_session_effective_rate,
                cand_channel_count,
                self.active_session_channel_count,
            );
            return;
        }
        self.stage_successor(path, false, ctx);
    }

    // `optimistic` is only logged.
    #[cfg_attr(not(feature = "profiling-logs"), allow(unused_variables))]
    fn stage_successor(&mut self, path: PathBuf, optimistic: bool, ctx: &AnalysisContext<'_>) {
        if self.display_mode == SpectrogramDisplayMode::Rolling {
            // Rolling mode: send early ContinueWithFile to live worker.
            profile_eprintln!(
                "[analysis] early ContinueWithFile for {} optimistic={optimistic}",
                path.display(),
            );
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::ContinueWithFile {
                    path: path.clone(),
                    track_token: self.active_track_token,
                });
            self.staged_continuation_path = Some(path);
            return;
        }
        // Centered mode: spawn staging decode thread.
        profile_eprintln!(
            "[analysis] centered staging for {} optimistic={optimistic}",
            path.display(),
        );
        let stop = Arc::new(AtomicBool::new(false));
        #[cfg(test)]
        let open_gate = self.staging_open_gate.clone();
        #[cfg(not(test))]
        let open_gate = None;
        // Without the thread there is nothing staged, and the switch
        // starts a fresh session.
        let Some((rx, thread)) = spawn_centered_staging_worker(
            path.clone(),
            self.fft_size,
            self.hop_size,
            self.zoom_level,
            self.spectrogram_view_mode,
            Arc::clone(&stop),
            open_gate,
        ) else {
            return;
        };
        self.staged_centered_rx = Some(rx);
        self.staged_centered_stop = Some(stop);
        self.staged_centered_thread = Some(thread);
        self.staged_centered_path = Some(path);
    }

    /// Positions are the playing file's from now on, as the worker's
    /// timeline is: no continuation's start is added to them.
    fn reset_position_offset(&mut self) {
        self.spectrogram_position_offset = 0.0;
        self.provisional_continuation = None;
    }

    /// Clear any early continuation, sending `CancelPendingContinue` to
    /// the worker if one was in flight.  Does NOT restart the session.
    fn clear_early_continuation(&mut self, ctx: &AnalysisContext<'_>) {
        self.pending_candidate = None;
        self.staged_continuation_start = None;
        if self.staged_continuation_path.take().is_some() {
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::CancelPendingContinue);
        }
    }

    /// Take the chunks the staging thread has decoded so far for the
    /// session showing them as `track_token`; the first carries the reset.
    ///
    /// The chunks have been accumulating in the channel for ~2 seconds and
    /// `try_iter()` grabs them all; the staging thread is told to stop and
    /// is not waited for.
    fn take_staged_centered_chunks(
        &mut self,
        track_token: u64,
    ) -> Vec<PrecomputedSpectrogramChunk> {
        let Some(rx) = self.staged_centered_rx.as_ref() else {
            return Vec::new();
        };
        let mut chunks: Vec<_> = rx.try_iter().collect();
        for chunk in &mut chunks {
            chunk.track_token = track_token;
        }
        if let Some(first) = chunks.first_mut() {
            first.buffer_reset = true;
            first.clear_history = true;
        }
        chunks
    }

    /// Stop any in-progress centered-mode staging and discard its chunks.
    /// The thread may be opening its file, which can take seconds, so it is
    /// not waited for here: it is kept to be joined at shutdown.
    fn cancel_centered_staging(&mut self) {
        self.pending_candidate = None;
        if let Some(stop) = self.staged_centered_stop.take() {
            stop.store(true, Ordering::Release);
        }
        let rx = self.staged_centered_rx.take();
        if let (Some(thread), Some(rx)) = (self.staged_centered_thread.take(), rx) {
            // Drop what the retired threads flushed. A thread whose
            // receiver disconnected has ended, or is about to.
            self.retired_staging.retain(|(_, rx)| loop {
                match rx.try_recv() {
                    Ok(_) => {}
                    Err(TryRecvError::Empty) => break true,
                    Err(TryRecvError::Disconnected) => break false,
                }
            });
            self.retired_staging.push((thread, rx));
        }
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
            // GStreamer's position resets to 0 for the new track, but the
            // worker's timeline is continuous: the file starts where the
            // worker went on with it, which it reports when it opens it.
            let matched = self.staged_continuation_path.take().as_deref() == Some(path);
            let opened_at = self
                .staged_continuation_start
                .take()
                .filter(|(staged, _)| matched && staged == path);
            if let Some((_, start)) = opened_at {
                self.spectrogram_position_offset = start;
                self.provisional_continuation = None;
            } else {
                // Until it does, the outgoing track ends at its last position.
                self.spectrogram_position_offset += self.last_spectrogram_position;
                self.provisional_continuation = Some(path.to_path_buf());
            }

            if matched {
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
            // Format changed, or not known yet — ContinueWithFile would be
            // rejected by the worker and fall back to NewTrack internally,
            // but the analysis would still have the accumulated offset,
            // sending the worker insane position updates past EOF.  Reset
            // and start fresh; the reset keeps the history on screen.
            self.clear_early_continuation(ctx);
            self.reset_position_offset();
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
        let track_change_started_at = std::time::Instant::now();

        self.suppress_next_spectrogram_position_update = false;
        // A successor still being probed is not the one that follows now.
        self.pending_candidate = None;
        self.unconfirmed_continuation = None;
        self.worker_format_track = None;

        // Save previous compat params so the rolling gapless path can
        // detect incompatible transitions (e.g. 48 kHz/6ch → 44.1 kHz/2ch)
        // that would cause ContinueWithFile to fall back inside the worker.
        let prev_effective_rate = self.active_session_effective_rate;
        let prev_channel_count = self.active_session_channel_count;

        // A gapless successor's format was probed when it was staged, and a
        // track played again was probed before; the file helper probes it
        // again if the file has changed since. Otherwise the helper probes
        // it below, and the compatibility is not known yet.
        let cached = self.formats.get(&path);
        let format = match cached.map(|probed| probed.format) {
            // Read by the worker when it opened the file, or probed.
            Some(known @ TrackFormat::Known { .. }) => Some(known),
            cached_format => format_from_name(&path).or(cached_format),
        };
        if let Some(format) = format {
            self.apply_track_format(format);
        }
        // An early continuation was only sent for a compatible successor.
        let format_compatible = self.staged_continuation_path.as_deref() == Some(path.as_path())
            || match format {
                Some(TrackFormat::Known { .. }) => {
                    self.active_session_effective_rate == prev_effective_rate
                        && self.active_session_channel_count == prev_channel_count
                }
                // The worker finds the format when it opens the file, and
                // reports a continuation that does not fit, which then
                // starts afresh.
                Some(TrackFormat::Unknown) => true,
                None => false,
            };

        self.active_track_token = track_token;
        ctx.waveform_decode_active_token
            .store(track_token, Ordering::Relaxed);
        self.active_track_path = Some(path.clone());
        // Invalidate the duration published by the outgoing track's worker
        // session.  The new session's worker re-publishes after its file
        // probe; until then the centered window math treats it as unknown
        // instead of sizing windows against the previous track's length.
        ctx.spectrogram_track_duration_ms
            .store(0, Ordering::Relaxed);

        self.snapshot.waveform_peaks.clear();
        self.snapshot.waveform_coverage_seconds = 0.0;
        self.snapshot.waveform_complete = false;
        self.waveform_dirty = true;
        if reset_spectrogram {
            self.reset_spectrogram_state();
        }
        self.emit_snapshot(ctx.event_tx, true);

        if !self.spectrogram_active {
            self.last_spectrogram_position = 0.0;
            self.reset_position_offset();
        } else if gapless
            && !reset_spectrogram
            && self.display_mode == SpectrogramDisplayMode::Rolling
        {
            if format_compatible {
                self.unconfirmed_continuation =
                    Some((track_token, prev_effective_rate, prev_channel_count));
            }
            self.handle_rolling_gapless(&path, track_token, format_compatible, ctx);
        } else if gapless && !reset_spectrogram {
            self.handle_centered_gapless(&path, track_token, ctx);
        } else {
            // Non-gapless: cancel any stale staged continuation.
            self.clear_early_continuation(ctx);
            self.cancel_centered_staging();
            // Non-gapless: reset the offset since a new session starts
            // with fresh coordinates.
            self.reset_position_offset();
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

        // The outgoing position is needed above for rolling gapless offsets.
        // Subsequent settings/visibility restarts belong to the new track.
        self.last_spectrogram_position = 0.0;

        let probed_at = cached.and_then(|probed| probed.stamp);
        self.request_track_load(track_token, path, probed_at, ctx);

        profile_eprintln!(
            "[analysis] handle_track_change: completed in {:.2}ms",
            track_change_started_at.elapsed().as_secs_f64() * 1000.0,
        );
    }

    /// Centered mode gapless: show the pre-staged columns, if any, then
    /// start the worker from 0.  The new session continues the current
    /// generation: the outgoing session first publishes its decoded extent
    /// so Qt can shrink the old-track estimate in the short window before
    /// the new track's reset lands.  Without this, the centered-mode
    /// playhead stays pinned at center through the transition because the
    /// file-metadata estimate overshoots the true playback end by up to
    /// several seconds (especially visible at max zoom).
    fn handle_centered_gapless(
        &mut self,
        path: &Path,
        track_token: u64,
        ctx: &AnalysisContext<'_>,
    ) {
        self.clear_early_continuation(ctx);
        self.reset_position_offset();

        let staged = if self.staged_centered_path.as_deref() == Some(path) {
            self.take_staged_centered_chunks(track_token)
        } else {
            Vec::new()
        };
        self.cancel_centered_staging();

        // The staged chunks already perform the reset.  The worker
        // re-decodes from the beginning; its identical columns overwrite
        // staged data in the ring.
        let reset = staged.is_empty();
        profile_eprintln!(
            "[analysis] handle_track_change: centered gapless with {} staged chunks",
            staged.len(),
        );
        self.start_session(
            SessionStart {
                emit_initial_reset: reset,
                clear_history_on_reset: reset,
                continue_generation: true,
                handoff: SessionHandoff {
                    finalize_previous: true,
                    prelude: staged,
                },
                ..SessionStart::default()
            },
            ctx,
        );
    }

    /// Use a format the file helper found for the active track.
    fn apply_track_format(&mut self, format: TrackFormat) {
        let TrackFormat::Known {
            sample_rate,
            channels,
        } = format
        else {
            return;
        };
        self.active_session_effective_rate = sample_rate;
        self.active_session_channel_count = match self.spectrogram_view_mode {
            SpectrogramViewMode::Downmix => 1,
            SpectrogramViewMode::PerChannel => channels,
        };
    }

    /// Ask the file helper for the new track's stamp, its saved waveform,
    /// and its format unless the file still has `probed_at`. The waveform is
    /// decoded when the helper has none.
    fn request_track_load(
        &mut self,
        track_token: u64,
        path: PathBuf,
        probed_at: Option<WaveformSourceStamp>,
        ctx: &AnalysisContext<'_>,
    ) {
        self.pending_track_load = Some(track_token);
        let request = TrackIoRequest::LoadTrack {
            track_token,
            path,
            probed_at,
        };
        if self.track_io_lost || self.track_io.send(request).is_err() {
            self.handle_track_io_lost(ctx);
        }
    }

    fn handle_track_io_reply(&mut self, reply: TrackIoReply, ctx: &AnalysisContext<'_>) {
        match reply {
            TrackIoReply::TrackLoaded {
                track_token,
                stamp,
                format,
                peaks,
            } => self.handle_track_loaded(track_token, stamp, format, peaks, ctx),
            TrackIoReply::CandidateProbed {
                request,
                stamp,
                format,
            } => self.handle_candidate_probed(request, stamp, format, ctx),
        }
    }

    fn handle_track_loaded(
        &mut self,
        track_token: u64,
        stamp: Option<WaveformSourceStamp>,
        format: Option<TrackFormat>,
        peaks: Option<Vec<f32>>,
        ctx: &AnalysisContext<'_>,
    ) {
        if self.pending_track_load != Some(track_token) || track_token != self.active_track_token {
            return;
        }
        self.pending_track_load = None;
        // The path may have been renamed since the helper read it.
        let Some(path) = self.active_track_path.clone() else {
            return;
        };
        // The format the worker reported is the one it decodes.
        let format = format.filter(|_| self.worker_format_track != Some(track_token));
        if let Some(format) = format {
            self.formats.insert(path.clone(), format, stamp);
            self.apply_track_format(format);
        }
        if let Some((_, rate, channels)) = self.unconfirmed_continuation.take() {
            if (
                self.active_session_effective_rate,
                self.active_session_channel_count,
            ) != (rate, channels)
            {
                // The file changed after its format was read, and does not
                // fit the continued timeline.
                self.restart_changed_format(ctx);
            }
        }
        if let Some(peaks) = peaks {
            self.snapshot.waveform_peaks = peaks;
            self.snapshot.waveform_coverage_seconds = 0.0;
            self.snapshot.waveform_complete = true;
            self.waveform_dirty = true;
            self.emit_snapshot(ctx.event_tx, true);
            // The decode worker drops what it built for the last track.
            let _ = ctx.waveform_job_tx.send(WaveformDecodeJob {
                track_token,
                path,
                decode: false,
            });
            return;
        }
        let _ = ctx.waveform_job_tx.send(WaveformDecodeJob {
            track_token,
            path,
            decode: true,
        });
    }

    /// The file helper has stopped: tracks play on without saved
    /// waveforms or probes, and the user is told once.
    fn handle_track_io_lost(&mut self, ctx: &AnalysisContext<'_>) {
        if !self.track_io_lost {
            self.track_io_lost = true;
            let message = "Saved waveforms are unavailable because the analysis file \
                           worker stopped. Restart Ferrous to use them again.";
            eprintln!("[ferrous] {message}");
            let _ = ctx
                .event_tx
                .send(AnalysisEvent::WorkerFailed(message.to_string()));
        }
        self.pending_candidate = None;
        // Decode the waveform of a track whose load is lost.
        if let (Some(track_token), Some(path)) = (
            self.pending_track_load.take(),
            self.active_track_path.clone(),
        ) {
            if track_token == self.active_track_token {
                let _ = ctx.waveform_job_tx.send(WaveformDecodeJob {
                    track_token,
                    path,
                    decode: true,
                });
            }
        }
    }

    /// A rolling continuation's file does not fit the worker's timeline:
    /// the worker starts it afresh, so the position offset would send it
    /// past the end.  Start a session at the playback position, without
    /// the offset; its reset keeps the history shown.
    fn restart_changed_format(&mut self, ctx: &AnalysisContext<'_>) {
        self.clear_early_continuation(ctx);
        self.reset_position_offset();
        let position = self.last_spectrogram_position;
        self.start_spectrogram_session(position, true, false, ctx);
    }

    /// The worker went on with `path` at `start` seconds of its timeline:
    /// position 0 of the file is there. A track switch to it, before or
    /// after, offsets the file's positions by that.
    fn continuation_opened(&mut self, path: &Path, start: f64) {
        if self.provisional_continuation.as_deref() == Some(path) {
            profile_eprintln!(
                "[analysis] continuation opened at {start:.2}, offset was {:.2}",
                self.spectrogram_position_offset,
            );
            self.provisional_continuation = None;
            self.spectrogram_position_offset = start;
        } else if self.staged_continuation_path.as_deref() == Some(path) {
            self.staged_continuation_start = Some((path.to_path_buf(), start));
        }
    }

    fn handle_session_report(&mut self, report: SessionReport, ctx: &AnalysisContext<'_>) {
        // A newer session supersedes what an older one reports.
        if report.generation() != ctx.spectrogram_decode_generation.load(Ordering::Relaxed) {
            return;
        }
        match report {
            SessionReport::Opened {
                track_token,
                path,
                stamp,
                format,
                continued_at,
                ..
            } => {
                if let Some(start) = continued_at {
                    self.continuation_opened(&path, start);
                }
                let active = track_token == self.active_track_token
                    && self.active_track_path.as_deref() == Some(path.as_path());
                self.formats.insert(path, format, stamp);
                if active {
                    // The worker decodes the file: a continued timeline it
                    // accepted fits, so the helper's reply need not confirm it.
                    self.worker_format_track = Some(track_token);
                    self.unconfirmed_continuation = None;
                    self.apply_track_format(format);
                }
            }
            SessionReport::Repositioned { start_seconds, .. } => {
                self.spectrogram_session_start = start_seconds;
            }
            SessionReport::ContinuationRejected {
                path,
                stamp,
                format,
                ..
            } => {
                if let Some(format) = format {
                    self.formats.insert(path.clone(), format, stamp);
                }
                if self.staged_continuation_path.as_deref() == Some(path.as_path()) {
                    // Rejected before the switch, which judges the format
                    // again.
                    self.staged_continuation_path = None;
                    self.staged_continuation_start = None;
                } else if self.active_track_path.as_deref() == Some(path.as_path())
                    && self.display_mode == SpectrogramDisplayMode::Rolling
                {
                    if let Some(format) = format {
                        self.worker_format_track = Some(self.active_track_token);
                        self.apply_track_format(format);
                    }
                    self.restart_changed_format(ctx);
                }
            }
            SessionReport::Failed {
                track_token,
                path,
                error,
                ..
            } => {
                if track_token != self.active_track_token {
                    return;
                }
                let name = path
                    .file_name()
                    .map_or_else(|| path.to_string_lossy(), |name| name.to_string_lossy());
                let message = format!("The spectrogram cannot show {name}: {error}.");
                let _ = ctx.event_tx.send(AnalysisEvent::WorkerFailed(message));
            }
        }
    }

    fn start_spectrogram_session(
        &mut self,
        start_seconds: f64,
        emit_initial_reset: bool,
        clear_history_on_reset: bool,
        ctx: &AnalysisContext<'_>,
    ) {
        self.start_spectrogram_session_with_target(
            start_seconds,
            start_seconds,
            emit_initial_reset,
            clear_history_on_reset,
            ctx,
        );
    }

    fn start_spectrogram_session_with_target(
        &mut self,
        start_seconds: f64,
        target_position_seconds: f64,
        emit_initial_reset: bool,
        clear_history_on_reset: bool,
        ctx: &AnalysisContext<'_>,
    ) {
        self.start_session(
            SessionStart {
                start_seconds,
                target_position_seconds,
                emit_initial_reset,
                clear_history_on_reset,
                ..SessionStart::default()
            },
            ctx,
        );
    }

    /// Start a worker session for the active track. This thread is the only
    /// one that creates generations; a new one makes everything decoded for
    /// the previous session stale.
    fn start_session(&mut self, start: SessionStart, ctx: &AnalysisContext<'_>) {
        if !self.spectrogram_active {
            return;
        }
        let Some(path) = self.active_track_path.clone() else {
            profile_eprintln!(
                "[analysis] start_spectrogram_session: no active_track_path, skipping"
            );
            return;
        };
        // NewTrack starts a fresh worker timeline. Any rolling-mode gapless
        // offset must be cleared first, otherwise the next PositionUpdate is
        // re-adjusted into the old continuous coordinate space and the worker
        // seeks far forward immediately after the restart.
        self.reset_position_offset();
        self.unconfirmed_continuation = None;
        self.spectrogram_session_start = start.start_seconds;
        self.spectrogram_session_widget_width = self.spectrogram_widget_width;
        self.spectrogram_session_margin = self.centered_margin_seconds();
        let generation = ctx.spectrogram_decode_generation.load(Ordering::Relaxed);
        // Generation 0 belongs to no session, so a first session always
        // takes a new one.
        let generation = if start.continue_generation && generation != 0 {
            generation
        } else {
            ctx.spectrogram_decode_generation
                .fetch_add(1, Ordering::Relaxed)
                + 1
        };
        let mut handoff = start.handoff;
        for chunk in &mut handoff.prelude {
            chunk.generation = generation;
            chunk.widget_width = self.spectrogram_session_widget_width;
        }
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::NewTrack {
                track_token: self.active_track_token,
                generation,
                path,
                fft_size: self.fft_size,
                hop_size: self.hop_size,
                zoom_level: self.zoom_level,
                widget_width: self.spectrogram_session_widget_width,
                channel_count: self.active_session_channel_count.max(1),
                start_seconds: start.start_seconds,
                target_position_seconds: start.target_position_seconds,
                emit_initial_reset: start.emit_initial_reset,
                clear_history_on_reset: start.clear_history_on_reset,
                view_mode: self.spectrogram_view_mode,
                display_mode: self.display_mode,
                handoff,
            });
    }

    /// Track duration (seconds) of the active worker session's file, as
    /// published by the decode worker after its file probe.  Returns 0.0
    /// when unknown (no session yet, probe pending, or degenerate
    /// estimate) so callers can fall back to playhead-relative margins.
    fn centered_track_duration_seconds(ctx: &AnalysisContext<'_>) -> f64 {
        let ms = ctx.spectrogram_track_duration_ms.load(Ordering::Relaxed);
        // Track durations are far below f64's exact-integer range, so the
        // u64→f64 conversion loses no meaningful precision.
        #[allow(clippy::cast_precision_loss)]
        let seconds = ms as f64 / 1000.0;
        seconds
    }

    /// Model the Qt centered display's visible left edge (seconds) for a
    /// playhead at `position_seconds`, given the full decode margin
    /// (`centered_margin_seconds()`, i.e. visible window + 2 s warmup).
    ///
    /// The display centers the window on the playhead, but two clamps can
    /// pull the left edge further back than half a window:
    ///
    /// - Full-track lock: when the visible window covers >= 90% of the
    ///   track estimate, Qt pins the display to the full track extent
    ///   (left edge at 0).
    /// - EOF detach: once the remaining tail is shorter than half a
    ///   window, the right edge clamps to the decoded end and the left
    ///   edge sits a full window behind the track end.
    ///
    /// See [`centered_view::visible_left_edge_seconds`], which is tested
    /// against the same cases as the Qt display.
    ///
    /// Decode windows must cover this edge or the zoomed-out view shows
    /// black to the left of the decoded content near the track end.
    fn centered_visible_left_edge_seconds(
        position_seconds: f64,
        full_margin: f64,
        ctx: &AnalysisContext<'_>,
    ) -> f64 {
        centered_view::visible_left_edge_seconds(
            position_seconds,
            (full_margin - centered_view::WARMUP_SECONDS).max(0.0),
            Self::centered_track_duration_seconds(ctx),
        )
    }

    /// Start position (seconds) for a centered-mode spectrogram session
    /// restart.  Covers the worst-case visible left edge (see
    /// [`Self::centered_visible_left_edge_seconds`]) plus the STFT warmup
    /// buffer, so playback from the restart position all the way to EOF
    /// never exposes undecoded black at the left of the viewport.
    fn spectrogram_restart_start_seconds(
        &self,
        position_seconds: f64,
        ctx: &AnalysisContext<'_>,
    ) -> f64 {
        if self.display_mode != SpectrogramDisplayMode::Centered {
            return position_seconds;
        }
        let full_margin = self.centered_margin_seconds();
        (Self::centered_visible_left_edge_seconds(position_seconds, full_margin, ctx)
            - centered_view::WARMUP_SECONDS)
            .max(0.0)
    }

    fn effective_hop_for_current_zoom(&self) -> Option<usize> {
        if self.hop_size == 0 {
            return None;
        }
        if self.zoom_level > 1.0 {
            return Some(self.hop_size);
        }
        let base_hop = (self.fft_size / 8).max(64);
        let zoom = f64::from(self.zoom_level.max(0.001));
        #[allow(clippy::cast_precision_loss)]
        let target_effective = REFERENCE_HOP as f64 / zoom;
        #[allow(clippy::cast_precision_loss)]
        let interval = (target_effective / base_hop as f64).max(1.0);
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            clippy::cast_precision_loss
        )]
        let hop = (base_hop as f64 * interval).round() as usize;
        Some(hop)
    }

    fn current_centered_cols_per_second(&self) -> Option<f64> {
        let effective_hop = self.effective_hop_for_current_zoom()?;
        let rate = if self.active_session_effective_rate > 0 {
            self.active_session_effective_rate
        } else if self.snapshot.sample_rate_hz > 0 {
            self.snapshot.sample_rate_hz
        } else {
            0
        };
        if rate == 0 || effective_hop == 0 {
            return None;
        }
        #[allow(clippy::cast_precision_loss)]
        let cols_per_second = f64::from(rate) / effective_hop as f64;
        Some(cols_per_second)
    }

    fn centered_ring_capacity_columns(&self) -> Option<u64> {
        self.current_centered_cols_per_second()?;
        let config = retention::SpectrogramBufferConfig {
            width: self.spectrogram_session_widget_width,
            sample_rate: if self.active_session_effective_rate > 0 {
                self.active_session_effective_rate
            } else {
                self.snapshot.sample_rate_hz
            },
            hop: u32::try_from(self.effective_hop_for_current_zoom()?).ok()?,
            bins: u32::try_from(self.fft_size / 2 + 1).ok()?,
            channels: u32::try_from(self.active_session_channel_count.max(1)).ok()?,
            zoom: f64::from(self.zoom_level),
            centered: true,
        };
        Some(config.limits(retention::lookahead_seconds()).capacity)
    }

    fn centered_retained_start_seconds(&self, ctx: &AnalysisContext<'_>) -> f64 {
        if let (Some(cols_per_second), Some(ring_capacity)) = (
            self.current_centered_cols_per_second(),
            self.centered_ring_capacity_columns(),
        ) {
            let produced = ctx
                .spectrogram_decode_columns_produced
                .load(Ordering::Relaxed);
            let oldest_retained_col = produced.saturating_sub(ring_capacity);
            // Column-to-seconds conversion only needs approximate
            // floating-point precision for seek-window comparisons.
            #[allow(clippy::cast_precision_loss)]
            let oldest_retained_seconds = oldest_retained_col as f64 / cols_per_second;
            oldest_retained_seconds.max(self.spectrogram_session_start)
        } else {
            self.spectrogram_session_start
        }
    }

    /// Compute the pre-decode margin for centered mode: how many seconds
    /// before the playhead to start decoding.  Based on the actual widget
    /// width, sample rate, zoom level, plus a small buffer.
    fn centered_margin_seconds(&self) -> f64 {
        if let (Some(effective_hop), Some(cols_per_second)) = (
            self.effective_hop_for_current_zoom(),
            self.current_centered_cols_per_second(),
        ) {
            // effective_hop and REFERENCE_HOP are hop sizes (small usize);
            // precision loss from usize→f64 is negligible for these values.
            // The view as wide as it is now. A seek judges the live
            // session's window by the larger of this and the margin the
            // session decodes for (`spectrogram_session_margin`).
            let width = self.spectrogram_widget_width;
            // Full screen width in seconds plus 2 s for STFT warmup.
            // A full width (not half) is needed because near the end of
            // a track the playhead detaches from center and moves right,
            // making the visible left edge up to one full screen width
            // behind the playhead position.
            centered_view::window_seconds(width, self.zoom_level, effective_hop, cols_per_second)
                + centered_view::WARMUP_SECONDS
        } else {
            // No rate info yet (first track load) — use generous fallback.
            30.0
        }
    }

    fn update_spectrogram_position(&mut self, position_seconds: f64, ctx: &AnalysisContext<'_>) {
        self.last_spectrogram_position = position_seconds;
        if !self.spectrogram_active {
            return;
        }
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
        // The worker may decode a file other than the one the seek is in:
        // the staged successor, which it goes on with before the switch, or
        // the outgoing track, before it reaches the successor playback has
        // switched to.
        let continuation_unsettled =
            self.staged_continuation_path.is_some() || self.provisional_continuation.is_some();
        self.reset_position_offset();
        self.unconfirmed_continuation = None;
        self.last_spectrogram_position = position_seconds;
        if !self.spectrogram_active {
            return;
        }

        if self.display_mode == SpectrogramDisplayMode::Centered {
            // Windowed centered: check if the seek target is within the
            // window the session decodes before restarting it.  The
            // window extends from the session start for approximately
            // 2× the visible screen width plus lookahead (~10 s).  If
            // the seek is inside, the session takes it: it keeps its
            // columns when what it has decoded covers the view, and
            // otherwise seeks to the view itself, without a restart.
            // Only the worker knows what it has decoded so far, which
            // after a recent restart can be far less than the window.
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

            // Model the Qt display's visible left edge (full-track lock /
            // EOF detach aware) rather than assuming the playhead stays
            // centered: near the track end the left edge sits up to a full
            // window behind the playhead, and at deep zoom-out the display
            // pins to the track start.  Checking against that edge keeps
            // seeks near EOF from reusing a window that would render black
            // on its left.
            let visible_left =
                Self::centered_visible_left_edge_seconds(position_seconds, margin, ctx);
            let retained_window_start = self.centered_retained_start_seconds(ctx);
            if visible_left >= retained_window_start && position_seconds <= window_end {
                // A seek, not a position: a position the session has not
                // decoded up to yet would have it decode every column
                // before the playhead's.
                let _ = ctx
                    .spectrogram_cmd_tx
                    .send(SpectrogramWorkerCommand::Seek { position_seconds });
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
                self.refill_centered_spectrogram(position_seconds, ctx);
            }
        } else if ctx.event_tx.spectral_backpressured() || continuation_unsettled {
            // The worker is waiting to hand over columns decoded before the
            // seek, so a Seek would reach it, and its reset the UI, only
            // after they drain.  A new session at the target makes them
            // stale instead, and opens the file the seek is in.  Like the
            // Seek's reset, its reset keeps the history on screen: rolling
            // history is what has played.
            self.clear_early_continuation(ctx);
            self.start_spectrogram_session(position_seconds, true, false, ctx);
        } else {
            // Rolling mode: an explicit seek breaks the continuous gapless
            // timeline.  Send a Seek command (existing behavior).
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::Seek { position_seconds });
        }
    }

    fn refill_centered_spectrogram(&mut self, position_seconds: f64, ctx: &AnalysisContext<'_>) {
        // The worker emits this reset before it opens the file, so the
        // frontend clears the ring before the next render frame and old
        // ring data is not briefly visible at the new playhead.
        let clear_ring = PrecomputedSpectrogramChunk {
            track_token: self.active_track_token,
            generation: 0,
            columns_u8: Vec::new(),
            bins_per_column: 0,
            column_count: 0,
            channel_count: u8::try_from(self.active_session_channel_count.max(1))
                .unwrap_or(u8::MAX),
            start_column_index: 0,
            total_columns_estimate: 0,
            sample_rate_hz: 0,
            hop_size: 0,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: true,
            widget_width: 0,
        };
        let start = self.spectrogram_restart_start_seconds(position_seconds, ctx);
        self.start_session(
            SessionStart {
                start_seconds: start,
                target_position_seconds: position_seconds,
                emit_initial_reset: true,
                clear_history_on_reset: true,
                handoff: SessionHandoff {
                    prelude: vec![clear_ring],
                    ..SessionHandoff::default()
                },
                ..SessionStart::default()
            },
            ctx,
        );
        // Suppress the next PositionUpdate to prevent a race: the
        // playback snapshot may send a PositionUpdate at the new
        // position before the worker processes our NewTrack.
        self.suppress_next_spectrogram_position_update = true;
    }

    fn handle_waveform_progress(
        &mut self,
        track_token: u64,
        update: WaveformUpdate,
        event_tx: &dyn AnalysisEventOutput,
    ) {
        let WaveformUpdate {
            peaks,
            coverage_seconds,
            complete,
            done,
            source,
        } = update;
        if track_token != self.active_track_token || peaks.is_empty() {
            return;
        }

        self.snapshot.waveform_peaks = peaks;
        self.snapshot.waveform_coverage_seconds = coverage_seconds;
        self.snapshot.waveform_complete = complete;
        // A decode stopped by an error shows what it covered, and the next
        // play decodes the file again.
        if done && complete {
            self.save_active_waveform(source);
        }
        self.waveform_dirty = true;
        if done || self.snapshot.waveform_peaks.len() >= MIN_PREVIEW_PEAKS {
            self.emit_snapshot(event_tx, true);
        }
    }

    /// Saves the waveform decoded from the file of stamp `source`: the file
    /// at the track's path may have been another when the track was loaded,
    /// as when a swap came between.
    fn save_active_waveform(&self, source: Option<WaveformSourceStamp>) {
        let Some(path) = self.active_track_path.clone() else {
            return;
        };
        let _ = self.track_io.send(TrackIoRequest::SaveWaveform {
            path,
            stamp: source,
            peaks: self.snapshot.waveform_peaks.clone(),
            coverage_seconds: self.snapshot.waveform_coverage_seconds,
        });
    }

    fn reset_spectrogram_state(&mut self) {
        self.pending_channels.clear();
        self.suppress_next_spectrogram_position_update = false;
    }

    fn emit_snapshot(&mut self, event_tx: &dyn AnalysisEventOutput, force: bool) {
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
    closed: Arc<AtomicBool>,
) -> Option<JoinableThread> {
    crate::worker_thread::spawn_worker("ferrous-waveform-decode", move || {
        crate::worker_thread::lower_current_thread_priority();
        while let Ok(mut job) = waveform_job_rx.recv() {
            while let Ok(next_job) = waveform_job_rx.try_recv() {
                job = next_job;
            }
            // What the detail service holds of another track, and nothing
            // reads, is no longer needed: such as what the last decode
            // built for it, if no session took it.
            waveform_service::track_changed(&job.path);
            if !job.decode {
                continue;
            }
            let track_token = job.track_token;
            let superseded = || {
                closed.load(Ordering::Relaxed)
                    || waveform_decode_active_token.load(Ordering::Relaxed) != track_token
            };
            decode_overview_job(
                &job.path,
                &superseded,
                |update| {
                    let _ = waveform_tx.send(AnalysisCommand::WaveformProgress {
                        track_token,
                        update,
                    });
                },
                waveform_service::offer_prefill,
            );
        }
    })
}

/// Decode the overview of the file at `path` for a track, and `send` its
/// updates until it is `superseded`. A decode that reached the end of the
/// file, for a track not superseded by then, also builds the coarse
/// summaries of the waveform detail service and `offer`s them, before it
/// sends its last update: a whole-track window asked for once the overview
/// is complete finds them.
fn decode_overview_job(
    path: &Path,
    superseded: &dyn Fn() -> bool,
    mut send: impl FnMut(WaveformUpdate),
    offer: impl FnOnce(waveform_prefill::CoarsePrefill),
) {
    let source = Cell::new(None);
    let mut offer = Some(offer);
    // A track superseded after this check has its summaries dropped by the
    // next job, which the next track sends.
    let mut sink = |prefill| {
        if let Some(offer) = offer.take().filter(|_| !superseded()) {
            offer(prefill);
        }
    };
    let mut prefill = PrefillSlot::offering(&mut sink);
    let _ = decode_waveform_peaks_stream(
        path,
        1024,
        &source,
        &mut prefill,
        |peaks, coverage_seconds, progress| {
            if superseded() {
                return false;
            }
            send(WaveformUpdate::of(
                peaks,
                coverage_seconds,
                progress,
                source.get(),
            ));
            true
        },
        superseded,
    );
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
    event_tx: AnalysisOutputs,
    waveform_job_tx: Sender<WaveformDecodeJob>,
    waveform_decode_active_token: Arc<AtomicU64>,
    track_io: TrackIoChannels,
    spectrogram: SpectrogramWorkerHandles,
) -> Option<JoinableThread> {
    crate::worker_thread::spawn_worker("ferrous-analysis", move || {
        let mut state = AnalysisRuntimeState::with_track_io(track_io.requests);
        state.spectrogram_stops = Arc::clone(&spectrogram.stops);
        let mut track_io_replies = track_io.replies;
        let mut session_reports = spectrogram.reports;
        let mut spectrogram_loss_reported = false;
        report_lost_spectrogram_worker(
            &spectrogram.status,
            &mut spectrogram_loss_reported,
            &event_tx,
        );
        loop {
            let ctx = AnalysisContext {
                event_tx: &event_tx,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: waveform_decode_active_token.as_ref(),
                spectrogram_cmd_tx: &spectrogram.cmd_tx,
                spectrogram_decode_generation: spectrogram.decode_generation.as_ref(),
                spectrogram_decode_columns_produced: spectrogram.columns_produced.as_ref(),
                spectrogram_track_duration_ms: spectrogram.track_duration_ms.as_ref(),
            };
            crossbeam_channel::select! {
                recv(cmd_rx) -> cmd => {
                    let Ok(cmd) = cmd else {
                        return;
                    };
                    event_tx.begin_command(&cmd);
                    let shutdown = matches!(cmd, AnalysisCommand::Shutdown);
                    state.handle_command(cmd, &ctx);
                    if shutdown {
                        // Returning drops the senders the other workers wait
                        // on. The spectrogram worker ends with them, so its
                        // status is not checked again: that is not a failure.
                        return;
                    }
                }
                recv(track_io_replies) -> reply => {
                    if let Ok(reply) = reply {
                        state.handle_track_io_reply(reply, &ctx);
                    } else {
                        track_io_replies = crossbeam_channel::never();
                        state.handle_track_io_lost(&ctx);
                    }
                }
                recv(session_reports) -> report => {
                    if let Ok(report) = report {
                        state.handle_session_report(report, &ctx);
                    } else {
                        // The worker is gone; its status says why.
                        session_reports = crossbeam_channel::never();
                    }
                }
            }
            report_lost_spectrogram_worker(
                &spectrogram.status,
                &mut spectrogram_loss_reported,
                &event_tx,
            );
        }
    })
}

/// Tell the user, once, that the spectrogram decode worker is gone: the
/// commands sent to it fail without a word, so the spectrogram would just
/// stay empty.
fn report_lost_spectrogram_worker(
    status: &SpectrogramWorkerStatus,
    reported: &mut bool,
    event_tx: &dyn AnalysisEventOutput,
) {
    if *reported {
        return;
    }
    let Some(reason) = status.stopped() else {
        return;
    };
    *reported = true;
    let message = format!(
        "The spectrogram is unavailable because {reason}. Restart Ferrous to use it again."
    );
    eprintln!("[ferrous] {message}");
    let _ = event_tx.send(AnalysisEvent::WorkerFailed(message));
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
    event_tx: &dyn AnalysisEventOutput,
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

/// Decode the overview of the file at `path`. `source` tells, for each
/// update, the stamp of the file the decode reads, or `None` if not known.
fn decode_waveform_peaks_stream<F, C>(
    path: &Path,
    max_points: usize,
    source: &Cell<Option<WaveformSourceStamp>>,
    prefill: &mut PrefillSlot<'_>,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, OverviewProgress) -> bool,
    C: FnMut() -> bool,
{
    source.set(None);
    #[cfg(feature = "gst")]
    if is_raw_surround_file(path) {
        return decode_waveform_peaks_stream_gst(
            path,
            max_points,
            source,
            prefill,
            on_update,
            is_cancelled,
        );
    }

    let unreadable = match File::open(path)
        .map_err(anyhow::Error::from)
        .and_then(|file| {
            // The file this handle reads, whichever is at the path later.
            let metadata = file.metadata().ok();
            source.set(metadata.as_ref().and_then(stamp_of));
            prefill.opened(path, metadata);
            decode_waveform_peaks_symphonia(
                Box::new(file),
                path.extension().and_then(|e| e.to_str()),
                max_points,
                prefill,
                &mut on_update,
                &mut is_cancelled,
            )
        }) {
        Ok(true) => return Ok(()),
        Ok(false) => anyhow::anyhow!("no audio could be decoded"),
        Err(error) => error,
    };
    // Symphonia has no demuxer or decoder for the file (Opus, WavPack, and
    // others GStreamer plays), or decoded none of its audio.
    #[cfg(feature = "gst")]
    {
        let _ = unreadable;
        if is_cancelled() {
            return Ok(());
        }
        decode_waveform_peaks_stream_gst(path, max_points, source, prefill, on_update, is_cancelled)
    }
    #[cfg(not(feature = "gst"))]
    Err(unreadable)
}

/// Decode the overview with Symphonia. Returns whether it decoded audio
/// or was cancelled; `false` or an error means it could not read the file.
///
/// The last update is [`OverviewProgress::Complete`] only if the decode
/// reached the end of the file. An error that stops it before, such as a
/// read error, ends it with [`OverviewProgress::Truncated`] and the peaks of
/// the audio before the error. Only a decode that reaches the end builds
/// the summaries `prefill` asks for.
fn decode_waveform_peaks_symphonia<F, C>(
    source: Box<dyn MediaSource>,
    extension: Option<&str>,
    max_points: usize,
    prefill: &mut PrefillSlot<'_>,
    on_update: &mut F,
    is_cancelled: &mut C,
) -> anyhow::Result<bool>
where
    F: FnMut(Vec<f32>, f32, OverviewProgress) -> bool,
    C: FnMut() -> bool,
{
    if is_cancelled() {
        return Ok(true);
    }
    let mut hint = Hint::new();
    if let Some(ext) = extension {
        hint.with_extension(ext);
    }

    let mss = MediaSourceStream::new(source, MediaSourceStreamOptions::default());

    let mut format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &analysis_format_options(),
            &MetadataOptions::default(),
        )?
        .format;

    let track = format
        .default_track()
        .ok_or_else(|| anyhow::anyhow!("no default track"))?;
    let track_id = track.id;

    let mut audio_decoder =
        symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default())?;
    let (sample_rate_hz, track_channels) = symphonia_track_shape(&track.codec_params);
    let estimated_frames = track.codec_params.n_frames.unwrap_or(sample_rate_hz * 240);
    // The detail service reads the file with the same reader and decoder.
    let mut builder = prefill
        .builder(sample_rate_hz, track_channels, false)
        .map(|builder| builder.expecting(track.codec_params.n_frames));

    let mut sample_buf: Option<SampleBuffer<f32>> = None;
    let mut waveform = WaveformAccumulator::new(max_points, estimated_frames, sample_rate_hz);

    let mut decoded_any = false;
    let reached_end = loop {
        if is_cancelled() {
            return Ok(true);
        }
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                break true;
            }
            // Any other error stops the decode short of the end. That
            // includes a reset at the next link of a chained Ogg file:
            // Symphonia 0.5 would go on with the last link, not the next.
            Err(_) => break false,
        };

        if packet.track_id() != track_id {
            continue;
        }

        let decoded_audio = match audio_decoder.decode(&packet) {
            Ok(decoded_audio) => decoded_audio,
            // A damaged packet; the next one may decode.
            Err(SymphoniaError::DecodeError(_)) => continue,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                // The detail service stops at this error.
                if let Some(builder) = builder.as_mut() {
                    builder.fail();
                }
                continue;
            }
            Err(_) => break false,
        };

        let spec = *decoded_audio.spec();
        let channels = spec.channels.count().max(1);
        let decoded_capacity = decoded_audio.capacity();
        let buf = ensure_sample_buffer(&mut sample_buf, decoded_capacity, spec);
        buf.copy_interleaved_ref(decoded_audio);

        let samples = buf.samples();
        decoded_any |= !samples.is_empty();
        if let Some(builder) = builder.as_mut() {
            let first_frame = packet_first_frame(format.as_ref(), track_id, packet.ts(), spec.rate);
            builder.push(first_frame, samples, channels);
        }
        if !waveform.push_frames(samples, channels, on_update, is_cancelled) {
            return Ok(true);
        }
    };

    if !decoded_any {
        return Ok(false);
    }
    if reached_end {
        prefill.finished(builder);
    }
    if !is_cancelled() {
        let progress = if reached_end {
            OverviewProgress::Complete
        } else {
            OverviewProgress::Truncated
        };
        let coverage_seconds = waveform.coverage_seconds();
        let _ = on_update(waveform.finish(), coverage_seconds, progress);
    }
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;
    use std::io::Write as IoWrite;
    use std::time::Duration;

    #[test]
    fn overview_preserves_transients_at_every_sample_offset() {
        use std::io::{Seek, SeekFrom};
        for rate in [48_000, 192_000] {
            let path = write_engine_test_wave(1, rate);
            let mut file = std::fs::OpenOptions::new()
                .write(true)
                .open(&path)
                .expect("fixture");
            file.seek(SeekFrom::Start(44)).expect("PCM start");
            file.write_all(&vec![0_u8; rate as usize * 2])
                .expect("silence");
            for frame in 0..16 {
                file.seek(SeekFrom::Start(44 + frame * 2))
                    .expect("transient position");
                file.write_all(&30_000_i16.to_le_bytes())
                    .expect("transient");
                let mut maximum = 0.0_f32;
                decode_waveform_peaks_stream(
                    &path,
                    1024,
                    &Cell::new(None),
                    &mut PrefillSlot::default(),
                    |peaks, _, progress| {
                        if progress == OverviewProgress::Complete {
                            maximum = peaks.iter().copied().fold(0.0_f32, f32::max);
                        }
                        true
                    },
                    || false,
                )
                .expect("overview decode");
                assert!(maximum > 0.91, "missing frame {frame} at {rate} Hz");
                file.seek(SeekFrom::Start(44 + frame * 2))
                    .expect("clear position");
                file.write_all(&0_i16.to_le_bytes())
                    .expect("clear transient");
            }
            drop(file);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    /// A temporary copy of `bytes` named `name`.
    fn write_fixture(name: &str, bytes: &[u8]) -> PathBuf {
        static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "ferrous-analysis-{}-{}-{name}",
            std::process::id(),
            NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::write(&path, bytes).expect("write fixture");
        path
    }

    /// A 0.1 s mono 440 Hz tone at 8 kHz as 16-bit AIFF.
    fn aiff_tone() -> Vec<u8> {
        let rate: u32 = 8_000;
        let frames = rate / 10;
        let mut samples = Vec::new();
        for frame in 0..frames {
            let t = f64::from(frame) / f64::from(rate);
            let value = ((t * 440.0 * std::f64::consts::TAU).sin() * 16_000.0) as i16;
            samples.extend_from_slice(&value.to_be_bytes());
        }
        // The rate as an 80-bit extended float: 8000 = 1.953125 × 2^12.
        let mut rate_bytes = (16_383_u16 + 12).to_be_bytes().to_vec();
        rate_bytes.extend_from_slice(&(u64::from(rate) << (63 - 12)).to_be_bytes());
        let mut comm = Vec::new();
        comm.extend_from_slice(&1_u16.to_be_bytes());
        comm.extend_from_slice(&frames.to_be_bytes());
        comm.extend_from_slice(&16_u16.to_be_bytes());
        comm.extend_from_slice(&rate_bytes);
        let ssnd_size = u32::try_from(8 + samples.len()).unwrap();
        let mut body = b"AIFFCOMM".to_vec();
        body.extend_from_slice(&u32::try_from(comm.len()).unwrap().to_be_bytes());
        body.extend_from_slice(&comm);
        body.extend_from_slice(b"SSND");
        body.extend_from_slice(&ssnd_size.to_be_bytes());
        body.extend_from_slice(&[0; 8]);
        body.extend_from_slice(&samples);
        let mut bytes = b"FORM".to_vec();
        bytes.extend_from_slice(&u32::try_from(body.len()).unwrap().to_be_bytes());
        bytes.extend_from_slice(&body);
        bytes
    }

    /// The overview the waveform worker publishes for `path`, if any.
    fn overview(path: &Path) -> Option<Vec<f32>> {
        let mut overview = None;
        let _ = decode_waveform_peaks_stream(
            path,
            1024,
            &Cell::new(None),
            &mut PrefillSlot::default(),
            |peaks, _, progress| {
                if progress == OverviewProgress::Complete {
                    overview = Some(peaks);
                }
                true
            },
            || false,
        );
        overview
    }

    #[test]
    fn overviews_cover_alac_and_aiff() {
        for (name, bytes) in [
            ("tone.m4a", include_bytes!("fixtures/tag-tone.m4a").to_vec()),
            ("tone.aiff", aiff_tone()),
        ] {
            let path = write_fixture(name, &bytes);
            let peaks = overview(&path).unwrap_or_else(|| panic!("no overview of {name}"));
            let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
            assert!(loudest > 0.05, "{name}: {loudest}");
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    /// Symphonia has no Opus or WavPack decoder, and reads nothing from a
    /// WAV whose data chunk claims no data, which GStreamer reads to the end.
    #[cfg(feature = "gst")]
    #[test]
    fn overviews_fall_back_to_gstreamer_for_files_symphonia_cannot_decode() {
        let wave = write_engine_test_wave(1, 8_000);
        let mut unsized_wave = std::fs::read(&wave).expect("read fixture");
        unsized_wave[40..44].copy_from_slice(&0_u32.to_le_bytes());
        std::fs::remove_file(wave).expect("remove fixture");
        for (name, bytes) in [
            (
                "tone.opus",
                include_bytes!("fixtures/tag-tone.opus").to_vec(),
            ),
            ("tone.wv", include_bytes!("fixtures/tag-tone.wv").to_vec()),
            ("unsized.wav", unsized_wave),
        ] {
            let path = write_fixture(name, &bytes);
            let peaks = overview(&path).unwrap_or_else(|| panic!("no overview of {name}"));
            let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
            assert!(loudest > 0.05, "{name}: {loudest}");

            // The detail view and level meter decode it as well.
            let window = decode_waveform_window_cancellable(&path, 0.0, 0.1, 64, &|| false)
                .unwrap_or_else(|error| panic!("{name}: {error}"));
            assert!(
                window.extrema.iter().any(|&value| value > 0.05),
                "{name}: {:?}",
                window.extrema
            );
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    /// Write a mono 16-bit PCM WAV and return its path.  Used by the
    /// end-to-end engine tests below to drive the real decode worker.
    fn write_engine_test_wave(seconds: u32, sample_rate: u32) -> std::path::PathBuf {
        let samples_len = usize::try_from(seconds * sample_rate).unwrap_or(0);
        static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(0);
        let mut path = std::env::temp_dir();
        path.push(format!(
            "ferrous-analysis-engine-{}-{}.wav",
            std::process::id(),
            NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed)
        ));
        let data_bytes = u32::try_from(samples_len.saturating_mul(2)).unwrap();
        let mut file = std::fs::File::create(&path).unwrap();
        file.write_all(b"RIFF").unwrap();
        file.write_all(&(36 + data_bytes).to_le_bytes()).unwrap();
        file.write_all(b"WAVEfmt ").unwrap();
        file.write_all(&16_u32.to_le_bytes()).unwrap();
        file.write_all(&1_u16.to_le_bytes()).unwrap();
        file.write_all(&1_u16.to_le_bytes()).unwrap();
        file.write_all(&sample_rate.to_le_bytes()).unwrap();
        file.write_all(&(sample_rate * 2).to_le_bytes()).unwrap();
        file.write_all(&2_u16.to_le_bytes()).unwrap();
        file.write_all(&16_u16.to_le_bytes()).unwrap();
        file.write_all(b"data").unwrap();
        file.write_all(&data_bytes.to_le_bytes()).unwrap();
        for i in 0..samples_len {
            let t = i as f32 / sample_rate as f32;
            let sample = (t * 440.0).sin() * 0.5;
            let value = (sample * 32_767.0) as i16;
            file.write_all(&value.to_le_bytes()).unwrap();
        }
        path
    }

    fn drain_events(rx: &Receiver<AnalysisEvent>, dur: Duration) -> Vec<AnalysisEvent> {
        let mut out = Vec::new();
        let deadline = std::time::Instant::now() + dur;
        loop {
            let now = std::time::Instant::now();
            if now >= deadline {
                break;
            }
            match rx.recv_timeout(deadline - now) {
                Ok(event) => out.push(event),
                Err(_) => break,
            }
        }
        out
    }

    fn summarize(events: &[AnalysisEvent]) -> String {
        events
            .iter()
            .map(|e| match e {
                AnalysisEvent::PrecomputedSpectrogramChunk(c) => format!(
                    "chunk(cols={}, start={}, est={}, hop={}, reset={}, clear={}, complete={})",
                    c.column_count,
                    c.start_column_index,
                    c.total_columns_estimate,
                    c.hop_size,
                    c.buffer_reset,
                    c.clear_history,
                    c.complete,
                ),
                AnalysisEvent::Snapshot(_) => "snapshot".to_string(),
                AnalysisEvent::WorkerFailed(message) => format!("failed({message})"),
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// Every event left in `rx`. Fails if a sender remains, which means a
    /// thread holding one is still running.
    fn events_after_threads_ended<T>(rx: &Receiver<T>) -> Vec<T> {
        let events: Vec<_> = rx.try_iter().collect();
        assert_eq!(
            rx.try_recv().err(),
            Some(crossbeam_channel::TryRecvError::Disconnected),
            "a thread still holds the channel"
        );
        events
    }

    fn assert_no_worker_failure(events: &[AnalysisEvent]) {
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, AnalysisEvent::WorkerFailed(_))),
            "shutdown reported a failure:\n{}",
            summarize(events)
        );
    }

    #[test]
    fn dropping_the_engine_stops_its_threads_while_others_hold_its_sender() {
        let (mut engine, event_rx) = AnalysisEngine::new();
        // The playback engine holds one of these for as long as it runs.
        let playback_sender = engine.sender();
        assert_eq!(engine.running_threads(), 4);

        assert_eq!(engine.stop_threads(), 0);
        drop(engine);

        assert_no_worker_failure(&events_after_threads_ended(&event_rx));
        assert!(
            playback_sender
                .send(AnalysisCommand::PositionUpdate(1.0))
                .is_err(),
            "the analysis thread still takes commands"
        );
    }

    #[test]
    fn dropping_a_busy_engine_stops_its_decodes_and_blocked_sends() {
        let path = write_engine_test_wave(10, 16_000);
        // Nothing drains this, so the spectrogram worker blocks on its
        // output, as when the UI stops reading during shutdown.
        let (spectral_tx, spectral_rx) = crossbeam_channel::bounded(1);
        let (engine, event_rx) =
            AnalysisEngine::new_with_spectrogram_output(Some(SpectrogramOutputRoute {
                sender: spectral_tx,
                generation: Arc::new(AtomicU64::new(0)),
                stops: Arc::default(),
            }));
        engine.command(AnalysisCommand::SetTrack {
            path: path.clone(),
            reset_spectrogram: true,
            track_token: 1,
            gapless: false,
        });
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        while !spectral_rx.is_full() {
            assert!(
                std::time::Instant::now() < deadline,
                "the spectrogram produced no output"
            );
            std::thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(engine.running_threads(), 4);
        let finished: Vec<_> = engine
            .threads
            .iter()
            .map(JoinableThread::finished)
            .collect();
        drop(engine);

        // Wait for the threads themselves, not the shutdown bound, which a
        // loaded machine can outlast.
        for thread in finished {
            assert_eq!(
                thread.recv_timeout(Duration::from_secs(20)),
                Err(crossbeam_channel::RecvTimeoutError::Disconnected),
                "a thread did not stop"
            );
        }
        events_after_threads_ended(&spectral_rx);
        assert_no_worker_failure(&events_after_threads_ended(&event_rx));
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn a_spectrogram_worker_that_cannot_start_is_reported() {
        session::spawn_test_hooks::fail_next_spawn();
        let (engine, event_rx) = AnalysisEngine::new();

        // Nothing has to be sent to the missing worker for the user to hear.
        let message = loop {
            match event_rx.recv_timeout(Duration::from_secs(5)) {
                Ok(AnalysisEvent::WorkerFailed(message)) => break message,
                Ok(_) => {}
                Err(error) => panic!("the failed start is not reported: {error}"),
            }
        };
        assert_eq!(
            message,
            "The spectrogram is unavailable because its thread could not start \
             (injected spawn failure). Restart Ferrous to use it again."
        );
        drop(engine);
    }

    #[test]
    fn a_lost_spectrogram_worker_is_reported_once() {
        let status = SpectrogramWorkerStatus::default();
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let mut reported = false;

        report_lost_spectrogram_worker(&status, &mut reported, &event_tx);
        assert!(
            event_rx.try_recv().is_err(),
            "a running worker is not reported"
        );

        session::stop_spectrogram_worker_for_test(&status);
        report_lost_spectrogram_worker(&status, &mut reported, &event_tx);
        report_lost_spectrogram_worker(&status, &mut reported, &event_tx);
        let reports: Vec<_> = event_rx.try_iter().collect();
        assert!(
            matches!(
                reports.as_slice(),
                [AnalysisEvent::WorkerFailed(message)]
                    if message.contains("because it stopped unexpectedly")
            ),
            "{reports:?}"
        );
    }

    // End-to-end: while stopped, the middle-click zoom reset must still
    // produce spectrogram data chunks for the new zoom level (the worker
    // decodes from the track start and parks after its lookahead), so the
    // UI can refill instead of staying black.
    #[test]
    fn stopped_zoom_reset_pipeline_covers_stopped_viewport() {
        // Long enough that the default-zoom viewport does not cover the
        // whole track.  If RestartCurrentTrack(0) leaves the old 315 s
        // position cached, the following zoom restart begins around 80 s
        // and still emits plenty of data -- just none for viewport column 0.
        let path = write_engine_test_wave(320, 8_000);
        let (engine, event_rx) = AnalysisEngine::new();

        engine.command(AnalysisCommand::SetTrack {
            path: path.clone(),
            reset_spectrogram: true,
            track_token: 1,
            gapless: false,
        });
        engine.command(AnalysisCommand::SetSpectrogramDisplayMode(
            SpectrogramDisplayMode::Centered,
        ));
        engine.command(AnalysisCommand::SetSpectrogramWidgetWidth(1_165));
        engine.command(AnalysisCommand::SetSpectrogramZoomLevel(0.1));
        engine.command(AnalysisCommand::PositionUpdate(315.0));
        let _ = drain_events(&event_rx, Duration::from_millis(600));

        eprintln!("=== STOP (RestartCurrentTrack at 0) ===");
        engine.command(AnalysisCommand::RestartCurrentTrack {
            position_seconds: 0.0,
            clear_history: true,
        });
        let stop_events = drain_events(&event_rx, Duration::from_millis(600));
        eprintln!("after stop:\n{}", summarize(&stop_events));

        eprintln!("=== MIDDLE-CLICK ZOOM RESET (1.0) ===");
        engine.command(AnalysisCommand::SetSpectrogramZoomLevel(1.0));
        let reset_events = drain_events(&event_rx, Duration::from_millis(1_500));
        eprintln!("after zoom reset:\n{}", summarize(&reset_events));

        let default_zoom_chunks = reset_events
            .iter()
            .filter(|e| {
                matches!(
                    e,
                    AnalysisEvent::PrecomputedSpectrogramChunk(c)
                        if c.column_count > 0
                            && usize::from(c.hop_size) == REFERENCE_HOP
                )
            })
            .collect::<Vec<_>>();
        let covers_stopped_viewport = default_zoom_chunks.iter().any(|event| {
            matches!(
                event,
                AnalysisEvent::PrecomputedSpectrogramChunk(c)
                    if c.start_column_index == 0
            )
        });
        let _ = std::fs::remove_file(path);
        assert!(
            covers_stopped_viewport,
            "zoom reset while stopped must decode column 0, got:\n{}",
            summarize(&reset_events),
        );
    }

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
    fn a_deactivation_publishes_the_generation_its_stale_chunks_stay_within() {
        // The frontend frees the panes it hides and, once shown again, must
        // take only the session that showing them starts: every chunk that
        // reaches it after the deactivation is published is no newer than
        // its generation, and the session after reactivation is newer.
        let path = write_engine_test_wave(4, 48_000);
        let generation = Arc::new(AtomicU64::new(0));
        let stops = Arc::new(SpectrogramStops::default());
        let (spectral_tx, spectral_rx) = crossbeam_channel::unbounded();
        let (engine, _) =
            AnalysisEngine::new_with_spectrogram_output(Some(SpectrogramOutputRoute {
                sender: spectral_tx,
                generation: Arc::clone(&generation),
                stops: Arc::clone(&stops),
            }));
        let next_chunk = || match spectral_rx.recv_timeout(Duration::from_secs(5)) {
            Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) => chunk,
            other => panic!("expected a spectral chunk, got {other:?}"),
        };
        engine.command(AnalysisCommand::SetTrack {
            path: path.clone(),
            reset_spectrogram: true,
            track_token: 1,
            gapless: false,
        });
        let first = next_chunk();
        assert!(first.generation > 0);
        assert_eq!(stops.load(), (0, 0));

        // A seek just before it, as Repeat-One or a click would make.
        engine.command(AnalysisCommand::SeekPosition(2.0));
        engine.command(AnalysisCommand::SetSpectrogramActive(false));
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while stops.load().0 == 0 {
            assert!(
                std::time::Instant::now() < deadline,
                "deactivation not published"
            );
            std::thread::sleep(Duration::from_millis(1));
        }
        let (count, stop_generation) = stops.load();
        assert_eq!(count, 1);
        assert!(stop_generation > first.generation);
        std::thread::sleep(Duration::from_millis(100));
        while let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) = spectral_rx.try_recv() {
            assert!(
                chunk.generation < stop_generation,
                "chunk after the stop is not stale"
            );
        }

        engine.command(AnalysisCommand::SetSpectrogramActive(true));
        let fresh = next_chunk();
        assert!(fresh.generation > stop_generation);
        assert!(fresh.buffer_reset);
        assert_eq!(stops.load(), (1, stop_generation));

        engine.command(AnalysisCommand::SetSpectrogramActive(false));
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while stops.load().0 == 1 {
            assert!(
                std::time::Instant::now() < deadline,
                "second deactivation not published"
            );
            std::thread::sleep(Duration::from_millis(1));
        }
        assert!(stops.load().1 > fresh.generation);
        drop(engine);
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn seek_and_stop_restart_a_backpressured_spectral_stream() {
        let path = write_engine_test_wave(2, 48_000);
        let generation = Arc::new(AtomicU64::new(0));
        let (spectral_tx, spectral_rx) = crossbeam_channel::bounded(1);
        let (engine, _) =
            AnalysisEngine::new_with_spectrogram_output(Some(SpectrogramOutputRoute {
                sender: spectral_tx,
                generation: Arc::clone(&generation),
                stops: Arc::default(),
            }));
        engine.command(AnalysisCommand::SetTrack {
            path: path.clone(),
            reset_spectrogram: true,
            track_token: 1,
            gapless: false,
        });
        let deadline = std::time::Instant::now() + Duration::from_secs(2);
        while spectral_rx.is_empty() {
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(1));
        }
        for command in [
            AnalysisCommand::SeekPosition(0.5),
            AnalysisCommand::SetSpectrogramActive(false),
            AnalysisCommand::SetSpectrogramActive(true),
            AnalysisCommand::RestartCurrentTrack {
                position_seconds: 0.0,
                clear_history: true,
            },
        ] {
            let previous = generation.load(Ordering::Relaxed);
            engine.command(command);
            let deadline = std::time::Instant::now() + Duration::from_secs(2);
            while generation.load(Ordering::Relaxed) <= previous {
                assert!(
                    std::time::Instant::now() < deadline,
                    "reset must interrupt backpressure"
                );
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        drop(spectral_rx);
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn waveform_completion_publishes_without_playback_pcm_and_rejects_stale_jobs() {
        let mut state = AnalysisRuntimeState::new();
        state.active_track_token = 2;
        let (tx, rx) = unbounded();
        state.handle_waveform_progress(1, done(vec![0.8; 12], 1.0, true), &tx);
        assert!(rx.try_recv().is_err());
        state.handle_waveform_progress(2, done(vec![0.5; 12], 1.0, true), &tx);
        let AnalysisEvent::Snapshot(snapshot) = rx.try_recv().expect("completion without PCM wake")
        else {
            panic!("expected snapshot");
        };
        assert!(snapshot.waveform_complete);
        assert_eq!(snapshot.waveform_peaks, vec![0.5; 12]);
    }

    #[test]
    fn reset_spectrogram_state_clears_pending_position_suppression() {
        let mut state = AnalysisRuntimeState::new();
        state.suppress_next_spectrogram_position_update = true;

        state.reset_spectrogram_state();

        assert!(
            !state.suppress_next_spectrogram_position_update,
            "stale centered-seek suppression must not survive a reset"
        );
    }

    #[test]
    fn gapless_track_change_starts_seamless_new_session_immediately() {
        let mut state = AnalysisRuntimeState::new();
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        // Gapless track change now sends ContinueWithFile, not NewTrack.
        successor_probed(&mut state, "/tmp/next.flac");
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
        state.suppress_next_spectrogram_position_update = true;
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        assert!(
            !state.suppress_next_spectrogram_position_update,
            "track changes must clear stale centered-seek suppression"
        );
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
    fn hidden_spectrogram_worker_produces_data_after_repeated_resume() {
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let paths = [0, 1].map(|index| {
                std::env::temp_dir().join(format!(
                    "ferrous-resume-{}-{mode:?}-{index}.mp3",
                    std::process::id()
                ))
            });
            for path in &paths {
                std::fs::write(path, include_bytes!("fixtures/seek-tone.mp3")).unwrap();
            }
            // A single output slot also exercises cancellation while the producer
            // is backpressured, not just the idle/EOF path.
            let (tx, rx) = crossbeam_channel::bounded(1);
            let (engine, _) =
                AnalysisEngine::new_with_spectrogram_output(Some(SpectrogramOutputRoute {
                    sender: tx,
                    generation: Arc::new(AtomicU64::new(0)),
                    stops: Arc::default(),
                }));
            engine.command(AnalysisCommand::SetSpectrogramDisplayMode(mode));
            let mut previous_generation = 0;
            for token in 1..=4 {
                engine.command(AnalysisCommand::SetSpectrogramActive(false));
                engine.command(AnalysisCommand::SetTrack {
                    path: paths[usize::try_from(token % 2).unwrap()].clone(),
                    reset_spectrogram: true,
                    track_token: token,
                    gapless: false,
                });
                engine.command(AnalysisCommand::SeekPosition(0.25));
                engine.command(AnalysisCommand::SetSpectrogramActive(true));
                let deadline = std::time::Instant::now() + Duration::from_secs(2);
                loop {
                    assert!(
                        std::time::Instant::now() < deadline,
                        "{mode:?}: worker did not resume for track {token}"
                    );
                    if let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) =
                        rx.recv_timeout(Duration::from_millis(20))
                    {
                        if chunk.track_token == token && chunk.column_count > 0 {
                            assert!(chunk.generation > previous_generation);
                            previous_generation = chunk.generation;
                            break;
                        }
                    }
                }
            }
            engine.command(AnalysisCommand::SetSpectrogramActive(false));
            for path in paths {
                std::fs::remove_file(path).unwrap();
            }
        }
    }

    #[test]
    fn hidden_spectrogram_resumes_latest_track_and_position_in_both_modes() {
        let path = write_engine_test_wave(1, 16_000);
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let mut state = AnalysisRuntimeState::new();
            state.display_mode = mode;
            let (event_tx, _) = unbounded::<AnalysisEvent>();
            let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
            let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
            let waveform_decode_active_token = AtomicU64::new(0);
            let spectrogram_decode_generation = AtomicU64::new(0);
            let spectrogram_track_duration_ms = AtomicU64::new(0);
            let spectrogram_decode_columns_produced = AtomicU64::new(0);
            let ctx = AnalysisContext {
                event_tx: &event_tx,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_decode_active_token,
                spectrogram_cmd_tx: &spectrogram_cmd_tx,
                spectrogram_decode_generation: &spectrogram_decode_generation,
                spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
                spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            };

            state.handle_command(AnalysisCommand::SetSpectrogramActive(false), &ctx);
            assert!(matches!(
                spectrogram_cmd_rx.try_recv().unwrap(),
                SpectrogramWorkerCommand::StopSession
            ));
            assert_eq!(spectrogram_decode_generation.load(Ordering::Relaxed), 1);
            state.handle_command(AnalysisCommand::SetSpectrogramActive(false), &ctx);
            state.update_spectrogram_position(200.0, &ctx);
            state.handle_track_change(path.clone(), false, true, 9, &ctx);
            assert_eq!(state.last_spectrogram_position, 0.0);
            state.handle_prepare_gapless_continuation(path.clone(), &ctx);
            state.seek_spectrogram_position(0.5, &ctx);
            state.handle_command(AnalysisCommand::SetFftSize(512), &ctx);
            state.update_spectrogram_position(0.75, &ctx);
            assert!(spectrogram_cmd_rx.try_recv().is_err());
            assert_eq!(spectrogram_decode_generation.load(Ordering::Relaxed), 1);
            assert!(state.staged_continuation_path.is_none());
            assert!(state.staged_centered_rx.is_none());

            state.handle_command(AnalysisCommand::SetSpectrogramActive(true), &ctx);
            let command = spectrogram_cmd_rx.try_recv().unwrap();
            assert!(
                matches!(command, SpectrogramWorkerCommand::NewTrack {
                path: ref resumed_path, track_token: 9, generation: 2, fft_size: 512,
                start_seconds, target_position_seconds: 0.75,
                emit_initial_reset: true, clear_history_on_reset: true, ..
            } if resumed_path == &path && start_seconds < 0.75),
                "{command:?}"
            );
            state.handle_command(AnalysisCommand::SetSpectrogramActive(true), &ctx);
            assert!(spectrogram_cmd_rx.try_recv().is_err());
        }
        std::fs::remove_file(path).unwrap();
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        state.clear_early_continuation(&ctx);
        // No command should be sent.
        assert!(spectrogram_cmd_rx.try_recv().is_err());
    }

    #[test]
    fn centered_gapless_continues_the_stream_and_finalizes_the_outgoing_track() {
        // Regression for zoom-dependent playhead detachment at track end:
        // at high zoom the file-metadata total_columns_estimate can
        // overshoot the actual playable extent by ~1 second worth of
        // columns.  The outgoing session publishes its decoded extent for
        // its track before the new track's chunks, so Qt can shrink its
        // estimate and detach the centered-mode playhead.  That finalize
        // belongs to the outgoing session's generation, so the switch
        // continues it; the worker emits everything, in order.
        for staged in [false, true] {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            channels.generation.store(5, Ordering::Relaxed);
            let mut state = playing_state(SpectrogramDisplayMode::Centered, Path::new("/a.flac"));
            state.active_track_token = 7;
            if staged {
                stage_successor(&mut state, Path::new("/b.flac"));
            }

            state.handle_track_change(PathBuf::from("/b.flac"), false, true, 8, &ctx);

            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            let [SpectrogramWorkerCommand::NewTrack {
                track_token: 8,
                generation: 5,
                emit_initial_reset,
                handoff,
                ..
            }] = commands.as_slice()
            else {
                panic!("expected one NewTrack in generation 5: {commands:?}");
            };
            assert!(handoff.finalize_previous);
            assert_eq!(
                *emit_initial_reset, !staged,
                "staged columns carry the reset"
            );
            assert_eq!(handoff.prelude.len(), usize::from(staged));
            assert!(handoff
                .prelude
                .iter()
                .all(|chunk| chunk.generation == 5 && chunk.track_token == 8));
            assert_eq!(channels.generation.load(Ordering::Relaxed), 5);
            assert!(
                !channels
                    .event_rx
                    .try_iter()
                    .any(|event| matches!(event, AnalysisEvent::PrecomputedSpectrogramChunk(_))),
                "only the worker emits chunks"
            );
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
    fn centered_seek_within_window_is_left_to_the_session() {
        // Seeking within the session's window does not restart it: the
        // session keeps its columns if they cover the view, and seeks to
        // the view otherwise.  A position would never seek forward to it.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.spectrogram_session_start = 0.0;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        // Seek to 60s — within the window [0, 100].
        state.seek_spectrogram_position(60.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { position_seconds } if position_seconds == 60.0),
            "centered seek within window should go to the session, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_restarts_when_visible_left_falls_before_retained_ring() {
        // Regression: at max zoom the centered UI ring only retains the
        // latest ~3 screens + lookahead. After enough forward decode,
        // small backward seeks can still fit the original session window
        // while their visible left edge has already been evicted.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_session_effective_rate = 44_100;
        state.hop_size = 64;
        state.zoom_level = 16.0;
        state.spectrogram_widget_width = 1_183;
        state.spectrogram_session_widget_width = 1_183;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(15_000);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        let seek_target = 3.554;
        let expected_start =
            (seek_target - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        state.seek_spectrogram_position(seek_target, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::NewTrack {
                start_seconds,
                target_position_seconds,
                clear_history_on_reset,
                ..
            } if (start_seconds - expected_start).abs() < 0.01
                && (target_position_seconds - seek_target).abs() < 0.01
                && clear_history_on_reset),
            "seek whose visible left edge was evicted from the centered ring should restart, got {cmd:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn centered_seek_outside_window_restarts_near_visible_left_edge() {
        // Restarting from the full conservative margin makes the current
        // viewport wait for thousands of off-screen columns after a seek.
        // Start near the normal centered visible-left edge so the fresh
        // viewport fills first.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_session_effective_rate = 48_000;
        state.fft_size = 2_048;
        state.hop_size = 64;
        state.zoom_level = 16.0;
        state.spectrogram_widget_width = 3_440;
        state.spectrogram_session_widget_width = 3_440;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        let seek_target = 85.0;
        let full_margin_start = (seek_target - state.centered_margin_seconds()).max(0.0);
        let fast_start =
            (seek_target - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        assert!(
            fast_start > full_margin_start + 1.0,
            "test setup should distinguish fast viewport start from the old full-margin start"
        );

        state.seek_spectrogram_position(seek_target, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::NewTrack {
                start_seconds,
                target_position_seconds,
                clear_history_on_reset,
                ..
            } if (start_seconds - fast_start).abs() < 0.01
                && (target_position_seconds - seek_target).abs() < 0.01
                && clear_history_on_reset),
            "centered restart should prioritize the visible-left edge while targeting the playhead, got {cmd:?}"
        );
    }

    #[test]
    fn centered_seek_after_fast_restart_reuses_replenishing_window() {
        // After a fast restart begins near the visible-left edge, small
        // follow-up seeks should reuse the replenishing decoded window
        // instead of requiring the old full-margin window to be present.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_session_effective_rate = 48_000;
        state.fft_size = 2_048;
        state.hop_size = 256;
        state.zoom_level = 1.0;
        state.spectrogram_widget_width = 1_920;
        state.spectrogram_session_widget_width = 1_920;

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        // Placeholder produced count for the restart computation below;
        // shadowed by the real post-restart value before the seek.
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let first_restart_target = 96.975;
        state.spectrogram_session_start =
            state.spectrogram_restart_start_seconds(first_restart_target, &ctx);
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let produced_after_restart = f64_to_u64_saturating(
            (state.spectrogram_session_start + 35.0)
                * state.current_centered_cols_per_second().unwrap(),
        );
        let spectrogram_decode_columns_produced = AtomicU64::new(produced_after_restart);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(107.328, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "small seek after a fast centered restart should go to the running session, got {cmd:?}"
        );
        assert!(
            event_rx.try_recv().is_err(),
            "in-window seek should not emit a synthetic clear reset"
        );
    }

    /// Shared setup for zoomed-out centered restart tests.  At zoom 0.1
    /// with a 2048 FFT the effective hop is 10240, giving 4.6875 columns
    /// per second and a 1 px-per-column display, so the visible window in
    /// seconds is `width / 4.6875`.
    fn zoomed_out_centered_state(widget_width: u32) -> AnalysisRuntimeState {
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_session_effective_rate = 48_000;
        state.fft_size = 2_048;
        state.hop_size = 1024;
        state.zoom_level = 0.1;
        state.spectrogram_widget_width = widget_width;
        state.spectrogram_session_widget_width = widget_width;
        state
    }

    #[test]
    fn centered_restart_locks_to_track_start_when_window_covers_track_near_eof() {
        // Regression for zooming out near the end of a track: when the
        // visible window covers >= 90% of the track the Qt display pins
        // to the full track extent (left edge at column 0).  The decode
        // session must therefore start at the track start, or the left
        // edge of the decoded content shows as permanent black.
        let state = zoomed_out_centered_state(1_300);
        // full_screen = 1300 / 4.6875 = 277.33 s >= 0.9 * 288 s → lock.
        let spectrogram_track_duration_ms = AtomicU64::new(288_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let start = state.spectrogram_restart_start_seconds(280.0, &ctx);

        assert!(
            start.abs() < 0.01,
            "zoomed-out restart near EOF with a track-covering window must decode from the \
             track start, got {start}"
        );
    }

    #[test]
    fn centered_restart_covers_eof_detached_visible_left_edge() {
        // Below the lock threshold the playhead still detaches from center
        // near EOF: the right edge clamps to the track end and the visible
        // left edge extends to a full window behind it.  The decode window
        // must start at least that far back instead of the centered
        // half-window margin.
        let state = zoomed_out_centered_state(938);
        // full_screen = 938 / 4.6875 = 200.05 s (< 0.9 * 400 s, no lock).
        let spectrogram_track_duration_ms = AtomicU64::new(400_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        // 395 s into a 400 s track: detached (395 > 400 - 200.05/2), so
        // the visible left edge is 400 - 200.05 = 199.95 s.
        let start = state.spectrogram_restart_start_seconds(395.0, &ctx);

        let expected = (400.0_f64 - 938.0 * 10_240.0 / 48_000.0 - 2.0).max(0.0);
        assert!(
            (start - expected).abs() < 0.01,
            "restart near EOF must cover the detached visible left edge ({expected}), got {start}"
        );
        // And it must start earlier than the centered half-window margin,
        // which would leave the detached edge undecoded.
        let centered_start = (395.0 - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        assert!(
            start < centered_start - 1.0,
            "detached-edge start ({start}) must precede the centered margin start ({centered_start})"
        );
    }

    #[test]
    fn centered_restart_mid_track_keeps_centered_visible_left_start() {
        // Away from EOF the playhead stays centered: the restart start
        // must remain the half-window margin (no extra pre-roll latency).
        let state = zoomed_out_centered_state(938);
        let spectrogram_track_duration_ms = AtomicU64::new(400_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let start = state.spectrogram_restart_start_seconds(250.0, &ctx);

        let expected = (250.0 - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        assert!(
            (start - expected).abs() < 0.01,
            "mid-track restart should keep the centered half-window start ({expected}), got {start}"
        );
    }

    #[test]
    fn centered_restart_without_duration_falls_back_to_centered_left_margin() {
        // Until the worker publishes a track duration (0 ms = unknown),
        // the restart start falls back to the centered half-window margin.
        let state = zoomed_out_centered_state(938);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, _spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        let start = state.spectrogram_restart_start_seconds(395.0, &ctx);

        let expected = (395.0 - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        assert!(
            (start - expected).abs() < 0.01,
            "unknown duration should fall back to the centered half-window start ({expected}), \
             got {start}"
        );
    }

    #[test]
    fn centered_seek_near_eof_restarts_when_detached_left_edge_uncovered() {
        // Regression for seeks near the end of a track: the seek-window
        // check must compare against the EOF-detached visible left edge
        // (a full window behind the track end), not the centered
        // half-window margin.  A session whose decoded window starts
        // after that edge must restart even though the centered check
        // would have passed.
        let mut state = zoomed_out_centered_state(610);
        // full_screen = 610 / 4.6875 = 130.13 s; detached left edge at
        // EOF = 288 - 130.13 = 157.87 s.
        let spectrogram_track_duration_ms = AtomicU64::new(288_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        // Session started mid-track (e.g. before a widget-width growth)
        // with its decoded window beginning at 200 s.
        state.spectrogram_session_start = 200.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(287.0, &ctx);

        let expected_start = 288.0 - 610.0 * 10_240.0 / 48_000.0 - 2.0;
        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::NewTrack {
                start_seconds,
                target_position_seconds,
                clear_history_on_reset,
                ..
            } if (start_seconds - expected_start).abs() < 0.01
                && (target_position_seconds - 287.0).abs() < 0.01
                && clear_history_on_reset),
            "seek near EOF with an uncovered detached left edge should restart at the edge, \
             got {cmd:?}"
        );
        // The worker emits a synthetic reset before it opens the file so
        // the frontend clears the ring before the next render frame.
        assert!(
            matches!(prelude(&cmd), [chunk] if chunk.buffer_reset && chunk.generation == 1),
            "restart path should emit a synthetic buffer reset, got {cmd:?}"
        );
        assert!(event_rx.try_recv().is_err(), "only the worker emits chunks");
    }

    #[test]
    fn centered_seek_near_eof_reuses_window_covering_detached_left_edge() {
        // Conversely, a decoded window that already reaches back past the
        // EOF-detached visible left edge must stay cheap: no restart, no
        // ring clear.
        let mut state = zoomed_out_centered_state(610);
        let spectrogram_track_duration_ms = AtomicU64::new(288_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        // Window starting at 100 s covers the detached edge at 157.87 s.
        state.spectrogram_session_start = 100.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();

        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
        };

        state.seek_spectrogram_position(287.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "seek near EOF whose detached left edge is covered should go to the running session, got {cmd:?}"
        );
        assert!(
            event_rx.try_recv().is_err(),
            "in-window seek should not emit a synthetic clear reset"
        );
    }

    #[test]
    fn centered_seek_uses_session_width_for_zoomed_out_window() {
        // Regression for 6ch AC3 max-zoom-out seeks: the worker session
        // and Qt ring are sized for the width the session started at, but
        // the seek-window check used only the current pane width. On a
        // pane narrowed since, this misclassified an in-window seek as
        // "outside" and restarted decoding from the tail, smearing the
        // display.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("/tmp/track.ac3"));
        state.active_session_effective_rate = 48_000;
        state.zoom_level = 1024.0 / 14_088.0;
        state.spectrogram_widget_width = 1_920;
        state.spectrogram_session_widget_width = 1_920;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();
        state.spectrogram_widget_width = 120;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        state.seek_spectrogram_position(266.791, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "seek within the session's window should go to the running session, got {cmd:?}"
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
        state.spectrogram_widget_width = 1_920;
        state.spectrogram_session_widget_width = 1_920;
        state.spectrogram_session_start = 0.0;
        state.spectrogram_session_margin = state.centered_margin_seconds();
        state.spectrogram_widget_width = 1_200;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        state.seek_spectrogram_position(217.777, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "seek within the raw-surround window should go to the running session, got {cmd:?}"
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        // Seek to 200s — outside the decoded window, so the session
        // should restart near the normal centered visible-left edge.
        let expected_start = (200.0 - (state.centered_margin_seconds() - 2.0) * 0.5 - 2.0).max(0.0);
        state.seek_spectrogram_position(200.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::NewTrack {
                start_seconds,
                target_position_seconds,
                clear_history_on_reset,
                ..
            } if (start_seconds - expected_start).abs() < 0.01
                && (target_position_seconds - 200.0).abs() < 0.01
                && clear_history_on_reset),
            "centered seek outside window should restart with clear_history=true and target the playhead, got {cmd:?}"
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        state.seek_spectrogram_position(60.0, &ctx);

        let cmd = spectrogram_cmd_rx.try_recv().unwrap();
        assert!(
            matches!(cmd, SpectrogramWorkerCommand::Seek { .. }),
            "rolling seek should send Seek, got {cmd:?}"
        );
    }

    #[test]
    fn track_change_clears_position_before_settings_restart_in_both_modes() {
        let path = write_engine_test_wave(1, 16_000);
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let mut state = AnalysisRuntimeState::new();
            state.display_mode = mode;
            let (event_tx, _) = unbounded::<AnalysisEvent>();
            let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
            let (waveform_job_tx, _) = unbounded::<WaveformDecodeJob>();
            let waveform_decode_active_token = AtomicU64::new(0);
            let spectrogram_decode_generation = AtomicU64::new(0);
            let spectrogram_track_duration_ms = AtomicU64::new(0);
            let spectrogram_decode_columns_produced = AtomicU64::new(0);
            let ctx = AnalysisContext {
                event_tx: &event_tx,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_decode_active_token,
                spectrogram_cmd_tx: &spectrogram_cmd_tx,
                spectrogram_decode_generation: &spectrogram_decode_generation,
                spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
                spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            };

            state.active_track_path = Some(path.clone());
            state.active_track_token = 1;
            state.last_spectrogram_position = 120.0;
            state.handle_track_change(path.clone(), true, false, 2, &ctx);
            assert_eq!(state.last_spectrogram_position, 0.0);
            let _ = spectrogram_cmd_rx.try_iter().collect::<Vec<_>>();
            state.handle_command(AnalysisCommand::SetFftSize(512), &ctx);
            let command = spectrogram_cmd_rx.try_recv().unwrap();
            assert!(
                matches!(
                    command,
                    SpectrogramWorkerCommand::NewTrack {
                        track_token: 2,
                        start_seconds: 0.0,
                        target_position_seconds: 0.0,
                        ..
                    }
                ),
                "{command:?}"
            );
        }
        std::fs::remove_file(path).unwrap();
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        successor_probed(&mut state, "/tmp/track_b.flac");
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
        assert_eq!(state.last_spectrogram_position, 0.0);
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        successor_probed(&mut state, "/tmp/b.flac");
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
        assert_eq!(state.last_spectrogram_position, 0.0);
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        assert_eq!(state.last_spectrogram_position, 0.0);
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        assert_eq!(state.last_spectrogram_position, 0.0);
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        assert!(state.staged_centered_thread.is_none());
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
                        generation: 0,
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
                        widget_width: 0,
                    });
                    std::thread::sleep(Duration::from_millis(1));
                }
                // Wait for stop signal, then produce one more (flush-on-stop).
                while !stop_clone.load(Ordering::Acquire) {
                    std::thread::sleep(Duration::from_millis(1));
                }
                let _ = tx.send(PrecomputedSpectrogramChunk {
                    track_token: 0,
                    generation: 0,
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
                    widget_width: 0,
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
                generation: 0,
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
                widget_width: 0,
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
    fn zoom_requests_within_the_tolerance_keep_the_current_zoom() {
        assert_eq!(accepted_spectrogram_zoom(0.1, 0.100_559), None);
        assert_eq!(accepted_spectrogram_zoom(0.1, 0.100_934), None);
        assert_eq!(accepted_spectrogram_zoom(0.1, 0.101_2), Some(0.101_2));
        // Clamped first, so a request below the floor at the floor is kept.
        assert_eq!(accepted_spectrogram_zoom(0.05, 0.01), None);
        assert_eq!(accepted_spectrogram_zoom(1.0, 40.0), Some(16.0));
    }

    #[test]
    fn spectrogram_column_hop_matches_the_session_hops() {
        assert_eq!(spectrogram_column_hop(8192, 1.0), 1024);
        assert_eq!(spectrogram_column_hop(2048, 1.0), 1024);
        assert_eq!(spectrogram_column_hop(8192, 4.0), 256);
        assert_eq!(spectrogram_column_hop(2048, 3.815), 268);
        assert_eq!(spectrogram_column_hop(8192, 0.5), 2048);
        assert_eq!(spectrogram_column_hop(512, 0.1), 10240);
        // The backend's clamp applies.
        assert_eq!(spectrogram_column_hop(8192, 64.0), 64);
        assert_eq!(spectrogram_column_hop(8192, 0.001), 20480);
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
    fn fullscreen_after_centered_seek_refills_newly_exposed_history() {
        for (target, duration_ms, zoom, fullscreen_width) in [
            (120.0, 300_000, 1.0, 3_840),
            (290.0, 300_000, 1.0, 3_840), // EOF detaches the playhead from center.
            (120.0, 300_000, 16.0, 8_192),
        ] {
            let mut state = AnalysisRuntimeState::new();
            state.display_mode = SpectrogramDisplayMode::Centered;
            state.active_track_path = Some(PathBuf::from("unused-test-track.flac"));
            state.active_track_token = 1;
            state.active_session_effective_rate = 48_000;
            state.zoom_level = zoom;
            state.hop_size = zoom_hop_size(state.fft_size, zoom);

            let (event_tx, event_rx) = unbounded::<AnalysisEvent>();
            let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
            let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
            let waveform_decode_active_token = AtomicU64::new(0);
            let spectrogram_decode_generation = AtomicU64::new(0);
            let spectrogram_track_duration_ms = AtomicU64::new(duration_ms);
            let spectrogram_decode_columns_produced = AtomicU64::new(0);
            let ctx = AnalysisContext {
                event_tx: &event_tx,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_decode_active_token,
                spectrogram_cmd_tx: &spectrogram_cmd_tx,
                spectrogram_decode_generation: &spectrogram_decode_generation,
                spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
                spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            };

            state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(1_200), &ctx);
            state.seek_spectrogram_position(target, &ctx);
            assert!(matches!(
                spectrogram_cmd_rx.try_recv().unwrap(),
                SpectrogramWorkerCommand::NewTrack { .. }
            ));
            let narrow_start = state.spectrogram_session_start;
            let generation = spectrogram_decode_generation.load(Ordering::Relaxed);
            event_rx.try_iter().for_each(drop);

            state.handle_command(
                AnalysisCommand::SetSpectrogramWidgetWidth(fullscreen_width),
                &ctx,
            );
            let expected_start = state.spectrogram_restart_start_seconds(target, &ctx);
            assert!(expected_start < narrow_start);
            let commands: Vec<_> = spectrogram_cmd_rx.try_iter().collect();
            assert!(
                commands.iter().any(|cmd| matches!(cmd,
                    SpectrogramWorkerCommand::NewTrack {
                        start_seconds, target_position_seconds, widget_width,
                        generation: new_generation, emit_initial_reset: true,
                        clear_history_on_reset: true, ..
                    } if (*start_seconds - expected_start).abs() < 0.001
                        && (*target_position_seconds - target).abs() < 0.001
                        && *widget_width == fullscreen_width && *new_generation > generation
                )),
                "fullscreen must decode the missing left edge: {commands:?}"
            );
            assert!(commands.iter().any(|cmd| matches!(prelude(cmd), [chunk]
                if chunk.buffer_reset && chunk.clear_history && chunk.column_count == 0
            )));

            // The session keeps decoding for the fullscreen width while the
            // pane shrinks. Reopening at the same zoom must reuse the
            // replenishing window.
            state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(1_200), &ctx);
            state.handle_command(
                AnalysisCommand::SetSpectrogramWidgetWidth(fullscreen_width),
                &ctx,
            );
            state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(zoom), &ctx);
            assert!(spectrogram_cmd_rx.try_recv().is_err());

            // On a later track the session can start at zero yet lose older
            // columns to ring eviction. Compare against the capacity BEFORE
            // growing; sizing it at the new width would hide the missing data.
            state.last_spectrogram_position = 120.0;
            state.active_track_token = 2;
            state.spectrogram_session_start = 0.0;
            state.spectrogram_widget_width = 1_920;
            state.spectrogram_session_widget_width = 1_920;
            let old_capacity = state.centered_ring_capacity_columns().unwrap();
            let oldest_column = if zoom > 1.0 { 88_900 } else { 4_300 };
            spectrogram_decode_columns_produced
                .store(old_capacity + oldest_column, Ordering::Relaxed);
            state.handle_command(
                AnalysisCommand::SetSpectrogramWidgetWidth(fullscreen_width),
                &ctx,
            );
            assert!(
                spectrogram_cmd_rx.try_iter().any(|cmd| matches!(
                    cmd,
                    SpectrogramWorkerCommand::NewTrack { track_token: 2, .. }
                )),
                "fullscreen must also recover evicted history"
            );
        }
    }

    #[test]
    fn a_session_after_the_viewer_closed_decodes_for_the_pane_not_the_viewer() {
        // The fullscreen viewer used to leave a process-wide maximum that
        // sized every later session's lookahead, and the UI's rings with
        // it, for the viewer. The session running while the viewer closes
        // keeps its width; the next one decodes for the pane.
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = SpectrogramDisplayMode::Centered;
        state.active_track_path = Some(PathBuf::from("unused-test-track.flac"));
        state.active_track_token = 1;
        state.active_session_effective_rate = 96_000;
        state.active_session_channel_count = 6;
        state.spectrogram_view_mode = SpectrogramViewMode::PerChannel;
        state.fft_size = 2_048;
        state.hop_size = zoom_hop_size(state.fft_size, 1.0);

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(300_000);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };
        let capacity_for = |width: u32| {
            retention::SpectrogramBufferConfig {
                width,
                sample_rate: 96_000,
                hop: 1_024,
                bins: 1_025,
                channels: 6,
                zoom: 1.0,
                centered: true,
            }
            .limits(retention::lookahead_seconds())
            .capacity
        };
        let new_track_width = |commands: &[SpectrogramWorkerCommand]| {
            commands.iter().find_map(|cmd| match cmd {
                SpectrogramWorkerCommand::NewTrack {
                    widget_width,
                    handoff,
                    ..
                } => {
                    assert!(
                        handoff
                            .prelude
                            .iter()
                            .all(|chunk| chunk.widget_width == *widget_width),
                        "the chunks a session starts with carry its width"
                    );
                    Some(*widget_width)
                }
                _ => None,
            })
        };

        // The main window's pane, and a session for it.
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(2_545), &ctx);
        state.seek_spectrogram_position(120.0, &ctx);
        let commands: Vec<_> = spectrogram_cmd_rx.try_iter().collect();
        assert_eq!(new_track_width(&commands), Some(2_545), "{commands:?}");
        assert_eq!(
            state.centered_ring_capacity_columns(),
            Some(capacity_for(2_545))
        );

        // The viewer opens: the running session grows for it.
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(3_440), &ctx);
        spectrogram_cmd_rx.try_iter().for_each(drop);
        assert_eq!(state.spectrogram_session_widget_width, 3_440);
        assert_eq!(
            state.centered_ring_capacity_columns(),
            Some(capacity_for(3_440))
        );

        // It closes: the session decodes on for the viewer's width, which
        // the UI's ring keeps holding until the session ends.
        state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(2_545), &ctx);
        assert!(spectrogram_cmd_rx.try_recv().is_err());
        assert_eq!(state.spectrogram_session_widget_width, 3_440);

        // The next session, here a seek out of the window, is for the pane.
        state.seek_spectrogram_position(250.0, &ctx);
        let commands: Vec<_> = spectrogram_cmd_rx.try_iter().collect();
        assert_eq!(new_track_width(&commands), Some(2_545), "{commands:?}");
        assert_eq!(state.spectrogram_session_widget_width, 2_545);
        assert_eq!(
            state.centered_ring_capacity_columns(),
            Some(capacity_for(2_545))
        );
        // The columns per pane of the 2545 and 3440 pixel views of a 24/96
        // 5.1 track at FFT 2048.
        assert_eq!(capacity_for(2_545), 8_572);
        assert_eq!(capacity_for(3_440), 11_257);
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
        for mode in [
            SpectrogramDisplayMode::Rolling,
            SpectrogramDisplayMode::Centered,
        ] {
            let mut state = AnalysisRuntimeState::new();
            state.display_mode = mode;
            state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
            state.active_track_token = 1;

            let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
            let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
            let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
            let waveform_decode_active_token = AtomicU64::new(0);
            let spectrogram_decode_generation = AtomicU64::new(0);
            let spectrogram_track_duration_ms = AtomicU64::new(0);
            let spectrogram_decode_columns_produced = AtomicU64::new(0);
            let ctx = AnalysisContext {
                event_tx: &event_tx,
                waveform_job_tx: &waveform_job_tx,
                waveform_decode_active_token: &waveform_decode_active_token,
                spectrogram_cmd_tx: &spectrogram_cmd_tx,
                spectrogram_decode_generation: &spectrogram_decode_generation,
                spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
                spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
            };

            // Default starts at 1920; first shrink to 1000 simulates the
            // windowed layout.  No UpdateWidgetWidth should flow because the
            // session's width has not grown.
            state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(1000), &ctx);
            assert_eq!(state.spectrogram_widget_width, 1000);
            assert_eq!(state.spectrogram_session_widget_width, 1920);
            assert!(
                spectrogram_cmd_rx.try_recv().is_err(),
                "shrinking the widget must not refresh the worker's lookahead"
            );

            // Growing past the session's width (fullscreen on a wider
            // display) must dispatch an UpdateWidgetWidth carrying the new
            // value.
            state.handle_command(AnalysisCommand::SetSpectrogramWidgetWidth(3840), &ctx);
            assert_eq!(state.spectrogram_widget_width, 3840);
            assert_eq!(state.spectrogram_session_widget_width, 3840);
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
            assert_eq!(state.spectrogram_session_widget_width, 3840);
            assert!(spectrogram_cmd_rx.try_recv().is_err());
        }
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
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

    #[test]
    fn rolling_zoom_restart_clears_gapless_position_offset() {
        // Regression: after a rolling-mode gapless handoff, a later zoom
        // restart must start from the real playback position. Keeping the
        // old gapless offset causes the first post-restart PositionUpdate to
        // look like a huge forward seek, sending the worker near EOF where
        // it stops feeding after a few seconds.
        let mut state = AnalysisRuntimeState::new();
        state.active_track_path = Some(PathBuf::from("/tmp/track.flac"));
        state.active_track_token = 4;
        state.display_mode = SpectrogramDisplayMode::Rolling;
        state.last_spectrogram_position = 7.6;
        state.spectrogram_position_offset = 232.2;

        let (event_tx, _event_rx) = unbounded::<AnalysisEvent>();
        let (waveform_job_tx, _waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded::<SpectrogramWorkerCommand>();
        let waveform_decode_active_token = AtomicU64::new(0);
        let spectrogram_decode_generation = AtomicU64::new(0);
        let spectrogram_track_duration_ms = AtomicU64::new(0);
        let spectrogram_decode_columns_produced = AtomicU64::new(0);
        let ctx = AnalysisContext {
            event_tx: &event_tx,
            waveform_job_tx: &waveform_job_tx,
            waveform_decode_active_token: &waveform_decode_active_token,
            spectrogram_cmd_tx: &spectrogram_cmd_tx,
            spectrogram_decode_generation: &spectrogram_decode_generation,
            spectrogram_decode_columns_produced: &spectrogram_decode_columns_produced,
            spectrogram_track_duration_ms: &spectrogram_track_duration_ms,
        };

        state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(2.0), &ctx);

        let cmd = spectrogram_cmd_rx
            .recv_timeout(Duration::from_millis(50))
            .expect("zoom change must send a spectrogram command");
        match cmd {
            SpectrogramWorkerCommand::NewTrack { start_seconds, .. } => {
                assert!(
                    (start_seconds - 7.6).abs() < 0.001,
                    "rolling zoom restart should use the live playback position"
                );
            }
            other => panic!("expected NewTrack for zoom restart, got {other:?}"),
        }
        assert_eq!(
            state.spectrogram_position_offset, 0.0,
            "zoom restart should clear stale rolling gapless offset"
        );
    }

    /// Record that the file helper probed the successor at `path` when it
    /// was staged, and found the live session's format.
    fn successor_probed(state: &mut AnalysisRuntimeState, path: &str) {
        state.active_session_effective_rate = 48_000;
        state.active_session_channel_count = 1;
        state.formats.insert(
            PathBuf::from(path),
            TrackFormat::Known {
                sample_rate: 48_000,
                channels: 2,
            },
            None,
        );
    }

    /// The chunks a `NewTrack` has the worker emit before it opens its file.
    fn prelude(cmd: &SpectrogramWorkerCommand) -> &[PrecomputedSpectrogramChunk] {
        match cmd {
            SpectrogramWorkerCommand::NewTrack { handoff, .. } => &handoff.prelude,
            _ => &[],
        }
    }

    /// Channels and counters for driving `AnalysisRuntimeState` directly.
    struct TestChannels {
        event_tx: Sender<AnalysisEvent>,
        event_rx: Receiver<AnalysisEvent>,
        spectrogram_cmd_tx: Sender<SpectrogramWorkerCommand>,
        spectrogram_cmd_rx: Receiver<SpectrogramWorkerCommand>,
        waveform_job_tx: Sender<WaveformDecodeJob>,
        waveform_job_rx: Receiver<WaveformDecodeJob>,
        waveform_decode_active_token: AtomicU64,
        generation: AtomicU64,
        columns_produced: AtomicU64,
        track_duration_ms: AtomicU64,
    }

    /// An output whose spectral stream is full.
    struct Backpressured(Sender<AnalysisEvent>);

    impl AnalysisEventOutput for Backpressured {
        fn send(&self, event: AnalysisEvent) -> Result<(), ()> {
            AnalysisEventOutput::send(&self.0, event)
        }

        fn spectral_backpressured(&self) -> bool {
            true
        }
    }

    impl TestChannels {
        fn new() -> Self {
            let (event_tx, event_rx) = unbounded();
            let (spectrogram_cmd_tx, spectrogram_cmd_rx) = unbounded();
            let (waveform_job_tx, waveform_job_rx) = unbounded();
            Self {
                event_tx,
                event_rx,
                spectrogram_cmd_tx,
                spectrogram_cmd_rx,
                waveform_job_tx,
                waveform_job_rx,
                waveform_decode_active_token: AtomicU64::new(0),
                generation: AtomicU64::new(0),
                columns_produced: AtomicU64::new(0),
                track_duration_ms: AtomicU64::new(0),
            }
        }

        fn ctx(&self) -> AnalysisContext<'_> {
            self.ctx_with_output(&self.event_tx)
        }

        fn ctx_with_output<'a>(
            &'a self,
            output: &'a dyn AnalysisEventOutput,
        ) -> AnalysisContext<'a> {
            AnalysisContext {
                event_tx: output,
                waveform_job_tx: &self.waveform_job_tx,
                waveform_decode_active_token: &self.waveform_decode_active_token,
                spectrogram_cmd_tx: &self.spectrogram_cmd_tx,
                spectrogram_decode_generation: &self.generation,
                spectrogram_decode_columns_produced: &self.columns_produced,
                spectrogram_track_duration_ms: &self.track_duration_ms,
            }
        }

        /// The files of the sessions sent to the worker since the last call.
        fn opened_paths(&self) -> Vec<PathBuf> {
            self.spectrogram_cmd_rx
                .try_iter()
                .filter_map(|cmd| match cmd {
                    SpectrogramWorkerCommand::NewTrack { path, .. } => Some(path),
                    _ => None,
                })
                .collect()
        }
    }

    /// Analysis of `path` playing as track 1, with its waveform shown.
    fn playing_state(mode: SpectrogramDisplayMode, path: &Path) -> AnalysisRuntimeState {
        let mut state = AnalysisRuntimeState::new();
        state.display_mode = mode;
        state.active_track_path = Some(path.to_path_buf());
        state.active_track_token = 1;
        state.active_session_effective_rate = 48_000;
        state.active_session_channel_count = 1;
        state.snapshot.waveform_peaks = vec![0.5; 4];
        state.snapshot.waveform_complete = true;
        state
    }

    fn remap(pairs: &[(&Path, &Path)]) -> AnalysisCommand {
        AnalysisCommand::RemapPaths(Arc::new(
            pairs
                .iter()
                .map(|(old, new)| (old.to_path_buf(), new.to_path_buf()))
                .collect(),
        ))
    }

    const MODES: [SpectrogramDisplayMode; 2] = [
        SpectrogramDisplayMode::Rolling,
        SpectrogramDisplayMode::Centered,
    ];

    #[test]
    fn a_renamed_current_track_keeps_its_analysis_and_restarts_from_its_new_path() {
        let (old, new) = (Path::new("/music/old.flac"), Path::new("/music/new.flac"));
        for mode in MODES {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = playing_state(mode, old);

            state.handle_command(remap(&[(old, new)]), &ctx);
            // The audio is the same, so nothing restarts or clears.
            assert!(matches!(
                channels.spectrogram_cmd_rx.try_recv(),
                Ok(SpectrogramWorkerCommand::RemapPaths(_))
            ));
            assert!(channels.spectrogram_cmd_rx.try_recv().is_err(), "{mode:?}");
            assert!(channels.event_rx.try_recv().is_err(), "{mode:?}");
            assert_eq!(channels.generation.load(Ordering::Relaxed), 0);
            assert_eq!(state.active_track_token, 1);
            assert_eq!(state.snapshot.waveform_peaks, vec![0.5; 4]);

            // A seek out of the decoded window, a zoom change, and Stop
            // then Play. A rolling seek stays in the open file.
            state.handle_command(AnalysisCommand::SeekPosition(500.0), &ctx);
            state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(2.0), &ctx);
            state.handle_command(
                AnalysisCommand::RestartCurrentTrack {
                    position_seconds: 0.0,
                    clear_history: true,
                },
                &ctx,
            );
            let restarts = if mode == SpectrogramDisplayMode::Centered {
                3
            } else {
                2
            };
            assert_eq!(
                channels.opened_paths(),
                vec![new.to_path_buf(); restarts],
                "{mode:?}"
            );
        }
    }

    #[test]
    fn restarts_decode_a_renamed_current_track_from_its_new_path() {
        for mode in MODES {
            let old = write_engine_test_wave(4, 8_000);
            let new = old.with_extension("renamed.wav");
            let generation = Arc::new(AtomicU64::new(0));
            let (spectral_tx, spectral_rx) = unbounded();
            let (engine, _event_rx) =
                AnalysisEngine::new_with_spectrogram_output(Some(SpectrogramOutputRoute {
                    sender: spectral_tx,
                    generation: Arc::clone(&generation),
                    stops: Arc::default(),
                }));
            let wait_for_columns = |after_generation: u64| {
                let deadline = std::time::Instant::now() + Duration::from_secs(5);
                loop {
                    assert!(
                        std::time::Instant::now() < deadline,
                        "{mode:?}: no columns after generation {after_generation}"
                    );
                    if let Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) =
                        spectral_rx.recv_timeout(Duration::from_millis(20))
                    {
                        if chunk.generation > after_generation && chunk.column_count > 0 {
                            assert_eq!(chunk.track_token, 1);
                            return;
                        }
                    }
                }
            };
            engine.command(AnalysisCommand::SetSpectrogramDisplayMode(mode));
            engine.command(AnalysisCommand::SetTrack {
                path: old.clone(),
                reset_spectrogram: true,
                track_token: 1,
                gapless: false,
            });
            wait_for_columns(0);

            std::fs::rename(&old, &new).unwrap();
            engine.command(remap(&[(&old, &new)]));
            for command in [
                AnalysisCommand::SetSpectrogramZoomLevel(2.0),
                AnalysisCommand::RestartCurrentTrack {
                    position_seconds: 0.0,
                    clear_history: true,
                },
            ] {
                let previous = generation.load(Ordering::Relaxed);
                engine.command(command);
                wait_for_columns(previous);
            }
            drop(engine);
            std::fs::remove_file(new).unwrap();
        }
    }

    fn staged_chunk() -> PrecomputedSpectrogramChunk {
        PrecomputedSpectrogramChunk {
            track_token: 0,
            generation: 0,
            columns_u8: vec![0; 12],
            bins_per_column: 4,
            column_count: 3,
            channel_count: 1,
            start_column_index: 0,
            total_columns_estimate: 100,
            sample_rate_hz: 48_000,
            hop_size: 1024,
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: false,
            clear_history: false,
            widget_width: 0,
        }
    }

    /// Stage `path` as the successor, as `PrepareGaplessContinuation` does
    /// in `state`'s display mode once the file helper found its format.
    fn stage_successor(state: &mut AnalysisRuntimeState, path: &Path) {
        state.formats.insert(
            path.to_path_buf(),
            TrackFormat::Known {
                sample_rate: state.active_session_effective_rate,
                channels: state.active_session_channel_count,
            },
            None,
        );
        if state.display_mode == SpectrogramDisplayMode::Rolling {
            state.staged_continuation_path = Some(path.to_path_buf());
        } else {
            let (tx, rx) = unbounded();
            tx.send(staged_chunk()).unwrap();
            state.staged_centered_rx = Some(rx);
            state.staged_centered_stop = Some(Arc::new(AtomicBool::new(false)));
            state.staged_centered_path = Some(path.to_path_buf());
        }
    }

    /// Check that the gapless switch to track 2 used what was staged, and
    /// return the files of the sessions it started.
    fn assert_successor_continued(
        channels: &TestChannels,
        mode: SpectrogramDisplayMode,
    ) -> Vec<PathBuf> {
        let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
        if mode == SpectrogramDisplayMode::Rolling {
            assert!(
                matches!(
                    commands.as_slice(),
                    [SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 }]
                ),
                "the continuation must not be sent again: {commands:?}"
            );
            return Vec::new();
        }
        let staged: Vec<_> = commands.iter().flat_map(prelude).collect();
        assert!(
            matches!(staged.as_slice(), [chunk] if chunk.track_token == 2 && chunk.buffer_reset),
            "the staged columns must be shown: {staged:?}"
        );
        commands
            .into_iter()
            .filter_map(|cmd| match cmd {
                SpectrogramWorkerCommand::NewTrack {
                    path,
                    emit_initial_reset,
                    ..
                } => {
                    assert!(!emit_initial_reset, "the staged columns were reset");
                    Some(path)
                }
                _ => None,
            })
            .collect()
    }

    #[test]
    fn a_renamed_successor_is_continued_at_its_new_path() {
        let (a, a_new) = (Path::new("/music/a.flac"), Path::new("/music/01 a.flac"));
        let (b, b_new) = (Path::new("/music/b.flac"), Path::new("/music/02 b.flac"));
        for mode in MODES {
            // The next track, and Repeat-One, which plays the same file.
            for (successor, successor_new) in [(b, b_new), (a, a_new)] {
                let channels = TestChannels::new();
                let ctx = channels.ctx();
                let mut state = playing_state(mode, a);
                stage_successor(&mut state, successor);

                state.handle_command(remap(&[(a, a_new), (b, b_new)]), &ctx);
                assert!(matches!(
                    channels.spectrogram_cmd_rx.try_recv(),
                    Ok(SpectrogramWorkerCommand::RemapPaths(_))
                ));
                // The engine reports the switch at the new path.
                state.handle_command(
                    AnalysisCommand::SetTrack {
                        path: successor_new.to_path_buf(),
                        reset_spectrogram: false,
                        track_token: 2,
                        gapless: true,
                    },
                    &ctx,
                );
                let opened = assert_successor_continued(&channels, mode);
                assert!(
                    opened.iter().all(|path| path == successor_new),
                    "{opened:?}"
                );

                state.handle_command(
                    AnalysisCommand::RestartCurrentTrack {
                        position_seconds: 0.0,
                        clear_history: true,
                    },
                    &ctx,
                );
                assert_eq!(channels.opened_paths(), vec![successor_new.to_path_buf()]);
            }
        }

        // Where the worker went on with a rolling successor stays known
        // across a rename: reported before it, or after it for a switch
        // made before it, at the path the worker opened.
        let format = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 1,
        };
        for (successor, successor_new) in [(b, b_new), (a, a_new)] {
            for reported_first in [true, false] {
                let case = format!("{} reported_first={reported_first}", successor.display());
                let channels = TestChannels::new();
                let ctx = channels.ctx();
                let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
                state.last_spectrogram_position = 200.0;
                stage_successor(&mut state, successor);
                let generation = channels.generation.load(Ordering::Relaxed);
                let opened = |path: &Path| SessionReport::Opened {
                    generation,
                    track_token: 1,
                    path: path.to_path_buf(),
                    stamp: None,
                    format,
                    continued_at: Some(200.4),
                };
                let set_track = |path: &Path| AnalysisCommand::SetTrack {
                    path: path.to_path_buf(),
                    reset_spectrogram: false,
                    track_token: 2,
                    gapless: true,
                };
                if reported_first {
                    state.handle_session_report(opened(successor), &ctx);
                    state.handle_command(remap(&[(a, a_new), (b, b_new)]), &ctx);
                    state.handle_command(set_track(successor_new), &ctx);
                } else {
                    // The switch is made before the worker opens the file,
                    // which it does at the new path.
                    state.handle_command(set_track(successor), &ctx);
                    assert!(
                        (state.spectrogram_position_offset - 200.0).abs() < 1e-9,
                        "{case}"
                    );
                    state.handle_command(remap(&[(a, a_new), (b, b_new)]), &ctx);
                    state.handle_session_report(opened(successor_new), &ctx);
                }
                assert!(
                    (state.spectrogram_position_offset - 200.4).abs() < 1e-9,
                    "{case}: {}",
                    state.spectrogram_position_offset
                );
                assert!(state.provisional_continuation.is_none(), "{case}");
                // The worker decodes the playing file, so a seek reaches it.
                channels.spectrogram_cmd_rx.try_iter().for_each(drop);
                state.handle_command(AnalysisCommand::SeekPosition(5.0), &ctx);
                let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
                assert!(
                    matches!(commands.as_slice(), [SpectrogramWorkerCommand::Seek { .. }]),
                    "{case}: {commands:?}"
                );
            }
        }
    }

    #[test]
    fn a_swap_keeps_each_file_with_its_own_analysis() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        for mode in MODES {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            // The file at A plays, and the file at B is staged after it.
            let mut state = playing_state(mode, a);
            stage_successor(&mut state, b);

            state.handle_command(remap(&[(a, b), (b, a)]), &ctx);
            assert_eq!(state.active_track_path.as_deref(), Some(b));
            // The file helper moves the saved waveforms with their files.
            assert!(matches!(
                state.track_io_requests.try_recv(),
                Ok(TrackIoRequest::RemapPaths(renames)) if renames.len() == 2
            ));
            let _ = channels.spectrogram_cmd_rx.try_recv();

            // The staged file is at A now.
            state.handle_command(
                AnalysisCommand::SetTrack {
                    path: a.to_path_buf(),
                    reset_spectrogram: false,
                    track_token: 2,
                    gapless: true,
                },
                &ctx,
            );
            let opened = assert_successor_continued(&channels, mode);
            assert!(opened.iter().all(|path| path == a), "{mode:?}: {opened:?}");
        }
    }

    #[test]
    fn a_session_that_seeked_itself_moves_the_window_a_seek_may_reuse() {
        let channels = TestChannels::new();
        let mut state = playing_state(SpectrogramDisplayMode::Centered, Path::new("/music/a.ac3"));
        state.active_session_effective_rate = 48_000;
        state.spectrogram_widget_width = 1185;
        state.spectrogram_session_start = 100.0;
        channels.generation.store(4, Ordering::Relaxed);

        // A report of a superseded session changes nothing.
        state.handle_session_report(
            SessionReport::Repositioned {
                generation: 3,
                start_seconds: 300.0,
            },
            &channels.ctx(),
        );
        assert!((state.spectrogram_session_start - 100.0).abs() < f64::EPSILON);

        state.handle_session_report(
            SessionReport::Repositioned {
                generation: 4,
                start_seconds: 300.0,
            },
            &channels.ctx(),
        );
        assert!((state.spectrogram_session_start - 300.0).abs() < f64::EPSILON);

        // The columns the session started with are gone, so a seek back
        // among them restarts it instead of expecting them on screen.
        state.handle_command(AnalysisCommand::SeekPosition(120.0), &channels.ctx());
        assert!(
            channels
                .spectrogram_cmd_rx
                .try_iter()
                .any(|cmd| matches!(cmd, SpectrogramWorkerCommand::NewTrack { .. })),
            "the seek must restart the session"
        );
    }

    /// The columns the UI holds of the last session: its ring, as far as
    /// the chunks tell. A centered session sends the left of its view after
    /// the columns at its playhead.
    #[cfg(feature = "gst")]
    #[derive(Default)]
    struct ModelRing {
        /// The spans of columns since the last reset, in order and merged.
        columns: Vec<(u64, u64)>,
        reset_pending: bool,
        /// Whether the session behind the columns reached the end.
        complete: bool,
    }

    #[cfg(feature = "gst")]
    impl ModelRing {
        fn take(&mut self, chunk: &PrecomputedSpectrogramChunk) {
            if chunk.buffer_reset && chunk.bins_per_column > 0 {
                self.reset_pending = true;
                self.complete = false;
            }
            self.complete |= chunk.complete;
            if chunk.column_count == 0 {
                return;
            }
            if std::mem::take(&mut self.reset_pending) {
                self.columns.clear();
            }
            let start = u64::from(chunk.start_column_index);
            let end = start + u64::from(chunk.column_count);
            self.columns.push((start, end));
            self.columns.sort_unstable();
            let mut merged: Vec<(u64, u64)> = Vec::with_capacity(self.columns.len());
            for &(first, last) in &self.columns {
                match merged.last_mut() {
                    Some(previous) if first <= previous.1 => previous.1 = previous.1.max(last),
                    _ => merged.push((first, last)),
                }
            }
            self.columns = merged;
        }

        /// Whether the columns run from `left` past `right` without a gap.
        fn covers(&self, left: u64, right: u64) -> bool {
            self.columns
                .iter()
                .any(|&(first, last)| first <= left && last > right)
        }

        /// The first column since the last reset.
        fn first(&self) -> Option<u64> {
            self.columns.first().map(|&(first, _)| first)
        }
    }

    /// Wait until `done` holds of the ring fed by `engine`'s output.
    #[cfg(feature = "gst")]
    #[track_caller]
    fn fill_ring_until(
        events: &Receiver<AnalysisEvent>,
        ring: &mut ModelRing,
        what: &str,
        mut done: impl FnMut(&ModelRing) -> bool,
    ) {
        let deadline = std::time::Instant::now() + Duration::from_secs(30);
        while !done(ring) {
            let left = deadline.saturating_duration_since(std::time::Instant::now());
            match events.recv_timeout(left) {
                Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) => ring.take(&chunk),
                Ok(AnalysisEvent::WorkerFailed(error)) => panic!("{what}: {error}"),
                Ok(AnalysisEvent::Snapshot(_)) => {}
                Err(error) => panic!("{what}: {error}; columns {:?}", ring.columns),
            }
        }
    }

    #[cfg(feature = "gst")]
    const BURST_VIEW_WIDTH: u32 = 1185;
    #[cfg(feature = "gst")]
    /// Columns a second, at the reference hop and 48 kHz.
    const BURST_COLUMNS_PER_SECOND: f64 = 46.875;
    #[cfg(feature = "gst")]
    /// The seconds of a view of [`BURST_VIEW_WIDTH`] columns.
    const BURST_WINDOW_SECONDS: f64 = 25.28;
    #[cfg(feature = "gst")]
    /// The length of the stream, of 720 repeats of 1.024 s: longer than the
    /// 300 s assumed of a file that does not tell its length.
    const BURST_STREAM_SECONDS: f64 = 737.28;

    #[cfg(feature = "gst")]
    fn burst_column(seconds: f64) -> u64 {
        f64_to_u64_saturating(seconds * BURST_COLUMNS_PER_SECOND)
    }

    /// An engine in `mode` that plays a raw AC3 stream, with its ring.
    #[cfg(feature = "gst")]
    fn engine_on_raw_stream(
        name: &str,
        mode: SpectrogramDisplayMode,
    ) -> (AnalysisEngine, Receiver<AnalysisEvent>, ModelRing, PathBuf) {
        let path = std::env::temp_dir().join(format!(
            "ferrous-seek-burst-{name}-{mode:?}-{}.ac3",
            std::process::id()
        ));
        std::fs::write(&path, crate::raw_audio::TEST_AC3.repeat(720)).expect("write fixture");
        let (engine, events) = AnalysisEngine::new();
        engine.command(AnalysisCommand::SetSpectrogramActive(true));
        engine.command(AnalysisCommand::SetSpectrogramDisplayMode(mode));
        engine.command(AnalysisCommand::SetSpectrogramWidgetWidth(BURST_VIEW_WIDTH));
        engine.command(AnalysisCommand::SetTrack {
            path: path.clone(),
            reset_spectrogram: true,
            track_token: 1,
            gapless: false,
        });
        engine.command(AnalysisCommand::PositionUpdate(0.0));
        let mut ring = ModelRing::default();
        fill_ring_until(&events, &mut ring, "the track did not start", |ring| {
            ring.first().is_some()
        });
        (engine, events, ring, path)
    }

    /// Seek through `targets`, each reported as the engine reports it: a
    /// seek, the position it landed at, and the next heartbeat. Returns the
    /// last one.
    #[cfg(feature = "gst")]
    fn seek_burst(engine: &AnalysisEngine, targets: &[f64]) -> f64 {
        for &target in targets {
            engine.command(AnalysisCommand::SeekPosition(target));
            engine.command(AnalysisCommand::PositionUpdate(target));
            engine.command(AnalysisCommand::PositionUpdate(target + 0.04));
        }
        targets[targets.len() - 1]
    }

    /// A burst of seeks in a raw file, whose parser tells its length only
    /// after some frames, must leave the view around the last target
    /// covered, and the columns following playback.
    #[cfg(feature = "gst")]
    #[test]
    fn a_burst_of_seeks_in_a_raw_file_leaves_the_view_covered_and_following_playback() {
        for (mode, seconds_played) in [
            (SpectrogramDisplayMode::Centered, 120),
            (SpectrogramDisplayMode::Rolling, 15),
        ] {
            let (engine, events, mut ring, path) = engine_on_raw_stream("mid", mode);
            let target = seek_burst(&engine, &[692.3, 21.0, 609.7, 53.9, 544.6, 610.2]);
            let left = match mode {
                SpectrogramDisplayMode::Centered => {
                    burst_column(target - BURST_WINDOW_SECONDS / 2.0)
                }
                SpectrogramDisplayMode::Rolling => burst_column(target),
            };

            let what = format!("{mode:?}: the view at {left} was not covered");
            fill_ring_until(&events, &mut ring, &what, |ring| {
                ring.covers(left, burst_column(target))
            });

            // Playback goes on past what the session decoded at first.
            for second in 1..=seconds_played {
                let position = target + f64::from(second);
                engine.command(AnalysisCommand::PositionUpdate(position));
                let what = format!("{mode:?}: the columns stopped at {position} s");
                fill_ring_until(&events, &mut ring, &what, |ring| {
                    ring.covers(left, burst_column(position))
                });
            }
            drop(engine);
            std::fs::remove_file(&path).expect("remove fixture");
        }
    }

    /// Near the end of the file the view stops following the playhead: it
    /// ends with the track, and the session reaches the end soon after it
    /// starts, so no later column would cover its left edge.
    #[cfg(feature = "gst")]
    #[test]
    fn a_seek_near_the_end_of_a_raw_file_covers_the_view_of_its_end() {
        let (engine, events, mut ring, path) =
            engine_on_raw_stream("end", SpectrogramDisplayMode::Centered);
        let target = seek_burst(&engine, &[60.0, 21.0, BURST_STREAM_SECONDS - 5.0]);
        // The session publishes the end once its right reaches it, and
        // fills the left of the view after that.
        let left = burst_column(BURST_STREAM_SECONDS - BURST_WINDOW_SECONDS);
        let what = "the session did not end with the view of the end covered";
        fill_ring_until(&events, &mut ring, what, |ring| {
            ring.complete
                && ring
                    .first()
                    .is_some_and(|first| first > burst_column(target - 60.0))
                && ring.covers(left, burst_column(target))
        });
        drop(engine);
        std::fs::remove_file(&path).expect("remove fixture");
    }

    #[test]
    fn a_seek_is_applied_while_a_staging_thread_cannot_open_its_file() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let channels = TestChannels::new();
        let mut state = playing_state(SpectrogramDisplayMode::Centered, a);
        // Holds the staging thread where it would open the file, as a slow
        // network share or GStreamer would.
        let (release_staging, staging_gate) = crossbeam_channel::bounded::<()>(0);
        state.staging_open_gate = Some(staging_gate);
        successor_probed(&mut state, "/music/b.flac");
        state.handle_command(
            AnalysisCommand::PrepareGaplessContinuation {
                path: b.to_path_buf(),
            },
            &channels.ctx(),
        );
        assert!(state.staged_centered_thread.is_some(), "staging started");

        let (done_tx, done_rx) = crossbeam_channel::bounded(1);
        let state = std::thread::scope(|scope| {
            let handler = scope.spawn(|| {
                let ctx = channels.ctx();
                // Seeking back near the end drops the staged successor, then
                // seeks; a zoom change drops it as well.
                state.handle_command(AnalysisCommand::CancelStagedContinuation, &ctx);
                state.handle_command(AnalysisCommand::SeekPosition(500.0), &ctx);
                state.handle_command(AnalysisCommand::SetSpectrogramZoomLevel(2.0), &ctx);
                let _ = done_tx.send(());
                state
            });
            let handled = done_rx.recv_timeout(Duration::from_secs(10)).is_ok();
            // Let a blocked handler finish, so a failure is reported.
            drop(release_staging);
            let state = handler.join().expect("handler");
            assert!(handled, "the seek waited for the staging thread");
            state
        });
        let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
        assert!(
            commands.iter().any(|cmd| matches!(cmd,
                SpectrogramWorkerCommand::NewTrack { target_position_seconds, .. }
                    if (*target_position_seconds - 500.0).abs() < 0.001)),
            "the seek must restart the session: {commands:?}"
        );
        assert!(state.staged_centered_thread.is_none());

        // The stopped thread is joined at shutdown.
        let mut state = state;
        assert_eq!(state.retired_staging.len(), 1);
        state.handle_command(AnalysisCommand::Shutdown, &channels.ctx());
        assert!(state.retired_staging.is_empty());
    }

    #[test]
    fn a_seek_while_the_spectral_stream_is_full_keeps_history_inside_the_analysed_range() {
        for mode in MODES {
            let channels = TestChannels::new();
            let full = Backpressured(channels.event_tx.clone());
            let ctx = channels.ctx_with_output(&full);
            let mut state = playing_state(mode, Path::new("/music/a.flac"));

            state.handle_command(AnalysisCommand::SeekPosition(5.0), &ctx);
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            if mode == SpectrogramDisplayMode::Rolling {
                // Queued columns were decoded before the seek: a new session
                // drops them, and its reset keeps what is on screen.
                assert!(
                    matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                        start_seconds, generation: 1, emit_initial_reset: true,
                        clear_history_on_reset: false, ..
                    }] if (*start_seconds - 5.0).abs() < 0.001),
                    "{commands:?}"
                );
            } else {
                // The target is inside the session's window: the session
                // takes the seek, and nothing restarts or clears.
                assert!(
                    matches!(commands.as_slice(), [SpectrogramWorkerCommand::Seek {
                        position_seconds
                    }] if (*position_seconds - 5.0).abs() < 0.001),
                    "{commands:?}"
                );
                assert_eq!(channels.generation.load(Ordering::Relaxed), 0);

                // Outside the window there is no history to keep.
                state.handle_command(AnalysisCommand::SeekPosition(500.0), &ctx);
                let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
                assert!(
                    matches!(
                        commands.as_slice(),
                        [SpectrogramWorkerCommand::NewTrack {
                            clear_history_on_reset: true,
                            ..
                        }]
                    ),
                    "{commands:?}"
                );
            }
        }
    }

    /// The request the file helper received, which must be the only one.
    fn only_track_io_request(state: &AnalysisRuntimeState) -> TrackIoRequest {
        let requests: Vec<_> = state.track_io_requests.try_iter().collect();
        let [request] = <[_; 1]>::try_from(requests).expect("one request");
        request
    }

    #[test]
    fn a_new_track_is_read_by_the_file_helper_and_its_saved_waveform_shown() {
        let path = Path::new("/music/a.flac");
        for saved in [false, true] {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = AnalysisRuntimeState::new();
            state.handle_track_change(path.to_path_buf(), true, false, 4, &ctx);
            assert!(matches!(
                only_track_io_request(&state),
                TrackIoRequest::LoadTrack {
                    track_token: 4,
                    probed_at: None,
                    ..
                }
            ));
            assert!(channels.waveform_job_rx.try_recv().is_err());
            channels.event_rx.try_iter().for_each(drop);

            let stamp = WaveformSourceStamp {
                size_bytes: 10,
                modified_secs: 1,
                modified_nanos: 0,
                file_id: None,
            };
            let loaded = |track_token| TrackIoReply::TrackLoaded {
                track_token,
                stamp: Some(stamp),
                format: Some(TrackFormat::Known {
                    sample_rate: 44_100,
                    channels: 2,
                }),
                peaks: saved.then(|| vec![0.5; 4]),
            };
            // A reply for a track that is no longer current changes nothing.
            state.handle_track_io_reply(loaded(3), &ctx);
            assert_eq!(state.pending_track_load, Some(4));
            state.handle_track_io_reply(loaded(4), &ctx);
            assert_eq!(state.pending_track_load, None);
            assert_eq!(state.active_session_effective_rate, 44_100);
            if saved {
                // Only for the decode worker to drop what it built for the
                // last track.
                assert!(matches!(
                    channels.waveform_job_rx.try_recv(),
                    Ok(WaveformDecodeJob {
                        track_token: 4,
                        decode: false,
                        ..
                    })
                ));
                assert!(channels.event_rx.try_iter().any(|event| matches!(event,
                    AnalysisEvent::Snapshot(snapshot)
                        if snapshot.waveform_complete && snapshot.waveform_peaks == [0.5; 4])));
            } else {
                assert!(matches!(
                    channels.waveform_job_rx.try_recv(),
                    Ok(WaveformDecodeJob {
                        track_token: 4,
                        decode: true,
                        ..
                    })
                ));
            }

            // Playing it again probes it only if the file has changed.
            state.handle_track_change(path.to_path_buf(), true, false, 5, &ctx);
            assert!(matches!(
                only_track_io_request(&state),
                TrackIoRequest::LoadTrack {
                    track_token: 5,
                    probed_at: Some(probed_at),
                    ..
                } if probed_at == stamp
            ));
        }
    }

    #[test]
    fn an_overview_cut_short_by_an_error_is_shown_partial_and_not_saved() {
        let path = Path::new("/music/a.flac");
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = AnalysisRuntimeState::new();
        state.handle_track_change(path.to_path_buf(), true, false, 4, &ctx);
        state.track_io_requests.try_iter().for_each(drop);
        channels.event_rx.try_iter().for_each(drop);

        // The decode stopped at an error 30 s into the file.
        state.handle_waveform_progress(4, done(vec![0.5; 12], 30.0, false), ctx.event_tx);

        let requests: Vec<_> = state.track_io_requests.try_iter().collect();
        assert!(requests.is_empty(), "saved: {requests:?}");
        assert!(channels.event_rx.try_iter().any(|event| matches!(event,
            AnalysisEvent::Snapshot(snapshot)
                if !snapshot.waveform_complete
                    && (snapshot.waveform_coverage_seconds - 30.0).abs() < f32::EPSILON)));

        // A decode that reaches the end of the file is saved, with what it
        // covered, and the stamp of the file it read.
        state.handle_waveform_progress(4, done(vec![0.25; 12], 180.0, true), ctx.event_tx);
        assert!(matches!(
            only_track_io_request(&state),
            TrackIoRequest::SaveWaveform { path: saved, peaks, coverage_seconds, stamp }
                if saved == path && peaks == [0.25; 12]
                    && (coverage_seconds - 180.0).abs() < f32::EPSILON
                    && stamp == Some(decoded_stamp())
        ));
    }

    /// A `GStreamer` overview whose pipeline fails is shown with the audio
    /// decoded before the failure, the audio still queued included, and is
    /// not saved.
    #[cfg(feature = "gst")]
    #[test]
    fn a_gstreamer_overview_cut_short_by_a_failure_is_shown_partial_and_not_saved() {
        use crate::raw_audio::TEST_DTS;

        let path = crate::paths::test_root().join("analysis-failing-overview.dts");
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        std::fs::write(&path, TEST_DTS).expect("write fixture");
        // The pipeline fails before the decode reads any of its audio.
        let (updates, decoded) = gst_waveform::overview_failing_after(&path, 5, true);
        std::fs::remove_file(&path).expect("remove fixture");
        let (peaks, coverage, progress) = updates.into_iter().last().expect("a last update");

        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = AnalysisRuntimeState::new();
        state.handle_track_change(path.clone(), true, false, 4, &ctx);
        state.track_io_requests.try_iter().for_each(drop);
        channels.event_rx.try_iter().for_each(drop);
        let update = WaveformUpdate::of(peaks, coverage, progress, Some(decoded_stamp()));
        state.handle_waveform_progress(4, update, ctx.event_tx);

        let requests: Vec<_> = state.track_io_requests.try_iter().collect();
        assert!(requests.is_empty(), "saved: {requests:?}");
        assert!(channels.event_rx.try_iter().any(|event| matches!(event,
            AnalysisEvent::Snapshot(snapshot)
                if !snapshot.waveform_complete
                    && (snapshot.waveform_coverage_seconds - decoded).abs() < 1e-3)));
    }

    fn decoded_stamp() -> WaveformSourceStamp {
        WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: Some(crate::file_identity::FileId::from_numbers(1, 2)),
        }
    }

    /// The last update of a decode of the file of [`decoded_stamp`].
    fn done(peaks: Vec<f32>, coverage_seconds: f32, complete: bool) -> WaveformUpdate {
        WaveformUpdate {
            peaks,
            coverage_seconds,
            complete,
            done: true,
            source: Some(decoded_stamp()),
        }
    }

    /// Two files of the same size and time, `a` and `b`, of decodable audio,
    /// each with `extension`; and the folder they are in.
    fn same_size_and_time(
        name: &str,
        audio: &[u8],
        extension: &str,
    ) -> (PathBuf, PathBuf, PathBuf) {
        let folder = crate::paths::test_root().join(format!("analysis-{name}"));
        let _ = std::fs::remove_dir_all(&folder);
        std::fs::create_dir_all(&folder).expect("folder");
        let (a, b) = (
            folder.join(format!("a.{extension}")),
            folder.join(format!("b.{extension}")),
        );
        let time = std::time::UNIX_EPOCH + Duration::from_secs(1_700_000_000);
        for path in [&a, &b] {
            std::fs::write(path, audio).expect("audio");
            File::options()
                .write(true)
                .open(path)
                .and_then(|file| file.set_modified(time))
                .expect("modification time");
        }
        (a, b, folder)
    }

    fn swap(a: &Path, b: &Path) {
        let aside = a.with_extension("aside");
        std::fs::rename(a, &aside).expect("aside");
        std::fs::rename(b, a).expect("b to a");
        std::fs::rename(&aside, b).expect("a to b");
    }

    /// The stamps the overview decode of `path` reports with its updates,
    /// with the files at `a` and `b` swapped, as by another program: before
    /// the decode starts if `before_start`, and at each of its first
    /// `swaps` checks for a cancel; and whether it completed.
    fn decoded_sources(
        path: &Path,
        b: &Path,
        before_start: bool,
        swaps: usize,
    ) -> (Vec<Option<WaveformSourceStamp>>, bool) {
        let source = Cell::new(None);
        let mut sources = Vec::new();
        let mut complete = false;
        if before_start {
            swap(path, b);
        }
        let mut swaps_left = swaps;
        decode_waveform_peaks_stream(
            path,
            64,
            &source,
            &mut PrefillSlot::default(),
            |_, _, progress| {
                sources.push(source.get());
                complete |= progress == OverviewProgress::Complete;
                true
            },
            || {
                if swaps_left > 0 {
                    swap(path, b);
                    swaps_left -= 1;
                }
                false
            },
        )
        .expect("decode");
        assert_eq!(swaps_left, 0, "the decode checked for a cancel too seldom");
        (sources, complete)
    }

    #[test]
    fn a_waveform_is_saved_with_the_stamp_of_the_file_the_decode_opened() {
        let (a, b, folder) = same_size_and_time(
            "decode-opened",
            include_bytes!("fixtures/seek-tone.mp3"),
            "mp3",
        );
        // The track was loaded when the file first at `a` was there; the
        // decode opens the one swapped in since.
        let loaded = track_io::source_stamp(&a).expect("stamp");
        let (sources, complete) = decoded_sources(&a, &b, true, 0);
        assert!(complete);
        let opened = track_io::source_stamp(&a).expect("stamp");
        assert_ne!(opened, loaded);
        assert!(sources.iter().all(|source| *source == Some(opened)));

        // Swapped once the decode has the file open, it reads on, and tells
        // the stamp of the file it opened.
        let (sources, complete) = decoded_sources(&a, &b, false, 1);
        assert!(complete);
        assert!(sources.iter().all(|source| *source == Some(opened)));
        std::fs::remove_dir_all(folder).expect("cleanup");
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_waveform_gstreamer_decoded_is_saved_with_the_stamp_of_the_file_its_source_opened() {
        let (a, b, folder) =
            same_size_and_time("decode-gst", include_bytes!("fixtures/raw-tone.dts"), "dts");
        // Swapped before the decode starts, or after it starts and before
        // its source opens the file by its path: the file it opens.
        for (before_start, swaps) in [(true, 0), (false, 1)] {
            let (sources, complete) = decoded_sources(&a, &b, before_start, swaps);
            assert!(complete);
            let opened = track_io::source_stamp(&a);
            assert!(opened.is_some());
            assert!(sources.iter().all(|source| *source == opened));
        }

        // Swapped before the source opens the file, and back while it
        // reads: the file at the path has its stamp again, but the decode
        // read the other file, whose stamp it tells.
        let (sources, complete) = decoded_sources(&a, &b, false, 2);
        assert!(complete);
        let read = track_io::source_stamp(&b);
        assert_ne!(read, track_io::source_stamp(&a));
        assert!(sources.iter().all(|source| *source == read));
        std::fs::remove_dir_all(folder).expect("cleanup");
    }

    /// The stamp the overview decode of `path` with `GStreamer` tells for
    /// its last update, and the seconds it covers.
    #[cfg(feature = "gst")]
    fn gst_overview(path: &Path) -> (Option<WaveformSourceStamp>, f32) {
        let source = Cell::new(None);
        let mut finished = None;
        decode_waveform_peaks_stream_gst(
            path,
            64,
            &source,
            &mut PrefillSlot::default(),
            |_, coverage, progress| {
                if progress == OverviewProgress::Complete {
                    finished = Some((source.get(), coverage));
                }
                true
            },
            || false,
        )
        .expect("decode");
        finished.expect("a complete overview")
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_gstreamer_decode_trims_the_raw_surround_file_it_reads() {
        use crate::raw_audio::{ripped_surround_bytes_after_junk, TEST_DTS};

        // A ripped file whose audio starts late, and a plain stream of the
        // same size, which holds about twice as much audio.
        let (ripped, _) = ripped_surround_bytes_after_junk(12_000, TEST_DTS);
        let plain: Vec<u8> = TEST_DTS
            .iter()
            .copied()
            .cycle()
            .take(ripped.len())
            .collect();
        let folder = crate::paths::test_root().join("analysis-decode-gst-trim");
        let _ = std::fs::remove_dir_all(&folder);
        std::fs::create_dir_all(&folder).expect("folder");
        let (a, b) = (folder.join("a.dts"), folder.join("b.dts"));
        let time = std::time::UNIX_EPOCH + Duration::from_secs(1_700_000_000);
        for (path, bytes) in [(&a, &ripped), (&b, &plain)] {
            std::fs::write(path, bytes).expect("audio");
            File::options()
                .write(true)
                .open(path)
                .and_then(|file| file.set_modified(time))
                .expect("modification time");
        }
        let (ripped_stamp, ripped_coverage) = gst_overview(&a);
        let (plain_stamp, plain_coverage) = gst_overview(&b);
        assert!(
            plain_coverage > ripped_coverage + 0.3,
            "{plain_coverage} s, {ripped_coverage} s"
        );

        // Swapped once the decode has read the audio range of the file at
        // `a`, before its source starts.
        let (from, to) = (a.clone(), b.clone());
        let mut swapped = false;
        gst_waveform::test_hooks::after_each_source_built(move || {
            if !std::mem::replace(&mut swapped, true) {
                swap(&from, &to);
            }
        });
        let (stamp, coverage) = gst_overview(&a);
        gst_waveform::test_hooks::clear();

        // The overview is that of the file whose stamp it tells.
        let expected = if stamp == ripped_stamp {
            ripped_coverage
        } else {
            assert_eq!(stamp, plain_stamp);
            plain_coverage
        };
        assert!(
            (coverage - expected).abs() < 0.05,
            "covers {coverage} s, the file it tells {expected} s"
        );
        std::fs::remove_dir_all(folder).expect("cleanup");
    }

    /// A file source that fails to read past `limit` bytes, as a file on a
    /// share that goes away does.
    struct FailingSource {
        bytes: std::io::Cursor<Vec<u8>>,
        limit: u64,
    }

    impl std::io::Read for FailingSource {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            let left = self.limit.saturating_sub(self.bytes.position());
            if left == 0 {
                return Err(std::io::Error::other("the share went away"));
            }
            let len = buf.len().min(usize::try_from(left).unwrap_or(usize::MAX));
            self.bytes.read(&mut buf[..len])
        }
    }

    impl std::io::Seek for FailingSource {
        fn seek(&mut self, pos: std::io::SeekFrom) -> std::io::Result<u64> {
            self.bytes.seek(pos)
        }
    }

    impl MediaSource for FailingSource {
        fn is_seekable(&self) -> bool {
            true
        }

        fn byte_len(&self) -> Option<u64> {
            u64::try_from(self.bytes.get_ref().len()).ok()
        }
    }

    /// The overview updates of a Symphonia decode of `source`.
    fn symphonia_overview(
        source: Box<dyn MediaSource>,
        extension: &str,
    ) -> Vec<(Vec<f32>, f32, OverviewProgress)> {
        let mut updates = Vec::new();
        let decoded = decode_waveform_peaks_symphonia(
            source,
            Some(extension),
            1024,
            &mut PrefillSlot::default(),
            &mut |peaks, coverage, progress| {
                updates.push((peaks, coverage, progress));
                true
            },
            &mut || false,
        )
        .expect("overview decode");
        assert!(decoded);
        updates
    }

    #[test]
    fn an_overview_reports_completion_only_when_it_reaches_the_end_of_the_file() {
        let wave = write_engine_test_wave(8, 8_000);
        let bytes = std::fs::read(&wave).expect("read fixture");
        std::fs::remove_file(wave).expect("remove fixture");
        let source = |limit| {
            Box::new(FailingSource {
                bytes: std::io::Cursor::new(bytes.clone()),
                limit,
            })
        };

        let updates = symphonia_overview(source(u64::MAX), "wav");
        let (_, coverage, progress) = updates.last().expect("final update");
        assert_eq!(*progress, OverviewProgress::Complete);
        assert!((coverage - 8.0).abs() < 0.01, "covers {coverage} s");

        // A read error half way through.
        let updates = symphonia_overview(source(64 * 1024), "wav");
        let (peaks, coverage, progress) = updates.last().expect("final update");
        assert_eq!(*progress, OverviewProgress::Truncated);
        assert!((0.5..6.0).contains(coverage), "covers {coverage} s");
        assert!(!peaks.is_empty());
        assert!(updates
            .iter()
            .all(|(_, _, progress)| *progress != OverviewProgress::Complete));
    }

    #[test]
    fn an_overview_decode_sends_its_first_preview_without_waiting() {
        let wave = write_engine_test_wave(8, 8_000);
        let bytes = std::fs::read(&wave).expect("read fixture");
        std::fs::remove_file(wave).expect("remove fixture");

        // The decode takes far less than the preview interval.
        let updates = symphonia_overview(Box::new(std::io::Cursor::new(bytes)), "wav");
        let [(first, coverage, progress), .., (_, _, last)] = updates.as_slice() else {
            panic!("{} updates", updates.len());
        };
        assert_eq!(*progress, OverviewProgress::Partial);
        assert_eq!(first.len(), MIN_PREVIEW_PEAKS);
        assert!(*coverage < 0.5, "covers {coverage} s");
        assert_eq!(*last, OverviewProgress::Complete);
    }

    #[test]
    fn the_first_overview_preview_is_shown_at_once_and_not_as_complete() {
        let path = Path::new("/music/a.flac");
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = AnalysisRuntimeState::new();
        state.handle_track_change(path.to_path_buf(), true, false, 4, &ctx);
        state.track_io_requests.try_iter().for_each(drop);
        channels.event_rx.try_iter().for_each(drop);

        let preview = WaveformUpdate {
            peaks: vec![0.5; MIN_PREVIEW_PEAKS],
            coverage_seconds: 0.5,
            complete: false,
            done: false,
            source: Some(decoded_stamp()),
        };
        state.handle_waveform_progress(4, preview, ctx.event_tx);

        assert!(state.track_io_requests.try_iter().next().is_none());
        let snapshots: Vec<_> = channels
            .event_rx
            .try_iter()
            .filter_map(|event| match event {
                AnalysisEvent::Snapshot(snapshot) => Some(snapshot),
                _ => None,
            })
            .collect();
        let [snapshot] = snapshots.as_slice() else {
            panic!("{} snapshots", snapshots.len());
        };
        assert!(!snapshot.waveform_complete);
        assert_eq!(snapshot.waveform_peaks.len(), MIN_PREVIEW_PEAKS);
        assert!((snapshot.waveform_coverage_seconds - 0.5).abs() < f32::EPSILON);
    }

    #[test]
    fn an_mp3_overview_starts_where_playback_does() {
        // A click at 0.2 s of 0.4 s; see `fixtures/README.md`.
        let bytes = include_bytes!("fixtures/gapless-click.mp3").to_vec();
        let updates = symphonia_overview(Box::new(std::io::Cursor::new(bytes)), "mp3");
        let (peaks, coverage, progress) = updates.last().expect("final update");
        assert_eq!(*progress, OverviewProgress::Complete);
        assert!((coverage - 0.4).abs() < 0.001, "covers {coverage} s");
        let loudest = (0..peaks.len())
            .max_by(|&a, &b| peaks[a].total_cmp(&peaks[b]))
            .expect("peaks");
        // Each peak covers an equal share of the 17,640 frames.
        #[allow(clippy::cast_precision_loss)]
        let frames_per_peak = 17_640.0 / peaks.len() as f64;
        #[allow(clippy::cast_precision_loss)]
        let click = loudest as f64 * frames_per_peak;
        assert!(
            (click - 8_820.0).abs() <= 2.0 * frames_per_peak,
            "the click is near frame {click}"
        );
    }

    #[test]
    fn a_gapless_successor_is_probed_once_off_the_analysis_thread() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
        state.last_spectrogram_position = 200.0;

        // A probe answered after its staging was dropped is ignored.
        state.handle_prepare_gapless_continuation(b.to_path_buf(), &ctx);
        let TrackIoRequest::ProbeCandidate { request: stale, .. } = only_track_io_request(&state)
        else {
            panic!("expected a probe");
        };
        state.handle_command(AnalysisCommand::ClearStagedContinuation, &ctx);
        let compatible = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 2,
        };
        state.handle_track_io_reply(
            TrackIoReply::CandidateProbed {
                request: stale,
                stamp: None,
                format: compatible,
            },
            &ctx,
        );
        assert!(state.staged_continuation_path.is_none());

        state.handle_prepare_gapless_continuation(b.to_path_buf(), &ctx);
        let TrackIoRequest::ProbeCandidate { request, .. } = only_track_io_request(&state) else {
            panic!("expected a probe");
        };
        assert!(channels.spectrogram_cmd_rx.try_recv().is_err());
        state.handle_track_io_reply(
            TrackIoReply::CandidateProbed {
                request,
                stamp: Some(WaveformSourceStamp {
                    size_bytes: 10,
                    modified_secs: 1,
                    modified_nanos: 0,
                    file_id: None,
                }),
                format: compatible,
            },
            &ctx,
        );
        assert!(matches!(
            channels.spectrogram_cmd_rx.try_recv(),
            Ok(SpectrogramWorkerCommand::ContinueWithFile { ref path, .. }) if path == b
        ));

        // The switch uses what the probe found.
        state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
        assert!(matches!(
            channels.spectrogram_cmd_rx.try_recv(),
            Ok(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 })
        ));
        assert!(matches!(
            only_track_io_request(&state),
            TrackIoRequest::LoadTrack {
                probed_at: Some(_),
                ..
            }
        ));
        assert!((state.spectrogram_position_offset - 200.0).abs() < 0.001);
    }

    #[test]
    fn an_unprobed_rolling_successor_starts_a_session_that_keeps_history() {
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = playing_state(SpectrogramDisplayMode::Rolling, Path::new("/music/a.flac"));
        state.last_spectrogram_position = 200.0;

        // Without its format the switch cannot know that the worker's
        // continuous timeline still fits.
        state.handle_track_change(PathBuf::from("/music/b.flac"), false, true, 2, &ctx);
        let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
        assert!(
            matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                start_seconds, emit_initial_reset: true, clear_history_on_reset: false, ..
            }] if *start_seconds == 0.0),
            "{commands:?}"
        );
        assert_eq!(state.spectrogram_position_offset, 0.0);
    }

    #[test]
    fn a_lost_file_helper_is_reported_and_waveforms_are_still_decoded() {
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = AnalysisRuntimeState::with_track_io(unbounded().0);
        state.handle_track_change(PathBuf::from("/music/a.flac"), true, false, 4, &ctx);
        assert!(matches!(
            channels.waveform_job_rx.try_recv(),
            Ok(WaveformDecodeJob { track_token: 4, .. })
        ));
        state.handle_track_change(PathBuf::from("/music/b.flac"), true, false, 5, &ctx);
        assert!(matches!(
            channels.waveform_job_rx.try_recv(),
            Ok(WaveformDecodeJob { track_token: 5, .. })
        ));
        let failures = channels
            .event_rx
            .try_iter()
            .filter(|event| matches!(event, AnalysisEvent::WorkerFailed(_)))
            .count();
        assert_eq!(failures, 1, "the loss is reported once");

        // A load lost with the helper is decoded too.
        let (track_io, requests) = unbounded();
        let mut state = AnalysisRuntimeState::with_track_io(track_io);
        state.handle_track_change(PathBuf::from("/music/a.flac"), true, false, 6, &ctx);
        drop(requests);
        state.handle_track_io_lost(&ctx);
        assert!(matches!(
            channels.waveform_job_rx.try_recv(),
            Ok(WaveformDecodeJob { track_token: 6, .. })
        ));
    }

    #[test]
    fn repeat_one_stages_the_playing_file_without_probing_it_again() {
        let a = Path::new("/music/a.flac");
        for mode in MODES {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = AnalysisRuntimeState::new();
            state.display_mode = mode;
            // Held, so the centered staging thread never opens the file.
            let (release_staging, staging_gate) = crossbeam_channel::bounded::<()>(0);
            state.staging_open_gate = Some(staging_gate);
            state.handle_track_change(a.to_path_buf(), true, false, 1, &ctx);
            let _ = only_track_io_request(&state);
            state.handle_track_io_reply(
                TrackIoReply::TrackLoaded {
                    track_token: 1,
                    stamp: Some(WaveformSourceStamp {
                        size_bytes: 10,
                        modified_secs: 1,
                        modified_nanos: 0,
                        file_id: None,
                    }),
                    format: Some(TrackFormat::Known {
                        sample_rate: 44_100,
                        channels: 2,
                    }),
                    peaks: Some(vec![0.5; 4]),
                },
                &ctx,
            );
            channels.spectrogram_cmd_rx.try_iter().for_each(drop);
            let generation = channels.generation.load(Ordering::Relaxed);

            // About to finish: the same file follows.
            state.handle_prepare_gapless_continuation(a.to_path_buf(), &ctx);
            assert!(state.track_io_requests.try_recv().is_err(), "{mode:?}");
            if mode == SpectrogramDisplayMode::Rolling {
                assert!(matches!(
                    channels.spectrogram_cmd_rx.try_recv(),
                    Ok(SpectrogramWorkerCommand::ContinueWithFile { track_token: 1, .. })
                ));
            } else {
                assert!(state.staged_centered_thread.is_some());
            }

            state.handle_track_change(a.to_path_buf(), false, true, 2, &ctx);
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            if mode == SpectrogramDisplayMode::Rolling {
                assert!(
                    matches!(
                        commands.as_slice(),
                        [SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 }]
                    ),
                    "{commands:?}"
                );
            } else {
                // Nothing was staged yet: a reset in the same generation,
                // after the outgoing track's extent.
                assert!(
                    matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                        track_token: 2, generation: g, emit_initial_reset: true, handoff, ..
                    }] if *g == generation && handoff.finalize_previous),
                    "{commands:?}"
                );
            }
            assert!(matches!(
                only_track_io_request(&state),
                TrackIoRequest::LoadTrack {
                    track_token: 2,
                    probed_at: Some(_),
                    ..
                }
            ));
            drop(release_staging);
            state.handle_command(AnalysisCommand::Shutdown, &ctx);
            assert!(state.retired_staging.is_empty());
        }
    }

    #[test]
    fn a_rolling_continuation_on_a_format_that_changed_starts_afresh_keeping_history() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let probed_at = WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: None,
        };
        let changed = WaveformSourceStamp {
            modified_secs: 2,
            ..probed_at
        };
        let cd = TrackFormat::Known {
            sample_rate: 44_100,
            channels: 2,
        };
        let hi_res = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 2,
        };
        // B was probed at 44.1 kHz, then replaced by a 48 kHz file; with and
        // without the continuation sent early, and when the file still fits.
        for (staged_early, reprobed, restarts) in [
            (false, hi_res, true),
            (true, hi_res, true),
            (false, cd, false),
        ] {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
            state.active_session_effective_rate = 44_100;
            state.last_spectrogram_position = 200.0;
            state.formats.insert(b.to_path_buf(), cd, Some(probed_at));
            if staged_early {
                state.handle_prepare_gapless_continuation(b.to_path_buf(), &ctx);
                assert!(state.staged_continuation_path.is_some());
            }
            state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
            assert!((state.spectrogram_position_offset - 200.0).abs() < 0.001);
            assert!(matches!(
                only_track_io_request(&state),
                TrackIoRequest::LoadTrack { probed_at: Some(stamp), .. } if stamp == probed_at
            ));
            channels.spectrogram_cmd_rx.try_iter().for_each(drop);
            // Playback has moved on in B meanwhile.
            state.handle_command(AnalysisCommand::PositionUpdate(0.3), &ctx);
            channels.spectrogram_cmd_rx.try_iter().for_each(drop);

            let stamp = if restarts { changed } else { probed_at };
            state.handle_track_io_reply(
                TrackIoReply::TrackLoaded {
                    track_token: 2,
                    stamp: Some(stamp),
                    format: restarts.then_some(reprobed),
                    peaks: None,
                },
                &ctx,
            );
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            if restarts {
                assert_eq!(state.spectrogram_position_offset, 0.0, "{staged_early}");
                assert_eq!(state.active_session_effective_rate, 48_000);
                assert!(
                    matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                        track_token: 2, start_seconds, emit_initial_reset: true,
                        clear_history_on_reset: false, ..
                    }] if (*start_seconds - 0.3).abs() < 0.001),
                    "staged_early={staged_early}: {commands:?}"
                );
            } else {
                assert!(commands.is_empty(), "{commands:?}");
                assert!((state.spectrogram_position_offset - 200.0).abs() < 0.001);
            }
        }
    }

    /// A tone encoded by `encoder` at `rate` Hz with `channels` channels.
    #[cfg(feature = "gst")]
    fn encode_tone(
        extension: &str,
        encoder: &str,
        seconds: u32,
        rate: u32,
        channels: u32,
    ) -> PathBuf {
        use gstreamer as gst;
        use gstreamer::prelude::*;
        static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(0);
        gst::init().expect("gstreamer initialization");
        let path = std::env::temp_dir().join(format!(
            "ferrous-analysis-tone-{}-{}.{extension}",
            std::process::id(),
            NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed)
        ));
        let buffers = (seconds * rate).div_ceil(1024);
        let pipeline = gst::parse::launch(&format!(
            "audiotestsrc num-buffers={buffers} samplesperbuffer=1024 freq=440 ! audioconvert \
             ! audio/x-raw,rate={rate},channels={channels} ! audioconvert ! {encoder} \
             ! filesink location={}",
            path.display()
        ))
        .expect("encoder pipeline");
        pipeline.set_state(gst::State::Playing).expect("encode");
        let message = pipeline.bus().expect("bus").timed_pop_filtered(
            gst::ClockTime::from_seconds(30),
            &[gst::MessageType::Eos, gst::MessageType::Error],
        );
        pipeline.set_state(gst::State::Null).expect("stop encoder");
        assert!(
            matches!(
                message.as_ref().map(|message| message.view()),
                Some(gst::MessageView::Eos(_))
            ),
            "{encoder}: {message:?}"
        );
        path
    }

    /// Events until one is a chunk that `done` accepts, calling `tick`
    /// every 50 ms, as playback publishes its position.
    fn events_until(
        rx: &Receiver<AnalysisEvent>,
        tick: impl Fn(),
        done: impl Fn(&PrecomputedSpectrogramChunk) -> bool,
    ) -> Vec<AnalysisEvent> {
        let deadline = std::time::Instant::now() + Duration::from_secs(30);
        let mut events = Vec::new();
        let mut next_tick = std::time::Instant::now();
        loop {
            let now = std::time::Instant::now();
            assert!(now < deadline, "timed out after:\n{}", summarize(&events));
            if now >= next_tick {
                tick();
                next_tick = now + Duration::from_millis(50);
            }
            let Ok(event) = rx.recv_timeout(next_tick.saturating_duration_since(now)) else {
                continue;
            };
            let finished =
                matches!(&event, AnalysisEvent::PrecomputedSpectrogramChunk(chunk) if done(chunk));
            events.push(event);
            if finished {
                return events;
            }
        }
    }

    /// One file of a gapless pair with different formats.
    #[cfg(feature = "gst")]
    struct Tone {
        rate: u32,
        channels: u32,
        seconds: u32,
    }

    /// Play `first` to its end and switch gaplessly to `second`, whose format
    /// differs, in `mode`, and check that the switch restarts cleanly.
    #[cfg(feature = "gst")]
    fn assert_format_change_restarts_cleanly(
        encoded: (&str, &str),
        view: SpectrogramViewMode,
        mode: SpectrogramDisplayMode,
        (first, second): (Tone, Tone),
    ) {
        let (extension, encoder) = encoded;
        let a = encode_tone(
            extension,
            encoder,
            first.seconds,
            first.rate,
            first.channels,
        );
        let b = encode_tone(
            extension,
            encoder,
            second.seconds,
            second.rate,
            second.channels,
        );
        let case = format!("{extension} {view:?} {mode:?}");
        let (engine, events) = AnalysisEngine::new();
        engine.command(AnalysisCommand::SetSpectrogramViewMode(view));
        engine.command(AnalysisCommand::SetSpectrogramDisplayMode(mode));
        let set_track = |path: &Path, track_token, gapless: bool| AnalysisCommand::SetTrack {
            path: path.to_path_buf(),
            reset_spectrogram: !gapless,
            track_token,
            gapless,
        };
        // Symphonia cannot read WavPack, so its format is known only from
        // playing it, once the file helper's probe has failed.
        let play_at = |position: f64| {
            let engine = &engine;
            move || engine.command(AnalysisCommand::PositionUpdate(position))
        };
        if extension == "wv" {
            engine.command(set_track(&b, 1, false));
            events_until(
                &events,
                || {},
                |chunk| chunk.track_token == 1 && chunk.column_count > 0,
            );
        }
        engine.command(set_track(&a, 2, false));
        // Near its end; the worker decodes it to the end.
        events_until(&events, play_at(f64::from(first.seconds) - 0.5), |chunk| {
            chunk.track_token == 2 && chunk.complete
        });

        // About to finish, then the switch, and playback in the new track.
        engine.command(AnalysisCommand::PrepareGaplessContinuation { path: b.clone() });
        engine.command(set_track(&b, 3, true));
        engine.command(AnalysisCommand::PositionUpdate(0.5));
        let after = events_until(&events, play_at(f64::from(second.seconds) - 0.5), |chunk| {
            chunk.track_token == 3 && chunk.complete
        });
        assert!(
            !after
                .iter()
                .any(|event| matches!(event, AnalysisEvent::WorkerFailed(_))),
            "{case}:\n{}",
            summarize(&after)
        );
        let chunks: Vec<_> = after
            .iter()
            .filter_map(|event| match event {
                AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => Some(chunk),
                _ => None,
            })
            .collect();
        let channels = match view {
            SpectrogramViewMode::Downmix => 1,
            SpectrogramViewMode::PerChannel => u8::try_from(second.channels).unwrap(),
        };
        let is_second_format = |chunk: &PrecomputedSpectrogramChunk| {
            chunk.sample_rate_hz == second.rate && chunk.channel_count == channels
        };
        // The outgoing track's token never labels the new file's columns.
        assert!(
            !chunks.iter().any(|chunk| chunk.track_token != 3
                && chunk.bins_per_column > 0
                && is_second_format(chunk)),
            "{case}:\n{}",
            summarize(&after)
        );
        let new_track: Vec<_> = chunks
            .iter()
            .filter(|chunk| chunk.track_token == 3)
            .collect();
        let reset = new_track[0];
        assert!(
            reset.buffer_reset && reset.bins_per_column > 0 && is_second_format(reset),
            "{case}: the new track starts without its reset:\n{}",
            summarize(&after)
        );
        if mode == SpectrogramDisplayMode::Rolling {
            assert!(
                !reset.clear_history,
                "{case}: the rolling history is cleared"
            );
        }
        for chunk in &new_track {
            // Positions are in the new track's timeline, not the continued
            // one, so no seek lands past its end.
            assert!(
                is_second_format(chunk) && chunk.start_column_index <= reset.total_columns_estimate,
                "{case}:\n{}",
                summarize(&after)
            );
        }
        drop(engine);
        std::fs::remove_file(a).expect("remove fixture");
        std::fs::remove_file(b).expect("remove fixture");
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_gapless_switch_to_another_sample_rate_restarts_cleanly() {
        for mode in MODES {
            assert_format_change_restarts_cleanly(
                ("wv", "wavpackenc"),
                SpectrogramViewMode::Downmix,
                mode,
                (
                    Tone {
                        rate: 44_100,
                        channels: 1,
                        seconds: 20,
                    },
                    Tone {
                        rate: 96_000,
                        channels: 1,
                        seconds: 3,
                    },
                ),
            );
            assert_format_change_restarts_cleanly(
                ("ac3", "avenc_ac3"),
                SpectrogramViewMode::Downmix,
                mode,
                (
                    Tone {
                        rate: 44_100,
                        channels: 2,
                        seconds: 10,
                    },
                    Tone {
                        rate: 48_000,
                        channels: 2,
                        seconds: 3,
                    },
                ),
            );
        }
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_gapless_switch_to_another_channel_count_restarts_cleanly() {
        for mode in MODES {
            assert_format_change_restarts_cleanly(
                ("wv", "wavpackenc"),
                SpectrogramViewMode::PerChannel,
                mode,
                (
                    Tone {
                        rate: 44_100,
                        channels: 1,
                        seconds: 20,
                    },
                    Tone {
                        rate: 44_100,
                        channels: 2,
                        seconds: 3,
                    },
                ),
            );
            assert_format_change_restarts_cleanly(
                ("ac3", "avenc_ac3"),
                SpectrogramViewMode::PerChannel,
                mode,
                (
                    Tone {
                        rate: 48_000,
                        channels: 2,
                        seconds: 10,
                    },
                    Tone {
                        rate: 48_000,
                        channels: 6,
                        seconds: 3,
                    },
                ),
            );
        }
    }
    #[test]
    fn a_file_the_spectrogram_cannot_read_is_reported_and_the_next_track_analyses() {
        let broken = write_fixture("broken.flac", b"fLaC but not really audio");
        let good = write_engine_test_wave(1, 16_000);
        // Symphonia cannot read the broken file, so the worker tries
        // GStreamer, whose first initialisation builds its registry.  With
        // a fresh cache, as the test script gives each run, that takes
        // seconds, and it is not what the waits below are for.
        #[cfg(feature = "gst")]
        gstreamer::init().expect("gstreamer initialization");
        for mode in MODES {
            let (engine, events) = AnalysisEngine::new();
            engine.command(AnalysisCommand::SetSpectrogramDisplayMode(mode));
            engine.command(AnalysisCommand::SetTrack {
                path: broken.clone(),
                reset_spectrogram: true,
                track_token: 1,
                gapless: false,
            });
            // The worker clears what the UI shows, as a far seek does.
            events_until(
                &events,
                || {},
                |chunk| chunk.track_token == 1 && chunk.buffer_reset && chunk.bins_per_column == 0,
            );
            let message = loop {
                match events.recv_timeout(Duration::from_secs(10)) {
                    Ok(AnalysisEvent::WorkerFailed(message)) => break message,
                    Ok(AnalysisEvent::PrecomputedSpectrogramChunk(chunk)) => {
                        assert_ne!(chunk.track_token, 1, "{mode:?}: {chunk:?}");
                    }
                    Ok(AnalysisEvent::Snapshot(_)) => {}
                    Err(error) => panic!("{mode:?}: the failure is not reported: {error}"),
                }
            };
            let name = broken.file_name().unwrap().to_string_lossy().into_owned();
            assert_eq!(
                message,
                format!("The spectrogram cannot show {name}: its audio could not be read."),
                "{mode:?}"
            );

            // The worker is still there for the next track.
            engine.command(AnalysisCommand::SetTrack {
                path: good.clone(),
                reset_spectrogram: true,
                track_token: 2,
                gapless: false,
            });
            let next = events_until(
                &events,
                || {},
                |chunk| chunk.track_token == 2 && chunk.column_count > 0,
            );
            assert!(
                !next
                    .iter()
                    .any(|event| matches!(event, AnalysisEvent::WorkerFailed(_))),
                "{mode:?}:\n{}",
                summarize(&next)
            );
        }
        std::fs::remove_file(broken).expect("remove fixture");
        std::fs::remove_file(good).expect("remove fixture");
    }

    fn opened(
        generation: u64,
        track_token: u64,
        path: &Path,
        format: TrackFormat,
    ) -> SessionReport {
        SessionReport::Opened {
            generation,
            track_token,
            path: path.to_path_buf(),
            stamp: None,
            format,
            continued_at: None,
        }
    }

    #[test]
    fn the_format_the_worker_opened_replaces_a_failed_probe_in_both_modes() {
        let (a, b) = (Path::new("/music/a.wv"), Path::new("/music/b.wv"));
        let hi_res = TrackFormat::Known {
            sample_rate: 96_000,
            channels: 2,
        };
        for mode in MODES {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = playing_state(mode, a);
            state.active_session_effective_rate = 44_100;
            state.spectrogram_view_mode = SpectrogramViewMode::PerChannel;
            state.active_session_channel_count = 1;
            // The helper could not read B before; the switch cannot know.
            state
                .formats
                .insert(b.to_path_buf(), TrackFormat::Unknown, None);
            state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
            let _ = only_track_io_request(&state);
            let generation = channels.generation.load(Ordering::Relaxed);
            channels.spectrogram_cmd_rx.try_iter().for_each(drop);

            // A report from a session since superseded changes nothing.
            state.handle_session_report(opened(generation + 1, 2, b, hi_res), &ctx);
            assert_eq!(state.active_session_effective_rate, 44_100, "{mode:?}");
            state.handle_session_report(opened(generation, 2, b, hi_res), &ctx);
            assert_eq!(state.active_session_effective_rate, 96_000, "{mode:?}");
            assert_eq!(state.active_session_channel_count, 2, "{mode:?}");
            // The helper's failed probe, answered later, does not undo it.
            state.handle_track_io_reply(
                TrackIoReply::TrackLoaded {
                    track_token: 2,
                    stamp: None,
                    format: Some(TrackFormat::Unknown),
                    peaks: None,
                },
                &ctx,
            );
            assert_eq!(state.active_session_effective_rate, 96_000, "{mode:?}");
            assert_eq!(
                state.formats.get(b).map(|probed| probed.format),
                Some(hi_res)
            );
            let cols_per_second = state.current_centered_cols_per_second().expect("rate");
            assert!(
                (cols_per_second - 96_000.0 / 1024.0).abs() < 0.001,
                "{mode:?}"
            );

            // A is judged from what the worker read of B when it follows.
            state.handle_session_report(
                opened(
                    generation,
                    2,
                    a,
                    TrackFormat::Known {
                        sample_rate: 44_100,
                        channels: 1,
                    },
                ),
                &ctx,
            );
            assert_eq!(state.active_session_effective_rate, 96_000, "{mode:?}");
            state.last_spectrogram_position = 100.0;
            state.handle_track_change(a.to_path_buf(), false, true, 3, &ctx);
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            assert!(
                matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                    track_token: 3, start_seconds, emit_initial_reset: true, ..
                }] if *start_seconds == 0.0),
                "{mode:?}: {commands:?}"
            );
            assert_eq!(state.spectrogram_position_offset, 0.0);
        }
    }

    #[test]
    fn a_rolling_continuation_the_worker_rejects_starts_afresh_at_the_playback_position() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let surround = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 6,
        };
        let rejected = |generation, path: &Path| SessionReport::ContinuationRejected {
            generation,
            path: path.to_path_buf(),
            stamp: None,
            format: Some(surround),
        };
        // Rejected before the switch, or after it.
        for rejected_first in [true, false] {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
            state.spectrogram_view_mode = SpectrogramViewMode::PerChannel;
            state.active_session_channel_count = 2;
            state.last_spectrogram_position = 200.0;
            // B was probed in stereo, and has been replaced by a 5.1 file
            // since: the worker finds that when it opens it.
            state.formats.insert(
                b.to_path_buf(),
                TrackFormat::Known {
                    sample_rate: 48_000,
                    channels: 2,
                },
                None,
            );
            state.handle_prepare_gapless_continuation(b.to_path_buf(), &ctx);
            assert!(matches!(
                channels.spectrogram_cmd_rx.try_recv(),
                Ok(SpectrogramWorkerCommand::ContinueWithFile { .. })
            ));
            let generation = channels.generation.load(Ordering::Relaxed);
            if rejected_first {
                state.handle_session_report(rejected(generation, b), &ctx);
                assert!(state.staged_continuation_path.is_none());
                assert!(channels.spectrogram_cmd_rx.try_recv().is_err());
            }
            state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
            if !rejected_first {
                assert!(matches!(
                    channels.spectrogram_cmd_rx.try_recv(),
                    Ok(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 })
                ));
                assert!((state.spectrogram_position_offset - 200.0).abs() < 0.001);
                state.handle_command(AnalysisCommand::PositionUpdate(0.4), &ctx);
                channels.spectrogram_cmd_rx.try_iter().for_each(drop);
                state.handle_session_report(rejected(generation, b), &ctx);
            }
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            let start = if rejected_first { 0.0 } else { 0.4 };
            assert!(
                matches!(commands.as_slice(), [SpectrogramWorkerCommand::NewTrack {
                    track_token: 2, start_seconds, emit_initial_reset: true,
                    clear_history_on_reset: false, channel_count: 6, generation: g, ..
                }] if (*start_seconds - start).abs() < 0.001 && *g > generation),
                "rejected_first={rejected_first}: {commands:?}"
            );
            assert_eq!(state.spectrogram_position_offset, 0.0);
            state.handle_command(AnalysisCommand::PositionUpdate(0.5), &ctx);
            assert!(matches!(
                channels.spectrogram_cmd_rx.try_recv(),
                Ok(SpectrogramWorkerCommand::PositionUpdate { position_seconds }) if position_seconds == 0.5
            ));
            // A rejection the restart superseded changes nothing.
            state.handle_session_report(rejected(generation, b), &ctx);
            assert!(channels.spectrogram_cmd_rx.try_recv().is_err());
        }
    }

    #[test]
    fn a_rolling_continuation_offsets_positions_to_where_the_worker_went_on() {
        // Playback's last position before a gapless switch falls short of
        // the end of the outgoing track, which is where the worker goes on
        // with the next file: position 0 of the file is there.
        let a = Path::new("/music/a.flac");
        let format = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 1,
        };
        let (last_position, continued_at) = (1_257.23, 1_257.45);
        let opened = |generation, track_token, path: &Path| SessionReport::Opened {
            generation,
            track_token,
            path: path.to_path_buf(),
            stamp: None,
            format,
            continued_at: Some(continued_at),
        };
        let forwarded = |channels: &TestChannels| -> Vec<f64> {
            channels
                .spectrogram_cmd_rx
                .try_iter()
                .filter_map(|command| match command {
                    SpectrogramWorkerCommand::PositionUpdate { position_seconds } => {
                        Some(position_seconds)
                    }
                    _ => None,
                })
                .collect()
        };
        // Repeat-One, and another track; staged early or continued at the
        // switch; opened by the worker before the switch or after it.
        for next in [a, Path::new("/music/b.flac")] {
            for (staged, opened_first) in [(true, true), (true, false), (false, false)] {
                let case = format!(
                    "{} staged={staged} opened_first={opened_first}",
                    next.display()
                );
                let channels = TestChannels::new();
                let ctx = channels.ctx();
                let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
                state.formats.insert(next.to_path_buf(), format, None);
                state.last_spectrogram_position = last_position;
                let generation = channels.generation.load(Ordering::Relaxed);
                if staged {
                    state.handle_prepare_gapless_continuation(next.to_path_buf(), &ctx);
                    assert!(
                        matches!(
                            channels.spectrogram_cmd_rx.try_recv(),
                            Ok(SpectrogramWorkerCommand::ContinueWithFile { track_token: 1, .. })
                        ),
                        "{case}"
                    );
                }
                if opened_first {
                    // The worker reached the end of the outgoing track first.
                    state.handle_session_report(opened(generation, 1, next), &ctx);
                }
                state.handle_track_change(next.to_path_buf(), false, true, 2, &ctx);
                let switch = channels.spectrogram_cmd_rx.try_recv();
                if staged {
                    assert!(
                        matches!(
                            switch,
                            Ok(SpectrogramWorkerCommand::UpdateTrackToken { track_token: 2 })
                        ),
                        "{case}: {switch:?}"
                    );
                } else {
                    assert!(
                        matches!(
                            switch,
                            Ok(SpectrogramWorkerCommand::ContinueWithFile { track_token: 2, .. })
                        ),
                        "{case}: {switch:?}"
                    );
                }
                if !opened_first {
                    // Until the worker reports where it went on, the
                    // outgoing track ends at its last position.
                    assert!(
                        (state.spectrogram_position_offset - last_position).abs() < 1e-9,
                        "{case}"
                    );
                    state.handle_command(AnalysisCommand::PositionUpdate(0.1), &ctx);
                    assert_eq!(forwarded(&channels), [last_position + 0.1], "{case}");
                    let token = if staged { 1 } else { 2 };
                    state.handle_session_report(opened(generation, token, next), &ctx);
                }
                assert!(
                    (state.spectrogram_position_offset - continued_at).abs() < 1e-9,
                    "{case}"
                );
                // Positions that step back after the switch stay in the
                // file, at or after its start.
                for position in [0.0, 0.23, 0.09] {
                    state.handle_command(AnalysisCommand::PositionUpdate(position), &ctx);
                }
                assert_eq!(
                    forwarded(&channels),
                    [continued_at, continued_at + 0.23, continued_at + 0.09],
                    "{case}"
                );
                // A later report of the same file, such as a stale one,
                // does not move the offset.
                state.handle_session_report(opened(generation, 2, next), &ctx);
                assert!(
                    (state.spectrogram_position_offset - continued_at).abs() < 1e-9,
                    "{case}"
                );
            }
        }
    }

    #[test]
    fn a_seek_before_the_worker_reaches_the_continuation_restarts_the_session() {
        // Playback switched to a continuation the worker has not opened yet,
        // or the worker may decode a staged successor: a Seek would move
        // the file it decodes, which the seek is not in.
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let format = TrackFormat::Known {
            sample_rate: 48_000,
            channels: 1,
        };
        for switched in [true, false] {
            let channels = TestChannels::new();
            let ctx = channels.ctx();
            let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
            state.formats.insert(b.to_path_buf(), format, None);
            state.last_spectrogram_position = 200.0;
            state.handle_prepare_gapless_continuation(b.to_path_buf(), &ctx);
            if switched {
                state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
            }
            let generation = channels.generation.load(Ordering::Relaxed);
            channels.spectrogram_cmd_rx.try_iter().for_each(drop);

            state.handle_command(AnalysisCommand::SeekPosition(5.0), &ctx);
            let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
            // A staged continuation is cancelled; the new session drops one
            // the switch made.
            let commands = if switched {
                commands.as_slice()
            } else {
                assert!(
                    matches!(
                        commands.first(),
                        Some(SpectrogramWorkerCommand::CancelPendingContinue)
                    ),
                    "{commands:?}"
                );
                &commands[1..]
            };
            let token = if switched { 2 } else { 1 };
            assert!(
                matches!(commands, [
                    SpectrogramWorkerCommand::NewTrack {
                        track_token, start_seconds, emit_initial_reset: true,
                        clear_history_on_reset: false, generation: g, ..
                    },
                ] if *track_token == token && (*start_seconds - 5.0).abs() < 1e-9 && *g > generation),
                "switched={switched}: {commands:?}"
            );
            assert_eq!(state.spectrogram_position_offset, 0.0);
            // What the superseded session reports changes nothing.
            state.handle_session_report(
                SessionReport::Opened {
                    generation,
                    track_token: 2,
                    path: b.to_path_buf(),
                    stamp: None,
                    format,
                    continued_at: Some(200.4),
                },
                &ctx,
            );
            assert_eq!(state.spectrogram_position_offset, 0.0);
            state.handle_command(AnalysisCommand::PositionUpdate(5.1), &ctx);
            assert!(matches!(
                channels.spectrogram_cmd_rx.try_recv(),
                Ok(SpectrogramWorkerCommand::PositionUpdate { position_seconds }) if position_seconds == 5.1
            ));
        }

        // Once the worker has opened the file playback switched to, a seek
        // reaches the session.
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = playing_state(SpectrogramDisplayMode::Rolling, a);
        state.formats.insert(b.to_path_buf(), format, None);
        state.last_spectrogram_position = 200.0;
        state.handle_track_change(b.to_path_buf(), false, true, 2, &ctx);
        let generation = channels.generation.load(Ordering::Relaxed);
        state.handle_session_report(
            SessionReport::Opened {
                generation,
                track_token: 2,
                path: b.to_path_buf(),
                stamp: None,
                format,
                continued_at: Some(200.4),
            },
            &ctx,
        );
        channels.spectrogram_cmd_rx.try_iter().for_each(drop);
        state.handle_command(AnalysisCommand::SeekPosition(5.0), &ctx);
        let commands: Vec<_> = channels.spectrogram_cmd_rx.try_iter().collect();
        assert!(
            matches!(commands.as_slice(), [SpectrogramWorkerCommand::Seek { position_seconds }]
                if (*position_seconds - 5.0).abs() < 1e-9),
            "{commands:?}"
        );
    }

    #[test]
    fn only_a_failure_of_the_current_session_is_reported() {
        let path = Path::new("/music/broken.flac");
        let channels = TestChannels::new();
        let ctx = channels.ctx();
        let mut state = playing_state(SpectrogramDisplayMode::Rolling, path);
        let failed = |generation, track_token| SessionReport::Failed {
            generation,
            track_token,
            path: path.to_path_buf(),
            error: "decoding failed (bad data)".to_string(),
        };
        state.handle_session_report(failed(1, 1), &ctx);
        state.handle_session_report(failed(0, 7), &ctx);
        assert!(channels.event_rx.try_recv().is_err());
        state.handle_session_report(failed(0, 1), &ctx);
        assert!(matches!(
            channels.event_rx.try_recv(),
            Ok(AnalysisEvent::WorkerFailed(message))
                if message == "The spectrogram cannot show broken.flac: decoding failed (bad data)."
        ));
    }
}
