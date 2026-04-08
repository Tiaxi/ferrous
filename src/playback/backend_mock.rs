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
            let mut solo_pre_mask: Option<u64> = None;
            let mut soloed_channel: Option<u8> = None;

            macro_rules! reset_mute {
                () => {
                    snapshot.muted_channels_mask = 0;
                    snapshot.soloed_channel = None;
                    solo_pre_mask = None;
                    soloed_channel = None;
                };
            }

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
                        reset_mute!();
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
                            reset_mute!();
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
                        reset_mute!();
                        snapshot.current = None;
                        snapshot.current_queue_index = None;
                        snapshot.state = PlaybackState::Stopped;
                        snapshot.position = Duration::ZERO;
                        snapshot.duration = Duration::ZERO;
                    }
                    PlaybackCommand::PlayAt(idx) => {
                        if let Some(path) = queue.get(idx).cloned() {
                            queue_idx = idx;
                            reset_mute!();
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
                            reset_mute!();
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
                            reset_mute!();
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
                        reset_mute!();
                        snapshot.state = PlaybackState::Stopped;
                        snapshot.position = Duration::ZERO;
                        snapshot.current_queue_index = None;
                    }
                    PlaybackCommand::Seek(pos) => {
                        snapshot.position = pos.min(snapshot.duration);
                        let _ = event_tx.send(PlaybackEvent::Seeked {
                            position: snapshot.position,
                        });
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
                    PlaybackCommand::ToggleChannelMute(ch) => {
                        let ch = ch.min(63);
                        if soloed_channel == Some(ch) {
                            if let Some(pre) = solo_pre_mask.take() {
                                snapshot.muted_channels_mask = pre;
                            }
                            soloed_channel = None;
                        } else {
                            snapshot.muted_channels_mask ^= 1u64 << ch;
                            solo_pre_mask = None;
                            soloed_channel = None;
                        }
                        snapshot.soloed_channel = soloed_channel;
                    }
                    PlaybackCommand::SoloChannel(ch) => {
                        let ch = ch.min(63);
                        if soloed_channel == Some(ch) {
                            // solo_pre_mask is always Some when soloed_channel is Some.
                            if let Some(pre) = solo_pre_mask.take() {
                                snapshot.muted_channels_mask = pre;
                            }
                            soloed_channel = None;
                        } else {
                            if soloed_channel.is_none() {
                                solo_pre_mask = Some(snapshot.muted_channels_mask);
                            }
                            snapshot.muted_channels_mask = u64::MAX ^ (1u64 << ch);
                            soloed_channel = Some(ch);
                        }
                        snapshot.soloed_channel = soloed_channel;
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
                            let next_queue_idx = queue_idx + 1;
                            if let Some(next) = queue.get(next_queue_idx).cloned() {
                                queue_idx = next_queue_idx;
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
                                super::stop_snapshot_at_terminal_eos(&mut snapshot);
                            }
                        }
                    }
                }

                let _ = event_tx.send(PlaybackEvent::Snapshot(snapshot.clone()));
            }
        });

    (cmd_tx, event_rx)
}
