// SPDX-License-Identifier: GPL-3.0-or-later

//! Headless performance driver for the analysis and playback backend.
//!
//! It runs the real threads through the public API and prints one JSON
//! object per measurement on stdout, for before/after comparisons. Run it
//! in a release build, with `HOME` and the `XDG_*_HOME` variables pointing
//! at directories of its own, since the backend opens the library database
//! and the settings there:
//!
//! ```text
//! cargo run --release --example perf_analysis -- analysis FILE [options]
//! cargo run --release --example perf_analysis -- seek FILE [options]
//! cargo run --release --example perf_analysis -- window FILE [options]
//! ```
//!
//! `analysis` drives an `AnalysisEngine` without playback: it loads the
//! file as the bridge does and times the overview waveform and the
//! spectrogram columns until both stop. In centered mode the spectrogram
//! decodes as fast as it can up to its lookahead, which
//! `FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS` raises up to the 64 MiB budget.
//!
//! `seek` plays the file through the bridge, with its `GStreamer` engine and
//! audio output, and times each seek: until the engine applied it, until
//! playback advances again, and until spectrogram columns at the new
//! position arrive.
//!
//! `window` times waveform detail windows through the FFI function the Qt
//! waveform editor and level meter call.
//!
//! Every mode reports the CPU time of each thread, sampled from
//! `/proc/self/task` while it runs.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use ferrous::analysis::{
    AnalysisCommand, AnalysisEngine, AnalysisEvent, SpectrogramDisplayMode, SpectrogramViewMode,
};
use ferrous::frontend_bridge::{
    BridgeAnalysisCommand, BridgeCommand, BridgeEvent, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSettingsCommand, FrontendBridgeHandle,
};
use ferrous::playback::PlaybackState;

struct Options {
    file: PathBuf,
    fft: usize,
    view: SpectrogramViewMode,
    mode: SpectrogramDisplayMode,
    width: u32,
    timeout: Duration,
    /// Seek targets as fractions of the duration.
    seeks: Vec<f64>,
    /// Seconds to wait after each seek.
    settle: f64,
    /// Seconds of playback before the first seek.
    warmup: f64,
    /// Wait for the analysis to go idle before the first seek.
    idle: bool,
    volume: f32,
    /// Window length in seconds and points, for `window`.
    window_seconds: f64,
    points: u32,
    /// The track's duration, for a whole-track window in `window`.
    duration: f64,
}

fn parse_options(args: &[String]) -> Result<Options, String> {
    let mut options = Options {
        file: PathBuf::new(),
        fft: 2048,
        view: SpectrogramViewMode::PerChannel,
        mode: SpectrogramDisplayMode::Centered,
        width: 3440,
        timeout: Duration::from_secs(600),
        seeks: vec![0.6, 0.2, 0.8, 0.4, 0.65],
        settle: 3.0,
        warmup: 4.0,
        idle: false,
        volume: 0.15,
        window_seconds: 10.0,
        points: 3440,
        duration: 0.0,
    };
    let mut iter = args.iter();
    let mut file = None;
    while let Some(arg) = iter.next() {
        let mut value = || iter.next().cloned().ok_or(format!("{arg} needs a value"));
        match arg.as_str() {
            "--fft" => options.fft = value()?.parse().map_err(|_| "bad --fft")?,
            "--view" => {
                options.view = match value()?.as_str() {
                    "downmix" => SpectrogramViewMode::Downmix,
                    _ => SpectrogramViewMode::PerChannel,
                };
            }
            "--mode" => {
                options.mode = match value()?.as_str() {
                    "rolling" => SpectrogramDisplayMode::Rolling,
                    _ => SpectrogramDisplayMode::Centered,
                };
            }
            "--width" => options.width = value()?.parse().map_err(|_| "bad --width")?,
            "--timeout" => {
                options.timeout =
                    Duration::from_secs(value()?.parse().map_err(|_| "bad --timeout")?);
            }
            "--seeks" => {
                options.seeks = value()?
                    .split(',')
                    .map(str::parse)
                    .collect::<Result<_, _>>()
                    .map_err(|_| "bad --seeks")?;
            }
            "--settle" => options.settle = value()?.parse().map_err(|_| "bad --settle")?,
            "--warmup" => options.warmup = value()?.parse().map_err(|_| "bad --warmup")?,
            "--idle" => options.idle = true,
            "--volume" => options.volume = value()?.parse().map_err(|_| "bad --volume")?,
            "--window" => {
                options.window_seconds = value()?.parse().map_err(|_| "bad --window")?;
            }
            "--points" => options.points = value()?.parse().map_err(|_| "bad --points")?,
            "--duration" => {
                options.duration = value()?.parse().map_err(|_| "bad --duration")?;
            }
            other if !other.starts_with("--") && file.is_none() => file = Some(other.into()),
            other => return Err(format!("unknown argument {other}")),
        }
    }
    options.file = file.ok_or("no file given")?;
    Ok(options)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some((mode, rest)) = args.split_first() else {
        eprintln!("usage: perf_analysis analysis|seek|window FILE [options]");
        std::process::exit(2);
    };
    let options = match parse_options(rest) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(2);
        }
    };
    let sampler = ThreadSampler::start();
    let result = match mode.as_str() {
        "analysis" => run_analysis(&options),
        "seek" => run_seek(&options),
        "window" => run_window(&options),
        _ => Err(format!("unknown mode {mode}")),
    };
    let threads = sampler.finish();
    match result {
        Ok(fields) => println!("{}", json_object(&fields, &threads)),
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(1);
        }
    }
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

