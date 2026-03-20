use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::{BufRead, Cursor};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{after, bounded, select, unbounded, Receiver, Sender, TrySendError};
use serde_json::json;
use walkdir::WalkDir;

use crate::analysis::{
    AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot, SpectrogramViewMode,
};
use crate::artwork::apply_artwork_to_track;
use crate::lastfm::{
    self, Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    NowPlayingTrack as LastFmNowPlayingTrack, RuntimeState as LastFmRuntimeState,
    ScrobbleEntry as LastFmScrobbleEntry, ServiceOptions as LastFmServiceOptions,
};
use crate::library::{
    is_supported_audio, load_external_track_cache, load_external_track_caches, read_track_info,
    refresh_cover_paths_for_tracks, refresh_cover_paths_for_tracks_with_override,
    refresh_indexed_metadata_for_paths, search_tracks_fts, store_external_track_cache,
    track_file_fingerprint, IndexedTrack, LibraryCommand, LibraryEvent, LibraryRoot,
    LibrarySearchTrack, LibraryService, LibrarySnapshot, LibraryTrack, TrackFileFingerprint,
};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{
    PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode,
    TrackChangeKind,
};

pub mod ffi;
pub mod library_tree;

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
    Library(BridgeLibraryCommand),
    Analysis(BridgeAnalysisCommand),
    Settings(BridgeSettingsCommand),
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
    Move {
        from: usize,
        to: usize,
    },
    Select(Option<usize>),
    Clear,
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
    ApplyAlbumArt {
        track_path: PathBuf,
        artwork_path: PathBuf,
    },
    SetNodeExpanded {
        key: String,
        expanded: bool,
    },
    SetSearchQuery {
        seq: u32,
        query: String,
    },
    RefreshEditedPaths(Vec<PathBuf>),
    RefreshRenamedPaths(Vec<(PathBuf, PathBuf)>),
}

#[derive(Debug, Clone)]
pub enum BridgeAnalysisCommand {
    SetFftSize(usize),
}

#[derive(Debug, Clone)]
pub enum BridgeSettingsCommand {
    LoadFromDisk,
    SaveToDisk,
    SetVolume(f32),
    SetFftSize(usize),
    SetSpectrogramViewMode(SpectrogramViewMode),
    SetViewerFullscreenMode(ViewerFullscreenMode),
    SetDbRange(f32),
    SetLogScale(bool),
    SetShowFps(bool),
    SetSystemMediaControlsEnabled(bool),
    SetLibrarySortMode(LibrarySortMode),
    SetLastFmScrobblingEnabled(bool),
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
    Error(String),
    Stopped,
}

#[derive(Debug, Clone)]
pub struct BridgeSearchResultsFrame {
    pub seq: u32,
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
    pub(crate) queue_details: HashMap<PathBuf, IndexedTrack>,
    pub library_artist_count: usize,
    pub library_album_count: usize,
    pub pre_built_tree_bytes: Option<Arc<Vec<u8>>>,
    pub queue_included: bool,
    pub queue: Vec<PathBuf>,
    pub selected_queue_index: Option<usize>,
    pub settings: BridgeSettings,
    pub lastfm: LastFmRuntimeState,
}

#[derive(Debug, Clone)]
pub struct BridgeDisplaySettings {
    pub log_scale: bool,
    pub show_fps: bool,
}

#[derive(Debug, Clone)]
pub struct BridgeIntegrationSettings {
    pub system_media_controls_enabled: bool,
    pub lastfm_scrobbling_enabled: bool,
    pub lastfm_username: String,
}

#[derive(Debug, Clone)]
pub struct BridgeSettings {
    pub volume: f32,
    pub fft_size: usize,
    pub spectrogram_view_mode: SpectrogramViewMode,
    pub viewer_fullscreen_mode: ViewerFullscreenMode,
    pub db_range: f32,
    pub display: BridgeDisplaySettings,
    pub library_sort_mode: LibrarySortMode,
    pub integrations: BridgeIntegrationSettings,
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
            viewer_fullscreen_mode: ViewerFullscreenMode::WithinWindow,
            db_range: 90.0,
            display: BridgeDisplaySettings {
                log_scale: false,
                show_fps,
            },
            library_sort_mode: LibrarySortMode::Year,
            integrations: BridgeIntegrationSettings {
                system_media_controls_enabled: true,
                lastfm_scrobbling_enabled: false,
                lastfm_username: String::new(),
            },
        }
    }
}

#[derive(Debug, Clone, Default)]
struct BridgeState {
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    library: Arc<LibrarySnapshot>,
    queue_details: HashMap<PathBuf, IndexedTrack>,
    queue_detail_fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    pending_queue_detail_fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    library_artist_count: usize,
    library_album_count: usize,
    pre_built_tree_bytes: Arc<Vec<u8>>,
    expanded_keys: HashSet<String>,
    queue: Vec<PathBuf>,
    selected_queue_index: Option<usize>,
    settings: BridgeSettings,
    lastfm: LastFmRuntimeState,
    pending_search_results: Option<BridgeSearchResultsFrame>,
    pending_waveform_track: Option<PendingWaveformTrack>,
}

#[derive(Debug, Clone, Default)]
struct LastFmPlaybackTracker {
    active_path: Option<PathBuf>,
    artist: String,
    track: String,
    album: String,
    track_number: Option<u32>,
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

#[derive(Debug)]
struct SearchWorkerQuery {
    seq: u32,
    query: String,
    library: Arc<LibrarySnapshot>,
}

#[derive(Debug, Clone)]
struct ExternalQueueDetailsRequest {
    path: PathBuf,
    fingerprint: TrackFileFingerprint,
}

#[derive(Debug, Clone)]
struct ExternalQueueDetailsEvent {
    path: PathBuf,
    fingerprint: TrackFileFingerprint,
    indexed: IndexedTrack,
}

#[derive(Debug, Clone)]
struct ApplyAlbumArtRequest {
    track_path: PathBuf,
    artwork_path: PathBuf,
}

#[derive(Debug, Clone)]
struct ApplyAlbumArtEvent {
    track_path: PathBuf,
    indexed_by_path: HashMap<PathBuf, IndexedTrack>,
    error: Option<String>,
}

#[derive(Debug, Clone, Default)]
struct AlbumInventoryAcc {
    main_track_count: u32,
    main_total_length: f32,
    has_main_duration: bool,
}

#[derive(Debug, Clone)]
struct PreparedSearchTrack {
    path: PathBuf,
    root_path: PathBuf,
    path_string: String,
    path_lower: String,
    title: String,
    artist: String,
    album: String,
    cover_path: String,
    genre: String,
    year: Option<i32>,
    track_no: Option<u32>,
    duration_secs: Option<f32>,
    title_l: String,
    artist_l: String,
    album_l: String,
    genre_l: String,
    haystack_l: String,
}

#[derive(Debug, Clone, Default)]
struct PreparedSearchLibrary {
    tracks: Vec<PreparedSearchTrack>,
    album_inventory: HashMap<String, AlbumInventoryAcc>,
}

#[derive(Default)]
struct SearchWorkerPreparedCache {
    source_library: Option<Arc<LibrarySnapshot>>,
    source_search_revision: Option<u64>,
    prepared: Option<Arc<PreparedSearchLibrary>>,
}

impl SearchWorkerPreparedCache {
    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    fn prepared_for(&mut self, library: &Arc<LibrarySnapshot>) -> Arc<PreparedSearchLibrary> {
        if let (Some(source), Some(prepared)) = (&self.source_library, &self.prepared) {
            let revision = library.search_revision;
            if revision != 0 && self.source_search_revision == Some(revision) {
                self.source_library = Some(Arc::clone(library));
                return Arc::clone(prepared);
            }
            if revision == 0 && Arc::ptr_eq(source, library) {
                return Arc::clone(prepared);
            }
        }
        #[allow(unused_variables)]
        let started = Instant::now();
        let prepared = Arc::new(prepare_search_library(library.as_ref()));
        if search_profile_enabled() {
            profile_eprintln!(
                "[search-worker] cache rebuild tracks={} elapsed_ms={}",
                prepared.tracks.len(),
                started.elapsed().as_millis()
            );
        }
        self.source_library = Some(Arc::clone(library));
        self.source_search_revision =
            (library.search_revision != 0).then_some(library.search_revision);
        self.prepared = Some(Arc::clone(&prepared));
        prepared
    }
}

enum SearchBuildOutcome {
    Frame(BridgeSearchResultsFrame),
    Cancelled(SearchWorkerQuery),
}

enum SearchFallbackOutcome {
    Hits(Vec<LibrarySearchTrack>),
    Cancelled(SearchWorkerQuery),
}

impl BridgeState {
    fn snapshot(&self, include_tree: bool, include_queue: bool) -> BridgeSnapshot {
        let mut metadata = metadata_for_snapshot(&self.metadata);
        metadata.current_bitrate_kbps = self
            .metadata
            .displayed_bitrate_kbps(self.playback.position.as_secs_f64());
        BridgeSnapshot {
            playback: self.playback.clone(),
            analysis: self.analysis.clone(),
            metadata,
            library: self.library.clone(),
            queue_details: self.queue_details.clone(),
            library_artist_count: self.library_artist_count,
            library_album_count: self.library_album_count,
            pre_built_tree_bytes: if include_tree {
                Some(self.pre_built_tree_bytes.clone())
            } else {
                None
            },
            queue_included: include_queue,
            queue: if include_queue {
                self.queue.clone()
            } else {
                Vec::new()
            },
            selected_queue_index: self.selected_queue_index,
            settings: self.settings.clone(),
            lastfm: self.lastfm.clone(),
        }
    }

    fn rebuild_pre_built_tree(&mut self) {
        library_tree::retain_valid_expanded_keys(&self.library, &mut self.expanded_keys);
        (self.library_artist_count, self.library_album_count) =
            library_tree::compute_artist_album_counts(&self.library);
        self.pre_built_tree_bytes = Arc::new(library_tree::build_library_tree_flat_binary(
            &self.library,
            self.settings.library_sort_mode,
            Some(&self.expanded_keys),
        ));
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
struct SessionSnapshot {
    queue: Vec<PathBuf>,
    selected_queue_index: Option<usize>,
    current_queue_index: Option<usize>,
    current_path: Option<PathBuf>,
}

fn metadata_for_snapshot(metadata: &TrackMetadata) -> TrackMetadata {
    TrackMetadata {
        source_path: metadata.source_path.clone(),
        title: metadata.title.clone(),
        artist: metadata.artist.clone(),
        album: metadata.album.clone(),
        genre: metadata.genre.clone(),
        year: metadata.year,
        sample_rate_hz: metadata.sample_rate_hz,
        bitrate_kbps: metadata.bitrate_kbps,
        channels: metadata.channels,
        bit_depth: metadata.bit_depth,
        format_label: metadata.format_label.clone(),
        current_bitrate_kbps: metadata.current_bitrate_kbps,
        bitrate_timeline_kbps: Vec::new(),
        cover_art_path: metadata.cover_art_path.clone(),
        // Large RGBA cover payload is not needed in bridge snapshots; avoid per-snapshot megabyte clones.
        cover_art_rgba: None,
    }
}

pub struct FrontendBridgeHandle {
    tx: Sender<BridgeCommand>,
    rx: Receiver<BridgeEvent>,
}

#[derive(Debug, Clone, Copy, Default)]
struct BridgeRuntimeOptions {
    metadata_delay: Duration,
}

impl FrontendBridgeHandle {
    #[must_use]
    pub fn spawn() -> Self {
        Self::spawn_with_options(BridgeRuntimeOptions::default())
    }

    #[cfg(all(test, not(feature = "gst")))]
    fn spawn_with_metadata_delay(metadata_delay: Duration) -> Self {
        Self::spawn_with_options(BridgeRuntimeOptions { metadata_delay })
    }

    fn spawn_with_options(options: BridgeRuntimeOptions) -> Self {
        let (cmd_tx, cmd_rx) = unbounded::<BridgeCommand>();
        // Keep snapshot/event queue bounded so a slow UI consumer cannot grow memory unbounded.
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);

        let _ = std::thread::Builder::new()
            .name("ferrous-bridge".to_string())
            .spawn(move || run_bridge_loop(&cmd_rx, &event_tx, options));
        Self {
            tx: cmd_tx,
            rx: event_rx,
        }
    }

    pub fn command(&self, cmd: BridgeCommand) {
        let _ = self.tx.send(cmd);
    }

    #[must_use]
    pub fn recv_timeout(&self, timeout: Duration) -> Option<BridgeEvent> {
        self.rx.recv_timeout(timeout).ok()
    }

    #[must_use]
    pub fn try_recv(&self) -> Option<BridgeEvent> {
        self.rx.try_recv().ok()
    }

    pub(crate) fn into_parts(self) -> (Sender<BridgeCommand>, Receiver<BridgeEvent>) {
        (self.tx, self.rx)
    }
}

struct BridgeLoopRuntime {
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
    external_queue_details_tx: Sender<ExternalQueueDetailsRequest>,
    external_queue_details_event_rx: Receiver<ExternalQueueDetailsEvent>,
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
    queue_detail_revalidate_interval: Duration,
    last_queue_detail_revalidate: Instant,
    tree_emit_interval: Duration,
    tree_emit_min_track_delta: usize,
    last_tree_emit_at: Option<Instant>,
    last_tree_emit_track_count: usize,
    deferred_tree_rebuild_at: Option<Instant>,
}

struct BridgeLoopFlags {
    running: bool,
    settings_dirty: bool,
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
    include_queue_in_next_snapshot: bool,
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
    Command(BridgeCommand),
    Playback(PlaybackEvent),
    Analysis(AnalysisEvent),
    Metadata(MetadataEvent),
    Library(LibraryEvent),
    SearchResults(BridgeSearchResultsFrame),
    ExternalQueueDetails(ExternalQueueDetailsEvent),
    ApplyAlbumArt(ApplyAlbumArtEvent),
    LastFm(LastFmEvent),
    Tick,
    Shutdown,
}

fn env_duration_ms(var: &str, default: u64, min: u64, max: u64) -> Duration {
    Duration::from_millis(
        std::env::var(var)
            .ok()
            .and_then(|raw| raw.parse::<u64>().ok())
            .map_or(default, |v| v.clamp(min, max)),
    )
}

impl BridgeLoopRuntime {
    fn new(options: BridgeRuntimeOptions) -> Self {
        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (playback, playback_rx) = PlaybackEngine::new(analysis.sender(), analysis.pcm_sender());
        let (metadata, metadata_rx) = MetadataService::new_with_delay(options.metadata_delay);
        let (library, library_rx) = LibraryService::new();
        let (search_query_tx, search_query_rx) = unbounded::<SearchWorkerQuery>();
        let (search_results_tx, search_results_rx) = unbounded::<BridgeSearchResultsFrame>();
        let (external_queue_details_tx, external_queue_details_rx) =
            unbounded::<ExternalQueueDetailsRequest>();
        let (external_queue_details_event_tx, external_queue_details_event_rx) =
            unbounded::<ExternalQueueDetailsEvent>();
        let (apply_album_art_tx, apply_album_art_rx) = unbounded::<ApplyAlbumArtRequest>();
        let (apply_album_art_event_tx, apply_album_art_event_rx) =
            unbounded::<ApplyAlbumArtEvent>();
        spawn_bridge_support_threads(
            search_query_rx,
            search_results_tx,
            external_queue_details_rx,
            external_queue_details_event_tx,
            apply_album_art_rx,
            apply_album_art_event_tx,
        );

        let mut state = BridgeState::default();
        load_settings_into(&mut state.settings);
        state.lastfm.enabled = state.settings.integrations.lastfm_scrobbling_enabled;
        let (lastfm, lastfm_rx) = spawn_lastfm_service(&state.settings);
        restore_initial_bridge_state(
            &mut state,
            &analysis,
            &playback,
            &external_queue_details_tx,
            &lastfm,
        );

        let playing_poll_interval = env_duration_ms("FERROUS_PLAYBACK_POLL_MS", 40, 8, 500);
        let paused_poll_interval =
            env_duration_ms("FERROUS_PLAYBACK_PAUSED_POLL_MS", 333, 125, 1000);
        let playing_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_PLAYING_HEARTBEAT_MS", 100, 33, 1000);
        let analysis_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_ANALYSIS_SNAPSHOT_MS", 16, 8, 1000);
        let paused_snapshot_interval =
            env_duration_ms("FERROUS_BRIDGE_PAUSED_HEARTBEAT_MS", 333, 125, 1000);
        Self {
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
            external_queue_details_tx,
            external_queue_details_event_rx,
            apply_album_art_tx,
            apply_album_art_rx: apply_album_art_event_rx,
            lastfm,
            lastfm_rx,
            state,
            lastfm_tracker: LastFmPlaybackTracker::default(),
            flags: BridgeLoopFlags {
                running: true,
                settings_dirty: false,
                session_dirty: false,
                pending_snapshot: SnapshotUrgency::None,
            },
            last_settings_save: Instant::now(),
            last_session_save: Instant::now(),
            last_saved_session: None,
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
                include_queue_in_next_snapshot: true,
            },
            queue_detail_revalidate_interval: Duration::from_secs(2),
            last_queue_detail_revalidate: Instant::now(),
            tree_emit_interval: scan_tree_emit_interval(),
            tree_emit_min_track_delta: scan_tree_emit_min_track_delta(),
            last_tree_emit_at: None,
            last_tree_emit_track_count: 0,
            deferred_tree_rebuild_at: None,
        }
    }

