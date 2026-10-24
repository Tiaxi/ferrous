use std::cell::RefCell;
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc, Condvar, Mutex, MutexGuard, OnceLock, PoisonError,
};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Context};
use crossbeam_channel::{bounded, unbounded, Receiver, Sender};
use gst::prelude::*;
use gstreamer as gst;
use gstreamer_pbutils as gst_pbutils;

use crate::analysis::AnalysisCommand;
#[cfg(feature = "profiling-logs")]
use crate::profile_logging::heartbeat_trace_enabled;
use crate::raw_audio::{
    configure_raw_surround_decoding, install_raw_surround_source_probe, is_dts_file,
    is_raw_surround_file,
};

use super::clock::{self, MonotonicClock, MonotonicTime};
use super::queue::{
    same_audio_extension, CancelledAdvance, CommittedAdvance, GaplessQueue, Handoff, ManualMove,
};
use super::{
    PlaybackCommand, PlaybackError, PlaybackErrorOutcome, PlaybackEvent, PlaybackSnapshot,
    PlaybackState, RepeatMode, TrackChangeKind,
};

/// Tracks that may fail in a row before playback stops. A track that plays
/// to its end, or a command that starts or navigates playback, ends a run
/// of failures.
const MAX_CONSECUTIVE_FAILURES: u32 = 5;

/// A plugin that the pipeline reported missing in the current run.
#[derive(Debug, Clone)]
struct MissingPlugin {
    /// The file whose input needs the plugin, when the message tells.
    file: Option<PathBuf>,
    /// Description such as "FLAC decoder".
    description: String,
}

#[cfg(any(test, feature = "profiling-logs"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GstPositionTrace {
    previous: i128,
    current: i128,
    delta: i128,
    jumped_backward: bool,
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

/// How closely a position read after a gapless switch must match the
/// previous track's timeline to have come from its stream.
const PREVIOUS_STREAM_TOLERANCE: Duration = Duration::from_millis(250);
/// How long after the successor's start the pipeline may still answer
/// position queries from the previous stream.
const GAPLESS_HANDOVER_WINDOW: Duration = Duration::from_secs(1);

/// The previous track's timeline at a gapless switch. Right after the
/// successor's stream starts, the pipeline can still answer a position
/// query from the previous stream, before the successor's segment applies;
/// such a position would show the successor at the end of the previous
/// track. The successor starts where the previous track ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GaplessHandover {
    /// The previous track's last position sample.
    previous_position: Duration,
    previous_sampled_at: MonotonicTime,
    /// When the previous track ends, which is when the successor starts.
    successor_starts_at: MonotonicTime,
}

impl GaplessHandover {
    /// The handover from the last sample of a track that advanced and
    /// lasts `duration`, read at `now`.
    fn after(
        previous_position: Duration,
        previous_sampled_at: MonotonicTime,
        duration: Duration,
        now: MonotonicTime,
    ) -> Self {
        let remaining = duration.saturating_sub(previous_position);
        Self {
            previous_position,
            previous_sampled_at,
            successor_starts_at: now.max(previous_sampled_at.saturating_add(remaining)),
        }
    }

    /// Whether `position`, read at `now`, is the previous track's: where
    /// its last sample says that track is now, rather than near the
    /// successor's start.
    fn is_previous_stream(&self, position: Duration, now: MonotonicTime) -> bool {
        let previous_now =
            self.previous_position + now.saturating_duration_since(self.previous_sampled_at);
        position > PREVIOUS_STREAM_TOLERANCE
            && position.abs_diff(previous_now) <= PREVIOUS_STREAM_TOLERANCE
    }

    /// Whether the pipeline has had long enough to report the successor.
    fn is_over(&self, now: MonotonicTime) -> bool {
        now > self
            .successor_starts_at
            .saturating_add(GAPLESS_HANDOVER_WINDOW)
    }
}

/// A position to seek the current track to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct SeekTarget {
    position: Duration,
    /// Serial of the newest `Seek` command that this seek carries out, or
    /// 0 for a seek the engine makes to restart the track where it plays.
    serial: u64,
}

/// Progress of a pipeline run that restarts the current track for a seek.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RunRestart {
    /// The pipeline must start a new run before it takes the queued seek,
    /// because it was given a successor that has been dropped since.
    Required,
    /// Waiting for the new run to preroll before it is given the seek.
    Prerolling,
    /// The new run was given the seek and waits for it to land.
    Seeking,
}

struct GstPlaybackRuntime {
    playbin: gst::Element,
    /// Shared with the about-to-finish callback, which hands a successor
    /// to playbin while it holds this lock. Lock order: this lock, then
    /// `stream_durations`. Apart from that handover, never call into
    /// `GStreamer` while holding either lock: playbin can wait for a
    /// streaming thread that waits for the lock.
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
    /// Number of `Seek` commands received, which numbers each seek.
    seek_requests: u64,
    /// The newest seek that the pipeline has not been given yet.
    queued_seek: Option<SeekTarget>,
    /// The seek the pipeline was given, until it reports `async-done`.
    seek_in_flight: Option<SeekTarget>,
    /// A new pipeline run for the queued seek. The run prerolls in Paused
    /// and plays once the seek has landed, if playback is playing then.
    restart: Option<RunRestart>,
    /// Shared with the parser probes, which hold the end of each stream
    /// while a seek is prepared.
    stream_ends: Arc<Mutex<StreamEndHold>>,
    last_seek_issued_at: Option<Instant>,
    last_seek_released_at: Option<Instant>,
    last_seek_target: Option<Duration>,
    /// Time stamps the position samples in the snapshot.
    clock: MonotonicClock,
    /// Set after a gapless switch while the pipeline may still report the
    /// previous track's position.
    gapless_handover: Option<GaplessHandover>,
    /// The next `stream-start` belongs to the track the pipeline was last
    /// started with, not to a staged successor.
    awaiting_run_stream_start: bool,
    /// Group of the last `stream-start`. Chained streams within one file
    /// share their group; each file the pipeline starts gets a new one.
    last_stream_group: Option<gst::GroupId>,
    /// Shared with the about-to-finish callback and the parser probes.
    stream_durations: Arc<Mutex<StreamDurations>>,
    /// Set when the pipeline is set playing, or a restarted run prerolls.
    /// Cleared when the bus handler sees `StateChanged(_, Playing)` for
    /// the playbin.  Used by `poll()` to detect stuck state transitions.
    /// A seek in flight holds the pipeline back from Playing, so the wait
    /// starts over once the seek has landed.
    playing_state_requested_at: Option<Instant>,
    /// How long the pipeline may take to reach Playing:
    /// `PLAYING_STATE_TIMEOUT`, which tests shorten.
    playing_state_timeout: Duration,
    channel_mute_mask: Arc<AtomicU64>,
    solo_pre_mask: Option<u64>,
    soloed_channel: Option<u8>,
    /// Tracks that failed since a track last played to its end or a
    /// command started or navigated playback.
    consecutive_failures: u32,
    /// The first missing plugin reported in the current pipeline run.
    missing_plugin: Option<MissingPlugin>,
    /// Shared with the about-to-finish callback and the probes on the
    /// outputs of the pipeline's sources; see `InputSwitches`.
    input_switches: Arc<InputSwitches>,
    #[cfg(test)]
    about_to_finish: Arc<AboutToFinishCalls>,
}

pub fn spawn_engine(
    analysis_tx: Sender<AnalysisCommand>,
    event_tx: Sender<PlaybackEvent>,
    failure: &Arc<OnceLock<String>>,
) -> Sender<PlaybackCommand> {
    spawn_engine_with_output(analysis_tx, event_tx, failure, build_output_sink)
}

/// Spawn the engine with the audio output that `build_output` creates
/// after `GStreamer` is initialized. When the engine cannot start, the
/// thread records why in `failure`, reports it, and exits, so that later
/// commands fail.
fn spawn_engine_with_output(
    analysis_tx: Sender<AnalysisCommand>,
    event_tx: Sender<PlaybackEvent>,
    failure: &Arc<OnceLock<String>>,
    build_output: fn() -> anyhow::Result<gst::Element>,
) -> Sender<PlaybackCommand> {
    let (cmd_tx, cmd_rx) = unbounded::<PlaybackCommand>();
    let failure = Arc::clone(failure);

    let spawned = std::thread::Builder::new()
        .name("ferrous-playback-gst".to_string())
        .spawn(move || {
            match start_gst_engine(event_tx.clone(), analysis_tx, build_output) {
                Ok((runtime, bus)) => run_gst_engine(runtime, &bus, &cmd_rx),
                Err(err) => {
                    // Commands fail from now on, before the failure is
                    // reported.
                    drop(cmd_rx);
                    let detail = format!("{err:#}");
                    eprintln!("[ferrous] gstreamer playback engine failed: {detail}");
                    let _ = failure.set(detail.clone());
                    let _ = event_tx.send(PlaybackEvent::Error(PlaybackError {
                        path: None,
                        detail,
                        outcome: PlaybackErrorOutcome::EngineUnavailable,
                    }));
                }
            }
        });
    if let Err(err) = spawned {
        // The command receiver was dropped with the closure, so commands
        // report the engine as unavailable.
        eprintln!("[ferrous] failed to start the playback thread: {err}");
    }

    cmd_tx
}

