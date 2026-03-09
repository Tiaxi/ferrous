use std::path::PathBuf;
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};

use crate::analysis::{AnalysisCommand, AnalysisPcmChunk};

#[cfg(feature = "profiling-logs")]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {
        eprintln!($($arg)*);
    };
}

#[cfg(not(feature = "profiling-logs"))]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {};
}

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

#[derive(Debug, Clone, Default, PartialEq)]
pub struct PlaybackSnapshot {
    pub state: PlaybackState,
    pub position: Duration,
    pub duration: Duration,
    pub current: Option<PathBuf>,
    pub current_queue_index: Option<usize>,
    pub volume: f32,
    pub repeat_mode: RepeatMode,
    pub shuffle_enabled: bool,
}

#[derive(Debug, Clone)]
pub enum PlaybackCommand {
    LoadQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    RemoveAt(usize),
    MoveQueue { from: usize, to: usize },
    ClearQueue,
    PlayAt(usize),
    Next,
    Previous,
    Play,
    Pause,
    Stop,
    Seek(Duration),
    SetVolume(f32),
    SetRepeatMode(RepeatMode),
    SetShuffle(bool),
    Poll,
}

#[derive(Debug, Clone)]
pub enum TrackChangeKind {
    Manual,
    Natural,
}

#[derive(Debug, Clone)]
pub enum PlaybackEvent {
    Snapshot(PlaybackSnapshot),
    TrackChanged {
        path: PathBuf,
        queue_index: usize,
        kind: TrackChangeKind,
    },
    Seeked,
}

pub struct PlaybackEngine {
    tx: Sender<PlaybackCommand>,
}

impl PlaybackEngine {
    #[must_use]
    pub fn new(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> (Self, Receiver<PlaybackEvent>) {
        let (tx, rx) = backend::spawn_engine(analysis_tx, pcm_tx);
        (Self { tx }, rx)
    }

    pub fn command(&self, cmd: PlaybackCommand) {
        let _ = self.tx.send(cmd);
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
            if let Ok(evt) = rx.recv_timeout(Duration::from_millis(10)) {
                if let PlaybackEvent::Snapshot(s) = evt {
                    last = Some(s);
                }
            }
        }
        last
    }

    fn saw_seeked_event(
        rx: &crossbeam_channel::Receiver<PlaybackEvent>,
        timeout: Duration,
    ) -> bool {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Ok(evt) = rx.recv_timeout(Duration::from_millis(10)) {
                if matches!(evt, PlaybackEvent::Seeked) {
                    return true;
                }
            }
        }
        false
    }

    #[test]
    fn rapid_track_switch_keeps_current_consistent() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

        let a = PathBuf::from("/tmp/a.flac");
        let b = PathBuf::from("/tmp/b.flac");
        engine.command(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(180)));
        engine.command(PlaybackCommand::Poll);

        let deadline = Instant::now() + Duration::from_millis(300);
        let mut observed = None;
        while Instant::now() < deadline {
            if let Ok(evt) = rx.recv_timeout(Duration::from_millis(10)) {
                if let PlaybackEvent::TrackChanged {
                    path,
                    queue_index: _,
                    kind,
                } = evt
                {
                    if path == b && matches!(kind, TrackChangeKind::Natural) {
                        observed = Some((path, kind));
                        break;
                    }
                }
            }
        }

        let (path, kind) = observed.expect("natural handoff track change");
        assert_eq!(path, b);
        assert!(matches!(kind, TrackChangeKind::Natural));
    }

    #[test]
    fn set_volume_clamps_to_unit_interval() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

        engine.command(PlaybackCommand::LoadQueue(vec![PathBuf::from(
            "/tmp/a.flac",
        )]));
        engine.command(PlaybackCommand::Play);
        engine.command(PlaybackCommand::Seek(Duration::from_secs(999)));

        assert!(saw_seeked_event(&rx, Duration::from_millis(300)));
        let snap = recv_snapshot(&rx, Duration::from_millis(300)).expect("snapshot");
        assert_eq!(snap.duration, Duration::from_secs(180));
        assert_eq!(snap.position, Duration::from_secs(180));
    }

    #[test]
    fn add_to_queue_allows_navigation_into_appended_track() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
    fn previous_at_start_keeps_first_track() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
}

#[cfg(not(feature = "gst"))]
mod backend {
    use std::f32::consts::PI;
    use std::path::PathBuf;
    use std::time::{Duration, Instant};

    use crossbeam_channel::{unbounded, Receiver, Sender};

    use crate::analysis::{AnalysisCommand, AnalysisPcmChunk, SpectrogramChannelLabel};

    use super::{PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, TrackChangeKind};

