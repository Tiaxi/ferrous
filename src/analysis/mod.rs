use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{select, tick, unbounded, Receiver, Sender};
use realfft::{num_complex::Complex32, RealFftPlanner, RealToComplex};
use rusqlite::{params, Connection};
use symphonia::core::audio::SampleBuffer;
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
    WaveformProgress {
        track_token: u64,
        peaks: Vec<f32>,
        done: bool,
    },
}

#[derive(Debug, Clone, Default)]
pub struct AnalysisSnapshot {
    pub waveform_peaks: Vec<f32>,
    pub spectrogram_rows: Vec<Vec<f32>>,
    pub spectrogram_seq: u64,
    pub sample_rate_hz: u32,
}

#[derive(Debug, Clone)]
pub enum AnalysisEvent {
    Snapshot(AnalysisSnapshot),
}

pub struct AnalysisEngine {
    tx: Sender<AnalysisCommand>,
    pcm_tx: Sender<Vec<f32>>,
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

impl AnalysisEngine {
    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    pub fn new() -> (Self, Receiver<AnalysisEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<AnalysisCommand>();
        // Bounded PCM queue to prevent unbounded backlog under decode bursts.
        let (pcm_tx, pcm_rx) = crossbeam_channel::bounded::<Vec<f32>>(12);
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();

        let waveform_tx = cmd_tx.clone();
        let (waveform_job_tx, waveform_job_rx) = unbounded::<WaveformDecodeJob>();
        let waveform_decode_active_token = Arc::new(AtomicU64::new(0));
        {
            let waveform_tx = waveform_tx.clone();
            let waveform_decode_active_token = Arc::clone(&waveform_decode_active_token);
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
                            |peaks, done| {
                                if waveform_decode_active_token.load(Ordering::Relaxed)
                                    != track_token
                                {
                                    return false;
                                }
                                let _ = waveform_tx.send(AnalysisCommand::WaveformProgress {
                                    track_token,
                                    peaks,
                                    done,
                                });
                                true
                            },
                            || waveform_decode_active_token.load(Ordering::Relaxed) != track_token,
                        );
                    }
                });
        }
        let _ = std::thread::Builder::new()
            .name("ferrous-analysis".to_string())
            .spawn(move || {
            let mut snapshot = AnalysisSnapshot {
                sample_rate_hz: 48_000,
                ..AnalysisSnapshot::default()
            };
            let mut pending_rows: Vec<Vec<f32>> = Vec::new();
            let mut waveform_dirty = false;
            let mut last_emit = std::time::Instant::now();

            let mut stft = StftComputer::new(8192, 1024);
            let mut decimator = SpectrogramDecimator::new(decimation_factor_for_hop(1024));
            let mut active_track_token = 0u64;
            let mut active_track_path: Option<PathBuf> = None;
            let mut active_track_stamp: Option<WaveformSourceStamp> = None;
            let mut waveform_cache: HashMap<PathBuf, WaveformCacheEntry> = HashMap::new();
            let mut waveform_cache_lru: VecDeque<PathBuf> = VecDeque::new();
            let mut waveform_db = open_waveform_cache_db().ok();
            let mut waveform_db_writes_since_prune = 0usize;
            let ticker = tick(Duration::from_millis(16));
            let mut pcm_fifo: VecDeque<f32> = VecDeque::with_capacity(48_000);
            let mut last_tick_time = std::time::Instant::now();
            let mut sample_credit = 0.0f64;
            let profile_enabled =
                cfg!(feature = "profiling-logs") && std::env::var_os("FERROUS_PROFILE").is_some();
            let mut prof_last = std::time::Instant::now();
            #[allow(unused_variables, unused_assignments)]
            let mut prof_pcm = 0usize;
            #[allow(unused_variables, unused_assignments)]
            let mut prof_rows = 0usize;
            #[allow(unused_variables, unused_assignments)]
            let mut prof_ticks = 0usize;
            #[allow(unused_variables, unused_assignments)]
            let mut prof_in_samples = 0usize;
            #[allow(unused_variables, unused_assignments)]
            let mut prof_out_samples = 0usize;
            let mut ticks_without_row = 0usize;

            loop {
                select! {
                    recv(cmd_rx) -> msg => {
                        let Ok(cmd) = msg else { break; };
                        match cmd {
                            AnalysisCommand::SetTrack {
                                path,
                                reset_spectrogram,
                            } => {
                                active_track_token = active_track_token.wrapping_add(1);
                                let track_token = active_track_token;
                                waveform_decode_active_token.store(track_token, Ordering::Relaxed);
                                active_track_stamp = source_stamp(&path);
                                active_track_path = Some(path.clone());

                                snapshot.waveform_peaks.clear();
                                waveform_dirty = true;
                                if reset_spectrogram {
                                    snapshot.spectrogram_seq = 0;
                                    pending_rows.clear();
                                    stft.reset_full();
                                    decimator.reset();
                                    drain_pcm_queue(&pcm_rx);
                                    pcm_fifo.clear();
                                    last_tick_time = std::time::Instant::now();
                                    sample_credit = 0.0;
                                }
                                emit_snapshot(
                                    &event_tx,
                                    &snapshot,
                                    &mut pending_rows,
                                    &mut waveform_dirty,
                                    &mut last_emit,
                                    true,
                                );

                                let cache_hit = waveform_cache
                                    .get(&path)
                                    .filter(|entry| entry.stamp == active_track_stamp)
                                    .map(|entry| entry.peaks.clone())
                                    .filter(|peaks| !peaks.is_empty());
                                let peaks = if let Some(peaks) = cache_hit {
                                    touch_waveform_cache_lru(&mut waveform_cache_lru, &path);
                                    Some(peaks)
                                } else if let (Some(conn), Some(stamp)) =
                                    (waveform_db.as_ref(), active_track_stamp)
                                {
                                    let disk_hit = load_waveform_from_db(conn, &path, stamp);
                                    if let Some(peaks) = disk_hit.as_ref() {
                                        insert_waveform_cache_entry(
                                            &mut waveform_cache,
                                            &mut waveform_cache_lru,
                                            path.clone(),
                                            WaveformCacheEntry {
                                                stamp: Some(stamp),
                                                peaks: peaks.clone(),
                                            },
                                        );
                                    }
                                    disk_hit
                                } else {
                                    None
                                };

                                if let Some(peaks) = peaks {
                                    snapshot.waveform_peaks = peaks;
                                    waveform_dirty = true;
                                    emit_snapshot(
                                        &event_tx,
                                        &snapshot,
                                        &mut pending_rows,
                                        &mut waveform_dirty,
                                        &mut last_emit,
                                        true,
                                    );
                                } else {
                                    let _ = waveform_job_tx.send(WaveformDecodeJob {
                                        track_token,
                                        path,
                                    });
                                }
                            }
                            AnalysisCommand::SetSampleRate(rate) => {
                                if rate > 0 {
                                    snapshot.sample_rate_hz = rate;
                                    emit_snapshot(
                                        &event_tx,
                                        &snapshot,
                                        &mut pending_rows,
                                        &mut waveform_dirty,
                                        &mut last_emit,
                                        true,
                                    );
                                }
                            }
                            AnalysisCommand::SetFftSize(size) => {
                                let fft = size.clamp(512, 8192).next_power_of_two();
                                let hop = (fft / 8).max(64);
                                stft = StftComputer::new(fft, hop);
                                decimator = SpectrogramDecimator::new(decimation_factor_for_hop(hop));
                                pending_rows.clear();
                                snapshot.spectrogram_seq = 0;
                                drain_pcm_queue(&pcm_rx);
                                pcm_fifo.clear();
                                sample_credit = 0.0;
                                last_tick_time = std::time::Instant::now();
                                emit_snapshot(
                                    &event_tx,
                                    &snapshot,
                                    &mut pending_rows,
                                    &mut waveform_dirty,
                                    &mut last_emit,
                                    true,
                                );
                            }
                            AnalysisCommand::WaveformProgress {
                                track_token,
                                peaks,
                                done,
                            } => {
                                if track_token == active_track_token {
                                    if peaks.is_empty() {
                                        if done {
                                            // Ignore and do not persist empty waveform snapshots.
                                            // A zero-point waveform is treated as a decode miss.
                                        }
                                        continue;
                                    }
                                    snapshot.waveform_peaks = peaks;
                                    if done {
                                        if let Some(path) = active_track_path.as_ref() {
                                            let cached_peaks = snapshot.waveform_peaks.clone();
                                            insert_waveform_cache_entry(
                                                &mut waveform_cache,
                                                &mut waveform_cache_lru,
                                                path.clone(),
                                                WaveformCacheEntry {
                                                    stamp: active_track_stamp,
                                                    peaks: cached_peaks.clone(),
                                                },
                                            );
                                            if let (Some(conn), Some(stamp)) =
                                                (waveform_db.as_mut(), active_track_stamp)
                                            {
                                                let _ = persist_waveform_to_db(
                                                    conn,
                                                    path,
                                                    stamp,
                                                    &cached_peaks,
                                                );
                                                waveform_db_writes_since_prune = waveform_db_writes_since_prune
                                                    .saturating_add(1);
                                                if waveform_db_writes_since_prune
                                                    >= PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL
                                                {
                                                    let _ = prune_persistent_waveform_cache(
                                                        conn,
                                                        PERSISTENT_WAVEFORM_CACHE_MAX_ROWS,
                                                    );
                                                    waveform_db_writes_since_prune = 0;
                                                }
                                            }
                                        }
                                    }
                                    waveform_dirty = true;
                                    if done || snapshot.waveform_peaks.len() >= 24 {
                                        emit_snapshot(
                                            &event_tx,
                                            &snapshot,
                                            &mut pending_rows,
                                            &mut waveform_dirty,
                                            &mut last_emit,
                                            true,
                                        );
                                    }
                                }
                            }
                        }
                    }
                    recv(ticker) -> _ => {
                        prof_ticks += 1;

                        // Pull available PCM chunks into a sample FIFO first.
                        for _ in 0..64 {
                            let Ok(samples) = pcm_rx.try_recv() else {
                                break;
                            };
                            prof_pcm += 1;
                            prof_in_samples += samples.len();
                            pcm_fifo.extend(samples);
                        }

                        // Keep FIFO bounded to roughly 0.5s to avoid visual lead/lag buildup.
                        let fifo_max = (snapshot.sample_rate_hz as usize / 2).max(4096);
                        while pcm_fifo.len() > fifo_max {
                            let _ = pcm_fifo.pop_front();
                        }

                        // Feed STFT at real-time cadence from elapsed clock time to minimize drift.
                        let now = std::time::Instant::now();
                        let dt = now.duration_since(last_tick_time).as_secs_f64();
                        last_tick_time = now;
                        sample_credit += dt * snapshot.sample_rate_hz as f64;
                        let mut target_samples = sample_credit.floor() as usize;
                        sample_credit -= target_samples as f64;
                        target_samples = target_samples.clamp(256, 2048);

                        // Keep visuals slightly behind output to compensate sink/device buffering.
                        let visual_delay_ms = BASE_VISUAL_DELAY_MS as usize;
                        let effective_delay_samples =
                            (snapshot.sample_rate_hz as usize) * visual_delay_ms / 1000;
                        // Enforce configured visual delay by consuming only from samples older
                        // than the delay horizon.
                        let available = pcm_fifo.len().saturating_sub(effective_delay_samples);

                        // Closed-loop backlog control: steer FIFO depth toward configured delay
                        // to limit drift when producer/consumer pacing differs.
                        let backlog_error =
                            pcm_fifo.len() as isize - effective_delay_samples as isize;
                        let correction = (backlog_error / 8).clamp(-512, 512);
                        let adjusted_target =
                            (target_samples as isize + correction).clamp(0, 4096) as usize;

                        let to_feed = adjusted_target.min(available);
                        if to_feed > 0 {
                            let mut feed = Vec::with_capacity(to_feed);
                            for _ in 0..to_feed {
                                if let Some(v) = pcm_fifo.pop_front() {
                                    feed.push(v);
                                }
                            }
                            prof_out_samples += feed.len();
                            stft.enqueue_samples(&feed, snapshot.sample_rate_hz);
                        }

                        let rows = stft.take_rows(8);
                        prof_rows += rows.len();
                        if rows.is_empty() {
                            ticks_without_row = ticks_without_row.saturating_add(1);
                        } else {
                            ticks_without_row = 0;
                        }
                        for row in rows {
                            if let Some(slow_row) = decimator.push(row) {
                                pending_rows.push(slow_row);
                                snapshot.spectrogram_seq = snapshot.spectrogram_seq.wrapping_add(1);
                            }
                        }
                        emit_snapshot(
                            &event_tx,
                            &snapshot,
                            &mut pending_rows,
                            &mut waveform_dirty,
                            &mut last_emit,
                            false,
                        );

                        if profile_enabled && prof_last.elapsed() >= Duration::from_secs(1) {
                            profile_eprintln!(
                                "[analysis] ticks/s={} pcm_chunks/s={} in_samples/s={} out_samples/s={} rows/s={} pending_samples={} fifo_samples={} fft={} hop={}",
                                prof_ticks,
                                prof_pcm,
                                prof_in_samples,
                                prof_out_samples,
                                prof_rows,
                                stft.pending_len(),
                                pcm_fifo.len(),
                                stft.fft_size(),
                                stft.hop_size()
                            );
                            prof_last = std::time::Instant::now();
                            prof_pcm = 0;
                            prof_in_samples = 0;
                            prof_out_samples = 0;
                            prof_rows = 0;
                            prof_ticks = 0;
                        }
                    }
                }
            }
        });

        (Self { tx: cmd_tx, pcm_tx }, event_rx)
    }

    pub fn command(&self, cmd: AnalysisCommand) {
        let _ = self.tx.send(cmd);
    }

    pub fn sender(&self) -> Sender<AnalysisCommand> {
        self.tx.clone()
    }

    pub fn pcm_sender(&self) -> Sender<Vec<f32>> {
        self.pcm_tx.clone()
    }
}

