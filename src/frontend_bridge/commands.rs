// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{BufRead, Cursor};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crossbeam_channel::Sender;
use walkdir::WalkDir;

use crate::analysis::{AnalysisCommand, AnalysisEngine};
use crate::lastfm::{self, Command as LastFmCommand, Handle as LastFmHandle};
use crate::library::{
    is_audio_track_file, is_supported_audio, track_file_fingerprint, IndexedTrack, LibraryCommand,
    LibraryService, LibraryTrack, TrackFileFingerprint,
};
use crate::metadata::MetadataService;
use crate::playback::{remap_path, PathRenames, PlaybackCommand, PlaybackEngine};

use super::config::{load_settings_into, save_settings};
use super::imports::{QueueRequest, RenamedDetails};
use super::queue::handle_queue_command;
use super::queue_sync::QueueEdit;
use super::search::{keyed_roots, tree_key_is, tree_path_parts};
use super::{
    library_tree, try_send_event, ApplyAlbumArtRequest, BridgeAnalysisCommand, BridgeCommand,
    BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSettingsCommand, BridgeState, LibrarySortMode, SearchWorkerQuery,
};

pub(super) struct BridgeCommandContext<'a> {
    pub(super) playback: &'a PlaybackEngine,
    pub(super) analysis: &'a AnalysisEngine,
    pub(super) library: &'a LibraryService,
    pub(super) lastfm: &'a LastFmHandle,
    pub(super) search_query_tx: &'a Sender<SearchWorkerQuery>,
    pub(super) apply_album_art_tx: &'a Sender<ApplyAlbumArtRequest>,
    pub(super) event_tx: &'a Sender<BridgeEvent>,
    pub(super) running: &'a mut bool,
    pub(super) settings_dirty: &'a mut bool,
}

struct LibraryCommandRuntime<'a> {
    library: &'a LibraryService,
    apply_album_art_tx: &'a Sender<ApplyAlbumArtRequest>,
    search_query_tx: &'a Sender<SearchWorkerQuery>,
    event_tx: &'a Sender<BridgeEvent>,
}

pub(super) fn handle_bridge_command(
    cmd: BridgeCommand,
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
) -> bool {
    match cmd {
        BridgeCommand::RequestSnapshot => true,
        BridgeCommand::Shutdown => {
            *context.running = false;
            false
        }
        // The runtime answers, resolves, or hands these to its workers
        // before commands get here.
        BridgeCommand::SaveState(_)
        | BridgeCommand::QueueAt { .. }
        | BridgeCommand::Library(BridgeLibraryCommand::RefreshEditedPaths(_)) => false,
        BridgeCommand::Playback(cmd) => {
            handle_playback_bridge_command(&cmd, state, context);
            false
        }
        BridgeCommand::Queue(cmd) => {
            handle_queue_command(cmd, state, context.playback, context.event_tx)
        }
        BridgeCommand::Library(cmd) => handle_library_command(
            cmd,
            state,
            &LibraryCommandRuntime {
                library: context.library,
                apply_album_art_tx: context.apply_album_art_tx,
                search_query_tx: context.search_query_tx,
                event_tx: context.event_tx,
            },
        ),
        BridgeCommand::Analysis(cmd) => match cmd {
            BridgeAnalysisCommand::SetFftSize(size) => {
                let fft = size.clamp(512, 8192).next_power_of_two();
                state.settings.fft_size = fft;
                *context.settings_dirty = true;
                context.analysis.command(AnalysisCommand::SetFftSize(fft));
                true
            }
            BridgeAnalysisCommand::SetSpectrogramZoomLevel(level) => {
                context
                    .analysis
                    .command(AnalysisCommand::SetSpectrogramZoomLevel(level));
                true
            }
            BridgeAnalysisCommand::SetSpectrogramActive(active) => {
                context
                    .analysis
                    .command(AnalysisCommand::SetSpectrogramActive(active));
                false
            }
            BridgeAnalysisCommand::SetSpectrogramWidgetWidth(width) => {
                context
                    .analysis
                    .command(AnalysisCommand::SetSpectrogramWidgetWidth(width));
                true
            }
        },
        BridgeCommand::Settings(cmd) => {
            handle_settings_bridge_command(&cmd, state, context);
            true
        }
    }
}

fn handle_playback_bridge_command(
    cmd: &BridgePlaybackCommand,
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
) {
    match cmd {
        BridgePlaybackCommand::Play => context.playback.command(PlaybackCommand::Play),
        BridgePlaybackCommand::Pause => context.playback.command(PlaybackCommand::Pause),
        BridgePlaybackCommand::Stop => context.playback.command(PlaybackCommand::Stop),
        BridgePlaybackCommand::Next => context.playback.command(PlaybackCommand::Next),
        BridgePlaybackCommand::Previous => context.playback.command(PlaybackCommand::Previous),
        BridgePlaybackCommand::Seek(pos) => {
            context.playback.command(PlaybackCommand::Seek(*pos));
        }
        BridgePlaybackCommand::SetVolume(volume) => {
            let volume = volume.clamp(0.0, 1.0);
            context.playback.command(PlaybackCommand::SetVolume(volume));
            state.settings.volume = volume;
            *context.settings_dirty = true;
        }
        BridgePlaybackCommand::SetRepeatMode(mode) => {
            context
                .playback
                .command(PlaybackCommand::SetRepeatMode(*mode));
        }
        BridgePlaybackCommand::SetShuffle(enabled) => {
            context
                .playback
                .command(PlaybackCommand::SetShuffle(*enabled));
        }
        BridgePlaybackCommand::ToggleChannelMute(ch) => {
            context
                .playback
                .command(PlaybackCommand::ToggleChannelMute(*ch));
        }
        BridgePlaybackCommand::SoloChannel(ch) => {
            context.playback.command(PlaybackCommand::SoloChannel(*ch));
        }
    }
}

fn apply_analysis_spectrogram_settings(state: &mut BridgeState, analysis: &AnalysisEngine) {
    // Send display mode first so that self.display_mode is correct before
    // SetFftSize / SetSpectrogramViewMode restart the session.
    analysis.command(AnalysisCommand::SetSpectrogramDisplayMode(
        state.settings.spectrogram_display_mode,
    ));
    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));
    analysis.command(AnalysisCommand::SetSpectrogramViewMode(
        state.settings.spectrogram_view_mode,
    ));
}

fn handle_load_settings_from_disk(state: &mut BridgeState, context: &mut BridgeCommandContext<'_>) {
    load_settings_into(&mut state.settings);
    context
        .library
        .set_watching(state.settings.library_watch_enabled);
    state.lastfm.enabled = state.settings.integrations.lastfm_scrobbling_enabled;
    context
        .playback
        .command(PlaybackCommand::SetVolume(state.settings.volume));
    apply_analysis_spectrogram_settings(state, context.analysis);
    context.lastfm.command(LastFmCommand::SetEnabled(
        state.settings.integrations.lastfm_scrobbling_enabled,
    ));
    if !state
        .settings
        .integrations
        .lastfm_username
        .trim()
        .is_empty()
    {
        context.lastfm.command(LastFmCommand::LoadStoredSession {
            username: state.settings.integrations.lastfm_username.clone(),
        });
    }
}

fn set_display_sleep_prevention(
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
    enabled: bool,
) {
    state.settings.display.prevent_display_sleep_in_fullscreen = enabled;
    *context.settings_dirty = true;
}

fn set_library_watching(
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
    enabled: bool,
) {
    state.settings.library_watch_enabled = enabled;
    context.library.set_watching(enabled);
    *context.settings_dirty = true;
}

fn set_fft_size(state: &mut BridgeState, context: &mut BridgeCommandContext<'_>, size: usize) {
    let fft = size.clamp(512, 8192).next_power_of_two();
    state.settings.fft_size = fft;
    context.analysis.command(AnalysisCommand::SetFftSize(fft));
    *context.settings_dirty = true;
}

/// Save the settings, showing the user a failure.
fn save_settings_now(state: &BridgeState, context: &mut BridgeCommandContext<'_>) {
    match save_settings(&state.settings) {
        Ok(()) => *context.settings_dirty = false,
        Err(failure) => {
            let _ = try_send_event(context.event_tx, BridgeEvent::Error(failure.message));
        }
    }
}