    fn run(&mut self, cmd_rx: &Receiver<BridgeCommand>, event_tx: &Sender<BridgeEvent>) {
        self.emit_initial_snapshot(event_tx);
        while self.flags.running {
            let wake = self.wait_for_wake(cmd_rx);
            self.handle_wake(wake, event_tx);
            self.maybe_log_profile();
            self.maybe_persist();
        }
        self.shutdown(event_tx);
    }

    fn emit_initial_snapshot(&mut self, event_tx: &Sender<BridgeEvent>) {
        if !self.emit_snapshot(event_tx, self.snapshot_plan.include_queue_in_next_snapshot) {
            self.flags.pending_snapshot = SnapshotUrgency::Immediate;
        }
    }

    fn wait_for_wake(&mut self, cmd_rx: &Receiver<BridgeCommand>) -> BridgeLoopWake {
        let wake_delay = self.next_wake_delay();
        select! {
            recv(cmd_rx) -> msg => {
                match msg {
                    Ok(cmd) => BridgeLoopWake::Command(cmd),
                    Err(_) => BridgeLoopWake::Shutdown,
                }
            }
            recv(&self.playback_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::Playback),
            recv(&self.analysis_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::Analysis),
            recv(&self.metadata_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::Metadata),
            recv(&self.library_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::Library),
            recv(&self.search_results_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::SearchResults),
            recv(&self.external_queue_details_event_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::ExternalQueueDetails),
            recv(&self.apply_album_art_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::ApplyAlbumArt),
            recv(&self.lastfm_rx) -> msg => msg.map_or(BridgeLoopWake::Tick, BridgeLoopWake::LastFm),
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
            self.next_queue_detail_revalidate_delay(),
            self.next_deferred_tree_rebuild_delay(),
            self.next_settings_save_delay(),
            self.next_session_save_delay(),
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

    fn next_queue_detail_revalidate_delay(&self) -> Option<Duration> {
        if self.state.queue.is_empty() && self.state.pending_queue_detail_fingerprints.is_empty() {
            return None;
        }
        Some(
            self.queue_detail_revalidate_interval
                .saturating_sub(self.last_queue_detail_revalidate.elapsed()),
        )
    }

    fn next_deferred_tree_rebuild_delay(&self) -> Option<Duration> {
        self.deferred_tree_rebuild_at
            .map(|deadline| deadline.saturating_duration_since(Instant::now()))
    }

    fn next_settings_save_delay(&self) -> Option<Duration> {
        self.flags
            .settings_dirty
            .then(|| Duration::from_secs(2).saturating_sub(self.last_settings_save.elapsed()))
    }

    fn next_session_save_delay(&self) -> Option<Duration> {
        self.flags
            .session_dirty
            .then(|| Duration::from_secs(2).saturating_sub(self.last_session_save.elapsed()))
    }

    fn handle_wake(&mut self, wake: BridgeLoopWake, event_tx: &Sender<BridgeEvent>) {
        let mut urgency = match wake {
            BridgeLoopWake::Command(cmd) => self.handle_command(cmd, event_tx),
            BridgeLoopWake::Playback(event) => self.handle_playback_event(event),
            BridgeLoopWake::Analysis(event) => self.handle_analysis_event(event, event_tx),
            BridgeLoopWake::Metadata(event) => self.handle_metadata_event(event),
            BridgeLoopWake::Library(event) => self.handle_library_event(event),
            BridgeLoopWake::SearchResults(frame) => self.handle_search_results(frame),
            BridgeLoopWake::ExternalQueueDetails(event) => {
                self.handle_external_queue_detail_event(event)
            }
            BridgeLoopWake::ApplyAlbumArt(event) => {
                self.handle_apply_album_art_event(event, event_tx)
            }
            BridgeLoopWake::LastFm(event) => self.handle_lastfm_event(event),
            BridgeLoopWake::Tick => SnapshotUrgency::None,
            BridgeLoopWake::Shutdown => {
                self.flags.running = false;
                SnapshotUrgency::None
            }
        };
        urgency = urgency.max(self.drain_pending_updates(event_tx));
        self.note_snapshot_urgency(urgency);
        self.maybe_emit_pending_snapshot(event_tx);
    }

    fn handle_command(
        &mut self,
        cmd: BridgeCommand,
        event_tx: &Sender<BridgeEvent>,
    ) -> SnapshotUrgency {
        let rebuild_tree =
            command_requires_tree_rebuild(&cmd, self.state.settings.library_sort_mode);
        let deferred_tree_rebuild = command_tree_rebuild_delay(&cmd);
        let refresh_queue_snapshot = command_requires_queue_snapshot(&cmd);
        let force_snapshot = matches!(cmd, BridgeCommand::RequestSnapshot);
        let mut command_context = BridgeCommandContext {
            playback: &self.playback,
            analysis: &self.analysis,
            metadata: &self.metadata,
            library: &self.library,
            lastfm: &self.lastfm,
            search_query_tx: &self.search_query_tx,
            external_queue_details_tx: &self.external_queue_details_tx,
            apply_album_art_tx: &self.apply_album_art_tx,
            event_tx,
            running: &mut self.flags.running,
            settings_dirty: &mut self.flags.settings_dirty,
        };
        let changed = handle_bridge_command(cmd, &mut self.state, &mut command_context);
        let mut urgency = SnapshotUrgency::None;
        if rebuild_tree {
            self.state.rebuild_pre_built_tree();
            self.snapshot_plan.include_tree_in_next_snapshot = true;
            urgency = SnapshotUrgency::Immediate;
        } else if changed {
            if let Some(delay) = deferred_tree_rebuild {
                self.deferred_tree_rebuild_at = Some(Instant::now() + delay);
            }
        }
        if changed {
            self.flags.session_dirty = true;
            urgency = SnapshotUrgency::Immediate;
            if refresh_queue_snapshot {
                self.snapshot_plan.include_queue_in_next_snapshot = true;
            }
        }
        if force_snapshot && self.flags.running {
            let include_queue = self.snapshot_plan.include_queue_in_next_snapshot || force_snapshot;
            if !self.emit_snapshot(event_tx, include_queue) {
                urgency = urgency.max(SnapshotUrgency::Immediate);
            }
        }
        urgency
    }

    fn handle_playback_event(&mut self, event: PlaybackEvent) -> SnapshotUrgency {
        let urgency =
            process_playback_event(event, &self.analysis, &self.metadata, &mut self.state);
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
        }
    }

    fn handle_metadata_event(&mut self, event: MetadataEvent) -> SnapshotUrgency {
        let urgency = process_metadata_event(event, &mut self.state);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
    }

    fn handle_library_event(&mut self, event: LibraryEvent) -> SnapshotUrgency {
        let urgency =
            process_library_event(event, &self.external_queue_details_tx, &mut self.state);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
            self.handle_library_refresh(urgency);
            if !self.state.library.scan_in_progress {
                let _ = self.search_query_tx.send(SearchWorkerQuery {
                    seq: 0,
                    query: String::new(),
                    library: Arc::clone(&self.state.library),
                });
            }
        }
        urgency
    }

    fn handle_search_results(&mut self, frame: BridgeSearchResultsFrame) -> SnapshotUrgency {
        process_search_results(frame, &mut self.state);
        SnapshotUrgency::None
    }

    fn handle_external_queue_detail_event(
        &mut self,
        event: ExternalQueueDetailsEvent,
    ) -> SnapshotUrgency {
        let urgency = process_external_queue_detail_event(event, &mut self.state);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
            self.snapshot_plan.include_queue_in_next_snapshot = true;
        }
        urgency
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

    fn handle_lastfm_event(&mut self, event: LastFmEvent) -> SnapshotUrgency {
        let urgency = process_lastfm_event(event, &mut self.state, &mut self.flags.settings_dirty);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
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
            &mut self.state,
        );
        let analysis_urgency = drain_analysis_events(&self.analysis_rx, &mut self.state);
        let metadata_urgency = drain_metadata_events(&self.metadata_rx, &mut self.state);
        let library_urgency = drain_library_events(
            &self.library_rx,
            &self.external_queue_details_tx,
            &mut self.state,
        );
        let apply_album_art_urgency = drain_apply_album_art_events(
            &self.apply_album_art_rx,
            &self.metadata,
            event_tx,
            &mut self.state,
        );
        let external_queue_details_urgency = drain_external_queue_detail_events(
            &self.external_queue_details_event_rx,
            &mut self.state,
        );
        let lastfm_urgency = drain_lastfm_events(
            &self.lastfm_rx,
            &mut self.state,
            &mut self.flags.settings_dirty,
        );
        drain_search_results(&self.search_results_rx, &mut self.state);
        tick_lastfm_playback(&self.state, &self.lastfm, &mut self.lastfm_tracker);
        self.revalidate_queue_details();
        if apply_album_art_urgency.is_pending() {
            self.deferred_tree_rebuild_at = Some(Instant::now() + Duration::from_millis(150));
        }
        self.handle_library_refresh(library_urgency);
        if external_queue_details_urgency.is_pending() {
            self.snapshot_plan.include_queue_in_next_snapshot = true;
        }
        urgency = urgency
            .max(analysis_urgency)
            .max(metadata_urgency)
            .max(library_urgency)
            .max(apply_album_art_urgency)
            .max(external_queue_details_urgency)
            .max(lastfm_urgency);
        if urgency.is_pending() {
            self.flags.session_dirty = true;
        }
        urgency
    }

    fn revalidate_queue_details(&mut self) {
        if self.last_queue_detail_revalidate.elapsed() < self.queue_detail_revalidate_interval {
            return;
        }
        if sync_queue_details(&mut self.state, &self.external_queue_details_tx) {
            self.snapshot_plan.include_queue_in_next_snapshot = true;
            self.note_snapshot_urgency(SnapshotUrgency::Immediate);
            self.flags.session_dirty = true;
        }
        self.last_queue_detail_revalidate = Instant::now();
    }