type Fields = Vec<(String, String)>;

fn field(fields: &mut Fields, name: &str, value: impl std::fmt::Display) {
    fields.push((name.to_string(), value.to_string()));
}

fn ms(duration: Option<Duration>) -> String {
    duration.map_or_else(
        || "null".to_string(),
        |d| format!("{:.1}", d.as_secs_f64() * 1000.0),
    )
}

fn json_object(fields: &Fields, threads: &BTreeMap<String, ThreadCpu>) -> String {
    let mut out = String::from("{");
    for (name, value) in fields {
        let _ = write!(out, "\"{name}\":{value},");
    }
    out.push_str("\"threads\":{");
    let entries: Vec<String> = threads
        .iter()
        .map(|(name, cpu)| {
            format!(
                "\"{}\":{{\"count\":{},\"cpu_ms\":{:.0}}}",
                name.replace('"', "'"),
                cpu.count,
                cpu.seconds * 1000.0
            )
        })
        .collect();
    out.push_str(&entries.join(","));
    out.push_str("}}");
    out
}

// ---------------------------------------------------------------------------
// Per-thread CPU time
// ---------------------------------------------------------------------------

#[derive(Default)]
struct ThreadCpu {
    count: usize,
    seconds: f64,
}

/// Samples the CPU time of every thread of this process every 50 ms, so
/// that threads which end before the run does are counted too.
struct ThreadSampler {
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    thread: std::thread::JoinHandle<BTreeMap<u32, (String, u64)>>,
    started: Instant,
    process_start: u64,
}

impl ThreadSampler {
    fn start() -> Self {
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let stop_flag = std::sync::Arc::clone(&stop);
        let thread = std::thread::Builder::new()
            .name("perf-sampler".to_string())
            .spawn(move || {
                let mut seen = BTreeMap::new();
                loop {
                    sample_threads(&mut seen);
                    if stop_flag.load(std::sync::atomic::Ordering::Relaxed) {
                        return seen;
                    }
                    std::thread::sleep(Duration::from_millis(50));
                }
            })
            .expect("sampler thread");
        Self {
            stop,
            thread,
            started: Instant::now(),
            process_start: process_cpu_ticks(),
        }
    }

