use std::collections::BTreeMap;
use std::io::{self, BufRead, Write};
use std::path::PathBuf;
use std::time::{Duration, Instant};

use crossbeam_channel::unbounded;
use ferrous::frontend_bridge::{
    BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSnapshot, FrontendBridgeHandle,
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

    let mut emit_state = JsonEmitState {
        profile_enabled: std::env::var_os("FERROUS_PROFILE").is_some(),
        ..JsonEmitState::default()
    };
    bridge.command(BridgeCommand::RequestSnapshot);
    drain_bridge_events_as_json(&bridge, 32, Duration::from_millis(1), &mut emit_state);

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
                                let _ =
                                    emit_json_line(&json!({ "event": "stopped" }), &mut emit_state);
                                return;
                            }
                            bridge.command(cmd);
                        }
                        Ok(None) => {}
                        Err(err) => {
                            let _ = emit_json_line(
                                &json!({ "event": "error", "message": err }),
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

        drain_bridge_events_as_json(&bridge, 64, Duration::from_millis(1), &mut emit_state);

        if eof_seen {
            bridge.command(BridgeCommand::Shutdown);
            let _ = emit_json_line(&json!({ "event": "stopped" }), &mut emit_state);
            return;
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

#[derive(Default)]
struct JsonEmitState {
    last_waveform_peaks: Vec<f32>,
    last_library_digest: Option<LibraryDigest>,
    last_queue_digest: Option<QueueDigest>,
    last_spectrogram_seq: u64,
    profile_enabled: bool,
    profile_last: Option<Instant>,
    profile_snapshots: usize,
    profile_bytes: usize,
    profile_max_payload_bytes: usize,
    profile_max_write_ms: f64,
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct QueueDigest {
    len: usize,
    selected: Option<usize>,
    first: Option<String>,
    last: Option<String>,
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
            BridgeEvent::Snapshot(s) => latest_snapshot = Some(s),
            BridgeEvent::Error(message) => {
                let _ =
                    emit_json_line(&json!({ "event": "error", "message": message }), emit_state);
            }
            BridgeEvent::Stopped => {
                let _ = emit_json_line(&json!({ "event": "stopped" }), emit_state);
                return;
            }
        }
    }

    if let Some(s) = latest_snapshot {
        let payload = encode_snapshot_payload(&s, emit_state);
        let _ = emit_json_line(&payload, emit_state);
    }
}

fn encode_snapshot_payload(
    s: &BridgeSnapshot,
    emit_state: &mut JsonEmitState,
) -> serde_json::Value {
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
    let should_emit_albums =
        albums_changed && (!s.library.scan_in_progress || emit_state.last_library_digest.is_none());
    emit_state.last_library_digest = Some(library_digest);
    let library_albums = if should_emit_albums {
        let mut grouped: BTreeMap<(String, String), usize> = BTreeMap::new();
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
            *grouped.entry((artist, album)).or_insert(0) += 1;
        }
        serde_json::Value::Array(
            grouped
                .into_iter()
                .map(|((artist, album), count)| {
                    json!({
                        "artist": artist,
                        "name": album,
                        "count": count,
                    })
                })
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };

    let queue_digest = QueueDigest {
        len: s.queue.len(),
        selected: s.selected_queue_index,
        first: s.queue.first().map(|p| p.to_string_lossy().to_string()),
        last: s.queue.last().map(|p| p.to_string_lossy().to_string()),
    };
    let queue_changed = emit_state
        .last_queue_digest
        .as_ref()
        .map(|d| d != &queue_digest)
        .unwrap_or(true);
    let queue_tracks = if queue_changed {
        emit_state.last_queue_digest = Some(queue_digest);
        serde_json::Value::Array(
            s.queue
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
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };

    let waveform_changed = s.analysis.waveform_peaks != emit_state.last_waveform_peaks;
    let waveform_peaks = if waveform_changed {
        emit_state.last_waveform_peaks = s.analysis.waveform_peaks.clone();
        let reduced = downsample_waveform_peaks(&s.analysis.waveform_peaks, 1024);
        serde_json::Value::Array(reduced.iter().map(|v| json!(v)).collect())
    } else {
        serde_json::Value::Null
    };

    let spectrogram_reset = s.analysis.spectrogram_seq < emit_state.last_spectrogram_seq
        || (s.analysis.spectrogram_seq == 0
            && s.analysis.spectrogram_rows.is_empty()
            && emit_state.last_spectrogram_seq > 0);
    let spectrogram_seq = s.analysis.spectrogram_seq;
    let spectrogram_delta =
        spectrogram_seq.saturating_sub(emit_state.last_spectrogram_seq) as usize;
    let spectrogram_rows = if spectrogram_delta > 0 && !s.analysis.spectrogram_rows.is_empty() {
        let tail = spectrogram_delta
            .min(s.analysis.spectrogram_rows.len())
            .min(3);
        let start = s.analysis.spectrogram_rows.len().saturating_sub(tail);
        serde_json::Value::Array(
            s.analysis.spectrogram_rows[start..]
                .iter()
                .map(|row| {
                    let reduced = downsample_spectrogram_row(row, 160);
                    serde_json::Value::Array(reduced.iter().map(|v| json!(v)).collect())
                })
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };
    emit_state.last_spectrogram_seq = spectrogram_seq;

    json!({
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
            "albums_changed": should_emit_albums,
            "albums": library_albums,
        },
        "analysis": {
            "spectrogram_seq": spectrogram_seq,
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
    })
}

fn emit_json_line(payload: &serde_json::Value, emit_state: &mut JsonEmitState) -> io::Result<()> {
    let started = Instant::now();
    let bytes = serde_json::to_vec(payload)?;
    let mut out = io::stdout().lock();
    out.write_all(&bytes)?;
    out.write_all(b"\n")?;
    out.flush()?;

    if emit_state.profile_enabled {
        emit_state.profile_snapshots = emit_state.profile_snapshots.saturating_add(1);
        emit_state.profile_bytes = emit_state.profile_bytes.saturating_add(bytes.len());
        emit_state.profile_max_payload_bytes =
            emit_state.profile_max_payload_bytes.max(bytes.len());
        let write_ms = started.elapsed().as_secs_f64() * 1000.0;
        emit_state.profile_max_write_ms = emit_state.profile_max_write_ms.max(write_ms);

        let now = Instant::now();
        let should_report = emit_state
            .profile_last
            .map(|t| now.duration_since(t) >= Duration::from_secs(1))
            .unwrap_or(true);
        if should_report {
            eprintln!(
                "[bridge-json] snaps/s={} bytes/s={} max_payload={}B max_write_ms={:.2}",
                emit_state.profile_snapshots,
                emit_state.profile_bytes,
                emit_state.profile_max_payload_bytes,
                emit_state.profile_max_write_ms
            );
            emit_state.profile_last = Some(now);
            emit_state.profile_snapshots = 0;
            emit_state.profile_bytes = 0;
            emit_state.profile_max_payload_bytes = 0;
            emit_state.profile_max_write_ms = 0.0;
        }
    }

    Ok(())
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