impl GstPlaybackRuntime {
    fn command_wait_timeout(&self) -> Duration {
        if self.volume_ramp_pending() {
            VOLUME_RAMP_TICK
        } else if self.snapshot.state == PlaybackState::Playing
            || self.buffering_active
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
        stream_ends: Arc<Mutex<StreamEndHold>>,
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
            seek_requests: 0,
            queued_seek: None,
            seek_in_flight: None,
            restart: None,
            stream_ends,
            last_seek_issued_at: None,
            last_seek_released_at: None,
            last_seek_target: None,
            clock: clock::system_clock(),
            gapless_handover: None,
            awaiting_run_stream_start: true,
            last_stream_group: None,
            stream_durations,
            playing_state_requested_at: None,
            playing_state_timeout: Self::PLAYING_STATE_TIMEOUT,
            channel_mute_mask,
            solo_pre_mask: None,
            soloed_channel: None,
            consecutive_failures: 0,
            missing_plugin: None,
            input_switches: Arc::default(),
            #[cfg(test)]
            about_to_finish: Arc::default(),
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

    /// Whether a seek waits for the pipeline. Positions are not sampled
    /// meanwhile: until the seek lands they describe neither the old nor
    /// the new position.
    fn seek_pending(&self) -> bool {
        self.queued_seek.is_some() || self.seek_in_flight.is_some()
    }

    /// Whether the pipeline runs a restart that waits for its seek before
    /// it may play.
    fn restarting(&self) -> bool {
        matches!(
            self.restart,
            Some(RunRestart::Prerolling | RunRestart::Seeking)
        )
    }

    /// Whether the pipeline position advances in real time: playback plays,
    /// the pipeline has reached Playing and is not leaving it, and no seek,
    /// restart, or buffering holds it.
    fn pipeline_advancing(&self) -> bool {
        self.snapshot.state == PlaybackState::Playing
            && !self.seek_pending()
            && !self.restarting()
            && !self.buffering_active
            && self.playbin.current_state() == gst::State::Playing
            && self.playbin.pending_state() == gst::State::VoidPending
    }

    /// Report that the pipeline is at `position` now, and whether it
    /// advances from there. A position that stands still keeps its time
    /// stamp. Returns whether the snapshot changed.
    fn set_position_sample(&mut self, position: Duration, advancing: bool) -> bool {
        let now = (self.clock)();
        self.set_position_sample_at(position, now, advancing)
    }

    /// [`Self::set_position_sample`] for a position read at `sampled_at`.
    fn set_position_sample_at(
        &mut self,
        position: Duration,
        sampled_at: MonotonicTime,
        advancing: bool,
    ) -> bool {
        if !advancing && !self.snapshot.position_advancing && self.snapshot.position == position {
            return false;
        }
        // A stream that has not started keeps the time it starts at: the
        // pipeline reports its start until then.
        if advancing
            && self.snapshot.position_advancing
            && position.is_zero()
            && self.snapshot.position.is_zero()
            && sampled_at < self.snapshot.position_sampled_at
        {
            return false;
        }
        self.snapshot.position = position;
        self.snapshot.position_sampled_at = sampled_at;
        self.snapshot.position_advancing = advancing;
        true
    }

    /// Read the pipeline position into the snapshot. A pipeline that cannot
    /// tell where it is keeps the last position, which no longer advances.
    /// Returns whether the snapshot changed.
    fn sample_pipeline_position(&mut self) -> bool {
        let Some(position) = self.playbin.query_position::<gst::ClockTime>() else {
            return self.set_position_sample(self.snapshot.position, false);
        };
        let position = Duration::from_nanos(position.nseconds());
        let now = (self.clock)();
        let advancing = self.pipeline_advancing();
        self.apply_position_read(position, now, advancing)
    }

    /// Report a pipeline position read at `now`, while the pipeline
    /// `advancing` or not. Returns whether the snapshot changed.
    ///
    /// Right after a gapless switch, a reading on the previous track's
    /// timeline is not the current track's position. A position query does
    /// not tell which stream's segment answered it, so the reading's value
    /// decides, which cannot tell the streams apart after a track shorter
    /// than [`PREVIOUS_STREAM_TOLERANCE`]. Such a mistake stays harmless: a
    /// rejected reading only stands in for the position the current sample
    /// has reached, which is what the UI shows, and whether the position
    /// advances is always taken from the pipeline. The handover ends as
    /// soon as the pipeline reports the successor past its start, after
    /// which the previous stream's segment no longer applies.
    fn apply_position_read(
        &mut self,
        position: Duration,
        now: MonotonicTime,
        advancing: bool,
    ) -> bool {
        if let Some(handover) = self.gapless_handover {
            if handover.is_over(now) {
                self.gapless_handover = None;
            } else if handover.is_previous_stream(position, now) {
                // While the position advances, the sample from the switch
                // still holds; otherwise it stops where that sample has
                // reached.
                if advancing && self.snapshot.position_advancing {
                    return false;
                }
                let reached = self.sampled_position_at(now);
                return self.set_position_sample_at(reached, now, advancing);
            } else if !position.is_zero() {
                self.gapless_handover = None;
            }
        }
        self.set_position_sample_at(position, now, advancing)
    }

    /// Where the snapshot's position sample has reached at `now`.
    fn sampled_position_at(&self, now: MonotonicTime) -> Duration {
        let mut position = self.snapshot.position;
        if self.snapshot.position_advancing {
            position += now.saturating_duration_since(self.snapshot.position_sampled_at);
        }
        if self.snapshot.duration.is_zero() {
            position
        } else {
            position.min(self.snapshot.duration)
        }
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
    fn log_position_sample(&self, trace: GstPositionTrace) {
        if !heartbeat_trace_enabled() {
            return;
        }
        let seek_trace = self.recent_seek_trace();
        let clock_trace = self.clock_trace();
        eprintln!(
            "[gst-pos] prev_ms={} next_ms={} delta_ms={} jumped_backward={} advancing={} sampled_at_ns={} seek_target_ms={} since_seek_ms={} since_seek_release_ms={} current_state={:?} pending_state={:?} running_time_ms={} clock_time_ms={} base_time_ms={} successor_handed={} current={}",
            trace.previous,
            trace.current,
            trace.delta,
            i32::from(trace.jumped_backward),
            i32::from(self.snapshot.position_advancing),
            self.snapshot.position_sampled_at.as_nanos(),
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
        // Positions from now on belong to another run or position.
        self.gapless_handover = None;
        self.mute();
        let _ = self.set_pipeline_state(gst::State::Ready);
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
        self.set_position_sample(Duration::ZERO, false);
        self.snapshot.duration = Duration::ZERO;
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

    /// Apply a burst of Next (`true`) and Previous presses: move once per
    /// press, and start only the track the moves end on.
    fn navigate(&mut self, moves: &[bool]) {
        let Ok(mut state) = self.queue_state.lock() else {
            return;
        };
        let repeat_mode = state.repeat_mode();
        let shuffle_enabled = state.shuffle_enabled();
        let resume_from_pause = self.snapshot.state == PlaybackState::Paused;
        let mut target: Option<ManualMove> = None;
        for &next in moves {
            let step = if next {
                state.next_manual()
            } else {
                state.previous_manual()
            };
            if let Some(mut step) = step {
                step.cancelled = CancelledAdvance::merge(
                    target.as_ref().and_then(|target| target.cancelled),
                    step.cancelled,
                );
                target = Some(step);
            }
        }
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
        if let Some(path) = restart_path.as_deref() {
            if let Some(uri) = file_uri(path) {
                self.set_run_uri(path, &uri);
            }
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
        if self.restarting() {
            // The restarted run would play from its start; it plays once
            // its seek has landed.
            self.snapshot.state = PlaybackState::Playing;
            self.emit_snapshot();
            return;
        }
        if self.set_pipeline_state(gst::State::Playing).is_ok() {
            self.playing_state_requested_at = Some(Instant::now());
            if was_stopped {
                // Re-assert mute after state transition to close the race
                // window where new internal elements may not have volume=0.
                self.playbin.set_property("volume", 0.0_f64);
            }
            self.snapshot.state = PlaybackState::Playing;
            // The position advances once the pipeline has reached Playing.
            self.sample_pipeline_position();
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
        if self.restarting() {
            // The restarted run stays in Paused until its seek has landed.
            self.snapshot.state = PlaybackState::Paused;
            self.emit_snapshot();
            return;
        }
        self.playing_state_requested_at = None;
        if self.set_pipeline_state(gst::State::Paused).is_ok() {
            self.snapshot.state = PlaybackState::Paused;
            // Where playback paused, rather than where it was last polled.
            self.sample_pipeline_position();
            self.emit_snapshot();
        }
    }

    fn stop(&mut self) {
        self.mute();
        self.playing_state_requested_at = None;
        if self.set_pipeline_state(gst::State::Ready).is_ok() {
            self.reset_channel_mute_state();
            self.enter_stopped();
            self.emit_snapshot();
        }
    }

    /// Account for a pipeline that was stopped in Ready or Null: the current
    /// track stays current, and Play starts it from the beginning.
    fn enter_stopped(&mut self) {
        // Positions from now on belong to another run or position.
        self.gapless_handover = None;
        self.playing_state_requested_at = None;
        self.begin_pipeline_run(false);
        // The pipeline may have been given the successor. Play must
        // start the stopped track again.
        if let Some(path) = self.snapshot.current.as_deref() {
            if let Some(uri) = file_uri(path) {
                self.set_run_uri(path, &uri);
            }
        }
        self.startup_gain_ramp = false;
        self.startup_ramp_hold_until = None;
        self.buffering_active = false;
        self.clear_recent_seek_trace();
        self.snapshot.state = PlaybackState::Stopped;
        self.set_position_sample(Duration::ZERO, false);
        self.snapshot.current_queue_index = None;
        self.snapshot.current_bitrate_kbps = None;
    }

    /// Handle `requests` Seek commands in a row, of which only the last
    /// one, to `position`, is carried out. A newer seek replaces one that
    /// waits for the pipeline, so a burst of seeks lands only at its last
    /// target.
    fn seek(&mut self, position: Duration, requests: u64) {
        self.seek_requests = self.seek_requests.saturating_add(requests);
        let serial = self.seek_requests;
        if self.snapshot.current.is_none() || self.snapshot.state == PlaybackState::Stopped {
            // Play starts a stopped track from its beginning, so a stopped
            // track is not seekable.
            self.snapshot.seek_serial = serial;
            self.emit_snapshot();
            return;
        }
        let position = if self.snapshot.duration.is_zero() {
            position
        } else {
            position.min(self.snapshot.duration)
        };
        self.queue_seek(SeekTarget { position, serial });
    }

    /// Queue `seek` for the pipeline, replacing a seek that waits.
    fn queue_seek(&mut self, seek: SeekTarget) {
        // Positions from now on belong to another run or position.
        self.gapless_handover = None;
        self.queued_seek = Some(seek);
        // Positions are not sampled until the seek lands, and the position
        // does not advance meanwhile.
        if self.snapshot.position_advancing {
            let position = self
                .playbin
                .query_position::<gst::ClockTime>()
                .map_or(self.snapshot.position, |position| {
                    Duration::from_nanos(position.nseconds())
                });
            self.set_position_sample(position, false);
            self.emit_snapshot();
        }
        self.issue_queued_seek();
    }

    /// Give the pipeline the queued seek once it can take it: when it has
    /// prerolled, no other seek is in flight, and it has no successor that
    /// the seek would reach. Called again when the pipeline reports
    /// `async-done`.
    fn issue_queued_seek(&mut self) {
        // A run that is restarted need not have prerolled, and a pipeline
        // that a failure stopped never will.
        if self.restart == Some(RunRestart::Required) || self.pipeline_prerolled() {
            self.give_queued_seek();
        }
    }

    /// Give the queued seek to the pipeline, which has prerolled, or
    /// restart the run for it.
    fn give_queued_seek(&mut self) {
        if self.seek_in_flight.is_some() || self.restart == Some(RunRestart::Prerolling) {
            return;
        }
        let Some(seek) = self.queued_seek else {
            return;
        };
        // playbin3 applies a seek to a successor it was given, and it asks
        // for the successor of a stream it has read to the end only once.
        // Until the seek has been applied, successors are held back and so
        // is the end of every stream, so that the pipeline asks for the
        // successor of the current track after the seek.
        self.hold_handoffs();
        self.hold_stream_ends();
        let cancelled = self.cancel_pending_advance(true);
        if self.restart == Some(RunRestart::Required)
            || cancelled.is_some_and(|cancelled| cancelled.handed_to_pipeline)
        {
            self.restart_for_seek();
            return;
        }
        self.queued_seek = None;
        let applied = self
            .playbin
            .seek_simple(
                seek_flags_for_path(self.snapshot.current.as_deref()),
                clock_time(seek.position),
            )
            .is_ok();
        self.release_handoffs();
        self.release_stream_ends();
        #[cfg(feature = "profiling-logs")]
        self.log_seek_event(seek.position, seek.position, self.restarting(), applied);
        self.last_seek_issued_at = Some(Instant::now());
        self.last_seek_released_at = None;
        self.last_seek_target = Some(seek.position);
        if applied {
            self.seek_in_flight = Some(seek);
        } else {
            self.finish_seek(seek, None);
        }
    }

    /// Whether the pipeline has prerolled in this run, so that it takes a
    /// seek, without waiting for it. Its state becomes Paused before it
    /// posts the `async-done` of its preroll, and stays Paused or Playing
    /// while it goes on to Playing or prerolls again after a seek.
    fn pipeline_prerolled(&self) -> bool {
        let (_, current, _) = self.playbin.state(Some(gst::ClockTime::ZERO));
        current >= gst::State::Paused
    }

    /// The pipeline prerolled after a state change or a seek. It may still
    /// be on its way to Playing, which does not keep it from taking a seek.
    fn handle_async_done(&mut self) {
        if self.restart == Some(RunRestart::Prerolling) {
            // The restarted run takes its seek now.
            self.restart = Some(RunRestart::Seeking);
            self.give_queued_seek();
            return;
        }
        let Some(seek) = self.seek_in_flight.take() else {
            // Give the pipeline a seek that waited for it to preroll.
            self.give_queued_seek();
            return;
        };
        if self.queued_seek.is_some() {
            // A newer seek replaces this one before it is reported.
            self.give_queued_seek();
            return;
        }
        let landing = self
            .playbin
            .query_position::<gst::ClockTime>()
            .map(|position| Duration::from_nanos(position.nseconds()));
        self.finish_seek(seek, Some(landing.unwrap_or(seek.position)));
    }

    /// Account for `seek`, which landed at `landing`, or failed with
    /// `None`. A seek of the user is reported, and a restarted run plays
    /// again if playback is playing.
    fn finish_seek(&mut self, seek: SeekTarget, landing: Option<Duration>) {
        let now = Instant::now();
        self.last_seek_released_at = Some(now);
        #[cfg(feature = "profiling-logs")]
        self.log_seek_hold_release(seek.position);
        if let Some(landing) = landing {
            let advancing = self.pipeline_advancing();
            self.set_position_sample(landing, advancing);
        } else {
            // Playback goes on where the pipeline is, which is the start of
            // the track for a restarted run.
            self.sample_pipeline_position();
        }
        if self.playing_state_requested_at.is_some() {
            // The pipeline goes on to Playing now that the seek is done.
            self.playing_state_requested_at = Some(now);
        }
        if seek.serial != 0 {
            self.snapshot.seek_serial = self.snapshot.seek_serial.max(seek.serial);
            if let Some(landing) = landing {
                self.snapshot.seek_landed_serial = seek.serial;
                let _ = self
                    .event_tx
                    .send(PlaybackEvent::Seeked { position: landing });
            }
        }
        if self.restarting() {
            self.restart = None;
            if self.snapshot.state == PlaybackState::Playing {
                self.start_playing_muted();
            } else {
                self.playing_state_requested_at = None;
            }
        }
        self.emit_snapshot();
    }

    /// Drop seeks that wait for the pipeline, because the track changed or
    /// playback stopped. Every seek received so far is done with.
    fn abandon_seeks(&mut self) {
        self.queued_seek = None;
        self.seek_in_flight = None;
        self.restart = None;
        self.release_handoffs();
        self.release_stream_ends();
        self.snapshot.seek_serial = self.seek_requests;
    }

    /// Drop every staged successor so the committed track stays current.
    /// `current_continues` tells analysis whether the committed track
    /// keeps playing, so a prepared spectrogram continuation is restarted
    /// rather than superseded.
    ///
    /// When the result reports a successor handed to the pipeline, the
    /// caller must either restart the committed track with
    /// `restart_at_current_position` or switch tracks before playback
    /// continues.
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
        if cancelled.is_some_and(|cancelled| cancelled.handed_to_pipeline) {
            self.restart_at_current_position();
        }
    }

    /// Restart the committed track in a new pipeline run without the
    /// successors the pipeline was given: where it plays now, or at the
    /// target of a seek that waits.
    fn restart_at_current_position(&mut self) {
        if self.restart != Some(RunRestart::Prerolling) {
            // A run that prerolls has not been given a successor.
            self.restart = Some(RunRestart::Required);
        }
        if let Some(seek) = self.seek_in_flight.take() {
            // The old run's seek is redone in the new run.
            self.queued_seek.get_or_insert(seek);
        }
        if self.queued_seek.is_some() {
            self.issue_queued_seek();
            return;
        }
        let position = self
            .playbin
            .query_position::<gst::ClockTime>()
            .map_or(self.snapshot.position, |position| {
                Duration::from_nanos(position.nseconds())
            });
        self.queue_seek(SeekTarget {
            position,
            serial: 0,
        });
    }

    /// Start a new pipeline run of the current track that prerolls in
    /// Paused, without the successors the old run was given. Once it has
    /// prerolled, it takes the queued seek, and it plays once that seek has
    /// landed. Stream ends stay held until then; see `issue_queued_seek`.
    fn restart_for_seek(&mut self) {
        let Some((path, uri)) = self
            .snapshot
            .current
            .clone()
            .and_then(|path| file_uri(&path).map(|uri| (path, uri)))
        else {
            self.abandon_seeks();
            return;
        };
        self.mute();
        let _ = self.set_pipeline_state(gst::State::Null);
        let _ = self.set_pipeline_state(gst::State::Ready);
        // Also drops a successor staged while the old run was stopping.
        self.begin_pipeline_run(true);
        self.set_run_uri(&path, &uri);
        self.restart = Some(RunRestart::Prerolling);
        // A run that never prerolls is reported like one that never plays.
        // The seek it then takes is not timed.
        self.playing_state_requested_at = Some(Instant::now());
        // A failure posts an error, which ends the restart.
        if matches!(
            self.set_pipeline_state(gst::State::Paused),
            Ok(gst::StateChangeSuccess::Success | gst::StateChangeSuccess::NoPreroll)
        ) {
            // Prerolled at once, so no async-done follows.
            self.handle_async_done();
        }
    }

    /// Set the pipeline playing from silence, with the fade-in of a track
    /// start.
    fn start_playing_muted(&mut self) {
        let _ = self.set_pipeline_state(gst::State::Playing);
        self.playing_state_requested_at = Some(Instant::now());
        self.applied_volume = 0.0;
        self.playbin.set_property("volume", 0.0_f64);
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

    fn hold_stream_ends(&self) {
        if let Ok(mut stream_ends) = self.stream_ends.lock() {
            stream_ends.hold();
        }
    }

    fn release_stream_ends(&self) {
        if let Ok(mut stream_ends) = self.stream_ends.lock() {
            stream_ends.release();
        }
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

    /// Change the state of the pipeline, which activates the sources of a
    /// new run (see `activate_sources`), or ends the run at Ready or Null
    /// (see `InputSwitches::end_run`).
    fn set_pipeline_state(
        &self,
        state: gst::State,
    ) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        let change = || activate_sources(|| self.playbin.set_state(state));
        if state <= gst::State::Ready {
            self.input_switches.end_run(change)
        } else {
            change()
        }
    }

    /// Give the stopped pipeline the current track at `path`, which starts
    /// the next run.
    fn set_run_uri(&self, path: &Path, uri: &str) {
        if let Ok(mut durations) = self.stream_durations.lock() {
            durations.start_run(path.to_path_buf());
        }
        self.playbin.set_property("uri", uri);
    }

    /// Account for a new pipeline run after the pipeline was stopped:
    /// discard the previous run's bus messages, streams, and staged
    /// successors. Unless the current track continues in the new run,
    /// also drop the seeks that wait.
    fn begin_pipeline_run(&mut self, current_continues: bool) {
        if let Some(bus) = self.playbin.bus() {
            bus.set_flushing(true);
            bus.set_flushing(false);
        }
        if !current_continues {
            self.abandon_seeks();
        }
        self.awaiting_run_stream_start = true;
        self.last_stream_group = None;
        self.missing_plugin = None;
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
        let committed = self.queue_state.lock().ok().and_then(|mut queue| {
            let committed = queue.commit_gapless()?;
            // Under the queue lock, so that the streams stay in step with
            // the successors that about-to-finish hands over.
            if let Ok(mut durations) = self.stream_durations.lock() {
                durations.commit();
            }
            Some(committed)
        });
        if let Some(committed) = committed {
            self.commit_gapless_successor(committed);
            // The previous track played to its end.
            self.reset_failures();
        }
    }

    fn commit_gapless_successor(&mut self, committed: CommittedAdvance) {
        let track_token = self.advance_track_token();
        let mut position = self
            .playbin
            .query_position::<gst::ClockTime>()
            .map_or(Duration::ZERO, |position| {
                Duration::from_nanos(position.nseconds())
            });
        let mut sampled_at = (self.clock)();
        // The successor's stream starts while the end of the previous track
        // is still to be heard, and its position stays at its start until
        // then, if the pipeline does not still report the previous track's.
        self.gapless_handover = self.snapshot.position_advancing.then(|| {
            GaplessHandover::after(
                self.snapshot.position,
                self.snapshot.position_sampled_at,
                self.snapshot.duration,
                sampled_at,
            )
        });
        if let Some(handover) = self.gapless_handover {
            if handover.is_previous_stream(position, sampled_at) {
                position = Duration::ZERO;
            }
            if position.is_zero() {
                sampled_at = handover.successor_starts_at;
            }
        }
        // A seek for the previous track no longer applies.
        self.abandon_seeks();
        self.clear_recent_seek_trace();
        self.snapshot.current = Some(committed.path.clone());
        self.snapshot.current_queue_index = Some(committed.index);
        self.snapshot.track_token = track_token;
        let advancing = self.pipeline_advancing();
        self.set_position_sample_at(position, sampled_at, advancing);
        self.snapshot.duration = self.current_duration().unwrap_or(Duration::ZERO);
        self.snapshot.current_bitrate_kbps = None;
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
        let reported = || self.stream_durations.lock().ok()?.current();
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
            && self.startup_ramp_hold_until.is_none()
        {
            return;
        }

        let mut snapshot_changed = false;
        if !self.seek_pending() && self.snapshot.state != PlaybackState::Stopped {
            #[cfg(feature = "profiling-logs")]
            let previous = self.snapshot.position;
            snapshot_changed |= self.sample_pipeline_position();
            #[cfg(feature = "profiling-logs")]
            if previous != self.snapshot.position {
                self.log_position_sample(gst_position_trace(previous, self.snapshot.position));
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
        // A seek can take long, such as an accurate seek in a long file
        // without an index, and the pipeline plays only once it has landed.
        // A seek that fails posts an error.
        if self.seek_in_flight.is_some() || requested_at.elapsed() < self.playing_state_timeout {
            return false;
        }
        let current_path = self.snapshot.current.as_ref().map_or("?", |p| {
            p.file_name().unwrap_or_default().to_str().unwrap_or("?")
        });
        eprintln!(
            "[ferrous] pipeline stuck: Playing state not confirmed within {}s for {current_path}, advancing",
            self.playing_state_timeout.as_secs(),
        );
        let (file, detail) = match self.missing_plugin.take() {
            Some(missing) => (missing.file, missing_plugin_detail(&missing.description)),
            None => (
                None,
                format!(
                    "playback did not start within {} s",
                    self.playing_state_timeout.as_secs()
                ),
            ),
        };
        self.fail_track(file, detail, false);
        true
    }

    /// Forget earlier failures: a track played to its end, or a command
    /// started or navigated playback.
    fn reset_failures(&mut self) {
        self.consecutive_failures = 0;
        if let Ok(mut queue) = self.queue_state.lock() {
            queue.clear_failed();
        }
    }

    fn report_failure(&self, path: Option<PathBuf>, detail: String, outcome: PlaybackErrorOutcome) {
        let _ = self.event_tx.send(PlaybackEvent::Error(PlaybackError {
            path,
            detail,
            outcome,
        }));
    }

    /// Handle a track that cannot be played. `file` is the path that the
    /// pipeline was given for the input the failure came from, when known,
    /// and `detail` describes the failure. `not_found` tells that the
    /// pipeline found no file at that path.
    ///
    /// A failed successor is dropped and the current track plays on from
    /// where it is. A failed current track is skipped. Either way the failed
    /// track is not tried again at its current path until the failures end;
    /// after `MAX_CONSECUTIVE_FAILURES` failures in a row, playback stops.
    fn fail_track(&mut self, file: Option<PathBuf>, detail: String, not_found: bool) {
        self.playing_state_requested_at = None;
        self.consecutive_failures = self.consecutive_failures.saturating_add(1);
        // The input's place in the run identifies the track, because a
        // rename since the pipeline was given `file` can have moved that
        // path to another track. An unknown input is the committed track's.
        let position = file
            .as_deref()
            .and_then(|file| self.stream_durations.lock().ok()?.position(file))
            .unwrap_or(0);
        let successor_failed = position > 0;
        let failed = self
            .queue_state
            .lock()
            .ok()
            .and_then(|mut queue| {
                let path = queue.path_in_pipeline(position)?;
                // The pipeline was given an old path of a renamed file and
                // found nothing there, so the file is tried again at its
                // new path.
                let stale_path = not_found && file.as_ref().is_some_and(|file| *file != path);
                if !stale_path {
                    queue.mark_failed(path.clone());
                }
                Some(path)
            })
            .or(file);

        if self.consecutive_failures >= MAX_CONSECUTIVE_FAILURES {
            self.stop_after_failure();
            let outcome = PlaybackErrorOutcome::StoppedAfterFailures(self.consecutive_failures);
            self.report_failure(failed, detail, outcome);
            self.emit_snapshot();
            return;
        }
        if successor_failed {
            // The pipeline stops at the end of the current track when its
            // successor fails, so restart the current track without it.
            let _ = self.cancel_pending_advance(true);
            self.restart_at_current_position();
            self.report_failure(failed, detail, PlaybackErrorOutcome::Skipped);
            self.emit_snapshot();
            return;
        }
        let next = self
            .queue_state
            .lock()
            .ok()
            .and_then(|mut queue| queue.skip_failed_current());
        if let Some(next) = next {
            self.report_failure(failed, detail, PlaybackErrorOutcome::Skipped);
            self.release_staged_spectrogram(next.cancelled, false);
            // Keeps a paused player paused.
            self.switch_to_path(next.path, next.index, TrackChangeKind::Natural, false);
        } else {
            self.stop_after_failure();
            self.report_failure(failed, detail, PlaybackErrorOutcome::Stopped);
            self.emit_snapshot();
        }
    }

    /// Tear down the faulted pipeline and stop on the current track.
    fn stop_after_failure(&mut self) {
        self.mute();
        let _ = self.set_pipeline_state(gst::State::Null);
        let _ = self.set_pipeline_state(gst::State::Ready);
        self.enter_stopped();
    }

    fn apply_step(&mut self, step: EngineStep) {
        match step {
            EngineStep::Command(cmd) => self.apply_command(cmd),
            EngineStep::Seek { position, requests } => self.seek(position, requests),
            EngineStep::Navigate(moves) => {
                self.reset_failures();
                self.navigate(&moves);
            }
        }
    }

    fn apply_command(&mut self, cmd: PlaybackCommand) {
        if matches!(
            cmd,
            PlaybackCommand::LoadQueue(_)
                | PlaybackCommand::ClearQueue
                | PlaybackCommand::PlayAt(_)
                | PlaybackCommand::Next
                | PlaybackCommand::Previous
                | PlaybackCommand::Play
                | PlaybackCommand::Stop
        ) {
            self.reset_failures();
        }
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
            PlaybackCommand::Next => self.navigate(&[true]),
            PlaybackCommand::Previous => self.navigate(&[false]),
            PlaybackCommand::Play => self.play(),
            PlaybackCommand::Pause => self.pause(),
            PlaybackCommand::Stop => self.stop(),
            PlaybackCommand::Seek(pos) => self.seek(pos, 1),
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
                        let _ = self.set_pipeline_state(gst::State::Paused);
                        self.buffering_active = true;
                    }
                } else if self.buffering_active {
                    self.buffering_active = false;
                    if self.snapshot.state == PlaybackState::Playing {
                        let _ = self.set_pipeline_state(gst::State::Playing);
                    }
                }
            }
            gst::MessageView::Eos(..) => {
                let successor = self
                    .queue_state
                    .lock()
                    .ok()
                    .and_then(|mut queue| queue.take_eos_successor());
                // The track played to its end.
                self.reset_failures();
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
                self.clear_recent_seek_trace();
                self.startup_gain_ramp = false;
                self.startup_ramp_hold_until = None;
                let _ = self.set_pipeline_state(gst::State::Ready);
                self.begin_pipeline_run(false);
                super::stop_snapshot_at_terminal_eos(&mut self.snapshot, (self.clock)());
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
            gst::MessageView::Error(err) => self.handle_error_message(err),
            gst::MessageView::Element(_) => {
                if let Ok(missing) = gst_pbutils::MissingPluginMessage::parse(msg) {
                    self.note_missing_plugin(&missing);
                }
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
            gst::MessageView::AsyncDone(_) => {
                if msg.src().is_some_and(|src| src == &self.playbin) {
                    self.handle_async_done();
                }
            }
            gst::MessageView::StateChanged(sc) => {
                if sc.src().is_some_and(|s| s == &self.playbin) {
                    #[cfg(feature = "profiling-logs")]
                    self.log_playbin_state_change(sc.old(), sc.current(), sc.pending());
                    if sc.current() == gst::State::Playing {
                        self.playing_state_requested_at = None;
                    }
                    // Whether the position advances follows the pipeline's
                    // state; a seek reports its own position once it lands.
                    if self.snapshot.state != PlaybackState::Stopped
                        && !self.seek_pending()
                        && self.sample_pipeline_position()
                    {
                        self.emit_snapshot();
                    }
                }
            }
            _ => {}
        }
    }

    fn handle_error_message(&mut self, err: &gst::message::Error) {
        eprintln!(
            "[ferrous] gstreamer error from {:?}: {} ({:?})",
            err.src().map(gstreamer::prelude::GstObjectExt::path_string),
            err.error(),
            err.debug()
        );
        let error = err.error();
        let file = input_file(err.src());
        let not_found = error.matches(gst::ResourceError::NotFound);
        let (file, detail) = match self.missing_plugin.take() {
            Some(missing)
                if error.matches(gst::CoreError::MissingPlugin)
                    || error.matches(gst::StreamError::CodecNotFound) =>
            {
                (
                    file.or(missing.file),
                    missing_plugin_detail(&missing.description),
                )
            }
            _ => (file, error.to_string()),
        };
        // Every later message of the faulted run is discarded.
        self.fail_track(file, detail, not_found);
    }

    /// Remember a missing plugin, to name it in the error that follows.
    fn note_missing_plugin(&mut self, missing: &gst_pbutils::MissingPluginMessage<'_>) {
        let description = missing.description().to_string();
        eprintln!("[ferrous] missing GStreamer plugin: {description}");
        // The first report names the input; later ones repeat it.
        if self.missing_plugin.is_none() {
            self.missing_plugin = Some(MissingPlugin {
                file: input_file(missing.msg.src()),
                description,
            });
        }
    }

    /// Silence the pipeline before it is stopped or restarted. The samples
    /// it has queued are dropped with the run, so a stepped fade would only
    /// delay the switch.
    fn mute(&mut self) {
        self.applied_volume = 0.0;
        self.volume_ramp_tick_at = None;
        self.playbin.set_property("volume", 0.0_f64);
    }

    fn switch_track(&mut self, path: &Path, uri: &str, force_play: bool) {
        // Positions from now on belong to another run or position.
        self.gapless_handover = None;
        let was_playing = self.snapshot.state == PlaybackState::Playing || force_play;
        self.mute();
        self.playing_state_requested_at = None;
        let _ = self.set_pipeline_state(gst::State::Null);
        let _ = self.set_pipeline_state(gst::State::Ready);
        // Also drops a successor staged while the old run was stopping.
        self.begin_pipeline_run(false);
        self.set_run_uri(path, uri);
        if was_playing {
            let _ = self.set_pipeline_state(gst::State::Playing);
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
            let _ = self.set_pipeline_state(gst::State::Paused);
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
        } else {
            self.startup_gain_ramp = false;
            self.startup_ramp_hold_until = None;
        }
        self.snapshot.current = Some(path.to_path_buf());
        // The new run starts at the beginning and advances once it plays.
        self.set_position_sample(Duration::ZERO, false);
        self.snapshot.duration = Duration::ZERO;
        self.snapshot.current_bitrate_kbps = None;
        self.clear_recent_seek_trace();
    }
}

/// Create the pipeline and the runtime that drives it.
fn start_gst_engine(
    event_tx: Sender<PlaybackEvent>,
    analysis_tx: Sender<AnalysisCommand>,
    build_output: fn() -> anyhow::Result<gst::Element>,
) -> anyhow::Result<(GstPlaybackRuntime, gst::Bus)> {
    gst::init().context("GStreamer could not be initialized")?;
    configure_raw_surround_decoding();

    let playbin = gst::ElementFactory::make("playbin3").build().map_err(|_| {
        anyhow!("GStreamer's playbin3 element is missing; install the GStreamer base plugins")
    })?;
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
    let stream_ends = Arc::new(Mutex::new(StreamEndHold::default()));
    install_stream_probes(&playbin, &stream_durations, &stream_ends);

    let queue_state = Arc::new(Mutex::new(GaplessQueue::new()));

    let bus = playbin.bus().context("playbin has no bus")?;
    let runtime = GstPlaybackRuntime::new(
        playbin,
        queue_state,
        analysis_tx,
        event_tx,
        channel_mute_mask,
        stream_durations,
        stream_ends,
    );
    connect_type_finding_hold(&runtime.playbin);
    runtime.input_switches.connect(&runtime.playbin);
    connect_about_to_finish(&runtime);
    runtime
        .playbin
        .set_property("volume", f64::from(runtime.applied_volume));
    Ok((runtime, bus))
}

/// Stage the successor whenever the pipeline of `runtime` asks for one.
fn connect_about_to_finish(runtime: &GstPlaybackRuntime) {
    let queue_state = Arc::clone(&runtime.queue_state);
    let stream_durations = Arc::clone(&runtime.stream_durations);
    let analysis_tx = runtime.analysis_tx.clone();
    let input_switches = Arc::clone(&runtime.input_switches);
    #[cfg(test)]
    let calls = Arc::clone(&runtime.about_to_finish);
    // Runs on a streaming thread once the pipeline has read all of the
    // last stream it was given, usually about 2 s before that stream ends
    // and at once for a short file.
    runtime
        .playbin
        .connect("about-to-finish", false, move |values| {
            #[cfg(test)]
            calls.emitted.fetch_add(1, Ordering::SeqCst);
            if let Some(playbin) = values
                .first()
                .and_then(|value| value.get::<gst::Element>().ok())
            {
                // Not while the run ends, which drops the successor anyway.
                input_switches.hand_over(|| {
                    stage_successor(&queue_state, &stream_durations, &analysis_tx, |uri| {
                        // Activates the successor's source.
                        activate_sources(|| playbin.set_property("uri", uri));
                    });
                });
            }
            #[cfg(test)]
            calls.returned.fetch_add(1, Ordering::SeqCst);
            None
        });
}

/// How often the pipeline emitted about-to-finish and how often the
/// engine's handler returned, for the reports of engine tests.
#[cfg(test)]
#[derive(Debug, Default)]
struct AboutToFinishCalls {
    emitted: AtomicU64,
    returned: AtomicU64,
}

fn run_gst_engine(
    mut runtime: GstPlaybackRuntime,
    bus: &gst::Bus,
    cmd_rx: &Receiver<PlaybackCommand>,
) {
    // Wake for bus messages, such as the async-done that tells where a seek
    // landed, instead of finding them at the next timeout.
    let (bus_wake_tx, bus_wake_rx) = bounded::<()>(1);
    bus.set_sync_handler(move |_, _| {
        let _ = bus_wake_tx.try_send(());
        gst::BusSyncReply::Pass
    });
    loop {
        crossbeam_channel::select! {
            recv(cmd_rx) -> cmd => {
                let Ok(cmd) = cmd else {
                    break;
                };
                // Commands that queued up while the engine was busy are
                // handled as one burst.
                let burst: Vec<_> = std::iter::once(cmd).chain(cmd_rx.try_iter()).collect();
                for step in merge_burst(burst) {
                    // Act on the pipeline's latest state, such as a successor
                    // that has just started.
                    runtime.drain_bus(bus);
                    runtime.apply_step(step);
                }
            }
            recv(bus_wake_rx) -> _ => {}
            default(runtime.command_wait_timeout()) => {}
        }
        runtime.drain_bus(bus);
        runtime.poll_volume_ramp(Instant::now());
    }

    bus.unset_sync_handler();
    let _ = runtime.set_pipeline_state(gst::State::Null);
}

/// A step of the engine: a command, or a burst of commands merged into one.
#[derive(Debug)]
enum EngineStep {
    Command(PlaybackCommand),
    /// `requests` Seek commands in a row, of which only the last one, to
    /// `position`, is carried out.
    Seek {
        position: Duration,
        requests: u64,
    },
    /// Next (`true`) and Previous presses in a row.
    Navigate(Vec<bool>),
}

/// Merge a burst of commands: seeks in a row become one seek to the last
/// target, and Next and Previous presses in a row become one move by all of
/// them, which starts only the track it ends on. The bridge's polls are
/// answered once, after the burst.
fn merge_burst(commands: Vec<PlaybackCommand>) -> Vec<EngineStep> {
    let mut steps: Vec<EngineStep> = Vec::new();
    let mut poll = false;
    for command in commands {
        match (command, steps.last_mut()) {
            (PlaybackCommand::Poll, _) => poll = true,
            (PlaybackCommand::Seek(target), Some(EngineStep::Seek { position, requests })) => {
                *position = target;
                *requests += 1;
            }
            (PlaybackCommand::Seek(position), _) => steps.push(EngineStep::Seek {
                position,
                requests: 1,
            }),
            (command @ (PlaybackCommand::Next | PlaybackCommand::Previous), last) => {
                let next = matches!(command, PlaybackCommand::Next);
                if let Some(EngineStep::Navigate(moves)) = last {
                    moves.push(next);
                } else {
                    steps.push(EngineStep::Navigate(vec![next]));
                }
            }
            (command, _) => steps.push(EngineStep::Command(command)),
        }
    }
    if poll {
        steps.push(EngineStep::Command(PlaybackCommand::Poll));
    }
    steps
}

/// Stage the natural successor when the pipeline asks for one, and give
/// a gapless successor to the pipeline with `hand_over`, which receives
/// its URI. Only stage it here; the runtime commits it when its stream
/// starts or at end of stream.
///
/// The URI is handed over under the queue lock, so a rename reaches the
/// queue either before the successor is staged or after the pipeline was
/// given its URI. Do not call into playbin here otherwise: a query can
/// deadlock with a state change on the engine thread.
fn stage_successor(
    queue_state: &Mutex<GaplessQueue>,
    stream_durations: &Mutex<StreamDurations>,
    analysis_tx: &Sender<AnalysisCommand>,
    hand_over: impl FnOnce(&str),
) {
    let mut queue = match queue_state.lock() {
        Ok(queue) => queue,
        Err(e) => {
            eprintln!("[ferrous] about-to-finish: queue lock poisoned: {e}");
            return;
        }
    };
    // Same file extension → gapless (same decoder reused).
    // Different extension → let EOS fire, handle in bus handler.
    let Some(staged) = queue.stage_natural(|finishing, next| {
        if same_audio_extension(finishing, next) && file_uri(next).is_some() {
            Handoff::Gapless
        } else {
            Handoff::AtEos
        }
    }) else {
        return;
    };
    if staged.staged_spectrogram {
        // Send while holding the queue lock so that a cancellation cannot
        // reach analysis before this request. The staging thread checks
        // format compatibility itself.
        let _ = analysis_tx.send(AnalysisCommand::PrepareGaplessContinuation {
            path: staged.path.clone(),
        });
    }
    if staged.handoff == Handoff::Gapless {
        if let Some(uri) = file_uri(&staged.path) {
            if let Ok(mut durations) = stream_durations.lock() {
                durations.hand_over(staged.path);
            }
            hand_over(&uri);
        }
    }
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

/// The streams of the pipeline run that are still needed, in play order:
/// the audible track's stream, then each successor handed to the pipeline,
/// with the duration its parser last reported. A stream is identified by
/// its place in this list. Its file, the path the pipeline was given, only
/// matches parser reports and errors to it and does not change when the
/// track is renamed.
#[derive(Debug, Default)]
struct StreamDurations {
    streams: VecDeque<RunStream>,
}

#[derive(Debug)]
struct RunStream {
    file: PathBuf,
    duration: Option<Duration>,
}

impl StreamDurations {
    /// Start a run with the stream of `file`.
    fn start_run(&mut self, file: PathBuf) {
        self.clear();
        self.hand_over(file);
    }

    /// The pipeline was given a successor that reads `file`.
    fn hand_over(&mut self, file: PathBuf) {
        self.streams.push_back(RunStream {
            file,
            duration: None,
        });
    }

    /// The oldest successor became audible.
    fn commit(&mut self) {
        self.streams.pop_front();
    }

    /// Record what the parser of a stream reading `file` reports. A file
    /// queued twice has one duration, so every stream reading it takes the
    /// report.
    fn record(&mut self, file: &Path, duration: Duration) {
        for stream in self.streams.iter_mut().filter(|stream| stream.file == file) {
            stream.duration = Some(duration);
        }
    }

    /// Place in play order of the first stream reading `file`: 0 for the
    /// audible stream, then the successors handed to the pipeline.
    fn position(&self, file: &Path) -> Option<usize> {
        self.streams.iter().position(|stream| stream.file == file)
    }

    /// Duration of the audible stream.
    fn current(&self) -> Option<Duration> {
        self.streams.front()?.duration
    }

    fn clear(&mut self) {
        self.streams.clear();
    }
}

thread_local! {
    /// While the thread runs `activate_sources`, the probes that hold back
    /// the type finding of the sources it activates.
    static TYPE_FINDING_HOLDS: RefCell<Option<Vec<(gst::Pad, gst::PadProbeId)>>> =
        const { RefCell::new(None) };
}

/// Run `activate`, which can activate sources of `playbin`: a state change
/// of the pipeline, or a successor handed over. The sources find the type
/// of their files only once it has returned.
///
/// `urisourcebin` starts to find the type of its file while it still changes
/// state. Its streaming thread then adds a `parsebin` and changes its state,
/// which deadlocks when the activating thread changes the state of the new
/// `parsebin` too: each waits for a lock that the other holds. Under load
/// the activating thread is slow enough for that to happen. See
/// <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/4472>, open
/// in 1.28.
fn activate_sources<R>(activate: impl FnOnce() -> R) -> R {
    let outer = TYPE_FINDING_HOLDS.with(|holds| holds.replace(Some(Vec::new())));
    let result = activate();
    let holds = TYPE_FINDING_HOLDS.with(|holds| holds.replace(outer));
    for (pad, probe) in holds.into_iter().flatten() {
        pad.remove_probe(probe);
    }
    result
}

/// Hold back the type finding of every source that `urisourcebin` sets up
/// in `activate_sources`, until that returns. The type finder of a source
/// first announces the stream it starts, so the probe holds it there,
/// before it reads the file.
fn connect_type_finding_hold(playbin: &gst::Element) {
    // Runs on the thread that adds the type finder, before it starts.
    playbin.connect("deep-element-added", false, |values| {
        let bin = values.get(1)?.get::<gst::Bin>().ok()?;
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        let factory_is = |element: &gst::Element, name: &str| {
            element
                .factory()
                .is_some_and(|factory| factory.name() == name)
        };
        if !factory_is(&element, "typefind") || !factory_is(bin.upcast_ref(), "urisourcebin") {
            return None;
        }
        TYPE_FINDING_HOLDS.with(|holds| {
            let mut holds = holds.borrow_mut();
            let holds = holds.as_mut()?;
            let pad = element.static_pad("src")?;
            let probe = pad.add_probe(
                gst::PadProbeType::BLOCK | gst::PadProbeType::DATA_DOWNSTREAM,
                |_, _| gst::PadProbeReturn::Ok,
            )?;
            holds.push((pad, probe));
            Some(())
        });
        None
    });
}

/// Orders the state changes that end a pipeline run, to Ready or Null,
/// against what uridecodebin3 does at a gapless switch under its state lock
/// on streaming threads: it activates the successor's source when
/// about-to-finish hands it over, and it switches its input to the
/// successor once the input has drained.
///
/// A run ends with uridecodebin3 going from Paused to Ready. Holding its
/// state lock, it marks itself as shutting down, which wakes a successor
/// that waits for the input to drain, and then deactivates the outputs of
/// its sources, which waits for their streaming threads. A streaming thread
/// that meanwhile hands over a successor waits for that state lock, and so
/// does one that brings a successor's first buffer to uridecodebin3 once
/// the input has drained (`uri_src_block_probe` switches the input, and
/// `free_source_handler` takes the lock). One that brings it before the
/// input has drained waits for the drain, which the shutdown no longer
/// announces. Each holds a lock that the state change goes on to wait for,
/// the play items lock or a stream lock, so the engine thread and the
/// streaming thread deadlock. The code involved is the same in `GStreamer`
/// 1.24 and 1.28; see `GStreamer` issues 3490, 4137, 4310, and 4644.
///
/// So the end of a run waits for a handover or a switch in progress, and
/// neither starts until the run has ended:
///
/// - about-to-finish hands over a successor only while the run does not
///   end, as the end drops the successor anyway;
/// - the first data of a successor's source is held at the source's output,
///   in a block that the deactivation of the output flushes, until the input
///   has drained and the run does not end. uridecodebin3 then switches to the
///   successor at once, and the switch ends when the data has passed it.
///
/// The changes of state between Paused and Playing take neither the play
/// items lock of uridecodebin3 nor stream locks, so a switch only waits for
/// them; they are not ordered.
#[derive(Default)]
struct InputSwitches {
    state: Mutex<InputSwitchState>,
    /// Signalled when a handover or a switch has finished.
    settled: Condvar,
}

#[derive(Default)]
struct InputSwitchState {
    /// The engine ends the run: it changes the pipeline's state to Ready or
    /// Null.
    ending: bool,
    /// About-to-finish callbacks that hand over a successor.
    handovers: usize,
    /// The sources of the run, the `urisourcebin` of each input, in the
    /// order of their activation.
    sources: Vec<RunSource>,
}

struct RunSource {
    bin: gst::glib::WeakRef<gst::Element>,
    phase: SourcePhase,
    outputs: Vec<SourceOutput>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SourcePhase {
    /// uridecodebin3 has not taken data from the source yet. Its first data
    /// waits at its outputs.
    Waiting,
    /// The first data was let through to uridecodebin3, which switches its
    /// input to the source unless it is the input already.
    Switching,
    /// uridecodebin3 takes the data of the source: it is the input, or was.
    Input,
}

struct SourceOutput {
    /// The source's output pad, a ghost pad.
    pad: gst::glib::WeakRef<gst::Pad>,
    /// End of stream has passed uridecodebin3 at this output.
    drained: bool,
    /// The block of the output's first data, on the internal pad of the
    /// output, until the source may be switched to.
    hold: Option<(gst::glib::WeakRef<gst::Pad>, gst::PadProbeId)>,
    /// The first data waits in the block.
    held: bool,
}

/// Blocks to remove, outside the lock of `InputSwitches`.
type Holds = Vec<(gst::glib::WeakRef<gst::Pad>, gst::PadProbeId)>;

fn remove_holds(holds: Holds) {
    for (pad, probe) in holds {
        if let Some(pad) = pad.upgrade() {
            pad.remove_probe(probe);
        }
    }
}

impl InputSwitchState {
    fn source_of(&mut self, output: &gst::Pad) -> Option<&mut RunSource> {
        self.sources.iter_mut().find(|source| {
            source
                .outputs
                .iter()
                .any(|known| known.pad.upgrade().as_ref() == Some(output))
        })
    }

    fn output(&mut self, pad: &gst::Pad) -> Option<&mut SourceOutput> {
        self.sources
            .iter_mut()
            .flat_map(|source| source.outputs.iter_mut())
            .find(|known| known.pad.upgrade().as_ref() == Some(pad))
    }

    /// Whether uridecodebin3 may switch its input to a waiting source now:
    /// the run does not end, no switch is in progress, and the input has
    /// drained, so that the switch happens at once.
    fn may_switch(&self) -> bool {
        !self.ending
            && self
                .sources
                .iter()
                .all(|source| source.phase != SourcePhase::Switching)
            && self
                .sources
                .iter()
                .filter(|source| source.phase == SourcePhase::Input)
                .flat_map(|source| source.outputs.iter())
                .filter(|output| output.pad.upgrade().is_some())
                .all(|output| output.drained)
    }

    /// Let the first waiting source whose data waits be switched to, if it
    /// may be. Returns the blocks to remove.
    fn release_next(&mut self) -> Holds {
        if !self.may_switch() {
            return Vec::new();
        }
        let Some(source) = self.sources.iter_mut().find(|source| {
            source.phase == SourcePhase::Waiting && source.outputs.iter().any(|output| output.held)
        }) else {
            return Vec::new();
        };
        source.phase = SourcePhase::Switching;
        source.take_holds()
    }
}

impl RunSource {
    fn take_holds(&mut self) -> Holds {
        self.outputs
            .iter_mut()
            .filter_map(|output| {
                output.held = false;
                output.hold.take()
            })
            .collect()
    }
}

impl InputSwitches {
    fn lock(&self) -> MutexGuard<'_, InputSwitchState> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Run `end`, which changes the pipeline's state to Ready or Null, once
    /// no handover or switch is in progress, and keep new ones from starting
    /// until it has returned. Both take moments: a switch is let through
    /// only once the input has drained, so uridecodebin3 does not wait for
    /// the input.
    fn end_run<R>(&self, end: impl FnOnce() -> R) -> R {
        let mut state = self.lock();
        state.ending = true;
        while state.handovers > 0
            || state
                .sources
                .iter()
                .any(|source| source.phase == SourcePhase::Switching)
        {
            state = self
                .settled
                .wait(state)
                .unwrap_or_else(PoisonError::into_inner);
        }
        drop(state);
        let result = end();
        let holds = {
            let mut state = self.lock();
            state.ending = false;
            // The sources removed their outputs, which flushed the data that
            // waited. A source that the next run reuses adds new ones.
            state
                .sources
                .drain(..)
                .flat_map(|mut source| source.take_holds())
                .collect()
        };
        remove_holds(holds);
        result
    }

    /// Run `hand_over`, which hands a successor to the pipeline, unless the
    /// run ends.
    fn hand_over(&self, hand_over: impl FnOnce()) {
        {
            let mut state = self.lock();
            if state.ending {
                return;
            }
            state.handovers += 1;
        }
        hand_over();
        self.lock().handovers -= 1;
        self.settled.notify_all();
    }

    /// Watch the outputs of every source that uridecodebin3 in `playbin`
    /// adds.
    fn connect(self: &Arc<Self>, playbin: &gst::Element) {
        let switches = Arc::clone(self);
        playbin.connect("deep-element-added", false, move |values| {
            let element = values.get(2)?.get::<gst::Element>().ok()?;
            if element
                .factory()
                .is_some_and(|factory| factory.name() == "urisourcebin")
            {
                let switches = Arc::clone(&switches);
                // Connected after uridecodebin3's own handler, which blocks
                // the outputs of a successor in a probe, so the probes added
                // here on the same pad come after uridecodebin3's.
                element.connect_pad_added(move |bin, pad| switches.watch_output(bin, pad));
            }
            None
        });
    }

    fn watch_output(self: &Arc<Self>, bin: &gst::Element, pad: &gst::Pad) {
        let Some(internal) = pad
            .downcast_ref::<gst::GhostPad>()
            .and_then(gst::prelude::ProxyPadExt::internal)
        else {
            return;
        };
        if pad.direction() != gst::PadDirection::Src {
            return;
        }
        let mut state = self.lock();
        let known = state
            .sources
            .iter()
            .position(|source| source.bin.upgrade().as_ref() == Some(bin));
        let index = known.unwrap_or_else(|| {
            state.sources.push(RunSource {
                bin: bin.downgrade(),
                phase: SourcePhase::Waiting,
                outputs: Vec::new(),
            });
            state.sources.len() - 1
        });
        let switches = Arc::clone(self);
        let output = pad.downgrade();
        // Added under the lock, so the block is known before data arrives.
        let hold = internal.add_probe(
            gst::PadProbeType::BLOCK
                | gst::PadProbeType::BUFFER
                | gst::PadProbeType::BUFFER_LIST
                | gst::PadProbeType::EVENT_DOWNSTREAM,
            move |_, info| {
                if is_event_but_gap(info) {
                    return gst::PadProbeReturn::Pass;
                }
                output
                    .upgrade()
                    .map_or(gst::PadProbeReturn::Remove, |output| {
                        switches.first_data(&output)
                    })
            },
        );
        state.sources[index].outputs.push(SourceOutput {
            pad: pad.downgrade(),
            drained: false,
            hold: hold.map(|probe| (internal.upcast_ref::<gst::Pad>().downgrade(), probe)),
            held: false,
        });
        drop(state);

        let switches = Arc::clone(self);
        let output = pad.downgrade();
        internal.add_probe(
            gst::PadProbeType::EVENT_DOWNSTREAM,
            move |internal, info| {
                if let (Some(output), Some(gst::PadProbeData::Event(event))) =
                    (output.upgrade(), &info.data)
                {
                    switches.watch_drain(internal.upcast_ref(), &output, event);
                }
                gst::PadProbeReturn::Ok
            },
        );

        // Runs after uridecodebin3's block of a successor's output, so the
        // first data that passes it has passed the switch.
        let switches = Arc::clone(self);
        pad.add_probe(
            gst::PadProbeType::BUFFER
                | gst::PadProbeType::BUFFER_LIST
                | gst::PadProbeType::EVENT_DOWNSTREAM,
            move |output, info| {
                if is_event_but_gap(info) {
                    return gst::PadProbeReturn::Ok;
                }
                switches.switched(output);
                gst::PadProbeReturn::Remove
            },
        );
    }

    /// The first data of `output` arrived at its internal pad, before
    /// uridecodebin3. Let it through if its source may take part in the
    /// input, and hold it otherwise.
    fn first_data(&self, output: &gst::Pad) -> gst::PadProbeReturn {
        let mut state = self.lock();
        let may_switch = state.may_switch();
        let Some(source) = state.source_of(output) else {
            // A source of a run that has ended.
            return gst::PadProbeReturn::Remove;
        };
        let holds = match source.phase {
            SourcePhase::Input | SourcePhase::Switching => Vec::new(),
            SourcePhase::Waiting if may_switch => {
                source.phase = SourcePhase::Switching;
                source.take_holds()
            }
            SourcePhase::Waiting => {
                if let Some(own) = state.output(output) {
                    own.held = true;
                }
                return gst::PadProbeReturn::Ok;
            }
        };
        if let Some(own) = state.output(output) {
            own.hold = None;
        }
        drop(state);
        // The other outputs of the source; this one's block goes with the
        // returned value.
        let own = output
            .downcast_ref::<gst::GhostPad>()
            .and_then(gst::prelude::ProxyPadExt::internal)
            .map(Cast::upcast::<gst::Pad>);
        remove_holds(
            holds
                .into_iter()
                .filter(|(pad, _)| pad.upgrade() != own)
                .collect(),
        );
        gst::PadProbeReturn::Remove
    }

    /// Data of `output` passed uridecodebin3, which has switched its input
    /// to the output's source if it was not the input yet.
    fn switched(&self, output: &gst::Pad) {
        let mut state = self.lock();
        if let Some(source) = state.source_of(output) {
            source.phase = SourcePhase::Input;
        }
        drop(state);
        self.settled.notify_all();
    }

    /// Track whether `output` has drained, from the `event` at its
    /// `internal` pad.
    fn watch_drain(self: &Arc<Self>, internal: &gst::Pad, output: &gst::Pad, event: &gst::Event) {
        match event.type_() {
            gst::EventType::Eos => {
                let switches = Arc::clone(self);
                let output = output.downgrade();
                let drained = move || {
                    if let Some(output) = output.upgrade() {
                        switches.drained(&output);
                    }
                };
                // The end of stream drains the input once uridecodebin3 has
                // taken it, which is when the push that brings it returns.
                match internal.peer() {
                    Some(peer) => {
                        peer.add_probe(gst::PadProbeType::IDLE, move |_, _| {
                            drained();
                            gst::PadProbeReturn::Remove
                        });
                    }
                    None => drained(),
                }
            }
            gst::EventType::StreamStart | gst::EventType::Segment => {
                if let Some(output) = self.lock().output(output) {
                    output.drained = false;
                }
            }
            _ => {}
        }
    }

    fn drained(&self, output: &gst::Pad) {
        let holds = {
            let mut state = self.lock();
            if let Some(output) = state.output(output) {
                output.drained = true;
            }
            state.release_next()
        };
        remove_holds(holds);
    }
}

/// Whether the probe `info` carries an event other than a gap, which
/// uridecodebin3 lets through to a successor that it has not switched to.
fn is_event_but_gap(info: &gst::PadProbeInfo) -> bool {
    matches!(
        &info.data,
        Some(gst::PadProbeData::Event(event)) if event.type_() != gst::EventType::Gap
    )
}

/// While a seek is prepared, holds back the end of every stream that the
/// pipeline parses, so the pipeline asks for a successor only after the
/// seek. playbin3 asks for the successor of a stream once, when it has read
/// the stream to its end; a short track is read to its end while a
/// restarted run prerolls, before its seek. Without the hold, its successor
/// could then only start after end of stream, with a gap.
#[derive(Default)]
struct StreamEndHold {
    held: bool,
    /// Source pads of the parsers, which carry the parsed streams.
    pads: Vec<gst::glib::WeakRef<gst::Pad>>,
    /// The blocking probes of a hold.
    blocks: Vec<(gst::glib::WeakRef<gst::Pad>, gst::PadProbeId)>,
}

impl StreamEndHold {
    /// A parser exposed `pad`. Hold its end too while a hold lasts.
    fn add_pad(&mut self, pad: &gst::Pad) {
        self.pads.retain(|pad| pad.upgrade().is_some());
        self.pads.push(pad.downgrade());
        if self.held {
            self.block(pad);
        }
    }

    fn hold(&mut self) {
        if std::mem::replace(&mut self.held, true) {
            return;
        }
        let pads: Vec<_> = self
            .pads
            .iter()
            .filter_map(gst::glib::WeakRef::upgrade)
            .collect();
        for pad in &pads {
            self.block(pad);
        }
    }

    fn release(&mut self) {
        self.held = false;
        for (pad, probe) in self.blocks.drain(..) {
            if let Some(pad) = pad.upgrade() {
                pad.remove_probe(probe);
            }
        }
    }

    /// Block `pad` at its end of stream. A flush, as from a seek, drops
    /// the held end; the stream ends again after the seek.
    fn block(&mut self, pad: &gst::Pad) {
        let probe = pad.add_probe(
            gst::PadProbeType::BLOCK | gst::PadProbeType::EVENT_DOWNSTREAM,
            |_, info| match info.data {
                Some(gst::PadProbeData::Event(ref event))
                    if event.type_() == gst::EventType::Eos =>
                {
                    gst::PadProbeReturn::Ok
                }
                _ => gst::PadProbeReturn::Pass,
            },
        );
        if let Some(probe) = probe {
            self.blocks.push((pad.downgrade(), probe));
        }
    }
}

/// Install the probes on the source pads of every parser of the pipeline:
/// they record each stream's duration and hold its end during a seek. See
/// `install_stream_duration_probe` and `StreamEndHold`. Lock order: the
/// probes take `durations` and `stream_ends` one at a time.
fn install_stream_probes(
    playbin: &gst::Element,
    durations: &Arc<Mutex<StreamDurations>>,
    stream_ends: &Arc<Mutex<StreamEndHold>>,
) {
    let durations = Arc::clone(durations);
    let stream_ends = Arc::clone(stream_ends);
    playbin.connect("deep-element-added", false, move |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        if element
            .factory()
            .is_some_and(|factory| factory.name() == "parsebin")
        {
            let durations = Arc::clone(&durations);
            let stream_ends = Arc::clone(&stream_ends);
            element.connect_pad_added(move |_, pad| {
                if pad.direction() == gst::PadDirection::Src {
                    install_stream_duration_probe(pad, Arc::clone(&durations));
                    if let Ok(mut stream_ends) = stream_ends.lock() {
                        stream_ends.add_pad(pad);
                    }
                }
            });
        }
        None
    });
}

/// Record the duration of every parsed stream under its file, from the
/// streaming thread that parses it: when its first buffer leaves the
/// parser and again at its end, when the parser has read all of it. Queries
/// on the parser's own pad cannot describe another stream, and they avoid
/// the playbin locks that a state change on the engine thread holds. (The
/// group of a stream is no key: the pipeline may restamp it.)
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
                        durations.record(&path, Duration::from_nanos(duration.nseconds()));
                    }
                }
            }
            gst::PadProbeReturn::Ok
        },
    );
}

fn missing_plugin_detail(description: &str) -> String {
    format!("missing GStreamer plugin: {description}")
}

/// File of the pipeline input that `src` belongs to: a file source, or an
/// element of the `urisourcebin` that reads one input, which also parses
/// it. Elements that the inputs share, such as decoders, belong to none.
fn input_file(src: Option<&gst::Object>) -> Option<PathBuf> {
    let mut element = src?.clone().downcast::<gst::Element>().ok()?;
    loop {
        let factory = element.factory().map(|factory| factory.name().to_string());
        let uri = match factory.as_deref() {
            Some("urisourcebin") => element
                .property_value("uri")
                .get::<Option<String>>()
                .ok()
                .flatten(),
            Some("decodebin3" | "uridecodebin3" | "playbin3") => return None,
            _ => element
                .dynamic_cast_ref::<gst::URIHandler>()
                .filter(|handler| handler.uri_type() == gst::URIType::Src)
                .and_then(gst::prelude::URIHandlerExt::uri)
                .map(String::from),
        };
        if let Some(uri) = uri {
            return url::Url::parse(&uri).ok()?.to_file_path().ok();
        }
        element = element.parent()?.downcast::<gst::Element>().ok()?;
    }
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

/// Seek flags for the track at `path`. Seeks are accurate: without the
/// flag, parsers such as those of MP3 and ADTS AAC estimate where the target
/// is from the bitrate and label the audio found there with the target time,
/// so the pipeline itself cannot tell where the seek landed, and playback
/// is offset from the spectrogram, which seeks accurately. An accurate seek
/// can make a parser scan, but bursts of seeks are merged, so scrubbing
/// pays for one. DTS seeks to the key frame before the target; its
/// position is reported as where the seek landed.
fn seek_flags_for_path(path: Option<&Path>) -> gst::SeekFlags {
    if path.is_some_and(is_dts_file) {
        gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT
    } else {
        gst::SeekFlags::FLUSH | gst::SeekFlags::ACCURATE
    }
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
        .map_err(|_| {
            anyhow!("no audio output is available; install the GStreamer good plugins (autoaudiosink)")
        })
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
    use crate::playback::PathRenames;
    use std::sync::TryLockError;

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
            Arc::new(Mutex::new(StreamEndHold::default())),
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
    fn successor_uri_is_handed_over_under_the_queue_lock() {
        let dir = std::env::temp_dir();
        let (a, b, renamed) = (
            dir.join("a.wav"),
            dir.join("b.wav"),
            dir.join("renamed b.wav"),
        );
        let queue = Mutex::new(GaplessQueue::new());
        let _ = queue.lock().unwrap().set_queue(vec![a.clone(), b.clone()]);
        let durations = Mutex::new(StreamDurations::default());
        durations.lock().unwrap().start_run(a);
        let (analysis_tx, analysis_rx) = unbounded();
        let mut handed = None;
        stage_successor(&queue, &durations, &analysis_tx, |uri| {
            // A rename cannot reach the queue between staging and handover.
            assert!(matches!(queue.try_lock(), Err(TryLockError::WouldBlock)));
            handed = Some(uri.to_owned());
        });
        assert_eq!(handed, file_uri(&b));
        assert!(matches!(
            analysis_rx.try_recv(),
            Ok(AnalysisCommand::PrepareGaplessContinuation { path }) if path == b
        ));

        // A rename after the handover leaves the stream on the file the
        // pipeline reads.
        queue
            .lock()
            .unwrap()
            .remap_paths(&PathRenames::from([(b.clone(), renamed.clone())]));
        durations
            .lock()
            .unwrap()
            .record(&b, Duration::from_millis(1_200));
        let committed = queue.lock().unwrap().commit_gapless();
        assert_eq!(committed.map(|committed| committed.path), Some(renamed));
        durations.lock().unwrap().commit();
        assert_eq!(
            durations.lock().unwrap().current(),
            Some(Duration::from_millis(1_200))
        );
    }

    #[test]
    fn stream_durations_keep_only_the_streams_still_needed() {
        let (a, b) = (PathBuf::from("/music/a.wav"), PathBuf::from("/music/b.wav"));
        let seconds = Duration::from_secs;
        let mut durations = StreamDurations::default();
        durations.start_run(b.clone());
        durations.start_run(a.clone());
        // Not a stream of this run.
        durations.record(&b, seconds(1));
        assert_eq!(durations.current(), None);
        durations.record(&a, seconds(4));
        durations.hand_over(b.clone());
        durations.hand_over(a.clone());
        // A file queued twice has one duration.
        durations.record(&a, seconds(5));
        assert_eq!(durations.current(), Some(seconds(5)));

        durations.commit();
        assert_eq!(durations.current(), None, "B has not reported yet");
        durations.record(&b, seconds(1));
        assert_eq!(durations.current(), Some(seconds(1)));
        durations.commit();
        assert_eq!(durations.current(), Some(seconds(5)));
        durations.commit();
        assert!(durations.streams.is_empty());
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

    fn at(millis: u64) -> MonotonicTime {
        MonotonicTime::from_nanos(1_000_000_000_000).saturating_add(Duration::from_millis(millis))
    }

    #[test]
    fn gapless_handover_starts_the_successor_where_the_previous_track_ends() {
        // The previous track was at 3.8 s of 4 s at 0 ms; the switch is
        // committed at 15 ms, and the successor starts at 200 ms.
        let handover = GaplessHandover::after(
            Duration::from_millis(3_800),
            at(0),
            Duration::from_secs(4),
            at(15),
        );
        assert_eq!(handover.successor_starts_at, at(200));
        // A track that already ended starts its successor now.
        let late = GaplessHandover::after(
            Duration::from_millis(3_800),
            at(0),
            Duration::from_secs(4),
            at(500),
        );
        assert_eq!(late.successor_starts_at, at(500));
    }

    #[test]
    fn gapless_handover_tells_the_previous_streams_position_from_the_successors() {
        let handover = GaplessHandover::after(
            Duration::from_millis(3_837),
            at(0),
            Duration::from_secs(4),
            at(3),
        );
        // The pipeline still reports the previous pass of a Repeat-One
        // replay, seen at its commit in #187.
        assert!(handover.is_previous_stream(Duration::from_millis(3_840), at(3)));
        assert!(handover.is_previous_stream(Duration::from_millis(3_900), at(60)));
        // The successor's own positions near its start.
        assert!(!handover.is_previous_stream(Duration::ZERO, at(3)));
        assert!(!handover.is_previous_stream(Duration::from_millis(40), at(203)));
        assert!(!handover.is_previous_stream(Duration::from_millis(200), at(400)));
        // Until the pipeline has had long enough to report the successor.
        assert!(!handover.is_over(at(1_163)));
        assert!(handover.is_over(at(1_164)));
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
    fn seeks_are_accurate_except_for_dts() {
        for path in [
            "/tmp/test.flac",
            "/tmp/test.mp3",
            "/tmp/test.aac",
            "/tmp/test.ogg",
        ] {
            let flags = seek_flags_for_path(Some(Path::new(path)));
            assert!(flags.contains(gst::SeekFlags::FLUSH), "{path}");
            assert!(flags.contains(gst::SeekFlags::ACCURATE), "{path}");
            assert!(!flags.contains(gst::SeekFlags::KEY_UNIT), "{path}");
        }
    }

    #[test]
    fn dts_seek_stays_key_unit_based() {
        let flags = seek_flags_for_path(Some(Path::new("/tmp/test.dts")));
        assert!(flags.contains(gst::SeekFlags::FLUSH));
        assert!(flags.contains(gst::SeekFlags::KEY_UNIT));
        assert!(!flags.contains(gst::SeekFlags::ACCURATE));
    }
}

fn clock_time(position: Duration) -> gst::ClockTime {
    gst::ClockTime::from_nseconds(u64::try_from(position.as_nanos()).unwrap_or(u64::MAX))
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
                Arc::new(Mutex::new(StreamEndHold::default())),
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

    fn at(millis: u64) -> MonotonicTime {
        MonotonicTime::from_nanos(1_000_000_000_000).saturating_add(Duration::from_millis(millis))
    }

    /// A runtime that just switched gaplessly from a 200 ms track, whose
    /// last sample read 150 ms at 0 ms, to a successor that starts at 50 ms.
    fn after_short_track_switch() -> TestEngine {
        let mut engine = TestEngine::new();
        let runtime = &mut engine.runtime;
        runtime.gapless_handover = Some(GaplessHandover::after(
            Duration::from_millis(150),
            at(0),
            Duration::from_millis(200),
            at(20),
        ));
        runtime.snapshot.position = Duration::ZERO;
        runtime.snapshot.position_sampled_at = at(50);
        runtime.snapshot.position_advancing = true;
        runtime.snapshot.duration = Duration::from_secs(4);
        engine
    }

    #[test]
    fn pause_after_a_short_track_stops_the_position_at_once() {
        let mut engine = after_short_track_switch();
        let runtime = &mut engine.runtime;
        // The successor's own position at 400 ms is within the tolerance of
        // the previous track's timeline, so it is taken for that track's.
        let successor = Duration::from_millis(350);
        assert!(runtime
            .gapless_handover
            .unwrap()
            .is_previous_stream(successor, at(400)));
        assert!(!runtime.apply_position_read(successor, at(400), true));
        assert!(runtime.snapshot.position_advancing);

        // Paused there: the position stops advancing at once, where the
        // sample from the switch has reached.
        assert!(runtime.apply_position_read(successor, at(400), false));
        assert!(!runtime.snapshot.position_advancing);
        assert_eq!(runtime.snapshot.position, successor);
        assert_eq!(runtime.snapshot.position_sampled_at, at(400));

        // And stays there, whether a reading is still taken for the
        // previous track's or no longer.
        for millis in [450, 600, 2_000] {
            assert!(!runtime.apply_position_read(successor, at(millis), false));
            assert!(!runtime.snapshot.position_advancing);
            assert_eq!(runtime.snapshot.position, successor);
        }
        assert_eq!(runtime.gapless_handover, None);
    }

    #[test]
    fn successor_reported_past_its_start_ends_the_gapless_handover() {
        let mut engine = after_short_track_switch();
        let runtime = &mut engine.runtime;
        // Before its start the successor reports its start.
        assert!(!runtime.apply_position_read(Duration::ZERO, at(30), true));
        assert!(runtime.gapless_handover.is_some());
        assert!(runtime.apply_position_read(Duration::from_millis(40), at(90), true));
        assert_eq!(runtime.gapless_handover, None);
        // Its later positions are taken as they are.
        assert!(runtime.apply_position_read(Duration::from_millis(350), at(400), false));
        assert_eq!(runtime.snapshot.position, Duration::from_millis(350));
        assert!(!runtime.snapshot.position_advancing);
    }

    #[test]
    fn previous_streams_reading_keeps_the_successors_sample_while_advancing() {
        // The Repeat-One replay from #187: the pipeline still answers from
        // the previous pass right after the switch.
        let mut engine = TestEngine::new();
        let runtime = &mut engine.runtime;
        runtime.gapless_handover = Some(GaplessHandover::after(
            Duration::from_millis(3_837),
            at(0),
            Duration::from_secs(4),
            at(3),
        ));
        runtime.snapshot.position = Duration::ZERO;
        runtime.snapshot.position_sampled_at = at(163);
        runtime.snapshot.position_advancing = true;
        runtime.snapshot.duration = Duration::from_secs(4);
        assert!(!runtime.apply_position_read(Duration::from_millis(3_900), at(60), true));
        assert_eq!(runtime.snapshot.position, Duration::ZERO);
        assert_eq!(runtime.snapshot.position_sampled_at, at(163));
        assert!(runtime.gapless_handover.is_some());
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
    use crate::playback::pipeline_trace::PipelineTrace;
    use crate::playback::PathRenames;

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

        /// A file of noise that no element can parse.
        fn corrupt(&self, name: &str) -> PathBuf {
            let mut state = 0x2545_f491_u32;
            let bytes: Vec<u8> = (0..32_768)
                .map(|_| {
                    state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                    state.to_be_bytes()[0]
                })
                .collect();
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

    impl Fixtures {
        /// MPEG-1 Layer III at 48 kHz without a Xing header, silent but for
        /// the tone of `seek-tone.mp3` from `MARKER_AT` on: 24 s of 32
        /// kbit/s frames, then 320 kbit/s frames. A seek that estimates
        /// its target from the bitrate lands seconds from it. (The parser
        /// seeks accurately anyway to a target less than 20 s after the
        /// audio it has read.)
        fn marked_mp3(&self, name: &str) -> PathBuf {
            let silent = |bitrate_index: u8, bitrate: usize| {
                let mut frame = vec![0xFF, 0xFB, (bitrate_index << 4) | 0x04, 0xC0];
                // Zeroed side information decodes as silence.
                frame.resize(144 * bitrate / 48_000, 0);
                frame
            };
            let mut bytes = Vec::new();
            for _ in 0..1_000 {
                bytes.extend(silent(1, 32_000));
            }
            for _ in 0..250 {
                bytes.extend(silent(14, 320_000));
            }
            bytes.extend(tone_frames());
            for _ in 0..100 {
                bytes.extend(silent(14, 320_000));
            }
            let path = self.dir.join(name);
            std::fs::write(&path, bytes).unwrap();
            path
        }
    }

    /// Where the tone of `marked_mp3` starts: after 1,250 frames of 24 ms.
    const MARKER_AT: Duration = Duration::from_secs(30);

    /// The audio frames of `seek-tone.mp3`, a 48 kHz mono tone, without its
    /// tags and its silent Info frame.
    fn tone_frames() -> Vec<u8> {
        const KBITS: [usize; 15] = [
            0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
        ];
        let bytes = include_bytes!("../analysis/fixtures/seek-tone.mp3");
        let tag_size = (bytes[6..10])
            .iter()
            .fold(0, |size, byte| (size << 7) | usize::from(*byte));
        let mut offset = 10 + tag_size;
        let mut frames = Vec::new();
        while offset + 4 <= bytes.len() && bytes[offset] == 0xFF && bytes[offset + 1] == 0xFB {
            assert_eq!(bytes[offset + 2] & 0x0C, 0x04, "48 kHz frames");
            let bitrate = KBITS[usize::from(bytes[offset + 2] >> 4)] * 1_000;
            let padding = usize::from((bytes[offset + 2] >> 1) & 1);
            let size = 144 * bitrate / 48_000 + padding;
            frames.push(&bytes[offset..offset + size]);
            offset += size;
        }
        assert!(frames.len() > 20, "{} frames", frames.len());
        frames[1..].concat()
    }

    /// The sink pad of the fake output that `playbin` plays to.
    fn output_sink_pad(playbin: &gst::Element) -> gst::Pad {
        let output = playbin
            .property::<gst::Element>("audio-sink")
            .downcast::<gst::Bin>()
            .unwrap();
        let sink = output
            .iterate_elements()
            .into_iter()
            .filter_map(Result::ok)
            .find(|element| {
                element
                    .factory()
                    .is_some_and(|factory| factory.name() == "fakesink")
            })
            .expect("fake output");
        sink.static_pad("sink").unwrap()
    }

    /// Holds every buffer before the output until it is released, so the
    /// pipeline can neither preroll nor land a seek it is given.
    struct OutputHold {
        pad: gst::Pad,
        probe: Option<gst::PadProbeId>,
    }

    impl OutputHold {
        fn new(playbin: &gst::Element) -> Self {
            let pad = output_sink_pad(playbin);
            let probe = pad.add_probe(
                gst::PadProbeType::BLOCK | gst::PadProbeType::BUFFER,
                |_, _| gst::PadProbeReturn::Ok,
            );
            assert!(probe.is_some());
            Self { pad, probe }
        }

        fn release(&mut self) {
            if let Some(probe) = self.probe.take() {
                self.pad.remove_probe(probe);
            }
        }
    }

    impl Drop for OutputHold {
        fn drop(&mut self) {
            self.release();
        }
    }

    /// Record the stream time at which the output first carries sound
    /// louder than a tenth of full scale.
    fn listen_for_sound(playbin: &gst::Element) -> Arc<Mutex<Option<Duration>>> {
        let heard = Arc::new(Mutex::new(None));
        let pad = output_sink_pad(playbin);
        let recorded = Arc::clone(&heard);
        pad.add_probe(gst::PadProbeType::BUFFER, move |pad, info| {
            let Some(gst::PadProbeData::Buffer(ref buffer)) = info.data else {
                return gst::PadProbeReturn::Ok;
            };
            let format = pad
                .current_caps()
                .and_then(|caps| Some(caps.structure(0)?.get::<String>("format").ok()?))
                .unwrap_or_default();
            let Ok(map) = buffer.map_readable() else {
                return gst::PadProbeReturn::Ok;
            };
            let loud = match format.as_str() {
                "S16LE" => map.chunks_exact(2).any(|sample| {
                    i16::from_le_bytes([sample[0], sample[1]]).unsigned_abs() > 3_277
                }),
                "S32LE" => map.chunks_exact(4).any(|sample| {
                    i32::from_le_bytes(sample.try_into().unwrap()).unsigned_abs() > 214_748_365
                }),
                "F32LE" => map
                    .chunks_exact(4)
                    .any(|sample| f32::from_le_bytes(sample.try_into().unwrap()).abs() > 0.1),
                "F64LE" => map
                    .chunks_exact(8)
                    .any(|sample| f64::from_le_bytes(sample.try_into().unwrap()).abs() > 0.1),
                other => panic!("unexpected sample format {other:?}"),
            };
            let stream_time = pad
                .sticky_event::<gst::event::Segment>(0)
                .and_then(|segment| {
                    segment
                        .segment()
                        .downcast_ref::<gst::ClockTime>()?
                        .to_stream_time(buffer.pts()?)
                });
            if let (true, Some(time), Ok(mut heard)) = (loud, stream_time, recorded.lock()) {
                heard.get_or_insert(Duration::from_nanos(time.nseconds()));
            }
            gst::PadProbeReturn::Ok
        });
        heard
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

    /// Records what the pipeline of a test does, to report it with the
    /// engine's state when a wait times out.
    struct Trace {
        pipeline: PipelineTrace,
        queue: Arc<Mutex<GaplessQueue>>,
        about_to_finish: Arc<AboutToFinishCalls>,
    }

    impl Trace {
        fn new(runtime: &GstPlaybackRuntime) -> Self {
            Self {
                pipeline: PipelineTrace::new(&runtime.playbin),
                queue: Arc::clone(&runtime.queue_state),
                about_to_finish: Arc::clone(&runtime.about_to_finish),
            }
        }

        /// Report `what` the test waited for, `engine`, a description of
        /// the engine, the queue, and what the pipeline does.
        fn report(&self, what: &str, engine: &str) -> String {
            let queue = match self.queue.try_lock() {
                Ok(queue) => format!(
                    "free, current row {:?}, successor handed to the pipeline: {}",
                    queue.current_index(),
                    queue.successor_handed_to_pipeline()
                ),
                Err(std::sync::TryLockError::WouldBlock) => "held".to_string(),
                Err(std::sync::TryLockError::Poisoned(_)) => "poisoned".to_string(),
            };
            let details = format!(
                "  about-to-finish: emitted {} times, the handler returned {} times\n\
                 {engine}  queue lock: {queue}\n",
                self.about_to_finish.emitted.load(Ordering::SeqCst),
                self.about_to_finish.returned.load(Ordering::SeqCst),
            );
            self.pipeline.report(what, &details)
        }
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
        /// The engine's pipeline, to post messages on its bus.
        playbin: gst::Element,
        /// The engine's queue, to seed its shuffle.
        queue: Arc<Mutex<GaplessQueue>>,
        snapshot: PlaybackSnapshot,
        snapshots: Vec<PlaybackSnapshot>,
        changes: Vec<Change>,
        errors: Vec<PlaybackError>,
        /// Positions of the `Seeked` events.
        seeked: Vec<Duration>,
        prepared: Vec<PathBuf>,
        cancelled_staging: usize,
        trace: Trace,
        /// When the engine last reported an event.
        last_event_at: Option<Instant>,
    }

    impl Engine {
        fn start() -> Self {
            Self::start_with_clock(clock::system_clock())
        }

        /// Start an engine that stamps its position samples with `clock`.
        fn start_with_clock(clock: MonotonicClock) -> Self {
            gst::init().unwrap();
            let (analysis_tx, analysis) = unbounded();
            let (event_tx, events) = unbounded();
            let (mut runtime, bus) =
                start_gst_engine(event_tx, analysis_tx, fake_output).expect("engine starts");
            runtime.clock = clock;
            let playbin = runtime.playbin.clone();
            let queue = Arc::clone(&runtime.queue_state);
            let trace = Trace::new(&runtime);
            let (commands, command_rx) = unbounded();
            std::thread::spawn(move || run_gst_engine(runtime, &bus, &command_rx));
            Self {
                commands,
                events,
                analysis,
                playbin,
                queue,
                snapshot: PlaybackSnapshot::default(),
                snapshots: Vec::new(),
                changes: Vec::new(),
                errors: Vec::new(),
                seeked: Vec::new(),
                prepared: Vec::new(),
                cancelled_staging: 0,
                trace,
                last_event_at: None,
            }
        }

        fn send(&self, command: PlaybackCommand) {
            self.commands.send(command).unwrap();
        }

        /// Describe what the engine and its pipeline do, for a wait for
        /// `what` that timed out.
        fn report(&self, what: &str) -> String {
            let snapshot = &self.snapshot;
            let engine = format!(
                "  last snapshot: {:?}, row {:?}, position {:?} of {:?}, current {:?}\n  \
                 last event {:?} ago\n  changes: {:?}\n  errors: {:?}\n  \
                 staged spectrograms: {:?}, cancelled {}\n",
                snapshot.state,
                snapshot.current_queue_index,
                snapshot.position,
                snapshot.duration,
                snapshot.current,
                self.last_event_at.map(|at| at.elapsed()),
                self.changes,
                self.errors,
                self.prepared,
                self.cancelled_staging,
            );
            self.trace.report(what, &engine)
        }

        /// Poll and record until `done` holds, or panic with a report of
        /// `what` was awaited.
        #[track_caller]
        fn wait_until(&mut self, what: &str, mut done: impl FnMut(&Self) -> bool) {
            let deadline = Instant::now() + TIMEOUT;
            loop {
                if done(self) {
                    return;
                }
                assert!(Instant::now() < deadline, "{}", self.report(what));
                self.send(PlaybackCommand::Poll);
                while let Ok(event) = self.events.recv_timeout(Duration::from_millis(20)) {
                    self.last_event_at = Some(Instant::now());
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
                    assert!(
                        snapshot.state == PlaybackState::Playing || !snapshot.position_advancing,
                        "only playback that plays advances: {snapshot:?}"
                    );
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
                PlaybackEvent::Error(error) => self.errors.push(error),
                PlaybackEvent::Seeked { position } => self.seeked.push(position),
                PlaybackEvent::PathsRemapped => {}
            }
        }

        /// Wait until about-to-finish staged `successor`.
        #[track_caller]
        fn wait_for_window(&mut self, successor: &Path) {
            let what = format!("about-to-finish did not stage {}", successor.display());
            self.wait_until(&what, |engine| {
                engine.prepared.iter().any(|path| path == successor)
            });
        }

        /// Wait inside the window until the current track has played 1 s.
        /// When about-to-finish fires, uridecodebin3 switches its input to
        /// the successor; a state change at that moment can deadlock
        /// uridecodebin3 (seen with GStreamer 1.28), independent of this
        /// engine.
        #[track_caller]
        fn wait_for_settled_window(&mut self, successor: &Path) {
            self.wait_for_window(successor);
            let row = self.snapshot.current_queue_index;
            self.wait_until("the current track did not play 1 s", |engine| {
                engine.snapshot.position >= Duration::from_secs(1)
            });
            assert_eq!(
                self.snapshot.current_queue_index, row,
                "the successor is not audible yet"
            );
        }

        #[track_caller]
        fn wait_for_errors(&mut self, count: usize) -> PlaybackError {
            self.wait_until(&format!("expected {count} errors"), |engine| {
                engine.errors.len() >= count
            });
            self.errors[count - 1].clone()
        }

        /// Post `message` on the pipeline's bus.
        fn post(&self, message: gst::Message) {
            self.playbin.bus().unwrap().post(message).unwrap();
        }

        /// Post an error from an element that every input shares, such as
        /// a decoder, so the error names no file.
        fn post_shared_error(&self, error: impl gst::MessageErrorDomain, text: &str) {
            self.post(
                gst::message::Error::builder(error, text)
                    .src(&self.playbin)
                    .build(),
            );
        }

        #[track_caller]
        fn wait_for_changes(&mut self, count: usize) -> Change {
            self.wait_until(&format!("expected {count} track changes"), |engine| {
                engine.changes.len() >= count
            });
            self.changes[count - 1].clone()
        }

        /// Wait until the snapshot shows row `index` with about `duration`,
        /// which identifies the fixture the pipeline plays.
        #[track_caller]
        fn wait_for_playing(&mut self, index: usize, duration: Duration) {
            self.wait_until(
                &format!("expected row {index} lasting {duration:?}"),
                |engine| {
                    engine.snapshot.current_queue_index == Some(index)
                        && engine.snapshot.duration.abs_diff(duration) < Duration::from_millis(100)
                },
            );
        }
    }

    #[derive(Debug, Clone, Copy)]
    enum Expect {
        Manual,
        Gapless,
        /// After a failure, which can leave only end of stream to start the
        /// next track.
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
    fn renamed_staged_track_keeps_its_duration() {
        // C is handed over while B plays, so B's duration comes from its
        // parser, which reports it under the name the pipeline opened.
        let fixtures = Fixtures::new("rename-staged");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 1_200);
        let c = fixtures.wav("c.wav", 4_500);
        let renamed = fixtures.dir.join("renamed b.wav");
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_window(&b);
        // Well after the pipeline opened B, before A ends.
        engine.wait_until("the position did not reach 3 s", |engine| {
            engine.snapshot.position >= millis(3_000)
        });
        assert_eq!(engine.snapshot.current_queue_index, Some(0));

        std::fs::rename(&b, &renamed).unwrap();
        engine.send(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
            (b, renamed.clone()),
        ]))));
        assert_change(&engine.wait_for_changes(2), &renamed, 1, Expect::Gapless);
        engine.wait_for_playing(1, millis(1_200));
    }

    #[test]
    fn many_short_tracks_keep_their_durations() {
        // The pipeline reads ahead by time and bytes, not by tracks, so
        // every short track is handed over before the first is audible.
        let fixtures = Fixtures::new("many-short");
        let mut queue = vec![fixtures.wav("a.wav", 4_000)];
        queue.extend((0..15).map(|index| fixtures.wav(&format!("short {index:02}.wav"), 100)));
        queue.push(fixtures.wav("last.wav", 4_500));
        let last = queue.len() - 1;
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(queue.clone()));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_changes(queue.len());
        engine.wait_for_playing(last, millis(4_500));

        for (index, path) in queue.iter().enumerate().take(last).skip(1) {
            let change = &engine.changes[index];
            assert_change(change, path, index, Expect::Gapless);
            let durations: Vec<_> = engine
                .snapshots
                .iter()
                .filter(|snapshot| snapshot.track_token == change.token)
                .map(|snapshot| snapshot.duration)
                .collect();
            assert!(
                !durations.is_empty()
                    && durations
                        .iter()
                        .all(|duration| duration.abs_diff(millis(100)) < millis(20)),
                "row {index}: {durations:?}"
            );
        }
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
        engine.wait_until("the staged spectrogram was not cancelled", |engine| {
            engine.cancelled_staging == 1
        });
        assert_eq!(engine.snapshot.current.as_deref(), Some(a.as_path()));
        assert_eq!(engine.snapshot.current_queue_index, Some(0));

        assert_change(&engine.wait_for_changes(2), &c, 1, Expect::Gapless);
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
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
        assert!(
            sought.elapsed() >= millis(3_000),
            "the successor became current {:?} after the seek",
            sought.elapsed()
        );
    }

    #[test]
    fn stopped_track_is_not_seekable_and_plays_from_its_start() {
        let fixtures = Fixtures::new("stopped-seek");
        let a = fixtures.wav("a.wav", 6_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone()]));
        engine.send(PlaybackCommand::Seek(millis(3_000)));
        engine.wait_until("the seek was not acknowledged", |engine| {
            engine.snapshot.seek_serial == 1
        });
        assert_eq!(engine.snapshot.state, PlaybackState::Stopped);
        assert_eq!(engine.snapshot.position, Duration::ZERO);
        assert_eq!(engine.snapshot.seek_landed_serial, 0);
        assert!(engine.seeked.is_empty());

        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(6_000));
        engine.wait_until("the position did not advance", |engine| {
            engine.snapshot.position > Duration::ZERO
        });
        assert!(
            engine.snapshot.position < millis(1_500),
            "{:?}",
            engine.snapshot
        );
        assert!(engine.seeked.is_empty());

        // Paused, the track is seekable, and Play goes on from there.
        engine.send(PlaybackCommand::Pause);
        engine.send(PlaybackCommand::Seek(millis(3_000)));
        engine.wait_until("the second seek did not land", |engine| {
            engine.snapshot.seek_landed_serial == 2
        });
        assert_eq!(engine.seeked.len(), 1);
        assert!(
            engine.seeked[0].abs_diff(millis(3_000)) < millis(50),
            "{:?}",
            engine.seeked
        );
        assert_eq!(engine.snapshot.position, engine.seeked[0]);
        assert_eq!(engine.snapshot.state, PlaybackState::Paused);
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the position did not reach 3.3 s", |engine| {
            engine.snapshot.position >= millis(3_300)
        });
        assert!(
            engine.snapshot.position < millis(4_500),
            "{:?}",
            engine.snapshot
        );
    }

    #[test]
    fn seek_lands_where_the_audio_it_plays_is() {
        // Without the accurate flag, the MP3 parser estimates where the
        // target is from the bitrate and labels the audio found there with
        // the target time: the playhead would show the target while the
        // audio comes from elsewhere.
        assert_mp3_playback();
        let fixtures = Fixtures::new("seek-landing");
        let track = fixtures.marked_mp3("marked.mp3");
        let mut engine = Engine::start();
        let heard = listen_for_sound(&engine.playbin);
        engine.send(PlaybackCommand::LoadQueue(vec![track]));
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the position did not reach 0.3 s", |engine| {
            engine.snapshot.position >= millis(300)
        });
        assert_eq!(*heard.lock().unwrap(), None);

        let target = MARKER_AT - millis(400);
        engine.send(PlaybackCommand::Seek(target));
        engine.wait_until("the seek did not land", |engine| {
            engine.snapshot.seek_landed_serial == 1
        });
        assert_eq!(engine.seeked.len(), 1);
        let landing = engine.seeked[0];
        assert!(
            landing.abs_diff(target) < millis(50),
            "landed at {landing:?}"
        );
        let landed = engine
            .snapshots
            .iter()
            .find(|snapshot| snapshot.seek_landed_serial == 1)
            .unwrap();
        assert_eq!(landed.position, landing);

        engine.wait_until(
            &format!("the tone at {MARKER_AT:?} was not heard after seeking to {target:?}"),
            |_| heard.lock().unwrap().is_some(),
        );
        let heard_at = heard.lock().unwrap().unwrap();
        // Within a frame or two: the tone's first frame refers to audio
        // before it.
        assert!(
            heard_at.abs_diff(MARKER_AT) < millis(100),
            "the tone at {MARKER_AT:?} plays at {heard_at:?}"
        );
    }

    #[test]
    fn seeks_that_wait_for_the_pipeline_land_only_at_the_last_target() {
        let fixtures = Fixtures::new("seek-burst");
        let a = fixtures.wav("a.wav", 8_000);
        let mut engine = DirectEngine::start();
        engine.play_until_opened(vec![a], RepeatMode::Off);
        engine.wait_until("the position did not reach 0.2 s", |runtime| {
            runtime.snapshot.position >= millis(200)
        });
        let before = engine.runtime.snapshot.position;
        let _ = engine.events.try_iter().count();

        // The first seek is in flight when the others arrive; they replace
        // each other while they wait.
        for target in [1_000, 2_000, 5_000, 3_000] {
            engine
                .runtime
                .apply_command(PlaybackCommand::Seek(millis(target)));
        }
        assert_eq!(
            engine.runtime.seek_in_flight.map(|seek| seek.serial),
            Some(1)
        );
        assert_eq!(
            engine.runtime.queued_seek,
            Some(SeekTarget {
                position: millis(3_000),
                serial: 4
            })
        );
        // The position stands where the first seek found it.
        let held = engine.runtime.snapshot.clone();
        assert!(!held.position_advancing, "{held:?}");
        assert!(held.position >= before, "{held:?}");
        engine.wait_until("the seeks were not acknowledged", |runtime| {
            runtime.snapshot.seek_serial == 4
        });
        assert!(!engine.runtime.seek_pending());

        let events: Vec<_> = engine.events.try_iter().collect();
        let seeked: Vec<_> = events
            .iter()
            .filter_map(|event| match event {
                PlaybackEvent::Seeked { position } => Some(*position),
                _ => None,
            })
            .collect();
        assert_eq!(seeked.len(), 1, "{seeked:?}");
        assert!(seeked[0].abs_diff(millis(3_000)) < millis(50), "{seeked:?}");
        // No snapshot shows where an earlier seek landed.
        for event in &events {
            if let PlaybackEvent::Snapshot(snapshot) = event {
                assert!(
                    snapshot.position == held.position || snapshot.position >= seeked[0],
                    "{snapshot:?}"
                );
                assert!(snapshot.seek_landed_serial == 0 || snapshot.seek_landed_serial == 4);
            }
        }
        assert_eq!(engine.runtime.snapshot.seek_landed_serial, 4);
    }

    #[test]
    fn seek_before_the_pipeline_prerolls_lands_once_it_has() {
        let fixtures = Fixtures::new("seek-preroll");
        let a = fixtures.wav("a.wav", 6_000);
        let mut engine = DirectEngine::start();
        // The pipeline cannot preroll until the seek waits for it.
        let mut hold = OutputHold::new(&engine.runtime.playbin);
        engine
            .runtime
            .apply_command(PlaybackCommand::LoadQueue(vec![a]));
        engine.runtime.apply_command(PlaybackCommand::Play);
        engine
            .runtime
            .apply_command(PlaybackCommand::Seek(millis(3_000)));
        assert!(engine.runtime.queued_seek.is_some());
        assert!(engine.runtime.seek_in_flight.is_none());
        assert!(engine.runtime.playbin.current_state() < gst::State::Paused);
        hold.release();
        engine.wait_until("the seek did not land", |runtime| {
            runtime.snapshot.seek_landed_serial == 1
        });
        assert!(
            engine.runtime.snapshot.position.abs_diff(millis(3_000)) < millis(50),
            "{:?}",
            engine.runtime.snapshot
        );
        engine.wait_until("the position did not reach 3.2 s", |runtime| {
            runtime.snapshot.position >= millis(3_200)
        });
    }

    #[test]
    fn play_during_a_restart_waits_for_its_seek() {
        let fixtures = Fixtures::new("restart-play");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = DirectEngine::start();
        engine.play_until_opened(vec![a, b], RepeatMode::Off);
        // Well after the pipeline was given B.
        engine.wait_until("the successor was not handed over by 3 s", |runtime| {
            runtime
                .queue_state
                .lock()
                .unwrap()
                .successor_handed_to_pipeline()
                && runtime.snapshot.position >= millis(3_000)
        });
        engine.runtime.apply_command(PlaybackCommand::Pause);
        engine
            .runtime
            .apply_command(PlaybackCommand::Seek(millis(500)));
        assert_eq!(engine.runtime.restart, Some(RunRestart::Prerolling));
        engine.runtime.apply_command(PlaybackCommand::Play);
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);
        assert!(engine.runtime.playbin.current_state() <= gst::State::Paused);
        engine.wait_until("the seek did not land", |runtime| {
            runtime.snapshot.seek_landed_serial == 1
        });
        assert!(
            engine.runtime.snapshot.position.abs_diff(millis(500)) < millis(50),
            "{:?}",
            engine.runtime.snapshot
        );
        assert_eq!(engine.runtime.restart, None);
        engine.wait_until("the position did not reach 0.7 s", |runtime| {
            runtime.snapshot.position >= millis(700)
        });
        assert_eq!(engine.runtime.playbin.current_state(), gst::State::Playing);
        assert_eq!(engine.runtime.snapshot.current_queue_index, Some(0));
    }

    #[test]
    fn seek_that_takes_longer_than_playback_may_take_to_start_lands_and_plays() {
        let fixtures = Fixtures::new("slow-seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = DirectEngine::start();
        let mut hold = engine.hold_seek_of_a_restart(vec![a, b]);
        // However long the seek takes, the track has not failed to start.
        engine.runtime.playing_state_timeout = Duration::ZERO;
        for _ in 0..3 {
            engine.handle_posted_messages();
        }
        assert!(engine.errors().is_empty());
        assert!(engine.runtime.seek_in_flight.is_some());
        assert_eq!(engine.runtime.snapshot.current_queue_index, Some(0));
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);

        // Playback then has the usual time to start.
        engine.runtime.playing_state_timeout = GstPlaybackRuntime::PLAYING_STATE_TIMEOUT;
        hold.release();
        engine.wait_until("the seek did not land", |runtime| {
            runtime.snapshot.seek_landed_serial == 1
        });
        assert!(
            engine.runtime.snapshot.position.abs_diff(millis(500)) < millis(50),
            "{:?}",
            engine.runtime.snapshot
        );
        engine.wait_until("the position did not reach 0.7 s", |runtime| {
            runtime.snapshot.position >= millis(700)
        });
        assert_eq!(engine.runtime.playbin.current_state(), gst::State::Playing);
        assert_eq!(engine.runtime.snapshot.current_queue_index, Some(0));
        assert!(engine.errors().is_empty());
    }

    #[test]
    fn seek_that_fails_in_flight_is_reported_and_skipped() {
        let fixtures = Fixtures::new("failed-seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = DirectEngine::start();
        let mut hold = engine.hold_seek_of_a_restart(vec![a.clone(), b]);
        engine.runtime.playing_state_timeout = Duration::ZERO;
        engine.handle_posted_messages();
        assert!(engine.errors().is_empty());
        engine.runtime.playing_state_timeout = GstPlaybackRuntime::PLAYING_STATE_TIMEOUT;

        let source = engine.file_source();
        engine.fail_reading(&source);
        let errors = engine.errors();
        assert_eq!(errors.len(), 1, "{errors:?}");
        assert_eq!(errors[0].path.as_deref(), Some(a.as_path()));
        assert_eq!(errors[0].detail, "injected read failure");
        assert_eq!(errors[0].outcome, PlaybackErrorOutcome::Skipped);
        assert!(!engine.runtime.seek_pending());
        assert_eq!(engine.runtime.restart, None);
        assert_eq!(engine.runtime.snapshot.seek_serial, 1);
        assert_eq!(engine.runtime.snapshot.seek_landed_serial, 0);

        hold.release();
        engine.wait_until("row 1 did not play 0.2 s", |runtime| {
            runtime.snapshot.current_queue_index == Some(1)
                && runtime.snapshot.position >= millis(200)
        });
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);
        assert!(engine.errors().is_empty());
    }

    #[test]
    fn bursts_merge_seeks_and_count_every_next_press() {
        let merged = merge_burst(vec![
            PlaybackCommand::Seek(millis(1_000)),
            PlaybackCommand::Poll,
            PlaybackCommand::Seek(millis(2_000)),
            PlaybackCommand::Seek(millis(3_000)),
            PlaybackCommand::Next,
            PlaybackCommand::Poll,
            PlaybackCommand::Next,
            PlaybackCommand::Previous,
            PlaybackCommand::Next,
            PlaybackCommand::Pause,
            PlaybackCommand::Next,
            PlaybackCommand::Seek(millis(4_000)),
        ]);
        let summary: Vec<String> = merged
            .iter()
            .map(|step| match step {
                EngineStep::Command(command) => format!("{command:?}"),
                EngineStep::Seek { position, requests } => {
                    format!("seek {} x{requests}", position.as_millis())
                }
                EngineStep::Navigate(moves) => format!("navigate {moves:?}"),
            })
            .collect();
        assert_eq!(
            summary,
            [
                "seek 3000 x3",
                "navigate [true, true, false, true]",
                "Pause",
                "navigate [true]",
                "seek 4000 x1",
                "Poll",
            ]
        );
    }

    #[test]
    fn next_presses_in_a_burst_start_only_the_track_they_end_on() {
        let fixtures = Fixtures::new("next-burst");
        let queue: Vec<_> = (0..5)
            .map(|index| fixtures.wav(&format!("{index}.wav"), 4_000 + 500 * index))
            .collect();
        let mut engine = DirectEngine::start();
        engine.play_until_opened(queue.clone(), RepeatMode::Off);
        let _ = engine.events.try_iter().count();
        engine
            .runtime
            .apply_step(EngineStep::Navigate(vec![true, true, true]));
        let changes: Vec<_> = engine
            .events
            .try_iter()
            .filter_map(|event| match event {
                PlaybackEvent::TrackChanged {
                    path,
                    queue_index,
                    kind,
                    ..
                } => Some((path, queue_index, kind)),
                _ => None,
            })
            .collect();
        assert_eq!(changes.len(), 1, "{changes:?}");
        assert_eq!((&changes[0].0, changes[0].1), (&queue[3], 3));
        assert!(matches!(changes[0].2, TrackChangeKind::Manual));
        engine.wait_until("the last track did not start", |runtime| {
            runtime.snapshot.duration.abs_diff(millis(5_500)) < millis(100)
        });

        // From the bridge, however the presses fall into bursts.
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(queue.clone()));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));
        for _ in 0..3 {
            engine.send(PlaybackCommand::Next);
        }
        engine.send(PlaybackCommand::Previous);
        engine.wait_for_playing(2, millis(5_000));
        let rows: Vec<_> = engine.changes.iter().map(|change| change.index).collect();
        assert_eq!(rows.first(), Some(&0));
        assert_eq!(rows.last(), Some(&2));
        assert!(rows.len() <= 5, "{rows:?}");
        assert!(engine
            .changes
            .iter()
            .all(|change| matches!(change.kind, TrackChangeKind::Manual)));
    }

    #[test]
    fn seek_in_a_short_track_keeps_its_successor_gapless() {
        // B is shorter than the pipeline reads ahead, so the pipeline was
        // given C before B became audible, and the seek restarts B. The new
        // run reads all of B while it prerolls, before its seek.
        let fixtures = Fixtures::new("short-seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 1_500);
        let c = fixtures.wav("c.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c.clone()]));
        engine.send(PlaybackCommand::Play);
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
        engine.send(PlaybackCommand::Seek(millis(300)));
        let sought = Instant::now();
        assert_change(&engine.wait_for_changes(3), &c, 2, Expect::Gapless);
        assert!(
            sought.elapsed() >= millis(1_000),
            "C became current {:?} after the seek",
            sought.elapsed()
        );
        assert_eq!(engine.seeked.len(), 1);
        assert!(
            engine.seeked[0].abs_diff(millis(300)) < millis(50),
            "{:?}",
            engine.seeked
        );
        engine.wait_for_playing(2, millis(4_500));
    }

    #[test]
    fn repeat_one_replays_gaplessly_after_seeks() {
        let fixtures = Fixtures::new("repeat-seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::One));
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b]));
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the position did not reach 0.3 s", |engine| {
            engine.snapshot.position >= millis(300)
        });
        // Before the pipeline asked for the replay, then inside its window.
        engine.send(PlaybackCommand::Seek(millis(2_500)));
        let replay = engine.wait_for_changes(2);
        assert_change(&replay, &a, 0, Expect::Gapless);
        engine.prepared.clear();
        engine.wait_for_settled_window(&a);
        engine.send(PlaybackCommand::Seek(millis(2_000)));
        let sought = Instant::now();
        let second = engine.wait_for_changes(3);
        assert_change(&second, &a, 0, Expect::Gapless);
        assert!(sought.elapsed() >= millis(1_500), "{:?}", sought.elapsed());
        assert_ne!(second.token, replay.token);
        assert_eq!(engine.seeked.len(), 2, "{:?}", engine.seeked);
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
        assert_change(&engine.wait_for_changes(2), &a, 0, Expect::Gapless);
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
        engine.wait_until("the replay did not become current", |engine| {
            engine.snapshot.track_token == replay.token
        });
        assert_eq!(engine.snapshot.current_queue_index, Some(0));
        assert!(engine.snapshot.position < millis(1_000));
        assert_eq!(engine.changes.len(), 2, "one track change per play");
    }

    /// How far ahead of `CLOCK_MONOTONIC` the test clock reads, so that a
    /// time stamp tells which clock it came from.
    const CLOCK_SHIFT: Duration = Duration::from_secs(1_000_000);

    /// `CLOCK_MONOTONIC` shifted by [`CLOCK_SHIFT`]. It runs at the rate of
    /// the pipeline clock, so samples stamped with it can be extrapolated.
    fn shifted_clock() -> MonotonicClock {
        Arc::new(|| clock::monotonic_now().saturating_add(CLOCK_SHIFT))
    }

    /// How much later than it was read a position may be stamped, or how
    /// much later a sink may report it, when the scheduler delays the
    /// engine or the pipeline under load.
    const SAMPLE_SLACK: Duration = Duration::from_millis(150);
    /// How far a position may fall behind real time between two samples
    /// that both advance: a starved sink holds its position until it has
    /// rendered the next buffer.
    const STALL_SLACK: Duration = Duration::from_millis(300);

    fn signed_millis(value: Duration) -> i128 {
        i128::try_from(value.as_micros()).unwrap() / 1_000
    }

    /// Check that consecutive snapshots describe one playback clock that
    /// the UI can extrapolate: time stamps come from `clock` and do not go
    /// back; within a track and seek, the position neither runs ahead of
    /// real time nor goes back, and keeps up with it while it advances;
    /// a gapless switch goes on where the previous track ended.
    #[track_caller]
    fn assert_continuous(snapshots: &[PlaybackSnapshot], now: MonotonicTime) {
        let shifted_epoch = MonotonicTime::default().saturating_add(CLOCK_SHIFT);
        let violations: Vec<String> = snapshots
            .windows(2)
            .filter_map(|pair| {
                let (previous, next) = (&pair[0], &pair[1]);
                let elapsed = next
                    .position_sampled_at
                    .saturating_duration_since(previous.position_sampled_at);
                let position = signed_millis(next.position);
                let start = signed_millis(previous.position);
                let elapsed_ms = signed_millis(elapsed);
                let slack = signed_millis(SAMPLE_SLACK);
                let went_back = previous
                    .position_sampled_at
                    .saturating_duration_since(next.position_sampled_at);
                let problem = if went_back > SAMPLE_SLACK {
                    // Only a predicted start of a gapless successor may be
                    // later than the sample after it.
                    Some("time stamp went back".to_string())
                } else if next.position_advancing
                    && !(shifted_epoch..=now).contains(&next.position_sampled_at)
                {
                    Some("time stamp is not from the injected clock".to_string())
                } else if next.track_token != previous.track_token {
                    // A gapless switch: the new track starts where the
                    // previous one ended.
                    let gapless = previous.position_advancing
                        && next.position_advancing
                        && !previous.duration.is_zero();
                    let expected = start + elapsed_ms - signed_millis(previous.duration);
                    (gapless && (position - expected).abs() > slack).then(|| {
                        format!("a gapless switch landed at {position} ms, not {expected} ms")
                    })
                } else if next.seek_landed_serial != previous.seek_landed_serial {
                    // A seek landed; its target is checked by the test.
                    None
                } else if position > start + elapsed_ms + slack {
                    Some(format!("ran ahead of real time ({elapsed_ms} ms)"))
                } else if position + slack < start {
                    Some("went back".to_string())
                } else if previous.position_advancing
                    && next.position_advancing
                    && position + signed_millis(STALL_SLACK) < start + elapsed_ms
                {
                    Some(format!("fell behind real time ({elapsed_ms} ms)"))
                } else {
                    None
                };
                problem.map(|problem| {
                    format!(
                        "{problem}: {:?} at {} -> {:?} at {} (token {} -> {}, advancing {} -> {})",
                        previous.position,
                        previous.position_sampled_at.as_nanos(),
                        next.position,
                        next.position_sampled_at.as_nanos(),
                        previous.track_token,
                        next.track_token,
                        previous.position_advancing,
                        next.position_advancing,
                    )
                })
            })
            .collect();
        assert!(
            violations.is_empty(),
            "{violations:#?}\nsnapshots (position, time stamp, advancing, token):\n{}",
            snapshots
                .iter()
                .map(|snapshot| format!(
                    "  {:?} {} {} {}",
                    snapshot.position,
                    snapshot.position_sampled_at.as_nanos(),
                    snapshot.position_advancing,
                    snapshot.track_token
                ))
                .collect::<Vec<_>>()
                .join("\n")
        );
    }

    #[test]
    fn position_samples_are_the_raw_pipeline_position_at_their_time_stamp() {
        let fixtures = Fixtures::new("raw-position");
        let a = fixtures.wav("a.wav", 6_000);
        let clock = shifted_clock();
        let mut engine = DirectEngine::start_with_clock(Arc::clone(&clock));
        engine.play_until_opened(vec![a], RepeatMode::Off);
        engine.wait_until("the position did not advance from 0.3 s", |runtime| {
            runtime.snapshot.position_advancing && runtime.snapshot.position >= millis(300)
        });
        for _ in 0..10 {
            let before = clock();
            engine.runtime.poll();
            let after = clock();
            let queried = engine
                .runtime
                .playbin
                .query_position::<gst::ClockTime>()
                .map(|position| Duration::from_nanos(position.nseconds()))
                .unwrap();
            let queried_at = clock();
            let snapshot = &engine.runtime.snapshot;
            assert!(snapshot.position_advancing, "{snapshot:?}");
            assert!(
                (before..=after).contains(&snapshot.position_sampled_at),
                "{snapshot:?} was not stamped while it was polled"
            );
            // The pipeline advanced from the sample in real time, which an
            // estimate that smooths or extrapolates the position would not
            // match.
            let advanced = queried.checked_sub(snapshot.position);
            let window = queried_at.saturating_duration_since(before);
            assert!(
                advanced.is_some_and(|advanced| advanced <= window + millis(2)),
                "the pipeline was at {queried:?} {window:?} after a sample of {:?}",
                snapshot.position
            );
        }
        let events: Vec<_> = engine.events.try_iter().collect();
        assert!(events
            .iter()
            .any(|event| matches!(event, PlaybackEvent::Snapshot(_))));
    }

    #[test]
    fn position_stays_continuous_across_a_seek_a_pause_and_a_gapless_switch() {
        let fixtures = Fixtures::new("continuous");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let clock = shifted_clock();
        let mut engine = Engine::start_with_clock(Arc::clone(&clock));
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the position did not advance from 0.3 s", |engine| {
            engine.snapshot.position_advancing && engine.snapshot.position >= millis(300)
        });

        engine.send(PlaybackCommand::Seek(millis(1_500)));
        engine.wait_until("the seek did not land", |engine| {
            engine.snapshot.seek_landed_serial == 1
        });
        let landed = engine
            .snapshots
            .iter()
            .find(|snapshot| snapshot.seek_landed_serial == 1)
            .unwrap()
            .clone();
        assert!(
            landed.position.abs_diff(millis(1_500)) < millis(50),
            "{landed:?}"
        );
        engine.wait_until("playback did not go on from the seek", |engine| {
            engine.snapshot.position_advancing && engine.snapshot.position >= millis(1_800)
        });

        engine.send(PlaybackCommand::Pause);
        engine.wait_until("playback did not pause", |engine| {
            engine.snapshot.state == PlaybackState::Paused
        });
        let paused = engine.snapshot.clone();
        assert!(!paused.position_advancing);
        engine.send(PlaybackCommand::Play);
        engine.wait_until("playback did not resume", |engine| {
            engine.snapshot.position_advancing
        });
        let resumed = engine.snapshot.clone();
        // Playback resumes where it paused, however long it was paused.
        assert!(
            resumed.position >= paused.position
                && resumed.position
                    <= paused.position
                        + resumed
                            .position_sampled_at
                            .saturating_duration_since(clock())
                        + SAMPLE_SLACK,
            "paused at {paused:?}, resumed at {resumed:?}"
        );

        let change = engine.wait_for_changes(2);
        assert_change(&change, &b, 1, Expect::Gapless);
        engine.wait_until("the successor did not play 0.3 s", |engine| {
            engine.snapshot.track_token == change.token
                && engine.snapshot.position_advancing
                && engine.snapshot.position >= millis(300)
        });
        assert_continuous(&engine.snapshots, clock());
    }

    #[test]
    fn position_stays_continuous_across_a_repeat_one_replay() {
        let fixtures = Fixtures::new("continuous-repeat");
        let a = fixtures.wav("a.wav", 4_000);
        let clock = shifted_clock();
        let mut engine = Engine::start_with_clock(Arc::clone(&clock));
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::One));
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone()]));
        engine.send(PlaybackCommand::Play);
        let replay = engine.wait_for_changes(2);
        assert_change(&replay, &a, 0, Expect::Gapless);
        engine.wait_until("the replay did not play 0.3 s", |engine| {
            engine.snapshot.track_token == replay.token
                && engine.snapshot.position_advancing
                && engine.snapshot.position >= millis(300)
        });
        assert_continuous(&engine.snapshots, clock());
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
        engine.wait_until("row 1 did not become current", |engine| {
            engine.snapshot.current_queue_index == Some(1)
        });
        assert_eq!(engine.snapshot.track_token, second.token);
        assert_eq!(engine.changes.len(), 2, "one track change per play");
    }

    fn assert_mp3_playback() {
        gst::init().unwrap();
        assert!(
            gst::ElementFactory::find("mpegaudioparse").is_some()
                && ["mpg123audiodec", "avdec_mp3", "avdec_mp3float"]
                    .iter()
                    .any(|name| gst::ElementFactory::find(name).is_some()),
            "install an MP3 parser and decoder (GStreamer good and libav plugins)"
        );
    }

    #[test]
    fn duration_refinements_apply_after_a_gapless_switch() {
        assert_mp3_playback();
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

    #[test]
    fn corrupt_current_track_is_reported_and_skipped() {
        let fixtures = Fixtures::new("corrupt-current");
        let bad = fixtures.corrupt("bad.wav");
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![bad.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        let error = engine.wait_for_errors(1);
        assert_eq!(error.path.as_deref(), Some(bad.as_path()));
        assert_eq!(error.outcome, PlaybackErrorOutcome::Skipped);
        assert!(!error.detail.is_empty());
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Automatic);
        engine.wait_for_playing(1, millis(4_500));
        assert_eq!(engine.snapshot.state, PlaybackState::Playing);
    }

    #[test]
    fn corrupt_track_in_the_middle_is_skipped_after_the_track_before_it() {
        // The corrupt track is handed to the pipeline as the successor of
        // A and fails to open while A plays.
        let fixtures = Fixtures::new("corrupt-successor");
        let a = fixtures.wav("a.wav", 4_000);
        let bad = fixtures.corrupt("bad.wav");
        let c = fixtures.wav("c.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, bad.clone(), c.clone()]));
        engine.send(PlaybackCommand::Play);
        let started = Instant::now();
        let error = engine.wait_for_errors(1);
        assert_eq!(error.path.as_deref(), Some(bad.as_path()));
        assert_eq!(error.outcome, PlaybackErrorOutcome::Skipped);

        let change = engine.wait_for_changes(2);
        assert_change(&change, &c, 2, Expect::Automatic);
        assert!(
            started.elapsed() >= millis(3_500),
            "A was cut short: C started {:?} after A",
            started.elapsed()
        );
        engine.wait_for_playing(2, millis(4_500));
        assert_eq!(engine.errors.len(), 1);
        assert!(engine
            .snapshots
            .iter()
            .all(|snapshot| snapshot.current_queue_index != Some(1)));
    }

    #[test]
    fn successor_renamed_before_its_remap_is_reported_and_skipped() {
        // The rename reaches the engine only after the pipeline failed to
        // open the old path.
        let fixtures = Fixtures::new("rename-before-remap");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let renamed = fixtures.dir.join("renamed b.wav");
        std::fs::rename(&b, &renamed).unwrap();
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a, b.clone(), c.clone()]));
        engine.send(PlaybackCommand::Play);
        let started = Instant::now();
        let error = engine.wait_for_errors(1);
        assert_eq!(error.path.as_deref(), Some(b.as_path()));
        assert_eq!(error.outcome, PlaybackErrorOutcome::Skipped);
        assert_change(&engine.wait_for_changes(2), &c, 2, Expect::Automatic);
        assert!(
            started.elapsed() >= millis(3_500),
            "A was cut short: C started {:?} after A",
            started.elapsed()
        );

        // Once the rename arrives, the track plays from its new path.
        engine.send(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
            (b, renamed.clone()),
        ]))));
        engine.send(PlaybackCommand::PlayAt(1));
        assert_change(&engine.wait_for_changes(3), &renamed, 1, Expect::Manual);
        engine.wait_for_playing(1, millis(4_500));
        assert_eq!(engine.errors.len(), 1);
    }

    /// The runtime without its thread, so a test decides when it handles
    /// each bus message.
    struct DirectEngine {
        runtime: GstPlaybackRuntime,
        bus: gst::Bus,
        events: Receiver<PlaybackEvent>,
        _analysis: Receiver<AnalysisCommand>,
        trace: Trace,
    }

    impl DirectEngine {
        fn start() -> Self {
            Self::start_with_clock(clock::system_clock())
        }

        /// Start a runtime that stamps its position samples with `clock`.
        fn start_with_clock(clock: MonotonicClock) -> Self {
            gst::init().unwrap();
            let (analysis_tx, analysis) = unbounded();
            let (event_tx, events) = unbounded();
            let (mut runtime, bus) =
                start_gst_engine(event_tx, analysis_tx, fake_output).expect("engine starts");
            runtime.clock = clock;
            let trace = Trace::new(&runtime);
            Self {
                runtime,
                bus,
                events,
                _analysis: analysis,
                trace,
            }
        }

        /// Describe what the runtime and its pipeline do, for a wait for
        /// `what` that timed out.
        fn report(&self, what: &str) -> String {
            let runtime = &self.runtime;
            let snapshot = &runtime.snapshot;
            let engine = format!(
                "  snapshot: {:?}, row {:?}, position {:?} of {:?}, current {:?}\n  \
                 restart {:?}, queued seek {:?}, seek in flight {:?}, Playing requested \
                 {:?} ago, awaiting the run's stream start: {}\n",
                snapshot.state,
                snapshot.current_queue_index,
                snapshot.position,
                snapshot.duration,
                snapshot.current,
                runtime.restart,
                runtime.queued_seek,
                runtime.seek_in_flight,
                runtime.playing_state_requested_at.map(|at| at.elapsed()),
                runtime.awaiting_run_stream_start,
            );
            self.trace.report(what, &engine)
        }

        /// Handle bus messages until one matches `stop`, which is returned
        /// unhandled. Panics with a report of `what` was awaited on
        /// timeout.
        #[track_caller]
        fn handle_messages_until(
            &mut self,
            what: &str,
            mut stop: impl FnMut(&gst::Message) -> bool,
        ) -> gst::Message {
            let deadline = Instant::now() + TIMEOUT;
            while Instant::now() < deadline {
                let Some(message) = self.bus.timed_pop(gst::ClockTime::from_mseconds(20)) else {
                    continue;
                };
                if stop(&message) {
                    return message;
                }
                self.runtime.handle_bus_message(&message);
            }
            panic!("{}", self.report(&format!("no {what} on the bus")));
        }

        /// Handle bus messages and poll until `done` holds, or panic with a
        /// report of `what` was awaited.
        #[track_caller]
        fn wait_until(&mut self, what: &str, mut done: impl FnMut(&GstPlaybackRuntime) -> bool) {
            let deadline = Instant::now() + TIMEOUT;
            while !done(&self.runtime) {
                assert!(Instant::now() < deadline, "{}", self.report(what));
                if let Some(message) = self.bus.timed_pop(gst::ClockTime::from_mseconds(20)) {
                    self.runtime.handle_bus_message(&message);
                }
                self.runtime.poll();
            }
        }

        /// Play `queue` until the pipeline has opened the first track, and
        /// return the file source that reads it.
        fn play_until_opened(&mut self, queue: Vec<PathBuf>, mode: RepeatMode) -> gst::Element {
            let first = queue[0].clone();
            self.runtime
                .apply_command(PlaybackCommand::SetRepeatMode(mode));
            self.runtime
                .apply_command(PlaybackCommand::LoadQueue(queue));
            self.runtime.apply_command(PlaybackCommand::Play);
            let stream_start = self.handle_messages_until("stream start", |message| {
                matches!(message.view(), gst::MessageView::StreamStart(_))
            });
            self.runtime.handle_bus_message(&stream_start);
            let source = self.file_source();
            assert_eq!(input_file(Some(source.upcast_ref())), Some(first));
            source
        }

        /// The file source of the pipeline's run.
        fn file_source(&self) -> gst::Element {
            self.runtime
                .playbin
                .clone()
                .downcast::<gst::Bin>()
                .unwrap()
                .iterate_recurse()
                .into_iter()
                .filter_map(Result::ok)
                .find(|element| {
                    element
                        .factory()
                        .is_some_and(|factory| factory.name() == "filesrc")
                })
                .expect("file source")
        }

        /// Play `queue`, of two tracks, and seek to 0.5 s after the pipeline
        /// was given the second, so that the first restarts. Returns once
        /// the restarted run was given the seek, which `OutputHold` keeps
        /// from landing.
        fn hold_seek_of_a_restart(&mut self, queue: Vec<PathBuf>) -> OutputHold {
            self.play_until_opened(queue, RepeatMode::Off);
            self.wait_until("the successor was not handed over by 3 s", |runtime| {
                runtime
                    .queue_state
                    .lock()
                    .unwrap()
                    .successor_handed_to_pipeline()
                    && runtime.snapshot.position >= millis(3_000)
            });
            self.runtime
                .apply_command(PlaybackCommand::Seek(millis(500)));
            assert_eq!(self.runtime.restart, Some(RunRestart::Prerolling));
            let playbin = self.runtime.playbin.clone();
            let prerolled = self.handle_messages_until("async-done of the restart", |message| {
                matches!(message.view(), gst::MessageView::AsyncDone(_))
                    && message.src().is_some_and(|src| src == &playbin)
            });
            let hold = OutputHold::new(&playbin);
            self.runtime.handle_bus_message(&prerolled);
            assert_eq!(self.runtime.restart, Some(RunRestart::Seeking));
            assert!(self.runtime.seek_in_flight.is_some());
            hold
        }

        /// Handle the bus messages posted so far, and poll.
        fn handle_posted_messages(&mut self) {
            while let Some(message) = self.bus.pop() {
                self.runtime.handle_bus_message(&message);
            }
            self.runtime.poll();
        }

        /// Fail reading from `source`, as a disk error would, and handle
        /// the error.
        fn fail_reading(&mut self, source: &gst::Element) {
            let error =
                gst::message::Error::builder(gst::ResourceError::Read, "injected read failure")
                    .src(source)
                    .build();
            self.bus.post(error).unwrap();
            let error = self.handle_messages_until("posted error", |message| {
                matches!(message.view(), gst::MessageView::Error(_))
            });
            self.runtime.handle_bus_message(&error);
        }

        fn errors(&self) -> Vec<PlaybackError> {
            self.events
                .try_iter()
                .filter_map(|event| match event {
                    PlaybackEvent::Error(error) => Some(error),
                    _ => None,
                })
                .collect()
        }
    }

    impl Drop for DirectEngine {
        fn drop(&mut self) {
            let _ = self.runtime.playbin.set_state(gst::State::Null);
        }
    }

    #[test]
    fn successor_renamed_before_its_remap_plays_from_the_new_path_once_remapped() {
        // The rename reaches the engine after the pipeline failed to open
        // the old path, but before the engine handles the failure.
        let fixtures = Fixtures::new("rename-remapped");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let renamed = fixtures.dir.join("renamed b.wav");
        std::fs::rename(&b, &renamed).unwrap();
        let mut engine = DirectEngine::start();
        engine
            .runtime
            .apply_command(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone(), c]));
        engine.runtime.apply_command(PlaybackCommand::Play);
        let failure = engine.handle_messages_until("error", |message| {
            matches!(message.view(), gst::MessageView::Error(_))
        });
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (b.clone(), renamed.clone()),
            ]))));
        engine.runtime.handle_bus_message(&failure);

        let errors: Vec<_> = engine
            .events
            .try_iter()
            .filter_map(|event| match event {
                PlaybackEvent::Error(error) => Some(error),
                _ => None,
            })
            .collect();
        assert_eq!(errors.len(), 1, "{errors:?}");
        assert_eq!(errors[0].path.as_deref(), Some(renamed.as_path()));
        assert_eq!(errors[0].outcome, PlaybackErrorOutcome::Skipped);
        assert_eq!(
            engine.runtime.snapshot.current.as_deref(),
            Some(a.as_path())
        );
        assert_eq!(engine.runtime.snapshot.current_queue_index, Some(0));

        let deadline = Instant::now() + TIMEOUT;
        let change = loop {
            let change = engine.events.try_iter().find_map(|event| match event {
                PlaybackEvent::TrackChanged {
                    path, queue_index, ..
                } => Some((path, queue_index)),
                _ => None,
            });
            if change.is_some() || Instant::now() >= deadline {
                break change;
            }
            if let Some(message) = engine.bus.timed_pop(gst::ClockTime::from_mseconds(20)) {
                engine.runtime.handle_bus_message(&message);
            }
        };
        assert_eq!(
            change,
            Some((renamed, 1)),
            "{}",
            engine.report("the renamed track did not start")
        );
    }

    /// What came first while the thread that activated the source of a file
    /// was held in the source's state change.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ActivationRace {
        /// The type finding of the source was held back.
        TypeFindingHeld,
        /// `urisourcebin` found the type of the file and set up a
        /// `parsebin`, which can deadlock with the activating thread.
        ParsebinSetUp,
        /// Neither happened within `TIMEOUT`.
        Neither,
    }

    /// Hold the thread that activates the source of `file` when the source
    /// becomes Paused, which is while its `urisourcebin` changes state,
    /// until the type finding of the source is held back or `urisourcebin`
    /// has set up a `parsebin` for it. Returns which came first.
    fn race_type_finding_with_activation(
        playbin: &gst::Element,
        file: &Path,
    ) -> Arc<Mutex<Option<ActivationRace>>> {
        let outcome = Arc::new(Mutex::new(None));
        let recorded = Arc::clone(&outcome);
        let file = file.to_path_buf();
        let bus = playbin.bus().unwrap();
        bus.enable_sync_message_emission();
        bus.connect_sync_message(None, move |_, message| {
            let gst::MessageView::StateChanged(change) = message.view() else {
                return;
            };
            let Some(source) = message
                .src()
                .and_then(|src| src.downcast_ref::<gst::Element>())
                .filter(|source| {
                    source
                        .factory()
                        .is_some_and(|factory| factory.name() == "filesrc")
                })
            else {
                return;
            };
            let location = source.property::<Option<String>>("location");
            if (change.old(), change.current()) != (gst::State::Ready, gst::State::Paused)
                || location.as_deref().map(Path::new) != Some(file.as_path())
                || recorded.lock().unwrap().is_some()
            {
                return;
            }
            let urisourcebin = source
                .parent()
                .and_downcast::<gst::Bin>()
                .expect("urisourcebin");
            let deadline = Instant::now() + TIMEOUT;
            let race = loop {
                let parsebin = urisourcebin
                    .iterate_elements()
                    .into_iter()
                    .filter_map(Result::ok)
                    .any(|element| {
                        element
                            .factory()
                            .is_some_and(|factory| factory.name() == "parsebin")
                    });
                if parsebin {
                    break ActivationRace::ParsebinSetUp;
                }
                let held = urisourcebin
                    .iterate_elements()
                    .into_iter()
                    .filter_map(Result::ok)
                    .filter_map(|element| element.static_pad("src"))
                    .any(|pad| pad.is_blocking());
                if held {
                    break ActivationRace::TypeFindingHeld;
                }
                if Instant::now() >= deadline {
                    break ActivationRace::Neither;
                }
                std::thread::sleep(millis(1));
            };
            *recorded.lock().unwrap() = Some(race);
        });
        outcome
    }

    #[test]
    fn sources_find_the_type_of_their_files_only_once_activated() {
        // urisourcebin starts to find the type of a file while it changes
        // state. Once found, it sets up a parsebin, which can deadlock with
        // the thread that changes its state (GStreamer issue 4472). The
        // engine's own state changes and handovers must hold it back.
        let fixtures = Fixtures::new("activation-race");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = DirectEngine::start();
        let first = race_type_finding_with_activation(&engine.runtime.playbin, &a);
        let second = race_type_finding_with_activation(&engine.runtime.playbin, &b);
        // The state change of the pipeline activates A's source.
        engine.play_until_opened(vec![a, b], RepeatMode::Off);
        assert_eq!(
            *first.lock().unwrap(),
            Some(ActivationRace::TypeFindingHeld)
        );
        // The successor handed over in about-to-finish activates B's.
        engine.wait_until("B did not start", |runtime| {
            runtime.snapshot.current_queue_index == Some(1)
                && runtime.snapshot.duration.abs_diff(millis(4_500)) < millis(100)
        });
        assert_eq!(
            *second.lock().unwrap(),
            Some(ActivationRace::TypeFindingHeld)
        );
    }

    /// How long a `SwitchHold` holds the state change it released the
    /// successor in, to see what uridecodebin3 does with it.
    const SWITCH_RACE_TIMEOUT: Duration = Duration::from_secs(2);

    /// How uridecodebin3's switch of its input to a successor met the state
    /// change of the engine that a `SwitchHold` released the successor in.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum SwitchRace {
        /// The engine held the successor back until its state change had
        /// returned.
        HeldBack,
        /// uridecodebin3 switched to the successor during the state change,
        /// which can deadlock.
        Switched,
        /// Neither happened within `TIMEOUT`.
        Neither,
    }

    /// Where in its state change the engine releases a held successor.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ReleaseIn {
        /// While it deactivates the output of uridecodebin3, on the way
        /// down to Ready.
        Deactivation,
        /// Once decodebin3 inside uridecodebin3 has reached Playing.
        Playing,
    }

    #[derive(Default)]
    struct SwitchHoldState {
        /// The block of the successor's first buffer, before it reaches the
        /// output of its `urisourcebin`.
        block: Option<(gst::Pad, gst::PadProbeId)>,
        /// The block was set up. Later runs that read the file are not held.
        blocked: bool,
        /// The first buffer reached the block.
        reached: bool,
        /// The output pad of the successor's `urisourcebin`.
        output: Option<gst::Pad>,
        /// uridecodebin3 linked the output: it switches to the successor.
        switching: bool,
        release_in: Option<ReleaseIn>,
        race: Option<SwitchRace>,
    }

    /// Holds the first buffer of the successor that reads a file before
    /// uridecodebin3 sees it, so the input switch to the successor waits,
    /// and releases it inside the engine's next state change of
    /// uridecodebin3, once armed.
    #[derive(Default)]
    struct SwitchHold {
        state: Mutex<SwitchHoldState>,
    }

    impl SwitchHold {
        /// Hold the successor that reads `file` in the pipeline `playbin`.
        fn new(playbin: &gst::Element, file: &Path) -> Arc<Self> {
            let hold = Arc::new(Self::default());
            let reads_file = {
                let file = file.to_path_buf();
                move |bin: &gst::Element| {
                    bin.factory()
                        .is_some_and(|factory| factory.name() == "urisourcebin")
                        && input_file(Some(bin.upcast_ref())).as_ref() == Some(&file)
                }
            };
            let state = Arc::downgrade(&hold);
            playbin.connect("deep-element-added", false, move |values| {
                let element = values.get(2)?.get::<gst::Element>().ok()?;
                let hold = state.upgrade()?;
                if reads_file(&element) {
                    element.connect_pad_added(move |_, pad| hold.watch_output(pad));
                }
                None
            });
            hold
        }

        fn watch_output(self: &Arc<Self>, pad: &gst::Pad) {
            // The pad that feeds the output, the source pad of the
            // multiqueue of `urisourcebin`, which carries no other blocks.
            if let Some(feed) = pad
                .downcast_ref::<gst::GhostPad>()
                .and_then(gst::prelude::ProxyPadExt::internal)
                .and_then(|internal| internal.peer())
            {
                self.block(&feed);
            }
            self.state.lock().unwrap().output = Some(pad.clone());
            let hold = Arc::downgrade(self);
            // uridecodebin3 links the output inside its switch, holding its
            // play items lock and before it takes its state lock.
            pad.connect_linked(move |_, _| {
                if let Some(hold) = hold.upgrade() {
                    hold.state.lock().unwrap().switching = true;
                }
            });
        }

        fn block(self: &Arc<Self>, pad: &gst::Pad) {
            let mut state = self.state.lock().unwrap();
            if std::mem::replace(&mut state.blocked, true) {
                return;
            }
            let hold = Arc::downgrade(self);
            let probe = pad
                .add_probe(
                    gst::PadProbeType::BLOCK | gst::PadProbeType::BUFFER,
                    move |_, _| {
                        if let Some(hold) = hold.upgrade() {
                            hold.state.lock().unwrap().reached = true;
                        }
                        gst::PadProbeReturn::Ok
                    },
                )
                .unwrap();
            state.block = Some((pad.clone(), probe));
        }

        fn reached(&self) -> bool {
            self.state.lock().unwrap().reached
        }

        fn race(&self) -> Option<SwitchRace> {
            self.state.lock().unwrap().race
        }

        /// Release the successor inside the next state change of the
        /// uridecodebin3 of `playbin`, in `release_in`.
        fn arm(self: &Arc<Self>, playbin: &gst::Element, release_in: ReleaseIn) {
            self.state.lock().unwrap().release_in = Some(release_in);
            let hold = Arc::downgrade(self);
            match release_in {
                ReleaseIn::Deactivation => on_output_deactivation(playbin, move || {
                    if let Some(hold) = hold.upgrade() {
                        hold.release_in(ReleaseIn::Deactivation);
                    }
                }),
                ReleaseIn::Playing => {
                    let bus = playbin.bus().unwrap();
                    bus.enable_sync_message_emission();
                    bus.connect_sync_message(None, move |_, message| {
                        let gst::MessageView::StateChanged(change) = message.view() else {
                            return;
                        };
                        let decodebin = message
                            .src()
                            .and_then(|src| src.downcast_ref::<gst::Element>())
                            .and_then(gst::Element::factory)
                            .is_some_and(|factory| factory.name() == "decodebin3");
                        if decodebin && change.current() == gst::State::Playing {
                            if let Some(hold) = hold.upgrade() {
                                hold.release_in(ReleaseIn::Playing);
                            }
                        }
                    });
                }
            }
        }

        /// On the thread that changes the state, inside the state change:
        /// release the successor, and wait until uridecodebin3 switches to
        /// it or the engine holds it back.
        fn release_in(&self, point: ReleaseIn) {
            let (block, output) = {
                let mut state = self.state.lock().unwrap();
                if state.release_in != Some(point) {
                    return;
                }
                state.release_in = None;
                (state.block.take(), state.output.clone())
            };
            let internal = output
                .and_then(|output| output.downcast::<gst::GhostPad>().ok())
                .and_then(|output| output.internal());
            // Runs in a callback of GStreamer, so it records instead of
            // panicking.
            let (Some((pad, probe)), Some(internal)) = (block, internal) else {
                self.state.lock().unwrap().race = Some(SwitchRace::Neither);
                return;
            };
            pad.remove_probe(probe);
            let deadline = Instant::now() + SWITCH_RACE_TIMEOUT;
            let race = loop {
                if self.state.lock().unwrap().switching {
                    break SwitchRace::Switched;
                }
                if internal.is_blocking() {
                    break SwitchRace::HeldBack;
                }
                if Instant::now() >= deadline {
                    break SwitchRace::Neither;
                }
                std::thread::sleep(millis(1));
            };
            self.state.lock().unwrap().race = Some(race);
        }
    }

    /// Call `deactivating` on the thread that deactivates the outputs of the
    /// uridecodebin3 of `playbin`, inside the state change of uridecodebin3
    /// to Ready. A deactivated pad drops its caps. A bin deactivates its
    /// outputs first: after uridecodebin3 has marked itself as shutting
    /// down, and before its children change state.
    fn on_output_deactivation(
        playbin: &gst::Element,
        deactivating: impl Fn() + Clone + Send + Sync + 'static,
    ) {
        let uridecodebin = playbin
            .clone()
            .downcast::<gst::Bin>()
            .unwrap()
            .iterate_recurse()
            .into_iter()
            .filter_map(Result::ok)
            .find(|element| {
                element
                    .factory()
                    .is_some_and(|factory| factory.name() == "uridecodebin3")
            })
            .expect("uridecodebin3");
        for pad in uridecodebin.src_pads() {
            let deactivating = deactivating.clone();
            pad.connect_notify(Some("caps"), move |pad, _| {
                if pad.current_caps().is_none() {
                    deactivating();
                }
            });
        }
    }

    /// Whether the end of the stream of every output of the source that
    /// reads `file` in `playbin` has passed the output, so uridecodebin3
    /// can switch to the successor at once.
    fn source_drained(playbin: &gst::Element, file: &Path) -> bool {
        playbin
            .clone()
            .downcast::<gst::Bin>()
            .unwrap()
            .iterate_recurse()
            .into_iter()
            .filter_map(Result::ok)
            .find(|element| {
                element
                    .factory()
                    .is_some_and(|factory| factory.name() == "urisourcebin")
                    && input_file(Some(element.upcast_ref())).as_deref() == Some(file)
            })
            .is_some_and(|source| {
                let outputs = source.src_pads();
                !outputs.is_empty()
                    && outputs
                        .iter()
                        .all(|output| output.sticky_event::<gst::event::Eos>(0).is_some())
            })
    }

    impl Engine {
        /// Play `queue` until `hold` holds the successor of the first track,
        /// and the first track has drained, so that uridecodebin3 switches
        /// to the successor as soon as it gets the successor's data.
        #[track_caller]
        fn play_until_switch_held(&mut self, queue: Vec<PathBuf>, hold: &SwitchHold) {
            let first = queue[0].clone();
            self.play_until_successor_held(queue, hold);
            let playbin = self.playbin.clone();
            self.wait_until("the first track did not drain", |_| {
                source_drained(&playbin, &first)
            });
        }

        /// Play `queue` until `hold` holds the successor of the first track.
        #[track_caller]
        fn play_until_successor_held(&mut self, queue: Vec<PathBuf>, hold: &SwitchHold) {
            self.send(PlaybackCommand::LoadQueue(queue));
            self.send(PlaybackCommand::Play);
            self.wait_until("the successor was not held", |_| hold.reached());
        }
    }

    /// Holds the end of the stream of the first source that reads a file,
    /// before the source announces that it is about to finish, and
    /// releases it inside the engine's next state change to Ready, once
    /// armed.
    #[derive(Default)]
    struct EndOfStreamHold {
        /// The block of the end of stream and whether it was reached.
        block: Mutex<(Option<(gst::Pad, gst::PadProbeId)>, bool)>,
        /// What the about-to-finish callback did with the released end.
        handover: Mutex<Option<Handover>>,
    }

    /// What the engine's about-to-finish callback did when the end of the
    /// stream that an `EndOfStreamHold` held was released inside a state
    /// change to Ready.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Handover {
        /// The callback returned during the state change.
        Returned,
        /// The callback did not return within `SWITCH_RACE_TIMEOUT`: it
        /// waits for the state change.
        Waiting,
    }

    impl EndOfStreamHold {
        fn new(playbin: &gst::Element, file: &Path) -> Arc<Self> {
            let hold = Arc::new(Self::default());
            let file = file.to_path_buf();
            let state = Arc::downgrade(&hold);
            playbin.connect("deep-element-added", false, move |values| {
                let bin = values.get(1)?.get::<gst::Element>().ok()?;
                let element = values.get(2)?.get::<gst::Element>().ok()?;
                let parses_file = element
                    .factory()
                    .is_some_and(|factory| factory.name() == "parsebin")
                    && input_file(Some(bin.upcast_ref())).as_ref() == Some(&file);
                if parses_file {
                    let hold = state.upgrade()?;
                    // Connected before urisourcebin's own handler, whose probe
                    // announces about-to-finish at the end of the stream.
                    element.connect_pad_added(move |_, pad| hold.block(pad));
                }
                None
            });
            hold
        }

        fn block(self: &Arc<Self>, pad: &gst::Pad) {
            let mut block = self.block.lock().unwrap();
            if block.0.is_some() || block.1 {
                return;
            }
            let hold = Arc::downgrade(self);
            let probe = pad
                .add_probe(
                    gst::PadProbeType::BLOCK | gst::PadProbeType::EVENT_DOWNSTREAM,
                    move |_, info| match &info.data {
                        Some(gst::PadProbeData::Event(event))
                            if event.type_() == gst::EventType::Eos =>
                        {
                            if let Some(hold) = hold.upgrade() {
                                hold.block.lock().unwrap().1 = true;
                            }
                            gst::PadProbeReturn::Ok
                        }
                        _ => gst::PadProbeReturn::Pass,
                    },
                )
                .unwrap();
            block.0 = Some((pad.clone(), probe));
        }

        fn reached(&self) -> bool {
            self.block.lock().unwrap().1
        }

        fn handover(&self) -> Option<Handover> {
            *self.handover.lock().unwrap()
        }

        /// Release the end of stream inside the engine's next state change
        /// of `playbin` to Ready, and see whether the about-to-finish
        /// callback, which `calls` counts, returns during it.
        fn arm(self: &Arc<Self>, playbin: &gst::Element, calls: &Arc<AboutToFinishCalls>) {
            let hold = Arc::downgrade(self);
            let calls = Arc::clone(calls);
            on_output_deactivation(playbin, move || {
                let Some(hold) = hold.upgrade() else {
                    return;
                };
                let Some((pad, probe)) = hold.block.lock().unwrap().0.take() else {
                    return;
                };
                let emitted = calls.emitted.load(Ordering::SeqCst);
                pad.remove_probe(probe);
                let deadline = Instant::now() + SWITCH_RACE_TIMEOUT;
                let handover = loop {
                    if calls.emitted.load(Ordering::SeqCst) > emitted
                        && calls.returned.load(Ordering::SeqCst)
                            == calls.emitted.load(Ordering::SeqCst)
                    {
                        break Handover::Returned;
                    }
                    if Instant::now() >= deadline {
                        break Handover::Waiting;
                    }
                    std::thread::sleep(millis(1));
                };
                *hold.handover.lock().unwrap() = Some(handover);
            });
        }
    }

    #[test]
    fn stop_while_uridecodebin3_switches_to_the_successor_stops() {
        let fixtures = Fixtures::new("switch-stop");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        let hold = SwitchHold::new(&engine.playbin, &b);
        engine.play_until_switch_held(vec![a.clone(), b.clone()], &hold);
        hold.arm(&engine.playbin, ReleaseIn::Deactivation);
        engine.send(PlaybackCommand::Stop);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });
        assert_eq!(hold.race(), Some(SwitchRace::HeldBack));

        // Play starts the stopped track again, and its successor follows.
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
    }

    #[test]
    fn stop_while_the_successor_waits_for_the_input_to_drain_stops() {
        // The successor reaches uridecodebin3 before the current track has
        // drained, so uridecodebin3 would wait for the drain, which the stop
        // does not announce once it has begun.
        let fixtures = Fixtures::new("switch-stop-undrained");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        let hold = SwitchHold::new(&engine.playbin, &b);
        engine.play_until_successor_held(vec![a.clone(), b.clone()], &hold);
        assert!(!source_drained(&engine.playbin, &a));
        hold.arm(&engine.playbin, ReleaseIn::Deactivation);
        engine.send(PlaybackCommand::Stop);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });
        assert_eq!(hold.race(), Some(SwitchRace::HeldBack));

        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
    }

    #[test]
    fn stop_while_about_to_finish_hands_over_the_successor_stops() {
        // The handover activates the successor's source, which needs the
        // state lock of uridecodebin3 that the stop holds.
        let fixtures = Fixtures::new("handover-stop");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        let hold = EndOfStreamHold::new(&engine.playbin, &a);
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the end of A was not held", |_| hold.reached());
        hold.arm(&engine.playbin, &engine.trace.about_to_finish);
        engine.send(PlaybackCommand::Stop);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });
        assert_eq!(hold.handover(), Some(Handover::Returned));
        assert!(engine.prepared.is_empty(), "{:?}", engine.prepared);

        // The replayed track hands over its successor.
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
    }

    #[test]
    fn next_while_uridecodebin3_switches_to_the_successor_plays_the_next_track() {
        let fixtures = Fixtures::new("switch-next");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        let hold = SwitchHold::new(&engine.playbin, &b);
        engine.play_until_switch_held(vec![a, b.clone(), c.clone()], &hold);
        hold.arm(&engine.playbin, ReleaseIn::Deactivation);
        engine.send(PlaybackCommand::Next);
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Manual);
        engine.wait_for_playing(1, millis(4_500));
        assert_eq!(hold.race(), Some(SwitchRace::HeldBack));
        assert_change(&engine.wait_for_changes(3), &c, 2, Expect::Gapless);
    }

    #[test]
    fn seek_while_uridecodebin3_switches_to_the_successor_lands_and_plays_on() {
        let fixtures = Fixtures::new("switch-seek");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        let hold = SwitchHold::new(&engine.playbin, &b);
        engine.play_until_switch_held(vec![a, b.clone()], &hold);
        hold.arm(&engine.playbin, ReleaseIn::Deactivation);
        // The pipeline was given B, so the seek restarts A.
        engine.send(PlaybackCommand::Seek(millis(500)));
        engine.wait_until("the seek did not land", |engine| !engine.seeked.is_empty());
        assert_eq!(hold.race(), Some(SwitchRace::HeldBack));
        assert!(engine.seeked[0].abs_diff(millis(500)) < millis(100));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
    }

    #[test]
    fn play_while_uridecodebin3_switches_to_the_successor_plays_it_gaplessly() {
        let fixtures = Fixtures::new("switch-play");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        let hold = SwitchHold::new(&engine.playbin, &b);
        engine.play_until_switch_held(vec![a, b.clone()], &hold);
        engine.send(PlaybackCommand::Pause);
        engine.wait_until("playback did not pause", |engine| {
            engine.snapshot.state == PlaybackState::Paused
        });
        hold.arm(&engine.playbin, ReleaseIn::Playing);
        engine.send(PlaybackCommand::Play);
        engine.wait_until("the successor was not released", |_| hold.race().is_some());
        // Going to Playing takes neither lock that the switch holds, so the
        // switch only waits for the state change to return.
        assert_eq!(hold.race(), Some(SwitchRace::Switched));
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Gapless);
    }

    #[test]
    fn current_track_failing_after_a_swap_with_its_successor_plays_the_successor() {
        // A and B swap names after the pipeline opened A, so the path the
        // pipeline was given for A now names B.
        let fixtures = Fixtures::new("swap-then-fail");
        let a = fixtures.wav("a.wav", 8_000);
        let b = fixtures.wav("b.wav", 9_000);
        let mut engine = DirectEngine::start();
        let source = engine.play_until_opened(vec![a.clone(), b.clone()], RepeatMode::Off);
        let swap = fixtures.dir.join("swap.wav");
        std::fs::rename(&a, &swap).unwrap();
        std::fs::rename(&b, &a).unwrap();
        std::fs::rename(&swap, &b).unwrap();
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (a.clone(), b.clone()),
                (b.clone(), a.clone()),
            ]))));
        engine.fail_reading(&source);

        let errors = engine.errors();
        assert_eq!(errors.len(), 1, "{errors:?}");
        assert_eq!(errors[0].path.as_deref(), Some(b.as_path()), "A's new name");
        assert_eq!(errors[0].outcome, PlaybackErrorOutcome::Skipped);
        // B plays under its new name, which was A's.
        engine.wait_until("row 1 did not start", |runtime| {
            runtime.snapshot.current_queue_index == Some(1)
                && runtime.snapshot.duration.abs_diff(millis(9_000)) < millis(100)
        });
        assert_eq!(
            engine.runtime.snapshot.current.as_deref(),
            Some(a.as_path())
        );
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Playing);
        assert!(engine.errors().is_empty());
    }

    #[test]
    fn renamed_current_track_failing_with_repeat_one_stops() {
        let fixtures = Fixtures::new("rename-then-fail-repeat-one");
        let a = fixtures.wav("a.wav", 8_000);
        let renamed = fixtures.dir.join("renamed a.wav");
        let mut engine = DirectEngine::start();
        let source = engine.play_until_opened(vec![a.clone()], RepeatMode::One);
        std::fs::rename(&a, &renamed).unwrap();
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (a, renamed.clone()),
            ]))));
        engine.fail_reading(&source);

        let errors = engine.errors();
        assert_eq!(errors.len(), 1, "{errors:?}");
        assert_eq!(errors[0].path.as_deref(), Some(renamed.as_path()));
        assert_eq!(errors[0].outcome, PlaybackErrorOutcome::Stopped);
        assert_eq!(engine.runtime.snapshot.state, PlaybackState::Stopped);
        assert_eq!(
            engine.runtime.snapshot.current.as_deref(),
            Some(renamed.as_path())
        );
    }

    #[test]
    fn renamed_current_track_failing_is_reported_under_its_new_name() {
        let fixtures = Fixtures::new("rename-then-fail");
        let a = fixtures.wav("a.wav", 8_000);
        let b = fixtures.wav("b.wav", 4_500);
        let renamed = fixtures.dir.join("renamed a.wav");
        let mut engine = DirectEngine::start();
        let source = engine.play_until_opened(vec![a.clone(), b.clone()], RepeatMode::Off);
        std::fs::rename(&a, &renamed).unwrap();
        engine
            .runtime
            .apply_command(PlaybackCommand::RemapPaths(Arc::new(PathRenames::from([
                (a, renamed.clone()),
            ]))));
        engine.fail_reading(&source);

        let errors = engine.errors();
        assert_eq!(errors.len(), 1, "{errors:?}");
        assert_eq!(errors[0].path.as_deref(), Some(renamed.as_path()));
        assert!(
            errors[0].message().contains("renamed a.wav"),
            "{}",
            errors[0].message()
        );
        assert_eq!(errors[0].outcome, PlaybackErrorOutcome::Skipped);
        engine.wait_until("row 1 did not start", |runtime| {
            runtime.snapshot.current_queue_index == Some(1)
                && runtime.snapshot.duration.abs_diff(millis(4_500)) < millis(100)
        });
        assert_eq!(
            engine.runtime.snapshot.current.as_deref(),
            Some(b.as_path())
        );
    }

    #[test]
    fn shuffle_with_repeat_all_replays_the_track_left_after_its_successors_fail() {
        // With this seed, the successor search refreshes the shuffle pool
        // and picks a failed track again before it reaches A.
        let fixtures = Fixtures::new("shuffle-failed-successors");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.corrupt("b.wav");
        let c = fixtures.corrupt("c.wav");
        let mut engine = Engine::start();
        engine.queue.lock().unwrap().seed_shuffle(2);
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::All));
        engine.send(PlaybackCommand::SetShuffle(true));
        engine.send(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_errors(2);
        assert_change(&engine.wait_for_changes(2), &a, 0, Expect::Automatic);
        engine.wait_for_playing(0, millis(4_000));
        assert_eq!(engine.snapshot.state, PlaybackState::Playing);

        assert!(
            engine
                .errors
                .iter()
                .all(|error| error.outcome == PlaybackErrorOutcome::Skipped),
            "{:?}",
            engine.errors
        );
        let mut failed: Vec<_> = engine
            .errors
            .iter()
            .map(|error| error.path.clone())
            .collect();
        failed.sort();
        assert_eq!(failed, vec![Some(b), Some(c)]);
    }

    #[test]
    fn tracks_failing_in_a_row_stop_playback_with_a_message() {
        let fixtures = Fixtures::new("failures-in-a-row");
        let mut queue: Vec<_> = (0..6)
            .map(|index| fixtures.corrupt(&format!("bad {index}.wav")))
            .collect();
        queue.push(fixtures.wav("good.wav", 4_000));
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(queue.clone()));
        engine.send(PlaybackCommand::Play);
        let last = engine.wait_for_errors(5);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });

        for (index, error) in engine.errors.iter().enumerate().take(4) {
            assert_eq!(error.path.as_deref(), Some(queue[index].as_path()));
            assert_eq!(error.outcome, PlaybackErrorOutcome::Skipped);
        }
        assert_eq!(last.path.as_deref(), Some(queue[4].as_path()));
        assert_eq!(last.outcome, PlaybackErrorOutcome::StoppedAfterFailures(5));
        assert!(
            last.message().contains("5 tracks in a row"),
            "{}",
            last.message()
        );
        assert_eq!(engine.snapshot.current.as_deref(), Some(queue[4].as_path()));
        assert_eq!(engine.errors.len(), 5);
        let rows: Vec<_> = engine.changes.iter().map(|change| change.index).collect();
        assert_eq!(rows, vec![0, 1, 2, 3, 4]);
    }

    #[test]
    fn queue_where_every_track_fails_stops_with_repeat_all_and_repeat_one() {
        let fixtures = Fixtures::new("all-fail");
        let queue = vec![fixtures.corrupt("bad 0.wav"), fixtures.corrupt("bad 1.wav")];
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::All));
        engine.send(PlaybackCommand::LoadQueue(queue.clone()));
        engine.send(PlaybackCommand::Play);
        let stopped = engine.wait_for_errors(2);
        assert_eq!(engine.errors[0].outcome, PlaybackErrorOutcome::Skipped);
        assert_eq!(stopped.path.as_deref(), Some(queue[1].as_path()));
        assert_eq!(stopped.outcome, PlaybackErrorOutcome::Stopped);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });

        // Repeat-one plays only the failed track, so it stops at once.
        engine.send(PlaybackCommand::SetRepeatMode(RepeatMode::One));
        engine.send(PlaybackCommand::Play);
        let stopped = engine.wait_for_errors(3);
        assert_eq!(stopped.path.as_deref(), Some(queue[1].as_path()));
        assert_eq!(stopped.outcome, PlaybackErrorOutcome::Stopped);
        engine.wait_until("playback did not stop", |engine| {
            engine.snapshot.state == PlaybackState::Stopped
        });
        assert_eq!(engine.errors.len(), 3);
        assert_eq!(engine.changes.len(), 2, "{:?}", engine.changes);
    }

    #[test]
    fn failure_with_a_successor_staged_for_end_of_stream_leaves_no_stale_advance() {
        // B has another extension, so it is staged to start at the end of
        // A rather than handed to the pipeline.
        let fixtures = Fixtures::new("fail-staged");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wave", 4_500);
        let c = fixtures.wav("c.wav", 5_000);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![
            a.clone(),
            b.clone(),
            c.clone(),
        ]));
        engine.send(PlaybackCommand::Play);
        // The pipeline read all of A, so it asked for the successor.
        engine.wait_until("the position did not reach 3 s", |engine| {
            engine.snapshot.position >= millis(3_000)
        });
        assert_eq!(engine.snapshot.current_queue_index, Some(0));

        engine.post_shared_error(gst::StreamError::Decode, "injected decoder failure");
        let error = engine.wait_for_errors(1);
        assert_eq!(error.path.as_deref(), Some(a.as_path()));
        assert_eq!(error.outcome, PlaybackErrorOutcome::Skipped);
        assert_eq!(error.detail, "injected decoder failure");
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Automatic);
        engine.wait_for_playing(1, millis(4_500));

        // Stop and Play replay B, and C follows it.
        engine.send(PlaybackCommand::Stop);
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(1, millis(4_500));
        assert_change(&engine.wait_for_changes(3), &c, 2, Expect::Automatic);
        engine.wait_for_playing(2, millis(5_000));
        assert_eq!(engine.changes.len(), 3, "{:?}", engine.changes);
    }

    #[test]
    fn missing_plugin_is_named_in_the_error() {
        let fixtures = Fixtures::new("missing-plugin");
        let a = fixtures.wav("a.wav", 4_000);
        let b = fixtures.wav("b.wav", 4_500);
        let mut engine = Engine::start();
        engine.send(PlaybackCommand::LoadQueue(vec![a.clone(), b.clone()]));
        engine.send(PlaybackCommand::Play);
        engine.wait_for_playing(0, millis(4_000));

        let caps = gst::Caps::builder("audio/x-flac").build();
        let missing = gst_pbutils::MissingPluginMessage::builder_for_decoder(&caps)
            .src(&engine.playbin)
            .build();
        let description = gst_pbutils::MissingPluginMessage::parse(&missing)
            .unwrap()
            .description()
            .to_string();
        assert!(!description.is_empty());
        engine.post(missing);
        engine.post_shared_error(
            gst::CoreError::MissingPlugin,
            "Your GStreamer installation is missing a plug-in.",
        );
        let error = engine.wait_for_errors(1);
        assert_eq!(error.path.as_deref(), Some(a.as_path()));
        assert_eq!(
            error.detail,
            format!("missing GStreamer plugin: {description}")
        );
        assert!(
            error.message().contains(&description),
            "{}",
            error.message()
        );
        assert_change(&engine.wait_for_changes(2), &b, 1, Expect::Automatic);
    }

    #[test]
    fn engine_that_cannot_start_reports_why_and_fails_commands() {
        fn no_output() -> anyhow::Result<gst::Element> {
            Err(anyhow!("no audio output in this test"))
        }
        let (analysis_tx, _analysis) = unbounded();
        let (engine, events) = crate::playback::PlaybackEngine::spawn(|event_tx, failure| {
            spawn_engine_with_output(analysis_tx, event_tx, failure, no_output)
        });
        let unavailable = |event: Option<PlaybackEvent>| match event {
            Some(PlaybackEvent::Error(error)) => {
                assert_eq!(error.outcome, PlaybackErrorOutcome::EngineUnavailable);
                assert_eq!(error.path, None);
                assert!(
                    error.detail.contains("no audio output in this test"),
                    "{error:?}"
                );
            }
            other => panic!("expected the engine failure, got {other:?}"),
        };
        unavailable(events.recv_timeout(TIMEOUT).ok());

        engine.command(PlaybackCommand::Play);
        unavailable(events.try_recv().ok());
        engine.command(PlaybackCommand::Poll);
        // The bridge's polls are not reported, and the event channel stays
        // open, so the bridge does not spin on a closed channel.
        assert!(matches!(
            events.recv_timeout(millis(100)),
            Err(crossbeam_channel::RecvTimeoutError::Timeout)
        ));
    }
}
