// SPDX-License-Identifier: GPL-3.0-or-later

use super::queue::{same_audio_extension, CancelledAdvance, GaplessQueue, Handoff};
use super::{
    EngineExit, FileStamp, PlaybackCommand, PlaybackError, PlaybackErrorOutcome, PlaybackEvent,
    PlaybackSnapshot, PlaybackState, TrackChangeKind,
};
use crate::analysis::AnalysisCommand;
use crossbeam_channel::{unbounded, Sender};
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

/// Length of every simulated track.
const TRACK_DURATION: Duration = Duration::from_secs(180);

/// How long before the end of a track the simulation stages its successor,
/// as playbin3 does when it emits `about-to-finish`.
const ABOUT_TO_FINISH_LEAD: Duration = Duration::from_secs(2);

/// Spawn the simulation. It cannot fail to start, so it leaves `_failure`
/// unset. It reads no files, so it leaves none open when it stops.
pub fn spawn_engine(
    analysis_tx: Sender<AnalysisCommand>,
    event_tx: Sender<PlaybackEvent>,
    _failure: &Arc<OnceLock<String>>,
    exit_tx: Sender<EngineExit>,
) -> Sender<PlaybackCommand> {
    spawn_with_clock(analysis_tx, event_tx, exit_tx, Instant::now)
}

/// Spawn the simulation with time that passes only when the test advances
/// `clock`, so a test decides when playback reaches a track boundary.
#[cfg(test)]
pub(super) fn spawn_engine_with_clock(
    analysis_tx: Sender<AnalysisCommand>,
    event_tx: Sender<PlaybackEvent>,
    exit_tx: Sender<EngineExit>,
    clock: &ManualClock,
) -> Sender<PlaybackCommand> {
    let clock = clock.clone();
    spawn_with_clock(analysis_tx, event_tx, exit_tx, move || clock.now())
}

/// Simulated time for tests: it starts when created and advances only by
/// [`ManualClock::advance`].
#[cfg(test)]
#[derive(Clone)]
pub(super) struct ManualClock {
    start: Instant,
    elapsed_nanos: Arc<std::sync::atomic::AtomicU64>,
}

#[cfg(test)]
impl ManualClock {
    pub(super) fn new() -> Self {
        Self {
            start: Instant::now(),
            elapsed_nanos: Arc::default(),
        }
    }

    pub(super) fn advance(&self, by: Duration) {
        let nanos = u64::try_from(by.as_nanos()).expect("a test advances by less than 584 years");
        self.elapsed_nanos
            .fetch_add(nanos, std::sync::atomic::Ordering::SeqCst);
    }

    fn now(&self) -> Instant {
        self.start
            + Duration::from_nanos(self.elapsed_nanos.load(std::sync::atomic::Ordering::SeqCst))
    }
}

/// Run the simulation, reading the time that passes while it plays from
/// `now`.
fn spawn_with_clock(
    analysis_tx: Sender<AnalysisCommand>,
    event_tx: Sender<PlaybackEvent>,
    exit_tx: Sender<EngineExit>,
    now: impl Fn() -> Instant + Send + 'static,
) -> Sender<PlaybackCommand> {
    let (cmd_tx, cmd_rx) = unbounded();
    crate::worker_thread::spawn_worker("ferrous-playback-sim", move || {
        let mut runtime = MockPlaybackRuntime {
            snapshot: PlaybackSnapshot {
                volume: 1.0,
                ..PlaybackSnapshot::default()
            },
            queue: GaplessQueue::new(),
            about_to_finish_fired: false,
            track_token_counter: 0,
            solo_pre_mask: None,
            soloed_channel: None,
            event_tx,
            analysis_tx,
        };
        let mut last_tick = now();
        while let Ok(cmd) = cmd_rx.recv() {
            let tick = now();
            if runtime.snapshot.state == PlaybackState::Playing {
                runtime.snapshot.position = runtime
                    .snapshot
                    .position
                    .saturating_add(tick.saturating_duration_since(last_tick));
            }
            last_tick = tick;
            runtime.handle_command(cmd);
            // The simulated position is where playback is now, and
            // advances while it plays.
            runtime.snapshot.position_sampled_at = super::monotonic_now();
            runtime.snapshot.position_advancing = runtime.snapshot.state == PlaybackState::Playing;
            let _ = runtime
                .event_tx
                .send(PlaybackEvent::Snapshot(runtime.snapshot.clone()));
        }
        let _ = exit_tx.send(EngineExit::default());
    });
    cmd_tx
}

