// Temporary pedantic-lint baseline so strict clippy can be part of regular checks.
// Keep this list shrinking over time; see docs/ROADMAP.md quality/performance section.
#![allow(
    clippy::assigning_clones,
    clippy::cast_lossless,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::match_same_arms,
    clippy::missing_safety_doc,
    clippy::must_use_candidate,
    clippy::needless_pass_by_value,
    clippy::needless_range_loop,
    clippy::ptr_arg,
    clippy::question_mark,
    clippy::result_large_err,
    clippy::semicolon_if_nothing_returned,
    clippy::similar_names,
    clippy::single_match_else,
    clippy::struct_excessive_bools,
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::type_complexity,
    clippy::uninlined_format_args
)]

use std::collections::HashMap;
use std::io::{self, BufRead, Write};
#[cfg(unix)]
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, unbounded, Sender, TrySendError};
use ferrous::frontend_bridge::{
    library_tree::build_library_tree_json, BridgeCommand, BridgeEvent, BridgeLibraryCommand,
    BridgePlaybackCommand, BridgeQueueCommand, BridgeSettingsCommand, BridgeSnapshot,
    FrontendBridgeHandle, LibrarySortMode,
};
use ferrous::playback::RepeatMode;
use serde::Deserialize;
use serde_json::json;
use tracing_subscriber::{fmt, EnvFilter};

fn main() {
    fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .without_time()
        .init();

    let bridge = FrontendBridgeHandle::spawn();
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--json-bridge") {
        run_json_bridge(bridge);
    } else {
        run_interactive_cli(bridge);
    }
}

fn run_interactive_cli(bridge: FrontendBridgeHandle) {
    println!("Ferrous UI bootstrap");
    println!(
        "Commands: play, pause, stop, next, prev, vol <0..1>, seek <secs>, dbrange <50..120>, log <0|1>, repeat <0|1|2>, shuffle <0|1>, snap, quit"
    );

    loop {
        print!("ui> ");
        let _ = io::stdout().flush();

        let mut line = String::new();
        if io::stdin().read_line(&mut line).is_err() {
            eprintln!("failed to read input");
            continue;
        }
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        if line == "quit" || line == "exit" {
            bridge.command(BridgeCommand::Shutdown);
            break;
        }

        if line == "play" {
            bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Play));
        } else if line == "pause" {
            bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Pause));
        } else if line == "stop" {
            bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Stop));
        } else if line == "next" {
            bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Next));
        } else if line == "prev" {
            bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Previous));
        } else if let Some(rest) = line.strip_prefix("vol ") {
            match rest.parse::<f32>() {
                Ok(v) => {
                    bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(v)))
                }
                Err(_) => eprintln!("invalid volume value"),
            }
        } else if let Some(rest) = line.strip_prefix("seek ") {
            match rest.parse::<f64>() {
                Ok(seconds) if seconds >= 0.0 => bridge.command(BridgeCommand::Playback(
                    BridgePlaybackCommand::Seek(Duration::from_secs_f64(seconds)),
                )),
                _ => eprintln!("invalid seek value"),
            }
        } else if let Some(rest) = line.strip_prefix("dbrange ") {
            match rest.parse::<f64>() {
                Ok(value) if value.is_finite() => bridge.command(BridgeCommand::Settings(
                    BridgeSettingsCommand::SetDbRange(value as f32),
                )),
                _ => eprintln!("invalid dbrange value"),
            }
        } else if let Some(rest) = line.strip_prefix("log ") {
            match rest.parse::<i32>() {
                Ok(value) => bridge.command(BridgeCommand::Settings(
                    BridgeSettingsCommand::SetLogScale(value != 0),
                )),
                Err(_) => eprintln!("invalid log value, expected 0 or 1"),
            }
        } else if let Some(rest) = line.strip_prefix("repeat ") {
            match rest.parse::<i32>() {
                Ok(value) => {
                    let mode = match value {
                        1 => RepeatMode::One,
                        2 => RepeatMode::All,
                        _ => RepeatMode::Off,
                    };
                    bridge.command(BridgeCommand::Playback(
                        BridgePlaybackCommand::SetRepeatMode(mode),
                    ));
                }
                Err(_) => eprintln!("invalid repeat value, expected 0, 1, or 2"),
            }
        } else if let Some(rest) = line.strip_prefix("shuffle ") {
            match rest.parse::<i32>() {
                Ok(value) => bridge.command(BridgeCommand::Playback(
                    BridgePlaybackCommand::SetShuffle(value != 0),
                )),
                Err(_) => eprintln!("invalid shuffle value, expected 0 or 1"),
            }
        } else if let Some(rest) = line.strip_prefix("fps ") {
            match rest.parse::<i32>() {
                Ok(value) => bridge.command(BridgeCommand::Settings(
                    BridgeSettingsCommand::SetShowFps(value != 0),
                )),
                Err(_) => eprintln!("invalid fps value, expected 0 or 1"),
            }
        } else if line == "snap" {
            bridge.command(BridgeCommand::RequestSnapshot);
        } else {
            eprintln!("unknown command");
            continue;
        }

        for _ in 0..8 {
            let Some(event) = bridge.recv_timeout(Duration::from_millis(60)) else {
                break;
            };
            match event {
                BridgeEvent::Snapshot(s) => {
                    println!(
                        "state={:?} pos={}/{} queue={} volume={:.2}",
                        s.playback.state,
                        s.playback.position.as_secs(),
                        s.playback.duration.as_secs(),
                        s.queue.len(),
                        s.playback.volume
                    );
                }
                BridgeEvent::Error(err) => {
                    eprintln!("bridge error: {err}");
                }
                BridgeEvent::Stopped => {
                    println!("bridge stopped");
                    return;
                }
            }
        }
    }
}

