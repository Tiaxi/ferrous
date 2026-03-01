use std::collections::BTreeMap;
use std::io::{self, BufRead, Write};
use std::path::PathBuf;
use std::time::Duration;

use ferrous::frontend_bridge::{
    BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    FrontendBridgeHandle,
};
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
    println!("Ferrous native frontend bootstrap");
    println!("Commands: play, pause, stop, next, prev, vol <0..1>, seek <secs>, snap, quit");

    loop {
        print!("native> ");
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
    let mut emit_state = JsonEmitState::default();
    bridge.command(BridgeCommand::RequestSnapshot);
    drain_bridge_events_as_json(&bridge, 16, Duration::from_millis(10), &mut emit_state);

    let stdin = io::stdin();
    let mut reader = io::BufReader::new(stdin.lock());
    loop {
        let mut line = String::new();
        let Ok(n) = reader.read_line(&mut line) else {
            break;
        };
        if n == 0 {
            break;
        }

        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        match parse_json_command(line) {
            Ok(Some(cmd)) => {
                if matches!(cmd, BridgeCommand::Shutdown) {
                    bridge.command(BridgeCommand::Shutdown);
                    let _ = emit_json_line(&json!({ "event": "stopped" }));
                    break;
                }
                bridge.command(cmd);
            }
            Ok(None) => {}
            Err(err) => {
                let _ = emit_json_line(&json!({ "event": "error", "message": err }));
            }
        }
        drain_bridge_events_as_json(&bridge, 16, Duration::from_millis(10), &mut emit_state);
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
}

#[derive(Default)]
struct JsonEmitState {
    last_waveform_peaks: Vec<f32>,
    last_library_digest: Option<LibraryDigest>,
    last_spectrogram_seq: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LibraryDigest {
    roots_len: usize,
    tracks_len: usize,
    scan_in_progress: bool,
    first_root: Option<String>,
    last_root: Option<String>,
    first_track: Option<String>,
    last_track: Option<String>,
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
        "scan_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "scan_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::ScanRoot(
                PathBuf::from(path),
            )))
        }
        "clear_queue" => Some(BridgeCommand::Queue(BridgeQueueCommand::Clear)),
        "request_snapshot" => Some(BridgeCommand::RequestSnapshot),
        "shutdown" => Some(BridgeCommand::Shutdown),
        _ => return Err(format!("unknown command '{}'", parsed.cmd)),
    };
    Ok(out)
}

