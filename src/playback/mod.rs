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
    pub current_bitrate_kbps: Option<u32>,
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
                    track_token: _,
                } = evt
                {
                    if path == b && matches!(kind, TrackChangeKind::Gapless) {
                        observed = Some((path, kind));
                        break;
                    }
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
    fn play_at_without_play_stays_stopped() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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

    #[test]
    fn pause_from_stopped_keeps_stopped_state() {
        let (analysis_tx, _analysis_rx) = unbounded();
        let (pcm_tx, _pcm_rx) = unbounded();
        let (engine, rx) = PlaybackEngine::new(analysis_tx, pcm_tx);

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
                                    track_token: 0,
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
                                            track_token: 0,
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
                                    track_token: 0,
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
                                        track_token: 0,
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
                                        track_token: 0,
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
                            if snapshot.state == PlaybackState::Playing {
                                snapshot.state = PlaybackState::Paused;
                            }
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
                                    track_token: 0,
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
                                        kind: TrackChangeKind::Gapless,
                                        track_token: 0,
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
    use std::sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    };
    use std::time::{Duration, Instant};

    use anyhow::{anyhow, Context};
    use crossbeam_channel::{unbounded, Receiver, RecvTimeoutError, Sender};
    use gst::prelude::*;
    use gstreamer as gst;
    use gstreamer_app as gst_app;
    use gstreamer_audio as gst_audio;

    use crate::analysis::{AnalysisCommand, AnalysisPcmChunk, SpectrogramChannelLabel};
    use crate::raw_audio::{
        audio_byte_range, is_dts_file, is_raw_surround_file, register_raw_surround_typefinders,
    };

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
        analysis_tx: Sender<AnalysisCommand>,
        analysis_track_token: Arc<AtomicU64>,
        event_tx: Sender<PlaybackEvent>,
        snapshot: PlaybackSnapshot,
        target_volume: f32,
        applied_volume: f32,
        startup_gain_ramp: bool,
        startup_ramp_hold_until: Option<Instant>,
        buffering_active: bool,
        seek_hold: Option<(Instant, Duration)>,
        /// Set by the about-to-finish handler when the next track has a
        /// different codec (file extension).  The EOS handler will perform
        /// a full pipeline switch instead of relying on gapless playback.
        pending_eos_track_switch: Arc<AtomicBool>,
    }

    pub fn spawn_engine(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
    ) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
        let (event_tx, event_rx) = unbounded::<PlaybackEvent>();
        let analysis_track_token = Arc::new(AtomicU64::new(0));

        let _ = std::thread::Builder::new()
            .name("ferrous-playback-gst".to_string())
            .spawn(move || {
                if let Err(err) = run_gst_engine(
                    &cmd_rx,
                    event_tx.clone(),
                    analysis_tx,
                    pcm_tx,
                    analysis_track_token,
                ) {
                    eprintln!("[ferrous] gstreamer playback engine failed: {err:#}");
                }
            });

        (cmd_tx, event_rx)
    }

    impl GstPlaybackRuntime {
        fn command_wait_timeout(&self) -> Duration {
            if self.snapshot.state == PlaybackState::Playing
                || self.buffering_active
                || self.seek_hold.is_some()
                || self.startup_gain_ramp
            {
                Duration::from_millis(20)
            } else if self.snapshot.state == PlaybackState::Paused {
                Duration::from_millis(80)
            } else {
                Duration::from_millis(250)
            }
        }

        fn new(
            playbin: gst::Element,
            queue_state: Arc<Mutex<GaplessQueue>>,
            analysis_tx: Sender<AnalysisCommand>,
            analysis_track_token: Arc<AtomicU64>,
            event_tx: Sender<PlaybackEvent>,
            pending_eos_track_switch: Arc<AtomicBool>,
        ) -> Self {
            Self {
                playbin,
                queue_state,
                analysis_tx,
                analysis_track_token,
                event_tx,
                snapshot: PlaybackSnapshot {
                    volume: 1.0,
                    ..PlaybackSnapshot::default()
                },
                target_volume: 1.0,
                applied_volume: 1.0,
                startup_gain_ramp: false,
                startup_ramp_hold_until: None,
                buffering_active: false,
                seek_hold: None,
                pending_eos_track_switch,
            }
        }

        fn emit_snapshot(&self) {
            let _ = self
                .event_tx
                .send(PlaybackEvent::Snapshot(self.snapshot.clone()));
        }

        fn advance_track_token(&self) -> u64 {
            let track_token = self.analysis_track_token.fetch_add(1, Ordering::Relaxed) + 1;
            let _ = self
                .analysis_tx
                .send(AnalysisCommand::SetTrackToken(track_token));
            track_token
        }

        fn emit_track_changed(
            &self,
            path: PathBuf,
            queue_index: usize,
            kind: TrackChangeKind,
            track_token: u64,
        ) {
            let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                path,
                queue_index,
                kind,
                track_token,
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
            let track_token = self.advance_track_token();
            self.snapshot.current_queue_index = Some(queue_index);
            switch_track(
                &self.playbin,
                &mut self.snapshot,
                path.as_path(),
                &uri,
                &mut self.applied_volume,
                &mut self.startup_gain_ramp,
                &mut self.startup_ramp_hold_until,
                force_play,
            );
            self.buffering_active = false;
            self.pending_eos_track_switch
                .store(false, Ordering::Release);
            self.emit_track_changed(path, queue_index, kind, track_token);
            self.emit_snapshot();
        }

        fn stop_with_empty_queue(&mut self) {
            soft_mute(&self.playbin, &mut self.applied_volume);
            let _ = self.playbin.set_state(gst::State::Ready);
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
            self.buffering_active = false;
            self.pending_eos_track_switch
                .store(false, Ordering::Release);
            self.snapshot.current = None;
            self.snapshot.current_queue_index = None;
            self.snapshot.current_bitrate_kbps = None;
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
                self.startup_ramp_hold_until = if self
                    .snapshot
                    .current
                    .as_deref()
                    .is_some_and(is_raw_surround_file)
                {
                    Some(Instant::now() + Duration::from_millis(80))
                } else {
                    None
                };
            }
            self.buffering_active = false;
            if self.playbin.set_state(gst::State::Playing).is_ok() {
                if was_stopped {
                    // Re-assert mute after state transition to close the race
                    // window where new internal elements may not have volume=0.
                    self.playbin.set_property("volume", 0.0_f64);
                }
                self.snapshot.state = PlaybackState::Playing;
                if (self.target_volume - self.applied_volume).abs() > f32::EPSILON {
                    self.startup_gain_ramp = true;
                }
                self.emit_snapshot();
            }
        }

        fn pause(&mut self) {
            if self.snapshot.state != PlaybackState::Playing {
                return;
            }
            self.buffering_active = false;
            if self.playbin.set_state(gst::State::Paused).is_ok() {
                self.snapshot.state = PlaybackState::Paused;
                self.emit_snapshot();
            }
        }

        fn stop(&mut self) {
            soft_mute(&self.playbin, &mut self.applied_volume);
            if self.playbin.set_state(gst::State::Ready).is_ok() {
                self.startup_gain_ramp = false;
                self.startup_ramp_hold_until = None;
                self.buffering_active = false;
                self.seek_hold = None;
                self.pending_eos_track_switch
                    .store(false, Ordering::Release);
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.position = Duration::ZERO;
                self.snapshot.current_queue_index = None;
                self.snapshot.current_bitrate_kbps = None;
                self.emit_snapshot();
            }
        }

        fn seek(&mut self, pos: Duration) {
            let nanos = u64::try_from(pos.as_nanos().min(u128::from(u64::MAX))).unwrap_or(u64::MAX);
            let target = gst::ClockTime::from_nseconds(nanos);
            let seek_flags = seek_flags_for_path(self.snapshot.current.as_deref());
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

        /// Drive the startup volume ramp, respecting the optional silence hold
        /// for AC3/DTS decoder stabilisation.  Returns `true` when the
        /// snapshot volume changed and needs to be emitted.
        fn poll_volume_ramp(&mut self) -> bool {
            // Startup silence hold: keep volume at zero until the decoder has
            // had time to stabilise (prevents AC3/DTS garbage frames from
            // reaching the speakers).
            if let Some(hold_until) = self.startup_ramp_hold_until {
                if Instant::now() < hold_until {
                    if self.applied_volume != 0.0 {
                        self.applied_volume = 0.0;
                        self.playbin.set_property("volume", 0.0_f64);
                    }
                    return false;
                }
                self.startup_ramp_hold_until = None;
            }

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
            }
            if (self.snapshot.volume - self.applied_volume).abs() > f32::EPSILON {
                self.snapshot.volume = self.applied_volume;
                return true;
            }
            false
        }

        fn poll(&mut self) {
            if self.snapshot.state == PlaybackState::Stopped
                && !self.startup_gain_ramp
                && self.seek_hold.is_none()
                && self.startup_ramp_hold_until.is_none()
            {
                return;
            }

            let mut snapshot_changed = self.poll_volume_ramp();

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
            // Only check for same-format gapless handoff when we are NOT
            // waiting for EOS to drive a cross-format switch.
            if !self.pending_eos_track_switch.load(Ordering::Acquire) {
                if let Some((path, queue_index)) =
                    maybe_emit_natural_handoff(&self.queue_state, &mut self.snapshot)
                {
                    let track_token = self.advance_track_token();
                    self.emit_track_changed(
                        path,
                        queue_index,
                        TrackChangeKind::Gapless,
                        track_token,
                    );
                    snapshot_changed = true;
                }
            }
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
                gst::MessageView::Buffering(buffering) => {
                    let percent = buffering.percent();
                    if percent < 100 {
                        if self.snapshot.state == PlaybackState::Playing && !self.buffering_active {
                            let _ = self.playbin.set_state(gst::State::Paused);
                            self.buffering_active = true;
                        }
                    } else if self.buffering_active {
                        self.buffering_active = false;
                        if self.snapshot.state == PlaybackState::Playing {
                            let _ = self.playbin.set_state(gst::State::Playing);
                        }
                    }
                }
                gst::MessageView::Eos(..) => {
                    if self.pending_eos_track_switch.swap(false, Ordering::AcqRel) {
                        // Cross-format transition: about-to-finish advanced the
                        // queue but didn't set the URI.  Do a full pipeline switch.
                        let next = self.queue_state.lock().ok().and_then(|state| {
                            let path = state.current()?;
                            let index = state.current_index().unwrap_or(0);
                            Some((path, index))
                        });
                        if let Some((path, index)) = next {
                            self.switch_to_path(path, index, TrackChangeKind::Natural, true);
                            return;
                        }
                    }
                    // Normal EOS: end of queue
                    self.buffering_active = false;
                    self.snapshot.state = PlaybackState::Stopped;
                    self.snapshot.position = Duration::ZERO;
                    self.emit_snapshot();
                }
                gst::MessageView::Error(err) => {
                    eprintln!(
                        "[ferrous] gstreamer error from {:?}: {} ({:?})",
                        err.src().map(gstreamer::prelude::GstObjectExt::path_string),
                        err.error(),
                        err.debug()
                    );
                    self.buffering_active = false;
                    self.snapshot.state = PlaybackState::Stopped;
                    // Tear down the faulted pipeline fully so subsequent
                    // tracks can start from a clean state.
                    let _ = self.playbin.set_state(gst::State::Null);
                    let _ = self.playbin.set_state(gst::State::Ready);
                    self.emit_snapshot();
                }
                gst::MessageView::Warning(warn) => {
                    eprintln!(
                        "[ferrous] gstreamer warning from {:?}: {} ({:?})",
                        warn.src()
                            .map(gstreamer::prelude::GstObjectExt::path_string),
                        warn.error(),
                        warn.debug()
                    );
                }
                gst::MessageView::StateChanged(sc) => {
                    // Only log playbin-level state changes, not every
                    // internal element, to keep output manageable.
                    if sc.src().is_some_and(|s| s == &self.playbin) {
                        eprintln!(
                            "[ferrous] playbin state: {:?} → {:?}",
                            sc.old(),
                            sc.current()
                        );
                    }
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
        analysis_track_token: Arc<AtomicU64>,
    ) -> anyhow::Result<()> {
        gst::init().context("gst::init failed")?;
        register_raw_surround_typefinders();

        let playbin = gst::ElementFactory::make("playbin")
            .build()
            .map_err(|_| anyhow!("failed to create playbin"))?;
        configure_playbin_buffering(&playbin);
        install_apedemux_block(&playbin);

        let analysis_sink = build_analysis_audio_sink(
            analysis_tx.clone(),
            pcm_tx,
            Arc::clone(&analysis_track_token),
        )?;
        playbin.set_property("audio-sink", &analysis_sink);

        // Strip leading partial-frame data and trailing APEv2 tags from raw
        // surround file sources so the AC3/DTS parser only receives clean,
        // sync-aligned audio.  Without this, the parser must scan for sync
        // words through garbage data, causing gaps during gapless transitions.
        install_raw_surround_source_probe(&playbin);

        let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));
        let pending_eos_track_switch = Arc::new(AtomicBool::new(false));

        {
            let queue_state = Arc::clone(&queue_state);
            let pending_eos = Arc::clone(&pending_eos_track_switch);
            playbin.connect("about-to-finish", false, move |values| {
                let playbin_obj = values.first()?.get::<gst::Element>().ok()?;

                let mut q = match queue_state.lock() {
                    Ok(q) => q,
                    Err(e) => {
                        eprintln!("[ferrous] about-to-finish: queue lock poisoned: {e}");
                        return None;
                    }
                };
                let old_path = q.current()?;
                let new_path = q.next_natural()?;
                drop(q);

                // Same file extension → gapless (same decoder reused).
                // Different extension → let EOS fire, handle in bus handler.
                if same_audio_extension(&old_path, &new_path) {
                    if let Some(uri) = file_uri(&new_path) {
                        playbin_obj.set_property("uri", uri);
                    } else {
                        eprintln!(
                            "[ferrous] about-to-finish: failed to convert path to URI: {}",
                            new_path.display()
                        );
                    }
                } else {
                    pending_eos.store(true, Ordering::Release);
                }
                None
            });
        }

        let bus = playbin.bus().context("playbin has no bus")?;
        let mut runtime = GstPlaybackRuntime::new(
            playbin,
            queue_state,
            analysis_tx,
            analysis_track_token,
            event_tx,
            pending_eos_track_switch,
        );
        runtime
            .playbin
            .set_property("volume", f64::from(runtime.applied_volume));

        loop {
            match cmd_rx.recv_timeout(runtime.command_wait_timeout()) {
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

    /// Prevent `apedemux` from being auto-plugged inside playbin's internal
    /// decodebins.  We handle `APEv2` tags ourselves; `apedemux` causes
    /// crashes and decode failures for AC3/DTS files with appended tags.
    ///
    /// Uses `deep-element-added` (a recursive `GstBin` signal) to intercept
    /// every element created anywhere in the pipeline, including nested
    /// decodebins inside uridecodebin.  When a decodebin appears, its
    /// `autoplug-select` signal is hooked to return SKIP for `apedemux`.
    fn install_apedemux_block(playbin: &gst::Element) {
        playbin.connect("deep-element-added", false, |values| {
            // deep-element-added(bin, sub_bin, element)
            let element = values.get(2).and_then(|v| v.get::<gst::Element>().ok())?;
            let has_autoplug = element.factory().is_some_and(|f| {
                let name = f.name();
                name == "decodebin"
                    || name == "uridecodebin"
                    || name == "decodebin3"
                    || name == "uridecodebin3"
            });
            if has_autoplug {
                element.connect("autoplug-select", false, |values| {
                    let factory = values
                        .get(3)
                        .and_then(|v| v.get::<gst::ElementFactory>().ok());
                    let skip = factory.is_some_and(|f| f.name() == "apedemux");
                    autoplug_select_result(if skip { 2 } else { 0 })
                });
            }
            None
        });
    }

    /// Install a `source-setup` handler on playbin that attaches a pad
    /// probe to each source element feeding a raw surround file.  The
    /// probe strips leading partial-frame bytes (before the first sync
    /// word) and trailing `APEv2` tag bytes, so the parser only receives
    /// clean, sync-aligned audio data.
    fn install_raw_surround_source_probe(playbin: &gst::Element) {
        playbin.connect("source-setup", false, |values| {
            let source = values.get(1)?.get::<gst::Element>().ok()?;
            if !source.has_property("location") {
                return None;
            }
            let location = source.property_value("location").get::<String>().ok()?;
            let path = PathBuf::from(&location);
            let (audio_start, audio_end) = audio_byte_range(&path)?;

            let src_pad = source.static_pad("src")?;
            let bytes_seen = AtomicU64::new(0);
            src_pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, info| {
                let buf_size = match info.buffer() {
                    Some(buf) => buf.size() as u64,
                    None => return gst::PadProbeReturn::Ok,
                };
                let offset = bytes_seen.fetch_add(buf_size, Ordering::Relaxed);
                let buf_end = offset + buf_size;

                // Entirely outside the audio region → drop.
                if buf_end <= audio_start || offset >= audio_end {
                    return gst::PadProbeReturn::Drop;
                }

                // Buffer spans the front boundary (leading partial frame).
                if offset < audio_start {
                    let skip = usize::try_from(audio_start - offset).unwrap_or(0);
                    let tail = usize::try_from(buf_end.min(audio_end) - audio_start).unwrap_or(0);
                    if tail == 0 {
                        return gst::PadProbeReturn::Drop;
                    }
                    if let Some(buf) = info.buffer_mut() {
                        // Replace with a sub-region copy that skips the
                        // leading bytes — can't just set_size here because
                        // the unwanted bytes are at the front.
                        if let Ok(sub) =
                            buf.copy_region(gst::BufferCopyFlags::all(), skip..skip + tail)
                        {
                            *buf = sub;
                        }
                    }
                    return gst::PadProbeReturn::Ok;
                }

                // Buffer spans the back boundary (APEv2 tag region).
                if buf_end > audio_end {
                    let keep = usize::try_from(audio_end - offset).unwrap_or(0);
                    if keep == 0 {
                        return gst::PadProbeReturn::Drop;
                    }
                    if let Some(buf) = info.buffer_mut() {
                        buf.make_mut().set_size(keep);
                    }
                }

                gst::PadProbeReturn::Ok
            });
            None
        });
    }

    /// Build a `GstAutoplugSelectResult` enum value for signal return.
    fn autoplug_select_result(value: i32) -> Option<gst::glib::Value> {
        use gst::glib::translate::ToGlibPtrMut;
        let gtype = gst::glib::Type::from_name("GstAutoplugSelectResult")?;
        let mut gvalue = gst::glib::Value::from_type(gtype);
        unsafe {
            gst::glib::gobject_ffi::g_value_set_enum(gvalue.to_glib_none_mut().0, value);
        }
        Some(gvalue)
    }

    fn configure_playbin_buffering(playbin: &gst::Element) {
        let flags = playbin.property_value("flags");
        let Some(flags_class) = gst::glib::FlagsClass::with_type(flags.type_()) else {
            return;
        };
        let Some(tuned_flags) = flags_class
            .builder_with_value(flags.clone())
            .and_then(|builder| builder.unset_by_nick("download").build())
        else {
            return;
        };
        playbin.set_property_from_value("flags", &tuned_flags);
    }

    fn seek_flags_for_path(path: Option<&Path>) -> gst::SeekFlags {
        if path.is_some_and(is_dts_file) {
            gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT
        } else {
            // Accurate seeks can force parsers/demuxers to scan large files.
            // Default playback should prefer responsiveness, especially on slow
            // network-backed filesystems.
            gst::SeekFlags::FLUSH
        }
    }

    /// Check whether gapless playback has rolled over to the next track.
    ///
    /// Returns `Some` when the queue has advanced (about-to-finish set a
    /// new URI) AND the position confirms the new track is playing
    /// (position near zero).  This handles same-format gapless where
    /// the pipeline reuses the decoder and resets the stream position.
    ///
    /// Cross-format transitions are handled separately via the
    /// `pending_eos_track_switch` flag and the EOS bus handler.
    fn maybe_emit_natural_handoff(
        queue_state: &Arc<Mutex<GaplessQueue>>,
        snapshot: &mut PlaybackSnapshot,
    ) -> Option<(PathBuf, usize)> {
        if snapshot.state != PlaybackState::Playing {
            return None;
        }
        let Ok(state) = queue_state.lock() else {
            return None;
        };
        let current_path = state.current()?;
        let current_index = state.current_index().unwrap_or(0);
        let path_changed = snapshot.current.as_ref() != Some(&current_path);
        let at_track_start = snapshot.position <= Duration::from_secs(2);
        if path_changed && at_track_start {
            snapshot.current = Some(current_path.clone());
            snapshot.current_queue_index = Some(current_index);
            return Some((current_path, current_index));
        }
        None
    }

    #[allow(clippy::too_many_arguments)]
    fn switch_track(
        playbin: &gst::Element,
        snapshot: &mut PlaybackSnapshot,
        path: &Path,
        uri: &str,
        applied_volume: &mut f32,
        startup_gain_ramp: &mut bool,
        startup_ramp_hold_until: &mut Option<Instant>,
        force_play: bool,
    ) {
        let was_playing = snapshot.state == PlaybackState::Playing || force_play;
        soft_mute(playbin, applied_volume);
        let _ = playbin.set_state(gst::State::Null);
        let _ = playbin.set_state(gst::State::Ready);
        playbin.set_property("uri", uri);
        if was_playing {
            let _ = playbin.set_state(gst::State::Playing);
            // Re-assert mute after state transition to close the race window
            // where new internal elements may not have volume=0.
            playbin.set_property("volume", 0.0_f64);
            snapshot.state = PlaybackState::Playing;
            *startup_gain_ramp = true;
            // AC3/DTS decoders produce garbage frames during startup — hold
            // silence until the decoder has stabilised.  Other codecs produce
            // clean output immediately, so skip the hold to avoid swallowing
            // the start of the track.
            *startup_ramp_hold_until = if is_raw_surround_file(path) {
                Some(Instant::now() + Duration::from_millis(80))
            } else {
                None
            };
        } else if snapshot.state == PlaybackState::Paused {
            let _ = playbin.set_state(gst::State::Paused);
            *startup_gain_ramp = false;
            *startup_ramp_hold_until = None;
        } else {
            *startup_gain_ramp = false;
            *startup_ramp_hold_until = None;
        }
        snapshot.current = Some(path.to_path_buf());
        snapshot.position = Duration::ZERO;
        snapshot.duration = Duration::ZERO;
        snapshot.current_bitrate_kbps = None;
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
        track_token: Arc<AtomicU64>,
        last_rate_hz: u32,
        tap_chunk_samples: usize,
        profile_enabled: bool,
        prof_last: Instant,
        prof_sent: usize,
        prof_dropped: usize,
        prof_samples: usize,
        /// Exponential moving average of per-buffer RMS, used by the PCM
        /// spike detector to identify anomalous decoder output.
        rolling_rms: f32,
        /// Suppress spike detection for the first N buffers after a track
        /// change, while the rolling RMS is still settling from zero.
        spike_warmup_remaining: u32,
        /// Last observed track token, for detecting track changes.
        last_track_token: u64,
    }

    impl AnalysisTapState {
        fn new(
            analysis_tx: Sender<AnalysisCommand>,
            pcm_tx: Sender<AnalysisPcmChunk>,
            track_token: Arc<AtomicU64>,
            tap_chunk_samples: usize,
        ) -> Self {
            Self {
                analysis_tx,
                pcm_tx,
                track_token,
                last_rate_hz: 0,
                tap_chunk_samples,
                profile_enabled: cfg!(feature = "profiling-logs")
                    && std::env::var_os("FERROUS_PROFILE").is_some(),
                prof_last: Instant::now(),
                prof_sent: 0,
                prof_dropped: 0,
                prof_samples: 0,
                rolling_rms: 0.0,
                spike_warmup_remaining: 20,
                last_track_token: 0,
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

            let current_token = self.track_token.load(Ordering::Relaxed);
            if current_token != self.last_track_token {
                self.last_track_token = current_token;
                self.reset_spike_detector();
            }

            let channel_labels = self.channel_labels_for_sample(sample);
            let pcm = decode_interleaved_f32(bytes);
            if pcm.is_empty() {
                return;
            }

            self.check_pcm_spike(&pcm, sample);

            let channels = channel_labels.len().max(1);
            let chunk_width = self.tap_chunk_samples.saturating_mul(channels);
            for part in pcm.chunks(chunk_width.max(channels)) {
                if self
                    .pcm_tx
                    .try_send(AnalysisPcmChunk {
                        samples: part.to_vec(),
                        channel_labels: channel_labels.clone(),
                        track_token: self.track_token.load(Ordering::Relaxed),
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

        /// Detect anomalous amplitude spikes in decoded PCM that may indicate
        /// decoder corruption.  Logs full diagnostic context when triggered.
        fn check_pcm_spike(&mut self, pcm: &[f32], sample: &gst::Sample) {
            // Compute buffer peak and RMS.
            let mut sum_sq: f64 = 0.0;
            let mut peak: f32 = 0.0;
            for &s in pcm {
                let abs = s.abs();
                if abs > peak {
                    peak = abs;
                }
                sum_sq += f64::from(s) * f64::from(s);
            }
            #[allow(clippy::cast_precision_loss, clippy::cast_possible_truncation)]
            let rms = (sum_sq / pcm.len().max(1) as f64).sqrt() as f32;

            // Let the rolling average settle before checking for spikes.
            if self.spike_warmup_remaining > 0 {
                self.spike_warmup_remaining -= 1;
                self.rolling_rms = rms;
                return;
            }

            // Slow-moving exponential average (α ≈ 0.03).  At ~46 buffers/s
            // (1024 samples at 44.1 kHz) the time constant is roughly 0.7 s.
            let alpha: f32 = 0.03;
            self.rolling_rms = self.rolling_rms * (1.0 - alpha) + rms * alpha;

            // Spike criterion: peak near full-scale AND energy far above
            // recent average.  Thresholds are deliberately conservative to
            // avoid false positives on legitimately loud music.
            let spike = peak > 0.95 && self.rolling_rms > 0.001 && rms > self.rolling_rms * 20.0; // ~26 dB above average

            if spike {
                let caps_info = sample
                    .caps()
                    .and_then(|c| gst_audio::AudioInfo::from_caps(c).ok());
                let (channels, rate) = caps_info
                    .as_ref()
                    .map_or((0, 0), |info| (info.channels(), info.rate()));

                let pts = sample
                    .buffer()
                    .and_then(gst::BufferRef::pts)
                    .map_or(0, gst::ClockTime::nseconds);

                eprintln!(
                    "[ferrous] PCM SPIKE DETECTED: peak={peak:.4} rms={rms:.4} \
                     rolling_rms={:.4} ratio={:.1}x | \
                     channels={channels} rate={rate} pts={pts}ns samples={}",
                    self.rolling_rms,
                    rms / self.rolling_rms.max(f32::EPSILON),
                    pcm.len(),
                );

                // Dump a few samples around the peak so we can inspect the
                // waveform shape (descending tone vs white noise vs click).
                #[allow(clippy::float_cmp)] // exact: finding the sample that set `peak`
                if let Some(peak_idx) = pcm.iter().position(|s| s.abs() == peak) {
                    let start = peak_idx.saturating_sub(8);
                    let end = (peak_idx + 9).min(pcm.len());
                    let window: Vec<String> =
                        pcm[start..end].iter().map(|s| format!("{s:.4}")).collect();
                    eprintln!(
                        "[ferrous]   peak at sample {peak_idx}: [{}]",
                        window.join(", ")
                    );
                }
            }
        }

        /// Reset spike detector state for a new track.
        fn reset_spike_detector(&mut self) {
            self.rolling_rms = 0.0;
            self.spike_warmup_remaining = 20;
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
                eprintln!(
                    "[ferrous] failed to build output sink '{output_sink_name}', falling back to autoaudiosink"
                );
                gst::ElementFactory::make("autoaudiosink").build()
            })
            .map_err(|_| anyhow!("missing output sink element"))
    }

    fn build_raw_audio_caps() -> gst::Caps {
        gst::Caps::builder("audio/x-raw").build()
    }

    fn build_analysis_audio_caps() -> gst::Caps {
        gst::Caps::builder("audio/x-raw")
            .field("format", "F32LE")
            .field("layout", "interleaved")
            // Keep analysis workload constant across source formats/codecs.
            .field("rate", 44_100i32)
            .build()
    }

    fn build_capsfilter(caps: &gst::Caps) -> anyhow::Result<gst::Element> {
        let capsfilter = gst::ElementFactory::make("capsfilter")
            .build()
            .map_err(|_| anyhow!("missing capsfilter element"))?;
        capsfilter.set_property("caps", caps);
        Ok(capsfilter)
    }

    fn build_analysis_queue() -> anyhow::Result<gst::Element> {
        let queue = gst::ElementFactory::make("queue")
            .build()
            .map_err(|_| anyhow!("missing queue element"))?;
        queue.set_property_from_str("leaky", "downstream");
        queue.set_property("max-size-buffers", 128u32);
        queue.set_property("max-size-bytes", 0u32);
        queue.set_property("max-size-time", 0u64);
        Ok(queue)
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

    /// Log every caps change that reaches the tee — these indicate decoder
    /// format renegotiations (channel count, sample rate, etc.) that could
    /// be the trigger for noise bursts.
    fn install_caps_change_probe(tee: &gst::Element) {
        let Some(tee_sink) = tee.static_pad("sink") else {
            return;
        };
        tee_sink.add_probe(gst::PadProbeType::EVENT_DOWNSTREAM, |_pad, info| {
            if let Some(gst::PadProbeData::Event(ref event)) = info.data {
                if let gst::EventView::Caps(caps_ev) = event.view() {
                    let caps = caps_ev.caps();
                    eprintln!("[ferrous] audio sink caps change: {caps}");
                }
            }
            gst::PadProbeReturn::Ok
        });
    }

    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    fn build_analysis_audio_sink(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<AnalysisPcmChunk>,
        track_token: Arc<AtomicU64>,
    ) -> anyhow::Result<gst::Bin> {
        let bin = gst::Bin::new();
        let raw_audio_caps = build_raw_audio_caps();
        let analysis_caps = build_analysis_audio_caps();

        let input_capsfilter = build_capsfilter(&raw_audio_caps)?;

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
        let output_capsfilter = build_capsfilter(&raw_audio_caps)?;
        let sink_out = build_output_sink()?;

        let queue_tap = build_analysis_queue()?;
        let conv = gst::ElementFactory::make("audioconvert")
            .build()
            .map_err(|_| anyhow!("missing audioconvert element"))?;
        let resample = gst::ElementFactory::make("audioresample")
            .build()
            .map_err(|_| anyhow!("missing audioresample element"))?;
        let capsfilter = build_capsfilter(&analysis_caps)?;

        // Keep tap synced by default to avoid analysis racing ahead of
        // audible playback; explicit env override is still available for
        // controlled experiments.
        let analysis_sync = std::env::var("FERROUS_GST_ANALYSIS_SYNC")
            .ok()
            .and_then(|raw| raw.parse::<i32>().ok())
            != Some(0);

        let appsink = gst_app::AppSink::builder()
            .caps(&analysis_caps)
            .drop(true)
            .max_buffers(8)
            .sync(analysis_sync)
            .build();

        let tap_chunk_samples = std::env::var("FERROUS_GST_TAP_CHUNK_SAMPLES")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .map_or(2048, |v| v.clamp(256, 16384));
        let mut tap_state =
            AnalysisTapState::new(analysis_tx, pcm_tx, track_token, tap_chunk_samples);

        appsink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |sink| Ok(tap_state.handle_sample(sink)))
                .build(),
        );

        bin.add_many([
            &input_capsfilter,
            &tee,
            &queue_out,
            &conv_out,
            &resample_out,
            &output_capsfilter,
            &sink_out,
            &queue_tap,
            &conv,
            &resample,
            &capsfilter,
            appsink.upcast_ref(),
        ])
        .context("failed to add elements to analysis audio bin")?;

        gst::Element::link_many([&input_capsfilter, &tee])
            .context("failed to link audio sink ingress")?;
        install_caps_change_probe(&tee);
        gst::Element::link_many([
            &queue_out,
            &conv_out,
            &resample_out,
            &output_capsfilter,
            &sink_out,
        ])
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

        let ingress_sink_pad = input_capsfilter
            .static_pad("sink")
            .ok_or_else(|| anyhow!("missing input capsfilter sink pad"))?;
        let ghost = gst::GhostPad::with_target(&ingress_sink_pad)
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
            let first = PathBuf::from("/tmp/gst_handoff_a.flac");
            let second = PathBuf::from("/tmp/gst_handoff_b.flac");
            let queue_state = setup_queue_two_tracks(&first, &second);
            let mut snapshot = PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_millis(800),
                current: Some(first),
                ..PlaybackSnapshot::default()
            };

            let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot);
            assert_eq!(emitted, Some((second.clone(), 1)));
            assert_eq!(snapshot.current.as_ref(), Some(&second));
        }

        #[test]
        fn same_audio_extension_matches_same_codecs() {
            assert!(same_audio_extension(
                Path::new("/tmp/a.flac"),
                Path::new("/tmp/b.flac")
            ));
            assert!(same_audio_extension(
                Path::new("/tmp/a.AC3"),
                Path::new("/tmp/b.ac3")
            ));
        }

        #[test]
        fn same_audio_extension_rejects_different_codecs() {
            assert!(!same_audio_extension(
                Path::new("/tmp/a.flac"),
                Path::new("/tmp/b.ac3")
            ));
            assert!(!same_audio_extension(
                Path::new("/tmp/a.ac3"),
                Path::new("/tmp/b.dts")
            ));
        }

        #[test]
        fn same_audio_extension_rejects_missing_extension() {
            assert!(!same_audio_extension(
                Path::new("/tmp/noext"),
                Path::new("/tmp/b.flac")
            ));
            assert!(!same_audio_extension(
                Path::new("/tmp/a.flac"),
                Path::new("/tmp/noext")
            ));
        }

        #[test]
        fn natural_handoff_does_not_emit_mid_track() {
            let first = PathBuf::from("/tmp/gst_handoff_a.flac");
            let second = PathBuf::from("/tmp/gst_handoff_b.flac");
            let queue_state = setup_queue_two_tracks(&first, &second);
            let mut snapshot = PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_secs(100),
                duration: Duration::from_secs(200),
                current: Some(first.clone()),
                ..PlaybackSnapshot::default()
            };

            let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot);
            assert_eq!(emitted, None);
            assert_eq!(snapshot.current.as_ref(), Some(&first));
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
        fn flac_seek_prefers_fast_non_accurate_mode() {
            let flags = seek_flags_for_path(Some(Path::new("/tmp/test.flac")));
            assert!(flags.contains(gst::SeekFlags::FLUSH));
            assert!(!flags.contains(gst::SeekFlags::ACCURATE));
            assert!(!flags.contains(gst::SeekFlags::KEY_UNIT));
        }

        #[test]
        fn dts_seek_stays_key_unit_based() {
            let flags = seek_flags_for_path(Some(Path::new("/tmp/test.dts")));
            assert!(flags.contains(gst::SeekFlags::FLUSH));
            assert!(flags.contains(gst::SeekFlags::KEY_UNIT));
            assert!(!flags.contains(gst::SeekFlags::ACCURATE));
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

    /// Compare lowercase file extensions to decide whether two tracks can
    /// share a gapless decoder chain.  Conservative: any mismatch (including
    /// missing extensions) falls back to the safe EOS-based switch.
    fn same_audio_extension(a: &Path, b: &Path) -> bool {
        let ext_a = a
            .extension()
            .and_then(|e| e.to_str())
            .map(str::to_lowercase);
        let ext_b = b
            .extension()
            .and_then(|e| e.to_str())
            .map(str::to_lowercase);
        match (ext_a, ext_b) {
            (Some(a), Some(b)) => a == b,
            _ => false,
        }
    }

    fn file_uri(path: &Path) -> Option<String> {
        url::Url::from_file_path(path).ok().map(|u| u.to_string())
    }
}
