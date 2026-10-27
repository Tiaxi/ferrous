// SPDX-License-Identifier: GPL-3.0-or-later

// As in src/lib.rs, test code is exempt from Clippy's pedantic group only.
#![cfg_attr(test, allow(clippy::pedantic))]

use std::io::{self, BufRead, Write};
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

    let bridge = match FrontendBridgeHandle::spawn() {
        Ok(bridge) => bridge,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(1);
        }
    };
    run_interactive_cli(&bridge);
    if !bridge.shutdown() {
        eprintln!(
            "The backend did not finish shutting down in time; recent settings, the queue, \
             or Last.fm scrobbles may not have been saved."
        );
    }
}

/// What [`read_cli_line`] got from the input.
enum CliInput {
    Line(String),
    /// A line that is not UTF-8, which has been read past.
    NotUtf8,
    /// The input ended, as when stdin is a closed pipe or Ctrl-D is pressed.
    Eof,
    /// The input failed and cannot be read further.
    Failed(io::Error),
}

#[derive(Debug)]
enum CliCommand {
    Quit,
    Bridge(BridgeCommand),
}

fn run_interactive_cli(bridge: &FrontendBridgeHandle) {
    println!("Ferrous UI bootstrap");
    println!(
        "Commands: play, pause, stop, next, prev, vol <0..1>, seek <secs>, dbrange <50..150>, log <0|1>, repeat <0|1|2>, shuffle <0|1>, fps <0|1>, snap, quit"
    );

    run_cli_loop(io::stdin().lock(), |command| {
        bridge.command(command);
        drain_bridge_events(bridge);
    });
}

/// Runs commands from `input` until it ends, fails, or asks to quit.
fn run_cli_loop(mut input: impl BufRead, mut run_command: impl FnMut(BridgeCommand)) {
    loop {
        print!("ui> ");
        let _ = io::stdout().flush();

        let line = match read_cli_line(&mut input) {
            CliInput::Line(line) => line,
            CliInput::NotUtf8 => {
                eprintln!("input is not UTF-8");
                continue;
            }
            CliInput::Eof => {
                println!();
                return;
            }
            CliInput::Failed(err) => {
                eprintln!("failed to read input: {err}");
                return;
            }
        };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        match parse_cli_command(line) {
            Ok(CliCommand::Quit) => return,
            Ok(CliCommand::Bridge(command)) => run_command(command),
            Err(message) => eprintln!("{message}"),
        }
    }
}

fn read_cli_line(input: &mut impl BufRead) -> CliInput {
    let mut line = String::new();
    match input.read_line(&mut line) {
        Ok(0) => CliInput::Eof,
        Ok(_) => CliInput::Line(line),
        Err(err) if err.kind() == io::ErrorKind::InvalidData => CliInput::NotUtf8,
        Err(err) => CliInput::Failed(err),
    }
}

fn parse_cli_command(line: &str) -> Result<CliCommand, String> {
    let command = match line {
        "quit" | "exit" => return Ok(CliCommand::Quit),
        "play" => BridgeCommand::Playback(BridgePlaybackCommand::Play),
        "pause" => BridgeCommand::Playback(BridgePlaybackCommand::Pause),
        "stop" => BridgeCommand::Playback(BridgePlaybackCommand::Stop),
        "next" => BridgeCommand::Playback(BridgePlaybackCommand::Next),
        "prev" => BridgeCommand::Playback(BridgePlaybackCommand::Previous),
        "snap" => BridgeCommand::RequestSnapshot,
        _ => parse_cli_command_with_value(line)?,
    };
    Ok(CliCommand::Bridge(command))
}

fn parse_cli_command_with_value(line: &str) -> Result<BridgeCommand, String> {
    let Some((name, value)) = line.split_once(' ') else {
        return Err("unknown command".to_string());
    };
    let value = value.trim();
    let command = match name {
        "vol" => match value.parse::<f32>() {
            Ok(volume) if (0.0..=1.0).contains(&volume) => {
                BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(volume))
            }
            _ => return Err("invalid volume value, expected 0 to 1".to_string()),
        },
        "seek" => {
            // Rejects negative, infinite and NaN values, and ones too large
            // for a `Duration`.
            match value.parse::<f64>().map(Duration::try_from_secs_f64) {
                Ok(Ok(position)) => BridgeCommand::Playback(BridgePlaybackCommand::Seek(position)),
                _ => return Err("invalid seek value, expected seconds from 0".to_string()),
            }
        }
        "dbrange" => match value.parse::<f32>() {
            Ok(range) if range.is_finite() => {
                BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(range))
            }
            _ => return Err("invalid dbrange value".to_string()),
        },
        "log" => BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(parse_toggle(
            value, "log",
        )?)),
        "repeat" => {
            let Ok(mode) = value.parse::<i32>() else {
                return Err("invalid repeat value, expected 0, 1, or 2".to_string());
            };
            let mode = match mode {
                1 => RepeatMode::One,
                2 => RepeatMode::All,
                _ => RepeatMode::Off,
            };
            BridgeCommand::Playback(BridgePlaybackCommand::SetRepeatMode(mode))
        }
        "shuffle" => BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(parse_toggle(
            value, "shuffle",
        )?)),
        "fps" => BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(parse_toggle(
            value, "fps",
        )?)),
        _ => return Err("unknown command".to_string()),
    };
    Ok(command)
}