    fn handle_library_refresh(&mut self, library_urgency: SnapshotUrgency) {
        if !library_urgency.is_pending() {
            return;
        }
        self.deferred_tree_rebuild_at = None;
        self.snapshot_plan.include_queue_in_next_snapshot = true;
        let now = Instant::now();
        let track_delta = self
            .state
            .library
            .tracks
            .len()
            .saturating_sub(self.last_tree_emit_track_count);
        let scan_emit_due = self
            .last_tree_emit_at
            .is_none_or(|last| now.duration_since(last) >= self.tree_emit_interval);
        let should_emit_tree = !self.state.library.scan_in_progress
            || self.last_tree_emit_at.is_none()
            || (scan_emit_due && track_delta >= self.tree_emit_min_track_delta);
        if should_emit_tree {
            self.state.rebuild_pre_built_tree();
            self.snapshot_plan.include_tree_in_next_snapshot = true;
            self.note_snapshot_urgency(SnapshotUrgency::Immediate);
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
        self.state.rebuild_pre_built_tree();
        self.snapshot_plan.include_tree_in_next_snapshot = true;
        self.note_snapshot_urgency(SnapshotUrgency::Immediate);
    }

    fn note_snapshot_urgency(&mut self, urgency: SnapshotUrgency) {
        self.flags.pending_snapshot = self.flags.pending_snapshot.max(urgency);
    }

    fn maybe_emit_pending_snapshot(&mut self, event_tx: &Sender<BridgeEvent>) {
        match self.flags.pending_snapshot {
            SnapshotUrgency::None => {}
            SnapshotUrgency::Immediate => {
                let _ =
                    self.emit_snapshot(event_tx, self.snapshot_plan.include_queue_in_next_snapshot);
            }
            SnapshotUrgency::Analysis => {
                if self.last_snapshot_emit.elapsed() >= self.analysis_snapshot_interval {
                    let _ = self
                        .emit_snapshot(event_tx, self.snapshot_plan.include_queue_in_next_snapshot);
                }
            }
            SnapshotUrgency::Heartbeat => {
                let Some(interval) = self.snapshot_heartbeat_interval() else {
                    return;
                };
                if self.last_snapshot_emit.elapsed() >= interval {
                    let _ = self
                        .emit_snapshot(event_tx, self.snapshot_plan.include_queue_in_next_snapshot);
                }
            }
        }
    }

    fn emit_snapshot(&mut self, event_tx: &Sender<BridgeEvent>, include_queue: bool) -> bool {
        if send_snapshot_event(
            event_tx,
            &self.state,
            self.snapshot_plan.include_tree_in_next_snapshot,
            include_queue,
        ) {
            self.diagnostics.prof_snapshots_sent += 1;
            if self.snapshot_plan.include_tree_in_next_snapshot {
                self.last_tree_emit_at = Some(Instant::now());
                self.last_tree_emit_track_count = self.state.library.tracks.len();
            }
            self.last_snapshot_emit = Instant::now();
            self.snapshot_plan.include_tree_in_next_snapshot = false;
            if include_queue {
                self.snapshot_plan.include_queue_in_next_snapshot = false;
            }
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
        let _rss_kb = current_rss_kb();
        let _spectro_rows = self
            .state
            .analysis
            .spectrogram_channels
            .first()
            .map_or(0, |channel| channel.rows.len());
        let _spectro_bins = self
            .state
            .analysis
            .spectrogram_channels
            .first()
            .and_then(|channel| channel.rows.first())
            .map_or(0, std::vec::Vec::len);
        profile_eprintln!(
            "[bridge] rss_kb={} playback_q={} analysis_q={} metadata_q={} library_q={} wave_len={} spectro_rows={} spectro_bins={} sent_snap/s={} drop_snap/s={}",
            _rss_kb,
            self.playback_rx.len(),
            self.analysis_rx.len(),
            self.metadata_rx.len(),
            self.library_rx.len(),
            self.state.analysis.waveform_peaks.len(),
            _spectro_rows,
            _spectro_bins,
            self.diagnostics.prof_snapshots_sent,
            self.diagnostics.prof_snapshots_dropped
        );
        self.diagnostics.prof_snapshots_sent = 0;
        self.diagnostics.prof_snapshots_dropped = 0;
        self.diagnostics.profile_last = Instant::now();
    }

    fn maybe_persist(&mut self) {
        if self.flags.settings_dirty && self.last_settings_save.elapsed() >= Duration::from_secs(2)
        {
            save_settings(&self.state.settings);
            self.flags.settings_dirty = false;
            self.last_settings_save = Instant::now();
        }
        if !self.flags.session_dirty || self.last_session_save.elapsed() < Duration::from_secs(2) {
            return;
        }
        let session = session_snapshot_for_state(&self.state);
        if self.last_saved_session.as_ref() != Some(&session) {
            save_session_snapshot(&session);
            self.last_saved_session = Some(session);
        }
        self.flags.session_dirty = false;
        self.last_session_save = Instant::now();
    }

    fn shutdown(&mut self, event_tx: &Sender<BridgeEvent>) {
        save_settings(&self.state.settings);
        save_session_snapshot(&session_snapshot_for_state(&self.state));
        self.lastfm.command(LastFmCommand::Shutdown);
        let _ = try_send_event(event_tx, BridgeEvent::Stopped);
    }
}

fn spawn_bridge_support_threads(
    search_query_rx: Receiver<SearchWorkerQuery>,
    search_results_tx: Sender<BridgeSearchResultsFrame>,
    external_queue_details_rx: Receiver<ExternalQueueDetailsRequest>,
    external_queue_details_event_tx: Sender<ExternalQueueDetailsEvent>,
    apply_album_art_rx: Receiver<ApplyAlbumArtRequest>,
    apply_album_art_event_tx: Sender<ApplyAlbumArtEvent>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-bridge-search".to_string())
        .spawn(move || run_search_worker(&search_query_rx, &search_results_tx));
    let _ = std::thread::Builder::new()
        .name("ferrous-queue-details".to_string())
        .spawn(move || {
            run_external_queue_detail_worker(
                &external_queue_details_rx,
                &external_queue_details_event_tx,
            );
        });
    let _ = std::thread::Builder::new()
        .name("ferrous-apply-artwork".to_string())
        .spawn(move || run_apply_album_art_worker(&apply_album_art_rx, &apply_album_art_event_tx));
}

fn spawn_lastfm_service(settings: &BridgeSettings) -> (LastFmHandle, Receiver<LastFmEvent>) {
    let lastfm_queue_path = config_base_path().map(|base| lastfm::queue_path(&base));
    let (lastfm, lastfm_rx) = lastfm::spawn(LastFmServiceOptions {
        queue_path: lastfm_queue_path,
        initial_enabled: settings.integrations.lastfm_scrobbling_enabled,
    });
    if !settings.integrations.lastfm_username.trim().is_empty() {
        lastfm.command(LastFmCommand::LoadStoredSession {
            username: settings.integrations.lastfm_username.clone(),
        });
    }
    (lastfm, lastfm_rx)
}

fn restore_initial_bridge_state(
    state: &mut BridgeState,
    analysis: &AnalysisEngine,
    playback: &PlaybackEngine,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    lastfm: &LastFmHandle,
) {
    state.playback.volume = state.settings.volume;
    playback.command(PlaybackCommand::SetVolume(state.settings.volume));
    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));
    analysis.command(AnalysisCommand::SetSpectrogramViewMode(
        state.settings.spectrogram_view_mode,
    ));
    lastfm.command(LastFmCommand::SetEnabled(
        state.settings.integrations.lastfm_scrobbling_enabled,
    ));
    apply_session_restore(state, playback, load_session_snapshot().as_ref());
    if should_sync_queue_details_on_initial_restore(state) {
        let _ = sync_queue_details(state, external_queue_details_tx);
    }
    state.rebuild_pre_built_tree();
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
) {
    let mut runtime = BridgeLoopRuntime::new(options);
    runtime.run(cmd_rx, event_tx);
}

#[cfg_attr(
    not(feature = "profiling-logs"),
    allow(unused_variables, unused_assignments)
)]
fn run_search_worker(
    query_rx: &Receiver<SearchWorkerQuery>,
    results_tx: &Sender<BridgeSearchResultsFrame>,
) {
    let Ok(mut query) = query_rx.recv() else {
        return;
    };
    let mut prepared_cache = SearchWorkerPreparedCache::default();
    let profile_search = search_profile_enabled();
    loop {
        while let Ok(next) = query_rx.try_recv() {
            query = next;
        }

        #[allow(unused_variables)]
        let query_started = Instant::now();
        if query.seq == 0 && query.query.is_empty() {
            let _ = prepared_cache.prepared_for(&query.library);
            match query_rx.recv() {
                Ok(next) => {
                    query = next;
                }
                Err(_) => break,
            }
            continue;
        }
        match build_search_results_frame(&query, &mut prepared_cache, query_rx) {
            SearchBuildOutcome::Frame(frame) => {
                if profile_search {
                    profile_eprintln!(
                        "[search-worker] seq={} chars={} tracks={} rows={} elapsed_ms={}",
                        query.seq,
                        query.query.chars().count(),
                        query.library.tracks.len(),
                        frame.rows.len(),
                        query_started.elapsed().as_millis()
                    );
                }
                let _ = results_tx.send(frame);
            }
            SearchBuildOutcome::Cancelled(next) => {
                if profile_search {
                    profile_eprintln!(
                        "[search-worker] cancel seq={} -> {} elapsed_ms={}",
                        query.seq,
                        next.seq,
                        query_started.elapsed().as_millis()
                    );
                }
                query = next;
                continue;
            }
        }

        match query_rx.recv() {
            Ok(next) => {
                query = next;
            }
            Err(_) => break,
        }
    }
}

fn try_send_event(
    event_tx: &Sender<BridgeEvent>,
    event: BridgeEvent,
) -> Result<(), TrySendError<BridgeEvent>> {
    event_tx.try_send(event)
}

fn send_snapshot_event(
    event_tx: &Sender<BridgeEvent>,
    state: &BridgeState,
    include_tree: bool,
    include_queue: bool,
) -> bool {
    // Drop stale snapshot updates when the consumer is behind; next snapshot will replace it.
    if event_tx.is_full() {
        return false;
    }
    try_send_event(
        event_tx,
        BridgeEvent::Snapshot(Box::new(state.snapshot(include_tree, include_queue))),
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

fn search_profile_enabled() -> bool {
    cfg!(feature = "profiling-logs") && std::env::var_os("FERROUS_SEARCH_PROFILE").is_some()
}

fn search_fallback_limit() -> usize {
    std::env::var("FERROUS_SEARCH_FALLBACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(256, |v| v.clamp(64, 5_000))
}

fn search_short_query_char_threshold() -> usize {
    std::env::var("FERROUS_SEARCH_SHORT_QUERY_CHARS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(1, |v| v.clamp(1, 8))
}

fn search_fallback_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_FALLBACK_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(128, |v| v.clamp(64, 5_000))
}

fn search_artist_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ARTIST_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(5, |v| v.clamp(1, 400))
}

fn search_artist_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_ARTIST_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(5, |v| v.clamp(1, 400))
}

fn search_album_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ALBUM_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(10, |v| v.clamp(1, 800))
}

fn search_album_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_ALBUM_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(10, |v| v.clamp(1, 800))
}

fn search_track_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_TRACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(20, |v| v.clamp(1, 2_000))
}

fn search_track_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_TRACK_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(20, |v| v.clamp(1, 2_000))
}

fn search_cancel_poll_rows() -> usize {
    std::env::var("FERROUS_SEARCH_CANCEL_POLL_ROWS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(64, |v| v.clamp(16, 4_096))
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
            | BridgeLibraryCommand::RefreshEditedPaths(_)
            | BridgeLibraryCommand::RefreshRenamedPaths(_),
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

fn command_requires_queue_snapshot(cmd: &BridgeCommand) -> bool {
    match cmd {
        BridgeCommand::Queue(queue_cmd) => !matches!(
            queue_cmd,
            BridgeQueueCommand::Select(_) | BridgeQueueCommand::PlayAt(_)
        ),
        BridgeCommand::Library(library_cmd) => matches!(
            library_cmd,
            BridgeLibraryCommand::AddTrack(_)
                | BridgeLibraryCommand::PlayTrack(_)
                | BridgeLibraryCommand::ReplaceWithAlbum(_)
                | BridgeLibraryCommand::AppendAlbum(_)
                | BridgeLibraryCommand::ReplaceAlbumByKey { .. }
                | BridgeLibraryCommand::AppendAlbumByKey { .. }
                | BridgeLibraryCommand::ReplaceArtistByKey { .. }
                | BridgeLibraryCommand::AppendArtistByKey { .. }
                | BridgeLibraryCommand::ReplaceRootByPath { .. }
                | BridgeLibraryCommand::AppendRootByPath { .. }
                | BridgeLibraryCommand::ReplaceAllTracks
                | BridgeLibraryCommand::AppendAllTracks
                | BridgeLibraryCommand::RefreshEditedPaths(_)
                | BridgeLibraryCommand::RefreshRenamedPaths(_)
        ),
        _ => false,
    }
}

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

struct BridgeCommandContext<'a> {
    playback: &'a PlaybackEngine,
    analysis: &'a AnalysisEngine,
    metadata: &'a MetadataService,
    library: &'a LibraryService,
    lastfm: &'a LastFmHandle,
    search_query_tx: &'a Sender<SearchWorkerQuery>,
    external_queue_details_tx: &'a Sender<ExternalQueueDetailsRequest>,
    apply_album_art_tx: &'a Sender<ApplyAlbumArtRequest>,
    event_tx: &'a Sender<BridgeEvent>,
    running: &'a mut bool,
    settings_dirty: &'a mut bool,
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
    }
}

fn handle_settings_bridge_command(
    cmd: &BridgeSettingsCommand,
    state: &mut BridgeState,
    context: &mut BridgeCommandContext<'_>,
) {
    match cmd {
        BridgeSettingsCommand::LoadFromDisk => {
            load_settings_into(&mut state.settings);
            state.lastfm.enabled = state.settings.integrations.lastfm_scrobbling_enabled;
            context
                .playback
                .command(PlaybackCommand::SetVolume(state.settings.volume));
            context
                .analysis
                .command(AnalysisCommand::SetFftSize(state.settings.fft_size));
            context
                .analysis
                .command(AnalysisCommand::SetSpectrogramViewMode(
                    state.settings.spectrogram_view_mode,
                ));
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
        BridgeSettingsCommand::SetViewerFullscreenMode(mode) => {
            state.settings.viewer_fullscreen_mode = *mode;
            *context.settings_dirty = true;
        }
        BridgeSettingsCommand::SetDbRange(value) => {
            state.settings.db_range = value.clamp(50.0, 120.0);
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

fn handle_bridge_command(
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
        },
        BridgeCommand::Settings(cmd) => {
            handle_settings_bridge_command(&cmd, state, context);
            true
        }
    }
}

fn handle_queue_command(
    cmd: BridgeQueueCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let outcome = apply_queue_command_state(
        cmd,
        &mut state.queue,
        &mut state.selected_queue_index,
        state.playback.state,
    );
    if outcome.changed {
        let _ = sync_queue_details(state, external_queue_details_tx);
    }
    for op in &outcome.playback_ops {
        match op {
            QueuePlaybackOp::LoadQueue(tracks) => {
                playback.command(PlaybackCommand::LoadQueue(tracks.clone()));
            }
            QueuePlaybackOp::AddToQueue(tracks) => {
                playback.command(PlaybackCommand::AddToQueue(tracks.clone()));
            }
            QueuePlaybackOp::RemoveAt(idx) => playback.command(PlaybackCommand::RemoveAt(*idx)),
            QueuePlaybackOp::Move { from, to } => playback.command(PlaybackCommand::MoveQueue {
                from: *from,
                to: *to,
            }),
            QueuePlaybackOp::ClearQueue => playback.command(PlaybackCommand::ClearQueue),
            QueuePlaybackOp::PlayAt(idx) => playback.command(PlaybackCommand::PlayAt(*idx)),
            QueuePlaybackOp::Play => playback.command(PlaybackCommand::Play),
        }
    }
    if let Some(error) = outcome.error {
        let _ = try_send_event(event_tx, BridgeEvent::Error(error));
    }
    outcome.changed
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum QueuePlaybackOp {
    LoadQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    RemoveAt(usize),
    Move { from: usize, to: usize },
    ClearQueue,
    PlayAt(usize),
    Play,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
struct QueueCommandOutcome {
    changed: bool,
    playback_ops: Vec<QueuePlaybackOp>,
    error: Option<String>,
}

fn replace_queue_command_outcome(
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
    tracks: Vec<PathBuf>,
    autoplay: bool,
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    *queue = tracks;
    *selected_queue_index = if queue.is_empty() { None } else { Some(0) };
    let mut playback_ops = Vec::new();
    if queue.is_empty() {
        playback_ops.push(QueuePlaybackOp::ClearQueue);
    } else {
        playback_ops.push(QueuePlaybackOp::LoadQueue(queue.clone()));
        if autoplay {
            playback_ops.push(QueuePlaybackOp::PlayAt(0));
            if playback_state != PlaybackState::Playing {
                playback_ops.push(QueuePlaybackOp::Play);
            }
        }
    }
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
    }
}

fn append_queue_command_outcome(
    queue: &mut Vec<PathBuf>,
    tracks: Vec<PathBuf>,
) -> QueueCommandOutcome {
    if tracks.is_empty() {
        return QueueCommandOutcome::default();
    }

    let mut playback_ops = Vec::new();
    if queue.is_empty() {
        queue.extend(tracks);
        playback_ops.push(QueuePlaybackOp::LoadQueue(queue.clone()));
    } else {
        queue.extend(tracks.clone());
        playback_ops.push(QueuePlaybackOp::AddToQueue(tracks));
    }
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
    }
}

fn play_at_queue_command_outcome(
    idx: usize,
    queue_len: usize,
    selected_queue_index: &mut Option<usize>,
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    if idx >= queue_len {
        return QueueCommandOutcome {
            changed: false,
            playback_ops: Vec::new(),
            error: Some(format!("queue index {idx} out of bounds")),
        };
    }

    let mut playback_ops = vec![QueuePlaybackOp::PlayAt(idx)];
    if playback_state != PlaybackState::Playing {
        playback_ops.push(QueuePlaybackOp::Play);
    }
    *selected_queue_index = Some(idx);
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
    }
}

fn remove_queue_command_outcome(
    idx: usize,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    if idx >= queue.len() {
        return QueueCommandOutcome::default();
    }

    queue.remove(idx);
    let playback_ops = if queue.is_empty() {
        *selected_queue_index = None;
        vec![QueuePlaybackOp::ClearQueue]
    } else {
        *selected_queue_index = selected_queue_index.and_then(|sel| match sel.cmp(&idx) {
            std::cmp::Ordering::Equal => Some(sel.min(queue.len().saturating_sub(1))),
            std::cmp::Ordering::Greater => Some(sel - 1),
            std::cmp::Ordering::Less => Some(sel),
        });
        vec![QueuePlaybackOp::RemoveAt(idx)]
    };
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
    }
}

