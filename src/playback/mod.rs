// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};

use crate::analysis::AnalysisCommand;

pub mod clock;
pub use clock::{monotonic_now, MonotonicTime};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum PlaybackState {
    #[default]
    Stopped,
    Playing,
    Paused,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum RepeatMode {
    #[default]
    Off,
    One,
    All,
}

/// Convert a volume level, the value shown by the slider, reported over MPRIS,
/// and saved in the settings, to the linear amplitude gain applied to audio.
///
/// The scale is cubic like `GST_STREAM_VOLUME_FORMAT_CUBIC` and the
/// PulseAudio/PipeWire mixers, so equal slider steps sound roughly equal:
/// level 0.5 is gain 0.125 (about −18 dB).
#[must_use]
pub fn volume_level_to_gain(level: f32) -> f32 {
    let level = level.clamp(0.0, 1.0);
    level * level * level
}

/// Inverse of [`volume_level_to_gain`].
#[must_use]
pub fn volume_gain_to_level(gain: f32) -> f32 {
    gain.clamp(0.0, 1.0).cbrt()
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct PlaybackSnapshot {
    pub state: PlaybackState,
    /// Where the pipeline was at `position_sampled_at`, as it reported it.
    /// The engine does not smooth or extrapolate it; the UI does, once.
    pub position: Duration,
    /// When the engine read `position`, on `CLOCK_MONOTONIC`.
    pub position_sampled_at: MonotonicTime,
    /// Whether `position` advances in real time from `position_sampled_at`:
    /// the pipeline plays, and no seek or restart holds it. False while
    /// playback starts, resumes, or waits for a seek, even though `state`
    /// is already Playing.
    pub position_advancing: bool,
    pub duration: Duration,
    pub current: Option<PathBuf>,
    pub current_queue_index: Option<usize>,
    pub current_bitrate_kbps: Option<u32>,
    /// Volume level requested by the user; see [`volume_level_to_gain`].
    pub volume: f32,
    pub repeat_mode: RepeatMode,
    pub shuffle_enabled: bool,
    pub muted_channels_mask: u64,
    pub soloed_channel: Option<u8>,
    /// `track_token` of the `TrackChanged` event that made `current`
    /// current, or 0 before any. It changes on every track change, also
    /// when the same file plays again.
    pub track_token: u64,
    /// Serial of the last [`PlaybackCommand::Seek`] the engine is done
    /// with: applied, refused because nothing plays, failed, or superseded
    /// by a newer seek or a track change. The engine numbers seeks from 1
    /// in the order it receives them. Until this reaches the serial of a
    /// seek, `position` describes playback before that seek.
    pub seek_serial: u64,
    /// Serial of the last seek that the pipeline applied, or 0. The first
    /// snapshot with a new value carries where that seek landed in
    /// `position`, and follows its [`PlaybackEvent::Seeked`].
    pub seek_landed_serial: u64,
}

/// New paths of renamed files, keyed by their old paths.
pub type PathRenames = HashMap<PathBuf, PathBuf>;

#[derive(Debug, Clone)]
pub enum PlaybackCommand {
    LoadQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    /// Replace the paths of renamed files in place. The queue order, current
    /// track, position, and playback state stay the same, and the current
    /// track keeps playing from the file it has open. A track whose old path
    /// the engine opened after the rename, finding another file or none, is
    /// started again from its new path. The engine answers with
    /// [`PlaybackEvent::PathsRemapped`].
    RemapPaths(Arc<PathRenames>),
    /// Replace the queue with `paths`, as an undo of a queue change does.
    /// When the track the engine has open, playing, paused or stopped, is
    /// in `paths`, it stays open undisturbed: at row `current` when that is
    /// its path, otherwise at its first row. Otherwise playback stops and
    /// row `current` becomes the current track, stopped at its start. The
    /// engine never starts playback for this command.
    RestoreQueue {
        paths: Vec<PathBuf>,
        current: usize,
    },
    RemoveAt(usize),
    RemoveMany(Vec<usize>),
    MoveQueue {
        from: usize,
        to: usize,
    },
    /// Put the rows in this order, which names for each new row the row it
    /// was before. The committed track stays open and keeps its row's
    /// identity; staged successors stay while they still follow it. An
    /// order that does not name every row once is ignored.
    ReorderQueue(Vec<usize>),
    /// Insert `paths` before row `at`, or after the last row when `at` is
    /// past it. The committed track stays open.
    InsertIntoQueue {
        at: usize,
        paths: Vec<PathBuf>,
    },
    ClearQueue,
    PlayAt(usize),
    /// Move to the next track. A burst of Next and Previous presses moves
    /// once per press and starts only the track it ends on.
    Next,
    Previous,
    Play,
    Pause,
    Stop,
    /// Seek in the current track. Ignored while stopped: Play starts a
    /// stopped track from its beginning. A burst of seeks lands only at
    /// the last target.
    Seek(Duration),
    SetVolume(f32),
    SetRepeatMode(RepeatMode),
    SetShuffle(bool),
    ToggleChannelMute(u8),
    SoloChannel(u8),
    Poll,
}

#[derive(Debug, Clone)]
pub enum TrackChangeKind {
    Manual,
    /// Same-format gapless: decoder reused, audio stream is continuous.
    Gapless,
    /// Cross-format or EOS-based: pipeline was restarted.
    Natural,
}

#[derive(Debug, Clone)]
pub enum PlaybackEvent {
    Snapshot(PlaybackSnapshot),
    TrackChanged {
        path: PathBuf,
        queue_index: usize,
        kind: TrackChangeKind,
        track_token: u64,
    },
    /// A seek was applied; `position` is where it landed. Seeks that are
    /// refused, fail, or are superseded before they land send none.
    Seeked {
        position: Duration,
    },
    /// The engine applied a [`PlaybackCommand::RemapPaths`]. Events sent
    /// before this one may still carry the old paths; later ones do not.
    PathsRemapped,
    /// A track could not be played, or the engine cannot play at all.
    Error(PlaybackError),
}

/// What playback did after an error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlaybackErrorOutcome {
    /// Playback goes on without the failed track.
    Skipped,
    /// Playback stopped because no other track is left to play.
    Stopped,
    /// Playback stopped because this many tracks failed in a row.
    StoppedAfterFailures(u32),
    /// The engine is not running, so no command is carried out.
    EngineUnavailable,
    /// Playback went on, but the file has not been closed since playback
    /// stopped reading it, behind a read or in a close that has not
    /// returned, so it stays open.
    NotReleased,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlaybackError {
    /// The track that failed, if a track failed.
    pub path: Option<PathBuf>,
    /// What went wrong, such as the `GStreamer` error or the missing codec.
    pub detail: String,
    pub outcome: PlaybackErrorOutcome,
}

impl PlaybackError {
    /// The message shown to the user.
    #[must_use]
    pub fn message(&self) -> String {
        let detail = self.detail.trim().trim_end_matches('.');
        let failure = || match self.path.as_deref().and_then(std::path::Path::file_name) {
            Some(name) => format!(
                "Couldn't play \u{201c}{}\u{201d} ({detail})",
                name.to_string_lossy()
            ),
            None => format!("Playback failed ({detail})"),
        };
        match self.outcome {
            PlaybackErrorOutcome::Skipped => format!("{}; skipped it.", failure()),
            PlaybackErrorOutcome::Stopped => format!("{}; playback stopped.", failure()),
            PlaybackErrorOutcome::StoppedAfterFailures(count) => format!(
                "{}; playback stopped after {count} tracks in a row failed.",
                failure()
            ),
            PlaybackErrorOutcome::EngineUnavailable => {
                format!("Playback is unavailable: {detail}.")
            }
            // The whole path, which names the drive or share that hangs.
            PlaybackErrorOutcome::NotReleased => match self.path.as_deref() {
                Some(path) => format!(
                    "Can't release \u{201c}{}\u{201d}: {detail}.",
                    path.display()
                ),
                None => format!("Can't release a file: {detail}."),
            },
        }
    }
}

/// Rename `path` if `renames` lists it. Returns whether it changed.
pub(crate) fn remap_path(path: &mut PathBuf, renames: &PathRenames) -> bool {
    match renames.get(path) {
        Some(new_path) if new_path != path => {
            path.clone_from(new_path);
            true
        }
        _ => false,
    }
}

/// For a reorder of `len` rows into `order`, which names for each new row
/// the row it was before, the new row of each old row. `None` unless
/// `order` names every row exactly once.
pub(crate) fn reorder_positions(order: &[usize], len: usize) -> Option<Vec<usize>> {
    if order.len() != len {
        return None;
    }
    let mut positions = vec![usize::MAX; len];
    for (position, index) in order.iter().enumerate() {
        let slot = positions.get_mut(*index)?;
        if *slot != usize::MAX {
            return None;
        }
        *slot = position;
    }
    Some(positions)
}

/// Remove a batch in one retain pass and remap the current/selected index.
pub(crate) fn remove_queue_indices<T>(
    queue: &mut Vec<T>,
    current: Option<usize>,
    indices: &[usize],
) -> Option<usize> {
    let mut removed: Vec<usize> = indices
        .iter()
        .copied()
        .filter(|index| *index < queue.len())
        .collect();
    removed.sort_unstable();
    removed.dedup();
    if removed.is_empty() {
        return current;
    }
    let next = current
        .map(|index| index.saturating_sub(removed.partition_point(|removed| *removed < index)));
    let mut index = 0;
    let mut cursor = 0;
    queue.retain(|_| {
        let keep = removed.get(cursor) != Some(&index);
        if !keep {
            cursor += 1;
        }
        index += 1;
        keep
    });
    next.filter(|_| !queue.is_empty())
        .map(|index| index.min(queue.len() - 1))
}

pub struct PlaybackEngine {
    tx: Sender<PlaybackCommand>,
    /// Reports commands that cannot reach the engine.
    event_tx: Sender<PlaybackEvent>,
    /// Why the engine stopped, if it failed to start.
    failure: Arc<OnceLock<String>>,
    /// What the engine left behind when it stopped.
    exit: Receiver<EngineExit>,
}

/// What a playback engine that stopped left behind.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct EngineExit {
    /// Files that were not closed yet, behind a read or in a close that
    /// had not returned, so they stay open.
    pub(crate) unreleased: Vec<PathBuf>,
}

/// How a playback engine stopped; see [`PlaybackEngine::shutdown`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PlaybackShutdown {
    Stopped,
    /// The engine stopped, but reads of these files had not returned.
    Unreleased(Vec<PathBuf>),
    /// The engine did not confirm that it stopped in time.
    Unconfirmed(Duration),
}