fn decimation_factor_for_hop(hop: usize) -> usize {
    if hop == 0 {
        return 1;
    }
    (REFERENCE_HOP / hop).max(1)
}

fn drain_pcm_queue(pcm_rx: &Receiver<Vec<f32>>) {
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
                stamp.size_bytes as i64,
                stamp.modified_secs as i64,
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
            stamp.size_bytes as i64,
            stamp.modified_secs as i64,
            i64::from(stamp.modified_nanos),
            WAVEFORM_CACHE_FORMAT_VERSION,
            peaks.len() as i64,
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
        params![max_rows as i64],
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
        .map(|d| d.as_secs() as i64)
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
    path: PathBuf,
    entry: WaveformCacheEntry,
) {
    if entry.peaks.is_empty() {
        cache.remove(&path);
        if let Some(pos) = lru.iter().position(|p| p == &path) {
            lru.remove(pos);
        }
        return;
    }
    cache.insert(path.clone(), entry);
    touch_waveform_cache_lru(lru, &path);

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
    pending_rows: &mut Vec<Vec<f32>>,
    waveform_dirty: &mut bool,
    last_emit: &mut std::time::Instant,
    force: bool,
) {
    if !force && last_emit.elapsed() < std::time::Duration::from_millis(16) {
        return;
    }
    if !*waveform_dirty && pending_rows.is_empty() && !force {
        return;
    }

    let out = AnalysisSnapshot {
        waveform_peaks: if *waveform_dirty {
            snapshot.waveform_peaks.clone()
        } else {
            Vec::new()
        },
        spectrogram_rows: std::mem::take(pending_rows),
        spectrogram_seq: snapshot.spectrogram_seq,
        sample_rate_hz: snapshot.sample_rate_hz,
    };
    let _ = event_tx.send(AnalysisEvent::Snapshot(out));
    *waveform_dirty = false;
    *last_emit = std::time::Instant::now();
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
        let max_pending = (sample_rate_hz as usize / 2).max(self.fft_size * 4);
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

        let inv = 1.0 / self.count as f32;
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
    let n = size as f32;
    (0..size)
        .map(|i| {
            let phase = (2.0 * std::f32::consts::PI * i as f32) / n;
            0.35875 - 0.48829 * phase.cos() + 0.14128 * (2.0 * phase).cos()
                - 0.01168 * (3.0 * phase).cos()
        })
        .collect()
}