    fn finish(self) -> BTreeMap<String, ThreadCpu> {
        self.stop.store(true, std::sync::atomic::Ordering::Relaxed);
        let seen = self.thread.join().expect("sampler");
        let ticks = clock_ticks();
        let mut by_name: BTreeMap<String, ThreadCpu> = BTreeMap::new();
        for (name, total) in seen.into_values() {
            let entry = by_name.entry(name).or_default();
            entry.count += 1;
            entry.seconds += approx_f64(total) / ticks;
        }
        let process = by_name.entry("_process_total".to_string()).or_default();
        process.count = 1;
        process.seconds = approx_f64(process_cpu_ticks() - self.process_start) / ticks;
        let wall = by_name.entry("_wall".to_string()).or_default();
        wall.count = 1;
        wall.seconds = self.started.elapsed().as_secs_f64();
        by_name
    }
}

fn clock_ticks() -> f64 {
    // SAFETY: sysconf has no preconditions.
    let ticks = unsafe { libc::sysconf(libc::_SC_CLK_TCK) };
    u32::try_from(ticks).map_or(100.0, f64::from).max(1.0)
}

/// utime + stime from a `stat` line, whose second field may hold spaces.
fn stat_ticks(stat: &str) -> Option<u64> {
    let rest = &stat[stat.rfind(')')? + 2..];
    let fields: Vec<&str> = rest.split_whitespace().collect();
    // Fields after the name start at the state, field 3; utime is 14.
    Some(fields.get(11)?.parse::<u64>().ok()? + fields.get(12)?.parse::<u64>().ok()?)
}

fn process_cpu_ticks() -> u64 {
    std::fs::read_to_string("/proc/self/stat")
        .ok()
        .and_then(|stat| stat_ticks(&stat))
        .unwrap_or(0)
}

