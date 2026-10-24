use std::collections::VecDeque;
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

use crate::analysis::AnalysisCommand;
#[cfg(feature = "profiling-logs")]
use crate::profile_logging::heartbeat_trace_enabled;
use crate::raw_audio::{
    configure_raw_surround_decoding, install_raw_surround_source_probe, is_dts_file,
    is_raw_surround_file,
};

use super::queue::{
    same_audio_extension, CancelledAdvance, CommittedAdvance, GaplessQueue, Handoff,
};
use super::{
    PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode, TrackChangeKind,
};

#[cfg(any(test, feature = "profiling-logs"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GstPositionTrace {
    previous: i128,
    current: i128,
    delta: i128,
    jumped_backward: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PlaybackClockMode {
    Steady,
    SeekHold,
    SeekReacquire,
    Discontinuity,
}

#[derive(Debug, Clone)]
struct SmoothedPlaybackClock {
    anchor_position: Duration,
    anchor_instant: Instant,
    rate: f64,
    last_raw_position: Option<Duration>,
    initialized: bool,
    mode: PlaybackClockMode,
}

impl SmoothedPlaybackClock {
    const SNAP_ERROR_SECONDS: f64 = 0.75;
    const IGNORE_ERROR_SECONDS: f64 = 0.012;
    const PHASE_RECOVERY_HORIZON_SECONDS: f64 = 0.75;
    const PHASE_RATE_OFFSET_MAX: f64 = 0.10;
    const RATE_CLAMP_MIN: f64 = 0.92;
    const RATE_CLAMP_MAX: f64 = 1.10;

    fn new(now: Instant) -> Self {
        Self {
            anchor_position: Duration::ZERO,
            anchor_instant: now,
            rate: 1.0,
            last_raw_position: None,
            initialized: false,
            mode: PlaybackClockMode::Discontinuity,
        }
    }

    fn current_position(&self, now: Instant) -> Duration {
        if !self.initialized {
            return self.anchor_position;
        }
        let elapsed_seconds = now
            .checked_duration_since(self.anchor_instant)
            .unwrap_or(Duration::ZERO)
            .as_secs_f64();
        add_signed_seconds(self.anchor_position, elapsed_seconds * self.rate)
    }

    fn reset(&mut self, position: Duration, now: Instant) {
        self.reset_with_mode(position, now, PlaybackClockMode::Discontinuity);
    }

    fn reset_with_mode(&mut self, position: Duration, now: Instant, mode: PlaybackClockMode) {
        self.anchor_position = position;
        self.anchor_instant = now;
        self.rate = 1.0;
        self.last_raw_position = Some(position);
        self.initialized = true;
        self.mode = mode;
    }

    fn last_raw_position(&self) -> Option<Duration> {
        self.last_raw_position
    }

    #[cfg(feature = "profiling-logs")]
    fn mode(&self) -> PlaybackClockMode {
        self.mode
    }

    fn enter_seek_hold(&mut self, target: Duration, now: Instant) {
        self.reset_with_mode(target, now, PlaybackClockMode::SeekHold);
        self.rate = 0.0;
    }

    fn enter_seek_reacquire(&mut self) {
        self.mode = PlaybackClockMode::SeekReacquire;
    }

    fn update_playing_sample(&mut self, raw_position: Duration, now: Instant) -> Duration {
        if !self.initialized {
            self.reset_with_mode(raw_position, now, PlaybackClockMode::Steady);
            return raw_position;
        }

        let predicted = self.current_position(now);
        let error_seconds = raw_position.as_secs_f64() - predicted.as_secs_f64();
        if error_seconds.abs() >= Self::SNAP_ERROR_SECONDS {
            self.reset(raw_position, now);
            return raw_position;
        }

        if matches!(self.mode, PlaybackClockMode::SeekReacquire) {
            self.reset_with_mode(raw_position, now, PlaybackClockMode::Steady);
            return raw_position;
        }

        let phase_rate_offset = if error_seconds.abs() <= Self::IGNORE_ERROR_SECONDS {
            0.0
        } else {
            (error_seconds / Self::PHASE_RECOVERY_HORIZON_SECONDS)
                .clamp(-Self::PHASE_RATE_OFFSET_MAX, Self::PHASE_RATE_OFFSET_MAX)
        };
        // Playback is fixed at the MPRIS/GStreamer nominal rate of 1.0.
        // Do not infer a base rate from adjacent position queries: scheduling
        // delays make individual query deltas irregular, and a delayed sample
        // followed by a catch-up must not become a false playback rate. The
        // bounded phase offset below corrects sustained clock error instead.
        self.rate = (1.0 + phase_rate_offset).clamp(Self::RATE_CLAMP_MIN, Self::RATE_CLAMP_MAX);
        let corrected = predicted;
        self.anchor_position = corrected;
        self.anchor_instant = now;
        self.last_raw_position = Some(raw_position);
        self.mode = PlaybackClockMode::Steady;
        corrected
    }
}

#[cfg(any(test, feature = "profiling-logs"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GstClockTrace {
    current_state: gst::State,
    pending_state: gst::State,
    running: i128,
    clock: i128,
    base: i128,
}

#[cfg(any(test, feature = "profiling-logs"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct RecentSeekTrace {
    target: i128,
    since_issue: i128,
    since_release: i128,
}

#[cfg(any(test, feature = "profiling-logs"))]
fn duration_ms_i128(value: Duration) -> i128 {
    i128::try_from(value.as_millis()).unwrap_or(i128::MAX)
}

fn add_signed_seconds(base: Duration, delta_seconds: f64) -> Duration {
    if delta_seconds >= 0.0 {
        base.saturating_add(Duration::from_secs_f64(delta_seconds))
    } else {
        let magnitude = Duration::from_secs_f64(-delta_seconds);
        base.checked_sub(magnitude).unwrap_or(Duration::ZERO)
    }
}

fn release_seek_hold_sample(
    clock: &mut SmoothedPlaybackClock,
    _target: Duration,
    _released_at: Instant,
    raw_position: Duration,
    now: Instant,
) -> Duration {
    clock.enter_seek_reacquire();
    clock.update_playing_sample(raw_position, now)
}

#[cfg(any(test, feature = "profiling-logs"))]
fn clock_time_ms_i128(value: Option<gst::ClockTime>) -> i128 {
    value.map_or(-1, |time| i128::from(time.mseconds()))
}

#[cfg(any(test, feature = "profiling-logs"))]
fn gst_position_trace(previous: Duration, current: Duration) -> GstPositionTrace {
    let previous_ms = duration_ms_i128(previous);
    let current_ms = duration_ms_i128(current);
    let delta_ms = current_ms - previous_ms;
    GstPositionTrace {
        previous: previous_ms,
        current: current_ms,
        delta: delta_ms,
        jumped_backward: delta_ms <= -1_000,
    }
}

#[cfg(any(test, feature = "profiling-logs"))]
fn gst_clock_trace(
    current_state: gst::State,
    pending_state: gst::State,
    running_time: Option<gst::ClockTime>,
    clock_time: Option<gst::ClockTime>,
    base_time: Option<gst::ClockTime>,
) -> GstClockTrace {
    GstClockTrace {
        current_state,
        pending_state,
        running: clock_time_ms_i128(running_time),
        clock: clock_time_ms_i128(clock_time),
        base: clock_time_ms_i128(base_time),
    }
}

#[cfg(any(test, feature = "profiling-logs"))]
fn recent_seek_trace(
    target: Option<Duration>,
    since_issue: Option<Duration>,
    since_release: Option<Duration>,
) -> RecentSeekTrace {
    RecentSeekTrace {
        target: target.map_or(-1, duration_ms_i128),
        since_issue: since_issue.map_or(-1, duration_ms_i128),
        since_release: since_release.map_or(-1, duration_ms_i128),
    }
}

struct GstPlaybackRuntime {
    playbin: gst::Element,
    queue_state: Arc<Mutex<GaplessQueue>>,
    analysis_tx: Sender<AnalysisCommand>,
    /// Local counter for generating unique track tokens (waveform jobs,
    /// track-changed events).  Always incremented, even for gapless.
    track_token_counter: u64,
    event_tx: Sender<PlaybackEvent>,
    snapshot: PlaybackSnapshot,
    /// Linear gain for the requested volume level (`snapshot.volume`).
    target_volume: f32,
    /// Linear gain currently set on playbin.
    applied_volume: f32,
    startup_gain_ramp: bool,
    startup_ramp_hold_until: Option<Instant>,
    /// Time of the previous fade step; `None` while no fade is running.
    volume_ramp_tick_at: Option<Instant>,
    buffering_active: bool,
    seek_hold: Option<(Instant, Duration)>,
    last_seek_issued_at: Option<Instant>,
    last_seek_released_at: Option<Instant>,
    last_seek_target: Option<Duration>,
    position_clock: SmoothedPlaybackClock,
    /// The next `stream-start` belongs to the track the pipeline was last
    /// started with, not to a staged successor.
    awaiting_run_stream_start: bool,
    /// Group of the last `stream-start`. Chained streams within one file
    /// share their group; each file the pipeline starts gets a new one.
    last_stream_group: Option<gst::GroupId>,
    stream_durations: Arc<Mutex<StreamDurations>>,
    /// Set when `set_state(Playing)` is called in `switch_track`.
    /// Cleared when the bus handler sees `StateChanged(_, Playing)` for
    /// the playbin.  Used by `poll()` to detect stuck state transitions.
    playing_state_requested_at: Option<Instant>,
    channel_mute_mask: Arc<AtomicU64>,
    solo_pre_mask: Option<u64>,
    soloed_channel: Option<u8>,
}

pub fn spawn_engine(
    analysis_tx: Sender<AnalysisCommand>,
) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
    spawn_engine_with_output(analysis_tx, build_output_sink)
}

/// Spawn the engine with the audio output that `build_output` creates
/// after `GStreamer` is initialized.
fn spawn_engine_with_output(
    analysis_tx: Sender<AnalysisCommand>,
    build_output: fn() -> anyhow::Result<gst::Element>,
) -> (Sender<PlaybackCommand>, Receiver<PlaybackEvent>) {
    let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
    let (event_tx, event_rx) = unbounded::<PlaybackEvent>();

    let _ = std::thread::Builder::new()
        .name("ferrous-playback-gst".to_string())
        .spawn(move || {
            if let Err(err) = run_gst_engine(&cmd_rx, event_tx.clone(), analysis_tx, build_output) {
                eprintln!("[ferrous] gstreamer playback engine failed: {err:#}");
            }
        });

    (cmd_tx, event_rx)
}

impl GstPlaybackRuntime {
    fn command_wait_timeout(&self) -> Duration {
        if self.volume_ramp_pending() {
            VOLUME_RAMP_TICK
        } else if self.snapshot.state == PlaybackState::Playing
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

        event_tx: Sender<PlaybackEvent>,
        channel_mute_mask: Arc<AtomicU64>,
        stream_durations: Arc<Mutex<StreamDurations>>,
    ) -> Self {
        Self {
            playbin,
            queue_state,
            analysis_tx,

            track_token_counter: 0,
            event_tx,
            snapshot: PlaybackSnapshot {
                volume: 1.0,
                ..PlaybackSnapshot::default()
            },
            target_volume: 1.0,
            applied_volume: 1.0,
            startup_gain_ramp: false,
            startup_ramp_hold_until: None,
            volume_ramp_tick_at: None,
            buffering_active: false,
            seek_hold: None,
            last_seek_issued_at: None,
            last_seek_released_at: None,
            last_seek_target: None,
            position_clock: SmoothedPlaybackClock::new(Instant::now()),
            awaiting_run_stream_start: true,
            last_stream_group: None,
            stream_durations,
            playing_state_requested_at: None,
            channel_mute_mask,
            solo_pre_mask: None,
            soloed_channel: None,
        }
    }

    fn emit_snapshot(&self) {
        let _ = self
            .event_tx
            .send(PlaybackEvent::Snapshot(self.snapshot.clone()));
    }

    fn clear_recent_seek_trace(&mut self) {
        self.last_seek_issued_at = None;
        self.last_seek_released_at = None;
        self.last_seek_target = None;
    }

    fn begin_seek_hold(&mut self, target: Duration) {
        let issued_at = Instant::now();
        self.snapshot.position = target;
        self.position_clock.enter_seek_hold(target, issued_at);
        self.last_seek_issued_at = Some(issued_at);
        self.last_seek_released_at = None;
        self.last_seek_target = Some(target);
        self.seek_hold = Some((issued_at + Duration::from_millis(220), target));
    }

    #[cfg(feature = "profiling-logs")]
    fn recent_seek_trace(&self) -> RecentSeekTrace {
        recent_seek_trace(
            self.last_seek_target,
            self.last_seek_issued_at.map(|issued| issued.elapsed()),
            self.last_seek_released_at
                .map(|released| released.elapsed()),
        )
    }

    #[cfg(feature = "profiling-logs")]
    fn clock_trace(&self) -> GstClockTrace {
        gst_clock_trace(
            self.playbin.current_state(),
            self.playbin.pending_state(),
            self.playbin.current_running_time(),
            self.playbin.current_clock_time(),
            self.playbin.base_time(),
        )
    }

