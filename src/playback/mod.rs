// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::PathBuf;
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};

use crate::analysis::{AnalysisCommand, AnalysisPcmChunk};

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
    pub muted_channels_mask: u64,
    pub soloed_channel: Option<u8>,
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
    Seeked {
        position: Duration,
    },
}

pub struct PlaybackEngine {
    tx: Sender<PlaybackCommand>,
}

fn stop_snapshot_at_terminal_eos(snapshot: &mut PlaybackSnapshot) {
    snapshot.state = PlaybackState::Stopped;
    snapshot.position = Duration::ZERO;
    snapshot.current_bitrate_kbps = None;
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

#[cfg(test)]
mod shared_tests {
    use std::path::PathBuf;
    use std::time::Duration;

    use super::{stop_snapshot_at_terminal_eos, PlaybackSnapshot, PlaybackState};

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
            ..PlaybackSnapshot::default()
        };

        stop_snapshot_at_terminal_eos(&mut snapshot);

        assert_eq!(snapshot.state, PlaybackState::Stopped);
        assert_eq!(snapshot.position, Duration::ZERO);
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
            if let Ok(evt) = rx.recv_timeout(Duration::from_millis(10)) {
                if let PlaybackEvent::Snapshot(s) = evt {
                    last = Some(s);
                }
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
            if let Ok(evt) = rx.recv_timeout(Duration::from_millis(10)) {
                if let PlaybackEvent::Seeked { position } = evt {
                    return Some(position);
                }
            }
        }
        None
    }

    fn make_test_engine() -> (PlaybackEngine, crossbeam_channel::Receiver<PlaybackEvent>) {
        let (analysis_tx, _) = unbounded();
        let (pcm_tx, _) = unbounded();
        PlaybackEngine::new(analysis_tx, pcm_tx)
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

        assert_eq!(
            recv_seeked_event(&rx, Duration::from_millis(300)),
            Some(Duration::from_secs(180))
        );
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

#[cfg(not(feature = "gst"))]
#[path = "backend_mock.rs"]
mod backend;

#[cfg(feature = "gst")]
#[path = "backend_gst.rs"]
mod backend;