impl PlaybackShutdown {
    /// What to tell the user about a shutdown that did not stop cleanly.
    #[must_use]
    pub fn problem(&self) -> Option<String> {
        match self {
            Self::Stopped => None,
            Self::Unreleased(paths) => Some(format!(
                "Couldn't release {} before quitting: reading or closing it had not finished, so \
                 its drive or network share may not be responding.",
                paths
                    .iter()
                    .map(|path| format!("\u{201c}{}\u{201d}", path.display()))
                    .collect::<Vec<_>>()
                    .join(", ")
            )),
            Self::Unconfirmed(timeout) => Some(format!(
                "Playback did not confirm that it stopped within {:.1} s.",
                timeout.as_secs_f64()
            )),
        }
    }
}

fn stop_snapshot_at_terminal_eos(snapshot: &mut PlaybackSnapshot, now: MonotonicTime) {
    snapshot.state = PlaybackState::Stopped;
    snapshot.position = Duration::ZERO;
    snapshot.position_sampled_at = now;
    snapshot.position_advancing = false;
    snapshot.current_bitrate_kbps = None;
}

impl PlaybackEngine {
    #[must_use]
    pub fn new(analysis_tx: Sender<AnalysisCommand>) -> (Self, Receiver<PlaybackEvent>) {
        Self::spawn(|event_tx, failure, exit_tx| {
            backend::spawn_engine(analysis_tx, event_tx, failure, exit_tx)
        })
    }