fn sample_threads(seen: &mut BTreeMap<u32, (String, u64)>) {
    let Ok(entries) = std::fs::read_dir("/proc/self/task") else {
        return;
    };
    for entry in entries.flatten() {
        let Ok(tid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let dir = entry.path();
        let Some(ticks) = std::fs::read_to_string(dir.join("stat"))
            .ok()
            .and_then(|stat| stat_ticks(&stat))
        else {
            continue;
        };
        let name = std::fs::read_to_string(dir.join("comm"))
            .map(|name| normalize_thread_name(name.trim()))
            .unwrap_or_default();
        seen.insert(tid, (name, ticks));
    }
}

/// Groups numbered threads such as `qtdemux0:sink` or `pool-12` by name.
fn normalize_thread_name(name: &str) -> String {
    name.chars()
        .map(|c| if c.is_ascii_digit() { '#' } else { c })
        .collect::<String>()
}

// ---------------------------------------------------------------------------
// analysis: the engine without playback
// ---------------------------------------------------------------------------

fn run_analysis(options: &Options) -> Result<Fields, String> {
    let (engine, events) = AnalysisEngine::new();
    engine.command(AnalysisCommand::SetFftSize(options.fft));
    engine.command(AnalysisCommand::SetSpectrogramViewMode(options.view));
    engine.command(AnalysisCommand::SetSpectrogramDisplayMode(options.mode));
    engine.command(AnalysisCommand::SetSpectrogramWidgetWidth(options.width));
    engine.command(AnalysisCommand::SetSpectrogramActive(true));
    let started = Instant::now();
    engine.command(AnalysisCommand::SetTrack {
        path: options.file.clone(),
        reset_spectrogram: true,
        track_token: 1,
        gapless: false,
    });
    engine.command(AnalysisCommand::PositionUpdate(0.0));

    let mut tally = AnalysisTally::default();
    // A centered session that parks at its lookahead stops sending; it has
    // gone idle when nothing arrived for this long.
    let idle = Duration::from_millis(1500);
    loop {
        let now = started.elapsed();
        if now > options.timeout || tally.finished(now, idle) {
            break;
        }
        if let Ok(event) = events.recv_timeout(Duration::from_millis(50)) {
            tally.record(event, started.elapsed())?;
        }
    }
    drop(engine);
    Ok(tally.report(options))
}

/// What `analysis` saw of the engine's output, with times since the track
/// was set.
#[derive(Default)]
struct AnalysisTally {
    first_peaks: Option<Duration>,
    waveform_done: Option<Duration>,
    waveform_coverage: f32,
    snapshots: u64,
    first_columns: Option<Duration>,
    last_columns: Option<Duration>,
    spectral_complete: Option<Duration>,
    columns: u64,
    chunks: u64,
    chunk_bytes: u64,
    hop: u16,
    rate: u32,
    channels: u8,
    bins: u16,
}

impl AnalysisTally {
    /// Whether the overview is done and the spectrogram reached the end or
    /// sent nothing for `idle`.
    fn finished(&self, now: Duration, idle: Duration) -> bool {
        let spectral_idle = self.spectral_complete.is_some()
            || self
                .last_columns
                .is_some_and(|last| now.saturating_sub(last) > idle);
        self.waveform_done.is_some() && spectral_idle
    }

    fn record(&mut self, event: AnalysisEvent, at: Duration) -> Result<(), String> {
        match event {
            AnalysisEvent::Snapshot(snapshot) => {
                self.snapshots += 1;
                if !snapshot.waveform_peaks.is_empty() {
                    self.first_peaks.get_or_insert(at);
                }
                self.waveform_coverage = snapshot.waveform_coverage_seconds;
                if snapshot.waveform_complete {
                    self.waveform_done.get_or_insert(at);
                }
            }
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                self.chunks += 1;
                self.chunk_bytes += usize_u64(chunk.columns_u8.len());
                if chunk.column_count > 0 {
                    self.first_columns.get_or_insert(at);
                    self.last_columns = Some(at);
                    self.columns += u64::from(chunk.column_count);
                    self.hop = chunk.hop_size;
                    self.rate = chunk.sample_rate_hz;
                    self.channels = chunk.channel_count;
                    self.bins = chunk.bins_per_column;
                }
                if chunk.complete {
                    self.spectral_complete.get_or_insert(at);
                }
            }
            AnalysisEvent::WorkerFailed(message) => return Err(message),
        }
        Ok(())
    }

    fn report(&self, options: &Options) -> Fields {
        let spectral_seconds = if self.rate > 0 {
            approx_f64(self.columns) * f64::from(self.hop) / f64::from(self.rate)
        } else {
            0.0
        };
        // The time the columns took, from the first command to the last one.
        let spectral_wall = self.last_columns.map_or(0.0, |d| d.as_secs_f64());
        let waveform_rtf = self
            .waveform_done
            .map_or(0.0, |d| f64::from(self.waveform_coverage) / d.as_secs_f64());
        let spectral_rtf = if spectral_wall > 0.0 {
            spectral_seconds / spectral_wall
        } else {
            0.0
        };
        let mut fields = Fields::new();
        field(&mut fields, "mode", "\"analysis\"");
        field(
            &mut fields,
            "file",
            format!("\"{}\"", file_label(&options.file)),
        );
        field(&mut fields, "fft", options.fft);
        field(&mut fields, "view", format!("\"{:?}\"", options.view));
        field(&mut fields, "display", format!("\"{:?}\"", options.mode));
        field(&mut fields, "width", options.width);
        field(&mut fields, "waveform_first_ms", ms(self.first_peaks));
        field(&mut fields, "waveform_done_ms", ms(self.waveform_done));
        field(
            &mut fields,
            "waveform_coverage_s",
            format!("{:.1}", self.waveform_coverage),
        );
        field(&mut fields, "waveform_rtf", format!("{waveform_rtf:.1}"));
        field(&mut fields, "snapshots", self.snapshots);
        field(&mut fields, "spectral_first_ms", ms(self.first_columns));
        field(&mut fields, "spectral_last_ms", ms(self.last_columns));
        field(
            &mut fields,
            "spectral_eof",
            self.spectral_complete.is_some(),
        );
        field(&mut fields, "spectral_columns", self.columns);
        field(&mut fields, "spectral_chunks", self.chunks);
        field(&mut fields, "spectral_bytes", self.chunk_bytes);
        field(
            &mut fields,
            "spectral_audio_s",
            format!("{spectral_seconds:.1}"),
        );
        field(&mut fields, "spectral_rtf", format!("{spectral_rtf:.1}"));
        field(&mut fields, "rate", self.rate);
        field(&mut fields, "channels", self.channels);
        field(&mut fields, "bins", self.bins);
        field(&mut fields, "hop", self.hop);
        fields
    }
}

