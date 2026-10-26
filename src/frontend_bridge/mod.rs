// SPDX-License-Identifier: GPL-3.0-or-later

use crate::analysis::SpectrogramOutputRoute;
use std::collections::{HashMap, HashSet, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{after, bounded, never, select, unbounded, Receiver, Sender, TrySendError};

use crate::analysis::{
    AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot, SpectrogramDisplayMode,
    SpectrogramViewMode, ANALYSIS_SHUTDOWN_TIMEOUT,
};
use crate::lastfm::{
    Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    RuntimeState as LastFmRuntimeState, ShutdownOutcome as LastFmShutdownOutcome,
};
use crate::library::{
    IndexedTrack, LibraryCommand, LibraryEvent, LibraryService, LibrarySnapshot, LibraryStatus,
    TrackFileFingerprint, LIBRARY_SHUTDOWN_TIMEOUT,
};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{
    PathRenames, PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot, PlaybackState,
    RepeatMode,
};
use crate::worker_thread::{spawn_joinable, JoinableThread};

mod commands;
mod config;
mod edit_refresh;
mod errors;
mod events;
pub mod ffi;
mod imports;
mod library_index;
pub mod library_tree;
mod queue;
mod queue_relay;
mod queue_sync;
mod queue_undo;
mod queue_validation;
mod search;
mod search_query;
mod tree_worker;
mod workers;

use commands::{handle_bridge_command, sync_queue_details, BridgeCommandContext};
use config::{
    apply_session_restore, load_session_snapshot, load_settings_into, save_session_snapshot,
    save_settings, session_snapshot_for_state, SaveFailure, SessionSnapshot,
};
pub use errors::{BridgeError, ErrorCategory, ErrorSeverity};
use events::{
    apply_library_events, drain_analysis_events, drain_apply_album_art_events,
    drain_metadata_events, drain_playback_events, finalize_lastfm_track,
    note_precomputed_spectrogram_chunk, process_analysis_event, process_apply_album_art_event,
    process_lastfm_state, process_metadata_event, process_playback_event, tick_lastfm_playback,
};
pub use queue_sync::{QueueEdit, QueueUpdate};
pub use queue_undo::{QueueUndoKind, QueueUndoRestored, QueueUndoState};
pub(crate) use search::is_main_album_disc_section;
use search::{drain_search_results, process_search_results, SearchWorkerQuery};
use workers::{spawn_apply_album_art_worker, spawn_lastfm_service, spawn_search_worker};

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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LibrarySortMode {
    #[default]
    Year,
    Title,
}

impl LibrarySortMode {
    #[must_use]
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => Self::Title,
            _ => Self::Year,
        }
    }

    #[must_use]
    pub fn to_i32(self) -> i32 {
        match self {
            Self::Year => 0,
            Self::Title => 1,
        }
    }
}

#[derive(Debug, Clone)]
pub enum BridgeCommand {
    RequestSnapshot,
    Playback(BridgePlaybackCommand),
    Queue(BridgeQueueCommand),
    /// A queue command from the UI with the queue revision its rows
    /// showed, so the bridge can carry its indices across later edits; see
    /// [`queue_sync::QueueSync::resolve`].
    QueueAt {
        revision: u64,
        command: BridgeQueueCommand,
    },
    Library(BridgeLibraryCommand),
    Analysis(BridgeAnalysisCommand),
    Settings(BridgeSettingsCommand),
    /// Save the settings and the session now, and answer with
    /// [`BridgeEvent::StateSaved`], so the UI can close the window only once
    /// they are on disk, or tell the user they are not. The answer carries
    /// this request ID, so the UI can tell which of its attempts it settles.
    SaveState(u32),
    Shutdown,
}

#[derive(Debug, Clone)]
pub enum BridgePlaybackCommand {
    Play,
    Pause,
    Stop,
    Next,
    Previous,
    Seek(Duration),
    SetVolume(f32),
    SetRepeatMode(RepeatMode),
    SetShuffle(bool),
    ToggleChannelMute(u8),
    SoloChannel(u8),
}

#[derive(Debug, Clone)]
pub enum BridgeQueueCommand {
    Replace {
        tracks: Vec<PathBuf>,
        autoplay: bool,
    },
    Append(Vec<PathBuf>),
    PlayAt(usize),
    Remove(usize),
    RemoveMany(Vec<usize>),
    Move {
        from: usize,
        to: usize,
    },
    /// Move the rows at these indices together, as `to` says. The rows
    /// keep their order, and one undo puts them all back.
    MoveRows {
        rows: Vec<usize>,
        to: RowsMove,
    },
    /// Insert `tracks` before row `at` of the queue at `revision`, which
    /// the bridge carries across the edits made since; after the last row
    /// when that row is gone.
    InsertAt {
        revision: u64,
        at: usize,
        tracks: Vec<PathBuf>,
    },
    Select(Option<usize>),
    Clear,
    /// Undo the destructive queue change the UI was offered under this ID;
    /// see [`queue_undo`].
    Undo(u64),
}

/// Where [`BridgeQueueCommand::MoveRows`] moves its rows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RowsMove {
    /// Together, in their order, before the row now at this index, or
    /// after the last row when it is past it.
    Before(usize),
    /// Each run of rows one row up, keeping the gaps between runs; a run
    /// at the top stays.
    Up,
    /// Each run of rows one row down; a run at the bottom stays.
    Down,
}

/// A library row to queue, as [`BridgeLibraryCommand::QueueItems`] names it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LibraryQueueItem {
    /// Tracks, files or folders, such as a track's or an album's paths.
    Paths(Vec<PathBuf>),
    /// Every track of the artist with this tree key or name.
    Artist(String),
    /// Every track under this library root.
    Root(String),
}

/// Where queued tracks go.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueuePlacement {
    Append,
    /// Replace the queue and start playing.
    Replace,
    /// Before row `at` of the queue at `revision`; see
    /// [`BridgeQueueCommand::InsertAt`].
    Insert {
        revision: u64,
        at: usize,
    },
}

#[derive(Debug, Clone)]
pub enum BridgeLibraryCommand {
    ScanRoot(PathBuf),
    AddRoot {
        path: PathBuf,
        name: String,
    },
    RenameRoot {
        path: PathBuf,
        name: String,
    },
    RemoveRoot(PathBuf),
    RescanRoot(PathBuf),
    RescanAll,
    AddTrack(PathBuf),
    PlayTrack(PathBuf),
    ReplaceWithAlbum(Vec<PathBuf>),
    AppendAlbum(Vec<PathBuf>),
    ReplaceAlbumByKey {
        artist: String,
        album: String,
    },
    AppendAlbumByKey {
        artist: String,
        album: String,
    },
    ReplaceArtistByKey {
        artist: String,
    },
    AppendArtistByKey {
        artist: String,
    },
    ReplaceRootByPath {
        root: String,
    },
    AppendRootByPath {
        root: String,
    },
    ReplaceAllTracks,
    AppendAllTracks,
    /// Queue library rows, in order, as one change to the queue: replacing
    /// it with a selection is one change to undo.
    QueueItems {
        items: Vec<LibraryQueueItem>,
        placement: QueuePlacement,
    },
    ApplyAlbumArt {
        track_path: PathBuf,
        artwork_path: PathBuf,
        /// The track file as the artwork search found it; nothing is
        /// written unless it is still that file.
        track_identity: crate::artwork::TrackFileIdentity,
    },
    ShowTrackInLibrary(PathBuf),
    SetNodeExpanded {
        key: String,
        expanded: bool,
    },
    SetViewState {
        selection_key: String,
        anchor_key: String,
        anchor_offset: f32,
    },
    SetSearchQuery {
        limit: u32,
        seq: u32,
        query: String,
    },
    RefreshEditedPaths(Vec<PathBuf>),
}

#[derive(Debug, Clone)]
pub enum BridgeAnalysisCommand {
    SetFftSize(usize),
    SetSpectrogramZoomLevel(f32),
    SetSpectrogramWidgetWidth(u32),
    SetSpectrogramActive(bool),
}

#[derive(Debug, Clone)]
pub enum BridgeSettingsCommand {
    LoadFromDisk,
    SaveToDisk,
    SetVolume(f32),
    SetFftSize(usize),
    SetSpectrogramViewMode(SpectrogramViewMode),
    SetSpectrogramDisplayMode(SpectrogramDisplayMode),
    SetViewerFullscreenMode(ViewerFullscreenMode),
    SetPreventDisplaySleepInFullscreen(bool),
    SetDbRange(f32),
    SetLogScale(bool),
    SetShowFps(bool),
    SetShowLevelMeter(bool),
    SetShowSpectrogramCrosshair(bool),
    SetShowSpectrogramScale(bool),
    SetSystemMediaControlsEnabled(bool),
    SetLibrarySortMode(LibrarySortMode),
    SetLastFmScrobblingEnabled(bool),
    SetChannelButtonsVisibility(u8),
    SetSpectrogramZoomEnabled(bool),
    SetLibraryWatchEnabled(bool),
    BeginLastFmAuth,
    CompleteLastFmAuth,
    DisconnectLastFm,
}

impl SpectrogramViewMode {
    #[must_use]
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => Self::PerChannel,
            _ => Self::Downmix,
        }
    }

    #[must_use]
    pub fn to_i32(self) -> i32 {
        match self {
            Self::Downmix => 0,
            Self::PerChannel => 1,
        }
    }

    fn parse_settings_value(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "downmix" | "single" | "mono" | "0" => Some(Self::Downmix),
            "per_channel" | "per-channel" | "channels" | "1" => Some(Self::PerChannel),
            _ => None,
        }
    }

    fn settings_value(self) -> &'static str {
        match self {
            Self::Downmix => "downmix",
            Self::PerChannel => "per_channel",
        }
    }
}

impl SpectrogramDisplayMode {
    #[must_use]
    pub fn parse_settings_value(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "rolling" | "0" => Some(Self::Rolling),
            "centered" | "centred" | "1" => Some(Self::Centered),
            _ => None,
        }
    }

    fn settings_value(self) -> &'static str {
        match self {
            Self::Rolling => "rolling",
            Self::Centered => "centered",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ViewerFullscreenMode {
    WithinWindow,
    WholeScreen,
}

impl ViewerFullscreenMode {
    #[must_use]
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => Self::WholeScreen,
            _ => Self::WithinWindow,
        }
    }

    #[must_use]
    pub fn to_i32(self) -> i32 {
        match self {
            Self::WithinWindow => 0,
            Self::WholeScreen => 1,
        }
    }

    fn parse_settings_value(raw: &str) -> Option<Self> {
        match raw.trim() {
            "within_window" => Some(Self::WithinWindow),
            "whole_screen" => Some(Self::WholeScreen),
            _ => None,
        }
    }

    fn settings_value(self) -> &'static str {
        match self {
            Self::WithinWindow => "within_window",
            Self::WholeScreen => "whole_screen",
        }
    }
}

#[derive(Debug, Clone)]
pub enum BridgeEvent {
    Snapshot(Box<BridgeSnapshot>),
    SearchResults(Box<BridgeSearchResultsFrame>),
    PrecomputedSpectrogramChunk(crate::analysis::PrecomputedSpectrogramChunk),
    Error(BridgeError),
    Stopped,
    /// The bridge stopped without being asked to, for this reason.
    Failed(String),
    /// The answer to [`BridgeCommand::SaveState`] with that request ID: what
    /// to show the user if a save failed.
    StateSaved {
        request_id: u32,
        result: Result<(), String>,
    },
}

#[derive(Debug, Clone)]
pub struct BridgeSearchResultsFrame {
    pub seq: u32,
    pub totals: [u32; 3],
    pub rows: Vec<BridgeSearchResultRow>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BridgeSearchResultRowType {
    Artist = 1,
    Album = 2,
    Track = 3,
}

#[derive(Debug, Clone)]
pub struct BridgeSearchResultRow {
    pub match_detail: String,
    pub row_type: BridgeSearchResultRowType,
    pub score: f32,
    pub year: Option<i32>,
    pub track_number: Option<u32>,
    pub count: u32,
    pub length_seconds: Option<f32>,
    pub label: String,
    pub artist: String,
    pub album: String,
    pub root_label: String,
    pub genre: String,
    pub cover_path: String,
    /// See [`crate::library::LibraryTrack::cover_stamp`].
    pub cover_stamp: u64,
    pub artist_key: String,
    pub album_key: String,
    pub section_key: String,
    pub track_key: String,
    pub track_path: String,
}

#[derive(Debug, Clone)]
pub struct BridgeSnapshot {
    pub playback: PlaybackSnapshot,
    pub analysis: AnalysisSnapshot,
    pub metadata: TrackMetadata,
    pub library: Arc<LibrarySnapshot>,
    pub library_status: LibraryStatus,
    pub(crate) queue_details: Arc<HashMap<PathBuf, IndexedTrack>>,
    pub library_artist_count: usize,
    pub library_album_count: usize,
    /// The tracks of each library root.
    pub library_root_track_counts: Arc<HashMap<PathBuf, usize>>,
    pub library_expanded_keys: Vec<String>,
    pub library_view_selection_key: String,
    pub library_view_anchor_key: String,
    pub library_view_anchor_offset: f32,
    pub library_reveal_generation: u32,
    pub pre_built_tree_bytes: Option<Arc<Vec<u8>>>,
    pub queue_len: usize,
    pub queue_revision: u64,
    /// The queue's changes since the previous snapshot, if any.
    pub queue_update: Option<QueueUpdate>,
    pub selected_queue_index: Option<usize>,
    /// The queue change the UI can undo, and the rows the latest undo
    /// brought back.
    pub queue_undo: QueueUndoState,
    pub settings: BridgeSettings,
    pub lastfm: LastFmRuntimeState,
}

// Settings struct with individual toggle fields — not a state machine.
#[allow(clippy::struct_excessive_bools)]
#[derive(Debug, Clone)]
pub struct BridgeDisplaySettings {
    pub log_scale: bool,
    pub show_fps: bool,
    pub show_level_meter: bool,
    pub prevent_display_sleep_in_fullscreen: bool,
    pub show_spectrogram_crosshair: bool,
    pub show_spectrogram_scale: bool,
    pub channel_buttons_visibility: u8,
    pub spectrogram_zoom_enabled: bool,
}

#[derive(Debug, Clone)]
pub struct BridgeIntegrationSettings {
    pub system_media_controls_enabled: bool,
    pub lastfm_scrobbling_enabled: bool,
    pub lastfm_username: String,
}

#[derive(Debug, Clone)]
pub struct BridgeSettings {
    /// Volume level on the cubic scale; see [`crate::playback::volume_level_to_gain`].
    pub volume: f32,
    pub fft_size: usize,
    pub spectrogram_view_mode: SpectrogramViewMode,
    pub spectrogram_display_mode: SpectrogramDisplayMode,
    pub viewer_fullscreen_mode: ViewerFullscreenMode,
    pub db_range: f32,
    pub display: BridgeDisplaySettings,
    pub library_sort_mode: LibrarySortMode,
    /// Whether the library watches its roots for changes, and rescans them
    /// at startup.
    pub library_watch_enabled: bool,
    pub integrations: BridgeIntegrationSettings,
    /// Lines of the settings file with keys this build does not know, such
    /// as a newer build's, kept as they were.
    pub unknown_lines: Vec<String>,
}

impl Default for BridgeSettings {
    fn default() -> Self {
        let show_fps = std::env::var_os("FERROUS_UI_SHOW_FPS").is_some()
            || std::env::var_os("FERROUS_PROFILE_UI").is_some()
            || std::env::var_os("FERROUS_PROFILE").is_some();
        Self {
            volume: 1.0,
            fft_size: 8192,
            spectrogram_view_mode: SpectrogramViewMode::Downmix,
            spectrogram_display_mode: SpectrogramDisplayMode::Rolling,
            viewer_fullscreen_mode: ViewerFullscreenMode::WithinWindow,
            db_range: 132.0,
            display: BridgeDisplaySettings {
                log_scale: false,
                show_fps,
                show_level_meter: true,
                prevent_display_sleep_in_fullscreen: true,
                show_spectrogram_crosshair: false,
                show_spectrogram_scale: false,
                channel_buttons_visibility: 1,
                spectrogram_zoom_enabled: true,
            },
            library_sort_mode: LibrarySortMode::Year,
            library_watch_enabled: true,
            integrations: BridgeIntegrationSettings {
                system_media_controls_enabled: true,
                lastfm_scrobbling_enabled: false,
                lastfm_username: String::new(),
            },
            unknown_lines: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Default)]
pub(super) struct BridgeState {
    queue_detail_generation: u64,
    queue_detail_validation_requested: bool,
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    library: Arc<LibrarySnapshot>,
    library_status: LibraryStatus,
    /// Finds `library`'s tracks by path.
    library_index: library_index::LibraryPathIndex,
    /// Paths whose tracks the bridge changed itself, from what other
    /// workers read. The library worker reads their rows again, so that a
    /// change applied after a newer one is corrected.
    library_rows_to_read: Vec<PathBuf>,
    queue_details: Arc<HashMap<PathBuf, IndexedTrack>>,
    queue_detail_fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    library_artist_count: usize,
    library_album_count: usize,
    library_root_track_counts: Arc<HashMap<PathBuf, usize>>,
    pre_built_tree_bytes: Arc<Vec<u8>>,
    expanded_keys: HashSet<String>,
    library_view_selection_key: String,
    library_view_anchor_key: String,
    library_view_anchor_offset: f32,
    library_reveal_generation: u32,
    queue: Vec<PathBuf>,
    queue_sync: queue_sync::QueueSync,
    queue_undo: queue_undo::QueueUndo,
    /// The row of the engine's current track as it last reported one, with
    /// the queue revision then. A stopped engine reports the track but no
    /// row, so this tells which of its rows it is.
    last_current_row: Option<(u64, usize)>,
    selected_queue_index: Option<usize>,
    settings: BridgeSettings,
    lastfm: LastFmRuntimeState,
    pending_search_results: Option<BridgeSearchResultsFrame>,
    pending_waveform_track: Option<PendingWaveformTrack>,
    analysis_track_token: u64,
    last_track_change: TrackChangeMarker,
    /// Renames sent to the playback engine that it has not acknowledged.
    /// Events it sent before the acknowledgement still carry the old paths.
    unacknowledged_engine_renames: VecDeque<Arc<PathRenames>>,
}

/// The most recent `TrackChanged` event. The generation advances on every
/// event, including one that restarts the current path, so consumers can
/// tell a new play of a track from a continuing one.
#[derive(Debug, Clone, Default)]
struct TrackChangeMarker {
    generation: u64,
    path: Option<PathBuf>,
}

#[derive(Debug, Clone, Default)]
struct LastFmPlaybackTracker {
    active_path: Option<PathBuf>,
    /// `TrackChangeMarker::generation` when this play started.
    track_change_generation: u64,
    artist: String,
    track: String,
    album: String,
    track_number: Option<u32>,
    /// The library last found to have no number for the track.
    library_without_track_number: std::sync::Weak<LibrarySnapshot>,
    duration_seconds: Option<u32>,
    started_at_utc: Option<i64>,
    listened_duration: Duration,
    last_listen_tick: Option<Instant>,
    now_playing_sent: bool,
    scrobble_queued: bool,
}

#[derive(Debug, Clone)]
struct PendingWaveformTrack {
    path: PathBuf,
    reset_spectrogram: bool,
    track_token: u64,
}

#[derive(Debug, Clone)]
pub(super) struct ApplyAlbumArtRequest {
    pub(super) track_path: PathBuf,
    pub(super) artwork_path: PathBuf,
    pub(super) track_identity: crate::artwork::TrackFileIdentity,
}

#[derive(Debug, Clone)]
pub(super) struct ApplyAlbumArtEvent {
    pub(super) track_path: PathBuf,
    pub(super) indexed_by_path: HashMap<PathBuf, IndexedTrack>,
    /// The state of each file in `indexed_by_path` once its art was applied.
    pub(super) fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    pub(super) error: Option<String>,
}

impl BridgeState {
    /// Apply `changes` to a copy of the library, which shares the tracks it
    /// keeps and keeps them in the database's order, and keep the path
    /// index for it.
    fn apply_library_changes(
        &mut self,
        changes: &crate::library::LibraryChanges,
    ) -> crate::library::AppliedChanges {
        let previous = Arc::clone(&self.library);
        let mut next = (*previous).clone();
        let applied = next.apply_changes(changes, |path| {
            self.library_index.positions(&previous, path)
        });
        if applied.changed {
            self.library = Arc::new(next);
            self.library_index
                .follow(&previous, &self.library, &applied);
        }
        applied
    }

    /// Have the UI compare its queue rows with the queue's details, which
    /// may have changed.
    fn refresh_queue_rows(&mut self) {
        if !self.queue.is_empty() {
            self.queue_sync.record(QueueEdit::Refresh, self.queue.len());
        }
    }

    fn snapshot(&self, include_tree: bool) -> BridgeSnapshot {
        let mut metadata = metadata_for_snapshot(&self.metadata);
        metadata.current_bitrate_kbps = self
            .metadata
            .displayed_bitrate_kbps(self.playback.position.as_secs_f64());
        // The keys are only sent with a tree.
        let library_expanded_keys = if include_tree {
            let mut keys = self.expanded_keys.iter().cloned().collect::<Vec<_>>();
            keys.sort_unstable();
            keys
        } else {
            Vec::new()
        };
        BridgeSnapshot {
            playback: self.playback.clone(),
            analysis: self.analysis.clone(),
            metadata,
            library: self.library.clone(),
            library_status: self.library_status.clone(),
            queue_details: self.queue_details.clone(),
            library_artist_count: self.library_artist_count,
            library_album_count: self.library_album_count,
            library_root_track_counts: Arc::clone(&self.library_root_track_counts),
            library_expanded_keys,
            library_view_selection_key: self.library_view_selection_key.clone(),
            library_view_anchor_key: self.library_view_anchor_key.clone(),
            library_view_anchor_offset: self.library_view_anchor_offset,
            library_reveal_generation: self.library_reveal_generation,
            pre_built_tree_bytes: if include_tree {
                Some(self.pre_built_tree_bytes.clone())
            } else {
                None
            },
            queue_len: self.queue.len(),
            queue_revision: self.queue_sync.revision(),
            queue_update: self.queue_sync.update(&self.queue),
            selected_queue_index: self.selected_queue_index,
            queue_undo: self.queue_undo.state(),
            settings: self.settings.clone(),
            lastfm: self.lastfm.clone(),
        }
    }
}

fn metadata_for_snapshot(metadata: &TrackMetadata) -> TrackMetadata {
    TrackMetadata {
        source_path: metadata.source_path.clone(),
        title: metadata.title.clone(),
        artist: metadata.artist.clone(),
        album: metadata.album.clone(),
        album_artist: metadata.album_artist.clone(),
        genre: metadata.genre.clone(),
        year: metadata.year,
        track_number: metadata.track_number,
        sample_rate_hz: metadata.sample_rate_hz,
        bitrate_kbps: metadata.bitrate_kbps,
        channels: metadata.channels,
        channel_labels: metadata.channel_labels.clone(),
        bit_depth: metadata.bit_depth,
        format_label: metadata.format_label.clone(),
        current_bitrate_kbps: metadata.current_bitrate_kbps,
        bitrate_timeline_kbps: Vec::new(),
        cover_art_path: metadata.cover_art_path.clone(),
        cover_art_stamp: metadata.cover_art_stamp,
        cover_read_at: metadata.cover_read_at,
    }
}

/// The bridge thread and its channels. Stop it with [`Self::shutdown`];
/// dropping the handle leaves the thread running on its own until it sees
/// the command channel close.
pub struct FrontendBridgeHandle {
    tx: Sender<BridgeCommand>,
    rx: Receiver<BridgeEvent>,
    spectral_rx: Receiver<AnalysisEvent>,
    spectral_generation: Arc<AtomicU64>,
    import_gate: Arc<imports::ImportGate>,
    thread: BridgeThread,
}

/// The thread running the bridge loop.
pub(crate) struct BridgeThread {
    thread: JoinableThread,
    /// Set when the loop has stopped with the settings, the session, and
    /// Last.fm's queue saved.
    saved: Arc<AtomicBool>,
}

impl BridgeThread {
    /// Wait until `deadline` for the bridge loop to stop. Returns false if
    /// a save failed, or if the loop is still running at the deadline; it is
    /// left running then.
    pub(crate) fn join_by(self, deadline: Instant) -> bool {
        self.thread.join_by(deadline) && self.saved.load(Ordering::Acquire)
    }
}

/// The parts of a [`FrontendBridgeHandle`], for a frontend that drives them
/// itself.
pub(crate) struct BridgeParts {
    pub(crate) commands: Sender<BridgeCommand>,
    pub(crate) events: Receiver<BridgeEvent>,
    pub(crate) spectra: Receiver<AnalysisEvent>,
    pub(crate) spectral_generation: Arc<AtomicU64>,
    pub(crate) thread: BridgeThread,
}

struct BridgeRuntimeOptions {
    metadata_delay: Duration,
    persisted_state_enabled: bool,
    /// A Last.fm service to use instead of starting one.
    #[cfg(test)]
    lastfm: Option<(LastFmHandle, Receiver<LastFmEvent>)>,
    /// A library database of the test's own, instead of the process's.
    #[cfg(test)]
    library_db: Option<PathBuf>,
    /// Whether the library watches its roots as the settings say. Tests
    /// share the process's library database, so they watch only when they
    /// ask to.
    #[cfg(test)]
    library_watching: bool,
}

impl Default for BridgeRuntimeOptions {
    fn default() -> Self {
        Self {
            metadata_delay: Duration::ZERO,
            persisted_state_enabled: cfg!(not(test)),
            #[cfg(test)]
            lastfm: None,
            #[cfg(test)]
            library_db: None,
            #[cfg(test)]
            library_watching: false,
        }
    }
}

impl FrontendBridgeHandle {
    /// Start the bridge thread.
    ///
    /// # Errors
    ///
    /// Returns a message if the thread cannot be started.
    pub fn spawn() -> Result<Self, String> {
        Self::spawn_with_options(BridgeRuntimeOptions::default())
    }

    #[cfg(all(test, not(feature = "gst")))]
    fn spawn_with_metadata_delay(metadata_delay: Duration) -> Result<Self, String> {
        Self::spawn_with_options(BridgeRuntimeOptions {
            metadata_delay,
            ..BridgeRuntimeOptions::default()
        })
    }

    fn spawn_with_options(options: BridgeRuntimeOptions) -> Result<Self, String> {
        let (cmd_tx, cmd_rx) = unbounded::<BridgeCommand>();
        let (event_tx, event_rx) = event_channel();
        let (spectral_tx, spectral_rx) = bounded::<AnalysisEvent>(4);
        let spectral_generation = Arc::new(AtomicU64::new(0));
        let spectral_route = SpectrogramOutputRoute {
            sender: spectral_tx,
            generation: Arc::clone(&spectral_generation),
        };

        let imports = imports::ImportWorker::new();
        let import_gate = imports.gate();
        let saved = Arc::new(AtomicBool::new(false));
        let saved_by_loop = Arc::clone(&saved);
        // The bridge saves in the config directory of the test that starts it.
        #[cfg(test)]
        let config_dir = crate::paths::config_dir_override();
        let thread = spawn_joinable("ferrous-bridge", move || {
            #[cfg(test)]
            crate::paths::set_config_dir_override(config_dir);
            let all_saved = run_bridge_loop(&cmd_rx, &event_tx, options, spectral_route, imports);
            saved_by_loop.store(all_saved, Ordering::Release);
        })
        .map_err(|error| format!("failed to start the bridge thread: {error}"))?;
        let thread = BridgeThread { thread, saved };
        Ok(Self {
            tx: cmd_tx,
            rx: event_rx,
            spectral_rx,
            spectral_generation,
            import_gate,
            thread,
        })
    }

    /// Stop the bridge and wait, up to [`BRIDGE_SHUTDOWN_TIMEOUT`], for it
    /// to save the settings, the session, and Last.fm's queue, and to stop
    /// playback. Returns false if it is still running then, if one of them
    /// was not saved, or if playback left a file open.
    #[must_use]
    pub fn shutdown(self) -> bool {
        let _ = self.tx.send(BridgeCommand::Shutdown);
        self.thread
            .join_by(Instant::now() + BRIDGE_SHUTDOWN_TIMEOUT)
    }