    #[cfg(feature = "profiling-logs")]
    fn log_seek_event(
        &self,
        requested: Duration,
        target: Duration,
        cancelled_gapless_advance: bool,
        seek_result_ok: bool,
    ) {
        let clock_trace = self.clock_trace();
        eprintln!(
            "[gst-seek] requested_ms={} target_ms={} result={} cancelled_gapless={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} current={}",
            duration_ms_i128(requested),
            duration_ms_i128(target),
            if seek_result_ok { "ok" } else { "err" },
            i32::from(cancelled_gapless_advance),
            clock_trace.current_state,
            clock_trace.pending_state,
            clock_trace.running,
            clock_trace.clock,
            clock_trace.base,
            self
                .snapshot
                .current
                .as_ref()
                .map_or_else(|| "<none>".to_string(), |current| current.display().to_string()),
        );
    }

    #[cfg(feature = "profiling-logs")]
    fn log_seek_hold_release(&self, target: Duration) {
        let seek_trace = recent_seek_trace(
            Some(target),
            self.last_seek_issued_at.map(|issued| issued.elapsed()),
            Some(Duration::ZERO),
        );
        let clock_trace = self.clock_trace();
        eprintln!(
            "[gst-seek-hold] phase=clear target_ms={} since_seek_ms={} since_seek_release_ms={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} current={}",
            seek_trace.target,
            seek_trace.since_issue,
            seek_trace.since_release,
            clock_trace.current_state,
            clock_trace.pending_state,
            clock_trace.running,
            clock_trace.clock,
            clock_trace.base,
            self
                .snapshot
                .current
                .as_ref()
                .map_or_else(|| "<none>".to_string(), |path| path.display().to_string()),
        );
    }

    #[cfg(feature = "profiling-logs")]
    fn log_position_sample(
        &self,
        trace: GstPositionTrace,
        mode: PlaybackClockMode,
        accepted: Duration,
    ) {
        if !heartbeat_trace_enabled() {
            return;
        }
        let seek_trace = self.recent_seek_trace();
        let clock_trace = self.clock_trace();
        eprintln!(
            "[gst-pos] prev_ms={} next_ms={} delta_ms={} jumped_backward={} mode={:?} accepted_ms={} seek_locked=0 seek_target_ms={} since_seek_ms={} since_seek_release_ms={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} successor_handed={} current={}",
            trace.previous,
            trace.current,
            trace.delta,
            i32::from(trace.jumped_backward),
            mode,
            duration_ms_i128(accepted),
            seek_trace.target,
            seek_trace.since_issue,
            seek_trace.since_release,
            clock_trace.current_state,
            clock_trace.pending_state,
            clock_trace.running,
            clock_trace.clock,
            clock_trace.base,
            i32::from(
                self.queue_state
                    .lock()
                    .is_ok_and(|queue| queue.successor_handed_to_pipeline())
            ),
            self
                .snapshot
                .current
                .as_ref()
                .map_or_else(|| "<none>".to_string(), |path| path.display().to_string()),
        );
    }

    #[cfg(feature = "profiling-logs")]
    fn log_playbin_state_change(&self, old: gst::State, current: gst::State, pending: gst::State) {
        let seek_trace = self.recent_seek_trace();
        eprintln!(
            "[ferrous] playbin state: {:?} → {:?} (pending {:?}) seek_target_ms={} since_seek_ms={} since_seek_release_ms={}",
            old,
            current,
            pending,
            seek_trace.target,
            seek_trace.since_issue,
            seek_trace.since_release,
        );
    }

    fn poll_seek_hold(&mut self) -> (bool, bool, Option<(Duration, Instant)>) {
        let mut position_locked = false;
        let mut snapshot_changed = false;
        let mut released_seek_sample = None;
        if let Some((until, target)) = self.seek_hold.as_ref().copied() {
            let now = Instant::now();
            if now < until {
                let hold_position = self.position_clock.current_position(now);
                if self.snapshot.position != hold_position {
                    self.snapshot.position = hold_position;
                    snapshot_changed = true;
                }
                position_locked = true;
            } else {
                self.seek_hold = None;
                let released_at = now;
                self.last_seek_released_at = Some(released_at);
                released_seek_sample = Some((target, released_at));
                #[cfg(feature = "profiling-logs")]
                self.log_seek_hold_release(target);
            }
        }
        (position_locked, snapshot_changed, released_seek_sample)
    }