/// A count as `f64`; the counts here are far below 2^52.
#[allow(clippy::cast_precision_loss)]
fn approx_f64(value: u64) -> f64 {
    value as f64
}

fn usize_u64(value: usize) -> u64 {
    u64::try_from(value).unwrap_or(u64::MAX)
}

fn file_label(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
}

// ---------------------------------------------------------------------------
// seek: playback through the bridge
// ---------------------------------------------------------------------------

#[derive(Default)]
struct SeekTiming {
    target: f64,
    /// The engine reports the seek done.
    landed: Option<Duration>,
    /// The first snapshot after it that says the position advances.
    advancing: Option<Duration>,
    /// The first spectrogram chunk of a new generation or a reset.
    first_chunk: Option<Duration>,
    /// The session reset for the seek; without one, the columns at the
    /// target were there already.
    reset: bool,
    /// The first chunk that covers the target column.
    target_chunk: Option<Duration>,
    /// Snapshots the bridge sent while the seek settled.
    snapshots: u64,
}

struct Bridge {
    handle: FrontendBridgeHandle,
    duration: f64,
    state: PlaybackState,
    seek_landed_serial: u64,
    position_advancing: bool,
    generation: u64,
    snapshots: u64,
    chunks: u64,
    chunk_bytes: u64,
    /// The sample rate and hop of the latest chunk.
    column_rate: f64,
    /// Waveform overview complete.
    waveform_complete: bool,
}

enum Seen {
    Snapshot,
    Chunk {
        /// A new generation, or a reset of the current one.
        reset: bool,
        start: u64,
        count: u64,
    },
    Nothing,
}

impl Bridge {
    fn poll(&mut self, timeout: Duration) -> Seen {
        match self.handle.recv_timeout(timeout) {
            Some(BridgeEvent::Snapshot(snapshot)) => {
                self.snapshots += 1;
                let playback = &snapshot.playback;
                self.duration = playback.duration.as_secs_f64();
                self.state = playback.state;
                self.seek_landed_serial = playback.seek_landed_serial;
                self.position_advancing = playback.position_advancing;
                self.waveform_complete = snapshot.analysis.waveform_complete;
                Seen::Snapshot
            }
            Some(BridgeEvent::PrecomputedSpectrogramChunk(chunk)) => {
                self.chunks += 1;
                self.chunk_bytes += usize_u64(chunk.columns_u8.len());
                let new_generation = chunk.generation != self.generation;
                self.generation = chunk.generation;
                if chunk.hop_size > 0 && chunk.sample_rate_hz > 0 {
                    self.column_rate = f64::from(chunk.sample_rate_hz) / f64::from(chunk.hop_size);
                }
                Seen::Chunk {
                    reset: new_generation || chunk.buffer_reset,
                    start: u64::from(chunk.start_column_index),
                    count: u64::from(chunk.column_count),
                }
            }
            Some(BridgeEvent::Error(error)) => {
                eprintln!("bridge error: {error:?}");
                Seen::Nothing
            }
            _ => Seen::Nothing,
        }
    }

    fn wait_for(&mut self, timeout: Duration, mut done: impl FnMut(&Self) -> bool) -> bool {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if done(self) {
                return true;
            }
            self.poll(Duration::from_millis(20));
        }
        done(self)
    }
}