fn move_queue_command_outcome(
    from: usize,
    to: usize,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    if from >= queue.len() || to >= queue.len() || from == to {
        return QueueCommandOutcome::default();
    }

    let item = queue.remove(from);
    queue.insert(to, item);
    *selected_queue_index = selected_queue_index.map(|sel| {
        if sel == from {
            to
        } else if from < sel && to >= sel {
            sel - 1
        } else if from > sel && to <= sel {
            sel + 1
        } else {
            sel
        }
    });
    QueueCommandOutcome {
        changed: true,
        playback_ops: vec![QueuePlaybackOp::Move { from, to }],
        error: None,
    }
}

fn apply_queue_command_state(
    cmd: BridgeQueueCommand,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    match cmd {
        BridgeQueueCommand::Replace { tracks, autoplay } => replace_queue_command_outcome(
            queue,
            selected_queue_index,
            tracks,
            autoplay,
            playback_state,
        ),
        BridgeQueueCommand::Append(tracks) => append_queue_command_outcome(queue, tracks),
        BridgeQueueCommand::PlayAt(idx) => {
            play_at_queue_command_outcome(idx, queue.len(), selected_queue_index, playback_state)
        }
        BridgeQueueCommand::Remove(idx) => {
            remove_queue_command_outcome(idx, queue, selected_queue_index)
        }
        BridgeQueueCommand::Move { from, to } => {
            move_queue_command_outcome(from, to, queue, selected_queue_index)
        }
        BridgeQueueCommand::Select(sel) => {
            let normalized = sel.filter(|idx| *idx < queue.len());
            let changed = *selected_queue_index != normalized;
            *selected_queue_index = normalized;
            QueueCommandOutcome {
                changed,
                playback_ops: Vec::new(),
                error: None,
            }
        }
        BridgeQueueCommand::Clear => {
            queue.clear();
            *selected_queue_index = None;
            QueueCommandOutcome {
                changed: true,
                playback_ops: vec![QueuePlaybackOp::ClearQueue],
                error: None,
            }
        }
    }
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

fn normalized_library_track_title(track: &LibraryTrack) -> String {
    if !track.title.trim().is_empty() {
        return track.title.trim().to_string();
    }
    track
        .path
        .file_stem()
        .map_or_else(String::new, |name| name.to_string_lossy().into_owned())
}

fn leading_track_number(input: &str) -> Option<u32> {
    let mut n: u32 = 0;
    let mut saw_digit = false;
    for ch in input.chars() {
        if let Some(d) = ch.to_digit(10) {
            saw_digit = true;
            n = n.saturating_mul(10).saturating_add(d);
        } else {
            break;
        }
    }
    if saw_digit {
        Some(n)
    } else {
        None
    }
}

fn natural_cmp(a: &str, b: &str) -> Ordering {
    let a = a.as_bytes();
    let b = b.as_bytes();
    let mut ia = 0usize;
    let mut ib = 0usize;

    while ia < a.len() && ib < b.len() {
        let ca = a[ia];
        let cb = b[ib];

        if ca.is_ascii_digit() && cb.is_ascii_digit() {
            let start_a = ia;
            let start_b = ib;
            while ia < a.len() && a[ia].is_ascii_digit() {
                ia += 1;
            }
            while ib < b.len() && b[ib].is_ascii_digit() {
                ib += 1;
            }

            let mut na = &a[start_a..ia];
            let mut nb = &b[start_b..ib];
            while na.len() > 1 && na[0] == b'0' {
                na = &na[1..];
            }
            while nb.len() > 1 && nb[0] == b'0' {
                nb = &nb[1..];
            }

            let cmp = na
                .len()
                .cmp(&nb.len())
                .then_with(|| na.cmp(nb))
                .then_with(|| (ia - start_a).cmp(&(ib - start_b)));
            if cmp != Ordering::Equal {
                return cmp;
            }
            continue;
        }

        let la = ca.to_ascii_lowercase();
        let lb = cb.to_ascii_lowercase();
        let cmp = la.cmp(&lb);
        if cmp != Ordering::Equal {
            return cmp;
        }
        ia += 1;
        ib += 1;
    }

    a.len().cmp(&b.len())
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

fn resolved_album_year(tracks: &[&LibraryTrack]) -> Option<i32> {
    resolve_uniform_year(tracks.iter().map(|track| track.year))
}

fn ordered_track_paths_for_queue(tracks: Vec<&LibraryTrack>) -> Vec<PathBuf> {
    struct QueueTrackOrder<'a> {
        track: &'a LibraryTrack,
        title: String,
        path: String,
        rank: u8,
        number: u32,
    }

    let mut ordered = tracks
        .into_iter()
        .map(|track| {
            let file_stem = track
                .path
                .file_stem()
                .map_or_else(String::new, |name| name.to_string_lossy().into_owned());
            let filename_number = leading_track_number(&file_stem);
            let number = track
                .track_no
                .or(filename_number)
                .unwrap_or_else(|| u32::MAX.saturating_sub(1));
            let rank = if track.track_no.is_some() {
                0
            } else if filename_number.is_some() {
                1
            } else {
                2
            };
            QueueTrackOrder {
                track,
                title: normalized_library_track_title(track),
                path: track.path.to_string_lossy().to_string(),
                rank,
                number,
            }
        })
        .collect::<Vec<_>>();

    ordered.sort_by(|a, b| {
        a.rank
            .cmp(&b.rank)
            .then_with(|| a.number.cmp(&b.number))
            .then_with(|| natural_cmp(&a.title, &b.title))
            .then_with(|| natural_cmp(&a.path, &b.path))
    });

    ordered
        .into_iter()
        .map(|item| item.track.path.clone())
        .collect()
}

fn collect_artist_paths_for_queue(
    library: &LibrarySnapshot,
    artist: &str,
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    struct AlbumBucket<'a> {
        key: String,
        title: String,
        year: Option<i32>,
        tracks: Vec<&'a LibraryTrack>,
    }

    let mut loose_tracks = Vec::new();
    let mut album_buckets: HashMap<String, AlbumBucket<'_>> = HashMap::new();
    let artist_selector = artist.trim();
    let artist_selector_is_key = artist_selector.starts_with("artist|");

    for track in &library.tracks {
        let context = derive_tree_path_context(&track.path, &library.roots, &track.artist);
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
            continue;
        }
        let Some(context) = context else {
            loose_tracks.push(track);
            continue;
        };
        let Some(album_key) = context.album_key else {
            loose_tracks.push(track);
            continue;
        };
        let fallback_title = normalized_library_album(track);
        let bucket = album_buckets
            .entry(album_key.clone())
            .or_insert_with(|| AlbumBucket {
                key: album_key.clone(),
                title: fallback_title.to_string(),
                year: None,
                tracks: Vec::new(),
            });
        if bucket.title == "Unknown Album" && fallback_title != "Unknown Album" {
            bucket.title = fallback_title.to_string();
        }
        bucket.tracks.push(track);
    }

    let mut albums = album_buckets.into_values().collect::<Vec<_>>();
    for bucket in &mut albums {
        bucket.year = resolved_album_year(&bucket.tracks);
    }
    albums.sort_by(|a, b| match sort_mode {
        LibrarySortMode::Year => {
            let a_unknown = a.year.is_none();
            let b_unknown = b.year.is_none();
            a_unknown
                .cmp(&b_unknown)
                .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
                .then_with(|| natural_cmp(&a.title, &b.title))
                .then_with(|| natural_cmp(&a.key, &b.key))
        }
        LibrarySortMode::Title => natural_cmp(&a.title, &b.title)
            .then_with(|| {
                let a_unknown = a.year.is_none();
                let b_unknown = b.year.is_none();
                a_unknown
                    .cmp(&b_unknown)
                    .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
            })
            .then_with(|| natural_cmp(&a.key, &b.key)),
    });

    let mut out = ordered_track_paths_for_queue(loose_tracks);
    for bucket in albums {
        out.extend(ordered_track_paths_for_queue(bucket.tracks));
    }
    out
}

fn collect_album_paths_for_queue(
    library: &LibrarySnapshot,
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
                return context
                    .as_ref()
                    .and_then(|ctx| ctx.album_key.as_ref())
                    .is_some_and(|key| key == album_selector);
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
                return context_album == album_selector;
            }
            normalized_library_album(track) == album_selector
        })
        .map(|track| track.path.clone())
        .collect()
}

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
        let mut reader = Cursor::new(String::from_utf8_lossy(&bytes).into_owned());
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

