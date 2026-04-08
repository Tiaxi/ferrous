// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{after, bounded, select, unbounded, Receiver, Sender, TrySendError};

use crate::analysis::{
    AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot, SpectrogramDisplayMode,
    SpectrogramViewMode,
};
use crate::artwork::apply_artwork_to_track;
use crate::lastfm::{
    self, Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    RuntimeState as LastFmRuntimeState, ServiceOptions as LastFmServiceOptions,
};
use crate::library::{
    load_external_track_cache, read_track_info, refresh_cover_paths_for_tracks,
    refresh_cover_paths_for_tracks_with_override, store_external_track_cache, IndexedTrack,
    LibraryEvent, LibraryService, LibrarySnapshot, TrackFileFingerprint,
};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{
    PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode,
};

mod commands;
mod config;
mod events;
pub mod ffi;
pub mod library_tree;
mod queue;
mod search;

use commands::{handle_bridge_command, sync_queue_details, BridgeCommandContext};
use config::{
    apply_session_restore, config_base_path, load_session_snapshot, load_settings_into,
    save_session_snapshot, save_settings, session_snapshot_for_state, SessionSnapshot,
};
use events::{
    drain_analysis_events, drain_apply_album_art_events, drain_external_queue_detail_events,
    drain_lastfm_events, drain_library_events, drain_metadata_events, drain_playback_events,
    note_precomputed_spectrogram_chunk, process_analysis_event, process_apply_album_art_event,
    process_external_queue_detail_event, process_lastfm_event, process_library_event,
    process_metadata_event, process_playback_event, tick_lastfm_playback,
};
pub(crate) use search::is_main_album_disc_section;
use search::{drain_search_results, process_search_results, run_search_worker, SearchWorkerQuery};

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
    SetSpectrogramDisplayMode(SpectrogramDisplayMode),
    SetViewerFullscreenMode(ViewerFullscreenMode),
    SetDbRange(f32),
    SetLogScale(bool),
    SetShowFps(bool),
    SetShowSpectrogramCrosshair(bool),
    SetShowSpectrogramScale(bool),
    SetSystemMediaControlsEnabled(bool),
    SetLibrarySortMode(LibrarySortMode),
    SetLastFmScrobblingEnabled(bool),
    SetChannelButtonsVisibility(u8),
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