fn run_seek(options: &Options) -> Result<Fields, String> {
    let (mut bridge, start_latency) = start_playback(options)?;
    bridge.wait_for(Duration::from_secs_f64(options.warmup), |_| false);
    let mut idle_wait = None;
    if options.idle {
        let idle_started = Instant::now();
        // A centered session keeps sending columns at the playback rate,
        // so idle is the overview done and two seconds for the lookahead.
        bridge.wait_for(options.timeout, |b| b.waveform_complete);
        bridge.wait_for(Duration::from_secs(2), |_| false);
        idle_wait = Some(idle_started.elapsed());
    }
    let busy_at_first_seek = !bridge.waveform_complete;
    let targets: Vec<f64> = options.seeks.iter().map(|f| f * bridge.duration).collect();
    let timings: Vec<SeekTiming> = targets
        .into_iter()
        .map(|target| measure_seek(&mut bridge, target, options.settle))
        .collect();
    let _ = bridge.handle.shutdown();

    let mut fields = Fields::new();
    field(&mut fields, "mode", "\"seek\"");
    field(
        &mut fields,
        "file",
        format!("\"{}\"", file_label(&options.file)),
    );
    field(&mut fields, "fft", options.fft);
    field(&mut fields, "view", format!("\"{:?}\"", options.view));
    field(&mut fields, "display", format!("\"{:?}\"", options.mode));
    field(&mut fields, "idle_before_seeks", options.idle);
    field(
        &mut fields,
        "analysis_busy_at_first_seek",
        busy_at_first_seek,
    );
    field(&mut fields, "idle_wait_ms", ms(idle_wait));
    field(&mut fields, "start_ms", ms(Some(start_latency)));
    field(&mut fields, "duration_s", format!("{:.1}", bridge.duration));
    field(&mut fields, "chunks", bridge.chunks);
    field(&mut fields, "chunk_bytes", bridge.chunk_bytes);
    let seeks: Vec<String> = timings.iter().map(SeekTiming::json).collect();
    field(&mut fields, "seeks", format!("[{}]", seeks.join(",")));
    Ok(fields)
}

/// Start the bridge with the options' settings and play the file. Returns
/// the bridge and how long playback took to start.
fn start_playback(options: &Options) -> Result<(Bridge, Duration), String> {
    let mut bridge = Bridge {
        handle: FrontendBridgeHandle::spawn()?,
        duration: 0.0,
        state: PlaybackState::Stopped,
        seek_landed_serial: 0,
        position_advancing: false,
        generation: 0,
        snapshots: 0,
        chunks: 0,
        chunk_bytes: 0,
        column_rate: 0.0,
        waveform_complete: false,
    };
    let settings = [
        BridgeSettingsCommand::SetVolume(options.volume),
        BridgeSettingsCommand::SetFftSize(options.fft),
        BridgeSettingsCommand::SetSpectrogramViewMode(options.view),
        BridgeSettingsCommand::SetSpectrogramDisplayMode(options.mode),
    ];
    for command in settings {
        bridge.handle.command(BridgeCommand::Settings(command));
    }
    bridge.handle.command(BridgeCommand::Analysis(
        BridgeAnalysisCommand::SetSpectrogramWidgetWidth(options.width),
    ));
    bridge.handle.command(BridgeCommand::Analysis(
        BridgeAnalysisCommand::SetSpectrogramActive(true),
    ));
    let started = Instant::now();
    bridge
        .handle
        .command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![options.file.clone()],
            autoplay: true,
        }));
    if !bridge.wait_for(Duration::from_secs(20), |b| {
        b.state == PlaybackState::Playing && b.position_advancing
    }) {
        return Err("playback did not start".to_string());
    }
    Ok((bridge, started.elapsed()))
}

