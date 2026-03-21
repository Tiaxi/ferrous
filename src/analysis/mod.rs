use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{select, unbounded, Receiver, Sender};
use realfft::{num_complex::Complex32, RealFftPlanner, RealToComplex};
use rusqlite::{params, Connection};
use symphonia::core::audio::{SampleBuffer, SignalSpec};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;

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
    SetSpectrogramViewMode(SpectrogramViewMode),
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
    pub spectrogram_channels: Vec<AnalysisSpectrogramChannel>,
    pub spectrogram_seq: u64,
    pub sample_rate_hz: u32,
    pub spectrogram_view_mode: SpectrogramViewMode,
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

const MAX_WAVEFORM_CACHE_TRACKS: usize = 256;
const PERSISTENT_WAVEFORM_CACHE_MAX_ROWS: usize = 4096;
const PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL: usize = 24;
const WAVEFORM_CACHE_FORMAT_VERSION: i64 = 1;
const BASE_VISUAL_DELAY_MS: i32 = 0;
const REFERENCE_HOP: usize = 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct WaveformSourceStamp {
    size_bytes: u64,
    modified_secs: u64,
    modified_nanos: u32,
}

#[derive(Debug, Clone)]
struct WaveformCacheEntry {
    stamp: Option<WaveformSourceStamp>,
    peaks: Vec<f32>,
}

#[derive(Debug, Clone)]
struct WaveformDecodeJob {
    track_token: u64,
    path: PathBuf,
}

#[derive(Debug, Clone)]
enum SpectrogramWorkerCommand {
    NewTrack {
        track_token: u64,
        generation: u64,
        path: PathBuf,
        fft_size: usize,
        hop_size: usize,
        channel_count: usize,
        start_seconds: f64,
        clear_history_on_reset: bool,
        view_mode: SpectrogramViewMode,
        display_mode: SpectrogramDisplayMode,
    },
    /// Gapless: finish current file, seamlessly open next without resetting
    /// STFT state or sequence counter. The ring buffer keeps history from
    /// the previous track for visual continuity.
    GaplessTransition {
        track_token: u64,
        path: PathBuf,
    },
    PositionUpdate {
        position_seconds: f64,
    },
    Seek {
        position_seconds: f64,
    },
    #[allow(dead_code)]
    SetDisplayMode(SpectrogramDisplayMode),
    Stop,
}

struct AnalysisRuntimeState {
    snapshot: AnalysisSnapshot,
    pending_channels: Vec<AnalysisSpectrogramChannel>,
    waveform_dirty: bool,
    last_emit: std::time::Instant,
    spectrogram: SpectrogramRuntime,
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
        spawn_spectrogram_decode_worker(
            spectrogram_cmd_rx,
            event_tx.clone(),
            Arc::clone(&waveform_decode_active_token),
            Arc::clone(&spectrogram_decode_generation),
        );

        spawn_analysis_worker(
            cmd_rx,
            pcm_rx,
            event_tx,
            waveform_job_tx,
            waveform_decode_active_token,
            spectrogram_cmd_tx,
            spectrogram_decode_generation,
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
}

impl AnalysisRuntimeState {
    fn new() -> Self {
        Self {
            snapshot: AnalysisSnapshot {
                sample_rate_hz: 48_000,
                spectrogram_view_mode: SpectrogramViewMode::Downmix,
                ..AnalysisSnapshot::default()
            },
            pending_channels: Vec::new(),
            waveform_dirty: false,
            last_emit: std::time::Instant::now(),
            spectrogram: SpectrogramRuntime::new(8192, 1024, SpectrogramViewMode::Downmix, &[]),
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
        }
    }

    fn handle_command(&mut self, cmd: AnalysisCommand, ctx: &AnalysisContext<'_>) {
        match cmd {
            AnalysisCommand::SetTrack {
                ref path,
                reset_spectrogram,
                track_token,
                gapless,
            } => {
                eprintln!(
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
                let fft = size.clamp(512, 8192).next_power_of_two();
                let hop = (fft / 8).max(64);
                self.spectrogram.set_fft_size(fft, hop);
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(0.0, true, ctx);
            }
            AnalysisCommand::SetSpectrogramViewMode(view_mode) => {
                eprintln!("[analysis] SetSpectrogramViewMode({view_mode:?})");
                self.snapshot.spectrogram_view_mode = view_mode;
                self.spectrogram.set_view_mode(view_mode);
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(0.0, true, ctx);
            }
            AnalysisCommand::RestartCurrentTrack {
                position_seconds,
                clear_history,
            } => {
                eprintln!(
                    "[analysis] RestartCurrentTrack pos={position_seconds:.2} clear_history={clear_history}"
                );
                self.reset_spectrogram_state();
                self.emit_snapshot(ctx.event_tx, true);
                self.start_spectrogram_session(position_seconds, clear_history, ctx);
            }
            AnalysisCommand::PositionUpdate(position_seconds) => {
                eprintln!("[analysis] PositionUpdate pos={position_seconds:.2}");
                Self::update_spectrogram_position(position_seconds, ctx);
            }
            AnalysisCommand::SeekPosition(position_seconds) => {
                eprintln!("[analysis] SeekPosition pos={position_seconds:.2}");
                Self::seek_spectrogram_position(position_seconds, ctx);
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

        if gapless {
            // Gapless transition: tell the decode worker to seamlessly continue
            // with the new file without resetting the ring buffer.
            eprintln!("[analysis] handle_track_change: gapless transition");
            let _ = ctx
                .spectrogram_cmd_tx
                .send(SpectrogramWorkerCommand::GaplessTransition {
                    track_token,
                    path: path.clone(),
                });
        } else {
            // Normal track change: start a fresh decode session.
            eprintln!("[analysis] handle_track_change: dispatching NewTrack from 0.0");
            self.start_spectrogram_session(0.0, reset_spectrogram, ctx);
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
    }

    fn start_spectrogram_session(
        &self,
        start_seconds: f64,
        clear_history_on_reset: bool,
        ctx: &AnalysisContext<'_>,
    ) {
        let Some(path) = self.active_track_path.as_ref() else {
            eprintln!("[analysis] start_spectrogram_session: no active_track_path, skipping");
            return;
        };
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
                fft_size: self.spectrogram.fft_size,
                hop_size: self.spectrogram.hop_size,
                channel_count: self.spectrogram.pipelines.len().max(1),
                start_seconds,
                clear_history_on_reset,
                view_mode: self.snapshot.spectrogram_view_mode,
                display_mode: self.display_mode,
            });
    }

    fn update_spectrogram_position(position_seconds: f64, ctx: &AnalysisContext<'_>) {
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::PositionUpdate { position_seconds });
    }

    fn seek_spectrogram_position(position_seconds: f64, ctx: &AnalysisContext<'_>) {
        let _ = ctx
            .spectrogram_cmd_tx
            .send(SpectrogramWorkerCommand::Seek { position_seconds });
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
        self.snapshot.spectrogram_seq = 0;
        self.spectrogram.reset();
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
        let to_feed_frames = self.frames_available_to_feed(channel_count);
        self.feed_spectrogram(channel_count, to_feed_frames);
        self.collect_spectrogram_rows();
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
            self.spectrogram.update_channel_labels(&chunk_labels);
            self.spectrogram.reset();
            self.snapshot.spectrogram_seq = 0;
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

    fn frames_available_to_feed(&self, channel_count: usize) -> usize {
        let visual_delay_ms = u32_to_usize(BASE_VISUAL_DELAY_MS.unsigned_abs());
        let effective_delay_frames =
            u32_to_usize(self.snapshot.sample_rate_hz).saturating_mul(visual_delay_ms) / 1000;
        (self.pcm_fifo.len() / channel_count).saturating_sub(effective_delay_frames)
    }

    fn feed_spectrogram(&mut self, channel_count: usize, to_feed_frames: usize) {
        if to_feed_frames == 0 {
            return;
        }

        let mut feed = Vec::with_capacity(to_feed_frames.saturating_mul(channel_count));
        for _ in 0..to_feed_frames.saturating_mul(channel_count) {
            if let Some(sample) = self.pcm_fifo.pop_front() {
                feed.push(sample);
            }
        }
        self.prof_out_samples += feed.len();
        self.spectrogram.feed_chunk(
            &AnalysisPcmChunk {
                samples: feed,
                channel_labels: self.pcm_labels.clone(),
                track_token: self.active_pcm_track_token,
            },
            self.snapshot.sample_rate_hz,
        );
    }

    fn collect_spectrogram_rows(&mut self) {
        let channels = self.spectrogram.take_channels(8);
        let row_count = channels.first().map_or(0, |channel| channel.rows.len());
        self.prof_rows += row_count;
        if row_count > 0 {
            self.snapshot.spectrogram_seq = self
                .snapshot
                .spectrogram_seq
                .wrapping_add(usize_to_u64(row_count));
            merge_pending_channels(&mut self.pending_channels, channels);
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
            self.spectrogram
                .pipelines
                .first()
                .map_or(0, |pipeline| pipeline.stft.pending_len()),
            self.pcm_fifo.len() / _channel_count,
            self.spectrogram.fft_size,
            self.spectrogram.hop_size,
            self.spectrogram.labels.len()
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

const PRECOMPUTED_DB_RANGE: f32 = 132.0;

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

/// Lossless `usize`-to-`f32` is impossible on 32/64-bit targets. Precision
/// loss is acceptable here (channel-count reciprocal, typically <=8).
fn usize_to_f32_approx(v: usize) -> f32 {
    #[allow(clippy::cast_precision_loss)]
    let r = v as f32;
    r
}

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

fn spawn_spectrogram_decode_worker(
    cmd_rx: Receiver<SpectrogramWorkerCommand>,
    event_tx: Sender<AnalysisEvent>,
    active_token: Arc<AtomicU64>,
    generation: Arc<AtomicU64>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-spectrogram-decode".to_string())
        .spawn(move || {
            spectrogram_worker_loop(&cmd_rx, &event_tx, &active_token, &generation);
        });
}

fn spectrogram_worker_loop(
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
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

        match cmd {
            SpectrogramWorkerCommand::NewTrack { .. } => {
                next_cmd =
                    run_spectrogram_session(&cmd, cmd_rx, event_tx, active_token, generation);
                if matches!(next_cmd, Some(SpectrogramWorkerCommand::Stop)) {
                    break;
                }
            }
            SpectrogramWorkerCommand::Stop => break,
            _ => {} // ignore commands outside session
        }
    }
}

/// Holds decode state for a spectrogram session.
struct SpectrogramSessionState {
    track_token: u64,
    gen: u64,
    fft_size: usize,
    hop_size: usize,
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
    /// Absolute column index of the next column to be produced (monotonic within session).
    columns_produced: u64,
    /// Column index where the current decode segment started (after last seek/reset).
    session_start_column: u64,

    // Chunking / STFT state
    stfts: Vec<StftComputer>,
    decimators: Vec<SpectrogramDecimator>,
    sample_buf: Option<SampleBuffer<f32>>,
    packet_counter: usize,
    chunk_buf: Vec<u8>,
    chunk_columns: u16,
    chunk_start_index: u64,
    target_chunk_columns: u16,
    total_covered_samples: u64,

    // Rate throttling
    session_start_time: std::time::Instant,
    post_reset_burst: u32,
    decode_rate_limit: f64,
    lookahead_columns: u64,

    // Pending gapless transition (stored until EOF of current file).
    pending_gapless: Option<(u64, PathBuf)>,
}

/// Action returned by command processing in the session loop.
enum SessionAction {
    Continue,
    Stop,
    NewSession(SpectrogramWorkerCommand),
    SeekRequired { position_seconds: f64 },
}

/// Runs a spectrogram decode session for a single track (or sequence of gapless tracks).
/// Returns `Some(cmd)` if interrupted by a NewTrack/Stop, `None` if session ended naturally.
#[allow(clippy::too_many_lines)]
fn run_spectrogram_session(
    initial_cmd: &SpectrogramWorkerCommand,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    let &SpectrogramWorkerCommand::NewTrack {
        track_token,
        generation: gen,
        ref path,
        fft_size,
        hop_size,
        channel_count,
        start_seconds,
        clear_history_on_reset,
        view_mode,
        display_mode,
    } = initial_cmd
    else {
        return None;
    };

    let start = std::time::Instant::now();
    eprintln!(
        "[spect-worker] SESSION START path={} gen={gen} token={track_token} fft={fft_size} hop={hop_size} ch={channel_count} view={view_mode:?} display={display_mode:?} start_s={start_seconds:.2}",
        path.file_name().unwrap_or_default().to_string_lossy(),
    );

    let bins_per_column = (fft_size / 2) + 1;
    let Some(total_columns_estimate) = estimate_total_columns(path) else {
        eprintln!("[spect-worker] estimate_total_columns returned None, aborting");
        return None;
    };

    let Some((mut format, mut audio_decoder, track_id, native_sample_rate, native_channels)) =
        open_symphonia_file(path)
    else {
        eprintln!("[spect-worker] failed to open file");
        return None;
    };

    let divisor = usize::try_from(waveform_sample_rate_divisor(native_sample_rate)).unwrap_or(1);
    let divisor_u64 = u64::try_from(divisor).unwrap_or(1);
    let effective_rate = u32::try_from(native_sample_rate / divisor_u64.max(1)).unwrap_or(48_000);
    let actual_channel_count = match view_mode {
        SpectrogramViewMode::Downmix => 1,
        SpectrogramViewMode::PerChannel => native_channels,
    };
    let decimation_factor = decimation_factor_for_hop(hop_size);
    let cols_per_second = f64::from(effective_rate) / usize_to_f64_approx(REFERENCE_HOP);

    // Lookahead configuration
    let lookahead_seconds = std::env::var("FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS")
        .ok()
        .and_then(|s| s.parse::<f64>().ok())
        .unwrap_or(10.0);
    let lookahead_columns = f64_to_u64_saturating(lookahead_seconds * cols_per_second);

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
        seek_symphonia(
            &mut format,
            track_id,
            native_sample_rate,
            actual_seek_seconds,
        );
    }

    let start_column = f64_to_u64_saturating((start_seconds * cols_per_second).floor());

    let mut session = SpectrogramSessionState {
        track_token,
        gen,
        fft_size,
        hop_size,
        view_mode,
        display_mode,
        channel_count: actual_channel_count,
        bins_per_column,
        total_columns_estimate,
        effective_rate,
        cols_per_second,
        divisor,
        target_position_seconds: start_seconds,
        columns_produced: start_column,
        session_start_column: start_column,
        stfts: (0..actual_channel_count)
            .map(|_| StftComputer::new(fft_size, hop_size))
            .collect(),
        decimators: (0..actual_channel_count)
            .map(|_| SpectrogramDecimator::new(decimation_factor))
            .collect(),
        sample_buf: None,
        packet_counter: 0,
        chunk_buf: Vec::new(),
        chunk_columns: 0,
        chunk_start_index: start_column,
        target_chunk_columns: 1, // Start with 1 for fastest first-pixel
        total_covered_samples: 0,
        session_start_time: std::time::Instant::now(),
        post_reset_burst: 16,
        decode_rate_limit,
        lookahead_columns,
        pending_gapless: None,
    };

    // Send initial metadata chunk (0 columns, carries estimates + buffer_reset).
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
            hop_size: clamp_to_u16(REFERENCE_HOP),
            coverage_seconds: 0.0,
            complete: false,
            buffer_reset: true,
            clear_history: clear_history_on_reset,
        },
    ));

