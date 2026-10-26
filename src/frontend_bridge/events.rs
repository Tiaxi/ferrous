// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Weak};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[cfg(test)]
use crossbeam_channel::bounded;
use crossbeam_channel::{Receiver, Sender};

use crate::analysis::{
    AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot, SpectrogramDisplayMode,
};
use crate::lastfm::{
    self, Command as LastFmCommand, Handle as LastFmHandle,
    NowPlayingTrack as LastFmNowPlayingTrack, ScrobbleEntry as LastFmScrobbleEntry,
};
use crate::library::{
    CoverReadAt, IndexedTrack, LibraryEvent, LibrarySnapshot, LibraryTrack, TrackFileFingerprint,
};
use crate::metadata::{MetadataEvent, MetadataService};
use crate::playback::{
    remap_path, PlaybackErrorOutcome, PlaybackEvent, PlaybackSnapshot, PlaybackState,
    TrackChangeKind,
};
use crate::profile_logging::heartbeat_trace_enabled;

use super::{
    try_send_event, ApplyAlbumArtEvent, BridgeError, BridgeEvent, BridgeLoopFlags, BridgeState,
    ErrorCategory, ErrorSeverity, LastFmPlaybackTracker, PendingWaveformTrack, SettingsSaveTiming,
    SnapshotUrgency,
};

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

pub(super) fn process_apply_album_art_event(
    event: ApplyAlbumArtEvent,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    if let Some(error) = event.error.as_ref() {
        let error = BridgeError::error(ErrorCategory::Library, error.clone())
            .about(event.track_path.to_string_lossy());
        let _ = try_send_event(event_tx, BridgeEvent::Error(error));
        if event.indexed_by_path.is_empty() {
            return SnapshotUrgency::None;
        }
    }

    update_library_cover_paths(state, &event.indexed_by_path);
    update_queue_cover_paths(state, &event.indexed_by_path, &event.fingerprints);

    if let Some(current_path) = state.playback.current.as_ref() {
        if let Some(indexed) = event.indexed_by_path.get(current_path) {
            let cover = (indexed.cover_path.clone(), indexed.cover_stamp);
            set_metadata_cover(state, cover.0, cover.1, indexed.cover_read_at);
        }
    }

    metadata.request(event.track_path);
    SnapshotUrgency::Immediate
}

