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
    remap_path, PathRenames, PlaybackErrorOutcome, PlaybackEvent, PlaybackSnapshot, PlaybackState,
    TrackChangeKind,
};
use crate::profile_logging::heartbeat_trace_enabled;

use super::{
    send_error, ApplyAlbumArtEvent, BridgeError, BridgeEvent, BridgeLoopFlags, BridgeState,
    EndedEarlyPlay, ErrorCategory, ErrorSeverity, LastFmPlaybackTracker, PendingWaveformTrack,
    PlayScrobble, ScrobbleIndicator, ScrobbleIndicatorState, ScrobbleIneligibility,
    SettingsSaveTiming, SnapshotUrgency,
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
    event: &ApplyAlbumArtEvent,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    if let Some(error) = event.error.as_ref() {
        let error = BridgeError::error(ErrorCategory::Library, error.clone())
            .about(event.track_path.to_string_lossy());
        send_error(event_tx, error);
        if event.indexed_by_path.is_empty() {
            return SnapshotUrgency::None;
        }
    }

    update_library_cover_paths(state, &event.indexed_by_path);
    update_queue_cover_paths(state, &event.indexed_by_path, &event.fingerprints);

    // Only the playing track's metadata is shown. An apply that finishes
    // after the track changed asks for nothing of the track before, which
    // would replace the playing track's metadata or drop its read.
    let playing = playing_file(state);
    if let Some(indexed) = playing
        .as_ref()
        .and_then(|playing| event.indexed_by_path.get(playing))
    {
        let cover = (indexed.cover_path.clone(), indexed.cover_stamp);
        set_metadata_cover(state, cover.0, cover.1, indexed.cover_read_at);
    }
    if playing.is_some_and(|playing| {
        playing == event.track_path || event.indexed_by_path.contains_key(&playing)
    }) {
        request_playing_metadata(state, metadata);
    }
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
            &event, metadata, event_tx, state,
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
            state.analysis.waveform_ends_early = false;
            state.analysis_track_token = track_token;
            state.analysis.track_token = track_token;
            state.last_track_change.generation = state.last_track_change.generation.wrapping_add(1);
            state.last_track_change.path = Some(path.clone());
            state.last_track_change.track_token = track_token;
            state.play_serial = state.play_serial.wrapping_add(1);
            note_track_change_read(state, &path, track_token);
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
            send_error(event_tx, playback_error(&error));
            SnapshotUrgency::None
        }
    }
}

