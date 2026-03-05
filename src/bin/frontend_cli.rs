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
    run_interactive_cli(bridge);
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
                BridgeEvent::SearchResults(frame) => {
                    println!("search seq={} rows={}", frame.seq, frame.rows.len());
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