    pub fn spawn_engine(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
        let (event_tx, event_rx) = unbounded::<PlaybackEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-playback-sim".to_string())
            .spawn(move || {
                let mut snapshot = PlaybackSnapshot {
                    volume: 1.0,
                    ..PlaybackSnapshot::default()
                };
                let mut queue: Vec<PathBuf> = Vec::new();
                let mut queue_idx = 0usize;
                let mut last_tick = Instant::now();
                let mut phase = 0.0f32;

                while let Ok(cmd) = cmd_rx.recv() {
                    if snapshot.state == PlaybackState::Playing {
                        let delta = last_tick.elapsed();
                        snapshot.position = snapshot.position.saturating_add(delta);
                    }
                    last_tick = Instant::now();

                    match cmd {
                        PlaybackCommand::LoadQueue(paths) => {
                            queue = paths;
                            queue_idx = 0;
                            snapshot.position = Duration::ZERO;
                            snapshot.duration = Duration::from_secs(180);
                            snapshot.current = queue.first().cloned();
                            snapshot.current_queue_index = if snapshot.current.is_some() {
                                Some(queue_idx)
                            } else {
                                None
                            };
                            if let Some(path) = snapshot.current.clone() {
                                let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                    path,
                                    queue_index: queue_idx,
                                    kind: TrackChangeKind::Manual,
                                });
                                let _ = analysis_tx.send(AnalysisCommand::SetSampleRate(48_000));
                            }
                        }
                        PlaybackCommand::AddToQueue(paths) => {
                            queue.extend(paths);
                        }
                        PlaybackCommand::RemoveAt(idx) => {
                            if idx < queue.len() {
                                queue.remove(idx);
                                if queue.is_empty() {
                                    queue_idx = 0;
                                    snapshot.current = None;
                                    snapshot.current_queue_index = None;
                                    snapshot.state = PlaybackState::Stopped;
                                    snapshot.position = Duration::ZERO;
                                    snapshot.duration = Duration::ZERO;
                                } else {
                                    if idx < queue_idx {
                                        queue_idx = queue_idx.saturating_sub(1);
                                    } else if idx == queue_idx && queue_idx >= queue.len() {
                                        queue_idx = queue.len().saturating_sub(1);
                                    }
                                    snapshot.current = queue.get(queue_idx).cloned();
                                    snapshot.current_queue_index = Some(queue_idx);
                                    snapshot.position = Duration::ZERO;
                                    snapshot.duration = Duration::from_secs(180);
                                    if let Some(path) = snapshot.current.clone() {
                                        let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                            path,
                                            queue_index: queue_idx,
                                            kind: TrackChangeKind::Manual,
                                        });
                                    }
                                }
                            }
                        }
                        PlaybackCommand::MoveQueue { from, to } => {
                            if from < queue.len() && to < queue.len() && from != to {
                                let item = queue.remove(from);
                                queue.insert(to, item);
                                if queue_idx == from {
                                    queue_idx = to;
                                } else if from < queue_idx && to >= queue_idx {
                                    queue_idx = queue_idx.saturating_sub(1);
                                } else if from > queue_idx && to <= queue_idx {
                                    queue_idx += 1;
                                }
                                snapshot.current = queue.get(queue_idx).cloned();
                                snapshot.current_queue_index = if snapshot.current.is_some() {
                                    Some(queue_idx)
                                } else {
                                    None
                                };
                            }
                        }
                        PlaybackCommand::ClearQueue => {
                            queue.clear();
                            queue_idx = 0;
                            snapshot.current = None;
                            snapshot.current_queue_index = None;
                            snapshot.state = PlaybackState::Stopped;
                            snapshot.position = Duration::ZERO;
                            snapshot.duration = Duration::ZERO;
                        }
                        PlaybackCommand::PlayAt(idx) => {
                            if let Some(path) = queue.get(idx).cloned() {
                                queue_idx = idx;
                                snapshot.current = Some(path.clone());
                                snapshot.current_queue_index = Some(queue_idx);
                                snapshot.position = Duration::ZERO;
                                snapshot.duration = Duration::from_secs(180);
                                let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                    path,
                                    queue_index: queue_idx,
                                    kind: TrackChangeKind::Manual,
                                });
                            }
                        }
                        PlaybackCommand::Next => {
                            if queue_idx + 1 < queue.len() {
                                queue_idx += 1;
                                if let Some(next) = queue.get(queue_idx).cloned() {
                                    snapshot.current = Some(next.clone());
                                    snapshot.current_queue_index = Some(queue_idx);
                                    snapshot.position = Duration::ZERO;
                                    snapshot.duration = Duration::from_secs(180);
                                    if snapshot.state == PlaybackState::Paused {
                                        snapshot.state = PlaybackState::Playing;
                                    }
                                    let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                        path: next,
                                        queue_index: queue_idx,
                                        kind: TrackChangeKind::Manual,
                                    });
                                }
                            }
                        }
                        PlaybackCommand::Previous => {
                            if queue_idx > 0 {
                                queue_idx -= 1;
                                if let Some(prev) = queue.get(queue_idx).cloned() {
                                    snapshot.current = Some(prev.clone());
                                    snapshot.current_queue_index = Some(queue_idx);
                                    snapshot.position = Duration::ZERO;
                                    snapshot.duration = Duration::from_secs(180);
                                    if snapshot.state == PlaybackState::Paused {
                                        snapshot.state = PlaybackState::Playing;
                                    }
                                    let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                        path: prev,
                                        queue_index: queue_idx,
                                        kind: TrackChangeKind::Manual,
                                    });
                                }
                            }
                        }
                        PlaybackCommand::Play => {
                            snapshot.state = PlaybackState::Playing;
                            snapshot.current_queue_index = if snapshot.current.is_some() {
                                Some(queue_idx)
                            } else {
                                None
                            };
                        }
                        PlaybackCommand::Pause => {
                            snapshot.state = PlaybackState::Paused;
                        }
                        PlaybackCommand::Stop => {
                            snapshot.state = PlaybackState::Stopped;
                            snapshot.position = Duration::ZERO;
                            snapshot.current_queue_index = None;
                        }
                        PlaybackCommand::Seek(pos) => {
                            snapshot.position = pos.min(snapshot.duration);
                            let _ = event_tx.send(PlaybackEvent::Seeked);
                        }
                        PlaybackCommand::SetVolume(vol) => {
                            snapshot.volume = vol.clamp(0.0, 1.0);
                        }
                        PlaybackCommand::SetRepeatMode(mode) => {
                            snapshot.repeat_mode = mode;
                        }
                        PlaybackCommand::SetShuffle(enabled) => {
                            snapshot.shuffle_enabled = enabled;
                        }
                        PlaybackCommand::Poll => {
                            if snapshot.state == PlaybackState::Playing {
                                // Generate synthetic PCM when GStreamer is disabled, so visuals remain testable.
                                let mut chunk = Vec::with_capacity(1024);
                                for _ in 0..1024 {
                                    chunk.push(0.25 * phase.sin());
                                    phase += (2.0 * PI * 440.0) / 48_000.0;
                                    if phase > 2.0 * PI {
                                        phase -= 2.0 * PI;
                                    }
                                }
                                let _ = pcm_tx.try_send(AnalysisPcmChunk {
                                    samples: chunk,
                                    channel_labels: vec![SpectrogramChannelLabel::Mono],
                                });
                            }

                            if snapshot.state == PlaybackState::Playing
                                && snapshot.position >= snapshot.duration
                            {
                                queue_idx += 1;
                                if let Some(next) = queue.get(queue_idx).cloned() {
                                    snapshot.current = Some(next.clone());
                                    snapshot.current_queue_index = Some(queue_idx);
                                    snapshot.position = Duration::ZERO;
                                    snapshot.duration = Duration::from_secs(180);
                                    let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                        path: next,
                                        queue_index: queue_idx,
                                        kind: TrackChangeKind::Natural,
                                    });
                                } else {
                                    snapshot.state = PlaybackState::Stopped;
                                    snapshot.position = Duration::ZERO;
                                    snapshot.current_queue_index = None;
                                }
                            }
                        }
                    }

                    let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                }
            });

        (cmd_tx, event_rx)
    }
}

#[cfg(feature = "gst")]
mod backend {
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use anyhow::{anyhow, Context};
    use crossbeam_channel::{unbounded, Receiver, RecvTimeoutError, Sender};
    use gst::prelude::*;
    use gstreamer as gst;
    use gstreamer_app as gst_app;
    use gstreamer_audio as gst_audio;

    use crate::analysis::{AnalysisCommand, AnalysisPcmChunk, SpectrogramChannelLabel};
    use crate::raw_audio::is_dts_file;

    use super::{
        PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode,
        TrackChangeKind,
    };

    struct GaplessQueue {
        queue: Vec<PathBuf>,
        current_idx: usize,
        repeat_mode: RepeatMode,
        shuffle_enabled: bool,
        shuffle_history: Vec<usize>,
        shuffle_forward: Vec<usize>,
        shuffle_pool: Vec<usize>,
        rng_state: u64,
    }

    impl GaplessQueue {
        fn new() -> Self {
            Self {
                queue: Vec::new(),
                current_idx: 0,
                repeat_mode: RepeatMode::Off,
                shuffle_enabled: false,
                shuffle_history: Vec::new(),
                shuffle_forward: Vec::new(),
                shuffle_pool: Vec::new(),
                rng_state: 0,
            }
        }

        fn set_queue(&mut self, queue: Vec<PathBuf>) {
            self.queue = queue;
            self.current_idx = 0;
            self.clear_shuffle_navigation();
            self.rebuild_shuffle_pool();
        }

        fn add_to_queue(&mut self, items: Vec<PathBuf>) {
            self.queue.extend(items);
            self.rebuild_shuffle_pool();
        }