fn drain_bridge_events_as_json(
    bridge: &FrontendBridgeHandle,
    max_events: usize,
    timeout: Duration,
    emit_state: &mut JsonEmitState,
) {
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
            BridgeEvent::Snapshot(s) => {
                let library_digest = LibraryDigest {
                    roots_len: s.library.roots.len(),
                    tracks_len: s.library.tracks.len(),
                    scan_in_progress: s.library.scan_in_progress,
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
                let albums_changed = emit_state
                    .last_library_digest
                    .as_ref()
                    .map(|d| d != &library_digest)
                    .unwrap_or(true);
                let library_albums = if albums_changed {
                    emit_state.last_library_digest = Some(library_digest);
                    let mut grouped: BTreeMap<(String, String), Vec<String>> = BTreeMap::new();
                    for track in &s.library.tracks {
                        let album = if track.album.trim().is_empty() {
                            String::from("Unknown Album")
                        } else {
                            track.album.clone()
                        };
                        let artist = if track.artist.trim().is_empty() {
                            String::from("Unknown Artist")
                        } else {
                            track.artist.clone()
                        };
                        grouped
                            .entry((artist, album))
                            .or_default()
                            .push(track.path.to_string_lossy().to_string());
                    }
                    serde_json::Value::Array(
                        grouped
                            .into_iter()
                            .map(|((artist, album), paths)| {
                                json!({
                                    "artist": artist,
                                    "name": album,
                                    "count": paths.len(),
                                    "paths": paths,
                                })
                            })
                            .collect(),
                    )
                } else {
                    serde_json::Value::Null
                };

                let waveform_changed = s.analysis.waveform_peaks != emit_state.last_waveform_peaks;
                let waveform_peaks = if waveform_changed {
                    emit_state.last_waveform_peaks = s.analysis.waveform_peaks.clone();
                    serde_json::Value::Array(
                        s.analysis.waveform_peaks.iter().map(|v| json!(v)).collect(),
                    )
                } else {
                    serde_json::Value::Null
                };
                let spectrogram_reset = s.analysis.spectrogram_seq
                    < emit_state.last_spectrogram_seq
                    || (s.analysis.spectrogram_seq == 0
                        && s.analysis.spectrogram_rows.is_empty()
                        && emit_state.last_spectrogram_seq > 0);
                let spectrogram_rows = if !s.analysis.spectrogram_rows.is_empty() {
                    serde_json::Value::Array(
                        s.analysis
                            .spectrogram_rows
                            .iter()
                            .map(|row| {
                                let reduced = downsample_spectrogram_row(row, 320);
                                serde_json::Value::Array(reduced.iter().map(|v| json!(v)).collect())
                            })
                            .collect(),
                    )
                } else {
                    serde_json::Value::Null
                };
                emit_state.last_spectrogram_seq = s.analysis.spectrogram_seq;
                let queue_tracks: Vec<_> = s
                    .queue
                    .iter()
                    .map(|path| {
                        let title = path
                            .file_name()
                            .map(|n| n.to_string_lossy().into_owned())
                            .unwrap_or_else(|| path.to_string_lossy().into_owned());
                        json!({
                            "path": path.to_string_lossy().to_string(),
                            "title": title,
                        })
                    })
                    .collect();
                let payload = json!({
                    "event": "snapshot",
                    "playback": {
                        "state": format!("{:?}", s.playback.state),
                        "position_secs": s.playback.position.as_secs_f64(),
                        "duration_secs": s.playback.duration.as_secs_f64(),
                        "volume": s.playback.volume,
                        "has_current": s.playback.current.is_some(),
                    },
                    "queue": {
                        "len": s.queue.len(),
                        "selected_index": s.selected_queue_index,
                        "tracks": queue_tracks,
                    },
                    "library": {
                        "roots": s.library.roots.len(),
                        "tracks": s.library.tracks.len(),
                        "scan_in_progress": s.library.scan_in_progress,
                        "albums_changed": albums_changed,
                        "albums": library_albums,
                    },
                    "analysis": {
                        "spectrogram_seq": s.analysis.spectrogram_seq,
                        "spectrogram_reset": spectrogram_reset,
                        "spectrogram_rows": spectrogram_rows,
                        "sample_rate_hz": s.analysis.sample_rate_hz,
                        "waveform_len": s.analysis.waveform_peaks.len(),
                        "waveform_changed": waveform_changed,
                        "waveform_peaks": waveform_peaks,
                    },
                    "settings": {
                        "volume": s.settings.volume,
                        "fft_size": s.settings.fft_size,
                        "db_range": s.settings.db_range,
                        "log_scale": s.settings.log_scale,
                    }
                });
                let _ = emit_json_line(&payload);
            }
            BridgeEvent::Error(message) => {
                let _ = emit_json_line(&json!({ "event": "error", "message": message }));
            }
            BridgeEvent::Stopped => {
                let _ = emit_json_line(&json!({ "event": "stopped" }));
            }
        }
    }
}

fn emit_json_line(payload: &serde_json::Value) -> io::Result<()> {
    let mut out = io::stdout().lock();
    writeln!(out, "{}", payload)?;
    out.flush()
}

fn downsample_spectrogram_row(row: &[f32], max_bins: usize) -> Vec<f32> {
    if row.len() <= max_bins || max_bins == 0 {
        return row.to_vec();
    }
    let mut out = Vec::with_capacity(max_bins);
    for i in 0..max_bins {
        let start = i * row.len() / max_bins;
        let mut end = (i + 1) * row.len() / max_bins;
        if end <= start {
            end = (start + 1).min(row.len());
        }
        let mut peak = 0.0f32;
        for &v in &row[start..end] {
            if v > peak {
                peak = v;
            }
        }
        out.push(peak);
    }
    out
}