    pub fn command(&self, cmd: BridgeCommand) {
        let _ = self.tx.send(cmd);
    }

    #[must_use]
    pub fn recv_timeout(&self, timeout: Duration) -> Option<BridgeEvent> {
        select! {
            recv(self.rx) -> event => event.ok(),
            recv(self.spectral_rx) -> event => event.ok().and_then(Self::spectral_event),
            default(timeout) => None,
        }
    }

    #[must_use]
    pub fn try_recv(&self) -> Option<BridgeEvent> {
        self.rx.try_recv().ok().or_else(|| {
            self.spectral_rx
                .try_recv()
                .ok()
                .and_then(Self::spectral_event)
        })
    }

    fn spectral_event(event: AnalysisEvent) -> Option<BridgeEvent> {
        match event {
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                Some(BridgeEvent::PrecomputedSpectrogramChunk(chunk))
            }
            AnalysisEvent::Snapshot(_) | AnalysisEvent::WorkerFailed(_) => None,
        }
    }

    /// The gate that file renames go through to reach this bridge in order.
    pub(crate) fn import_gate(&self) -> Arc<imports::ImportGate> {
        Arc::clone(&self.import_gate)
    }

    pub(crate) fn into_parts(self) -> BridgeParts {
        BridgeParts {
            commands: self.tx,
            events: self.rx,
            spectra: self.spectral_rx,
            spectral_generation: self.spectral_generation,
            thread: self.thread,
        }
    }
}

/// How long shutdown waits for the Last.fm service to save its queue.
const LASTFM_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);
/// How long a save before quitting waits for Last.fm to save its
/// scrobbles. Last.fm may be waiting for the network, which the save does
/// not wait for; the UI gives up on the whole save after 5 s.
const QUIT_SAVE_LASTFM_TIMEOUT: Duration = Duration::from_secs(2);

/// How long shutdown waits for the playback engine to stop and tell which
/// files it could not release. Stopping takes milliseconds, as the engine
/// does not wait for reads that do not return.
const PLAYBACK_SHUTDOWN_TIMEOUT: Duration = Duration::from_millis(500);

/// How long stopping the bridge waits for its thread to end.
///
/// Its shutdown waits up to [`LASTFM_SHUTDOWN_TIMEOUT`] for Last.fm, which
/// may be in the middle of a request, then up to
/// [`ANALYSIS_SHUTDOWN_TIMEOUT`] for the analysis threads, and up to
/// [`LIBRARY_SHUTDOWN_TIMEOUT`] for the library worker to stop a scan and
/// close its database. The second left covers the settings and session
/// saves, a few small files, and stopping playback, which waits up to
/// [`PLAYBACK_SHUTDOWN_TIMEOUT`]. A normal shutdown takes milliseconds; the
/// bound only keeps a stuck one from holding the app open.
pub(crate) const BRIDGE_SHUTDOWN_TIMEOUT: Duration = LASTFM_SHUTDOWN_TIMEOUT
    .saturating_add(ANALYSIS_SHUTDOWN_TIMEOUT)
    .saturating_add(LIBRARY_SHUTDOWN_TIMEOUT)
    .saturating_add(Duration::from_secs(1));

struct BridgeLoopRuntime {
    imports: imports::ImportWorker,
    tree_worker: tree_worker::TreeWorker,
    queue_validator: queue_validation::QueueValidationWorker,
    edit_refreshes: edit_refresh::EditRefreshWorker,
    /// The [`crate::library::next_file_sequence`] number of the last rename
    /// applied.
    applied_rename_sequence: u64,
    analysis: AnalysisEngine,
    analysis_rx: Receiver<AnalysisEvent>,
    playback: PlaybackEngine,
    playback_rx: Receiver<PlaybackEvent>,
    metadata: MetadataService,
    metadata_rx: Receiver<MetadataEvent>,
    library: LibraryService,
    library_rx: Receiver<LibraryEvent>,
    search_query_tx: Sender<SearchWorkerQuery>,
    search_results_rx: Receiver<BridgeSearchResultsFrame>,
    apply_album_art_tx: Sender<ApplyAlbumArtRequest>,
    apply_album_art_rx: Receiver<ApplyAlbumArtEvent>,
    lastfm: LastFmHandle,
    lastfm_rx: Receiver<LastFmEvent>,
    state: BridgeState,
    lastfm_tracker: LastFmPlaybackTracker,
    flags: BridgeLoopFlags,
    last_settings_save: Instant,
    last_session_save: Instant,
    last_saved_session: Option<SessionSnapshot>,
    save_errors: SaveErrors,
    quit_save: QuitSave,
    playing_poll_interval: Duration,
    last_playing_poll: Instant,
    paused_poll_interval: Duration,
    last_paused_poll: Instant,
    diagnostics: BridgeLoopDiagnostics,
    analysis_snapshot_interval: Duration,
    playing_snapshot_interval: Duration,
    paused_snapshot_interval: Duration,
    last_snapshot_emit: Instant,
    snapshot_plan: BridgeSnapshotPlan,
    tree_emit_interval: Duration,
    tree_emit_min_track_delta: usize,
    last_tree_emit_at: Option<Instant>,
    /// Rows that changed since the tree was last requested; while a scan
    /// runs, the tree is rebuilt only for enough of them.
    rows_changed_since_tree: usize,
    /// Whether the library changed since the tree was last requested.
    tree_stale: bool,
    /// Whether the library changed since the search worker last got it.
    search_library_stale: bool,
    deferred_tree_rebuild_at: Option<Instant>,
    persisted_state_enabled: bool,
    /// How often each worker was restarted after it stopped.
    worker_restarts: HashMap<Worker, u32>,
    /// How often the loop woke because a worker had stopped.
    #[cfg(test)]
    stopped_worker_wakes: usize,
}

struct BridgeLoopFlags {
    running: bool,
    settings_dirty: bool,
    settings_save_timing: SettingsSaveTiming,
    session_dirty: bool,
    pending_snapshot: SnapshotUrgency,
}

struct BridgeLoopDiagnostics {
    profile_enabled: bool,
    profile_last: Instant,
    prof_snapshots_sent: usize,
    prof_snapshots_dropped: usize,
}

struct BridgeSnapshotPlan {
    include_tree_in_next_snapshot: bool,
}

/// When dirty settings are saved.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
enum SettingsSaveTiming {
    /// With the periodic save.
    #[default]
    Periodic,
    /// Without waiting for the periodic save.
    Now,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
enum SnapshotUrgency {
    #[default]
    None,
    Heartbeat,
    Analysis,
    Immediate,
}

impl SnapshotUrgency {
    fn is_pending(self) -> bool {
        !matches!(self, Self::None)
    }
}

enum BridgeLoopWake {
    Import(imports::ImportMessage),
    Tree(tree_worker::TreeResult),
    QueueValidated(queue_validation::ValidationResult),
    EditsRefreshed(edit_refresh::RefreshResult),
    Command(BridgeCommand),
    Playback(PlaybackEvent),
    Analysis(AnalysisEvent),
    Metadata(MetadataEvent),
    Library(LibraryEvent),
    SearchResults(BridgeSearchResultsFrame),
    ApplyAlbumArt(ApplyAlbumArtEvent),
    LastFm(LastFmEvent),
    /// A worker's channel to the loop closed: the worker thread has ended.
    WorkerStopped(Worker),
    Tick,
    Shutdown,
}

/// A thread that sends the bridge loop its results.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum Worker {
    Imports,
    LibraryTree,
    QueueValidation,
    EditRefresh,
    Playback,
    Analysis,
    Metadata,
    Library,
    Search,
    ApplyAlbumArt,
    LastFm,
}

impl Worker {
    /// What the user loses while the worker is stopped.
    fn feature(self) -> &'static str {
        match self {
            Self::Imports => "Adding files to the queue",
            Self::LibraryTree => "The library view",
            Self::QueueValidation => "Checking the queue's files",
            Self::EditRefresh => "Showing saved tag edits",
            Self::Playback => "Playback",
            Self::Analysis => "The waveform and spectrogram",
            Self::Metadata => "Track details",
            Self::Library => "The library",
            Self::Search => "Search",
            Self::ApplyAlbumArt => "Applying album art",
            Self::LastFm => "Last.fm scrobbling",
        }
    }

    /// The category of the errors that say the worker stopped.
    fn category(self) -> ErrorCategory {
        match self {
            Self::Imports | Self::QueueValidation | Self::Playback => ErrorCategory::Playback,
            Self::LibraryTree
            | Self::Metadata
            | Self::Library
            | Self::Search
            | Self::ApplyAlbumArt => ErrorCategory::Library,
            Self::EditRefresh => ErrorCategory::Tags,
            Self::Analysis => ErrorCategory::Analysis,
            Self::LastFm => ErrorCategory::LastFm,
        }
    }

    /// Whether a new thread can take over: it must hold no state that
    /// outlives a request, and the loop must own all of its channels.
    fn restartable(self) -> bool {
        matches!(
            self,
            Self::LibraryTree
                | Self::QueueValidation
                | Self::EditRefresh
                | Self::Search
                | Self::ApplyAlbumArt
        )
    }
}

/// How often a worker that keeps stopping is restarted before the loop
/// stops listening to it.
const MAX_WORKER_RESTARTS: u32 = 3;

/// The farthest the library view's top edge is saved from the top of the
/// row at it, in pixels. The rows' height follows the font size, so this
/// only rules out nonsense; the UI bounds it the same way.
pub(super) const MAX_LIBRARY_VIEW_ANCHOR_OFFSET: f32 = 4096.0;

fn env_duration_ms(var: &str, default: u64, min: u64, max: u64) -> Duration {
    Duration::from_millis(
        std::env::var(var)
            .ok()
            .and_then(|raw| raw.parse::<u64>().ok())
            .map_or(default, |v| v.clamp(min, max)),
    )
}

impl BridgeLoopRuntime {
    #[cfg(test)]
    fn new(options: BridgeRuntimeOptions, spectral_tx: Option<SpectrogramOutputRoute>) -> Self {
        Self::with_imports(options, spectral_tx, imports::ImportWorker::new())
    }

    fn with_imports(
        mut options: BridgeRuntimeOptions,
        spectral_tx: Option<SpectrogramOutputRoute>,
        imports: imports::ImportWorker,
    ) -> Self {
        let (analysis, analysis_rx) = AnalysisEngine::new_with_spectrogram_output(spectral_tx);
        let (playback, playback_rx) = PlaybackEngine::new(analysis.sender());
        let (metadata, metadata_rx) = MetadataService::new_with_delay(options.metadata_delay);
        #[cfg(test)]
        let (library, library_rx) = match options.library_db.take() {
            Some(path) => LibraryService::at_database(path),
            None => LibraryService::new(),
        };
        #[cfg(not(test))]
        let (library, library_rx) = LibraryService::new();
        let (search_query_tx, search_results_rx) = spawn_search_worker();
        let (apply_album_art_tx, apply_album_art_event_rx) = spawn_apply_album_art_worker();

        let (state, lastfm, lastfm_rx) =
            prepare_initial_bridge_state(&mut options, &analysis, &playback);
        #[cfg(test)]
        let watching = state.settings.library_watch_enabled && options.library_watching;
        #[cfg(not(test))]
        let watching = state.settings.library_watch_enabled;
        library.set_watching(watching);

        let playing_poll_interval = env_duration_ms("FERROUS_PLAYBACK_POLL_MS", 40, 8, 500);
        let paused_poll_interval =
            env_duration_ms("FERROUS_PLAYBACK_PAUSED_POLL_MS", 333, 125, 1000);
        let playing_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_PLAYING_HEARTBEAT_MS", 40, 16, 1000);
        let analysis_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_ANALYSIS_SNAPSHOT_MS", 16, 8, 1000);
        let paused_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_PAUSED_HEARTBEAT_MS", 333, 125, 1000);
        Self {
            imports,
            tree_worker: tree_worker::TreeWorker::new(),
            queue_validator: queue_validation::QueueValidationWorker::new(),
            edit_refreshes: edit_refresh::EditRefreshWorker::new(),
            applied_rename_sequence: 0,
            analysis,
            analysis_rx,
            playback,
            playback_rx,
            metadata,
            metadata_rx,
            library,
            library_rx,
            search_query_tx,
            search_results_rx,
            apply_album_art_tx,
            apply_album_art_rx: apply_album_art_event_rx,
            lastfm,
            lastfm_rx,
            state,
            lastfm_tracker: LastFmPlaybackTracker::default(),
            flags: BridgeLoopFlags {
                running: true,
                settings_dirty: false,
                settings_save_timing: SettingsSaveTiming::Periodic,
                session_dirty: false,
                pending_snapshot: SnapshotUrgency::None,
            },
            last_settings_save: Instant::now(),
            last_session_save: Instant::now(),
            last_saved_session: None,
            save_errors: SaveErrors::default(),
            quit_save: QuitSave::default(),
            playing_poll_interval,
            last_playing_poll: Instant::now()
                .checked_sub(playing_poll_interval)
                .unwrap_or_else(Instant::now),
            paused_poll_interval,
            last_paused_poll: Instant::now()
                .checked_sub(paused_poll_interval)
                .unwrap_or_else(Instant::now),
            diagnostics: BridgeLoopDiagnostics {
                profile_enabled: cfg!(feature = "profiling-logs")
                    && std::env::var_os("FERROUS_PROFILE").is_some(),
                profile_last: Instant::now(),
                prof_snapshots_sent: 0,
                prof_snapshots_dropped: 0,
            },
            analysis_snapshot_interval,
            playing_snapshot_interval,
            paused_snapshot_interval,
            last_snapshot_emit: Instant::now(),
            snapshot_plan: BridgeSnapshotPlan {
                include_tree_in_next_snapshot: true,
            },
            tree_emit_interval: scan_tree_emit_interval(),
            tree_emit_min_track_delta: scan_tree_emit_min_track_delta(),
            last_tree_emit_at: None,
            rows_changed_since_tree: 0,
            tree_stale: false,
            search_library_stale: false,
            deferred_tree_rebuild_at: None,
            persisted_state_enabled: options.persisted_state_enabled,
            worker_restarts: HashMap::new(),
            #[cfg(test)]
            stopped_worker_wakes: 0,
        }
    }

    /// Run until shutdown. Returns whether the settings, the session, and
    /// Last.fm's queue were saved then, and playback released its files.
    fn run(&mut self, cmd_rx: &Receiver<BridgeCommand>, event_tx: &Sender<BridgeEvent>) -> bool {
        self.emit_initial_snapshot(event_tx);
        while self.flags.running {
            let wake = self.wait_for_wake(cmd_rx);
            self.handle_wake(wake, event_tx);
            self.expire_quit_save(Instant::now(), event_tx);
            self.maybe_log_profile();
            self.maybe_persist(event_tx);
        }
        self.shutdown(event_tx)
    }

    fn emit_initial_snapshot(&mut self, event_tx: &Sender<BridgeEvent>) {
        if !self.emit_snapshot(event_tx) {
            self.flags.pending_snapshot = SnapshotUrgency::Immediate;
        }
    }