        fn clear(&mut self) {
            self.queue.clear();
            self.current_idx = 0;
            self.clear_shuffle_navigation();
            self.shuffle_pool.clear();
        }

        fn current(&self) -> Option<PathBuf> {
            self.queue.get(self.current_idx).cloned()
        }

        fn current_index(&self) -> Option<usize> {
            if self.queue.is_empty() {
                None
            } else {
                Some(self.current_idx)
            }
        }

        fn set_current(&mut self, idx: usize) -> Option<PathBuf> {
            if idx < self.queue.len() {
                self.current_idx = idx;
                self.clear_shuffle_navigation();
                self.rebuild_shuffle_pool();
                self.current()
            } else {
                None
            }
        }

        fn remove_at(&mut self, idx: usize) -> Option<PathBuf> {
            if idx >= self.queue.len() {
                return self.current();
            }
            self.queue.remove(idx);
            if self.queue.is_empty() {
                self.current_idx = 0;
                self.clear_shuffle_navigation();
                self.shuffle_pool.clear();
                return None;
            }
            if idx < self.current_idx {
                self.current_idx = self.current_idx.saturating_sub(1);
            } else if idx == self.current_idx && self.current_idx >= self.queue.len() {
                self.current_idx = self.queue.len().saturating_sub(1);
            }
            self.clear_shuffle_navigation();
            self.rebuild_shuffle_pool();
            self.current()
        }

        fn move_item(&mut self, from: usize, to: usize) {
            if from >= self.queue.len() || to >= self.queue.len() || from == to {
                return;
            }
            let item = self.queue.remove(from);
            self.queue.insert(to, item);
            if self.current_idx == from {
                self.current_idx = to;
            } else if from < self.current_idx && to >= self.current_idx {
                self.current_idx = self.current_idx.saturating_sub(1);
            } else if from > self.current_idx && to <= self.current_idx {
                self.current_idx += 1;
            }
            self.clear_shuffle_navigation();
            self.rebuild_shuffle_pool();
        }

        fn set_repeat_mode(&mut self, mode: RepeatMode) {
            self.repeat_mode = mode;
        }

        fn set_shuffle_enabled(&mut self, enabled: bool) {
            if self.shuffle_enabled == enabled {
                return;
            }
            self.shuffle_enabled = enabled;
            self.clear_shuffle_navigation();
            if enabled {
                self.rebuild_shuffle_pool();
            } else {
                self.shuffle_pool.clear();
            }
        }

        fn next_manual(&mut self) -> Option<PathBuf> {
            if self.queue.is_empty() {
                return None;
            }

            if self.shuffle_enabled {
                return self.advance_shuffle_or_forward(self.repeat_mode == RepeatMode::All);
            }

            if self.current_idx + 1 < self.queue.len() {
                self.current_idx += 1;
                self.rebuild_shuffle_pool();
                return self.current();
            }
            if self.repeat_mode == RepeatMode::All {
                self.current_idx = 0;
                self.rebuild_shuffle_pool();
                return self.current();
            }
            None
        }

        fn previous_manual(&mut self) -> Option<PathBuf> {
            if self.queue.is_empty() {
                return None;
            }
            if self.shuffle_enabled {
                if let Some(prev_idx) = self.shuffle_history.pop() {
                    let old_idx = self.current_idx;
                    if old_idx != prev_idx {
                        self.shuffle_forward.push(old_idx);
                    }
                    self.current_idx = prev_idx;
                    return self.current();
                }
            }
            if self.current_idx > 0 {
                self.current_idx -= 1;
                self.rebuild_shuffle_pool();
                return self.current();
            }
            if self.repeat_mode == RepeatMode::All && !self.queue.is_empty() {
                self.current_idx = self.queue.len().saturating_sub(1);
                self.rebuild_shuffle_pool();
                return self.current();
            }
            None
        }

        fn next_natural(&mut self) -> Option<PathBuf> {
            if self.queue.is_empty() {
                return None;
            }
            if self.repeat_mode == RepeatMode::One {
                return self.current();
            }
            if self.shuffle_enabled {
                // When repeat-all is enabled, keep drawing from refreshed pools forever.
                // With repeat-off, stop once current shuffle pool is exhausted.
                return self.advance_shuffle_or_forward(self.repeat_mode == RepeatMode::All);
            }
            if self.current_idx + 1 < self.queue.len() {
                self.current_idx += 1;
                self.rebuild_shuffle_pool();
                return self.current();
            }
            if self.repeat_mode == RepeatMode::All {
                self.current_idx = 0;
                self.rebuild_shuffle_pool();
                return self.current();
            }
            None
        }

        fn rebuild_shuffle_pool(&mut self) {
            self.shuffle_pool.clear();
            for i in 0..self.queue.len() {
                if i != self.current_idx {
                    self.shuffle_pool.push(i);
                }
            }
        }

        fn clear_shuffle_navigation(&mut self) {
            self.shuffle_history.clear();
            self.shuffle_forward.clear();
        }