fn decode_waveform_peaks_stream<F, C>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, bool) -> bool,
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

    let mut decoder =
        symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default())?;
    let sample_rate_hz = track.codec_params.sample_rate.unwrap_or(48_000) as u64;
    let estimated_frames = track.codec_params.n_frames.unwrap_or(sample_rate_hz * 240);
    let mut block_size = (estimated_frames / max_points.max(1) as u64).max(1);

    let mut sample_buf: Option<SampleBuffer<f32>> = None;
    let mut peaks = Vec::with_capacity(max_points);
    let mut bucket_peak = 0.0f32;
    let mut bucket_count = 0u64;
    let mut last_preview_emit = std::time::Instant::now();

    let mut packet_counter = 0usize;
    loop {
        if is_cancelled() {
            return Ok(());
        }
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(SymphoniaError::ResetRequired) => break,
            Err(_) => break,
        };

        if packet.track_id() != track_id {
            continue;
        }
        packet_counter += 1;

        let decoded = match decoder.decode(&packet) {
            Ok(decoded) => decoded,
            Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => break,
            Err(SymphoniaError::DecodeError(_)) => continue,
            Err(_) => break,
        };

        let spec = *decoded.spec();
        let channels = spec.channels.count().max(1);
        let sample_stride = if channels >= 2 { 8usize } else { 4usize };
        let cap = decoded.capacity() as u64;
        let cap_usize = decoded.capacity();

        if sample_buf.is_none() {
            sample_buf = Some(SampleBuffer::<f32>::new(cap, spec));
        }
        let Some(buf) = sample_buf.as_mut() else {
            continue;
        };
        if buf.capacity() < cap_usize {
            *buf = SampleBuffer::<f32>::new(cap, spec);
        }

        buf.copy_interleaved_ref(decoded);

        let samples = buf.samples();
        let frame_width = channels.saturating_mul(sample_stride).max(1);
        for base in (0..samples.len()).step_by(frame_width) {
            if base.is_multiple_of(4096) && is_cancelled() {
                return Ok(());
            }
            let amp = samples[base].abs();
            if amp > bucket_peak {
                bucket_peak = amp;
            }
            bucket_count += sample_stride as u64;

            if bucket_count >= block_size {
                peaks.push(bucket_peak.clamp(0.0, 1.0));
                bucket_peak = 0.0;
                bucket_count = 0;
                // Keep waveform memory bounded even when duration/frame estimates are inaccurate.
                while peaks.len() > max_points {
                    let mut reduced = Vec::with_capacity(peaks.len().div_ceil(2));
                    for chunk in peaks.chunks(2) {
                        let mut p = 0.0f32;
                        for &v in chunk {
                            if v > p {
                                p = v;
                            }
                        }
                        reduced.push(p);
                    }
                    peaks = reduced;
                    block_size = block_size.saturating_mul(2).max(1);
                }
                if peaks.len() >= 12
                    && last_preview_emit.elapsed() >= std::time::Duration::from_millis(240)
                {
                    if !on_update(peaks.clone(), false) {
                        return Ok(());
                    }
                    last_preview_emit = std::time::Instant::now();
                }
            }
        }

        // Keep this worker from starving UI/render threads on heavy FLAC decode.
        if packet_counter.is_multiple_of(64) {
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }

    if bucket_count > 0 {
        peaks.push(bucket_peak.clamp(0.0, 1.0));
    }

    if peaks.len() > max_points {
        let stride = peaks.len() as f32 / max_points as f32;
        let mut reduced = Vec::with_capacity(max_points);
        for i in 0..max_points {
            let idx = (i as f32 * stride) as usize;
            reduced.push(peaks[idx.min(peaks.len() - 1)]);
        }
        peaks = reduced;
    }

    if !is_cancelled() {
        let _ = on_update(peaks, true);
    }
    Ok(())
}