    let mut warmup_remaining = warmup_columns;

    // Main session loop: decode current file, handle gapless transitions.
    loop {
        let result = session_decode_loop(
            &mut session,
            &mut format,
            &mut audio_decoder,
            track_id,
            &mut warmup_remaining,
            cmd_rx,
            event_tx,
            active_token,
            generation,
        );

        if let Some(cmd) = result {
            // Interrupted by a NewTrack or Stop command.
            eprintln!(
                "[spect-worker] SESSION END (interrupted) elapsed={:.1}ms cols_produced={}",
                start.elapsed().as_secs_f64() * 1000.0,
                session.columns_produced.saturating_sub(start_column),
            );
            return Some(cmd);
        }

        // EOF reached — check for pending gapless transition.
        if let Some((new_token, new_path)) = session.pending_gapless.take() {
            eprintln!(
                "[spect-worker] GAPLESS TRANSITION to {} token={new_token}",
                new_path.file_name().unwrap_or_default().to_string_lossy(),
            );

            // Flush any pending chunk from the old track.
            session_flush_chunk(&mut session, event_tx);

            // Open new file.
            let Some((new_format, new_decoder, new_track_id, new_native_rate, _)) =
                open_symphonia_file(&new_path)
            else {
                eprintln!("[spect-worker] gapless: failed to open new file");
                break;
            };

            let new_total =
                estimate_total_columns(&new_path).unwrap_or(session.total_columns_estimate);
            let new_divisor =
                usize::try_from(waveform_sample_rate_divisor(new_native_rate)).unwrap_or(1);

            // Update session for new track.
            session.track_token = new_token;
            session.total_columns_estimate = new_total;
            session.total_covered_samples = 0;
            session.session_start_column = session.columns_produced;
            session.divisor = new_divisor;

            // Reset STFT state (avoid cross-track spectral leakage)
            // but keep monotonic sequence counter.
            let dec_factor = decimation_factor_for_hop(session.hop_size);
            session.stfts = (0..session.channel_count)
                .map(|_| StftComputer::new(session.fft_size, session.hop_size))
                .collect();
            session.decimators = (0..session.channel_count)
                .map(|_| SpectrogramDecimator::new(dec_factor))
                .collect();
            session.chunk_buf.clear();
            session.chunk_columns = 0;
            session.chunk_start_index = session.columns_produced;
            session.target_chunk_columns = 8;

            // Send metadata chunk for new track — NOT a buffer_reset.
            // trackToken change without buffer_reset signals gapless.
            let _ = event_tx.send(AnalysisEvent::PrecomputedSpectrogramChunk(
                PrecomputedSpectrogramChunk {
                    track_token: new_token,
                    columns_u8: Vec::new(),
                    bins_per_column: clamp_to_u16(session.bins_per_column),
                    column_count: 0,
                    channel_count: clamp_to_u8(session.channel_count),
                    start_column_index: u64_to_u32_saturating(session.columns_produced),
                    total_columns_estimate: new_total,
                    sample_rate_hz: session.effective_rate,
                    hop_size: clamp_to_u16(REFERENCE_HOP),
                    coverage_seconds: 0.0,
                    complete: false,
                    buffer_reset: false,
                    clear_history: false,
                },
            ));

            format = new_format;
            audio_decoder = new_decoder;
            warmup_remaining = 0;
            // Continue the outer loop with the new file.
            // We need to update track_id for the decode loop.
            // Since track_id is immutable in the current scope,
            // we re-enter the decode loop with the new value.
            let result2 = session_decode_loop(
                &mut session,
                &mut format,
                &mut audio_decoder,
                new_track_id,
                &mut warmup_remaining,
                cmd_rx,
                event_tx,
                active_token,
                generation,
            );
            if let Some(cmd) = result2 {
                return Some(cmd);
            }
            // If EOF again, loop back to check for another gapless.
            continue;
        }

        // No pending gapless — session ends.
        session_flush_chunk(&mut session, event_tx);
        eprintln!(
            "[spect-worker] SESSION END (EOF) elapsed={:.1}ms cols_produced={}",
            start.elapsed().as_secs_f64() * 1000.0,
            session.columns_produced.saturating_sub(start_column),
        );
        return None;
    }
    None
}

/// Inner decode loop. Returns `Some(cmd)` if interrupted, `None` on EOF.
#[allow(clippy::too_many_arguments, clippy::too_many_lines)]
fn session_decode_loop(
    session: &mut SpectrogramSessionState,
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    audio_decoder: &mut Box<dyn symphonia::core::codecs::Decoder>,
    track_id: u32,
    warmup_remaining: &mut u64,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    // Only check generation for staleness — NOT active_token.
    // The active_token (waveform_decode_active_token) is updated for gapless
    // transitions too, but the spectrogram session should continue across
    // gapless transitions (it receives GaplessTransition commands via the
    // channel). Generation is only incremented by start_spectrogram_session
    // which is called for non-gapless track changes.
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
                SessionAction::Stop => return Some(SpectrogramWorkerCommand::Stop),
                SessionAction::NewSession(cmd) => return Some(cmd),
                SessionAction::SeekRequired { position_seconds } => {
                    return handle_session_seek(
                        session,
                        position_seconds,
                        format,
                        audio_decoder,
                        track_id,
                        warmup_remaining,
                        cmd_rx,
                        event_tx,
                        active_token,
                        generation,
                    );
                }
            }
        }

        // 2. Check lead — park if sufficiently ahead (unless gapless pending).
        let target_column =
            f64_to_u64_saturating(session.target_position_seconds * session.cols_per_second);
        let lead = session.columns_produced.saturating_sub(target_column);

        if lead >= session.lookahead_columns
            && session.post_reset_burst == 0
            && session.pending_gapless.is_none()
        {
            // Park: block until a command arrives.
            match cmd_rx.recv() {
                Ok(cmd) => match handle_single_command(session, cmd) {
                    SessionAction::Continue => continue,
                    SessionAction::Stop => {
                        return Some(SpectrogramWorkerCommand::Stop);
                    }
                    SessionAction::NewSession(cmd) => return Some(cmd),
                    SessionAction::SeekRequired { position_seconds } => {
                        return handle_session_seek(
                            session,
                            position_seconds,
                            format,
                            audio_decoder,
                            track_id,
                            warmup_remaining,
                            cmd_rx,
                            event_tx,
                            active_token,
                            generation,
                        );
                    }
                },
                Err(_) => return None,
            }
        }

        // 3. Rate throttle (rolling mode only, after burst).
        if session.post_reset_burst > 0 {
            session.post_reset_burst -= 1;
        } else if session.decode_rate_limit.is_finite() {
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
                                    format,
                                    audio_decoder,
                                    track_id,
                                    warmup_remaining,
                                    cmd_rx,
                                    event_tx,
                                    active_token,
                                    generation,
                                );
                            }
                        },
                        Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
                        Err(_) => return None,
                    }
                }
            }
        }

        // 4. Decode next packet.
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                break;
            }
            Err(SymphoniaError::ResetRequired | _) => break,
        };

        if packet.track_id() != track_id {
            continue;
        }
        session.packet_counter += 1;

        let decoded_audio = match audio_decoder.decode(&packet) {
            Ok(decoded_audio) => decoded_audio,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                break;
            }
            Err(SymphoniaError::DecodeError(_)) => continue,
            Err(_) => break,
        };

        let spec = *decoded_audio.spec();
        let decoded_channels = spec.channels.count().max(1);
        let decoded_capacity = decoded_audio.capacity();
        let buf = ensure_sample_buffer(&mut session.sample_buf, decoded_capacity, spec);
        buf.copy_interleaved_ref(decoded_audio);
        let samples = buf.samples();

        let frames = samples.len() / decoded_channels;
        let effective_frames = frames / session.divisor;

        let per_channel = deinterleave_samples(
            samples,
            frames,
            decoded_channels,
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
        session_drain_stft_rows(session, warmup_remaining, event_tx);

        // Yield periodically to avoid starving UI.
        if session.packet_counter.is_multiple_of(64) {
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    // EOF reached.
    None
}

fn process_session_commands(
    session: &mut SpectrogramSessionState,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
) -> Option<SessionAction> {
    let mut latest_position: Option<f64> = None;
    let mut latest_seek: Option<f64> = None;

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
            SpectrogramWorkerCommand::GaplessTransition { track_token, path } => {
                session.pending_gapless = Some((track_token, path));
            }
            SpectrogramWorkerCommand::SetDisplayMode(mode) => {
                session.display_mode = mode;
                session.decode_rate_limit = if mode == SpectrogramDisplayMode::Rolling {
                    std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
                        .ok()
                        .and_then(|s| s.parse::<f64>().ok())
                        .unwrap_or(2.0)
                } else {
                    f64::INFINITY
                };
            }
            SpectrogramWorkerCommand::Stop => {
                return Some(SessionAction::Stop);
            }
        }
    }

    if let Some(position_seconds) = latest_seek {
        session.target_position_seconds = position_seconds;
        return Some(SessionAction::SeekRequired { position_seconds });
    }

    // Process position update — check if seek is needed.
    if let Some(position_seconds) = latest_position {
        session.target_position_seconds = position_seconds;

        let target_col = f64_to_u64_saturating(position_seconds * session.cols_per_second);
        if target_col < session.session_start_column {
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
            if target_col < session.session_start_column {
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
        SpectrogramWorkerCommand::GaplessTransition { track_token, path } => {
            session.pending_gapless = Some((track_token, path));
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::SetDisplayMode(mode) => {
            session.display_mode = mode;
            session.decode_rate_limit = if mode == SpectrogramDisplayMode::Rolling {
                std::env::var("FERROUS_SPECTROGRAM_DECODE_RATE")
                    .ok()
                    .and_then(|s| s.parse::<f64>().ok())
                    .unwrap_or(2.0)
            } else {
                f64::INFINITY
            };
            SessionAction::Continue
        }
        SpectrogramWorkerCommand::Stop => SessionAction::Stop,
    }
}

/// Handles a seek within the current session by resetting STFT state and
/// repositioning the file reader.
#[allow(clippy::too_many_arguments)]
fn handle_session_seek(
    session: &mut SpectrogramSessionState,
    position_seconds: f64,
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    audio_decoder: &mut Box<dyn symphonia::core::codecs::Decoder>,
    track_id: u32,
    warmup_remaining: &mut u64,
    cmd_rx: &Receiver<SpectrogramWorkerCommand>,
    event_tx: &Sender<AnalysisEvent>,
    active_token: &AtomicU64,
    generation: &AtomicU64,
) -> Option<SpectrogramWorkerCommand> {
    eprintln!("[spect-worker] SEEK to {position_seconds:.2}s");

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
    seek_symphonia(format, track_id, native_rate, actual_seek_seconds);
    audio_decoder.reset();

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
    session.post_reset_burst = 16;
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
            hop_size: clamp_to_u16(REFERENCE_HOP),
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
        format,
        audio_decoder,
        track_id,
        warmup_remaining,
        cmd_rx,
        event_tx,
        active_token,
        generation,
    )
}

fn session_drain_stft_rows(
    session: &mut SpectrogramSessionState,
    warmup_remaining: &mut u64,
    event_tx: &Sender<AnalysisEvent>,
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
                    hop_size: clamp_to_u16(REFERENCE_HOP),
                    coverage_seconds: coverage,
                    complete: false,
                    buffer_reset: false,
                    clear_history: false,
                },
            ));
            session.chunk_start_index = session.columns_produced;
            session.chunk_columns = 0;
            // Ramp up chunk size: 1 → 2 → 4 → … → 256
            session.target_chunk_columns = (session.target_chunk_columns * 2).min(256);
        }
    }
}

