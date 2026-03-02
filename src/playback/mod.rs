use std::path::PathBuf;
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};

use crate::analysis::AnalysisCommand;

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum PlaybackState {
    #[default]
    Stopped,
    Playing,
    Paused,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct PlaybackSnapshot {
    pub state: PlaybackState,
    pub position: Duration,
    pub duration: Duration,
    pub current: Option<PathBuf>,
    pub volume: f32,
}

#[derive(Debug, Clone)]
pub enum PlaybackCommand {
    LoadQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    ClearQueue,
    PlayAt(usize),
    Next,
    Previous,
    Play,
    Pause,
    Stop,
    Seek(Duration),
    SetVolume(f32),
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
        kind: TrackChangeKind,
    },
    Seeked,
}

pub struct PlaybackEngine {
    tx: Sender<PlaybackCommand>,
}

impl PlaybackEngine {
    pub fn new(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<Vec<f32>>,
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
                if let PlaybackEvent::TrackChanged { path, kind } = evt {
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
}

#[cfg(not(feature = "gst"))]
mod backend {
    use std::f32::consts::PI;
    use std::path::PathBuf;
    use std::time::{Duration, Instant};

    use crossbeam_channel::{unbounded, Receiver, Sender};

    use crate::analysis::AnalysisCommand;

    use super::{PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, TrackChangeKind};

    pub fn spawn_engine(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<Vec<f32>>,
    ) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
        let (event_tx, event_rx) = unbounded::<PlaybackEvent>();

        std::thread::spawn(move || {
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
                        if let Some(path) = snapshot.current.clone() {
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path,
                                kind: TrackChangeKind::Manual,
                            });
                            let _ = analysis_tx.send(AnalysisCommand::SetSampleRate(48_000));
                        }
                    }
                    PlaybackCommand::AddToQueue(paths) => {
                        queue.extend(paths);
                    }
                    PlaybackCommand::ClearQueue => {
                        queue.clear();
                        queue_idx = 0;
                        snapshot.current = None;
                        snapshot.state = PlaybackState::Stopped;
                        snapshot.position = Duration::ZERO;
                        snapshot.duration = Duration::ZERO;
                    }
                    PlaybackCommand::PlayAt(idx) => {
                        if let Some(path) = queue.get(idx).cloned() {
                            queue_idx = idx;
                            snapshot.current = Some(path.clone());
                            snapshot.position = Duration::ZERO;
                            snapshot.duration = Duration::from_secs(180);
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path,
                                kind: TrackChangeKind::Manual,
                            });
                        }
                    }
                    PlaybackCommand::Next => {
                        if queue_idx + 1 < queue.len() {
                            queue_idx += 1;
                            if let Some(next) = queue.get(queue_idx).cloned() {
                                snapshot.current = Some(next.clone());
                                snapshot.position = Duration::ZERO;
                                snapshot.duration = Duration::from_secs(180);
                                let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                    path: next,
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
                                snapshot.position = Duration::ZERO;
                                snapshot.duration = Duration::from_secs(180);
                                let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                    path: prev,
                                    kind: TrackChangeKind::Manual,
                                });
                            }
                        }
                    }
                    PlaybackCommand::Play => {
                        snapshot.state = PlaybackState::Playing;
                    }
                    PlaybackCommand::Pause => {
                        snapshot.state = PlaybackState::Paused;
                    }
                    PlaybackCommand::Stop => {
                        snapshot.state = PlaybackState::Stopped;
                        snapshot.position = Duration::ZERO;
                    }
                    PlaybackCommand::Seek(pos) => {
                        snapshot.position = pos.min(snapshot.duration);
                        let _ = event_tx.send(PlaybackEvent::Seeked);
                    }
                    PlaybackCommand::SetVolume(vol) => {
                        snapshot.volume = vol.clamp(0.0, 1.0);
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
                            let _ = pcm_tx.try_send(chunk);
                        }

                        if snapshot.state == PlaybackState::Playing
                            && snapshot.position >= snapshot.duration
                        {
                            queue_idx += 1;
                            if let Some(next) = queue.get(queue_idx).cloned() {
                                snapshot.current = Some(next.clone());
                                snapshot.position = Duration::ZERO;
                                snapshot.duration = Duration::from_secs(180);
                                let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                    path: next,
                                    kind: TrackChangeKind::Natural,
                                });
                            } else {
                                snapshot.state = PlaybackState::Stopped;
                                snapshot.position = Duration::ZERO;
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

    use crate::analysis::AnalysisCommand;

    use super::{PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, TrackChangeKind};

    struct GaplessQueue {
        queue: Vec<PathBuf>,
        current_idx: usize,
    }

    impl GaplessQueue {
        fn new() -> Self {
            Self {
                queue: Vec::new(),
                current_idx: 0,
            }
        }

        fn set_queue(&mut self, queue: Vec<PathBuf>) {
            self.queue = queue;
            self.current_idx = 0;
        }

        fn add_to_queue(&mut self, items: Vec<PathBuf>) {
            self.queue.extend(items);
        }

        fn clear(&mut self) {
            self.queue.clear();
            self.current_idx = 0;
        }

        fn current(&self) -> Option<PathBuf> {
            self.queue.get(self.current_idx).cloned()
        }

        fn set_current(&mut self, idx: usize) -> Option<PathBuf> {
            if idx < self.queue.len() {
                self.current_idx = idx;
                self.current()
            } else {
                None
            }
        }

        fn next(&mut self) -> Option<PathBuf> {
            self.current_idx = self.current_idx.saturating_add(1);
            self.queue.get(self.current_idx).cloned()
        }

        fn previous(&mut self) -> Option<PathBuf> {
            if self.current_idx > 0 {
                self.current_idx -= 1;
                return self.current();
            }
            None
        }
    }

    pub fn spawn_engine(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<Vec<f32>>,
    ) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
        let (event_tx, event_rx) = unbounded::<PlaybackEvent>();

        std::thread::spawn(move || {
            if let Err(err) = run_gst_engine(cmd_rx, event_tx.clone(), analysis_tx, pcm_tx) {
                tracing::error!("gstreamer playback engine failed: {err:#}");
            }
        });

        (cmd_tx, event_rx)
    }

    fn run_gst_engine(
        cmd_rx: Receiver<PlaybackCommand>,
        event_tx: Sender<PlaybackEvent>,
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<Vec<f32>>,
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
                let Some(playbin_obj) = maybe_playbin else {
                    return None;
                };

                let next = queue_state.lock().ok().and_then(|mut q| q.next());
                if let Some(next_path) = next {
                    if let Some(uri) = file_uri(&next_path) {
                        playbin_obj.set_property("uri", uri);
                    }
                }
                None
            });
        }

        let bus = playbin.bus().context("playbin has no bus")?;
        let mut snapshot = PlaybackSnapshot {
            volume: 1.0,
            ..PlaybackSnapshot::default()
        };
        let mut target_volume = 1.0f64;
        let mut applied_volume = 1.0f64;
        let mut startup_gain_ramp = false;
        let mut seek_hold: Option<(Instant, Duration)> = None;
        playbin.set_property("volume", applied_volume);

        loop {
            match cmd_rx.recv_timeout(Duration::from_millis(20)) {
                Ok(cmd) => {
                    apply_command(
                        &playbin,
                        &queue_state,
                        &event_tx,
                        &mut snapshot,
                        &mut target_volume,
                        &mut applied_volume,
                        &mut startup_gain_ramp,
                        &mut seek_hold,
                        cmd,
                    );
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => break,
            }

            while let Some(msg) = bus.pop() {
                handle_bus_message(&playbin, &event_tx, &mut snapshot, msg);
            }
        }

        let _ = playbin.set_state(gst::State::Null);
        Ok(())
    }

    fn apply_command(
        playbin: &gst::Element,
        queue_state: &Arc<Mutex<GaplessQueue>>,
        event_tx: &Sender<PlaybackEvent>,
        snapshot: &mut PlaybackSnapshot,
        target_volume: &mut f64,
        applied_volume: &mut f64,
        startup_gain_ramp: &mut bool,
        seek_hold: &mut Option<(Instant, Duration)>,
        cmd: PlaybackCommand,
    ) {
        match cmd {
            PlaybackCommand::LoadQueue(paths) => {
                if paths.is_empty() {
                    return;
                }

                if let Ok(mut state) = queue_state.lock() {
                    state.set_queue(paths);
                    if let Some(first) = state.current() {
                        if let Some(uri) = file_uri(&first) {
                            switch_track(
                                playbin,
                                snapshot,
                                &first,
                                &uri,
                                applied_volume,
                                startup_gain_ramp,
                            );
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path: first.clone(),
                                kind: TrackChangeKind::Manual,
                            });
                            let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                        }
                    }
                }
            }
            PlaybackCommand::AddToQueue(paths) => {
                if let Ok(mut state) = queue_state.lock() {
                    state.add_to_queue(paths);
                }
            }
            PlaybackCommand::ClearQueue => {
                if let Ok(mut state) = queue_state.lock() {
                    state.clear();
                }
                soft_mute(playbin, applied_volume);
                let _ = playbin.set_state(gst::State::Ready);
                *startup_gain_ramp = false;
                snapshot.current = None;
                snapshot.state = PlaybackState::Stopped;
                snapshot.position = Duration::ZERO;
                snapshot.duration = Duration::ZERO;
                let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
            }
            PlaybackCommand::PlayAt(idx) => {
                if let Ok(mut state) = queue_state.lock() {
                    if let Some(path) = state.set_current(idx) {
                        if let Some(uri) = file_uri(&path) {
                            switch_track(
                                playbin,
                                snapshot,
                                &path,
                                &uri,
                                applied_volume,
                                startup_gain_ramp,
                            );
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path: path.clone(),
                                kind: TrackChangeKind::Manual,
                            });
                            let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                        }
                    }
                }
            }
            PlaybackCommand::Next => {
                if let Ok(mut state) = queue_state.lock() {
                    if let Some(path) = state.next() {
                        if let Some(uri) = file_uri(&path) {
                            switch_track(
                                playbin,
                                snapshot,
                                &path,
                                &uri,
                                applied_volume,
                                startup_gain_ramp,
                            );
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path: path.clone(),
                                kind: TrackChangeKind::Manual,
                            });
                            let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                        }
                    }
                }
            }
            PlaybackCommand::Previous => {
                if let Ok(mut state) = queue_state.lock() {
                    if let Some(path) = state.previous() {
                        if let Some(uri) = file_uri(&path) {
                            switch_track(
                                playbin,
                                snapshot,
                                &path,
                                &uri,
                                applied_volume,
                                startup_gain_ramp,
                            );
                            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                                path: path.clone(),
                                kind: TrackChangeKind::Manual,
                            });
                            let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                        }
                    }
                }
            }
            PlaybackCommand::Play => {
                let was_stopped = snapshot.state == PlaybackState::Stopped;
                if was_stopped {
                    // Prime startup gain before entering Playing so first output buffer starts silent.
                    *applied_volume = 0.0;
                    playbin.set_property("volume", *applied_volume);
                    *startup_gain_ramp = true;
                }
                if playbin.set_state(gst::State::Playing).is_ok() {
                    snapshot.state = PlaybackState::Playing;
                    if (*target_volume - *applied_volume).abs() > f64::EPSILON {
                        *startup_gain_ramp = true;
                    }
                    let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                }
            }
            PlaybackCommand::Pause => {
                if playbin.set_state(gst::State::Paused).is_ok() {
                    snapshot.state = PlaybackState::Paused;
                    let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                }
            }
            PlaybackCommand::Stop => {
                soft_mute(playbin, applied_volume);
                if playbin.set_state(gst::State::Ready).is_ok() {
                    *startup_gain_ramp = false;
                    *seek_hold = None;
                    snapshot.state = PlaybackState::Stopped;
                    snapshot.position = Duration::ZERO;
                    let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                }
            }
            PlaybackCommand::Seek(pos) => {
                let nanos = pos.as_nanos().min(u64::MAX as u128) as u64;
                let target = gst::ClockTime::from_nseconds(nanos);
                let _ =
                    playbin.seek_simple(gst::SeekFlags::FLUSH | gst::SeekFlags::ACCURATE, target);
                snapshot.position = pos.min(snapshot.duration);
                *seek_hold = Some((
                    Instant::now() + Duration::from_millis(220),
                    snapshot.position,
                ));
                let _ = event_tx.send(PlaybackEvent::Seeked);
            }
            PlaybackCommand::SetVolume(vol) => {
                *target_volume = vol.clamp(0.0, 1.0) as f64;
                snapshot.volume = *target_volume as f32;
                let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
            }
            PlaybackCommand::Poll => {
                if snapshot.state == PlaybackState::Stopped
                    && !*startup_gain_ramp
                    && seek_hold.is_none()
                {
                    return;
                }
                let mut snapshot_changed = false;
                let delta = *target_volume - *applied_volume;
                if delta.abs() > f64::EPSILON
                    && (snapshot.state == PlaybackState::Playing || *startup_gain_ramp)
                {
                    // Apply a short gain ramp to avoid zipper noise / clicks when dragging volume.
                    let step = if *startup_gain_ramp {
                        0.18_f64
                    } else {
                        0.03_f64
                    };
                    if delta.abs() <= step {
                        *applied_volume = *target_volume;
                        *startup_gain_ramp = false;
                    } else {
                        *applied_volume += delta.signum() * step;
                    }
                    playbin.set_property("volume", *applied_volume);
                    let volume = *applied_volume as f32;
                    if (snapshot.volume - volume).abs() > f32::EPSILON {
                        snapshot.volume = volume;
                        snapshot_changed = true;
                    }
                }
                let mut position_locked = false;
                if let Some((until, target)) = seek_hold.as_ref().copied() {
                    if Instant::now() < until {
                        if snapshot.position != target {
                            snapshot.position = target;
                            snapshot_changed = true;
                        }
                        position_locked = true;
                    } else {
                        *seek_hold = None;
                    }
                }
                if !position_locked && snapshot.state != PlaybackState::Stopped {
                    if let Some(pos) = playbin.query_position::<gst::ClockTime>() {
                        let next_pos = Duration::from_nanos(pos.nseconds());
                        if snapshot.position != next_pos {
                            snapshot.position = next_pos;
                            snapshot_changed = true;
                        }
                    }
                }
                if snapshot.state != PlaybackState::Stopped || snapshot.duration == Duration::ZERO {
                    if let Some(dur) = playbin.query_duration::<gst::ClockTime>() {
                        let next_dur = Duration::from_nanos(dur.nseconds());
                        if snapshot.duration != next_dur {
                            snapshot.duration = next_dur;
                            snapshot_changed = true;
                        }
                    }
                }
                snapshot_changed |= maybe_emit_natural_handoff(queue_state, snapshot, event_tx);
                if snapshot_changed {
                    let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
                }
            }
        }
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
        let path_changed = snapshot.current.as_ref() != Some(&current_path);
        let at_track_start = snapshot.position <= Duration::from_secs(2);
        if path_changed && at_track_start {
            snapshot.current = Some(current_path.clone());
            let _ = event_tx.send(PlaybackEvent::TrackChanged {
                path: current_path,
                kind: TrackChangeKind::Natural,
            });
            return true;
        }
        false
    }

    fn switch_track(
        playbin: &gst::Element,
        snapshot: &mut PlaybackSnapshot,
        path: &PathBuf,
        uri: &str,
        applied_volume: &mut f64,
        startup_gain_ramp: &mut bool,
    ) {
        let was_playing = snapshot.state == PlaybackState::Playing;
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
        snapshot.current = Some(path.clone());
        snapshot.position = Duration::ZERO;
        snapshot.duration = Duration::ZERO;
    }

    fn soft_mute(playbin: &gst::Element, applied_volume: &mut f64) {
        if *applied_volume <= 0.0001 {
            *applied_volume = 0.0;
            playbin.set_property("volume", *applied_volume);
            return;
        }
        for _ in 0..3 {
            *applied_volume *= 0.35;
            if *applied_volume <= 0.0001 {
                *applied_volume = 0.0;
            }
            playbin.set_property("volume", *applied_volume);
            std::thread::sleep(Duration::from_millis(4));
            if *applied_volume == 0.0 {
                break;
            }
        }
        *applied_volume = 0.0;
        playbin.set_property("volume", *applied_volume);
    }

    fn handle_bus_message(
        _playbin: &gst::Element,
        event_tx: &Sender<PlaybackEvent>,
        snapshot: &mut PlaybackSnapshot,
        msg: gst::Message,
    ) {
        match msg.view() {
            gst::MessageView::Eos(..) => {
                snapshot.state = PlaybackState::Stopped;
                snapshot.position = Duration::ZERO;
                let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
            }
            gst::MessageView::Error(err) => {
                tracing::error!(
                    "gstreamer error from {:?}: {} ({:?})",
                    err.src().map(gstreamer::prelude::GstObjectExt::path_string),
                    err.error(),
                    err.debug()
                );
                snapshot.state = PlaybackState::Stopped;
                let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
            }
            _ => {}
        }
    }

    fn build_analysis_audio_sink(
        analysis_tx: Sender<AnalysisCommand>,
        pcm_tx: Sender<Vec<f32>>,
    ) -> anyhow::Result<gst::Bin> {
        let bin = gst::Bin::new();

        let tee = gst::ElementFactory::make("tee")
            .build()
            .map_err(|_| anyhow!("missing tee element"))?;

        let queue_out = gst::ElementFactory::make("queue")
            .build()
            .map_err(|_| anyhow!("missing queue element"))?;
        let output_sink_name = std::env::var("FERROUS_GST_OUTPUT_SINK")
            .ok()
            .filter(|s| !s.trim().is_empty())
            .unwrap_or_else(|| "autoaudiosink".to_string());
        let sink_out = gst::ElementFactory::make(&output_sink_name)
            .build()
            .or_else(|_| {
                tracing::warn!(
                    "failed to build output sink '{}', falling back to autoaudiosink",
                    output_sink_name
                );
                gst::ElementFactory::make("autoaudiosink").build()
            })
            .map_err(|_| anyhow!("missing output sink element"))?;

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
            .field("channels", 1i32)
            // Keep analysis workload constant across source formats/codecs.
            .field("rate", 44_100i32)
            .build();
        capsfilter.set_property("caps", &caps);

        let appsink = gst_app::AppSink::builder()
            .caps(&caps)
            .drop(true)
            .max_buffers(8)
            .sync(true)
            .build();

        appsink.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample({
                    let mut last_rate_hz: u32 = 0;
                    let profile_enabled = std::env::var_os("FERROUS_PROFILE").is_some();
                    let mut prof_last = std::time::Instant::now();
                    let mut prof_sent = 0usize;
                    let mut prof_dropped = 0usize;
                    let mut prof_samples = 0usize;
                    move |sink| {
                        if let Ok(sample) = sink.pull_sample() {
                            if let Some(buffer) = sample.buffer() {
                                if let Ok(map) = buffer.map_readable() {
                                    let bytes = map.as_slice();
                                    if !bytes.is_empty() {
                                        let mut pcm = Vec::with_capacity(bytes.len() / 4);
                                        for chunk in bytes.chunks_exact(4) {
                                            pcm.push(f32::from_le_bytes([
                                                chunk[0], chunk[1], chunk[2], chunk[3],
                                            ]));
                                        }
                                        if !pcm.is_empty() {
                                            if let Some(caps) = sample.caps() {
                                                if let Some(s) = caps.structure(0) {
                                                    if let Ok(rate) = s.get::<i32>("rate") {
                                                        if rate > 0 && last_rate_hz != rate as u32 {
                                                            last_rate_hz = rate as u32;
                                                            let _ = analysis_tx.send(
                                                                AnalysisCommand::SetSampleRate(
                                                                    rate as u32,
                                                                ),
                                                            );
                                                        }
                                                    }
                                                }
                                            }
                                            // Split large buffers into smaller chunks for smoother analysis pacing.
                                            for part in pcm.chunks(512) {
                                                if pcm_tx.try_send(part.to_vec()).is_ok() {
                                                    prof_sent += 1;
                                                    prof_samples += part.len();
                                                } else {
                                                    prof_dropped += 1;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if profile_enabled && prof_last.elapsed() >= Duration::from_secs(1) {
                            eprintln!(
                                "[gst] pcm_chunks sent/s={} dropped/s={} samples/s={} rate={}Hz",
                                prof_sent, prof_dropped, prof_samples, last_rate_hz
                            );
                            prof_last = std::time::Instant::now();
                            prof_sent = 0;
                            prof_dropped = 0;
                            prof_samples = 0;
                        }
                        Ok(gst::FlowSuccess::Ok)
                    }
                })
                .build(),
        );

        bin.add_many([
            &tee,
            &queue_out,
            &sink_out,
            &queue_tap,
            &conv,
            &resample,
            &capsfilter,
            appsink.upcast_ref(),
        ])
        .context("failed to add elements to analysis audio bin")?;

        gst::Element::link_many([&queue_out, &sink_out]).context("failed to link output branch")?;
        gst::Element::link_many([
            &queue_tap,
            &conv,
            &resample,
            &capsfilter,
            appsink.upcast_ref(),
        ])
        .context("failed to link analysis branch")?;

        let tee_out_pad = tee
            .request_pad_simple("src_%u")
            .ok_or_else(|| anyhow!("failed requesting tee src pad for output"))?;
        let queue_out_sink_pad = queue_out
            .static_pad("sink")
            .ok_or_else(|| anyhow!("missing queue_out sink pad"))?;
        tee_out_pad
            .link(&queue_out_sink_pad)
            .map_err(|e| anyhow!("failed linking tee->output queue: {e:?}"))?;

        let tee_tap_pad = tee
            .request_pad_simple("src_%u")
            .ok_or_else(|| anyhow!("failed requesting tee src pad for analysis"))?;
        let queue_tap_sink_pad = queue_tap
            .static_pad("sink")
            .ok_or_else(|| anyhow!("missing queue_tap sink pad"))?;
        tee_tap_pad
            .link(&queue_tap_sink_pad)
            .map_err(|e| anyhow!("failed linking tee->analysis queue: {e:?}"))?;

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

        fn setup_queue_two_tracks(a: &PathBuf, b: &PathBuf) -> Arc<Mutex<GaplessQueue>> {
            let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));
            let mut queue = queue_state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            queue.set_queue(vec![a.clone(), b.clone()]);
            let _ = queue.next();
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
                PlaybackEvent::TrackChanged { path, kind } => {
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
    }

    fn file_uri(path: &Path) -> Option<String> {
        url::Url::from_file_path(path).ok().map(|u| u.to_string())
    }
}
