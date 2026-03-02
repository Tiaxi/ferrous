use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, select, tick, unbounded, Receiver, Sender, TrySendError};

use crate::analysis::{AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::library::{LibraryCommand, LibraryEvent, LibraryService, LibrarySnapshot};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{
    PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot, TrackChangeKind,
};

pub mod ffi;

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
    AddTrack(PathBuf),
    PlayTrack(PathBuf),
    ReplaceWithAlbum(Vec<PathBuf>),
    AppendAlbum(Vec<PathBuf>),
    ReplaceAlbumByKey { artist: String, album: String },
    AppendAlbumByKey { artist: String, album: String },
    ReplaceArtistByKey { artist: String },
    AppendArtistByKey { artist: String },
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
}

#[derive(Debug, Clone)]
pub enum BridgeEvent {
    Snapshot(Box<BridgeSnapshot>),
    Error(String),
    Stopped,
}

#[derive(Debug, Clone)]
pub struct BridgeSnapshot {
    pub playback: PlaybackSnapshot,
    pub analysis: AnalysisSnapshot,
    pub metadata: TrackMetadata,
    pub library: Arc<LibrarySnapshot>,
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
}

impl Default for BridgeSettings {
    fn default() -> Self {
        Self {
            volume: 1.0,
            fft_size: 8192,
            db_range: 90.0,
            log_scale: false,
        }
    }
}

#[derive(Debug, Clone, Default)]
struct BridgeState {
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    library: Arc<LibrarySnapshot>,
    queue: Vec<PathBuf>,
    selected_queue_index: Option<usize>,
    settings: BridgeSettings,
}

impl BridgeState {
    fn snapshot(&self) -> BridgeSnapshot {
        BridgeSnapshot {
            playback: self.playback.clone(),
            analysis: self.analysis.clone(),
            metadata: self.metadata.clone(),
            library: self.library.clone(),
            queue: self.queue.clone(),
            selected_queue_index: self.selected_queue_index,
            settings: self.settings.clone(),
        }
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

        std::thread::spawn(move || run_bridge_loop(cmd_rx, event_tx, options));
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

    let mut state = BridgeState::default();
    load_settings_into(&mut state.settings);
    state.playback.volume = state.settings.volume;
    playback.command(PlaybackCommand::SetVolume(state.settings.volume));
    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));

    let mut running = true;
    let mut settings_dirty = false;
    let mut last_settings_save = Instant::now();
    let ticker = tick(Duration::from_millis(16));
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

    if send_snapshot_event(&event_tx, &state) {
        prof_snapshots_sent += 1;
    } else {
        prof_snapshots_dropped += 1;
    }

    while running {
        select! {
            recv(cmd_rx) -> msg => {
                match msg {
                    Ok(cmd) => {
                        let force_snapshot = matches!(cmd, BridgeCommand::RequestSnapshot);
                        let mut command_context = BridgeCommandContext {
                            playback: &playback,
                            analysis: &analysis,
                            library: &library,
                            event_tx: &event_tx,
                            running: &mut running,
                            settings_dirty: &mut settings_dirty,
                        };
                        let changed =
                            handle_bridge_command(cmd, &mut state, &mut command_context);
                        if changed {
                            snapshot_dirty = true;
                        }
                        if force_snapshot && running {
                            if send_snapshot_event(&event_tx, &state) {
                                prof_snapshots_sent += 1;
                            } else {
                                prof_snapshots_dropped += 1;
                            }
                            last_snapshot_emit = Instant::now();
                            snapshot_dirty = false;
                        }
                    }
                    Err(_) => break,
                }
            }
            recv(ticker) -> _ => {
                playback.command(PlaybackCommand::Poll);
            }
        }

        let mut changed = false;
        changed |= pump_playback_events(&playback_rx, &analysis, &metadata, &mut state);
        changed |= pump_analysis_events(&analysis_rx, &mut state);
        changed |= pump_metadata_events(&metadata_rx, &mut state);
        changed |= pump_library_events(&library_rx, &mut state);

        if changed {
            snapshot_dirty = true;
        }
        if snapshot_dirty && last_snapshot_emit.elapsed() >= snapshot_interval {
            if send_snapshot_event(&event_tx, &state) {
                prof_snapshots_sent += 1;
            } else {
                prof_snapshots_dropped += 1;
            }
            snapshot_dirty = false;
            last_snapshot_emit = Instant::now();
        }

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
    }

    save_settings(&state.settings);
    let _ = try_send_event(&event_tx, BridgeEvent::Stopped);
}