    /// Start an engine with `start`, which receives the event sender, the
    /// slot for a start failure, and the sender for what the engine leaves
    /// behind when it stops, and returns the command sender.
    fn spawn(
        start: impl FnOnce(
            Sender<PlaybackEvent>,
            &Arc<OnceLock<String>>,
            Sender<EngineExit>,
        ) -> Sender<PlaybackCommand>,
    ) -> (Self, Receiver<PlaybackEvent>) {
        let (event_tx, event_rx) = crossbeam_channel::unbounded();
        let failure = Arc::new(OnceLock::new());
        let (exit_tx, exit) = crossbeam_channel::bounded(1);
        let tx = start(event_tx.clone(), &failure, exit_tx);
        (
            Self {
                tx,
                event_tx,
                failure,
                exit,
            },
            event_rx,
        )
    }

    /// Stop the engine, and wait up to `timeout` for it to tell what it
    /// left behind. Commands sent later are reported as failed.
    pub fn shutdown(&mut self, timeout: Duration) -> PlaybackShutdown {
        // The engine stops once its commands end.
        self.tx = crossbeam_channel::unbounded().0;
        match self.exit.recv_timeout(timeout) {
            Ok(exit) if !exit.unreleased.is_empty() => {
                PlaybackShutdown::Unreleased(exit.unreleased)
            }
            // An engine that could not start, or has stopped already, left
            // nothing behind.
            Ok(_) | Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                PlaybackShutdown::Stopped
            }
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {
                PlaybackShutdown::Unconfirmed(timeout)
            }
        }
    }

    /// Send `cmd` to the engine. When the engine is not running, report an
    /// [`PlaybackErrorOutcome::EngineUnavailable`] error instead, except
    /// for the bridge's own polls.
    pub fn command(&self, cmd: PlaybackCommand) {
        let Err(crossbeam_channel::SendError(cmd)) = self.tx.send(cmd) else {
            return;
        };
        if matches!(cmd, PlaybackCommand::Poll) {
            return;
        }
        let detail = self
            .failure
            .get()
            .cloned()
            .unwrap_or_else(|| "the playback engine has stopped".to_string());
        let _ = self.event_tx.send(PlaybackEvent::Error(PlaybackError {
            path: None,
            detail,
            outcome: PlaybackErrorOutcome::EngineUnavailable,
        }));
    }

    /// An engine whose commands go to `tx` instead of a backend.
    #[cfg(test)]
    pub(crate) fn from_command_sender(tx: Sender<PlaybackCommand>) -> Self {
        Self {
            tx,
            event_tx: crossbeam_channel::unbounded().0,
            failure: Arc::new(OnceLock::new()),
            exit: crossbeam_channel::never(),
        }
    }
}

#[cfg(test)]
mod shared_tests {
    use std::path::PathBuf;
    use std::time::Duration;

    use super::{
        remove_queue_indices, stop_snapshot_at_terminal_eos, volume_gain_to_level,
        volume_level_to_gain, EngineExit, MonotonicTime, PlaybackCommand, PlaybackEngine,
        PlaybackError, PlaybackErrorOutcome, PlaybackEvent, PlaybackShutdown, PlaybackSnapshot,
        PlaybackState,
    };

    #[test]
    fn playback_errors_name_the_track_and_what_playback_did() {
        let error = |path: Option<&str>, detail: &str, outcome| {
            PlaybackError {
                path: path.map(PathBuf::from),
                detail: detail.to_string(),
                outcome,
            }
            .message()
        };
        assert_eq!(
            error(
                Some("/music/b.flac"),
                "Resource not found.",
                PlaybackErrorOutcome::Skipped
            ),
            "Couldn't play \u{201c}b.flac\u{201d} (Resource not found); skipped it."
        );
        assert_eq!(
            error(
                Some("/music/b.flac"),
                "missing GStreamer plugin: FLAC decoder",
                PlaybackErrorOutcome::Stopped
            ),
            "Couldn't play \u{201c}b.flac\u{201d} (missing GStreamer plugin: FLAC decoder); \
             playback stopped."
        );
        assert_eq!(
            error(
                None,
                "decoder failed",
                PlaybackErrorOutcome::StoppedAfterFailures(5)
            ),
            "Playback failed (decoder failed); playback stopped after 5 tracks in a row failed."
        );
        assert_eq!(
            error(
                None,
                "no audio output",
                PlaybackErrorOutcome::EngineUnavailable
            ),
            "Playback is unavailable: no audio output."
        );
        assert_eq!(
            error(
                Some("/mnt/share/b.flac"),
                "reading or closing it has not finished for 15 s",
                PlaybackErrorOutcome::NotReleased
            ),
            "Can't release \u{201c}/mnt/share/b.flac\u{201d}: reading or closing it has not \
             finished for 15 s."
        );
    }

    #[test]
    fn shutdowns_that_left_files_open_or_were_not_confirmed_are_problems() {
        assert_eq!(PlaybackShutdown::Stopped.problem(), None);
        assert_eq!(
            PlaybackShutdown::Unreleased(vec![
                PathBuf::from("/mnt/share/a.flac"),
                PathBuf::from("/mnt/share/b.flac")
            ])
            .problem()
            .as_deref(),
            Some(
                "Couldn't release \u{201c}/mnt/share/a.flac\u{201d}, \u{201c}/mnt/share/b.flac\u{201d} \
                 before quitting: reading or closing it had not finished, so its drive or network \
                 share may not be responding."
            )
        );
        assert_eq!(
            PlaybackShutdown::Unconfirmed(Duration::from_millis(500))
                .problem()
                .as_deref(),
            Some("Playback did not confirm that it stopped within 0.5 s.")
        );
    }