fn run_json_bridge(bridge: FrontendBridgeHandle) {
    enum InputMsg {
        Line(String),
        Eof,
    }

    let (input_tx, input_rx) = unbounded::<InputMsg>();
    std::thread::spawn(move || {
        let stdin = io::stdin();
        let reader = io::BufReader::new(stdin.lock());
        for line in reader.lines() {
            let Ok(line) = line else {
                break;
            };
            if input_tx.send(InputMsg::Line(line)).is_err() {
                return;
            }
        }
        let _ = input_tx.send(InputMsg::Eof);
    });

    let dropped_counter = Arc::new(AtomicUsize::new(0));
    let out_tx = spawn_json_writer(
        std::env::var_os("FERROUS_PROFILE").is_some(),
        dropped_counter.clone(),
    );
    #[cfg(unix)]
    let mut analysis_writer = AnalysisSocketWriter::from_env();
    #[cfg(not(unix))]
    let mut analysis_writer: Option<()> = None;
    #[cfg(unix)]
    let mut analysis_profile_last = Instant::now();

    let mut emit_state = JsonEmitState {
        profile_enabled: std::env::var_os("FERROUS_PROFILE").is_some(),
        dropped_counter,
        ..JsonEmitState::default()
    };
    bridge.command(BridgeCommand::RequestSnapshot);
    drain_bridge_events_as_json(
        &bridge,
        &out_tx,
        32,
        Duration::from_millis(1),
        &mut emit_state,
        &mut analysis_writer,
    );

    let mut eof_seen = false;
    loop {
        while let Ok(msg) = input_rx.try_recv() {
            match msg {
                InputMsg::Line(line) => {
                    let line = line.trim();
                    if line.is_empty() {
                        continue;
                    }
                    match parse_json_command(line) {
                        Ok(Some(cmd)) => {
                            if matches!(cmd, BridgeCommand::Shutdown) {
                                bridge.command(BridgeCommand::Shutdown);
                                let _ = emit_json_line(
                                    &json!({ "event": "stopped" }),
                                    &out_tx,
                                    &mut emit_state,
                                );
                                return;
                            }
                            bridge.command(cmd);
                        }
                        Ok(None) => {}
                        Err(err) => {
                            let _ = emit_json_line(
                                &json!({ "event": "error", "message": err }),
                                &out_tx,
                                &mut emit_state,
                            );
                        }
                    }
                }
                InputMsg::Eof => {
                    eof_seen = true;
                }
            }
        }

        drain_bridge_events_as_json(
            &bridge,
            &out_tx,
            64,
            Duration::from_millis(1),
            &mut emit_state,
            &mut analysis_writer,
        );

        if eof_seen {
            bridge.command(BridgeCommand::Shutdown);
            let _ = emit_json_line(&json!({ "event": "stopped" }), &out_tx, &mut emit_state);
            return;
        }

        #[cfg(unix)]
        if emit_state.profile_enabled && analysis_profile_last.elapsed() >= Duration::from_secs(1) {
            if let Some(writer) = analysis_writer.as_ref() {
                let (enqueued, dropped) = writer.take_counters();
                eprintln!(
                    "[analysis-sock] frames/s={} dropped/s={}",
                    enqueued, dropped
                );
            }
            analysis_profile_last = Instant::now();
        }

        std::thread::sleep(Duration::from_millis(8));
    }
}

#[derive(Debug, Deserialize)]
struct JsonCommand {
    cmd: String,
    value: Option<f64>,
    from: Option<f64>,
    to: Option<f64>,
    paths: Option<Vec<String>>,
    path: Option<String>,
    artist: Option<String>,
    album: Option<String>,
}

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_RESET: u8 = 0x02;
const ANALYSIS_FLAG_SPECTROGRAM: u8 = 0x04;

#[derive(Default)]
struct AnalysisDelta {
    sample_rate_hz: u32,
    frame_seq: u32,
    spectrogram_seq: u64,
    spectrogram_reset: bool,
    waveform_len: usize,
    waveform_changed: bool,
    waveform_peaks_u8: Vec<u8>,
    spectrogram_rows_u8: Vec<Vec<u8>>,
}

#[cfg(unix)]
struct AnalysisSocketWriter {
    tx: Sender<Vec<u8>>,
    enqueued_counter: Arc<AtomicUsize>,
    dropped_counter: Arc<AtomicUsize>,
}

#[cfg(unix)]
impl AnalysisSocketWriter {
    fn from_env() -> Option<Self> {
        let path = std::env::var("FERROUS_ANALYSIS_SOCKET_PATH").ok()?;
        let stream = UnixStream::connect(path).ok()?;
        let (tx, rx) = bounded::<Vec<u8>>(32);
        let enqueued_counter = Arc::new(AtomicUsize::new(0));
        let dropped_counter = Arc::new(AtomicUsize::new(0));
        std::thread::spawn(move || {
            let mut stream = stream;
            while let Ok(frame) = rx.recv() {
                if stream.write_all(&frame).is_err() {
                    break;
                }
            }
        });
        Some(Self {
            tx,
            enqueued_counter,
            dropped_counter,
        })
    }

    fn send(&self, frame: Vec<u8>) -> bool {
        if frame.is_empty() {
            return true;
        }
        match self.tx.try_send(frame) {
            Ok(()) => {
                self.enqueued_counter.fetch_add(1, Ordering::Relaxed);
                true
            }
            Err(TrySendError::Full(_)) => {
                self.dropped_counter.fetch_add(1, Ordering::Relaxed);
                true
            }
            Err(TrySendError::Disconnected(_)) => false,
        }
    }

    fn take_counters(&self) -> (usize, usize) {
        (
            self.enqueued_counter.swap(0, Ordering::Relaxed),
            self.dropped_counter.swap(0, Ordering::Relaxed),
        )
    }
}

