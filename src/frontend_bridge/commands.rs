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
    is_supported_audio, load_external_track_caches, refresh_indexed_metadata_for_paths,
    track_file_fingerprint, IndexedTrack, LibraryCommand, LibraryService, LibraryTrack,
    TrackFileFingerprint,
};
use crate::metadata::MetadataService;
use crate::playback::{PlaybackCommand, PlaybackEngine};

use super::config::{load_settings_into, save_settings};
use super::queue::handle_queue_command;
use super::search::derive_tree_path_context;
use super::{
    library_tree, try_send_event, ApplyAlbumArtRequest, BridgeAnalysisCommand, BridgeCommand,
    BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeSettingsCommand, BridgeState,
    ExternalQueueDetailsRequest, LibrarySortMode, SearchWorkerQuery,
};

pub(super) struct BridgeCommandContext<'a> {
    pub(super) playback: &'a PlaybackEngine,
    pub(super) analysis: &'a AnalysisEngine,
    pub(super) metadata: &'a MetadataService,
    pub(super) library: &'a LibraryService,
    pub(super) lastfm: &'a LastFmHandle,
    pub(super) search_query_tx: &'a Sender<SearchWorkerQuery>,
    pub(super) external_queue_details_tx: &'a Sender<ExternalQueueDetailsRequest>,
    pub(super) apply_album_art_tx: &'a Sender<ApplyAlbumArtRequest>,
    pub(super) event_tx: &'a Sender<BridgeEvent>,
    pub(super) running: &'a mut bool,
    pub(super) settings_dirty: &'a mut bool,
}