fn sync_queue_details(
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

fn library_track_paths(library: &LibrarySnapshot) -> Vec<PathBuf> {
    library
        .tracks
        .iter()
        .map(|track| track.path.clone())
        .collect()
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
    let paths = collect_root_paths_for_queue(&state.library, root_path);
    queue_paths(state, runtime, paths, mode)
}

fn collect_root_paths_for_queue(library: &LibrarySnapshot, root_path: &str) -> Vec<PathBuf> {
    let root = std::path::Path::new(root_path);
    let mut paths: Vec<PathBuf> = library
        .tracks
        .iter()
        .filter(|track| track.path.starts_with(root))
        .map(|track| track.path.clone())
        .collect();
    paths.sort();
    paths
}

fn queue_all_tracks(
    state: &mut BridgeState,
    runtime: &LibraryCommandRuntime<'_>,
    mode: QueueMode,
) -> bool {
    queue_paths(state, runtime, library_track_paths(&state.library), mode)
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

fn process_apply_album_art_event(
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

fn drain_apply_album_art_events(
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
fn pump_apply_album_art_events(
    apply_album_art_rx: &Receiver<ApplyAlbumArtEvent>,
    metadata: &MetadataService,
    event_tx: &Sender<BridgeEvent>,
    state: &mut BridgeState,
) -> bool {
    drain_apply_album_art_events(apply_album_art_rx, metadata, event_tx, state).is_pending()
}

fn update_library_cover_paths(
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

fn update_queue_cover_paths(
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

#[derive(Debug, Clone)]
struct TreePathContext {
    artist_name: String,
    artist_key: String,
    root_label: String,
    album_folder: Option<String>,
    album_key: Option<String>,
    section_key: Option<String>,
    track_key: String,
    is_main_level_album_track: bool,
    is_disc_section_album_track: bool,
}

#[derive(Default)]
struct HitAlbumAcc {
    artist_name: String,
    album_title: String,
    artist_key: String,
    root_label: String,
    year_counts: HashMap<i32, usize>,
    genre_counts: HashMap<String, usize>,
}

struct SearchResultLimits {
    fallback: usize,
    artist: usize,
    album: usize,
    track: usize,
}

type SearchGroupMap = HashMap<String, (f32, String, String)>;

struct SearchRowBuckets {
    track_rows: Vec<BridgeSearchResultRow>,
    album_cover_paths: HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: HashMap<String, HitAlbumAcc>,
}

struct SearchRowAccumulator {
    roots: Vec<LibraryRoot>,
    roots_by_path: HashMap<PathBuf, PreparedSearchRoot>,
    album_cover_paths: HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: HashMap<String, HitAlbumAcc>,
    track_rows: Vec<BridgeSearchResultRow>,
}

impl SearchRowAccumulator {
    fn new(roots: Vec<LibraryRoot>) -> Self {
        Self {
            roots_by_path: roots_by_path_for_search(&roots),
            roots,
            album_cover_paths: HashMap::new(),
            artist_groups: HashMap::new(),
            album_groups: HashMap::new(),
            album_hit_stats: HashMap::new(),
            track_rows: Vec::new(),
        }
    }

    fn push_hit(&mut self, hit: &LibrarySearchTrack, query_terms: &[String]) {
        let Some(context) = derive_hit_context(hit, &self.roots, &self.roots_by_path) else {
            return;
        };
        let hit_path_string = hit.path.to_string_lossy().to_string();
        let hit_artist = if hit.artist.trim().is_empty() {
            context.artist_name.clone()
        } else {
            hit.artist.trim().to_string()
        };
        let hit_album = if hit.album.trim().is_empty() {
            context
                .album_folder
                .clone()
                .unwrap_or_else(|| String::from("Unknown Album"))
        } else {
            hit.album.trim().to_string()
        };
        let album_key = context.album_key.clone();
        if query_terms_match_text(query_terms, &context.artist_name) {
            let artist_entry = self
                .artist_groups
                .entry(context.artist_key.clone())
                .or_insert((
                    hit.score,
                    context.artist_name.clone(),
                    context.root_label.clone(),
                ));
            if hit.score < artist_entry.0 {
                artist_entry.0 = hit.score;
                artist_entry.1.clone_from(&context.artist_name);
                artist_entry.2.clone_from(&context.root_label);
            }
        }
        if let Some(album_key_value) = album_key.clone() {
            let album_query = format!("{} {}", context.artist_name, hit_album);
            if query_terms_match_text(query_terms, &album_query) {
                let album_entry = self.album_groups.entry(album_key_value.clone()).or_insert((
                    hit.score,
                    hit_album.clone(),
                    context.root_label.clone(),
                ));
                if hit.score < album_entry.0 {
                    album_entry.0 = hit.score;
                    album_entry.1.clone_from(&hit_album);
                    album_entry.2.clone_from(&context.root_label);
                }
                update_album_hit_stats(
                    &mut self.album_hit_stats,
                    album_key_value,
                    &context,
                    &hit_album,
                    hit.year,
                    hit.genre.trim(),
                );
            }
        }
        let row_cover_path = if let Some(album_key_value) = album_key.clone() {
            if !hit.cover_path.is_empty() {
                self.album_cover_paths
                    .entry(album_key_value.clone())
                    .or_insert_with(|| hit.cover_path.clone());
            }
            self.album_cover_paths
                .get(&album_key_value)
                .cloned()
                .unwrap_or_else(|| hit.cover_path.clone())
        } else {
            hit.cover_path.clone()
        };
        self.track_rows.push(build_track_search_result_row(
            hit,
            &context,
            &hit_artist,
            &hit_album,
            album_key,
            hit_path_string,
            row_cover_path,
        ));
    }

    fn finish(self) -> SearchRowBuckets {
        SearchRowBuckets {
            track_rows: self.track_rows,
            album_cover_paths: self.album_cover_paths,
            artist_groups: self.artist_groups,
            album_groups: self.album_groups,
            album_hit_stats: self.album_hit_stats,
        }
    }
}

fn update_album_hit_stats(
    album_hit_stats: &mut HashMap<String, HitAlbumAcc>,
    album_key: String,
    context: &TreePathContext,
    hit_album: &str,
    year: Option<i32>,
    genre: &str,
) {
    let stats_entry = album_hit_stats.entry(album_key).or_default();
    if stats_entry.artist_name.is_empty() {
        stats_entry.artist_name.clone_from(&context.artist_name);
    }
    if stats_entry.artist_key.is_empty() {
        stats_entry.artist_key.clone_from(&context.artist_key);
    }
    if stats_entry.root_label.is_empty() {
        stats_entry.root_label.clone_from(&context.root_label);
    }
    if stats_entry.album_title.is_empty() {
        stats_entry.album_title.clone_from(&hit_album.to_string());
    }
    if let Some(year) = year {
        *stats_entry.year_counts.entry(year).or_insert(0) += 1;
    }
    if !genre.is_empty() {
        *stats_entry
            .genre_counts
            .entry(genre.to_string())
            .or_insert(0) += 1;
    }
}

fn build_track_search_result_row(
    hit: &LibrarySearchTrack,
    context: &TreePathContext,
    hit_artist: &str,
    hit_album: &str,
    album_key: Option<String>,
    hit_path_string: String,
    cover_path: String,
) -> BridgeSearchResultRow {
    BridgeSearchResultRow {
        row_type: BridgeSearchResultRowType::Track,
        score: hit.score,
        year: hit.year,
        track_number: hit.track_no,
        count: 0,
        length_seconds: hit.duration_secs,
        label: if hit.title.trim().is_empty() {
            hit.path
                .file_name()
                .map_or_else(String::new, |name| name.to_string_lossy().to_string())
        } else {
            hit.title.trim().to_string()
        },
        artist: hit_artist.to_string(),
        album: hit_album.to_string(),
        root_label: context.root_label.clone(),
        genre: hit.genre.trim().to_string(),
        cover_path,
        artist_key: context.artist_key.clone(),
        album_key: album_key.unwrap_or_default(),
        section_key: context.section_key.clone().unwrap_or_default(),
        track_key: context.track_key.clone(),
        track_path: hit_path_string,
    }
}

fn empty_search_results_frame(seq: u32) -> SearchBuildOutcome {
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
        seq,
        rows: Vec::new(),
    })
}

fn search_result_limits(query_text: &str) -> SearchResultLimits {
    let is_short_query = query_text.chars().count() <= search_short_query_char_threshold();
    SearchResultLimits {
        fallback: if is_short_query {
            search_fallback_limit_short()
        } else {
            search_fallback_limit()
        },
        artist: if is_short_query {
            search_artist_row_limit_short()
        } else {
            search_artist_row_limit()
        },
        album: if is_short_query {
            search_album_row_limit_short()
        } else {
            search_album_row_limit()
        },
        track: if is_short_query {
            search_track_row_limit_short()
        } else {
            search_track_row_limit()
        },
    }
}

fn search_fts_enabled() -> bool {
    std::env::var_os("FERROUS_SEARCH_DISABLE_FTS").is_none()
}

fn populate_search_rows(
    roots: &[LibraryRoot],
    hits: &[LibrarySearchTrack],
    query_terms: &[String],
) -> SearchRowBuckets {
    let mut rows = SearchRowAccumulator::new(roots.to_vec());
    for hit in hits {
        rows.push_hit(hit, query_terms);
    }
    rows.finish()
}

fn finalize_search_rows(
    album_inventory: &HashMap<String, AlbumInventoryAcc>,
    limits: &SearchResultLimits,
    album_cover_paths: &HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: &HashMap<String, HitAlbumAcc>,
    mut track_rows: Vec<BridgeSearchResultRow>,
) -> Vec<BridgeSearchResultRow> {
    let mut artist_rows = artist_groups
        .into_iter()
        .map(
            |(artist_key, (score, artist_name, root_label))| BridgeSearchResultRow {
                row_type: BridgeSearchResultRowType::Artist,
                score,
                year: None,
                track_number: None,
                count: 0,
                length_seconds: None,
                label: artist_name.clone(),
                artist: artist_name,
                album: String::new(),
                root_label,
                genre: String::new(),
                cover_path: String::new(),
                artist_key,
                album_key: String::new(),
                section_key: String::new(),
                track_key: String::new(),
                track_path: String::new(),
            },
        )
        .collect::<Vec<_>>();

    let mut album_rows = album_groups
        .into_iter()
        .filter_map(|(album_key, (score, fallback_title, root_label))| {
            let stats = album_hit_stats.get(&album_key)?;
            let inventory = album_inventory.get(&album_key);
            Some(BridgeSearchResultRow {
                row_type: BridgeSearchResultRowType::Album,
                score,
                year: choose_most_common_year(&stats.year_counts),
                track_number: None,
                count: inventory.map_or(0, |value| value.main_track_count),
                length_seconds: inventory
                    .and_then(|value| value.has_main_duration.then_some(value.main_total_length)),
                label: if stats.album_title.is_empty() {
                    fallback_title
                } else {
                    stats.album_title.clone()
                },
                artist: stats.artist_name.clone(),
                album: if stats.album_title.is_empty() {
                    String::new()
                } else {
                    stats.album_title.clone()
                },
                root_label: if stats.root_label.is_empty() {
                    root_label
                } else {
                    stats.root_label.clone()
                },
                genre: choose_most_common_genre(&stats.genre_counts),
                cover_path: album_cover_paths
                    .get(&album_key)
                    .cloned()
                    .unwrap_or_default(),
                artist_key: stats.artist_key.clone(),
                album_key,
                section_key: String::new(),
                track_key: String::new(),
                track_path: String::new(),
            })
        })
        .collect::<Vec<_>>();

    artist_rows.sort_by(search_row_cmp);
    album_rows.sort_by(search_row_cmp);
    track_rows.sort_by(search_row_cmp);
    artist_rows.truncate(limits.artist);
    album_rows.truncate(limits.album);
    track_rows.truncate(limits.track);

    let mut rows = Vec::with_capacity(artist_rows.len() + album_rows.len() + track_rows.len());
    rows.extend(artist_rows);
    rows.extend(album_rows);
    rows.extend(track_rows);
    rows
}

fn build_search_results_frame(
    query: &SearchWorkerQuery,
    prepared_cache: &mut SearchWorkerPreparedCache,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> SearchBuildOutcome {
    let seq = query.seq;
    let query_text = query.query.trim();
    if query_text.is_empty() {
        return empty_search_results_frame(seq);
    }
    let query_terms = split_search_terms(query_text);
    if query_terms.is_empty() {
        return empty_search_results_frame(seq);
    }
    let limits = search_result_limits(query_text);
    let library = query.library.as_ref();
    if library.roots.is_empty() {
        return empty_search_results_frame(seq);
    }
    if search_fts_enabled() {
        if let Ok(hits) = search_tracks_fts(query_text, limits.fallback) {
            if !hits.is_empty() {
                let rows = build_search_rows_from_hits(library, &hits, &query_terms, &limits);
                return SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, rows });
            }
        }
    }

    let prepared = prepared_cache.prepared_for(&query.library);
    let hits = match search_tracks_fallback_prepared(
        query_text,
        prepared.as_ref(),
        limits.fallback,
        query_rx,
    ) {
        SearchFallbackOutcome::Hits(rows) => rows,
        SearchFallbackOutcome::Cancelled(next) => return SearchBuildOutcome::Cancelled(next),
    };
    if hits.is_empty() {
        return empty_search_results_frame(seq);
    }
    let buckets = populate_search_rows(&library.roots, &hits, &query_terms);
    let rows = finalize_search_rows(
        &prepared.album_inventory,
        &limits,
        &buckets.album_cover_paths,
        buckets.artist_groups,
        buckets.album_groups,
        &buckets.album_hit_stats,
        buckets.track_rows,
    );
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, rows })
}

fn process_search_results(frame: BridgeSearchResultsFrame, state: &mut BridgeState) {
    state.pending_search_results = Some(frame);
}

fn drain_search_results(search_rx: &Receiver<BridgeSearchResultsFrame>, state: &mut BridgeState) {
    let mut latest = None;
    while let Ok(frame) = search_rx.try_recv() {
        latest = Some(frame);
    }

    if let Some(frame) = latest {
        process_search_results(frame, state);
    }
}

fn poll_latest_search_query(query_rx: &Receiver<SearchWorkerQuery>) -> Option<SearchWorkerQuery> {
    let mut latest = None;
    while let Ok(next) = query_rx.try_recv() {
        latest = Some(next);
    }
    latest
}

#[derive(Clone)]
struct PreparedSearchRoot {
    path: PathBuf,
    root_key: String,
    root_label: String,
}

fn roots_by_path_for_search(roots: &[LibraryRoot]) -> HashMap<PathBuf, PreparedSearchRoot> {
    roots
        .iter()
        .map(|root| {
            (
                root.path.clone(),
                PreparedSearchRoot {
                    path: root.path.clone(),
                    root_key: root.path.to_string_lossy().to_string(),
                    root_label: root.search_label(),
                },
            )
        })
        .collect::<HashMap<_, _>>()
}

fn derive_hit_context(
    hit: &LibrarySearchTrack,
    roots: &[LibraryRoot],
    roots_by_path: &HashMap<PathBuf, PreparedSearchRoot>,
) -> Option<TreePathContext> {
    roots_by_path
        .get(&hit.root_path)
        .and_then(|root| derive_tree_path_context_for_root(&hit.path, root, &hit.artist))
        .or_else(|| derive_tree_path_context(&hit.path, roots, &hit.artist))
}

fn accumulate_album_inventory_for_hits(
    library: &LibrarySnapshot,
    roots_by_path: &HashMap<PathBuf, PreparedSearchRoot>,
    album_keys: &HashSet<String>,
) -> HashMap<String, AlbumInventoryAcc> {
    if album_keys.is_empty() {
        return HashMap::new();
    }

    let mut album_inventory: HashMap<String, AlbumInventoryAcc> =
        HashMap::with_capacity(album_keys.len());
    for track in &library.tracks {
        let artist = track.artist.trim().to_string();
        let Some(context) = roots_by_path
            .get(&track.root_path)
            .and_then(|root| derive_tree_path_context_for_root(&track.path, root, &artist))
        else {
            continue;
        };
        let Some(album_key) = context.album_key else {
            continue;
        };
        if !album_keys.contains(&album_key) {
            continue;
        }
        let include_in_main_album =
            context.is_main_level_album_track || context.is_disc_section_album_track;
        if !include_in_main_album {
            continue;
        }

        let inventory = album_inventory.entry(album_key).or_default();
        inventory.main_track_count = inventory.main_track_count.saturating_add(1);
        if let Some(duration) = track.duration_secs {
            if duration.is_finite() && duration >= 0.0 {
                inventory.main_total_length += duration;
                inventory.has_main_duration = true;
            }
        }
    }

    album_inventory
}

fn build_search_rows_from_hits(
    library: &LibrarySnapshot,
    hits: &[LibrarySearchTrack],
    query_terms: &[String],
    limits: &SearchResultLimits,
) -> Vec<BridgeSearchResultRow> {
    let buckets = populate_search_rows(&library.roots, hits, query_terms);
    let album_keys = buckets.album_groups.keys().cloned().collect::<HashSet<_>>();
    let album_inventory = accumulate_album_inventory_for_hits(
        library,
        &roots_by_path_for_search(&library.roots),
        &album_keys,
    );
    finalize_search_rows(
        &album_inventory,
        limits,
        &buckets.album_cover_paths,
        buckets.artist_groups,
        buckets.album_groups,
        &buckets.album_hit_stats,
        buckets.track_rows,
    )
}

fn prepare_search_library(library: &LibrarySnapshot) -> PreparedSearchLibrary {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return PreparedSearchLibrary::default();
    }
    let roots_by_path = roots_by_path_for_search(&roots);

    let mut tracks = Vec::with_capacity(library.tracks.len());
    let mut album_inventory: HashMap<String, AlbumInventoryAcc> = HashMap::new();

    for track in &library.tracks {
        let path_string = track.path.to_string_lossy().to_string();
        let path_lower = path_string.to_lowercase();
        let title = track.title.trim().to_string();
        let artist = track.artist.trim().to_string();
        let album = track.album.trim().to_string();
        let genre = track.genre.trim().to_string();
        let title_l = title.to_lowercase();
        let artist_l = artist.to_lowercase();
        let album_l = album.to_lowercase();
        let genre_l = genre.to_lowercase();
        let haystack_l = format!("{title_l} {artist_l} {album_l} {genre_l} {path_lower}");

        if let Some(context) = roots_by_path
            .get(&track.root_path)
            .and_then(|root| derive_tree_path_context_for_root(&track.path, root, &artist))
        {
            if let Some(album_key) = context.album_key.clone() {
                let include_in_main_album =
                    context.is_main_level_album_track || context.is_disc_section_album_track;
                let inventory = album_inventory.entry(album_key).or_default();
                if include_in_main_album {
                    inventory.main_track_count = inventory.main_track_count.saturating_add(1);
                    if let Some(duration) = track.duration_secs {
                        if duration.is_finite() && duration >= 0.0 {
                            inventory.main_total_length += duration;
                            inventory.has_main_duration = true;
                        }
                    }
                }
            }
        }

        tracks.push(PreparedSearchTrack {
            path: track.path.clone(),
            root_path: track.root_path.clone(),
            path_string,
            path_lower,
            title,
            artist,
            album,
            cover_path: track.cover_path.clone(),
            genre,
            year: track.year,
            track_no: track.track_no,
            duration_secs: track.duration_secs,
            title_l,
            artist_l,
            album_l,
            genre_l,
            haystack_l,
        });
    }

    PreparedSearchLibrary {
        tracks,
        album_inventory,
    }
}

fn compare_fallback_rank(
    a_score: f32,
    a_path_lower: &str,
    b_score: f32,
    b_path_lower: &str,
) -> Ordering {
    a_score
        .partial_cmp(&b_score)
        .unwrap_or(Ordering::Equal)
        .then_with(|| a_path_lower.cmp(b_path_lower))
}

#[derive(Clone)]
struct FallbackRankedHit {
    score: f32,
    path_lower: String,
    track_index: usize,
}

impl PartialEq for FallbackRankedHit {
    fn eq(&self, other: &Self) -> bool {
        compare_fallback_rank(self.score, &self.path_lower, other.score, &other.path_lower)
            == Ordering::Equal
    }
}

impl Eq for FallbackRankedHit {}

