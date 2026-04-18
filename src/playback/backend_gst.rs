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
    PlaybackCommand, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode, TrackChangeKind,
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
    learned_rate: f64,
    last_raw_position: Option<Duration>,
    last_raw_instant: Option<Instant>,
    initialized: bool,
    mode: PlaybackClockMode,
}

impl SmoothedPlaybackClock {
    const SNAP_ERROR_SECONDS: f64 = 0.75;
    const IGNORE_ERROR_SECONDS: f64 = 0.012;
    const RATE_ALPHA: f64 = 0.35;
    const PHASE_RECOVERY_HORIZON_SECONDS: f64 = 0.75;
    const PHASE_RATE_OFFSET_MAX: f64 = 0.10;
    const RATE_LEARN_MIN: f64 = 0.9;
    const RATE_LEARN_MAX: f64 = 1.1;
    const RATE_CLAMP_MIN: f64 = 0.92;
    const RATE_CLAMP_MAX: f64 = 1.10;

    fn new(now: Instant) -> Self {
        Self {
            anchor_position: Duration::ZERO,
            anchor_instant: now,
            rate: 1.0,
            learned_rate: 1.0,
            last_raw_position: None,
            last_raw_instant: None,
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
        self.learned_rate = 1.0;
        self.last_raw_position = Some(position);
        self.last_raw_instant = Some(now);
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

        if let (Some(previous_raw), Some(previous_raw_instant)) =
            (self.last_raw_position, self.last_raw_instant)
        {
            let sample_elapsed_seconds = now
                .checked_duration_since(previous_raw_instant)
                .unwrap_or(Duration::ZERO)
                .as_secs_f64();
            let sample_delta_seconds =
                (raw_position.as_secs_f64() - previous_raw.as_secs_f64()).max(0.0);
            if sample_elapsed_seconds >= 0.02 {
                let measured_rate = sample_delta_seconds / sample_elapsed_seconds;
                if (Self::RATE_LEARN_MIN..=Self::RATE_LEARN_MAX).contains(&measured_rate) {
                    self.learned_rate = ((self.learned_rate * (1.0 - Self::RATE_ALPHA))
                        + (measured_rate * Self::RATE_ALPHA))
                        .clamp(Self::RATE_LEARN_MIN, Self::RATE_LEARN_MAX);
                }
            }
        }

        let phase_rate_offset = if error_seconds.abs() <= Self::IGNORE_ERROR_SECONDS {
            0.0
        } else {
            (error_seconds / Self::PHASE_RECOVERY_HORIZON_SECONDS)
                .clamp(-Self::PHASE_RATE_OFFSET_MAX, Self::PHASE_RATE_OFFSET_MAX)
        };
        self.rate = (self.learned_rate + phase_rate_offset)
            .clamp(Self::RATE_CLAMP_MIN, Self::RATE_CLAMP_MAX);
        let corrected = predicted;
        self.anchor_position = corrected;
        self.anchor_instant = now;
        self.last_raw_position = Some(raw_position);
        self.last_raw_instant = Some(now);
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
    /// Shared with the PCM tap thread.  The tap reads this to tag each
    /// chunk; the analysis runtime uses the value to filter stale data.
    /// Only updated for non-gapless transitions (manual, cross-format).
    analysis_pcm_token: Arc<AtomicU64>,
    /// Local counter for generating unique track tokens (waveform jobs,
    /// track-changed events).  Always incremented, even for gapless.
    track_token_counter: u64,
    event_tx: Sender<PlaybackEvent>,
    snapshot: PlaybackSnapshot,
    target_volume: f32,
    applied_volume: f32,
    startup_gain_ramp: bool,
    startup_ramp_hold_until: Option<Instant>,
    buffering_active: bool,
    seek_hold: Option<(Instant, Duration)>,
    last_seek_issued_at: Option<Instant>,
    last_seek_released_at: Option<Instant>,
    last_seek_target: Option<Duration>,
    /// Set by the about-to-finish handler when the next track has a
    /// different codec (file extension).  The EOS handler will perform
    /// a full pipeline switch instead of relying on gapless playback.
    pending_eos_track_switch: Arc<AtomicBool>,
    /// playbin3 pre-rolls the next stream ~2 s before the current track
    /// ends, which causes `query_duration` to return the *next* track's
    /// duration prematurely.  We stash the pending duration here and only
    /// commit it when the position actually confirms the stream switch
    /// (i.e., position jumps backward).
    pending_gapless_duration: Option<Duration>,
    position_clock: SmoothedPlaybackClock,
    /// Set by the about-to-finish handler when a spectrogram staging
    /// thread has been started for the likely next track.  Checked on
    /// cancellation paths to send `CancelStagedContinuation`.
    staged_continuation_active: Arc<AtomicBool>,
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

    // All arguments are distinct shared-state handles passed from the
    // pipeline builder — grouping them would add indirection without
    // reducing real complexity.
    #[allow(clippy::too_many_arguments)]
    fn new(
        playbin: gst::Element,
        queue_state: Arc<Mutex<GaplessQueue>>,
        analysis_tx: Sender<AnalysisCommand>,
        analysis_pcm_token: Arc<AtomicU64>,
        event_tx: Sender<PlaybackEvent>,
        pending_eos_track_switch: Arc<AtomicBool>,
        staged_continuation_active: Arc<AtomicBool>,
        channel_mute_mask: Arc<AtomicU64>,
    ) -> Self {
        Self {
            playbin,
            queue_state,
            analysis_tx,
            analysis_pcm_token,
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
            buffering_active: false,
            seek_hold: None,
            last_seek_issued_at: None,
            last_seek_released_at: None,
            last_seek_target: None,
            pending_eos_track_switch,
            pending_gapless_duration: None,
            position_clock: SmoothedPlaybackClock::new(Instant::now()),
            staged_continuation_active,
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
        profile_eprintln!(
            "[gst-seek] requested_ms={} target_ms={} result={} cancelled_gapless={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} current={}",
            duration_ms_i128(requested),
            duration_ms_i128(target),
            if seek_result_ok { "ok" } else { "err" },
            if cancelled_gapless_advance { 1 } else { 0 },
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
        profile_eprintln!(
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
        let seek_trace = self.recent_seek_trace();
        let clock_trace = self.clock_trace();
        profile_eprintln!(
            "[gst-pos] prev_ms={} next_ms={} delta_ms={} jumped_backward={} mode={:?} accepted_ms={} seek_locked=0 seek_target_ms={} since_seek_ms={} since_seek_release_ms={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} pending_gapless_duration={} current={}",
            trace.previous,
            trace.current,
            trace.delta,
            if trace.jumped_backward { 1 } else { 0 },
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
            if self.pending_gapless_duration.is_some() { 1 } else { 0 },
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

    /// Advance the track token for a non-gapless transition (manual or
    /// cross-format).  Updates the shared PCM tap atomic so the analysis
    /// runtime accepts only chunks from the new track.
    fn advance_track_token(&mut self) -> u64 {
        self.track_token_counter += 1;
        let token = self.track_token_counter;
        self.analysis_pcm_token.store(token, Ordering::Relaxed);
        let _ = self.analysis_tx.send(AnalysisCommand::SetTrackToken(token));
        token
    }

    /// Advance the track token for a gapless transition.  Returns a
    /// unique token for waveform tracking but does NOT touch the shared
    /// PCM tap atomic — the audio stream is continuous and PCM chunks
    /// must keep flowing without interruption.
    fn advance_track_token_gapless(&mut self) -> u64 {
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
        self.switch_track(path.as_path(), &uri, force_play);
        self.buffering_active = false;
        self.pending_eos_track_switch
            .store(false, Ordering::Release);
        self.pending_gapless_duration = None;
        self.clear_staged_spectrogram();
        self.emit_track_changed(path, queue_index, kind, track_token);
        self.emit_snapshot();
    }

    fn stop_with_empty_queue(&mut self) {
        self.soft_mute();
        let _ = self.playbin.set_state(gst::State::Ready);
        self.startup_gain_ramp = false;
        self.startup_ramp_hold_until = None;
        self.buffering_active = false;
        self.pending_eos_track_switch
            .store(false, Ordering::Release);
        self.pending_gapless_duration = None;
        self.clear_staged_spectrogram();
        self.reset_channel_mute_state();
        self.snapshot.current = None;
        self.snapshot.current_queue_index = None;
        self.snapshot.current_bitrate_kbps = None;
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
        self.cancel_staged_spectrogram();
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
                // Same track stays current — restart spectrogram to
                // recover from possible wrong-file decode.
                self.cancel_staged_spectrogram();
                self.snapshot.current = Some(path);
            } else {
                // Track switches — switch_to_path calls
                // clear_staged_spectrogram internally.
                self.switch_to_path(
                    path,
                    current_index.unwrap_or(0),
                    TrackChangeKind::Manual,
                    false,
                );
                return;
            }
        } else {
            // Queue empties — stop_with_empty_queue calls
            // clear_staged_spectrogram internally.
            self.stop_with_empty_queue();
        }
        self.emit_snapshot();
    }

    fn move_queue_item(&mut self, from: usize, to: usize) {
        self.cancel_staged_spectrogram();
        let Some((repeat_mode, shuffle_enabled, current_index)) = (match self.queue_state.lock() {
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
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
            self.buffering_active = false;
            self.seek_hold = None;
            self.clear_recent_seek_trace();
            self.pending_eos_track_switch
                .store(false, Ordering::Release);
            self.clear_staged_spectrogram();
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

        // If about-to-finish has already advanced the queue past what the
        // UI considers the current track, revert the queue and reset the
        // pipeline so the seek applies to the correct (displayed) track.
        // Without this, seeking near the end of a track can land on the
        // next track's audio while the UI still shows the previous one.
        let cancelled_gapless_advance = self.cancel_pending_gapless_advance();
        if cancelled_gapless_advance {
            if let Some(ref path) = self.snapshot.current.clone() {
                if let Some(uri) = file_uri(path) {
                    self.soft_mute();
                    let _ = self.playbin.set_state(gst::State::Null);
                    let _ = self.playbin.set_state(gst::State::Ready);
                    self.playbin.set_property("uri", &uri);

                    // Transition to Paused first so the pipeline
                    // prerolls and can accept the seek.  For local
                    // files this is sub-millisecond.
                    let _ = self.playbin.set_state(gst::State::Paused);
                    let _ = self.playbin.state(Some(gst::ClockTime::from_mseconds(500)));

                    let seek_flags = seek_flags_for_path(self.snapshot.current.as_deref());
                    let seek_result_ok = self.playbin.seek_simple(seek_flags, target).is_ok();
                    #[cfg(not(feature = "profiling-logs"))]
                    let _ = seek_result_ok;

                    let was_playing = self.snapshot.state == PlaybackState::Playing;
                    if was_playing {
                        let _ = self.playbin.set_state(gst::State::Playing);
                        self.playbin.set_property("volume", 0.0_f64);
                        self.startup_gain_ramp = true;
                        self.startup_ramp_hold_until = if is_raw_surround_file(path) {
                            Some(Instant::now() + Duration::from_millis(80))
                        } else {
                            None
                        };
                    }

                    self.begin_seek_hold(requested);
                    #[cfg(feature = "profiling-logs")]
                    self.log_seek_event(pos, requested, true, seek_result_ok);
                    let _ = self.event_tx.send(PlaybackEvent::Seeked {
                        position: self.snapshot.position,
                    });
                    return;
                }
            }
        }

        let seek_flags = seek_flags_for_path(self.snapshot.current.as_deref());
        let seek_result_ok = self.playbin.seek_simple(seek_flags, target).is_ok();
        #[cfg(not(feature = "profiling-logs"))]
        let _ = seek_result_ok;
        self.begin_seek_hold(requested);
        #[cfg(feature = "profiling-logs")]
        self.log_seek_event(pos, requested, false, seek_result_ok);
        let _ = self.event_tx.send(PlaybackEvent::Seeked {
            position: self.snapshot.position,
        });
    }

    /// If the about-to-finish handler has advanced the queue past the
    /// track that the snapshot (UI) considers current, revert the queue
    /// index and clear all pending gapless/EOS state.  Returns `true`
    /// when the pipeline needs a full reset to cancel the pre-rolled
    /// next stream.
    fn cancel_pending_gapless_advance(&mut self) -> bool {
        let diverged = if let Ok(mut state) = self.queue_state.lock() {
            let queue_path = state.current();
            let snapshot_path = self.snapshot.current.as_ref();
            match (queue_path, snapshot_path) {
                (Some(qp), Some(sp)) if qp != *sp => {
                    if let Some(idx) = self.snapshot.current_queue_index {
                        state.set_current(idx);
                    }
                    true
                }
                _ => false,
            }
        } else {
            false
        };
        if diverged {
            self.pending_eos_track_switch
                .store(false, Ordering::Release);
            self.pending_gapless_duration = None;
            self.cancel_staged_spectrogram();
        }
        diverged
    }

    /// Cancel early continuation and restart the spectrogram session.
    /// Used when the current track stays playing but the gapless
    /// prediction is invalid (seek near EOF, queue mutation).
    fn cancel_staged_spectrogram(&mut self) {
        if self
            .staged_continuation_active
            .swap(false, Ordering::AcqRel)
        {
            let _ = self
                .analysis_tx
                .send(AnalysisCommand::CancelStagedContinuation);
        }
    }

    /// Clear early continuation without restarting.  Used when a
    /// `SetTrack` or stop follows immediately, superseding the worker.
    fn clear_staged_spectrogram(&mut self) {
        if self
            .staged_continuation_active
            .swap(false, Ordering::AcqRel)
        {
            let _ = self
                .analysis_tx
                .send(AnalysisCommand::ClearStagedContinuation);
        }
    }

    fn set_volume(&mut self, volume: f32) {
        self.target_volume = volume.clamp(0.0, 1.0);
        self.snapshot.volume = self.target_volume;
        self.emit_snapshot();
    }

    fn set_repeat_mode(&mut self, mode: RepeatMode) {
        self.cancel_staged_spectrogram();
        if let Ok(mut state) = self.queue_state.lock() {
            state.set_repeat_mode(mode);
            self.snapshot.repeat_mode = state.repeat_mode;
        } else {
            self.snapshot.repeat_mode = mode;
        }
        self.emit_snapshot();
    }

    fn set_shuffle(&mut self, enabled: bool) {
        self.cancel_staged_spectrogram();
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
        let (position_locked, seek_hold_changed, released_seek_sample) = self.poll_seek_hold();
        snapshot_changed |= seek_hold_changed;
        if !position_locked && self.snapshot.state != PlaybackState::Stopped {
            if let Some(pos) = self.playbin.query_position::<gst::ClockTime>() {
                let now = Instant::now();
                let previous_raw_pos = self
                    .position_clock
                    .last_raw_position()
                    .unwrap_or(self.snapshot.position);
                let raw_pos = Duration::from_nanos(pos.nseconds());
                // playbin3 pre-rolls the next stream ~2 s before the
                // current track ends.  Detect the actual stream switch by
                // a large backward position jump (the new stream starts
                // near zero while the old was near the end).
                let trace = gst_position_trace(previous_raw_pos, raw_pos);
                let jumped_backward = trace.jumped_backward;
                if jumped_backward {
                    // Commit the deferred duration from the pre-rolled
                    // stream now that the switch has actually happened.
                    if let Some(dur) = self.pending_gapless_duration.take() {
                        self.snapshot.duration = dur;
                        snapshot_changed = true;
                    }
                }
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
                        self.log_position_sample(trace, mode_before_sample, next_pos);
                    }
                }
            }
        }
        if self.snapshot.state != PlaybackState::Stopped || self.snapshot.duration == Duration::ZERO
        {
            if let Some(dur) = self.playbin.query_duration::<gst::ClockTime>() {
                let next_dur = Duration::from_nanos(dur.nseconds());
                if self.snapshot.duration != next_dur {
                    // playbin3 reports the next track's duration before
                    // the stream switch actually happens.  Defer the
                    // update until the position confirms the switch,
                    // unless we haven't committed any duration yet
                    // (initial track start).
                    if self.snapshot.duration == Duration::ZERO {
                        self.snapshot.duration = next_dur;
                        snapshot_changed = true;
                    } else {
                        self.pending_gapless_duration = Some(next_dur);
                    }
                }
            }
        }
        // Only check for same-format gapless handoff when we are NOT
        // waiting for EOS to drive a cross-format switch.
        if !self.pending_eos_track_switch.load(Ordering::Acquire) {
            if let Some((path, queue_index)) =
                maybe_emit_natural_handoff(&self.queue_state, &mut self.snapshot)
            {
                let track_token = self.advance_track_token_gapless();
                self.staged_continuation_active
                    .store(false, Ordering::Release);
                self.emit_track_changed(path, queue_index, TrackChangeKind::Gapless, track_token);
                snapshot_changed = true;
            }
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
        let repeat_mode = state.repeat_mode;
        let shuffle_enabled = state.shuffle_enabled;
        let next = state.next_manual();
        let current_index = state.current_index().unwrap_or(0);
        drop(state);
        self.set_queue_flags(repeat_mode, shuffle_enabled);
        if let Some(path) = next {
            self.switch_to_path(path, current_index, TrackChangeKind::Manual, true);
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
                self.playing_state_requested_at = None;
                self.buffering_active = false;
                self.seek_hold = None;
                self.position_clock.reset(Duration::ZERO, Instant::now());
                self.clear_recent_seek_trace();
                self.startup_gain_ramp = false;
                self.startup_ramp_hold_until = None;
                self.pending_gapless_duration = None;
                let _ = self.playbin.set_state(gst::State::Ready);
                super::stop_snapshot_at_terminal_eos(&mut self.snapshot);
                self.emit_snapshot();
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
    pcm_tx: Sender<AnalysisPcmChunk>,
    analysis_track_token: Arc<AtomicU64>,
) -> anyhow::Result<()> {
    gst::init().context("gst::init failed")?;
    register_raw_surround_typefinders();

    let playbin = gst::ElementFactory::make("playbin3")
        .build()
        .map_err(|_| anyhow!("failed to create playbin3"))?;
    configure_playbin_buffering(&playbin);

    let channel_mute_mask = Arc::new(AtomicU64::new(0));

    let analysis_sink = build_analysis_audio_sink(
        analysis_tx.clone(),
        pcm_tx,
        Arc::clone(&analysis_track_token),
        &channel_mute_mask,
    )?;
    playbin.set_property("audio-sink", &analysis_sink);

    // Strip leading partial-frame data and trailing APEv2 tags from raw
    // surround file sources so the AC3/DTS parser only receives clean,
    // sync-aligned audio.  Without this, the parser must scan for sync
    // words through garbage data, causing gaps during gapless transitions.
    install_raw_surround_source_probe(&playbin);

    let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));
    let pending_eos_track_switch = Arc::new(AtomicBool::new(false));
    let staged_continuation_active = Arc::new(AtomicBool::new(false));

    {
        let queue_state = Arc::clone(&queue_state);
        let pending_eos = Arc::clone(&pending_eos_track_switch);
        let analysis_tx = analysis_tx.clone();
        let staged_active = Arc::clone(&staged_continuation_active);
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
                // Tell analysis to stage spectrogram data for the next
                // track.  Runs on the analysis thread — the staging
                // thread will check format compatibility itself.
                let _ = analysis_tx
                    .send(AnalysisCommand::PrepareGaplessContinuation { path: new_path });
                staged_active.store(true, Ordering::Release);
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
        staged_continuation_active,
        channel_mute_mask,
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
                    if let Ok(sub) = buf.copy_region(gst::BufferCopyFlags::all(), skip..skip + tail)
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

    fn channel_labels_for_sample(&mut self, sample: &gst::Sample) -> Vec<SpectrogramChannelLabel> {
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
        // Precision loss is acceptable: len never exceeds audio buffer size, f32 RMS is sufficient.
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
                if cfg!(feature = "profiling-logs") {
                    let caps = caps_ev.caps();
                    eprintln!("[ferrous] audio sink caps change: {caps}");
                }
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
    channel_mute_mask: &Arc<AtomicU64>,
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
    let mut tap_state = AnalysisTapState::new(analysis_tx, pcm_tx, track_token, tap_chunk_samples);

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

    // Channel-mute probe: zeroes samples for muted channels in the output
    // branch.  The analysis branch is unaffected — spectrograms show all
    // channels regardless of mute state.
    install_channel_mute_probe(&output_capsfilter, channel_mute_mask);
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
    fn seek_hold_advances_visible_position_from_target() {
        let base = Instant::now();
        let mut clock = SmoothedPlaybackClock::new(base);
        clock.enter_seek_hold(Duration::from_secs_f64(12.0), base);

        let visible = clock.current_position(base + Duration::from_millis(120));
        assert!(
            visible > Duration::from_secs_f64(12.10),
            "seek hold should advance from target, got {:.6}",
            visible.as_secs_f64()
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

    /// When about-to-finish has advanced the queue (e.g. from A→B) but
    /// the snapshot still thinks A is current, seeking with a mid-track
    /// position must NOT trigger a false gapless handoff.  Reverting
    /// the queue index should make handoff detection silent again.
    #[test]
    fn seek_after_about_to_finish_reverts_queue_and_suppresses_handoff() {
        let a = PathBuf::from("/tmp/seek_revert_a.flac");
        let b = PathBuf::from("/tmp/seek_revert_b.flac");
        let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));
        {
            let mut q = queue_state.lock().unwrap();
            q.set_queue(vec![a.clone(), b.clone()]);
        }

        // Simulate about-to-finish advancing the queue from A to B.
        {
            let mut q = queue_state.lock().unwrap();
            let next = q.next_natural();
            assert_eq!(next.as_ref(), Some(&b));
            assert_eq!(q.current().as_ref(), Some(&b));
        }

        // Snapshot still thinks A is current (UI hasn't caught up).
        let mut snapshot = PlaybackSnapshot {
            state: PlaybackState::Playing,
            position: Duration::from_secs(30), // mid-track seek target
            duration: Duration::from_secs(180),
            current: Some(a.clone()),
            current_queue_index: Some(0),
            ..PlaybackSnapshot::default()
        };

        // With position > 2s, handoff detection should NOT fire even
        // though queue diverged — this is the bug scenario.
        let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot);
        assert_eq!(emitted, None, "handoff must not fire at mid-track position");
        assert_eq!(
            snapshot.current.as_ref(),
            Some(&a),
            "snapshot should remain on track A"
        );

        // Revert queue to match the snapshot (what cancel_pending_gapless_advance does).
        {
            let mut q = queue_state.lock().unwrap();
            q.set_current(0);
            assert_eq!(q.current().as_ref(), Some(&a));
        }

        // After revert, handoff detection should still be silent
        // (queue and snapshot agree on track A).
        let emitted = maybe_emit_natural_handoff(&queue_state, &mut snapshot);
        assert_eq!(emitted, None, "handoff must not fire after queue revert");
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

/// Integration test for MP3 files that have both ID3 and APE tags.
/// Reproduces the pipeline setup used by the real engine (custom
/// typefinders + source probe + analysis audio sink) to catch
/// `not-linked` regressions from GStreamer version bumps.
#[cfg(all(test, feature = "gst"))]
mod gst_integration_tests {
    use super::*;

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

    fn make_full_analysis_sink() -> gst::Bin {
        let bin = gst::Bin::new();
        let caps = gst::Caps::builder("audio/x-raw").build();
        let analysis_caps = gst::Caps::builder("audio/x-raw")
            .field("format", "F32LE")
            .field("layout", "interleaved")
            .field("rate", 44_100i32)
            .build();

        let input_cf = gst::ElementFactory::make("capsfilter")
            .property("caps", &caps)
            .build()
            .unwrap();
        let tee = gst::ElementFactory::make("tee").build().unwrap();

        // output branch
        let q_out = gst::ElementFactory::make("queue").build().unwrap();
        let conv_out = gst::ElementFactory::make("audioconvert").build().unwrap();
        let res_out = gst::ElementFactory::make("audioresample").build().unwrap();
        let cf_out = gst::ElementFactory::make("capsfilter")
            .property("caps", &caps)
            .build()
            .unwrap();
        let sink_out = gst::ElementFactory::make("fakesink")
            .property("sync", false)
            .build()
            .unwrap();

        // analysis branch
        let q_tap = gst::ElementFactory::make("queue")
            .property_from_str("leaky", "downstream")
            .property("max-size-buffers", 128u32)
            .property("max-size-bytes", 0u32)
            .property("max-size-time", 0u64)
            .build()
            .unwrap();
        let conv_tap = gst::ElementFactory::make("audioconvert").build().unwrap();
        let res_tap = gst::ElementFactory::make("audioresample").build().unwrap();
        let cf_tap = gst::ElementFactory::make("capsfilter")
            .property("caps", &analysis_caps)
            .build()
            .unwrap();
        let sink_tap = gst::ElementFactory::make("fakesink")
            .property("sync", false)
            .build()
            .unwrap();

        bin.add_many([
            &input_cf, &tee, &q_out, &conv_out, &res_out, &cf_out, &sink_out, &q_tap, &conv_tap,
            &res_tap, &cf_tap, &sink_tap,
        ])
        .unwrap();
        gst::Element::link_many([&input_cf, &tee]).unwrap();
        gst::Element::link_many([&q_out, &conv_out, &res_out, &cf_out, &sink_out]).unwrap();
        gst::Element::link_many([&q_tap, &conv_tap, &res_tap, &cf_tap, &sink_tap]).unwrap();
        link_tee_branch(&tee, &q_out, "out").unwrap();
        link_tee_branch(&tee, &q_tap, "tap").unwrap();

        let ghost = gst::GhostPad::with_target(&input_cf.static_pad("sink").unwrap()).unwrap();
        ghost.set_active(true).unwrap();
        bin.add_pad(&ghost).unwrap();
        bin
    }

    const TEST_MP3: &str =
        "/mnt/nassikka/Musiikki/Albumit/Coldplay/X&Y/Instrumental/02 - What If.mp3";

    /// Full app pipeline pattern: custom typefinders + source probe
    /// + full analysis sink.  Verifies that MP3 files with APE tags
    /// play correctly (regression test for apedemux rank demotion).
    #[test]
    fn playbin3_mp3_with_typefinders_and_full_sink() {
        let test_path = std::path::Path::new(TEST_MP3);
        if !test_path.exists() {
            eprintln!("skipping: test file not available");
            return;
        }

        gst::init().unwrap();
        register_raw_surround_typefinders();

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
    }
}