// Settings struct with individual toggle fields — not a state machine.
#[allow(clippy::struct_excessive_bools)]
#[derive(Debug, Clone)]
pub struct BridgeDisplaySettings {
    pub log_scale: bool,
    pub show_fps: bool,
    pub show_spectrogram_crosshair: bool,
    pub show_spectrogram_scale: bool,
    pub channel_buttons_visibility: u8,
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
    pub spectrogram_display_mode: SpectrogramDisplayMode,
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
            spectrogram_display_mode: SpectrogramDisplayMode::Rolling,
            viewer_fullscreen_mode: ViewerFullscreenMode::WithinWindow,
            db_range: 132.0,
            display: BridgeDisplaySettings {
                log_scale: false,
                show_fps,
                show_spectrogram_crosshair: false,
                show_spectrogram_scale: false,
                channel_buttons_visibility: 1,
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
pub(super) struct BridgeState {
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
    analysis_track_token: u64,
    /// Set by gapless `TrackChanged` events.  When true, the next snapshot
    /// should skip the queue section because only the playing index changed
    /// (already in the playback section).  Avoids the 100-250 ms model
    /// reset stall on the Qt side for large playlists.
    skip_queue_for_gapless: bool,
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

#[derive(Debug, Clone)]
pub(super) struct ExternalQueueDetailsRequest {
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

fn metadata_for_snapshot(metadata: &TrackMetadata) -> TrackMetadata {
    TrackMetadata {
        source_path: metadata.source_path.clone(),
        title: metadata.title.clone(),
        artist: metadata.artist.clone(),
        album: metadata.album.clone(),
        genre: metadata.genre.clone(),
        year: metadata.year,
        track_number: metadata.track_number,
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
            env_duration_ms("FERROUS_BRIDGE_PLAYING_HEARTBEAT_MS", 40, 16, 1000);
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
        let analysis_urgency = drain_analysis_events(&self.analysis_rx, event_tx, &mut self.state);
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
        // Gapless track changes don't modify the queue contents — only
        // the playing index moves, which is already in the playback section.
        // Suppress the queue section to avoid the 100-250 ms model reset
        // stall on the Qt side for large playlists.  Any deferred queue
        // detail sync catches up on the next revalidation cycle (~2 s).
        if self.state.skip_queue_for_gapless {
            self.state.skip_queue_for_gapless = false;
            self.snapshot_plan.include_queue_in_next_snapshot = false;
        }
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
        profile_eprintln!(
            "[bridge] rss_kb={} playback_q={} analysis_q={} metadata_q={} library_q={} wave_len={} sent_snap/s={} drop_snap/s={}",
            _rss_kb,
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

pub(super) fn try_send_event(
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

pub(super) fn command_requires_queue_snapshot(cmd: &BridgeCommand) -> bool {
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::{LibraryRoot, LibraryTrack};
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

    fn test_guard() -> MutexGuard<'static, ()> {
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
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
        let deadline = Instant::now() + Duration::from_millis(500);
        loop {
            let _ = runtime.drain_pending_updates(event_tx);
            let had_events = event_rx.try_recv().is_ok();
            // Drain any remaining events from this cycle.
            while event_rx.try_recv().is_ok() {}
            if !had_events || Instant::now() >= deadline {
                break;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        runtime.flags.pending_snapshot = SnapshotUrgency::None;
        while event_rx.try_recv().is_ok() {}
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
    fn bridge_queue_roundtrip_snapshot_integration() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn();
        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Replace {
            tracks: vec![p("/music/a.flac"), p("/music/b.flac")],
            autoplay: false,
        }));
        bridge.command(BridgeCommand::RequestSnapshot);

        let loaded = wait_for_snapshot_matching(&bridge, Duration::from_secs(10), |s| {
            s.queue.len() == 2 && s.selected_queue_index == Some(0)
        })
        .expect("snapshot with loaded queue");
        assert_eq!(loaded.queue.len(), 2);
        assert_eq!(loaded.selected_queue_index, Some(0));

        bridge.command(BridgeCommand::Queue(BridgeQueueCommand::Clear));
        bridge.command(BridgeCommand::RequestSnapshot);
        let cleared = wait_for_snapshot_matching(&bridge, Duration::from_secs(10), |s| {
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
    fn analysis_wake_forwards_following_precomputed_chunks_from_drain_path() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default());
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);

        let (analysis_tx, analysis_rx) = crossbeam_channel::unbounded::<AnalysisEvent>();
        runtime.analysis_rx = analysis_rx;

        let first = crate::analysis::PrecomputedSpectrogramChunk {
            track_token: 7,
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
    fn library_event_rebuilds_tree_on_first_wake() {
        let _guard = test_guard();
        let mut runtime = BridgeLoopRuntime::new(BridgeRuntimeOptions::default());
        let (event_tx, event_rx) = bounded::<BridgeEvent>(32);
        drain_initial_events(&mut runtime, &event_tx, &event_rx);

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
        let seeked = {
            let deadline = Instant::now() + Duration::from_secs(10);
            let mut result = None;
            let sc = second.clone();
            while Instant::now() < deadline {
                std::thread::sleep(Duration::from_millis(60));
                bridge.command(BridgeCommand::RequestSnapshot);
                if let Some(snap) =
                    wait_for_snapshot_matching(&bridge, Duration::from_millis(200), |s| {
                        s.queue.len() == 2
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
                        s.queue.len() == 1
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
        let bridge = FrontendBridgeHandle::spawn();
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
                        s.queue.len() == 2
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
                        s.queue.len() == 2
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
    fn bridge_natural_handoff_keeps_old_metadata_until_new_metadata_arrives() {
        let _guard = test_guard();
        let bridge = FrontendBridgeHandle::spawn_with_metadata_delay(Duration::from_secs(2));
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
                        s.queue.len() == 2
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
                        s.queue.len() == 2
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
                        s.queue.len() == 2
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
}
