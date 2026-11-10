// SPDX-License-Identifier: GPL-3.0-or-later

//! The events the bridge writes to the diagnostics log at info level, one
//! line each, so that the log reads as a trace of what the app did: the
//! backend starting and stopping, and playback. A line is written when
//! something changes, never for a snapshot that only moves the position on.

use std::path::{Path, PathBuf};

use crate::playback::{PlaybackSnapshot, PlaybackState, RepeatMode, TrackChangeKind};

use super::{log_info, log_position, quoted_path, BridgeCommand, BridgeState, ErrorCategory};

/// The line the backend logs when it starts: the versions it runs with and
/// the directories it keeps its files in.
pub(super) fn backend_started_line() -> String {
    let directory =
        |dir: Option<PathBuf>| dir.map_or_else(|| "none".to_string(), |dir| quoted_path(&dir));
    #[cfg(feature = "gst")]
    let playback = gstreamer::version_string().to_string();
    #[cfg(not(feature = "gst"))]
    let playback = "the mock playback backend".to_string();
    format!(
        "Backend started: Ferrous {}, {}, {playback}; config {}, data {}, cache {}.",
        env!("CARGO_PKG_VERSION"),
        env!("FERROUS_RUSTC_VERSION"),
        directory(crate::paths::config_dir()),
        directory(crate::paths::data_dir()),
        directory(crate::paths::cache_dir()),
    )
}

/// The most of a command [`debug_command_line`] shows: a queue of
/// thousands of paths would make one line of megabytes.
const DEBUG_COMMAND_MAX_CHARS: usize = 400;

/// `cmd` as `FERROUS_LOG=debug` logs it.
pub(super) fn debug_command_line(cmd: &BridgeCommand) -> String {
    let mut line = format!("Command: {cmd:?}");
    if let Some((cut, _)) = line.char_indices().nth(DEBUG_COMMAND_MAX_CHARS) {
        line.truncate(cut);
        line.push('…');
    }
    line
}

/// How a track came to play, for its line.
fn track_change_cause(state: &BridgeState, path: &Path, kind: &TrackChangeKind) -> &'static str {
    match kind {
        TrackChangeKind::Manual if state.restored_track_pending => "restored session",
        TrackChangeKind::Manual => "user",
        TrackChangeKind::Gapless | TrackChangeKind::Natural
            if state.playback.repeat_mode == RepeatMode::One
                && state.last_track_change.path.as_deref() == Some(path) =>
        {
            "Repeat-One"
        }
        TrackChangeKind::Gapless => "gapless",
        TrackChangeKind::Natural => "next in queue",
    }
}

/// Logs that the track at `path` became the current one, and how. Called
/// before `state` takes the change in, so it still knows the track before.
pub(super) fn log_track_change(state: &mut BridgeState, path: &Path, kind: &TrackChangeKind) {
    let cause = track_change_cause(state, path, kind);
    state.restored_track_pending = false;
    log_info(
        ErrorCategory::Playback,
        &format!("Track started ({cause}): {}.", quoted_path(path)),
    );
}

/// The line for playback going from `previous` to `now`, if its state
/// changed.
fn state_change_line(previous: PlaybackState, now: &PlaybackSnapshot) -> Option<String> {
    let position = log_position(now.position);
    match (previous, now.state) {
        (before, after) if before == after => None,
        (PlaybackState::Stopped, PlaybackState::Playing) => {
            Some(format!("Playing from {position}."))
        }
        (PlaybackState::Paused, PlaybackState::Playing) => Some(format!("Resumed at {position}.")),
        (_, PlaybackState::Paused) => Some(format!("Paused at {position}.")),
        (_, PlaybackState::Stopped) => Some("Stopped.".to_string()),
        // Every other pair is covered above; `Playing` is reached only from
        // the other two states.
        (_, PlaybackState::Playing) => None,
    }
}

/// Logs what changed between `previous`, the snapshot playback sent
/// before, and `now`: its state, repeat mode, or shuffle. A snapshot that
/// only moves the position on writes nothing.
pub(super) fn log_playback_changes(previous: &PlaybackSnapshot, now: &PlaybackSnapshot) {
    if let Some(line) = state_change_line(previous.state, now) {
        log_info(ErrorCategory::Playback, &line);
    }
    if previous.repeat_mode != now.repeat_mode {
        let mode = match now.repeat_mode {
            RepeatMode::Off => "off",
            RepeatMode::One => "one track",
            RepeatMode::All => "all tracks",
        };
        log_info(ErrorCategory::Playback, &format!("Repeat: {mode}."));
    }
    if previous.shuffle_enabled != now.shuffle_enabled {
        let shuffle = if now.shuffle_enabled { "on" } else { "off" };
        log_info(ErrorCategory::Playback, &format!("Shuffle: {shuffle}."));
    }
}