    /// Advance identity for every track change.
    fn advance_track_token(&mut self) -> u64 {
        self.track_token_counter += 1;
        self.track_token_counter
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

    fn reset_channel_mute_state(&mut self) {
        self.channel_mute_mask.store(0, Ordering::Relaxed);
        self.solo_pre_mask = None;
        self.soloed_channel = None;
        self.snapshot.muted_channels_mask = 0;
        self.snapshot.soloed_channel = None;
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
        if matches!(kind, TrackChangeKind::Manual) {
            self.reset_channel_mute_state();
        }
        self.snapshot.current_queue_index = Some(queue_index);
        self.snapshot.track_token = track_token;
        self.switch_track(path.as_path(), &uri, force_play);
        self.buffering_active = false;
        self.emit_track_changed(path, queue_index, kind, track_token);
        self.emit_snapshot();
    }

    fn stop_with_empty_queue(&mut self) {
        self.soft_mute();
        let _ = self.playbin.set_state(gst::State::Ready);
        self.begin_pipeline_run(false);
        self.startup_gain_ramp = false;
        self.startup_ramp_hold_until = None;
        self.buffering_active = false;
        self.reset_channel_mute_state();
        self.snapshot.current = None;
        self.snapshot.current_queue_index = None;
        self.snapshot.current_bitrate_kbps = None;
        self.snapshot.track_token = 0;
        self.snapshot.state = PlaybackState::Stopped;
        self.snapshot.position = Duration::ZERO;
        self.snapshot.duration = Duration::ZERO;
        self.position_clock.reset(Duration::ZERO, Instant::now());
    }

    fn load_queue(&mut self, paths: Vec<PathBuf>) {
        if paths.is_empty() {
            return;
        }
        let Ok(mut state) = self.queue_state.lock() else {
            return;
        };
        let cancelled = state.set_queue(paths);
        let repeat_mode = state.repeat_mode();
        let shuffle_enabled = state.shuffle_enabled();
        let first = state.current();
        let current_index = state.current_index().unwrap_or(0);
        drop(state);
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        self.release_staged_spectrogram(cancelled, false);
        let Some(first) = first else {
            return;
        };
        self.switch_to_path(first, current_index, TrackChangeKind::Manual, false);
    }

    fn add_to_queue(&mut self, paths: Vec<PathBuf>) {
        let Some((cancelled, repeat_mode, shuffle_enabled)) = (match self.queue_state.lock() {
            Ok(mut state) => Some((
                state.add_to_queue(paths),
                state.repeat_mode(),
                state.shuffle_enabled(),
            )),
            Err(_) => None,
        }) else {
            return;
        };
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        self.continue_after_cancelled_advance(cancelled);
    }

    fn remove_at(&mut self, idx: usize) {
        self.remove_many(&[idx]);
    }

    fn remove_many(&mut self, indices: &[usize]) {
        let Some((removal, next_current, repeat_mode, shuffle_enabled, current_index)) =
            (match self.queue_state.lock() {
                Ok(mut state) => {
                    let removal = state.remove_many(indices);
                    Some((
                        removal,
                        state.current(),
                        state.repeat_mode(),
                        state.shuffle_enabled(),
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
        match next_current {
            Some(path) if removal.current_removed => {
                self.release_staged_spectrogram(removal.cancelled, false);
                self.switch_to_path(
                    path,
                    current_index.unwrap_or(0),
                    TrackChangeKind::Manual,
                    false,
                );
                return;
            }
            Some(_) => self.continue_after_cancelled_advance(removal.cancelled),
            None => {
                self.release_staged_spectrogram(removal.cancelled, false);
                self.stop_with_empty_queue();
            }
        }
        self.emit_snapshot();
    }

    fn move_queue_item(&mut self, from: usize, to: usize) {
        let Some((cancelled, repeat_mode, shuffle_enabled, current_index)) =
            (match self.queue_state.lock() {
                Ok(mut state) => Some((
                    state.move_item(from, to),
                    state.repeat_mode(),
                    state.shuffle_enabled(),
                    state.current_index(),
                )),
                Err(_) => None,
            })
        else {
            return;
        };
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        self.snapshot.current_queue_index = current_index;
        self.continue_after_cancelled_advance(cancelled);
    }

    fn clear_queue(&mut self) {
        let flags = if let Ok(mut state) = self.queue_state.lock() {
            Some((state.clear(), state.repeat_mode(), state.shuffle_enabled()))
        } else {
            None
        };
        if let Some((cancelled, repeat_mode, shuffle_enabled)) = flags {
            self.set_queue_flags(repeat_mode, shuffle_enabled);
            self.release_staged_spectrogram(cancelled, false);
        }
        self.stop_with_empty_queue();
        self.emit_snapshot();
    }

    fn play_at(&mut self, idx: usize) {
        let Ok(mut state) = self.queue_state.lock() else {
            return;
        };
        let repeat_mode = state.repeat_mode();
        let shuffle_enabled = state.shuffle_enabled();
        let target = state.set_current(idx);
        drop(state);
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        let Some(target) = target else {
            return;
        };
        self.release_staged_spectrogram(target.cancelled, false);
        self.switch_to_path(target.path, target.index, TrackChangeKind::Manual, false);
    }

    fn advance_manual(&mut self, next: bool) {
        let Ok(mut state) = self.queue_state.lock() else {
            return;
        };
        let repeat_mode = state.repeat_mode();
        let shuffle_enabled = state.shuffle_enabled();
        let resume_from_pause = self.snapshot.state == PlaybackState::Paused;
        let target = if next {
            state.next_manual()
        } else {
            state.previous_manual()
        };
        drop(state);
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        let Some(target) = target else {
            return;
        };
        self.release_staged_spectrogram(target.cancelled, false);
        self.switch_to_path(
            target.path,
            target.index,
            TrackChangeKind::Manual,
            resume_from_pause,
        );
    }

    fn play(&mut self) {
        let was_stopped = self.snapshot.state == PlaybackState::Stopped;
        let mut restart_path = None;
        if let Ok(state) = self.queue_state.lock() {
            self.snapshot.current_queue_index = state.current_index();
            if was_stopped && state.current() == self.snapshot.current {
                restart_path.clone_from(&self.snapshot.current);
            }
        }
        // A stopped pipeline reopens its URI, which is stale if the current
        // file was renamed. A playing stream keeps reading the renamed file,
        // but playbin3 takes a URI set during playback as the next gapless
        // track, so the URI is only replaced once the pipeline has stopped.
        if let Some(uri) = restart_path.as_deref().and_then(file_uri) {
            self.playbin.set_property("uri", uri);
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
            let now = Instant::now();
            self.playing_state_requested_at = Some(now);
            self.position_clock.reset(self.snapshot.position, now);
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
        self.playing_state_requested_at = None;
        self.buffering_active = false;
        if self.playbin.set_state(gst::State::Paused).is_ok() {
            self.snapshot.state = PlaybackState::Paused;
            self.position_clock
                .reset(self.snapshot.position, Instant::now());
            self.emit_snapshot();
        }
    }

    fn stop(&mut self) {
        self.soft_mute();
        self.playing_state_requested_at = None;
        if self.playbin.set_state(gst::State::Ready).is_ok() {
            self.begin_pipeline_run(false);
            // The pipeline may have been given the successor. Play must
            // start the stopped track again.
            if let Some(uri) = self.snapshot.current.as_deref().and_then(file_uri) {
                self.playbin.set_property("uri", &uri);
            }
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
            self.buffering_active = false;
            self.seek_hold = None;
            self.clear_recent_seek_trace();
            self.reset_channel_mute_state();
            self.snapshot.state = PlaybackState::Stopped;
            self.snapshot.position = Duration::ZERO;
            self.position_clock.reset(Duration::ZERO, Instant::now());
            self.snapshot.current_queue_index = None;
            self.snapshot.current_bitrate_kbps = None;
            self.emit_snapshot();
        }
    }

    fn seek(&mut self, pos: Duration) {
        let nanos = u64::try_from(pos.as_nanos().min(u128::from(u64::MAX))).unwrap_or(u64::MAX);
        let target = gst::ClockTime::from_nseconds(nanos);
        let requested = pos.min(self.snapshot.duration);

        // playbin3 applies a seek to a successor it was given, so the
        // pipeline gets none until the seek is applied, and a seek inside
        // the about-to-finish window restarts the displayed track and seeks
        // there instead.
        self.hold_handoffs();
        let cancelled_gapless_advance = self
            .cancel_pending_advance(true)
            .is_some_and(|cancelled| cancelled.handed_to_pipeline);
        let restarted = if cancelled_gapless_advance {
            self.restart_current_track(target)
        } else {
            None
        };
        let seek_result_ok = restarted.unwrap_or_else(|| {
            let seek_flags = seek_flags_for_path(self.snapshot.current.as_deref());
            self.playbin.seek_simple(seek_flags, target).is_ok()
        });
        self.release_handoffs();
        #[cfg(not(feature = "profiling-logs"))]
        let _ = seek_result_ok;
        self.begin_seek_hold(requested);
        #[cfg(feature = "profiling-logs")]
        self.log_seek_event(pos, requested, cancelled_gapless_advance, seek_result_ok);
        let _ = self.event_tx.send(PlaybackEvent::Seeked {
            position: self.snapshot.position,
        });
    }

    /// Drop every staged successor so the committed track stays current.
    /// `current_continues` tells analysis whether the committed track
    /// keeps playing, so a prepared spectrogram continuation is restarted
    /// rather than superseded.
    ///
    /// When the result reports a successor handed to the pipeline, the
    /// caller must either restart the committed track with
    /// `restart_current_track` or switch tracks before playback continues.
    fn cancel_pending_advance(&mut self, current_continues: bool) -> Option<CancelledAdvance> {
        let cancelled = self
            .queue_state
            .lock()
            .ok()
            .and_then(|mut queue| queue.cancel_pending());
        self.release_staged_spectrogram(cancelled, current_continues);
        cancelled
    }

    /// Continue the committed track after a queue or setting change dropped
    /// staged successors.
    fn continue_after_cancelled_advance(&mut self, cancelled: Option<CancelledAdvance>) {
        self.release_staged_spectrogram(cancelled, true);
        if !cancelled.is_some_and(|cancelled| cancelled.handed_to_pipeline) {
            return;
        }
        let position = self
            .playbin
            .query_position::<gst::ClockTime>()
            .unwrap_or_else(|| {
                gst::ClockTime::from_nseconds(
                    u64::try_from(self.snapshot.position.as_nanos()).unwrap_or(u64::MAX),
                )
            });
        if self.restart_current_track(position).is_some() {
            let position = Duration::from_nanos(position.nseconds());
            self.snapshot.position = position;
            self.position_clock.reset(position, Instant::now());
        }
    }

    /// Restart the committed track at `position` in a new pipeline run,
    /// dropping a successor the pipeline was given. Returns whether the
    /// seek was accepted, or `None` without a current track.
    fn restart_current_track(&mut self, position: gst::ClockTime) -> Option<bool> {
        let path = self.snapshot.current.clone()?;
        let uri = file_uri(&path)?;
        // A short track can reach about-to-finish while it prerolls, before
        // the seek below.
        self.hold_handoffs();
        self.soft_mute();
        let _ = self.playbin.set_state(gst::State::Null);
        let _ = self.playbin.set_state(gst::State::Ready);
        // Also drops a successor staged while the old run was stopping.
        self.begin_pipeline_run(true);
        self.playbin.set_property("uri", &uri);

        // Preroll in Paused so the pipeline accepts the seek. For local
        // files this is sub-millisecond.
        let _ = self.playbin.set_state(gst::State::Paused);
        let _ = self.playbin.state(Some(gst::ClockTime::from_mseconds(500)));
        let seek_result_ok = self
            .playbin
            .seek_simple(seek_flags_for_path(Some(&path)), position)
            .is_ok();
        self.release_handoffs();

        if self.snapshot.state == PlaybackState::Playing {
            let _ = self.playbin.set_state(gst::State::Playing);
            self.playbin.set_property("volume", 0.0_f64);
            self.startup_gain_ramp = true;
            self.startup_ramp_hold_until = if is_raw_surround_file(&path) {
                Some(Instant::now() + Duration::from_millis(80))
            } else {
                None
            };
        }
        Some(seek_result_ok)
    }

    fn hold_handoffs(&self) {
        if let Ok(mut queue) = self.queue_state.lock() {
            queue.hold_handoffs();
        }
    }

    fn release_handoffs(&self) {
        if let Ok(mut queue) = self.queue_state.lock() {
            queue.release_handoffs();
        }
    }

    /// Account for a new pipeline run after the pipeline was stopped:
    /// discard the previous run's bus messages and staged successors.
    fn begin_pipeline_run(&mut self, current_continues: bool) {
        if let Some(bus) = self.playbin.bus() {
            bus.set_flushing(true);
            bus.set_flushing(false);
        }
        self.awaiting_run_stream_start = true;
        self.last_stream_group = None;
        if let Ok(mut durations) = self.stream_durations.lock() {
            durations.clear();
        }
        let _ = self.cancel_pending_advance(current_continues);
    }

    /// Tell analysis that a prepared spectrogram continuation will not be
    /// used. When the current track continues, analysis restarts its
    /// session because the worker may already decode the dropped file;
    /// otherwise the following `SetTrack` or stop supersedes it.
    fn release_staged_spectrogram(
        &self,
        cancelled: Option<CancelledAdvance>,
        current_continues: bool,
    ) {
        if !cancelled.is_some_and(|cancelled| cancelled.staged_spectrogram) {
            return;
        }
        let _ = self.analysis_tx.send(if current_continues {
            AnalysisCommand::CancelStagedContinuation
        } else {
            AnalysisCommand::ClearStagedContinuation
        });
    }

    /// Commit a staged successor when the pipeline reports its stream.
    fn handle_stream_start(&mut self, group: Option<gst::GroupId>) {
        if group.is_some() && group == self.last_stream_group {
            return;
        }
        self.last_stream_group = group;
        if std::mem::take(&mut self.awaiting_run_stream_start) {
            if self.refresh_duration() {
                self.emit_snapshot();
            }
            return;
        }
        let committed = self
            .queue_state
            .lock()
            .ok()
            .and_then(|mut queue| queue.commit_gapless());
        if let Some(committed) = committed {
            self.commit_gapless_successor(committed);
        }
    }

    fn commit_gapless_successor(&mut self, committed: CommittedAdvance) {
        let track_token = self.advance_track_token();
        let position = self
            .playbin
            .query_position::<gst::ClockTime>()
            .map_or(Duration::ZERO, |position| {
                Duration::from_nanos(position.nseconds())
            });
        self.seek_hold = None;
        self.clear_recent_seek_trace();
        self.snapshot.current = Some(committed.path.clone());
        self.snapshot.current_queue_index = Some(committed.index);
        self.snapshot.track_token = track_token;
        self.snapshot.position = position;
        self.snapshot.duration = self.current_duration().unwrap_or(Duration::ZERO);
        self.snapshot.current_bitrate_kbps = None;
        self.position_clock.reset(position, Instant::now());
        // A prepared spectrogram continuation is consumed by the gapless
        // `SetTrack` that the bridge sends for this event.
        self.emit_track_changed(
            committed.path,
            committed.index,
            TrackChangeKind::Gapless,
            track_token,
        );
        self.emit_snapshot();
    }

    /// Duration of the current track. Once the pipeline was given a
    /// successor, its duration query switches to the successor when the
    /// decoder links it, which can be long before the successor is audible
    /// and is not announced. The duration the current stream's parser last
    /// reported is used then.
    fn current_duration(&self) -> Option<Duration> {
        let handed = self
            .queue_state
            .lock()
            .map_or(true, |queue| queue.successor_handed_to_pipeline());
        let reported = || {
            let current = self.snapshot.current.as_deref()?;
            self.stream_durations.lock().ok()?.get(current)
        };
        if handed {
            reported()
        } else {
            self.playbin
                .query_duration::<gst::ClockTime>()
                .map(|duration| Duration::from_nanos(duration.nseconds()))
                .or_else(reported)
        }
    }

    /// Update the duration of the current track. Returns whether it changed.
    fn refresh_duration(&mut self) -> bool {
        let Some(duration) = self
            .current_duration()
            .filter(|duration| *duration != self.snapshot.duration)
        else {
            return false;
        };
        self.snapshot.duration = duration;
        true
    }

    fn set_volume(&mut self, level: f32) {
        self.snapshot.volume = level.clamp(0.0, 1.0);
        self.target_volume = super::volume_level_to_gain(self.snapshot.volume);
        self.emit_snapshot();
    }

    fn set_repeat_mode(&mut self, mode: RepeatMode) {
        let cancelled = if let Ok(mut state) = self.queue_state.lock() {
            let cancelled = state.set_repeat_mode(mode);
            self.snapshot.repeat_mode = state.repeat_mode();
            cancelled
        } else {
            self.snapshot.repeat_mode = mode;
            None
        };
        self.continue_after_cancelled_advance(cancelled);
        self.emit_snapshot();
    }

    fn set_shuffle(&mut self, enabled: bool) {
        let cancelled = if let Ok(mut state) = self.queue_state.lock() {
            let cancelled = state.set_shuffle_enabled(enabled);
            self.snapshot.shuffle_enabled = state.shuffle_enabled();
            cancelled
        } else {
            self.snapshot.shuffle_enabled = enabled;
            None
        };
        self.continue_after_cancelled_advance(cancelled);
        self.emit_snapshot();
    }

    /// Whether the engine loop must wake on [`VOLUME_RAMP_TICK`] to drive a
    /// silence hold or fade.
    fn volume_ramp_pending(&self) -> bool {
        self.startup_ramp_hold_until.is_some()
            || ((self.target_volume - self.applied_volume).abs() > f32::EPSILON
                && (self.snapshot.state == PlaybackState::Playing || self.startup_gain_ramp))
    }

    /// Advance the volume fade to `now`, respecting the optional silence hold
    /// for AC3/DTS decoder stabilisation.  The engine loop calls this after
    /// every wake-up, so fade speed depends on elapsed time, not on how often
    /// commands or bridge polls arrive.  Fades run on the volume-level scale,
    /// so they sound even.
    fn poll_volume_ramp(&mut self, now: Instant) {
        // Startup silence hold: keep volume at zero until the decoder has
        // had time to stabilise (prevents AC3/DTS garbage frames from
        // reaching the speakers).
        if let Some(hold_until) = self.startup_ramp_hold_until {
            if now < hold_until {
                if self.applied_volume != 0.0 {
                    self.applied_volume = 0.0;
                    self.playbin.set_property("volume", 0.0_f64);
                }
                self.volume_ramp_tick_at = None;
                return;
            }
            self.startup_ramp_hold_until = None;
        }

        if !self.volume_ramp_pending() {
            self.startup_gain_ramp = false;
            self.volume_ramp_tick_at = None;
            return;
        }
        // The first step of a fade only starts its clock.
        let Some(previous_tick) = self.volume_ramp_tick_at.replace(now) else {
            return;
        };
        let full_scale = if self.startup_gain_ramp {
            STARTUP_FADE_FULL_SCALE
        } else {
            VOLUME_FADE_FULL_SCALE
        };
        if let Some(level) = step_volume_level(
            super::volume_gain_to_level(self.applied_volume),
            super::volume_gain_to_level(self.target_volume),
            now.saturating_duration_since(previous_tick),
            full_scale,
        ) {
            self.applied_volume = super::volume_level_to_gain(level);
        } else {
            self.applied_volume = self.target_volume;
            self.startup_gain_ramp = false;
            self.volume_ramp_tick_at = None;
        }
        self.playbin
            .set_property("volume", f64::from(self.applied_volume));
    }

    fn poll(&mut self) {
        if self.snapshot.state == PlaybackState::Stopped
            && !self.startup_gain_ramp
            && self.seek_hold.is_none()
            && self.startup_ramp_hold_until.is_none()
        {
            return;
        }

        let (position_locked, seek_hold_changed, released_seek_sample) = self.poll_seek_hold();
        let mut snapshot_changed = seek_hold_changed;
        if !position_locked && self.snapshot.state != PlaybackState::Stopped {
            if let Some(pos) = self.playbin.query_position::<gst::ClockTime>() {
                let now = Instant::now();
                let previous_raw_pos = self
                    .position_clock
                    .last_raw_position()
                    .unwrap_or(self.snapshot.position);
                let raw_pos = Duration::from_nanos(pos.nseconds());
                let next_pos = if self.snapshot.state == PlaybackState::Playing {
                    if let Some((target, released_at)) = released_seek_sample {
                        release_seek_hold_sample(
                            &mut self.position_clock,
                            target,
                            released_at,
                            raw_pos,
                            now,
                        )
                    } else {
                        self.position_clock.update_playing_sample(raw_pos, now)
                    }
                } else {
                    self.position_clock.reset(raw_pos, now);
                    raw_pos
                };
                if self.snapshot.position != next_pos {
                    self.snapshot.position = next_pos;
                    snapshot_changed = true;
                }
                if previous_raw_pos != raw_pos {
                    #[cfg(feature = "profiling-logs")]
                    {
                        let mode_before_sample = if released_seek_sample.is_some() {
                            PlaybackClockMode::SeekReacquire
                        } else {
                            self.position_clock.mode()
                        };
                        self.log_position_sample(
                            gst_position_trace(previous_raw_pos, raw_pos),
                            mode_before_sample,
                            next_pos,
                        );
                    }
                }
            }
        }
        if self.snapshot.state != PlaybackState::Stopped || self.snapshot.duration == Duration::ZERO
        {
            snapshot_changed |= self.refresh_duration();
        }
        let current_mask = self.channel_mute_mask.load(Ordering::Relaxed);
        if self.snapshot.muted_channels_mask != current_mask {
            self.snapshot.muted_channels_mask = current_mask;
            snapshot_changed = true;
        }
        if self.check_stuck_state_change() {
            // Pipeline was stuck; recovery already emitted a snapshot.
            return;
        }
        if snapshot_changed {
            self.emit_snapshot();
        }
    }

    /// Maximum time to wait for `GStreamer` to confirm the Playing state
    /// change via a bus `StateChanged` message.  If this expires, the
    /// pipeline is stuck (e.g. audio backend failed to negotiate) and
    /// we advance to the next track.
    const PLAYING_STATE_TIMEOUT: Duration = Duration::from_secs(5);

    /// Returns `true` if the pipeline was stuck and recovery was attempted.
    fn check_stuck_state_change(&mut self) -> bool {
        let Some(requested_at) = self.playing_state_requested_at else {
            return false;
        };
        if requested_at.elapsed() < Self::PLAYING_STATE_TIMEOUT {
            return false;
        }
        let current_path = self.snapshot.current.as_ref().map_or("?", |p| {
            p.file_name().unwrap_or_default().to_str().unwrap_or("?")
        });
        eprintln!(
            "[ferrous] pipeline stuck: Playing state not confirmed within {}s for {current_path}, advancing",
            Self::PLAYING_STATE_TIMEOUT.as_secs(),
        );
        self.playing_state_requested_at = None;
        // Force the pipeline to a clean state.
        let _ = self.playbin.set_state(gst::State::Null);
        self.begin_pipeline_run(false);
        self.advance_after_stuck_pipeline();
        true
    }

    /// After detecting a stuck pipeline, try the next track in the queue.
    /// If no next track exists, stop playback.
    fn advance_after_stuck_pipeline(&mut self) {
        let Ok(mut state) = self.queue_state.lock() else {
            self.snapshot.state = PlaybackState::Stopped;
            self.emit_snapshot();
            return;
        };
        let repeat_mode = state.repeat_mode();
        let shuffle_enabled = state.shuffle_enabled();
        let next = state.next_manual();
        drop(state);
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        if let Some(next) = next {
            self.release_staged_spectrogram(next.cancelled, false);
            self.switch_to_path(next.path, next.index, TrackChangeKind::Manual, true);
        } else {
            self.snapshot.state = PlaybackState::Stopped;
            self.snapshot.position = Duration::ZERO;
            self.snapshot.duration = Duration::ZERO;
            self.emit_snapshot();
        }
    }

    fn apply_command(&mut self, cmd: PlaybackCommand) {
        match cmd {
            PlaybackCommand::LoadQueue(paths) => self.load_queue(paths),
            PlaybackCommand::AddToQueue(paths) => self.add_to_queue(paths),
            PlaybackCommand::RemapPaths(renames) => {
                if let Ok(mut state) = self.queue_state.lock() {
                    state.remap_paths(&renames);
                }
                let renamed_current = self
                    .snapshot
                    .current
                    .as_mut()
                    .is_some_and(|current| super::remap_path(current, &renames));
                // Acknowledge before the first snapshot with the new paths.
                let _ = self.event_tx.send(PlaybackEvent::PathsRemapped);
                if renamed_current {
                    self.emit_snapshot();
                }
            }
            PlaybackCommand::RemoveAt(idx) => self.remove_at(idx),
            PlaybackCommand::RemoveMany(indices) => self.remove_many(&indices),
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
            PlaybackCommand::ToggleChannelMute(ch) => {
                let ch = ch.min(63);
                if self.soloed_channel == Some(ch) {
                    // Clicking the soloed channel unsolos (restores pre-mask).
                    if let Some(pre) = self.solo_pre_mask.take() {
                        self.channel_mute_mask.store(pre, Ordering::Relaxed);
                    }
                    self.soloed_channel = None;
                } else {
                    let prev = self.channel_mute_mask.load(Ordering::Relaxed);
                    self.channel_mute_mask
                        .store(prev ^ (1u64 << ch), Ordering::Relaxed);
                    self.solo_pre_mask = None;
                    self.soloed_channel = None;
                }
                self.snapshot.muted_channels_mask = self.channel_mute_mask.load(Ordering::Relaxed);
                self.snapshot.soloed_channel = self.soloed_channel;
                self.emit_snapshot();
            }
            PlaybackCommand::SoloChannel(ch) => {
                let ch = ch.min(63);
                if self.soloed_channel == Some(ch) {
                    // Un-solo: restore saved mask.  solo_pre_mask is always
                    // Some when soloed_channel is Some (set on entry).
                    if let Some(pre) = self.solo_pre_mask.take() {
                        self.channel_mute_mask.store(pre, Ordering::Relaxed);
                    }
                    self.soloed_channel = None;
                } else {
                    // Solo (fresh or switching target).
                    if self.soloed_channel.is_none() {
                        self.solo_pre_mask = Some(self.channel_mute_mask.load(Ordering::Relaxed));
                    }
                    self.channel_mute_mask
                        .store(u64::MAX ^ (1u64 << ch), Ordering::Relaxed);
                    self.soloed_channel = Some(ch);
                }
                self.snapshot.muted_channels_mask = self.channel_mute_mask.load(Ordering::Relaxed);
                self.snapshot.soloed_channel = self.soloed_channel;
                self.emit_snapshot();
            }
            PlaybackCommand::Poll => self.poll(),
        }
    }

    fn drain_bus(&mut self, bus: &gst::Bus) {
        while let Some(msg) = bus.pop() {
            self.handle_bus_message(&msg);
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
                let successor = self
                    .queue_state
                    .lock()
                    .ok()
                    .and_then(|mut queue| queue.take_eos_successor());
                if let Some(successor) = successor {
                    self.release_staged_spectrogram(successor.released, false);
                    self.switch_to_path(
                        successor.path,
                        successor.index,
                        TrackChangeKind::Natural,
                        true,
                    );
                    return;
                }
                // Normal EOS: end of queue
                self.playing_state_requested_at = None;
                self.buffering_active = false;
                self.seek_hold = None;
                self.position_clock.reset(Duration::ZERO, Instant::now());
                self.clear_recent_seek_trace();
                self.startup_gain_ramp = false;
                self.startup_ramp_hold_until = None;
                let _ = self.playbin.set_state(gst::State::Ready);
                self.begin_pipeline_run(false);
                super::stop_snapshot_at_terminal_eos(&mut self.snapshot);
                self.emit_snapshot();
            }
            gst::MessageView::StreamStart(stream_start) => {
                self.handle_stream_start(stream_start.group_id());
            }
            gst::MessageView::DurationChanged(_) => {
                if self.refresh_duration() {
                    self.emit_snapshot();
                }
            }
            gst::MessageView::Error(err) => {
                eprintln!(
                    "[ferrous] gstreamer error from {:?}: {} ({:?})",
                    err.src().map(gstreamer::prelude::GstObjectExt::path_string),
                    err.error(),
                    err.debug()
                );
                self.playing_state_requested_at = None;
                self.buffering_active = false;
                self.position_clock
                    .reset(self.snapshot.position, Instant::now());
                self.snapshot.state = PlaybackState::Stopped;
                // Tear down the faulted pipeline fully so subsequent
                // tracks can start from a clean state.
                let _ = self.playbin.set_state(gst::State::Null);
                let _ = self.playbin.set_state(gst::State::Ready);
                self.begin_pipeline_run(false);
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
                if sc.src().is_some_and(|s| s == &self.playbin) {
                    #[cfg(feature = "profiling-logs")]
                    self.log_playbin_state_change(sc.old(), sc.current(), sc.pending());
                    if sc.current() == gst::State::Playing {
                        self.playing_state_requested_at = None;
                    }
                }
            }
            _ => {}
        }
    }

    fn soft_mute(&mut self) {
        if self.applied_volume <= 0.0001 {
            self.applied_volume = 0.0;
            self.playbin
                .set_property("volume", f64::from(self.applied_volume));
            return;
        }
        for _ in 0..3 {
            self.applied_volume *= 0.35;
            if self.applied_volume <= 0.0001 {
                self.applied_volume = 0.0;
            }
            self.playbin
                .set_property("volume", f64::from(self.applied_volume));
            std::thread::sleep(Duration::from_millis(4));
            if self.applied_volume == 0.0 {
                break;
            }
        }
        self.applied_volume = 0.0;
        self.playbin
            .set_property("volume", f64::from(self.applied_volume));
    }

    fn switch_track(&mut self, path: &Path, uri: &str, force_play: bool) {
        let was_playing = self.snapshot.state == PlaybackState::Playing || force_play;
        self.soft_mute();
        self.playing_state_requested_at = None;
        let _ = self.playbin.set_state(gst::State::Null);
        let _ = self.playbin.set_state(gst::State::Ready);
        // Also drops a successor staged while the old run was stopping.
        self.begin_pipeline_run(false);
        self.playbin.set_property("uri", uri);
        if was_playing {
            let _ = self.playbin.set_state(gst::State::Playing);
            self.playing_state_requested_at = Some(Instant::now());
            // Re-assert mute after state transition to close the race window
            // where new internal elements may not have volume=0.
            self.playbin.set_property("volume", 0.0_f64);
            self.snapshot.state = PlaybackState::Playing;
            self.startup_gain_ramp = true;
            // AC3/DTS decoders produce garbage frames during startup — hold
            // silence until the decoder has stabilised.  Other codecs produce
            // clean output immediately, so skip the hold to avoid swallowing
            // the start of the track.
            self.startup_ramp_hold_until = if is_raw_surround_file(path) {
                Some(Instant::now() + Duration::from_millis(80))
            } else {
                None
            };
        } else if self.snapshot.state == PlaybackState::Paused {
            let _ = self.playbin.set_state(gst::State::Paused);
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
        } else {
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
        }
        self.snapshot.current = Some(path.to_path_buf());
        self.snapshot.position = Duration::ZERO;
        self.snapshot.duration = Duration::ZERO;
        self.snapshot.current_bitrate_kbps = None;
        self.position_clock.reset(Duration::ZERO, Instant::now());
        self.clear_recent_seek_trace();
    }
}

fn run_gst_engine(
    cmd_rx: &Receiver<PlaybackCommand>,
    event_tx: Sender<PlaybackEvent>,
    analysis_tx: Sender<AnalysisCommand>,
    build_output: fn() -> anyhow::Result<gst::Element>,
) -> anyhow::Result<()> {
    gst::init().context("gst::init failed")?;
    configure_raw_surround_decoding();

    let playbin = gst::ElementFactory::make("playbin3")
        .build()
        .map_err(|_| anyhow!("failed to create playbin3"))?;
    configure_playbin_buffering(&playbin);

    let channel_mute_mask = Arc::new(AtomicU64::new(0));

    let analysis_sink =
        build_audio_output_bin(analysis_tx.clone(), &channel_mute_mask, &build_output()?)?;
    playbin.set_property("audio-sink", &analysis_sink);

    // Strip leading partial-frame data and trailing APEv2 tags from raw
    // surround file sources so the AC3/DTS parser only receives clean,
    // sync-aligned audio.  Without this, the parser must scan for sync
    // words through garbage data, causing gaps during gapless transitions.
    install_raw_surround_source_probe(&playbin);

    let stream_durations = Arc::new(Mutex::new(StreamDurations::default()));
    install_stream_duration_probes(&playbin, &stream_durations);

    let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));

    {
        let queue_state = Arc::clone(&queue_state);
        let analysis_tx = analysis_tx.clone();
        // Runs on a streaming thread once the pipeline has read all of the
        // last stream it was given, usually about 2 s before that stream
        // ends and at once for a short file. Only stage the successor here;
        // the runtime commits it when its stream starts or at end of stream.
        playbin.connect("about-to-finish", false, move |values| {
            // Do not call into playbin here except to hand over the URI: a
            // query can deadlock with a state change on the engine thread.
            let playbin_obj = values.first()?.get::<gst::Element>().ok()?;

            let mut queue = match queue_state.lock() {
                Ok(queue) => queue,
                Err(e) => {
                    eprintln!("[ferrous] about-to-finish: queue lock poisoned: {e}");
                    return None;
                }
            };
            // Same file extension → gapless (same decoder reused).
            // Different extension → let EOS fire, handle in bus handler.
            let staged = queue.stage_natural(|finishing, next| {
                if same_audio_extension(finishing, next) && file_uri(next).is_some() {
                    Handoff::Gapless
                } else {
                    Handoff::AtEos
                }
            })?;
            if staged.staged_spectrogram {
                // Send while holding the queue lock so that a cancellation
                // cannot reach analysis before this request. The staging
                // thread checks format compatibility itself.
                let _ = analysis_tx.send(AnalysisCommand::PrepareGaplessContinuation {
                    path: staged.path.clone(),
                });
            }
            drop(queue);
            if staged.handoff == Handoff::Gapless {
                if let Some(uri) = file_uri(&staged.path) {
                    playbin_obj.set_property("uri", uri);
                }
            }
            None
        });
    }

    let bus = playbin.bus().context("playbin has no bus")?;
    let mut runtime = GstPlaybackRuntime::new(
        playbin,
        queue_state,
        analysis_tx,
        event_tx,
        channel_mute_mask,
        stream_durations,
    );
    runtime
        .playbin
        .set_property("volume", f64::from(runtime.applied_volume));

    loop {
        match cmd_rx.recv_timeout(runtime.command_wait_timeout()) {
            Ok(cmd) => {
                // Act on the pipeline's latest state, such as a successor
                // that has just started.
                runtime.drain_bus(&bus);
                runtime.apply_command(cmd);
            }
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => break,
        }
        runtime.drain_bus(&bus);
        runtime.poll_volume_ramp(Instant::now());
    }

    let _ = runtime.playbin.set_state(gst::State::Null);
    Ok(())
}

/// Engine wake-up interval while a fade or silence hold is running.
const VOLUME_RAMP_TICK: Duration = Duration::from_millis(5);
/// Time a volume change takes to cross the whole level scale.
const VOLUME_FADE_FULL_SCALE: Duration = Duration::from_millis(220);
/// Time the fade-in after a start or track switch takes to reach full level.
const STARTUP_FADE_FULL_SCALE: Duration = Duration::from_millis(90);

/// Remaining level distance that counts as arrived; it absorbs rounding from
/// stepping in small increments (about 0.003 dB at full level).
const VOLUME_LEVEL_SNAP: f32 = 1e-4;

/// Move `level` toward `target` by the distance a full-scale fade covers in
/// `elapsed`.  Returns `None` once the target is reached.
fn step_volume_level(
    level: f32,
    target: f32,
    elapsed: Duration,
    full_scale: Duration,
) -> Option<f32> {
    let max_step = elapsed.as_secs_f32() / full_scale.as_secs_f32();
    let delta = target - level;
    (delta.abs() > max_step + VOLUME_LEVEL_SNAP).then(|| level + delta.signum() * max_step)
}

/// Durations that the parsers of recently started streams reported, keyed
/// by the stream's file.
#[derive(Debug, Default)]
struct StreamDurations {
    recent: VecDeque<(PathBuf, Duration)>,
}

impl StreamDurations {
    /// More than the streams a pipeline run can buffer at once.
    const CAPACITY: usize = 8;

    fn record(&mut self, path: PathBuf, duration: Duration) {
        self.recent.retain(|(known, _)| *known != path);
        if self.recent.len() == Self::CAPACITY {
            self.recent.pop_front();
        }
        self.recent.push_back((path, duration));
    }

    fn get(&self, path: &Path) -> Option<Duration> {
        self.recent
            .iter()
            .find_map(|(known, duration)| (known == path).then_some(*duration))
    }

    fn clear(&mut self) {
        self.recent.clear();
    }
}

/// Record the duration of every parsed stream under its file, from the
/// streaming thread that parses it: when its first buffer leaves the
/// parser and again at its end, when the parser has read all of it. Queries
/// on the parser's own pad cannot describe another stream, and they avoid
/// the playbin locks that a state change on the engine thread holds. (The
/// group of a stream is no key: the pipeline may restamp it.)
fn install_stream_duration_probes(playbin: &gst::Element, durations: &Arc<Mutex<StreamDurations>>) {
    let durations = Arc::clone(durations);
    playbin.connect("deep-element-added", false, move |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        if element
            .factory()
            .is_some_and(|factory| factory.name() == "parsebin")
        {
            let durations = Arc::clone(&durations);
            element.connect_pad_added(move |_, pad| {
                if pad.direction() == gst::PadDirection::Src {
                    install_stream_duration_probe(pad, Arc::clone(&durations));
                }
            });
        }
        None
    });
}

fn install_stream_duration_probe(pad: &gst::Pad, durations: Arc<Mutex<StreamDurations>>) {
    let first_buffer_seen = AtomicBool::new(false);
    pad.add_probe(
        gst::PadProbeType::BUFFER | gst::PadProbeType::EVENT_DOWNSTREAM,
        move |pad, info| {
            let record = match info.data {
                Some(gst::PadProbeData::Event(ref event)) => match event.view() {
                    gst::EventView::StreamStart(_) => {
                        first_buffer_seen.store(false, Ordering::Relaxed);
                        false
                    }
                    gst::EventView::Eos(_) => true,
                    _ => false,
                },
                Some(gst::PadProbeData::Buffer(_)) => {
                    !first_buffer_seen.swap(true, Ordering::Relaxed)
                }
                _ => false,
            };
            if record {
                if let (Some(path), Some(duration)) =
                    (stream_file(pad), pad.query_duration::<gst::ClockTime>())
                {
                    if let Ok(mut durations) = durations.lock() {
                        durations.record(path, Duration::from_nanos(duration.nseconds()));
                    }
                }
            }
            gst::PadProbeReturn::Ok
        },
    );
}

/// File that the source upstream of `pad` reads.
fn stream_file(pad: &gst::Pad) -> Option<PathBuf> {
    let mut query = gst::query::Uri::new();
    if !pad.query(&mut query) {
        return None;
    }
    url::Url::parse(&query.uri()?).ok()?.to_file_path().ok()
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
    } else if path.is_some_and(is_flac_file) {
        // Fast FLAC seeks can leave flacparse in an internally errored state
        // after rapid synced seeks. Accurate flushed seeks avoid that parser
        // stall while keeping other formats on the low-latency path.
        gst::SeekFlags::FLUSH | gst::SeekFlags::ACCURATE
    } else {
        // Accurate seeks can force parsers/demuxers to scan large files.
        // Default playback should prefer responsiveness, especially on slow
        // network-backed filesystems.
        gst::SeekFlags::FLUSH
    }
}

fn is_flac_file(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("flac"))
}

/// Installs a buffer probe on `capsfilter`'s src pad that silences
/// muted channels.  Placed after `audioconvert` + `audioresample` so
/// the format is fully negotiated.
///
/// Muted samples are replaced with inaudible low-level noise rather
/// than hard zero.  Sustained digital silence triggers the auto-mute
/// circuit on ESS Sabre DACs (e.g. Topping DX3 Pro+) after ~10 s,
/// killing the analog output.  The alternating ±1-LSB pattern keeps
/// the detector seeing AC energy without producing audible sound.
fn install_channel_mute_probe(capsfilter: &gst::Element, channel_mute_mask: &Arc<AtomicU64>) {
    let Some(src_pad) = capsfilter.static_pad("src") else {
        return;
    };
    let mute_mask = Arc::clone(channel_mute_mask);
    src_pad.add_probe(gst::PadProbeType::BUFFER, move |pad, info| {
        let mask = mute_mask.load(Ordering::Relaxed);
        if mask == 0 {
            return gst::PadProbeReturn::Ok;
        }
        let Some(gst::PadProbeData::Buffer(ref mut buffer)) = info.data else {
            return gst::PadProbeReturn::Ok;
        };
        let Some(caps) = pad.current_caps() else {
            return gst::PadProbeReturn::Ok;
        };
        let Some(structure) = caps.structure(0) else {
            return gst::PadProbeReturn::Ok;
        };
        // Only process interleaved audio.  Planar layouts have a
        // different memory arrangement; skip gracefully (no common
        // audio sink negotiates planar).
        let layout: &str = structure.get::<&str>("layout").unwrap_or("interleaved");
        if layout != "interleaved" {
            return gst::PadProbeReturn::Ok;
        }
        let channels: i32 = structure.get("channels").unwrap_or(1);
        if channels <= 0 {
            return gst::PadProbeReturn::Ok;
        }
        let format_str: &str = structure.get::<&str>("format").unwrap_or("S16LE");
        let bps = gst_audio_format_bytes_per_sample(format_str);
        if bps == 0 {
            return gst::PadProbeReturn::Ok;
        }
        let is_float = matches!(format_str, "F32LE" | "F32BE" | "F64LE" | "F64BE");
        let is_big_endian = matches!(
            format_str,
            "S16BE"
                | "U16BE"
                | "S24BE"
                | "U24BE"
                | "S24_32BE"
                | "U24_32BE"
                | "S32BE"
                | "U32BE"
                | "F32BE"
                | "F64BE"
        );
        // channels is guaranteed positive by the check above.
        let num_channels = channels.cast_unsigned() as usize;
        let frame_size = num_channels * bps;
        let buffer = buffer.make_mut();
        if let Ok(mut map) = buffer.map_writable() {
            let data = map.as_mut_slice();
            let mut toggle = false;
            for frame in data.chunks_exact_mut(frame_size) {
                toggle = !toggle;
                for ch in 0..num_channels {
                    if mask & (1u64 << ch) != 0 {
                        let start = ch * bps;
                        let end = start + bps;
                        if end <= frame.len() {
                            write_mute_sample(
                                &mut frame[start..end],
                                bps,
                                is_float,
                                is_big_endian,
                                toggle,
                            );
                        }
                    }
                }
            }
        }
        gst::PadProbeReturn::Ok
    });
}

/// Write an inaudible sample that alternates polarity each frame,
/// producing a ~24 kHz square wave at ±1 LSB (integer) or ±1e-20
/// (float).  The AC energy prevents DAC auto-mute from engaging.
fn write_mute_sample(
    dst: &mut [u8],
    bps: usize,
    is_float: bool,
    is_big_endian: bool,
    positive: bool,
) {
    dst.fill(0);
    if is_float {
        // ±1e-20: ~−400 dB, well below any audible threshold.
        if bps == 4 {
            let val: f32 = if positive { 1e-20 } else { -1e-20 };
            let bytes = if is_big_endian {
                val.to_be_bytes()
            } else {
                val.to_le_bytes()
            };
            dst[..4].copy_from_slice(&bytes);
        } else {
            let val: f64 = if positive { 1e-20 } else { -1e-20 };
            let bytes = if is_big_endian {
                val.to_be_bytes()
            } else {
                val.to_le_bytes()
            };
            dst[..8].copy_from_slice(&bytes);
        }
    } else {
        // Integer: ±1 LSB.  Two's complement: +1 = 0x01 in LSB,
        // −1 = 0xFF in all bytes (sign-extended).
        if positive {
            if is_big_endian {
                dst[bps - 1] = 0x01;
            } else {
                dst[0] = 0x01;
            }
        } else {
            dst.fill(0xFF);
        }
    }
}

/// Returns the number of bytes per sample for a `GStreamer` audio format
/// string (e.g. "F32LE" → 4, "S16LE" → 2).  Returns 0 for unknown
/// formats.
fn gst_audio_format_bytes_per_sample(format: &str) -> usize {
    match format {
        "S8" | "U8" => 1,
        "S16LE" | "S16BE" | "U16LE" | "U16BE" => 2,
        "S24LE" | "S24BE" | "U24LE" | "U24BE" => 3,
        // 24-bit audio packed in 32-bit containers uses 4 bytes per sample.
        "S24_32LE" | "S24_32BE" | "U24_32LE" | "U24_32BE" | "S32LE" | "S32BE" | "U32LE"
        | "U32BE" | "F32LE" | "F32BE" => 4,
        "F64LE" | "F64BE" => 8,
        _ => 0,
    }
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

fn build_capsfilter(caps: &gst::Caps) -> anyhow::Result<gst::Element> {
    let capsfilter = gst::ElementFactory::make("capsfilter")
        .build()
        .map_err(|_| anyhow!("missing capsfilter element"))?;
    capsfilter.set_property("caps", caps);
    Ok(capsfilter)
}

/// Observe format renegotiation without copying or converting audio for analysis.
fn install_caps_change_probe(element: &gst::Element, analysis_tx: Sender<AnalysisCommand>) {
    let Some(sink) = element.static_pad("sink") else {
        return;
    };
    let last_rate = AtomicU64::new(0);
    sink.add_probe(gst::PadProbeType::EVENT_DOWNSTREAM, move |_pad, info| {
        if let Some(gst::PadProbeData::Event(ref event)) = info.data {
            if let gst::EventView::Caps(caps_ev) = event.view() {
                let caps = caps_ev.caps();
                if cfg!(feature = "profiling-logs") {
                    eprintln!("[ferrous] audio sink caps change: {caps}");
                }
                if let Some(rate) = caps
                    .structure(0)
                    .and_then(|s| s.get::<i32>("rate").ok())
                    .and_then(|rate| u32::try_from(rate).ok())
                    .filter(|rate| *rate > 0)
                {
                    if last_rate.swap(u64::from(rate), Ordering::Relaxed) != u64::from(rate) {
                        let _ = analysis_tx.send(AnalysisCommand::SetSampleRate(rate));
                    }
                }
            }
        }
        gst::PadProbeReturn::Ok
    });
}

fn build_audio_output_bin(
    analysis_tx: Sender<AnalysisCommand>,
    channel_mute_mask: &Arc<AtomicU64>,
    sink: &gst::Element,
) -> anyhow::Result<gst::Bin> {
    let bin = gst::Bin::new();
    let raw_audio_caps = build_raw_audio_caps();
    let input_capsfilter = build_capsfilter(&raw_audio_caps)?;
    let queue = gst::ElementFactory::make("queue").build()?;
    let convert = gst::ElementFactory::make("audioconvert").build()?;
    let resample = gst::ElementFactory::make("audioresample").build()?;
    let output_capsfilter = build_capsfilter(&raw_audio_caps)?;
    let elements = [
        &input_capsfilter,
        &queue,
        &convert,
        &resample,
        &output_capsfilter,
        sink,
    ];
    bin.add_many(elements)
        .context("failed to add audio output elements")?;
    gst::Element::link_many(elements).context("failed to link audio output")?;
    install_caps_change_probe(&input_capsfilter, analysis_tx);
    install_channel_mute_probe(&output_capsfilter, channel_mute_mask);
    let ingress = input_capsfilter
        .static_pad("sink")
        .context("missing ingress pad")?;
    let ghost = gst::GhostPad::with_target(&ingress)?;
    ghost.set_active(true)?;
    bin.add_pad(&ghost)?;
    Ok(bin)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_volume_test_runtime() -> (GstPlaybackRuntime, Receiver<PlaybackEvent>) {
        gst::init().unwrap();
        let playbin = gst::ElementFactory::make("playbin3")
            .build()
            .expect("playbin3");
        let (analysis_tx, _) = unbounded();
        let (event_tx, event_rx) = unbounded();
        let runtime = GstPlaybackRuntime::new(
            playbin,
            Arc::new(Mutex::new(GaplessQueue::new())),
            analysis_tx,
            event_tx,
            Arc::new(AtomicU64::new(0)),
            Arc::new(Mutex::new(StreamDurations::default())),
        );
        runtime.playbin.set_property("volume", 1.0_f64);
        (runtime, event_rx)
    }

    fn playbin_gain(runtime: &GstPlaybackRuntime) -> f64 {
        runtime.playbin.property::<f64>("volume")
    }

    fn assert_gain(runtime: &GstPlaybackRuntime, expected_level: f64) {
        let expected = expected_level.powi(3);
        let actual = playbin_gain(runtime);
        assert!(
            (actual - expected).abs() < 1e-4,
            "gain {actual}, expected {expected} for level {expected_level}"
        );
    }

    #[test]
    fn volume_fade_depends_on_elapsed_time_not_tick_rate() {
        let full_scale = Duration::from_millis(200);
        for tick_ms in [1_u64, 5, 40] {
            let tick = Duration::from_millis(tick_ms);
            let mut level = 0.0_f32;
            for _ in 0..(120 / tick_ms) {
                let next = step_volume_level(level, 1.0, tick, full_scale).expect("fade running");
                assert!(next - level <= tick.as_secs_f32() / full_scale.as_secs_f32() + 1e-6);
                level = next;
            }
            assert!(
                (level - 0.6).abs() < 1e-4,
                "{tick_ms} ms ticks reached {level}"
            );
            assert_eq!(
                step_volume_level(level, 1.0, Duration::from_millis(80), full_scale),
                None
            );
        }
        assert_eq!(
            step_volume_level(0.8, 0.2, Duration::from_millis(60), full_scale),
            Some(0.5)
        );
    }

    #[test]
    fn engine_ticks_fade_volume_change_on_cubic_gain_without_snapshots() {
        let (mut runtime, events) = make_volume_test_runtime();
        runtime.snapshot.state = PlaybackState::Playing;

        runtime.set_volume(0.5);
        match events.try_recv() {
            Ok(PlaybackEvent::Snapshot(snapshot)) => {
                assert!((snapshot.volume - 0.5).abs() < f32::EPSILON);
            }
            other => panic!("expected volume snapshot, got {other:?}"),
        }
        assert_eq!(runtime.command_wait_timeout(), VOLUME_RAMP_TICK);

        let start = Instant::now();
        runtime.poll_volume_ramp(start);
        assert_gain(&runtime, 1.0);
        for tick in 1..=11 {
            runtime.poll_volume_ramp(start + VOLUME_RAMP_TICK * tick);
        }
        // 55 ms of a 220 ms full-scale fade: a quarter of the way down.
        assert_gain(&runtime, 0.75);

        runtime.poll_volume_ramp(start + Duration::from_millis(200));
        assert_gain(&runtime, 0.5);
        assert!((runtime.snapshot.volume - 0.5).abs() < f32::EPSILON);
        assert_eq!(runtime.command_wait_timeout(), Duration::from_millis(20));
        assert!(
            events.try_recv().is_err(),
            "fade steps must not emit snapshots"
        );
    }

    #[test]
    fn paused_fade_restarts_its_clock_on_resume() {
        let (mut runtime, _events) = make_volume_test_runtime();
        runtime.snapshot.state = PlaybackState::Playing;
        runtime.set_volume(0.0);
        let start = Instant::now();
        runtime.poll_volume_ramp(start);
        runtime.poll_volume_ramp(start + Duration::from_millis(55));
        assert_gain(&runtime, 0.75);

        runtime.snapshot.state = PlaybackState::Paused;
        runtime.poll_volume_ramp(start + Duration::from_millis(60));
        assert_gain(&runtime, 0.75);
        assert_ne!(runtime.command_wait_timeout(), VOLUME_RAMP_TICK);

        runtime.snapshot.state = PlaybackState::Playing;
        let resumed = start + Duration::from_secs(5);
        runtime.poll_volume_ramp(resumed);
        assert_gain(&runtime, 0.75);
        runtime.poll_volume_ramp(resumed + Duration::from_millis(55));
        assert_gain(&runtime, 0.5);
    }

    #[test]
    fn startup_fade_waits_for_silence_hold_then_reaches_level() {
        let (mut runtime, _events) = make_volume_test_runtime();
        runtime.set_volume(0.8);
        let start = Instant::now();
        runtime.snapshot.state = PlaybackState::Playing;
        runtime.applied_volume = 0.0;
        runtime.playbin.set_property("volume", 0.0_f64);
        runtime.startup_gain_ramp = true;
        runtime.startup_ramp_hold_until = Some(start + Duration::from_millis(80));

        runtime.poll_volume_ramp(start + Duration::from_millis(40));
        assert_gain(&runtime, 0.0);
        assert_eq!(runtime.command_wait_timeout(), VOLUME_RAMP_TICK);
        let hold_end = start + Duration::from_millis(80);
        runtime.poll_volume_ramp(hold_end);
        assert_gain(&runtime, 0.0);
        runtime.poll_volume_ramp(hold_end + Duration::from_millis(45));
        assert_gain(&runtime, 0.5);
        runtime.poll_volume_ramp(hold_end + Duration::from_millis(90));
        assert_gain(&runtime, 0.8);
        assert!(!runtime.startup_gain_ramp);
        assert_eq!(runtime.command_wait_timeout(), Duration::from_millis(20));
    }

    #[test]
    fn gst_position_trace_computes_signed_delta_ms() {
        let trace = gst_position_trace(Duration::from_millis(6_880), Duration::from_millis(7_050));
        assert_eq!(trace.previous, 6_880);
        assert_eq!(trace.current, 7_050);
        assert_eq!(trace.delta, 170);
        assert!(!trace.jumped_backward);
    }

    #[test]
    fn gst_position_trace_flags_large_backward_jump() {
        let trace = gst_position_trace(Duration::from_secs(15), Duration::from_millis(800));
        assert_eq!(trace.delta, -14_200);
        assert!(trace.jumped_backward);
    }

    #[test]
    fn smoothed_playback_clock_damps_large_forward_sample_jump() {
        let base = Instant::now();
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.reset(Duration::from_secs_f64(12.0), base);

        let first = clock.update_playing_sample(
            Duration::from_secs_f64(12.039),
            base + Duration::from_millis(40),
        );
        let second = clock.update_playing_sample(
            Duration::from_secs_f64(12.148),
            base + Duration::from_millis(80),
        );

        let first_jump = first.as_secs_f64() - 12.0;
        let second_jump = second.as_secs_f64() - first.as_secs_f64();
        assert!(first_jump > 0.03);
        assert!(
            second_jump < 0.07,
            "modeled jump should be damped, got {second_jump:.6}"
        );
        assert!(
            (Duration::from_secs_f64(12.148).as_secs_f64() - second.as_secs_f64()) > 0.04,
            "modeled position should not fully follow the raw jump immediately"
        );
    }

    #[test]
    fn smoothed_playback_clock_recovers_burst_via_bounded_cadence() {
        let base = Instant::now();
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.reset(Duration::from_secs_f64(12.0), base);

        let first = clock.update_playing_sample(
            Duration::from_secs_f64(12.039),
            base + Duration::from_millis(40),
        );
        let mut modeled = clock.update_playing_sample(
            Duration::from_secs_f64(12.160),
            base + Duration::from_millis(80),
        );
        let mut previous_modeled = modeled;
        let mut maximum_step = modeled.as_secs_f64() - first.as_secs_f64();

        for tick in 3..=20 {
            let raw_position = Duration::from_secs_f64(12.160 + (0.039 * f64::from(tick - 2)));
            modeled = clock.update_playing_sample(
                raw_position,
                base + Duration::from_millis(40 * u64::from(tick as u32)),
            );
            let step = modeled.as_secs_f64() - previous_modeled.as_secs_f64();
            maximum_step = maximum_step.max(step);
            previous_modeled = modeled;
        }

        assert!(
            maximum_step < 0.05,
            "modeled recovery should stay near realtime cadence, got max step {maximum_step:.6}"
        );
        let final_raw = Duration::from_secs_f64(12.160 + (0.039 * 18.0));
        let remaining_error = final_raw.as_secs_f64() - modeled.as_secs_f64();
        assert!(
            remaining_error.abs() < 0.03,
            "modeled recovery should converge back near raw position, remaining error {remaining_error:.6}"
        );
    }

    #[test]
    fn smoothed_playback_clock_stays_synced_to_quantized_44k1_pcm_positions() {
        let base = Instant::now();
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.reset(Duration::ZERO, base);

        const SAMPLE_RATE: u64 = 44_100;
        const AUDIO_BUFFER_FRAMES: u64 = 1_024;
        let mut modeled = Duration::ZERO;

        // A 100 ms query cadence sees either four buffers (92.9 ms) or five
        // buffers (116.1 ms). The media clock still advances at exactly 1x;
        // only the reported position is quantized to the last buffer edge.
        for tick in 1_u64..=200 {
            let elapsed_nanos = tick * 100_000_000;
            let elapsed_frames = elapsed_nanos * SAMPLE_RATE / 1_000_000_000;
            let reported_frames = elapsed_frames / AUDIO_BUFFER_FRAMES * AUDIO_BUFFER_FRAMES;
            let raw_position = Duration::from_nanos(reported_frames * 1_000_000_000 / SAMPLE_RATE);
            modeled = clock
                .update_playing_sample(raw_position, base + Duration::from_nanos(elapsed_nanos));
        }

        let ideal_position = Duration::from_secs(20);
        let lag = ideal_position.saturating_sub(modeled).as_secs_f64();
        assert!(
            lag < 0.025,
            "quantized 44.1 kHz positions must not bias the playback clock, lag={lag:.6}s"
        );
    }

    #[test]
    fn seek_hold_does_not_run_ahead_before_backend_reacquires_position() {
        let base = Instant::now();
        let target = Duration::from_secs_f64(12.0);
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.enter_seek_hold(target, base);

        let visible = clock.current_position(base + Duration::from_millis(120));
        assert_eq!(
            visible, target,
            "seek hold should keep the requested target stable until GStreamer confirms playback position"
        );
    }

    #[test]
    fn smoothed_playback_clock_resets_exactly_on_seek_target() {
        let base = Instant::now();
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.reset(Duration::from_secs_f64(48.0), base);

        assert_eq!(clock.current_position(base), Duration::from_secs(48));

        let post_release = clock.update_playing_sample(
            Duration::from_secs_f64(48.24),
            base + Duration::from_millis(240),
        );
        let visible_jump = post_release.as_secs_f64() - 48.0;
        assert!(
            visible_jump < 0.26,
            "post-seek modeled jump should stay below the raw catch-up, got {visible_jump:.6}"
        );
    }

    #[test]
    fn post_seek_hold_release_preserves_first_catch_up_sample() {
        let base = Instant::now();
        let target = Duration::from_secs_f64(48.0);
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.reset(target, base);
        let raw = Duration::from_secs_f64(48.24);

        let modeled = release_seek_hold_sample(
            &mut clock,
            target,
            base + Duration::from_millis(220),
            raw,
            base + Duration::from_millis(240),
        );

        assert!(
            (raw.as_secs_f64() - modeled.as_secs_f64()) < 0.05,
            "post-seek release should stay close to the first raw sample, raw={:.6} modeled={:.6}",
            raw.as_secs_f64(),
            modeled.as_secs_f64()
        );
    }

    #[test]
    fn gst_clock_trace_converts_unknown_clock_values_to_negative_one() {
        let trace = gst_clock_trace(
            gst::State::Playing,
            gst::State::VoidPending,
            None,
            None,
            None,
        );
        assert_eq!(trace.current_state, gst::State::Playing);
        assert_eq!(trace.pending_state, gst::State::VoidPending);
        assert_eq!(trace.running, -1);
        assert_eq!(trace.clock, -1);
        assert_eq!(trace.base, -1);
    }

    #[test]
    fn backend_clock_trace_reports_mode() {
        let mode = PlaybackClockMode::SeekReacquire;
        let text = format!("{mode:?}");
        assert!(text.contains("SeekReacquire"));
    }

    #[test]
    fn recent_seek_trace_preserves_target_and_elapsed_ms() {
        let trace = recent_seek_trace(
            Some(Duration::from_millis(2_500)),
            Some(Duration::from_millis(180)),
            Some(Duration::from_millis(35)),
        );
        assert_eq!(trace.target, 2_500);
        assert_eq!(trace.since_issue, 180);
        assert_eq!(trace.since_release, 35);
    }

    #[test]
    fn flac_seek_uses_accurate_mode_to_avoid_parser_stall() {
        let flags = seek_flags_for_path(Some(Path::new("/tmp/test.flac")));
        assert!(flags.contains(gst::SeekFlags::FLUSH));
        assert!(flags.contains(gst::SeekFlags::ACCURATE));
        assert!(!flags.contains(gst::SeekFlags::KEY_UNIT));
    }

    #[test]
    fn dts_seek_stays_key_unit_based() {
        let flags = seek_flags_for_path(Some(Path::new("/tmp/test.dts")));
        assert!(flags.contains(gst::SeekFlags::FLUSH));
        assert!(flags.contains(gst::SeekFlags::KEY_UNIT));
        assert!(!flags.contains(gst::SeekFlags::ACCURATE));
    }
}

fn file_uri(path: &Path) -> Option<String> {
    url::Url::from_file_path(path).ok().map(|u| u.to_string())
}

/// Integration test for MP3 files that have both ID3 and APE tags.
/// Reproduces the pipeline setup used by the real engine (custom
/// typefinders + source probe + analysis audio sink) to catch
/// `not-linked` regressions from GStreamer version bumps.
#[cfg(all(test, feature = "gst"))]
mod gst_integration_tests {
    use super::*;
    use crate::playback::PathRenames;

    fn wait_for_playing_or_error(playbin: &gst::Element, timeout_secs: u64) -> Result<(), String> {
        let bus = playbin.bus().expect("bus");
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(timeout_secs);
        while std::time::Instant::now() < deadline {
            let Some(msg) = bus.timed_pop(gst::ClockTime::from_mseconds(100)) else {
                continue;
            };
            match msg.view() {
                gst::MessageView::Error(err) => {
                    return Err(format!(
                        "GStreamer error from {:?}: {} ({:?})",
                        err.src().map(gstreamer::prelude::GstObjectExt::path_string),
                        err.error(),
                        err.debug()
                    ));
                }
                gst::MessageView::StateChanged(sc) => {
                    if sc.src().is_some_and(|s| s == playbin) && sc.current() == gst::State::Playing
                    {
                        return Ok(());
                    }
                }
                _ => {}
            }
        }
        Err("timeout waiting for Playing state".into())
    }

    #[test]
    fn output_bin_reports_native_format_without_a_pcm_analysis_branch() {
        gst::init().unwrap();
        for (rate, channels) in [(192_000i32, 6i32), (44_100, 2)] {
            let (analysis_tx, analysis_rx) = unbounded();
            let sink = gst::ElementFactory::make("fakesink")
                .property("sync", false)
                .build()
                .unwrap();
            let bin =
                build_audio_output_bin(analysis_tx, &Arc::new(AtomicU64::new(0)), &sink).unwrap();
            let factories: Vec<_> = bin
                .children()
                .iter()
                .filter_map(|element| element.factory().map(|factory| factory.name().to_string()))
                .collect();
            assert!(!factories
                .iter()
                .any(|name| name == "appsink" || name == "tee"));
            assert_eq!(
                factories
                    .iter()
                    .filter(|name| *name == "audioresample")
                    .count(),
                1
            );
            let source = gst::ElementFactory::make("audiotestsrc")
                .property("num-buffers", 3i32)
                .build()
                .unwrap();
            let caps = gst::Caps::builder("audio/x-raw")
                .field("rate", rate)
                .field("channels", channels)
                .build();
            let filter = build_capsfilter(&caps).unwrap();
            let pipeline = gst::Pipeline::new();
            pipeline
                .add_many([&source, &filter, bin.upcast_ref()])
                .unwrap();
            gst::Element::link_many([&source, &filter, bin.upcast_ref()]).unwrap();
            pipeline.set_state(gst::State::Playing).unwrap();
            let bus = pipeline.bus().unwrap();
            let result = bus.timed_pop_filtered(
                gst::ClockTime::from_seconds(2),
                &[gst::MessageType::Eos, gst::MessageType::Error],
            );
            pipeline.set_state(gst::State::Null).unwrap();
            assert!(
                matches!(
                    result.as_ref().map(|message| message.view()),
                    Some(gst::MessageView::Eos(_))
                ),
                "{result:?}"
            );
            assert!(
                matches!(analysis_rx.try_recv(), Ok(AnalysisCommand::SetSampleRate(value)) if value == u32::try_from(rate).unwrap())
            );
            assert!(analysis_rx.try_recv().is_err());
        }
    }

    /// The engine runtime on a playbin3 that plays into a clocked fakesink.
    struct TestEngine {
        runtime: GstPlaybackRuntime,
        bus: gst::Bus,
        events: Receiver<PlaybackEvent>,
        _analysis: Receiver<AnalysisCommand>,
    }

    impl TestEngine {
        fn new() -> Self {
            gst::init().unwrap();
            let playbin = gst::ElementFactory::make("playbin3")
                .build()
                .expect("playbin3");
            let sink = gst::ElementFactory::make("fakesink")
                .property("sync", true)
                .build()
                .unwrap();
            playbin.set_property("audio-sink", &sink);
            let bus = playbin.bus().expect("bus");
            let (analysis_tx, analysis) = unbounded();
            let (event_tx, events) = unbounded();
            let runtime = GstPlaybackRuntime::new(
                playbin,
                Arc::new(Mutex::new(GaplessQueue::new())),
                analysis_tx,
                event_tx,
                Arc::new(AtomicU64::new(0)),
                Arc::new(Mutex::new(StreamDurations::default())),
            );
            Self {
                runtime,
                bus,
                events,
                _analysis: analysis,
            }
        }

        /// Pass bus messages to the runtime until the playbin plays or fails.
        fn wait_until_playing(&mut self) -> Result<(), String> {
            let deadline = Instant::now() + Duration::from_secs(5);
            while Instant::now() < deadline {
                let Some(msg) = self.bus.timed_pop(gst::ClockTime::from_mseconds(20)) else {
                    continue;
                };
                let error = match msg.view() {
                    gst::MessageView::Error(err) => Some(err.error().to_string()),
                    _ => None,
                };
                let playing = matches!(msg.view(), gst::MessageView::StateChanged(changed)
                    if changed.src().is_some_and(|src| src == &self.runtime.playbin)
                        && changed.current() == gst::State::Playing);
                self.runtime.handle_bus_message(&msg);
                if let Some(error) = error {
                    return Err(error);
                }
                if playing {
                    return Ok(());
                }
            }
            Err("timeout waiting for Playing state".into())
        }

        fn current_uri(&self) -> Option<String> {
            self.runtime.playbin.property("current-uri")
        }
    }

    impl Drop for TestEngine {
        fn drop(&mut self) {
            let _ = self.runtime.playbin.set_state(gst::State::Null);
        }
    }

    fn tone_fixtures(test: &str, names: &[&str]) -> (PathBuf, Vec<PathBuf>) {
        let dir = std::env::temp_dir().join(format!("ferrous-{test}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let paths = names
            .iter()
            .map(|name| {
                let path = dir.join(name);
                std::fs::write(&path, include_bytes!("../analysis/fixtures/seek-tone.mp3"))
                    .unwrap();
                path
            })
            .collect();
        (dir, paths)
    }

    #[test]
    fn renamed_next_track_plays_from_its_new_path() {
        let (dir, paths) = tone_fixtures("rename-next", &["a.mp3", "b.mp3"]);
        let renamed = dir.join("renamed b.mp3");
        let mut engine = TestEngine::new();
        engine
            .runtime
            .apply_command(PlaybackCommand::LoadQueue(paths.clone()));
        engine.runtime.apply_command(PlaybackCommand::Play);
        engine.wait_until_playing().expect("first track plays");

        std::fs::rename(&paths[1], &renamed).unwrap();
        while engine.events.try_recv().is_ok() {}
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (paths[1].clone(), renamed.clone()),
            ]))));
        assert!(matches!(
            engine.events.try_recv(),
            Ok(PlaybackEvent::PathsRemapped)
        ));
        assert_eq!(engine.runtime.snapshot.current.as_ref(), Some(&paths[0]));
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);

        engine.runtime.apply_command(PlaybackCommand::Next);
        engine.wait_until_playing().expect("renamed track plays");
        engine.runtime.apply_command(PlaybackCommand::Poll);

        assert_eq!(engine.runtime.snapshot.current.as_ref(), Some(&renamed));
        assert_eq!(engine.runtime.snapshot.current_queue_index, Some(1));
        assert_eq!(engine.current_uri(), file_uri(&renamed));
        for event in engine.events.try_iter() {
            match event {
                PlaybackEvent::Snapshot(snapshot) => {
                    assert_ne!(snapshot.current.as_ref(), Some(&paths[1]));
                }
                PlaybackEvent::TrackChanged { path, .. } => assert_eq!(path, renamed),
                _ => {}
            }
        }
        drop(engine);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn renamed_current_track_replays_after_stop() {
        let (dir, paths) = tone_fixtures("rename-current", &["current.mp3"]);
        let renamed = dir.join("renamed current.mp3");
        let mut engine = TestEngine::new();
        engine
            .runtime
            .apply_command(PlaybackCommand::LoadQueue(paths.clone()));
        engine.runtime.apply_command(PlaybackCommand::Play);
        engine.wait_until_playing().expect("track plays");

        std::fs::rename(&paths[0], &renamed).unwrap();
        while engine.events.try_recv().is_ok() {}
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (paths[0].clone(), renamed.clone()),
            ]))));
        assert!(matches!(
            engine.events.try_recv(),
            Ok(PlaybackEvent::PathsRemapped)
        ));
        assert!(matches!(
            engine.events.try_recv(),
            Ok(PlaybackEvent::Snapshot(snapshot))
                if snapshot.current.as_ref() == Some(&renamed)
                    && snapshot.state == PlaybackState::Playing
        ));
        assert_eq!(
            engine.runtime.playbin.current_state(),
            gst::State::Playing,
            "the rename does not interrupt the open stream"
        );

        engine.runtime.apply_command(PlaybackCommand::Stop);
        engine.runtime.apply_command(PlaybackCommand::Play);
        engine.wait_until_playing().expect("renamed track replays");
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);
        assert_eq!(engine.current_uri(), file_uri(&renamed));
        drop(engine);
        std::fs::remove_dir_all(dir).unwrap();
    }

    fn make_full_analysis_sink() -> gst::Bin {
        let (analysis_tx, _) = unbounded();
        let sink = gst::ElementFactory::make("fakesink")
            .property("sync", false)
            .build()
            .unwrap();
        build_audio_output_bin(analysis_tx, &Arc::new(AtomicU64::new(0)), &sink).unwrap()
    }

    /// Full app pipeline pattern: custom typefinders + source probe
    /// + full analysis sink.  Verifies that MP3 files with APE tags
    /// play correctly (regression test for apedemux rank demotion).
    #[test]
    fn playbin3_mp3_with_typefinders_and_full_sink() {
        gst::init().unwrap();
        for plugin in ["id3demux", "apedemux", "mpegaudioparse"] {
            assert!(
                gst::ElementFactory::find(plugin).is_some(),
                "install GStreamer good runtime plugins: missing {plugin}"
            );
        }
        assert!(
            ["mpg123audiodec", "avdec_mp3", "avdec_mp3float"]
                .iter()
                .any(|name| gst::ElementFactory::find(name).is_some()),
            "install a GStreamer MP3 decoder (e.g. the libav runtime plugin)"
        );
        let fixture =
            std::env::temp_dir().join(format!("ferrous-output-tags-{}.mp3", std::process::id()));
        let test_path = fixture.as_path();
        let mut bytes = b"ID3\x04\0\0\0\0\0\0".to_vec();
        bytes.extend_from_slice(include_bytes!("../analysis/fixtures/seek-tone.mp3"));
        bytes.extend_from_slice(&crate::raw_audio::build_test_apev2_tag(
            &[("Title", "Synthetic fixture")],
            true,
        ));
        std::fs::write(test_path, bytes).unwrap();

        gst::init().unwrap();
        configure_raw_surround_decoding();

        let playbin = gst::ElementFactory::make("playbin3")
            .build()
            .expect("playbin3");
        configure_playbin_buffering(&playbin);
        install_raw_surround_source_probe(&playbin);
        playbin.set_property("audio-sink", &make_full_analysis_sink());

        let _ = playbin.set_state(gst::State::Ready);
        let uri = file_uri(test_path).expect("file_uri");
        playbin.set_property("uri", &uri);
        playbin
            .set_state(gst::State::Playing)
            .expect("set_state(Playing)");

        match wait_for_playing_or_error(&playbin, 5) {
            Ok(()) => {}
            Err(e) => {
                let _ = playbin.set_state(gst::State::Null);
                panic!("{e}");
            }
        }
        let _ = playbin.set_state(gst::State::Null);
        std::fs::remove_file(test_path).unwrap();
    }
}

