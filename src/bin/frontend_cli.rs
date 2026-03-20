use std::io::{self, Write};
use std::time::Duration;

use ferrous::frontend_bridge::{
    BridgeCommand, BridgeEvent, BridgePlaybackCommand, BridgeSettingsCommand, FrontendBridgeHandle,
};
use ferrous::playback::RepeatMode;
use tracing_subscriber::{fmt, EnvFilter};

fn main() {
    fmt()
        .with_env_filter(EnvFilter::from_default_env())
        .without_time()
        .init();

    let bridge = FrontendBridgeHandle::spawn();
    run_interactive_cli(&bridge);
}

enum CliCommandOutcome {
    Continue,
    Quit,
}

fn run_interactive_cli(bridge: &FrontendBridgeHandle) {
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

        if matches!(handle_cli_command(bridge, line), CliCommandOutcome::Quit) {
            break;
        }
        drain_bridge_events(bridge);
    }
}

fn handle_cli_command(bridge: &FrontendBridgeHandle, line: &str) -> CliCommandOutcome {
    if line == "quit" || line == "exit" {
        bridge.command(BridgeCommand::Shutdown);
        return CliCommandOutcome::Quit;
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
            Ok(value) => {
                bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(
                    value,
                )));
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
        match rest.parse::<f32>() {
            Ok(value) if value.is_finite() => bridge.command(BridgeCommand::Settings(
                BridgeSettingsCommand::SetDbRange(value),
            )),
            _ => eprintln!("invalid dbrange value"),
        }
    } else if let Some(rest) = line.strip_prefix("log ") {
        handle_toggle_command(
            bridge,
            rest,
            |value| BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(value)),
            "log",
        );
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
        handle_toggle_command(
            bridge,
            rest,
            |value| BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(value)),
            "shuffle",
        );
    } else if let Some(rest) = line.strip_prefix("fps ") {
        handle_toggle_command(
            bridge,
            rest,
            |value| BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(value)),
            "fps",
        );
    } else if line == "snap" {
        bridge.command(BridgeCommand::RequestSnapshot);
    } else {
        eprintln!("unknown command");
    }
    CliCommandOutcome::Continue
}

fn handle_toggle_command<C>(
    bridge: &FrontendBridgeHandle,
    rest: &str,
    build_command: C,
    label: &str,
) where
    C: FnOnce(bool) -> BridgeCommand,
{
    match rest.parse::<i32>() {
        Ok(value) => bridge.command(build_command(value != 0)),
        Err(_) => eprintln!("invalid {label} value, expected 0 or 1"),
    }
}

fn drain_bridge_events(bridge: &FrontendBridgeHandle) {
    for _ in 0..8 {
        let Some(event) = bridge.recv_timeout(Duration::from_millis(60)) else {
            break;
        };
        match event {
            BridgeEvent::Snapshot(snapshot) => {
                println!(
                    "state={:?} pos={}/{} queue={} volume={:.2}",
                    snapshot.playback.state,
                    snapshot.playback.position.as_secs(),
                    snapshot.playback.duration.as_secs(),
                    snapshot.queue.len(),
                    snapshot.playback.volume
                );
            }
            BridgeEvent::SearchResults(frame) => {
                println!("search seq={} rows={}", frame.seq, frame.rows.len());
            }
            BridgeEvent::PrecomputedSpectrogramChunk(_) => {}
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
