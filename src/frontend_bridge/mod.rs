use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, select, tick, unbounded, Receiver, Sender, TrySendError};
use serde_json::json;

use crate::analysis::{AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::library::{
    search_tracks_fts, LibraryCommand, LibraryEvent, LibrarySearchTrack, LibraryService,
    LibrarySnapshot,
};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{
    PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot, PlaybackState, RepeatMode,
    TrackChangeKind,
};

pub mod ffi;
pub mod library_tree;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LibrarySortMode {
    #[default]
    Year,
    Title,
}

impl LibrarySortMode {
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => Self::Title,
            _ => Self::Year,
        }
    }

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
    AddRoot(PathBuf),
    RemoveRoot(PathBuf),
    RescanRoot(PathBuf),
    RescanAll,
    AddTrack(PathBuf),
    PlayTrack(PathBuf),
    ReplaceWithAlbum(Vec<PathBuf>),
    AppendAlbum(Vec<PathBuf>),
    ReplaceAlbumByKey { artist: String, album: String },
    AppendAlbumByKey { artist: String, album: String },
    ReplaceArtistByKey { artist: String },
    AppendArtistByKey { artist: String },
    SetNodeExpanded { key: String, expanded: bool },
    SetSearchQuery { seq: u32, query: String },
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
    SetDbRange(f32),
    SetLogScale(bool),
    SetShowFps(bool),
    SetLibrarySortMode(LibrarySortMode),
}

#[derive(Debug, Clone)]
pub enum BridgeEvent {
    Snapshot(Box<BridgeSnapshot>),
    SearchResults(Box<BridgeSearchResultsFrame>),
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
    pub library_artist_count: usize,
    pub library_album_count: usize,
    pub pre_built_tree_bytes: Option<Arc<Vec<u8>>>,
    pub queue: Vec<PathBuf>,
    pub selected_queue_index: Option<usize>,
    pub settings: BridgeSettings,
}

#[derive(Debug, Clone)]
pub struct BridgeSettings {
    pub volume: f32,
    pub fft_size: usize,
    pub db_range: f32,
    pub log_scale: bool,
    pub show_fps: bool,
    pub library_sort_mode: LibrarySortMode,
}

impl Default for BridgeSettings {
    fn default() -> Self {
        let show_fps = std::env::var_os("FERROUS_UI_SHOW_FPS").is_some()
            || std::env::var_os("FERROUS_PROFILE_UI").is_some()
            || std::env::var_os("FERROUS_PROFILE").is_some();
        Self {
            volume: 1.0,
            fft_size: 8192,
            db_range: 90.0,
            log_scale: false,
            show_fps,
            library_sort_mode: LibrarySortMode::Year,
        }
    }
}

#[derive(Debug, Clone, Default)]
struct BridgeState {
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    library: Arc<LibrarySnapshot>,
    library_artist_count: usize,
    library_album_count: usize,
    pre_built_tree_bytes: Arc<Vec<u8>>,
    expanded_keys: HashSet<String>,
    queue: Vec<PathBuf>,
    selected_queue_index: Option<usize>,
    settings: BridgeSettings,
    pending_search_results: Option<BridgeSearchResultsFrame>,
}

#[derive(Debug)]
struct SearchWorkerQuery {
    seq: u32,
    query: String,
    library: Arc<LibrarySnapshot>,
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
    path_string: String,
    path_lower: String,
    title: String,
    artist: String,
    album: String,
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
    roots: Vec<PathBuf>,
    tracks: Vec<PreparedSearchTrack>,
    context_by_path: HashMap<String, TreePathContext>,
    album_inventory: HashMap<String, AlbumInventoryAcc>,
}

#[derive(Default)]
struct SearchWorkerPreparedCache {
    source_library: Option<Arc<LibrarySnapshot>>,
    prepared: Option<Arc<PreparedSearchLibrary>>,
}