pub(super) fn drain_apply_album_art_events(
    apply_album_art_rx: &Receiver<ApplyAlbumArtEvent>,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;

    while let Ok(event) = apply_album_art_rx.try_recv() {
        urgency = urgency.max(process_apply_album_art_event(
            event, metadata, event_tx, state,
        ));
    }

    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_apply_album_art_events(
    apply_album_art_rx: &Receiver<ApplyAlbumArtEvent>,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> bool {
    drain_apply_album_art_events(apply_album_art_rx, metadata, event_tx, state).is_pending()
}

/// Give the library's tracks the covers in `indexed_by_path`. The library
/// is found through its path index and copied only if a cover changes.
pub(super) fn update_library_cover_paths(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) {
    let mut updates = Vec::new();
    for (path, indexed) in indexed_by_path {
        for position in state.library_index.positions(&state.library, path) {
            let track = &state.library.tracks[position];
            if track.cover_path != indexed.cover_path || track.cover_stamp != indexed.cover_stamp {
                updates.push((position, indexed));
            }
        }
    }
    if updates.is_empty() {
        return;
    }
    let previous = Arc::clone(&state.library);
    let mut next_library = (*previous).clone();
    for (position, indexed) in updates {
        let track = Arc::make_mut(&mut next_library.tracks[position]);
        track.cover_path.clone_from(&indexed.cover_path);
        track.cover_stamp = indexed.cover_stamp;
        track.cover_read_at = indexed.cover_read_at;
        state.library_rows_to_read.push(track.path.clone());
    }
    next_library.search_revision = next_library.search_revision.saturating_add(1);
    state.library = Arc::new(next_library);
    state.library_index.carry_over(&previous, &state.library);
}

pub(super) fn update_queue_cover_paths(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
    fingerprints: &HashMap<PathBuf, TrackFileFingerprint>,
) {
    for (path, indexed) in indexed_by_path {
        let Some(existing) = Arc::make_mut(&mut state.queue_details).get_mut(path) else {
            continue;
        };
        if existing.cover_path != indexed.cover_path {
            existing.cover_path.clone_from(&indexed.cover_path);
        }
        existing.cover_stamp = indexed.cover_stamp;
        existing.cover_read_at = indexed.cover_read_at;
        if let Some(fingerprint) = fingerprints.get(path) {
            state
                .queue_detail_fingerprints
                .insert(path.clone(), *fingerprint);
        }
    }
}

pub(super) fn playback_snapshot_urgency(
    previous: &PlaybackSnapshot,
    next: &PlaybackSnapshot,
) -> SnapshotUrgency {
    if previous.state != next.state
        || previous.current != next.current
        || previous.current_queue_index != next.current_queue_index
        || (previous.volume - next.volume).abs() > f32::EPSILON
        || previous.repeat_mode != next.repeat_mode
        || previous.shuffle_enabled != next.shuffle_enabled
        || previous.duration != next.duration
        // The UI holds its playhead at a seek target until the engine is
        // done with that seek.
        || previous.seek_serial != next.seek_serial
        || previous.seek_landed_serial != next.seek_landed_serial
        // The UI stops or starts moving its playhead.
        || previous.position_advancing != next.position_advancing
    {
        return SnapshotUrgency::Immediate;
    }
    if previous.position != next.position
        || previous.position_sampled_at != next.position_sampled_at
        || previous.current_bitrate_kbps != next.current_bitrate_kbps
    {
        return SnapshotUrgency::Heartbeat;
    }
    SnapshotUrgency::None
}

#[cfg(any(test, feature = "profiling-logs"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct PlaybackPositionTrace {
    previous: i128,
    current: i128,
    delta: i128,
}

#[cfg(any(test, feature = "profiling-logs"))]
fn duration_ms_i128(value: Duration) -> i128 {
    i128::try_from(value.as_millis()).unwrap_or(i128::MAX)
}

#[cfg(any(test, feature = "profiling-logs"))]
fn playback_position_trace(previous: Duration, current: Duration) -> PlaybackPositionTrace {
    let previous_ms = duration_ms_i128(previous);
    let current_ms = duration_ms_i128(current);
    PlaybackPositionTrace {
        previous: previous_ms,
        current: current_ms,
        delta: current_ms - previous_ms,
    }
}

pub(super) fn process_playback_event(
    event: PlaybackEvent,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    match event {
        PlaybackEvent::Snapshot(mut snapshot) => {
            if let Some(current) = snapshot.current.as_mut() {
                remap_unacknowledged_engine_path(state, current);
            }
            process_playback_snapshot_event(snapshot, analysis, state)
        }
        PlaybackEvent::TrackChanged {
            mut path,
            queue_index,
            kind,
            track_token,
        } => {
            remap_unacknowledged_engine_path(state, &mut path);
            state.playback.current_queue_index = Some(queue_index);
            state.analysis.waveform_peaks.clear();
            state.analysis.waveform_coverage_seconds = 0.0;
            state.analysis.waveform_complete = false;
            state.analysis_track_token = track_token;
            state.last_track_change.generation = state.last_track_change.generation.wrapping_add(1);
            state.last_track_change.path = Some(path.clone());
            metadata.request(path.clone());
            // A track starts: check the queue's files again, since they may
            // have changed on disk since they were last checked.
            super::sync_queue_details(state);
            let is_gapless = matches!(kind, TrackChangeKind::Gapless);
            let reset_spectrogram = matches!(kind, TrackChangeKind::Manual);
            if state.playback.state == PlaybackState::Stopped {
                profile_eprintln!(
                    "[bridge] TrackChanged while Stopped → deferred token={track_token}",
                );
                state.pending_waveform_track = Some(PendingWaveformTrack {
                    path,
                    reset_spectrogram,
                    track_token,
                });
            } else {
                profile_eprintln!(
                    "[bridge] TrackChanged while {:?} → immediate SetTrack token={}",
                    state.playback.state,
                    track_token,
                );
                state.pending_waveform_track = None;
                analysis.command(AnalysisCommand::SetTrack {
                    path,
                    reset_spectrogram,
                    track_token,
                    gapless: is_gapless,
                });
            }
            // The playing index travels in the playback section, so a track
            // change asks for no queue section; one already asked for stays.
            SnapshotUrgency::Immediate
        }
        PlaybackEvent::Seeked { position } => {
            // The engine's next snapshot carries the landing position with
            // the time it was read; the UI takes it from there.
            if state.analysis_track_token != 0 && state.playback.current.is_some() {
                let pos_seconds = position.as_secs_f64();
                analysis.command(AnalysisCommand::SeekPosition(pos_seconds));
            }
            SnapshotUrgency::None
        }
        PlaybackEvent::PathsRemapped => {
            state.unacknowledged_engine_renames.pop_front();
            SnapshotUrgency::None
        }
        PlaybackEvent::Error(mut error) => {
            if let Some(path) = error.path.as_mut() {
                remap_unacknowledged_engine_path(state, path);
            }
            let _ = try_send_event(event_tx, BridgeEvent::Error(playback_error(&error)));
            SnapshotUrgency::None
        }
    }
}

/// The error the user is shown for `error`, about its file. A skipped file
/// leaves playback going.
fn playback_error(error: &crate::playback::PlaybackError) -> BridgeError {
    let severity = match error.outcome {
        PlaybackErrorOutcome::Skipped | PlaybackErrorOutcome::NotResumed => ErrorSeverity::Warning,
        PlaybackErrorOutcome::Stopped
        | PlaybackErrorOutcome::StoppedAfterFailures(_)
        | PlaybackErrorOutcome::EngineUnavailable
        | PlaybackErrorOutcome::NotReleased => ErrorSeverity::Error,
    };
    BridgeError {
        category: ErrorCategory::Playback,
        severity,
        subject: error
            .path
            .as_deref()
            .map(|path| path.to_string_lossy().into_owned())
            .unwrap_or_default(),
        message: error.message(),
    }
}

/// Give a path from an engine event the name the bridge already uses, so an
/// event sent before the engine applied a rename cannot restore the old path.
fn remap_unacknowledged_engine_path(state: &BridgeState, path: &mut PathBuf) {
    for renames in &state.unacknowledged_engine_renames {
        remap_path(path, renames);
    }
}

fn process_playback_snapshot_event(
    snapshot: PlaybackSnapshot,
    analysis: &AnalysisEngine,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let next_state = snapshot.state;
    let previous_playback = state.playback.clone();
    let mut urgency = SnapshotUrgency::None;
    if state.playback != snapshot {
        urgency = playback_snapshot_urgency(&state.playback, &snapshot);
        state.playback = snapshot;
    }
    match (state.playback.current_queue_index, &state.playback.current) {
        (Some(row), _) => state.last_current_row = Some((state.queue_sync.revision(), row)),
        (None, None) => state.last_current_row = None,
        (None, Some(_)) => {}
    }
    if next_state == PlaybackState::Playing && previous_playback.state == PlaybackState::Stopped {
        // Queued files may have changed on disk while nothing played.
        super::sync_queue_details(state);
    }
    if next_state == PlaybackState::Stopped {
        if previous_playback.state != PlaybackState::Stopped {
            // Stop resets the transport to the beginning (the backend
            // always reports position 0 once stopped), and the Qt side
            // re-anchors the centered spectrogram viewport to column 0 on
            // the same transition.  The centered ring only retains a
            // window around the playhead, so the start of the track has
            // usually been evicted by then — restart the decode session
            // at 0 so the snapped viewport shows content instead of
            // vacated slots.  Rolling mode is skipped: its write-order
            // history stays parked while stopped and the next play
            // restarts its session anyway.
            if state.analysis_track_token != 0 {
                if let Some(cmd) =
                    stopped_track_spectrogram_restart(state.settings.spectrogram_display_mode)
                {
                    analysis.command(cmd);
                }
            }
        }
        if !state.analysis.waveform_peaks.is_empty() {
            state.analysis.waveform_peaks.clear();
            state.analysis.waveform_coverage_seconds = 0.0;
            state.analysis.waveform_complete = false;
            urgency = SnapshotUrgency::Immediate;
        }
        return urgency;
    }
    let mut fired_pending_track_change = false;
    if let Some(pending) = state.pending_waveform_track.take() {
        if state.playback.current.as_ref() == Some(&pending.path) {
            fired_pending_track_change = true;
            profile_eprintln!(
                "[bridge] deferred pending_waveform_track firing → SetTrack token={}",
                pending.track_token,
            );
            analysis.command(AnalysisCommand::SetTrack {
                path: pending.path,
                reset_spectrogram: pending.reset_spectrogram,
                track_token: pending.track_token,
                gapless: false,
            });
        } else {
            profile_eprintln!("[bridge] deferred pending_waveform_track SKIPPED (path mismatch)",);
        }
    }
    let replayed_same_track_from_stop = previous_playback.state == PlaybackState::Stopped
        && next_state == PlaybackState::Playing
        && previous_playback.current.is_some()
        && previous_playback.current == state.playback.current
        && !fired_pending_track_change
        && state.pending_waveform_track.is_none()
        && state.analysis_track_token != 0;
    if replayed_same_track_from_stop {
        let pos_seconds = state.playback.position.as_secs_f64();
        profile_eprintln!(
            "[bridge] stopped->playing replay → RestartCurrentTrack token={}",
            state.analysis_track_token,
        );
        analysis.command(AnalysisCommand::RestartCurrentTrack {
            position_seconds: pos_seconds,
            clear_history: true,
        });
    }
    // Forward current playback position so the spectrogram decode worker
    // knows how far playback has progressed and can keep its lookahead
    // window moving forward.
    if next_state == PlaybackState::Playing
        && state.analysis_track_token != 0
        && state.playback.current.is_some()
        && !replayed_same_track_from_stop
    {
        let pos_seconds = state.playback.position.as_secs_f64();
        #[cfg(feature = "profiling-logs")]
        let position_trace =
            playback_position_trace(previous_playback.position, state.playback.position);
        if heartbeat_trace_enabled() {
            profile_eprintln!(
                "[bridge-pos] prev_ms={} next_ms={} delta_ms={} urgency={urgency:?} token={} current={}",
                position_trace.previous,
                position_trace.current,
                position_trace.delta,
                state.analysis_track_token,
                state
                    .playback
                    .current
                    .as_ref()
                    .map_or_else(|| "<none>".to_string(), |path| path.display().to_string()),
            );
        }
        analysis.command(AnalysisCommand::PositionUpdate(pos_seconds));
    }
    urgency
}

/// Command (if any) that re-decodes the spectrogram when the transport
/// transitions into Stopped.  See the call site in
/// `process_playback_snapshot_event` for the full rationale.
fn stopped_track_spectrogram_restart(
    display_mode: SpectrogramDisplayMode,
) -> Option<AnalysisCommand> {
    if display_mode != SpectrogramDisplayMode::Centered {
        return None;
    }
    Some(AnalysisCommand::RestartCurrentTrack {
        position_seconds: 0.0,
        clear_history: true,
    })
}

pub(super) fn drain_playback_events(
    playback_rx: &Receiver<PlaybackEvent>,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    for _ in 0..192 {
        let Ok(event) = playback_rx.try_recv() else {
            break;
        };
        urgency = urgency.max(process_playback_event(
            event, analysis, metadata, event_tx, state,
        ));
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_playback_events(
    playback_rx: &Receiver<PlaybackEvent>,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    state: &mut BridgeState,
) -> bool {
    let mut changed = false;
    let (event_tx, _event_rx) = bounded(16);
    for _ in 0..192 {
        let Ok(event) = playback_rx.try_recv() else {
            break;
        };
        let event_changed = !matches!(event, PlaybackEvent::Seeked { .. });
        let _ = process_playback_event(event, analysis, metadata, &event_tx, state);
        changed |= event_changed;
    }
    changed
}

pub(super) fn process_analysis_event(
    snapshot: AnalysisSnapshot,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    state.analysis.sample_rate_hz = snapshot.sample_rate_hz;
    state.analysis.waveform_coverage_seconds = snapshot.waveform_coverage_seconds;
    state.analysis.waveform_complete = snapshot.waveform_complete;
    if !snapshot.waveform_peaks.is_empty() {
        state.analysis.waveform_peaks = snapshot.waveform_peaks;
    }
    SnapshotUrgency::Analysis
}

pub(super) fn note_precomputed_spectrogram_chunk(
    _state: &mut BridgeState,
    _chunk: &crate::analysis::PrecomputedSpectrogramChunk,
) {
    // With the ring buffer approach, we no longer gate seeks on completion.
}

pub(super) fn drain_analysis_events(
    analysis_rx: &Receiver<AnalysisEvent>,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    for _ in 0..8 {
        let Ok(event) = analysis_rx.try_recv() else {
            break;
        };
        match event {
            AnalysisEvent::Snapshot(snapshot) => {
                urgency = urgency.max(process_analysis_event(snapshot, state));
            }
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                note_precomputed_spectrogram_chunk(state, &chunk);
                let _ = event_tx.send(BridgeEvent::PrecomputedSpectrogramChunk(chunk));
            }
            AnalysisEvent::WorkerFailed(message) => {
                let _ = try_send_event(
                    event_tx,
                    BridgeEvent::Error(BridgeError::error(ErrorCategory::Analysis, message)),
                );
            }
        }
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_analysis_events(
    analysis_rx: &Receiver<AnalysisEvent>,
    state: &mut BridgeState,
) -> bool {
    let (event_tx, _event_rx) = bounded::<BridgeEvent>(8);
    drain_analysis_events(analysis_rx, &event_tx, state).is_pending()
}

pub(super) fn process_metadata_event(
    event: MetadataEvent,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    match event {
        MetadataEvent::Loaded(mut metadata) => {
            keep_newer_cover(state, &mut metadata);
            state.metadata = metadata;
            SnapshotUrgency::Immediate
        }
    }
}

pub(super) fn drain_metadata_events(
    metadata_rx: &Receiver<MetadataEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    for _ in 0..4 {
        let Ok(event) = metadata_rx.try_recv() else {
            break;
        };
        urgency = urgency.max(process_metadata_event(event, state));
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_metadata_events(
    metadata_rx: &Receiver<MetadataEvent>,
    state: &mut BridgeState,
) -> bool {
    drain_metadata_events(metadata_rx, state).is_pending()
}

/// Take Last.fm's new state, and show the user what went wrong since the
/// previous one.
pub(super) fn process_lastfm_state(
    mut runtime: lastfm::RuntimeState,
    state: &mut BridgeState,
    flags: &mut BridgeLoopFlags,
    event_tx: &Sender<BridgeEvent>,
) -> SnapshotUrgency {
    for problem in std::mem::take(&mut runtime.problems) {
        let error = if problem.warning {
            BridgeError::warning(ErrorCategory::LastFm, problem.message)
        } else {
            BridgeError::error(ErrorCategory::LastFm, problem.message)
        };
        let _ = try_send_event(event_tx, BridgeEvent::Error(error));
    }
    let mut urgency = SnapshotUrgency::None;
    if state.lastfm != runtime {
        state.lastfm = runtime.clone();
        urgency = SnapshotUrgency::Immediate;
    }
    if state.settings.integrations.lastfm_username != runtime.username {
        state.settings.integrations.lastfm_username = runtime.username;
        flags.settings_dirty = true;
        // Save now — the username is the lookup key for the session
        // in the keyring.  Losing it (e.g. Ctrl+C before the
        // periodic 2 s save) makes the session irrecoverable.
        flags.settings_save_timing = SettingsSaveTiming::Now;
        urgency = SnapshotUrgency::Immediate;
    }
    urgency
}

pub(super) fn tick_lastfm_playback(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
) {
    tick_lastfm_playback_at(
        state,
        lastfm_handle,
        tracker,
        Instant::now(),
        unix_timestamp_now(),
    );
}

pub(super) fn tick_lastfm_playback_at(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
    now: Instant,
    now_utc: i64,
) {
    let current_path = state.playback.current.clone();
    let track_change = &state.last_track_change;
    // A track change for the current path starts a new play even though the
    // path stays the same, as when the current file is played again. A change
    // to another path takes effect once playback reports that path.
    let restarted = track_change.path.is_some()
        && track_change.path == current_path
        && track_change.generation != tracker.track_change_generation;
    if restarted || tracker.active_path != current_path {
        finalize_lastfm_track(state, lastfm_handle, tracker, now);
        *tracker = LastFmPlaybackTracker {
            active_path: current_path,
            track_change_generation: track_change.generation,
            duration_seconds: match u32::try_from(state.playback.duration.as_secs()) {
                Ok(value) if value > 0 => Some(value),
                _ => None,
            },
            ..LastFmPlaybackTracker::default()
        };
    }

    if state.playback.state == PlaybackState::Stopped || tracker.active_path.is_none() {
        finalize_lastfm_track(state, lastfm_handle, tracker, now);
        *tracker = LastFmPlaybackTracker {
            track_change_generation: tracker.track_change_generation,
            ..LastFmPlaybackTracker::default()
        };
        return;
    }

    // Playback may correct the duration during the play. Follow it until the
    // play qualifies, so the threshold matches the track.
    if !tracker.scrobble_queued {
        if let Ok(value @ 1..) = u32::try_from(state.playback.duration.as_secs()) {
            tracker.duration_seconds = Some(value);
        }
    }
    if tracker.track_number.is_none() {
        tracker.track_number =
            current_track_number(state, &mut tracker.library_without_track_number);
    }

    let metadata_matches_current = state
        .metadata
        .source_path
        .as_ref()
        .zip(state.playback.current.as_ref())
        .is_some_and(|(source, path)| source == &path.to_string_lossy());
    if metadata_matches_current {
        tracker.artist = state.metadata.artist.trim().to_string();
        tracker.track = state.metadata.title.trim().to_string();
        tracker.album = state.metadata.album.trim().to_string();
    }

    if state.playback.state == PlaybackState::Playing {
        if tracker.started_at_utc.is_none() {
            tracker.started_at_utc = Some(now_utc);
        }
        advance_lastfm_listened_duration(tracker, now);
        tracker.last_listen_tick = Some(now);
    } else {
        tracker.last_listen_tick = None;
    }
    // Queue as soon as the play qualifies, so that quitting or crashing
    // later in the track cannot lose it.
    queue_lastfm_scrobble_if_ready(state, lastfm_handle, tracker);

    if state.lastfm.enabled
        && !tracker.now_playing_sent
        && state.playback.state == PlaybackState::Playing
        && tracker.started_at_utc.is_some()
        && !tracker.artist.is_empty()
        && !tracker.track.is_empty()
    {
        lastfm_handle.command(LastFmCommand::SendNowPlaying(LastFmNowPlayingTrack {
            artist: tracker.artist.clone(),
            track: tracker.track.clone(),
            album: lastfm::normalize_album_for_submission(&tracker.album),
            track_number: tracker.track_number,
            duration_seconds: tracker.duration_seconds,
        }));
        tracker.now_playing_sent = true;
    }
}

/// Credits listening up to `now` and queues the scrobble if the play
/// qualifies. Runs when a play ends, including at shutdown.
pub(super) fn finalize_lastfm_track(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
    now: Instant,
) {
    advance_lastfm_listened_duration(tracker, now);
    queue_lastfm_scrobble_if_ready(state, lastfm_handle, tracker);
}

fn advance_lastfm_listened_duration(tracker: &mut LastFmPlaybackTracker, now: Instant) {
    if let Some(previous_tick) = tracker.last_listen_tick {
        tracker.listened_duration = tracker
            .listened_duration
            .saturating_add(now.saturating_duration_since(previous_tick));
    }
}

fn queue_lastfm_scrobble_if_ready(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
) {
    if !state.lastfm.enabled || tracker.scrobble_queued || tracker.started_at_utc.is_none() {
        return;
    }
    let Some(duration_seconds) = tracker.duration_seconds else {
        return;
    };
    let Some(threshold_seconds) = lastfm::scrobble_threshold_seconds(duration_seconds) else {
        return;
    };
    if tracker.listened_duration < Duration::from_secs(u64::from(threshold_seconds)) {
        return;
    }
    if tracker.artist.is_empty() || tracker.track.is_empty() {
        return;
    }
    lastfm_handle.command(LastFmCommand::QueueScrobble(LastFmScrobbleEntry {
        artist: tracker.artist.clone(),
        track: tracker.track.clone(),
        album: lastfm::normalize_album_for_submission(&tracker.album),
        track_number: tracker.track_number,
        duration_seconds: tracker.duration_seconds,
        timestamp_utc: tracker.started_at_utc.unwrap_or_else(unix_timestamp_now),
    }));
    tracker.scrobble_queued = true;
}

/// The current track's number, from its queue details or its library track.
/// `checked` is the library last found to have no number for the track,
/// which is not searched again: this runs on every pass of the bridge loop.
pub(super) fn current_track_number(
    state: &BridgeState,
    checked: &mut Weak<LibrarySnapshot>,
) -> Option<u32> {
    let path = state.playback.current.as_ref()?;
    if let Some(number) = state
        .queue_details
        .get(path)
        .and_then(|track| track.track_no)
    {
        return Some(number);
    }
    if std::ptr::eq(checked.as_ptr(), Arc::as_ptr(&state.library)) {
        return None;
    }
    let number = library_track_number(state, path);
    if number.is_none() {
        *checked = Arc::downgrade(&state.library);
    }
    number
}

fn library_track_number(state: &BridgeState, path: &Path) -> Option<u32> {
    #[cfg(test)]
    test_hooks::LIBRARY_TRACK_NUMBER_LOOKUPS.with(|lookups| lookups.set(lookups.get() + 1));
    let position = state.library_index.position(&state.library, path)?;
    state.library.tracks[position].track_no
}

#[cfg(test)]
pub(super) mod test_hooks {
    use std::cell::Cell;

    thread_local! {
        /// How often this thread looked up the current track's number in
        /// the library.
        pub(in super::super) static LIBRARY_TRACK_NUMBER_LOOKUPS: Cell<usize> = const { Cell::new(0) };
    }
}

pub(super) fn unix_timestamp_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| i64::try_from(duration.as_secs()).unwrap_or(i64::MAX))
        .unwrap_or(0)
}

/// What a batch of library events changed in the bridge's library.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(super) struct LibraryEventsApplied {
    /// How many tracks were upserted or removed; roots count as one.
    pub(super) rows_changed: usize,
    pub(super) status_changed: bool,
}

impl LibraryEventsApplied {
    pub(super) fn urgency(self) -> SnapshotUrgency {
        if self.rows_changed > 0 || self.status_changed {
            SnapshotUrgency::Immediate
        } else {
            SnapshotUrgency::None
        }
    }
}

/// Apply the library worker's events, in order, as one change: the bridge
/// copies its library once for all of them, and only if a row changed. The
/// copy shares every track it keeps, and the path index is kept for it when
/// no track moved.
pub(super) fn apply_library_events(
    events: impl IntoIterator<Item = LibraryEvent>,
    state: &mut BridgeState,
) -> LibraryEventsApplied {
    let mut changes = crate::library::LibraryChanges::default();
    let mut status = None;
    for event in events {
        changes.merge(event.changes);
        status = Some(event.status);
    }
    let Some(status) = status else {
        return LibraryEventsApplied::default();
    };
    let mut applied = LibraryEventsApplied {
        rows_changed: 0,
        status_changed: state.library_status != status,
    };
    state.library_status = status;
    if changes.is_empty() {
        return applied;
    }

    if !state.apply_library_changes(&changes).changed {
        return applied;
    }
    applied.rows_changed =
        changes.upserted.len() + changes.removed.len() + usize::from(changes.roots.is_some());
    follow_current_track_cover(state, &changes.upserted);
    if !state.queue.is_empty() {
        super::sync_queue_details(state);
    }
    applied
}

/// Shows the cover that a scan chose for the current track, such as after
/// its folder's cover image was replaced. Now-playing chooses covers by the
/// library's rule, so the row's cover is the one its metadata would have.
fn follow_current_track_cover(state: &mut BridgeState, upserted: &[Arc<LibraryTrack>]) {
    let Some(current) = state.playback.current.as_ref() else {
        return;
    };
    if state.metadata.source_path.as_deref().map(Path::new) != Some(current.as_path()) {
        return;
    }
    let Some(track) = upserted.iter().rev().find(|track| &track.path == current) else {
        return;
    };
    set_metadata_cover(
        state,
        track.cover_path.clone(),
        track.cover_stamp,
        track.cover_read_at,
    );
}

/// Gives the current metadata the cover chosen at `read_at`, unless the
/// cover it shows was chosen later. Covers reach the bridge from scans,
/// artwork apply and the metadata worker, each after its own delay, so only
/// when they were read tells which is newer.
fn set_metadata_cover(
    state: &mut BridgeState,
    cover_path: String,
    stamp: u64,
    read_at: CoverReadAt,
) {
    if !read_at.is_not_older_than(state.metadata.cover_read_at) {
        return;
    }
    state.metadata.cover_art_path = (!cover_path.is_empty()).then_some(cover_path);
    state.metadata.cover_art_stamp = stamp;
    state.metadata.cover_read_at = read_at;
}

/// Keeps the cover the current metadata shows when `metadata`, of the same
/// track, chose its cover before that one was read; its other fields still
/// apply. See [`set_metadata_cover`].
fn keep_newer_cover(state: &BridgeState, metadata: &mut crate::metadata::TrackMetadata) {
    let shown = &state.metadata;
    if metadata.source_path.is_none()
        || metadata.source_path != shown.source_path
        || metadata
            .cover_read_at
            .is_not_older_than(shown.cover_read_at)
    {
        return;
    }
    metadata.cover_art_path.clone_from(&shown.cover_art_path);
    metadata.cover_art_stamp = shown.cover_art_stamp;
    metadata.cover_read_at = shown.cover_read_at;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lastfm::{self, Command as LastFmCommand};
    use crate::playback::MonotonicTime;
    use std::path::PathBuf;
    use std::time::Instant;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    #[test]
    fn a_track_that_could_not_resume_is_a_playback_warning_about_its_file() {
        let error = playback_error(&crate::playback::PlaybackError {
            path: Some(PathBuf::from("/media/usb/a.flac")),
            detail: "the file is gone, or its drive is not connected".to_string(),
            outcome: PlaybackErrorOutcome::NotResumed,
        });
        assert_eq!(error.category, ErrorCategory::Playback);
        assert_eq!(error.severity, ErrorSeverity::Warning);
        assert_eq!(error.subject, "/media/usb/a.flac");
        assert!(
            error.message.starts_with("Couldn't resume"),
            "{}",
            error.message
        );
    }

    #[test]
    fn complete_precomputed_chunk_is_noted_without_panic() {
        let mut state = BridgeState {
            analysis_track_token: 9,
            ..BridgeState::default()
        };
        let chunk = crate::analysis::PrecomputedSpectrogramChunk {
            track_token: 9,
            generation: 3,
            columns_u8: Vec::new(),
            bins_per_column: 4097,
            column_count: 0,
            channel_count: 1,
            start_column_index: 0,
            total_columns_estimate: 32,
            sample_rate_hz: 44_100,
            hop_size: 1_024,
            coverage_seconds: 1.0,
            complete: true,
            buffer_reset: false,
            clear_history: false,
        };
        note_precomputed_spectrogram_chunk(&mut state, &chunk);
    }

    #[test]
    fn playback_position_trace_computes_signed_delta_ms() {
        let trace =
            playback_position_trace(Duration::from_millis(6_880), Duration::from_millis(7_050));
        assert_eq!(trace.previous, 6_880);
        assert_eq!(trace.current, 7_050);
        assert_eq!(trace.delta, 170);
    }

    #[test]
    fn playback_position_trace_preserves_backward_jumps() {
        let trace =
            playback_position_trace(Duration::from_millis(8_000), Duration::from_millis(7_100));
        assert_eq!(trace.delta, -900);
    }

    #[test]
    fn finished_seeks_reach_the_ui_without_waiting_for_a_heartbeat() {
        let previous = PlaybackSnapshot {
            state: PlaybackState::Playing,
            position: Duration::from_secs(10),
            seek_serial: 4,
            seek_landed_serial: 3,
            ..PlaybackSnapshot::default()
        };
        let moved = PlaybackSnapshot {
            position: Duration::from_secs(11),
            ..previous.clone()
        };
        assert_eq!(
            playback_snapshot_urgency(&previous, &moved),
            SnapshotUrgency::Heartbeat
        );
        // A refused or superseded seek: the playhead must stop waiting.
        let refused = PlaybackSnapshot {
            seek_serial: 5,
            ..previous.clone()
        };
        assert_eq!(
            playback_snapshot_urgency(&previous, &refused),
            SnapshotUrgency::Immediate
        );
        let landed = PlaybackSnapshot {
            position: Duration::from_secs(60),
            seek_serial: 5,
            seek_landed_serial: 5,
            ..previous.clone()
        };
        assert_eq!(
            playback_snapshot_urgency(&previous, &landed),
            SnapshotUrgency::Immediate
        );
    }

    #[test]
    fn position_samples_reach_the_ui_and_a_stopped_clock_without_delay() {
        let previous = PlaybackSnapshot {
            state: PlaybackState::Playing,
            position: Duration::from_secs(10),
            position_sampled_at: MonotonicTime::from_nanos(1_000),
            position_advancing: true,
            ..PlaybackSnapshot::default()
        };
        // A pipeline that stalls reports the same position at a later time.
        let resampled = PlaybackSnapshot {
            position_sampled_at: MonotonicTime::from_nanos(2_000),
            ..previous.clone()
        };
        assert_eq!(
            playback_snapshot_urgency(&previous, &resampled),
            SnapshotUrgency::Heartbeat
        );
        let held = PlaybackSnapshot {
            position_advancing: false,
            ..resampled.clone()
        };
        assert_eq!(
            playback_snapshot_urgency(&resampled, &held),
            SnapshotUrgency::Immediate
        );
        assert_eq!(
            playback_snapshot_urgency(&held, &resampled),
            SnapshotUrgency::Immediate
        );
    }

    #[test]
    fn seek_event_does_not_trigger_early_track_switch_side_effects() {
        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();

        let mut state = BridgeState::default();
        state.playback.current = Some(p("/music/a.flac"));
        state.analysis_track_token = 1;
        state.analysis.waveform_peaks = vec![0.2, 0.4, 0.6];
        state.metadata.title = "Track A".to_string();
        state.metadata.artist = "Artist A".to_string();

        playback_tx
            .send(PlaybackEvent::Seeked {
                position: Duration::from_secs(42),
            })
            .expect("send seeked event");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(!changed);
        // The landing position arrives with the engine's next snapshot,
        // which says when it was read.
        assert_eq!(state.playback.position, Duration::ZERO);
        assert_eq!(state.analysis.waveform_peaks, vec![0.2, 0.4, 0.6]);
        assert_eq!(state.metadata.title, "Track A");
        assert_eq!(state.metadata.artist, "Artist A");

        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: p("/music/b.flac"),
                queue_index: 1,
                kind: TrackChangeKind::Manual,
                track_token: 1,
            })
            .expect("send track-changed event");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(changed);
        assert!(state.analysis.waveform_peaks.is_empty());
    }

    #[test]
    fn stopped_snapshot_clears_waveform_peaks() {
        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();

        let mut state = BridgeState::default();
        state.analysis.waveform_peaks = vec![0.1, 0.2, 0.3];

        let mut snapshot = state.playback.clone();
        snapshot.state = PlaybackState::Stopped;
        playback_tx
            .send(PlaybackEvent::Snapshot(snapshot))
            .expect("send stopped snapshot");

        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(changed);
        assert!(state.analysis.waveform_peaks.is_empty());
    }

    fn playback_harness(
        display_mode: SpectrogramDisplayMode,
    ) -> (
        AnalysisEngine,
        Receiver<AnalysisEvent>,
        crossbeam_channel::Sender<PlaybackEvent>,
        Receiver<PlaybackEvent>,
        MetadataService,
        BridgeState,
    ) {
        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();
        let mut state = BridgeState::default();
        state.settings.spectrogram_display_mode = display_mode;
        state.playback.current = Some(p("/music/track.flac"));
        state.analysis_track_token = 7;
        state.playback.state = PlaybackState::Playing;
        (
            analysis,
            analysis_rx,
            playback_tx,
            playback_rx,
            metadata,
            state,
        )
    }

    #[test]
    fn stopped_transition_restarts_centered_spectrogram_session() {
        let (analysis, analysis_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);

        // Heartbeat while playing must not restart the session.
        let mut playing_snapshot = state.playback.clone();
        playing_snapshot.position = Duration::from_secs(170);
        playback_tx
            .send(PlaybackEvent::Snapshot(playing_snapshot))
            .expect("send playing snapshot");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);

        // Stop transition: the centered session must restart at 0 so the
        // re-anchored viewport shows decoded content again instead of
        // vacated ring slots.
        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        playback_tx
            .send(PlaybackEvent::Snapshot(stopped_snapshot))
            .expect("send stopped snapshot");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);

        let event = analysis_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("stop transition should restart the centered spectrogram session");
        assert!(matches!(event, AnalysisEvent::Snapshot(_)));
    }

    #[test]
    fn stopped_transition_keeps_rolling_spectrogram_parked() {
        let (analysis, analysis_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Rolling);

        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        playback_tx
            .send(PlaybackEvent::Snapshot(stopped_snapshot))
            .expect("send stopped snapshot");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);

        // Rolling rings are write-order history; the stop transition must
        // not restart (and thereby clear) the rolling session.
        assert!(analysis_rx
            .recv_timeout(Duration::from_millis(300))
            .is_err());
    }

    #[test]
    fn heartbeat_while_stopped_does_not_restart_spectrogram_session() {
        let (analysis, analysis_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);

        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        playback_tx
            .send(PlaybackEvent::Snapshot(stopped_snapshot))
            .expect("send stopped snapshot");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);

        let event = analysis_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("first stop transition should restart the session");
        assert!(matches!(event, AnalysisEvent::Snapshot(_)));
        while analysis_rx.try_recv().is_ok() {}

        // Stopped heartbeats repeat every ~100 ms; only the transition may
        // restart the session.
        let mut repeat_snapshot = state.playback.clone();
        repeat_snapshot.state = PlaybackState::Stopped;
        repeat_snapshot.position = Duration::ZERO;
        playback_tx
            .send(PlaybackEvent::Snapshot(repeat_snapshot))
            .expect("send repeat stopped snapshot");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);

        assert!(analysis_rx
            .recv_timeout(Duration::from_millis(300))
            .is_err());
    }

    /// A playing 240 s track (scrobble threshold 120 s) with its metadata.
    fn lastfm_playing_state(track_path: &Path) -> BridgeState {
        let mut state = BridgeState::default();
        state.lastfm.enabled = true;
        state.playback.current = Some(track_path.to_path_buf());
        state.playback.state = PlaybackState::Playing;
        state.playback.duration = Duration::from_secs(240);
        state.metadata.source_path = Some(track_path.to_string_lossy().into_owned());
        state.metadata.artist = "Artist".to_string();
        state.metadata.title = "Track".to_string();
        state.metadata.album = "Album".to_string();
        state
    }

    fn queued_scrobbles(commands: &[LastFmCommand]) -> Vec<LastFmScrobbleEntry> {
        commands
            .iter()
            .filter_map(|command| match command {
                LastFmCommand::QueueScrobble(entry) => Some(entry.clone()),
                _ => None,
            })
            .collect()
    }

    fn now_playing_tracks(commands: &[LastFmCommand]) -> Vec<String> {
        commands
            .iter()
            .filter_map(|command| match command {
                LastFmCommand::SendNowPlaying(track) => Some(track.track.clone()),
                _ => None,
            })
            .collect()
    }

    fn library_track_number_lookups() -> usize {
        test_hooks::LIBRARY_TRACK_NUMBER_LOOKUPS.with(std::cell::Cell::get)
    }

    #[test]
    fn a_track_without_a_number_is_looked_up_once_per_library_and_track() {
        use crate::library::{LibrarySnapshot, LibraryTrack};

        let (lastfm_handle, _commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/untitled.flac");
        let other_path = p("/music/other.flac");
        let library = |track_no| {
            Arc::new(LibrarySnapshot {
                tracks: (0..1000)
                    .map(|index| LibraryTrack {
                        path: p(&format!("/music/filler-{index}.flac")),
                        ..LibraryTrack::default()
                    })
                    .chain(
                        [track_path.clone(), other_path.clone()].map(|path| LibraryTrack {
                            path,
                            track_no,
                            ..LibraryTrack::default()
                        }),
                    )
                    .map(std::sync::Arc::new)
                    .collect(),
                ..LibrarySnapshot::default()
            })
        };
        let mut state = lastfm_playing_state(&track_path);
        state.library = library(None);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        let before = library_track_number_lookups();
        let tick = |state: &BridgeState, tracker: &mut LastFmPlaybackTracker, wake: u64| {
            tick_lastfm_playback_at(
                state,
                &lastfm_handle,
                tracker,
                start + Duration::from_millis(wake * 40),
                1_700_000_000,
            );
        };

        // The loop wakes about 25 times a second while playing.
        for wake in 0..250 {
            tick(&state, &mut tracker, wake);
        }
        assert_eq!(tracker.track_number, None);
        assert_eq!(library_track_number_lookups() - before, 1);
        assert_eq!(state.library_index.builds(), 1);

        // A new library may have the number.
        state.library = library(Some(7));
        for wake in 250..260 {
            tick(&state, &mut tracker, wake);
        }
        assert_eq!(tracker.track_number, Some(7));
        assert_eq!(library_track_number_lookups() - before, 2);

        // So may another track.
        state.library = library(None);
        state.playback.current = Some(other_path.clone());
        state.metadata.source_path = Some(other_path.to_string_lossy().into_owned());
        for wake in 260..270 {
            tick(&state, &mut tracker, wake);
        }
        assert_eq!(tracker.track_number, None);
        assert_eq!(library_track_number_lookups() - before, 3);
    }

    #[test]
    fn lastfm_scrobble_requires_actual_listened_time_instead_of_seek_position() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/seek-test.flac");
        let mut state = lastfm_playing_state(&track_path);
        state.playback.position = Duration::from_secs(5);

        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);
        assert_eq!(tracker.listened_duration, Duration::ZERO);

        // Seeking past the threshold is not listening.
        state.playback.position = Duration::from_secs(180);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(1),
            1_700_000_001,
        );
        assert!(tracker.listened_duration < Duration::from_secs(2));
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(119),
            1_700_000_119,
        );
        assert!(!tracker.scrobble_queued);
        assert!(queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).is_empty());

        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(120),
            1_700_000_120,
        );
        assert!(tracker.scrobble_queued);
        let queued = queued_scrobbles(&commands.try_iter().collect::<Vec<_>>());
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].artist, "Artist");
        assert_eq!(queued[0].track, "Track");
        assert_eq!(queued[0].timestamp_utc, 1_700_000_000);
    }

    #[test]
    fn lastfm_scrobble_is_queued_at_the_threshold_once_per_play() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/threshold.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();

        for seconds in [0, 60, 121] {
            tick_lastfm_playback_at(
                &state,
                &lastfm_handle,
                &mut tracker,
                start + Duration::from_secs(seconds),
                1_700_000_000,
            );
        }
        // Queued while the track is still playing, before it ends.
        assert_eq!(
            queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).len(),
            1
        );

        // Neither more listening, a seek back, nor stopping queues it again.
        state.playback.position = Duration::ZERO;
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(239),
            1_700_000_239,
        );
        state.playback.state = PlaybackState::Stopped;
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(240),
            1_700_000_240,
        );
        assert!(queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).is_empty());
    }

    #[test]
    fn lastfm_scrobble_does_not_queue_when_disabled() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/disabled-scrobble.flac");
        let mut state = lastfm_playing_state(&track_path);
        state.lastfm.enabled = false;

        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(150),
            1_700_000_150,
        );
        state.playback.state = PlaybackState::Stopped;
        state.playback.current = None;
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(151),
            1_700_000_151,
        );
        finalize_lastfm_track(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(152),
        );

        assert!(!tracker.scrobble_queued);
        assert!(commands.try_iter().next().is_none());
    }

    #[test]
    fn lastfm_scrobble_omits_unknown_album_placeholder() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/unknown-album-scrobble.flac");
        let mut state = lastfm_playing_state(&track_path);
        state.metadata.artist = "Boards of Canada".to_string();
        state.metadata.title = "Web Loops".to_string();
        state.metadata.album = "Unknown album".to_string();

        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(121),
            1_700_000_121,
        );

        let queued = queued_scrobbles(&commands.try_iter().collect::<Vec<_>>());
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].artist, "Boards of Canada");
        assert_eq!(queued[0].track, "Web Loops");
        assert!(queued[0].album.is_empty());
    }

    #[test]
    fn lastfm_replaying_the_current_track_scrobbles_each_play() {
        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/repeat-one.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();

        // Playing the file, its gapless repeat, and playing it again by hand.
        // Every token is 0 here, so only the track change itself can tell
        // the plays apart.
        for (offset, started_utc, kind) in [
            (0, 1_700_000_000, TrackChangeKind::Manual),
            (240, 1_700_000_240, TrackChangeKind::Gapless),
            (480, 1_700_000_480, TrackChangeKind::Manual),
        ] {
            playback_tx
                .send(PlaybackEvent::TrackChanged {
                    path: track_path.clone(),
                    queue_index: 0,
                    kind,
                    track_token: 0,
                })
                .expect("send track change");
            pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
            tick_lastfm_playback_at(
                &state,
                &lastfm_handle,
                &mut tracker,
                start + Duration::from_secs(offset),
                started_utc,
            );
            tick_lastfm_playback_at(
                &state,
                &lastfm_handle,
                &mut tracker,
                start + Duration::from_secs(offset + 130),
                started_utc + 130,
            );
        }

        let sent = commands.try_iter().collect::<Vec<_>>();
        let timestamps = queued_scrobbles(&sent)
            .iter()
            .map(|entry| entry.timestamp_utc)
            .collect::<Vec<_>>();
        assert_eq!(
            timestamps,
            vec![1_700_000_000, 1_700_000_240, 1_700_000_480]
        );
        assert_eq!(now_playing_tracks(&sent).len(), 3);
    }

    #[test]
    fn lastfm_track_change_to_another_path_waits_for_playback_to_switch() {
        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let first = p("/music/first.flac");
        let second = p("/music/second.flac");
        let mut state = lastfm_playing_state(&first);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);

        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: second.clone(),
                queue_index: 1,
                kind: TrackChangeKind::Gapless,
                track_token: 2,
            })
            .expect("send track change");
        pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(10),
            1_700_000_010,
        );
        // Still the first track's play until playback reports the switch.
        assert_eq!(tracker.active_path.as_ref(), Some(&first));
        assert_eq!(tracker.listened_duration, Duration::from_secs(10));

        state.playback.current = Some(second.clone());
        state.metadata.source_path = Some(second.to_string_lossy().into_owned());
        state.metadata.title = "Second".to_string();
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(11),
            1_700_000_011,
        );
        assert_eq!(tracker.active_path.as_ref(), Some(&second));
        assert_eq!(tracker.started_at_utc, Some(1_700_000_011));
        assert_eq!(
            now_playing_tracks(&commands.try_iter().collect::<Vec<_>>()),
            vec!["Track".to_string(), "Second".to_string()]
        );
    }

    #[test]
    fn lastfm_finalize_queues_a_play_that_qualified_since_the_last_tick() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/final-track.flac");
        let state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);

        finalize_lastfm_track(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(130),
        );
        finalize_lastfm_track(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(131),
        );

        let queued = queued_scrobbles(&commands.try_iter().collect::<Vec<_>>());
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].timestamp_utc, 1_700_000_000);
    }

    #[test]
    fn lastfm_threshold_follows_duration_corrections_until_the_play_qualifies() {
        let (lastfm_handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/music/corrected.flac");
        let start = Instant::now();
        let at = |seconds| start + Duration::from_secs(seconds);

        // 600 s corrected to 120 s: the threshold drops from 240 s to 60 s.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.duration = Duration::from_secs(600);
        let mut tracker = LastFmPlaybackTracker::default();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(0), 1_700_000_000);
        state.playback.duration = Duration::from_secs(120);
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(30), 1_700_000_030);
        assert!(queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).is_empty());
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(60), 1_700_000_060);
        let queued = queued_scrobbles(&commands.try_iter().collect::<Vec<_>>());
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].duration_seconds, Some(120));

        // 120 s corrected to 600 s: 60 s of listening no longer qualifies.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.duration = Duration::from_secs(120);
        let mut tracker = LastFmPlaybackTracker::default();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(0), 1_700_001_000);
        state.playback.duration = Duration::from_secs(600);
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(30), 1_700_001_030);
        // A moment without a known duration keeps the last one.
        state.playback.duration = Duration::ZERO;
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(60), 1_700_001_060);
        assert!(queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).is_empty());
        state.playback.duration = Duration::from_secs(600);
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, at(240), 1_700_001_240);
        let queued = queued_scrobbles(&commands.try_iter().collect::<Vec<_>>());
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].duration_seconds, Some(600));
    }

    fn library_event(
        upserted: Vec<crate::library::LibraryTrack>,
        removed: Vec<PathBuf>,
    ) -> LibraryEvent {
        LibraryEvent {
            changes: crate::library::LibraryChanges {
                roots: None,
                upserted: upserted.into_iter().map(Arc::new).collect(),
                removed,
            },
            ..LibraryEvent::default()
        }
    }

    fn library_track(path: &str, title: &str) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
            path: p(path),
            root_path: p("/music"),
            title: title.to_string(),
            ..crate::library::LibraryTrack::default()
        }
    }

    fn filler_library(count: usize) -> Vec<crate::library::LibraryTrack> {
        (0..count)
            .map(|index| {
                library_track(
                    &format!("/music/Artist {}/Album/{index}.flac", index % 50),
                    "Song",
                )
            })
            .collect()
    }

    #[test]
    fn pending_library_events_are_applied_as_one_change() {
        let mut state = BridgeState {
            queue: vec![p("/elsewhere/queued.flac")],
            ..BridgeState::default()
        };
        apply_library_events([library_event(filler_library(10), Vec::new())], &mut state);
        let generation = state.queue_detail_generation;
        let revision = state.library.search_revision;

        let mut scanning = crate::library::LibraryStatus {
            scan_in_progress: true,
            ..crate::library::LibraryStatus::default()
        };
        let mut events = Vec::new();
        for batch in 0..3 {
            let mut event = library_event(
                vec![library_track(
                    &format!("/music/New/Album/{batch}.flac"),
                    "New",
                )],
                Vec::new(),
            );
            event.status = scanning.clone();
            events.push(event);
        }
        events.push(library_event(
            vec![library_track(
                "/music/New/Album/1.flac",
                "Renamed in the scan",
            )],
            vec![p("/music/Artist 3/Album/3.flac")],
        ));
        scanning.scan_in_progress = false;
        scanning.last_error = Some("one file could not be read".to_string());
        events.last_mut().expect("event").status = scanning;

        let applied = apply_library_events(events, &mut state);

        assert_eq!(
            state.queue_detail_generation,
            generation + 1,
            "the queue's details are checked once for the batch"
        );
        assert_eq!(
            state.library.search_revision,
            revision + 1,
            "one library for the batch"
        );
        assert!(applied.status_changed);
        assert!(!state.library_status.scan_in_progress, "the latest status");
        assert_eq!(
            state.library_status.last_error.as_deref(),
            Some("one file could not be read")
        );
        assert_eq!(state.library.tracks.len(), 12);
        let renamed = state
            .library_index
            .position(&state.library, Path::new("/music/New/Album/1.flac"))
            .expect("added track");
        assert_eq!(state.library.tracks[renamed].title, "Renamed in the scan");
        assert!(!state
            .library_index
            .contains(&state.library, Path::new("/music/Artist 3/Album/3.flac")));
    }

    #[test]
    fn a_one_row_change_to_a_large_library_does_not_copy_or_count_the_library() {
        let mut state = BridgeState::default();
        apply_library_events(
            [library_event(filler_library(5_000), Vec::new())],
            &mut state,
        );
        // The index is built when it is first needed.
        assert!(state
            .library_index
            .contains(&state.library, Path::new("/music/Artist 0/Album/0.flac")));
        let edit = library_event(
            vec![library_track("/music/Artist 7/Album/7.flac", "Edited")],
            Vec::new(),
        );
        let addition = library_event(
            vec![library_track("/music/Artist 9/Album/new.flac", "New")],
            Vec::new(),
        );

        let before = crate::test_alloc::allocations();
        let applied = apply_library_events([edit, addition], &mut state);
        let allocations = crate::test_alloc::allocations() - before;

        assert_eq!(applied.rows_changed, 2);
        // Copying the track list's pointers and merging the index allocate a
        // few times; copying or counting the tracks would allocate for each.
        assert!(allocations < 100, "{allocations} allocations");
        assert_eq!(state.library.tracks.len(), 5_001);
    }

    #[test]
    fn the_path_index_follows_library_changes() {
        let mut state = BridgeState::default();
        apply_library_events([library_event(filler_library(100), Vec::new())], &mut state);
        assert!(state
            .library_index
            .contains(&state.library, Path::new("/music/Artist 1/Album/1.flac")));
        assert_eq!(state.library_index.builds(), 1);

        // Edits and additions keep the index.
        apply_library_events(
            [library_event(
                vec![
                    library_track("/music/Artist 1/Album/1.flac", "Edited"),
                    library_track("/music/Artist 2/Album/new.flac", "New"),
                ],
                Vec::new(),
            )],
            &mut state,
        );
        let new = state
            .library_index
            .position(&state.library, Path::new("/music/Artist 2/Album/new.flac"))
            .expect("new track");
        assert_eq!(state.library.tracks[new].title, "New");
        let edited = state
            .library_index
            .position(&state.library, Path::new("/music/Artist 1/Album/1.flac"))
            .expect("edited track");
        assert_eq!(state.library.tracks[edited].title, "Edited");
        for (position, track) in state.library.tracks.iter().enumerate() {
            assert_eq!(
                state.library_index.position(&state.library, &track.path),
                Some(position)
            );
        }
        assert_eq!(state.library_index.builds(), 1, "no rebuild for additions");

        // A removal moves tracks, and the index is built again.
        apply_library_events(
            [library_event(
                Vec::new(),
                vec![p("/music/Artist 0/Album/0.flac")],
            )],
            &mut state,
        );
        assert!(!state
            .library_index
            .contains(&state.library, Path::new("/music/Artist 0/Album/0.flac")));
        for (position, track) in state.library.tracks.iter().enumerate() {
            assert_eq!(
                state.library_index.position(&state.library, &track.path),
                Some(position)
            );
        }
    }

    /// Covers read at `base + millis`, in the order the workers read them.
    fn read_at(base: Instant, millis: u64) -> CoverReadAt {
        CoverReadAt(Some(base + Duration::from_millis(millis)))
    }

    fn covered_metadata(
        source: &str,
        cover: &str,
        stamp: u64,
        read: CoverReadAt,
    ) -> crate::metadata::TrackMetadata {
        crate::metadata::TrackMetadata {
            source_path: Some(source.to_owned()),
            cover_art_path: Some(cover.to_string()),
            cover_art_stamp: stamp,
            cover_read_at: read,
            ..crate::metadata::TrackMetadata::default()
        }
    }

    fn covered_row(cover: &str, stamp: u64, read: CoverReadAt) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
            cover_path: cover.to_string(),
            cover_stamp: stamp,
            cover_read_at: read,
            ..library_track("/music/Artist/Album/01.flac", "Song")
        }
    }

    fn playing_state() -> (BridgeState, String) {
        let current = p("/music/Artist/Album/01.flac");
        let mut state = BridgeState::default();
        state.playback.current = Some(current.clone());
        (state, current.to_string_lossy().into_owned())
    }

    #[test]
    fn a_metadata_result_read_before_a_cover_refresh_does_not_undo_it() {
        let base = Instant::now();
        let (mut state, source) = playing_state();
        let cover = "/music/Artist/Album/cover.jpg";
        // The metadata worker chose the cover, then went on probing the
        // file; its later results carry the cover it chose first.
        let read = covered_metadata(&source, cover, 1, read_at(base, 0));
        process_metadata_event(MetadataEvent::Loaded(read.clone()), &mut state);
        assert_eq!(state.metadata.cover_art_stamp, 1);

        // The image is replaced, and the watcher's scan gives the row stamp 2.
        let row = covered_row(cover, 2, read_at(base, 10));
        apply_library_events([library_event(vec![row], Vec::new())], &mut state);
        assert_eq!(state.metadata.cover_art_stamp, 2);

        // The probe's result arrives: its details apply, its cover does not.
        let probed = crate::metadata::TrackMetadata {
            bitrate_kbps: Some(905),
            ..read
        };
        process_metadata_event(MetadataEvent::Loaded(probed), &mut state);
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(cover));
        assert_eq!(state.metadata.cover_art_stamp, 2);
        assert_eq!(state.metadata.bitrate_kbps, Some(905));

        // A read made since then is newer than the scan's cover.
        let front = "/music/Artist/Album/front.jpg";
        let reread = covered_metadata(&source, front, 3, read_at(base, 20));
        process_metadata_event(MetadataEvent::Loaded(reread), &mut state);
        assert_eq!(state.metadata.cover_art_stamp, 3);
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(front));
    }

    #[test]
    fn a_scan_that_read_the_cover_before_artwork_apply_does_not_restore_it() {
        let base = Instant::now();
        let (mut state, source) = playing_state();
        let old = "/music/Artist/Album/folder.jpg";
        let new = "/music/Artist/Album/cover.png";
        process_metadata_event(
            MetadataEvent::Loaded(covered_metadata(&source, old, 1, read_at(base, 0))),
            &mut state,
        );
        // 1. A large rescan reads the old cover; its batch is published later.
        let delayed_scan = covered_row(old, 1, read_at(base, 10));
        // 2. Artwork apply installs a new cover and reads it.
        let applied = IndexedTrack {
            cover_path: new.to_string(),
            cover_stamp: 2,
            cover_read_at: read_at(base, 20),
            ..IndexedTrack::from(&delayed_scan)
        };
        // 3. The metadata worker reads the new cover too.
        let reread = covered_metadata(&source, new, 2, read_at(base, 30));

        let (metadata, _requests) = MetadataService::new();
        let (event_tx, _events) = bounded(8);
        let current = p("/music/Artist/Album/01.flac");
        process_apply_album_art_event(
            ApplyAlbumArtEvent {
                track_path: current.clone(),
                indexed_by_path: HashMap::from([(current, applied)]),
                fingerprints: HashMap::new(),
                error: None,
            },
            &metadata,
            &event_tx,
            &mut state,
        );
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(new));
        // 4. The delayed scan arrives: what it read is older than the apply.
        apply_library_events([library_event(vec![delayed_scan], Vec::new())], &mut state);
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(new));
        assert_eq!(state.metadata.cover_art_stamp, 2);
        // 5. The metadata read after the apply arrives, and agrees.
        process_metadata_event(MetadataEvent::Loaded(reread), &mut state);
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(new));
        assert_eq!(state.metadata.cover_art_stamp, 2);
    }

    #[test]
    fn now_playing_follows_a_cover_that_a_scan_checked_again() {
        let current = p("/music/Artist/Album/01.flac");
        let mut state = BridgeState::default();
        state.playback.current = Some(current.clone());
        state.metadata.source_path = Some(current.to_string_lossy().into_owned());
        state.metadata.cover_art_path = Some("/music/Artist/Album/cover.jpg".to_string());
        state.metadata.cover_art_stamp = 1;
        let row = |cover: &str, stamp| crate::library::LibraryTrack {
            cover_path: cover.to_string(),
            cover_stamp: stamp,
            ..library_track("/music/Artist/Album/01.flac", "Song")
        };

        // The image was replaced at the same name.
        apply_library_events(
            [library_event(
                vec![row("/music/Artist/Album/cover.jpg", 2)],
                Vec::new(),
            )],
            &mut state,
        );
        assert_eq!(
            state.metadata.cover_art_path.as_deref(),
            Some("/music/Artist/Album/cover.jpg")
        );
        assert_eq!(state.metadata.cover_art_stamp, 2);

        // The image was removed, and the track has no picture of its own.
        apply_library_events([library_event(vec![row("", 3)], Vec::new())], &mut state);
        assert_eq!(state.metadata.cover_art_path, None);
        assert_eq!(state.metadata.cover_art_stamp, 3);

        // Metadata of another track is left to its own read.
        state.metadata.source_path = Some("/music/other.flac".to_string());
        apply_library_events(
            [library_event(
                vec![row("/music/Artist/Album/front.jpg", 4)],
                Vec::new(),
            )],
            &mut state,
        );
        assert_eq!(state.metadata.cover_art_path, None);
        assert_eq!(state.metadata.cover_art_stamp, 3);
    }

    #[test]
    fn progress_alone_leaves_the_library_as_it_was() {
        let mut state = BridgeState::default();
        apply_library_events([library_event(filler_library(10), Vec::new())], &mut state);
        let library = Arc::clone(&state.library);

        let applied = apply_library_events(
            [LibraryEvent {
                changes: crate::library::LibraryChanges::default(),
                status: crate::library::LibraryStatus {
                    scan_in_progress: true,
                    ..crate::library::LibraryStatus::default()
                },
                read_after: 0,
            }],
            &mut state,
        );

        assert_eq!(applied.rows_changed, 0);
        assert!(applied.status_changed);
        assert!(Arc::ptr_eq(&library, &state.library));
    }
}