/// Logs a seek that landed at `position`.
pub(super) fn log_seek(position: std::time::Duration) {
    log_info(
        ErrorCategory::Playback,
        &format!("Seeked to {}.", log_position(position)),
    );
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::super::test_log;
    use super::*;

    fn snapshot(state: PlaybackState, position_ms: u64) -> PlaybackSnapshot {
        PlaybackSnapshot {
            state,
            position: Duration::from_millis(position_ms),
            current: Some(PathBuf::from("/fixture/library/one.flac")),
            ..PlaybackSnapshot::default()
        }
    }

    #[test]
    fn the_backend_start_line_names_the_versions_and_directories() {
        let line = backend_started_line();
        assert!(line.starts_with("Backend started: Ferrous "), "{line}");
        assert!(line.contains("rustc "), "{line}");
        let config = crate::paths::config_dir().expect("a config directory");
        assert!(line.contains(&quoted_path(&config)), "{line}");
    }

    #[test]
    fn a_debug_command_line_is_cut_short() {
        let tracks = (0..1_000)
            .map(|n| PathBuf::from(format!("/fixture/library/{n}.flac")))
            .collect();
        let line = debug_command_line(&BridgeCommand::Queue(
            super::super::BridgeQueueCommand::Append(tracks),
        ));
        assert!(line.starts_with("Command: Queue(Append(["), "{line}");
        assert_eq!(line.chars().count(), DEBUG_COMMAND_MAX_CHARS + 1);
        assert!(line.ends_with('…'));
    }

    #[test]
    fn a_track_start_names_its_file_and_how_it_began() {
        test_log::clear();
        let path = PathBuf::from("/fixture/library/one.flac");
        let mut state = BridgeState {
            restored_track_pending: true,
            ..BridgeState::default()
        };
        log_track_change(&mut state, &path, &TrackChangeKind::Manual);
        log_track_change(&mut state, &path, &TrackChangeKind::Manual);
        log_track_change(&mut state, &path, &TrackChangeKind::Gapless);
        log_track_change(&mut state, &path, &TrackChangeKind::Natural);
        state.playback.repeat_mode = RepeatMode::One;
        state.last_track_change.path = Some(path.clone());
        log_track_change(&mut state, &path, &TrackChangeKind::Gapless);
        let other = PathBuf::from("/fixture/library/two.flac");
        log_track_change(&mut state, &other, &TrackChangeKind::Natural);
        assert_eq!(
            test_log::lines_containing("Track started"),
            [
                "restored session",
                "user",
                "gapless",
                "next in queue",
                "Repeat-One",
            ]
            .iter()
            .map(|cause| {
                format!("[ferrous] [playback] info: Track started ({cause}): \"/fixture/library/one.flac\".")
            })
            .chain(std::iter::once(
                "[ferrous] [playback] info: Track started (next in queue): \"/fixture/library/two.flac\"."
                    .to_string()
            ))
            .collect::<Vec<_>>()
        );
    }

    #[test]
    fn state_changes_write_one_line_each() {
        test_log::clear();
        let stopped = snapshot(PlaybackState::Stopped, 0);
        let playing = snapshot(PlaybackState::Playing, 0);
        let paused = snapshot(PlaybackState::Paused, 83_400);
        let resumed = snapshot(PlaybackState::Playing, 83_400);
        log_playback_changes(&stopped, &playing);
        log_playback_changes(&playing, &paused);
        log_playback_changes(&paused, &resumed);
        log_playback_changes(&resumed, &stopped);
        let shuffled = PlaybackSnapshot {
            shuffle_enabled: true,
            repeat_mode: RepeatMode::All,
            ..stopped.clone()
        };
        log_playback_changes(&stopped, &shuffled);
        log_seek(Duration::from_millis(3_723_000));
        assert_eq!(
            test_log::all(),
            [
                "Playing from 0:00.0.",
                "Paused at 1:23.4.",
                "Resumed at 1:23.4.",
                "Stopped.",
                "Repeat: all tracks.",
                "Shuffle: on.",
                "Seeked to 1:02:03.0.",
            ]
            .map(|line| format!("[ferrous] [playback] info: {line}"))
        );
    }

    #[test]
    fn snapshots_that_only_move_the_position_on_log_nothing() {
        test_log::clear();
        let mut previous = snapshot(PlaybackState::Playing, 0);
        // An hour of snapshots, ten a second.
        for tick in 1..36_000 {
            let now = snapshot(PlaybackState::Playing, tick * 100);
            log_playback_changes(&previous, &now);
            previous = now;
        }
        assert!(test_log::all().is_empty(), "{:?}", test_log::all());
    }
    #[test]
    fn a_minute_of_playback_writes_a_line_per_event_and_none_per_tick() {
        use crate::analysis::AnalysisEngine;
        use crate::metadata::MetadataService;
        use crate::playback::PlaybackEvent;

        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();
        let mut state = BridgeState::default();
        let feed = |event: PlaybackEvent, state: &mut BridgeState| {
            let _ = super::super::events::process_playback_event(
                event, &analysis, &metadata, &event_tx, state,
            );
        };
        test_log::clear();
        feed(
            PlaybackEvent::TrackChanged {
                path: PathBuf::from("/fixture/library/one.flac"),
                queue_index: 0,
                kind: TrackChangeKind::Manual,
                track_token: 1,
            },
            &mut state,
        );
        // A minute of playback, snapshots ten times a second, with a pause
        // and a seek on the way.
        for tick in 0..600 {
            let at = tick * 100;
            let playing = if (300..320).contains(&tick) {
                PlaybackState::Paused
            } else {
                PlaybackState::Playing
            };
            feed(PlaybackEvent::Snapshot(snapshot(playing, at)), &mut state);
            if tick == 400 {
                feed(
                    PlaybackEvent::Seeked {
                        position: Duration::from_secs(50),
                    },
                    &mut state,
                );
            }
        }
        feed(
            PlaybackEvent::Snapshot(snapshot(PlaybackState::Stopped, 0)),
            &mut state,
        );
        assert_eq!(
            test_log::all(),
            [
                "Track started (user): \"/fixture/library/one.flac\".",
                "Playing from 0:00.0.",
                "Paused at 0:30.0.",
                "Resumed at 0:32.0.",
                "Seeked to 0:50.0.",
                "Stopped.",
            ]
            .map(|line| format!("[ferrous] [playback] info: {line}"))
        );
    }
}