    fn wait_for_wake(&mut self, cmd_rx: &Receiver<BridgeCommand>) -> BridgeLoopWake {
        let wake_delay = self.next_wake_delay();
        select! {
            recv(&self.imports.results) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Imports), BridgeLoopWake::Import),
            recv(&self.tree_worker.results) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::LibraryTree), BridgeLoopWake::Tree),
            recv(&self.queue_validator.results) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::QueueValidation), BridgeLoopWake::QueueValidated),
            recv(&self.edit_refreshes.results) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::EditRefresh), BridgeLoopWake::EditsRefreshed),
            recv(cmd_rx) -> msg => {
                match msg {
                    Ok(cmd) => BridgeLoopWake::Command(cmd),
                    Err(_) => BridgeLoopWake::Shutdown,
                }
            }
            recv(&self.playback_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Playback), BridgeLoopWake::Playback),
            recv(&self.analysis_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Analysis), BridgeLoopWake::Analysis),
            recv(&self.metadata_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Metadata), BridgeLoopWake::Metadata),
            recv(&self.library_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Library), BridgeLoopWake::Library),
            recv(&self.search_results_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::Search), BridgeLoopWake::SearchResults),
            recv(&self.apply_album_art_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::ApplyAlbumArt), BridgeLoopWake::ApplyAlbumArt),
            recv(&self.lastfm_rx) -> msg => msg.map_or(BridgeLoopWake::WorkerStopped(Worker::LastFm), BridgeLoopWake::LastFm),
            recv(after(wake_delay)) -> _ => BridgeLoopWake::Tick,
        }
    }

    fn next_playback_poll_delay(&self) -> Option<Duration> {
        if self.state.playback.state == PlaybackState::Playing {
            return Some(
                self.playing_poll_interval
                    .saturating_sub(self.last_playing_poll.elapsed()),
            );
        }
        if self.state.playback.state == PlaybackState::Paused
            && self.state.playback.current.is_some()
        {
            return Some(
                self.paused_poll_interval
                    .saturating_sub(self.last_paused_poll.elapsed()),
            );
        }
        None
    }

    fn next_snapshot_emit_delay(&self) -> Option<Duration> {
        match self.flags.pending_snapshot {
            SnapshotUrgency::None => None,
            SnapshotUrgency::Immediate => Some(Duration::from_millis(8)),
            SnapshotUrgency::Analysis => Some(
                self.analysis_snapshot_interval
                    .saturating_sub(self.last_snapshot_emit.elapsed()),
            ),
            SnapshotUrgency::Heartbeat => self
                .snapshot_heartbeat_interval()
                .map(|interval| interval.saturating_sub(self.last_snapshot_emit.elapsed())),
        }
    }

    fn snapshot_heartbeat_interval(&self) -> Option<Duration> {
        match self.state.playback.state {
            PlaybackState::Playing => Some(self.playing_snapshot_interval),
            PlaybackState::Paused if self.state.playback.current.is_some() => {
                Some(self.paused_snapshot_interval)
            }
            _ => None,
        }
    }

    fn next_wake_delay(&self) -> Duration {
        let mut delay = Duration::from_secs(24 * 60 * 60);
        for candidate in [
            self.next_playback_poll_delay(),
            self.next_snapshot_emit_delay(),
            self.next_pending_search_retry_delay(),
            self.next_deferred_tree_rebuild_delay(),
            self.next_settings_save_delay(),
            self.next_session_save_delay(),
            self.quit_save
                .deadline
                .map(|deadline| deadline.saturating_duration_since(Instant::now())),
        ]
        .into_iter()
        .flatten()
        {
            delay = delay.min(candidate);
        }
        delay
    }

    fn next_pending_search_retry_delay(&self) -> Option<Duration> {
        self.state
            .pending_search_results
            .as_ref()
            .map(|_| Duration::from_millis(8))
    }

    fn next_deferred_tree_rebuild_delay(&self) -> Option<Duration> {
        self.deferred_tree_rebuild_at
            .map(|deadline| deadline.saturating_duration_since(Instant::now()))
    }

    fn next_settings_save_delay(&self) -> Option<Duration> {
        self.flags.settings_dirty.then(|| {
            if self.flags.settings_save_timing == SettingsSaveTiming::Now {
                Duration::ZERO
            } else {
                Duration::from_secs(2).saturating_sub(self.last_settings_save.elapsed())
            }
        })
    }

    fn next_session_save_delay(&self) -> Option<Duration> {
        self.flags
            .session_dirty
            .then(|| Duration::from_secs(2).saturating_sub(self.last_session_save.elapsed()))
    }

    fn handle_wake(&mut self, wake: BridgeLoopWake, event_tx: &Sender<BridgeEvent>) {
        let mut urgency = match wake {
            BridgeLoopWake::Import(imports::ImportMessage::Finished(result)) => {
                self.apply_import_result(result, event_tx)
            }
            BridgeLoopWake::Import(imports::ImportMessage::Renamed {
                number,
                renames,
                details,
                sequence,
            }) => self.apply_renamed_paths(number, renames, details, sequence, event_tx),
            BridgeLoopWake::Tree(result) => self.apply_tree_result(result),
            BridgeLoopWake::QueueValidated(result) => {
                let (changed, problem) = self.queue_validator.apply(result, &mut self.state);
                if let Some(problem) = problem {
                    eprintln!("[ferrous] {problem}");
                    let _ = try_send_event(
                        event_tx,
                        BridgeEvent::Error(BridgeError::warning(ErrorCategory::Playback, problem)),
                    );
                }
                if changed {
                    self.state.refresh_queue_rows();
                    self.flags.session_dirty = true;
                    SnapshotUrgency::Immediate
                } else {
                    SnapshotUrgency::None
                }
            }
            BridgeLoopWake::EditsRefreshed(result) => self.apply_edit_refresh(result, event_tx),
            BridgeLoopWake::Command(cmd) => self.handle_command(cmd, event_tx),
            BridgeLoopWake::Playback(event) => self.handle_playback_event(event, event_tx),
            BridgeLoopWake::Analysis(event) => self.handle_analysis_event(event, event_tx),
            BridgeLoopWake::Metadata(event) => self.handle_metadata_event(event),
            BridgeLoopWake::Library(event) => self.handle_library_events(Some(event)),
            BridgeLoopWake::SearchResults(frame) => self.handle_search_results(frame),
            BridgeLoopWake::ApplyAlbumArt(event) => {
                self.handle_apply_album_art_event(event, event_tx)
            }
            BridgeLoopWake::LastFm(event) => self.handle_lastfm_event(event, event_tx),
            BridgeLoopWake::WorkerStopped(worker) => {
                self.handle_stopped_worker(worker, event_tx);
                SnapshotUrgency::None
            }
            BridgeLoopWake::Tick => SnapshotUrgency::None,
            BridgeLoopWake::Shutdown => {
                self.flags.running = false;
                SnapshotUrgency::None
            }
        };
        urgency = urgency.max(self.drain_pending_updates(event_tx));
        self.read_patched_library_rows();
        self.note_snapshot_urgency(urgency);
        self.maybe_emit_pending_snapshot(event_tx);
    }

    /// Have the library worker read again the rows the bridge changed
    /// itself. Its changes reach the bridge as events, after any it sent
    /// before, so the bridge's copy ends with what the database has even if
    /// it applied a change, such as a rename's, after a newer one.
    fn read_patched_library_rows(&mut self) {
        if self.state.library_rows_to_read.is_empty() {
            return;
        }
        let mut paths = std::mem::take(&mut self.state.library_rows_to_read);
        paths.sort_unstable();
        paths.dedup();
        self.library.command(LibraryCommand::ReadRows(paths));
    }

    fn apply_import_result(
        &mut self,
        result: imports::ImportResult,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let changes = self.imports.complete(result);
        self.apply_queue_changes(changes, event_tx)
    }

    /// Apply the files renamed on disk by rename `number`. Imports that have
    /// no result yet read the files after the renames, so they are left
    /// alone; see [`imports::ImportGate`].
    fn apply_renamed_paths(
        &mut self,
        number: u64,
        renames: Arc<PathRenames>,
        mut details: imports::RenamedDetails,
        sequence: u64,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        self.imports.remap_pending_paths(number, &renames);
        // Refreshes and library events that read the files after the rename
        // keep their rows.
        self.edit_refreshes
            .renamed(&renames, sequence, &mut details);
        self.applied_rename_sequence = self.applied_rename_sequence.max(sequence);
        self.forget_applied_rows_once_renames_arrived();
        let current = self.state.playback.current.clone();
        if commands::refresh_renamed_paths(
            &mut self.state,
            renames,
            details,
            &self.playback,
            &self.analysis,
            &self.metadata,
            event_tx,
        ) {
            self.flags.session_dirty = true;
            self.state.refresh_queue_rows();
        }
        // Renaming the playing file continues its play. The tracker follows
        // the current path here, before the next tick could take the new
        // path for a new play.
        if self.lastfm_tracker.active_path == current {
            self.lastfm_tracker
                .active_path
                .clone_from(&self.state.playback.current);
        }
        self.request_tree();
        SnapshotUrgency::Immediate
    }

    /// Ask the edit refresh worker to read the files the tag editor saved.
    fn request_edit_refresh(&mut self, paths: Vec<PathBuf>, event_tx: &Sender<BridgeEvent>) {
        if paths.is_empty() {
            return;
        }
        if self.edit_refreshes.request(paths).is_none() {
            let _ = try_send_event(
                event_tx,
                BridgeEvent::Error(BridgeError::warning(
                    ErrorCategory::Tags,
                    "The library could not be updated with the saved tag edits; \
                     a rescan of the library will show them.",
                )),
            );
        }
    }

    fn apply_edit_refresh(
        &mut self,
        result: edit_refresh::RefreshResult,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let refresh = self.edit_refreshes.accept(result);
        self.forget_applied_rows_once_renames_arrived();
        if let Some(problem) = refresh.problem {
            let _ = try_send_event(
                event_tx,
                BridgeEvent::Error(BridgeError::warning(ErrorCategory::Tags, problem)),
            );
        }
        if !commands::apply_edited_details(
            &mut self.state,
            &refresh.indexed,
            &refresh.fingerprints,
            &self.metadata,
        ) {
            return SnapshotUrgency::None;
        }
        self.flags.session_dirty = true;
        self.state.refresh_queue_rows();
        self.request_tree();
        SnapshotUrgency::Immediate
    }

    /// The refresh worker keeps the rows applied for renames numbered below
    /// them whose notifications are still on their way. Once every rename
    /// sent has been applied here, none can be: a rename numbered later reads
    /// the files after those rows were read.
    fn forget_applied_rows_once_renames_arrived(&mut self) {
        if !self.renames_on_their_way() {
            self.edit_refreshes.forget_applied();
        }
    }

    fn renames_on_their_way(&self) -> bool {
        self.imports
            .renames_sent_after(self.applied_rename_sequence)
    }

    fn apply_queue_changes(
        &mut self,
        changes: Vec<imports::QueueChange>,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let mut urgency = SnapshotUrgency::None;
        for change in changes {
            if let Some(warning) = change.warning {
                let _ = try_send_event(
                    event_tx,
                    BridgeEvent::Error(BridgeError::warning(ErrorCategory::Playback, warning)),
                );
            }
            let Some(command) = change.command else {
                continue;
            };
            if queue::handle_queue_command(command, &mut self.state, &self.playback, event_tx) {
                self.flags.session_dirty = true;
                urgency = SnapshotUrgency::Immediate;
            }
        }
        urgency
    }

    /// Replace a worker whose thread ended with a new one, or stop listening
    /// to it, and tell the user. Either way its closed channel leaves the
    /// loop's `select!`, which would otherwise find it ready at once on every
    /// pass and never block again.
    fn handle_stopped_worker(&mut self, worker: Worker, event_tx: &Sender<BridgeEvent>) {
        #[cfg(test)]
        {
            self.stopped_worker_wakes += 1;
        }
        let restarts = self.worker_restarts.entry(worker).or_insert(0);
        let restart = worker.restartable() && *restarts < MAX_WORKER_RESTARTS;
        if restart {
            *restarts += 1;
        }
        match worker {
            Worker::LibraryTree if restart => {
                self.tree_worker = tree_worker::TreeWorker::new();
                // The tree the old worker was building is lost.
                self.request_tree();
            }
            Worker::LibraryTree => self.tree_worker.results = never(),
            Worker::QueueValidation if restart => {
                self.queue_validator = queue_validation::QueueValidationWorker::new();
                self.state.queue_detail_validation_requested = true;
            }
            Worker::QueueValidation => self.queue_validator.results = never(),
            Worker::EditRefresh if restart => {
                self.edit_refreshes.restart();
            }
            Worker::EditRefresh => self.edit_refreshes.results = never(),
            Worker::Search if restart => {
                (self.search_query_tx, self.search_results_rx) = spawn_search_worker();
            }
            Worker::Search => self.search_results_rx = never(),
            Worker::ApplyAlbumArt if restart => {
                (self.apply_album_art_tx, self.apply_album_art_rx) = spawn_apply_album_art_worker();
            }
            Worker::ApplyAlbumArt => self.apply_album_art_rx = never(),
            Worker::Imports => self.imports.results = never(),
            Worker::Playback => self.playback_rx = never(),
            Worker::Analysis => self.analysis_rx = never(),
            Worker::Metadata => self.metadata_rx = never(),
            Worker::Library => self.library_rx = never(),
            Worker::LastFm => {
                self.lastfm_rx = never();
                self.quit_save.lastfm_stopped = true;
                if let Some(request_id) = self.quit_save_waiting_for_lastfm() {
                    self.finish_quit_save(
                        request_id,
                        Some(
                            "Last.fm stopped unexpectedly; scrobbles it had not saved are lost."
                                .to_string(),
                        ),
                        event_tx,
                    );
                }
            }
        }
        let message = if restart {
            format!(
                "{} stopped unexpectedly and was restarted; \
                 a request in progress may have been lost.",
                worker.feature()
            )
        } else {
            format!(
                "{} stopped unexpectedly. Restart Ferrous to use it again.",
                worker.feature()
            )
        };
        eprintln!("[ferrous] {message}");
        let _ = try_send_event(
            event_tx,
            BridgeEvent::Error(BridgeError::error(worker.category(), message)),
        );
    }

    fn request_tree(&mut self) {
        self.tree_stale = false;
        self.rows_changed_since_tree = 0;
        self.tree_worker.request(
            Arc::clone(&self.state.library),
            self.state.library_status.scan_in_progress,
            self.state.settings.library_sort_mode,
            self.state.expanded_keys.clone(),
        );
    }

    fn apply_tree_result(&mut self, result: tree_worker::TreeResult) -> SnapshotUrgency {
        if !self.tree_worker.is_current(&result) {
            return SnapshotUrgency::None;
        }
        self.state.pre_built_tree_bytes = result.bytes;
        self.flags.session_dirty |= self.state.expanded_keys != result.expanded;
        self.state.expanded_keys = result.expanded;
        (
            self.state.library_artist_count,
            self.state.library_album_count,
        ) = result.counts;
        self.state.library_root_track_counts = result.root_track_counts;
        self.snapshot_plan.include_tree_in_next_snapshot = true;
        SnapshotUrgency::Immediate
    }

    fn handle_command(
        &mut self,
        cmd: BridgeCommand,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        #[cfg(test)]
        crate::panic_guard::test_hooks::checkpoint("bridge_command");
        let cmd = match cmd {
            BridgeCommand::QueueAt { revision, command } => {
                match self.state.queue_sync.resolve(revision, command) {
                    Ok(Some(command)) => BridgeCommand::Queue(command),
                    Ok(None) => return SnapshotUrgency::None,
                    Err(message) => {
                        let _ = try_send_event(
                            event_tx,
                            BridgeEvent::Error(BridgeError::error(
                                ErrorCategory::Playback,
                                message,
                            )),
                        );
                        return SnapshotUrgency::None;
                    }
                }
            }
            cmd => cmd,
        };
        if let Some(request) = commands::queue_request(&cmd, &self.state) {
            return match self.imports.submit(request) {
                Ok(changes) => self.apply_queue_changes(changes, event_tx),
                Err(error) => {
                    let _ = try_send_event(
                        event_tx,
                        BridgeEvent::Error(BridgeError::error(ErrorCategory::Playback, error)),
                    );
                    SnapshotUrgency::None
                }
            };
        }
        let cmd = match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::RefreshEditedPaths(paths)) => {
                self.request_edit_refresh(paths, event_tx);
                return SnapshotUrgency::None;
            }
            cmd => cmd,
        };
        match &cmd {
            BridgeCommand::Shutdown | BridgeCommand::Queue(BridgeQueueCommand::Clear) => {
                self.imports.discard_pending();
            }
            BridgeCommand::SaveState(request_id) => {
                self.save_state_for_quit(*request_id, event_tx);
                return SnapshotUrgency::None;
            }
            _ => {}
        }

        let session_only = matches!(
            &cmd,
            BridgeCommand::Library(BridgeLibraryCommand::SetViewState { .. })
        );
        let rebuild_tree =
            command_requires_tree_rebuild(&cmd, self.state.settings.library_sort_mode);
        let deferred_tree_rebuild = command_tree_rebuild_delay(&cmd);
        let force_snapshot = matches!(cmd, BridgeCommand::RequestSnapshot);
        let mut command_context = BridgeCommandContext {
            playback: &self.playback,
            analysis: &self.analysis,
            library: &self.library,
            lastfm: &self.lastfm,
            search_query_tx: &self.search_query_tx,
            apply_album_art_tx: &self.apply_album_art_tx,
            event_tx,
            running: &mut self.flags.running,
            settings_dirty: &mut self.flags.settings_dirty,
        };
        let changed = handle_bridge_command(cmd, &mut self.state, &mut command_context);
        let mut urgency = SnapshotUrgency::None;
        if rebuild_tree {
            self.request_tree();
            urgency = SnapshotUrgency::Immediate;
        } else if changed {
            if let Some(delay) = deferred_tree_rebuild {
                self.deferred_tree_rebuild_at = Some(Instant::now() + delay);
            }
        }
        if changed {
            self.flags.session_dirty = true;
            if !session_only {
                urgency = SnapshotUrgency::Immediate;
            }
        }
        if force_snapshot && self.flags.running {
            // The UI asks for a snapshot when it starts and when it lost
            // track of the queue, so it gets the queue whole.
            self.state.queue_sync.request_full();
            if !self.emit_snapshot(event_tx) {
                urgency = urgency.max(SnapshotUrgency::Immediate);
            }
        }
        urgency
    }

    fn handle_playback_event(
        &mut self,
        event: PlaybackEvent,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let urgency = process_playback_event(
            event,
            &self.analysis,
            &self.metadata,
            event_tx,
            &mut self.state,
        );
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
    }

    fn handle_analysis_event(
        &mut self,
        event: AnalysisEvent,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        match event {
            AnalysisEvent::PrecomputedSpectrogramChunk(chunk) => {
                note_precomputed_spectrogram_chunk(&mut self.state, &chunk);
                let _ = event_tx.send(BridgeEvent::PrecomputedSpectrogramChunk(chunk));
                SnapshotUrgency::None
            }
            AnalysisEvent::Snapshot(snapshot) => {
                let urgency = process_analysis_event(snapshot, &mut self.state);
                if urgency.is_pending() {
                    self.flags.session_dirty = true;
                }
                urgency
            }
            AnalysisEvent::WorkerFailed(message) => {
                let _ = try_send_event(
                    event_tx,
                    BridgeEvent::Error(BridgeError::error(ErrorCategory::Analysis, message)),
                );
                SnapshotUrgency::None
            }
        }
    }

    fn handle_metadata_event(&mut self, event: MetadataEvent) -> SnapshotUrgency {
        let urgency = process_metadata_event(event, &mut self.state);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
    }

    /// Apply `first` and the library events after it as one change.
    fn handle_library_events(&mut self, first: Option<LibraryEvent>) -> SnapshotUrgency {
        let events: Vec<LibraryEvent> = first
            .into_iter()
            .chain(self.library_rx.try_iter())
            .collect();
        // A rename on its way that read its files before these rows were
        // read must not replace them, nor older rows kept for it. Without
        // one, a rename sent later takes a higher number than theirs.
        if self.renames_on_their_way() {
            for event in &events {
                self.edit_refreshes
                    .library_changed(&event.changes, event.read_after);
            }
        } else {
            self.edit_refreshes.forget_applied();
        }
        let applied = apply_library_events(events, &mut self.state);
        self.handle_library_refresh(applied);
        applied.urgency()
    }

    fn handle_search_results(&mut self, frame: BridgeSearchResultsFrame) -> SnapshotUrgency {
        process_search_results(frame, &mut self.state);
        SnapshotUrgency::None
    }

    fn handle_apply_album_art_event(
        &mut self,
        event: ApplyAlbumArtEvent,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let urgency =
            process_apply_album_art_event(event, &self.metadata, event_tx, &mut self.state);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
            self.deferred_tree_rebuild_at = Some(Instant::now() + Duration::from_millis(150));
        }
        urgency
    }

    fn handle_lastfm_event(
        &mut self,
        event: LastFmEvent,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        match event {
            LastFmEvent::State(runtime) => {
                let urgency =
                    process_lastfm_state(runtime, &mut self.state, &mut self.flags, event_tx);
                if urgency.is_pending() {
                    self.flags.session_dirty = true;
                }
                // Last.fm tells the scrobbles it saved before it sends them,
                // which can wait for the network; a save that waits for them
                // need not wait for the network too.
                if !self.lastfm_may_hold_unsaved_scrobbles() {
                    if let Some(request_id) = self.quit_save_waiting_for_lastfm() {
                        self.finish_quit_save(request_id, None, event_tx);
                    }
                }
                urgency
            }
            LastFmEvent::SavedForQuit { request_id, result } => {
                self.finish_quit_save(request_id, result.err(), event_tx);
                SnapshotUrgency::None
            }
        }
    }

    /// Save the state for the UI to quit, and answer with what could not be
    /// saved.
    fn save_state_for_quit(&mut self, request_id: u32, event_tx: &Sender<BridgeEvent>) {
        let result = self.save_state_now();
        if self.lastfm_may_hold_unsaved_scrobbles() && !self.quit_save.lastfm_stopped {
            // Quitting could lose them, so the user is told before the
            // window closes. Last.fm saves on its own thread, after the
            // scrobbles sent before, and its answer completes this one.
            self.lastfm.command(LastFmCommand::SaveForQuit(request_id));
            self.quit_save.waiting_for_lastfm = Some((request_id, result));
            self.quit_save.deadline = Some(Instant::now() + QUIT_SAVE_LASTFM_TIMEOUT);
        } else {
            let _ = try_send_event(event_tx, BridgeEvent::StateSaved { request_id, result });
        }
    }

    /// Whether Last.fm holds scrobbles only in memory, or has not yet said
    /// whether it saved the latest ones it was sent.
    fn lastfm_may_hold_unsaved_scrobbles(&self) -> bool {
        self.state.lastfm.unsaved_scrobbles || self.unconfirmed_lastfm_scrobbles() > 0
    }

    fn unconfirmed_lastfm_scrobbles(&self) -> u64 {
        self.lastfm
            .scrobbles_sent()
            .saturating_sub(self.state.lastfm.scrobbles_received)
    }

    /// Answer a save before quitting that Last.fm has kept waiting past its
    /// deadline, with the scrobbles that may be lost: Last.fm may be waiting
    /// for the network, which quitting does not wait for.
    fn expire_quit_save(&mut self, now: Instant, event_tx: &Sender<BridgeEvent>) {
        if self
            .quit_save
            .deadline
            .is_none_or(|deadline| now < deadline)
        {
            return;
        }
        let Some(request_id) = self.quit_save_waiting_for_lastfm() else {
            self.quit_save.deadline = None;
            return;
        };
        let unconfirmed = self.unconfirmed_lastfm_scrobbles();
        let unsaved = if self.state.lastfm.unsaved_scrobbles {
            self.state.lastfm.pending_scrobble_count
        } else {
            0
        };
        let at_risk = u64::try_from(unsaved)
            .unwrap_or(u64::MAX)
            .saturating_add(unconfirmed);
        if at_risk == 0 {
            // Nothing quitting would lose.
            self.finish_quit_save(request_id, None, event_tx);
            return;
        }
        let problem = format!(
            "Last.fm: {at_risk} {} not saved and would be lost on quit. Last.fm is busy, \
             possibly waiting for the network, and could not save {} in time.",
            if at_risk == 1 {
                "scrobble is"
            } else {
                "scrobbles are"
            },
            if at_risk == 1 { "it" } else { "them" }
        );
        self.finish_quit_save(request_id, Some(problem), event_tx);
    }

    fn quit_save_waiting_for_lastfm(&self) -> Option<u32> {
        self.quit_save
            .waiting_for_lastfm
            .as_ref()
            .map(|(request_id, _)| *request_id)
    }

    /// Answer the save before quitting with that request ID, if it waits for
    /// Last.fm, with what Last.fm could not save.
    fn finish_quit_save(
        &mut self,
        request_id: u32,
        lastfm_problem: Option<String>,
        event_tx: &Sender<BridgeEvent>,
    ) {
        if self
            .quit_save
            .waiting_for_lastfm
            .as_ref()
            .is_none_or(|(waiting, _)| *waiting != request_id)
        {
            // A save that a retry has replaced.
            return;
        }
        let Some((_, saved)) = self.quit_save.waiting_for_lastfm.take() else {
            return;
        };
        self.quit_save.deadline = None;
        let failures: Vec<String> = saved.err().into_iter().chain(lastfm_problem).collect();
        let _ = try_send_event(
            event_tx,
            BridgeEvent::StateSaved {
                request_id,
                result: if failures.is_empty() {
                    Ok(())
                } else {
                    Err(failures.join("\n"))
                },
            },
        );
    }

    fn poll_playback_if_due(&mut self) {
        if self.state.playback.state == PlaybackState::Playing {
            if self.last_playing_poll.elapsed() >= self.playing_poll_interval {
                self.playback.command(PlaybackCommand::Poll);
                self.last_playing_poll = Instant::now();
            }
            return;
        }
        if self.state.playback.state == PlaybackState::Paused
            && self.state.playback.current.is_some()
            && self.last_paused_poll.elapsed() >= self.paused_poll_interval
        {
            self.playback.command(PlaybackCommand::Poll);
            self.last_paused_poll = Instant::now();
        }
    }

    fn drain_pending_updates(&mut self, event_tx: &Sender<BridgeEvent>) -> SnapshotUrgency {
        self.poll_playback_if_due();
        let mut urgency = self.pump_bridge_events(event_tx);
        self.maybe_rebuild_tree_after_deferred_update();
        if !flush_pending_search_results_event(event_tx, &mut self.state.pending_search_results)
            && self.state.pending_search_results.is_some()
        {
            urgency = urgency.max(SnapshotUrgency::Immediate);
        }
        urgency
    }

    fn pump_bridge_events(&mut self, event_tx: &Sender<BridgeEvent>) -> SnapshotUrgency {
        let mut urgency = drain_playback_events(
            &self.playback_rx,
            &self.analysis,
            &self.metadata,
            event_tx,
            &mut self.state,
        );
        let analysis_urgency = drain_analysis_events(&self.analysis_rx, event_tx, &mut self.state);
        let metadata_urgency = drain_metadata_events(&self.metadata_rx, &mut self.state);
        let library_urgency = self.handle_library_events(None);
        let apply_album_art_urgency = drain_apply_album_art_events(
            &self.apply_album_art_rx,
            &self.metadata,
            event_tx,
            &mut self.state,
        );
        let mut lastfm_urgency = SnapshotUrgency::None;
        for _ in 0..8 {
            let Ok(event) = self.lastfm_rx.try_recv() else {
                break;
            };
            lastfm_urgency = lastfm_urgency.max(self.handle_lastfm_event(event, event_tx));
        }
        drain_search_results(&self.search_results_rx, &mut self.state);
        tick_lastfm_playback(&self.state, &self.lastfm, &mut self.lastfm_tracker);
        if self.state.queue_detail_validation_requested {
            self.state.queue_detail_validation_requested = false;
            self.queue_validator.request(&self.state);
        }
        if apply_album_art_urgency.is_pending() {
            self.deferred_tree_rebuild_at = Some(Instant::now() + Duration::from_millis(150));
        }
        urgency = urgency
            .max(analysis_urgency)
            .max(metadata_urgency)
            .max(library_urgency)
            .max(apply_album_art_urgency)
            .max(lastfm_urgency);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
    }

    /// Rebuild the tree and the search index for library changes. While a
    /// scan runs, the tree is rebuilt at intervals, for enough changed rows,
    /// and the search index after the scan.
    fn handle_library_refresh(&mut self, applied: events::LibraryEventsApplied) {
        if applied.rows_changed > 0 {
            self.flags.session_dirty = true;
            self.deferred_tree_rebuild_at = None;
            self.state.refresh_queue_rows();
            self.tree_stale = true;
            self.search_library_stale = true;
            self.rows_changed_since_tree = self
                .rows_changed_since_tree
                .saturating_add(applied.rows_changed);
        }
        let scanning = self.state.library_status.scan_in_progress;
        if self.tree_stale {
            let scan_emit_due = self
                .last_tree_emit_at
                .is_none_or(|last| last.elapsed() >= self.tree_emit_interval);
            let should_emit_tree = !scanning
                || self.last_tree_emit_at.is_none()
                || (scan_emit_due
                    && self.rows_changed_since_tree >= self.tree_emit_min_track_delta);
            if should_emit_tree {
                self.request_tree();
                self.note_snapshot_urgency(SnapshotUrgency::Immediate);
            }
        }
        if self.search_library_stale && !scanning {
            self.search_library_stale = false;
            let _ = self.search_query_tx.send(SearchWorkerQuery {
                limit: 0,
                seq: 0,
                query: String::new(),
                library: Arc::clone(&self.state.library),
            });
        }
    }

    fn maybe_rebuild_tree_after_deferred_update(&mut self) {
        let Some(deadline) = self.deferred_tree_rebuild_at else {
            return;
        };
        if Instant::now() < deadline {
            return;
        }
        self.deferred_tree_rebuild_at = None;
        self.request_tree();
        self.note_snapshot_urgency(SnapshotUrgency::Immediate);
    }

    fn note_snapshot_urgency(&mut self, urgency: SnapshotUrgency) {
        self.flags.pending_snapshot = self.flags.pending_snapshot.max(urgency);
    }

    fn maybe_emit_pending_snapshot(&mut self, event_tx: &Sender<BridgeEvent>) {
        match self.flags.pending_snapshot {
            SnapshotUrgency::None => {}
            SnapshotUrgency::Immediate => {
                let _ = self.emit_snapshot(event_tx);
            }
            SnapshotUrgency::Analysis => {
                if self.last_snapshot_emit.elapsed() >= self.analysis_snapshot_interval {
                    let _ = self.emit_snapshot(event_tx);
                }
            }
            SnapshotUrgency::Heartbeat => {
                let Some(interval) = self.snapshot_heartbeat_interval() else {
                    return;
                };
                if self.last_snapshot_emit.elapsed() >= interval {
                    let _ = self.emit_snapshot(event_tx);
                }
            }
        }
    }

    fn emit_snapshot(&mut self, event_tx: &Sender<BridgeEvent>) -> bool {
        if send_snapshot_event(
            event_tx,
            &self.state,
            self.snapshot_plan.include_tree_in_next_snapshot,
        ) {
            self.diagnostics.prof_snapshots_sent += 1;
            if self.snapshot_plan.include_tree_in_next_snapshot {
                self.last_tree_emit_at = Some(Instant::now());
            }
            self.last_snapshot_emit = Instant::now();
            self.snapshot_plan.include_tree_in_next_snapshot = false;
            self.state.queue_sync.mark_sent();
            self.flags.pending_snapshot = SnapshotUrgency::None;
            true
        } else {
            self.diagnostics.prof_snapshots_dropped += 1;
            self.flags.pending_snapshot =
                self.flags.pending_snapshot.max(SnapshotUrgency::Immediate);
            false
        }
    }

    fn maybe_log_profile(&mut self) {
        if !self.diagnostics.profile_enabled
            || self.diagnostics.profile_last.elapsed() < Duration::from_secs(1)
        {
            return;
        }
        profile_eprintln!(
            "[bridge] rss_kb={} playback_q={} analysis_q={} metadata_q={} library_q={} wave_len={} sent_snap/s={} drop_snap/s={}",
            current_rss_kb(),
            self.playback_rx.len(),
            self.analysis_rx.len(),
            self.metadata_rx.len(),
            self.library_rx.len(),
            self.state.analysis.waveform_peaks.len(),
            self.diagnostics.prof_snapshots_sent,
            self.diagnostics.prof_snapshots_dropped
        );
        self.diagnostics.prof_snapshots_sent = 0;
        self.diagnostics.prof_snapshots_dropped = 0;
        self.diagnostics.profile_last = Instant::now();
    }

    fn maybe_persist(&mut self, event_tx: &Sender<BridgeEvent>) {
        let save_now =
            std::mem::take(&mut self.flags.settings_save_timing) == SettingsSaveTiming::Now;
        if !self.persisted_state_enabled {
            self.flags.settings_dirty = false;
            self.flags.session_dirty = false;
            return;
        }
        // A failed save stays dirty, so the periodic save tries it again.
        if self.flags.settings_dirty
            && (save_now || self.last_settings_save.elapsed() >= Duration::from_secs(2))
        {
            let saved = self.save_settings_reported(event_tx);
            self.flags.settings_dirty = !saved;
            self.last_settings_save = Instant::now();
        }
        if !self.flags.session_dirty || self.last_session_save.elapsed() < Duration::from_secs(2) {
            return;
        }
        let session = session_snapshot_for_state(&self.state);
        let mut saved = true;
        if self.last_saved_session.as_ref() != Some(&session) {
            saved = report_save(
                save_session_snapshot(&session),
                &mut self.save_errors.session,
                event_tx,
            );
            if saved {
                self.last_saved_session = Some(session);
            }
        }
        self.flags.session_dirty = !saved;
        self.last_session_save = Instant::now();
    }

    fn save_settings_reported(&mut self, event_tx: &Sender<BridgeEvent>) -> bool {
        report_save(
            save_settings(&self.state.settings),
            &mut self.save_errors.settings,
            event_tx,
        )
    }

    /// Save the settings and the session now, for the UI to close the
    /// window only once they are on disk. Returns what to show the user if
    /// either save failed.
    fn save_state_now(&mut self) -> Result<(), String> {
        if !self.persisted_state_enabled {
            return Ok(());
        }
        let mut failures = Vec::new();
        match save_settings(&self.state.settings) {
            Ok(()) => {
                self.flags.settings_dirty = false;
                self.save_errors.settings = None;
            }
            Err(failure) => failures.push(failure.message),
        }
        let session = session_snapshot_for_state(&self.state);
        match save_session_snapshot(&session) {
            Ok(()) => {
                self.flags.session_dirty = false;
                self.save_errors.session = None;
                self.last_saved_session = Some(session);
            }
            Err(failure) => failures.push(failure.message),
        }
        if failures.is_empty() {
            Ok(())
        } else {
            Err(failures.join("\n"))
        }
    }

    /// Save the state and stop the workers. Returns whether the settings,
    /// the session, and Last.fm's queue were saved, and playback stopped
    /// with every file released.
    fn shutdown(&mut self, event_tx: &Sender<BridgeEvent>) -> bool {
        let mut saved = true;
        if self.persisted_state_enabled {
            saved &= self.save_settings_reported(event_tx);
            saved &= report_save(
                save_session_snapshot(&session_snapshot_for_state(&self.state)),
                &mut self.save_errors.session,
                event_tx,
            );
        }
        finalize_lastfm_track(
            &self.state,
            &self.lastfm,
            &mut self.lastfm_tracker,
            Instant::now(),
        );
        // A file whose read has not returned stays open after the app quits.
        if let Some(message) = self.playback.shutdown(PLAYBACK_SHUTDOWN_TIMEOUT).problem() {
            eprintln!("[ferrous] {message}");
            let _ = try_send_event(
                event_tx,
                BridgeEvent::Error(BridgeError::error(ErrorCategory::Playback, message)),
            );
            saved = false;
        }
        let lastfm_problem = match self.lastfm.shutdown(LASTFM_SHUTDOWN_TIMEOUT) {
            LastFmShutdownOutcome::Stopped => None,
            LastFmShutdownOutcome::SaveFailed(message) => Some(message),
            LastFmShutdownOutcome::Unconfirmed => Some(
                "Last.fm did not confirm shutdown; the latest scrobble may not have been saved."
                    .to_string(),
            ),
        };
        if let Some(message) = lastfm_problem {
            eprintln!("{message}");
            let _ = try_send_event(
                event_tx,
                BridgeEvent::Error(BridgeError::error(ErrorCategory::LastFm, message)),
            );
            saved = false;
        }
        let _ = try_send_event(event_tx, BridgeEvent::Stopped);
        saved
    }
}

/// What a save before quitting waits for.
#[derive(Default)]
struct QuitSave {
    /// A save that waits for Last.fm to save scrobbles it holds only in
    /// memory: its request ID, and how saving the settings and the session
    /// went.
    waiting_for_lastfm: Option<(u32, Result<(), String>)>,
    /// Set once the Last.fm service has stopped unexpectedly; nothing waits
    /// for it then.
    lastfm_stopped: bool,
    /// When the waiting save is answered without Last.fm.
    deadline: Option<Instant>,
}

/// The save failures last shown to the user, so a save that keeps failing
/// the same way is shown once.
#[derive(Default)]
struct SaveErrors {
    settings: Option<(PathBuf, std::io::ErrorKind)>,
    session: Option<(PathBuf, std::io::ErrorKind)>,
}

/// Show the user a failed save, unless `reported` says the same file failed
/// the same way last time. Returns whether the save succeeded.
fn report_save(
    result: Result<(), SaveFailure>,
    reported: &mut Option<(PathBuf, std::io::ErrorKind)>,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let Err(failure) = result else {
        *reported = None;
        return true;
    };
    if reported.as_ref() != Some(&failure.identity) {
        eprintln!("[ferrous] {}", failure.message);
        let error = BridgeError::error(ErrorCategory::Settings, failure.message)
            .about(failure.identity.0.to_string_lossy());
        if try_send_event(event_tx, BridgeEvent::Error(error)).is_ok() {
            *reported = Some(failure.identity);
        }
    }
    false
}

fn prepare_initial_bridge_state(
    options: &mut BridgeRuntimeOptions,
    analysis: &AnalysisEngine,
    playback: &PlaybackEngine,
) -> (BridgeState, LastFmHandle, Receiver<LastFmEvent>) {
    let mut state = BridgeState::default();
    if options.persisted_state_enabled {
        load_settings_into(&mut state.settings);
    }
    state.lastfm.enabled = state.settings.integrations.lastfm_scrobbling_enabled;
    #[cfg(test)]
    let (lastfm, lastfm_rx) = options
        .lastfm
        .take()
        .unwrap_or_else(|| spawn_lastfm_service(&state.settings));
    #[cfg(not(test))]
    let (lastfm, lastfm_rx) = spawn_lastfm_service(&state.settings);
    restore_initial_bridge_state(
        &mut state,
        analysis,
        playback,
        &lastfm,
        options.persisted_state_enabled,
    );
    (state, lastfm, lastfm_rx)
}

fn restore_initial_bridge_state(
    state: &mut BridgeState,
    analysis: &AnalysisEngine,
    playback: &PlaybackEngine,
    lastfm: &LastFmHandle,
    persisted_state_enabled: bool,
) {
    state.playback.volume = state.settings.volume;
    playback.command(PlaybackCommand::SetVolume(state.settings.volume));
    // Display mode first so self.display_mode is correct before
    // SetFftSize / SetSpectrogramViewMode restart the session.
    analysis.command(AnalysisCommand::SetSpectrogramDisplayMode(
        state.settings.spectrogram_display_mode,
    ));
    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));
    analysis.command(AnalysisCommand::SetSpectrogramViewMode(
        state.settings.spectrogram_view_mode,
    ));
    lastfm.command(LastFmCommand::SetEnabled(
        state.settings.integrations.lastfm_scrobbling_enabled,
    ));
    if persisted_state_enabled {
        apply_session_restore(state, playback, load_session_snapshot().as_ref());
    }
    if should_sync_queue_details_on_initial_restore(state) {
        sync_queue_details(state);
    }
}

fn should_sync_queue_details_on_initial_restore(state: &BridgeState) -> bool {
    !state.queue.is_empty() && !state.library.tracks.is_empty()
}