fn handle_settings_bridge_command(
    cmd: &BridgeSettingsCommand,
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
) {
    match cmd {
        BridgeSettingsCommand::LoadFromDisk => {
            handle_load_settings_from_disk(state, context);
        }
        BridgeSettingsCommand::SaveToDisk => save_settings_now(state, context),
        BridgeSettingsCommand::SetVolume(volume) => {
            let volume = volume.clamp(0.0, 1.0);
            state.settings.volume = volume;
            context.playback.command(PlaybackCommand::SetVolume(volume));
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetFftSize(size) => set_fft_size(state, context, *size),
        BridgeSettingsCommand::SetSpectrogramViewMode(view_mode) => {
            state.settings.spectrogram_view_mode = *view_mode;
            context
                .analysis
                .command(AnalysisCommand::SetSpectrogramViewMode(*view_mode));
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetSpectrogramDisplayMode(mode) => {
            state.settings.spectrogram_display_mode = *mode;
            context
                .analysis
                .command(AnalysisCommand::SetSpectrogramDisplayMode(*mode));
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetViewerFullscreenMode(mode) => {
            state.settings.viewer_fullscreen_mode = *mode;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetPreventDisplaySleepInFullscreen(enabled) => {
            set_display_sleep_prevention(state, context, *enabled);
        }
        BridgeSettingsCommand::SetDbRange(value) => {
            state.settings.db_range = value.clamp(50.0, 150.0);
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetLogScale(enabled) => {
            state.settings.display.log_scale = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetShowLevelMeter(enabled) => {
            state.settings.display.show_level_meter = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetShowFps(enabled) => {
            state.settings.display.show_fps = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetShowSpectrogramCrosshair(enabled) => {
            state.settings.display.show_spectrogram_crosshair = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetShowSpectrogramScale(enabled) => {
            state.settings.display.show_spectrogram_scale = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetChannelButtonsVisibility(value) => {
            state.settings.display.channel_buttons_visibility = (*value).min(2);
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetSpectrogramZoomEnabled(enabled) => {
            state.settings.display.spectrogram_zoom_enabled = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetSystemMediaControlsEnabled(enabled) => {
            state.settings.integrations.system_media_controls_enabled = *enabled;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetLibrarySortMode(mode) => {
            state.settings.library_sort_mode = *mode;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetLibraryWatchEnabled(enabled) => {
            set_library_watching(state, context, *enabled);
        }
        BridgeSettingsCommand::SetLastFmScrobblingEnabled(enabled) => {
            state.settings.integrations.lastfm_scrobbling_enabled = *enabled;
            state.lastfm.enabled = *enabled;
            context.lastfm.command(LastFmCommand::SetEnabled(*enabled));
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::BeginLastFmAuth => {
            context.lastfm.command(LastFmCommand::BeginDesktopAuth);
        }
        BridgeSettingsCommand::CompleteLastFmAuth => {
            context.lastfm.command(LastFmCommand::CompleteDesktopAuth);
        }
        BridgeSettingsCommand::DisconnectLastFm => {
            state.settings.integrations.lastfm_username.clear();
            state.lastfm.username.clear();
            state.lastfm.auth_url.clear();
            state.lastfm.auth_state = lastfm::AuthState::Disconnected;
            context
                .lastfm
                .command(LastFmCommand::Disconnect { clear_queue: true });
            *context.settings_dirty = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Library commands
// ---------------------------------------------------------------------------

fn handle_library_command(
    cmd: BridgeLibraryCommand,
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
) -> bool {
    if let Some(outcome) = handle_library_root_command(&cmd, runtime.library) {
        return outcome;
    }
    handle_library_view_command(cmd, state, runtime).unwrap_or(false)
}

fn handle_library_root_command(
    cmd: &BridgeLibraryCommand,
    library: &LibraryService,
) -> Option<bool> {
    match cmd {
        BridgeLibraryCommand::ScanRoot(path) => {
            library.command(LibraryCommand::ScanRoot(path.clone()));
            Some(false)
        }
        BridgeLibraryCommand::AddRoot { path, name } => {
            library.command(LibraryCommand::AddRoot {
                path: path.clone(),
                name: name.clone(),
            });
            Some(false)
        }
        BridgeLibraryCommand::RenameRoot { path, name } => {
            library.command(LibraryCommand::RenameRoot {
                path: path.clone(),
                name: name.clone(),
            });
            Some(false)
        }
        BridgeLibraryCommand::RemoveRoot(path) => {
            library.command(LibraryCommand::RemoveRoot(path.clone()));
            Some(false)
        }
        BridgeLibraryCommand::RescanRoot(path) => {
            library.command(LibraryCommand::RescanRoot(path.clone()));
            Some(false)
        }
        BridgeLibraryCommand::RescanAll => {
            library.command(LibraryCommand::RescanAll);
            Some(false)
        }
        _ => None,
    }
}

fn handle_library_view_command(
    cmd: BridgeLibraryCommand,
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
) -> Option<bool> {
    match cmd {
        BridgeLibraryCommand::ApplyAlbumArt {
            track_path,
            artwork_path,
        } => {
            if runtime
                .apply_album_art_tx
                .send(ApplyAlbumArtRequest {
                    track_path,
                    artwork_path,
                })
                .is_err()
            {
                let _ = try_send_event(
                    runtime.event_tx,
                    BridgeEvent::Error("failed to queue album art apply request".to_string()),
                );
            }
            Some(false)
        }
        BridgeLibraryCommand::ShowTrackInLibrary(path) => {
            Some(reveal_track_in_library(state, &path))
        }
        BridgeLibraryCommand::SetNodeExpanded { key, expanded } => {
            let normalized = key.trim();
            if normalized.is_empty() {
                return Some(false);
            }
            Some(if expanded {
                state.expanded_keys.insert(normalized.to_string())
            } else {
                state.expanded_keys.remove(normalized)
            })
        }
        BridgeLibraryCommand::SetViewState {
            selection_key,
            anchor_key,
            anchor_offset,
        } => {
            let selection_key = selection_key.trim();
            let anchor_key = anchor_key.trim();
            let anchor_offset = if anchor_offset.is_finite() {
                anchor_offset.clamp(-24.0, 24.0)
            } else {
                0.0
            };
            let changed = state.library_view_selection_key != selection_key
                || state.library_view_anchor_key != anchor_key
                || (state.library_view_anchor_offset - anchor_offset).abs() > f32::EPSILON;
            if changed {
                state.library_view_selection_key = selection_key.to_string();
                state.library_view_anchor_key = anchor_key.to_string();
                state.library_view_anchor_offset = anchor_offset;
            }
            Some(changed)
        }
        BridgeLibraryCommand::SetSearchQuery { seq, query, limit } => {
            let _ = runtime.search_query_tx.send(SearchWorkerQuery {
                limit,
                seq,
                query: query.trim().to_string(),
                library: Arc::clone(&state.library),
            });
            Some(false)
        }
        _ => None,
    }
}

fn reveal_track_in_library(state: &mut BridgeState, path: &Path) -> bool {
    let Some(track) = state
        .library_index
        .position(&state.library, path)
        .map(|position| &state.library.tracks[position])
    else {
        return false;
    };
    let Some(tree_context) =
        super::search::derive_tree_path_context(&track.path, &state.library.roots, &track.artist)
    else {
        return false;
    };
    state.expanded_keys.insert(tree_context.artist_key.clone());
    if let Some(album_key) = tree_context.album_key {
        state.expanded_keys.insert(album_key);
    }
    if let Some(section_key) = tree_context.section_key {
        state.expanded_keys.insert(section_key);
    }
    state
        .library_view_selection_key
        .clone_from(&tree_context.track_key);
    state
        .library_view_anchor_key
        .clone_from(&tree_context.track_key);
    state.library_view_anchor_offset = 0.0;
    state.library_reveal_generation = state.library_reveal_generation.wrapping_add(1);
    true
}

// ---------------------------------------------------------------------------
// Queue content changes
// ---------------------------------------------------------------------------

/// The queue content change `cmd` requests, if any. Library selections are
/// resolved here from the in-memory library; imports are expanded later on
/// the import worker. Both are applied in arrival order by `ImportWorker`.
pub(super) fn queue_request(cmd: &BridgeCommand, state: &BridgeState) -> Option<QueueRequest> {
    let library = match cmd {
        BridgeCommand::Queue(
            command @ (BridgeQueueCommand::Replace { .. } | BridgeQueueCommand::Append(_)),
        ) => return Some(QueueRequest::Command(command.clone())),
        BridgeCommand::Library(library) => library,
        _ => return None,
    };
    let sort_mode = state.settings.library_sort_mode;
    let (tracks, replace) = match library {
        BridgeLibraryCommand::AddTrack(path) | BridgeLibraryCommand::PlayTrack(path) => {
            return Some(QueueRequest::Import {
                paths: vec![path.clone()],
                replace: matches!(library, BridgeLibraryCommand::PlayTrack(_)),
            });
        }
        BridgeLibraryCommand::ReplaceWithAlbum(paths)
        | BridgeLibraryCommand::AppendAlbum(paths) => {
            return Some(QueueRequest::Import {
                paths: paths.clone(),
                replace: matches!(library, BridgeLibraryCommand::ReplaceWithAlbum(_)),
            });
        }
        BridgeLibraryCommand::ReplaceAlbumByKey { artist, album } => (
            collect_album_paths_for_queue(&state.library, artist, album),
            true,
        ),
        BridgeLibraryCommand::AppendAlbumByKey { artist, album } => (
            collect_album_paths_for_queue(&state.library, artist, album),
            false,
        ),
        BridgeLibraryCommand::ReplaceArtistByKey { artist } => (
            collect_artist_paths_for_queue(&state.library, artist, sort_mode),
            true,
        ),
        BridgeLibraryCommand::AppendArtistByKey { artist } => (
            collect_artist_paths_for_queue(&state.library, artist, sort_mode),
            false,
        ),
        BridgeLibraryCommand::ReplaceRootByPath { root } => (
            library_tree::collect_root_paths_tree_order(&state.library, root, sort_mode),
            true,
        ),
        BridgeLibraryCommand::AppendRootByPath { root } => (
            library_tree::collect_root_paths_tree_order(&state.library, root, sort_mode),
            false,
        ),
        BridgeLibraryCommand::ReplaceAllTracks => (
            library_tree::collect_all_paths_tree_order(&state.library, sort_mode),
            true,
        ),
        BridgeLibraryCommand::AppendAllTracks => (
            library_tree::collect_all_paths_tree_order(&state.library, sort_mode),
            false,
        ),
        _ => return None,
    };
    Some(QueueRequest::Tracks { tracks, replace })
}

// ---------------------------------------------------------------------------
// Collection path helpers
// ---------------------------------------------------------------------------

fn collect_artist_paths_for_queue(
    library: &crate::library::LibrarySnapshot,
    artist: &str,
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    // Delegate to the tree module so the playlist order exactly matches
    // the library tree display order (album sorting by year/title,
    // root tracks → disc sections → non-disc sections).
    library_tree::collect_artist_paths_tree_order(library, artist.trim(), sort_mode)
}

fn normalized_library_artist(track: &LibraryTrack) -> &str {
    if track.artist.trim().is_empty() {
        "Unknown Artist"
    } else {
        track.artist.as_str()
    }
}

fn normalized_library_album(track: &LibraryTrack) -> &str {
    if track.album.trim().is_empty() {
        "Unknown Album"
    } else {
        track.album.as_str()
    }
}

/// The library tracks of the album selected by `artist` and `album`. Every
/// library track is matched, so the match borrows from the tracks instead of
/// building their tree keys.
fn collect_album_paths_for_queue(
    library: &crate::library::LibrarySnapshot,
    artist: &str,
    album: &str,
) -> Vec<PathBuf> {
    let artist_selector = artist.trim();
    let album_selector = album.trim();
    if album_selector.is_empty() {
        return Vec::new();
    }
    let artist_selector_is_key = artist_selector.starts_with("artist|");
    let album_selector_is_key = album_selector.starts_with("album|");
    let roots = keyed_roots(&library.roots);

    library
        .tracks
        .iter()
        .filter(|track| {
            let parts = tree_path_parts(&track.path, &roots, &track.artist);

            if album_selector_is_key {
                let Some(parts) = parts else {
                    return false;
                };
                let Some(album_folder) = parts.album_folder.as_deref() else {
                    return false;
                };
                if !tree_key_is(
                    album_selector,
                    "album",
                    &[parts.root_key, &parts.artist_name, album_folder],
                ) {
                    return false;
                }
                // Only include root album tracks and recognised disc
                // sections (Disc 1, CD 2, …).  Exclude bonus/extra
                // subfolders so the queue represents the main album.
                return parts.is_main_level_album_track || parts.is_disc_section_album_track();
            }

            let artist_matches = if artist_selector_is_key {
                parts.as_ref().is_some_and(|parts| {
                    tree_key_is(
                        artist_selector,
                        "artist",
                        &[parts.root_key, &parts.artist_name],
                    )
                })
            } else if let Some(parts) = parts.as_ref() {
                parts.artist_name == artist_selector
            } else {
                normalized_library_artist(track) == artist_selector
            };
            if !artist_matches {
                return false;
            }

            if let Some(parts) = parts {
                if parts.album_folder.as_deref().unwrap_or("Unknown Album") != album_selector {
                    return false;
                }
                return parts.is_main_level_album_track || parts.is_disc_section_album_track();
            }
            normalized_library_album(track) == album_selector
        })
        .map(|track| track.path.clone())
        .collect()
}

// ---------------------------------------------------------------------------
// Import / path expansion
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(super) struct ImportExpandOutcome {
    pub(super) tracks: Vec<PathBuf>,
    missing_count: usize,
    unsupported_count: usize,
    unreadable_count: usize,
    non_local_url_count: usize,
    invalid_playlist_count: usize,
}

impl ImportExpandOutcome {
    /// The outcome of an import that could read none of its `count` inputs.
    pub(super) fn all_unreadable(count: usize) -> Self {
        Self {
            unreadable_count: count,
            ..Self::default()
        }
    }

    fn skipped_count(&self) -> usize {
        self.missing_count
            + self.unsupported_count
            + self.unreadable_count
            + self.non_local_url_count
            + self.invalid_playlist_count
    }

    fn push_missing(&mut self) {
        self.missing_count = self.missing_count.saturating_add(1);
    }

    fn push_unsupported(&mut self) {
        self.unsupported_count = self.unsupported_count.saturating_add(1);
    }

    fn push_unreadable(&mut self) {
        self.unreadable_count = self.unreadable_count.saturating_add(1);
    }

    fn push_non_local_url(&mut self) {
        self.non_local_url_count = self.non_local_url_count.saturating_add(1);
    }
}

/// Decode playlist file bytes. Tries UTF-8 first; falls back to ISO-8859-1
/// (the legacy default for `.m3u` files, where every byte maps 1:1 to a Unicode code point).
fn decode_playlist_bytes(bytes: &[u8]) -> String {
    match std::str::from_utf8(bytes) {
        Ok(s) => s.to_owned(),
        Err(_) => bytes.iter().map(|&b| char::from(b)).collect(),
    }
}

fn is_playlist_file(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| matches!(ext.to_ascii_lowercase().as_str(), "m3u" | "m3u8"))
}

fn canonicalize_existing_path(path: &Path) -> Option<PathBuf> {
    if !path.exists() {
        return None;
    }
    path.canonicalize()
        .ok()
        .or_else(|| Some(path.to_path_buf()))
}

fn playlist_entry_path(
    line: &str,
    base_dir: &Path,
    outcome: &mut ImportExpandOutcome,
) -> Option<PathBuf> {
    let trimmed = line.trim();
    if trimmed.is_empty() || trimmed.starts_with('#') {
        return None;
    }

    if let Ok(url) = url::Url::parse(trimmed) {
        if url.scheme() != "file" {
            outcome.push_non_local_url();
            return None;
        }
        let Ok(path) = url.to_file_path() else {
            outcome.push_non_local_url();
            return None;
        };
        let Some(path) = canonicalize_existing_path(&path) else {
            outcome.push_missing();
            return None;
        };
        return Some(path);
    }

    let candidate = PathBuf::from(trimmed);
    let resolved = if candidate.is_absolute() {
        candidate
    } else {
        base_dir.join(candidate)
    };
    let Some(resolved) = canonicalize_existing_path(&resolved) else {
        outcome.push_missing();
        return None;
    };
    Some(resolved)
}

fn append_folder_tracks(
    root: &Path,
    outcome: &mut ImportExpandOutcome,
    cancelled: &impl Fn() -> bool,
) {
    let mut folder_tracks = Vec::new();
    for entry in WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_map(std::result::Result::ok)
    {
        if cancelled() {
            return;
        }
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        if is_playlist_file(path) || !is_audio_track_file(path) {
            continue;
        }
        folder_tracks.push(path.canonicalize().unwrap_or_else(|_| path.to_path_buf()));
    }
    folder_tracks.sort_by(|a, b| a.to_string_lossy().cmp(&b.to_string_lossy()));
    outcome.tracks.extend(folder_tracks);
}

fn append_import_path(
    path: &Path,
    outcome: &mut ImportExpandOutcome,
    active_playlists: &mut HashSet<PathBuf>,
    cancelled: &impl Fn() -> bool,
) {
    if cancelled() {
        return;
    }
    if path.is_dir() {
        append_folder_tracks(path, outcome, cancelled);
        return;
    }

    if is_playlist_file(path) {
        if active_playlists.len() >= 64 || !active_playlists.insert(path.to_path_buf()) {
            outcome.invalid_playlist_count += 1;
            return;
        }
        let Ok(bytes) = fs::read(path) else {
            outcome.push_unreadable();
            active_playlists.remove(path);
            return;
        };
        let mut reader = Cursor::new(decode_playlist_bytes(&bytes));
        let base_dir = path.parent().unwrap_or_else(|| Path::new(""));
        let mut line = String::new();
        while let Ok(read) = reader.read_line(&mut line) {
            if read == 0 || cancelled() {
                break;
            }
            let cleaned = line.trim_start_matches('\u{feff}');
            if let Some(entry_path) = playlist_entry_path(cleaned, base_dir, outcome) {
                append_import_path(&entry_path, outcome, active_playlists, cancelled);
            }
            line.clear();
        }
        active_playlists.remove(path);
        return;
    }

    if path.is_file() && is_audio_track_file(path) {
        outcome.tracks.push(path.to_path_buf());
        return;
    }

    outcome.push_unsupported();
}

#[cfg(test)]
fn expand_import_paths(paths: Vec<PathBuf>) -> ImportExpandOutcome {
    expand_import_paths_cancellable(paths, &|| false)
}

pub(super) fn expand_import_paths_cancellable(
    paths: Vec<PathBuf>,
    cancelled: &impl Fn() -> bool,
) -> ImportExpandOutcome {
    let mut active_playlists = HashSet::new();
    let mut outcome = ImportExpandOutcome::default();
    for raw_path in paths {
        if cancelled() {
            break;
        }
        let Some(path) = canonicalize_existing_path(&raw_path) else {
            outcome.push_missing();
            continue;
        };
        append_import_path(&path, &mut outcome, &mut active_playlists, cancelled);
    }
    outcome
}

pub(super) fn format_import_warning(action: &str, outcome: &ImportExpandOutcome) -> Option<String> {
    let skipped = outcome.skipped_count();
    if skipped == 0 {
        return None;
    }

    let mut parts = Vec::new();
    if outcome.missing_count > 0 {
        parts.push(format!("{} missing", outcome.missing_count));
    }
    if outcome.unsupported_count > 0 {
        parts.push(format!("{} unsupported", outcome.unsupported_count));
    }
    if outcome.unreadable_count > 0 {
        parts.push(format!("{} unreadable", outcome.unreadable_count));
    }
    if outcome.non_local_url_count > 0 {
        parts.push(format!("{} non-local URLs", outcome.non_local_url_count));
    }

    if outcome.invalid_playlist_count > 0 {
        parts.push(format!(
            "{} cyclic or excessively nested playlists",
            outcome.invalid_playlist_count
        ));
    }
    let joined = parts.join(", ");
    if outcome.tracks.is_empty() {
        Some(format!("Import {action} skipped all entries ({joined})"))
    } else {
        Some(format!(
            "Import {} queued {} track(s); skipped {} item(s) ({joined})",
            action,
            outcome.tracks.len(),
            skipped,
        ))
    }
}

// ---------------------------------------------------------------------------
// Queue detail sync
// ---------------------------------------------------------------------------

/// Have the validation worker check the queue's files again.
pub(super) fn sync_queue_details(state: &mut BridgeState) {
    state.queue_detail_generation = state.queue_detail_generation.saturating_add(1);
    state.queue_detail_validation_requested = true;
}

/// Bring the details of the queued files outside the library up to date
/// with the files, taking them from `load_cached` where it has them. Returns
/// the files to read, with the state they were found in. It looks at every
/// queued file, so it runs on the validation worker, never the bridge thread.
pub(super) fn validate_queue_details(
    state: &mut BridgeState,
    cancelled: &impl Fn() -> bool,
    mut load_cached: impl FnMut(&[(PathBuf, TrackFileFingerprint)]) -> HashMap<PathBuf, IndexedTrack>,
) -> Vec<(PathBuf, TrackFileFingerprint)> {
    let queue_paths: HashSet<&Path> = state.queue.iter().map(PathBuf::as_path).collect();
    let library_paths: HashSet<&Path> = state
        .library
        .tracks
        .iter()
        .map(|track| track.path.as_path())
        .collect();

    Arc::make_mut(&mut state.queue_details).retain(|path, _| queue_paths.contains(path.as_path()));
    state
        .queue_detail_fingerprints
        .retain(|path, _| queue_paths.contains(path.as_path()));
    let mut unknown = Vec::<(PathBuf, TrackFileFingerprint)>::new();

    for path in &state.queue {
        if cancelled() {
            return Vec::new();
        }
        let fingerprint = if library_paths.contains(path.as_path()) || !is_supported_audio(path) {
            None
        } else {
            path.is_file()
                .then(|| track_file_fingerprint(path))
                .flatten()
        };
        let Some(fingerprint) = fingerprint else {
            Arc::make_mut(&mut state.queue_details).remove(path);
            state.queue_detail_fingerprints.remove(path);
            continue;
        };

        let cached_fingerprint = state.queue_detail_fingerprints.get(path).copied();
        if cached_fingerprint == Some(fingerprint) && state.queue_details.contains_key(path) {
            continue;
        }
        if cached_fingerprint.is_some_and(|cached| cached != fingerprint) {
            Arc::make_mut(&mut state.queue_details).remove(path);
            state.queue_detail_fingerprints.remove(path);
        }
        unknown.push((path.clone(), fingerprint));
    }

    let cached_rows = load_cached(&unknown);
    unknown.retain(|(path, fingerprint)| {
        let Some(indexed) = cached_rows.get(path) else {
            return true;
        };
        state
            .queue_detail_fingerprints
            .insert(path.clone(), *fingerprint);
        if state.queue_details.get(path) != Some(indexed) {
            Arc::make_mut(&mut state.queue_details).insert(path.clone(), indexed.clone());
        }
        false
    });
    unknown
}

// ---------------------------------------------------------------------------
// Track updates
// ---------------------------------------------------------------------------

fn track_details_differ(track: &LibraryTrack, indexed: &IndexedTrack) -> bool {
    track.title != indexed.title
        || track.artist != indexed.artist
        || track.album != indexed.album
        || track.cover_path != indexed.cover_path
        || track.search_tags != indexed.search_tags
        || track.genre != indexed.genre
        || track.year != indexed.year
        || track.track_no != indexed.track_no
        || track.duration_secs != indexed.duration_secs
}

fn copy_track_details(track: &mut LibraryTrack, indexed: &IndexedTrack) {
    track.title.clone_from(&indexed.title);
    track.artist.clone_from(&indexed.artist);
    track.album.clone_from(&indexed.album);
    track.cover_path.clone_from(&indexed.cover_path);
    track.search_tags.clone_from(&indexed.search_tags);
    track.genre.clone_from(&indexed.genre);
    track.year = indexed.year;
    track.track_no = indexed.track_no;
    track.duration_secs = indexed.duration_secs;
}

/// Give the library's tracks the details in `indexed_by_path`. The library is
/// found through its path index and copied only if a track changes.
fn update_library_track_details(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) -> bool {
    let mut updates = Vec::new();
    for (path, indexed) in indexed_by_path {
        for position in state.library_index.positions(&state.library, path) {
            if track_details_differ(&state.library.tracks[position], indexed) {
                updates.push((position, indexed));
            }
        }
    }
    if updates.is_empty() {
        return false;
    }
    let previous = Arc::clone(&state.library);
    let mut next_library = (*previous).clone();
    for (position, indexed) in updates {
        let track = Arc::make_mut(&mut next_library.tracks[position]);
        copy_track_details(track, indexed);
        state.library_rows_to_read.push(track.path.clone());
    }
    next_library.search_revision = next_library.search_revision.saturating_add(1);
    state.library = Arc::new(next_library);
    // The paths stay as they were, so the index still holds.
    state.library_index.carry_over(&previous, &state.library);
    true
}

/// Give queued files outside the library the details in `indexed_by_path`,
/// which were read when the files were in the states in `fingerprints`.
fn update_queue_track_details(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
    fingerprints: &HashMap<PathBuf, TrackFileFingerprint>,
) -> bool {
    let mut changed = false;
    for (path, indexed) in indexed_by_path {
        if state.library_index.contains(&state.library, path) {
            changed |= Arc::make_mut(&mut state.queue_details)
                .remove(path)
                .is_some();
            state.queue_detail_fingerprints.remove(path);
            continue;
        }
        let needs_update = state
            .queue_details
            .get(path)
            .is_none_or(|existing| existing != indexed);
        if needs_update {
            Arc::make_mut(&mut state.queue_details).insert(path.clone(), indexed.clone());
            changed = true;
        }
        if let Some(fingerprint) = fingerprints.get(path) {
            state
                .queue_detail_fingerprints
                .insert(path.clone(), *fingerprint);
        }
    }
    changed
}

/// Apply the details the edit refresh worker read again from files the tag
/// editor saved.
pub(super) fn apply_edited_details(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
    fingerprints: &HashMap<PathBuf, TrackFileFingerprint>,
    metadata: &MetadataService,
) -> bool {
    if indexed_by_path.is_empty() {
        return false;
    }
    let mut changed = update_library_track_details(state, indexed_by_path);
    changed |= update_queue_track_details(state, indexed_by_path, fingerprints);

    if let Some(current_path) = state.playback.current.as_ref() {
        if indexed_by_path.contains_key(current_path) {
            metadata.request(current_path.clone());
            changed = true;
        }
    }

    sync_queue_details(state);
    changed
}

/// Apply one map of files renamed on disk to the library, the queue, the
/// engine, and analysis, with the details the rename read from the files.
/// The whole map applies at once, so a swap or renumbering, where a file
/// takes another renamed file's old name, renames each file exactly once.
pub(super) fn refresh_renamed_paths(
    state: &mut BridgeState,
    rename_map: Arc<PathRenames>,
    mut details: RenamedDetails,
    playback: &PlaybackEngine,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    if rename_map.is_empty() {
        return false;
    }
    if let Some(problem) = details.problem.take() {
        let _ = try_send_event(event_tx, BridgeEvent::Error(problem));
    }

    let mut changed = false;
    changed |= apply_renamed_library_paths(state, &rename_map, &details);
    changed |= apply_renamed_queue_paths(state, &rename_map, &details.indexed, metadata);
    changed |= update_queue_track_details(state, &details.indexed, &details.fingerprints);
    sync_queue_details(state);
    // The engine plays from its own copy of the queue. Until it acknowledges
    // the renames, its events are remapped so they cannot restore old paths.
    state
        .unacknowledged_engine_renames
        .push_back(Arc::clone(&rename_map));
    // Analysis reopens the current and staged files on later restarts.
    analysis.command(AnalysisCommand::RemapPaths(Arc::clone(&rename_map)));
    playback.command(PlaybackCommand::RemapPaths(rename_map));
    changed
}

/// Move the library's renamed tracks to their new paths, in one pass that
/// finds them through the path index. A renamed track goes to its new
/// path's place in the library's order, unless the path is gone. The library
/// is copied only if a track changes.
fn apply_renamed_library_paths(
    state: &mut BridgeState,
    rename_map: &PathRenames,
    details: &RenamedDetails,
) -> bool {
    let new_paths: HashSet<&PathBuf> = rename_map.values().collect();
    let mut changes = crate::library::LibraryChanges::default();
    for (old_path, new_path) in rename_map {
        let indexed = details.indexed.get(new_path);
        let gone = details.gone.contains(new_path);
        let Some(position) = state.library_index.position(&state.library, old_path) else {
            continue;
        };
        let track = &state.library.tracks[position];
        if &track.path == new_path
            && !gone
            && !indexed.is_some_and(|indexed| track_details_differ(track, indexed))
        {
            continue;
        }
        if gone {
            // A read after the rename found the file gone again.
            if !new_paths.contains(old_path) {
                changes.removed.push(old_path.clone());
            }
            changes.removed.push(new_path.clone());
            state
                .library_rows_to_read
                .extend([old_path.clone(), new_path.clone()]);
            continue;
        }
        let mut renamed = (**track).clone();
        renamed.path.clone_from(new_path);
        if let Some(indexed) = indexed {
            copy_track_details(&mut renamed, indexed);
        }
        changes.upserted.push(Arc::new(renamed));
        // A path another file was renamed to is replaced, not removed.
        if !new_paths.contains(old_path) {
            changes.removed.push(old_path.clone());
        }
        state
            .library_rows_to_read
            .extend([old_path.clone(), new_path.clone()]);
    }
    if changes.upserted.is_empty() && changes.removed.is_empty() {
        return false;
    }
    state.apply_library_changes(&changes).changed
}

fn apply_renamed_queue_paths(
    state: &mut BridgeState,
    rename_map: &PathRenames,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
    metadata: &MetadataService,
) -> bool {
    let renamed: Vec<(usize, PathBuf)> = state
        .queue
        .iter_mut()
        .enumerate()
        .filter_map(|(index, path)| remap_path(path, rename_map).then(|| (index, path.clone())))
        .collect();
    let mut changed = !renamed.is_empty();
    if changed {
        let queue_len = state.queue.len();
        state
            .queue_sync
            .record(QueueEdit::SetPaths(renamed), queue_len);
    }

    if let Some(current_path) = state.playback.current.as_mut() {
        if remap_path(current_path, rename_map) {
            metadata.request(current_path.clone());
            changed = true;
        }
    }
    // A rename keeps the current track's identity.
    if let Some(path) = state.last_track_change.path.as_mut() {
        remap_path(path, rename_map);
    }
    if let Some(pending) = state.pending_waveform_track.as_mut() {
        remap_path(&mut pending.path, rename_map);
    }
    for (new_path, details) in
        take_renamed_entries(Arc::make_mut(&mut state.queue_details), rename_map)
    {
        changed = true;
        if !state.library_index.contains(&state.library, &new_path) {
            let details = indexed_by_path.get(&new_path).cloned().unwrap_or(details);
            Arc::make_mut(&mut state.queue_details).insert(new_path, details);
        }
    }
    let moved = take_renamed_entries(&mut state.queue_detail_fingerprints, rename_map);
    state.queue_detail_fingerprints.extend(moved);
    changed
}

/// Remove the entries of renamed paths from `map` and return them under their
/// new paths. Every entry is removed before any is reinserted, so an entry is
/// never overwritten by another file moving to its old path.
fn take_renamed_entries<V>(
    map: &mut HashMap<PathBuf, V>,
    rename_map: &PathRenames,
) -> Vec<(PathBuf, V)> {
    rename_map
        .iter()
        .filter_map(|(old_path, new_path)| {
            map.remove(old_path).map(|value| (new_path.clone(), value))
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::{LibraryRoot, LibrarySnapshot, LibraryTrack};
    use crate::playback::TrackChangeKind;
    use std::io::Write;
    use std::time::Duration;

    use super::super::events::{
        apply_library_events, process_playback_event, pump_metadata_events, pump_playback_events,
    };
    use crate::library::load_external_track_caches;

    /// Validate the queue's details as the validation worker does.
    fn validate_now(state: &mut BridgeState) -> Vec<(PathBuf, TrackFileFingerprint)> {
        validate_queue_details(state, &|| false, load_external_track_caches)
    }

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
            "ferrous-frontend-{name}-{}-{nanos}",
            std::process::id()
        ));
        path
    }

    fn write_stub(path: &Path, bytes: &[u8]) {
        fs::File::create(path)
            .and_then(|mut file| file.write_all(bytes))
            .expect("write stub file");
    }

    #[test]
    fn playlist_cycles_are_skipped_but_repeated_playlists_preserve_duplicates() {
        let root = test_dir("playlist-cycles");
        fs::create_dir_all(&root).expect("fixture directory");
        write_stub(&root.join("song.flac"), b"fixture");
        write_stub(&root.join("a.m3u"), b"b.m3u\nsong.flac\n");
        write_stub(&root.join("b.m3u"), b"a.m3u\n");
        let outcome = expand_import_paths(vec![root.join("a.m3u"), root.join("a.m3u")]);
        assert_eq!(outcome.tracks, vec![root.join("song.flac"); 2]);
        assert_eq!(outcome.invalid_playlist_count, 2);
        assert!(format_import_warning("open", &outcome)
            .expect("cycle warning")
            .contains("cyclic"));
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn cancelled_import_does_not_inspect_paths() {
        let outcome = expand_import_paths_cancellable(vec![p("/missing/fixture.m3u")], &|| true);
        assert_eq!(outcome, ImportExpandOutcome::default());
    }

    #[test]
    fn playlist_nesting_is_bounded() {
        let root = test_dir("playlist-depth");
        fs::create_dir_all(&root).expect("fixture directory");
        for index in 0..66 {
            write_stub(
                &root.join(format!("{index}.m3u")),
                format!("{}.m3u\n", index + 1).as_bytes(),
            );
        }
        let outcome = expand_import_paths(vec![root.join("0.m3u")]);
        assert_eq!(outcome.invalid_playlist_count, 1);
        assert!(outcome.tracks.is_empty());
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn queue_sync_schedules_validation_without_file_or_database_work() {
        let mut state = BridgeState::default();
        state.queue = vec![p("/unavailable/queue-track.flac")];
        sync_queue_details(&mut state);
        assert!(state.queue_detail_validation_requested);
        assert_eq!(state.queue_detail_generation, 1);
    }
    #[test]
    fn show_track_in_library_expands_ancestors_and_selects_track() {
        let root_path = p("/music");
        let track_path = p("/music/Artist/Album/Disc 1/01.flac");
        let mut state = BridgeState {
            library: Arc::new(LibrarySnapshot {
                roots: vec![LibraryRoot {
                    path: root_path.clone(),
                    name: String::new(),
                }],
                tracks: crate::library::shared_tracks(vec![LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: track_path.clone(),
                    root_path,
                    artist: "Artist".to_string(),
                    ..LibraryTrack::default()
                }]),
                ..LibrarySnapshot::default()
            }),
            ..BridgeState::default()
        };

        assert!(reveal_track_in_library(&mut state, &track_path));
        assert!(state.expanded_keys.contains("artist|/music|Artist"));
        assert!(state.expanded_keys.contains("album|/music|Artist|Album"));
        assert!(state
            .expanded_keys
            .contains("section|/music|Artist|Album|Disc 1"));
        assert_eq!(
            state.library_view_selection_key,
            "track|/music/Artist/Album/Disc 1/01.flac"
        );
        assert_eq!(state.library_reveal_generation, 1);
    }

    #[test]
    fn show_external_track_in_library_is_a_no_op() {
        let mut state = BridgeState::default();

        assert!(!reveal_track_in_library(
            &mut state,
            Path::new("/external/song.flac")
        ));
        assert!(state.expanded_keys.is_empty());
        assert!(state.library_view_selection_key.is_empty());
        assert_eq!(state.library_reveal_generation, 0);
    }

    fn library_track(
        path: &str,
        root: &PathBuf,
        artist: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
    ) -> LibraryTrack {
        LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: p(path),
            root_path: root.clone(),
            title: String::new(),
            artist: artist.to_string(),
            album: album.to_string(),
            cover_path: String::new(),
            genre: String::new(),
            year,
            track_no,
            duration_secs: None,
        }
    }

    fn library_root(path: &PathBuf) -> LibraryRoot {
        LibraryRoot {
            path: path.clone(),
            name: String::new(),
        }
    }

    #[test]
    fn expand_import_paths_preserves_playlist_order_and_tracks_missing_entries() {
        let root = test_dir("playlist-import");
        fs::create_dir_all(&root).expect("mkdir root");
        let track_a = root.join("b-song.flac");
        let track_b = root.join("a-song.mp3");
        let playlist = root.join("mix.m3u8");
        write_stub(&track_a, b"a");
        write_stub(&track_b, b"b");
        fs::write(
            &playlist,
            b"#EXTM3U\nb-song.flac\nmissing.flac\na-song.mp3\n",
        )
        .expect("write playlist");

        let outcome = expand_import_paths(vec![playlist.clone()]);
        assert_eq!(outcome.tracks, vec![track_a, track_b]);
        assert_eq!(outcome.missing_count, 1);
        assert_eq!(outcome.unsupported_count, 0);
        assert_eq!(outcome.unreadable_count, 0);
    }

    #[test]
    fn expand_import_paths_sorts_folder_tracks_and_skips_nested_playlists() {
        let root = test_dir("folder-import");
        let nested = root.join("Disc 1");
        fs::create_dir_all(&nested).expect("mkdir nested");
        let track_z = nested.join("02-zeta.flac");
        let track_a = nested.join("01-alpha.mp3");
        let playlist = nested.join("ignored.m3u");
        write_stub(&track_z, b"z");
        write_stub(&track_a, b"a");
        fs::write(&playlist, b"02-zeta.flac\n").expect("write nested playlist");

        let outcome = expand_import_paths(vec![root.clone()]);
        assert_eq!(outcome.tracks, vec![track_a, track_z]);
        assert_eq!(outcome.skipped_count(), 0);
    }

    #[test]
    fn expand_import_paths_skips_apple_double_sidecars_but_keeps_dot_underscore_tracks() {
        let root = test_dir("apple-double-import");
        fs::create_dir_all(&root).expect("mkdir");
        let track = root.join("01.mp3");
        let sidecar = root.join("._01.mp3");
        let live = root.join("._live.mp3");
        write_stub(&track, crate::tag_write::TEST_MP3);
        crate::library::write_test_apple_double(&sidecar);
        write_stub(&live, crate::tag_write::TEST_MP3);
        let playlist = root.join("list.m3u");
        fs::write(&playlist, b"._01.mp3\n._live.mp3\n01.mp3\n").expect("write playlist");

        let folder = expand_import_paths(vec![root.clone()]);
        assert_eq!(folder.tracks, [live.clone(), track.clone()]);
        assert_eq!(folder.skipped_count(), 0);

        let listed = expand_import_paths(vec![playlist, sidecar]);
        assert_eq!(listed.tracks, [live, track]);
        assert_eq!(listed.unsupported_count, 2);
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn expand_import_paths_preserves_playlist_order_and_sorts_folder_tracks() {
        let root = test_dir("import-expand");
        let folder = root.join("folder");
        fs::create_dir_all(&folder).expect("mkdir folder");
        let a = folder.join("a.flac");
        let b = folder.join("b.flac");
        let text = folder.join("notes.txt");
        let nested_playlist = folder.join("nested.m3u8");
        write_stub(&a, b"not-real-audio");
        write_stub(&b, b"not-real-audio");
        write_stub(&text, b"ignore");
        write_stub(&nested_playlist, b"#EXTM3U\n");

        let playlist = root.join("mix.m3u8");
        let playlist_text = [
            "#EXTM3U",
            "folder/b.flac",
            "folder/a.flac",
            "http://example.com/skip.mp3",
            "missing.flac",
        ]
        .join("\n");
        fs::write(&playlist, playlist_text).expect("write playlist");

        let outcome = expand_import_paths(vec![playlist.clone(), folder.clone()]);
        assert_eq!(
            outcome.tracks,
            vec![b, a, folder.join("a.flac"), folder.join("b.flac")]
        );
        assert_eq!(outcome.non_local_url_count, 1);
        assert_eq!(outcome.missing_count, 1);
        assert_eq!(outcome.unsupported_count, 0);
        assert_eq!(outcome.unreadable_count, 0);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn expand_import_paths_decodes_latin1_m3u() {
        let root = test_dir("latin1-playlist");
        fs::create_dir_all(&root).expect("mkdir root");
        let track_ascii = root.join("01 - Svefnsund.flac");
        let track_latin = root.join("02 - Grasi vaxin göng.flac");
        write_stub(&track_ascii, b"a");
        write_stub(&track_latin, b"b");

        // Write an M3U file in ISO-8859-1 encoding where ö = 0xF6.
        let playlist = root.join("test.m3u");
        let mut m3u_bytes = b"#EXTM3U\r\n01 - Svefnsund.flac\r\n".to_vec();
        m3u_bytes.extend_from_slice(b"02 - Grasi vaxin g");
        m3u_bytes.push(0xF6); // ö in ISO-8859-1
        m3u_bytes.extend_from_slice(b"ng.flac\r\n");
        fs::write(&playlist, &m3u_bytes).expect("write playlist");

        let outcome = expand_import_paths(vec![playlist]);
        assert_eq!(outcome.tracks, vec![track_ascii, track_latin]);
        assert_eq!(outcome.missing_count, 0);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn validation_reads_external_tracks_and_prunes_removed_entries() {
        let root = test_dir("queue-details");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"not-real-audio");

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];
        let reads = validate_now(&mut state);
        assert_eq!(
            reads.iter().map(|(path, _)| path).collect::<Vec<_>>(),
            vec![&track]
        );
        // The worker reads the file and keeps what it found.
        Arc::make_mut(&mut state.queue_details).insert(
            track.clone(),
            IndexedTrack {
                search_tags: crate::library::SearchTags::default(),
                title: "song".to_string(),
                artist: String::new(),
                album: String::new(),
                cover_path: String::new(),
                genre: String::new(),
                year: None,
                track_no: None,
                duration_secs: None,
            },
        );
        state
            .queue_detail_fingerprints
            .insert(track.clone(), reads[0].1);
        assert!(validate_now(&mut state).is_empty());
        assert!(state.queue_details.contains_key(&track));

        state.queue.clear();
        assert!(validate_now(&mut state).is_empty());
        assert!(state.queue_details.is_empty());

        let _ = fs::remove_dir_all(root);
    }
    #[test]
    fn validation_skips_library_tracks_in_queue() {
        let track = p("/library/song.flac");
        let mut state = BridgeState::default();
        state.library = Arc::new(LibrarySnapshot {
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                search_tags: crate::library::SearchTags::default(),
                path: track.clone(),
                ..LibraryTrack::default()
            }]),
            ..LibrarySnapshot::default()
        });
        state.queue = vec![track.clone()];

        assert!(validate_now(&mut state).is_empty());
        assert!(!state.queue_details.contains_key(&track));
    }
    #[test]
    fn apply_session_restore_does_not_eagerly_populate_queue_details() {
        use super::super::config::{apply_session_restore, SessionSnapshot};
        use crate::playback::PlaybackEngine;

        let (analysis_tx, _) = crossbeam_channel::unbounded();

        let (playback, _playback_rx) = PlaybackEngine::new(analysis_tx);
        let root = test_dir("session-restore-queue-details");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"not-real-audio");

        let mut state = BridgeState::default();
        let session = SessionSnapshot {
            queue: vec![track.clone()],
            selected_queue_index: Some(0),
            current_queue_index: Some(0),
            current_path: Some(track.clone()),
            ..SessionSnapshot::default()
        };

        apply_session_restore(&mut state, &playback, Some(&session));

        assert_eq!(state.queue, vec![track]);
        assert!(state.queue_details.is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn library_changes_request_queue_details_for_restored_external_tracks() {
        use crate::library::LibraryEvent;

        let root = test_dir("restored-external-queue-details");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"not-real-audio");

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];

        let library = LibrarySnapshot {
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                path: root.join("other.flac"),
                ..LibraryTrack::default()
            }]),
            ..LibrarySnapshot::default()
        };
        let applied = apply_library_events([LibraryEvent::with_library(library)], &mut state);

        assert!(applied.rows_changed > 0);
        assert!(state.queue_detail_validation_requested);
        assert!(state.queue_details.is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn sync_queue_details_invalidates_stale_external_metadata_when_file_changes() {
        let root = test_dir("queue-details-stale");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"version-a");
        let old_fingerprint = track_file_fingerprint(&track).expect("initial fingerprint");
        write_stub(&track, b"version-b-with-new-size");

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];
        Arc::make_mut(&mut state.queue_details).insert(
            track.clone(),
            IndexedTrack {
                search_tags: crate::library::SearchTags::default(),
                title: "Old".to_string(),
                artist: String::new(),
                album: String::new(),
                cover_path: String::new(),
                genre: String::new(),
                year: None,
                track_no: None,
                duration_secs: None,
            },
        );
        state
            .queue_detail_fingerprints
            .insert(track.clone(), old_fingerprint);

        let reads = validate_now(&mut state);
        assert!(!state.queue_details.contains_key(&track));
        assert_eq!(reads.len(), 1);
        assert_eq!(reads[0].0, track);
        assert_ne!(reads[0].1, old_fingerprint);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn collect_artist_paths_for_queue_respects_year_sort_mode() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![
                library_track(
                    "/music/Artist/Alpha/01 - One.flac",
                    &root,
                    "Artist",
                    "Alpha",
                    Some(2020),
                    Some(1),
                ),
                library_track(
                    "/music/Artist/Beta/01 - One.flac",
                    &root,
                    "Artist",
                    "Beta",
                    Some(2010),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let ordered = collect_artist_paths_for_queue(&library, "Artist", LibrarySortMode::Year);
        assert_eq!(
            ordered,
            vec![
                p("/music/Artist/Beta/01 - One.flac"),
                p("/music/Artist/Alpha/01 - One.flac"),
            ]
        );
    }

    #[test]
    fn collect_artist_paths_for_queue_treats_mixed_year_album_as_unknown() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![
                library_track(
                    "/music/Artist/Alpha/01 - One.flac",
                    &root,
                    "Artist",
                    "Alpha",
                    Some(1998),
                    Some(1),
                ),
                library_track(
                    "/music/Artist/Alpha/02 - Two.flac",
                    &root,
                    "Artist",
                    "Alpha",
                    Some(2001),
                    Some(2),
                ),
                library_track(
                    "/music/Artist/Beta/01 - One.flac",
                    &root,
                    "Artist",
                    "Beta",
                    Some(2010),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let ordered = collect_artist_paths_for_queue(&library, "Artist", LibrarySortMode::Year);
        assert_eq!(
            ordered,
            vec![
                p("/music/Artist/Beta/01 - One.flac"),
                p("/music/Artist/Alpha/01 - One.flac"),
                p("/music/Artist/Alpha/02 - Two.flac"),
            ]
        );
    }

    #[test]
    fn collect_artist_paths_for_queue_respects_title_sort_mode() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![
                library_track(
                    "/music/Artist/Alpha/01 - One.flac",
                    &root,
                    "Artist",
                    "Alpha",
                    Some(2020),
                    Some(1),
                ),
                library_track(
                    "/music/Artist/Beta/01 - One.flac",
                    &root,
                    "Artist",
                    "Beta",
                    Some(2010),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let ordered = collect_artist_paths_for_queue(&library, "Artist", LibrarySortMode::Title);
        assert_eq!(
            ordered,
            vec![
                p("/music/Artist/Alpha/01 - One.flac"),
                p("/music/Artist/Beta/01 - One.flac"),
            ]
        );
    }

    #[test]
    fn collect_artist_paths_for_queue_uses_tree_artist_not_track_artist_tag() {
        let root = p("/music");
        let sampler = library_track(
            "/music/Porcupine Tree/Muut/Porcupine Tree Sampler 2005/01 - Hello.flac",
            &root,
            "Blackfield",
            "Porcupine Tree Sampler 2005",
            Some(2005),
            Some(1),
        );
        let blackfield = library_track(
            "/music/Blackfield/Blackfield/01 - Open Mind.flac",
            &root,
            "Blackfield",
            "Blackfield",
            Some(2004),
            Some(1),
        );
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![sampler, blackfield]),
            ..LibrarySnapshot::default()
        };

        let ordered =
            collect_artist_paths_for_queue(&library, "Blackfield", LibrarySortMode::Title);
        assert_eq!(
            ordered,
            vec![p("/music/Blackfield/Blackfield/01 - Open Mind.flac")]
        );
    }

    #[test]
    fn collect_artist_paths_for_queue_honors_artist_key_scope() {
        let root_a = p("/music-a");
        let root_b = p("/music-b");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root_a), library_root(&root_b)],
            tracks: crate::library::shared_tracks(vec![
                library_track(
                    "/music-a/Same Artist/Alpha/01 - One.flac",
                    &root_a,
                    "Same Artist",
                    "Alpha",
                    Some(2020),
                    Some(1),
                ),
                library_track(
                    "/music-b/Same Artist/Beta/01 - Two.flac",
                    &root_b,
                    "Same Artist",
                    "Beta",
                    Some(2021),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let ordered = collect_artist_paths_for_queue(
            &library,
            "artist|/music-a|Same Artist",
            LibrarySortMode::Title,
        );
        assert_eq!(ordered, vec![p("/music-a/Same Artist/Alpha/01 - One.flac")]);
    }

    #[test]
    fn collect_album_paths_for_queue_honors_album_key_scope() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![
                // Root-level album track — always included.
                library_track(
                    "/music/Porcupine Tree/Muut/01 - Intro.flac",
                    &root,
                    "Porcupine Tree",
                    "Muut",
                    Some(2005),
                    Some(1),
                ),
                // Track in a non-disc subfolder — excluded from album queue.
                library_track(
                    "/music/Porcupine Tree/Muut/Porcupine Tree Sampler 2005/01 - Hello.flac",
                    &root,
                    "Blackfield",
                    "Porcupine Tree Sampler 2005",
                    Some(2005),
                    Some(1),
                ),
                // Track in a disc subfolder — included in album queue.
                library_track(
                    "/music/Porcupine Tree/Muut/Disc 2/01 - Bonus.flac",
                    &root,
                    "Porcupine Tree",
                    "Muut",
                    Some(2005),
                    Some(1),
                ),
                // Track from a different artist — never included.
                library_track(
                    "/music/Blackfield/Blackfield/01 - Open Mind.flac",
                    &root,
                    "Blackfield",
                    "Blackfield",
                    Some(2004),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let ordered = collect_album_paths_for_queue(
            &library,
            "artist|/music|Porcupine Tree",
            "album|/music|Porcupine Tree|Muut",
        );
        assert_eq!(
            ordered,
            vec![
                p("/music/Porcupine Tree/Muut/01 - Intro.flac"),
                p("/music/Porcupine Tree/Muut/Disc 2/01 - Bonus.flac"),
            ]
        );
    }

    /// How albums were collected before the match borrowed from the tracks:
    /// through each track's full tree context.
    fn album_paths_through_tree_contexts(
        library: &LibrarySnapshot,
        artist: &str,
        album: &str,
    ) -> Vec<PathBuf> {
        use super::super::search::derive_tree_path_context;
        let (artist, album) = (artist.trim(), album.trim());
        if album.is_empty() {
            return Vec::new();
        }
        library
            .tracks
            .iter()
            .filter(|track| {
                let context = derive_tree_path_context(&track.path, &library.roots, &track.artist);
                let in_album = |ctx: &super::super::search::TreePathContext| {
                    ctx.is_main_level_album_track || ctx.is_disc_section_album_track
                };
                if album.starts_with("album|") {
                    return context.as_ref().is_some_and(|ctx| {
                        ctx.album_key.as_deref() == Some(album) && in_album(ctx)
                    });
                }
                let artist_matches = if artist.starts_with("artist|") {
                    context.as_ref().is_some_and(|ctx| ctx.artist_key == artist)
                } else if let Some(ctx) = context.as_ref() {
                    ctx.artist_name == artist
                } else {
                    normalized_library_artist(track) == artist
                };
                if !artist_matches {
                    return false;
                }
                match context {
                    Some(ctx) => {
                        ctx.album_folder.as_deref().unwrap_or("Unknown Album") == album
                            && in_album(&ctx)
                    }
                    None => normalized_library_album(track) == album,
                }
            })
            .map(|track| track.path.clone())
            .collect()
    }

    /// Tracks at every depth below two roots, one inside the other, and
    /// outside them.
    fn album_layout_library() -> LibrarySnapshot {
        let (outer, inner) = (p("/music"), p("/music/Live"));
        let tracks = [
            ("/music/A/Album/01.flac", "A", "Album"),
            ("/music/A/Album/Disc 1/01.flac", "A", "Album"),
            ("/music/A/Album/CD2/01.flac", "A", "Album"),
            ("/music/A/Album/Bonus/01.flac", "A", "Album"),
            ("/music/A/Album/Disc 1/Extra/01.flac", "A", "Album"),
            ("/music/A/Other/01.flac", "A", "Other"),
            ("/music/A/loose.flac", "A", ""),
            ("/music/single.flac", "", "Unknown Album"),
            ("/music/tagged.flac", " Tagged ", "Album"),
            ("/music/B|C/Al|bum/01.flac", "B", "Al|bum"),
            ("/music/Live/A/Album/01.flac", "A", "Album"),
            ("/music/Live/A/Album/Disc 3/01.flac", "A", "Album"),
            ("/elsewhere/A/Album/01.flac", "A", "Album"),
            ("/elsewhere/loose.flac", "", ""),
        ];
        LibrarySnapshot {
            roots: vec![library_root(&outer), library_root(&inner)],
            tracks: tracks
                .iter()
                .map(|(path, artist, album)| library_track(path, &outer, artist, album, None, None))
                .map(std::sync::Arc::new)
                .collect(),
            ..LibrarySnapshot::default()
        }
    }

    #[test]
    fn collect_album_paths_for_queue_matches_the_tree_contexts() {
        let library = album_layout_library();
        let selections = [
            ("artist|/music|A", "album|/music|A|Album"),
            ("", "album|/music/Live|A|Album"),
            ("", "album|/music|B|C|Al|bum"),
            ("", "album|/music|B|C|Al"),
            ("", "album|/music|A"),
            ("", "album|/music|A|Album|"),
            ("artist|/music|A", "Album"),
            ("artist|/music/Live|A", "Album"),
            ("artist|/music|A|", "Album"),
            ("A", "Album"),
            ("A", "Other"),
            ("A", "Unknown Album"),
            ("Unknown Artist", "Unknown Album"),
            ("Tagged", "Unknown Album"),
            ("Unknown Artist", "Album"),
            ("B|C", "Al|bum"),
            ("A", "  "),
        ];
        for (artist, album) in selections {
            assert_eq!(
                collect_album_paths_for_queue(&library, artist, album),
                album_paths_through_tree_contexts(&library, artist, album),
                "{artist:?} / {album:?}"
            );
        }
        assert_eq!(
            collect_album_paths_for_queue(&library, "artist|/music|A", "album|/music|A|Album"),
            vec![
                p("/music/A/Album/01.flac"),
                p("/music/A/Album/Disc 1/01.flac"),
                p("/music/A/Album/CD2/01.flac"),
            ]
        );
    }

    #[test]
    fn queueing_an_album_does_not_allocate_per_library_track() {
        let root = p("/music");
        let mut tracks = Vec::new();
        for artist in 0..50 {
            for album in 0..4 {
                for number in 0..10 {
                    tracks.push(library_track(
                        &format!("/music/Artist {artist}/Album {album}/{number:02}.flac"),
                        &root,
                        &format!("Artist {artist}"),
                        &format!("Album {album}"),
                        None,
                        Some(number),
                    ));
                }
            }
        }
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(tracks),
            ..LibrarySnapshot::default()
        };

        for (artist, album) in [
            ("artist|/music|Artist 7", "album|/music|Artist 7|Album 2"),
            ("Artist 7", "Album 2"),
        ] {
            let before = crate::test_alloc::allocations();
            let paths = collect_album_paths_for_queue(&library, artist, album);
            let allocations = crate::test_alloc::allocations() - before;
            assert_eq!(paths.len(), 10);
            // The paths of the album's tracks, the vector that holds them, and
            // the root's key; nothing for the other 1,990 tracks.
            assert!(
                allocations <= paths.len() + 8,
                "{allocations} allocations for {artist:?} / {album:?}"
            );
        }
    }

    #[test]
    fn collect_album_paths_for_queue_excludes_bonus_by_name_selector() {
        // Test the name-based (non-key) album selector path: when both
        // artist and album selectors are plain names instead of keys,
        // the filter should still exclude non-disc sections.
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![
                // Root-level album track — included.
                library_track(
                    "/music/Opeth/Blackwater Park/01 - The Leper Affinity.flac",
                    &root,
                    "Opeth",
                    "Blackwater Park",
                    Some(2001),
                    Some(1),
                ),
                // Disc section track — included.
                library_track(
                    "/music/Opeth/Blackwater Park/CD2/01 - Patterns.flac",
                    &root,
                    "Opeth",
                    "Blackwater Park",
                    Some(2001),
                    Some(1),
                ),
                // Non-disc section track — excluded.
                library_track(
                    "/music/Opeth/Blackwater Park/Deluxe Bonus/01 - Still Day.flac",
                    &root,
                    "Opeth",
                    "Deluxe Bonus",
                    Some(2001),
                    Some(1),
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        // Use plain name selectors (not key-based).
        let ordered = collect_album_paths_for_queue(&library, "Opeth", "Blackwater Park");
        assert_eq!(
            ordered,
            vec![
                p("/music/Opeth/Blackwater Park/01 - The Leper Affinity.flac"),
                p("/music/Opeth/Blackwater Park/CD2/01 - Patterns.flac"),
            ]
        );
    }

    #[test]
    fn stopped_track_change_defers_waveform_load_until_playback_resumes() {
        use crate::analysis::AnalysisEngine;
        use crate::playback::{PlaybackEvent, PlaybackState};

        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();

        let mut state = BridgeState::default();
        state.playback.state = PlaybackState::Stopped;
        let path = p("/music/deferred.flac");

        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: path.clone(),
                queue_index: 0,
                kind: TrackChangeKind::Manual,
                track_token: 1,
            })
            .expect("send track-changed while stopped");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(changed);
        assert!(state.pending_waveform_track.is_some());
        assert!(analysis_rx
            .recv_timeout(Duration::from_millis(120))
            .is_err());

        let mut snapshot = state.playback.clone();
        snapshot.state = PlaybackState::Playing;
        snapshot.current = Some(path.clone());
        snapshot.current_queue_index = Some(0);
        playback_tx
            .send(PlaybackEvent::Snapshot(snapshot))
            .expect("send resumed snapshot");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(changed);
        assert!(state.pending_waveform_track.is_none());

        let evt = analysis_rx
            .recv_timeout(Duration::from_millis(500))
            .expect("analysis event after resume");
        match evt {
            crate::analysis::AnalysisEvent::Snapshot(_) => {}
            _ => panic!("unexpected event variant"),
        }
        // The file does not exist, so the spectrogram reports that it cannot
        // read it; no further waveform snapshot follows.
        let later: Vec<_> =
            std::iter::from_fn(|| analysis_rx.recv_timeout(Duration::from_millis(120)).ok())
                .collect();
        assert!(
            !later
                .iter()
                .any(|event| matches!(event, crate::analysis::AnalysisEvent::Snapshot(_))),
            "{later:?}"
        );
    }

    #[test]
    fn stopped_replay_restarts_spectrogram_on_resume() {
        use crate::analysis::AnalysisEngine;
        use crate::playback::{PlaybackEvent, PlaybackState};

        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();

        let path = p("/music/replay.flac");
        let mut state = BridgeState::default();
        state.playback.state = PlaybackState::Stopped;
        state.playback.current = Some(path.clone());
        state.playback.position = Duration::ZERO;
        state.analysis_track_token = 7;

        let mut snapshot = state.playback.clone();
        snapshot.state = PlaybackState::Playing;
        snapshot.current = Some(path);
        snapshot.position = Duration::ZERO;
        playback_tx
            .send(PlaybackEvent::Snapshot(snapshot))
            .expect("send replay snapshot");

        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(changed);
        assert!(state.pending_waveform_track.is_none());

        let evt = analysis_rx
            .recv_timeout(Duration::from_millis(500))
            .expect("analysis event after replay resume");
        match evt {
            crate::analysis::AnalysisEvent::Snapshot(_snapshot) => {}
            _ => panic!("unexpected event variant"),
        }
    }

    #[test]
    fn track_change_does_not_swap_metadata_until_metadata_event_arrives() {
        use crate::analysis::AnalysisEngine;
        use crate::metadata::{MetadataEvent, TrackMetadata};
        use crate::playback::PlaybackEvent;

        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata_service, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();
        let (metadata_tx, metadata_rx) = crossbeam_channel::unbounded::<MetadataEvent>();

        let mut state = BridgeState::default();
        state.metadata.title = "Old Title".to_string();
        state.metadata.artist = "Old Artist".to_string();
        state.metadata.album = "Old Album".to_string();

        playback_tx
            .send(PlaybackEvent::TrackChanged {
                path: p("/music/new.flac"),
                queue_index: 1,
                kind: TrackChangeKind::Natural,
                track_token: 1,
            })
            .expect("send track-changed event");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata_service, &mut state);
        assert!(changed);
        assert_eq!(state.metadata.title, "Old Title");
        assert_eq!(state.metadata.artist, "Old Artist");
        assert_eq!(state.metadata.album, "Old Album");

        metadata_tx
            .send(MetadataEvent::Loaded(TrackMetadata {
                title: "New Title".to_string(),
                artist: "New Artist".to_string(),
                album: "New Album".to_string(),
                ..TrackMetadata::default()
            }))
            .expect("send metadata event");
        let changed = pump_metadata_events(&metadata_rx, &mut state);
        assert!(changed);
        assert_eq!(state.metadata.title, "New Title");
        assert_eq!(state.metadata.artist, "New Artist");
        assert_eq!(state.metadata.album, "New Album");
    }
    #[test]
    fn renames_reach_the_engine_and_its_stale_events_keep_the_new_paths() {
        use crate::analysis::AnalysisEngine;
        use crate::playback::{PlaybackEvent, PlaybackState};

        let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded();
        let analysis = AnalysisEngine::from_command_sender(analysis_tx);
        let (metadata, _metadata_rx) = MetadataService::new();
        let (engine_tx, engine_rx) = crossbeam_channel::unbounded();
        let playback = PlaybackEngine::from_command_sender(engine_tx);
        let (event_tx, event_rx) = crossbeam_channel::unbounded();
        let (old_current, new_current) = (p("/music/old current.flac"), p("/music/current.flac"));
        let (old_next, new_next) = (p("/music/old next.flac"), p("/music/next.flac"));
        let mut state = BridgeState {
            queue: vec![
                p("/music/first.flac"),
                old_current.clone(),
                old_next.clone(),
            ],
            playback: crate::playback::PlaybackSnapshot {
                state: PlaybackState::Playing,
                current: Some(old_current.clone()),
                current_queue_index: Some(1),
                ..crate::playback::PlaybackSnapshot::default()
            },
            ..BridgeState::default()
        };
        state.last_track_change.path = Some(old_current.clone());
        let stale = state.playback.clone();

        let renames = PathRenames::from([
            (old_current.clone(), new_current.clone()),
            (old_next.clone(), new_next.clone()),
        ]);
        assert!(refresh_renamed_paths(
            &mut state,
            Arc::new(renames.clone()),
            RenamedDetails::default(),
            &playback,
            &analysis,
            &metadata,
            &event_tx,
        ));
        assert_eq!(
            state.queue,
            vec![
                p("/music/first.flac"),
                new_current.clone(),
                new_next.clone()
            ]
        );
        assert_eq!(state.playback.current.as_ref(), Some(&new_current));
        assert_eq!(state.last_track_change.path.as_ref(), Some(&new_current));
        match engine_rx.try_recv() {
            Ok(PlaybackCommand::RemapPaths(sent)) => {
                assert_eq!(*sent, renames);
            }
            other => panic!("expected a path remap for the engine, got {other:?}"),
        }
        match analysis_rx.try_recv() {
            Ok(AnalysisCommand::RemapPaths(sent)) => assert_eq!(*sent, renames),
            other => panic!("expected a path remap for analysis, got {other:?}"),
        }

        // Events the engine sent before applying the renames carry old paths.
        let _ = process_playback_event(
            PlaybackEvent::Snapshot(stale.clone()),
            &analysis,
            &metadata,
            &event_tx,
            &mut state,
        );
        assert_eq!(state.playback.current.as_ref(), Some(&new_current));
        let _ = process_playback_event(
            PlaybackEvent::TrackChanged {
                path: old_next.clone(),
                queue_index: 2,
                kind: TrackChangeKind::Gapless,
                track_token: 3,
            },
            &analysis,
            &metadata,
            &event_tx,
            &mut state,
        );
        assert_eq!(state.last_track_change.path.as_ref(), Some(&new_next));
        // Analysis is told the successor's new path, after the renames.
        assert!(analysis_rx.try_iter().any(|command| matches!(
            command,
            AnalysisCommand::SetTrack { path, track_token: 3, .. } if path == new_next
        )));
        while event_rx.try_recv().is_ok() {}
        let _ = process_playback_event(
            PlaybackEvent::Error(crate::playback::PlaybackError {
                path: Some(old_next.clone()),
                detail: "Resource not found.".to_string(),
                outcome: crate::playback::PlaybackErrorOutcome::Skipped,
            }),
            &analysis,
            &metadata,
            &event_tx,
            &mut state,
        );
        match event_rx.try_recv() {
            Ok(BridgeEvent::Error(message)) => assert_eq!(
                message,
                "Couldn't play \u{201c}next.flac\u{201d} (Resource not found); skipped it."
            ),
            other => panic!("expected the playback error, got {other:?}"),
        }

        // After the acknowledgement, engine paths are taken as reported.
        let _ = process_playback_event(
            PlaybackEvent::PathsRemapped,
            &analysis,
            &metadata,
            &event_tx,
            &mut state,
        );
        assert!(state.unacknowledged_engine_renames.is_empty());
        let _ = process_playback_event(
            PlaybackEvent::Snapshot(stale),
            &analysis,
            &metadata,
            &event_tx,
            &mut state,
        );
        assert_eq!(state.playback.current.as_ref(), Some(&old_current));
    }

    #[test]
    fn a_rename_reports_the_files_it_could_not_read() {
        let (analysis_tx, _analysis_rx) = crossbeam_channel::unbounded();
        let analysis = AnalysisEngine::from_command_sender(analysis_tx);
        let (metadata, _metadata_rx) = MetadataService::new();
        let (engine_tx, _engine_rx) = crossbeam_channel::unbounded();
        let playback = PlaybackEngine::from_command_sender(engine_tx);
        let (event_tx, event_rx) = crossbeam_channel::unbounded();
        let (old, new) = (p("/music/old.flac"), p("/music/new.flac"));
        let mut state = BridgeState {
            queue: vec![old.clone()],
            ..BridgeState::default()
        };

        assert!(refresh_renamed_paths(
            &mut state,
            Arc::new(PathRenames::from([(old, new.clone())])),
            RenamedDetails {
                problem: Some("could not read the tags of 1 file".to_string()),
                ..RenamedDetails::default()
            },
            &playback,
            &analysis,
            &metadata,
            &event_tx,
        ));

        assert_eq!(state.queue, vec![new]);
        assert!(matches!(
            event_rx.try_recv(),
            Ok(BridgeEvent::Error(message)) if message == "could not read the tags of 1 file"
        ));
    }

    #[test]
    fn a_swap_moves_each_file_once() {
        let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded();
        let analysis = AnalysisEngine::from_command_sender(analysis_tx);
        let (metadata, _metadata_rx) = MetadataService::new();
        let (engine_tx, engine_rx) = crossbeam_channel::unbounded();
        let playback = PlaybackEngine::from_command_sender(engine_tx);
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();
        let (a, b, other) = (p("/music/a.flac"), p("/music/b.flac"), p("/music/c.flac"));
        let fingerprint = |size_bytes| crate::library::TrackFileFingerprint {
            mtime_ns: 1,
            size_bytes,
        };
        let mut state = BridgeState {
            queue: vec![a.clone(), b.clone(), other.clone()],
            queue_detail_fingerprints: HashMap::from([
                (a.clone(), fingerprint(1)),
                (b.clone(), fingerprint(2)),
            ]),
            ..BridgeState::default()
        };
        let swap = PathRenames::from([(a.clone(), b.clone()), (b.clone(), a.clone())]);
        assert!(refresh_renamed_paths(
            &mut state,
            Arc::new(swap.clone()),
            RenamedDetails::default(),
            &playback,
            &analysis,
            &metadata,
            &event_tx,
        ));
        assert_eq!(state.queue, vec![b.clone(), a.clone(), other]);
        assert_eq!(
            state.queue_detail_fingerprints,
            HashMap::from([(b.clone(), fingerprint(1)), (a.clone(), fingerprint(2))])
        );
        assert!(matches!(
            engine_rx.try_recv(),
            Ok(PlaybackCommand::RemapPaths(sent)) if *sent == swap
        ));
        assert!(engine_rx.try_recv().is_err(), "one remap for the whole map");
        assert!(matches!(
            analysis_rx.try_recv(),
            Ok(AnalysisCommand::RemapPaths(sent)) if *sent == swap
        ));
        assert!(analysis_rx.try_recv().is_err(), "one remap for analysis");
    }

    #[test]
    fn edits_and_renames_copy_the_library_only_for_a_track_they_change() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: (0..2000)
                .map(|index| {
                    library_track(
                        &format!("/music/Artist/Album/{index:04}.flac"),
                        &root,
                        "Artist",
                        "Album",
                        None,
                        None,
                    )
                })
                .map(std::sync::Arc::new)
                .collect(),
            ..LibrarySnapshot::default()
        };
        let mut state = BridgeState {
            library: Arc::new(library),
            ..BridgeState::default()
        };
        let details = |title: &str| IndexedTrack {
            search_tags: crate::library::SearchTags::default(),
            title: title.to_string(),
            artist: "Artist".to_string(),
            album: "Album".to_string(),
            cover_path: String::new(),
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        };
        let outside = p("/elsewhere/song.flac");
        let edits = HashMap::from([(outside.clone(), details("Outside"))]);
        let renames = PathRenames::from([(outside.clone(), p("/elsewhere/renamed.flac"))]);
        // The bridge finds tracks through the library's index, built once.
        assert!(!state.library_index.contains(&state.library, &outside));

        let before = crate::test_alloc::allocations();
        assert!(!update_library_track_details(&mut state, &edits));
        assert!(!apply_renamed_library_paths(
            &mut state,
            &renames,
            &RenamedDetails::default()
        ));
        let allocations = crate::test_alloc::allocations() - before;
        assert!(allocations < 16, "{allocations} allocations");

        let inside = p("/music/Artist/Album/1234.flac");
        let library = Arc::clone(&state.library);
        let edits = HashMap::from([(inside.clone(), details("Edited"))]);
        assert!(update_library_track_details(&mut state, &edits));
        assert!(!Arc::ptr_eq(&library, &state.library));
        assert_eq!(state.library.tracks[1234].title, "Edited");
        assert!(state
            .library
            .tracks
            .iter()
            .enumerate()
            .all(|(index, track)| index == 1234 || track.title.is_empty()));
        assert!(
            Arc::ptr_eq(&library.tracks[0], &state.library.tracks[0]),
            "the copy shares the tracks it keeps"
        );
        // The library worker reads the edited row again, in case a newer
        // change reached the bridge first.
        assert_eq!(state.library_rows_to_read, [inside.clone()]);

        state.library_rows_to_read.clear();
        let renamed = p("/music/Artist/Album/renamed.flac");
        let renames = PathRenames::from([(inside.clone(), renamed.clone())]);
        assert!(apply_renamed_library_paths(
            &mut state,
            &renames,
            &RenamedDetails::default()
        ));
        assert_eq!(state.library_rows_to_read, [inside, renamed]);
    }

    #[test]
    fn metadata_only_tag_edits_invalidate_search_snapshot() {
        let path = p("/music/Artist/Album/song.flac");
        let mut state = BridgeState::default();
        state.library = Arc::new(crate::library::LibrarySnapshot {
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                path: path.clone(),
                root_path: p("/music"),
                ..LibraryTrack::default()
            }]),
            ..crate::library::LibrarySnapshot::default()
        });
        let tags =
            crate::library::SearchTags::from([("comment".into(), vec!["Remastered".into()])]);
        let indexed = IndexedTrack {
            search_tags: tags.clone(),
            title: String::new(),
            artist: String::new(),
            album: String::new(),
            cover_path: String::new(),
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        };
        let updates = HashMap::from([(path, indexed)]);
        assert!(update_library_track_details(&mut state, &updates));
        assert_eq!(state.library.search_revision, 1);
        assert_eq!(state.library.tracks[0].search_tags, tags);
        assert!(!update_library_track_details(&mut state, &updates));
    }

    #[test]
    fn a_refresh_reports_a_file_whose_tags_cannot_be_read_and_keeps_its_details() {
        use crate::vorbis_comment_scan::fixtures::{comment_block, flac};

        let dir = test_dir("refresh-unreadable");
        fs::create_dir_all(&dir).expect("fixture directory");
        let dir = dir.canonicalize().expect("canonical fixture directory");
        let path = dir.join("song.flac");
        let tags: [&[u8]; 3] = [
            b"TITLE=Saved Title",
            b"ARTIST=Saved Artist",
            b"ALBUM=Saved Album",
        ];
        fs::write(&path, flac(&[&comment_block(&tags)])).expect("write track");
        crate::library::index_tracks_for_test(&dir, std::slice::from_ref(&path));
        // A `COVERART` picture too short for lofty, which panics on reading it.
        let broken: [&[u8]; 4] = [tags[0], tags[1], tags[2], b"COVERART=AAAA"];
        fs::write(&path, flac(&[&comment_block(&broken)])).expect("break track");
        let mut state = BridgeState {
            library: Arc::new(LibrarySnapshot {
                tracks: crate::library::shared_tracks(vec![LibraryTrack {
                    path: path.clone(),
                    root_path: dir.clone(),
                    title: "Saved Title".to_string(),
                    artist: "Saved Artist".to_string(),
                    album: "Saved Album".to_string(),
                    ..LibraryTrack::default()
                }]),
                ..LibrarySnapshot::default()
            }),
            ..BridgeState::default()
        };
        let (metadata, _metadata_rx) = MetadataService::new();
        let mut refreshes = super::super::edit_refresh::EditRefreshWorker::new();

        assert!(refreshes.request(vec![path.clone()]).is_some());
        let result = refreshes
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("refresh result");
        let refresh = refreshes.accept(result);
        apply_edited_details(
            &mut state,
            &refresh.indexed,
            &refresh.fingerprints,
            &metadata,
        );

        let message = refresh.problem.expect("the unreadable file is reported");
        assert!(
            message.contains("could not read the tags of 1 file")
                && message.contains(&path.display().to_string()),
            "{message}"
        );
        let track = &state.library.tracks[0];
        assert_eq!(
            (
                track.title.as_str(),
                track.artist.as_str(),
                track.album.as_str()
            ),
            ("Saved Title", "Saved Artist", "Saved Album")
        );
        fs::remove_dir_all(dir).expect("remove fixture");
    }
}