/// The warning the user is shown for a file that is cut off, about the file:
/// playback goes on to the next track at the cut.
pub(super) fn file_ends_early(path: &std::path::Path, message: String) -> BridgeError {
    BridgeError::warning(ErrorCategory::Playback, message).about(path.to_string_lossy())
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

/// Advances the play serial when playback leaves Stopped with a track: the
/// stopped track plays again from its start, under the same track token.
fn count_play_of_stopped_track(previous: PlaybackState, state: &mut BridgeState) {
    if previous == PlaybackState::Stopped
        && state.playback.state != PlaybackState::Stopped
        && state.playback.current.is_some()
    {
        state.play_serial = state.play_serial.wrapping_add(1);
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
    note_playback_of_playing_file_read(state);
    match (state.playback.current_queue_index, &state.playback.current) {
        (Some(row), _) => state.last_current_row = Some((state.queue_sync.revision(), row)),
        (None, None) => state.last_current_row = None,
        (None, Some(_)) => {}
    }
    count_play_of_stopped_track(previous_playback.state, state);
    if next_state == PlaybackState::Playing && previous_playback.state == PlaybackState::Stopped {
        // Queued files may have changed on disk while nothing played.
        super::sync_queue_details(state);
    }
    if next_state == PlaybackState::Stopped {
        if previous_playback.state != PlaybackState::Stopped {
            // Stop resets the transport to the beginning (the backend
            // always reports position 0 once stopped), and the Qt side
            // re-anchors the centered spectrogram viewport to column 0 on
            // the same transition.  A centered session takes that as a
            // seek to 0: it keeps its columns if the ring still covers the
            // stopped view, as it does for the whole track at full
            // zoom-out, and refills the view from the track start
            // otherwise.  Rolling mode is skipped: its write-order history
            // stays parked while stopped and the next play restarts its
            // session anyway.
            if state.analysis_track_token != 0 {
                if let Some(cmd) =
                    stopped_track_spectrogram_command(state.settings.spectrogram_display_mode)
                {
                    analysis.command(cmd);
                }
            }
        }
        if !state.analysis.waveform_peaks.is_empty() {
            state.analysis.waveform_peaks.clear();
            state.analysis.waveform_coverage_seconds = 0.0;
            state.analysis.waveform_complete = false;
            state.analysis.waveform_ends_early = false;
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
            "[bridge] stopped->playing replay token={} mode={:?}",
            state.analysis_track_token,
            state.settings.spectrogram_display_mode,
        );
        analysis.command(replayed_track_spectrogram_command(
            state.settings.spectrogram_display_mode,
            pos_seconds,
        ));
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

/// Command (if any) that moves the spectrogram to the stopped transport's
/// position 0.  See the call site in `process_playback_snapshot_event` for
/// the full rationale.
fn stopped_track_spectrogram_command(
    display_mode: SpectrogramDisplayMode,
) -> Option<AnalysisCommand> {
    (display_mode == SpectrogramDisplayMode::Centered).then_some(AnalysisCommand::SeekPosition(0.0))
}

/// Command that resumes the spectrogram when the stopped track plays again
/// from `position_seconds`.  A centered view shows the columns around the
/// playhead, which the stop kept or refilled, so it seeks there and keeps
/// them while they cover the view.  A rolling view shows what has played,
/// which starts again: its session restarts and clears the history.
fn replayed_track_spectrogram_command(
    display_mode: SpectrogramDisplayMode,
    position_seconds: f64,
) -> AnalysisCommand {
    match display_mode {
        SpectrogramDisplayMode::Centered => AnalysisCommand::SeekPosition(position_seconds),
        SpectrogramDisplayMode::Rolling => AnalysisCommand::RestartCurrentTrack {
            position_seconds,
            clear_history: true,
        },
    }
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
    // A snapshot of the track before can still be queued when the next one
    // starts, also one of the same file, as for Repeat One: its waveform,
    // coverage and cut are not the current track's.
    if snapshot.track_token != state.analysis_track_token {
        return SnapshotUrgency::None;
    }
    state.analysis.track_token = snapshot.track_token;
    state.analysis.sample_rate_hz = snapshot.sample_rate_hz;
    state.analysis.waveform_coverage_seconds = snapshot.waveform_coverage_seconds;
    state.analysis.waveform_complete = snapshot.waveform_complete;
    state.analysis.waveform_ends_early = snapshot.waveform_ends_early;
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
                send_error(
                    event_tx,
                    BridgeError::error(ErrorCategory::Analysis, message),
                );
            }
            AnalysisEvent::FileEndsEarly { path, message } => {
                send_error(event_tx, file_ends_early(&path, message));
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
            state.metadata = *metadata;
            SnapshotUrgency::Immediate
        }
        MetadataEvent::Superseded(path) => {
            note_dropped_metadata_read(state, &path);
            SnapshotUrgency::None
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
        send_error(event_tx, problem.into_bridge_error());
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
        let ended_early = end_lastfm_play(state, lastfm_handle, tracker, now);
        *tracker = LastFmPlaybackTracker {
            active_path: current_path,
            track_change_generation: track_change.generation,
            duration_seconds: match u32::try_from(state.playback.duration.as_secs()) {
                Ok(value) if value > 0 => Some(value),
                _ => None,
            },
            ended_early,
            ..LastFmPlaybackTracker::default()
        };
    }

    if state.playback.state == PlaybackState::Stopped || tracker.active_path.is_none() {
        let ended_early = end_lastfm_play(state, lastfm_handle, tracker, now);
        *tracker = LastFmPlaybackTracker {
            track_change_generation: tracker.track_change_generation,
            ended_early,
            ..LastFmPlaybackTracker::default()
        };
        return;
    }
    // A play is under way, so the last one is behind it.
    tracker.ended_early = None;

    // Playback may correct the duration during the play. Follow it until the
    // play qualifies, so the threshold matches the track.
    if !tracker.scrobble_queued() {
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
        tracker.metadata_seen = true;
    }

    if state.playback.state == PlaybackState::Playing {
        // Before the play can qualify below: a play that plays while
        // scrobbling is paused is never scrobbled, even after resuming.
        if state.lastfm_scrobbling_paused && tracker.scrobble == PlayScrobble::Counting {
            tracker.scrobble = PlayScrobble::Skipped;
        }
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
        && !state.lastfm_scrobbling_paused
        && tracker.scrobble != PlayScrobble::Skipped
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

/// Ends the play `tracker` follows, as [`finalize_lastfm_track`] does, and
/// returns it if it ended before its scrobble point. Without a play, which
/// a tracker that has been reset has, it keeps the play that ended last.
fn end_lastfm_play(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
    now: Instant,
) -> Option<EndedEarlyPlay> {
    finalize_lastfm_track(state, lastfm_handle, tracker, now);
    if tracker.started_at_utc.is_none() {
        return tracker.ended_early;
    }
    // A play that would never scrobble, or that scrobbling paused, did not
    // miss its scrobble point.
    if !state.lastfm.enabled
        || state.lastfm_scrobbling_paused
        || tracker.scrobble != PlayScrobble::Counting
        || tracker.artist.is_empty()
        || tracker.track.is_empty()
    {
        return None;
    }
    let threshold_seconds = tracker
        .duration_seconds
        .and_then(lastfm::scrobble_threshold_seconds)?;
    Some(EndedEarlyPlay {
        listened_duration: tracker.listened_duration,
        threshold_seconds,
    })
}

fn queue_lastfm_scrobble_if_ready(
    state: &BridgeState,
    lastfm_handle: &LastFmHandle,
    tracker: &mut LastFmPlaybackTracker,
) {
    if !state.lastfm.enabled
        || state.lastfm_scrobbling_paused
        || tracker.scrobble != PlayScrobble::Counting
        || tracker.started_at_utc.is_none()
    {
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
    let ordinal = lastfm_handle.queue_scrobble(LastFmScrobbleEntry {
        artist: tracker.artist.clone(),
        track: tracker.track.clone(),
        album: lastfm::normalize_album_for_submission(&tracker.album),
        track_number: tracker.track_number,
        duration_seconds: tracker.duration_seconds,
        timestamp_utc: tracker.started_at_utc.unwrap_or_else(unix_timestamp_now),
    });
    tracker.scrobble = PlayScrobble::Queued(ordinal);
}

/// What the scrobble progress indicator shows for the play `tracker`
/// follows, from the scrobbler's own count and what Last.fm did with its
/// scrobble.
pub(super) fn scrobble_indicator(
    state: &BridgeState,
    tracker: &LastFmPlaybackTracker,
) -> ScrobbleIndicator {
    // Only a connected account that scrobbles shows the indicator.
    if !state.lastfm.enabled || state.lastfm.auth_state != lastfm::AuthState::Connected {
        return ScrobbleIndicator::default();
    }
    // Once playback stops, it tells only a play that ended before its
    // scrobble point.
    if tracker.active_path.is_none() || state.playback.state == PlaybackState::Stopped {
        return tracker
            .ended_early
            .map_or_else(ScrobbleIndicator::default, |play| ScrobbleIndicator {
                state: ScrobbleIndicatorState::Ineligible,
                ineligibility: ScrobbleIneligibility::EndedEarly,
                listened_ms: u32::try_from(play.listened_duration.as_millis()).unwrap_or(u32::MAX),
                threshold_ms: play.threshold_seconds.saturating_mul(1000),
                message: String::new(),
            });
    }
    let threshold_seconds = tracker
        .duration_seconds
        .and_then(lastfm::scrobble_threshold_seconds);
    let mut indicator = ScrobbleIndicator {
        state: ScrobbleIndicatorState::Listening,
        listened_ms: u32::try_from(tracker.listened_duration.as_millis()).unwrap_or(u32::MAX),
        threshold_ms: threshold_seconds.map_or(0, |seconds| seconds.saturating_mul(1000)),
        ..ScrobbleIndicator::default()
    };
    if let PlayScrobble::Queued(ordinal) = tracker.scrobble {
        let latest = state
            .lastfm
            .latest_scrobble
            .as_ref()
            .filter(|latest| ordinal == Some(latest.ordinal));
        indicator.state = match latest.map(|latest| latest.delivery) {
            None | Some(lastfm::ScrobbleDelivery::Pending) => ScrobbleIndicatorState::Sending,
            Some(lastfm::ScrobbleDelivery::Waiting) => ScrobbleIndicatorState::Waiting,
            Some(lastfm::ScrobbleDelivery::Accepted) => ScrobbleIndicatorState::Accepted,
            Some(lastfm::ScrobbleDelivery::Failed) => ScrobbleIndicatorState::Failed,
        };
        if let Some(latest) = latest {
            indicator.message.clone_from(&latest.message);
        }
    } else if tracker.scrobble == PlayScrobble::Skipped {
        indicator.state = ScrobbleIndicatorState::Skipped;
    } else if tracker.duration_seconds.is_some() && threshold_seconds.is_none() {
        indicator.state = ScrobbleIndicatorState::Ineligible;
        indicator.ineligibility = ScrobbleIneligibility::TooShort;
    } else if tracker.metadata_seen && (tracker.artist.is_empty() || tracker.track.is_empty()) {
        indicator.state = ScrobbleIndicatorState::Ineligible;
        indicator.ineligibility = ScrobbleIneligibility::MissingTags;
    }
    indicator
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
        /// How often this thread copied the bridge's library to apply
        /// changes to it.
        pub(in super::super) static LIBRARY_COPIES: Cell<usize> = const { Cell::new(0) };
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
    note_reported_playing_file(state, &changes.changed_files);
    // Files reported changed with their rows as they were leave the
    // library alone, so it is not copied for them.
    if changes.roots.is_none() && !changes.has_rows() {
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

/// The metadata read the bridge wants for the playing track, from its
/// track change on, also while the playback snapshot still names the track
/// before. The metadata worker reads only the newest of its queued
/// requests and reports each one it drops, so a read of the playing file
/// that it dropped is asked for again; see [`metadata_to_read_again`].
#[derive(Debug, Clone)]
pub(super) struct PlayingFileRead {
    path: PathBuf,
    /// The `track_token` of the play the read is for.
    track_token: u64,
    /// Whether a playback snapshot reported that play. Until one does,
    /// snapshots of the play before still come.
    reported_by_playback: bool,
    read: PlayingFileReadState,
    /// The state a scan last reported the file in, while metadata read in
    /// that state has yet to arrive.
    reported: Option<TrackFileFingerprint>,
}

/// Where the read of a [`PlayingFileRead`] stands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PlayingFileReadState {
    /// Asked for, and since the scan report if there is one, so metadata
    /// read in the reported state or later is on its way or arrived.
    Asked,
    /// Not asked for since a scan reported the file changed. Metadata of
    /// the file may still be on its way, read before or after the change.
    NotAskedSinceReport,
    /// The metadata worker dropped a request for the file for a newer one
    /// for another file, which it reads instead, so the metadata shown
    /// will be another file's.
    Dropped,
}

impl PlayingFileRead {
    /// The state a scan reported the file in, while metadata read in that
    /// state has yet to arrive.
    #[cfg(test)]
    pub(super) fn reported(&self) -> Option<TrackFileFingerprint> {
        self.reported
    }

    /// Whether the read is still the playing track's: playback names its
    /// file, or has yet to report the play the track change started.
    fn is_for(&self, playback: &PlaybackSnapshot) -> bool {
        playback.current.as_ref() == Some(&self.path)
            || (!self.reported_by_playback && playback.track_token < self.track_token)
    }
}

/// A track change starts the read of the new track's metadata. A report
/// about the file that played before is no longer wanted, even while the
/// playback snapshot still names it; one about the same file, played
/// again, still is.
fn note_track_change_read(state: &mut BridgeState, path: &Path, track_token: u64) {
    let reported = state
        .playing_file_read
        .take()
        .filter(|read| read.path == path)
        .and_then(|read| read.reported);
    state.playing_file_read = Some(PlayingFileRead {
        path: path.to_path_buf(),
        track_token,
        reported_by_playback: false,
        read: PlayingFileReadState::Asked,
        reported,
    });
}

fn note_playback_of_playing_file_read(state: &mut BridgeState) {
    if let Some(read) = state.playing_file_read.as_mut() {
        if state.playback.track_token == read.track_token {
            read.reported_by_playback = true;
        }
    }
    drop_stale_playing_file_read(state);
}

/// Forgets the read when its track no longer plays.
fn drop_stale_playing_file_read(state: &mut BridgeState) {
    if state
        .playing_file_read
        .as_ref()
        .is_some_and(|read| !read.is_for(&state.playback))
    {
        state.playing_file_read = None;
    }
}

/// The read of the playing track's metadata. Without a track change that
/// started one, as for the track a session restored, it is the read of the
/// file playback names.
fn playing_file_read(state: &mut BridgeState) -> Option<&mut PlayingFileRead> {
    drop_stale_playing_file_read(state);
    if state.playing_file_read.is_none() {
        state.playing_file_read = Some(PlayingFileRead {
            path: state.playback.current.clone()?,
            track_token: state.playback.track_token,
            reported_by_playback: true,
            read: PlayingFileReadState::Asked,
            reported: None,
        });
    }
    state.playing_file_read.as_mut()
}

/// The playing track's file, whose metadata the bridge shows: the file of
/// the latest track change, also before a playback snapshot names it.
pub(super) fn playing_file(state: &mut BridgeState) -> Option<PathBuf> {
    playing_file_read(state).map(|read| read.path.clone())
}

/// Asks for the playing track's metadata, noting the read as asked for,
/// so that one the metadata worker drops is asked for again.
pub(super) fn request_playing_metadata(state: &mut BridgeState, metadata: &MetadataService) {
    if let Some(read) = playing_file_read(state) {
        read.read = PlayingFileReadState::Asked;
        metadata.request(read.path.clone());
    }
}

/// A rename keeps the playing track's read.
pub(super) fn remap_playing_file_read(state: &mut BridgeState, rename_map: &PathRenames) {
    if let Some(read) = state.playing_file_read.as_mut() {
        remap_path(&mut read.path, rename_map);
    }
}

/// Keeps the latest state that `changed`, files a scan found changed on
/// disk and read again, has the playing file in. Its row may be as it was,
/// since a row holds only some of the tags, so only the file's state tells
/// whether the metadata shown is out of date.
fn note_reported_playing_file(
    state: &mut BridgeState,
    changed: &[(PathBuf, TrackFileFingerprint)],
) {
    if changed.is_empty() {
        return;
    }
    let Some(read) = playing_file_read(state) else {
        return;
    };
    let Some((_, reported)) = changed.iter().rev().find(|(path, _)| *path == read.path) else {
        return;
    };
    if read.reported == Some(*reported) {
        return;
    }
    read.reported = Some(*reported);
    // A dropped read still has to be asked for, whatever is shown.
    if read.read == PlayingFileReadState::Asked {
        read.read = PlayingFileReadState::NotAskedSinceReport;
    }
}

/// The metadata worker dropped a request for `path` unread, and reads one
/// for another file instead. When `path` is the playing file, nothing now
/// brings its metadata, so [`metadata_to_read_again`] asks for it again.
/// Dropped reads of other files are not asked for again, so that two files
/// never ask for each other back and forth.
fn note_dropped_metadata_read(state: &mut BridgeState, path: &Path) {
    if let Some(read) = playing_file_read(state).filter(|read| read.path == path) {
        read.read = PlayingFileReadState::Dropped;
    }
}

/// The playing file, when its metadata is to be read again: the metadata
/// worker dropped its read, or the metadata shown was read in another
/// state than a scan last reported the file in. Asks once, since the read
/// asked for comes after any read still on its way, and forgets the report
/// once metadata read in its state arrives. Metadata still to arrive for
/// the track waits until it does, as it may have been read before the
/// change. A dropped read is asked for again, whatever is shown, since no
/// metadata of the file is then on its way.
pub(super) fn metadata_to_read_again(state: &mut BridgeState) -> Option<PathBuf> {
    drop_stale_playing_file_read(state);
    let read = state.playing_file_read.as_mut()?;
    let shown = &state.metadata;
    let shows_file = shown.source_path.as_deref().map(Path::new) == Some(read.path.as_path());
    if shows_file && read.reported.is_some() && shown.file_fingerprint == read.reported {
        read.reported = None;
    }
    match read.read {
        PlayingFileReadState::Asked => return None,
        PlayingFileReadState::NotAskedSinceReport if read.reported.is_none() || !shows_file => {
            return None;
        }
        PlayingFileReadState::NotAskedSinceReport | PlayingFileReadState::Dropped => {}
    }
    read.read = PlayingFileReadState::Asked;
    Some(read.path.clone())
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
            fft_size: 8192,
            coverage_seconds: 1.0,
            complete: true,
            buffer_reset: false,
            clear_history: false,
            widget_width: 0,
            track_frames: 0,
            end_column: 0,
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
    fn a_queued_snapshot_of_the_track_before_leaves_the_next_track_alone() {
        // A manual change, a gapless one, and Repeat One, where the next
        // track is the same file: each is a new track with a new token.
        for (kind, next) in [
            (TrackChangeKind::Manual, "/music/next.flac"),
            (TrackChangeKind::Gapless, "/music/next.flac"),
            (TrackChangeKind::Gapless, "/music/track.flac"),
            (TrackChangeKind::Natural, "/music/track.flac"),
        ] {
            let (analysis, _commands, playback_tx, playback_rx, metadata, mut state) =
                playback_harness(SpectrogramDisplayMode::Centered);
            let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded::<AnalysisEvent>();
            let (event_tx, _event_rx) = crossbeam_channel::unbounded::<BridgeEvent>();
            let cut = |token: u64, seconds: f32| {
                AnalysisEvent::Snapshot(AnalysisSnapshot {
                    waveform_peaks: vec![0.5; 8],
                    waveform_coverage_seconds: seconds,
                    waveform_ends_early: true,
                    sample_rate_hz: 44_100,
                    track_token: token,
                    ..AnalysisSnapshot::default()
                })
            };
            // The playing track's (token 7) last overview update, cut off,
            // waits in the queue while playback moves on.
            analysis_tx.send(cut(7, 242.0)).expect("queue the snapshot");
            playback_tx
                .send(PlaybackEvent::TrackChanged {
                    path: p(next),
                    queue_index: 1,
                    kind: kind.clone(),
                    track_token: 8,
                })
                .expect("send the track change");
            pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
            assert_eq!(state.analysis.track_token, 8);
            drain_analysis_events(&analysis_rx, &event_tx, &mut state);
            assert!(!state.analysis.waveform_ends_early, "{kind:?} {next}");
            assert!(state.analysis.waveform_peaks.is_empty(), "{kind:?} {next}");
            assert!(state.analysis.waveform_coverage_seconds.abs() < f32::EPSILON);
            // The next track's own snapshot applies.
            analysis_tx.send(cut(8, 100.0)).expect("queue the snapshot");
            drain_analysis_events(&analysis_rx, &event_tx, &mut state);
            assert!(state.analysis.waveform_ends_early, "{kind:?} {next}");
            assert!((state.analysis.waveform_coverage_seconds - 100.0).abs() < f32::EPSILON);
        }
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
        Receiver<AnalysisCommand>,
        crossbeam_channel::Sender<PlaybackEvent>,
        Receiver<PlaybackEvent>,
        MetadataService,
        BridgeState,
    ) {
        let (command_tx, command_rx) = crossbeam_channel::unbounded();
        let analysis = AnalysisEngine::from_command_sender(command_tx);
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();
        let mut state = BridgeState::default();
        state.settings.spectrogram_display_mode = display_mode;
        state.playback.current = Some(p("/music/track.flac"));
        state.analysis_track_token = 7;
        state.playback.state = PlaybackState::Playing;
        (
            analysis,
            command_rx,
            playback_tx,
            playback_rx,
            metadata,
            state,
        )
    }

    /// Send `snapshot` through the bridge and return the analysis commands
    /// it sent.
    fn snapshot_commands(
        snapshot: PlaybackSnapshot,
        harness: (
            &AnalysisEngine,
            &Receiver<AnalysisCommand>,
            &crossbeam_channel::Sender<PlaybackEvent>,
            &Receiver<PlaybackEvent>,
            &MetadataService,
        ),
        state: &mut BridgeState,
    ) -> Vec<AnalysisCommand> {
        let (analysis, command_rx, playback_tx, playback_rx, metadata) = harness;
        playback_tx
            .send(PlaybackEvent::Snapshot(snapshot))
            .expect("send snapshot");
        pump_playback_events(playback_rx, analysis, metadata, state);
        command_rx.try_iter().collect()
    }

    fn is_seek_to(command: &AnalysisCommand, seconds: f64) -> bool {
        matches!(command, AnalysisCommand::SeekPosition(position) if (position - seconds).abs() < 1e-9)
    }

    #[test]
    fn stopped_transition_seeks_a_centered_spectrogram_to_the_start() {
        let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);
        let harness = (
            &analysis,
            &command_rx,
            &playback_tx,
            &playback_rx,
            &metadata,
        );

        // Heartbeat while playing only moves the playhead.
        let mut playing_snapshot = state.playback.clone();
        playing_snapshot.position = Duration::from_secs(170);
        let commands = snapshot_commands(playing_snapshot, harness, &mut state);
        assert!(
            matches!(commands.as_slice(), [AnalysisCommand::PositionUpdate(_)]),
            "{commands:?}"
        );

        // Stop transition: the re-anchored viewport shows the start, which
        // the session takes as a seek there. It keeps its columns if they
        // cover that view, so nothing restarts the session or clears the
        // ring here.
        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        let commands = snapshot_commands(stopped_snapshot, harness, &mut state);
        assert!(
            matches!(commands.as_slice(), [command] if is_seek_to(command, 0.0)),
            "{commands:?}"
        );
    }

    #[test]
    fn stopped_transition_keeps_rolling_spectrogram_parked() {
        let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Rolling);
        let harness = (
            &analysis,
            &command_rx,
            &playback_tx,
            &playback_rx,
            &metadata,
        );

        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        let commands = snapshot_commands(stopped_snapshot, harness, &mut state);

        // Rolling rings are write-order history; the stop transition must
        // not restart (and thereby clear) the rolling session.
        assert!(commands.is_empty(), "{commands:?}");
    }

    #[test]
    fn heartbeat_while_stopped_does_not_move_the_spectrogram() {
        let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);
        let harness = (
            &analysis,
            &command_rx,
            &playback_tx,
            &playback_rx,
            &metadata,
        );

        let mut stopped_snapshot = state.playback.clone();
        stopped_snapshot.state = PlaybackState::Stopped;
        stopped_snapshot.position = Duration::ZERO;
        let commands = snapshot_commands(stopped_snapshot.clone(), harness, &mut state);
        assert!(
            matches!(commands.as_slice(), [command] if is_seek_to(command, 0.0)),
            "the stop transition seeks to the start: {commands:?}"
        );

        // Stopped heartbeats repeat every ~100 ms; only the transition
        // moves the spectrogram.
        let commands = snapshot_commands(stopped_snapshot, harness, &mut state);
        assert!(commands.is_empty(), "{commands:?}");
    }

    #[test]
    fn playing_the_stopped_track_again_seeks_a_centered_spectrogram_and_restarts_a_rolling_one() {
        for mode in [
            SpectrogramDisplayMode::Centered,
            SpectrogramDisplayMode::Rolling,
        ] {
            let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
                playback_harness(mode);
            let harness = (
                &analysis,
                &command_rx,
                &playback_tx,
                &playback_rx,
                &metadata,
            );
            state.playback.state = PlaybackState::Stopped;

            let mut playing_snapshot = state.playback.clone();
            playing_snapshot.state = PlaybackState::Playing;
            playing_snapshot.position = Duration::from_millis(40);
            let commands = snapshot_commands(playing_snapshot, harness, &mut state);
            match mode {
                // The stop kept, or refilled, the columns of the view at the
                // start: the session keeps them while they cover the view.
                SpectrogramDisplayMode::Centered => assert!(
                    matches!(commands.as_slice(), [command] if is_seek_to(command, 0.04)),
                    "{mode:?}: {commands:?}"
                ),
                // What has played starts again.
                SpectrogramDisplayMode::Rolling => assert!(
                    matches!(
                        commands.as_slice(),
                        [AnalysisCommand::RestartCurrentTrack {
                            position_seconds,
                            clear_history: true,
                        }] if (position_seconds - 0.04).abs() < 1e-9
                    ),
                    "{mode:?}: {commands:?}"
                ),
            }
        }
    }

    #[test]
    fn playing_another_track_after_a_stop_starts_its_own_session() {
        let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);
        let harness = (
            &analysis,
            &command_rx,
            &playback_tx,
            &playback_rx,
            &metadata,
        );
        state.playback.state = PlaybackState::Stopped;

        // The track changes while stopped, and then plays.
        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: p("/music/other.flac"),
                queue_index: 1,
                kind: TrackChangeKind::Manual,
                track_token: 8,
            })
            .expect("send track change");
        let mut playing_snapshot = state.playback.clone();
        playing_snapshot.state = PlaybackState::Playing;
        playing_snapshot.current = Some(p("/music/other.flac"));
        let commands = snapshot_commands(playing_snapshot, harness, &mut state);
        // The change is kept with its token, by which the scrobbler tells
        // this play's snapshots from the previous play's.
        assert_eq!(state.last_track_change.track_token, 8);
        assert_eq!(
            state.last_track_change.path.as_deref(),
            Some(Path::new("/music/other.flac"))
        );
        assert!(
            matches!(
                commands.as_slice(),
                [
                    AnalysisCommand::SetTrack {
                        track_token: 8,
                        reset_spectrogram: true,
                        ..
                    },
                    AnalysisCommand::PositionUpdate(_)
                ]
            ),
            "{commands:?}"
        );
    }

    #[test]
    fn play_serial_advances_for_each_play_of_the_same_track() {
        let (analysis, command_rx, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Centered);
        let harness = (
            &analysis,
            &command_rx,
            &playback_tx,
            &playback_rx,
            &metadata,
        );
        let track = p("/music/track.flac");
        let replay = |token: u64, kind: TrackChangeKind, state: &mut BridgeState| {
            playback_tx
                .send(PlaybackEvent::TrackChanged {
                    path: track.clone(),
                    queue_index: 0,
                    kind,
                    track_token: token,
                })
                .expect("send track change");
            pump_playback_events(&playback_rx, &analysis, &metadata, state);
        };
        let mut playing = state.playback.clone();
        playing.track_token = 7;
        let _ = snapshot_commands(playing.clone(), harness, &mut state);
        let mut serial = state.play_serial;

        // Seeking, pausing and resuming keep the play.
        let mut sought = playing.clone();
        sought.position = Duration::from_secs(90);
        sought.seek_serial = 1;
        sought.seek_landed_serial = 1;
        let _ = snapshot_commands(sought.clone(), harness, &mut state);
        let mut paused = sought.clone();
        paused.state = PlaybackState::Paused;
        let _ = snapshot_commands(paused, harness, &mut state);
        let _ = snapshot_commands(sought.clone(), harness, &mut state);
        assert_eq!(state.play_serial, serial);

        // Repeat One, or the same file next in the queue: a gapless replay.
        replay(8, TrackChangeKind::Gapless, &mut state);
        assert_ne!(state.play_serial, serial);
        serial = state.play_serial;
        let mut replayed = sought.clone();
        replayed.track_token = 8;
        replayed.position = Duration::ZERO;
        let _ = snapshot_commands(replayed.clone(), harness, &mut state);
        assert_eq!(state.play_serial, serial);

        // Played again by hand.
        replay(9, TrackChangeKind::Manual, &mut state);
        assert_ne!(state.play_serial, serial);
        serial = state.play_serial;
        replayed.track_token = 9;
        let _ = snapshot_commands(replayed.clone(), harness, &mut state);

        // Stopped, and played again under the same token: stopping ends the
        // play, playing starts the next.
        let mut stopped = replayed.clone();
        stopped.state = PlaybackState::Stopped;
        stopped.current_queue_index = None;
        let _ = snapshot_commands(stopped, harness, &mut state);
        assert_eq!(state.play_serial, serial);
        let _ = snapshot_commands(replayed, harness, &mut state);
        assert_ne!(state.play_serial, serial);
        assert_eq!(state.snapshot(false).play_serial, state.play_serial);
    }

    /// A playing 240 s track (scrobble threshold 120 s) with its metadata.
    fn lastfm_playing_state(track_path: &Path) -> BridgeState {
        let mut state = BridgeState::default();
        state.lastfm.enabled = true;
        state.lastfm.auth_state = lastfm::AuthState::Connected;
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
        assert!(!tracker.scrobble_queued());
        assert!(queued_scrobbles(&commands.try_iter().collect::<Vec<_>>()).is_empty());

        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(120),
            1_700_000_120,
        );
        assert!(tracker.scrobble_queued());
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

        assert!(!tracker.scrobble_queued());
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
                changed_files: Vec::new(),
            },
            ..LibraryEvent::default()
        }
    }

    fn file_state(mtime_ns: i64) -> TrackFileFingerprint {
        TrackFileFingerprint {
            mtime_ns,
            size_bytes: 100,
        }
    }

    fn scanned(files: Vec<(PathBuf, TrackFileFingerprint)>) -> LibraryEvent {
        let mut event = library_event(Vec::new(), Vec::new());
        event.changes.changed_files = files;
        event
    }

    fn loaded(path: &Path, read_in: TrackFileFingerprint) -> MetadataEvent {
        MetadataEvent::Loaded(Box::new(crate::metadata::TrackMetadata {
            source_path: Some(path.to_string_lossy().into_owned()),
            file_fingerprint: Some(read_in),
            ..crate::metadata::TrackMetadata::default()
        }))
    }

    fn reported(state: &BridgeState) -> Option<TrackFileFingerprint> {
        state
            .playing_file_read
            .as_ref()
            .and_then(PlayingFileRead::reported)
    }

    #[test]
    fn a_scan_that_read_the_playing_file_in_another_state_asks_for_its_metadata() {
        let playing = p("/music/A/Album/02.flac");
        let (read_state, changed_state) = (file_state(1), file_state(2));
        let mut state = BridgeState::default();
        state.playback.current = Some(playing.clone());
        process_metadata_event(loaded(&playing, read_state), &mut state);
        let mut apply = |events: Vec<LibraryEvent>| {
            apply_library_events(events, &mut state);
            metadata_to_read_again(&mut state)
        };

        // The state its metadata was read in, another file, or no file:
        // nothing to read.
        assert_eq!(
            apply(vec![scanned(vec![(playing.clone(), read_state)])]),
            None
        );
        assert_eq!(
            apply(vec![scanned(vec![(
                p("/music/A/Album/03.flac"),
                changed_state
            )])]),
            None
        );
        assert_eq!(apply(vec![library_event(Vec::new(), Vec::new())]), None);
        // The latest state a batch of events read counts.
        assert_eq!(
            apply(vec![
                scanned(vec![(playing.clone(), changed_state)]),
                scanned(vec![(playing.clone(), read_state)]),
            ]),
            None
        );
        // Another state of the playing file: read it again, once.
        assert_eq!(
            apply(vec![scanned(vec![(playing.clone(), changed_state)])]),
            Some(playing.clone())
        );
        assert_eq!(
            apply(vec![scanned(vec![(playing.clone(), changed_state)])]),
            None
        );
        // Metadata read in that state settles it.
        process_metadata_event(loaded(&playing, changed_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        assert_eq!(reported(&state), None);
    }

    #[test]
    fn a_scan_report_waits_for_metadata_on_its_way_and_rereads_if_it_is_older() {
        let playing = p("/music/A/Album/02.flac");
        let (read_state, changed_state) = (file_state(1), file_state(2));
        let mut state = BridgeState::default();
        state.playback.current = Some(playing.clone());
        state.metadata.source_path = Some("/music/A/Album/01.flac".to_string());

        // The report comes while the track's first metadata is on its way.
        apply_library_events(
            vec![scanned(vec![(playing.clone(), changed_state)])],
            &mut state,
        );
        assert_eq!(metadata_to_read_again(&mut state), None);
        // That metadata was read before the change: read it again, once,
        // whichever of its stages arrive.
        process_metadata_event(loaded(&playing, read_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));
        process_metadata_event(loaded(&playing, read_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(loaded(&playing, changed_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        assert_eq!(reported(&state), None);

        // Metadata on its way that was read after the change needs no read.
        state.metadata.source_path = Some("/music/A/Album/01.flac".to_string());
        apply_library_events(
            vec![scanned(vec![(playing.clone(), file_state(3))])],
            &mut state,
        );
        process_metadata_event(loaded(&playing, file_state(3)), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);

        // A report about a track no longer playing is dropped.
        apply_library_events(
            vec![scanned(vec![(playing.clone(), file_state(4))])],
            &mut state,
        );
        state.playback.current = Some(p("/music/A/Album/03.flac"));
        assert_eq!(metadata_to_read_again(&mut state), None);
        assert_eq!(reported(&state), None);
    }

    #[test]
    fn a_dropped_read_of_the_reported_playing_file_is_asked_for_again() {
        let playing = p("/music/A/Album/02.flac");
        let previous = p("/music/A/Album/01.flac");
        let (read_state, changed_state) = (file_state(1), file_state(2));
        let mut state = BridgeState::default();
        state.playback.current = Some(playing.clone());
        process_metadata_event(loaded(&playing, read_state), &mut state);
        apply_library_events(
            vec![scanned(vec![(playing.clone(), changed_state)])],
            &mut state,
        );
        assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));

        // A request for the track before comes while the worker is busy,
        // and the worker reads that one instead.
        process_metadata_event(MetadataEvent::Superseded(playing.clone()), &mut state);
        process_metadata_event(loaded(&previous, read_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));
        // Asked once more, until that read is dropped too.
        process_metadata_event(loaded(&previous, read_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(MetadataEvent::Superseded(previous.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(MetadataEvent::Superseded(playing.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));
        // Metadata read in the reported state settles it.
        process_metadata_event(loaded(&playing, changed_state), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        assert_eq!(reported(&state), None);

        // With nothing reported, a dropped read of the playing file is
        // asked for again too, and one of another file is not.
        process_metadata_event(MetadataEvent::Superseded(previous.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(MetadataEvent::Superseded(playing.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));
        assert_eq!(metadata_to_read_again(&mut state), None);
    }

    /// Sends a track change to `path` through the bridge.
    fn change_track(
        harness: &(
            AnalysisEngine,
            Receiver<AnalysisCommand>,
            crossbeam_channel::Sender<PlaybackEvent>,
            Receiver<PlaybackEvent>,
            MetadataService,
        ),
        state: &mut BridgeState,
        path: &Path,
        kind: TrackChangeKind,
        track_token: u64,
    ) {
        let (analysis, _commands, playback_tx, playback_rx, metadata) = harness;
        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: path.to_path_buf(),
                queue_index: 1,
                kind,
                track_token,
            })
            .expect("send the track change");
        pump_playback_events(playback_rx, analysis, metadata, state);
    }

    #[test]
    fn a_dropped_read_of_the_track_a_change_started_is_asked_for_again() {
        // A manual change, a gapless one, and Repeat One, where the next
        // track is the same file.
        for (kind, next) in [
            (TrackChangeKind::Manual, "/music/next.flac"),
            (TrackChangeKind::Gapless, "/music/next.flac"),
            (TrackChangeKind::Gapless, "/music/track.flac"),
        ] {
            let (analysis, commands, playback_tx, playback_rx, metadata, mut state) =
                playback_harness(SpectrogramDisplayMode::Rolling);
            let harness = (analysis, commands, playback_tx, playback_rx, metadata);
            let (previous, next) = (p("/music/track.flac"), p(next));
            state.playback.track_token = 7;
            process_metadata_event(loaded(&previous, file_state(1)), &mut state);
            change_track(&harness, &mut state, &next, kind.clone(), 8);
            assert_eq!(metadata_to_read_again(&mut state), None, "{kind:?}");

            // The playback snapshot still names the track before when the
            // worker drops the change's read for a late one of another file.
            assert_eq!(state.playback.current, Some(previous.clone()));
            process_metadata_event(MetadataEvent::Superseded(next.clone()), &mut state);
            process_metadata_event(loaded(&p("/music/other.flac"), file_state(1)), &mut state);
            assert_eq!(
                metadata_to_read_again(&mut state),
                Some(next.clone()),
                "{kind:?}"
            );
            assert_eq!(metadata_to_read_again(&mut state), None, "{kind:?}");

            // Once the snapshot names the track, still so.
            let snapshot = PlaybackSnapshot {
                current: Some(next.clone()),
                track_token: 8,
                state: PlaybackState::Playing,
                ..PlaybackSnapshot::default()
            };
            harness
                .2
                .send(PlaybackEvent::Snapshot(snapshot))
                .expect("send the snapshot");
            pump_playback_events(&harness.3, &harness.0, &harness.4, &mut state);
            process_metadata_event(MetadataEvent::Superseded(next.clone()), &mut state);
            assert_eq!(
                metadata_to_read_again(&mut state),
                Some(next.clone()),
                "{kind:?}"
            );

            // Stopped with nothing left to play, the track's read is over.
            harness
                .2
                .send(PlaybackEvent::Snapshot(PlaybackSnapshot::default()))
                .expect("send the snapshot");
            pump_playback_events(&harness.3, &harness.0, &harness.4, &mut state);
            process_metadata_event(MetadataEvent::Superseded(next.clone()), &mut state);
            assert_eq!(metadata_to_read_again(&mut state), None, "{kind:?}");
            assert!(state.playing_file_read.is_none(), "{kind:?}");
        }
    }

    #[test]
    fn the_first_track_from_an_empty_queue_has_its_dropped_read_asked_for_again() {
        let (analysis, commands, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Rolling);
        let harness = (analysis, commands, playback_tx, playback_rx, metadata);
        state.playback = PlaybackSnapshot::default();
        let first = p("/music/first.flac");
        change_track(&harness, &mut state, &first, TrackChangeKind::Manual, 1);
        // The snapshot names no track yet.
        process_metadata_event(MetadataEvent::Superseded(first.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(first));
    }

    #[test]
    fn a_rename_keeps_the_playing_tracks_read() {
        let (analysis, commands, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Rolling);
        let harness = (analysis, commands, playback_tx, playback_rx, metadata);
        state.playback.track_token = 7;
        let (next, renamed) = (p("/music/next.flac"), p("/music/01 next.flac"));
        change_track(&harness, &mut state, &next, TrackChangeKind::Manual, 8);
        // Renamed before a playback snapshot names it.
        remap_playing_file_read(
            &mut state,
            &PathRenames::from([(next.clone(), renamed.clone())]),
        );
        assert_eq!(playing_file(&mut state), Some(renamed.clone()));
        process_metadata_event(MetadataEvent::Superseded(next), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(MetadataEvent::Superseded(renamed.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(renamed));
    }

    #[test]
    fn only_the_latest_track_changes_dropped_read_is_asked_for_again() {
        let (analysis, commands, playback_tx, playback_rx, metadata, mut state) =
            playback_harness(SpectrogramDisplayMode::Rolling);
        let harness = (analysis, commands, playback_tx, playback_rx, metadata);
        state.playback.track_token = 7;
        let (b, c) = (p("/music/b.flac"), p("/music/c.flac"));
        // A to B to C before any snapshot names B or C, and the worker reads
        // C's request instead of B's.
        change_track(&harness, &mut state, &b, TrackChangeKind::Manual, 8);
        change_track(&harness, &mut state, &c, TrackChangeKind::Manual, 9);
        process_metadata_event(MetadataEvent::Superseded(b.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(loaded(&c, file_state(1)), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        // B's dropped read stays dropped; only C's is asked for again.
        process_metadata_event(MetadataEvent::Superseded(b), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), None);
        process_metadata_event(MetadataEvent::Superseded(c.clone()), &mut state);
        assert_eq!(metadata_to_read_again(&mut state), Some(c));
    }

    #[test]
    fn a_dropped_read_of_a_reported_file_no_longer_playing_is_not_asked_for() {
        // A manual change, a gapless one, and Repeat One, where the next
        // track is the same file and the report still holds.
        for (kind, next, keeps_report) in [
            (TrackChangeKind::Manual, "/music/next.flac", false),
            (TrackChangeKind::Gapless, "/music/next.flac", false),
            (TrackChangeKind::Gapless, "/music/track.flac", true),
        ] {
            let (analysis, _commands, playback_tx, playback_rx, metadata, mut state) =
                playback_harness(SpectrogramDisplayMode::Rolling);
            let playing = p("/music/track.flac");
            process_metadata_event(loaded(&playing, file_state(1)), &mut state);
            apply_library_events(
                vec![scanned(vec![(playing.clone(), file_state(2))])],
                &mut state,
            );
            assert_eq!(metadata_to_read_again(&mut state), Some(playing.clone()));

            // The track changes, and the change's request drops the read
            // before the playback snapshot names the next track.
            playback_tx
                .send(PlaybackEvent::TrackChanged {
                    path: p(next),
                    queue_index: 1,
                    kind: kind.clone(),
                    track_token: 8,
                })
                .expect("send the track change");
            pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
            assert_eq!(state.playback.current, Some(playing.clone()));
            process_metadata_event(MetadataEvent::Superseded(playing.clone()), &mut state);
            let expected = keeps_report.then(|| playing.clone());
            assert_eq!(metadata_to_read_again(&mut state), expected, "{kind:?}");
        }
    }

    #[test]
    fn files_reported_changed_without_row_changes_do_not_copy_the_library() {
        let mut state = BridgeState {
            library: Arc::new(LibrarySnapshot {
                tracks: crate::library::shared_tracks(vec![
                    library_track("/music/a.flac", "A"),
                    library_track("/music/b.flac", "B"),
                ]),
                ..LibrarySnapshot::default()
            }),
            ..BridgeState::default()
        };
        let copies = || test_hooks::LIBRARY_COPIES.with(std::cell::Cell::get);
        let before = copies();
        let library = Arc::clone(&state.library);
        let applied = apply_library_events(
            vec![scanned(vec![(p("/music/a.flac"), file_state(2))])],
            &mut state,
        );
        assert_eq!(applied.rows_changed, 0);
        assert_eq!(copies(), before);
        assert!(Arc::ptr_eq(&library, &state.library));
        // A row change still copies it.
        apply_library_events(
            vec![library_event(
                vec![library_track("/music/c.flac", "C")],
                Vec::new(),
            )],
            &mut state,
        );
        assert_eq!(copies(), before + 1);
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
        process_metadata_event(MetadataEvent::Loaded(Box::new(read.clone())), &mut state);
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
        process_metadata_event(MetadataEvent::Loaded(Box::new(probed)), &mut state);
        assert_eq!(state.metadata.cover_art_path.as_deref(), Some(cover));
        assert_eq!(state.metadata.cover_art_stamp, 2);
        assert_eq!(state.metadata.bitrate_kbps, Some(905));

        // A read made since then is newer than the scan's cover.
        let front = "/music/Artist/Album/front.jpg";
        let reread = covered_metadata(&source, front, 3, read_at(base, 20));
        process_metadata_event(MetadataEvent::Loaded(Box::new(reread)), &mut state);
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
            MetadataEvent::Loaded(Box::new(covered_metadata(
                &source,
                old,
                1,
                read_at(base, 0),
            ))),
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
            &ApplyAlbumArtEvent {
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
        process_metadata_event(MetadataEvent::Loaded(Box::new(reread)), &mut state);
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

    /// Ticks the tracker `seconds` after `start`, and returns what the
    /// scrobble indicator shows then.
    fn tick_at(
        state: &BridgeState,
        handle: &LastFmHandle,
        tracker: &mut LastFmPlaybackTracker,
        start: Instant,
        seconds: u64,
    ) -> ScrobbleIndicator {
        tick_lastfm_playback_at(
            state,
            handle,
            tracker,
            start + Duration::from_secs(seconds),
            1_700_000_000 + i64::try_from(seconds).unwrap_or(0),
        );
        scrobble_indicator(state, tracker)
    }

    fn sent(commands: &Receiver<LastFmCommand>) -> Vec<LastFmCommand> {
        commands.try_iter().collect()
    }

    #[test]
    fn scrobble_indicator_follows_listened_time_through_seeks_and_pauses() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/indicator.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();

        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (0, 120_000));
        let shown = tick_at(&state, &handle, &mut tracker, start, 30);
        assert_eq!(shown.listened_ms, 30_000);

        // A seek either way moves the playhead, not the count.
        state.playback.position = Duration::from_secs(100);
        let shown = tick_at(&state, &handle, &mut tracker, start, 31);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (31_000, 120_000));
        state.playback.position = Duration::from_secs(10);
        let shown = tick_at(&state, &handle, &mut tracker, start, 31);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (31_000, 120_000));

        // A pause holds it.
        state.playback.state = PlaybackState::Paused;
        let shown = tick_at(&state, &handle, &mut tracker, start, 90);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.listened_ms, 31_000);
        state.playback.state = PlaybackState::Playing;
        let shown = tick_at(&state, &handle, &mut tracker, start, 100);
        assert_eq!(shown.listened_ms, 31_000);
        let shown = tick_at(&state, &handle, &mut tracker, start, 188);
        assert_eq!(shown.listened_ms, 119_000);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());

        // At the scrobble point it is sent, and waits for Last.fm's answer.
        let shown = tick_at(&state, &handle, &mut tracker, start, 189);
        assert_eq!(queued_scrobbles(&sent(&commands)).len(), 1);
        assert_eq!(shown.state, ScrobbleIndicatorState::Sending);
        assert_eq!(tracker.scrobble, PlayScrobble::Queued(Some(1)));

        state.lastfm.latest_scrobble = Some(lastfm::LatestScrobble {
            ordinal: 1,
            delivery: lastfm::ScrobbleDelivery::Accepted,
            message: String::new(),
        });
        let shown = tick_at(&state, &handle, &mut tracker, start, 190);
        assert_eq!(shown.state, ScrobbleIndicatorState::Accepted);

        // Stopping clears it.
        state.playback.state = PlaybackState::Stopped;
        assert_eq!(
            tick_at(&state, &handle, &mut tracker, start, 191),
            ScrobbleIndicator::default()
        );
    }

    #[test]
    fn a_seek_forward_near_the_end_keeps_the_countdown_and_the_play_scrobbles() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/seek-near-end.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        state.playback.position = Duration::from_secs(30);
        let shown = tick_at(&state, &handle, &mut tracker, start, 30);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);

        // At 3:20 of 4:00, 40 s are left for the 90 s still needed. The
        // countdown goes on: only the listened time decides.
        state.playback.position = Duration::from_secs(200);
        for (seconds, position) in [(30, 200), (31, 201), (60, 230), (69, 239)] {
            state.playback.position = Duration::from_secs(position);
            let shown = tick_at(&state, &handle, &mut tracker, start, seconds);
            assert_eq!(shown.state, ScrobbleIndicatorState::Listening, "{seconds}");
            assert_eq!(
                shown.ineligibility,
                ScrobbleIneligibility::None,
                "{seconds}"
            );
            assert_eq!(
                (shown.listened_ms, shown.threshold_ms),
                (u32::try_from(seconds).unwrap() * 1000, 120_000),
                "{seconds}"
            );
        }

        // A seek back, and the listening on from there reaches the
        // scrobble point.
        state.playback.position = Duration::from_secs(60);
        let shown = tick_at(&state, &handle, &mut tracker, start, 70);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());
        state.playback.position = Duration::from_secs(110);
        let shown = tick_at(&state, &handle, &mut tracker, start, 120);
        assert_eq!(shown.state, ScrobbleIndicatorState::Sending);
        assert_eq!(queued_scrobbles(&sent(&commands)).len(), 1);
    }

    #[test]
    fn a_play_that_ends_before_its_scrobble_point_says_so_once_it_has_ended() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/ends-early.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        // A seek forward near the end, and the track plays out.
        state.playback.position = Duration::from_secs(200);
        let shown = tick_at(&state, &handle, &mut tracker, start, 30);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        state.playback.position = Duration::from_secs(240);
        let shown = tick_at(&state, &handle, &mut tracker, start, 70);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);

        // The end of the queue stops playback on the track; the listening
        // up to then counts.
        state.playback.state = PlaybackState::Stopped;
        state.playback.position = Duration::ZERO;
        let ended = ScrobbleIndicator {
            state: ScrobbleIndicatorState::Ineligible,
            ineligibility: ScrobbleIneligibility::EndedEarly,
            listened_ms: 71_000,
            threshold_ms: 120_000,
            message: String::new(),
        };
        for seconds in [71, 72, 300] {
            assert_eq!(
                tick_at(&state, &handle, &mut tracker, start, seconds),
                ended
            );
        }
        // Clearing the queue leaves it as it was.
        state.playback.current = None;
        assert_eq!(tick_at(&state, &handle, &mut tracker, start, 301), ended);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());

        // Playing again starts over.
        state.playback.current = Some(track_path.clone());
        state.playback.state = PlaybackState::Playing;
        let shown = tick_at(&state, &handle, &mut tracker, start, 302);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::None);
        assert_eq!(shown.listened_ms, 0);

        // A stop ends a play too.
        tick_at(&state, &handle, &mut tracker, start, 312);
        state.playback.state = PlaybackState::Stopped;
        let shown = tick_at(&state, &handle, &mut tracker, start, 313);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::EndedEarly);
        assert_eq!(shown.listened_ms, 11_000);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());
    }

    #[test]
    fn only_a_play_that_missed_its_scrobble_point_says_so_once_it_has_ended() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/ended.flac");
        let start = Instant::now();
        let ended_after =
            |state: &mut BridgeState, tracker: &mut LastFmPlaybackTracker, seconds| {
                tick_at(state, &handle, tracker, start, 0);
                tick_at(state, &handle, tracker, start, seconds);
                state.playback.state = PlaybackState::Stopped;
                tick_at(state, &handle, tracker, start, seconds + 1)
            };

        // A scrobbled play.
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        assert_eq!(
            ended_after(&mut state, &mut tracker, 130),
            ScrobbleIndicator::default()
        );
        assert_eq!(queued_scrobbles(&sent(&commands)).len(), 1);

        // A track too short to scrobble.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.duration = Duration::from_secs(30);
        let mut tracker = LastFmPlaybackTracker::default();
        assert_eq!(
            ended_after(&mut state, &mut tracker, 10),
            ScrobbleIndicator::default()
        );

        // A play while scrobbling is paused.
        let mut state = lastfm_playing_state(&track_path);
        state.lastfm_scrobbling_paused = true;
        let mut tracker = LastFmPlaybackTracker::default();
        assert_eq!(
            ended_after(&mut state, &mut tracker, 10),
            ScrobbleIndicator::default()
        );

        // A track that was cued but never played.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.state = PlaybackState::Paused;
        let mut tracker = LastFmPlaybackTracker::default();
        assert_eq!(
            ended_after(&mut state, &mut tracker, 10),
            ScrobbleIndicator::default()
        );
        assert!(queued_scrobbles(&sent(&commands)).is_empty());

        // A play that playback moves on from leaves the slot to the next.
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        tick_at(&state, &handle, &mut tracker, start, 0);
        tick_at(&state, &handle, &mut tracker, start, 10);
        let next_path = p("/fixtures/ended-next.flac");
        state.playback.current = Some(next_path.clone());
        state.metadata.source_path = Some(next_path.to_string_lossy().into_owned());
        let shown = tick_at(&state, &handle, &mut tracker, start, 11);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::None);
        assert_eq!(shown.listened_ms, 0);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());
    }

    #[test]
    fn a_new_play_counts_from_zero_whatever_the_position_it_starts_from() {
        let (handle, _commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/replayed.flac");
        let mut state = lastfm_playing_state(&track_path);
        state.last_track_change.path = Some(track_path.clone());
        state.last_track_change.generation = 1;
        state.last_track_change.track_token = 1;
        state.playback.track_token = 1;
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        state.playback.position = Duration::from_secs(100);
        let shown = tick_at(&state, &handle, &mut tracker, start, 100);
        assert_eq!(shown.listened_ms, 100_000);

        // Repeat One plays the track again. Playback tells the replay
        // before its snapshots carry the replay's token: the end of the
        // previous play, still in them, says nothing about this one.
        state.playback.position = Duration::from_millis(239_900);
        tick_at(&state, &handle, &mut tracker, start, 240);
        state.last_track_change.generation += 1;
        state.last_track_change.track_token = 2;
        for _ in 0..3 {
            let shown = tick_at(&state, &handle, &mut tracker, start, 240);
            assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
            assert_eq!((shown.listened_ms, shown.threshold_ms), (0, 120_000));
        }
        state.playback.track_token = 2;
        state.playback.position = Duration::from_secs(1);
        let shown = tick_at(&state, &handle, &mut tracker, start, 241);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.listened_ms, 1_000);

        // A seek to where the previous play ended leaves the count.
        state.playback.position = Duration::from_millis(239_900);
        let shown = tick_at(&state, &handle, &mut tracker, start, 241);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (1_000, 120_000));

        // A new track counts from zero too, and its own length decides.
        let next_path = p("/fixtures/next-after-replay.flac");
        state.playback.current = Some(next_path.clone());
        state.playback.track_token = 3;
        state.playback.duration = Duration::from_secs(600);
        state.playback.position = Duration::from_secs(2);
        state.metadata.source_path = Some(next_path.to_string_lossy().into_owned());
        let shown = tick_at(&state, &handle, &mut tracker, start, 242);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (0, 240_000));
    }

    /// A restored session cues its track paused where it was left, and
    /// learns its length later. However late in the track, the play counts
    /// down from the whole scrobble point.
    #[test]
    fn a_restored_position_late_in_the_track_keeps_the_countdown() {
        let (handle, _commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/restored.flac");
        let start = Instant::now();
        let mut state = lastfm_playing_state(&track_path);
        state.playback.state = PlaybackState::Paused;
        state.playback.position = Duration::from_secs(200);
        state.playback.duration = Duration::ZERO;
        let mut tracker = LastFmPlaybackTracker::default();
        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.threshold_ms, 0);

        state.playback.duration = Duration::from_secs(240);
        let shown = tick_at(&state, &handle, &mut tracker, start, 1);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::None);
        assert_eq!((shown.listened_ms, shown.threshold_ms), (0, 120_000));
    }

    #[test]
    fn scrobble_indicator_shows_what_last_fm_did_with_this_play_only() {
        let (handle, _commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/delivery.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        tick_at(&state, &handle, &mut tracker, start, 120);
        let PlayScrobble::Queued(Some(ordinal)) = tracker.scrobble else {
            panic!("queued: {:?}", tracker.scrobble);
        };

        let cases = [
            (
                lastfm::ScrobbleDelivery::Pending,
                ScrobbleIndicatorState::Sending,
            ),
            (
                lastfm::ScrobbleDelivery::Waiting,
                ScrobbleIndicatorState::Waiting,
            ),
            (
                lastfm::ScrobbleDelivery::Accepted,
                ScrobbleIndicatorState::Accepted,
            ),
            (
                lastfm::ScrobbleDelivery::Failed,
                ScrobbleIndicatorState::Failed,
            ),
        ];
        for (delivery, expected) in cases {
            state.lastfm.latest_scrobble = Some(lastfm::LatestScrobble {
                ordinal,
                delivery,
                message: format!("{delivery:?}"),
            });
            let shown = scrobble_indicator(&state, &tracker);
            assert_eq!(shown.state, expected);
            assert_eq!(shown.message, format!("{delivery:?}"));
        }

        // Another play's scrobble says nothing about this one.
        state.lastfm.latest_scrobble = Some(lastfm::LatestScrobble {
            ordinal: ordinal + 1,
            delivery: lastfm::ScrobbleDelivery::Failed,
            message: "other".to_string(),
        });
        let shown = scrobble_indicator(&state, &tracker);
        assert_eq!(shown.state, ScrobbleIndicatorState::Sending);
        assert!(shown.message.is_empty());
    }

    #[test]
    fn scrobble_indicator_shows_plays_that_cannot_be_scrobbled() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/short.flac");
        let start = Instant::now();

        // 30 seconds is too short; 31 is not.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.duration = Duration::from_secs(30);
        let mut tracker = LastFmPlaybackTracker::default();
        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Ineligible);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::TooShort);
        assert_eq!(shown.threshold_ms, 0);
        let shown = tick_at(&state, &handle, &mut tracker, start, 30);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::TooShort);
        assert!(queued_scrobbles(&sent(&commands)).is_empty());

        state.playback.duration = Duration::from_secs(31);
        let mut tracker = LastFmPlaybackTracker::default();
        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.threshold_ms, 15_000);

        // Without the track's metadata yet, it may still be scrobbled.
        let mut state = lastfm_playing_state(&track_path);
        state.metadata.source_path = None;
        let mut tracker = LastFmPlaybackTracker::default();
        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        // With metadata that lacks a title, it cannot.
        state.metadata.source_path = Some(track_path.to_string_lossy().into_owned());
        state.metadata.title = String::new();
        let shown = tick_at(&state, &handle, &mut tracker, start, 1);
        assert_eq!(shown.state, ScrobbleIndicatorState::Ineligible);
        assert_eq!(shown.ineligibility, ScrobbleIneligibility::MissingTags);

        // Until playback knows the length, the scrobble point is unknown.
        let mut state = lastfm_playing_state(&track_path);
        state.playback.duration = Duration::ZERO;
        let mut tracker = LastFmPlaybackTracker::default();
        let shown = tick_at(&state, &handle, &mut tracker, start, 0);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        assert_eq!(shown.threshold_ms, 0);
    }

    #[test]
    fn scrobble_indicator_is_idle_unless_a_connected_account_scrobbles() {
        let (handle, _commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/not-connected.flac");
        let start = Instant::now();
        for (enabled, auth_state) in [
            (false, lastfm::AuthState::Connected),
            (true, lastfm::AuthState::Disconnected),
            (true, lastfm::AuthState::ReauthRequired),
        ] {
            let mut state = lastfm_playing_state(&track_path);
            state.lastfm.enabled = enabled;
            state.lastfm.auth_state = auth_state;
            let mut tracker = LastFmPlaybackTracker::default();
            tick_at(&state, &handle, &mut tracker, start, 0);
            assert_eq!(
                tick_at(&state, &handle, &mut tracker, start, 30),
                ScrobbleIndicator::default(),
                "{enabled} {auth_state:?}"
            );
        }
    }

    #[test]
    fn a_play_that_plays_while_scrobbling_is_paused_is_never_scrobbled() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/paused-midway.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        tick_at(&state, &handle, &mut tracker, start, 10);
        assert_eq!(now_playing_tracks(&sent(&commands)).len(), 1);

        state.lastfm_scrobbling_paused = true;
        let shown = tick_at(&state, &handle, &mut tracker, start, 11);
        assert_eq!(shown.state, ScrobbleIndicatorState::Skipped);

        // Resumed before the scrobble point, the play still is not sent.
        state.lastfm_scrobbling_paused = false;
        for seconds in [12, 120, 200, 239] {
            let shown = tick_at(&state, &handle, &mut tracker, start, seconds);
            assert_eq!(shown.state, ScrobbleIndicatorState::Skipped);
        }
        // Nor when it ends.
        finalize_lastfm_track(
            &state,
            &handle,
            &mut tracker,
            start + Duration::from_secs(240),
        );
        assert!(sent(&commands).is_empty());

        // Scrobbling resumes with the next track.
        let next_path = p("/fixtures/next.flac");
        state.playback.current = Some(next_path.clone());
        state.metadata.source_path = Some(next_path.to_string_lossy().into_owned());
        let shown = tick_at(&state, &handle, &mut tracker, start, 241);
        assert_eq!(shown.state, ScrobbleIndicatorState::Listening);
        tick_at(&state, &handle, &mut tracker, start, 361);
        let commands = sent(&commands);
        assert_eq!(now_playing_tracks(&commands).len(), 1);
        assert_eq!(queued_scrobbles(&commands).len(), 1);
    }

    #[test]
    fn nothing_is_sent_for_a_play_while_scrobbling_is_paused() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/paused-throughout.flac");
        let mut state = lastfm_playing_state(&track_path);
        state.lastfm_scrobbling_paused = true;
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        for seconds in [0, 60, 121, 239] {
            let shown = tick_at(&state, &handle, &mut tracker, start, seconds);
            assert_eq!(shown.state, ScrobbleIndicatorState::Skipped);
        }
        assert!(sent(&commands).is_empty());
    }

    /// A play that reached its scrobble point before the pause counts as
    /// scrobbled, as it would once sent: its scrobble is kept, also while
    /// it waits in the offline queue, and Last.fm gets it on resuming.
    #[test]
    fn a_play_that_reached_its_scrobble_point_before_the_pause_counts_as_scrobbled() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/paused-after.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        tick_at(&state, &handle, &mut tracker, start, 121);
        assert_eq!(queued_scrobbles(&sent(&commands)).len(), 1);

        state.lastfm_scrobbling_paused = true;
        let shown = tick_at(&state, &handle, &mut tracker, start, 122);
        assert_eq!(shown.state, ScrobbleIndicatorState::Sending);
    }

    #[test]
    fn pausing_scrobbling_only_while_playback_is_paused_skips_nothing() {
        let (handle, commands) = lastfm::test_support::command_handle();
        let track_path = p("/fixtures/paused-both.flac");
        let mut state = lastfm_playing_state(&track_path);
        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_at(&state, &handle, &mut tracker, start, 0);
        state.playback.state = PlaybackState::Paused;
        state.lastfm_scrobbling_paused = true;
        tick_at(&state, &handle, &mut tracker, start, 5);
        state.lastfm_scrobbling_paused = false;
        state.playback.state = PlaybackState::Playing;
        tick_at(&state, &handle, &mut tracker, start, 6);
        let shown = tick_at(&state, &handle, &mut tracker, start, 126);
        assert_eq!(shown.state, ScrobbleIndicatorState::Sending);
        assert_eq!(queued_scrobbles(&sent(&commands)).len(), 1);
    }
}
