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
    },
    SetSampleRate(u32),
    SetFftSize(usize),
    SetSpectrogramViewMode(SpectrogramViewMode),
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
pub enum AnalysisEvent {
    Snapshot(AnalysisSnapshot),
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

struct AnalysisRuntimeState {
    snapshot: AnalysisSnapshot,
    pending_channels: Vec<AnalysisSpectrogramChannel>,
    waveform_dirty: bool,
    last_emit: std::time::Instant,
    spectrogram: SpectrogramRuntime,
    active_track_token: u64,
    active_track_path: Option<PathBuf>,
    active_track_stamp: Option<WaveformSourceStamp>,
    waveform_cache: HashMap<PathBuf, WaveformCacheEntry>,
    waveform_cache_lru: VecDeque<PathBuf>,
    waveform_db: Option<Connection>,
    waveform_db_writes_since_prune: usize,
    pcm_fifo: VecDeque<f32>,
    pcm_labels: Vec<SpectrogramChannelLabel>,
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
        spawn_analysis_worker(
            cmd_rx,
            pcm_rx,
            event_tx,
            waveform_job_tx,
            waveform_decode_active_token,
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
            waveform_cache: HashMap::new(),
            waveform_cache_lru: VecDeque::new(),
            waveform_db: open_waveform_cache_db().ok(),
            waveform_db_writes_since_prune: 0,
            pcm_fifo: VecDeque::with_capacity(48_000),
            pcm_labels: vec![SpectrogramChannelLabel::Mono],
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

    fn handle_command(
        &mut self,
        cmd: AnalysisCommand,
        pcm_rx: &Receiver<AnalysisPcmChunk>,
        event_tx: &Sender<AnalysisEvent>,
        waveform_job_tx: &Sender<WaveformDecodeJob>,
        waveform_decode_active_token: &AtomicU64,
    ) {
        match cmd {
            AnalysisCommand::SetTrack {
                path,
                reset_spectrogram,
            } => self.handle_track_change(
                path,
                reset_spectrogram,
                pcm_rx,
                event_tx,
                waveform_job_tx,
                waveform_decode_active_token,
            ),
            AnalysisCommand::SetSampleRate(rate) => {
                if rate > 0 {
                    self.snapshot.sample_rate_hz = rate;
                    self.emit_snapshot(event_tx, true);
                }
            }
            AnalysisCommand::SetFftSize(size) => {
                let fft = size.clamp(512, 8192).next_power_of_two();
                let hop = (fft / 8).max(64);
                self.spectrogram.set_fft_size(fft, hop);
                self.reset_spectrogram_state(pcm_rx);
                self.emit_snapshot(event_tx, true);
            }
            AnalysisCommand::SetSpectrogramViewMode(view_mode) => {
                self.snapshot.spectrogram_view_mode = view_mode;
                self.spectrogram.set_view_mode(view_mode);
                self.reset_spectrogram_state(pcm_rx);
                self.emit_snapshot(event_tx, true);
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
                event_tx,
            ),
        }
    }

    fn handle_track_change(
        &mut self,
        path: PathBuf,
        reset_spectrogram: bool,
        pcm_rx: &Receiver<AnalysisPcmChunk>,
        event_tx: &Sender<AnalysisEvent>,
        waveform_job_tx: &Sender<WaveformDecodeJob>,
        waveform_decode_active_token: &AtomicU64,
    ) {
        self.active_track_token = self.active_track_token.wrapping_add(1);
        let track_token = self.active_track_token;
        waveform_decode_active_token.store(track_token, Ordering::Relaxed);
        self.active_track_stamp = source_stamp(&path);
        self.active_track_path = Some(path.clone());

        self.snapshot.waveform_peaks.clear();
        self.snapshot.waveform_coverage_seconds = 0.0;
        self.snapshot.waveform_complete = false;
        self.waveform_dirty = true;
        if reset_spectrogram {
            self.reset_spectrogram_state(pcm_rx);
        }
        self.emit_snapshot(event_tx, true);

        if let Some(peaks) = self.load_cached_waveform(&path) {
            self.snapshot.waveform_peaks = peaks;
            self.snapshot.waveform_coverage_seconds = 0.0;
            self.snapshot.waveform_complete = true;
            self.waveform_dirty = true;
            self.emit_snapshot(event_tx, true);
            return;
        }

        let _ = waveform_job_tx.send(WaveformDecodeJob { track_token, path });
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

    fn reset_spectrogram_state(&mut self, pcm_rx: &Receiver<AnalysisPcmChunk>) {
        self.pending_channels.clear();
        self.snapshot.spectrogram_seq = 0;
        self.spectrogram.reset();
        drain_pcm_queue(pcm_rx);
        self.pcm_fifo.clear();
        self.pcm_labels = vec![SpectrogramChannelLabel::Mono];
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
        self.prof_pcm += 1;
        self.prof_in_samples += chunk.samples.len();
        let chunk_labels = if chunk.channel_labels.is_empty() {
            vec![SpectrogramChannelLabel::Mono]
        } else {
            chunk.channel_labels.clone()
        };
        if chunk_labels != self.pcm_labels {
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

fn spawn_analysis_worker(
    cmd_rx: Receiver<AnalysisCommand>,
    pcm_rx: Receiver<AnalysisPcmChunk>,
    event_tx: Sender<AnalysisEvent>,
    waveform_job_tx: Sender<WaveformDecodeJob>,
    waveform_decode_active_token: Arc<AtomicU64>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-analysis".to_string())
        .spawn(move || {
            let mut state = AnalysisRuntimeState::new();
            loop {
                select! {
                    recv(cmd_rx) -> msg => {
                        let Ok(cmd) = msg else { break; };
                        state.handle_command(
                            cmd,
                            &pcm_rx,
                            &event_tx,
                            &waveform_job_tx,
                            waveform_decode_active_token.as_ref(),
                        );
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

fn drain_pcm_queue(pcm_rx: &Receiver<AnalysisPcmChunk>) {
    while pcm_rx.try_recv().is_ok() {}
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
    if !force && last_emit.elapsed() < std::time::Duration::from_millis(16) {
        return;
    }
    if !*waveform_dirty && pending_channels.is_empty() && !force {
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

        // If producer is outrunning us, drop backlog to keep spectrogram in real-time sync.
        let max_backlog = self.fft_size * 4;
        let available = self.pending_available();
        if available > max_backlog {
            let drop = available - max_backlog;
            self.pending_start = self.pending_start.saturating_add(drop);
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
        }
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
}
