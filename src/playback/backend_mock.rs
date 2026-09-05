// SPDX-License-Identifier: GPL-3.0-or-later

use super::{PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, TrackChangeKind};
use crate::analysis::AnalysisCommand;
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::path::PathBuf;
use std::time::{Duration, Instant};

pub fn spawn_engine(
    analysis_tx: Sender<AnalysisCommand>,
) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
    let (cmd_tx, cmd_rx) = unbounded();
    let (event_tx, event_rx) = unbounded();
    let _ = std::thread::Builder::new()
        .name("ferrous-playback-sim".to_string())
        .spawn(move || {
            let mut runtime = MockPlaybackRuntime {
                snapshot: PlaybackSnapshot {
                    volume: 1.0,
                    ..PlaybackSnapshot::default()
                },
                queue: Vec::new(),
                queue_idx: 0,
                solo_pre_mask: None,
                soloed_channel: None,
                event_tx,
                analysis_tx,
            };
            let mut last_tick = Instant::now();
            while let Ok(cmd) = cmd_rx.recv() {
                if runtime.snapshot.state == PlaybackState::Playing {
                    runtime.snapshot.position = runtime
                        .snapshot
                        .position
                        .saturating_add(last_tick.elapsed());
                }
                last_tick = Instant::now();
                runtime.handle_command(cmd);
                let _ = runtime
                    .event_tx
                    .send(PlaybackEvent::Snapshot(runtime.snapshot.clone()));
            }
        });
    (cmd_tx, event_rx)
}

