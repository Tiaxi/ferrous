use std::io::{self, BufRead, Write};
use std::time::Duration;

use ferrous::frontend_bridge::{
    BridgeCommand, BridgeEvent, BridgePlaybackCommand, BridgeQueueCommand, FrontendBridgeHandle,
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
    bridge.command(BridgeCommand::RequestSnapshot);
    drain_bridge_events_as_json(&bridge, 16, Duration::from_millis(10));

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
        drain_bridge_events_as_json(&bridge, 16, Duration::from_millis(10));
    }
}

#[derive(Debug, Deserialize)]
struct JsonCommand {
    cmd: String,
    value: Option<f64>,
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
                    },
                    "analysis": {
                        "spectrogram_seq": s.analysis.spectrogram_seq,
                        "sample_rate_hz": s.analysis.sample_rate_hz,
                        "waveform_peaks": s.analysis.waveform_peaks.len(),
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