fn session_flush_chunk(session: &mut SpectrogramSessionState, event_tx: &Sender<AnalysisEvent>) {
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
                hop_size: clamp_to_u16(REFERENCE_HOP),
                coverage_seconds: coverage,
                complete: false,
                buffer_reset: false,
                clear_history: false,
            },
        ));
        session.chunk_columns = 0;
    }
}

/// Opens a Symphonia file and returns the format reader, decoder, and track info.
#[allow(clippy::type_complexity)]
fn open_symphonia_file(
    path: &Path,
) -> Option<(
    Box<dyn symphonia::core::formats::FormatReader>,
    Box<dyn symphonia::core::codecs::Decoder>,
    u32,
    u64,
    usize,
)> {
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let file = File::open(path).ok()?;
    let mss = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
    let format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?
        .format;
    let track = format.default_track()?;
    let track_id = track.id;
    let native_sample_rate = u64::from(track.codec_params.sample_rate.unwrap_or(48_000));
    let native_channels = track
        .codec_params
        .channels
        .map_or(2, |ch| ch.count().max(1));
    let audio_decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .ok()?;
    Some((
        format,
        audio_decoder,
        track_id,
        native_sample_rate,
        native_channels,
    ))
}

fn seek_symphonia(
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    track_id: u32,
    native_sample_rate: u64,
    seek_seconds: f64,
) {
    use symphonia::core::formats::SeekMode;
    use symphonia::core::formats::SeekTo;
    let native_rate_u32 = u32::try_from(native_sample_rate).unwrap_or(u32::MAX);
    let ts = f64_to_u64_saturating(seek_seconds * f64::from(native_rate_u32));
    let _ = format.seek(SeekMode::Coarse, SeekTo::TimeStamp { ts, track_id });
}