#[derive(Default)]
struct JsonEmitState {
    last_waveform_peaks: Vec<f32>,
    last_library_digest: Option<LibraryDigest>,
    last_emitted_library_tree_digest: Option<LibraryDigest>,
    last_library_tree_emit_at: Option<Instant>,
    last_queue_digest: Option<QueueDigest>,
    last_queue_total_duration_secs: f64,
    last_queue_unknown_duration_count: usize,
    last_spectrogram_seq: u64,
    analysis_frame_seq: u32,
    profile_enabled: bool,
    dropped_counter: Arc<AtomicUsize>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LibraryDigest {
    roots_len: usize,
    tracks_len: usize,
    sort_mode: i32,
    first_root: Option<String>,
    last_root: Option<String>,
    first_track: Option<String>,
    last_track: Option<String>,
}

fn scan_tree_emit_interval() -> Duration {
    static INTERVAL: OnceLock<Duration> = OnceLock::new();
    *INTERVAL.get_or_init(|| {
        let ms = std::env::var("FERROUS_LIBRARY_TREE_EMIT_MS")
            .ok()
            .and_then(|raw| raw.parse::<u64>().ok())
            .map_or(1000, |v| v.clamp(100, 5000));
        Duration::from_millis(ms)
    })
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct QueueDigest {
    len: usize,
    selected: Option<usize>,
    first: Option<String>,
    last: Option<String>,
}

fn compute_queue_total_duration(snapshot: &BridgeSnapshot) -> (f64, usize) {
    let mut duration_by_path: HashMap<&std::path::Path, f64> =
        HashMap::with_capacity(snapshot.library.tracks.len());
    for track in &snapshot.library.tracks {
        let Some(duration_secs) = track.duration_secs else {
            continue;
        };
        let duration = f64::from(duration_secs);
        if duration.is_finite() && duration > 0.0 {
            duration_by_path.insert(track.path.as_path(), duration);
        }
    }

    let mut total_duration_secs = 0.0;
    let mut unknown_duration_count = 0usize;
    for path in &snapshot.queue {
        if let Some(duration) = duration_by_path.get(path.as_path()) {
            total_duration_secs += *duration;
        } else {
            unknown_duration_count = unknown_duration_count.saturating_add(1);
        }
    }

    (total_duration_secs, unknown_duration_count)
}

fn parse_json_command(line: &str) -> Result<Option<BridgeCommand>, String> {
    let parsed: JsonCommand =
        serde_json::from_str(line).map_err(|err| format!("invalid json command: {err}"))?;

    let out = match parsed.cmd.as_str() {
        "play" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Play)),
        "pause" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Pause)),
        "stop" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Stop)),
        "next" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Next)),
        "prev" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Previous)),
        "set_volume" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_volume requires numeric field 'value'".to_string())?;
            Some(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(
                value as f32,
            )))
        }
        "seek" => {
            let value = parsed
                .value
                .ok_or_else(|| "seek requires numeric field 'value'".to_string())?;
            if value < 0.0 {
                return Err("seek value must be >= 0".to_string());
            }
            Some(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
                Duration::from_secs_f64(value),
            )))
        }
        "set_db_range" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_db_range requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_db_range value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(
                value as f32,
            )))
        }
        "set_log_scale" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_log_scale requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_log_scale value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(
                value != 0.0,
            )))
        }
        "set_repeat_mode" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_repeat_mode requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_repeat_mode value must be a finite number".to_string());
            }
            let mode = match value as i32 {
                1 => RepeatMode::One,
                2 => RepeatMode::All,
                _ => RepeatMode::Off,
            };
            Some(BridgeCommand::Playback(
                BridgePlaybackCommand::SetRepeatMode(mode),
            ))
        }
        "set_shuffle" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_shuffle requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_shuffle value must be a finite number".to_string());
            }
            Some(BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(
                value != 0.0,
            )))
        }
        "set_show_fps" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_show_fps requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_show_fps value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(
                value != 0.0,
            )))
        }
        "set_library_sort_mode" => {
            let value = parsed.value.ok_or_else(|| {
                "set_library_sort_mode requires numeric field 'value'".to_string()
            })?;
            if !value.is_finite() {
                return Err("set_library_sort_mode value must be a finite number".to_string());
            }
            let mode = match value as i32 {
                1 => LibrarySortMode::Title,
                _ => LibrarySortMode::Year,
            };
            Some(BridgeCommand::Settings(
                BridgeSettingsCommand::SetLibrarySortMode(mode),
            ))
        }
        "play_at" => {
            let value = parsed
                .value
                .ok_or_else(|| "play_at requires numeric field 'value'".to_string())?;
            if value < 0.0 || !value.is_finite() {
                return Err("play_at value must be a non-negative number".to_string());
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::PlayAt(
                value as usize,
            )))
        }
        "select_queue" => {
            let value = parsed
                .value
                .ok_or_else(|| "select_queue requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("select_queue value must be a number".to_string());
            }
            let selected = if value < 0.0 {
                None
            } else {
                Some(value as usize)
            };
            Some(BridgeCommand::Queue(BridgeQueueCommand::Select(selected)))
        }
        "remove_at" => {
            let value = parsed
                .value
                .ok_or_else(|| "remove_at requires numeric field 'value'".to_string())?;
            if value < 0.0 || !value.is_finite() {
                return Err("remove_at value must be a non-negative number".to_string());
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::Remove(
                value as usize,
            )))
        }
        "move_queue" => {
            let from = parsed
                .from
                .ok_or_else(|| "move_queue requires numeric field 'from'".to_string())?;
            let to = parsed
                .to
                .ok_or_else(|| "move_queue requires numeric field 'to'".to_string())?;
            if !from.is_finite() || !to.is_finite() || from < 0.0 || to < 0.0 {
                return Err(
                    "move_queue fields 'from' and 'to' must be non-negative numbers".to_string(),
                );
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::Move {
                from: from as usize,
                to: to as usize,
            }))
        }
        "replace_album" => {
            let paths = parsed
                .paths
                .ok_or_else(|| "replace_album requires array field 'paths'".to_string())?;
            let items: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceWithAlbum(items),
            ))
        }
        "append_album" => {
            let paths = parsed
                .paths
                .ok_or_else(|| "append_album requires array field 'paths'".to_string())?;
            let items: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
            Some(BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(
                items,
            )))
        }
        "replace_album_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "replace_album_by_key requires string field 'artist'".to_string())?;
            let album = parsed
                .album
                .ok_or_else(|| "replace_album_by_key requires string field 'album'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceAlbumByKey { artist, album },
            ))
        }
        "append_album_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "append_album_by_key requires string field 'artist'".to_string())?;
            let album = parsed
                .album
                .ok_or_else(|| "append_album_by_key requires string field 'album'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::AppendAlbumByKey { artist, album },
            ))
        }
        "replace_artist_by_key" => {
            let artist = parsed.artist.ok_or_else(|| {
                "replace_artist_by_key requires string field 'artist'".to_string()
            })?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceArtistByKey { artist },
            ))
        }
        "append_artist_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "append_artist_by_key requires string field 'artist'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::AppendArtistByKey { artist },
            ))
        }
        "add_track" => {
            let path = parsed
                .path
                .ok_or_else(|| "add_track requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::AddTrack(
                PathBuf::from(path),
            )))
        }
        "play_track" => {
            let path = parsed
                .path
                .ok_or_else(|| "play_track requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(
                PathBuf::from(path),
            )))
        }
        "scan_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "scan_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::AddRoot(
                PathBuf::from(path),
            )))
        }
        "add_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "add_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::AddRoot(
                PathBuf::from(path),
            )))
        }
        "remove_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "remove_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::RemoveRoot(
                PathBuf::from(path),
            )))
        }
        "rescan_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "rescan_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::RescanRoot(
                PathBuf::from(path),
            )))
        }
        "rescan_all" => Some(BridgeCommand::Library(BridgeLibraryCommand::RescanAll)),
        "clear_queue" => Some(BridgeCommand::Queue(BridgeQueueCommand::Clear)),
        "request_snapshot" => Some(BridgeCommand::RequestSnapshot),
        "shutdown" => Some(BridgeCommand::Shutdown),
        _ => return Err(format!("unknown command '{}'", parsed.cmd)),
    };
    Ok(out)
}