/// Engine tests against a real playbin3 with generated fixtures. A
/// clock-synchronized `fakesink` replaces the audio device, so the tests
/// run in real time.
#[cfg(all(test, feature = "gst"))]
mod engine_tests {
    use super::*;

    const TIMEOUT: Duration = Duration::from_secs(10);
    const WAV_RATE: u32 = 8_000;

    struct Fixtures {
        dir: PathBuf,
    }

    impl Fixtures {
        fn new(tag: &str) -> Self {
            let dir =
                std::env::temp_dir().join(format!("ferrous-gapless-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Self { dir }
        }

        /// 16-bit mono PCM WAV of `millis` milliseconds. Tracks that start a
        /// pipeline run last at least 4 s, longer than the pipeline reads
        /// ahead: a shorter one reaches about-to-finish while the pipeline
        /// is still changing state, and a successor handed over then can
        /// deadlock uridecodebin3 (seen with GStreamer 1.28), independent
        /// of this engine.
        fn wav(&self, name: &str, millis: u32) -> PathBuf {
            let frames = WAV_RATE * millis / 1_000;
            let data_len = frames * 2;
            let mut bytes = Vec::new();
            bytes.extend_from_slice(b"RIFF");
            bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
            bytes.extend_from_slice(b"WAVEfmt ");
            bytes.extend_from_slice(&16u32.to_le_bytes());
            bytes.extend_from_slice(&1u16.to_le_bytes());
            bytes.extend_from_slice(&1u16.to_le_bytes());
            bytes.extend_from_slice(&WAV_RATE.to_le_bytes());
            bytes.extend_from_slice(&(WAV_RATE * 2).to_le_bytes());
            bytes.extend_from_slice(&2u16.to_le_bytes());
            bytes.extend_from_slice(&16u16.to_le_bytes());
            bytes.extend_from_slice(b"data");
            bytes.extend_from_slice(&data_len.to_le_bytes());
            for frame in 0..frames {
                let sample = i16::try_from(frame % 40).unwrap() * 400 - 8_000;
                bytes.extend_from_slice(&sample.to_le_bytes());
            }
            let path = self.dir.join(name);
            std::fs::write(&path, bytes).unwrap();
            path
        }

        /// MPEG-1 Layer III without a Xing header: 2 s of 32 kbit/s frames,
        /// then 3 s of 320 kbit/s frames. The parser first estimates the
        /// duration from the low bitrate and refines it while reading.
        fn vbr_mp3(&self, name: &str) -> PathBuf {
            let frame = |bitrate_index: u8, bitrate: usize| {
                let mut frame = vec![0xFF, 0xFB, bitrate_index << 4, 0xC0];
                // Zeroed side information decodes as silence.
                frame.resize(144 * bitrate / 44_100, 0);
                frame
            };
            let mut bytes = Vec::new();
            for _ in 0..VBR_LOW_FRAMES {
                bytes.extend(frame(1, 32_000));
            }
            for _ in 0..VBR_HIGH_FRAMES {
                bytes.extend(frame(14, 320_000));
            }
            let path = self.dir.join(name);
            std::fs::write(&path, bytes).unwrap();
            path
        }
    }

    impl Drop for Fixtures {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.dir);
        }
    }