/// Seek to `target` seconds and watch the bridge for `settle` seconds.
fn measure_seek(bridge: &mut Bridge, target: f64, settle: f64) -> SeekTiming {
    let mut timing = SeekTiming {
        target,
        ..SeekTiming::default()
    };
    let serial = bridge.seek_landed_serial;
    let snapshots_before = bridge.snapshots;
    let issued = Instant::now();
    bridge
        .handle
        .command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs_f64(target),
        )));
    let deadline = issued + Duration::from_secs_f64(settle);
    while Instant::now() < deadline {
        let seen = bridge.poll(Duration::from_millis(5));
        let elapsed = issued.elapsed();
        match seen {
            Seen::Snapshot => {
                if bridge.seek_landed_serial > serial {
                    timing.landed.get_or_insert(elapsed);
                }
                if timing.landed.is_some() && bridge.position_advancing {
                    timing.advancing.get_or_insert(elapsed);
                }
            }
            Seen::Chunk {
                reset,
                start,
                count,
            } => {
                timing.reset |= reset;
                if timing.reset {
                    timing.first_chunk.get_or_insert(elapsed);
                    // Column indices are far below 2^52, and targets positive.
                    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
                    let target_column = (target * bridge.column_rate) as u64;
                    if count > 0 && start <= target_column && target_column < start + count {
                        timing.target_chunk.get_or_insert(elapsed);
                    }
                }
            }
            Seen::Nothing => {}
        }
    }
    timing.snapshots = bridge.snapshots - snapshots_before;
    timing
}

impl SeekTiming {
    fn json(&self) -> String {
        format!(
            "{{\"target_s\":{:.1},\"landed_ms\":{},\"advancing_ms\":{},\"first_chunk_ms\":{},\"target_chunk_ms\":{},\"reset\":{},\"snapshots\":{}}}",
            self.target,
            ms(self.landed),
            ms(self.advancing),
            ms(self.first_chunk),
            ms(self.target_chunk),
            self.reset,
            self.snapshots
        )
    }
}

// ---------------------------------------------------------------------------
// window: waveform detail through the FFI
// ---------------------------------------------------------------------------

unsafe extern "C" fn never_cancelled(_: *const std::ffi::c_void) -> bool {
    false
}

fn decode_window(path: &str, start: f64, end: f64, points: u32) -> (Duration, usize) {
    let mut len = 0_usize;
    let started = Instant::now();
    // SAFETY: The path outlives the call, `len` is writable, and the
    // callback ignores its context.
    let data = unsafe {
        ferrous::frontend_bridge::ffi::ferrous_ffi_waveform_window_cancellable(
            path.as_ptr(),
            path.len(),
            start,
            end,
            points,
            &raw mut len,
            never_cancelled,
            std::ptr::null(),
        )
    };
    let elapsed = started.elapsed();
    if !data.is_null() {
        // SAFETY: The buffer came from the function above and is freed once.
        unsafe { ferrous::frontend_bridge::ffi::ferrous_ffi_waveform_window_free(data, len) };
    }
    (elapsed, len)
}

fn run_window(options: &Options) -> Result<Fields, String> {
    let path = options
        .file
        .to_str()
        .ok_or("the path is not UTF-8")?
        .to_string();
    let mut fields = Fields::new();
    field(&mut fields, "mode", "\"window\"");
    field(
        &mut fields,
        "file",
        format!("\"{}\"", file_label(&options.file)),
    );
    field(&mut fields, "points", options.points);
    field(&mut fields, "window_s", options.window_seconds);
    let mut results = Vec::new();
    // A cold window at 30 s, the same again (cached), the next window as
    // playback would ask for it, and windows after seeks.
    // With --duration, the whole track first and again (summaries cached),
    // and once more after the 10 s windows.
    let mut windows: Vec<(f64, f64)> = [
        30.0,
        30.0,
        30.0 + options.window_seconds,
        150.0,
        90.0,
        210.0,
    ]
    .iter()
    .map(|&start| (start, start + options.window_seconds))
    .collect();
    if options.duration > 0.0 {
        windows.insert(0, (0.0, options.duration));
        windows.insert(1, (0.0, options.duration));
        windows.push((0.0, options.duration));
    }
    for (index, &(start, end)) in windows.iter().enumerate() {
        let (elapsed, len) = decode_window(&path, start, end, options.points);
        results.push(format!(
            "{{\"i\":{index},\"start_s\":{start:.1},\"end_s\":{end:.1},\"ms\":{},\"bytes\":{len}}}",
            ms(Some(elapsed))
        ));
    }
    field(&mut fields, "windows", format!("[{}]", results.join(",")));
    Ok(fields)
}