fn drain_bridge_events_as_json(
    bridge: &FrontendBridgeHandle,
    out_tx: &Sender<Vec<u8>>,
    max_events: usize,
    timeout: Duration,
    emit_state: &mut JsonEmitState,
    #[cfg(unix)] analysis_writer: &mut Option<AnalysisSocketWriter>,
    #[cfg(not(unix))] _analysis_writer: &mut Option<()>,
) {
    let mut latest_snapshot: Option<BridgeSnapshot> = None;
    for i in 0..max_events {
        let event = if i == 0 {
            bridge.recv_timeout(timeout)
        } else {
            bridge.try_recv()
        };
        let Some(event) = event else {
            break;
        };
        match event {
            BridgeEvent::Snapshot(s) => latest_snapshot = Some(*s),
            BridgeEvent::Error(message) => {
                let _ = emit_json_line(
                    &json!({ "event": "error", "message": message }),
                    out_tx,
                    emit_state,
                );
            }
            BridgeEvent::Stopped => {
                let _ = emit_json_line(&json!({ "event": "stopped" }), out_tx, emit_state);
                return;
            }
        }
    }

    if let Some(s) = latest_snapshot {
        let analysis_delta = compute_analysis_delta(&s, emit_state);
        #[cfg(unix)]
        let analysis_on_socket = {
            let frame = encode_analysis_frame(&analysis_delta);
            let mut connected = false;
            let mut drop_writer = false;
            if let Some(writer) = analysis_writer.as_ref() {
                connected = writer.send(frame);
                if !connected {
                    drop_writer = true;
                }
            }
            if drop_writer {
                *analysis_writer = None;
            }
            connected
        };
        #[cfg(not(unix))]
        let analysis_on_socket = false;
        let payload = encode_snapshot_payload(&s, &analysis_delta, emit_state, !analysis_on_socket);
        let _ = emit_json_line(&payload, out_tx, emit_state);
    }
}