impl PartialOrd for FallbackRankedHit {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for FallbackRankedHit {
    fn cmp(&self, other: &Self) -> Ordering {
        compare_fallback_rank(self.score, &self.path_lower, other.score, &other.path_lower)
    }
}

fn search_tracks_fallback_prepared(
    query: &str,
    prepared: &PreparedSearchLibrary,
    limit: usize,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> SearchFallbackOutcome {
    let terms = split_search_terms(query);
    if terms.is_empty() {
        return SearchFallbackOutcome::Hits(Vec::new());
    }

    let capped_limit = limit.clamp(1, 5_000);
    let mut heap =
        std::collections::BinaryHeap::<FallbackRankedHit>::with_capacity(capped_limit + 1);
    let cancel_poll_rows = search_cancel_poll_rows();
    for (index, track) in prepared.tracks.iter().enumerate() {
        if index % cancel_poll_rows == 0 {
            if let Some(next) = poll_latest_search_query(query_rx) {
                return SearchFallbackOutcome::Cancelled(next);
            }
        }
        if !terms.iter().all(|term| track.haystack_l.contains(term)) {
            continue;
        }

        let mut score = 0.0f32;
        for term in &terms {
            score += if track.title_l.starts_with(term) {
                0.0
            } else if track.title_l.contains(term) {
                0.8
            } else if track.artist_l.starts_with(term) {
                1.2
            } else if track.artist_l.contains(term) {
                1.8
            } else if track.album_l.starts_with(term) {
                2.0
            } else if track.album_l.contains(term) {
                2.6
            } else if track.genre_l.contains(term) {
                3.2
            } else {
                4.0
            };
        }
        score += f32::from(
            u16::try_from(track.path_string.len().min(usize::from(u16::MAX))).unwrap_or(u16::MAX),
        ) / 10_000.0;

        if heap.len() >= capped_limit {
            if let Some(worst) = heap.peek() {
                let is_better =
                    compare_fallback_rank(score, &track.path_lower, worst.score, &worst.path_lower)
                        == Ordering::Less;
                if !is_better {
                    continue;
                }
            }
            let _ = heap.pop();
        }
        heap.push(FallbackRankedHit {
            score,
            path_lower: track.path_lower.clone(),
            track_index: index,
        });
    }

    if let Some(next) = poll_latest_search_query(query_rx) {
        return SearchFallbackOutcome::Cancelled(next);
    }

    let mut ranked = heap.into_vec();
    ranked.sort_by(|a, b| compare_fallback_rank(a.score, &a.path_lower, b.score, &b.path_lower));

    let mut out = Vec::with_capacity(ranked.len());
    for rank in ranked {
        let track = &prepared.tracks[rank.track_index];
        out.push(LibrarySearchTrack {
            path: track.path.clone(),
            root_path: track.root_path.clone(),
            title: track.title.clone(),
            artist: track.artist.clone(),
            album: track.album.clone(),
            cover_path: track.cover_path.clone(),
            genre: track.genre.clone(),
            year: track.year,
            track_no: track.track_no,
            duration_secs: track.duration_secs,
            score: rank.score,
        });
    }
    SearchFallbackOutcome::Hits(out)
}

fn search_row_cmp(a: &BridgeSearchResultRow, b: &BridgeSearchResultRow) -> Ordering {
    a.score
        .partial_cmp(&b.score)
        .unwrap_or(Ordering::Equal)
        .then_with(|| a.label.to_lowercase().cmp(&b.label.to_lowercase()))
        .then_with(|| a.artist.to_lowercase().cmp(&b.artist.to_lowercase()))
        .then_with(|| {
            a.track_path
                .to_lowercase()
                .cmp(&b.track_path.to_lowercase())
        })
}

fn split_search_terms(query: &str) -> Vec<String> {
    query
        .split_whitespace()
        .map(|term| term.trim().to_lowercase())
        .filter(|term| !term.is_empty())
        .collect::<Vec<_>>()
}

fn query_terms_match_text(terms: &[String], text: &str) -> bool {
    if terms.is_empty() {
        return false;
    }
    let text_l = text.to_lowercase();
    terms.iter().all(|term| text_l.contains(term))
}

fn choose_most_common_year(counts: &HashMap<i32, usize>) -> Option<i32> {
    let mut best: Option<(i32, usize)> = None;
    for (&year, &count) in counts {
        best = match best {
            Some((best_year, best_count))
                if count > best_count || (count == best_count && year < best_year) =>
            {
                Some((year, count))
            }
            None => Some((year, count)),
            other => other,
        };
    }
    best.map(|(year, _)| year)
}

fn choose_most_common_genre(counts: &HashMap<String, usize>) -> String {
    let mut best: Option<(&str, usize)> = None;
    for (genre, &count) in counts {
        let key = genre.as_str();
        best = match best {
            Some((best_genre, best_count))
                if count > best_count || (count == best_count && key < best_genre) =>
            {
                Some((key, count))
            }
            None => Some((key, count)),
            other => other,
        };
    }
    best.map_or_else(String::new, |(genre, _)| genre.to_string())
}

pub(crate) fn is_main_album_disc_section(section_name: &str) -> bool {
    let section = section_name.trim().to_ascii_lowercase();
    if section.is_empty() {
        return false;
    }
    for prefix in ["cd", "disc", "disk", "dvd"] {
        let Some(rest) = section.strip_prefix(prefix) else {
            continue;
        };
        let mut saw_digit = false;
        let mut valid = true;
        for ch in rest.chars() {
            if ch.is_ascii_digit() {
                saw_digit = true;
                continue;
            }
            if !saw_digit && matches!(ch, ' ' | '-' | '_' | '.') {
                continue;
            }
            if saw_digit && matches!(ch, ' ' | '-' | '_' | '.' | '(' | ')' | '[' | ']') {
                continue;
            }
            if saw_digit && ch.is_ascii_alphabetic() {
                continue;
            }
            valid = false;
            break;
        }
        if valid && saw_digit {
            return true;
        }
    }
    false
}

fn pick_root_for_path<'a>(roots: &'a [LibraryRoot], path: &Path) -> Option<&'a LibraryRoot> {
    roots
        .iter()
        .filter(|root| path.starts_with(&root.path))
        .max_by_key(|root| root.path.components().count())
}

fn derive_tree_path_context_for_root(
    path: &Path,
    root: &PreparedSearchRoot,
    fallback_artist: &str,
) -> Option<TreePathContext> {
    let rel = path.strip_prefix(&root.path).ok()?;
    let components = rel
        .components()
        .filter_map(|component| {
            let std::path::Component::Normal(name) = component else {
                return None;
            };
            Some(name.to_string_lossy().to_string())
        })
        .collect::<Vec<_>>();
    if components.is_empty() {
        return None;
    }

    let artist_name = if components.len() >= 2 {
        components[0].clone()
    } else if fallback_artist.trim().is_empty() {
        String::from("Unknown Artist")
    } else {
        fallback_artist.trim().to_string()
    };
    let artist_key = format!("artist|{}|{artist_name}", root.root_key);
    let track_path = path.to_string_lossy().to_string();
    let track_key = format!("track|{track_path}");

    if components.len() <= 2 {
        return Some(TreePathContext {
            artist_name,
            artist_key,
            root_label: root.root_label.clone(),
            album_folder: None,
            album_key: None,
            section_key: None,
            track_key,
            is_main_level_album_track: false,
            is_disc_section_album_track: false,
        });
    }

    let album_folder = components[1].clone();
    let album_key = format!("album|{}|{artist_name}|{album_folder}", root.root_key);
    let section_key = if components.len() >= 4 {
        Some(format!(
            "section|{}|{artist_name}|{album_folder}|{}",
            root.root_key, components[2]
        ))
    } else {
        None
    };
    let is_main_level_album_track = components.len() == 3;
    let is_disc_section_album_track =
        components.len() == 4 && is_main_album_disc_section(&components[2]);
    Some(TreePathContext {
        artist_name: artist_name.clone(),
        artist_key,
        root_label: root.root_label.clone(),
        album_folder: Some(album_folder.clone()),
        album_key: Some(album_key),
        section_key,
        track_key,
        is_main_level_album_track,
        is_disc_section_album_track,
    })
}

fn derive_tree_path_context(
    path: &Path,
    roots: &[LibraryRoot],
    fallback_artist: &str,
) -> Option<TreePathContext> {
    let root = pick_root_for_path(roots, path)?;
    let prepared = PreparedSearchRoot {
        path: root.path.clone(),
        root_key: root.path.to_string_lossy().to_string(),
        root_label: root.search_label(),
    };
    derive_tree_path_context_for_root(path, &prepared, fallback_artist)
}

fn playback_snapshot_urgency(
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

fn process_playback_event(
    event: PlaybackEvent,
    analysis: &AnalysisEngine,
    metadata: &MetadataService,
    state: &mut BridgeState,
) -> SnapshotUrgency {
    match event {
        PlaybackEvent::Snapshot(snapshot) => {
            let next_state = snapshot.state;
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
            if let Some(pending) = state.pending_waveform_track.take() {
                if state.playback.current.as_ref() == Some(&pending.path) {
                    analysis.command(AnalysisCommand::SetTrack {
                        path: pending.path,
                        reset_spectrogram: pending.reset_spectrogram,
                        track_token: pending.track_token,
                        gapless: false,
                    });
                }
            }
            urgency
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
            metadata.request(path.clone());
            let is_gapless = matches!(kind, TrackChangeKind::Gapless);
            let reset_spectrogram = matches!(kind, TrackChangeKind::Manual);
            if state.playback.state == PlaybackState::Stopped {
                state.pending_waveform_track = Some(PendingWaveformTrack {
                    path,
                    reset_spectrogram,
                    track_token,
                });
            } else {
                state.pending_waveform_track = None;
                analysis.command(AnalysisCommand::SetTrack {
                    path,
                    reset_spectrogram,
                    track_token,
                    gapless: is_gapless,
                });
            }
            SnapshotUrgency::Immediate
        }
        PlaybackEvent::Seeked => {
            let pos_seconds = state.playback.position.as_secs_f64();
            analysis.command(AnalysisCommand::Seek(pos_seconds));
            SnapshotUrgency::Heartbeat
        }
    }
}

fn drain_playback_events(
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
fn pump_playback_events(
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
        let event_changed = !matches!(event, PlaybackEvent::Seeked);
        let _ = process_playback_event(event, analysis, metadata, state);
        changed |= event_changed;
    }
    changed
}

fn process_analysis_event(snapshot: AnalysisSnapshot, state: &mut BridgeState) -> SnapshotUrgency {
    if snapshot.spectrogram_seq == 0 && snapshot.spectrogram_channels.is_empty() {
        state.analysis.spectrogram_channels.clear();
    } else if !snapshot.spectrogram_channels.is_empty() {
        state.analysis.spectrogram_channels = snapshot.spectrogram_channels;
    }
    state.analysis.spectrogram_seq = snapshot.spectrogram_seq;
    state.analysis.sample_rate_hz = snapshot.sample_rate_hz;
    state.analysis.spectrogram_view_mode = snapshot.spectrogram_view_mode;
    state.analysis.waveform_coverage_seconds = snapshot.waveform_coverage_seconds;
    state.analysis.waveform_complete = snapshot.waveform_complete;
    if !snapshot.waveform_peaks.is_empty() {
        state.analysis.waveform_peaks = snapshot.waveform_peaks;
    }
    SnapshotUrgency::Analysis
}

fn drain_analysis_events(
    analysis_rx: &Receiver<AnalysisEvent>,
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
            AnalysisEvent::PrecomputedSpectrogramChunk(_) => {
                // Pre-computed chunks are handled via handle_analysis_event
                // in the main loop, not in the drain path.
            }
        }
    }
    urgency
}

#[cfg(test)]
#[allow(dead_code)]
fn pump_analysis_events(analysis_rx: &Receiver<AnalysisEvent>, state: &mut BridgeState) -> bool {
    drain_analysis_events(analysis_rx, state).is_pending()
}

fn process_metadata_event(event: MetadataEvent, state: &mut BridgeState) -> SnapshotUrgency {
    match event {
        MetadataEvent::Loaded(metadata) => {
            state.metadata = metadata;
            SnapshotUrgency::Immediate
        }
    }
}

fn drain_metadata_events(
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
fn pump_metadata_events(metadata_rx: &Receiver<MetadataEvent>, state: &mut BridgeState) -> bool {
    drain_metadata_events(metadata_rx, state).is_pending()
}

fn process_lastfm_event(
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
                urgency = SnapshotUrgency::Immediate;
            }
            urgency
        }
    }
}

fn drain_lastfm_events(
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
fn pump_lastfm_events(
    lastfm_rx: &Receiver<LastFmEvent>,
    state: &mut BridgeState,
    settings_dirty: &mut bool,
) -> bool {
    drain_lastfm_events(lastfm_rx, state, settings_dirty).is_pending()
}

fn tick_lastfm_playback(
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

fn tick_lastfm_playback_at(
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

fn current_track_number(state: &BridgeState) -> Option<u32> {
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

fn run_external_queue_detail_worker(
    req_rx: &Receiver<ExternalQueueDetailsRequest>,
    event_tx: &Sender<ExternalQueueDetailsEvent>,
) {
    while let Ok(request) = req_rx.recv() {
        let indexed =
            load_external_track_cache(&request.path, request.fingerprint).unwrap_or_else(|| {
                let indexed = read_track_info(&request.path);
                let _ = store_external_track_cache(&request.path, request.fingerprint, &indexed);
                indexed
            });
        let _ = event_tx.send(ExternalQueueDetailsEvent {
            path: request.path,
            fingerprint: request.fingerprint,
            indexed,
        });
    }
}

fn run_apply_album_art_worker(
    req_rx: &Receiver<ApplyAlbumArtRequest>,
    event_tx: &Sender<ApplyAlbumArtEvent>,
) {
    while let Ok(request) = req_rx.recv() {
        let event = match apply_artwork_to_track(&request.track_path, &request.artwork_path) {
            Ok(outcome) => {
                let mut affected_paths = outcome.affected_track_paths;
                affected_paths.sort();
                affected_paths.dedup();

                let (refresh_error, indexed_by_path) = if let Some(cover_path) =
                    outcome.cover_path_override
                {
                    let refresh_error =
                        refresh_cover_paths_for_tracks_with_override(&affected_paths, &cover_path)
                            .err();
                    let cover_path_string = cover_path.to_string_lossy().to_string();
                    let indexed_by_path = affected_paths
                        .into_iter()
                        .map(|path| {
                            let indexed = IndexedTrack {
                                title: String::new(),
                                artist: String::new(),
                                album: String::new(),
                                cover_path: cover_path_string.clone(),
                                genre: String::new(),
                                year: None,
                                track_no: None,
                                duration_secs: None,
                            };
                            (path, indexed)
                        })
                        .collect::<HashMap<_, _>>();
                    (refresh_error, indexed_by_path)
                } else {
                    let refresh_error = refresh_cover_paths_for_tracks(&affected_paths).err();
                    let indexed_by_path = affected_paths
                        .into_iter()
                        .map(|path| {
                            let indexed = read_track_info(&path);
                            (path, indexed)
                        })
                        .collect::<HashMap<_, _>>();
                    (refresh_error, indexed_by_path)
                };

                ApplyAlbumArtEvent {
                    track_path: request.track_path,
                    indexed_by_path,
                    error: refresh_error
                        .map(|error| format!("failed to refresh cover paths: {error}")),
                }
            }
            Err(error) => ApplyAlbumArtEvent {
                track_path: request.track_path,
                indexed_by_path: HashMap::new(),
                error: Some(format!("failed to apply album art: {error}")),
            },
        };
        let _ = event_tx.send(event);
    }
}

fn unix_timestamp_now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| i64::try_from(duration.as_secs()).unwrap_or(i64::MAX))
        .unwrap_or(0)
}

fn process_library_event(
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
                let _ = sync_queue_details(state, external_queue_details_tx);
            }
            SnapshotUrgency::Immediate
        }
    }
}