    const VBR_LOW_FRAMES: u32 = 77;
    const VBR_HIGH_FRAMES: u32 = 115;

    fn vbr_duration() -> Duration {
        Duration::from_nanos(
            u64::from(VBR_LOW_FRAMES + VBR_HIGH_FRAMES) * 1_152 * 1_000_000_000 / 44_100,
        )
    }

    fn fake_output() -> anyhow::Result<gst::Element> {
        Ok(gst::ElementFactory::make("fakesink")
            .property("sync", true)
            .build()?)
    }

    #[derive(Debug, Clone)]
    struct Change {
        path: PathBuf,
        index: usize,
        kind: TrackChangeKind,
        token: u64,
    }

    /// Drives the engine as the bridge does and records what it reports.
    struct Engine {
        commands: Sender<PlaybackCommand>,
        events: Receiver<PlaybackEvent>,
        analysis: Receiver<AnalysisCommand>,
        snapshot: PlaybackSnapshot,
        snapshots: Vec<PlaybackSnapshot>,
        changes: Vec<Change>,
        prepared: Vec<PathBuf>,
        cancelled_staging: usize,
    }

    impl Engine {
        fn start() -> Self {
            gst::init().unwrap();
            let (analysis_tx, analysis) = unbounded();
            let (commands, events) = spawn_engine_with_output(analysis_tx, fake_output);
            Self {
                commands,
                events,
                analysis,
                snapshot: PlaybackSnapshot::default(),
                snapshots: Vec::new(),
                changes: Vec::new(),
                prepared: Vec::new(),
                cancelled_staging: 0,
            }
        }