fn try_send_event(
    event_tx: &Sender<BridgeEvent>,
    event: BridgeEvent,
) -> Result<(), TrySendError<BridgeEvent>> {
    event_tx.try_send(event)
}

fn send_snapshot_event(event_tx: &Sender<BridgeEvent>, state: &BridgeState) -> bool {
    // Drop stale snapshot updates when the consumer is behind; next snapshot will replace it.
    if event_tx.is_full() {
        return false;
    }
    try_send_event(event_tx, BridgeEvent::Snapshot(Box::new(state.snapshot()))).is_ok()
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
            }
            false
        }
        BridgeCommand::Queue(cmd) => {
            handle_queue_command(cmd, state, context.playback, context.event_tx)
        }
        BridgeCommand::Library(cmd) => {
            handle_library_command(cmd, state, context.playback, context.library)
        }
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
                *selected_queue_index = idx.checked_sub(1);
                vec![QueuePlaybackOp::LoadQueue(queue.clone())]
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
            *selected_queue_index = Some(to);
            QueueCommandOutcome {
                changed: true,
                playback_ops: vec![QueuePlaybackOp::LoadQueue(queue.clone())],
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
) -> bool {
    match cmd {
        BridgeLibraryCommand::ScanRoot(path) => {
            library.command(LibraryCommand::ScanRoot(path));
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
    }
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
            PlaybackEvent::TrackChanged { path, kind } => {
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
    let mut changed = false;
    for _ in 0..4 {
        let Ok(event) = library_rx.try_recv() else {
            break;
        };
        match event {
            LibraryEvent::Snapshot(snapshot) => {
                state.library = Arc::new(snapshot);
                changed = true;
            }
        }
    }
    changed
}

fn settings_path() -> Option<PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME")
                .map(PathBuf::from)
                .map(|h| h.join(".config"))
        })?;
    Some(base.join("ferrous").join("settings.txt"))
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
        match k {
            "volume" => {
                if let Ok(x) = v.parse::<f32>() {
                    settings.volume = x.clamp(0.0, 1.0);
                }
            }
            "fft_size" => {
                if let Ok(x) = v.parse::<usize>() {
                    settings.fft_size = x.clamp(512, 8192).next_power_of_two();
                }
            }
            "db_range" => {
                if let Ok(x) = v.parse::<f32>() {
                    settings.db_range = x.clamp(50.0, 120.0);
                }
            }
            "log_scale" => {
                if let Ok(x) = v.parse::<i32>() {
                    settings.log_scale = x != 0;
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
        "volume={:.4}\nfft_size={}\ndb_range={:.2}\nlog_scale={}\n",
        settings.volume,
        settings.fft_size,
        settings.db_range,
        i32::from(settings.log_scale),
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
        };
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!((parsed.volume - 0.42).abs() < 0.0001);
        assert_eq!(parsed.fft_size, 2048);
        assert!((parsed.db_range - 77.5).abs() < 0.0001);
        assert!(parsed.log_scale);
    }

    #[test]
    fn settings_parse_clamps_invalid_ranges() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=2.5\nfft_size=111\ndb_range=500\nlog_scale=0\n",
        );
        assert_eq!(settings.volume, 1.0);
        assert_eq!(settings.fft_size, 512);
        assert_eq!(settings.db_range, 120.0);
        assert!(!settings.log_scale);
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
    fn queue_move_updates_selection_and_reloads() {
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
            vec![QueuePlaybackOp::LoadQueue(vec![
                p("/b.flac"),
                p("/c.flac"),
                p("/a.flac")
            ])]
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
                && s.playback.current.as_ref() == Some(&first)
        })
        .expect("snapshot after removing selected track");
        assert_eq!(removed.playback.current.as_ref(), Some(&first));
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