fn drain_library_events(
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
fn pump_library_events(
    library_rx: &Receiver<LibraryEvent>,
    external_queue_details_tx: &Sender<ExternalQueueDetailsRequest>,
    state: &mut BridgeState,
) -> bool {
    drain_library_events(library_rx, external_queue_details_tx, state).is_pending()
}

fn process_external_queue_detail_event(
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

fn drain_external_queue_detail_events(
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
fn pump_external_queue_detail_events(
    queue_detail_rx: &Receiver<ExternalQueueDetailsEvent>,
    state: &mut BridgeState,
) -> bool {
    drain_external_queue_detail_events(queue_detail_rx, state).is_pending()
}

fn config_base_path() -> Option<PathBuf> {
    #[cfg(test)]
    {
        static TEST_CONFIG_BASE: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();
        let path = TEST_CONFIG_BASE.get_or_init(|| {
            let mut base = std::env::temp_dir();
            base.push(format!("ferrous-test-config-{}", std::process::id()));
            let _ = fs::create_dir_all(&base);
            base
        });
        return Some(path.clone());
    }

    #[cfg(not(test))]
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME")
                .map(PathBuf::from)
                .map(|h| h.join(".config"))
        })
        .map(|base| base.join("ferrous"))
}

fn settings_path() -> Option<PathBuf> {
    config_base_path().map(|base| base.join("settings.txt"))
}

fn session_path() -> Option<PathBuf> {
    config_base_path().map(|base| base.join("session.json"))
}

fn session_snapshot_for_state(state: &BridgeState) -> SessionSnapshot {
    let current_path = state
        .playback
        .current
        .clone()
        .filter(|path| state.queue.iter().any(|queued| queued == path));
    let current_queue_index = resolve_session_current_index(
        &state.queue,
        state.playback.current_queue_index,
        current_path.as_ref(),
    );
    SessionSnapshot {
        queue: state.queue.clone(),
        selected_queue_index: state.selected_queue_index,
        current_queue_index,
        current_path,
    }
}

fn resolve_session_current_index(
    queue: &[PathBuf],
    current_queue_index: Option<usize>,
    current_path: Option<&PathBuf>,
) -> Option<usize> {
    if let Some(idx) = current_queue_index.filter(|idx| *idx < queue.len()) {
        return Some(idx);
    }
    current_path.and_then(|path| queue.iter().position(|queued| queued == path))
}

fn apply_session_restore(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    session: Option<&SessionSnapshot>,
) {
    let Some(session) = session else {
        return;
    };
    state.queue.clone_from(&session.queue);
    let restored_current_index = resolve_session_current_index(
        &state.queue,
        session.current_queue_index,
        session.current_path.as_ref(),
    );
    state.selected_queue_index = session
        .selected_queue_index
        .filter(|idx| *idx < state.queue.len())
        .or(restored_current_index);
    if state.queue.is_empty() {
        return;
    }
    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    if let Some(idx) = restored_current_index {
        state.playback.current = state.queue.get(idx).cloned();
        state.playback.current_queue_index = Some(idx);
        playback.command(PlaybackCommand::PlayAt(idx));
    } else {
        state.playback.current_queue_index = None;
    }
}

fn load_session_snapshot() -> Option<SessionSnapshot> {
    let path = session_path()?;
    let text = fs::read_to_string(path).ok()?;
    parse_session_text(&text)
}

fn parse_session_text(text: &str) -> Option<SessionSnapshot> {
    let value = serde_json::from_str::<serde_json::Value>(text).ok()?;
    let queue_values = value.get("queue")?.as_array()?;
    let queue = queue_values
        .iter()
        .filter_map(|v| v.as_str().map(PathBuf::from))
        .collect::<Vec<_>>();
    let selected_queue_index = value
        .get("selected_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_queue_index = value
        .get("current_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_path = value
        .get("current_path")
        .and_then(serde_json::Value::as_str)
        .map(PathBuf::from);
    Some(SessionSnapshot {
        queue,
        selected_queue_index,
        current_queue_index,
        current_path,
    })
}

fn format_session_text(session: &SessionSnapshot) -> String {
    let payload = json!({
        "queue": session
            .queue
            .iter()
            .map(|p| p.to_string_lossy().to_string())
            .collect::<Vec<_>>(),
        "selected_queue_index": session.selected_queue_index,
        "current_queue_index": session.current_queue_index,
        "current_path": session
            .current_path
            .as_ref()
            .map(|p| p.to_string_lossy().to_string()),
    });
    serde_json::to_string_pretty(&payload).unwrap_or_else(|_| "{}".to_string())
}

fn save_session_snapshot(session: &SessionSnapshot) {
    let Some(path) = session_path() else {
        return;
    };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let text = format_session_text(session);
    let tmp_path = path.with_extension("json.tmp");
    if fs::write(&tmp_path, text).is_ok() {
        let _ = fs::rename(&tmp_path, &path);
    } else {
        let _ = fs::remove_file(&tmp_path);
    }
}

fn load_settings_into(settings: &mut BridgeSettings) {
    let Some(path) = settings_path() else {
        return;
    };
    let Ok(text) = fs::read_to_string(path) else {
        return;
    };
    parse_settings_text(settings, &text);
}

fn parse_settings_text(settings: &mut BridgeSettings, text: &str) {
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let key = k.trim();
        let value = v.trim();
        match key {
            "volume" => {
                if let Ok(x) = value.parse::<f32>() {
                    settings.volume = x.clamp(0.0, 1.0);
                }
            }
            "fft_size" => {
                if let Ok(x) = value.parse::<usize>() {
                    settings.fft_size = x.clamp(512, 8192).next_power_of_two();
                }
            }
            "spectrogram_view_mode" => {
                if let Some(mode) = SpectrogramViewMode::parse_settings_value(value) {
                    settings.spectrogram_view_mode = mode;
                }
            }
            "viewer_fullscreen_mode" => {
                if let Some(mode) = ViewerFullscreenMode::parse_settings_value(value) {
                    settings.viewer_fullscreen_mode = mode;
                }
            }
            "db_range" => {
                if let Ok(x) = value.parse::<f32>() {
                    settings.db_range = x.clamp(50.0, 120.0);
                }
            }
            "log_scale" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.log_scale = x != 0;
                }
            }
            "show_fps" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.show_fps = x != 0;
                }
            }
            "system_media_controls_enabled" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.integrations.system_media_controls_enabled = x != 0;
                }
            }
            "library_sort_mode" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.library_sort_mode = LibrarySortMode::from_i32(x);
                }
            }
            "lastfm_scrobbling_enabled" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.integrations.lastfm_scrobbling_enabled = x != 0;
                }
            }
            "lastfm_username" => {
                settings.integrations.lastfm_username = value.to_string();
            }
            _ => {}
        }
    }
}

fn save_settings(settings: &BridgeSettings) {
    let Some(path) = settings_path() else {
        return;
    };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let text = format_settings_text(settings);
    let _ = fs::write(path, text);
}