struct LibraryCommandRuntime<'a> {
    playback: &'a PlaybackEngine,
    metadata: &'a MetadataService,
    library: &'a LibraryService,
    external_queue_details_tx: &'a Sender<ExternalQueueDetailsRequest>,
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
        BridgeCommand::Playback(cmd) => {
            handle_playback_bridge_command(&cmd, state, context);
            false
        }
        BridgeCommand::Queue(cmd) => handle_queue_command(
            cmd,
            state,
            context.playback,
            context.external_queue_details_tx,
            context.event_tx,
        ),
        BridgeCommand::Library(cmd) => handle_library_command(
            cmd,
            state,
            &LibraryCommandRuntime {
                playback: context.playback,
                metadata: context.metadata,
                library: context.library,
                external_queue_details_tx: context.external_queue_details_tx,
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

fn handle_settings_bridge_command(
    cmd: &BridgeSettingsCommand,
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
) {
    match cmd {
        BridgeSettingsCommand::LoadFromDisk => {
            handle_load_settings_from_disk(state, context);
        }
        BridgeSettingsCommand::SaveToDisk => {
            save_settings(&state.settings);
            *context.settings_dirty = false;
        }
        BridgeSettingsCommand::SetVolume(volume) => {
            let volume = volume.clamp(0.0, 1.0);
            state.settings.volume = volume;
            context.playback.command(PlaybackCommand::SetVolume(volume));
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetFftSize(size) => {
            let fft = (*size).clamp(512, 8192).next_power_of_two();
            state.settings.fft_size = fft;
            context.analysis.command(AnalysisCommand::SetFftSize(fft));
            *context.settings_dirty = true;
        }
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
        BridgeSettingsCommand::SetDbRange(value) => {
            state.settings.db_range = value.clamp(50.0, 150.0);
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetLogScale(enabled) => {
            state.settings.display.log_scale = *enabled;
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
    if let Some(outcome) = handle_library_view_command(cmd.clone(), state, runtime) {
        return outcome;
    }
    handle_library_collection_command(cmd, state, runtime).unwrap_or(false)
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
        BridgeLibraryCommand::SetSearchQuery { seq, query } => {
            let _ = runtime.search_query_tx.send(SearchWorkerQuery {
                seq,
                query: query.trim().to_string(),
                library: Arc::clone(&state.library),
            });
            Some(false)
        }
        _ => None,
    }
}

fn handle_library_collection_command(
    cmd: BridgeLibraryCommand,
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
) -> Option<bool> {
    match cmd {
        BridgeLibraryCommand::AddTrack(path) => Some(import_library_paths(
            state,
            runtime,
            "append",
            vec![path],
            false,
        )),
        BridgeLibraryCommand::PlayTrack(path) => Some(import_library_paths(
            state,
            runtime,
            "open",
            vec![path],
            true,
        )),
        BridgeLibraryCommand::ReplaceWithAlbum(paths) => {
            Some(import_library_paths(state, runtime, "open", paths, true))
        }
        BridgeLibraryCommand::AppendAlbum(paths) => {
            Some(import_library_paths(state, runtime, "append", paths, false))
        }
        BridgeLibraryCommand::ReplaceAlbumByKey { artist, album } => Some(queue_album_by_key(
            state,
            runtime,
            &artist,
            &album,
            QueueMode::Replace,
        )),
        BridgeLibraryCommand::AppendAlbumByKey { artist, album } => Some(queue_album_by_key(
            state,
            runtime,
            &artist,
            &album,
            QueueMode::Append,
        )),
        BridgeLibraryCommand::ReplaceArtistByKey { artist } => Some(queue_artist_by_key(
            state,
            runtime,
            &artist,
            QueueMode::Replace,
        )),
        BridgeLibraryCommand::AppendArtistByKey { artist } => Some(queue_artist_by_key(
            state,
            runtime,
            &artist,
            QueueMode::Append,
        )),
        BridgeLibraryCommand::ReplaceRootByPath { root } => Some(queue_root_by_path(
            state,
            runtime,
            &root,
            QueueMode::Replace,
        )),
        BridgeLibraryCommand::AppendRootByPath { root } => {
            Some(queue_root_by_path(state, runtime, &root, QueueMode::Append))
        }
        BridgeLibraryCommand::ReplaceAllTracks => {
            Some(queue_all_tracks(state, runtime, QueueMode::Replace))
        }
        BridgeLibraryCommand::AppendAllTracks => {
            Some(queue_all_tracks(state, runtime, QueueMode::Append))
        }
        BridgeLibraryCommand::RefreshEditedPaths(paths) => Some(refresh_edited_paths(
            state,
            &paths,
            runtime.metadata,
            runtime.external_queue_details_tx,
            runtime.event_tx,
        )),
        BridgeLibraryCommand::RefreshRenamedPaths(renames) => Some(refresh_renamed_paths(
            state,
            &renames,
            runtime.metadata,
            runtime.external_queue_details_tx,
            runtime.event_tx,
        )),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// Queue helpers
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
enum QueueMode {
    Append,
    Replace,
}

fn import_library_paths(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    action: &'static str,
    paths: Vec<PathBuf>,
    start_playback: bool,
) -> bool {
    handle_import_library_command(
        state,
        runtime.playback,
        runtime.external_queue_details_tx,
        runtime.event_tx,
        action,
        paths,
        start_playback,
    )
}

fn queue_album_by_key(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    artist: &str,
    album: &str,
    mode: QueueMode,
) -> bool {
    let paths = collect_album_paths_for_queue(&state.library, artist, album);
    queue_paths(state, runtime, paths, mode)
}

fn queue_artist_by_key(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    artist: &str,
    mode: QueueMode,
) -> bool {
    let paths =
        collect_artist_paths_for_queue(&state.library, artist, state.settings.library_sort_mode);
    queue_paths(state, runtime, paths, mode)
}

fn queue_root_by_path(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    root_path: &str,
    mode: QueueMode,
) -> bool {
    let paths = library_tree::collect_root_paths_tree_order(
        &state.library,
        root_path,
        state.settings.library_sort_mode,
    );
    queue_paths(state, runtime, paths, mode)
}

fn queue_all_tracks(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    mode: QueueMode,
) -> bool {
    let paths = library_tree::collect_all_paths_tree_order(
        &state.library,
        state.settings.library_sort_mode,
    );
    queue_paths(state, runtime, paths, mode)
}

fn queue_paths(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    paths: Vec<PathBuf>,
    mode: QueueMode,
) -> bool {
    match mode {
        QueueMode::Append => append_queue_paths(
            state,
            runtime.playback,
            runtime.external_queue_details_tx,
            paths,
        ),
        QueueMode::Replace => replace_queue_paths(
            state,
            runtime.playback,
            runtime.external_queue_details_tx,
            paths,
            true,
        ),
    }
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

    library
        .tracks
        .iter()
        .filter(|track| {
            let context = derive_tree_path_context(&track.path, &library.roots, &track.artist);

            if album_selector_is_key {
                let Some(ctx) = context.as_ref() else {
                    return false;
                };
                let key_matches = ctx
                    .album_key
                    .as_ref()
                    .is_some_and(|key| key == album_selector);
                if !key_matches {
                    return false;
                }
                // Only include root album tracks and recognised disc
                // sections (Disc 1, CD 2, …).  Exclude bonus/extra
                // subfolders so the queue represents the main album.
                return ctx.is_main_level_album_track || ctx.is_disc_section_album_track;
            }

            let artist_matches = if artist_selector_is_key {
                context
                    .as_ref()
                    .is_some_and(|ctx| ctx.artist_key == artist_selector)
            } else if let Some(ctx) = context.as_ref() {
                ctx.artist_name == artist_selector
            } else {
                normalized_library_artist(track) == artist_selector
            };
            if !artist_matches {
                return false;
            }

            if let Some(ctx) = context {
                let context_album = ctx
                    .album_folder
                    .unwrap_or_else(|| String::from("Unknown Album"));
                if context_album != album_selector {
                    return false;
                }
                return ctx.is_main_level_album_track || ctx.is_disc_section_album_track;
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
struct ImportExpandOutcome {
    tracks: Vec<PathBuf>,
    missing_count: usize,
    unsupported_count: usize,
    unreadable_count: usize,
    non_local_url_count: usize,
}

impl ImportExpandOutcome {
    fn skipped_count(&self) -> usize {
        self.missing_count
            + self.unsupported_count
            + self.unreadable_count
            + self.non_local_url_count
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

fn append_folder_tracks(root: &Path, outcome: &mut ImportExpandOutcome) {
    let mut folder_tracks = Vec::new();
    for entry in WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_map(std::result::Result::ok)
    {
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        if is_playlist_file(path) || !is_supported_audio(path) {
            continue;
        }
        folder_tracks.push(path.canonicalize().unwrap_or_else(|_| path.to_path_buf()));
    }
    folder_tracks.sort_by(|a, b| a.to_string_lossy().cmp(&b.to_string_lossy()));
    outcome.tracks.extend(folder_tracks);
}

fn append_import_path(path: &Path, outcome: &mut ImportExpandOutcome) {
    if path.is_dir() {
        append_folder_tracks(path, outcome);
        return;
    }

    if is_playlist_file(path) {
        let Ok(bytes) = fs::read(path) else {
            outcome.push_unreadable();
            return;
        };
        let mut reader = Cursor::new(decode_playlist_bytes(&bytes));
        let base_dir = path.parent().unwrap_or_else(|| Path::new(""));
        let mut line = String::new();
        while let Ok(read) = reader.read_line(&mut line) {
            if read == 0 {
                break;
            }
            let cleaned = line.trim_start_matches('\u{feff}');
            if let Some(entry_path) = playlist_entry_path(cleaned, base_dir, outcome) {
                append_import_path(&entry_path, outcome);
            }
            line.clear();
        }
        return;
    }

    if path.is_file() && is_supported_audio(path) {
        outcome.tracks.push(path.to_path_buf());
        return;
    }

    outcome.push_unsupported();
}

fn expand_import_paths(paths: Vec<PathBuf>) -> ImportExpandOutcome {
    let mut outcome = ImportExpandOutcome::default();
    for raw_path in paths {
        let Some(path) = canonicalize_existing_path(&raw_path) else {
            outcome.push_missing();
            continue;
        };
        append_import_path(&path, &mut outcome);
    }
    outcome
}

fn format_import_warning(action: &str, outcome: &ImportExpandOutcome) -> Option<String> {
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

pub(super) fn sync_queue_details(
    state: &mut BridgeState,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
) -> bool {
    let queue_paths: HashSet<&Path> = state.queue.iter().map(PathBuf::as_path).collect();
    let library_paths: HashSet<&Path> = state
        .library
        .tracks
        .iter()
        .map(|track| track.path.as_path())
        .collect();
    let mut changed = false;

    let previous_len = state.queue_details.len();
    state
        .queue_details
        .retain(|path, _| queue_paths.contains(path.as_path()));
    changed |= state.queue_details.len() != previous_len;
    state
        .queue_detail_fingerprints
        .retain(|path, _| queue_paths.contains(path.as_path()));
    state
        .pending_queue_detail_fingerprints
        .retain(|path, _| queue_paths.contains(path.as_path()));
    let mut pending_requests = Vec::<(PathBuf, TrackFileFingerprint)>::new();

    for path in &state.queue {
        if library_paths.contains(path.as_path()) {
            changed |= state.queue_details.remove(path).is_some();
            state.queue_detail_fingerprints.remove(path);
            state.pending_queue_detail_fingerprints.remove(path);
            continue;
        }
        if !path.is_file() || !is_supported_audio(path) {
            changed |= state.queue_details.remove(path).is_some();
            state.queue_detail_fingerprints.remove(path);
            state.pending_queue_detail_fingerprints.remove(path);
            continue;
        }
        let Some(fingerprint) = track_file_fingerprint(path) else {
            changed |= state.queue_details.remove(path).is_some();
            state.queue_detail_fingerprints.remove(path);
            state.pending_queue_detail_fingerprints.remove(path);
            continue;
        };

        let cached_fingerprint = state.queue_detail_fingerprints.get(path).copied();
        if cached_fingerprint == Some(fingerprint) && state.queue_details.contains_key(path) {
            continue;
        }

        if cached_fingerprint.is_some() && cached_fingerprint != Some(fingerprint) {
            changed |= state.queue_details.remove(path).is_some();
            state.queue_detail_fingerprints.remove(path);
        }

        if state.pending_queue_detail_fingerprints.get(path) == Some(&fingerprint) {
            continue;
        }
        pending_requests.push((path.clone(), fingerprint));
    }

    let cached_rows = load_external_track_caches(&pending_requests);
    for (path, fingerprint) in pending_requests {
        if let Some(indexed) = cached_rows.get(&path) {
            let needs_update = state.queue_details.get(&path) != Some(indexed);
            state
                .queue_detail_fingerprints
                .insert(path.clone(), fingerprint);
            state.pending_queue_detail_fingerprints.remove(&path);
            if needs_update {
                state.queue_details.insert(path, indexed.clone());
                changed = true;
            }
            continue;
        }

        state
            .pending_queue_detail_fingerprints
            .insert(path.clone(), fingerprint);
        if external_queue_details_tx
            .send(ExternalQueueDetailsRequest {
                path: path.clone(),
                fingerprint,
            })
            .is_err()
        {
            state.pending_queue_detail_fingerprints.remove(&path);
        }
    }
    changed
}

fn replace_queue_paths(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    paths: Vec<PathBuf>,
    autoplay: bool,
) -> bool {
    if paths.is_empty() {
        return false;
    }
    state.queue = paths;
    state.selected_queue_index = Some(0);
    let _ = sync_queue_details(state, external_queue_details_tx);
    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    if autoplay {
        playback.command(PlaybackCommand::PlayAt(0));
        playback.command(PlaybackCommand::Play);
    }
    true
}

fn append_queue_paths(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    paths: Vec<PathBuf>,
) -> bool {
    if paths.is_empty() {
        return false;
    }
    if state.queue.is_empty() {
        state.queue.extend(paths);
        let _ = sync_queue_details(state, external_queue_details_tx);
        playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    } else {
        state.queue.extend(paths.clone());
        let _ = sync_queue_details(state, external_queue_details_tx);
        playback.command(PlaybackCommand::AddToQueue(paths));
    }
    true
}

fn handle_import_library_command(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    event_tx: &Sender<BridgeEvent>,
    action: &str,
    paths: Vec<PathBuf>,
    replace: bool,
) -> bool {
    let outcome = expand_import_paths(paths);
    if let Some(message) = format_import_warning(action, &outcome) {
        let _ = try_send_event(event_tx, BridgeEvent::Error(message));
    }
    if replace {
        replace_queue_paths(
            state,
            playback,
            external_queue_details_tx,
            outcome.tracks,
            true,
        )
    } else {
        append_queue_paths(state, playback, external_queue_details_tx, outcome.tracks)
    }
}

// ---------------------------------------------------------------------------
// Track updates
// ---------------------------------------------------------------------------

fn update_library_track_details(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) -> bool {
    let mut next_library = (*state.library).clone();
    let mut changed = false;
    for track in &mut next_library.tracks {
        let Some(indexed) = indexed_by_path.get(&track.path) else {
            continue;
        };
        if track.title != indexed.title {
            track.title.clone_from(&indexed.title);
            changed = true;
        }
        if track.artist != indexed.artist {
            track.artist.clone_from(&indexed.artist);
            changed = true;
        }
        if track.album != indexed.album {
            track.album.clone_from(&indexed.album);
            changed = true;
        }
        if track.cover_path != indexed.cover_path {
            track.cover_path.clone_from(&indexed.cover_path);
            changed = true;
        }
        if track.genre != indexed.genre {
            track.genre.clone_from(&indexed.genre);
            changed = true;
        }
        if track.year != indexed.year {
            track.year = indexed.year;
            changed = true;
        }
        if track.track_no != indexed.track_no {
            track.track_no = indexed.track_no;
            changed = true;
        }
        if track.duration_secs != indexed.duration_secs {
            track.duration_secs = indexed.duration_secs;
            changed = true;
        }
    }
    if changed {
        next_library.search_revision = next_library.search_revision.saturating_add(1);
        state.library = Arc::new(next_library);
    }
    changed
}

fn update_queue_track_details(
    state: &mut BridgeState,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) -> bool {
    let library_paths: HashSet<&Path> = state
        .library
        .tracks
        .iter()
        .map(|track| track.path.as_path())
        .collect();
    let mut changed = false;

    for (path, indexed) in indexed_by_path {
        if library_paths.contains(path.as_path()) {
            changed |= state.queue_details.remove(path).is_some();
            state.queue_detail_fingerprints.remove(path);
            state.pending_queue_detail_fingerprints.remove(path);
            continue;
        }
        let needs_update = state
            .queue_details
            .get(path)
            .is_none_or(|existing| existing != indexed);
        if needs_update {
            state.queue_details.insert(path.clone(), indexed.clone());
            changed = true;
        }
        if let Some(fingerprint) = track_file_fingerprint(path) {
            state
                .queue_detail_fingerprints
                .insert(path.clone(), fingerprint);
        }
        state.pending_queue_detail_fingerprints.remove(path);
    }

    changed
}

fn refresh_edited_paths(
    state: &mut BridgeState,
    paths: &[PathBuf],
    metadata: &MetadataService,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    if paths.is_empty() {
        return false;
    }

    let indexed_by_path = match refresh_indexed_metadata_for_paths(paths) {
        Ok(indexed) => indexed,
        Err(err) => {
            let _ = try_send_event(event_tx, BridgeEvent::Error(err));
            return false;
        }
    };

    let mut changed = false;
    changed |= update_library_track_details(state, &indexed_by_path);
    changed |= update_queue_track_details(state, &indexed_by_path);

    if let Some(current_path) = state.playback.current.as_ref() {
        if paths.iter().any(|path| path == current_path) {
            metadata.request(current_path.clone());
            changed = true;
        }
    }

    if changed {
        state.rebuild_pre_built_tree();
    }
    changed |= sync_queue_details(state, external_queue_details_tx);
    changed
}

fn refresh_renamed_paths(
    state: &mut BridgeState,
    renames: &[(PathBuf, PathBuf)],
    metadata: &MetadataService,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    if renames.is_empty() {
        return false;
    }

    let rename_map: HashMap<PathBuf, PathBuf> = renames.iter().cloned().collect();
    let new_paths = renames
        .iter()
        .map(|(_, new_path)| new_path.clone())
        .collect::<Vec<_>>();
    let indexed_by_path = match refresh_indexed_metadata_for_paths(&new_paths) {
        Ok(indexed) => indexed,
        Err(err) => {
            let _ = try_send_event(event_tx, BridgeEvent::Error(err));
            return false;
        }
    };

    let mut changed = false;
    changed |= apply_renamed_library_paths(state, &rename_map, &indexed_by_path);
    changed |= apply_renamed_queue_paths(state, renames, &rename_map, &indexed_by_path, metadata);
    changed |= update_queue_track_details(state, &indexed_by_path);
    if changed {
        state.rebuild_pre_built_tree();
    }
    changed |= sync_queue_details(state, external_queue_details_tx);
    changed
}

fn apply_renamed_library_paths(
    state: &mut BridgeState,
    rename_map: &HashMap<PathBuf, PathBuf>,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
) -> bool {
    let mut changed = false;
    let mut next_library = (*state.library).clone();
    for track in &mut next_library.tracks {
        let Some(new_path) = rename_map.get(&track.path) else {
            continue;
        };
        if &track.path != new_path {
            track.path.clone_from(new_path);
            changed = true;
        }
        if let Some(indexed) = indexed_by_path.get(new_path) {
            if track.title != indexed.title {
                track.title.clone_from(&indexed.title);
                changed = true;
            }
            if track.artist != indexed.artist {
                track.artist.clone_from(&indexed.artist);
                changed = true;
            }
            if track.album != indexed.album {
                track.album.clone_from(&indexed.album);
                changed = true;
            }
            if track.cover_path != indexed.cover_path {
                track.cover_path.clone_from(&indexed.cover_path);
                changed = true;
            }
            if track.genre != indexed.genre {
                track.genre.clone_from(&indexed.genre);
                changed = true;
            }
            if track.year != indexed.year {
                track.year = indexed.year;
                changed = true;
            }
            if track.track_no != indexed.track_no {
                track.track_no = indexed.track_no;
                changed = true;
            }
            if track.duration_secs != indexed.duration_secs {
                track.duration_secs = indexed.duration_secs;
                changed = true;
            }
        }
    }
    if changed {
        next_library.search_revision = next_library.search_revision.saturating_add(1);
        state.library = Arc::new(next_library);
    }
    changed
}

fn apply_renamed_queue_paths(
    state: &mut BridgeState,
    renames: &[(PathBuf, PathBuf)],
    rename_map: &HashMap<PathBuf, PathBuf>,
    indexed_by_path: &HashMap<PathBuf, IndexedTrack>,
    metadata: &MetadataService,
) -> bool {
    let mut changed = false;
    for path in &mut state.queue {
        if let Some(new_path) = rename_map.get(path) {
            if path != new_path {
                *path = new_path.clone();
                changed = true;
            }
        }
    }

    if let Some(current_path) = state.playback.current.as_mut() {
        if let Some(new_path) = rename_map.get(current_path) {
            if current_path != new_path {
                *current_path = new_path.clone();
                metadata.request(new_path.clone());
                changed = true;
            }
        }
    }
    for (old_path, new_path) in renames {
        if let Some(details) = state.queue_details.remove(old_path) {
            if state
                .library
                .tracks
                .iter()
                .any(|track| track.path == *new_path)
            {
                changed = true;
            } else {
                state.queue_details.insert(
                    new_path.clone(),
                    indexed_by_path.get(new_path).cloned().unwrap_or(details),
                );
                changed = true;
            }
        }
        if let Some(fingerprint) = state.queue_detail_fingerprints.remove(old_path) {
            state
                .queue_detail_fingerprints
                .insert(new_path.clone(), fingerprint);
        }
        if let Some(fingerprint) = state.pending_queue_detail_fingerprints.remove(old_path) {
            state
                .pending_queue_detail_fingerprints
                .insert(new_path.clone(), fingerprint);
        }
    }
    changed
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
        pump_external_queue_detail_events, pump_library_events, pump_metadata_events,
        pump_playback_events,
    };
    use super::super::ExternalQueueDetailsEvent;

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

    fn library_track(
        path: &str,
        root: &PathBuf,
        artist: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
    ) -> LibraryTrack {
        LibraryTrack {
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
    fn sync_queue_details_requests_external_tracks_and_prunes_removed_entries() {
        let root = test_dir("queue-details");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        let cover = root.join("cover.jpg");
        write_stub(&track, b"not-real-audio");
        write_stub(&cover, b"not-real-jpg");
        let (request_tx, request_rx) = crossbeam_channel::unbounded();
        let (event_tx, event_rx) = crossbeam_channel::unbounded();

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];
        assert!(!sync_queue_details(&mut state, &request_tx));
        let request = request_rx.try_recv().expect("external detail request");
        assert_eq!(request.path, track);

        event_tx
            .send(ExternalQueueDetailsEvent {
                path: request.path.clone(),
                fingerprint: request.fingerprint,
                indexed: IndexedTrack {
                    title: "song".to_string(),
                    artist: String::new(),
                    album: String::new(),
                    cover_path: cover.to_string_lossy().to_string(),
                    genre: String::new(),
                    year: None,
                    track_no: None,
                    duration_secs: None,
                },
            })
            .expect("send queue detail event");
        assert!(pump_external_queue_detail_events(&event_rx, &mut state));
        assert_eq!(
            state
                .queue_details
                .get(&request.path)
                .map(|details| details.title.as_str()),
            Some("song")
        );
        assert_eq!(
            state
                .queue_details
                .get(&request.path)
                .map(|details| details.cover_path.as_str()),
            Some(cover.to_string_lossy().as_ref())
        );

        state.queue.clear();
        assert!(sync_queue_details(&mut state, &request_tx));
        assert!(state.queue_details.is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn sync_queue_details_skips_library_tracks_in_queue() {
        let track = p("/library/song.flac");
        let (request_tx, request_rx) = crossbeam_channel::unbounded();
        let mut state = BridgeState::default();
        state.library = Arc::new(LibrarySnapshot {
            tracks: vec![LibraryTrack {
                path: track.clone(),
                ..LibraryTrack::default()
            }],
            ..LibrarySnapshot::default()
        });
        state.queue = vec![track.clone()];

        assert!(!sync_queue_details(&mut state, &request_tx));

        assert!(!state.queue_details.contains_key(&track));
        assert!(request_rx.try_recv().is_err());
    }

    #[test]
    fn apply_session_restore_does_not_eagerly_populate_queue_details() {
        use super::super::config::{apply_session_restore, SessionSnapshot};
        use crate::playback::PlaybackEngine;

        let (analysis_tx, _) = crossbeam_channel::unbounded();
        let (pcm_tx, _) = crossbeam_channel::unbounded();
        let (playback, _playback_rx) = PlaybackEngine::new(analysis_tx, pcm_tx);
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
        };

        apply_session_restore(&mut state, &playback, Some(&session));

        assert_eq!(state.queue, vec![track]);
        assert!(state.queue_details.is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn pump_library_events_requests_queue_details_for_restored_external_tracks() {
        use crate::library::LibraryEvent;

        let root = test_dir("restored-external-queue-details");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"not-real-audio");

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];

        let (library_tx, library_rx) = crossbeam_channel::unbounded();
        let (request_tx, request_rx) = crossbeam_channel::unbounded();
        library_tx
            .send(LibraryEvent::Snapshot(LibrarySnapshot::default()))
            .expect("send library snapshot");

        assert!(pump_library_events(&library_rx, &request_tx, &mut state));
        let request = request_rx.try_recv().expect("external detail request");
        assert_eq!(request.path, track);
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
        let (request_tx, request_rx) = crossbeam_channel::unbounded();

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];
        state.queue_details.insert(
            track.clone(),
            IndexedTrack {
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

        assert!(sync_queue_details(&mut state, &request_tx));
        assert!(!state.queue_details.contains_key(&track));
        let request = request_rx.try_recv().expect("replacement request");
        assert_ne!(request.fingerprint, old_fingerprint);
        assert_eq!(
            state.pending_queue_detail_fingerprints.get(&track),
            Some(&request.fingerprint)
        );

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn external_queue_detail_event_is_ignored_when_track_is_in_library() {
        let root = test_dir("queue-details-library-owned");
        fs::create_dir_all(&root).expect("mkdir root");
        let track = root.join("song.flac");
        write_stub(&track, b"not-real-audio");
        let fingerprint = track_file_fingerprint(&track).expect("track fingerprint");

        let mut state = BridgeState::default();
        state.queue = vec![track.clone()];
        state.library = Arc::new(LibrarySnapshot {
            tracks: vec![LibraryTrack {
                path: track.clone(),
                title: "Library Title".to_string(),
                ..LibraryTrack::default()
            }],
            ..LibrarySnapshot::default()
        });
        state
            .pending_queue_detail_fingerprints
            .insert(track.clone(), fingerprint);

        let (event_tx, event_rx) = crossbeam_channel::unbounded();
        event_tx
            .send(ExternalQueueDetailsEvent {
                path: track.clone(),
                fingerprint,
                indexed: IndexedTrack {
                    title: "External Title".to_string(),
                    artist: String::new(),
                    album: String::new(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: None,
                    track_no: None,
                    duration_secs: None,
                },
            })
            .expect("send queue detail event");

        assert!(!pump_external_queue_detail_events(&event_rx, &mut state));
        assert!(!state.queue_details.contains_key(&track));
        assert!(!state.pending_queue_detail_fingerprints.contains_key(&track));

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn collect_artist_paths_for_queue_respects_year_sort_mode() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![
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
            ],
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
            tracks: vec![
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
            ],
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
            tracks: vec![
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
            ],
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
            tracks: vec![sampler, blackfield],
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
            tracks: vec![
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
            ],
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
            tracks: vec![
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
            ],
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

    #[test]
    fn collect_album_paths_for_queue_excludes_bonus_by_name_selector() {
        // Test the name-based (non-key) album selector path: when both
        // artist and album selectors are plain names instead of keys,
        // the filter should still exclude non-disc sections.
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![
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
            ],
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
        assert!(analysis_rx
            .recv_timeout(Duration::from_millis(120))
            .is_err());
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
}