fn u64_to_u32_saturating(v: u64) -> u32 {
    u32::try_from(v).unwrap_or(u32::MAX)
}

fn deinterleave_samples(
    samples: &[f32],
    frames: usize,
    decoded_channels: usize,
    channel_count: usize,
    divisor: usize,
    effective_frames: usize,
    view_mode: SpectrogramViewMode,
) -> Vec<Vec<f32>> {
    let mut per_channel: Vec<Vec<f32>> = vec![Vec::with_capacity(effective_frames); channel_count];

    match view_mode {
        SpectrogramViewMode::Downmix => {
            let mut downmixed = Vec::with_capacity(effective_frames);
            let inv_channels = 1.0 / usize_to_f32_approx(decoded_channels);
            for frame_idx in (0..frames).step_by(divisor) {
                let base = frame_idx * decoded_channels;
                let mut sum = 0.0f32;
                for ch in 0..decoded_channels {
                    sum += samples[base + ch];
                }
                downmixed.push(sum * inv_channels);
            }
            per_channel[0] = downmixed;
        }
        SpectrogramViewMode::PerChannel => {
            for frame_idx in (0..frames).step_by(divisor) {
                let base = frame_idx * decoded_channels;
                for ch in 0..channel_count.min(decoded_channels) {
                    per_channel[ch].push(samples[base + ch]);
                }
            }
        }
    }

    per_channel
}

fn estimate_total_columns(path: &Path) -> Option<u32> {
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let file = File::open(path).ok()?;
    let mss = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
    let format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?;
    let track = format.format.default_track()?;
    let sample_rate = u64::from(track.codec_params.sample_rate.unwrap_or(48_000));
    let n_frames = track.codec_params.n_frames.unwrap_or(sample_rate * 300);
    let divisor = waveform_sample_rate_divisor(sample_rate);
    let effective_frames = n_frames / divisor;
    let total = effective_frames / (REFERENCE_HOP as u64);
    Some(u32::try_from((total + 64).min(u64::from(u32::MAX))).unwrap_or(u32::MAX))
}

fn spawn_analysis_worker(
    cmd_rx: Receiver<AnalysisCommand>,
    pcm_rx: Receiver<AnalysisPcmChunk>,
    event_tx: Sender<AnalysisEvent>,
    waveform_job_tx: Sender<WaveformDecodeJob>,
    waveform_decode_active_token: Arc<AtomicU64>,
    spectrogram_cmd_tx: Sender<SpectrogramWorkerCommand>,
    spectrogram_decode_generation: Arc<AtomicU64>,
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
                            spectrogram_cmd_tx: &spectrogram_cmd_tx,
                            spectrogram_decode_generation: spectrogram_decode_generation.as_ref(),
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

fn decimation_factor_for_hop(hop: usize) -> usize {
    if hop == 0 {
        return 1;
    }
    (REFERENCE_HOP / hop).max(1)
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

fn usize_to_u64(value: usize) -> u64 {
    u64::try_from(value).unwrap_or(u64::MAX)
}

fn u64_to_i64(value: u64) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn usize_to_i64(value: usize) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
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

fn seconds_from_nanoseconds(span_ns: u64) -> f32 {
    Duration::from_nanos(span_ns).as_secs_f32()
}

fn open_waveform_cache_db() -> anyhow::Result<Connection> {
    let db_path = waveform_db_path()?;
    if let Some(parent) = db_path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let conn = Connection::open(db_path)?;
    let _ = conn.busy_timeout(Duration::from_millis(250));
    init_waveform_cache_schema(&conn)?;
    Ok(conn)
}

fn waveform_db_path() -> anyhow::Result<PathBuf> {
    if let Some(xdg_home) = std::env::var_os("XDG_DATA_HOME") {
        return Ok(PathBuf::from(xdg_home)
            .join("ferrous")
            .join("library.sqlite3"));
    }
    let home = std::env::var_os("HOME")
        .ok_or_else(|| anyhow::anyhow!("HOME is not set and XDG_DATA_HOME is missing"))?;
    Ok(PathBuf::from(home)
        .join(".local")
        .join("share")
        .join("ferrous")
        .join("library.sqlite3"))
}

fn init_waveform_cache_schema(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(
        r"
        CREATE TABLE IF NOT EXISTS waveform_cache (
            path TEXT PRIMARY KEY,
            size_bytes INTEGER NOT NULL,
            modified_secs INTEGER NOT NULL,
            modified_nanos INTEGER NOT NULL,
            format_version INTEGER NOT NULL,
            peak_count INTEGER NOT NULL,
            peaks_blob BLOB NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_waveform_cache_updated_at
            ON waveform_cache(updated_at);
        ",
    )?;
    // Legacy/failed decodes may have written empty entries; treat them as invalid cache rows.
    let _ = conn.execute("DELETE FROM waveform_cache WHERE peak_count <= 0", []);
    Ok(())
}

fn load_waveform_from_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
) -> Option<Vec<f32>> {
    let path = path.to_string_lossy().to_string();
    let row: (i64, i64, Vec<u8>) = conn
        .query_row(
            r"
            SELECT format_version, peak_count, peaks_blob
            FROM waveform_cache
            WHERE path=?1
              AND size_bytes=?2
              AND modified_secs=?3
              AND modified_nanos=?4
            ",
            params![
                path,
                u64_to_i64(stamp.size_bytes),
                u64_to_i64(stamp.modified_secs),
                i64::from(stamp.modified_nanos),
            ],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Vec<u8>>(2)?,
                ))
            },
        )
        .ok()?;
    if row.0 != WAVEFORM_CACHE_FORMAT_VERSION {
        return None;
    }
    let peak_count = usize::try_from(row.1).ok()?;
    if peak_count == 0 {
        return None;
    }
    decode_peaks_blob(&row.2, peak_count)
}