fn encode_snapshot_payload(
    s: &BridgeSnapshot,
    analysis_delta: &AnalysisDelta,
    emit_state: &mut JsonEmitState,
    include_analysis_payload: bool,
) -> serde_json::Value {
    let sort_mode_value = s.settings.library_sort_mode.to_i32();
    let library_digest = LibraryDigest {
        roots_len: s.library.roots.len(),
        tracks_len: s.library.tracks.len(),
        sort_mode: sort_mode_value,
        first_root: s
            .library
            .roots
            .first()
            .map(|p| p.to_string_lossy().to_string()),
        last_root: s
            .library
            .roots
            .last()
            .map(|p| p.to_string_lossy().to_string()),
        first_track: s
            .library
            .tracks
            .first()
            .map(|t| t.path.to_string_lossy().to_string()),
        last_track: s
            .library
            .tracks
            .last()
            .map(|t| t.path.to_string_lossy().to_string()),
    };
    let tree_changed = emit_state.last_library_digest.as_ref() != Some(&library_digest);
    let tree_changed_since_last_emit =
        emit_state.last_emitted_library_tree_digest.as_ref() != Some(&library_digest);
    let is_first_tree_emit = emit_state.last_emitted_library_tree_digest.is_none();
    let scan_emit_due = emit_state
        .last_library_tree_emit_at
        .map_or(true, |last| last.elapsed() >= scan_tree_emit_interval());
    let should_emit_tree = tree_changed_since_last_emit
        && (!s.library.scan_in_progress || is_first_tree_emit || scan_emit_due);
    emit_state.last_library_digest = Some(library_digest.clone());
    if should_emit_tree {
        emit_state.last_emitted_library_tree_digest = Some(library_digest.clone());
        emit_state.last_library_tree_emit_at = Some(Instant::now());
    }
    let library_tree = if should_emit_tree {
        build_library_tree_json(&s.library, s.settings.library_sort_mode)
    } else {
        serde_json::Value::Null
    };

    let queue_digest = QueueDigest {
        len: s.queue.len(),
        selected: s.selected_queue_index,
        first: s.queue.first().map(|p| p.to_string_lossy().to_string()),
        last: s.queue.last().map(|p| p.to_string_lossy().to_string()),
    };
    let queue_changed = emit_state.last_queue_digest.as_ref() != Some(&queue_digest);
    let queue_tracks = if queue_changed {
        emit_state.last_queue_digest = Some(queue_digest);
        serde_json::Value::Array(
            s.queue
                .iter()
                .map(|path| {
                    let title = path.file_name().map_or_else(
                        || path.to_string_lossy().into_owned(),
                        |n| n.to_string_lossy().into_owned(),
                    );
                    json!({
                        "path": path.to_string_lossy().to_string(),
                        "title": title,
                    })
                })
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };
    let (queue_total_duration_secs, queue_unknown_duration_count) = if queue_changed || tree_changed
    {
        let (total_duration_secs, unknown_duration_count) = compute_queue_total_duration(s);
        emit_state.last_queue_total_duration_secs = total_duration_secs;
        emit_state.last_queue_unknown_duration_count = unknown_duration_count;
        (total_duration_secs, unknown_duration_count)
    } else {
        (
            emit_state.last_queue_total_duration_secs,
            emit_state.last_queue_unknown_duration_count,
        )
    };

    let waveform_peaks = if include_analysis_payload && analysis_delta.waveform_changed {
        serde_json::Value::Array(
            analysis_delta
                .waveform_peaks_u8
                .iter()
                .map(|v| json!((*v as f64) / 255.0))
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };
    let spectrogram_rows =
        if include_analysis_payload && !analysis_delta.spectrogram_rows_u8.is_empty() {
            serde_json::Value::Array(
                analysis_delta
                    .spectrogram_rows_u8
                    .iter()
                    .map(|row| serde_json::Value::Array(row.iter().map(|v| json!(v)).collect()))
                    .collect(),
            )
        } else {
            serde_json::Value::Null
        };
    let current_queue_index = s
        .playback
        .current_queue_index
        .filter(|idx| *idx < s.queue.len());

    json!({
        "event": "snapshot",
        "playback": {
            "state": format!("{:?}", s.playback.state),
            "position_secs": s.playback.position.as_secs_f64(),
            "duration_secs": s.playback.duration.as_secs_f64(),
            "volume": s.playback.volume,
            "repeat_mode": match s.playback.repeat_mode {
                RepeatMode::Off => 0,
                RepeatMode::One => 1,
                RepeatMode::All => 2,
            },
            "shuffle_enabled": s.playback.shuffle_enabled,
            "has_current": s.playback.current.is_some(),
            "current_path": s.playback.current.as_ref().map(|path| path.to_string_lossy().to_string()),
            "current_queue_index": current_queue_index,
        },
        "queue": {
            "len": s.queue.len(),
            "selected_index": s.selected_queue_index,
            "total_duration_secs": queue_total_duration_secs,
            "unknown_duration_count": queue_unknown_duration_count,
            "tracks": queue_tracks,
        },
        "library": {
            "roots": s.library.roots.len(),
            "root_paths": s.library.roots.iter().map(|p| p.to_string_lossy().to_string()).collect::<Vec<_>>(),
            "tracks": s.library.tracks.len(),
            "scan_in_progress": s.library.scan_in_progress,
            "tree_changed": should_emit_tree,
            "tree": library_tree,
            "sort_mode": sort_mode_value,
            "last_error": s.library.last_error.clone(),
            "progress": {
                "current_root": s
                    .library
                    .scan_progress
                    .as_ref()
                    .and_then(|p| p.current_root.as_ref())
                    .map(|p| p.to_string_lossy().to_string()),
                "roots_completed": s
                    .library
                    .scan_progress
                    .as_ref()
                    .map_or(0, |p| p.roots_completed),
                "roots_total": s
                    .library
                    .scan_progress
                    .as_ref()
                    .map_or(0, |p| p.roots_total),
                "supported_files_discovered": s
                    .library
                    .scan_progress
                    .as_ref()
                    .map_or(0, |p| p.supported_files_discovered),
                "supported_files_processed": s
                    .library
                    .scan_progress
                    .as_ref()
                    .map_or(0, |p| p.supported_files_processed),
                "files_per_second": s
                    .library
                    .scan_progress
                    .as_ref()
                    .and_then(|p| p.files_per_second),
                "eta_seconds": s
                    .library
                    .scan_progress
                    .as_ref()
                    .and_then(|p| p.eta_seconds),
            },
        },
        "metadata": {
            "source_path": s.metadata.source_path.clone(),
            "title": s.metadata.title.clone(),
            "artist": s.metadata.artist.clone(),
            "album": s.metadata.album.clone(),
            "sample_rate_hz": s.metadata.sample_rate_hz,
            "bitrate_kbps": s.metadata.bitrate_kbps,
            "channels": s.metadata.channels,
            "bit_depth": s.metadata.bit_depth,
            "cover_path": s.metadata.cover_art_path.clone(),
        },
        "analysis": {
            "spectrogram_seq": analysis_delta.spectrogram_seq,
            "spectrogram_reset": include_analysis_payload && analysis_delta.spectrogram_reset,
            "spectrogram_rows": spectrogram_rows,
            "sample_rate_hz": if include_analysis_payload { analysis_delta.sample_rate_hz } else { 0 },
            "waveform_len": if include_analysis_payload { analysis_delta.waveform_len } else { 0 },
            "waveform_changed": include_analysis_payload && analysis_delta.waveform_changed,
            "waveform_peaks": waveform_peaks,
        },
        "settings": {
            "volume": s.settings.volume,
            "fft_size": s.settings.fft_size,
            "db_range": s.settings.db_range,
            "log_scale": s.settings.log_scale,
            "show_fps": s.settings.show_fps,
            "library_sort_mode": sort_mode_value,
        }
    })
}

fn compute_analysis_delta(s: &BridgeSnapshot, emit_state: &mut JsonEmitState) -> AnalysisDelta {
    let waveform_changed = s.analysis.waveform_peaks != emit_state.last_waveform_peaks;
    let waveform_peaks_u8 = if waveform_changed {
        emit_state.last_waveform_peaks = s.analysis.waveform_peaks.clone();
        downsample_waveform_peaks(&s.analysis.waveform_peaks, 1024)
            .into_iter()
            .map(to_u8_norm)
            .collect()
    } else {
        Vec::new()
    };

    let spectrogram_reset = s.analysis.spectrogram_seq < emit_state.last_spectrogram_seq
        || (s.analysis.spectrogram_seq == 0
            && s.analysis.spectrogram_rows.is_empty()
            && emit_state.last_spectrogram_seq > 0);
    let spectrogram_seq = s.analysis.spectrogram_seq;
    let spectrogram_delta =
        spectrogram_seq.saturating_sub(emit_state.last_spectrogram_seq) as usize;
    let spectrogram_rows_u8 = if spectrogram_delta > 0 && !s.analysis.spectrogram_rows.is_empty() {
        let tail = spectrogram_delta.min(s.analysis.spectrogram_rows.len());
        let start = s.analysis.spectrogram_rows.len().saturating_sub(tail);
        s.analysis.spectrogram_rows[start..]
            .iter()
            .map(|row| {
                row.iter()
                    .map(|v| to_u8_spectrum(*v, s.settings.db_range))
                    .collect::<Vec<u8>>()
            })
            .collect()
    } else {
        Vec::new()
    };
    emit_state.last_spectrogram_seq = spectrogram_seq;
    let has_payload = waveform_changed || spectrogram_reset || !spectrogram_rows_u8.is_empty();
    if has_payload {
        emit_state.analysis_frame_seq = emit_state.analysis_frame_seq.wrapping_add(1);
    }

    AnalysisDelta {
        sample_rate_hz: s.analysis.sample_rate_hz,
        frame_seq: emit_state.analysis_frame_seq,
        spectrogram_seq,
        spectrogram_reset,
        waveform_len: s.analysis.waveform_peaks.len(),
        waveform_changed,
        waveform_peaks_u8,
        spectrogram_rows_u8,
    }
}

fn to_u8_norm(v: f32) -> u8 {
    let clamped = v.clamp(0.0, 1.0);
    (clamped * 255.0).round() as u8
}

fn to_u8_spectrum(v: f32, db_range: f32) -> u8 {
    let range = db_range.clamp(50.0, 120.0) as f64;
    let db = if v > 0.0 {
        (10.0 / std::f64::consts::LN_10) * (v as f64).ln()
    } else {
        -200.0
    };
    let xdb = (db + range - 63.0).clamp(0.0, range);
    ((xdb / range) * 255.0).round().clamp(0.0, 255.0) as u8
}

fn encode_analysis_frame(delta: &AnalysisDelta) -> Vec<u8> {
    let waveform_len = delta.waveform_peaks_u8.len();
    let row_count = delta.spectrogram_rows_u8.len();
    let bin_count = delta
        .spectrogram_rows_u8
        .first()
        .map_or(0, std::vec::Vec::len);
    let has_spectrogram = row_count > 0 && bin_count > 0;

    let mut flags = 0u8;
    if delta.waveform_changed && waveform_len > 0 {
        flags |= ANALYSIS_FLAG_WAVEFORM;
    }
    if delta.spectrogram_reset {
        flags |= ANALYSIS_FLAG_RESET;
    }
    if has_spectrogram {
        flags |= ANALYSIS_FLAG_SPECTROGRAM;
    }

    if flags == 0 {
        return Vec::new();
    }

    let waveform_len_u16 = waveform_len.min(u16::MAX as usize) as u16;
    let row_count_u16 = row_count.min(u16::MAX as usize) as u16;
    let bin_count_u16 = bin_count.min(u16::MAX as usize) as u16;
    let spectrogram_bytes = row_count_u16 as usize * bin_count_u16 as usize;
    let payload_len = 16usize + waveform_len_u16 as usize + spectrogram_bytes;

    let mut out = Vec::with_capacity(4 + payload_len);
    out.extend_from_slice(&(payload_len as u32).to_le_bytes());
    out.push(ANALYSIS_FRAME_MAGIC);
    out.extend_from_slice(&delta.sample_rate_hz.to_le_bytes());
    out.push(flags);
    out.extend_from_slice(&waveform_len_u16.to_le_bytes());
    out.extend_from_slice(&row_count_u16.to_le_bytes());
    out.extend_from_slice(&bin_count_u16.to_le_bytes());
    out.extend_from_slice(&delta.frame_seq.to_le_bytes());

    if (flags & ANALYSIS_FLAG_WAVEFORM) != 0 {
        out.extend_from_slice(&delta.waveform_peaks_u8[..waveform_len_u16 as usize]);
    }
    if (flags & ANALYSIS_FLAG_SPECTROGRAM) != 0 {
        for row in delta
            .spectrogram_rows_u8
            .iter()
            .take(row_count_u16 as usize)
        {
            out.extend_from_slice(&row[..bin_count_u16 as usize]);
        }
    }

    out
}

fn emit_json_line(
    payload: &serde_json::Value,
    out_tx: &Sender<Vec<u8>>,
    emit_state: &mut JsonEmitState,
) -> io::Result<()> {
    let bytes = serde_json::to_vec(payload)?;
    match out_tx.try_send(bytes) {
        Ok(()) => Ok(()),
        Err(TrySendError::Full(_)) => {
            if emit_state.profile_enabled {
                emit_state.dropped_counter.fetch_add(1, Ordering::Relaxed);
            }
            Ok(())
        }
        Err(TrySendError::Disconnected(_)) => Err(io::Error::new(
            io::ErrorKind::BrokenPipe,
            "json writer disconnected",
        )),
    }
}

fn spawn_json_writer(profile_enabled: bool, dropped_counter: Arc<AtomicUsize>) -> Sender<Vec<u8>> {
    let (tx, rx) = bounded::<Vec<u8>>(32);
    std::thread::spawn(move || {
        let stdout = io::stdout();
        let mut out = stdout.lock();
        let mut snaps = 0usize;
        let mut bytes = 0usize;
        let mut max_payload = 0usize;
        let mut max_write_ms = 0.0f64;
        let mut last_report = Instant::now();
        while let Ok(line) = rx.recv() {
            let started = Instant::now();
            if out.write_all(&line).is_err() {
                break;
            }
            if out.write_all(b"\n").is_err() {
                break;
            }
            if out.flush().is_err() {
                break;
            }
            if profile_enabled {
                snaps = snaps.saturating_add(1);
                bytes = bytes.saturating_add(line.len());
                max_payload = max_payload.max(line.len());
                let write_ms = started.elapsed().as_secs_f64() * 1000.0;
                max_write_ms = max_write_ms.max(write_ms);
                if last_report.elapsed() >= Duration::from_secs(1) {
                    let dropped = dropped_counter.swap(0, Ordering::Relaxed);
                    eprintln!(
                        "[bridge-json] snaps/s={} bytes/s={} max_payload={}B max_write_ms={:.2} dropped/s={}",
                        snaps, bytes, max_payload, max_write_ms, dropped
                    );
                    snaps = 0;
                    bytes = 0;
                    max_payload = 0;
                    max_write_ms = 0.0;
                    last_report = Instant::now();
                }
            }
        }
    });
    tx
}

fn downsample_waveform_peaks(peaks: &[f32], max_points: usize) -> Vec<f32> {
    if peaks.len() <= max_points || max_points == 0 {
        return peaks.to_vec();
    }
    let mut out = Vec::with_capacity(max_points);
    for i in 0..max_points {
        let start = i * peaks.len() / max_points;
        let mut end = (i + 1) * peaks.len() / max_points;
        if end <= start {
            end = (start + 1).min(peaks.len());
        }
        let mut peak = 0.0f32;
        for &v in &peaks[start..end] {
            if v > peak {
                peak = v;
            }
        }
        out.push(peak);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use ferrous::frontend_bridge::ffi::{
        ferrous_ffi_bridge_create, ferrous_ffi_bridge_destroy, ferrous_ffi_bridge_free_json_event,
        ferrous_ffi_bridge_poll, ferrous_ffi_bridge_pop_json_event, ferrous_ffi_bridge_send_json,
    };
    use std::ffi::{CStr, CString};
    use std::sync::{Mutex, MutexGuard, OnceLock};
    use std::time::Instant;

    fn test_guard() -> MutexGuard<'static, ()> {
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn wait_bridge_snapshot(
        bridge: &FrontendBridgeHandle,
        timeout: Duration,
    ) -> Option<BridgeSnapshot> {
        let deadline = Instant::now() + timeout;
        let mut last = None;
        while Instant::now() < deadline {
            if let Some(event) = bridge.recv_timeout(Duration::from_millis(30)) {
                if let BridgeEvent::Snapshot(s) = event {
                    last = Some(*s);
                }
            }
        }
        last
    }

    fn wait_bridge_snapshot_matching<F>(
        bridge: &FrontendBridgeHandle,
        timeout: Duration,
        predicate: F,
    ) -> Option<BridgeSnapshot>
    where
        F: Fn(&BridgeSnapshot) -> bool,
    {
        let deadline = Instant::now() + timeout;
        let mut last = None;
        while Instant::now() < deadline {
            if let Some(event) = bridge.recv_timeout(Duration::from_millis(30)) {
                if let BridgeEvent::Snapshot(snapshot) = event {
                    if predicate(&snapshot) {
                        return Some(*snapshot);
                    }
                    last = Some(*snapshot);
                }
            }
        }
        last
    }

    fn wait_bridge_error_message(
        bridge: &FrontendBridgeHandle,
        timeout: Duration,
    ) -> Option<String> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Some(event) = bridge.recv_timeout(Duration::from_millis(30)) {
                if let BridgeEvent::Error(message) = event {
                    return Some(message);
                }
            }
        }
        None
    }

    unsafe fn wait_ffi_json_event_matching<F>(
        handle: *mut ferrous::frontend_bridge::ffi::FerrousFfiBridge,
        timeout: Duration,
        predicate: F,
    ) -> Option<serde_json::Value>
    where
        F: Fn(&serde_json::Value) -> bool,
    {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            unsafe { ferrous_ffi_bridge_poll(handle, 64) };
            loop {
                let ptr = unsafe { ferrous_ffi_bridge_pop_json_event(handle) };
                if ptr.is_null() {
                    break;
                }
                let text = unsafe { CStr::from_ptr(ptr) }
                    .to_string_lossy()
                    .into_owned();
                unsafe { ferrous_ffi_bridge_free_json_event(ptr) };
                if let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) {
                    if predicate(&value) {
                        return Some(value);
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        None
    }

    fn snapshot_queue_len(snapshot: &serde_json::Value) -> u64 {
        snapshot
            .get("queue")
            .and_then(|v| v.get("len"))
            .and_then(|v| v.as_u64())
            .unwrap_or(0)
    }

    fn snapshot_selected_index(snapshot: &serde_json::Value) -> i64 {
        snapshot
            .get("queue")
            .and_then(|v| v.get("selected_index"))
            .and_then(|v| v.as_i64())
            .unwrap_or(-1)
    }

    fn snapshot_tracks(snapshot: &serde_json::Value) -> Vec<String> {
        snapshot
            .get("queue")
            .and_then(|v| v.get("tracks"))
            .and_then(serde_json::Value::as_array)
            .map_or_else(Vec::new, |tracks| {
                tracks
                    .iter()
                    .filter_map(|item| item.get("path").and_then(serde_json::Value::as_str))
                    .map(ToOwned::to_owned)
                    .collect::<Vec<_>>()
            })
    }

    fn snapshot_playback_position_secs(snapshot: &serde_json::Value) -> f64 {
        snapshot
            .get("playback")
            .and_then(|v| v.get("position_secs"))
            .and_then(|v| v.as_f64())
            .unwrap_or(0.0)
    }

    fn snapshot_playback_current_path(snapshot: &serde_json::Value) -> Option<String> {
        snapshot
            .get("playback")
            .and_then(|v| v.get("current_path"))
            .and_then(|v| v.as_str())
            .map(ToOwned::to_owned)
    }

    fn snapshot_playback_state(snapshot: &serde_json::Value) -> Option<String> {
        snapshot
            .get("playback")
            .and_then(|v| v.get("state"))
            .and_then(|v| v.as_str())
            .map(ToOwned::to_owned)
    }

    unsafe fn wait_ffi_snapshot_json(
        handle: *mut ferrous::frontend_bridge::ffi::FerrousFfiBridge,
        timeout: Duration,
        expected_queue_len: u64,
    ) -> Option<serde_json::Value> {
        let deadline = Instant::now() + timeout;
        let mut latest = None;
        while Instant::now() < deadline {
            let candidate = unsafe {
                wait_ffi_json_event_matching(handle, Duration::from_millis(50), |value| {
                    value.get("event").and_then(|v| v.as_str()) == Some("snapshot")
                        && snapshot_queue_len(value) == expected_queue_len
                })
            };
            if let Some(value) = candidate {
                if value.get("event").and_then(|v| v.as_str()) == Some("snapshot") {
                    return Some(value);
                }
                latest = Some(value);
            } else {
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        latest
    }

    fn direct_snapshot_track_paths(snapshot: &BridgeSnapshot) -> Vec<String> {
        snapshot
            .queue
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect::<Vec<_>>()
    }

    #[test]
    fn process_parser_and_ffi_path_match_queue_transition_sequence() {
        let _guard = test_guard();
        let commands = [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac","/tmp/c.flac"]}"#,
            r#"{"cmd":"select_queue","value":2}"#,
            r#"{"cmd":"move_queue","from":2,"to":0}"#,
            r#"{"cmd":"remove_at","value":1}"#,
            r#"{"cmd":"select_queue","value":-1}"#,
            r#"{"cmd":"select_queue","value":0}"#,
        ];

        let direct_bridge = FrontendBridgeHandle::spawn();
        for line in &commands {
            let cmd = parse_json_command(line).expect("parse").expect("cmd");
            direct_bridge.command(cmd);
        }
        direct_bridge.command(BridgeCommand::RequestSnapshot);
        let direct_snapshot =
            wait_bridge_snapshot_matching(&direct_bridge, Duration::from_secs(4), |snapshot| {
                snapshot.queue.len() == 2 && snapshot.selected_queue_index == Some(0)
            })
            .expect("direct snapshot");

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        for line in &commands {
            let cmd = CString::new(*line).expect("cstring");
            assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, cmd.as_ptr()) });
        }

        let ffi_snapshot = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(4), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("snapshot")
                    && snapshot_queue_len(value) == 2
                    && snapshot_selected_index(value) == 0
                    && !snapshot_tracks(value).is_empty()
            })
        }
        .expect("ffi snapshot");

        let direct_tracks = direct_snapshot_track_paths(&direct_snapshot);
        let ffi_tracks = snapshot_tracks(&ffi_snapshot);

        assert_eq!(
            direct_snapshot.queue.len() as u64,
            snapshot_queue_len(&ffi_snapshot)
        );
        assert_eq!(
            direct_snapshot
                .selected_queue_index
                .map_or(-1, |idx| idx as i64),
            snapshot_selected_index(&ffi_snapshot)
        );
        assert_eq!(direct_tracks, ffi_tracks);

        direct_bridge.command(BridgeCommand::Shutdown);
        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_match_invalid_seek_error() {
        let _guard = test_guard();
        let expected = parse_json_command(r#"{"cmd":"seek","value":-1}"#).unwrap_err();

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        let bad = CString::new(r#"{"cmd":"seek","value":-1}"#).expect("cstring");
        assert!(!unsafe { ferrous_ffi_bridge_send_json(ffi_handle, bad.as_ptr()) });

        let error_event = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(3), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("error")
            })
        }
        .expect("error event");
        let message = error_event
            .get("message")
            .and_then(serde_json::Value::as_str)
            .unwrap_or("");
        assert_eq!(message, expected);

        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_match_successful_seek_snapshot() {
        let _guard = test_guard();
        let direct_bridge = FrontendBridgeHandle::spawn();
        for line in [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac"]}"#,
            r#"{"cmd":"seek","value":42.5}"#,
        ] {
            let cmd = parse_json_command(line).expect("parse").expect("cmd");
            direct_bridge.command(cmd);
        }
        direct_bridge.command(BridgeCommand::RequestSnapshot);
        let direct_snapshot =
            wait_bridge_snapshot_matching(&direct_bridge, Duration::from_secs(4), |snapshot| {
                snapshot.queue.len() == 1 && snapshot.playback.current.is_some()
            })
            .expect("direct seek snapshot");
        let direct_current_path = direct_snapshot
            .playback
            .current
            .as_ref()
            .map(|path| path.to_string_lossy().into_owned());

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        for line in [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac"]}"#,
            r#"{"cmd":"seek","value":42.5}"#,
            r#"{"cmd":"request_snapshot"}"#,
        ] {
            let cmd = CString::new(line).expect("cstring");
            assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, cmd.as_ptr()) });
        }
        let ffi_snapshot = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(4), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("snapshot")
                    && snapshot_queue_len(value) == 1
                    && snapshot_playback_current_path(value) == direct_current_path
            })
        }
        .expect("ffi seek snapshot");

        assert_eq!(
            direct_snapshot.queue.len() as u64,
            snapshot_queue_len(&ffi_snapshot)
        );
        assert_eq!(
            direct_snapshot
                .selected_queue_index
                .map_or(-1, |idx| idx as i64),
            snapshot_selected_index(&ffi_snapshot)
        );
        assert_eq!(
            direct_current_path,
            snapshot_playback_current_path(&ffi_snapshot)
        );
        assert!(direct_snapshot.playback.position.as_secs_f64() >= 0.0);
        assert!(snapshot_playback_position_secs(&ffi_snapshot) >= 0.0);

        direct_bridge.command(BridgeCommand::Shutdown);
        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_match_playback_state_sequence() {
        let _guard = test_guard();
        let commands = [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac","/tmp/c.flac"]}"#,
            r#"{"cmd":"pause"}"#,
            r#"{"cmd":"play"}"#,
            r#"{"cmd":"next"}"#,
            r#"{"cmd":"prev"}"#,
        ];

        let direct_bridge = FrontendBridgeHandle::spawn();
        for line in &commands {
            let cmd = parse_json_command(line).expect("parse").expect("cmd");
            direct_bridge.command(cmd);
        }
        direct_bridge.command(BridgeCommand::RequestSnapshot);
        let direct_snapshot =
            wait_bridge_snapshot_matching(&direct_bridge, Duration::from_secs(4), |snapshot| {
                snapshot.queue.len() == 3
                    && snapshot.playback.current.is_some()
                    && snapshot.playback.state == ferrous::playback::PlaybackState::Playing
            })
            .expect("direct playback-state snapshot");
        let direct_current_path = direct_snapshot
            .playback
            .current
            .as_ref()
            .map(|path| path.to_string_lossy().into_owned());
        let direct_state = format!("{:?}", direct_snapshot.playback.state);
        direct_bridge.command(BridgeCommand::Shutdown);
        std::thread::sleep(Duration::from_millis(30));

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        for line in &commands {
            let cmd = CString::new(*line).expect("cstring");
            assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, cmd.as_ptr()) });
        }
        let request = CString::new(r#"{"cmd":"request_snapshot"}"#).expect("cstring");
        assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, request.as_ptr()) });
        let ffi_snapshot = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(4), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("snapshot")
                    && snapshot_queue_len(value) == 3
                    && snapshot_playback_current_path(value).is_some()
                    && snapshot_playback_state(value).as_deref() == Some(direct_state.as_str())
            })
        }
        .expect("ffi playback-state snapshot");

        assert_eq!(
            direct_snapshot.queue.len() as u64,
            snapshot_queue_len(&ffi_snapshot)
        );
        assert_eq!(
            direct_snapshot
                .selected_queue_index
                .map_or(-1, |idx| idx as i64),
            snapshot_selected_index(&ffi_snapshot)
        );
        assert_eq!(
            direct_current_path,
            snapshot_playback_current_path(&ffi_snapshot)
        );
        assert_eq!(Some(direct_state), snapshot_playback_state(&ffi_snapshot));

        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_match_stop_restart_sequence() {
        let _guard = test_guard();
        let commands = [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac"]}"#,
            r#"{"cmd":"play_at","value":1}"#,
            r#"{"cmd":"stop"}"#,
            r#"{"cmd":"play"}"#,
        ];

        let direct_bridge = FrontendBridgeHandle::spawn();
        for line in &commands {
            let cmd = parse_json_command(line).expect("parse").expect("cmd");
            direct_bridge.command(cmd);
        }
        direct_bridge.command(BridgeCommand::RequestSnapshot);
        let direct_snapshot =
            wait_bridge_snapshot_matching(&direct_bridge, Duration::from_secs(4), |snapshot| {
                snapshot.queue.len() == 2
                    && snapshot.playback.current.is_some()
                    && snapshot.playback.state == ferrous::playback::PlaybackState::Playing
            })
            .expect("direct stop/restart snapshot");
        let direct_state = format!("{:?}", direct_snapshot.playback.state);
        direct_bridge.command(BridgeCommand::Shutdown);
        std::thread::sleep(Duration::from_millis(30));

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        for line in &commands {
            let cmd = CString::new(*line).expect("cstring");
            assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, cmd.as_ptr()) });
        }
        let request = CString::new(r#"{"cmd":"request_snapshot"}"#).expect("cstring");
        assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, request.as_ptr()) });
        let ffi_snapshot = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(4), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("snapshot")
                    && snapshot_queue_len(value) == 2
                    && snapshot_playback_state(value).as_deref() == Some(direct_state.as_str())
            })
        }
        .expect("ffi stop/restart snapshot");

        assert_eq!(
            direct_snapshot.queue.len() as u64,
            snapshot_queue_len(&ffi_snapshot)
        );
        assert_eq!(Some(direct_state), snapshot_playback_state(&ffi_snapshot));

        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_match_play_at_out_of_bounds_error() {
        let _guard = test_guard();
        let commands = [
            r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac"]}"#,
            r#"{"cmd":"play_at","value":9}"#,
        ];

        let direct_bridge = FrontendBridgeHandle::spawn();
        for line in &commands {
            let cmd = parse_json_command(line).expect("parse").expect("cmd");
            direct_bridge.command(cmd);
        }
        let direct_error = wait_bridge_error_message(&direct_bridge, Duration::from_secs(4))
            .expect("direct error event");
        assert!(direct_error.contains("queue index 9 out of bounds"));

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        for line in &commands {
            let cmd = CString::new(*line).expect("cstring");
            assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, cmd.as_ptr()) });
        }
        let ffi_error = unsafe {
            wait_ffi_json_event_matching(ffi_handle, Duration::from_secs(4), |value| {
                value.get("event").and_then(|v| v.as_str()) == Some("error")
            })
        }
        .expect("ffi error event");
        let ffi_message = ffi_error
            .get("message")
            .and_then(serde_json::Value::as_str)
            .unwrap_or("");
        assert_eq!(direct_error, ffi_message);

        direct_bridge.command(BridgeCommand::Shutdown);
        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }

    #[test]
    fn process_parser_and_ffi_path_have_matching_queue_outcome() {
        let _guard = test_guard();
        let direct_bridge = FrontendBridgeHandle::spawn();
        let cmd =
            parse_json_command(r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac"]}"#)
                .expect("parse")
                .expect("cmd");
        direct_bridge.command(cmd);
        direct_bridge.command(BridgeCommand::RequestSnapshot);
        let direct_snapshot =
            wait_bridge_snapshot(&direct_bridge, Duration::from_secs(4)).expect("direct snapshot");

        let ffi_handle = ferrous_ffi_bridge_create();
        assert!(!ffi_handle.is_null());
        let line = CString::new(r#"{"cmd":"replace_album","paths":["/tmp/a.flac","/tmp/b.flac"]}"#)
            .expect("cstring");
        assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, line.as_ptr()) });
        let request = CString::new(r#"{"cmd":"request_snapshot"}"#).expect("cstring");
        assert!(unsafe { ferrous_ffi_bridge_send_json(ffi_handle, request.as_ptr()) });
        let ffi_snapshot = unsafe {
            wait_ffi_snapshot_json(
                ffi_handle,
                Duration::from_secs(4),
                direct_snapshot.queue.len() as u64,
            )
        }
        .expect("ffi snapshot");

        let ffi_queue_len = ffi_snapshot
            .get("queue")
            .and_then(|v| v.get("len"))
            .and_then(|v| v.as_u64())
            .unwrap_or(0);
        let ffi_selected = ffi_snapshot
            .get("queue")
            .and_then(|v| v.get("selected_index"))
            .and_then(|v| v.as_i64())
            .unwrap_or(-1);

        assert_eq!(direct_snapshot.queue.len() as u64, ffi_queue_len);
        assert_eq!(
            direct_snapshot
                .selected_queue_index
                .map(|i| i as i64)
                .unwrap_or(-1),
            ffi_selected
        );

        direct_bridge.command(BridgeCommand::Shutdown);
        unsafe { ferrous_ffi_bridge_destroy(ffi_handle) };
    }
}