        fn send(&self, command: PlaybackCommand) {
            self.commands.send(command).unwrap();
        }

        /// Poll and record until `done` holds. Returns `false` on timeout.
        fn run_until(&mut self, timeout: Duration, mut done: impl FnMut(&Self) -> bool) -> bool {
            let deadline = Instant::now() + timeout;
            loop {
                if done(self) {
                    return true;
                }
                if Instant::now() >= deadline {
                    return false;
                }
                self.send(PlaybackCommand::Poll);
                while let Ok(event) = self.events.recv_timeout(Duration::from_millis(20)) {
                    self.record(event);
                }
                while let Ok(command) = self.analysis.try_recv() {
                    match command {
                        AnalysisCommand::PrepareGaplessContinuation { path } => {
                            self.prepared.push(path);
                        }
                        AnalysisCommand::CancelStagedContinuation => self.cancelled_staging += 1,
                        _ => {}
                    }
                }
            }
        }

        fn record(&mut self, event: PlaybackEvent) {
            match event {
                PlaybackEvent::Snapshot(snapshot) => {
                    self.snapshots.push(snapshot.clone());
                    self.snapshot = snapshot;
                }
                PlaybackEvent::TrackChanged {
                    path,
                    queue_index,
                    kind,
                    track_token,
                } => self.changes.push(Change {
                    path,
                    index: queue_index,
                    kind,
                    token: track_token,
                }),
                PlaybackEvent::Seeked { .. } | PlaybackEvent::PathsRemapped => {}
            }
        }