#[cfg(feature = "gst")]
fn decode_waveform_peaks_stream_gst<F, C>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, bool) -> bool,
    C: FnMut() -> bool,
{
    if is_cancelled() {
        return Ok(());
    }

    gst::init()?;

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

    let mut peaks = Vec::with_capacity(max_points);
    let mut last_preview_emit = std::time::Instant::now();

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
                    if let Some(peak) = element.message().structure().and_then(level_message_peak) {
                        peaks.push(peak);
                        if peaks.len() > max_points {
                            peaks = reduce_waveform_peaks(&peaks, max_points);
                        }
                        if peaks.len() >= 12
                            && last_preview_emit.elapsed() >= Duration::from_millis(240)
                        {
                            if !on_update(peaks.clone(), false) {
                                let _ = pipeline.set_state(gst::State::Null);
                                return Ok(());
                            }
                            last_preview_emit = std::time::Instant::now();
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

    if peaks.len() > max_points {
        peaks = reduce_waveform_peaks(&peaks, max_points);
    }

    let _ = pipeline.set_state(gst::State::Null);

    if !is_cancelled() {
        let _ = on_update(peaks, true);
    }
    Ok(())
}

#[cfg(feature = "gst")]
fn level_message_interval_ns(max_points: usize, duration_ns: Option<u64>) -> u64 {
    let fallback_duration_ns = 240u64 * 1_000_000_000;
    (duration_ns.unwrap_or(fallback_duration_ns) / max_points.max(1) as u64)
        .clamp(20_000_000, 500_000_000)
}

#[cfg(feature = "gst")]
fn level_message_peak(structure: &gst::StructureRef) -> Option<f32> {
    if structure.name() != "level" {
        return None;
    }

    if let Ok(peaks) = structure.get::<gst::Array>("peak") {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    if let Ok(peaks) = structure.get::<gst::List>("peak") {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    None
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
    10f32.powf((db as f32) / 20.0).clamp(0.0, 1.0)
}

fn reduce_waveform_peaks(peaks: &[f32], max_points: usize) -> Vec<f32> {
    if peaks.len() <= max_points || max_points == 0 {
        return peaks.to_vec();
    }

    let stride = peaks.len() as f32 / max_points as f32;
    let mut reduced = Vec::with_capacity(max_points);
    for i in 0..max_points {
        let idx = (i as f32 * stride) as usize;
        reduced.push(peaks[idx.min(peaks.len() - 1)]);
    }
    reduced
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel::unbounded;

    #[test]
    fn peaks_blob_roundtrip() {
        let peaks = vec![0.0f32, 0.25, 0.5, 1.0];
        let blob = encode_peaks_blob(&peaks);
        let decoded = decode_peaks_blob(&blob, peaks.len()).expect("decode");
        assert_eq!(decoded, peaks);
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
            let x = (2.0 * std::f32::consts::PI * 440.0 * (i as f32 / 48_000.0)).sin();
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
        let input: Vec<f32> = (0..24).map(|v| v as f32).collect();
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
            spectrogram_rows: Vec::new(),
            spectrogram_seq: 0,
            sample_rate_hz: 48_000,
        };
        let mut pending_rows = Vec::<Vec<f32>>::new();
        let mut waveform_dirty = true;
        let mut last_emit = std::time::Instant::now() - Duration::from_secs(1);

        emit_snapshot(
            &tx,
            &snapshot,
            &mut pending_rows,
            &mut waveform_dirty,
            &mut last_emit,
            true,
        );
        let evt = rx.try_recv().expect("snapshot event");
        match evt {
            AnalysisEvent::Snapshot(s) => assert_eq!(s.waveform_peaks, vec![0.1, 0.2]),
        }
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
}