struct MockPlaybackRuntime {
    snapshot: PlaybackSnapshot,
    queue: Vec<PathBuf>,
    queue_idx: usize,
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
                self.queue.extend(paths);
            }
            PlaybackCommand::RemoveMany(indices) => self.remove_many(&indices),
            PlaybackCommand::RemoveAt(idx) => self.remove_at(idx),
            PlaybackCommand::MoveQueue { from, to } => self.move_queue(from, to),
            PlaybackCommand::ClearQueue => self.clear_queue(),
            PlaybackCommand::PlayAt(idx) => self.play_at(idx),
            PlaybackCommand::Next => self.next(),
            PlaybackCommand::Previous => self.previous(),
            PlaybackCommand::Play => {
                self.snapshot.state = PlaybackState::Playing;
                self.snapshot.current_queue_index = if self.snapshot.current.is_some() {
                    Some(self.queue_idx)
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
                self.reset_mute();
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.position = Duration::ZERO;
                self.snapshot.current_queue_index = None;
            }
            PlaybackCommand::Seek(pos) => {
                self.snapshot.position = pos.min(self.snapshot.duration);
                let _ = self.event_tx.send(PlaybackEvent::Seeked {
                    position: self.snapshot.position,
                });
            }
            PlaybackCommand::SetVolume(vol) => {
                self.snapshot.volume = vol.clamp(0.0, 1.0);
            }
            PlaybackCommand::SetRepeatMode(mode) => {
                self.snapshot.repeat_mode = mode;
            }
            PlaybackCommand::SetShuffle(enabled) => {
                self.snapshot.shuffle_enabled = enabled;
            }
            PlaybackCommand::ToggleChannelMute(ch) => self.toggle_channel_mute(ch),
            PlaybackCommand::SoloChannel(ch) => self.solo_channel(ch),
            PlaybackCommand::Poll => self.poll(),
        }
    }
    fn load_queue(&mut self, paths: Vec<PathBuf>) {
        self.queue = paths;
        self.queue_idx = 0;
        self.reset_mute();
        self.snapshot.position = Duration::ZERO;
        self.snapshot.duration = Duration::from_secs(180);
        self.snapshot.current = self.queue.first().cloned();
        self.snapshot.current_queue_index = if self.snapshot.current.is_some() {
            Some(self.queue_idx)
        } else {
            None
        };
        if let Some(path) = self.snapshot.current.clone() {
            let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                path,
                queue_index: self.queue_idx,
                kind: TrackChangeKind::Manual,
                track_token: 0,
            });
            let _ = self
                .analysis_tx
                .send(AnalysisCommand::SetSampleRate(48_000));
        }
    }
    fn remove_many(&mut self, indices: &[usize]) {
        let old_current = self.snapshot.current.clone();
        let next = super::remove_queue_indices(&mut self.queue, Some(self.queue_idx), indices);
        self.queue_idx = next.unwrap_or(0);
        self.snapshot.current_queue_index = next;
        self.snapshot.current = next.and_then(|index| self.queue.get(index).cloned());
        if self.snapshot.current != old_current {
            self.reset_mute();
            self.snapshot.position = Duration::ZERO;
            if let Some(path) = self.snapshot.current.clone() {
                self.snapshot.duration = Duration::from_secs(180);
                let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                    path,
                    queue_index: self.queue_idx,
                    kind: TrackChangeKind::Manual,
                    track_token: 0,
                });
            } else {
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.duration = Duration::ZERO;
            }
        }
    }
    fn remove_at(&mut self, idx: usize) {
        if idx < self.queue.len() {
            self.queue.remove(idx);
            self.reset_mute();
            if self.queue.is_empty() {
                self.queue_idx = 0;
                self.snapshot.current = None;
                self.snapshot.current_queue_index = None;
                self.snapshot.state = PlaybackState::Stopped;
                self.snapshot.position = Duration::ZERO;
                self.snapshot.duration = Duration::ZERO;
            } else {
                if idx < self.queue_idx {
                    self.queue_idx = self.queue_idx.saturating_sub(1);
                } else if idx == self.queue_idx && self.queue_idx >= self.queue.len() {
                    self.queue_idx = self.queue.len().saturating_sub(1);
                }
                self.snapshot.current = self.queue.get(self.queue_idx).cloned();
                self.snapshot.current_queue_index = Some(self.queue_idx);
                self.snapshot.position = Duration::ZERO;
                self.snapshot.duration = Duration::from_secs(180);
                if let Some(path) = self.snapshot.current.clone() {
                    let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                        path,
                        queue_index: self.queue_idx,
                        kind: TrackChangeKind::Manual,
                        track_token: 0,
                    });
                }
            }
        }
    }
    fn move_queue(&mut self, from: usize, to: usize) {
        if from < self.queue.len() && to < self.queue.len() && from != to {
            let item = self.queue.remove(from);
            self.queue.insert(to, item);
            if self.queue_idx == from {
                self.queue_idx = to;
            } else if from < self.queue_idx && to >= self.queue_idx {
                self.queue_idx = self.queue_idx.saturating_sub(1);
            } else if from > self.queue_idx && to <= self.queue_idx {
                self.queue_idx += 1;
            }
            self.snapshot.current = self.queue.get(self.queue_idx).cloned();
            self.snapshot.current_queue_index = if self.snapshot.current.is_some() {
                Some(self.queue_idx)
            } else {
                None
            };
        }
    }
    fn clear_queue(&mut self) {
        self.queue.clear();
        self.queue_idx = 0;
        self.reset_mute();
        self.snapshot.current = None;
        self.snapshot.current_queue_index = None;
        self.snapshot.state = PlaybackState::Stopped;
        self.snapshot.position = Duration::ZERO;
        self.snapshot.duration = Duration::ZERO;
    }
    fn play_at(&mut self, idx: usize) {
        if let Some(path) = self.queue.get(idx).cloned() {
            self.queue_idx = idx;
            self.reset_mute();
            self.snapshot.current = Some(path.clone());
            self.snapshot.current_queue_index = Some(self.queue_idx);
            self.snapshot.position = Duration::ZERO;
            self.snapshot.duration = Duration::from_secs(180);
            let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                path,
                queue_index: self.queue_idx,
                kind: TrackChangeKind::Manual,
                track_token: 0,
            });
        }
    }
    fn next(&mut self) {
        if self.queue_idx + 1 < self.queue.len() {
            self.queue_idx += 1;
            self.reset_mute();
            if let Some(next) = self.queue.get(self.queue_idx).cloned() {
                self.snapshot.current = Some(next.clone());
                self.snapshot.current_queue_index = Some(self.queue_idx);
                self.snapshot.position = Duration::ZERO;
                self.snapshot.duration = Duration::from_secs(180);
                if self.snapshot.state == PlaybackState::Paused {
                    self.snapshot.state = PlaybackState::Playing;
                }
                let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                    path: next,
                    queue_index: self.queue_idx,
                    kind: TrackChangeKind::Manual,
                    track_token: 0,
                });
            }
        }
    }
    fn previous(&mut self) {
        if self.queue_idx > 0 {
            self.queue_idx -= 1;
            self.reset_mute();
            if let Some(prev) = self.queue.get(self.queue_idx).cloned() {
                self.snapshot.current = Some(prev.clone());
                self.snapshot.current_queue_index = Some(self.queue_idx);
                self.snapshot.position = Duration::ZERO;
                self.snapshot.duration = Duration::from_secs(180);
                if self.snapshot.state == PlaybackState::Paused {
                    self.snapshot.state = PlaybackState::Playing;
                }
                let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                    path: prev,
                    queue_index: self.queue_idx,
                    kind: TrackChangeKind::Manual,
                    track_token: 0,
                });
            }
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
        if self.snapshot.state == PlaybackState::Playing
            && self.snapshot.position >= self.snapshot.duration
        {
            let next_queue_idx = self.queue_idx + 1;
            if let Some(next) = self.queue.get(next_queue_idx).cloned() {
                self.queue_idx = next_queue_idx;
                self.snapshot.current = Some(next.clone());
                self.snapshot.current_queue_index = Some(self.queue_idx);
                self.snapshot.position = Duration::ZERO;
                self.snapshot.duration = Duration::from_secs(180);
                let _ = self.event_tx.send(PlaybackEvent::TrackChanged {
                    path: next,
                    queue_index: self.queue_idx,
                    kind: TrackChangeKind::Gapless,
                    track_token: 0,
                });
            } else {
                super::stop_snapshot_at_terminal_eos(&mut self.snapshot);
            }
        }
    }
}