        /// Wait until about-to-finish staged `successor`.
        fn wait_for_window(&mut self, successor: &Path) {
            assert!(
                self.run_until(TIMEOUT, |engine| engine
                    .prepared
                    .iter()
                    .any(|path| path == successor)),
                "about-to-finish did not stage {}",
                successor.display()
            );
        }

        /// Wait inside the window until the current track has played 1 s.
        /// When about-to-finish fires, uridecodebin3 switches its input to
        /// the successor; a state change at that moment can deadlock
        /// uridecodebin3 (seen with GStreamer 1.28), independent of this
        /// engine.
        fn wait_for_settled_window(&mut self, successor: &Path) {
            self.wait_for_window(successor);
            let row = self.snapshot.current_queue_index;
            assert!(self.run_until(TIMEOUT, |engine| engine.snapshot.position
                >= Duration::from_secs(1)));
            assert_eq!(
                self.snapshot.current_queue_index, row,
                "the successor is not audible yet"
            );
        }

        fn wait_for_changes(&mut self, count: usize) -> Change {
            assert!(
                self.run_until(TIMEOUT, |engine| engine.changes.len() >= count),
                "expected {count} track changes, got {:?}",
                self.changes
            );
            self.changes[count - 1].clone()
        }

        /// Wait until the snapshot shows row `index` with about `duration`,
        /// which identifies the fixture the pipeline plays.
        fn wait_for_playing(&mut self, index: usize, duration: Duration) {
            let reached = self.run_until(TIMEOUT, |engine| {
                engine.snapshot.current_queue_index == Some(index)
                    && engine.snapshot.duration.abs_diff(duration) < Duration::from_millis(100)
            });
            assert!(
                reached,
                "expected row {index} lasting {duration:?}, got {:?}",
                self.snapshot
            );
        }
    }

