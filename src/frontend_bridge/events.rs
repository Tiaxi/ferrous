// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[cfg(test)]
use crossbeam_channel::bounded;
use crossbeam_channel::{Receiver, Sender};

use crate::analysis::{AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::lastfm::{
    self, Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    NowPlayingTrack as LastFmNowPlayingTrack, ScrobbleEntry as LastFmScrobbleEntry,
};
use crate::library::{is_supported_audio, track_file_fingerprint, IndexedTrack, LibraryEvent};
use crate::metadata::{MetadataEvent, MetadataService};
use crate::playback::{PlaybackEvent, PlaybackSnapshot, PlaybackState, TrackChangeKind};

use super::{
    library_tree, try_send_event, ApplyAlbumArtEvent, BridgeEvent, BridgeState,
    ExternalQueueDetailsEvent, ExternalQueueDetailsRequest, LastFmPlaybackTracker,
    PendingWaveformTrack, SnapshotUrgency,
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
        let _ = try_send_event(event_tx, BridgeEvent::Error(error.clone()));
        if event.indexed_by_path.is_empty() {
            return SnapshotUrgency::None;
        }
    }

    update_library_cover_paths(state, &event.indexed_by_path);
    update_queue_cover_paths(state, &event.indexed_by_path);

    if let Some(current_path) = state.playback.current.as_ref() {
        if let Some(indexed) = event.indexed_by_path.get(current_path) {
            let next_cover_path =
                (!indexed.cover_path.is_empty()).then(|| indexed.cover_path.clone());
            if state.metadata.cover_art_path != next_cover_path {
                state.metadata.cover_art_path = next_cover_path;
            }
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

pub(super) fn update_library_cover_paths(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) {
    let mut next_library = (*state.library).clone();
    let mut changed = false;
    for track in &mut next_library.tracks {
        let Some(indexed) = indexed_by_path.get(&track.path) else {
            continue;
        };
        if track.cover_path == indexed.cover_path {
            continue;
        }
        track.cover_path = indexed.cover_path.clone();
        changed = true;
    }
    if changed {
        next_library.search_revision = next_library.search_revision.saturating_add(1);
        state.library = Arc::new(next_library);
    }
}

pub(super) fn update_queue_cover_paths(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) {
    for (path, indexed) in indexed_by_path {
        let Some(existing) = state.queue_details.get_mut(path) else {
            continue;
        };
        if existing.cover_path != indexed.cover_path {
            existing.cover_path.clone_from(&indexed.cover_path);
        }
        if let Some(fingerprint) = track_file_fingerprint(path) {
            state
                .queue_detail_fingerprints
                .insert(path.clone(), fingerprint);
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
    {
        return SnapshotUrgency::Immediate;
    }
    if previous.position != next.position
        || previous.current_bitrate_kbps != next.current_bitrate_kbps
    {
        return SnapshotUrgency::Heartbeat;
    }
    SnapshotUrgency::None
}

pub(super) fn process_playback_event(
    event: PlaybackEvent,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    match event {
        PlaybackEvent::Snapshot(snapshot) => {
            process_playback_snapshot_event(snapshot, analysis, state)
        }
        PlaybackEvent::TrackChanged {
            path,
            queue_index,
            kind,
            track_token,
        } => {
            state.playback.current_queue_index = Some(queue_index);
            state.analysis.waveform_peaks.clear();
            state.analysis.waveform_coverage_seconds = 0.0;
            state.analysis.waveform_complete = false;
            state.analysis_track_token = track_token;
            metadata.request(path.clone());
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
            if is_gapless {
                state.skip_queue_for_gapless = true;
            }
            SnapshotUrgency::Immediate
        }
        PlaybackEvent::Seeked { position } => {
            if state.analysis_track_token != 0 && state.playback.current.is_some() {
                state.playback.position = position;
                let pos_seconds = position.as_secs_f64();
                analysis.command(AnalysisCommand::SeekPosition(pos_seconds));
            }
            SnapshotUrgency::Heartbeat
        }
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
    if next_state == PlaybackState::Stopped {
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
        analysis.command(AnalysisCommand::PositionUpdate(pos_seconds));
    }
    urgency
}

pub(super) fn drain_playback_events(
    playback_rx: &Receiver<PlaybackEvent>,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    for _ in 0..192 {
        let Ok(event) = playback_rx.try_recv() else {
            break;
        };
        urgency = urgency.max(process_playback_event(event, analysis, metadata, state));
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
    for _ in 0..192 {
        let Ok(event) = playback_rx.try_recv() else {
            break;
        };
        let event_changed = !matches!(event, PlaybackEvent::Seeked { .. });
        let _ = process_playback_event(event, analysis, metadata, state);
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
        MetadataEvent::Loaded(metadata) => {
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

pub(super) fn process_lastfm_event(
    event: LastFmEvent,
    state: &mut BridgeState,
    settings_dirty: &mut bool,
) -> SnapshotUrgency {
    match event {
        LastFmEvent::State(runtime) => {
            let mut urgency = SnapshotUrgency::None;
            if state.lastfm != runtime {
                state.lastfm = runtime.clone();
                urgency = SnapshotUrgency::Immediate;
            }
            if state.settings.integrations.lastfm_username != runtime.username {
                state.settings.integrations.lastfm_username = runtime.username;
                *settings_dirty = true;
                // Flush immediately — the username is the lookup key for the
                // session in the keyring.  Losing it (e.g. Ctrl+C before the
                // periodic 2 s save) makes the session irrecoverable.
                super::config::save_settings(&state.settings);
                urgency = SnapshotUrgency::Immediate;
            }
            urgency
        }
    }
}

pub(super) fn drain_lastfm_events(
    lastfm_rx: &Receiver<LastFmEvent>,
    state: &mut BridgeState,
    settings_dirty: &mut bool,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    for _ in 0..8 {
        let Ok(event) = lastfm_rx.try_recv() else {
            break;
        };
        urgency = urgency.max(process_lastfm_event(event, state, settings_dirty));
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_lastfm_events(
    lastfm_rx: &Receiver<LastFmEvent>,
    state: &mut BridgeState,
    settings_dirty: &mut bool,
) -> bool {
    drain_lastfm_events(lastfm_rx, state, settings_dirty).is_pending()
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
    if tracker.active_path != current_path {
        finalize_lastfm_track(state, lastfm_handle, tracker, now);
        *tracker = LastFmPlaybackTracker::default();
        tracker.active_path = current_path;
        tracker.duration_seconds = match u32::try_from(state.playback.duration.as_secs()) {
            Ok(value) if value > 0 => Some(value),
            _ => None,
        };
    }

    if state.playback.state == PlaybackState::Stopped || tracker.active_path.is_none() {
        finalize_lastfm_track(state, lastfm_handle, tracker, now);
        tracker.active_path = None;
        tracker.artist.clear();
        tracker.track.clear();
        tracker.album.clear();
        tracker.track_number = None;
        tracker.duration_seconds = None;
        tracker.started_at_utc = None;
        tracker.listened_duration = Duration::ZERO;
        tracker.last_listen_tick = None;
        tracker.now_playing_sent = false;
        tracker.scrobble_queued = false;
        return;
    }

    if tracker.duration_seconds.is_none() {
        tracker.duration_seconds = match u32::try_from(state.playback.duration.as_secs()) {
            Ok(value) if value > 0 => Some(value),
            _ => None,
        };
    }
    if tracker.track_number.is_none() {
        tracker.track_number = current_track_number(state);
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
            album: tracker.album.clone(),
            track_number: tracker.track_number,
            duration_seconds: tracker.duration_seconds,
        }));
        tracker.now_playing_sent = true;
    }
}

fn finalize_lastfm_track(
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
        album: tracker.album.clone(),
        track_number: tracker.track_number,
        duration_seconds: tracker.duration_seconds,
        timestamp_utc: tracker.started_at_utc.unwrap_or_else(unix_timestamp_now),
    }));
    tracker.scrobble_queued = true;
}

pub(super) fn current_track_number(state: &BridgeState) -> Option<u32> {
    let path = state.playback.current.as_ref()?;
    state
        .queue_details
        .get(path)
        .and_then(|track| track.track_no)
        .or_else(|| {
            state
                .library
                .tracks
                .iter()
                .find(|track| &track.path == path)
                .and_then(|track| track.track_no)
        })
}

pub(super) fn unix_timestamp_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| i64::try_from(duration.as_secs()).unwrap_or(i64::MAX))
        .unwrap_or(0)
}

pub(super) fn process_library_event(
    event: LibraryEvent,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    match event {
        LibraryEvent::Snapshot(snapshot) => {
            let (artist_count, album_count) = library_tree::compute_artist_album_counts(&snapshot);
            state.library = Arc::new(snapshot);
            state.library_artist_count = artist_count;
            state.library_album_count = album_count;
            if !state.queue.is_empty() {
                let _ = super::sync_queue_details(state, external_queue_details_tx);
            }
            SnapshotUrgency::Immediate
        }
    }
}

pub(super) fn drain_library_events(
    library_rx: &Receiver<LibraryEvent>,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    while let Ok(event) = library_rx.try_recv() {
        urgency = urgency.max(process_library_event(
            event,
            external_queue_details_tx,
            state,
        ));
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_library_events(
    library_rx: &Receiver<LibraryEvent>,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    state: &mut BridgeState,
) -> bool {
    drain_library_events(library_rx, external_queue_details_tx, state).is_pending()
}

pub(super) fn process_external_queue_detail_event(
    event: ExternalQueueDetailsEvent,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let library_paths: HashSet<&Path> = state
        .library
        .tracks
        .iter()
        .map(|track| track.path.as_path())
        .collect();
    let queue_paths: HashSet<&Path> = state.queue.iter().map(PathBuf::as_path).collect();

    let pending = state
        .pending_queue_detail_fingerprints
        .get(&event.path)
        .copied();
    if pending != Some(event.fingerprint) {
        return SnapshotUrgency::None;
    }
    state.pending_queue_detail_fingerprints.remove(&event.path);

    if !queue_paths.contains(event.path.as_path())
        || library_paths.contains(event.path.as_path())
        || !event.path.is_file()
        || !is_supported_audio(&event.path)
        || track_file_fingerprint(&event.path) != Some(event.fingerprint)
    {
        let removed = state.queue_details.remove(&event.path).is_some();
        state.queue_detail_fingerprints.remove(&event.path);
        return if removed {
            SnapshotUrgency::Immediate
        } else {
            SnapshotUrgency::None
        };
    }

    state
        .queue_detail_fingerprints
        .insert(event.path.clone(), event.fingerprint);
    let needs_update = state.queue_details.get(&event.path).is_none_or(|existing| {
        existing.title != event.indexed.title
            || existing.artist != event.indexed.artist
            || existing.album != event.indexed.album
            || existing.cover_path != event.indexed.cover_path
            || existing.genre != event.indexed.genre
            || existing.year != event.indexed.year
            || existing.track_no != event.indexed.track_no
            || existing.duration_secs != event.indexed.duration_secs
    });
    if needs_update {
        state.queue_details.insert(event.path, event.indexed);
        SnapshotUrgency::Immediate
    } else {
        SnapshotUrgency::None
    }
}

pub(super) fn drain_external_queue_detail_events(
    queue_detail_rx: &Receiver<ExternalQueueDetailsEvent>,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    let mut urgency = SnapshotUrgency::None;
    while let Ok(event) = queue_detail_rx.try_recv() {
        urgency = urgency.max(process_external_queue_detail_event(event, state));
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
pub(super) fn pump_external_queue_detail_events(
    queue_detail_rx: &Receiver<ExternalQueueDetailsEvent>,
    state: &mut BridgeState,
) -> bool {
    drain_external_queue_detail_events(queue_detail_rx, state).is_pending()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lastfm::{self, Command as LastFmCommand, ServiceOptions as LastFmServiceOptions};
    use std::fs;
    use std::path::PathBuf;
    use std::sync::{Mutex, MutexGuard, OnceLock};
    use std::time::Instant;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    fn test_dir(name: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|value| value.as_nanos())
            .unwrap_or(0);
        path.push(format!(
            "ferrous-events-{name}-{}-{nanos}",
            std::process::id()
        ));
        path
    }

    fn test_guard() -> MutexGuard<'static, ()> {
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn wait_for_scrobble_queue(path: &Path, timeout: Duration) -> Option<Vec<LastFmScrobbleEntry>> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Ok(text) = fs::read_to_string(path) {
                if let Ok(entries) = serde_json::from_str::<Vec<LastFmScrobbleEntry>>(&text) {
                    return Some(entries);
                }
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        None
    }

    #[test]
    fn complete_precomputed_chunk_is_noted_without_panic() {
        let mut state = BridgeState {
            analysis_track_token: 9,
            ..BridgeState::default()
        };
        let chunk = crate::analysis::PrecomputedSpectrogramChunk {
            track_token: 9,
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
        assert_eq!(state.playback.position, Duration::from_secs(42));
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

    #[test]
    fn lastfm_scrobble_requires_actual_listened_time_instead_of_seek_position() {
        let _guard = test_guard();
        let queue_path = test_dir("lastfm-seek-scrobble").join("lastfm_queue.json");
        let (lastfm_handle, _lastfm_rx) = lastfm::spawn(LastFmServiceOptions {
            queue_path: Some(queue_path.clone()),
            initial_enabled: false,
        });
        let track_path = p("/music/seek-test.flac");
        let mut state = BridgeState::default();
        state.lastfm.enabled = true;
        state.playback.current = Some(track_path.clone());
        state.playback.state = PlaybackState::Playing;
        state.playback.duration = Duration::from_secs(240);
        state.playback.position = Duration::from_secs(5);
        state.metadata.source_path = Some(track_path.to_string_lossy().into_owned());
        state.metadata.artist = "Artist".to_string();
        state.metadata.title = "Track".to_string();
        state.metadata.album = "Album".to_string();

        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);
        assert_eq!(tracker.listened_duration, Duration::ZERO);
        assert!(!tracker.scrobble_queued);

        state.playback.position = Duration::from_secs(180);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(1),
            1_700_000_001,
        );
        assert!(tracker.listened_duration < Duration::from_secs(2));
        assert!(!tracker.scrobble_queued);

        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(120),
            1_700_000_120,
        );
        assert!(tracker.listened_duration >= Duration::from_secs(120));
        assert!(!tracker.scrobble_queued);
        assert!(!queue_path.exists());

        state.playback.state = PlaybackState::Stopped;
        state.playback.current = None;
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(121),
            1_700_000_121,
        );
        assert!(tracker.active_path.is_none());
        assert!(!tracker.scrobble_queued);

        let queued = wait_for_scrobble_queue(&queue_path, Duration::from_secs(1))
            .expect("scrobble queued on stop");
        assert_eq!(queued.len(), 1);
        assert_eq!(queued[0].artist, "Artist");
        assert_eq!(queued[0].track, "Track");

        lastfm_handle.command(LastFmCommand::Shutdown);
    }

    #[test]
    fn lastfm_scrobble_does_not_queue_when_disabled() {
        let _guard = test_guard();
        let queue_path = test_dir("lastfm-disabled-scrobble").join("lastfm_queue.json");
        let (lastfm_handle, _lastfm_rx) = lastfm::spawn(LastFmServiceOptions {
            queue_path: Some(queue_path.clone()),
            initial_enabled: false,
        });
        let track_path = p("/music/disabled-scrobble.flac");
        let mut state = BridgeState::default();
        state.playback.current = Some(track_path.clone());
        state.playback.state = PlaybackState::Playing;
        state.playback.duration = Duration::from_secs(200);
        state.metadata.source_path = Some(track_path.to_string_lossy().into_owned());
        state.metadata.artist = "Artist".to_string();
        state.metadata.title = "Track".to_string();
        state.metadata.album = "Album".to_string();

        let mut tracker = LastFmPlaybackTracker::default();
        let start = Instant::now();
        tick_lastfm_playback_at(&state, &lastfm_handle, &mut tracker, start, 1_700_000_000);
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(101),
            1_700_000_101,
        );
        state.playback.state = PlaybackState::Stopped;
        state.playback.current = None;
        tick_lastfm_playback_at(
            &state,
            &lastfm_handle,
            &mut tracker,
            start + Duration::from_secs(102),
            1_700_000_102,
        );
        assert!(!tracker.scrobble_queued);
        assert!(wait_for_scrobble_queue(&queue_path, Duration::from_millis(150)).is_none());
        assert!(!queue_path.exists());

        lastfm_handle.command(LastFmCommand::Shutdown);
    }
}
