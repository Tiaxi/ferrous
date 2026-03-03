use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
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

#[derive(Debug, Clone)]
pub enum AnalysisCommand {
    SetTrack {
        path: PathBuf,
        reset_spectrogram: bool,
    },
    SetSampleRate(u32),
    SetFftSize(usize),
    SetSpectrogramOffsetMs(i32),
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
const BASE_VISUAL_DELAY_MS: i32 = 40;
const MIN_SPECTROGRAM_OFFSET_MS: i32 = -120;
const MAX_SPECTROGRAM_OFFSET_MS: i32 = 240;

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

impl AnalysisEngine {
    pub fn new() -> (Self, Receiver<AnalysisEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<AnalysisCommand>();
        // Bounded PCM queue to prevent unbounded backlog under decode bursts.
        let (pcm_tx, pcm_rx) = crossbeam_channel::bounded::<Vec<f32>>(12);
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();

        let waveform_tx = cmd_tx.clone();
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
            let mut decimator = SpectrogramDecimator::new(1);
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
            let profile_enabled = std::env::var_os("FERROUS_PROFILE").is_some();
            let mut prof_last = std::time::Instant::now();
            let mut prof_pcm = 0usize;
            let mut prof_rows = 0usize;
            let mut prof_ticks = 0usize;
            let mut prof_in_samples = 0usize;
            let mut prof_out_samples = 0usize;
            let mut ticks_without_row = 0usize;
            let mut spectrogram_offset_ms = 0_i32;

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
                                    .map(|entry| entry.peaks.clone());
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
                                    let tx = waveform_tx.clone();
                                    let _ = std::thread::Builder::new()
                                        .name("ferrous-waveform-decode".to_string())
                                        .spawn(move || {
                                        let _ =
                                            decode_waveform_peaks_stream(&path, 1024, |peaks, done| {
                                                let _ = tx.send(AnalysisCommand::WaveformProgress {
                                                    track_token,
                                                    peaks,
                                                    done,
                                                });
                                            });
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
                                decimator.reset();
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
                            AnalysisCommand::SetSpectrogramOffsetMs(offset_ms) => {
                                spectrogram_offset_ms =
                                    offset_ms.clamp(MIN_SPECTROGRAM_OFFSET_MS, MAX_SPECTROGRAM_OFFSET_MS);
                            }
                            AnalysisCommand::WaveformProgress {
                                track_token,
                                peaks,
                                done,
                            } => {
                                if track_token == active_track_token {
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
                        let visual_delay_ms =
                            (BASE_VISUAL_DELAY_MS + spectrogram_offset_ms).clamp(0, MAX_SPECTROGRAM_OFFSET_MS)
                                as usize;
                        let visual_delay_samples =
                            (snapshot.sample_rate_hz as usize) * visual_delay_ms / 1000;
                        // Enforce configured visual delay by consuming only from samples older
                        // than the delay horizon.
                        let available = pcm_fifo.len().saturating_sub(visual_delay_samples);

                        // Closed-loop backlog control: steer FIFO depth toward configured delay
                        // so offset remains effective even when producer/consumer drift.
                        let backlog_error = pcm_fifo.len() as isize - visual_delay_samples as isize;
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
                            eprintln!(
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
    decode_peaks_blob(&row.2, peak_count)
}

fn persist_waveform_to_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
    peaks: &[f32],
) -> rusqlite::Result<()> {
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

    fn pending_len(&self) -> usize {
        self.pending_available()
    }

    fn fft_size(&self) -> usize {
        self.fft_size
    }

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

fn decode_waveform_peaks_stream<F>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, bool),
{
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
                    on_update(peaks.clone(), false);
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

    on_update(peaks, true);
    Ok(())
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
}