        fn ensure_rng_seed(&mut self) {
            if self.rng_state != 0 {
                return;
            }
            let nanos = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|duration| u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX))
                .unwrap_or(0x9E37_79B9_7F4A_7C15);
            self.rng_state = nanos ^ 0xA5A5_A5A5_5A5A_5A5A;
            if self.rng_state == 0 {
                self.rng_state = 0xD134_2543_DE82_EF95;
            }
        }

        fn next_rand(&mut self, max: usize) -> usize {
            self.ensure_rng_seed();
            self.rng_state = self
                .rng_state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            usize::try_from(self.rng_state % u64::try_from(max).unwrap_or(u64::MAX)).unwrap_or(0)
        }

        fn advance_shuffle_or_forward(&mut self, allow_repeat_cycle: bool) -> Option<PathBuf> {
            if let Some(next_idx) = self.shuffle_forward.pop() {
                if next_idx != self.current_idx {
                    self.shuffle_history.push(self.current_idx);
                }
                self.current_idx = next_idx;
                return self.current();
            }
            self.advance_shuffle_random(allow_repeat_cycle)
        }

        fn advance_shuffle_random(&mut self, allow_repeat_cycle: bool) -> Option<PathBuf> {
            if self.queue.is_empty() {
                return None;
            }
            if self.queue.len() == 1 {
                return if allow_repeat_cycle || self.repeat_mode == RepeatMode::One {
                    self.current()
                } else {
                    None
                };
            }
            if self.shuffle_pool.is_empty() {
                if allow_repeat_cycle {
                    self.rebuild_shuffle_pool();
                } else {
                    return None;
                }
            }
            if self.shuffle_pool.is_empty() {
                return None;
            }
            let pick = self.next_rand(self.shuffle_pool.len());
            let next_idx = self.shuffle_pool.remove(pick);
            if next_idx != self.current_idx {
                self.shuffle_forward.clear();
                self.shuffle_history.push(self.current_idx);
            }
            self.current_idx = next_idx;
            self.current()
        }
    }

    struct GstPlaybackRuntime {
        playbin: gst::Element,
        queue_state: Arc<Mutex<GaplessQueue>>,
        event_tx: Sender<PlaybackEvent>,
        snapshot: PlaybackSnapshot,
        target_volume: f32,
        applied_volume: f32,
        startup_gain_ramp: bool,
        seek_hold: Option<(Instant, Duration)>,
    }

    pub fn spawn_engine(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
        let (event_tx, event_rx) = unbounded::<PlaybackEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-playback-gst".to_string())
            .spawn(move || {
                if let Err(err) = run_gst_engine(&cmd_rx, event_tx.clone(), analysis_tx, pcm_tx) {
                    tracing::error!("gstreamer playback engine failed: {err:#}");
                }
            });

        (cmd_tx, event_rx)
    }

    impl GstPlaybackRuntime {
        fn new(
            playbin: gst::Element,
            queue_state: Arc<Mutex<GaplessQueue>>,
            event_tx: Sender<PlaybackEvent>,
        ) -> Self {
            Self {
                playbin,
                queue_state,
                event_tx,
                snapshot: PlaybackSnapshot {
                    volume: 1.0,
                    ..PlaybackSnapshot::default()
                },
                target_volume: 1.0,
                applied_volume: 1.0,
                startup_gain_ramp: false,
                seek_hold: None,
            }
        }

        fn emit_snapshot(&self) {
            let _ = self
                .event_tx
                .send(PlaybackEvent::Snapshot(self.snapshot.clone()));
        }

        fn emit_track_changed(&self, path: PathBuf, queue_index: usize, kind: TrackChangeKind) {
            let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                path,
                queue_index,
                kind,
            });
        }

        fn set_queue_flags(&mut self, repeat_mode: RepeatMode, shuffle_enabled: bool) {
            self.snapshot.repeat_mode = repeat_mode;
            self.snapshot.shuffle_enabled = shuffle_enabled;
        }

        fn switch_to_path(
            &mut self,
            path: PathBuf,
            queue_index: usize,
            kind: TrackChangeKind,
            force_play: bool,
        ) {
            let Some(uri) = file_uri(&path) else {
                return;
            };
            self.snapshot.current_queue_index = Some(queue_index);
            switch_track(
                &self.playbin,
                &mut self.snapshot,
                path.as_path(),
                &uri,
                &mut self.applied_volume,
                &mut self.startup_gain_ramp,
                force_play,
            );
            self.emit_track_changed(path, queue_index, kind);
            self.emit_snapshot();
        }

        fn stop_with_empty_queue(&mut self) {
            soft_mute(&self.playbin, &mut self.applied_volume);
            let _ = self.playbin.set_state(gst::State::Ready);
            self.startup_gain_ramp = false;
            self.snapshot.current = None;
            self.snapshot.current_queue_index = None;
            self.snapshot.state = PlaybackState::Stopped;
            self.snapshot.position = Duration::ZERO;
            self.snapshot.duration = Duration::ZERO;
        }

        fn load_queue(&mut self, paths: Vec<PathBuf>) {
            if paths.is_empty() {
                return;
            }
            let Ok(mut state) = self.queue_state.lock() else {
                return;
            };
            state.set_queue(paths);
            let repeat_mode = state.repeat_mode;
            let shuffle_enabled = state.shuffle_enabled;
            let first = state.current();
            let current_index = state.current_index().unwrap_or(0);
            drop(state);
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            let Some(first) = first else {
                return;
            };
            self.switch_to_path(first, current_index, TrackChangeKind::Manual, false);
        }

        fn add_to_queue(&mut self, paths: Vec<PathBuf>) {
            let Some((repeat_mode, shuffle_enabled)) = (match self.queue_state.lock() {
                Ok(mut state) => {
                    state.add_to_queue(paths);
                    Some((state.repeat_mode, state.shuffle_enabled))
                }
                Err(_) => None,
            }) else {
                return;
            };
            self.set_queue_flags(repeat_mode, shuffle_enabled);
        }

        fn remove_at(&mut self, idx: usize) {
            let old_current = self.snapshot.current.clone();
            let Some((next_current, repeat_mode, shuffle_enabled, current_index)) =
                (match self.queue_state.lock() {
                    Ok(mut state) => {
                        let next_current = state.remove_at(idx);
                        Some((
                            next_current,
                            state.repeat_mode,
                            state.shuffle_enabled,
                            state.current_index(),
                        ))
                    }
                    Err(_) => None,
                })
            else {
                return;
            };
            self.snapshot.current_queue_index = current_index;
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            if let Some(path) = next_current {
                if old_current.as_ref() == Some(&path) {
                    self.snapshot.current = Some(path);
                } else {
                    self.switch_to_path(
                        path,
                        current_index.unwrap_or(0),
                        TrackChangeKind::Manual,
                        false,
                    );
                    return;
                }
            } else {
                self.stop_with_empty_queue();
            }
            self.emit_snapshot();
        }

        fn move_queue_item(&mut self, from: usize, to: usize) {
            let Some((repeat_mode, shuffle_enabled, current_index)) = (match self.queue_state.lock()
            {
                Ok(mut state) => {
                    state.move_item(from, to);
                    Some((
                        state.repeat_mode,
                        state.shuffle_enabled,
                        state.current_index(),
                    ))
                }
                Err(_) => None,
            }) else {
                return;
            };
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            self.snapshot.current_queue_index = current_index;
        }

        fn clear_queue(&mut self) {
            let flags = if let Ok(mut state) = self.queue_state.lock() {
                state.clear();
                Some((state.repeat_mode, state.shuffle_enabled))
            } else {
                None
            };
            if let Some((repeat_mode, shuffle_enabled)) = flags {
                self.set_queue_flags(repeat_mode, shuffle_enabled);
            }
            self.stop_with_empty_queue();
            self.emit_snapshot();
        }

        fn play_at(&mut self, idx: usize) {
            let Ok(mut state) = self.queue_state.lock() else {
                return;
            };
            let repeat_mode = state.repeat_mode;
            let shuffle_enabled = state.shuffle_enabled;
            let path = state.set_current(idx);
            let current_index = state.current_index().unwrap_or(idx);
            drop(state);
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            let Some(path) = path else {
                return;
            };
            self.switch_to_path(path, current_index, TrackChangeKind::Manual, false);
        }

        fn advance_manual(&mut self, next: bool) {
            let Ok(mut state) = self.queue_state.lock() else {
                return;
            };
            let repeat_mode = state.repeat_mode;
            let shuffle_enabled = state.shuffle_enabled;
            let resume_from_pause = self.snapshot.state == PlaybackState::Paused;
            let next_path = if next {
                state.next_manual()
            } else {
                state.previous_manual()
            };
            let current_index = state.current_index().unwrap_or(0);
            drop(state);
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            let Some(path) = next_path else {
                return;
            };
            self.switch_to_path(
                path,
                current_index,
                TrackChangeKind::Manual,
                resume_from_pause,
            );
        }

        fn play(&mut self) {
            let was_stopped = self.snapshot.state == PlaybackState::Stopped;
            if let Ok(state) = self.queue_state.lock() {
                self.snapshot.current_queue_index = state.current_index();
            }
            if was_stopped {
                self.applied_volume = 0.0;
                self.playbin
                    .set_property("volume", f64::from(self.applied_volume));
                self.startup_gain_ramp = true;
            }
            if self.playbin.set_state(gst::State::Playing).is_ok() {
                self.snapshot.state = PlaybackState::Playing;
                if (self.target_volume - self.applied_volume).abs() > f32::EPSILON {
                    self.startup_gain_ramp = true;
                }
                self.emit_snapshot();
            }
        }

        fn pause(&mut self) {
            if self.playbin.set_state(gst::State::Paused).is_ok() {
                self.snapshot.state = PlaybackState::Paused;
                self.emit_snapshot();
            }
        }

        fn stop(&mut self) {
            soft_mute(&self.playbin, &mut self.applied_volume);
            if self.playbin.set_state(gst::State::Ready).is_ok() {
                self.startup_gain_ramp = false;
                self.seek_hold = None;
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.position = Duration::ZERO;
                self.snapshot.current_queue_index = None;
                self.emit_snapshot();
            }
        }

        fn seek(&mut self, pos: Duration) {
            let nanos = u64::try_from(pos.as_nanos().min(u128::from(u64::MAX))).unwrap_or(u64::MAX);
            let target = gst::ClockTime::from_nseconds(nanos);
            let seek_flags = if self
                .snapshot
                .current
                .as_ref()
                .is_some_and(|path| is_dts_file(path))
            {
                gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT
            } else {
                gst::SeekFlags::FLUSH | gst::SeekFlags::ACCURATE
            };
            let _ = self.playbin.seek_simple(seek_flags, target);
            self.snapshot.position = pos.min(self.snapshot.duration);
            self.seek_hold = Some((
                Instant::now() + Duration::from_millis(220),
                self.snapshot.position,
            ));
            let _ = self.event_tx.send(PlaybackEvent::Seeked);
        }

        fn set_volume(&mut self, volume: f32) {
            self.target_volume = volume.clamp(0.0, 1.0);
            self.snapshot.volume = self.target_volume;
            self.emit_snapshot();
        }

        fn set_repeat_mode(&mut self, mode: RepeatMode) {
            if let Ok(mut state) = self.queue_state.lock() {
                state.set_repeat_mode(mode);
                self.snapshot.repeat_mode = state.repeat_mode;
            } else {
                self.snapshot.repeat_mode = mode;
            }
            self.emit_snapshot();
        }

        fn set_shuffle(&mut self, enabled: bool) {
            if let Ok(mut state) = self.queue_state.lock() {
                state.set_shuffle_enabled(enabled);
                self.snapshot.shuffle_enabled = state.shuffle_enabled;
            } else {
                self.snapshot.shuffle_enabled = enabled;
            }
            self.emit_snapshot();
        }

        fn poll(&mut self) {
            if self.snapshot.state == PlaybackState::Stopped
                && !self.startup_gain_ramp
                && self.seek_hold.is_none()
            {
                return;
            }

            let mut snapshot_changed = false;
            let delta = self.target_volume - self.applied_volume;
            if delta.abs() > f32::EPSILON
                && (self.snapshot.state == PlaybackState::Playing || self.startup_gain_ramp)
            {
                let step = if self.startup_gain_ramp { 0.45 } else { 0.18 };
                if delta.abs() <= step {
                    self.applied_volume = self.target_volume;
                    self.startup_gain_ramp = false;
                } else {
                    self.applied_volume += delta.signum() * step;
                }
                self.playbin
                    .set_property("volume", f64::from(self.applied_volume));
                if (self.snapshot.volume - self.applied_volume).abs() > f32::EPSILON {
                    self.snapshot.volume = self.applied_volume;
                    snapshot_changed = true;
                }
            }

            let mut position_locked = false;
            if let Some((until, target)) = self.seek_hold.as_ref().copied() {
                if Instant::now() < until {
                    if self.snapshot.position != target {
                        self.snapshot.position = target;
                        snapshot_changed = true;
                    }
                    position_locked = true;
                } else {
                    self.seek_hold = None;
                }
            }
            if !position_locked && self.snapshot.state != PlaybackState::Stopped {
                if let Some(pos) = self.playbin.query_position::<gst::ClockTime>() {
                    let next_pos = Duration::from_nanos(pos.nseconds());
                    if self.snapshot.position != next_pos {
                        self.snapshot.position = next_pos;
                        snapshot_changed = true;
                    }
                }
            }
            if self.snapshot.state != PlaybackState::Stopped
                || self.snapshot.duration == Duration::ZERO
            {
                if let Some(dur) = self.playbin.query_duration::<gst::ClockTime>() {
                    let next_dur = Duration::from_nanos(dur.nseconds());
                    if self.snapshot.duration != next_dur {
                        self.snapshot.duration = next_dur;
                        snapshot_changed = true;
                    }
                }
            }
            snapshot_changed |=
                maybe_emit_natural_handoff(&self.queue_state, &mut self.snapshot, &self.event_tx);
            if snapshot_changed {
                self.emit_snapshot();
            }
        }

        fn apply_command(&mut self, cmd: PlaybackCommand) {
            match cmd {
                PlaybackCommand::LoadQueue(paths) => self.load_queue(paths),
                PlaybackCommand::AddToQueue(paths) => self.add_to_queue(paths),
                PlaybackCommand::RemoveAt(idx) => self.remove_at(idx),
                PlaybackCommand::MoveQueue { from, to } => self.move_queue_item(from, to),
                PlaybackCommand::ClearQueue => self.clear_queue(),
                PlaybackCommand::PlayAt(idx) => self.play_at(idx),
                PlaybackCommand::Next => self.advance_manual(true),
                PlaybackCommand::Previous => self.advance_manual(false),
                PlaybackCommand::Play => self.play(),
                PlaybackCommand::Pause => self.pause(),
                PlaybackCommand::Stop => self.stop(),
                PlaybackCommand::Seek(pos) => self.seek(pos),
                PlaybackCommand::SetVolume(volume) => self.set_volume(volume),
                PlaybackCommand::SetRepeatMode(mode) => self.set_repeat_mode(mode),
                PlaybackCommand::SetShuffle(enabled) => self.set_shuffle(enabled),
                PlaybackCommand::Poll => self.poll(),
            }
        }

        fn handle_bus_message(&mut self, msg: &gst::Message) {
            match msg.view() {
                gst::MessageView::Eos(..) => {
                    self.snapshot.state = PlaybackState::Stopped;
                    self.snapshot.position = Duration::ZERO;
                    self.emit_snapshot();
                }
                gst::MessageView::Error(err) => {
                    tracing::error!(
                        "gstreamer error from {:?}: {} ({:?})",
                        err.src().map(gstreamer::prelude::GstObjectExt::path_string),
                        err.error(),
                        err.debug()
                    );
                    self.snapshot.state = PlaybackState::Stopped;
                    self.emit_snapshot();
                }
                _ => {}
            }
        }
    }

    fn run_gst_engine(
        cmd_rx: &Receiver<PlaybackCommand>,
        event_tx: Sender<PlaybackEvent>,
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> anyhow::Result<()> {
        gst::init().context("gst::init failed")?;

        let playbin = gst::ElementFactory::make("playbin")
            .build()
            .map_err(|_| anyhow!("failed to create playbin"))?;

        let analysis_sink = build_analysis_audio_sink(analysis_tx, pcm_tx)?;
        playbin.set_property("audio-sink", &analysis_sink);

        let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));

        {
            let queue_state = Arc::clone(&queue_state);
            playbin.connect("about-to-finish", false, move |values| {
                let maybe_playbin = values.first().and_then(|v| v.get::<gst::Element>().ok());
                let playbin_obj = maybe_playbin?;

                let next = queue_state.lock().ok().and_then(|mut q| q.next_natural());
                if let Some(next_path) = next {
                    if let Some(uri) = file_uri(&next_path) {
                        playbin_obj.set_property("uri", uri);
                    }
                }
                None
            });
        }

        let bus = playbin.bus().context("playbin has no bus")?;
        let mut runtime = GstPlaybackRuntime::new(playbin, queue_state, event_tx);
        runtime
            .playbin
            .set_property("volume", f64::from(runtime.applied_volume));

        loop {
            match cmd_rx.recv_timeout(Duration::from_millis(20)) {
                Ok(cmd) => runtime.apply_command(cmd),
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => break,
            }

            while let Some(msg) = bus.pop() {
                runtime.handle_bus_message(&msg);
            }
        }

        let _ = runtime.playbin.set_state(gst::State::Null);
        Ok(())
    }

    fn maybe_emit_natural_handoff(
        queue_state: &Arc<Mutex<GaplessQueue>>,
        snapshot: &mut PlaybackSnapshot,
        event_tx: &Sender<PlaybackEvent>,
    ) -> bool {
        // Gapless handoff sets next URI early in about-to-finish.
        // Emit TrackChanged only once playback has actually rolled over.
        if snapshot.state != PlaybackState::Playing {
            return false;
        }
        let Ok(state) = queue_state.lock() else {
            return false;
        };
        let Some(current_path) = state.current() else {
            return false;
        };
        let current_index = state.current_index().unwrap_or(0);
        let path_changed = snapshot.current.as_ref() != Some(&current_path);
        let at_track_start = snapshot.position <= Duration::from_secs(2);
        if path_changed && at_track_start {
            snapshot.current = Some(current_path.clone());
            snapshot.current_queue_index = Some(current_index);
            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                path: current_path,
                queue_index: current_index,
                kind: TrackChangeKind::Natural,
            });
            return true;
        }
        false
    }

    fn switch_track(
        playbin: &gst::Element,
        snapshot: &mut PlaybackSnapshot,
        path: &Path,
        uri: &str,
        applied_volume: &mut f32,
        startup_gain_ramp: &mut bool,
        force_play: bool,
    ) {
        let was_playing = snapshot.state == PlaybackState::Playing || force_play;
        soft_mute(playbin, applied_volume);
        let _ = playbin.set_state(gst::State::Ready);
        playbin.set_property("uri", uri);
        if was_playing {
            let _ = playbin.set_state(gst::State::Playing);
            snapshot.state = PlaybackState::Playing;
            *startup_gain_ramp = true;
        } else if snapshot.state == PlaybackState::Paused {
            let _ = playbin.set_state(gst::State::Paused);
            *startup_gain_ramp = false;
        } else {
            *startup_gain_ramp = false;
        }
        snapshot.current = Some(path.to_path_buf());
        snapshot.position = Duration::ZERO;
        snapshot.duration = Duration::ZERO;
    }

    fn soft_mute(playbin: &gst::Element, applied_volume: &mut f32) {
        if *applied_volume <= 0.0001 {
            *applied_volume = 0.0;
            playbin.set_property("volume", f64::from(*applied_volume));
            return;
        }
        for _ in 0..3 {
            *applied_volume *= 0.35;
            if *applied_volume <= 0.0001 {
                *applied_volume = 0.0;
            }
            playbin.set_property("volume", f64::from(*applied_volume));
            std::thread::sleep(Duration::from_millis(4));
            if *applied_volume == 0.0 {
                break;
            }
        }
        *applied_volume = 0.0;
        playbin.set_property("volume", f64::from(*applied_volume));
    }

    fn decode_interleaved_f32(bytes: &[u8]) -> Vec<f32> {
        let mut pcm = Vec::with_capacity(bytes.len() / 4);
        for chunk in bytes.chunks_exact(4) {
            pcm.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
        }
        pcm
    }

    fn positive_i32_to_usize(value: i32) -> Option<usize> {
        usize::try_from(value)
            .ok()
            .filter(|converted| *converted > 0)
    }

    fn positive_i32_to_u32(value: i32) -> Option<u32> {
        u32::try_from(value).ok().filter(|converted| *converted > 0)
    }

    fn fallback_channel_labels(channels: usize) -> Vec<SpectrogramChannelLabel> {
        match channels {
            0 | 1 => vec![SpectrogramChannelLabel::Mono],
            2 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
            ],
            3 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::FrontCenter,
            ],
            4 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::RearLeft,
                SpectrogramChannelLabel::RearRight,
            ],
            5 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::FrontCenter,
                SpectrogramChannelLabel::SideLeft,
                SpectrogramChannelLabel::SideRight,
            ],
            6 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::FrontCenter,
                SpectrogramChannelLabel::Lfe,
                SpectrogramChannelLabel::SideLeft,
                SpectrogramChannelLabel::SideRight,
            ],
            7 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::FrontCenter,
                SpectrogramChannelLabel::Lfe,
                SpectrogramChannelLabel::RearCenter,
                SpectrogramChannelLabel::SideLeft,
                SpectrogramChannelLabel::SideRight,
            ],
            8 => vec![
                SpectrogramChannelLabel::FrontLeft,
                SpectrogramChannelLabel::FrontRight,
                SpectrogramChannelLabel::FrontCenter,
                SpectrogramChannelLabel::Lfe,
                SpectrogramChannelLabel::RearLeft,
                SpectrogramChannelLabel::RearRight,
                SpectrogramChannelLabel::SideLeft,
                SpectrogramChannelLabel::SideRight,
            ],
            count => vec![SpectrogramChannelLabel::Unknown; count],
        }
    }

    fn map_channel_position(position: gst_audio::AudioChannelPosition) -> SpectrogramChannelLabel {
        use gst_audio::AudioChannelPosition as Position;

        match position {
            Position::Mono => SpectrogramChannelLabel::Mono,
            Position::FrontLeft => SpectrogramChannelLabel::FrontLeft,
            Position::FrontRight => SpectrogramChannelLabel::FrontRight,
            Position::FrontCenter => SpectrogramChannelLabel::FrontCenter,
            Position::Lfe1 | Position::Lfe2 => SpectrogramChannelLabel::Lfe,
            Position::SideLeft | Position::SurroundLeft => SpectrogramChannelLabel::SideLeft,
            Position::SideRight | Position::SurroundRight => SpectrogramChannelLabel::SideRight,
            Position::RearLeft => SpectrogramChannelLabel::RearLeft,
            Position::RearRight => SpectrogramChannelLabel::RearRight,
            Position::RearCenter => SpectrogramChannelLabel::RearCenter,
            _ => SpectrogramChannelLabel::Unknown,
        }
    }

    struct AnalysisTapState {
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
        last_rate_hz: u32,
        tap_chunk_samples: usize,
        profile_enabled: bool,
        prof_last: Instant,
        prof_sent: usize,
        prof_dropped: usize,
        prof_samples: usize,
    }

    impl AnalysisTapState {
        fn new(
            analysis_tx: Sender<AnalysisCommand>,
            pcm_tx: Sender<AnalysisPcmChunk>,
            tap_chunk_samples: usize,
        ) -> Self {
            Self {
                analysis_tx,
                pcm_tx,
                last_rate_hz: 0,
                tap_chunk_samples,
                profile_enabled: cfg!(feature = "profiling-logs")
                    && std::env::var_os("FERROUS_PROFILE").is_some(),
                prof_last: Instant::now(),
                prof_sent: 0,
                prof_dropped: 0,
                prof_samples: 0,
            }
        }

        fn handle_sample(&mut self, sink: &gst_app::AppSink) -> gst::FlowSuccess {
            if let Ok(sample) = sink.pull_sample() {
                self.process_sample(&sample);
            }
            self.maybe_log_profile();
            gst::FlowSuccess::Ok
        }

        fn process_sample(&mut self, sample: &gst::Sample) {
            let Some(buffer) = sample.buffer() else {
                return;
            };
            let Ok(map) = buffer.map_readable() else {
                return;
            };
            let bytes = map.as_slice();
            if bytes.is_empty() {
                return;
            }

            let channel_labels = self.channel_labels_for_sample(sample);
            let pcm = decode_interleaved_f32(bytes);
            if pcm.is_empty() {
                return;
            }

            let channels = channel_labels.len().max(1);
            let chunk_width = self.tap_chunk_samples.saturating_mul(channels);
            for part in pcm.chunks(chunk_width.max(channels)) {
                if self
                    .pcm_tx
                    .try_send(AnalysisPcmChunk {
                        samples: part.to_vec(),
                        channel_labels: channel_labels.clone(),
                    })
                    .is_ok()
                {
                    self.prof_sent += 1;
                    self.prof_samples += part.len();
                } else {
                    self.prof_dropped += 1;
                }
            }
        }

        fn channel_labels_for_sample(
            &mut self,
            sample: &gst::Sample,
        ) -> Vec<SpectrogramChannelLabel> {
            let Some(caps) = sample.caps() else {
                return vec![SpectrogramChannelLabel::Mono];
            };

            let channel_labels = channel_labels_from_caps(caps);
            if let Some(structure) = caps.structure(0) {
                if let Ok(rate) = structure.get::<i32>("rate") {
                    if let Some(rate_hz) = positive_i32_to_u32(rate) {
                        if self.last_rate_hz != rate_hz {
                            self.last_rate_hz = rate_hz;
                            let _ = self
                                .analysis_tx
                                .send(AnalysisCommand::SetSampleRate(rate_hz));
                        }
                    }
                }
            }
            channel_labels
        }

        fn maybe_log_profile(&mut self) {
            if !self.profile_enabled || self.prof_last.elapsed() < Duration::from_secs(1) {
                return;
            }
            profile_eprintln!(
                "[gst] pcm_chunks sent/s={} dropped/s={} samples/s={} rate={}Hz",
                self.prof_sent,
                self.prof_dropped,
                self.prof_samples,
                self.last_rate_hz
            );
            self.prof_last = Instant::now();
            self.prof_sent = 0;
            self.prof_dropped = 0;
            self.prof_samples = 0;
        }
    }

    fn channel_labels_from_caps(caps: &gst::CapsRef) -> Vec<SpectrogramChannelLabel> {
        if let Ok(info) = gst_audio::AudioInfo::from_caps(caps) {
            if let Some(positions) = info.positions() {
                let labels = positions
                    .iter()
                    .copied()
                    .map(map_channel_position)
                    .collect::<Vec<_>>();
                if !labels.is_empty() {
                    return labels;
                }
            }
            return fallback_channel_labels(usize::try_from(info.channels()).unwrap_or(usize::MAX));
        }
        if let Some(structure) = caps.structure(0) {
            if let Ok(channels) = structure.get::<i32>("channels") {
                if let Some(channel_count) = positive_i32_to_usize(channels) {
                    return fallback_channel_labels(channel_count);
                }
            }
        }
        vec![SpectrogramChannelLabel::Mono]
    }

    fn build_output_sink() -> anyhow::Result<gst::Element> {
        let output_sink_name = std::env::var("FERROUS_GST_OUTPUT_SINK")
            .ok()
            .filter(|sink| !sink.trim().is_empty())
            .unwrap_or_else(|| "autoaudiosink".to_string());
        gst::ElementFactory::make(&output_sink_name)
            .build()
            .or_else(|_| {
                tracing::warn!(
                    "failed to build output sink '{}', falling back to autoaudiosink",
                    output_sink_name
                );
                gst::ElementFactory::make("autoaudiosink").build()
            })
            .map_err(|_| anyhow!("missing output sink element"))
    }

    fn link_tee_branch(
        tee: &gst::Element,
        branch_sink: &gst::Element,
        label: &str,
    ) -> anyhow::Result<()> {
        let tee_pad = tee
            .request_pad_simple("src_%u")
            .ok_or_else(|| anyhow!("failed requesting tee src pad for {label}"))?;
        let branch_sink_pad = branch_sink
            .static_pad("sink")
            .ok_or_else(|| anyhow!("missing {label} sink pad"))?;
        tee_pad
            .link(&branch_sink_pad)
            .map_err(|err| anyhow!("failed linking tee->{label}: {err:?}"))?;
        Ok(())
    }

    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    fn build_analysis_audio_sink(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> anyhow::Result<gst::Bin> {
        let bin = gst::Bin::new();

        let tee = gst::ElementFactory::make("tee")
            .build()
            .map_err(|_| anyhow!("missing tee element"))?;

        let queue_out = gst::ElementFactory::make("queue")
            .build()
            .map_err(|_| anyhow!("missing queue element"))?;
        let conv_out = gst::ElementFactory::make("audioconvert")
            .build()
            .map_err(|_| anyhow!("missing audioconvert element"))?;
        let resample_out = gst::ElementFactory::make("audioresample")
            .build()
            .map_err(|_| anyhow!("missing audioresample element"))?;
        let sink_out = build_output_sink()?;

        let queue_tap = gst::ElementFactory::make("queue")
            .build()
            .map_err(|_| anyhow!("missing queue element"))?;
        queue_tap.set_property_from_str("leaky", "downstream");
        queue_tap.set_property("max-size-buffers", 128u32);
        queue_tap.set_property("max-size-bytes", 0u32);
        queue_tap.set_property("max-size-time", 0u64);
        let conv = gst::ElementFactory::make("audioconvert")
            .build()
            .map_err(|_| anyhow!("missing audioconvert element"))?;
        let resample = gst::ElementFactory::make("audioresample")
            .build()
            .map_err(|_| anyhow!("missing audioresample element"))?;
        let capsfilter = gst::ElementFactory::make("capsfilter")
            .build()
            .map_err(|_| anyhow!("missing capsfilter element"))?;

        let caps = gst::Caps::builder("audio/x-raw")
            .field("format", "F32LE")
            .field("layout", "interleaved")
            // Keep analysis workload constant across source formats/codecs.
            .field("rate", 44_100i32)
            .build();
        capsfilter.set_property("caps", &caps);

        // Keep tap synced by default to avoid analysis racing ahead of
        // audible playback; explicit env override is still available for
        // controlled experiments.
        let analysis_sync = std::env::var("FERROUS_GST_ANALYSIS_SYNC")
            .ok()
            .and_then(|raw| raw.parse::<i32>().ok())
            != Some(0);

        let appsink = gst_app::AppSink::builder()
            .caps(&caps)
            .drop(true)
            .max_buffers(8)
            .sync(analysis_sync)
            .build();

        let tap_chunk_samples = std::env::var("FERROUS_GST_TAP_CHUNK_SAMPLES")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .map_or(2048, |v| v.clamp(256, 16384));
        let mut tap_state = AnalysisTapState::new(analysis_tx, pcm_tx, tap_chunk_samples);

        appsink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |sink| Ok(tap_state.handle_sample(sink)))
                .build(),
        );

        bin.add_many([
            &tee,
            &queue_out,
            &conv_out,
            &resample_out,
            &sink_out,
            &queue_tap,
            &conv,
            &resample,
            &capsfilter,
            appsink.upcast_ref(),
        ])
        .context("failed to add elements to analysis audio bin")?;

        gst::Element::link_many([&queue_out, &conv_out, &resample_out, &sink_out])
            .context("failed to link output branch")?;
        gst::Element::link_many([
            &queue_tap,
            &conv,
            &resample,
            &capsfilter,
            appsink.upcast_ref(),
        ])
        .context("failed to link analysis branch")?;

        link_tee_branch(&tee, &queue_out, "output queue")?;
        link_tee_branch(&tee, &queue_tap, "analysis queue")?;

        let tee_sink_pad = tee
            .static_pad("sink")
            .ok_or_else(|| anyhow!("missing tee sink pad"))?;
        let ghost = gst::GhostPad::with_target(&tee_sink_pad)
            .map_err(|_| anyhow!("failed creating ghost pad"))?;
        ghost
            .set_active(true)
            .map_err(|_| anyhow!("failed activating ghost pad"))?;
        bin.add_pad(&ghost)
            .map_err(|_| anyhow!("failed adding ghost pad to bin"))?;

        Ok(bin)
    }

    #[cfg(test)]
    mod tests {
        use super::*;
        use crossbeam_channel::unbounded;

        fn setup_queue_two_tracks(a: &Path, b: &Path) -> Arc<Mutex<GaplessQueue>> {
            let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));
            let mut queue = queue_state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            queue.set_queue(vec![a.to_path_buf(), b.to_path_buf()]);
            let _ = queue.next_manual();
            drop(queue);
            queue_state
        }

        #[test]
        fn natural_handoff_emits_track_changed_near_track_start() {
            let (event_tx, event_rx) = unbounded::<PlaybackEvent>();
            let first = PathBuf::from("/tmp/gst_handoff_a.flac");
            let second = PathBuf::from("/tmp/gst_handoff_b.flac");
            let queue_state = setup_queue_two_tracks(&first, &second);
            let mut snapshot = PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_millis(800),
                current: Some(first),
                ..PlaybackSnapshot::default()
            };

            let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot, &event_tx);
            assert!(emitted);
            assert_eq!(snapshot.current.as_ref(), Some(&second));

            let event = event_rx.try_recv().expect("natural handoff event");
            match event {
                PlaybackEvent::TrackChanged {
                    path,
                    queue_index: _,
                    kind,
                } => {
                    assert_eq!(path, second);
                    assert!(matches!(kind, TrackChangeKind::Natural));
                }
                other => panic!("unexpected event: {other:?}"),
            }
        }

        #[test]
        fn natural_handoff_does_not_emit_before_track_start_window() {
            let (event_tx, event_rx) = unbounded::<PlaybackEvent>();
            let first = PathBuf::from("/tmp/gst_handoff_a.flac");
            let second = PathBuf::from("/tmp/gst_handoff_b.flac");
            let queue_state = setup_queue_two_tracks(&first, &second);
            let mut snapshot = PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_secs(3),
                current: Some(first.clone()),
                ..PlaybackSnapshot::default()
            };

            let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot, &event_tx);
            assert!(!emitted);
            assert_eq!(snapshot.current.as_ref(), Some(&first));
            assert!(event_rx.try_recv().is_err());
        }

        #[test]
        fn repeat_one_replays_current_on_natural_advance() {
            let a = PathBuf::from("/tmp/repeat_one.flac");
            let b = PathBuf::from("/tmp/repeat_one_b.flac");
            let mut queue = GaplessQueue::new();
            queue.set_queue(vec![a.clone(), b]);
            queue.set_repeat_mode(RepeatMode::One);
            assert_eq!(queue.current().as_ref(), Some(&a));
            assert_eq!(queue.next_natural().as_ref(), Some(&a));
        }

        #[test]
        fn repeat_all_loops_to_start_on_natural_advance() {
            let a = PathBuf::from("/tmp/repeat_all_a.flac");
            let b = PathBuf::from("/tmp/repeat_all_b.flac");
            let mut queue = GaplessQueue::new();
            queue.set_queue(vec![a.clone(), b]);
            let _ = queue.set_current(1);
            queue.set_repeat_mode(RepeatMode::All);
            assert_eq!(queue.next_natural().as_ref(), Some(&a));
        }

        #[test]
        fn analysis_interleaved_pcm_decodes_f32_frames() {
            let bytes = [
                1.0f32.to_le_bytes(),
                0.0f32.to_le_bytes(),
                (-1.0f32).to_le_bytes(),
                0.5f32.to_le_bytes(),
                0.5f32.to_le_bytes(),
                0.5f32.to_le_bytes(),
            ]
            .concat();

            let pcm = decode_interleaved_f32(&bytes);
            assert_eq!(pcm.len(), 6);
            assert!((pcm[0] - 1.0).abs() < f32::EPSILON);
            assert!((pcm[2] + 1.0).abs() < f32::EPSILON);
            assert!((pcm[5] - 0.5).abs() < f32::EPSILON);
        }

        #[test]
        fn shuffle_previous_returns_to_history() {
            let a = PathBuf::from("/tmp/shuffle_hist_a.flac");
            let b = PathBuf::from("/tmp/shuffle_hist_b.flac");
            let c = PathBuf::from("/tmp/shuffle_hist_c.flac");
            let mut queue = GaplessQueue::new();
            queue.set_queue(vec![a.clone(), b, c]);
            queue.set_shuffle_enabled(true);
            let next = queue.next_manual().expect("shuffle next");
            assert_ne!(next, a);
            assert_eq!(queue.previous_manual().as_ref(), Some(&a));
            assert_eq!(queue.next_manual().as_ref(), Some(&next));
        }

        #[test]
        fn shuffle_repeat_off_stops_when_pool_exhausted() {
            let a = PathBuf::from("/tmp/shuffle_stop_a.flac");
            let b = PathBuf::from("/tmp/shuffle_stop_b.flac");
            let mut queue = GaplessQueue::new();
            queue.set_queue(vec![a, b.clone()]);
            queue.set_shuffle_enabled(true);
            assert_eq!(queue.next_natural().as_ref(), Some(&b));
            assert!(queue.next_natural().is_none());
        }
    }

    fn file_uri(path: &Path) -> Option<String> {
        url::Url::from_file_path(path).ok().map(|u| u.to_string())
    }
}