/// Simulated playback. Time advances between commands, and `Poll` plays
/// the role of the `GStreamer` bus: it stages the successor shortly before
/// the end of a track and commits it at the boundary.
struct MockPlaybackRuntime {
    snapshot: PlaybackSnapshot,
    queue: GaplessQueue,
    /// The simulated pipeline asked for the successor of the current
    /// stream. It asks again only for a restarted or new stream.
    about_to_finish_fired: bool,
    track_token_counter: u64,
    solo_pre_mask: Option<u64>,
    soloed_channel: Option<u8>,
    event_tx: Sender<PlaybackEvent>,
    analysis_tx: Sender<AnalysisCommand>,
}

impl MockPlaybackRuntime {
    fn reset_mute(&mut self) {
        self.snapshot.muted_channels_mask = 0;
        self.snapshot.soloed_channel = None;
        self.solo_pre_mask = None;
        self.soloed_channel = None;
    }
    fn handle_command(&mut self, cmd: PlaybackCommand) {
        match cmd {
            PlaybackCommand::LoadQueue(paths) => self.load_queue(paths),
            PlaybackCommand::AddToQueue(paths) => {
                let cancelled = self.queue.add_to_queue(paths);
                self.continue_after_cancelled_advance(cancelled);
            }
            PlaybackCommand::RemapPaths(renames) => {
                self.queue.remap_paths(&renames);
                if let Some(current) = self.snapshot.current.as_mut() {
                    super::remap_path(current, &renames);
                }
                // The loop sends the remapped snapshot after this.
                let _ = self.event_tx.send(PlaybackEvent::PathsRemapped);
            }
            PlaybackCommand::RestoreQueue { paths, current } => self.restore_queue(paths, current),
            PlaybackCommand::RemoveMany(indices) => self.remove_many(&indices),
            PlaybackCommand::RemoveAt(idx) => self.remove_many(&[idx]),
            PlaybackCommand::MoveQueue { from, to } => {
                let cancelled = self.queue.move_item(from, to);
                self.continue_after_cancelled_advance(cancelled);
                self.snapshot.current_queue_index = self.queue.current_index();
            }
            PlaybackCommand::ReorderQueue(order) => {
                let cancelled = self.queue.reorder(&order);
                self.continue_after_cancelled_advance(cancelled);
                self.snapshot.current_queue_index = self.queue.current_index();
            }
            PlaybackCommand::InsertIntoQueue { at, paths } => {
                let cancelled = self.queue.insert(at, paths);
                self.continue_after_cancelled_advance(cancelled);
                self.snapshot.current_queue_index = self.queue.current_index();
            }
            PlaybackCommand::ClearQueue => {
                let _ = self.queue.clear();
                self.stop_with_empty_queue();
            }
            PlaybackCommand::PlayAt(idx) => {
                if let Some(target) = self.queue.set_current(idx) {
                    self.switch_manually(target.path, target.index);
                }
            }
            PlaybackCommand::Next => self.advance_manually(true),
            PlaybackCommand::Previous => self.advance_manually(false),
            PlaybackCommand::Play => {
                self.snapshot.state = PlaybackState::Playing;
                self.snapshot.current_queue_index = if self.snapshot.current.is_some() {
                    self.queue.current_index()
                } else {
                    None
                };
            }
            PlaybackCommand::Pause => {
                if self.snapshot.state == PlaybackState::Playing {
                    self.snapshot.state = PlaybackState::Paused;
                }
            }
            PlaybackCommand::Stop => {
                let _ = self.queue.cancel_pending();
                self.about_to_finish_fired = false;
                self.reset_mute();
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.position = Duration::ZERO;
                self.snapshot.current_queue_index = None;
            }
            PlaybackCommand::Seek(pos) => {
                self.snapshot.seek_serial += 1;
                // Like the GStreamer backend, a stopped track is not
                // seekable.
                if self.snapshot.current.is_none() || self.snapshot.state == PlaybackState::Stopped
                {
                    return;
                }
                // Like the GStreamer backend, keep the seek away from a
                // successor the pipeline was given.
                self.queue.hold_handoffs();
                let _ = self.queue.cancel_pending();
                self.queue.release_handoffs();
                self.about_to_finish_fired = false;
                self.snapshot.position = pos.min(self.snapshot.duration);
                self.snapshot.seek_landed_serial = self.snapshot.seek_serial;
                let _ = self.event_tx.send(PlaybackEvent::Seeked {
                    position: self.snapshot.position,
                });
            }
            PlaybackCommand::Cue { position, expected } => self.cue(position, expected),
            PlaybackCommand::SetVolume(vol) => {
                self.snapshot.volume = vol.clamp(0.0, 1.0);
            }
            PlaybackCommand::SetRepeatMode(mode) => {
                let cancelled = self.queue.set_repeat_mode(mode);
                self.continue_after_cancelled_advance(cancelled);
                self.snapshot.repeat_mode = self.queue.repeat_mode();
            }
            PlaybackCommand::SetShuffle(enabled) => {
                let cancelled = self.queue.set_shuffle_enabled(enabled);
                self.continue_after_cancelled_advance(cancelled);
                self.snapshot.shuffle_enabled = self.queue.shuffle_enabled();
            }
            PlaybackCommand::ToggleChannelMute(ch) => self.toggle_channel_mute(ch),
            PlaybackCommand::SoloChannel(ch) => self.solo_channel(ch),
            PlaybackCommand::Poll => self.poll(),
        }
    }
    /// The `GStreamer` backend restarts the current track when it drops a
    /// successor the pipeline was given, which asks for a successor again.
    fn continue_after_cancelled_advance(&mut self, cancelled: Option<CancelledAdvance>) {
        if cancelled.is_some_and(|cancelled| cancelled.handed_to_pipeline) {
            self.about_to_finish_fired = false;
        }
    }
    fn change_track(&mut self, path: PathBuf, queue_index: usize, kind: TrackChangeKind) {
        self.about_to_finish_fired = false;
        self.track_token_counter += 1;
        let track_token = self.track_token_counter;
        self.snapshot.current_file = FileStamp::of(&path);
        self.snapshot.current = Some(path.clone());
        self.snapshot.current_queue_index = Some(queue_index);
        self.snapshot.duration = TRACK_DURATION;
        self.snapshot.track_token = track_token;
        let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
            path,
            queue_index,
            kind,
            track_token,
        });
    }
    fn switch_manually(&mut self, path: PathBuf, queue_index: usize) {
        self.reset_mute();
        self.snapshot.position = Duration::ZERO;
        self.change_track(path, queue_index, TrackChangeKind::Manual);
    }
    fn load_queue(&mut self, paths: Vec<PathBuf>) {
        if paths.is_empty() {
            return;
        }
        let _ = self.queue.set_queue(paths);
        if let (Some(path), Some(index)) = (self.queue.current(), self.queue.current_index()) {
            self.switch_manually(path, index);
            let _ = self
                .analysis_tx
                .send(AnalysisCommand::SetSampleRate(48_000));
        }
    }
    /// Like the `GStreamer` backend; see [`PlaybackCommand::RestoreQueue`].
    fn restore_queue(&mut self, paths: Vec<PathBuf>, current: usize) {
        let Some(last) = paths.len().checked_sub(1) else {
            return;
        };
        let current = current.min(last);
        let open_row = self.snapshot.current.as_ref().and_then(|open| {
            if paths.get(current) == Some(open) {
                Some(current)
            } else {
                paths.iter().position(|path| path == open)
            }
        });
        let Some(row) = open_row else {
            let _ = self.queue.set_queue(paths);
            self.about_to_finish_fired = false;
            self.snapshot.state = PlaybackState::Stopped;
            if let Some(target) = self.queue.set_current(current) {
                self.switch_manually(target.path, target.index);
            }
            return;
        };
        let cancelled = self.queue.restore(paths, row);
        self.continue_after_cancelled_advance(cancelled);
        if self.snapshot.state != PlaybackState::Stopped {
            self.snapshot.current_queue_index = Some(row);
        }
    }
    fn remove_many(&mut self, indices: &[usize]) {
        let removal = self.queue.remove_many(indices);
        match (self.queue.current(), self.queue.current_index()) {
            (Some(path), Some(index)) if removal.current_removed => {
                self.switch_manually(path, index);
            }
            (Some(_), index) => {
                self.continue_after_cancelled_advance(removal.cancelled);
                self.snapshot.current_queue_index = index;
            }
            _ => self.stop_with_empty_queue(),
        }
    }
    fn stop_with_empty_queue(&mut self) {
        self.reset_mute();
        self.snapshot.current = None;
        self.snapshot.current_file = None;
        self.snapshot.current_queue_index = None;
        self.snapshot.state = PlaybackState::Stopped;
        self.snapshot.position = Duration::ZERO;
        self.snapshot.duration = Duration::ZERO;
        self.snapshot.track_token = 0;
    }
    /// Like the `GStreamer` backend; see [`PlaybackCommand::Cue`].
    fn cue(&mut self, position: Duration, expected: FileStamp) {
        let Some(path) = self
            .snapshot
            .current
            .clone()
            .filter(|_| self.snapshot.state == PlaybackState::Stopped)
        else {
            return;
        };
        match self.snapshot.current_file {
            None => {
                let _ = self.event_tx.send(PlaybackEvent::Error(PlaybackError {
                    detail: FileStamp::missing_detail(&path),
                    path: Some(path),
                    outcome: PlaybackErrorOutcome::NotResumed,
                }));
                return;
            }
            Some(stamp) if stamp != expected => return,
            Some(_) => {}
        }
        self.snapshot.state = PlaybackState::Paused;
        self.snapshot.current_queue_index = self.queue.current_index();
        self.snapshot.position = position.min(self.snapshot.duration);
        let _ = self.event_tx.send(PlaybackEvent::Seeked {
            position: self.snapshot.position,
        });
    }
    fn advance_manually(&mut self, next: bool) {
        let target = if next {
            self.queue.next_manual()
        } else {
            self.queue.previous_manual()
        };
        if let Some(target) = target {
            if self.snapshot.state == PlaybackState::Paused {
                self.snapshot.state = PlaybackState::Playing;
            }
            self.switch_manually(target.path, target.index);
        }
    }
    fn toggle_channel_mute(&mut self, ch: u8) {
        let ch = ch.min(63);
        if self.soloed_channel == Some(ch) {
            if let Some(pre) = self.solo_pre_mask.take() {
                self.snapshot.muted_channels_mask = pre;
            }
            self.soloed_channel = None;
        } else {
            self.snapshot.muted_channels_mask ^= 1u64 << ch;
            self.solo_pre_mask = None;
            self.soloed_channel = None;
        }
        self.snapshot.soloed_channel = self.soloed_channel;
    }
    fn solo_channel(&mut self, ch: u8) {
        let ch = ch.min(63);
        if self.soloed_channel == Some(ch) {
            // self.solo_pre_mask is always Some when self.soloed_channel is Some.
            if let Some(pre) = self.solo_pre_mask.take() {
                self.snapshot.muted_channels_mask = pre;
            }
            self.soloed_channel = None;
        } else {
            if self.soloed_channel.is_none() {
                self.solo_pre_mask = Some(self.snapshot.muted_channels_mask);
            }
            self.snapshot.muted_channels_mask = u64::MAX ^ (1u64 << ch);
            self.soloed_channel = Some(ch);
        }
        self.snapshot.soloed_channel = self.soloed_channel;
    }
    fn poll(&mut self) {
        if self.snapshot.state != PlaybackState::Playing {
            return;
        }
        let remaining = self
            .snapshot
            .duration
            .saturating_sub(self.snapshot.position);
        if remaining <= ABOUT_TO_FINISH_LEAD && !self.about_to_finish_fired {
            self.about_to_finish_fired = true;
            let _ = self.queue.stage_natural(|finishing, next| {
                if same_audio_extension(finishing, next) {
                    Handoff::Gapless
                } else {
                    Handoff::AtEos
                }
            });
        }
        if remaining > Duration::ZERO {
            return;
        }
        if let Some(committed) = self.queue.commit_gapless() {
            self.snapshot.position = self
                .snapshot
                .position
                .saturating_sub(self.snapshot.duration);
            self.change_track(committed.path, committed.index, TrackChangeKind::Gapless);
        } else if let Some(successor) = self.queue.take_eos_successor() {
            // A cross-format successor, or one appended after staging found
            // none, starts after end of stream.
            self.snapshot.position = Duration::ZERO;
            self.change_track(successor.path, successor.index, TrackChangeKind::Natural);
        } else {
            super::stop_snapshot_at_terminal_eos(&mut self.snapshot, super::monotonic_now());
        }
    }
}
