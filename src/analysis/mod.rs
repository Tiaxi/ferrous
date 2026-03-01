use std::collections::VecDeque;
use std::fs::File;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crossbeam_channel::{select, tick, unbounded, Receiver, Sender};
use realfft::{num_complex::Complex32, RealFftPlanner, RealToComplex};
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[derive(Debug, Clone)]
pub enum AnalysisCommand {
    SetTrack(PathBuf),
    SetSampleRate(u32),
    ResetRealtime,
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

impl AnalysisEngine {
    pub fn new() -> (Self, Receiver<AnalysisEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<AnalysisCommand>();
        // Bounded PCM queue to prevent unbounded backlog under decode bursts.
        let (pcm_tx, pcm_rx) = crossbeam_channel::bounded::<Vec<f32>>(12);
        let (event_tx, event_rx) = unbounded::<AnalysisEvent>();

        let waveform_tx = cmd_tx.clone();
        std::thread::spawn(move || {
            let mut snapshot = AnalysisSnapshot::default();
            snapshot.sample_rate_hz = 48_000;
            let mut pending_rows: Vec<Vec<f32>> = Vec::new();
            let mut waveform_dirty = false;
            let mut last_emit = std::time::Instant::now();

            let mut stft = StftComputer::new(512, 128);
            // Higher factor = slower horizontal scroll and larger visible time window.
            let mut decimator = SpectrogramDecimator::new(8);
            let mut active_track_token = 0u64;
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

            loop {
                select! {
                    recv(cmd_rx) -> msg => {
                        let Ok(cmd) = msg else { break; };
                        match cmd {
                            AnalysisCommand::SetTrack(path) => {
                                active_track_token = active_track_token.wrapping_add(1);
                                let track_token = active_track_token;

                                snapshot.waveform_peaks.clear();
                                snapshot.spectrogram_seq = 0;
                                pending_rows.clear();
                                waveform_dirty = true;
                                stft.reset_full();
                                decimator.reset();
                                drain_pcm_queue(&pcm_rx);
                                pcm_fifo.clear();
                                last_tick_time = std::time::Instant::now();
                                sample_credit = 0.0;
                                emit_snapshot(
                                    &event_tx,
                                    &snapshot,
                                    &mut pending_rows,
                                    &mut waveform_dirty,
                                    &mut last_emit,
                                    true,
                                );

                                let tx = waveform_tx.clone();
                                std::thread::spawn(move || {
                                    let _ = decode_waveform_peaks_stream(&path, 4096, |peaks, done| {
                                        let _ = tx.send(AnalysisCommand::WaveformProgress {
                                            track_token,
                                            peaks,
                                            done,
                                        });
                                    });
                                });
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
                            AnalysisCommand::ResetRealtime => {
                                snapshot.spectrogram_seq = 0;
                                pending_rows.clear();
                                stft.reset_full();
                                decimator.reset();
                                drain_pcm_queue(&pcm_rx);
                                pcm_fifo.clear();
                                last_tick_time = std::time::Instant::now();
                                sample_credit = 0.0;
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
                                    snapshot.waveform_peaks = peaks;
                                    waveform_dirty = true;
                                    if done || snapshot.waveform_peaks.len() >= 128 {
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
                        let visual_delay_samples =
                            ((snapshot.sample_rate_hz as usize) * 40 / 1000).max(512);
                        let available = pcm_fifo.len().saturating_sub(visual_delay_samples);
                        let to_feed = target_samples.min(available);
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
                                "[analysis] ticks/s={} pcm_chunks/s={} in_samples/s={} out_samples/s={} rows/s={} pending_samples={} fifo_samples={}",
                                prof_ticks,
                                prof_pcm,
                                prof_in_samples,
                                prof_out_samples,
                                prof_rows,
                                stft.pending_len(),
                                pcm_fifo.len()
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
        let window = hann_window(fft_size);

        Self {
            r2c,
            fft_in,
            fft_out,
            pending: Vec::with_capacity(fft_size * 2),
            window,
            fft_size,
            hop_size,
        }
    }

    fn reset_full(&mut self) {
        self.pending.clear();
    }

    fn enqueue_samples(&mut self, samples: &[f32], sample_rate_hz: u32) {
        self.pending.extend_from_slice(samples);
        // Keep pending bounded to avoid latency creep: max ~0.5s audio.
        let max_pending = (sample_rate_hz as usize / 2).max(self.fft_size * 4);
        if self.pending.len() > max_pending {
            let drop = self.pending.len() - max_pending;
            self.pending.drain(0..drop);
        }
    }

    fn take_rows(&mut self, max_rows: usize) -> Vec<Vec<f32>> {
        let mut rows = Vec::new();

        while self.pending.len() >= self.fft_size && rows.len() < max_rows {
            for i in 0..self.fft_size {
                self.fft_in[i] = self.pending[i] * self.window[i];
            }

            if self
                .r2c
                .process(&mut self.fft_in, &mut self.fft_out)
                .is_ok()
            {
                let row: Vec<f32> = self
                    .fft_out
                    .iter()
                    .map(|bin| {
                        let mag = bin.norm();
                        let db = 20.0 * (mag + 1e-8).log10();
                        let n = ((db + 92.0) / 92.0).clamp(0.0, 1.0);
                        n.powf(1.22)
                    })
                    .collect();
                rows.push(row);
            }

            let drain = self.hop_size.min(self.pending.len());
            self.pending.drain(0..drain);
        }

        // If producer is outrunning us, drop backlog to keep spectrogram in real-time sync.
        let max_backlog = self.fft_size * 4;
        if self.pending.len() > max_backlog {
            let keep_from = self.pending.len() - max_backlog;
            self.pending.drain(0..keep_from);
        }

        rows
    }

    fn pending_len(&self) -> usize {
        self.pending.len()
    }
}

struct SpectrogramDecimator {
    factor: usize,
    accum: Vec<f32>,
    count: usize,
    prev: Option<Vec<f32>>,
}

impl SpectrogramDecimator {
    fn new(factor: usize) -> Self {
        Self {
            factor: factor.max(1),
            accum: Vec::new(),
            count: 0,
            prev: None,
        }
    }

    fn reset(&mut self) {
        self.accum.clear();
        self.count = 0;
        self.prev = None;
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
            out.push((v * inv).clamp(0.0, 1.0));
        }

        self.accum.fill(0.0);
        self.count = 0;

        let blended = if let Some(prev) = self.prev.as_ref() {
            out.iter()
                .zip(prev.iter())
                .map(|(cur, old)| (old * 0.35 + cur * 0.65).clamp(0.0, 1.0))
                .collect::<Vec<f32>>()
        } else {
            out
        };
        self.prev = Some(blended.clone());
        Some(blended)
    }
}

fn hann_window(size: usize) -> Vec<f32> {
    let n = size as f32;
    (0..size)
        .map(|i| {
            let phase = (2.0 * std::f32::consts::PI * i as f32) / n;
            0.5 - 0.5 * phase.cos()
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
    let mss = MediaSourceStream::new(Box::new(file), Default::default());

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
    let block_size = (estimated_frames / max_points.max(1) as u64).max(1);

    let mut sample_buf: Option<SampleBuffer<f32>> = None;
    let mut peaks = Vec::with_capacity(max_points);
    let mut bucket_peak = 0.0f32;
    let mut bucket_count = 0u64;

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

        for frame in buf.samples().chunks(channels) {
            let mono = frame.iter().copied().sum::<f32>() / channels as f32;
            let amp = mono.abs();
            if amp > bucket_peak {
                bucket_peak = amp;
            }
            bucket_count += 1;

            if bucket_count >= block_size {
                peaks.push(bucket_peak.clamp(0.0, 1.0));
                bucket_peak = 0.0;
                bucket_count = 0;
                if peaks.len() % 256 == 0 {
                    on_update(peaks.clone(), false);
                }
            }
        }

        // Keep this worker from starving UI/render threads on heavy FLAC decode.
        if packet_counter % 64 == 0 {
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