#[cfg_attr(
    not(feature = "profiling-logs"),
    allow(unused_variables, unused_assignments)
)]
fn run_bridge_loop(
    cmd_rx: &Receiver<BridgeCommand>,
    event_tx: &Sender<BridgeEvent>,
    options: BridgeRuntimeOptions,
    spectral_tx: SpectrogramOutputRoute,
    imports: imports::ImportWorker,
) -> bool {
    let ran = crate::panic_guard::catch("the bridge thread", || {
        let mut runtime = BridgeLoopRuntime::with_imports(options, Some(spectral_tx), imports);
        runtime.run(cmd_rx, event_tx)
    });
    // The runtime was dropped while unwinding, which stopped its workers.
    // The relay shows the UI this reason.
    match ran {
        Ok(saved) => saved,
        Err(message) => {
            let _ = try_send_event(
                event_tx,
                BridgeEvent::Failed(format!(
                    "The Ferrous backend stopped after an {message}. Restart Ferrous to continue."
                )),
            );
            false
        }
    }
}

/// Events waiting for the relay past which the bridge holds back the ones a
/// newer one replaces, so a relay that falls behind cannot grow memory.
const MAX_RELAY_BACKLOG: usize = 32;

/// The bridge's channel to the relay. It is unbounded, so the events the UI
/// must receive, such as errors and the answers to save requests, never
/// wait or drop; [`try_send_event`] bounds the rest.
fn event_channel() -> (Sender<BridgeEvent>, Receiver<BridgeEvent>) {
    unbounded()
}

/// Whether a newer event of the same kind replaces this one, so it can be
/// held back while the relay is behind.
fn replaced_by_newer(event: &BridgeEvent) -> bool {
    matches!(
        event,
        BridgeEvent::Snapshot(_) | BridgeEvent::SearchResults(_)
    )
}

fn relay_is_behind(event_tx: &Sender<BridgeEvent>) -> bool {
    event_tx.is_full() || event_tx.len() >= MAX_RELAY_BACKLOG
}

/// Send an event to the relay without waiting: the bridge thread drives
/// playback. An event a newer one replaces is refused while the relay is
/// behind.
pub(super) fn try_send_event(
    event_tx: &Sender<BridgeEvent>,
    event: BridgeEvent,
) -> Result<(), TrySendError<BridgeEvent>> {
    if replaced_by_newer(&event) && relay_is_behind(event_tx) {
        return Err(TrySendError::Full(event));
    }
    event_tx.try_send(event)
}

fn send_snapshot_event(
    event_tx: &Sender<BridgeEvent>,
    state: &BridgeState,
    include_tree: bool,
) -> bool {
    // Drop stale snapshot updates when the consumer is behind; next snapshot will replace it.
    if relay_is_behind(event_tx) {
        return false;
    }
    try_send_event(
        event_tx,
        BridgeEvent::Snapshot(Box::new(state.snapshot(include_tree))),
    )
    .is_ok()
}

fn flush_pending_search_results_event(
    event_tx: &Sender<BridgeEvent>,
    pending_search_results: &mut Option<BridgeSearchResultsFrame>,
) -> bool {
    let Some(frame) = pending_search_results.take() else {
        return false;
    };

    match try_send_event(event_tx, BridgeEvent::SearchResults(Box::new(frame))) {
        Ok(()) => true,
        Err(TrySendError::Full(event)) => {
            if let BridgeEvent::SearchResults(frame) = event {
                *pending_search_results = Some(*frame);
            }
            false
        }
        Err(TrySendError::Disconnected(_)) => false,
    }
}

fn scan_tree_emit_interval() -> Duration {
    let interval_ms = std::env::var("FERROUS_BRIDGE_SCAN_TREE_MS")
        .ok()
        .and_then(|raw| raw.parse::<u64>().ok())
        .map_or(250, |v| v.clamp(50, 5000));
    Duration::from_millis(interval_ms)
}

fn scan_tree_emit_min_track_delta() -> usize {
    std::env::var("FERROUS_BRIDGE_SCAN_TREE_MIN_TRACK_DELTA")
        .ok()
        .and_then(|raw| raw.parse::<usize>().ok())
        .map_or(256, |v| v.clamp(16, 50_000))
}

fn command_requires_tree_rebuild(cmd: &BridgeCommand, current_sort_mode: LibrarySortMode) -> bool {
    match cmd {
        BridgeCommand::Settings(BridgeSettingsCommand::SetLibrarySortMode(mode)) => {
            *mode != current_sort_mode
        }
        BridgeCommand::Settings(BridgeSettingsCommand::LoadFromDisk)
        | BridgeCommand::Library(
            BridgeLibraryCommand::SetNodeExpanded { .. }
            | BridgeLibraryCommand::ShowTrackInLibrary(_),
        ) => true,
        _ => false,
    }
}

fn command_tree_rebuild_delay(cmd: &BridgeCommand) -> Option<Duration> {
    match cmd {
        BridgeCommand::Library(BridgeLibraryCommand::ApplyAlbumArt { .. }) => {
            Some(Duration::from_millis(350))
        }
        _ => None,
    }
}

#[cfg(feature = "profiling-logs")]
fn current_rss_kb() -> usize {
    let Ok(status) = std::fs::read_to_string("/proc/self/status") else {
        return 0;
    };
    for line in status.lines() {
        if let Some(rest) = line.strip_prefix("VmRSS:") {
            if let Some(num) = rest.split_whitespace().next() {
                if let Ok(v) = num.parse::<usize>() {
                    return v;
                }
            }
        }
    }
    0
}