    #[test]
    fn shutdown_reports_what_the_engine_left_behind() {
        let (mut engine, _events) = PlaybackEngine::spawn(|_, _, exit_tx| {
            let (tx, commands) = crossbeam_channel::unbounded::<PlaybackCommand>();
            std::thread::spawn(move || {
                while commands.recv().is_ok() {}
                let _ = exit_tx.send(EngineExit {
                    unreleased: vec![PathBuf::from("/mnt/share/a.flac")],
                });
            });
            tx
        });
        assert_eq!(
            engine.shutdown(Duration::from_secs(10)),
            PlaybackShutdown::Unreleased(vec![PathBuf::from("/mnt/share/a.flac")])
        );

        // An engine that does not stop in time.
        let mut running = None;
        let (mut engine, _events) = PlaybackEngine::spawn(|_, _, exit_tx| {
            running = Some(exit_tx);
            crossbeam_channel::unbounded().0
        });
        assert_eq!(
            engine.shutdown(Duration::from_millis(10)),
            PlaybackShutdown::Unconfirmed(Duration::from_millis(10))
        );
        drop(running);

        // An engine that could not start left nothing behind.
        let (mut engine, _events) =
            PlaybackEngine::spawn(|_, _, _| crossbeam_channel::unbounded().0);
        assert_eq!(
            engine.shutdown(Duration::from_secs(10)),
            PlaybackShutdown::Stopped
        );
    }

    #[test]
    fn commands_to_a_stopped_engine_report_it() {
        let (engine, events) = PlaybackEngine::spawn(|_, failure, _| {
            failure.set("no audio output".to_string()).unwrap();
            // The engine is gone: nothing receives its commands.
            crossbeam_channel::unbounded().0
        });
        engine.command(PlaybackCommand::Poll);
        assert!(events.try_recv().is_err(), "polls are not reported");
        engine.command(PlaybackCommand::Next);
        match events.try_recv() {
            Ok(PlaybackEvent::Error(error)) => {
                assert_eq!(error.outcome, PlaybackErrorOutcome::EngineUnavailable);
                assert_eq!(error.detail, "no audio output");
            }
            other => panic!("expected the engine failure, got {other:?}"),
        }

        let (engine, events) = PlaybackEngine::spawn(|_, _, _| crossbeam_channel::unbounded().0);
        engine.command(PlaybackCommand::Play);
        assert!(matches!(
            events.try_recv(),
            Ok(PlaybackEvent::Error(PlaybackError { detail, .. }))
                if detail == "the playback engine has stopped"
        ));
    }

    #[test]
    fn volume_levels_map_to_cubic_gain_and_back() {
        assert!((volume_level_to_gain(0.5) - 0.125).abs() < 1e-6);
        assert!(volume_level_to_gain(0.0).abs() < f32::EPSILON);
        assert!((volume_level_to_gain(1.0) - 1.0).abs() < f32::EPSILON);
        assert!((volume_level_to_gain(1.7) - 1.0).abs() < f32::EPSILON);
        assert!(volume_level_to_gain(-0.2).abs() < f32::EPSILON);
        let half_level_db = 20.0 * volume_level_to_gain(0.5).log10();
        assert!((half_level_db + 18.06).abs() < 0.01, "{half_level_db}");
        for level in [0.0_f32, 0.05, 0.25, 0.5, 0.8, 1.0] {
            let roundtrip = volume_gain_to_level(volume_level_to_gain(level));
            assert!((roundtrip - level).abs() < 1e-5, "{level} -> {roundtrip}");
        }
    }

    #[test]
    fn batched_removal_remaps_surviving_removed_and_last_current_tracks() {
        for current in 0..6 {
            let mut queue: Vec<_> = (0..6).collect();
            let actual = remove_queue_indices(&mut queue, Some(current), &[1, 4, 5, 5, 999]);
            assert_eq!(queue, vec![0, 2, 3]);
            assert_eq!(actual, Some([0, 1, 1, 2, 2, 2][current]));
        }
        let mut queue = vec![0, 1];
        assert_eq!(remove_queue_indices(&mut queue, Some(1), &[0, 1]), None);
        assert!(queue.is_empty());
    }

    #[test]
    fn muted_channels_mask_defaults_to_zero() {
        let snapshot = PlaybackSnapshot::default();
        assert_eq!(snapshot.muted_channels_mask, 0);
    }

    #[test]
    fn terminal_eos_stop_preserves_current_track_context_for_replay() {
        let track = PathBuf::from("/tmp/final.flac");
        let mut snapshot = PlaybackSnapshot {
            state: PlaybackState::Playing,
            position: Duration::from_secs(271),
            duration: Duration::from_secs(272),
            current: Some(track.clone()),
            current_queue_index: Some(9),
            current_bitrate_kbps: Some(1056),
            volume: 1.0,
            position_sampled_at: MonotonicTime::from_nanos(5),
            position_advancing: true,
            ..PlaybackSnapshot::default()
        };

        stop_snapshot_at_terminal_eos(&mut snapshot, MonotonicTime::from_nanos(9));

        assert_eq!(snapshot.state, PlaybackState::Stopped);
        assert_eq!(snapshot.position, Duration::ZERO);
        assert_eq!(snapshot.position_sampled_at, MonotonicTime::from_nanos(9));
        assert!(!snapshot.position_advancing);
        assert_eq!(snapshot.duration, Duration::from_secs(272));
        assert_eq!(snapshot.current.as_ref(), Some(&track));
        assert_eq!(snapshot.current_queue_index, Some(9));
        assert_eq!(snapshot.current_bitrate_kbps, None);
    }
}