impl SearchWorkerPreparedCache {
    fn prepared_for(&mut self, library: &Arc<LibrarySnapshot>) -> Arc<PreparedSearchLibrary> {
        if let (Some(source), Some(prepared)) = (&self.source_library, &self.prepared) {
            if Arc::ptr_eq(source, library) {
                return Arc::clone(prepared);
            }
        }
        let started = Instant::now();
        let prepared = Arc::new(prepare_search_library(library.as_ref()));
        if search_profile_enabled() {
            eprintln!(
                "[search-worker] cache rebuild roots={} tracks={} elapsed_ms={}",
                prepared.roots.len(),
                prepared.tracks.len(),
                started.elapsed().as_millis()
            );
        }
        self.source_library = Some(Arc::clone(library));
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
    fn snapshot(&self, include_tree: bool) -> BridgeSnapshot {
        BridgeSnapshot {
            playback: self.playback.clone(),
            analysis: self.analysis.clone(),
            metadata: metadata_for_snapshot(&self.metadata),
            library: self.library.clone(),
            library_artist_count: self.library_artist_count,
            library_album_count: self.library_album_count,
            pre_built_tree_bytes: if include_tree {
                Some(self.pre_built_tree_bytes.clone())
            } else {
                None
            },
            queue: self.queue.clone(),
            selected_queue_index: self.selected_queue_index,
            settings: self.settings.clone(),
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
}

fn metadata_for_snapshot(metadata: &TrackMetadata) -> TrackMetadata {
    TrackMetadata {
        source_path: metadata.source_path.clone(),
        title: metadata.title.clone(),
        artist: metadata.artist.clone(),
        album: metadata.album.clone(),
        sample_rate_hz: metadata.sample_rate_hz,
        bitrate_kbps: metadata.bitrate_kbps,
        channels: metadata.channels,
        bit_depth: metadata.bit_depth,
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
            .spawn(move || run_bridge_loop(cmd_rx, event_tx, options));
        Self {
            tx: cmd_tx,
            rx: event_rx,
        }
    }

    pub fn command(&self, cmd: BridgeCommand) {
        let _ = self.tx.send(cmd);
    }

    pub fn recv_timeout(&self, timeout: Duration) -> Option<BridgeEvent> {
        self.rx.recv_timeout(timeout).ok()
    }

    pub fn try_recv(&self) -> Option<BridgeEvent> {
        self.rx.try_recv().ok()
    }
}

fn run_bridge_loop(
    cmd_rx: Receiver<BridgeCommand>,
    event_tx: Sender<BridgeEvent>,
    options: BridgeRuntimeOptions,
) {
    let (analysis, analysis_rx) = AnalysisEngine::new();
    let (playback, playback_rx) = PlaybackEngine::new(analysis.sender(), analysis.pcm_sender());
    let (metadata, metadata_rx) = MetadataService::new_with_delay(options.metadata_delay);
    let (library, library_rx) = LibraryService::new();
    let (search_query_tx, search_query_rx) = unbounded::<SearchWorkerQuery>();
    let (search_results_tx, search_results_rx) = unbounded::<BridgeSearchResultsFrame>();
    let _ = std::thread::Builder::new()
        .name("ferrous-bridge-search".to_string())
        .spawn(move || run_search_worker(search_query_rx, search_results_tx));

    let mut state = BridgeState::default();
    load_settings_into(&mut state.settings);
    state.playback.volume = state.settings.volume;
    playback.command(PlaybackCommand::SetVolume(state.settings.volume));
    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));
    apply_session_restore(&mut state, &playback, load_session_snapshot().as_ref());
    state.rebuild_pre_built_tree();

    let mut running = true;
    let mut settings_dirty = false;
    let mut last_settings_save = Instant::now();
    let mut last_session_save = Instant::now();
    let mut last_saved_session: Option<SessionSnapshot> = None;
    let ticker = tick(Duration::from_millis(16));
    let playing_poll_interval_ms = std::env::var("FERROUS_PLAYBACK_POLL_MS")
        .ok()
        .and_then(|raw| raw.parse::<u64>().ok())
        .map_or(40, |v| v.clamp(8, 500));
    let playing_poll_interval = Duration::from_millis(playing_poll_interval_ms);
    let mut last_playing_poll = Instant::now()
        .checked_sub(playing_poll_interval)
        .unwrap_or_else(Instant::now);
    let idle_poll_interval = Duration::from_millis(250);
    let mut last_idle_poll = Instant::now();
    let profile_enabled = std::env::var_os("FERROUS_PROFILE").is_some();
    let mut profile_last = Instant::now();
    let mut prof_snapshots_sent = 0usize;
    let mut prof_snapshots_dropped = 0usize;
    let snapshot_interval_ms = std::env::var("FERROUS_BRIDGE_SNAPSHOT_MS")
        .ok()
        .and_then(|raw| raw.parse::<u64>().ok())
        .map_or(16, |v| v.clamp(8, 1000));
    let snapshot_interval = Duration::from_millis(snapshot_interval_ms);
    let mut last_snapshot_emit = Instant::now();
    let mut snapshot_dirty = false;
    let mut include_tree_in_next_snapshot = true;
    let tree_emit_interval = scan_tree_emit_interval();
    let tree_emit_min_track_delta = scan_tree_emit_min_track_delta();
    let mut last_tree_emit_at: Option<Instant> = None;
    let mut last_tree_emit_track_count = 0usize;

    if send_snapshot_event(&event_tx, &state, include_tree_in_next_snapshot) {
        prof_snapshots_sent += 1;
        include_tree_in_next_snapshot = false;
        last_tree_emit_at = Some(Instant::now());
        last_tree_emit_track_count = state.library.tracks.len();
    } else {
        prof_snapshots_dropped += 1;
    }

    while running {
        select! {
            recv(cmd_rx) -> msg => {
                match msg {
                    Ok(cmd) => {
                        let rebuild_tree =
                            command_requires_tree_rebuild(&cmd, state.settings.library_sort_mode);
                        let force_snapshot = matches!(cmd, BridgeCommand::RequestSnapshot);
                        let mut command_context = BridgeCommandContext {
                            playback: &playback,
                            analysis: &analysis,
                            library: &library,
                            search_query_tx: &search_query_tx,
                            event_tx: &event_tx,
                            running: &mut running,
                            settings_dirty: &mut settings_dirty,
                        };
                        let changed =
                            handle_bridge_command(cmd, &mut state, &mut command_context);
                        if rebuild_tree {
                            state.rebuild_pre_built_tree();
                            include_tree_in_next_snapshot = true;
                        }
                        if changed {
                            snapshot_dirty = true;
                        }
                        if force_snapshot && running {
                            if send_snapshot_event(
                                &event_tx,
                                &state,
                                include_tree_in_next_snapshot,
                            ) {
                                prof_snapshots_sent += 1;
                                if include_tree_in_next_snapshot {
                                    last_tree_emit_at = Some(Instant::now());
                                    last_tree_emit_track_count = state.library.tracks.len();
                                }
                                include_tree_in_next_snapshot = false;
                                snapshot_dirty = false;
                            } else {
                                prof_snapshots_dropped += 1;
                                snapshot_dirty = true;
                            }
                            last_snapshot_emit = Instant::now();
                        }
                    }
                    Err(_) => break,
                }
            }
            recv(ticker) -> _ => {
                if state.playback.state == PlaybackState::Playing {
                    if last_playing_poll.elapsed() >= playing_poll_interval {
                        playback.command(PlaybackCommand::Poll);
                        last_playing_poll = Instant::now();
                    }
                } else if last_idle_poll.elapsed() >= idle_poll_interval {
                    playback.command(PlaybackCommand::Poll);
                    last_idle_poll = Instant::now();
                }
            }
        }

        let playback_changed = pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        let analysis_changed = pump_analysis_events(&analysis_rx, &mut state);
        let metadata_changed = pump_metadata_events(&metadata_rx, &mut state);
        let library_changed = pump_library_events(&library_rx, &mut state);
        let _ = pump_search_results(&search_results_rx, &mut state);
        let _ = flush_pending_search_results_event(&event_tx, &mut state.pending_search_results);
        let changed = playback_changed || analysis_changed || metadata_changed || library_changed;
        if library_changed {
            let now = Instant::now();
            let track_delta = state
                .library
                .tracks
                .len()
                .saturating_sub(last_tree_emit_track_count);
            let scan_emit_due = last_tree_emit_at
                .map_or(true, |last| now.duration_since(last) >= tree_emit_interval);
            let should_emit_tree = !state.library.scan_in_progress
                || last_tree_emit_at.is_none()
                || (scan_emit_due && track_delta >= tree_emit_min_track_delta);
            if should_emit_tree {
                state.rebuild_pre_built_tree();
                include_tree_in_next_snapshot = true;
            }
        }

        if changed {
            snapshot_dirty = true;
        }
        if snapshot_dirty && last_snapshot_emit.elapsed() >= snapshot_interval {
            if send_snapshot_event(&event_tx, &state, include_tree_in_next_snapshot) {
                prof_snapshots_sent += 1;
                if include_tree_in_next_snapshot {
                    last_tree_emit_at = Some(Instant::now());
                    last_tree_emit_track_count = state.library.tracks.len();
                }
                include_tree_in_next_snapshot = false;
                snapshot_dirty = false;
            } else {
                prof_snapshots_dropped += 1;
                snapshot_dirty = true;
            }
            last_snapshot_emit = Instant::now();
        }

        let _ = flush_pending_search_results_event(&event_tx, &mut state.pending_search_results);

        if profile_enabled && profile_last.elapsed() >= Duration::from_secs(1) {
            let rss_kb = current_rss_kb();
            let spectro_rows = state.analysis.spectrogram_rows.len();
            let spectro_bins = state
                .analysis
                .spectrogram_rows
                .first()
                .map_or(0, std::vec::Vec::len);
            eprintln!(
                "[bridge] rss_kb={} playback_q={} analysis_q={} metadata_q={} library_q={} wave_len={} spectro_rows={} spectro_bins={} sent_snap/s={} drop_snap/s={}",
                rss_kb,
                playback_rx.len(),
                analysis_rx.len(),
                metadata_rx.len(),
                library_rx.len(),
                state.analysis.waveform_peaks.len(),
                spectro_rows,
                spectro_bins,
                prof_snapshots_sent,
                prof_snapshots_dropped
            );
            prof_snapshots_sent = 0;
            prof_snapshots_dropped = 0;
            profile_last = Instant::now();
        }

        if settings_dirty && last_settings_save.elapsed() >= Duration::from_secs(2) {
            save_settings(&state.settings);
            settings_dirty = false;
            last_settings_save = Instant::now();
        }

        if last_session_save.elapsed() >= Duration::from_secs(2) {
            let session = session_snapshot_for_state(&state);
            if last_saved_session.as_ref() != Some(&session) {
                save_session_snapshot(&session);
                last_saved_session = Some(session);
            }
            last_session_save = Instant::now();
        }
    }

    save_settings(&state.settings);
    save_session_snapshot(&session_snapshot_for_state(&state));
    let _ = try_send_event(&event_tx, BridgeEvent::Stopped);
}

fn run_search_worker(
    query_rx: Receiver<SearchWorkerQuery>,
    results_tx: Sender<BridgeSearchResultsFrame>,
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

        let query_started = Instant::now();
        let prepared = prepared_cache.prepared_for(&query.library);
        match build_search_results_frame(&query, prepared.as_ref(), &query_rx) {
            SearchBuildOutcome::Frame(frame) => {
                if profile_search {
                    eprintln!(
                        "[search-worker] seq={} chars={} tracks={} rows={} elapsed_ms={}",
                        query.seq,
                        query.query.chars().count(),
                        prepared.tracks.len(),
                        frame.rows.len(),
                        query_started.elapsed().as_millis()
                    );
                }
                let _ = results_tx.send(frame);
            }
            SearchBuildOutcome::Cancelled(next) => {
                if profile_search {
                    eprintln!(
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
) -> bool {
    // Drop stale snapshot updates when the consumer is behind; next snapshot will replace it.
    if event_tx.is_full() {
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

fn search_profile_enabled() -> bool {
    std::env::var_os("FERROUS_SEARCH_PROFILE").is_some()
}

fn search_fallback_limit() -> usize {
    std::env::var("FERROUS_SEARCH_FALLBACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(256, |v| v.clamp(64, 5_000))
}

fn search_artist_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ARTIST_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(32, |v| v.clamp(8, 400))
}

fn search_album_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ALBUM_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(64, |v| v.clamp(8, 800))
}

fn search_track_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_TRACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(128, |v| v.clamp(16, 2_000))
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
        BridgeCommand::Settings(BridgeSettingsCommand::LoadFromDisk) => true,
        BridgeCommand::Library(BridgeLibraryCommand::SetNodeExpanded { .. }) => true,
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
    library: &'a LibraryService,
    search_query_tx: &'a Sender<SearchWorkerQuery>,
    event_tx: &'a Sender<BridgeEvent>,
    running: &'a mut bool,
    settings_dirty: &'a mut bool,
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
            match cmd {
                BridgePlaybackCommand::Play => context.playback.command(PlaybackCommand::Play),
                BridgePlaybackCommand::Pause => context.playback.command(PlaybackCommand::Pause),
                BridgePlaybackCommand::Stop => context.playback.command(PlaybackCommand::Stop),
                BridgePlaybackCommand::Next => context.playback.command(PlaybackCommand::Next),
                BridgePlaybackCommand::Previous => {
                    context.playback.command(PlaybackCommand::Previous)
                }
                BridgePlaybackCommand::Seek(pos) => {
                    context.playback.command(PlaybackCommand::Seek(pos))
                }
                BridgePlaybackCommand::SetVolume(v) => {
                    let v = v.clamp(0.0, 1.0);
                    context.playback.command(PlaybackCommand::SetVolume(v));
                    state.settings.volume = v;
                    *context.settings_dirty = true;
                }
                BridgePlaybackCommand::SetRepeatMode(mode) => {
                    context
                        .playback
                        .command(PlaybackCommand::SetRepeatMode(mode));
                }
                BridgePlaybackCommand::SetShuffle(enabled) => {
                    context
                        .playback
                        .command(PlaybackCommand::SetShuffle(enabled));
                }
            }
            false
        }
        BridgeCommand::Queue(cmd) => {
            handle_queue_command(cmd, state, context.playback, context.event_tx)
        }
        BridgeCommand::Library(cmd) => handle_library_command(
            cmd,
            state,
            context.playback,
            context.library,
            context.search_query_tx,
            context.event_tx,
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
            match cmd {
                BridgeSettingsCommand::LoadFromDisk => {
                    load_settings_into(&mut state.settings);
                    context
                        .playback
                        .command(PlaybackCommand::SetVolume(state.settings.volume));
                    context
                        .analysis
                        .command(AnalysisCommand::SetFftSize(state.settings.fft_size));
                }
                BridgeSettingsCommand::SaveToDisk => {
                    save_settings(&state.settings);
                    *context.settings_dirty = false;
                }
                BridgeSettingsCommand::SetVolume(v) => {
                    let v = v.clamp(0.0, 1.0);
                    state.settings.volume = v;
                    context.playback.command(PlaybackCommand::SetVolume(v));
                    *context.settings_dirty = true;
                }
                BridgeSettingsCommand::SetFftSize(size) => {
                    let fft = size.clamp(512, 8192).next_power_of_two();
                    state.settings.fft_size = fft;
                    context.analysis.command(AnalysisCommand::SetFftSize(fft));
                    *context.settings_dirty = true;
                }
                BridgeSettingsCommand::SetDbRange(v) => {
                    state.settings.db_range = v.clamp(50.0, 120.0);
                    *context.settings_dirty = true;
                }
                BridgeSettingsCommand::SetLogScale(v) => {
                    state.settings.log_scale = v;
                    *context.settings_dirty = true;
                }
                BridgeSettingsCommand::SetShowFps(v) => {
                    state.settings.show_fps = v;
                    *context.settings_dirty = true;
                }
                BridgeSettingsCommand::SetLibrarySortMode(mode) => {
                    state.settings.library_sort_mode = mode;
                    *context.settings_dirty = true;
                }
            }
            true
        }
    }
}

fn handle_queue_command(
    cmd: BridgeQueueCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let outcome = apply_queue_command_state(cmd, &mut state.queue, &mut state.selected_queue_index);
    for op in &outcome.playback_ops {
        match op {
            QueuePlaybackOp::LoadQueue(tracks) => {
                playback.command(PlaybackCommand::LoadQueue(tracks.clone()))
            }
            QueuePlaybackOp::AddToQueue(tracks) => {
                playback.command(PlaybackCommand::AddToQueue(tracks.clone()))
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

fn apply_queue_command_state(
    cmd: BridgeQueueCommand,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    match cmd {
        BridgeQueueCommand::Replace { tracks, autoplay } => {
            *queue = tracks;
            *selected_queue_index = if queue.is_empty() { None } else { Some(0) };
            let mut playback_ops = Vec::new();
            if queue.is_empty() {
                playback_ops.push(QueuePlaybackOp::ClearQueue);
            } else {
                playback_ops.push(QueuePlaybackOp::LoadQueue(queue.clone()));
                if autoplay {
                    playback_ops.push(QueuePlaybackOp::PlayAt(0));
                    playback_ops.push(QueuePlaybackOp::Play);
                }
            }
            QueueCommandOutcome {
                changed: true,
                playback_ops,
                error: None,
            }
        }
        BridgeQueueCommand::Append(tracks) => {
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
        BridgeQueueCommand::PlayAt(idx) => {
            if idx < queue.len() {
                *selected_queue_index = Some(idx);
                QueueCommandOutcome {
                    changed: true,
                    playback_ops: vec![QueuePlaybackOp::PlayAt(idx), QueuePlaybackOp::Play],
                    error: None,
                }
            } else {
                QueueCommandOutcome {
                    changed: false,
                    playback_ops: Vec::new(),
                    error: Some(format!("queue index {idx} out of bounds")),
                }
            }
        }
        BridgeQueueCommand::Remove(idx) => {
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
        BridgeQueueCommand::Move { from, to } => {
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
        BridgeQueueCommand::Select(sel) => {
            *selected_queue_index = sel;
            QueueCommandOutcome {
                changed: true,
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

fn handle_library_command(
    cmd: BridgeLibraryCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    library: &LibraryService,
    search_query_tx: &Sender<SearchWorkerQuery>,
    _event_tx: &Sender<BridgeEvent>,
) -> bool {
    match cmd {
        BridgeLibraryCommand::ScanRoot(path) => {
            library.command(LibraryCommand::ScanRoot(path));
            false
        }
        BridgeLibraryCommand::AddRoot(path) => {
            library.command(LibraryCommand::AddRoot(path));
            false
        }
        BridgeLibraryCommand::RemoveRoot(path) => {
            library.command(LibraryCommand::RemoveRoot(path));
            false
        }
        BridgeLibraryCommand::RescanRoot(path) => {
            library.command(LibraryCommand::RescanRoot(path));
            false
        }
        BridgeLibraryCommand::RescanAll => {
            library.command(LibraryCommand::RescanAll);
            false
        }
        BridgeLibraryCommand::AddTrack(path) => {
            if state.queue.is_empty() {
                state.queue.push(path);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            } else {
                state.queue.push(path.clone());
                playback.command(PlaybackCommand::AddToQueue(vec![path]));
            }
            true
        }
        BridgeLibraryCommand::PlayTrack(path) => {
            state.queue.clear();
            state.queue.push(path.clone());
            state.selected_queue_index = Some(0);
            playback.command(PlaybackCommand::LoadQueue(vec![path]));
            playback.command(PlaybackCommand::Play);
            true
        }
        BridgeLibraryCommand::ReplaceWithAlbum(paths) => {
            if paths.is_empty() {
                return false;
            }
            state.queue = paths;
            state.selected_queue_index = Some(0);
            playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            playback.command(PlaybackCommand::PlayAt(0));
            playback.command(PlaybackCommand::Play);
            true
        }
        BridgeLibraryCommand::AppendAlbum(paths) => {
            if paths.is_empty() {
                return false;
            }
            if state.queue.is_empty() {
                state.queue.extend(paths);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            } else {
                state.queue.extend(paths.clone());
                playback.command(PlaybackCommand::AddToQueue(paths));
            }
            true
        }
        BridgeLibraryCommand::ReplaceAlbumByKey { artist, album } => {
            let paths: Vec<PathBuf> = state
                .library
                .tracks
                .iter()
                .filter(|track| {
                    let track_artist = if track.artist.trim().is_empty() {
                        "Unknown Artist"
                    } else {
                        track.artist.as_str()
                    };
                    let track_album = if track.album.trim().is_empty() {
                        "Unknown Album"
                    } else {
                        track.album.as_str()
                    };
                    track_artist == artist && track_album == album
                })
                .map(|track| track.path.clone())
                .collect();
            if paths.is_empty() {
                return false;
            }
            state.queue = paths;
            state.selected_queue_index = Some(0);
            playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            playback.command(PlaybackCommand::PlayAt(0));
            playback.command(PlaybackCommand::Play);
            true
        }
        BridgeLibraryCommand::AppendAlbumByKey { artist, album } => {
            let paths: Vec<PathBuf> = state
                .library
                .tracks
                .iter()
                .filter(|track| {
                    let track_artist = if track.artist.trim().is_empty() {
                        "Unknown Artist"
                    } else {
                        track.artist.as_str()
                    };
                    let track_album = if track.album.trim().is_empty() {
                        "Unknown Album"
                    } else {
                        track.album.as_str()
                    };
                    track_artist == artist && track_album == album
                })
                .map(|track| track.path.clone())
                .collect();
            if paths.is_empty() {
                return false;
            }
            if state.queue.is_empty() {
                state.queue.extend(paths);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            } else {
                state.queue.extend(paths.clone());
                playback.command(PlaybackCommand::AddToQueue(paths));
            }
            true
        }
        BridgeLibraryCommand::ReplaceArtistByKey { artist } => {
            let paths: Vec<PathBuf> = state
                .library
                .tracks
                .iter()
                .filter(|track| {
                    let track_artist = if track.artist.trim().is_empty() {
                        "Unknown Artist"
                    } else {
                        track.artist.as_str()
                    };
                    track_artist == artist
                })
                .map(|track| track.path.clone())
                .collect();
            if paths.is_empty() {
                return false;
            }
            state.queue = paths;
            state.selected_queue_index = Some(0);
            playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            playback.command(PlaybackCommand::PlayAt(0));
            playback.command(PlaybackCommand::Play);
            true
        }
        BridgeLibraryCommand::AppendArtistByKey { artist } => {
            let paths: Vec<PathBuf> = state
                .library
                .tracks
                .iter()
                .filter(|track| {
                    let track_artist = if track.artist.trim().is_empty() {
                        "Unknown Artist"
                    } else {
                        track.artist.as_str()
                    };
                    track_artist == artist
                })
                .map(|track| track.path.clone())
                .collect();
            if paths.is_empty() {
                return false;
            }
            if state.queue.is_empty() {
                state.queue.extend(paths);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            } else {
                state.queue.extend(paths.clone());
                playback.command(PlaybackCommand::AddToQueue(paths));
            }
            true
        }
        BridgeLibraryCommand::SetNodeExpanded { key, expanded } => {
            let normalized = key.trim();
            if normalized.is_empty() {
                return false;
            }
            if expanded {
                state.expanded_keys.insert(normalized.to_string())
            } else {
                state.expanded_keys.remove(normalized)
            }
        }
        BridgeLibraryCommand::SetSearchQuery { seq, query } => {
            let _ = search_query_tx.send(SearchWorkerQuery {
                seq,
                query: query.trim().to_string(),
                library: Arc::clone(&state.library),
            });
            false
        }
    }
}

#[derive(Debug, Clone)]
struct TreePathContext {
    artist_name: String,
    artist_key: String,
    album_folder: Option<String>,
    album_key: Option<String>,
    section_key: Option<String>,
    album_path: Option<PathBuf>,
    track_key: String,
    is_main_level_album_track: bool,
    is_disc_section_album_track: bool,
}

fn build_search_results_frame(
    query: &SearchWorkerQuery,
    prepared: &PreparedSearchLibrary,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> SearchBuildOutcome {
    #[derive(Default)]
    struct HitAlbumAcc {
        artist_name: String,
        album_title: String,
        artist_key: String,
        year_counts: HashMap<i32, usize>,
        genre_counts: HashMap<String, usize>,
    }

    let seq = query.seq;
    let query_text = query.query.trim();
    if query_text.is_empty() {
        return SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
            seq,
            rows: Vec::new(),
        });
    }
    let query_terms = split_search_terms(query_text);
    if query_terms.is_empty() {
        return SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
            seq,
            rows: Vec::new(),
        });
    }

    // In-memory search is deterministic and responsive while library scans are writing to SQLite.
    // Optional FTS can be enabled explicitly for experimentation.
    let fallback_limit = search_fallback_limit();
    let use_fts = std::env::var_os("FERROUS_SEARCH_USE_FTS").is_some();
    let hits = if use_fts {
        match search_tracks_fts(query_text, fallback_limit) {
            Ok(rows) if !rows.is_empty() => rows,
            Ok(_) | Err(_) => {
                match search_tracks_fallback_prepared(
                    query_text,
                    prepared,
                    fallback_limit,
                    query_rx,
                ) {
                    SearchFallbackOutcome::Hits(rows) => rows,
                    SearchFallbackOutcome::Cancelled(next) => {
                        return SearchBuildOutcome::Cancelled(next)
                    }
                }
            }
        }
    } else {
        match search_tracks_fallback_prepared(query_text, prepared, fallback_limit, query_rx) {
            SearchFallbackOutcome::Hits(rows) => rows,
            SearchFallbackOutcome::Cancelled(next) => return SearchBuildOutcome::Cancelled(next),
        }
    };
    if hits.is_empty() {
        return SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
            seq,
            rows: Vec::new(),
        });
    }

    let roots = prepared.roots.clone();
    if roots.is_empty() {
        return SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
            seq,
            rows: Vec::new(),
        });
    }

    let mut album_cover_paths: HashMap<String, String> = HashMap::new();
    let mut artist_groups: HashMap<String, (f32, String)> = HashMap::new();
    let mut album_groups: HashMap<String, (f32, String)> = HashMap::new();
    let mut album_hit_stats: HashMap<String, HitAlbumAcc> = HashMap::new();
    let mut track_rows = Vec::new();

    for hit in &hits {
        let hit_path_string = hit.path.to_string_lossy().to_string();
        let Some(context) = prepared
            .context_by_path
            .get(&hit_path_string)
            .cloned()
            .or_else(|| derive_tree_path_context(&hit.path, &roots, &hit.artist))
        else {
            continue;
        };
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
        let artist_query_match = query_terms_match_text(&query_terms, &context.artist_name);

        if artist_query_match {
            let artist_entry = artist_groups
                .entry(context.artist_key.clone())
                .or_insert((hit.score, context.artist_name.clone()));
            if hit.score < artist_entry.0 {
                artist_entry.0 = hit.score;
                artist_entry.1 = context.artist_name.clone();
            }
        }

        if let Some(album_key) = album_key.clone() {
            let album_query_match = query_terms_match_text(
                &query_terms,
                &format!("{} {}", context.artist_name, hit_album),
            );
            if album_query_match {
                let album_entry = album_groups
                    .entry(album_key.clone())
                    .or_insert((hit.score, hit_album.clone()));
                if hit.score < album_entry.0 {
                    album_entry.0 = hit.score;
                    album_entry.1 = hit_album.clone();
                }

                let stats_entry = album_hit_stats.entry(album_key).or_default();
                if stats_entry.artist_name.is_empty() {
                    stats_entry.artist_name = context.artist_name.clone();
                }
                if stats_entry.artist_key.is_empty() {
                    stats_entry.artist_key = context.artist_key.clone();
                }
                if stats_entry.album_title.is_empty() {
                    stats_entry.album_title = hit_album.clone();
                }
                if let Some(year) = hit.year {
                    *stats_entry.year_counts.entry(year).or_insert(0) += 1;
                }
                if !hit.genre.trim().is_empty() {
                    *stats_entry
                        .genre_counts
                        .entry(hit.genre.trim().to_string())
                        .or_insert(0) += 1;
                }
            }
        }

        track_rows.push(BridgeSearchResultRow {
            row_type: BridgeSearchResultRowType::Track,
            score: hit.score,
            year: hit.year,
            track_number: hit.track_no,
            count: 0,
            length_seconds: hit.duration_secs,
            label: if hit.title.trim().is_empty() {
                hit.path
                    .file_name()
                    .map_or_else(String::new, |v| v.to_string_lossy().to_string())
            } else {
                hit.title.trim().to_string()
            },
            artist: hit_artist,
            album: hit_album,
            genre: hit.genre.trim().to_string(),
            cover_path: album_key.as_ref().map_or_else(String::new, |key| {
                cached_album_cover_path(key, context.album_path.as_ref(), &mut album_cover_paths)
            }),
            artist_key: context.artist_key.clone(),
            album_key: album_key.unwrap_or_default(),
            section_key: context.section_key.unwrap_or_default(),
            track_key: context.track_key,
            track_path: hit_path_string,
        });
    }

    let mut artist_rows = artist_groups
        .into_iter()
        .map(|(artist_key, (score, artist_name))| BridgeSearchResultRow {
            row_type: BridgeSearchResultRowType::Artist,
            score,
            year: None,
            track_number: None,
            count: 0,
            length_seconds: None,
            label: artist_name.clone(),
            artist: artist_name,
            album: String::new(),
            genre: String::new(),
            cover_path: String::new(),
            artist_key,
            album_key: String::new(),
            section_key: String::new(),
            track_key: String::new(),
            track_path: String::new(),
        })
        .collect::<Vec<_>>();

    let mut album_rows = album_groups
        .into_iter()
        .filter_map(|(album_key, (score, fallback_title))| {
            let stats = album_hit_stats.get(&album_key)?;
            let inventory = prepared.album_inventory.get(&album_key);
            let year = choose_most_common_year(&stats.year_counts);
            let genre = choose_most_common_genre(&stats.genre_counts);
            Some(BridgeSearchResultRow {
                row_type: BridgeSearchResultRowType::Album,
                score,
                year,
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
                genre,
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
    artist_rows.truncate(search_artist_row_limit());
    album_rows.truncate(search_album_row_limit());
    track_rows.truncate(search_track_row_limit());

    let mut rows = Vec::with_capacity(artist_rows.len() + album_rows.len() + track_rows.len());
    rows.extend(artist_rows);
    rows.extend(album_rows);
    rows.extend(track_rows);
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, rows })
}

fn pump_search_results(
    search_rx: &Receiver<BridgeSearchResultsFrame>,
    state: &mut BridgeState,
) -> bool {
    let mut latest = None;
    while let Ok(frame) = search_rx.try_recv() {
        latest = Some(frame);
    }

    if let Some(frame) = latest {
        state.pending_search_results = Some(frame);
        return true;
    }
    false
}

fn poll_latest_search_query(query_rx: &Receiver<SearchWorkerQuery>) -> Option<SearchWorkerQuery> {
    let mut latest = None;
    while let Ok(next) = query_rx.try_recv() {
        latest = Some(next);
    }
    latest
}

fn prepare_search_library(library: &LibrarySnapshot) -> PreparedSearchLibrary {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return PreparedSearchLibrary::default();
    }

    let mut tracks = Vec::with_capacity(library.tracks.len());
    let mut context_by_path: HashMap<String, TreePathContext> =
        HashMap::with_capacity(library.tracks.len());
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

        if let Some(context) = derive_tree_path_context(&track.path, &roots, &artist) {
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
            context_by_path.insert(path_string.clone(), context);
        }

        tracks.push(PreparedSearchTrack {
            path: track.path.clone(),
            path_string,
            path_lower,
            title,
            artist,
            album,
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
        roots,
        tracks,
        context_by_path,
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
        score += (track.path_string.len() as f32) / 10_000.0;

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
            title: track.title.clone(),
            artist: track.artist.clone(),
            album: track.album.clone(),
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

fn find_cover_path_for_album(album_path: &PathBuf) -> Option<String> {
    let Ok(read_dir) = std::fs::read_dir(album_path) else {
        return None;
    };
    let mut candidates = Vec::new();
    for entry in read_dir.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(ext) = path.extension().and_then(|v| v.to_str()) else {
            continue;
        };
        let ext = ext.to_ascii_lowercase();
        if ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "webp" || ext == "bmp" {
            candidates.push(path.to_string_lossy().to_string());
        }
    }
    if candidates.is_empty() {
        return None;
    }
    candidates.sort_unstable();
    candidates.into_iter().next()
}

fn cached_album_cover_path(
    album_key: &str,
    album_path: Option<&PathBuf>,
    cache: &mut HashMap<String, String>,
) -> String {
    if let Some(existing) = cache.get(album_key) {
        return existing.clone();
    }
    let resolved = album_path
        .and_then(find_cover_path_for_album)
        .unwrap_or_default();
    cache.insert(album_key.to_string(), resolved.clone());
    resolved
}

fn is_main_album_disc_section(section_name: &str) -> bool {
    let section = section_name.trim().to_ascii_lowercase();
    if section.is_empty() {
        return false;
    }
    for prefix in ["cd", "disc", "disk"] {
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

fn pick_root_for_path<'a>(roots: &'a [PathBuf], path: &PathBuf) -> Option<&'a PathBuf> {
    roots
        .iter()
        .filter(|root| path.starts_with(root))
        .max_by_key(|root| root.components().count())
}

fn derive_tree_path_context(
    path: &PathBuf,
    roots: &[PathBuf],
    fallback_artist: &str,
) -> Option<TreePathContext> {
    let root = pick_root_for_path(roots, path)?;
    let rel = path.strip_prefix(root).ok()?;
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

    let root_key = root.to_string_lossy().to_string();
    let artist_name = if components.len() >= 2 {
        components[0].clone()
    } else if fallback_artist.trim().is_empty() {
        String::from("Unknown Artist")
    } else {
        fallback_artist.trim().to_string()
    };
    let artist_key = format!("artist|{root_key}|{artist_name}");
    let track_path = path.to_string_lossy().to_string();
    let track_key = format!("track|{track_path}");

    if components.len() <= 2 {
        return Some(TreePathContext {
            artist_name,
            artist_key,
            album_folder: None,
            album_key: None,
            section_key: None,
            album_path: None,
            track_key,
            is_main_level_album_track: false,
            is_disc_section_album_track: false,
        });
    }

    let album_folder = components[1].clone();
    let album_key = format!("album|{root_key}|{artist_name}|{album_folder}");
    let section_key = if components.len() >= 4 {
        Some(format!(
            "section|{root_key}|{artist_name}|{album_folder}|{}",
            components[2]
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
        album_folder: Some(album_folder.clone()),
        album_key: Some(album_key),
        section_key,
        album_path: Some(root.join(&artist_name).join(album_folder)),
        track_key,
        is_main_level_album_track,
        is_disc_section_album_track,
    })
}

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
        match event {
            PlaybackEvent::Snapshot(snapshot) => {
                if state.playback != snapshot {
                    state.playback = snapshot;
                    changed = true;
                }
            }
            PlaybackEvent::TrackChanged {
                path,
                queue_index,
                kind,
            } => {
                state.playback.current_queue_index = Some(queue_index);
                state.analysis.waveform_peaks.clear();
                metadata.request(path.clone());
                analysis.command(AnalysisCommand::SetTrack {
                    path,
                    reset_spectrogram: matches!(kind, TrackChangeKind::Manual),
                });
                changed = true;
            }
            PlaybackEvent::Seeked => {}
        }
    }
    changed
}

fn pump_analysis_events(analysis_rx: &Receiver<AnalysisEvent>, state: &mut BridgeState) -> bool {
    let mut changed = false;
    for _ in 0..8 {
        let Ok(event) = analysis_rx.try_recv() else {
            break;
        };
        match event {
            AnalysisEvent::Snapshot(snapshot) => {
                if snapshot.spectrogram_seq == 0 && snapshot.spectrogram_rows.is_empty() {
                    state.analysis.spectrogram_rows.clear();
                } else if !snapshot.spectrogram_rows.is_empty() {
                    state.analysis.spectrogram_rows = snapshot.spectrogram_rows;
                }
                state.analysis.spectrogram_seq = snapshot.spectrogram_seq;
                state.analysis.sample_rate_hz = snapshot.sample_rate_hz;
                if !snapshot.waveform_peaks.is_empty() {
                    state.analysis.waveform_peaks = snapshot.waveform_peaks;
                }
                changed = true;
            }
        }
    }
    changed
}

fn pump_metadata_events(metadata_rx: &Receiver<MetadataEvent>, state: &mut BridgeState) -> bool {
    let mut changed = false;
    for _ in 0..4 {
        let Ok(event) = metadata_rx.try_recv() else {
            break;
        };
        match event {
            MetadataEvent::Loaded(metadata) => {
                state.metadata = metadata;
                changed = true;
            }
        }
    }
    changed
}

fn pump_library_events(library_rx: &Receiver<LibraryEvent>, state: &mut BridgeState) -> bool {
    let mut latest_snapshot: Option<LibrarySnapshot> = None;
    while let Ok(event) = library_rx.try_recv() {
        match event {
            LibraryEvent::Snapshot(snapshot) => {
                latest_snapshot = Some(snapshot);
            }
        }
    }
    if let Some(snapshot) = latest_snapshot {
        let (artist_count, album_count) = library_tree::compute_artist_album_counts(&snapshot);
        state.library = Arc::new(snapshot);
        state.library_artist_count = artist_count;
        state.library_album_count = album_count;
        return true;
    }
    false
}

fn config_base_path() -> Option<PathBuf> {
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
    let current_queue_index = state
        .playback
        .current_queue_index
        .filter(|idx| *idx < state.queue.len());
    SessionSnapshot {
        queue: state.queue.clone(),
        selected_queue_index: state.selected_queue_index,
        current_queue_index,
    }
}

fn apply_session_restore(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    session: Option<&SessionSnapshot>,
) {
    let Some(session) = session else {
        return;
    };
    state.queue = session.queue.clone();
    state.selected_queue_index = session
        .selected_queue_index
        .filter(|idx| *idx < state.queue.len());
    if state.queue.is_empty() {
        return;
    }
    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    if let Some(idx) = session
        .current_queue_index
        .filter(|idx| *idx < state.queue.len())
    {
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
        .map(|v| v as usize);
    let current_queue_index = value
        .get("current_queue_index")
        .and_then(serde_json::Value::as_u64)
        .map(|v| v as usize);
    Some(SessionSnapshot {
        queue,
        selected_queue_index,
        current_queue_index,
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
    let _ = fs::write(path, text);
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
            "db_range" => {
                if let Ok(x) = value.parse::<f32>() {
                    settings.db_range = x.clamp(50.0, 120.0);
                }
            }
            "log_scale" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.log_scale = x != 0;
                }
            }
            "show_fps" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.show_fps = x != 0;
                }
            }
            "library_sort_mode" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.library_sort_mode = LibrarySortMode::from_i32(x);
                }
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
        "volume={:.4}\nfft_size={}\ndb_range={:.2}\nlog_scale={}\nshow_fps={}\nlibrary_sort_mode={}\n",
        settings.volume,
        settings.fft_size,
        settings.db_range,
        i32::from(settings.log_scale),
        i32::from(settings.show_fps),
        settings.library_sort_mode.to_i32(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, MutexGuard, OnceLock};
    use std::time::Instant;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    #[test]
    fn disc_section_detection_accepts_common_main_disc_names() {
        assert!(is_main_album_disc_section("CD1"));
        assert!(is_main_album_disc_section("CD 2"));
        assert!(is_main_album_disc_section("disc-03"));
        assert!(is_main_album_disc_section("Disk 4 (bonus)"));
        assert!(!is_main_album_disc_section("Live"));
        assert!(!is_main_album_disc_section("discography"));
    }

    #[test]
    fn prepare_search_library_counts_main_album_tracks_with_cd_sections() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![root.clone()],
            tracks: vec![
                crate::library::LibraryTrack {
                    path: p("/music/Artist/Album/01 - Intro.flac"),
                    root_path: root.clone(),
                    title: "Intro".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
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
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(3),
                    duration_secs: Some(80.0),
                },
            ],
            scan_in_progress: false,
            scan_progress: None,
            last_error: None,
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
            roots: vec![root.clone()],
            tracks: vec![crate::library::LibraryTrack {
                path: p("/music/Artist/Album/01 - Song.flac"),
                root_path: root,
                title: "Song".to_string(),
                artist: "Artist".to_string(),
                album: "Album".to_string(),
                genre: String::new(),
                year: Some(2020),
                track_no: Some(1),
                duration_secs: Some(60.0),
            }],
            scan_in_progress: false,
            scan_progress: None,
            last_error: None,
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
            db_range: 77.5,
            log_scale: true,
            show_fps: true,
            library_sort_mode: LibrarySortMode::Title,
        };
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!((parsed.volume - 0.42).abs() < 0.0001);
        assert_eq!(parsed.fft_size, 2048);
        assert!((parsed.db_range - 77.5).abs() < 0.0001);
        assert!(parsed.log_scale);
        assert!(parsed.show_fps);
        assert_eq!(parsed.library_sort_mode, LibrarySortMode::Title);
    }

    #[test]
    fn settings_parse_clamps_invalid_ranges() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=2.5\nfft_size=111\ndb_range=500\nlog_scale=0\nshow_fps=1\nlibrary_sort_mode=0\n",
        );
        assert_eq!(settings.volume, 1.0);
        assert_eq!(settings.fft_size, 512);
        assert_eq!(settings.db_range, 120.0);
        assert!(!settings.log_scale);
        assert!(settings.show_fps);
        assert_eq!(settings.library_sort_mode, LibrarySortMode::Year);
    }

    #[test]
    fn session_roundtrip_text_format() {
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), p("/b.flac")],
            selected_queue_index: Some(1),
            current_queue_index: Some(0),
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
    fn queue_append_into_empty_loads_full_queue() {
        let mut queue = Vec::new();
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(vec![p("/a.flac"), p("/b.flac")]),
            &mut queue,
            &mut selected,
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
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::PlayAt(3), &mut queue, &mut selected);
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
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(0), &mut queue, &mut selected);
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
    }

    #[test]
    fn queue_remove_middle_track_uses_remove_op_and_keeps_reasonable_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(2);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(1), &mut queue, &mut selected);
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::RemoveAt(1)]);
    }

    #[test]
    fn queue_remove_out_of_bounds_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(3), &mut queue, &mut selected);
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
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_clear_empties_state_and_emits_clear_queue_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Clear, &mut queue, &mut selected);
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
        assert!(outcome.error.is_none());
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
            })
            .expect("send track-changed event");
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
}