fn parse_toggle(value: &str, label: &str) -> Result<bool, String> {
    value
        .parse::<i32>()
        .map(|value| value != 0)
        .map_err(|_| format!("invalid {label} value, expected 0 or 1"))
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
                    snapshot.queue_len,
                    snapshot.playback.volume
                );
            }
            BridgeEvent::SearchResults(frame) => {
                println!("search seq={} rows={}", frame.seq, frame.rows.len());
            }
            // The CLI draws no spectrogram and has no window.
            BridgeEvent::PrecomputedSpectrogramChunk(_) | BridgeEvent::WindowLayout { .. } => {}
            BridgeEvent::Error(err) => {
                eprintln!("bridge error: {err}");
            }
            BridgeEvent::Stopped => {
                println!("bridge stopped");
                return;
            }
            BridgeEvent::Failed(err) => {
                eprintln!("bridge failed: {err}");
                return;
            }
            BridgeEvent::StateSaved { result: Ok(()), .. } => println!("state saved"),
            BridgeEvent::StateSaved {
                result: Err(err), ..
            } => eprintln!("state not saved: {err}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::io::Cursor;

    fn run_lines(input: &[u8]) -> Vec<BridgeCommand> {
        let mut commands = Vec::new();
        run_cli_loop(Cursor::new(input.to_vec()), |command| {
            commands.push(command)
        });
        commands
    }

    #[test]
    fn loop_returns_at_end_of_input() {
        let commands = run_lines(b"play\n\n  \npause");
        assert!(matches!(
            commands.as_slice(),
            [
                BridgeCommand::Playback(BridgePlaybackCommand::Play),
                BridgeCommand::Playback(BridgePlaybackCommand::Pause),
            ]
        ));
        assert!(run_lines(b"").is_empty());
    }

    #[test]
    fn loop_stops_at_quit_and_skips_invalid_lines() {
        let commands = run_lines(b"seek inf\n\xff\xfe\nstop\nquit\nplay\n");
        assert!(matches!(
            commands.as_slice(),
            [BridgeCommand::Playback(BridgePlaybackCommand::Stop)]
        ));
    }

    #[test]
    fn read_reports_end_of_input_and_failures() {
        assert!(matches!(
            read_cli_line(&mut Cursor::new(b"")),
            CliInput::Eof
        ));
        let mut input = Cursor::new(b"\xff\nplay\n");
        assert!(matches!(read_cli_line(&mut input), CliInput::NotUtf8));
        assert!(matches!(read_cli_line(&mut input), CliInput::Line(line) if line == "play\n"));

        struct FailingInput;
        impl io::Read for FailingInput {
            fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
                Err(io::Error::other("closed terminal"))
            }
        }
        let mut input = io::BufReader::new(FailingInput);
        assert!(matches!(read_cli_line(&mut input), CliInput::Failed(_)));
        // A loop over input that keeps failing returns instead of spinning.
        run_cli_loop(input, |_| panic!("no command expected"));
    }

    #[test]
    fn seek_accepts_seconds_from_zero() {
        for (value, expected) in [("0", 0.0), ("12.5", 12.5), (" 3 ", 3.0)] {
            match parse_cli_command(&format!("seek {value}")) {
                Ok(CliCommand::Bridge(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
                    position,
                )))) => assert_eq!(position, Duration::from_secs_f64(expected)),
                other => panic!("seek {value} parsed as {other:?}"),
            }
        }
    }

    #[test]
    fn seek_rejects_values_without_a_position() {
        for value in [
            "inf", "-inf", "infinity", "nan", "NaN", "-1", "-0.5", "1e300", "x", "",
        ] {
            assert!(
                parse_cli_command(&format!("seek {value}")).is_err(),
                "seek {value} was accepted"
            );
        }
    }

    #[test]
    fn volume_rejects_values_outside_zero_to_one() {
        assert!(matches!(
            parse_cli_command("vol 0.5"),
            Ok(CliCommand::Bridge(BridgeCommand::Playback(
                BridgePlaybackCommand::SetVolume(volume)
            ))) if volume == 0.5
        ));
        for value in ["nan", "inf", "-0.1", "1.5", "loud"] {
            assert!(
                parse_cli_command(&format!("vol {value}")).is_err(),
                "vol {value} was accepted"
            );
        }
    }

    #[test]
    fn parses_simple_and_toggle_commands() {
        assert!(matches!(parse_cli_command("quit"), Ok(CliCommand::Quit)));
        assert!(matches!(parse_cli_command("exit"), Ok(CliCommand::Quit)));
        assert!(matches!(
            parse_cli_command("snap"),
            Ok(CliCommand::Bridge(BridgeCommand::RequestSnapshot))
        ));
        assert!(matches!(
            parse_cli_command("shuffle 1"),
            Ok(CliCommand::Bridge(BridgeCommand::Playback(
                BridgePlaybackCommand::SetShuffle(true)
            )))
        ));
        assert!(matches!(
            parse_cli_command("repeat 2"),
            Ok(CliCommand::Bridge(BridgeCommand::Playback(
                BridgePlaybackCommand::SetRepeatMode(RepeatMode::All)
            )))
        ));
        assert!(parse_cli_command("log maybe").is_err());
        assert!(parse_cli_command("dbrange nan").is_err());
        assert!(parse_cli_command("rewind").is_err());
        assert!(parse_cli_command("play now").is_err());
    }
}