#[cfg(all(test, not(feature = "gst")))]
mod tests {
    use std::path::PathBuf;
    use std::time::{Duration, Instant};

    use crossbeam_channel::unbounded;

    use super::{PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackState, TrackChangeKind};

    fn recv_snapshot(
        rx: &crossbeam_channel::Receiver<PlaybackEvent>,
        timeout: Duration,
    ) -> Option<super::PlaybackSnapshot> {
        let deadline = Instant::now() + timeout;
        let mut last = None;
        while Instant::now() < deadline {
            if let Ok(PlaybackEvent::Snapshot(s)) = rx.recv_timeout(Duration::from_millis(10)) {
                last = Some(s);
            }
        }
        last
    }

    fn recv_seeked_event(
        rx: &crossbeam_channel::Receiver<PlaybackEvent>,
        timeout: Duration,
    ) -> Option<Duration> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Ok(PlaybackEvent::Seeked { position }) =
                rx.recv_timeout(Duration::from_millis(10))
            {
                return Some(position);
            }
        }
        None
    }

    fn make_test_engine() -> (PlaybackEngine, crossbeam_channel::Receiver<PlaybackEvent>) {
        let (analysis_tx, _) = unbounded();

        PlaybackEngine::new(analysis_tx)
    }

    #[test]
    fn batched_removal_preserves_surviving_current_position() {
        let (engine, rx) = make_test_engine();
        let paths: Vec<_> = (0..5)
            .map(|index| PathBuf::from(format!("/fixture/{index}.flac")))
            .collect();
        engine.command(PlaybackCommand::LoadQueue(paths.clone()));
        engine.command(PlaybackCommand::PlayAt(3));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(42)));
        engine.command(PlaybackCommand::RemoveMany(vec![0, 2, 4]));
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("surviving track");
        assert_eq!(snapshot.current, Some(paths[3].clone()));
        assert_eq!(snapshot.current_queue_index, Some(1));
        assert_eq!(snapshot.position, Duration::from_secs(42));
        assert_eq!(snapshot.state, PlaybackState::Paused);
        engine.command(PlaybackCommand::RemoveMany(vec![1]));
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("replacement track");
        assert_eq!(snapshot.current, Some(paths[1].clone()));
        assert_eq!(snapshot.current_queue_index, Some(0));
        assert_eq!(snapshot.position, Duration::ZERO);
    }

    #[test]
    fn a_restored_queue_keeps_the_open_track_or_stops_at_its_current_row() {
        let (engine, rx) = make_test_engine();
        let paths: Vec<_> = (0..4)
            .map(|index| PathBuf::from(format!("/fixture/{index}.flac")))
            .collect();
        engine.command(PlaybackCommand::LoadQueue(paths[1..3].to_vec()));
        engine.command(PlaybackCommand::PlayAt(1));
        engine.command(PlaybackCommand::Play);
        // Paused, so the simulated position stays where the seek put it.
        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(42)));
        engine.command(PlaybackCommand::RestoreQueue {
            paths: paths.clone(),
            current: 2,
        });
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("kept track");
        assert_eq!(snapshot.current, Some(paths[2].clone()));
        assert_eq!(snapshot.current_queue_index, Some(2));
        assert_eq!(snapshot.position, Duration::from_secs(42));
        assert_eq!(snapshot.state, PlaybackState::Paused);

        // The playing track is not in this queue: playback stops at row 1.
        let other = vec![
            PathBuf::from("/fixture/x.flac"),
            PathBuf::from("/fixture/y.flac"),
        ];
        engine.command(PlaybackCommand::RestoreQueue {
            paths: other.clone(),
            current: 1,
        });
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("stopped");
        assert_eq!(snapshot.current, Some(other[1].clone()));
        assert_eq!(snapshot.state, PlaybackState::Stopped);
        assert_eq!(snapshot.position, Duration::ZERO);
    }

    #[test]
    fn remapped_paths_keep_playback_and_are_used_for_later_tracks() {
        let (engine, rx) = make_test_engine();
        let paths: Vec<_> = ["a", "b", "c"]
            .iter()
            .map(|name| PathBuf::from(format!("/fixture/{name}.flac")))
            .collect();
        let renamed_b = PathBuf::from("/fixture/renamed-b.flac");
        let renamed_c = PathBuf::from("/fixture/renamed-c.flac");
        engine.command(PlaybackCommand::LoadQueue(paths.clone()));
        engine.command(PlaybackCommand::PlayAt(1));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(42)));
        let _ = recv_snapshot(&rx, Duration::from_millis(100)).expect("playing b");

        engine.command(PlaybackCommand::RemapPaths(std::sync::Arc::new(
            super::PathRenames::from([
                (paths[1].clone(), renamed_b.clone()),
                (paths[2].clone(), renamed_c.clone()),
            ]),
        )));
        let mut acknowledged = false;
        let mut remapped = None;
        let deadline = Instant::now() + Duration::from_millis(300);
        while remapped.is_none() && Instant::now() < deadline {
            match rx.recv_timeout(Duration::from_millis(10)) {
                Ok(PlaybackEvent::PathsRemapped) => acknowledged = true,
                Ok(PlaybackEvent::Snapshot(snapshot)) if acknowledged => remapped = Some(snapshot),
                Ok(PlaybackEvent::TrackChanged { .. }) => panic!("a rename is not a track change"),
                _ => {}
            }
        }
        let snapshot = remapped.expect("snapshot after the acknowledgement");
        assert_eq!(snapshot.current.as_ref(), Some(&renamed_b));
        assert_eq!(snapshot.current_queue_index, Some(1));
        assert_eq!(snapshot.state, PlaybackState::Playing);
        assert!(snapshot.position >= Duration::from_secs(42));

        engine.command(PlaybackCommand::Next);
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("next track");
        assert_eq!(snapshot.current.as_ref(), Some(&renamed_c));
        engine.command(PlaybackCommand::Stop);
        engine.command(PlaybackCommand::Play);
        let snapshot = recv_snapshot(&rx, Duration::from_millis(100)).expect("replayed");
        assert_eq!(snapshot.current.as_ref(), Some(&renamed_c));
        assert_eq!(snapshot.state, PlaybackState::Playing);
    }

    #[test]
    fn rapid_track_switch_keeps_current_consistent() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        let c = PathBuf::from("/tmp/c.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.command(PlaybackCommand::PlayAt(2));
        engine.command(PlaybackCommand::Previous);
        engine.command(PlaybackCommand::Next);
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&c));
    }

    #[test]
    fn clear_queue_stops_and_resets_snapshot() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.mp3");
        engine.command(PlaybackCommand::LoadQueue(vec![a]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::ClearQueue);
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current, None);
        assert_eq!(snap.state, PlaybackState::Stopped);
        assert_eq!(snap.position, Duration::ZERO);
    }

    #[test]
    fn seek_only_advances_to_next_track_at_duration_boundary() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(120)));
        engine.command(PlaybackCommand::Poll);

        let pre_end = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot pre-end");
        assert_eq!(pre_end.current.as_ref(), Some(&a));

        engine.command(PlaybackCommand::Seek(Duration::from_secs(180)));
        engine.command(PlaybackCommand::Poll);

        let post_end = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot post-end");
        assert_eq!(post_end.current.as_ref(), Some(&b));
    }

    #[test]
    fn boundary_handoff_emits_natural_track_changed_event() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(180)));
        engine.command(PlaybackCommand::Poll);

        let deadline = Instant::now() + Duration::from_millis(300);
        let mut observed = None;
        while Instant::now() < deadline {
            if let Ok(PlaybackEvent::TrackChanged {
                path,
                queue_index: _,
                kind,
                track_token: _,
            }) = rx.recv_timeout(Duration::from_millis(10))
            {
                if path == b && matches!(kind, TrackChangeKind::Gapless) {
                    observed = Some((path, kind));
                    break;
                }
            }
        }

        let (path, kind) = observed.expect("gapless handoff track change");
        assert_eq!(path, b);
        assert!(matches!(kind, TrackChangeKind::Gapless));
    }

    #[test]
    fn set_volume_clamps_to_unit_interval() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        engine.command(PlaybackCommand::SetVolume(1.7));
        engine.command(PlaybackCommand::Poll);
        let high = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot high");
        assert!((high.volume - 1.0).abs() < f32::EPSILON);

        engine.command(PlaybackCommand::SetVolume(-0.4));
        engine.command(PlaybackCommand::Poll);
        let low = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot low");
        assert!((low.volume - 0.0).abs() < f32::EPSILON);
    }

    #[test]
    fn seek_clamps_and_emits_seeked_event() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        engine.command(PlaybackCommand::LoadQueue(vec![PathBuf::from(
            "/tmp/a.flac",
        )]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(999)));

        assert_eq!(
            recv_seeked_event(&rx, Duration::from_millis(300)),
            Some(Duration::from_secs(180))
        );
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.duration, Duration::from_secs(180));
        assert_eq!(snap.position, Duration::from_secs(180));
    }

    #[test]
    fn stopped_seek_is_refused_but_acknowledged() {
        let (engine, rx) = make_test_engine();
        engine.command(PlaybackCommand::LoadQueue(vec![PathBuf::from(
            "/tmp/a.flac",
        )]));
        engine.command(PlaybackCommand::Seek(Duration::from_secs(42)));
        let events = recv_events(&rx, Duration::from_millis(50));
        assert!(!events
            .iter()
            .any(|event| matches!(event, PlaybackEvent::Seeked { .. })));
        let snap = last_snapshot(&events);
        assert_eq!(snap.position, Duration::ZERO);
        assert_eq!((snap.seek_serial, snap.seek_landed_serial), (1, 0));

        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(42)));
        let events = recv_events(&rx, Duration::from_millis(50));
        assert!(events.iter().any(|event| matches!(
            event,
            PlaybackEvent::Seeked { position } if *position == Duration::from_secs(42)
        )));
        let snap = last_snapshot(&events);
        assert_eq!((snap.seek_serial, snap.seek_landed_serial), (2, 2));
    }

    #[test]
    fn add_to_queue_allows_navigation_into_appended_track() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a]));
        engine.command(PlaybackCommand::AddToQueue(vec![b.clone()]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Next);
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&b));
    }

    #[test]
    fn remove_at_preserves_current_track_when_other_row_removed() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        let c = PathBuf::from("/tmp/c.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.command(PlaybackCommand::PlayAt(2));
        engine.command(PlaybackCommand::RemoveAt(0));
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&c));
        assert_eq!(snap.current_queue_index, Some(1));
    }

    #[test]
    fn move_queue_keeps_current_track_identity() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        let c = PathBuf::from("/tmp/c.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.command(PlaybackCommand::PlayAt(2));
        engine.command(PlaybackCommand::MoveQueue { from: 2, to: 0 });
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&c));
        assert_eq!(snap.current_queue_index, Some(0));
    }

    #[test]
    fn play_at_out_of_bounds_keeps_current_track() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.command(PlaybackCommand::PlayAt(1));
        engine.command(PlaybackCommand::PlayAt(99));
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&b));
    }

    #[test]
    fn play_at_without_play_stays_stopped() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.command(PlaybackCommand::PlayAt(1));
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&b));
        assert_eq!(
            snap.state,
            PlaybackState::Stopped,
            "PlayAt alone must not start playback (session restore relies on this)"
        );
    }

    #[test]
    fn previous_at_start_keeps_first_track() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a.clone(), b]));
        engine.command(PlaybackCommand::Previous);
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&a));
    }

    #[test]
    fn paused_navigation_resumes_playback() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        let c = PathBuf::from("/tmp/c.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.command(PlaybackCommand::PlayAt(1));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Next);
        engine.command(PlaybackCommand::Poll);

        let next_snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot next");
        assert_eq!(next_snap.current.as_ref(), Some(&c));
        assert_eq!(next_snap.state, PlaybackState::Playing);

        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Previous);
        engine.command(PlaybackCommand::Poll);

        let prev_snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot previous");
        assert_eq!(prev_snap.current.as_ref(), Some(&b));
        assert_eq!(prev_snap.state, PlaybackState::Playing);
    }

    #[test]
    fn pause_from_stopped_keeps_stopped_state() {
        let (analysis_tx, _analysis_rx) = unbounded();

        let (engine, rx) = PlaybackEngine::new(analysis_tx);

        let a = PathBuf::from("/tmp/a.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a.clone()]));
        engine.command(PlaybackCommand::Stop);
        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Poll);

        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.current.as_ref(), Some(&a));
        assert_eq!(snap.state, PlaybackState::Stopped);
        assert_eq!(snap.position, Duration::ZERO);
    }

    /// Collect events until `timeout` passes without one.
    fn recv_events(
        rx: &crossbeam_channel::Receiver<PlaybackEvent>,
        timeout: Duration,
    ) -> Vec<PlaybackEvent> {
        let mut events = Vec::new();
        while let Ok(event) = rx.recv_timeout(timeout) {
            events.push(event);
        }
        events
    }

    fn track_changes(events: &[PlaybackEvent]) -> Vec<(PathBuf, usize, &'static str, u64)> {
        events
            .iter()
            .filter_map(|event| match event {
                PlaybackEvent::TrackChanged {
                    path,
                    queue_index,
                    kind,
                    track_token,
                } => Some((
                    path.clone(),
                    *queue_index,
                    match kind {
                        TrackChangeKind::Manual => "manual",
                        TrackChangeKind::Gapless => "gapless",
                        TrackChangeKind::Natural => "natural",
                    },
                    *track_token,
                )),
                _ => None,
            })
            .collect()
    }

    fn last_snapshot(events: &[PlaybackEvent]) -> super::PlaybackSnapshot {
        events
            .iter()
            .rev()
            .find_map(|event| match event {
                PlaybackEvent::Snapshot(snapshot) => Some(snapshot.clone()),
                _ => None,
            })
            .expect("snapshot")
    }

    /// Start playing `paths` and enter the about-to-finish window of the
    /// first track: the successor is staged but not audible yet.
    fn play_into_window(
        paths: &[PathBuf],
    ) -> (PlaybackEngine, crossbeam_channel::Receiver<PlaybackEvent>) {
        let (engine, rx) = make_test_engine();
        engine.command(PlaybackCommand::LoadQueue(paths.to_vec()));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_millis(179_900)));
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        let snapshot = last_snapshot(&events);
        assert_eq!(snapshot.current.as_ref(), Some(&paths[0]));
        assert_eq!(snapshot.current_queue_index, Some(0));
        (engine, rx)
    }

    #[test]
    fn navigation_inside_the_window_starts_from_the_audible_track() {
        let paths: Vec<_> = ["a", "b", "c"]
            .iter()
            .map(|name| PathBuf::from(format!("/fixture/{name}.flac")))
            .collect();
        let (engine, rx) = play_into_window(&paths);
        engine.command(PlaybackCommand::Next);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert_eq!(
            track_changes(&events),
            vec![(paths[1].clone(), 1, "manual", 2)]
        );

        let (engine, rx) = play_into_window(&paths);
        engine.command(PlaybackCommand::Stop);
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert!(track_changes(&events).is_empty());
        let snapshot = last_snapshot(&events);
        assert_eq!(snapshot.current.as_ref(), Some(&paths[0]));
        assert_eq!(snapshot.current_queue_index, Some(0));
        assert!(snapshot.position < Duration::from_secs(1));
    }

    #[test]
    fn staged_successor_commits_at_the_boundary_after_queue_edits() {
        let paths: Vec<_> = ["a", "b", "c"]
            .iter()
            .map(|name| PathBuf::from(format!("/fixture/{name}.flac")))
            .collect();
        let (engine, rx) = play_into_window(&paths);
        engine.command(PlaybackCommand::RemoveAt(2));
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(150));
        assert!(track_changes(&events).is_empty(), "{events:?}");
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert_eq!(
            track_changes(&events),
            vec![(paths[1].clone(), 1, "gapless", 2)]
        );
        let snapshot = last_snapshot(&events);
        assert_eq!(snapshot.current_queue_index, Some(1));
        assert_eq!(snapshot.track_token, 2);
    }

    #[test]
    fn repeat_one_and_duplicate_rows_report_every_play() {
        let a = PathBuf::from("/fixture/a.flac");
        let (engine, rx) = make_test_engine();
        engine.command(PlaybackCommand::SetRepeatMode(super::RepeatMode::One));
        engine.command(PlaybackCommand::LoadQueue(vec![a.clone(), a.clone()]));
        engine.command(PlaybackCommand::Play);
        for _ in 0..2 {
            engine.command(PlaybackCommand::Seek(Duration::from_secs(180)));
            engine.command(PlaybackCommand::Poll);
        }
        engine.command(PlaybackCommand::SetRepeatMode(super::RepeatMode::Off));
        engine.command(PlaybackCommand::Seek(Duration::from_secs(180)));
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert_eq!(
            track_changes(&events),
            vec![
                (a.clone(), 0, "manual", 1),
                (a.clone(), 0, "gapless", 2),
                (a.clone(), 0, "gapless", 3),
                (a.clone(), 1, "gapless", 4),
            ]
        );
        assert_eq!(last_snapshot(&events).track_token, 4);
    }

    #[test]
    fn end_of_stream_starts_cross_format_and_late_successors() {
        let a = PathBuf::from("/fixture/a.flac");
        let b = PathBuf::from("/fixture/b.mp3");
        let c = PathBuf::from("/fixture/c.flac");
        let (engine, rx) = play_into_window(&[a, b.clone()]);
        engine.command(PlaybackCommand::AddToQueue(vec![c.clone()]));
        std::thread::sleep(Duration::from_millis(150));
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert_eq!(track_changes(&events), vec![(b, 1, "natural", 2)]);

        engine.command(PlaybackCommand::Seek(Duration::from_millis(179_900)));
        engine.command(PlaybackCommand::Poll);
        engine.command(PlaybackCommand::RemoveAt(2));
        engine.command(PlaybackCommand::AddToQueue(vec![c.clone()]));
        std::thread::sleep(Duration::from_millis(150));
        engine.command(PlaybackCommand::Poll);
        let events = recv_events(&rx, Duration::from_millis(50));
        assert_eq!(track_changes(&events), vec![(c, 2, "natural", 3)]);
    }

    #[test]
    fn solo_channel_mutes_all_others() {
        let (engine, rx) = make_test_engine();
        engine.command(PlaybackCommand::SoloChannel(1));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, u64::MAX ^ (1 << 1));
    }

    #[test]
    fn unsolo_restores_previous_mask() {
        let (engine, rx) = make_test_engine();
        // Mute channel 0 first.
        engine.command(PlaybackCommand::ToggleChannelMute(0));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Solo channel 2 — should save mask with bit 0 set.
        engine.command(PlaybackCommand::SoloChannel(2));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Un-solo channel 2 — should restore: only bit 0 set.
        engine.command(PlaybackCommand::SoloChannel(2));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, 1 << 0);
    }

    #[test]
    fn solo_switch_to_different_channel_keeps_original_pre_mask() {
        let (engine, rx) = make_test_engine();
        // Mute channel 3.
        engine.command(PlaybackCommand::ToggleChannelMute(3));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Solo channel 0.
        engine.command(PlaybackCommand::SoloChannel(0));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Switch solo to channel 1.
        engine.command(PlaybackCommand::SoloChannel(1));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Un-solo channel 1 — should restore original pre-mask (bit 3 set).
        engine.command(PlaybackCommand::SoloChannel(1));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, 1 << 3);
    }

    #[test]
    fn manual_toggle_clears_solo_state() {
        let (engine, rx) = make_test_engine();
        // Solo channel 0.
        engine.command(PlaybackCommand::SoloChannel(0));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Manual toggle channel 2 — should clear solo, XOR bit 2 into current mask.
        engine.command(PlaybackCommand::ToggleChannelMute(2));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Mask was all-except-0; toggling bit 2 clears it.
        let expected = (u64::MAX ^ (1 << 0)) ^ (1 << 2);
        assert_eq!(snap.muted_channels_mask, expected);
        // Now "solo channel 0 again" should act as a fresh solo (no restore).
        engine.command(PlaybackCommand::SoloChannel(0));
        engine.command(PlaybackCommand::Poll);
        let snap2 = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap2.muted_channels_mask, u64::MAX ^ (1 << 0));
        // Un-solo should restore the post-toggle mask, not the original.
        engine.command(PlaybackCommand::SoloChannel(0));
        engine.command(PlaybackCommand::Poll);
        let snap3 = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap3.muted_channels_mask, expected);
    }

    #[test]
    fn toggle_soloed_channel_unsolos() {
        let (engine, rx) = make_test_engine();
        // Mute channel 1 first, then solo channel 0.
        engine.command(PlaybackCommand::ToggleChannelMute(1));
        engine.command(PlaybackCommand::SoloChannel(0));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Single-click (toggle) the soloed channel — should unsolo,
        // restoring the pre-solo mask (bit 1 set).
        engine.command(PlaybackCommand::ToggleChannelMute(0));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, 1 << 1);
    }

    #[test]
    fn manual_track_switch_resets_mute_and_solo() {
        let (engine, rx) = make_test_engine();
        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a, b]));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Solo channel 1.
        engine.command(PlaybackCommand::SoloChannel(1));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_ne!(snap.muted_channels_mask, 0);
        assert_eq!(snap.soloed_channel, Some(1));
        // Switch to track B via Next.
        engine.command(PlaybackCommand::Next);
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, 0);
        assert_eq!(snap.soloed_channel, None);
    }

    #[test]
    fn stop_resets_mute_and_solo() {
        let (engine, rx) = make_test_engine();
        engine.command(PlaybackCommand::ToggleChannelMute(0));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_ne!(snap.muted_channels_mask, 0);
        engine.command(PlaybackCommand::Stop);
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.muted_channels_mask, 0);
        assert_eq!(snap.soloed_channel, None);
    }

    #[test]
    fn play_pause_preserves_mute_and_solo() {
        let (engine, rx) = make_test_engine();
        let a = PathBuf::from("/tmp/a.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a]));
        engine.command(PlaybackCommand::Poll);
        let _ = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        // Solo channel 2.
        engine.command(PlaybackCommand::SoloChannel(2));
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.soloed_channel, Some(2));
        assert_ne!(snap.muted_channels_mask, 0);
        // Pause and resume — mute/solo must survive.
        engine.command(PlaybackCommand::Pause);
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Poll);
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.soloed_channel, Some(2));
        assert_ne!(snap.muted_channels_mask, 0);
    }
}

#[cfg(feature = "gst")]
mod collection_order;
#[cfg(feature = "gst")]
pub(crate) mod file_source;
#[cfg(feature = "gst")]
pub(crate) mod parsebin_stall;
#[cfg(all(test, feature = "gst"))]
pub(crate) mod pipeline_trace;
mod queue;
#[cfg(feature = "gst")]
pub(crate) mod source_activation;

#[cfg(not(feature = "gst"))]
#[path = "backend_mock.rs"]
mod backend;

#[cfg(feature = "gst")]
#[path = "backend_gst.rs"]
mod backend;