fn persist_waveform_to_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
    peaks: &[f32],
) -> rusqlite::Result<()> {
    if peaks.is_empty() {
        let _ = conn.execute(
            "DELETE FROM waveform_cache WHERE path = ?1",
            params![path.to_string_lossy().to_string()],
        );
        return Ok(());
    }
    let blob = encode_peaks_blob(peaks);
    let now = unix_ts_i64();
    conn.execute(
        r"
        INSERT INTO waveform_cache(
            path, size_bytes, modified_secs, modified_nanos,
            format_version, peak_count, peaks_blob, updated_at
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(path) DO UPDATE SET
            size_bytes=excluded.size_bytes,
            modified_secs=excluded.modified_secs,
            modified_nanos=excluded.modified_nanos,
            format_version=excluded.format_version,
            peak_count=excluded.peak_count,
            peaks_blob=excluded.peaks_blob,
            updated_at=excluded.updated_at
        ",
        params![
            path.to_string_lossy().to_string(),
            u64_to_i64(stamp.size_bytes),
            u64_to_i64(stamp.modified_secs),
            i64::from(stamp.modified_nanos),
            WAVEFORM_CACHE_FORMAT_VERSION,
            usize_to_i64(peaks.len()),
            blob,
            now,
        ],
    )?;
    Ok(())
}

fn prune_persistent_waveform_cache(conn: &Connection, max_rows: usize) -> rusqlite::Result<()> {
    conn.execute(
        r"
        DELETE FROM waveform_cache
        WHERE path IN (
            SELECT path
            FROM waveform_cache
            ORDER BY updated_at DESC
            LIMIT -1 OFFSET ?1
        )
        ",
        params![usize_to_i64(max_rows)],
    )?;
    Ok(())
}

fn encode_peaks_blob(peaks: &[f32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(peaks.len() * 4);
    for &v in peaks {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

fn decode_peaks_blob(blob: &[u8], peak_count: usize) -> Option<Vec<f32>> {
    if blob.len() != peak_count.checked_mul(4)? {
        return None;
    }
    let mut out = Vec::with_capacity(peak_count);
    for chunk in blob.chunks_exact(4) {
        out.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(out)
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| u64_to_i64(duration.as_secs()))
        .unwrap_or(0)
}

fn touch_waveform_cache_lru(lru: &mut VecDeque<PathBuf>, path: &Path) {
    if let Some(pos) = lru.iter().position(|p| p == path) {
        lru.remove(pos);
    }
    lru.push_back(path.to_path_buf());
}

fn insert_waveform_cache_entry(
    cache: &mut HashMap<PathBuf, WaveformCacheEntry>,
    lru: &mut VecDeque<PathBuf>,
    path: &Path,
    entry: WaveformCacheEntry,
) {
    if entry.peaks.is_empty() {
        cache.remove(path);
        if let Some(pos) = lru.iter().position(|p| p == path) {
            lru.remove(pos);
        }
        return;
    }
    let owned_path = path.to_path_buf();
    cache.insert(owned_path.clone(), entry);
    touch_waveform_cache_lru(lru, &owned_path);

    while cache.len() > MAX_WAVEFORM_CACHE_TRACKS {
        let Some(evicted) = lru.pop_front() else {
            break;
        };
        cache.remove(&evicted);
    }
}

fn emit_snapshot(
    event_tx: &Sender<AnalysisEvent>,
    snapshot: &AnalysisSnapshot,
    pending_channels: &mut Vec<AnalysisSpectrogramChannel>,
    waveform_dirty: &mut bool,
    last_emit: &mut std::time::Instant,
    force: bool,
) {
    if !*waveform_dirty && pending_channels.is_empty() && !force {
        return;
    }
    if !force
        && pending_channels.is_empty()
        && last_emit.elapsed() < std::time::Duration::from_millis(16)
    {
        return;
    }

    let out = AnalysisSnapshot {
        waveform_peaks: if *waveform_dirty {
            snapshot.waveform_peaks.clone()
        } else {
            Vec::new()
        },
        waveform_coverage_seconds: snapshot.waveform_coverage_seconds,
        waveform_complete: snapshot.waveform_complete,
        spectrogram_channels: std::mem::take(pending_channels),
        spectrogram_seq: snapshot.spectrogram_seq,
        sample_rate_hz: snapshot.sample_rate_hz,
        spectrogram_view_mode: snapshot.spectrogram_view_mode,
    };
    let _ = event_tx.send(AnalysisEvent::Snapshot(out));
    *waveform_dirty = false;
    *last_emit = std::time::Instant::now();
}

struct SpectrogramPipeline {
    stft: StftComputer,
    decimator: SpectrogramDecimator,
}

impl SpectrogramPipeline {
    fn new(fft_size: usize, hop_size: usize) -> Self {
        Self {
            stft: StftComputer::new(fft_size, hop_size),
            decimator: SpectrogramDecimator::new(decimation_factor_for_hop(hop_size)),
        }
    }

    fn reset(&mut self) {
        self.stft.reset_full();
        self.decimator.reset();
    }
}

struct SpectrogramRuntime {
    view_mode: SpectrogramViewMode,
    labels: Vec<SpectrogramChannelLabel>,
    pipelines: Vec<SpectrogramPipeline>,
    fft_size: usize,
    hop_size: usize,
}

impl SpectrogramRuntime {
    fn new(
        fft_size: usize,
        hop_size: usize,
        view_mode: SpectrogramViewMode,
        labels: &[SpectrogramChannelLabel],
    ) -> Self {
        let normalized = Self::normalize_labels(view_mode, labels);
        let pipeline_count = Self::pipeline_count_for(view_mode, normalized.len());
        let pipelines = (0..pipeline_count)
            .map(|_| SpectrogramPipeline::new(fft_size, hop_size))
            .collect();
        Self {
            view_mode,
            labels: normalized,
            pipelines,
            fft_size,
            hop_size,
        }
    }

    fn set_fft_size(&mut self, fft_size: usize, hop_size: usize) {
        self.fft_size = fft_size;
        self.hop_size = hop_size;
        self.rebuild_pipelines();
    }

    fn set_view_mode(&mut self, view_mode: SpectrogramViewMode) {
        if self.view_mode == view_mode {
            return;
        }
        self.view_mode = view_mode;
        self.labels = Self::normalize_labels(view_mode, &self.labels);
        self.rebuild_pipelines();
    }

    fn update_channel_labels(&mut self, labels: &[SpectrogramChannelLabel]) {
        let normalized = Self::normalize_labels(self.view_mode, labels);
        if normalized == self.labels {
            return;
        }
        self.labels = normalized;
        self.rebuild_pipelines();
    }

    fn reset(&mut self) {
        for pipeline in &mut self.pipelines {
            pipeline.reset();
        }
    }

    fn feed_chunk(&mut self, chunk: &AnalysisPcmChunk, sample_rate_hz: u32) {
        if chunk.samples.is_empty() {
            return;
        }
        let labels = Self::normalize_labels(self.view_mode, &chunk.channel_labels);
        if labels != self.labels {
            self.labels = labels;
            self.rebuild_pipelines();
        }

        match self.view_mode {
            SpectrogramViewMode::Downmix => {
                let downmixed =
                    downmix_interleaved_samples(&chunk.samples, chunk.channel_labels.len());
                if let Some(pipeline) = self.pipelines.first_mut() {
                    pipeline.stft.enqueue_samples(&downmixed, sample_rate_hz);
                }
            }
            SpectrogramViewMode::PerChannel => {
                let channels = self.labels.len().max(1);
                if channels <= 1 {
                    if let Some(pipeline) = self.pipelines.first_mut() {
                        pipeline
                            .stft
                            .enqueue_samples(&chunk.samples, sample_rate_hz);
                    }
                    return;
                }
                let mut separated =
                    vec![Vec::with_capacity(chunk.samples.len() / channels); channels];
                for frame in chunk.samples.chunks_exact(channels) {
                    for (index, sample) in frame.iter().copied().enumerate() {
                        separated[index].push(sample);
                    }
                }
                for (pipeline, samples) in self.pipelines.iter_mut().zip(separated.into_iter()) {
                    pipeline.stft.enqueue_samples(&samples, sample_rate_hz);
                }
            }
        }
    }

    fn take_channels(&mut self, max_rows: usize) -> Vec<AnalysisSpectrogramChannel> {
        if self.pipelines.is_empty() {
            return Vec::new();
        }

        let raw_rows: Vec<Vec<Vec<f32>>> = self
            .pipelines
            .iter_mut()
            .map(|pipeline| pipeline.stft.take_rows(max_rows))
            .collect();
        let aligned_frames = raw_rows.iter().map(std::vec::Vec::len).min().unwrap_or(0);
        if aligned_frames == 0 {
            return Vec::new();
        }

        let mut output = self
            .labels
            .iter()
            .copied()
            .map(|label| AnalysisSpectrogramChannel {
                label,
                rows: Vec::new(),
            })
            .collect::<Vec<_>>();
        let mut iterators = raw_rows
            .into_iter()
            .map(std::iter::IntoIterator::into_iter)
            .collect::<Vec<_>>();

        for _ in 0..aligned_frames {
            for (channel_index, iter) in iterators.iter_mut().enumerate() {
                let Some(row) = iter.next() else {
                    continue;
                };
                if let Some(slow_row) = self.pipelines[channel_index].decimator.push(row) {
                    output[channel_index].rows.push(slow_row);
                }
            }
        }

        output.retain(|channel| !channel.rows.is_empty());
        output
    }

    fn pipeline_count_for(view_mode: SpectrogramViewMode, label_count: usize) -> usize {
        match view_mode {
            SpectrogramViewMode::Downmix => 1,
            SpectrogramViewMode::PerChannel => label_count.max(1),
        }
    }

    fn normalize_labels(
        view_mode: SpectrogramViewMode,
        labels: &[SpectrogramChannelLabel],
    ) -> Vec<SpectrogramChannelLabel> {
        match view_mode {
            SpectrogramViewMode::Downmix => vec![SpectrogramChannelLabel::Mono],
            SpectrogramViewMode::PerChannel => {
                if labels.is_empty() {
                    vec![SpectrogramChannelLabel::Mono]
                } else {
                    labels.to_vec()
                }
            }
        }
    }

    fn rebuild_pipelines(&mut self) {
        let pipeline_count = Self::pipeline_count_for(self.view_mode, self.labels.len());
        self.pipelines = (0..pipeline_count)
            .map(|_| SpectrogramPipeline::new(self.fft_size, self.hop_size))
            .collect();
    }
}

fn downmix_interleaved_samples(samples: &[f32], channels: usize) -> Vec<f32> {
    if channels <= 1 {
        return samples.to_vec();
    }

    let mut out = Vec::with_capacity(samples.len() / channels.max(1));
    for frame in samples.chunks_exact(channels) {
        let mut sum = 0.0f32;
        for sample in frame {
            sum += *sample;
        }
        out.push(sum / small_usize_to_f32(channels));
    }
    out
}

fn merge_pending_channels(
    pending: &mut Vec<AnalysisSpectrogramChannel>,
    incoming: Vec<AnalysisSpectrogramChannel>,
) {
    if incoming.is_empty() {
        return;
    }
    if pending.len() != incoming.len()
        || pending
            .iter()
            .zip(incoming.iter())
            .any(|(left, right)| left.label != right.label)
    {
        *pending = incoming;
        return;
    }
    for (pending_channel, incoming_channel) in pending.iter_mut().zip(incoming.into_iter()) {
        pending_channel.rows.extend(incoming_channel.rows);
    }
}

struct StftComputer {
    r2c: std::sync::Arc<dyn RealToComplex<f32>>,
    fft_in: Vec<f32>,
    fft_out: Vec<Complex32>,
    pending: Vec<f32>,
    pending_start: usize,
    window: Vec<f32>,
    fft_size: usize,
    hop_size: usize,
}

impl StftComputer {
    fn new(fft_size: usize, hop_size: usize) -> Self {
        let mut planner = RealFftPlanner::<f32>::new();
        let r2c = planner.plan_fft_forward(fft_size);
        let fft_in = r2c.make_input_vec();
        let fft_out = r2c.make_output_vec();
        // Blackman-Harris (as in DeaDBeeF spectrogram) gives cleaner bin separation.
        let window = blackman_harris_window(fft_size);

        Self {
            r2c,
            fft_in,
            fft_out,
            pending: Vec::with_capacity(fft_size * 2),
            pending_start: 0,
            window,
            fft_size,
            hop_size,
        }
    }

    fn reset_full(&mut self) {
        self.pending.clear();
        self.pending_start = 0;
    }

    fn enqueue_samples(&mut self, samples: &[f32], sample_rate_hz: u32) {
        self.compact_pending_if_needed();
        self.pending.extend_from_slice(samples);
        // Keep pending bounded to avoid latency creep: max ~0.5s audio.
        let max_pending = (u32_to_usize(sample_rate_hz) / 2).max(self.fft_size * 4);
        let available = self.pending_available();
        if available > max_pending {
            let drop = available - max_pending;
            self.pending_start = self.pending_start.saturating_add(drop);
            self.compact_pending_if_needed();
        }
    }

    fn take_rows(&mut self, max_rows: usize) -> Vec<Vec<f32>> {
        let mut rows = Vec::new();

        while self.pending_available() >= self.fft_size && rows.len() < max_rows {
            for i in 0..self.fft_size {
                self.fft_in[i] = self.pending[self.pending_start + i] * self.window[i];
            }

            if self
                .r2c
                .process(&mut self.fft_in, &mut self.fft_out)
                .is_ok()
            {
                let row: Vec<f32> = self
                    .fft_out
                    .iter()
                    .map(realfft::num_complex::Complex::norm_sqr)
                    .collect();
                rows.push(row);
            }

            let advance = self.hop_size.min(self.pending_available());
            self.pending_start = self.pending_start.saturating_add(advance);
        }

        self.compact_pending_if_needed();

        rows
    }

    #[allow(dead_code)]
    fn pending_len(&self) -> usize {
        self.pending_available()
    }

    #[allow(dead_code)]
    fn fft_size(&self) -> usize {
        self.fft_size
    }

    #[allow(dead_code)]
    fn hop_size(&self) -> usize {
        self.hop_size
    }

    fn pending_available(&self) -> usize {
        self.pending.len().saturating_sub(self.pending_start)
    }

    fn compact_pending_if_needed(&mut self) {
        if self.pending_start == 0 {
            return;
        }
        let should_compact = self.pending_start >= self.fft_size * 8
            || self.pending_start >= self.pending.len().saturating_div(2);
        if should_compact {
            self.pending.drain(0..self.pending_start);
            self.pending_start = 0;
        }
    }
}

struct SpectrogramDecimator {
    factor: usize,
    accum: Vec<f32>,
    count: usize,
}

impl SpectrogramDecimator {
    fn new(factor: usize) -> Self {
        Self {
            factor: factor.max(1),
            accum: Vec::new(),
            count: 0,
        }
    }

    fn reset(&mut self) {
        self.accum.clear();
        self.count = 0;
    }

    fn push(&mut self, row: Vec<f32>) -> Option<Vec<f32>> {
        if self.accum.is_empty() {
            self.accum = vec![0.0; row.len()];
        }
        if row.len() != self.accum.len() {
            self.accum = vec![0.0; row.len()];
            self.count = 0;
        }

        for (a, v) in self.accum.iter_mut().zip(row) {
            *a += v;
        }
        self.count += 1;

        if self.count < self.factor {
            return None;
        }

        let inv = 1.0 / small_usize_to_f32(self.count);
        let mut out = Vec::with_capacity(self.accum.len());
        for v in &self.accum {
            out.push(v * inv);
        }

        self.accum.fill(0.0);
        self.count = 0;
        Some(out)
    }
}

fn blackman_harris_window(size: usize) -> Vec<f32> {
    let n = small_usize_to_f32(size);
    (0..size)
        .map(|i| {
            let phase = (2.0 * std::f32::consts::PI * small_usize_to_f32(i)) / n;
            0.35875 - 0.48829 * phase.cos() + 0.14128 * (2.0 * phase).cos()
                - 0.01168 * (3.0 * phase).cos()
        })
        .collect()
}

struct WaveformAccumulator {
    peaks: Vec<f32>,
    bucket_peak: f32,
    bucket_count: u64,
    covered_frames: u64,
    block_size: u64,
    max_points: usize,
    sample_rate_hz: u64,
    last_preview_emit: std::time::Instant,
}

impl WaveformAccumulator {
    fn new(max_points: usize, estimated_frames: u64, sample_rate_hz: u64) -> Self {
        Self {
            peaks: Vec::with_capacity(max_points),
            bucket_peak: 0.0,
            bucket_count: 0,
            covered_frames: 0,
            block_size: (estimated_frames / usize_to_u64(max_points.max(1))).max(1),
            max_points,
            sample_rate_hz,
            last_preview_emit: std::time::Instant::now(),
        }
    }

    fn push_sample<F>(&mut self, amp: f32, sample_stride: usize, on_update: &mut F) -> bool
    where
        F: FnMut(Vec<f32>, f32, bool) -> bool,
    {
        if amp > self.bucket_peak {
            self.bucket_peak = amp;
        }
        let sample_stride = usize_to_u64(sample_stride);
        self.bucket_count = self.bucket_count.saturating_add(sample_stride);
        self.covered_frames = self.covered_frames.saturating_add(sample_stride);

        if self.bucket_count < self.block_size {
            return true;
        }

        self.peaks.push(self.bucket_peak.clamp(0.0, 1.0));
        self.bucket_peak = 0.0;
        self.bucket_count = 0;
        while self.peaks.len() > self.max_points {
            self.peaks = fold_waveform_peaks(&self.peaks);
            self.block_size = self.block_size.saturating_mul(2).max(1);
        }
        if self.peaks.len() < 12
            || self.last_preview_emit.elapsed() < std::time::Duration::from_millis(240)
        {
            return true;
        }

        self.last_preview_emit = std::time::Instant::now();
        on_update(
            self.peaks.clone(),
            seconds_from_frames(self.covered_frames, self.sample_rate_hz),
            false,
        )
    }

    fn finish(mut self) -> Vec<f32> {
        if self.bucket_count > 0 {
            self.peaks.push(self.bucket_peak.clamp(0.0, 1.0));
        }
        reduce_waveform_peaks(&self.peaks, self.max_points)
    }
}

fn ensure_sample_buffer(
    sample_buf: &mut Option<SampleBuffer<f32>>,
    capacity: usize,
    spec: SignalSpec,
) -> &mut SampleBuffer<f32> {
    let capacity_u64 = usize_to_u64(capacity);
    if sample_buf
        .as_ref()
        .is_none_or(|buffer| buffer.capacity() < capacity)
    {
        *sample_buf = Some(SampleBuffer::<f32>::new(capacity_u64, spec));
    }

    sample_buf
        .as_mut()
        .expect("sample buffer is initialized above")
}

fn waveform_sample_rate_divisor(sample_rate_hz: u64) -> u64 {
    const TARGET_48KHZ: u64 = 48_000;
    const TARGET_44K1HZ: u64 = 44_100;

    if sample_rate_hz <= TARGET_48KHZ {
        return 1;
    }
    if sample_rate_hz.is_multiple_of(TARGET_48KHZ) {
        return sample_rate_hz / TARGET_48KHZ;
    }
    if sample_rate_hz.is_multiple_of(TARGET_44K1HZ) {
        return sample_rate_hz / TARGET_44K1HZ;
    }
    1
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
            if !waveform.push_sample(samples[base].abs(), sample_stride, &mut on_update) {
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

#[cfg(feature = "gst")]
struct GstWaveformAccumulator {
    observed_span_ns: u64,
    peak_events: Vec<(u64, f32)>,
    fallback_peaks: Vec<f32>,
    level_messages_seen: usize,
    last_preview_emit: std::time::Instant,
    max_points: usize,
}

#[cfg(feature = "gst")]
impl GstWaveformAccumulator {
    fn new(max_points: usize, duration_ns: Option<u64>) -> Self {
        Self {
            observed_span_ns: duration_ns.unwrap_or(0),
            peak_events: Vec::with_capacity(max_points.saturating_mul(2)),
            fallback_peaks: Vec::with_capacity(max_points),
            level_messages_seen: 0,
            last_preview_emit: std::time::Instant::now(),
            max_points,
        }
    }

    fn record_peak(&mut self, structure: &gst::StructureRef, peak: f32) {
        self.level_messages_seen = self.level_messages_seen.saturating_add(1);
        if let Some((time_ns, end_ns)) = level_message_time_range_ns(structure) {
            self.observed_span_ns = self.observed_span_ns.max(end_ns);
            self.peak_events.push((time_ns, peak));
            return;
        }

        self.fallback_peaks.push(peak);
        if self.fallback_peaks.len() > self.max_points {
            self.fallback_peaks = reduce_waveform_peaks(&self.fallback_peaks, self.max_points);
        }
    }

    fn preview_ready(&self) -> bool {
        self.level_messages_seen >= 12
            && self.last_preview_emit.elapsed() >= Duration::from_millis(240)
    }

    fn take_preview(&mut self) -> Vec<f32> {
        self.last_preview_emit = std::time::Instant::now();
        if self.observed_span_ns > 0 && !self.peak_events.is_empty() {
            return materialize_waveform_peaks(
                &self.peak_events,
                self.observed_span_ns,
                self.max_points,
            );
        }
        self.fallback_peaks.clone()
    }

    fn coverage_seconds(&self) -> f32 {
        seconds_from_nanoseconds(self.observed_span_ns)
    }

    fn finish(self) -> Vec<f32> {
        if self.observed_span_ns > 0 && !self.peak_events.is_empty() {
            return materialize_waveform_peaks(
                &self.peak_events,
                self.observed_span_ns,
                self.max_points,
            );
        }
        if self.fallback_peaks.len() > self.max_points {
            return reduce_waveform_peaks(&self.fallback_peaks, self.max_points);
        }
        self.fallback_peaks
    }
}

#[cfg(feature = "gst")]
fn build_waveform_gst_pipeline(
    path: &Path,
) -> anyhow::Result<(gst::Pipeline, gst::Bus, gst::Element)> {
    let pipeline = gst::Pipeline::new();
    let src = gst::ElementFactory::make("filesrc")
        .build()
        .map_err(|_| anyhow::anyhow!("missing filesrc element"))?;
    src.set_property("location", path.to_string_lossy().to_string());

    let decodebin = gst::ElementFactory::make("decodebin")
        .build()
        .map_err(|_| anyhow::anyhow!("missing decodebin element"))?;
    let conv = gst::ElementFactory::make("audioconvert")
        .build()
        .map_err(|_| anyhow::anyhow!("missing audioconvert element"))?;
    let resample = gst::ElementFactory::make("audioresample")
        .build()
        .map_err(|_| anyhow::anyhow!("missing audioresample element"))?;
    let capsfilter = gst::ElementFactory::make("capsfilter")
        .build()
        .map_err(|_| anyhow::anyhow!("missing capsfilter element"))?;
    let caps = gst::Caps::builder("audio/x-raw")
        .field("format", "F32LE")
        .field("layout", "interleaved")
        .field("rate", 44_100i32)
        .build();
    capsfilter.set_property("caps", &caps);
    let level = gst::ElementFactory::make("level")
        .build()
        .map_err(|_| anyhow::anyhow!("missing level element"))?;
    let fakesink = gst::ElementFactory::make("fakesink")
        .build()
        .map_err(|_| anyhow::anyhow!("missing fakesink element"))?;
    fakesink.set_property("sync", false);

    pipeline.add_many([
        &src,
        &decodebin,
        &conv,
        &resample,
        &capsfilter,
        &level,
        &fakesink,
    ])?;
    src.link(&decodebin)?;
    gst::Element::link_many([&conv, &resample, &capsfilter, &level, &fakesink])?;

    let conv_sink_pad = conv
        .static_pad("sink")
        .ok_or_else(|| anyhow::anyhow!("missing audioconvert sink pad"))?;
    decodebin.connect_pad_added(move |_dbin, src_pad| {
        if conv_sink_pad.is_linked() {
            return;
        }
        let Some(caps) = src_pad
            .current_caps()
            .or_else(|| Some(src_pad.query_caps(None)))
        else {
            return;
        };
        let Some(structure) = caps.structure(0) else {
            return;
        };
        if !structure.name().starts_with("audio/") {
            return;
        }
        let _ = src_pad.link(&conv_sink_pad);
    });

    let bus = pipeline
        .bus()
        .ok_or_else(|| anyhow::anyhow!("waveform pipeline has no bus"))?;
    Ok((pipeline, bus, level))
}

#[cfg(feature = "gst")]
fn configure_waveform_gst_pipeline(
    pipeline: &gst::Pipeline,
    level: &gst::Element,
    max_points: usize,
) -> anyhow::Result<Option<u64>> {
    pipeline.set_state(gst::State::Paused)?;
    let _ = pipeline.state(gst::ClockTime::from_seconds(2));

    let duration_ns = pipeline
        .query_duration::<gst::ClockTime>()
        .map(gst::ClockTime::nseconds);
    level.set_property(
        "interval",
        level_message_interval_ns(max_points, duration_ns),
    );
    level.set_property("post-messages", true);
    pipeline.set_state(gst::State::Playing)?;
    Ok(duration_ns)
}

#[cfg(feature = "gst")]
fn decode_waveform_peaks_stream_gst<F, C>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, bool) -> bool,
    C: FnMut() -> bool,
{
    if is_cancelled() {
        return Ok(());
    }

    gst::init()?;
    let (pipeline, bus, level) = build_waveform_gst_pipeline(path)?;
    let duration_ns = configure_waveform_gst_pipeline(&pipeline, &level, max_points)?;
    let mut waveform = GstWaveformAccumulator::new(max_points, duration_ns);
    loop {
        if is_cancelled() {
            let _ = pipeline.set_state(gst::State::Null);
            return Ok(());
        }

        if let Some(msg) = bus.timed_pop_filtered(
            gst::ClockTime::from_mseconds(50),
            &[
                gst::MessageType::Element,
                gst::MessageType::Eos,
                gst::MessageType::Error,
            ],
        ) {
            match msg.view() {
                gst::MessageView::Element(element) => {
                    if let Some(structure) = element.message().structure() {
                        if let Some(peak) = level_message_peak(structure) {
                            waveform.record_peak(structure, peak);
                        }
                        if waveform.preview_ready()
                            && !on_update(
                                waveform.take_preview(),
                                waveform.coverage_seconds(),
                                false,
                            )
                        {
                            let _ = pipeline.set_state(gst::State::Null);
                            return Ok(());
                        }
                    }
                }
                gst::MessageView::Eos(..) => break,
                gst::MessageView::Error(err) => {
                    let _ = pipeline.set_state(gst::State::Null);
                    return Err(anyhow::anyhow!(
                        "gstreamer waveform decode failed: {} ({:?})",
                        err.error(),
                        err.debug()
                    ));
                }
                _ => {}
            }
        }
    }

    let coverage_seconds = waveform.coverage_seconds();
    let peaks = waveform.finish();

    let _ = pipeline.set_state(gst::State::Null);

    if !is_cancelled() {
        let _ = on_update(peaks, coverage_seconds, true);
    }
    Ok(())
}

#[cfg(feature = "gst")]
fn level_message_interval_ns(max_points: usize, duration_ns: Option<u64>) -> u64 {
    let fallback_duration_ns = 240u64 * 1_000_000_000;
    (duration_ns.unwrap_or(fallback_duration_ns) / usize_to_u64(max_points.max(1)))
        .clamp(20_000_000, 500_000_000)
}

#[cfg(feature = "gst")]
fn level_message_bin_index(time_ns: u64, duration_ns: u64, max_points: usize) -> Option<usize> {
    if duration_ns == 0 || max_points == 0 {
        return None;
    }

    let max_points_u64 = usize_to_u64(max_points);
    let raw_index = (time_ns.saturating_mul(max_points_u64) / duration_ns)
        .min(max_points_u64.saturating_sub(1));
    usize::try_from(raw_index).ok()
}

#[cfg(feature = "gst")]
fn level_message_time_range_ns(structure: &gst::StructureRef) -> Option<(u64, u64)> {
    let start = structure
        .get::<u64>("running-time")
        .ok()
        .or_else(|| structure.get::<u64>("stream-time").ok())
        .or_else(|| structure.get::<u64>("timestamp").ok())
        .or_else(|| {
            let end = structure.get::<u64>("endtime").ok()?;
            let duration = structure.get::<u64>("duration").ok().unwrap_or(0);
            Some(end.saturating_sub(duration))
        })?;
    let duration = structure.get::<u64>("duration").ok().unwrap_or(0);
    let end = structure
        .get::<u64>("endtime")
        .ok()
        .unwrap_or_else(|| start.saturating_add(duration));
    let center = start.saturating_add(duration / 2);
    Some((center, end.max(center)))
}

#[cfg(feature = "gst")]
fn level_message_peak(structure: &gst::StructureRef) -> Option<f32> {
    if structure.name() != "level" {
        return None;
    }

    let peaks = structure.value("peak").ok()?;
    collapse_level_peak_value(peaks)
}

#[cfg(feature = "gst")]
fn collapse_level_db_peaks(values: &[gst::glib::SendValue]) -> Option<f32> {
    let mut peak = 0.0f32;
    let mut seen_any = false;

    for value in values {
        let db = level_db_value(value)?;
        let linear = dbfs_peak_to_linear(db);
        if linear > peak {
            peak = linear;
        }
        seen_any = true;
    }

    seen_any.then_some(peak)
}

#[cfg(feature = "gst")]
fn collapse_level_peak_value(value: &gst::glib::SendValue) -> Option<f32> {
    if let Ok(peaks) = value.get::<gst::Array>() {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    if let Ok(peaks) = value.get::<gst::List>() {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    if let Ok(peaks) = value.get::<gst::glib::ValueArray>() {
        return collapse_level_db_values(peaks.as_slice());
    }
    level_db_value(value).map(dbfs_peak_to_linear)
}

#[cfg(feature = "gst")]
fn collapse_level_db_values(values: &[gst::glib::Value]) -> Option<f32> {
    let mut peak = 0.0f32;
    let mut seen_any = false;

    for value in values {
        let db = value
            .get::<f64>()
            .ok()
            .or_else(|| value.get::<f32>().ok().map(f64::from))?;
        let linear = dbfs_peak_to_linear(db);
        if linear > peak {
            peak = linear;
        }
        seen_any = true;
    }

    seen_any.then_some(peak)
}

#[cfg(feature = "gst")]
fn level_db_value(value: &gst::glib::SendValue) -> Option<f64> {
    value
        .get::<f64>()
        .ok()
        .or_else(|| value.get::<f32>().ok().map(f64::from))
}

#[cfg(feature = "gst")]
fn dbfs_peak_to_linear(db: f64) -> f32 {
    if !db.is_finite() || db <= -120.0 {
        return 0.0;
    }
    let linear = 10f64.powf(db / 20.0).clamp(0.0, 1.0);
    linear.to_string().parse::<f32>().unwrap_or(1.0)
}

fn fold_waveform_peaks(peaks: &[f32]) -> Vec<f32> {
    let mut reduced = Vec::with_capacity(peaks.len().div_ceil(2));
    for chunk in peaks.chunks(2) {
        let mut peak = 0.0f32;
        for &value in chunk {
            if value > peak {
                peak = value;
            }
        }
        reduced.push(peak);
    }
    reduced
}

fn reduce_waveform_peaks(peaks: &[f32], max_points: usize) -> Vec<f32> {
    if peaks.len() <= max_points || max_points == 0 {
        return peaks.to_vec();
    }

    let mut reduced = Vec::with_capacity(max_points);
    for i in 0..max_points {
        let idx = i.saturating_mul(peaks.len()) / max_points;
        reduced.push(peaks[idx.min(peaks.len() - 1)]);
    }
    reduced
}

#[cfg(feature = "gst")]
fn materialize_waveform_peaks(events: &[(u64, f32)], span_ns: u64, max_points: usize) -> Vec<f32> {
    if span_ns == 0 || max_points == 0 || events.is_empty() {
        return Vec::new();
    }

    let mut peaks = vec![0.0f32; max_points];
    for &(time_ns, peak) in events {
        if let Some(bin_index) = level_message_bin_index(time_ns, span_ns, max_points) {
            if peak > peaks[bin_index] {
                peaks[bin_index] = peak;
            }
        }
    }
    peaks
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;

    #[test]
    fn peaks_blob_roundtrip() {
        let peaks = vec![0.0f32, 0.25, 0.5, 1.0];
        let blob = encode_peaks_blob(&peaks);
        let decoded_peaks = decode_peaks_blob(&blob, peaks.len()).expect("decode");
        assert_eq!(decoded_peaks, peaks);
    }

    #[test]
    fn waveform_cache_persist_and_load_roundtrip() {
        let conn = Connection::open_in_memory().expect("in-memory db");
        init_waveform_cache_schema(&conn).expect("schema");
        let path = PathBuf::from("/tmp/test_track.flac");
        let stamp = WaveformSourceStamp {
            size_bytes: 1234,
            modified_secs: 100,
            modified_nanos: 55,
        };
        let peaks = vec![0.1f32, 0.3, 0.9];
        persist_waveform_to_db(&conn, &path, stamp, &peaks).expect("persist");
        let loaded = load_waveform_from_db(&conn, &path, stamp).expect("load");
        assert_eq!(loaded, peaks);
    }

    #[test]
    fn spectrogram_decimator_averages_rows() {
        let mut decimator = SpectrogramDecimator::new(2);
        let first = decimator.push(vec![2.0, 4.0]);
        assert!(first.is_none());
        let second = decimator.push(vec![4.0, 6.0]).expect("averaged row");
        assert_eq!(second, vec![3.0, 5.0]);
    }

    #[test]
    fn stft_computer_produces_rows_from_samples() {
        let mut stft = StftComputer::new(512, 128);
        let mut samples = Vec::new();
        for i in 0..4096usize {
            let x = (2.0 * std::f32::consts::PI * 440.0 * (small_usize_to_f32(i) / 48_000.0)).sin();
            samples.push(x);
        }
        stft.enqueue_samples(&samples, 48_000);
        let rows = stft.take_rows(4);
        assert!(!rows.is_empty());
        assert_eq!(rows[0].len(), 257);
    }

    #[test]
    fn stft_computer_keeps_row_count_with_chunked_input() {
        let mut stft = StftComputer::new(8, 4);
        let input: Vec<f32> = (0u16..24).map(f32::from).collect();
        let mut rows = 0usize;

        for chunk in input.chunks(3) {
            stft.enqueue_samples(chunk, 48_000);
            rows += stft.take_rows(1).len();
        }
        rows += stft.take_rows(64).len();

        assert_eq!(rows, 5);
        assert_eq!(stft.pending_len(), 4);
    }

    #[test]
    fn stft_computer_no_sample_loss_with_large_packet() {
        // FFT 512, hop 256: a typical audio packet of 4096 samples should
        // produce (4096 - 512) / 256 + 1 = 15 rows when drained one row
        // at a time (the pattern used by session_drain_stft_rows).
        let mut stft = StftComputer::new(512, 256);
        let samples: Vec<f32> = (0u32..4096).map(|i| (i as f32).sin()).collect();
        stft.enqueue_samples(&samples, 44_100);

        let mut rows = 0usize;
        loop {
            let batch = stft.take_rows(1);
            if batch.is_empty() {
                break;
            }
            rows += batch.len();
        }

        // Exact expected: floor((4096 - 512) / 256) + 1 = 15
        assert_eq!(rows, 15);
    }

    #[test]
    fn emit_snapshot_respects_force_and_waveform_dirty() {
        let (tx, rx) = unbounded::<AnalysisEvent>();
        let snapshot = AnalysisSnapshot {
            waveform_peaks: vec![0.1, 0.2],
            waveform_coverage_seconds: 0.0,
            waveform_complete: true,
            spectrogram_channels: Vec::new(),
            spectrogram_seq: 0,
            sample_rate_hz: 48_000,
            spectrogram_view_mode: SpectrogramViewMode::Downmix,
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
    fn emit_snapshot_bypasses_throttle_for_pending_spectrogram_rows() {
        let (tx, rx) = unbounded::<AnalysisEvent>();
        let snapshot = AnalysisSnapshot {
            waveform_peaks: Vec::new(),
            waveform_coverage_seconds: 0.0,
            waveform_complete: false,
            spectrogram_channels: Vec::new(),
            spectrogram_seq: 4,
            sample_rate_hz: 48_000,
            spectrogram_view_mode: SpectrogramViewMode::Downmix,
        };
        let mut pending_channels = vec![AnalysisSpectrogramChannel {
            label: SpectrogramChannelLabel::Mono,
            rows: vec![vec![0.1, 0.2, 0.3]],
        }];
        let mut waveform_dirty = false;
        let mut last_emit = std::time::Instant::now();

        emit_snapshot(
            &tx,
            &snapshot,
            &mut pending_channels,
            &mut waveform_dirty,
            &mut last_emit,
            false,
        );

        let evt = rx.try_recv().expect("spectrogram snapshot event");
        match evt {
            AnalysisEvent::Snapshot(s) => {
                assert_eq!(s.spectrogram_seq, 4);
                assert_eq!(s.spectrogram_channels.len(), 1);
                assert_eq!(s.spectrogram_channels[0].rows.len(), 1);
            }
            _ => panic!("unexpected event variant"),
        }
        assert!(pending_channels.is_empty());
    }

    #[test]
    fn waveform_sample_rate_divisor_targets_common_high_rate_multiples() {
        assert_eq!(waveform_sample_rate_divisor(44_100), 1);
        assert_eq!(waveform_sample_rate_divisor(48_000), 1);
        assert_eq!(waveform_sample_rate_divisor(88_200), 2);
        assert_eq!(waveform_sample_rate_divisor(96_000), 2);
        assert_eq!(waveform_sample_rate_divisor(176_400), 4);
        assert_eq!(waveform_sample_rate_divisor(192_000), 4);
        assert_eq!(waveform_sample_rate_divisor(384_000), 8);
    }

    #[test]
    fn waveform_sample_rate_divisor_leaves_non_matching_rates_untouched() {
        assert_eq!(waveform_sample_rate_divisor(32_000), 1);
        assert_eq!(waveform_sample_rate_divisor(44_000), 1);
        assert_eq!(waveform_sample_rate_divisor(50_000), 1);
        assert_eq!(waveform_sample_rate_divisor(64_000), 1);
    }

    #[cfg(feature = "gst")]
    #[test]
    fn collapse_level_message_peaks_uses_loudest_channel() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::Array::new([-18.0f64, -6.0, -12.0]))
            .build();

        let peak = level_message_peak(structure.as_ref()).expect("peak");

        assert!((peak - 10f32.powf(-6.0 / 20.0)).abs() < 0.0001);
    }

    #[cfg(feature = "gst")]
    #[test]
    fn collapse_level_message_peaks_treats_floor_as_silence() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::Array::new([-150.0f64, f64::NEG_INFINITY]))
            .build();

        assert_eq!(level_message_peak(structure.as_ref()), Some(0.0));
    }

    #[cfg(feature = "gst")]
    #[test]
    fn collapse_level_message_peaks_accepts_list_values_too() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::List::new([-9.0f64, -3.0]))
            .build();

        let peak = level_message_peak(structure.as_ref()).expect("peak");

        assert!((peak - 10f32.powf(-3.0 / 20.0)).abs() < 0.0001);
    }

    #[cfg(feature = "gst")]
    #[test]
    fn collapse_level_message_peaks_accepts_value_array_too() {
        let _ = gst::init();
        let peaks = gst::glib::ValueArray::new([-15.0f64, -4.0]);
        let peak = collapse_level_db_values(peaks.as_slice()).expect("peak");

        assert!((peak - 10f32.powf(-4.0 / 20.0)).abs() < 0.0001);
    }

    #[cfg(feature = "gst")]
    #[test]
    fn level_message_bin_index_uses_running_time_when_present() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("running-time", 5_000_000_000u64)
            .field("duration", 1_000_000_000u64)
            .field("peak", gst::Array::new([-9.0f64]))
            .build();

        let (time_ns, _) = level_message_time_range_ns(structure.as_ref()).expect("time");
        assert_eq!(
            level_message_bin_index(time_ns, 10_000_000_000, 100),
            Some(55)
        );
    }

    #[cfg(feature = "gst")]
    #[test]
    fn level_message_bin_index_falls_back_to_end_minus_duration() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("endtime", 8_000_000_000u64)
            .field("duration", 2_000_000_000u64)
            .field("peak", gst::Array::new([-9.0f64]))
            .build();

        let (time_ns, end_ns) = level_message_time_range_ns(structure.as_ref()).expect("time");
        assert_eq!(end_ns, 8_000_000_000);
        assert_eq!(
            level_message_bin_index(time_ns, 10_000_000_000, 100),
            Some(70)
        );
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
    fn explicit_seek_command_forces_seek_even_inside_lookahead() {
        let mut session = SpectrogramSessionState {
            track_token: 1,
            gen: 1,
            fft_size: 2_048,
            hop_size: 256,
            view_mode: SpectrogramViewMode::Downmix,
            display_mode: SpectrogramDisplayMode::Rolling,
            channel_count: 1,
            bins_per_column: 1_025,
            total_columns_estimate: 8_739,
            effective_rate: 48_000,
            cols_per_second: 46.875,
            divisor: 1,
            target_position_seconds: 2.0,
            columns_produced: 256,
            session_start_column: 0,
            stfts: Vec::new(),
            decimators: Vec::new(),
            sample_buf: None,
            packet_counter: 0,
            chunk_buf: Vec::new(),
            chunk_columns: 0,
            chunk_start_index: 256,
            target_chunk_columns: 1,
            total_covered_samples: 0,
            session_start_time: std::time::Instant::now(),
            post_reset_burst: 0,
            decode_rate_limit: 2.0,
            lookahead_columns: 512,
            pending_gapless: None,
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
}