    #[derive(Debug, Clone, Copy)]
    enum Expect {
        Manual,
        Gapless,
        /// After a restart of the audible track, which happens gaplessly
        /// unless a short track reached about-to-finish while it prerolled.
        Automatic,
    }

    fn assert_change(change: &Change, path: &Path, index: usize, expect: Expect) {
        assert_eq!(
            (change.path.as_path(), change.index),
            (path, index),
            "{change:?}"
        );
        let matches = match expect {
            Expect::Manual => matches!(change.kind, TrackChangeKind::Manual),
            Expect::Gapless => matches!(change.kind, TrackChangeKind::Gapless),
            Expect::Automatic => matches!(
                change.kind,
                TrackChangeKind::Gapless | TrackChangeKind::Natural
            ),
        };
        assert!(matches, "expected {expect:?}, got {change:?}");
    }

    fn millis(value: u64) -> Duration {
        Duration::from_millis(value)
    }

    #[test]
    fn gapless_successor_becomes_current_when_its_audio_starts() {
        let fixtures = Fixtures::new("commit");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        let started = Instant::now();
        engine.wait_for_window(&b);
        assert_eq!(engine.snapshot.current.as_deref(), Some(a.as_path()));
        assert_eq!(engine.snapshot.current_queue_index, Some(0));

        let change = engine.wait_for_changes(2);
        assert!(
            started.elapsed() >= millis(3_500),
            "the successor became current {:?} after A started",
            started.elapsed()
        );
        assert_change(&change, &b, 1, Expect::Gapless);
        engine.wait_for_playing(1, millis(4_500));
        assert_eq!(engine.snapshot.track_token, change.token);
        assert_ne!(change.token, engine.changes[0].token);
    }

    #[test]
    fn short_tracks_commit_in_order_with_their_own_durations() {
        // The pipeline reads a short track at once, so the track after it
        // is handed over before the short track is audible.
        let fixtures = Fixtures::new("chain");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 1_200);
        let c = fixtures.wav("c.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
        engine.wait_for_playing(1, millis(1_200));
        assert_change(&engine.wait_for_changes(3), &c, 2, Expect::Gapless);
        engine.wait_for_playing(2, millis(4_500));
    }

    #[test]
    fn next_inside_the_window_plays_the_following_track() {
        let fixtures = Fixtures::new("next");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::Next);
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Manual);
        engine.wait_for_playing(1, millis(4_500));
    }

    #[test]
    fn previous_inside_the_window_returns_to_the_previous_track() {
        let fixtures = Fixtures::new("previous");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b, c.clone()]));
        engine.send(PlaybackCommand::PlayAt(1));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&c);
        engine.send(PlaybackCommand::Previous);
        assert_change(&engine.wait_for_changes(3), &a, 0, Expect::Manual);
        engine.wait_for_playing(0, millis(4_000));
    }

    #[test]
    fn removing_another_row_inside_the_window_keeps_the_audible_track() {
        let fixtures = Fixtures::new("remove-other");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::RemoveAt(2));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
        engine.wait_for_playing(1, millis(4_500));
        assert_eq!(engine.cancelled_staging, 0);
    }

    #[test]
    fn removing_the_staged_successor_plays_the_next_one_after_the_audible_track() {
        let fixtures = Fixtures::new("remove-successor");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::RemoveAt(1));
        assert!(engine.run_until(TIMEOUT, |engine| engine.cancelled_staging == 1));
        assert_eq!(engine.snapshot.current.as_deref(), Some(a.as_path()));
        assert_eq!(engine.snapshot.current_queue_index, Some(0));

        assert_change(&engine.wait_for_changes(2), &c, 1, Expect::Automatic);
        engine.wait_for_playing(1, millis(5_000));
        assert!(!engine
            .snapshots
            .iter()
            .any(|snapshot| snapshot.duration.abs_diff(millis(4_500)) < millis(100)));
    }

    #[test]
    fn stop_then_play_inside_the_window_restarts_the_stopped_track() {
        let fixtures = Fixtures::new("stop-play");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::Stop);
        engine.send(PlaybackCommand::Play);
        let replayed = Instant::now();
        engine.wait_for_playing(0, millis(4_000));
        assert_eq!(engine.snapshot.current.as_deref(), Some(a.as_path()));

        let change = engine.wait_for_changes(2);
        assert!(
            replayed.elapsed() >= millis(3_500),
            "the successor became current {:?} after the replay",
            replayed.elapsed()
        );
        assert_change(&change, &b, 1, Expect::Gapless);
    }

    #[test]
    fn seek_inside_the_window_stays_on_the_audible_track() {
        let fixtures = Fixtures::new("seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::Seek(millis(500)));
        let sought = Instant::now();
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Automatic);
        assert!(
            sought.elapsed() >= millis(3_000),
            "the successor became current {:?} after the seek",
            sought.elapsed()
        );
    }

    #[test]
    fn enabling_repeat_one_inside_the_window_replays_the_audible_track() {
        let fixtures = Fixtures::new("repeat-toggle");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_settled_window(&b);
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::One));
        assert_change(&engine.wait_for_changes(2), &a, 0, Expect::Automatic);
        engine.wait_for_playing(0, millis(4_000));
    }

    #[test]
    fn repeat_one_reports_each_replay_as_a_track_change() {
        let fixtures = Fixtures::new("repeat-one");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::One));
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_window(&a);
        let replay = engine.wait_for_changes(2);
        assert_change(&replay, &a, 0, Expect::Gapless);
        assert_ne!(replay.token, engine.changes[0].token);
        assert!(
            engine.run_until(TIMEOUT, |engine| engine.snapshot.track_token
                == replay.token)
        );
        assert_eq!(engine.snapshot.current_queue_index, Some(0));
        assert!(engine.snapshot.position < millis(1_000));
        assert_eq!(engine.changes.len(), 2, "one track change per play");
    }

    #[test]
    fn same_file_queued_twice_advances_to_the_second_row() {
        let fixtures = Fixtures::new("duplicate");
        let a = fixtures.wav("a.wav", 4_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), a.clone()]));
        engine.send(PlaybackCommand::Play);
        let second = engine.wait_for_changes(2);
        assert_change(&second, &a, 1, Expect::Gapless);
        assert!(
            engine.run_until(TIMEOUT, |engine| engine.snapshot.current_queue_index
                == Some(1))
        );
        assert_eq!(engine.snapshot.track_token, second.token);
        assert_eq!(engine.changes.len(), 2, "one track change per play");
    }

    #[test]
    fn duration_refinements_apply_after_a_gapless_switch() {
        gst::init().unwrap();
        assert!(
            gst::ElementFactory::find("mpegaudioparse").is_some()
                && ["mpg123audiodec", "avdec_mp3", "avdec_mp3float"]
                    .iter()
                    .any(|name| gst::ElementFactory::find(name).is_some()),
            "install an MP3 parser and decoder (GStreamer good and libav plugins)"
        );
        let fixtures = Fixtures::new("vbr");
        let track = fixtures.vbr_mp3("vbr.mp3");
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![
            track.clone(),
            track.clone(),
        ]));
        engine.send(PlaybackCommand::Play);
        assert_change(&engine.wait_for_changes(2), &track, 1, Expect::Gapless);

        // The parser first estimates the duration from the low bitrate at
        // the start and refines it while reading on. The refined value must
        // replace the first one, and the successor's own first estimate
        // must not replace it before the switch.
        let first_token = engine.changes[0].token;
        let first_row: Vec<_> = engine
            .snapshots
            .iter()
            .filter(|snapshot| snapshot.track_token == first_token)
            .map(|snapshot| snapshot.duration)
            .filter(|duration| !duration.is_zero())
            .collect();
        let (Some(&estimate), Some(&refined)) = (first_row.first(), first_row.last()) else {
            panic!("no duration for the first row");
        };
        assert!(estimate > refined + Duration::from_secs(1), "{first_row:?}");
        let refined_at = first_row
            .iter()
            .position(|duration| *duration == refined)
            .unwrap();
        assert!(
            first_row[refined_at..]
                .iter()
                .all(|duration| *duration == refined),
            "{first_row:?}"
        );
        assert!(
            refined.abs_diff(vbr_duration()) < vbr_duration() / 5,
            "{refined:?} for {:?}",
            vbr_duration()
        );
        engine.wait_for_playing(1, refined);
    }
}