fn resolve_uniform_year<I>(years: I) -> Option<i32>
where
    I: IntoIterator<Item = Option<i32>>,
{
    let mut resolved = None;
    for year in years {
        let year = year?;
        match resolved {
            Some(existing) if existing != year => return None,
            Some(_) => {}
            None => resolved = Some(year),
        }
    }
    resolved
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::{LibraryRoot, LibraryTrack};
    use std::path::Path;
    use std::sync::{Mutex, MutexGuard, OnceLock};
    use std::time::Instant;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    fn library_track(
        path: &str,
        root: &PathBuf,
        artist: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
    ) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: p(path),
            root_path: root.clone(),
            title: String::new(),
            artist: artist.to_string(),
            album: album.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            cover_read_at: crate::library::CoverReadAt::default(),
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

    pub(super) fn test_guard() -> MutexGuard<'static, ()> {
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    fn error_messages(event_rx: &Receiver<BridgeEvent>) -> Vec<String> {
        event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Error(error) => Some(error.message),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn a_save_that_keeps_failing_the_same_way_is_reported_once() {
        let _config = crate::paths::TestConfigDir::new();
        let settings_path = config::settings_path().expect("settings path");
        let block = || {
            let _ = std::fs::remove_file(&settings_path);
            // Nothing can be renamed over a directory.
            std::fs::create_dir_all(&settings_path).expect("block the settings");
        };
        let settings = BridgeSettings::default();
        let (event_tx, event_rx) = unbounded();
        let mut reported = None;

        // Each attempt uses a temporary file with a new name.
        block();
        for _ in 0..3 {
            assert!(!report_save(
                save_settings(&settings),
                &mut reported,
                &event_tx
            ));
        }
        let messages = error_messages(&event_rx);
        assert_eq!(messages.len(), 1, "{messages:?}");

        // Once a save succeeds, the next failure is shown again.
        std::fs::remove_dir(&settings_path).expect("unblock the settings");
        assert!(report_save(
            save_settings(&settings),
            &mut reported,
            &event_tx
        ));
        block();
        assert!(!report_save(
            save_settings(&settings),
            &mut reported,
            &event_tx
        ));
        assert_eq!(error_messages(&event_rx), messages);
        std::fs::remove_dir(&settings_path).expect("unblock the settings");
    }

    #[test]
    fn library_view_state_keeps_offsets_into_rows_taller_than_the_default() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, _event_rx) = unbounded();
        let mut set_offset = |anchor_offset: f32| {
            runtime.handle_wake(
                BridgeLoopWake::Command(BridgeCommand::Library(
                    BridgeLibraryCommand::SetViewState {
                        selection_key: String::new(),
                        anchor_key: "album|/music|Artist|Album".to_string(),
                        anchor_offset,
                    },
                )),
                &event_tx,
            );
            runtime.state.library_view_anchor_offset
        };
        // Rows follow the font size, so the view's top can be well over
        // 24 pixels into the row at it.
        assert!((set_offset(40.0) - 40.0).abs() < f32::EPSILON);
        assert!((set_offset(-40.0) + 40.0).abs() < f32::EPSILON);
        assert!((set_offset(1e9) - MAX_LIBRARY_VIEW_ANCHOR_OFFSET).abs() < f32::EPSILON);
        assert!(set_offset(f32::NAN).abs() < f32::EPSILON);
    }

    #[test]
    fn a_stopped_worker_is_reported_once_and_does_not_wake_the_loop_again() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        // The import results channel closes, as when the worker thread ends.
        let (results_tx, results_rx) = unbounded();
        drop(results_tx);
        runtime.imports.results = results_rx;
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, event_rx) = unbounded();
        // A closed channel left in the loop's `select!` is ready on every
        // pass, so it would win about half of the passes that take these.
        for _ in 0..64 {
            cmd_tx.send(BridgeCommand::RequestSnapshot).unwrap();
        }
        cmd_tx.send(BridgeCommand::Shutdown).unwrap();

        runtime.run(&cmd_rx, &event_tx);

        assert_eq!(runtime.stopped_worker_wakes, 1);
        let errors = error_messages(&event_rx);
        let reports = errors
            .iter()
            .filter(|message| {
                message.starts_with("Adding files to the queue stopped unexpectedly.")
            })
            .count();
        assert_eq!(reports, 1, "{errors:?}");
    }

    #[test]
    fn a_stopped_worker_is_restarted_a_few_times_and_then_dropped() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = unbounded();
        for _ in 0..MAX_WORKER_RESTARTS {
            runtime.handle_stopped_worker(Worker::LibraryTree, &event_tx);
            // The new worker builds the tree the old one lost.
            let result = runtime
                .tree_worker
                .results
                .recv_timeout(Duration::from_secs(5))
                .expect("the restarted worker builds the tree");
            assert!(runtime.tree_worker.is_current(&result));
        }
        runtime.handle_stopped_worker(Worker::LibraryTree, &event_tx);

        let errors = error_messages(&event_rx);
        assert_eq!(errors.len(), 4, "{errors:?}");
        assert!(
            errors[..3]
                .iter()
                .all(|message| message.contains("was restarted")),
            "{errors:?}"
        );
        assert!(errors[3].contains("Restart Ferrous"), "{errors:?}");
        // The loop stops listening instead of finding a closed channel.
        assert!(matches!(
            runtime.tree_worker.results.try_recv(),
            Err(crossbeam_channel::TryRecvError::Empty)
        ));
    }

    #[test]
    fn a_panic_on_the_bridge_thread_reaches_the_ui_as_a_failure() {
        let _guard = test_guard();
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, event_rx) = unbounded();
        let (spectral_tx, _spectral_rx) = bounded(4);
        let route = SpectrogramOutputRoute {
            sender: spectral_tx,
            generation: Arc::new(AtomicU64::new(0)),
        };
        cmd_tx.send(BridgeCommand::RequestSnapshot).unwrap();
        crate::panic_guard::test_hooks::panic_at("bridge_command");

        run_bridge_loop(
            &cmd_rx,
            &event_tx,
            BridgeRuntimeOptions::default(),
            route,
            imports::ImportWorker::new(),
        );

        let failure = event_rx
            .try_iter()
            .find_map(|event| match event {
                BridgeEvent::Failed(message) => Some(message),
                _ => None,
            })
            .expect("the failure is reported");
        assert!(
            failure.contains("test panic at bridge_command"),
            "{failure}"
        );
    }

    /// Drain all initial background-thread events (playback engine startup
    /// snapshots, etc.) so that subsequent `handle_wake` calls in the test
    /// are not polluted by late-arriving initialisation events.
    ///
    /// A single `drain_pending_updates` is insufficient because background
    /// threads may not have sent their initial snapshot yet (thread
    /// scheduling).  We loop with short sleeps until a full drain cycle
    /// produces no new events, or bail after a generous timeout.
    fn drain_initial_events(
        runtime: &mut BridgeLoopRuntime,
        event_tx: &Sender<BridgeEvent>,
        event_rx: &Receiver<BridgeEvent>,
    ) {
        let deadline = Instant::now() + Duration::from_millis(2000);
        // Keep draining until two consecutive quiet cycles (no new events)
        // to handle slow CI where background threads haven't sent their
        // initial snapshot yet on the first attempt.
        let mut consecutive_quiet = 0u32;
        while Instant::now() < deadline {
            let _ = runtime.drain_pending_updates(event_tx);
            let had_events = event_rx.try_recv().is_ok();
            while event_rx.try_recv().is_ok() {}
            if had_events {
                consecutive_quiet = 0;
            } else {
                consecutive_quiet += 1;
                if consecutive_quiet >= 2 {
                    break;
                }
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        runtime.flags.pending_snapshot = SnapshotUrgency::None;
        while event_rx.try_recv().is_ok() {}
    }

    #[test]
    fn heartbeat_snapshots_share_queue_metadata_and_preserve_old_values() {
        let mut state = BridgeState::default();
        let path = PathBuf::from("/fixture/song.flac");
        Arc::make_mut(&mut state.queue_details).insert(
            path.clone(),
            IndexedTrack {
                search_tags: crate::library::SearchTags::default(),
                title: "Before".into(),
                artist: String::new(),
                album: String::new(),
                cover_path: String::new(),
                cover_stamp: 0,
                cover_read_at: crate::library::CoverReadAt::default(),
                genre: String::new(),
                year: None,
                track_no: None,
                duration_secs: Some(10.0),
            },
        );
        let first = state.snapshot(false);
        let second = state.snapshot(false);
        assert!(Arc::ptr_eq(&first.queue_details, &second.queue_details));
        assert!(Arc::ptr_eq(&first.queue_details, &state.queue_details));
        Arc::make_mut(&mut state.queue_details)
            .get_mut(&path)
            .expect("fixture track")
            .title = "After".into();
        assert_eq!(first.queue_details[&path].title, "Before");
        assert_eq!(state.snapshot(false).queue_details[&path].title, "After");
    }

    #[test]
    fn initial_restore_skips_queue_detail_sync_until_library_is_loaded() {
        let mut state = BridgeState::default();
        state.queue = vec![p("/music/a.flac")];
        assert!(!should_sync_queue_details_on_initial_restore(&state));

        state.library = Arc::new(LibrarySnapshot {
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                search_tags: crate::library::SearchTags::default(),
                path: p("/music/a.flac"),
                ..LibraryTrack::default()
            }]),
            ..LibrarySnapshot::default()
        });
        assert!(should_sync_queue_details_on_initial_restore(&state));
    }

    #[test]
    fn default_test_runtime_ignores_persisted_session_snapshot() {
        let _guard = test_guard();
        let _config = crate::paths::TestConfigDir::new();
        save_session_snapshot(&SessionSnapshot {
            queue: vec![p("/music/a.flac")],
            selected_queue_index: Some(0),
            current_queue_index: Some(0),
            current_path: Some(p("/music/a.flac")),
            ..SessionSnapshot::default()
        })
        .expect("save the session");

        let runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);

        assert!(
            !runtime.persisted_state_enabled,
            "unit-test runtimes must not restore state written by earlier tests"
        );
        assert!(runtime.state.queue.is_empty());
        assert!(runtime.state.selected_queue_index.is_none());
        assert!(runtime.state.playback.current.is_none());
    }

    fn lastfm_connected_as(username: &str) -> LastFmEvent {
        LastFmEvent::State(crate::lastfm::RuntimeState {
            username: username.to_string(),
            ..crate::lastfm::RuntimeState::default()
        })
    }

    #[test]
    fn a_runtime_that_does_not_persist_state_does_not_save_a_new_lastfm_username() {
        let _guard = test_guard();
        let _config = crate::paths::TestConfigDir::new();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, _event_rx) = unbounded();

        runtime.handle_lastfm_event(lastfm_connected_as("tester"), &event_tx);
        runtime.maybe_persist(&event_tx);

        assert_eq!(
            runtime.state.settings.integrations.lastfm_username,
            "tester"
        );
        assert!(!config::settings_path().expect("settings path").exists());
    }

    #[test]
    fn a_new_lastfm_username_is_saved_at_once_and_a_failed_save_is_shown_and_retried() {
        let _guard = test_guard();
        let _config = crate::paths::TestConfigDir::new();
        let mut runtime = BridgeLoopRuntime::new(
            BridgeRuntimeOptions {
                persisted_state_enabled: true,
                ..BridgeRuntimeOptions::default()
            },
            None,
        );
        let (event_tx, event_rx) = unbounded();
        let settings_path = config::settings_path().expect("settings path");
        let saved_username = || {
            let text = std::fs::read_to_string(&settings_path).expect("read the settings");
            let mut settings = BridgeSettings::default();
            config::parse_settings_text(&mut settings, &text);
            settings.integrations.lastfm_username
        };

        // Without waiting for the periodic save.
        runtime.last_settings_save = Instant::now();
        runtime.handle_lastfm_event(lastfm_connected_as("first"), &event_tx);
        assert_eq!(runtime.next_settings_save_delay(), Some(Duration::ZERO));
        runtime.maybe_persist(&event_tx);
        assert_eq!(saved_username(), "first");
        assert!(!runtime.flags.settings_dirty);
        assert_eq!(error_messages(&event_rx), Vec::<String>::new());

        // Nothing can be renamed over a directory.
        std::fs::remove_file(&settings_path).expect("remove the settings");
        std::fs::create_dir(&settings_path).expect("block the settings");
        runtime.handle_lastfm_event(lastfm_connected_as("second"), &event_tx);
        runtime.maybe_persist(&event_tx);
        let messages = error_messages(&event_rx);
        assert_eq!(messages.len(), 1, "{messages:?}");
        assert!(
            messages[0].starts_with("Settings could not be saved"),
            "{messages:?}"
        );
        assert!(runtime.flags.settings_dirty);
        assert_eq!(
            runtime.flags.settings_save_timing,
            SettingsSaveTiming::Periodic
        );

        // The periodic save tries again.
        std::fs::remove_dir(&settings_path).expect("unblock the settings");
        runtime.last_settings_save = Instant::now() - Duration::from_secs(3);
        runtime.maybe_persist(&event_tx);
        assert_eq!(saved_username(), "second");
        assert!(!runtime.flags.settings_dirty);
    }

    fn wait_for_snapshot_matching<F>(
        bridge: &FrontendBridgeHandle,
        timeout: Duration,
        predicate: F,
    ) -> Option<BridgeSnapshot>
    where
        F: Fn(&BridgeSnapshot) -> bool,
    {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            if let Some(event) = bridge.recv_timeout(Duration::from_millis(30)) {
                if let BridgeEvent::Snapshot(snapshot) = event {
                    if predicate(&snapshot) {
                        return Some(*snapshot);
                    }
                }
            }
            while let Some(event) = bridge.try_recv() {
                if let BridgeEvent::Snapshot(snapshot) = event {
                    if predicate(&snapshot) {
                        return Some(*snapshot);
                    }
                }
            }
        }
        None
    }

    #[test]
    fn level_meter_command_updates_setting_and_marks_it_for_persistence() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, _event_rx) = bounded::<BridgeEvent>(32);
        for enabled in [false, true] {
            runtime.flags.settings_dirty = false;
            runtime.handle_command(
                BridgeCommand::Settings(BridgeSettingsCommand::SetShowLevelMeter(enabled)),
                &event_tx,
            );
            assert_eq!(runtime.state.settings.display.show_level_meter, enabled);
            assert!(runtime.flags.settings_dirty);
        }
    }

    #[test]
    fn shutdown_queues_an_eligible_scrobble_before_stopping_lastfm() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (lastfm, commands) = crate::lastfm::test_support::command_handle();
        runtime.lastfm = lastfm;
        let track = p("/music/last-of-session.flac");
        runtime.state.lastfm.enabled = true;
        runtime.state.playback.current = Some(track.clone());
        runtime.state.playback.state = PlaybackState::Playing;
        // A 40 s track qualifies after 20 s of listening.
        runtime.state.playback.duration = Duration::from_secs(40);
        runtime.state.metadata.source_path = Some(track.to_string_lossy().into_owned());
        runtime.state.metadata.artist = "Artist".to_string();
        runtime.state.metadata.title = "Track".to_string();
        // The last tick came before the play qualified.
        let last_tick = Instant::now()
            .checked_sub(Duration::from_secs(21))
            .expect("monotonic clock is past 21 s");
        events::tick_lastfm_playback_at(
            &runtime.state,
            &runtime.lastfm,
            &mut runtime.lastfm_tracker,
            last_tick,
            1_700_000_000,
        );
        let service = std::thread::spawn(move || {
            let mut received = Vec::new();
            while let Ok(command) = commands.recv_timeout(Duration::from_secs(5)) {
                received.push(match command {
                    LastFmCommand::SendNowPlaying(_) => "now playing",
                    LastFmCommand::QueueScrobble(_) => "scrobble",
                    LastFmCommand::Shutdown(done) => {
                        let _ = done.send(Ok(()));
                        received.push("shutdown");
                        break;
                    }
                    _ => "other",
                });
            }
            received
        });

        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        runtime.shutdown(&event_tx);

        assert_eq!(
            service.join().expect("join Last.fm stand-in"),
            vec!["now playing", "scrobble", "shutdown"]
        );
        assert!(matches!(event_rx.try_recv(), Ok(BridgeEvent::Stopped)));
    }

    #[test]
    fn quitting_while_lastfm_sends_a_scrobble_it_saved_does_not_wait_for_the_network() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = event_channel();
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let dir = crate::paths::test_root().join("quit-while-sending-a-saved-scrobble");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("fixture folder");
        let (transport, network) = crate::lastfm::test_support::BlockingTransport::new();
        let (lastfm, lastfm_events) =
            crate::lastfm::test_support::spawn_connected(transport, dir.join("lastfm_queue.json"));
        runtime.lastfm = lastfm.clone();
        // The test hands the bridge Last.fm's events itself, in order.
        runtime.lastfm_rx = never();

        lastfm.command(LastFmCommand::QueueScrobble(crate::lastfm::ScrobbleEntry {
            artist: "Artist".to_string(),
            track: "Track".to_string(),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
            timestamp_utc: 1_700_000_000,
        }));
        // The scrobble is saved, then its request waits for the network.
        network
            .started
            .recv_timeout(Duration::from_secs(5))
            .expect("the scrobble's request starts");
        assert!(dir.join("lastfm_queue.json").is_file());

        // Quitting before the bridge has heard of the save waits for it...
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(1)),
            &event_tx,
        );
        let state_saved = |event_rx: &Receiver<BridgeEvent>| {
            event_rx.try_iter().find_map(|event| match event {
                BridgeEvent::StateSaved { request_id, result } => Some((request_id, result)),
                _ => None,
            })
        };
        assert_eq!(state_saved(&event_rx), None);
        // ...and the save's confirmation settles it, while the request still
        // waits: no deadline, no dialog.
        for event in lastfm_events.try_iter() {
            runtime.handle_wake(BridgeLoopWake::LastFm(event), &event_tx);
        }
        assert_eq!(state_saved(&event_rx), Some((1, Ok(()))));
        assert_eq!(network.answered(), 0, "the request still waits");
        runtime.expire_quit_save(Instant::now() + QUIT_SAVE_LASTFM_TIMEOUT, &event_tx);
        assert_eq!(state_saved(&event_rx), None, "nothing is reported lost");

        network.release.send(()).expect("release the request");
        let _ = lastfm.shutdown(Duration::from_secs(5));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn quitting_while_lastfm_sends_a_scrobble_it_could_not_save_reports_the_loss() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = event_channel();
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let dir = crate::paths::test_root().join("quit-while-scrobbling");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("fixture folder");
        // A file where the queue's folder should be: every save fails.
        let blocked_folder = dir.join("config");
        std::fs::write(&blocked_folder, b"").expect("blocking file");
        let (transport, network) = crate::lastfm::test_support::BlockingTransport::new();
        let (lastfm, lastfm_events) = crate::lastfm::test_support::spawn_connected(
            transport,
            blocked_folder.join("lastfm_queue.json"),
        );
        runtime.lastfm = lastfm.clone();
        runtime.lastfm_rx = lastfm_events.clone();
        let state_saved = |event_rx: &Receiver<BridgeEvent>| {
            event_rx.try_iter().find_map(|event| match event {
                BridgeEvent::StateSaved { request_id, result } => Some((request_id, result)),
                _ => None,
            })
        };

        lastfm.command(LastFmCommand::QueueScrobble(crate::lastfm::ScrobbleEntry {
            artist: "Artist".to_string(),
            track: "Track".to_string(),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
            timestamp_utc: 1_700_000_000,
        }));
        // The failed save is told, then the scrobble's request waits for
        // the network.
        network
            .started
            .recv_timeout(Duration::from_secs(5))
            .expect("the scrobble's request starts");

        // Quitting now, whether or not the bridge has heard of the failed
        // save yet, waits for Last.fm rather than approving the quit.
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(1)),
            &event_tx,
        );
        assert_eq!(state_saved(&event_rx), None, "the save waits for Last.fm");
        for event in lastfm_events.try_iter() {
            runtime.handle_wake(BridgeLoopWake::LastFm(event), &event_tx);
        }
        assert!(runtime.state.lastfm.unsaved_scrobbles);
        assert_eq!(runtime.state.lastfm.scrobbles_received, 1);
        assert_eq!(
            state_saved(&event_rx),
            None,
            "Last.fm waits for the network"
        );

        // The save does not wait for the network: past its deadline it
        // reports the loss, and the UI keeps the window open.
        runtime.expire_quit_save(Instant::now() + QUIT_SAVE_LASTFM_TIMEOUT, &event_tx);
        match state_saved(&event_rx) {
            Some((1, Err(message))) => assert!(
                message.contains("1 scrobble is not saved and would be lost on quit"),
                "{message}"
            ),
            other => panic!("expected the loss, got {other:?}"),
        }

        // Last.fm's late answer settles nothing more.
        network.release.send(()).expect("release the request");
        let answer = lastfm_events
            .recv_timeout(Duration::from_secs(5))
            .expect("Last.fm answers");
        runtime.handle_wake(BridgeLoopWake::LastFm(answer), &event_tx);
        for event in lastfm_events.try_iter() {
            runtime.handle_wake(BridgeLoopWake::LastFm(event), &event_tx);
        }
        assert_eq!(state_saved(&event_rx), None);
        drop(network);
        let _ = lastfm.shutdown(Duration::from_secs(5));
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_save_before_quitting_reports_scrobbles_that_lastfm_holds_only_in_memory() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (lastfm, commands) = crate::lastfm::test_support::command_handle();
        runtime.lastfm = lastfm;
        // The runtime's own Last.fm service is not heard from.
        runtime.lastfm_rx = never();
        let (event_tx, event_rx) = event_channel();
        let state_saved = |event_rx: &Receiver<BridgeEvent>| {
            event_rx.try_iter().find_map(|event| match event {
                BridgeEvent::StateSaved { request_id, result } => Some((request_id, result)),
                _ => None,
            })
        };

        // With nothing held only in memory, Last.fm is not asked.
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(1)),
            &event_tx,
        );
        assert_eq!(state_saved(&event_rx), Some((1, Ok(()))));
        assert!(commands.try_recv().is_err());

        // A scrobble Last.fm has not said it saved is not taken as saved:
        // the save waits for Last.fm, which handles the scrobble first.
        runtime
            .lastfm
            .command(LastFmCommand::QueueScrobble(crate::lastfm::ScrobbleEntry {
                artist: "Artist".to_string(),
                track: "Track".to_string(),
                album: String::new(),
                track_number: None,
                duration_seconds: Some(200),
                timestamp_utc: 1_700_000_000,
            }));
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(10)),
            &event_tx,
        );
        assert!(matches!(
            commands.try_recv(),
            Ok(LastFmCommand::QueueScrobble(_))
        ));
        assert!(matches!(
            commands.try_recv(),
            Ok(LastFmCommand::SaveForQuit(10))
        ));
        assert_eq!(state_saved(&event_rx), None);
        // Once Last.fm says it received and saved it, and answers, the
        // save succeeds.
        let received = LastFmRuntimeState {
            scrobbles_received: 1,
            ..runtime.state.lastfm.clone()
        };
        runtime.handle_wake(
            BridgeLoopWake::LastFm(LastFmEvent::State(received)),
            &event_tx,
        );
        runtime.handle_wake(
            BridgeLoopWake::LastFm(LastFmEvent::SavedForQuit {
                request_id: 10,
                result: Ok(()),
            }),
            &event_tx,
        );
        assert_eq!(state_saved(&event_rx), Some((10, Ok(()))));
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(11)),
            &event_tx,
        );
        assert_eq!(
            state_saved(&event_rx),
            Some((11, Ok(()))),
            "nothing is left unconfirmed"
        );

        runtime.state.lastfm.unsaved_scrobbles = true;
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(2)),
            &event_tx,
        );
        assert!(
            matches!(commands.try_recv(), Ok(LastFmCommand::SaveForQuit(2))),
            "Last.fm saves them first"
        );
        assert_eq!(state_saved(&event_rx), None, "the answer waits for Last.fm");
        let lost = "Last.fm: 2 scrobbles were not saved and would be lost on quit.";
        runtime.handle_wake(
            BridgeLoopWake::LastFm(LastFmEvent::SavedForQuit {
                request_id: 2,
                result: Err(lost.to_string()),
            }),
            &event_tx,
        );
        assert_eq!(state_saved(&event_rx), Some((2, Err(lost.to_string()))));

        // A retry replaces the save it retries; Last.fm's answer to the
        // older one is not taken for it.
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(3)),
            &event_tx,
        );
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(4)),
            &event_tx,
        );
        for request_id in [3, 4] {
            runtime.handle_wake(
                BridgeLoopWake::LastFm(LastFmEvent::SavedForQuit {
                    request_id,
                    result: Ok(()),
                }),
                &event_tx,
            );
        }
        assert_eq!(state_saved(&event_rx), Some((4, Ok(()))));
        assert_eq!(state_saved(&event_rx), None);

        // A Last.fm service that stops answers the save that waits for it.
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(5)),
            &event_tx,
        );
        runtime.handle_wake(BridgeLoopWake::WorkerStopped(Worker::LastFm), &event_tx);
        assert!(matches!(
            state_saved(&event_rx),
            Some((5, Err(message))) if message.contains("Last.fm stopped unexpectedly")
        ));
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(6)),
            &event_tx,
        );
        assert_eq!(state_saved(&event_rx), Some((6, Ok(()))));
    }

    #[test]
    fn lastfm_problems_are_shown_once_each_as_lastfm_errors() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = event_channel();
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let state = LastFmRuntimeState {
            status_text: "Last.fm session expired.".to_string(),
            problems: vec![
                crate::lastfm::Problem {
                    message: "Last.fm session expired.".to_string(),
                    warning: false,
                },
                crate::lastfm::Problem {
                    message: "Last.fm is unreachable; Ferrous keeps retrying.".to_string(),
                    warning: true,
                },
            ],
            ..LastFmRuntimeState::default()
        };
        runtime.handle_wake(BridgeLoopWake::LastFm(LastFmEvent::State(state)), &event_tx);

        let errors: Vec<BridgeError> = event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Error(error) => Some(error),
                _ => None,
            })
            .collect();
        assert_eq!(
            errors,
            [
                BridgeError::error(ErrorCategory::LastFm, "Last.fm session expired."),
                BridgeError::warning(
                    ErrorCategory::LastFm,
                    "Last.fm is unreachable; Ferrous keeps retrying."
                ),
            ]
        );
        // The bridge keeps the state without them, so they are not sent again.
        assert!(runtime.state.lastfm.problems.is_empty());
        assert_eq!(runtime.state.lastfm.status_text, "Last.fm session expired.");
    }

    #[test]
    fn shutdown_reports_scrobbles_that_lastfm_could_not_save() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (lastfm, commands) = crate::lastfm::test_support::command_handle();
        runtime.lastfm = lastfm;
        let service = std::thread::spawn(move || {
            while let Ok(command) = commands.recv_timeout(Duration::from_secs(5)) {
                if let LastFmCommand::Shutdown(done) = command {
                    let _ = done.send(Err("Last.fm: 2 scrobbles were lost on quit.".to_string()));
                    break;
                }
            }
        });

        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        runtime.shutdown(&event_tx);
        service.join().expect("join Last.fm stand-in");

        assert!(matches!(
            event_rx.try_recv(),
            Ok(BridgeEvent::Error(error)) if error.message == "Last.fm: 2 scrobbles were lost on quit."
                && error.category == ErrorCategory::LastFm
        ));
        assert!(matches!(event_rx.try_recv(), Ok(BridgeEvent::Stopped)));
    }

    #[test]
    fn renaming_the_playing_file_continues_its_lastfm_play() {
        let _guard = test_guard();
        let (mut runtime, _engine_rx, event_tx, _album) = queue_order_runtime();
        let (lastfm, commands) = crate::lastfm::test_support::command_handle();
        runtime.lastfm = lastfm;
        let (old_path, new_path) = (p("/music/old name.flac"), p("/music/new name.flac"));
        runtime.state.queue = vec![old_path.clone()];
        runtime.state.lastfm.enabled = true;
        runtime.state.playback.current = Some(old_path.clone());
        runtime.state.playback.current_queue_index = Some(0);
        runtime.state.playback.state = PlaybackState::Playing;
        // A 600 s track qualifies after 240 s of listening.
        runtime.state.playback.duration = Duration::from_secs(600);
        runtime.state.metadata.source_path = Some(old_path.to_string_lossy().into_owned());
        runtime.state.metadata.artist = "Artist".to_string();
        runtime.state.metadata.title = "Track".to_string();
        runtime.state.last_track_change.path = Some(old_path.clone());
        let start = Instant::now();
        let tick = |runtime: &mut BridgeLoopRuntime, seconds| {
            events::tick_lastfm_playback_at(
                &runtime.state,
                &runtime.lastfm,
                &mut runtime.lastfm_tracker,
                start + Duration::from_secs(seconds),
                1_700_000_000 + i64::try_from(seconds).expect("seconds fit"),
            );
        };
        tick(&mut runtime, 0);

        runtime.apply_renamed_paths(
            1,
            Arc::new(PathRenames::from([(old_path, new_path.clone())])),
            imports::RenamedDetails::default(),
            crate::library::next_file_sequence(),
            &event_tx,
        );
        assert_eq!(runtime.state.playback.current.as_ref(), Some(&new_path));
        tick(&mut runtime, 120);
        tick(&mut runtime, 240);
        tick(&mut runtime, 300);

        let received = commands
            .try_iter()
            .map(|command| match command {
                LastFmCommand::SendNowPlaying(_) => "now playing".to_string(),
                LastFmCommand::QueueScrobble(entry) => format!("scrobble {}", entry.timestamp_utc),
                other => format!("{other:?}"),
            })
            .collect::<Vec<_>>();
        assert_eq!(received, vec!["now playing", "scrobble 1700000000"]);
    }

    /// A runtime whose playback commands the test receives instead of an
    /// engine, with an album "Artist - Album" of two tracks in its library.
    fn queue_order_runtime() -> (
        BridgeLoopRuntime,
        Receiver<PlaybackCommand>,
        Sender<BridgeEvent>,
        Vec<PathBuf>,
    ) {
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let (engine_tx, engine_rx) = unbounded();
        runtime.playback = PlaybackEngine::from_command_sender(engine_tx);
        let album = vec![p("/library/Album/1.flac"), p("/library/Album/2.flac")];
        let root = p("/library");
        runtime.state.library = Arc::new(LibrarySnapshot {
            tracks: album
                .iter()
                .map(|path| {
                    library_track(
                        &path.to_string_lossy(),
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
        });
        (runtime, engine_rx, event_tx, album)
    }

    fn append_album() -> BridgeCommand {
        BridgeCommand::Library(BridgeLibraryCommand::AppendAlbumByKey {
            artist: "Artist".to_string(),
            album: "Album".to_string(),
        })
    }

    /// A track file for imports, which expand and canonicalize their paths.
    fn import_fixture(name: &str) -> (PathBuf, PathBuf) {
        let dir =
            std::env::temp_dir().join(format!("ferrous-queue-order-{name}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("fixture dir");
        let track = dir.join("track.flac");
        std::fs::write(&track, b"stub").expect("fixture track");
        let track = std::fs::canonicalize(track).expect("canonical track");
        (dir, track)
    }

    fn apply_next_import_result(runtime: &mut BridgeLoopRuntime, event_tx: &Sender<BridgeEvent>) {
        let result = runtime
            .imports
            .results
            .recv_timeout(Duration::from_secs(5))
            .expect("import result");
        runtime.handle_wake(BridgeLoopWake::Import(result), event_tx);
    }

    fn wait_for_import_result(runtime: &BridgeLoopRuntime) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while runtime.imports.results.is_empty() {
            assert!(Instant::now() < deadline, "import result");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn queue_ops(engine_rx: &Receiver<PlaybackCommand>) -> Vec<String> {
        engine_rx
            .try_iter()
            .map(|command| match command {
                PlaybackCommand::LoadQueue(tracks) => format!("load {tracks:?}"),
                PlaybackCommand::AddToQueue(tracks) => format!("add {tracks:?}"),
                PlaybackCommand::PlayAt(index) => format!("play at {index}"),
                PlaybackCommand::Play => "play".to_string(),
                other => format!("{other:?}"),
            })
            .collect()
    }

    #[test]
    fn play_track_then_append_album_keeps_both_in_order() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, album) = queue_order_runtime();
        let (dir, track) = import_fixture("play-then-append");

        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(track.clone())),
            &event_tx,
        );
        runtime.handle_command(append_album(), &event_tx);
        assert!(
            runtime.state.queue.is_empty(),
            "the append waits for the track to import"
        );
        apply_next_import_result(&mut runtime, &event_tx);

        let expected = [vec![track.clone()], album.clone()].concat();
        assert_eq!(runtime.state.queue, expected);
        assert_eq!(runtime.state.selected_queue_index, Some(0));
        assert_eq!(
            queue_ops(&engine_rx),
            vec![
                format!("load {:?}", vec![track]),
                "play at 0".to_string(),
                "play".to_string(),
                format!("add {album:?}"),
            ]
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn opening_thousands_of_files_queues_them_all_in_order_and_plays_the_first() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _) = queue_order_runtime();
        let dir = std::env::temp_dir().join(format!(
            "ferrous-queue-order-open-many-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).expect("fixture dir");
        // Reverse order, so a sort anywhere on the way would show.
        let tracks: Vec<PathBuf> = (0..5_000)
            .rev()
            .map(|i| {
                let track = dir.join(format!("{i:04} – Ärtist トラック.flac"));
                std::fs::write(&track, b"stub").expect("fixture track");
                std::fs::canonicalize(track).expect("canonical track")
            })
            .collect();

        // The command as the UI encodes it: a u16 id, a u32 payload length,
        // a u32 count, and u32-length strings.
        let mut payload = Vec::new();
        payload.extend_from_slice(&u32::try_from(tracks.len()).unwrap().to_le_bytes());
        for track in &tracks {
            ffi::push_string(&mut payload, &track.to_string_lossy());
        }
        assert!(payload.len() > usize::from(u16::MAX));
        let mut command = 15_u16.to_le_bytes().to_vec();
        command.extend_from_slice(&u32::try_from(payload.len()).unwrap().to_le_bytes());
        command.extend_from_slice(&payload);
        let parsed = ffi::parse_binary_command(&command)
            .expect("parse")
            .expect("command");

        runtime.handle_command(parsed, &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);

        assert_eq!(runtime.state.queue, tracks);
        assert_eq!(runtime.state.selected_queue_index, Some(0));
        assert_eq!(
            queue_ops(&engine_rx),
            vec![
                format!("load {tracks:?}"),
                "play at 0".to_string(),
                "play".to_string(),
            ],
            "one queue change, playing from the first file"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn add_track_then_append_album_keeps_their_order() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, album) = queue_order_runtime();
        let (dir, track) = import_fixture("add-then-append");
        let existing = p("/library/existing.flac");
        runtime.state.queue = vec![existing.clone()];

        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::AddTrack(track.clone())),
            &event_tx,
        );
        runtime.handle_command(append_album(), &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);

        assert_eq!(
            runtime.state.queue,
            [vec![existing, track.clone()], album.clone()].concat()
        );
        assert_eq!(
            queue_ops(&engine_rx),
            vec![format!("add {:?}", vec![track]), format!("add {album:?}")],
            "appends do not start playback"
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn replacement_discards_an_append_that_started_before_it() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, album) = queue_order_runtime();
        let (dir, track) = import_fixture("replace-after-append");

        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(vec![track])),
            &event_tx,
        );
        wait_for_import_result(&runtime);
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceAlbumByKey {
                artist: "Artist".to_string(),
                album: "Album".to_string(),
            }),
            &event_tx,
        );
        assert_eq!(runtime.state.queue, album);
        apply_next_import_result(&mut runtime, &event_tx);

        assert_eq!(runtime.state.queue, album, "the append was discarded");
        assert_eq!(
            queue_ops(&engine_rx),
            vec![
                format!("load {album:?}"),
                "play at 0".to_string(),
                "play".to_string(),
            ]
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn clearing_the_queue_discards_a_pending_import() {
        let _guard = test_guard();
        let (mut runtime, _engine_rx, event_tx, _album) = queue_order_runtime();
        let (dir, track) = import_fixture("clear-after-add");

        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::AddTrack(track)),
            &event_tx,
        );
        wait_for_import_result(&runtime);
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);

        assert!(runtime.state.queue.is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    /// What the UI sends to remove the row at `index` of the queue it
    /// shows at `revision`.
    fn ui_remove(revision: u64, index: usize) -> BridgeCommand {
        BridgeCommand::QueueAt {
            revision,
            command: BridgeQueueCommand::Remove(index),
        }
    }

    #[test]
    fn a_removal_issued_before_an_import_replaced_the_queue_leaves_the_import_alone() {
        let _guard = test_guard();
        for removal_first in [false, true] {
            let (mut runtime, _engine_rx, event_tx, _album) = queue_order_runtime();
            let (dir, c) = import_fixture("stale-removal");
            let (a, b) = (p("/music/a.flac"), p("/music/b.flac"));
            runtime.handle_command(
                BridgeCommand::Queue(BridgeQueueCommand::Replace {
                    tracks: vec![a.clone(), b.clone()],
                    autoplay: false,
                }),
                &event_tx,
            );
            // The UI shows [a, b].
            let shown = runtime.state.queue_sync.revision();
            runtime.handle_command(
                BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(c.clone())),
                &event_tx,
            );
            wait_for_import_result(&runtime);
            // The loop takes the import's result and the user's removal of
            // a, sent while the UI showed [a, b], in either order.
            if removal_first {
                runtime.handle_command(ui_remove(shown, 0), &event_tx);
                apply_next_import_result(&mut runtime, &event_tx);
            } else {
                apply_next_import_result(&mut runtime, &event_tx);
                runtime.handle_command(ui_remove(shown, 0), &event_tx);
            }
            assert_eq!(
                runtime.state.queue,
                vec![c],
                "removal first: {removal_first}"
            );
            let _ = std::fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn stale_queue_commands_act_on_the_rows_the_user_saw() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (event_tx_errors, event_rx) = bounded::<BridgeEvent>(64);
        let (a, b, c) = (p("/music/a.flac"), p("/music/b.flac"), p("/music/c.flac"));
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Replace {
                tracks: vec![a.clone(), b.clone(), c.clone()],
                autoplay: false,
            }),
            &event_tx,
        );
        let shown = runtime.state.queue_sync.revision();
        // A row arrives at the top before the UI's commands do.
        runtime.state.queue.insert(0, p("/music/new.flac"));
        runtime.state.queue_sync.record(
            QueueEdit::Insert {
                at: 0,
                paths: vec![p("/music/new.flac")],
            },
            4,
        );
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_remove(shown, 1), &event_tx_errors);
        assert_eq!(runtime.state.queue, vec![p("/music/new.flac"), a, c]);
        assert_eq!(queue_ops(&engine_rx), vec!["RemoveAt(2)".to_string()]);
        runtime.handle_command(
            BridgeCommand::QueueAt {
                revision: shown,
                command: BridgeQueueCommand::PlayAt(1),
            },
            &event_tx_errors,
        );
        assert!(queue_ops(&engine_rx).is_empty(), "b is gone; nothing plays");
        let errors: Vec<String> = event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Error(error) => Some(error.message),
                _ => None,
            })
            .collect();
        assert_eq!(
            errors,
            ["The track you chose to play is no longer in the queue."]
        );
    }

    fn replace_queue(
        runtime: &mut BridgeLoopRuntime,
        tracks: &[PathBuf],
        event_tx: &Sender<BridgeEvent>,
    ) {
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Replace {
                tracks: tracks.to_vec(),
                autoplay: false,
            }),
            event_tx,
        );
    }

    /// The undo of the change the bridge offers, as the UI sends it.
    fn ui_undo(id: u64) -> BridgeCommand {
        let mut command = 68_u16.to_le_bytes().to_vec();
        command.extend_from_slice(&8_u32.to_le_bytes());
        command.extend_from_slice(&id.to_le_bytes());
        ffi::parse_binary_command(&command)
            .expect("a valid command")
            .expect("a command")
    }

    fn restored_queues(engine_rx: &Receiver<PlaybackCommand>) -> Vec<(Vec<PathBuf>, usize)> {
        engine_rx
            .try_iter()
            .filter_map(|command| match command {
                PlaybackCommand::RestoreQueue { paths, current } => Some((paths, current)),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn an_undo_brings_back_a_cleared_queue_with_its_selection_and_current_track() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let queue = vec![p("/music/a.flac"), p("/music/b.flac"), p("/music/c.flac")];
        replace_queue(&mut runtime, &queue, &event_tx);
        // Replacing an empty queue takes nothing, so nothing is offered.
        assert_eq!(runtime.state.snapshot(false).queue_undo.offer_id, 0);
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Select(Some(1))),
            &event_tx,
        );
        runtime.state.playback.current_queue_index = Some(2);
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        let offer = runtime.state.snapshot(false).queue_undo;
        assert_ne!(offer.offer_id, 0);
        assert_eq!(offer.kind, Some(QueueUndoKind::Cleared));
        assert_eq!(offer.track_count, 3);
        // Clearing the empty queue again keeps what the first clear took.
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        assert_eq!(runtime.state.snapshot(false).queue_undo, offer);
        // Clearing stopped playback.
        runtime.state.playback.current_queue_index = None;
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_undo(offer.offer_id), &event_tx);
        assert_eq!(runtime.state.queue, queue);
        assert_eq!(runtime.state.selected_queue_index, Some(1));
        // The engine gets the queue with the track that was current.
        assert_eq!(restored_queues(&engine_rx), vec![(queue.clone(), 2)]);
        let snapshot = runtime.state.snapshot(false);
        assert_eq!(snapshot.queue_update, Some(QueueUpdate::Full(queue)));
        assert_eq!(snapshot.queue_undo.offer_id, 0, "an undo applies once");
        let restored = snapshot.queue_undo.restored.expect("the restored rows");
        assert_eq!(restored.id, offer.offer_id);
        assert_eq!(restored.revision, snapshot.queue_revision);
        assert_eq!(restored.ranges, vec![(1, 1)]);
    }

    #[test]
    fn an_undo_of_a_clear_while_stopped_brings_back_the_stopped_track_not_the_selection() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let queue = vec![p("/music/a.flac"), p("/music/b.flac"), p("/music/c.flac")];
        replace_queue(&mut runtime, &queue, &event_tx);
        // B played and was stopped: the engine keeps B but reports no row,
        // as the GStreamer backend does. Then C is selected, not played.
        runtime.state.playback.state = PlaybackState::Stopped;
        runtime.state.playback.current = Some(queue[1].clone());
        runtime.state.playback.current_queue_index = None;
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Select(Some(2))),
            &event_tx,
        );
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        let offer = runtime.state.snapshot(false).queue_undo.offer_id;
        // Clearing leaves the engine with no track.
        runtime.state.playback.current = None;
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_undo(offer), &event_tx);
        assert_eq!(runtime.state.queue, queue);
        assert_eq!(runtime.state.selected_queue_index, Some(2));
        // B comes back as the current track, so Play starts B.
        assert_eq!(restored_queues(&engine_rx), vec![(queue, 1)]);
    }

    #[test]
    fn an_undo_of_a_clear_while_stopped_brings_back_the_stopped_row_of_a_repeated_track() {
        let _guard = test_guard();
        for remove_first_row in [false, true] {
            let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
            let (a, b, c) = (p("/music/a.flac"), p("/music/b.flac"), p("/music/c.flac"));
            let queue = vec![a.clone(), b, a.clone(), c];
            replace_queue(&mut runtime, &queue, &event_tx);
            let engine = |state, row: Option<usize>| {
                BridgeLoopWake::Playback(PlaybackEvent::Snapshot(PlaybackSnapshot {
                    state,
                    current: Some(a.clone()),
                    current_queue_index: row,
                    ..PlaybackSnapshot::default()
                }))
            };
            // The second A plays, then stops: the engine keeps A but reports
            // no row, as the GStreamer backend does.
            runtime.handle_wake(engine(PlaybackState::Playing, Some(2)), &event_tx);
            runtime.handle_wake(engine(PlaybackState::Stopped, None), &event_tx);
            let mut expected = queue.clone();
            let mut stopped_row = 2;
            if remove_first_row {
                // A row removed while stopped moves the stopped track up.
                let shown = runtime.state.queue_sync.revision();
                runtime.handle_command(ui_remove(shown, 0), &event_tx);
                expected.remove(0);
                stopped_row = 1;
            }
            runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
            let offer = runtime.state.snapshot(false).queue_undo.offer_id;
            runtime.handle_wake(
                BridgeLoopWake::Playback(PlaybackEvent::Snapshot(PlaybackSnapshot::default())),
                &event_tx,
            );
            let _ = queue_ops(&engine_rx);

            runtime.handle_command(ui_undo(offer), &event_tx);
            assert_eq!(runtime.state.queue, expected);
            assert_eq!(
                restored_queues(&engine_rx),
                vec![(expected, stopped_row)],
                "the row of the A that stopped; removed first: {remove_first_row}"
            );
        }
    }

    #[test]
    fn an_undo_puts_removed_rows_back_where_they_were_around_the_playing_track() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, _event_tx, _album) = queue_order_runtime();
        // Snapshots reach the UI, so edits travel as edits.
        let (event_tx, _event_rx) = bounded::<BridgeEvent>(64);
        let queue: Vec<PathBuf> = ["a", "b", "c", "d", "e"]
            .iter()
            .map(|name| p(&format!("/music/{name}.flac")))
            .collect();
        replace_queue(&mut runtime, &queue, &event_tx);
        assert!(runtime.emit_snapshot(&event_tx));
        let shown = runtime.state.queue_sync.revision();
        runtime.handle_command(
            BridgeCommand::QueueAt {
                revision: shown,
                command: BridgeQueueCommand::RemoveMany(vec![3, 0, 1]),
            },
            &event_tx,
        );
        assert_eq!(
            runtime.state.queue,
            vec![queue[2].clone(), queue[4].clone()]
        );
        let offer = runtime.state.snapshot(false).queue_undo;
        assert_eq!(offer.kind, Some(QueueUndoKind::Removed));
        assert_eq!(offer.track_count, 3);
        // e, now row 1, plays.
        runtime.state.playback.current_queue_index = Some(1);
        assert!(runtime.emit_snapshot(&event_tx));
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_undo(offer.offer_id), &event_tx);
        assert_eq!(runtime.state.queue, queue);
        assert_eq!(restored_queues(&engine_rx), vec![(queue.clone(), 4)]);
        // The rows come back as edits, so the UI keeps its scroll position.
        let snapshot = runtime.state.snapshot(false);
        assert_eq!(
            snapshot.queue_update,
            Some(QueueUpdate::Edits {
                base_revision: snapshot.queue_revision - 2,
                edits: vec![
                    QueueEdit::Insert {
                        at: 0,
                        paths: queue[..2].to_vec()
                    },
                    QueueEdit::Insert {
                        at: 3,
                        paths: vec![queue[3].clone()]
                    },
                ],
            })
        );
        assert_eq!(
            snapshot.queue_undo.restored.expect("restored").ranges,
            vec![(0, 2), (3, 1)]
        );
        // A command the UI sent before the undo still finds its row.
        runtime.handle_command(ui_remove(shown + 1, 1), &event_tx);
        assert_eq!(runtime.state.queue.last(), Some(&queue[3]));
    }

    #[test]
    fn an_undo_of_a_replacement_brings_back_the_queue_it_replaced() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, album) = queue_order_runtime();
        let queue = vec![p("/music/a.flac"), p("/music/b.flac")];
        replace_queue(&mut runtime, &queue, &event_tx);
        runtime.state.playback.current_queue_index = Some(1);
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceAlbumByKey {
                artist: "Artist".to_string(),
                album: "Album".to_string(),
            }),
            &event_tx,
        );
        assert_eq!(runtime.state.queue, album);
        let offer = runtime.state.snapshot(false).queue_undo;
        assert_eq!(offer.kind, Some(QueueUndoKind::Replaced));
        assert_eq!(offer.track_count, 2);
        // The album plays from its first track.
        runtime.state.playback.current_queue_index = Some(0);
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_undo(offer.offer_id), &event_tx);
        assert_eq!(runtime.state.queue, queue);
        // The engine's row is in the album, not in the restored queue, so
        // the hint is the row that was current before; the engine stops
        // there since the album track it plays is not in that queue.
        assert_eq!(restored_queues(&engine_rx), vec![(queue, 1)]);
    }

    /// A move of `rows` as the UI sends it: `how` 0 before row `at`, 1 up,
    /// 2 down.
    fn ui_move_rows(revision: u64, how: u8, at: u32, rows: &[u32]) -> BridgeCommand {
        let mut payload = revision.to_le_bytes().to_vec();
        payload.push(how);
        payload.extend_from_slice(&at.to_le_bytes());
        payload.extend_from_slice(&u32::try_from(rows.len()).expect("count").to_le_bytes());
        for row in rows {
            payload.extend_from_slice(&row.to_le_bytes());
        }
        let mut command = 70_u16.to_le_bytes().to_vec();
        command.extend_from_slice(&u32::try_from(payload.len()).expect("len").to_le_bytes());
        command.extend_from_slice(&payload);
        ffi::parse_binary_command(&command)
            .expect("a valid command")
            .expect("a command")
    }

    fn reorders(engine_rx: &Receiver<PlaybackCommand>) -> Vec<Vec<usize>> {
        engine_rx
            .try_iter()
            .filter_map(|command| match command {
                PlaybackCommand::ReorderQueue(order) => Some(order),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn moving_rows_moves_them_all_and_one_undo_puts_them_back() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, _event_tx, _album) = queue_order_runtime();
        // Snapshots reach the UI, so edits travel as edits.
        let (event_tx, _event_rx) = bounded::<BridgeEvent>(64);
        let (a, b, c, d, e) = (
            p("/music/a.flac"),
            p("/music/b.flac"),
            p("/music/c.flac"),
            p("/music/d.flac"),
            p("/music/e.flac"),
        );
        // A is queued twice; the second A plays.
        let queue = vec![
            a.clone(),
            b.clone(),
            a.clone(),
            c.clone(),
            d.clone(),
            e.clone(),
        ];
        replace_queue(&mut runtime, &queue, &event_tx);
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Select(Some(3))),
            &event_tx,
        );
        runtime.state.playback.current = Some(a.clone());
        runtime.state.playback.current_queue_index = Some(2);
        assert!(runtime.emit_snapshot(&event_tx));
        let mut ui = runtime.state.queue.clone();
        let shown = runtime.state.queue_sync.revision();
        let _ = queue_ops(&engine_rx);

        // The first A and C, not next to each other, are dropped before E.
        runtime.handle_command(ui_move_rows(shown, 0, 5, &[3, 0]), &event_tx);
        let moved = vec![
            b.clone(),
            a.clone(),
            d.clone(),
            a.clone(),
            c.clone(),
            e.clone(),
        ];
        assert_eq!(runtime.state.queue, moved);
        // The engine reorders its rows in one step, keeping the playing A,
        // which is row 1 now, not the moved A at row 3.
        assert_eq!(reorders(&engine_rx), vec![vec![1, 2, 4, 0, 3, 5]]);
        assert_eq!(runtime.state.playback.current_queue_index, Some(1));
        assert_eq!(runtime.state.selected_queue_index, Some(4));
        // The UI gets row moves, so it keeps its rows and selection.
        let snapshot = runtime.state.snapshot(false);
        let Some(QueueUpdate::Edits { edits, .. }) = &snapshot.queue_update else {
            panic!("row moves, got {:?}", snapshot.queue_update);
        };
        assert!(edits.len() <= 2, "{edits:?}");
        for edit in edits {
            assert!(edit.apply_to(&mut ui));
        }
        assert_eq!(ui, moved);
        let offer = snapshot.queue_undo;
        assert_eq!(offer.kind, Some(QueueUndoKind::Moved));
        assert_eq!(offer.track_count, 2);
        assert!(runtime.emit_snapshot(&event_tx));

        runtime.handle_command(ui_undo(offer.offer_id), &event_tx);
        assert_eq!(runtime.state.queue, queue);
        assert_eq!(reorders(&engine_rx), vec![vec![3, 0, 1, 4, 2, 5]]);
        assert_eq!(runtime.state.playback.current_queue_index, Some(2));
        assert_eq!(runtime.state.selected_queue_index, Some(3));
        let snapshot = runtime.state.snapshot(false);
        let Some(QueueUpdate::Edits { edits, .. }) = &snapshot.queue_update else {
            panic!("row moves, got {:?}", snapshot.queue_update);
        };
        for edit in edits {
            assert!(edit.apply_to(&mut ui));
        }
        assert_eq!(ui, queue);
        assert_eq!(snapshot.queue_undo.offer_id, 0, "an undo applies once");
        // The moved rows are selected again.
        assert_eq!(
            snapshot.queue_undo.restored.expect("restored").ranges,
            vec![(0, 1), (3, 1)]
        );
    }

    #[test]
    fn rows_moved_up_down_or_from_an_older_queue_land_where_the_user_saw_them() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let tracks: Vec<PathBuf> = ["a", "b", "c", "d", "e"]
            .iter()
            .map(|name| p(&format!("/music/{name}.flac")))
            .collect();
        replace_queue(&mut runtime, &tracks, &event_tx);
        let shown = runtime.state.queue_sync.revision();
        // B and D move up a row each.
        runtime.handle_command(ui_move_rows(shown, 1, 0, &[1, 3]), &event_tx);
        let names = |queue: &[PathBuf]| {
            queue
                .iter()
                .map(|path| {
                    path.file_stem()
                        .expect("stem")
                        .to_string_lossy()
                        .into_owned()
                })
                .collect::<String>()
        };
        assert_eq!(names(&runtime.state.queue), "badce");
        // B and D move down again.
        runtime.handle_command(
            ui_move_rows(runtime.state.queue_sync.revision(), 2, 0, &[0, 2]),
            &event_tx,
        );
        assert_eq!(names(&runtime.state.queue), "abcde");
        // A row arrives at the top before a drag the UI started on "abcde":
        // C goes before E all the same.
        let shown = runtime.state.queue_sync.revision();
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::InsertAt {
                revision: shown,
                at: 0,
                tracks: vec![p("/music/x.flac")],
            }),
            &event_tx,
        );
        assert_eq!(names(&runtime.state.queue), "xabcde");
        runtime.handle_command(ui_move_rows(shown, 0, 4, &[2]), &event_tx);
        assert_eq!(names(&runtime.state.queue), "xabdce");
        let ops = queue_ops(&engine_rx);
        assert!(
            ops.iter()
                .any(|op| op.starts_with("InsertIntoQueue { at: 0")),
            "{ops:?}"
        );
    }

    #[test]
    fn a_library_selection_replaces_the_queue_as_one_change_to_undo() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (dir, _) = import_fixture("library-selection");
        let root = std::fs::canonicalize(&dir).expect("root");
        let track_path = |relative: &str| {
            let path = root.join(relative);
            std::fs::create_dir_all(path.parent().expect("parent")).expect("dirs");
            std::fs::write(&path, b"stub").expect("track");
            path
        };
        let first = track_path("Artist/Album/01.flac");
        let second = track_path("Artist/Album/02.flac");
        let other = track_path("Other/Album/01.flac");
        runtime.state.library = Arc::new(LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: [&first, &second, &other]
                .iter()
                .enumerate()
                .map(|(index, path)| {
                    let artist = if index == 2 { "Other" } else { "Artist" };
                    Arc::new(library_track(
                        &path.to_string_lossy(),
                        &root,
                        artist,
                        "Album",
                        None,
                        u32::try_from(index + 1).ok(),
                    ))
                })
                .collect(),
            ..LibrarySnapshot::default()
        });
        let queue = vec![p("/music/a.flac")];
        replace_queue(&mut runtime, &queue, &event_tx);
        let _ = queue_ops(&engine_rx);

        // A track row and an artist row, played together.
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::QueueItems {
                items: vec![
                    LibraryQueueItem::Paths(vec![other.clone()]),
                    LibraryQueueItem::Artist("Artist".to_string()),
                ],
                placement: QueuePlacement::Replace,
            }),
            &event_tx,
        );
        apply_next_import_result(&mut runtime, &event_tx);
        let selection = vec![other.clone(), first.clone(), second.clone()];
        assert_eq!(runtime.state.queue, selection);
        assert_eq!(
            queue_ops(&engine_rx),
            vec![
                format!("load {selection:?}"),
                "play at 0".to_string(),
                "play".to_string(),
            ]
        );
        // One undo brings back the queue the selection replaced.
        let offer = runtime.state.snapshot(false).queue_undo;
        assert_eq!(offer.kind, Some(QueueUndoKind::Replaced));
        runtime.handle_command(ui_undo(offer.offer_id), &event_tx);
        assert_eq!(runtime.state.queue, queue);

        // Rows dropped onto the queue go where they were dropped.
        let shown = runtime.state.queue_sync.revision();
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::QueueItems {
                items: vec![LibraryQueueItem::Paths(vec![first.clone()])],
                placement: QueuePlacement::Insert {
                    revision: shown,
                    at: 0,
                },
            }),
            &event_tx,
        );
        apply_next_import_result(&mut runtime, &event_tx);
        assert_eq!(runtime.state.queue, vec![first.clone(), queue[0].clone()]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_stale_undo_is_refused_and_leaves_the_queue_alone() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, _event_tx, _album) = queue_order_runtime();
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);
        let errors = || -> Vec<String> {
            event_rx
                .try_iter()
                .filter_map(|event| match event {
                    // A refused undo changes nothing, so it is a warning
                    // about the queue.
                    BridgeEvent::Error(error)
                        if error.category == ErrorCategory::Playback
                            && error.severity == ErrorSeverity::Warning =>
                    {
                        Some(error.message)
                    }
                    BridgeEvent::Error(error) => Some(format!("unexpected: {error:?}")),
                    _ => None,
                })
                .collect()
        };
        replace_queue(&mut runtime, &[p("/music/a.flac")], &event_tx);
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        let cleared = runtime.state.snapshot(false).queue_undo.offer_id;
        // Another change after the clear withdraws its undo.
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Append(vec![p("/music/new.flac")])),
            &event_tx,
        );
        assert_eq!(runtime.state.snapshot(false).queue_undo.offer_id, 0);
        let _ = errors();
        let _ = queue_ops(&engine_rx);

        runtime.handle_command(ui_undo(cleared), &event_tx);
        assert_eq!(runtime.state.queue, vec![p("/music/new.flac")]);
        assert!(restored_queues(&engine_rx).is_empty());
        assert_eq!(errors(), [queue_undo::STALE_UNDO_MESSAGE]);

        // So does a newer destructive change, which is offered instead.
        runtime.handle_command(BridgeCommand::Queue(BridgeQueueCommand::Clear), &event_tx);
        let newer = runtime.state.snapshot(false).queue_undo.offer_id;
        replace_queue(&mut runtime, &[p("/music/b.flac")], &event_tx);
        assert_eq!(
            runtime.state.snapshot(false).queue_undo.offer_id,
            0,
            "the queue was empty"
        );
        runtime.handle_command(ui_undo(newer), &event_tx);
        assert_eq!(runtime.state.queue, vec![p("/music/b.flac")]);
        assert_eq!(errors(), [queue_undo::STALE_UNDO_MESSAGE]);
    }

    #[test]
    fn a_ui_that_asks_for_a_snapshot_gets_the_whole_queue() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        // The UI has the queue the bridge started with.
        assert!(runtime.emit_snapshot(&event_tx));
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Queue(BridgeQueueCommand::Append(vec![p(
                "/music/a.flac",
            )]))),
            &event_tx,
        );
        let edited = emitted_snapshots(&event_rx).pop().expect("a snapshot");
        assert!(matches!(
            edited.queue_update,
            Some(QueueUpdate::Edits { .. })
        ));

        runtime.handle_command(BridgeCommand::RequestSnapshot, &event_tx);
        let resync = emitted_snapshots(&event_rx).pop().expect("a snapshot");
        assert_eq!(
            resync.queue_update,
            Some(QueueUpdate::Full(vec![p("/music/a.flac")]))
        );
        assert_eq!(resync.queue_revision, edited.queue_revision);
    }

    /// A folder of stub tracks, canonicalized like import results.
    fn rename_fixture(name: &str, files: &[&str]) -> (PathBuf, Vec<PathBuf>) {
        let dir = std::env::temp_dir().join(format!(
            "ferrous-import-rename-{name}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("fixture dir");
        let dir = std::fs::canonicalize(dir).expect("canonical dir");
        let paths = files
            .iter()
            .map(|file| {
                let path = dir.join(file);
                std::fs::write(&path, file.as_bytes()).expect("fixture track");
                path
            })
            .collect();
        (dir, paths)
    }

    /// Rename files on disk through temporary names, as the tag editor does
    /// when a target is another file's source, and return the rename map.
    fn rename_files(dir: &Path, pairs: &[(&str, &str)]) -> PathRenames {
        let pairs: Vec<_> = pairs
            .iter()
            .map(|(old, new)| (dir.join(old), dir.join(new)))
            .collect();
        for (old, _) in &pairs {
            std::fs::rename(old, old.with_extension("staged")).expect("stage rename");
        }
        for (old, new) in &pairs {
            std::fs::rename(old.with_extension("staged"), new).expect("finish rename");
        }
        pairs.into_iter().collect()
    }

    /// Renumbering `01 -> 02, 02 -> 03` and swapping `a <-> b` both rename a
    /// file to another selected file's old name.
    const OVERLAPPING_RENAMES: [(&str, [&str; 2], [(&str, &str); 2]); 2] = [
        (
            "renumber",
            ["01.flac", "02.flac"],
            [("01.flac", "02.flac"), ("02.flac", "03.flac")],
        ),
        (
            "swap",
            ["a.flac", "b.flac"],
            [("a.flac", "b.flac"), ("b.flac", "a.flac")],
        ),
    ];

    fn queued_tracks(engine_rx: &Receiver<PlaybackCommand>) -> Vec<Vec<PathBuf>> {
        engine_rx
            .try_iter()
            .filter_map(|command| match command {
                PlaybackCommand::LoadQueue(tracks) | PlaybackCommand::AddToQueue(tracks) => {
                    Some(tracks)
                }
                _ => None,
            })
            .collect()
    }

    #[test]
    fn an_import_that_reads_after_overlapping_renames_keeps_its_paths() {
        let _guard = test_guard();
        for (name, files, pairs) in OVERLAPPING_RENAMES {
            let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
            let (dir, _) = rename_fixture(&format!("after-{name}"), &files);
            let gate = runtime.imports.gate();
            // The worker waits for the rename in progress before it reads the
            // folder.
            let renames = gate.rename(|| {
                runtime.handle_command(
                    BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(vec![dir.clone()])),
                    &event_tx,
                );
                let renames = rename_files(&dir, &pairs);
                (renames.clone(), renames)
            });
            apply_next_import_result(&mut runtime, &event_tx);
            apply_next_import_result(&mut runtime, &event_tx);

            let mut after: Vec<_> = pairs.iter().map(|(_, new)| dir.join(new)).collect();
            after.sort();
            let mut renamed_twice = after.clone();
            for path in &mut renamed_twice {
                crate::playback::remap_path(path, &renames);
            }
            assert_ne!(
                after, renamed_twice,
                "{name}: the case must tell them apart"
            );
            assert_eq!(runtime.state.queue, after, "{name}");
            assert_eq!(queued_tracks(&engine_rx), vec![after], "{name}");
            let _ = std::fs::remove_dir_all(dir);
        }
    }

    #[test]
    fn queue_entries_imported_before_overlapping_renames_are_renamed_once() {
        let _guard = test_guard();
        for (name, files, pairs) in OVERLAPPING_RENAMES {
            let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
            let (dir, before) = rename_fixture(&format!("before-{name}"), &files);
            runtime.handle_command(
                BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(vec![dir.clone()])),
                &event_tx,
            );
            let finished = runtime
                .imports
                .results
                .recv_timeout(Duration::from_secs(5))
                .expect("import result");
            let renames = runtime.imports.gate().rename(|| {
                let renames = rename_files(&dir, &pairs);
                (renames.clone(), renames)
            });
            runtime.handle_wake(BridgeLoopWake::Import(finished), &event_tx);
            assert_eq!(
                runtime.state.queue, before,
                "{name}: added before the rename"
            );
            apply_next_import_result(&mut runtime, &event_tx);

            let after: Vec<_> = pairs.iter().map(|(_, new)| dir.join(new)).collect();
            assert_eq!(runtime.state.queue, after, "{name}");
            let commands: Vec<_> = engine_rx.try_iter().collect();
            assert!(
                matches!(
                    commands.as_slice(),
                    [
                        PlaybackCommand::LoadQueue(added) | PlaybackCommand::AddToQueue(added),
                        PlaybackCommand::RemapPaths(sent),
                    ]
                        if *added == before && **sent == renames
                ),
                "{name}: {commands:?}"
            );
            assert!(runtime.imports.results.is_empty());
            let _ = std::fs::remove_dir_all(dir);
        }
    }

    /// Create `playlist` as a FIFO, which blocks the import reading it.
    fn make_fifo(playlist: &Path) {
        use std::os::unix::ffi::OsStrExt;

        let fifo = std::ffi::CString::new(playlist.as_os_str().as_bytes()).expect("fifo path");
        // SAFETY: `fifo` is a NUL-terminated path that outlives the call.
        assert_eq!(unsafe { libc::mkfifo(fifo.as_ptr(), 0o600) }, 0, "mkfifo");
    }

    /// Open the FIFO `playlist` to write. This waits for the worker to open
    /// it to read, and the worker's read cannot end before the writer closes.
    fn playlist_writer(playlist: &Path) -> std::fs::File {
        std::fs::OpenOptions::new()
            .write(true)
            .open(playlist)
            .expect("open fifo")
    }

    fn playlist_lines(tracks: &[PathBuf]) -> String {
        tracks
            .iter()
            .map(|track| format!("{}\n", track.display()))
            .collect()
    }

    /// Replace `playlist` with a regular file listing `tracks`, so a read
    /// that starts after this does not block.
    fn replace_playlist(playlist: &Path, tracks: &[PathBuf]) {
        let staged = playlist.with_extension("tmp");
        std::fs::write(&staged, playlist_lines(tracks)).expect("playlist");
        std::fs::rename(&staged, playlist).expect("replace playlist");
    }

    /// Rename `pairs` in `dir` while `writer` keeps an import blocked reading
    /// a FIFO, and return the writer. A rename that waited for that read would
    /// wait for the writer this thread holds. So the rename runs on another
    /// thread, and after a generous deadline the test releases the read and
    /// fails instead of hanging.
    fn rename_during_blocked_read(
        gate: Arc<imports::ImportGate>,
        dir: &Path,
        pairs: &[(&'static str, &'static str)],
        writer: std::fs::File,
    ) -> std::fs::File {
        let (renamed_tx, renamed) = unbounded();
        let renamer = {
            let dir = dir.to_path_buf();
            let pairs = pairs.to_vec();
            std::thread::spawn(move || {
                gate.rename(|| ((), rename_files(&dir, &pairs)));
                let _ = renamed_tx.send(());
            })
        };
        if renamed.recv_timeout(Duration::from_secs(10)).is_err() {
            drop(writer);
            renamer.join().expect("rename thread");
            panic!("the rename waited for the blocked read");
        }
        renamer.join().expect("rename thread");
        writer
    }

    #[test]
    fn a_rename_does_not_wait_for_an_import_blocked_on_a_read() {
        use std::io::Write;

        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (dir, before) = rename_fixture("blocked-read", &["01.flac", "02.flac"]);
        let playlist = dir.join("blocked.m3u");
        make_fifo(&playlist);
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::AddTrack(playlist.clone())),
            &event_tx,
        );
        let pairs = [("01.flac", "02.flac"), ("02.flac", "03.flac")];
        let mut writer = rename_during_blocked_read(
            runtime.imports.gate(),
            &dir,
            &pairs,
            playlist_writer(&playlist),
        );
        let after: Vec<_> = pairs.iter().map(|(_, new)| dir.join(new)).collect();
        replace_playlist(&playlist, &after);
        // The blocked read gets the playlist as it was before the rename.
        writer
            .write_all(playlist_lines(&before).as_bytes())
            .expect("write fifo");
        drop(writer);
        apply_next_import_result(&mut runtime, &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);

        assert_eq!(runtime.state.queue, after, "the overlapped read was redone");
        assert_eq!(queued_tracks(&engine_rx), vec![after]);
        assert!(runtime.imports.results.is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_file_renamed_during_its_import_is_queued_under_its_new_name() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (dir, files) = rename_fixture("renamed-during-read", &["old.flac"]);
        let playlist = dir.join("blocked.m3u");
        make_fifo(&playlist);
        // The worker finds `old.flac`, then blocks reading the playlist.
        runtime.handle_command(
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(vec![
                files[0].clone(),
                playlist.clone(),
            ])),
            &event_tx,
        );
        let writer = rename_during_blocked_read(
            runtime.imports.gate(),
            &dir,
            &[("old.flac", "new.flac")],
            playlist_writer(&playlist),
        );
        replace_playlist(&playlist, &[]);
        drop(writer);
        apply_next_import_result(&mut runtime, &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);

        let after = vec![dir.join("new.flac")];
        assert_eq!(runtime.state.queue, after, "read again under its new name");
        assert_eq!(queued_tracks(&engine_rx), vec![after]);
        assert!(runtime.imports.results.is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_file_renamed_while_its_import_waits_is_queued_under_its_new_name() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (dir, files) = rename_fixture("renamed-while-waiting", &["old.flac"]);
        let playlist = dir.join("blocked.m3u");
        make_fifo(&playlist);
        for path in [playlist.clone(), files[0].clone()] {
            runtime.handle_command(
                BridgeCommand::Library(BridgeLibraryCommand::AddTrack(path)),
                &event_tx,
            );
        }
        // The worker blocks on the playlist, so `old.flac` waits behind it.
        let writer = rename_during_blocked_read(
            runtime.imports.gate(),
            &dir,
            &[("old.flac", "new.flac")],
            playlist_writer(&playlist),
        );
        replace_playlist(&playlist, &[]);
        drop(writer);
        for _ in 0..3 {
            apply_next_import_result(&mut runtime, &event_tx);
        }

        let after = vec![dir.join("new.flac")];
        assert_eq!(runtime.state.queue, after, "read under its new name");
        assert_eq!(queued_tracks(&engine_rx), vec![after]);
        assert!(runtime.imports.results.is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_large_rename_map_applies_at_once() {
        let _guard = test_guard();
        let (mut runtime, engine_rx, event_tx, _album) = queue_order_runtime();
        let (a, b) = (p("/music/large/a.flac"), p("/music/large/b.flac"));
        runtime.state.queue = vec![a.clone(), b.clone()];
        // Far more than one UI command could carry.
        let mut renames: PathRenames = (0..2000)
            .map(|index| {
                (
                    p(&format!("/music/large/track {index:04}.flac")),
                    p(&format!("/music/large/renamed {index:04}.flac")),
                )
            })
            .collect();
        renames.extend([(a.clone(), b.clone()), (b.clone(), a.clone())]);
        runtime.imports.gate().rename(|| ((), renames.clone()));
        apply_next_import_result(&mut runtime, &event_tx);

        assert!(runtime.imports.results.is_empty(), "one message");
        assert_eq!(runtime.state.queue, vec![b, a]);
        let remaps: Vec<_> = engine_rx
            .try_iter()
            .filter_map(|command| match command {
                PlaybackCommand::RemapPaths(sent) => Some(sent),
                _ => None,
            })
            .collect();
        assert_eq!(remaps.len(), 1);
        assert_eq!(*remaps[0], renames);
    }

    /// Wait for the bridge to show `volume`, which it takes from the
    /// playback engine after handling the user's command.
    fn wait_for_volume(bridge: &FrontendBridgeHandle, volume: f32) -> bool {
        wait_for_snapshot_matching(bridge, Duration::from_secs(10), |snapshot| {
            (snapshot.playback.volume - volume).abs() < 1e-3
        })
        .is_some()
    }

    #[test]
    fn a_bulk_tag_edit_refresh_does_not_hold_up_playback_commands() {
        let _guard = test_guard();
        let names = (0..200)
            .map(|index| format!("{index:03}.flac"))
            .collect::<Vec<_>>();
        let names = names.iter().map(String::as_str).collect::<Vec<_>>();
        let (dir, edited) = rename_fixture("bulk-edit-refresh", &names);
        // The first file's read waits, like a read from a stalled disk.
        let (read_started, release_read) = crate::library::hold_read(&edited[0]);
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");

        bridge.command(BridgeCommand::Library(
            BridgeLibraryCommand::RefreshEditedPaths(edited),
        ));
        read_started
            .recv_timeout(Duration::from_secs(10))
            .expect("the refresh reads the saved files");
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(30),
        )));
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(
            0.25,
        )));
        let handled = wait_for_volume(&bridge, 0.25);
        drop(release_read);

        assert!(
            handled,
            "the bridge handled the playback commands during the read"
        );
        bridge.command(BridgeCommand::Shutdown);
        std::fs::remove_dir_all(dir).expect("remove fixture");
    }

    #[test]
    fn a_bulk_rename_does_not_make_the_bridge_read_the_files() {
        let _guard = test_guard();
        let names = (0..200)
            .map(|index| format!("{index:03}.flac"))
            .collect::<Vec<_>>();
        let names = names.iter().map(String::as_str).collect::<Vec<_>>();
        let (dir, sources) = rename_fixture("bulk-rename-refresh", &names);
        let targets = sources
            .iter()
            .map(|path| path.with_extension("renamed.flac"))
            .collect::<Vec<_>>();
        // Reading a renamed file would wait, like a read from a stalled disk.
        let (_read_started, release_read) = crate::library::hold_read(&targets[0]);
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");

        // Rename the files as the tag editor does, which reads them once
        // they are renamed.
        bridge.import_gate().rename_with_details(|| {
            let mut renames = PathRenames::new();
            let mut details = imports::RenamedDetails::default();
            for (source, target) in sources.iter().zip(&targets) {
                std::fs::rename(source, target).expect("rename fixture");
                renames.insert(source.clone(), target.clone());
                details
                    .indexed
                    .insert(target.clone(), crate::library::read_track_info(target));
            }
            ((), renames, details)
        });
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(
            0.25,
        )));
        let handled = wait_for_volume(&bridge, 0.25);
        drop(release_read);

        assert!(
            handled,
            "the bridge handled the playback command after the renames"
        );
        bridge.command(BridgeCommand::Shutdown);
        std::fs::remove_dir_all(dir).expect("remove fixture");
    }

    #[test]
    fn a_tag_editor_rename_shows_what_the_rename_read() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (_library_tx, library_rx) = unbounded();
        runtime.library_rx = library_rx;
        let (event_tx, event_rx) = unbounded();
        let (dir, _) = rename_fixture("tag-editor-rename", &[]);
        let old = dir.join("01 - Old.flac");
        crate::tag_write::write_test_flac(&old, &[("TITLE", "New"), ("TRACKNUMBER", "1")]);
        crate::library::index_tracks_for_test(&dir, std::slice::from_ref(&old));
        // The bridge's copy of the library predates the last save.
        runtime.state.library = Arc::new(LibrarySnapshot {
            roots: vec![LibraryRoot {
                path: dir.clone(),
                name: String::new(),
            }],
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                path: old.clone(),
                root_path: dir.clone(),
                title: "Old".to_string(),
                ..LibraryTrack::default()
            }]),
            ..LibrarySnapshot::default()
        });
        runtime.state.queue = vec![old.clone()];
        let loaded = crate::tag_editor::serialize_load_response(
            &crate::tag_editor::load_rows_for_paths(std::slice::from_ref(&old)),
        )
        .expect("rows");
        let request = crate::tag_editor::parse_rename_request(&loaded).expect("request");

        let response = ffi::rename_through_gate(&runtime.imports.gate(), request);
        apply_next_import_result(&mut runtime, &event_tx);

        let new = dir.join("01 - New.flac");
        assert_eq!(
            response.successful_paths,
            vec![new.to_string_lossy().into_owned()]
        );
        assert_eq!(runtime.state.queue, vec![new.clone()]);
        let track = &runtime.state.library.tracks[0];
        assert_eq!(
            (&track.path, track.title.as_str(), track.track_no),
            (&new, "New", Some(1))
        );
        assert!(error_messages(&event_rx).is_empty());
        std::fs::remove_dir_all(dir).expect("remove fixture");
    }

    #[test]
    fn queued_files_are_checked_after_events_and_never_while_idle() {
        use crate::playback::{PlaybackEvent, TrackChangeKind};

        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        // Events reach the loop only from the test.
        let (library_tx, library_rx) = unbounded();
        runtime.library_rx = library_rx;
        let (_playback_tx, playback_rx) = unbounded();
        runtime.playback_rx = playback_rx;
        runtime.edit_refreshes = edit_refresh::EditRefreshWorker::with_refresh(|paths| {
            Ok(crate::library::TrackRefresh {
                indexed: paths
                    .iter()
                    .map(|path| (path.clone(), crate::library::read_track_info(path)))
                    .collect(),
                problem: None,
            })
        });
        let (event_tx, _event_rx) = unbounded();
        let (dir, files) = rename_fixture("idle-validation", &["a.flac", "b.flac"]);
        let checks = |runtime: &BridgeLoopRuntime| runtime.queue_validator.requests;
        let idle = |runtime: &mut BridgeLoopRuntime, event_tx: &Sender<BridgeEvent>| {
            let before = checks(runtime);
            // Saves and snapshots have timers of their own.
            runtime.flags.session_dirty = false;
            runtime.flags.settings_dirty = false;
            runtime.flags.pending_snapshot = SnapshotUrgency::None;
            assert!(
                runtime.next_wake_delay() > Duration::from_secs(60),
                "no timer wakes the loop to check the files"
            );
            for _ in 0..20 {
                runtime.handle_wake(BridgeLoopWake::Tick, event_tx);
            }
            assert_eq!(checks(runtime), before, "nothing is checked while idle");
        };

        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
                tracks: vec![files[0].clone()],
                autoplay: false,
            })),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 1, "a queue change");
        let result = runtime
            .queue_validator
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("validation result");
        runtime.handle_wake(BridgeLoopWake::QueueValidated(result), &event_tx);
        idle(&mut runtime, &event_tx);

        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::TrackChanged {
                path: files[0].clone(),
                queue_index: 0,
                kind: TrackChangeKind::Manual,
                track_token: 1,
            }),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 2, "a track change");

        let playing = PlaybackSnapshot {
            state: PlaybackState::Playing,
            current: Some(files[0].clone()),
            current_queue_index: Some(0),
            ..runtime.state.playback.clone()
        };
        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::Snapshot(playing.clone())),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 3, "playback starting");
        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::Snapshot(playing)),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 3, "playback going on");

        library_tx
            .send(LibraryEvent::with_library(LibrarySnapshot {
                tracks: crate::library::shared_tracks(vec![LibraryTrack {
                    path: dir.join("library.flac"),
                    ..LibraryTrack::default()
                }]),
                ..LibrarySnapshot::default()
            }))
            .expect("library snapshot");
        runtime.handle_wake(BridgeLoopWake::Tick, &event_tx);
        assert_eq!(checks(&runtime), 4, "a library change");

        let renamed = dir.join("renamed.flac");
        std::fs::rename(&files[0], &renamed).expect("rename fixture");
        let _ = runtime.apply_renamed_paths(
            1,
            Arc::new(PathRenames::from([(files[0].clone(), renamed.clone())])),
            imports::RenamedDetails::default(),
            crate::library::next_file_sequence(),
            &event_tx,
        );
        runtime.handle_wake(BridgeLoopWake::Tick, &event_tx);
        assert_eq!(checks(&runtime), 5, "a rename");

        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Library(
                BridgeLibraryCommand::RefreshEditedPaths(vec![renamed.clone()]),
            )),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 5, "a tag edit waits for its refresh");
        let refreshed = runtime
            .edit_refreshes
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("refresh result");
        runtime.handle_wake(BridgeLoopWake::EditsRefreshed(refreshed), &event_tx);
        assert_eq!(checks(&runtime), 6, "a refreshed tag edit");

        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Queue(BridgeQueueCommand::Append(vec![
                files[1].clone(),
            ]))),
            &event_tx,
        );
        assert_eq!(checks(&runtime), 7, "an append");

        let paused = PlaybackSnapshot {
            state: PlaybackState::Paused,
            ..runtime.state.playback.clone()
        };
        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::Snapshot(paused.clone())),
            &event_tx,
        );
        for _ in 0..20 {
            runtime.handle_wake(BridgeLoopWake::Tick, &event_tx);
        }
        assert_eq!(checks(&runtime), 7, "nothing is checked while paused");
        let stopped = PlaybackSnapshot {
            state: PlaybackState::Stopped,
            ..paused
        };
        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::Snapshot(stopped)),
            &event_tx,
        );
        idle(&mut runtime, &event_tx);

        std::fs::remove_dir_all(dir).expect("remove fixture");
    }

    /// An edit refresh worker that reads titles from `disk`, each read
    /// waiting for one message on `release`.
    struct GatedRefreshes {
        disk: Arc<Mutex<HashMap<PathBuf, String>>>,
        started: Receiver<Vec<PathBuf>>,
        release: Sender<()>,
    }

    fn indexed_titled(title: &str) -> IndexedTrack {
        IndexedTrack {
            search_tags: crate::library::SearchTags::default(),
            title: title.to_string(),
            artist: String::new(),
            album: String::new(),
            cover_path: String::new(),
            cover_stamp: 0,
            cover_read_at: crate::library::CoverReadAt::default(),
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        }
    }

    fn gated_refreshes(runtime: &mut BridgeLoopRuntime) -> GatedRefreshes {
        let disk = Arc::new(Mutex::new(HashMap::<PathBuf, String>::new()));
        let (started_tx, started) = unbounded();
        let (release, release_rx) = unbounded::<()>();
        let files = Arc::clone(&disk);
        runtime.edit_refreshes = edit_refresh::EditRefreshWorker::with_refresh(move |paths| {
            started_tx.send(paths.to_vec()).expect("report the read");
            release_rx
                .recv_timeout(Duration::from_secs(10))
                .expect("release the read");
            let files = files.lock().expect("disk");
            Ok(crate::library::TrackRefresh {
                indexed: paths
                    .iter()
                    .filter_map(|path| Some((path.clone(), indexed_titled(files.get(path)?))))
                    .collect(),
                problem: None,
            })
        });
        GatedRefreshes {
            disk,
            started,
            release,
        }
    }

    impl GatedRefreshes {
        fn set(&self, files: &[(&PathBuf, &str)]) {
            let mut disk = self.disk.lock().expect("disk");
            disk.clear();
            disk.extend(
                files
                    .iter()
                    .map(|(path, title)| ((*path).clone(), title.to_string())),
            );
        }

        fn wait_for_read(&self) -> Vec<PathBuf> {
            self.started
                .recv_timeout(Duration::from_secs(10))
                .expect("a refresh read")
        }
    }

    /// Release reads and apply their results until every request is answered.
    fn finish_edit_refreshes(
        runtime: &mut BridgeLoopRuntime,
        refreshes: &GatedRefreshes,
        event_tx: &Sender<BridgeEvent>,
    ) {
        while !runtime.edit_refreshes.is_idle() {
            refreshes.release.send(()).expect("release a read");
            let result = runtime
                .edit_refreshes
                .results
                .recv_timeout(Duration::from_secs(10))
                .expect("refresh result");
            runtime.handle_wake(BridgeLoopWake::EditsRefreshed(result), event_tx);
        }
    }

    fn library_titles(runtime: &BridgeLoopRuntime) -> HashMap<PathBuf, String> {
        runtime
            .state
            .library
            .tracks
            .iter()
            .map(|track| (track.path.clone(), track.title.clone()))
            .collect()
    }

    /// A runtime whose library holds `tracks` with their titles, and whose
    /// queue holds `queued`, files outside the library with their titles.
    fn stale_refresh_runtime(
        tracks: &[(&PathBuf, &str)],
        queued: &[(&PathBuf, &str)],
    ) -> BridgeLoopRuntime {
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (_library_tx, library_rx) = unbounded();
        runtime.library_rx = library_rx;
        let root = p("/music/stale");
        runtime.state.library = Arc::new(LibrarySnapshot {
            roots: vec![LibraryRoot {
                path: root.clone(),
                name: String::new(),
            }],
            tracks: tracks
                .iter()
                .map(|(path, title)| LibraryTrack {
                    path: (*path).clone(),
                    root_path: root.clone(),
                    title: title.to_string(),
                    ..LibraryTrack::default()
                })
                .map(std::sync::Arc::new)
                .collect(),
            ..LibrarySnapshot::default()
        });
        runtime.state.queue = tracks
            .iter()
            .chain(queued)
            .map(|(path, _)| (*path).clone())
            .collect();
        Arc::make_mut(&mut runtime.state.queue_details).extend(
            queued
                .iter()
                .map(|(path, title)| ((*path).clone(), indexed_titled(title))),
        );
        runtime
    }

    /// Rename files on disk through the gate, as the tag editor does, which
    /// reads them at their new names as `disk` then has them. The rename's
    /// notification waits on the import channel.
    fn rename_on_disk(
        runtime: &BridgeLoopRuntime,
        refreshes: &GatedRefreshes,
        renames: &[(&PathBuf, &PathBuf)],
        disk: &[(&PathBuf, &str)],
    ) {
        refreshes.set(disk);
        let renames = renames
            .iter()
            .map(|(old, new)| ((*old).clone(), (*new).clone()))
            .collect::<PathRenames>();
        let details = imports::RenamedDetails {
            indexed: disk
                .iter()
                .map(|(path, title)| ((*path).clone(), indexed_titled(title)))
                .collect(),
            ..imports::RenamedDetails::default()
        };
        runtime
            .imports
            .gate()
            .rename_with_details(|| ((), renames, details));
    }

    /// Have the bridge send a refresh of the files the tag editor saved.
    fn save(
        runtime: &mut BridgeLoopRuntime,
        refreshes: &GatedRefreshes,
        saved: &[&PathBuf],
        event_tx: &Sender<BridgeEvent>,
    ) {
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Library(
                BridgeLibraryCommand::RefreshEditedPaths(
                    saved.iter().map(|path| (*path).clone()).collect(),
                ),
            )),
            event_tx,
        );
        let mut read = refreshes.wait_for_read();
        read.sort();
        let mut expected = saved.iter().map(|path| (*path).clone()).collect::<Vec<_>>();
        expected.sort();
        assert_eq!(read, expected);
    }

    /// Let the refresh read and apply its result.
    fn accept_refresh(
        runtime: &mut BridgeLoopRuntime,
        refreshes: &GatedRefreshes,
        event_tx: &Sender<BridgeEvent>,
    ) {
        refreshes.release.send(()).expect("release the read");
        let result = runtime
            .edit_refreshes
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("refresh result");
        runtime.handle_wake(BridgeLoopWake::EditsRefreshed(result), event_tx);
    }

    fn queue_titles(runtime: &BridgeLoopRuntime) -> Vec<(PathBuf, String)> {
        runtime
            .state
            .queue
            .iter()
            .map(|path| {
                let title = runtime
                    .state
                    .library_index
                    .position(&runtime.state.library, path)
                    .map(|position| runtime.state.library.tracks[position].title.clone())
                    .or_else(|| {
                        runtime
                            .state
                            .queue_details
                            .get(path)
                            .map(|details| details.title.clone())
                    })
                    .unwrap_or_default();
                (path.clone(), title)
            })
            .collect()
    }

    fn owned(rows: &[(&PathBuf, &str)]) -> Vec<(PathBuf, String)> {
        rows.iter()
            .map(|(path, title)| ((*path).clone(), title.to_string()))
            .collect()
    }

    /// The order in which the bridge handles a rename A→B and a save at B
    /// made after it.
    #[derive(Clone, Copy, Debug)]
    enum SaveAfterRename {
        /// The save's result is applied before the rename's notification.
        ResultFirst,
        /// The save is asked for before the rename's notification, and its
        /// result arrives after it.
        RequestFirst,
        RenameFirst,
    }

    fn save_after_rename_shows_the_saved_tags(order: SaveAfterRename) {
        let _guard = test_guard();
        let (a, b) = (p("/music/stale/A/x/a.flac"), p("/music/stale/A/x/b.flac"));
        // A queued file outside the library, renamed and saved with a.
        let (e, f) = (p("/elsewhere/e.flac"), p("/elsewhere/f.flac"));
        let mut runtime = stale_refresh_runtime(&[(&a, "Old")], &[(&e, "Old E")]);
        let (event_tx, event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);

        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&a, &b), (&e, &f)],
            &[(&b, "Renamed"), (&f, "Renamed F")],
        );
        if matches!(order, SaveAfterRename::RenameFirst) {
            apply_next_import_result(&mut runtime, &event_tx);
        }
        refreshes.set(&[(&b, "Saved"), (&f, "Saved F")]);
        save(&mut runtime, &refreshes, &[&b, &f], &event_tx);
        match order {
            SaveAfterRename::ResultFirst => {
                accept_refresh(&mut runtime, &refreshes, &event_tx);
                apply_next_import_result(&mut runtime, &event_tx);
            }
            SaveAfterRename::RequestFirst => apply_next_import_result(&mut runtime, &event_tx),
            SaveAfterRename::RenameFirst => {}
        }
        finish_edit_refreshes(&mut runtime, &refreshes, &event_tx);

        assert_eq!(
            queue_titles(&runtime),
            owned(&[(&b, "Saved"), (&f, "Saved F")]),
            "{order:?}"
        );
        assert_eq!(
            library_titles(&runtime),
            HashMap::from([(b, "Saved".to_string())])
        );
        assert!(error_messages(&event_rx).is_empty());
    }

    #[test]
    fn a_save_after_a_rename_applied_before_the_rename_arrives_is_shown() {
        save_after_rename_shows_the_saved_tags(SaveAfterRename::ResultFirst);
    }

    #[test]
    fn a_save_after_a_rename_in_flight_when_the_rename_arrives_is_shown() {
        save_after_rename_shows_the_saved_tags(SaveAfterRename::RequestFirst);
    }

    #[test]
    fn a_save_after_a_rename_the_bridge_already_applied_is_shown() {
        save_after_rename_shows_the_saved_tags(SaveAfterRename::RenameFirst);
    }

    #[test]
    fn a_save_between_the_renames_of_a_chain_is_shown() {
        let _guard = test_guard();
        let (a, b, c) = (
            p("/music/stale/A/x/a.flac"),
            p("/music/stale/A/x/b.flac"),
            p("/music/stale/A/x/c.flac"),
        );
        let (e, f, g) = (
            p("/elsewhere/e.flac"),
            p("/elsewhere/f.flac"),
            p("/elsewhere/g.flac"),
        );
        let mut runtime = stale_refresh_runtime(&[(&a, "Old")], &[(&e, "Old E")]);
        let (event_tx, _event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);

        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&a, &b), (&e, &f)],
            &[(&b, "First rename"), (&f, "First rename F")],
        );
        apply_next_import_result(&mut runtime, &event_tx);
        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&b, &c), (&f, &g)],
            &[(&c, "Second rename"), (&g, "Second rename G")],
        );
        // The save at the last names is handled between the two
        // notifications, and its result too.
        refreshes.set(&[(&c, "Saved"), (&g, "Saved G")]);
        save(&mut runtime, &refreshes, &[&c, &g], &event_tx);
        accept_refresh(&mut runtime, &refreshes, &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);
        finish_edit_refreshes(&mut runtime, &refreshes, &event_tx);

        assert_eq!(
            queue_titles(&runtime),
            owned(&[(&c, "Saved"), (&g, "Saved G")])
        );
    }

    #[test]
    fn a_save_after_a_swap_is_shown() {
        let _guard = test_guard();
        let (a, b) = (p("/music/stale/A/x/a.flac"), p("/music/stale/A/x/b.flac"));
        let (e, f) = (p("/elsewhere/e.flac"), p("/elsewhere/f.flac"));
        let mut runtime = stale_refresh_runtime(&[(&a, "A"), (&b, "B")], &[(&e, "E"), (&f, "F")]);
        let (event_tx, _event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);

        // Each pair swaps names, and then one file of each is saved.
        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&a, &b), (&b, &a), (&e, &f), (&f, &e)],
            &[(&a, "B"), (&b, "A"), (&e, "F"), (&f, "E")],
        );
        refreshes.set(&[(&a, "B"), (&b, "A, saved"), (&e, "F"), (&f, "E, saved")]);
        save(&mut runtime, &refreshes, &[&b, &f], &event_tx);
        accept_refresh(&mut runtime, &refreshes, &event_tx);
        apply_next_import_result(&mut runtime, &event_tx);
        finish_edit_refreshes(&mut runtime, &refreshes, &event_tx);

        assert_eq!(
            queue_titles(&runtime),
            owned(&[(&b, "A, saved"), (&a, "B"), (&f, "E, saved"), (&e, "F")])
        );
    }

    #[test]
    fn an_edit_refresh_read_before_a_rename_leaves_the_renamed_tracks_alone() {
        let _guard = test_guard();
        let (a, b) = (p("/music/stale/A/x/a.flac"), p("/music/stale/A/x/b.flac"));
        let mut runtime = stale_refresh_runtime(&[(&a, "First"), (&b, "Second")], &[]);
        let (event_tx, _event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);
        refreshes.set(&[(&a, "First, edited"), (&b, "Second")]);

        save(&mut runtime, &refreshes, &[&a], &event_tx);
        refreshes.release.send(()).expect("release the read");
        // The read is done before the files swap names, and its result
        // reaches the bridge after the rename.
        let stale = runtime
            .edit_refreshes
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("refresh result");
        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&a, &b), (&b, &a)],
            &[(&a, "Second"), (&b, "First, edited")],
        );
        apply_next_import_result(&mut runtime, &event_tx);
        runtime.handle_wake(BridgeLoopWake::EditsRefreshed(stale), &event_tx);
        assert!(runtime.edit_refreshes.is_idle());

        assert_eq!(
            library_titles(&runtime),
            HashMap::from([(a, "Second".to_string()), (b, "First, edited".to_string()),])
        );
    }

    /// A library worker's event with these rows, read after `read_after`.
    fn library_rows_event(
        upserted: &[(&PathBuf, &str)],
        removed: &[&PathBuf],
        read_after: u64,
    ) -> LibraryEvent {
        LibraryEvent {
            changes: crate::library::LibraryChanges {
                roots: None,
                upserted: upserted
                    .iter()
                    .map(|(path, title)| {
                        Arc::new(LibraryTrack {
                            path: (*path).clone(),
                            root_path: p("/music/stale"),
                            title: title.to_string(),
                            ..LibraryTrack::default()
                        })
                    })
                    .collect(),
                removed: removed.iter().map(|path| (*path).clone()).collect(),
            },
            status: crate::library::LibraryStatus::default(),
            read_after,
        }
    }

    #[test]
    fn a_library_change_read_after_a_rename_stays_when_the_rename_is_applied() {
        let _guard = test_guard();
        let (a, b) = (p("/music/stale/A/x/a.flac"), p("/music/stale/A/x/b.flac"));
        let mut runtime = stale_refresh_runtime(&[(&a, "A"), (&b, "B")], &[]);
        let (event_tx, event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);

        // The files swap names, and the rename's notification waits.
        rename_on_disk(
            &runtime,
            &refreshes,
            &[(&a, &b), (&b, &a)],
            &[(&a, "B"), (&b, "A")],
        );
        // A save of b is read and applied, and its row is kept for the
        // rename.
        refreshes.set(&[(&a, "B"), (&b, "A, saved")]);
        save(&mut runtime, &refreshes, &[&b], &event_tx);
        accept_refresh(&mut runtime, &refreshes, &event_tx);
        assert_eq!(library_titles(&runtime)[&b], "A, saved");
        // A scan reads b again after it changed on disk once more.
        runtime.handle_wake(
            BridgeLoopWake::Library(library_rows_event(
                &[(&b, "A, changed again")],
                &[],
                crate::library::next_file_sequence(),
            )),
            &event_tx,
        );
        assert_eq!(library_titles(&runtime)[&b], "A, changed again");
        assert_eq!(runtime.edit_refreshes.kept_paths(), 1);

        apply_next_import_result(&mut runtime, &event_tx);
        let shown = HashMap::from([
            (a.clone(), "B".to_string()),
            (b.clone(), "A, changed again".to_string()),
        ]);
        assert_eq!(
            library_titles(&runtime),
            shown,
            "once the rename is applied"
        );
        assert_eq!(
            queue_titles(&runtime),
            owned(&[(&b, "A, changed again"), (&a, "B")])
        );
        // The worker reads the rows the rename changed again; the database
        // has the scan's row for b.
        runtime.handle_wake(
            BridgeLoopWake::Library(library_rows_event(
                &[(&a, "B"), (&b, "A, changed again")],
                &[],
                crate::library::next_file_sequence(),
            )),
            &event_tx,
        );
        assert_eq!(
            library_titles(&runtime),
            shown,
            "once the rows are read again"
        );
        // With no rename on its way, nothing is kept for one.
        assert_eq!(runtime.edit_refreshes.kept_paths(), 0);
        assert!(error_messages(&event_rx).is_empty());
    }

    #[test]
    fn a_path_removed_after_a_rename_is_not_restored_when_the_rename_is_applied() {
        let _guard = test_guard();
        let (a, c) = (p("/music/stale/A/x/a.flac"), p("/music/stale/A/x/c.flac"));
        let mut runtime = stale_refresh_runtime(&[(&a, "A")], &[]);
        let (event_tx, event_rx) = unbounded();
        let refreshes = gated_refreshes(&mut runtime);

        rename_on_disk(&runtime, &refreshes, &[(&a, &c)], &[(&c, "A")]);
        refreshes.set(&[(&c, "A, saved")]);
        save(&mut runtime, &refreshes, &[&c], &event_tx);
        accept_refresh(&mut runtime, &refreshes, &event_tx);
        // c is deleted, and a scan finds it gone.
        runtime.handle_wake(
            BridgeLoopWake::Library(library_rows_event(
                &[],
                &[&c],
                crate::library::next_file_sequence(),
            )),
            &event_tx,
        );

        apply_next_import_result(&mut runtime, &event_tx);
        assert_eq!(library_titles(&runtime), HashMap::new());
        assert!(error_messages(&event_rx).is_empty());
    }

    #[test]
    fn renamed_current_track_survives_engine_events() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let (old_path, new_path) = (p("/music/old name.flac"), p("/music/new name.flac"));
        runtime.handle_command(
            BridgeCommand::Queue(BridgeQueueCommand::Replace {
                tracks: vec![old_path.clone(), p("/music/next.flac")],
                autoplay: false,
            }),
            &event_tx,
        );
        let deadline = Instant::now() + Duration::from_secs(5);
        while runtime.state.playback.current.as_ref() != Some(&old_path) {
            assert!(Instant::now() < deadline, "engine reports the loaded track");
            let _ = runtime.drain_pending_updates(&event_tx);
            while event_rx.try_recv().is_ok() {}
            std::thread::sleep(Duration::from_millis(5));
        }

        runtime
            .imports
            .gate()
            .rename(|| ((), PathRenames::from([(old_path, new_path.clone())])));
        apply_next_import_result(&mut runtime, &event_tx);
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            assert_eq!(runtime.state.playback.current.as_ref(), Some(&new_path));
            if runtime.state.unacknowledged_engine_renames.is_empty() {
                break;
            }
            assert!(Instant::now() < deadline, "engine acknowledges the rename");
            let _ = runtime.drain_pending_updates(&event_tx);
            while event_rx.try_recv().is_ok() {}
            std::thread::sleep(Duration::from_millis(5));
        }
        runtime.playback.command(PlaybackCommand::Poll);
        runtime.playback.command(PlaybackCommand::SetVolume(0.5));
        let deadline = Instant::now() + Duration::from_millis(300);
        while Instant::now() < deadline {
            let _ = runtime.drain_pending_updates(&event_tx);
            while event_rx.try_recv().is_ok() {}
            assert_eq!(runtime.state.playback.current.as_ref(), Some(&new_path));
            std::thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(runtime.state.queue[0], new_path);
    }

    #[test]
    fn bridge_queue_roundtrip_snapshot_integration() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![p("/music/a.flac"), p("/music/b.flac")],
            autoplay: false,
        }));
        bridge.command(BridgeCommand::RequestSnapshot);

        let loaded = wait_for_snapshot_matching(&bridge, Duration::from_secs(10), |s| {
            s.queue_len == 2 && s.selected_queue_index == Some(0)
        })
        .expect("snapshot with loaded queue");
        assert_eq!(loaded.queue_len, 2);
        assert_eq!(loaded.selected_queue_index, Some(0));

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Clear));
        bridge.command(BridgeCommand::RequestSnapshot);
        let cleared = wait_for_snapshot_matching(&bridge, Duration::from_secs(10), |s| {
            s.queue_len == 0 && s.selected_queue_index.is_none()
        })
        .expect("snapshot with cleared queue");
        assert_eq!(cleared.queue_len, 0);
        assert!(cleared.selected_queue_index.is_none());

        bridge.command(BridgeCommand::Shutdown);
    }

    fn emitted_snapshots(event_rx: &Receiver<BridgeEvent>) -> Vec<BridgeSnapshot> {
        event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Snapshot(snapshot) => Some(*snapshot),
                _ => None,
            })
            .collect()
    }

    fn gapless_change(path: &str, queue_index: usize) -> PlaybackEvent {
        PlaybackEvent::TrackChanged {
            path: p(path),
            queue_index,
            kind: crate::playback::TrackChangeKind::Gapless,
            track_token: 2,
        }
    }

    #[test]
    fn a_gapless_track_change_keeps_a_queue_edit_in_the_snapshot() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let (playback_tx, playback_rx) = unbounded();
        runtime.playback_rx = playback_rx;
        // The UI has the queue the bridge started with.
        runtime.state.queue_sync.request_full();
        assert!(runtime.emit_snapshot(&event_tx));
        while event_rx.try_recv().is_ok() {}

        // On its own, a gapless change moves only the playing index, which
        // the playback section carries.
        runtime.handle_wake(
            BridgeLoopWake::Playback(gapless_change("/music/a.flac", 0)),
            &event_tx,
        );
        let snapshots = emitted_snapshots(&event_rx);
        assert!(!snapshots.is_empty());
        assert!(snapshots
            .iter()
            .all(|snapshot| snapshot.queue_update.is_none()));

        // The engine's next gapless change is waiting when the loop takes a
        // queue edit, and the loop handles both before it sends a snapshot.
        playback_tx
            .send(gapless_change("/music/b.flac", 1))
            .expect("queue the track change");
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::Queue(BridgeQueueCommand::Append(vec![p(
                "/music/edit.flac",
            )]))),
            &event_tx,
        );
        assert_eq!(runtime.state.playback.current_queue_index, Some(1));
        let snapshot = emitted_snapshots(&event_rx)
            .pop()
            .expect("a snapshot after the edit");
        let Some(QueueUpdate::Edits { edits, .. }) = snapshot.queue_update else {
            panic!("the edit, not the whole queue: {:?}", snapshot.queue_update);
        };
        assert_eq!(
            edits,
            vec![QueueEdit::Insert {
                at: 0,
                paths: vec![p("/music/edit.flac")]
            }]
        );
        assert_eq!(snapshot.playback.current_queue_index, Some(1));
        assert!(!runtime.state.queue_sync.has_pending());
    }

    #[test]
    fn queue_files_whose_tags_could_not_be_read_are_shown_counted_per_batch() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = event_channel();
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let dir = crate::paths::test_root().join("queue-detail-failures");
        std::fs::create_dir_all(&dir).expect("fixture folder");
        let paths: Vec<PathBuf> = ["a.ac3", "b.ac3"]
            .iter()
            .map(|name| dir.join(name))
            .collect();
        for path in &paths {
            std::fs::write(path, [0u8; 128]).expect("fixture");
            // Stands in for a tag reader that panics on the file.
            crate::library::panic_on_read(path);
        }
        runtime.state.queue.clone_from(&paths);
        runtime.state.queue_detail_generation += 1;

        runtime.queue_validator.request(&runtime.state);
        let result = runtime
            .queue_validator
            .results
            .recv_timeout(Duration::from_secs(5))
            .expect("validation result");
        runtime.handle_wake(BridgeLoopWake::QueueValidated(result), &event_tx);

        let errors: Vec<BridgeError> = event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Error(error) => Some(error),
                _ => None,
            })
            .collect();
        assert_eq!(errors.len(), 1, "one error for the batch: {errors:?}");
        assert_eq!(errors[0].category, ErrorCategory::Playback);
        assert_eq!(errors[0].severity, ErrorSeverity::Warning);
        assert!(
            errors[0]
                .message
                .starts_with("Could not read the tags of 2 files in the queue"),
            "{}",
            errors[0].message
        );
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn errors_and_save_answers_reach_a_relay_that_is_behind() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = event_channel();
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        // The relay falls behind: snapshots fill its backlog, and the bridge
        // holds back the next one.
        while runtime.emit_snapshot(&event_tx) {}
        assert_eq!(event_rx.len(), MAX_RELAY_BACKLOG);

        runtime.handle_wake(
            BridgeLoopWake::Analysis(AnalysisEvent::WorkerFailed("worker failed".to_string())),
            &event_tx,
        );
        runtime.handle_wake(
            BridgeLoopWake::Command(BridgeCommand::SaveState(7)),
            &event_tx,
        );

        let events: Vec<BridgeEvent> = event_rx.try_iter().collect();
        assert_eq!(events.len(), MAX_RELAY_BACKLOG + 2);
        assert!(events[..MAX_RELAY_BACKLOG]
            .iter()
            .all(|event| matches!(event, BridgeEvent::Snapshot(_))));
        assert!(matches!(
            &events[MAX_RELAY_BACKLOG],
            BridgeEvent::Error(error) if error.message == "worker failed"
                && error.category == ErrorCategory::Analysis
        ));
        assert!(matches!(
            events[MAX_RELAY_BACKLOG + 1],
            BridgeEvent::StateSaved {
                request_id: 7,
                result: Ok(())
            }
        ));
        // With the backlog gone, snapshots go again.
        assert!(runtime.emit_snapshot(&event_tx));
    }

    #[test]
    fn metadata_event_emits_snapshot_immediately_while_stopped() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let track = p("/tmp/ferrous_reactive_stopped_metadata.flac");
        runtime.state.playback.current = Some(track.clone());
        runtime.state.playback.state = PlaybackState::Stopped;

        runtime.handle_wake(
            BridgeLoopWake::Metadata(MetadataEvent::Loaded(TrackMetadata {
                source_path: Some(track.to_string_lossy().to_string()),
                title: "ferrous_reactive_stopped_metadata".to_string(),
                ..TrackMetadata::default()
            })),
            &event_tx,
        );

        let snapshot = match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::Snapshot(snapshot)) => *snapshot,
            other => panic!("expected immediate snapshot, got {other:?}"),
        };
        assert_eq!(snapshot.playback.state, PlaybackState::Stopped);
        assert_eq!(snapshot.playback.current.as_ref(), Some(&track));
        assert_eq!(snapshot.metadata.title, "ferrous_reactive_stopped_metadata");
    }

    #[test]
    fn analysis_worker_failures_reach_the_ui_as_errors() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded::<AnalysisEvent>();
        runtime.analysis_rx = analysis_rx;

        // One failure wakes the loop; the next is found by the drain path.
        analysis_tx
            .send(AnalysisEvent::WorkerFailed("drained".to_string()))
            .expect("queue drained failure");
        runtime.handle_wake(
            BridgeLoopWake::Analysis(AnalysisEvent::WorkerFailed("woken".to_string())),
            &event_tx,
        );

        let errors: Vec<String> = event_rx
            .try_iter()
            .filter_map(|event| match event {
                BridgeEvent::Error(error) => Some(error.message),
                _ => None,
            })
            .collect();
        assert_eq!(errors, ["woken", "drained"]);
    }

    #[test]
    fn analysis_wake_forwards_following_precomputed_chunks_from_drain_path() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);

        let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded::<AnalysisEvent>();
        runtime.analysis_rx = analysis_rx;

        let first = crate::analysis::PrecomputedSpectrogramChunk {
            track_token: 7,
            generation: 11,
            columns_u8: vec![1, 2],
            bins_per_column: 1,
            column_count: 2,
            channel_count: 1,
            start_column_index: 0,
            total_columns_estimate: 4,
            sample_rate_hz: 44_100,
            hop_size: 1_024,
            coverage_seconds: 0.05,
            complete: false,
            buffer_reset: false,
            clear_history: false,
        };
        let second = crate::analysis::PrecomputedSpectrogramChunk {
            track_token: 7,
            generation: 11,
            columns_u8: vec![3, 4],
            bins_per_column: 1,
            column_count: 2,
            channel_count: 1,
            start_column_index: 2,
            total_columns_estimate: 4,
            sample_rate_hz: 44_100,
            hop_size: 1_024,
            coverage_seconds: 0.10,
            complete: false,
            buffer_reset: false,
            clear_history: false,
        };

        analysis_tx
            .send(AnalysisEvent::PrecomputedSpectrogramChunk(second.clone()))
            .expect("queue follow-up precomputed chunk");

        runtime.handle_wake(
            BridgeLoopWake::Analysis(AnalysisEvent::PrecomputedSpectrogramChunk(first.clone())),
            &event_tx,
        );

        match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::PrecomputedSpectrogramChunk(chunk)) => {
                assert_eq!(chunk.start_column_index, first.start_column_index);
                assert_eq!(chunk.column_count, first.column_count);
                assert_eq!(chunk.columns_u8, first.columns_u8);
            }
            other => panic!("expected first precomputed chunk, got {other:?}"),
        }

        match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::PrecomputedSpectrogramChunk(chunk)) => {
                assert_eq!(chunk.start_column_index, second.start_column_index);
                assert_eq!(chunk.column_count, second.column_count);
                assert_eq!(chunk.columns_u8, second.columns_u8);
            }
            other => panic!("expected drained precomputed chunk, got {other:?}"),
        }

        assert!(event_rx.try_recv().is_err());
    }

    #[test]
    fn library_event_publishes_tree_after_worker_completion() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        // Isolate the fixture from late startup snapshots from the real library worker.
        let (_library_tx, library_rx) = unbounded();
        runtime.library_rx = library_rx;

        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: crate::library::shared_tracks(vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )]),
            ..LibrarySnapshot::default()
        };

        runtime.handle_wake(
            BridgeLoopWake::Library(LibraryEvent::with_library(snapshot)),
            &event_tx,
        );

        let result = loop {
            let result = runtime
                .tree_worker
                .results
                .recv_timeout(Duration::from_secs(2))
                .expect("tree worker result");
            if runtime.tree_worker.is_current(&result) {
                break result;
            }
        };
        while event_rx.try_recv().is_ok() {}
        runtime.handle_wake(BridgeLoopWake::Tree(result), &event_tx);

        let snapshot = match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::Snapshot(snapshot)) => *snapshot,
            other => panic!("expected library snapshot, got {other:?}"),
        };
        // The library worker's own events may add the tracks of the
        // process's test library.
        assert!(snapshot
            .library
            .tracks
            .iter()
            .any(|track| track.path == p("/music/Artist/Album/01 - Song.flac")));
        assert!(snapshot.pre_built_tree_bytes.is_some());
        assert!(!snapshot
            .pre_built_tree_bytes
            .as_ref()
            .is_some_and(|bytes| bytes.is_empty()));
    }

    #[test]
    fn playing_position_updates_wait_for_coarse_heartbeat() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default(), None);
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);
        let track = p("/tmp/ferrous_playing_heartbeat.flac");
        runtime.state.playback.current = Some(track.clone());
        runtime.state.playback.state = PlaybackState::Playing;
        runtime.last_playing_poll = Instant::now();
        runtime.last_snapshot_emit = Instant::now();

        let mut playback_snapshot = runtime.state.playback.clone();
        playback_snapshot.position = Duration::from_secs(1);
        runtime.handle_wake(
            BridgeLoopWake::Playback(PlaybackEvent::Snapshot(playback_snapshot)),
            &event_tx,
        );

        assert!(event_rx.recv_timeout(Duration::from_millis(20)).is_err());
        assert_eq!(runtime.state.playback.state, PlaybackState::Playing);
        assert_eq!(runtime.state.playback.current.as_ref(), Some(&track));
        assert_eq!(runtime.state.playback.position, Duration::from_secs(1));

        runtime.last_snapshot_emit = Instant::now()
            .checked_sub(runtime.playing_snapshot_interval)
            .unwrap_or_else(Instant::now);
        runtime.handle_wake(BridgeLoopWake::Tick, &event_tx);

        match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::Snapshot(_)) => {}
            other => panic!("expected heartbeat snapshot, got {other:?}"),
        }
    }

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_queue_play_seek_clamp_and_remove_integration() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");
        let first = p("/music/a.flac");
        let second = p("/music/b.flac");
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![first.clone(), second.clone()],
            autoplay: false,
        }));
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::PlayAt(1)));
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(500),
        )));
        let seeked = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            let sc = second.clone();
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), |s| {
                        s.queue_len == 2
                            && s.selected_queue_index == Some(1)
                            && s.playback.current.as_ref() == Some(&sc)
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("snapshot after play-at")
        };
        assert_eq!(seeked.playback.current.as_ref(), Some(&second));
        assert_eq!(seeked.selected_queue_index, Some(1));

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Remove(1)));
        let removed = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            let sc2 = second.clone();
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), |s| {
                        s.queue_len == 1
                            && s.selected_queue_index == Some(0)
                            && s.playback.current.as_ref() != Some(&sc2)
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("snapshot after removing selected track")
        };
        assert_ne!(removed.playback.current.as_ref(), Some(&second));
        if let Some(current) = removed.playback.current.as_ref() {
            assert_eq!(current, &first);
        }
        assert_eq!(removed.selected_queue_index, Some(0));

        bridge.command(BridgeCommand::Shutdown);
    }

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_natural_handoff_advances_current_track_and_keeps_playing() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");
        let first = p("/tmp/ferrous_gapless_case_a.flac");
        let second = p("/tmp/ferrous_gapless_case_b.flac");
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![first.clone(), second.clone()],
            autoplay: true,
        }));
        let loaded = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            let fc = first.clone();
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), |s| {
                        s.queue_len == 2
                            && s.playback.current.as_ref() == Some(&fc)
                            && s.playback.state == crate::playback::PlaybackState::Playing
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("loaded first track")
        };
        assert_eq!(loaded.playback.current.as_ref(), Some(&first));

        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        // Poll with repeated RequestSnapshot so at least one snapshot
        // includes the queue (heartbeat snapshots omit it).
        let handed_off = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                let sc = second.clone();
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), move |s| {
                        s.queue_len == 2
                            && s.playback.current.as_ref() == Some(&sc)
                            && s.playback.state == crate::playback::PlaybackState::Playing
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("handoff to second track")
        };
        assert_eq!(handed_off.playback.current.as_ref(), Some(&second));

        bridge.command(BridgeCommand::Shutdown);
    }

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_reports_each_play_of_a_repeated_or_duplicated_file() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn().expect("spawn bridge");
        let track = p("/tmp/ferrous_repeat_case.flac");
        let poll_for = |predicate: &dyn Fn(&BridgeSnapshot) -> bool| {
            let deadline = Instant::now() + Duration::from_secs(10);
            while Instant::now() < deadline {
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snapshot) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), predicate)
                {
                    return snapshot;
                }
            }
            panic!("no matching snapshot");
        };
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![track.clone(), track.clone()],
            autoplay: true,
        }));
        let first = poll_for(&|s| {
            s.queue_len == 2
                && s.playback.current.as_ref() == Some(&track)
                && s.playback.state == crate::playback::PlaybackState::Playing
        });
        assert_eq!(first.playback.current_queue_index, Some(0));

        bridge.command(BridgeCommand::Playback(
            BridgePlaybackCommand::SetRepeatMode(RepeatMode::One),
        ));
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        let replay = poll_for(&|s| s.playback.track_token > first.playback.track_token);
        assert_eq!(replay.playback.current_queue_index, Some(0));
        assert_eq!(replay.playback.current.as_ref(), Some(&track));

        bridge.command(BridgeCommand::Playback(
            BridgePlaybackCommand::SetRepeatMode(RepeatMode::Off),
        ));
        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        let second_row = poll_for(&|s| s.playback.track_token > replay.playback.track_token);
        assert_eq!(second_row.playback.current_queue_index, Some(1));
        assert_eq!(second_row.playback.current.as_ref(), Some(&track));

        bridge.command(BridgeCommand::Shutdown);
    }

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_natural_handoff_keeps_old_metadata_until_new_metadata_arrives() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn_with_metadata_delay(Duration::from_secs(2))
            .expect("spawn bridge");
        let first = p("/tmp/ferrous_metadata_case_a.flac");
        let second = p("/tmp/ferrous_metadata_case_b.flac");
        let first_title = "ferrous_metadata_case_a";
        let second_title = "ferrous_metadata_case_b";

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![first.clone(), second.clone()],
            autoplay: true,
        }));
        let first_loaded = {
            let deadline = Instant::now() + Duration::from_secs(8);
            let mut result = None;
            let fc = first.clone();
            let ft = first_title.to_string();
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), |s| {
                        s.queue_len == 2
                            && s.playback.current.as_ref() == Some(&fc)
                            && s.metadata.title == ft
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("first track + metadata loaded")
        };
        assert_eq!(first_loaded.metadata.title, first_title);

        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        // Wait for the handoff with repeated RequestSnapshot to ensure at
        // least one snapshot includes the queue.
        let second_for_handoff = second.clone();
        let first_title_str = first_title.to_string();
        let handoff_snapshot = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(100), |s| {
                        s.queue_len == 2
                            && s.playback.current.as_ref() == Some(&second_for_handoff)
                            && s.metadata.title == first_title_str
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("handoff snapshot keeps old metadata before new metadata arrives")
        };
        assert_eq!(handoff_snapshot.playback.current.as_ref(), Some(&second));
        assert_eq!(handoff_snapshot.metadata.title, first_title);

        let second_title_str = second_title.to_string();
        let second_for_meta = second.clone();
        let updated_metadata = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(100), |s| {
                        s.queue_len == 2
                            && s.playback.current.as_ref() == Some(&second_for_meta)
                            && s.metadata.title == second_title_str
                    })
                {
                    result = Some(snap);
                    break;
                }
            }
            result.expect("metadata updated for handed-off track")
        };
        assert_eq!(updated_metadata.metadata.title, second_title);

        bridge.command(BridgeCommand::Shutdown);
    }

    /// A runtime with a library database of its own, whose engine commands
    /// the test reads, and a library folder with one artist's album.
    struct LibraryOrderFixture {
        runtime: BridgeLoopRuntime,
        /// The library worker's events, which the test hands to the runtime
        /// one at a time; the runtime's own receiver gets none.
        library_rx: Receiver<LibraryEvent>,
        event_tx: Sender<BridgeEvent>,
        _event_rx: Receiver<BridgeEvent>,
        _engine_rx: Receiver<PlaybackCommand>,
        folder: PathBuf,
        root: PathBuf,
        album: PathBuf,
    }

    impl LibraryOrderFixture {
        fn new(name: &str, tracks: &[&str]) -> Self {
            static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
            let folder = crate::paths::test_root().join(format!(
                "bridge-library-order-{name}-{}",
                NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            let album = folder.join("music/Artist/Album");
            std::fs::create_dir_all(&album).expect("album folder");
            let root = folder.join("music").canonicalize().expect("canonical root");
            let album = root.join("Artist/Album");
            let mut runtime = BridgeLoopRuntime::new(
                BridgeRuntimeOptions {
                    library_db: Some(folder.join("library.sqlite3")),
                    ..BridgeRuntimeOptions::default()
                },
                None,
            );
            let (engine_tx, engine_rx) = unbounded();
            runtime.playback = PlaybackEngine::from_command_sender(engine_tx);
            let library_rx = std::mem::replace(&mut runtime.library_rx, never());
            let (event_tx, event_rx) = unbounded();
            let mut fixture = Self {
                runtime,
                library_rx,
                event_tx,
                _event_rx: event_rx,
                _engine_rx: engine_rx,
                folder,
                root,
                album,
            };
            for track in tracks {
                fixture.write(track);
            }
            fixture.command(BridgeLibraryCommand::AddRoot {
                path: fixture.root.clone(),
                name: String::new(),
            });
            fixture.wait_for_tracks(tracks.len());
            fixture
        }

        fn write(&self, track: &str) {
            std::fs::write(self.album.join(track), track.as_bytes()).expect("track file");
        }

        fn command(&mut self, command: BridgeLibraryCommand) {
            let event_tx = self.event_tx.clone();
            self.runtime.handle_wake(
                BridgeLoopWake::Command(BridgeCommand::Library(command)),
                &event_tx,
            );
        }

        /// Applies the library worker's events until the bridge has `count`
        /// tracks and no scan runs.
        fn wait_for_tracks(&mut self, count: usize) {
            let deadline = Instant::now() + Duration::from_secs(20);
            while self.runtime.state.library.tracks.len() != count
                || self.runtime.state.library_status.scan_in_progress
            {
                let event = self
                    .library_rx
                    .recv_deadline(deadline)
                    .expect("the library reaches the expected tracks");
                self.apply(event);
            }
        }

        /// Applies the library worker's events until a scan has started and
        /// finished, and returns the paths that the scan's events upserted
        /// or removed. Each event is seen, so the scan's status is too.
        fn wait_for_scan(&mut self) -> Vec<PathBuf> {
            let deadline = Instant::now() + Duration::from_secs(20);
            let mut started = false;
            let mut changed = Vec::new();
            loop {
                let event = self
                    .library_rx
                    .recv_deadline(deadline)
                    .expect("the scan finishes");
                let scanning = event.status.scan_in_progress;
                started |= scanning;
                if started {
                    changed.extend(
                        event
                            .changes
                            .upserted
                            .iter()
                            .map(|track| track.path.clone()),
                    );
                    changed.extend(event.changes.removed.iter().cloned());
                }
                self.apply(event);
                if started && !scanning {
                    return changed;
                }
            }
        }

        fn apply(&mut self, event: LibraryEvent) {
            let event_tx = self.event_tx.clone();
            self.runtime
                .handle_wake(BridgeLoopWake::Library(event), &event_tx);
        }

        /// The queue after `command` appends to an empty queue.
        fn queued_by(&mut self, command: BridgeLibraryCommand) -> Vec<String> {
            self.runtime.state.queue.clear();
            self.command(command);
            let deadline = Instant::now() + Duration::from_secs(10);
            let event_tx = self.event_tx.clone();
            while self.runtime.state.queue.is_empty() {
                if let Ok(result) = self.runtime.imports.results.recv_deadline(deadline) {
                    self.runtime
                        .handle_wake(BridgeLoopWake::Import(result), &event_tx);
                } else {
                    panic!("nothing was queued");
                }
            }
            self.runtime
                .state
                .queue
                .iter()
                .map(|path| {
                    path.file_name()
                        .expect("name")
                        .to_string_lossy()
                        .into_owned()
                })
                .collect()
        }

        fn album_key(&self) -> BridgeLibraryCommand {
            let root = self.root.to_string_lossy();
            BridgeLibraryCommand::AppendAlbumByKey {
                artist: format!("artist|{root}|Artist"),
                album: format!("album|{root}|Artist|Album"),
            }
        }
    }

    impl Drop for LibraryOrderFixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.folder);
        }
    }

    #[test]
    fn the_library_watches_its_roots_as_the_setting_says() {
        let _guard = test_guard();
        let folder = crate::paths::test_root().join("bridge-library-watching");
        let _ = std::fs::remove_dir_all(&folder);
        std::fs::create_dir_all(folder.join("music")).expect("library folder");
        let root = folder.join("music").canonicalize().expect("canonical root");
        std::fs::write(root.join("a.mp3"), b"a").expect("track");
        let mut runtime = BridgeLoopRuntime::new(
            BridgeRuntimeOptions {
                library_db: Some(folder.join("library.sqlite3")),
                library_watching: true,
                ..BridgeRuntimeOptions::default()
            },
            None,
        );
        assert!(runtime.state.settings.library_watch_enabled);
        let (event_tx, _event_rx) = unbounded();
        let run = |runtime: &mut BridgeLoopRuntime, command| {
            runtime.handle_wake(BridgeLoopWake::Command(command), &event_tx);
        };
        let wait_for = |runtime: &mut BridgeLoopRuntime, count: usize| {
            let (event_tx, _event_rx) = unbounded();
            let deadline = Instant::now() + Duration::from_secs(20);
            while runtime.state.library.tracks.len() != count
                || runtime.state.library_status.scan_in_progress
            {
                let event = runtime
                    .library_rx
                    .recv_deadline(deadline)
                    .unwrap_or_else(|_| panic!("the library reaches {count} tracks"));
                runtime.handle_wake(BridgeLoopWake::Library(event), &event_tx);
            }
        };
        run(
            &mut runtime,
            BridgeCommand::Library(BridgeLibraryCommand::AddRoot {
                path: root.clone(),
                name: String::new(),
            }),
        );
        wait_for(&mut runtime, 1);

        // Seen by the watcher: no rescan was asked for.
        std::fs::write(root.join("b.mp3"), b"b").expect("track");
        wait_for(&mut runtime, 2);

        run(
            &mut runtime,
            BridgeCommand::Settings(BridgeSettingsCommand::SetLibraryWatchEnabled(false)),
        );
        assert!(!runtime.state.settings.library_watch_enabled);
        assert!(runtime.flags.settings_dirty);
        std::fs::write(root.join("c.mp3"), b"c").expect("track");
        // Turning watching on rescans the roots.
        run(
            &mut runtime,
            BridgeCommand::Settings(BridgeSettingsCommand::SetLibraryWatchEnabled(true)),
        );
        wait_for(&mut runtime, 3);
        drop(runtime);
        let _ = std::fs::remove_dir_all(&folder);
    }

    #[test]
    fn a_track_added_by_a_rescan_is_queued_in_its_album_place() {
        let _guard = test_guard();
        let mut fixture = LibraryOrderFixture::new("rescan", &["02.mp3", "03.mp3"]);
        fixture.write("01.mp3");
        fixture.command(BridgeLibraryCommand::RescanRoot(fixture.root.clone()));
        fixture.wait_for_tracks(3);

        let expected = ["01.mp3", "02.mp3", "03.mp3"];
        let album = fixture.album_key();
        assert_eq!(fixture.queued_by(album), expected);
        assert_eq!(
            fixture.queued_by(BridgeLibraryCommand::AppendArtistByKey {
                artist: format!("artist|{}|Artist", fixture.root.to_string_lossy()),
            }),
            expected
        );
        assert_eq!(
            fixture.queued_by(BridgeLibraryCommand::AppendRootByPath {
                root: fixture.root.to_string_lossy().into_owned(),
            }),
            expected
        );
        assert_eq!(
            fixture.queued_by(BridgeLibraryCommand::AppendAllTracks),
            expected
        );
        // The bridge's tracks are in the order a full read of the database has.
        let mut read = LibrarySnapshot::default();
        crate::library::schema_load_snapshot_for_test(
            &fixture.folder.join("library.sqlite3"),
            &mut read,
        );
        let paths = |library: &LibrarySnapshot| {
            library
                .tracks
                .iter()
                .map(|track| track.path.clone())
                .collect::<Vec<_>>()
        };
        assert_eq!(paths(&fixture.runtime.state.library), paths(&read));
    }

    #[test]
    fn an_album_is_queued_in_the_order_the_tree_shows_it() {
        let _guard = test_guard();
        // Unnumbered files: the tree orders them by collated path, and the
        // database by bytes, which put "Zebra" first.
        let fixture_tracks = ["Zebra.mp3", "Éclair.mp3", "élan 10.mp3", "élan 9.mp3"];
        let mut fixture = LibraryOrderFixture::new("collated", &fixture_tracks);
        let expected = ["Éclair.mp3", "élan 9.mp3", "élan 10.mp3", "Zebra.mp3"];
        let album = fixture.album_key();
        assert_eq!(fixture.queued_by(album), expected);
        let root = fixture.root.to_string_lossy().into_owned();
        assert_eq!(
            fixture.queued_by(BridgeLibraryCommand::AppendAlbumByKey {
                artist: "Artist".to_string(),
                album: "Album".to_string(),
            }),
            expected
        );
        for command in [
            BridgeLibraryCommand::AppendArtistByKey {
                artist: format!("artist|{root}|Artist"),
            },
            BridgeLibraryCommand::AppendRootByPath { root },
            BridgeLibraryCommand::AppendAllTracks,
        ] {
            assert_eq!(fixture.queued_by(command), expected);
        }
        // The bridge keeps the database's order.
        assert_eq!(
            fixture
                .runtime
                .state
                .library
                .tracks
                .iter()
                .map(|track| track
                    .path
                    .file_name()
                    .expect("name")
                    .to_string_lossy()
                    .into_owned())
                .collect::<Vec<_>>(),
            ["Zebra.mp3", "Éclair.mp3", "élan 10.mp3", "élan 9.mp3"]
        );
    }

    #[test]
    fn a_renamed_track_is_queued_in_its_new_place() {
        let _guard = test_guard();
        let mut fixture = LibraryOrderFixture::new("rename", &["01.mp3", "02.mp3", "03.mp3"]);
        // A rename on disk and in the database, as the tag editor's.
        let old = fixture.album.join("03.mp3");
        let new = fixture.album.join("00.mp3");
        std::fs::rename(&old, &new).expect("rename the file");
        rusqlite::Connection::open(fixture.folder.join("library.sqlite3"))
            .and_then(|conn| {
                conn.execute(
                    "UPDATE tracks SET path = ?2 WHERE path = ?1",
                    rusqlite::params![old.to_string_lossy(), new.to_string_lossy()],
                )
            })
            .expect("rename the row");
        let event_tx = fixture.event_tx.clone();
        let _ = fixture.runtime.apply_renamed_paths(
            1,
            Arc::new(PathRenames::from([(old.clone(), new.clone())])),
            imports::RenamedDetails::default(),
            crate::library::next_file_sequence(),
            &event_tx,
        );

        let album = fixture.album_key();
        assert_eq!(fixture.queued_by(album), ["00.mp3", "01.mp3", "02.mp3"]);
        // The rows the rename changed are read again, and the renamed file
        // changed since, so the rescan reads it too; the order stays.
        crate::tag_write::write_test_mp3_with_only_id3v1(
            &new,
            &[
                (lofty::tag::ItemKey::TrackTitle, "Zero"),
                (lofty::tag::ItemKey::TrackArtist, "Artist"),
                (lofty::tag::ItemKey::AlbumTitle, "Album"),
            ],
        );
        fixture.command(BridgeLibraryCommand::RescanRoot(fixture.root.clone()));
        assert!(fixture.wait_for_scan().contains(&new));
        let position = fixture
            .runtime
            .state
            .library_index
            .position(&fixture.runtime.state.library, &new)
            .expect("the renamed track");
        assert_eq!(fixture.runtime.state.library.tracks[position].title, "Zero");
        let album = fixture.album_key();
        assert_eq!(fixture.queued_by(album), ["00.mp3", "01.mp3", "02.mp3"]);
    }

    #[test]
    fn a_rescan_that_reaches_a_root_after_a_rename_keeps_what_it_read_there() {
        let _guard = test_guard();
        let mut fixture = LibraryOrderFixture::new("rescan-all-rename", &["01.mp3"]);
        // A second root, which a rescan of all roots reads after the first.
        let next = fixture.folder.join("next");
        std::fs::create_dir_all(&next).expect("second root");
        let next = next.canonicalize().expect("canonical root");
        let [a, b, c, d] = ["a", "b", "c", "d"].map(|name| next.join(format!("{name}.flac")));
        for (path, title) in [(&a, "A"), (&b, "B"), (&c, "C")] {
            crate::tag_write::write_test_flac(path, &[("TITLE", title)]);
        }
        fixture.command(BridgeLibraryCommand::AddRoot {
            path: next.clone(),
            name: String::new(),
        });
        fixture.wait_for_tracks(4);
        let refreshes = gated_refreshes(&mut fixture.runtime);
        let event_tx = fixture.event_tx.clone();

        // The rescan waits in the first root.
        let first = fixture.album.join("01.mp3");
        std::fs::write(&first, b"01.mp3, changed").expect("change the first root");
        let (started, release) = crate::library::hold_read(&first);
        fixture.command(BridgeLibraryCommand::RescanAll);
        started
            .recv_timeout(Duration::from_secs(20))
            .expect("the rescan reads the first root");

        // Meanwhile a and b swap names and c becomes d, as the tag editor
        // renames them: on disk, in the database, and through the gate,
        // whose notification waits.
        let moved = next.join("moved.flac");
        for (from, to) in [(&a, &moved), (&b, &a), (&moved, &b), (&c, &d)] {
            std::fs::rename(from, to).expect("rename the file");
            rusqlite::Connection::open(fixture.folder.join("library.sqlite3"))
                .and_then(|conn| {
                    conn.execute(
                        "UPDATE tracks SET path = ?2 WHERE path = ?1",
                        rusqlite::params![from.to_string_lossy(), to.to_string_lossy()],
                    )
                })
                .expect("rename the row");
        }
        rename_on_disk(
            &fixture.runtime,
            &refreshes,
            &[(&a, &b), (&b, &a), (&c, &d)],
            &[(&a, "B"), (&b, "A"), (&d, "C")],
        );
        // Saves of b and d are read and applied, and kept for the rename.
        refreshes.set(&[(&b, "A, saved"), (&d, "C, saved")]);
        save(&mut fixture.runtime, &refreshes, &[&b, &d], &event_tx);
        accept_refresh(&mut fixture.runtime, &refreshes, &event_tx);
        // Then b changes again and d is deleted, before the rescan reaches
        // the second root.
        crate::tag_write::write_test_flac(&b, &[("TITLE", "A, changed again")]);
        std::fs::remove_file(&d).expect("delete d");
        drop(release);
        let scanned = fixture.wait_for_scan();
        assert!(
            scanned.contains(&b) && scanned.contains(&d),
            "the rescan read b and found d gone: {scanned:?}"
        );

        // What the rescan read in the second root is newer than the rename
        // and the saves: b keeps the rescan's row, and d stays gone.
        apply_next_import_result(&mut fixture.runtime, &event_tx);
        let titles = library_titles(&fixture.runtime);
        let titles: HashMap<&Path, &str> = titles
            .iter()
            .filter(|(path, _)| path.starts_with(&next))
            .map(|(path, title)| (path.as_path(), title.as_str()))
            .collect();
        assert_eq!(
            titles,
            HashMap::from([(a.as_path(), "B"), (b.as_path(), "A, changed again")])
        );
    }
}