fn format_settings_text(settings: &BridgeSettings) -> String {
    format!(
        "volume={:.4}\nfft_size={}\nspectrogram_view_mode={}\nviewer_fullscreen_mode={}\ndb_range={:.2}\nlog_scale={}\nshow_fps={}\nsystem_media_controls_enabled={}\nlibrary_sort_mode={}\nlastfm_scrobbling_enabled={}\nlastfm_username={}\n",
        settings.volume,
        settings.fft_size,
        settings.spectrogram_view_mode.settings_value(),
        settings.viewer_fullscreen_mode.settings_value(),
        settings.db_range,
        i32::from(settings.display.log_scale),
        i32::from(settings.display.show_fps),
        i32::from(settings.integrations.system_media_controls_enabled),
        settings.library_sort_mode.to_i32(),
        i32::from(settings.integrations.lastfm_scrobbling_enabled),
        settings.integrations.lastfm_username,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lastfm::ServiceOptions as LastFmServiceOptions;
    use crate::library::LibraryRoot;
    use std::io::Write;
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
    ) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
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
    fn disc_section_detection_accepts_common_main_disc_names() {
        assert!(is_main_album_disc_section("CD1"));
        assert!(is_main_album_disc_section("CD 2"));
        assert!(is_main_album_disc_section("disc-03"));
        assert!(is_main_album_disc_section("Disk 4 (bonus)"));
        assert!(is_main_album_disc_section("DVD1"));
        assert!(is_main_album_disc_section("DVD 2"));
        assert!(!is_main_album_disc_section("Live"));
        assert!(!is_main_album_disc_section("discography"));
    }

    #[test]
    fn prepare_search_library_counts_main_album_tracks_with_cd_sections() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![
                crate::library::LibraryTrack {
                    path: p("/music/Artist/Album/01 - Intro.flac"),
                    root_path: root.clone(),
                    title: "Intro".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: Some(100.0),
                },
                crate::library::LibraryTrack {
                    path: p("/music/Artist/Album/CD1/02 - Song.flac"),
                    root_path: root.clone(),
                    title: "Song".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(2),
                    duration_secs: Some(120.0),
                },
                crate::library::LibraryTrack {
                    path: p("/music/Artist/Album/Bonus/03 - Extra.flac"),
                    root_path: root.clone(),
                    title: "Extra".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(3),
                    duration_secs: Some(80.0),
                },
            ],
            ..LibrarySnapshot::default()
        };

        let prepared = prepare_search_library(&snapshot);
        let album_key = "album|/music|Artist|Album".to_string();
        let inv = prepared
            .album_inventory
            .get(&album_key)
            .expect("album inventory present");
        assert_eq!(inv.main_track_count, 2);
        assert!(inv.has_main_duration);
        assert!((inv.main_total_length - 220.0).abs() < 0.01);
    }

    #[test]
    fn fallback_search_cancels_when_newer_query_arrives() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![crate::library::LibraryTrack {
                path: p("/music/Artist/Album/01 - Song.flac"),
                root_path: root,
                title: "Song".to_string(),
                artist: "Artist".to_string(),
                album: "Album".to_string(),
                cover_path: String::new(),
                genre: String::new(),
                year: Some(2020),
                track_no: Some(1),
                duration_secs: Some(60.0),
            }],
            ..LibrarySnapshot::default()
        };
        let prepared = prepare_search_library(&snapshot);
        let (tx, rx) = unbounded::<SearchWorkerQuery>();
        tx.send(SearchWorkerQuery {
            seq: 99,
            query: "new".to_string(),
            library: Arc::new(snapshot),
        })
        .expect("queue newer search");

        match search_tracks_fallback_prepared("song", &prepared, 10, &rx) {
            SearchFallbackOutcome::Cancelled(next) => assert_eq!(next.seq, 99),
            SearchFallbackOutcome::Hits(_) => panic!("expected cancellation"),
        }
    }

    #[test]
    fn prepared_cache_reuses_same_search_revision_across_snapshot_arcs() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )],
            search_revision: 7,
            ..LibrarySnapshot::default()
        };
        let first = Arc::new(library.clone());
        let second = Arc::new(LibrarySnapshot {
            last_error: Some("scan still running".to_string()),
            ..library
        });

        let mut cache = SearchWorkerPreparedCache::default();
        let prepared_first = cache.prepared_for(&first);
        let prepared_second = cache.prepared_for(&second);

        assert!(Arc::ptr_eq(&prepared_first, &prepared_second));
    }

    #[test]
    fn prepared_cache_rebuilds_when_search_revision_changes() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )],
            search_revision: 7,
            ..LibrarySnapshot::default()
        };
        let first = Arc::new(library.clone());
        let second = Arc::new(LibrarySnapshot {
            search_revision: 8,
            ..library
        });

        let mut cache = SearchWorkerPreparedCache::default();
        let prepared_first = cache.prepared_for(&first);
        let prepared_second = cache.prepared_for(&second);

        assert!(!Arc::ptr_eq(&prepared_first, &prepared_second));
    }

    #[test]
    fn album_search_rows_include_album_cover_path() {
        let _guard = test_guard();
        std::env::set_var("FERROUS_SEARCH_DISABLE_FTS", "1");
        let root = test_dir("search-album-cover");
        let album_dir = root.join("Artist").join("Album");
        let cover = album_dir.join("cover.jpg");
        let track = album_dir.join("01 - Song.flac");

        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![crate::library::LibraryTrack {
                path: track,
                root_path: root.clone(),
                title: "Song".to_string(),
                artist: "Artist".to_string(),
                album: "Album".to_string(),
                cover_path: cover.to_string_lossy().to_string(),
                genre: "Rock".to_string(),
                year: Some(2020),
                track_no: Some(1),
                duration_secs: Some(60.0),
            }],
            search_revision: 1,
            ..LibrarySnapshot::default()
        };
        let (_tx, rx) = unbounded::<SearchWorkerQuery>();
        let mut prepared_cache = SearchWorkerPreparedCache::default();
        let outcome = build_search_results_frame(
            &SearchWorkerQuery {
                seq: 1,
                query: "album".to_string(),
                library: Arc::new(library),
            },
            &mut prepared_cache,
            &rx,
        );

        let frame = match outcome {
            SearchBuildOutcome::Frame(frame) => frame,
            SearchBuildOutcome::Cancelled(_) => panic!("unexpected cancellation"),
        };
        let album_row = frame
            .rows
            .iter()
            .find(|row| row.row_type == BridgeSearchResultRowType::Album)
            .expect("album row present");
        assert_eq!(album_row.cover_path, cover.to_string_lossy());
        std::env::remove_var("FERROUS_SEARCH_DISABLE_FTS");
    }

    fn test_guard() -> MutexGuard<'static, ()> {
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    #[test]
    fn settings_roundtrip_text_format() {
        let settings = BridgeSettings {
            volume: 0.42,
            fft_size: 2048,
            spectrogram_view_mode: SpectrogramViewMode::PerChannel,
            viewer_fullscreen_mode: ViewerFullscreenMode::WholeScreen,
            db_range: 77.5,
            display: BridgeDisplaySettings {
                log_scale: true,
                show_fps: true,
            },
            library_sort_mode: LibrarySortMode::Title,
            integrations: BridgeIntegrationSettings {
                system_media_controls_enabled: false,
                lastfm_scrobbling_enabled: true,
                lastfm_username: "tester".to_string(),
            },
        };
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!((parsed.volume - 0.42).abs() < 0.0001);
        assert_eq!(parsed.fft_size, 2048);
        assert_eq!(
            parsed.spectrogram_view_mode,
            SpectrogramViewMode::PerChannel
        );
        assert_eq!(
            parsed.viewer_fullscreen_mode,
            ViewerFullscreenMode::WholeScreen
        );
        assert!((parsed.db_range - 77.5).abs() < 0.0001);
        assert!(parsed.display.log_scale);
        assert!(parsed.display.show_fps);
        assert!(!parsed.integrations.system_media_controls_enabled);
        assert_eq!(parsed.library_sort_mode, LibrarySortMode::Title);
        assert!(parsed.integrations.lastfm_scrobbling_enabled);
        assert_eq!(parsed.integrations.lastfm_username, "tester");
    }

    #[test]
    fn settings_parse_clamps_invalid_ranges() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=2.5\nfft_size=111\nspectrogram_view_mode=bad\nviewer_fullscreen_mode=bad\ndb_range=500\nlog_scale=0\nshow_fps=1\nsystem_media_controls_enabled=0\nlibrary_sort_mode=0\n",
        );
        assert_eq!(settings.volume, 1.0);
        assert_eq!(settings.fft_size, 512);
        assert_eq!(settings.spectrogram_view_mode, SpectrogramViewMode::Downmix);
        assert_eq!(
            settings.viewer_fullscreen_mode,
            ViewerFullscreenMode::WithinWindow
        );
        assert_eq!(settings.db_range, 120.0);
        assert!(!settings.display.log_scale);
        assert!(settings.display.show_fps);
        assert!(!settings.integrations.system_media_controls_enabled);
        assert_eq!(settings.library_sort_mode, LibrarySortMode::Year);
        assert!(!settings.integrations.lastfm_scrobbling_enabled);
        assert!(settings.integrations.lastfm_username.is_empty());
    }

    #[test]
    fn settings_default_system_media_controls_enabled_when_omitted() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=0.5\nfft_size=2048\nspectrogram_view_mode=per_channel\nviewer_fullscreen_mode=whole_screen\ndb_range=80\nlog_scale=1\nshow_fps=0\nlibrary_sort_mode=1\n",
        );
        assert!(settings.integrations.system_media_controls_enabled);
        assert_eq!(
            settings.spectrogram_view_mode,
            SpectrogramViewMode::PerChannel
        );
        assert_eq!(
            settings.viewer_fullscreen_mode,
            ViewerFullscreenMode::WholeScreen
        );
    }

    #[test]
    fn session_roundtrip_text_format() {
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), p("/b.flac")],
            selected_queue_index: Some(1),
            current_queue_index: Some(0),
            current_path: Some(p("/a.flac")),
        };
        let text = format_session_text(&session);
        let parsed = parse_session_text(&text).expect("parse session text");
        assert_eq!(parsed, session);
    }

    #[test]
    fn session_parse_rejects_missing_queue_array() {
        let parsed = parse_session_text(r#"{"selected_queue_index":1}"#);
        assert!(parsed.is_none());
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
    fn initial_restore_skips_queue_detail_sync_until_library_is_loaded() {
        let mut state = BridgeState::default();
        state.queue = vec![p("/music/a.flac")];
        assert!(!should_sync_queue_details_on_initial_restore(&state));

        state.library = Arc::new(LibrarySnapshot {
            tracks: vec![LibraryTrack {
                path: p("/music/a.flac"),
                ..LibraryTrack::default()
            }],
            ..LibrarySnapshot::default()
        });
        assert!(should_sync_queue_details_on_initial_restore(&state));
    }

    #[test]
    fn pump_library_events_requests_queue_details_for_restored_external_tracks() {
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
    fn resolve_session_current_index_prefers_valid_index() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, Some(2), Some(&p("/a.flac")));
        assert_eq!(idx, Some(2));
    }

    #[test]
    fn resolve_session_current_index_falls_back_to_path_when_index_missing() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, None, Some(&p("/b.flac")));
        assert_eq!(idx, Some(1));
    }

    #[test]
    fn resolve_session_current_index_falls_back_to_path_when_index_invalid() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, Some(9), Some(&p("/c.flac")));
        assert_eq!(idx, Some(2));
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
                library_track(
                    "/music/Porcupine Tree/Muut/Porcupine Tree Sampler 2005/01 - Hello.flac",
                    &root,
                    "Blackfield",
                    "Porcupine Tree Sampler 2005",
                    Some(2005),
                    Some(1),
                ),
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
            vec![p(
                "/music/Porcupine Tree/Muut/Porcupine Tree Sampler 2005/01 - Hello.flac"
            )]
        );
    }

    #[test]
    fn queue_append_into_empty_loads_full_queue() {
        let mut queue = Vec::new();
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(vec![p("/a.flac"), p("/b.flac")]),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")])]
        );
    }

    #[test]
    fn queue_append_empty_is_noop() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(Vec::new()),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac")]);
        assert_eq!(selected, Some(0));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_play_at_out_of_bounds_emits_error() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::PlayAt(3),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(
            outcome.error.as_deref(),
            Some("queue index 3 out of bounds")
        );
        assert!(outcome.playback_ops.is_empty());
    }

    #[test]
    fn queue_move_updates_selection_and_uses_move_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Move { from: 0, to: 2 },
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/b.flac"), p("/c.flac"), p("/a.flac")]);
        assert_eq!(selected, Some(2));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::Move { from: 0, to: 2 }]
        );
    }

    #[test]
    fn queue_move_invalid_indices_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Move { from: 2, to: 0 },
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_replace_autoplay_loads_and_starts_playback() {
        let mut queue = Vec::new();
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a.flac"), p("/b.flac")],
                autoplay: true,
            },
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![
                QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")]),
                QueuePlaybackOp::PlayAt(0),
                QueuePlaybackOp::Play,
            ]
        );
    }

    #[test]
    fn queue_append_non_empty_uses_add_to_queue_op() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(vec![p("/b.flac"), p("/c.flac")]),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::AddToQueue(vec![
                p("/b.flac"),
                p("/c.flac")
            ])]
        );
    }

    #[test]
    fn queue_remove_last_track_clears_selection_and_playback_queue() {
        let mut queue = vec![p("/only.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(0),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
    }

    #[test]
    fn queue_remove_middle_track_uses_remove_op_and_keeps_reasonable_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(2);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(1),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::RemoveAt(1)]);
    }

    #[test]
    fn queue_remove_out_of_bounds_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(3),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(selected, Some(0));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_updates_state_without_playback_ops() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(1)),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_same_index_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(1)),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_out_of_bounds_clears_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(9)),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert!(selected.is_none());
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn selection_only_queue_commands_do_not_require_queue_snapshot() {
        assert!(!command_requires_queue_snapshot(&BridgeCommand::Queue(
            BridgeQueueCommand::Select(Some(0)),
        )));
        assert!(!command_requires_queue_snapshot(&BridgeCommand::Queue(
            BridgeQueueCommand::PlayAt(0),
        )));
    }

    #[test]
    fn queue_clear_empties_state_and_emits_clear_queue_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Clear,
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_play_at_while_playing_skips_redundant_play_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::PlayAt(1),
            &mut queue,
            &mut selected,
            PlaybackState::Playing,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::PlayAt(1)]);
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_replace_autoplay_while_playing_skips_redundant_play_op() {
        let mut queue = vec![p("/old.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a.flac"), p("/b.flac")],
                autoplay: true,
            },
            &mut queue,
            &mut selected,
            PlaybackState::Playing,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![
                QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")]),
                QueuePlaybackOp::PlayAt(0),
            ]
        );
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
        let mut last = None;
        while Instant::now() < deadline {
            if let Some(event) = bridge.recv_timeout(Duration::from_millis(30)) {
                if let BridgeEvent::Snapshot(snapshot) = event {
                    if predicate(&snapshot) {
                        return Some(*snapshot);
                    }
                    last = Some(*snapshot);
                }
            }
            while let Some(event) = bridge.try_recv() {
                if let BridgeEvent::Snapshot(snapshot) = event {
                    if predicate(&snapshot) {
                        return Some(*snapshot);
                    }
                    last = Some(*snapshot);
                }
            }
        }
        last
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
    fn bridge_queue_roundtrip_snapshot_integration() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn();
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![p("/music/a.flac"), p("/music/b.flac")],
            autoplay: false,
        }));
        bridge.command(BridgeCommand::RequestSnapshot);

        let loaded = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.len() == 2 && s.selected_queue_index == Some(0)
        })
        .expect("snapshot with loaded queue");
        assert_eq!(loaded.queue.len(), 2);
        assert_eq!(loaded.selected_queue_index, Some(0));

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Clear));
        bridge.command(BridgeCommand::RequestSnapshot);
        let cleared = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.is_empty() && s.selected_queue_index.is_none()
        })
        .expect("snapshot with cleared queue");
        assert!(cleared.queue.is_empty());
        assert!(cleared.selected_queue_index.is_none());

        bridge.command(BridgeCommand::Shutdown);
    }

    #[test]
    fn metadata_event_emits_snapshot_immediately_while_stopped() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default());
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        let _ = runtime.drain_pending_updates(&event_tx);
        runtime.flags.pending_snapshot = SnapshotUrgency::None;
        while event_rx.try_recv().is_ok() {}
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
    fn library_event_rebuilds_tree_on_first_wake() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default());
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        let _ = runtime.drain_pending_updates(&event_tx);
        runtime.flags.pending_snapshot = SnapshotUrgency::None;
        while event_rx.try_recv().is_ok() {}

        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )],
            ..LibrarySnapshot::default()
        };

        runtime.handle_wake(
            BridgeLoopWake::Library(LibraryEvent::Snapshot(snapshot)),
            &event_tx,
        );

        let snapshot = match event_rx.recv_timeout(Duration::from_millis(20)) {
            Ok(BridgeEvent::Snapshot(snapshot)) => *snapshot,
            other => panic!("expected library snapshot, got {other:?}"),
        };
        assert_eq!(snapshot.library.tracks.len(), 1);
        assert!(snapshot.pre_built_tree_bytes.is_some());
        assert!(!snapshot
            .pre_built_tree_bytes
            .as_ref()
            .is_some_and(|bytes| bytes.is_empty()));
    }

    #[test]
    fn playing_position_updates_wait_for_coarse_heartbeat() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default());
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        let _ = runtime.drain_pending_updates(&event_tx);
        runtime.flags.pending_snapshot = SnapshotUrgency::None;
        while event_rx.try_recv().is_ok() {}
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

    #[test]
    fn bridge_queue_play_seek_clamp_and_remove_integration() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn();
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
        bridge.command(BridgeCommand::RequestSnapshot);
        let seeked = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.len() == 2
                && s.selected_queue_index == Some(1)
                && s.playback.current.as_ref() == Some(&second)
                && s.playback.position >= Duration::from_secs(179)
        })
        .expect("snapshot after play-at + clamped seek");
        assert_eq!(seeked.playback.current.as_ref(), Some(&second));
        assert_eq!(seeked.selected_queue_index, Some(1));

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Remove(1)));
        bridge.command(BridgeCommand::RequestSnapshot);
        let removed = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.len() == 1
                && s.selected_queue_index == Some(0)
                && s.playback.current.as_ref() != Some(&second)
        })
        .expect("snapshot after removing selected track");
        assert_ne!(removed.playback.current.as_ref(), Some(&second));
        if let Some(current) = removed.playback.current.as_ref() {
            assert_eq!(current, &first);
        }
        assert_eq!(removed.selected_queue_index, Some(0));

        bridge.command(BridgeCommand::Shutdown);
    }

    #[test]
    fn seek_event_does_not_trigger_early_track_switch_side_effects() {
        let (analysis, _analysis_rx) = AnalysisEngine::new();
        let (metadata, _metadata_rx) = MetadataService::new();
        let (playback_tx, playback_rx) = crossbeam_channel::unbounded::<PlaybackEvent>();

        let mut state = BridgeState::default();
        state.analysis.waveform_peaks = vec![0.2, 0.4, 0.6];
        state.metadata.title = "Track A".to_string();
        state.metadata.artist = "Artist A".to_string();

        playback_tx
            .send(PlaybackEvent::Seeked)
            .expect("send seeked event");
        let changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        assert!(!changed);
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
    fn stopped_track_change_defers_waveform_load_until_playback_resumes() {
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
            AnalysisEvent::Snapshot(_) => {}
            _ => panic!("unexpected event variant"),
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

    #[test]
    fn track_change_does_not_swap_metadata_until_metadata_event_arrives() {
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

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_natural_handoff_advances_current_track_and_keeps_playing() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn();
        let first = p("/tmp/ferrous_gapless_case_a.flac");
        let second = p("/tmp/ferrous_gapless_case_b.flac");
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![first.clone(), second.clone()],
            autoplay: true,
        }));
        bridge.command(BridgeCommand::RequestSnapshot);
        let loaded = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.len() == 2
                && s.playback.current.as_ref() == Some(&first)
                && s.playback.state == crate::playback::PlaybackState::Playing
        })
        .expect("loaded first track");
        assert_eq!(loaded.playback.current.as_ref(), Some(&first));

        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        let deadline = Instant::now() + Duration::from_secs(4);
        let mut handed_off = None;
        while Instant::now() < deadline {
            // Leave idle time for the bridge ticker to drive playback Poll, then sample state.
            std::thread::sleep(Duration::from_millis(80));
            bridge.command(BridgeCommand::RequestSnapshot);
            if let Some(snapshot) =
                wait_for_snapshot_matching(&bridge, Duration::from_millis(120), |_| false)
            {
                if snapshot.queue.len() == 2
                    && snapshot.playback.current.as_ref() == Some(&second)
                    && snapshot.playback.state == crate::playback::PlaybackState::Playing
                {
                    handed_off = Some(snapshot);
                    break;
                }
            }
        }
        let handed_off = handed_off.expect("handoff to second track");
        assert_eq!(handed_off.playback.current.as_ref(), Some(&second));
        assert_eq!(handed_off.queue.len(), 2);

        bridge.command(BridgeCommand::Shutdown);
    }

    #[cfg(not(feature = "gst"))]
    #[test]
    fn bridge_natural_handoff_keeps_old_metadata_until_new_metadata_arrives() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn_with_metadata_delay(Duration::from_millis(300));
        let first = p("/tmp/ferrous_metadata_case_a.flac");
        let second = p("/tmp/ferrous_metadata_case_b.flac");
        let first_title = "ferrous_metadata_case_a";
        let second_title = "ferrous_metadata_case_b";

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![first.clone(), second.clone()],
            autoplay: true,
        }));
        bridge.command(BridgeCommand::RequestSnapshot);
        let first_loaded = wait_for_snapshot_matching(&bridge, Duration::from_secs(5), |s| {
            s.queue.len() == 2
                && s.playback.current.as_ref() == Some(&first)
                && s.metadata.title == first_title
        })
        .expect("first track + metadata loaded");
        assert_eq!(first_loaded.metadata.title, first_title);

        bridge.command(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
            Duration::from_secs(180),
        )));
        bridge.command(BridgeCommand::RequestSnapshot);
        let handoff_snapshot = wait_for_snapshot_matching(&bridge, Duration::from_secs(2), |s| {
            s.queue.len() == 2
                && s.playback.current.as_ref() == Some(&second)
                && s.metadata.title == first_title
        })
        .expect("handoff snapshot keeps old metadata before new metadata arrives");
        assert_eq!(handoff_snapshot.playback.current.as_ref(), Some(&second));
        assert_eq!(handoff_snapshot.metadata.title, first_title);

        bridge.command(BridgeCommand::RequestSnapshot);
        let updated_metadata = wait_for_snapshot_matching(&bridge, Duration::from_secs(4), |s| {
            s.queue.len() == 2
                && s.playback.current.as_ref() == Some(&second)
                && s.metadata.title == second_title
        })
        .expect("metadata updated for handed-off track");
        assert_eq!(updated_metadata.metadata.title, second_title);

        bridge.command(BridgeCommand::Shutdown);
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
