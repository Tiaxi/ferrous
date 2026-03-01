use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, select, tick, unbounded, Receiver, Sender, TrySendError};

use crate::analysis::{AnalysisCommand, AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::library::{LibraryCommand, LibraryEvent, LibraryService, LibrarySnapshot};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot};

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
    Snapshot(BridgeSnapshot),
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

impl FrontendBridgeHandle {
    pub fn spawn() -> Self {
        let (cmd_tx, cmd_rx) = unbounded::<BridgeCommand>();
        // Keep snapshot/event queue bounded so a slow UI consumer cannot cause unbounded RAM growth.
        let (event_tx, event_rx) = bounded::<BridgeEvent>(64);

        std::thread::spawn(move || run_bridge_loop(cmd_rx, event_tx));
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

fn run_bridge_loop(cmd_rx: Receiver<BridgeCommand>, event_tx: Sender<BridgeEvent>) {
    let (analysis, analysis_rx) = AnalysisEngine::new();
    let (playback, playback_rx) = PlaybackEngine::new(analysis.sender(), analysis.pcm_sender());
    let (metadata, metadata_rx) = MetadataService::new();
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

    send_snapshot_event(&event_tx, &state);

    while running {
        select! {
            recv(cmd_rx) -> msg => {
                match msg {
                    Ok(cmd) => {
                        let changed = handle_bridge_command(
                            cmd,
                            &mut state,
                            &playback,
                            &analysis,
                            &library,
                            &event_tx,
                            &mut running,
                            &mut settings_dirty,
                        );
                        if changed {
                            send_snapshot_event(&event_tx, &state);
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
            send_snapshot_event(&event_tx, &state);
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

fn send_snapshot_event(event_tx: &Sender<BridgeEvent>, state: &BridgeState) {
    // Drop stale snapshot updates when the consumer is behind; next snapshot will replace it.
    if event_tx.is_full() {
        return;
    }
    let _ = try_send_event(event_tx, BridgeEvent::Snapshot(state.snapshot()));
}

#[allow(clippy::too_many_arguments)]
fn handle_bridge_command(
    cmd: BridgeCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    analysis: &AnalysisEngine,
    library: &LibraryService,
    event_tx: &Sender<BridgeEvent>,
    running: &mut bool,
    settings_dirty: &mut bool,
) -> bool {
    match cmd {
        BridgeCommand::RequestSnapshot => true,
        BridgeCommand::Shutdown => {
            *running = false;
            false
        }
        BridgeCommand::Playback(cmd) => {
            match cmd {
                BridgePlaybackCommand::Play => playback.command(PlaybackCommand::Play),
                BridgePlaybackCommand::Pause => playback.command(PlaybackCommand::Pause),
                BridgePlaybackCommand::Stop => playback.command(PlaybackCommand::Stop),
                BridgePlaybackCommand::Next => playback.command(PlaybackCommand::Next),
                BridgePlaybackCommand::Previous => playback.command(PlaybackCommand::Previous),
                BridgePlaybackCommand::Seek(pos) => playback.command(PlaybackCommand::Seek(pos)),
                BridgePlaybackCommand::SetVolume(v) => {
                    let v = v.clamp(0.0, 1.0);
                    playback.command(PlaybackCommand::SetVolume(v));
                    state.settings.volume = v;
                    *settings_dirty = true;
                }
            }
            false
        }
        BridgeCommand::Queue(cmd) => handle_queue_command(cmd, state, playback, event_tx),
        BridgeCommand::Library(cmd) => handle_library_command(cmd, state, playback, library),
        BridgeCommand::Analysis(cmd) => match cmd {
            BridgeAnalysisCommand::SetFftSize(size) => {
                let fft = size.clamp(512, 8192).next_power_of_two();
                state.settings.fft_size = fft;
                *settings_dirty = true;
                analysis.command(AnalysisCommand::SetFftSize(fft));
                true
            }
        },
        BridgeCommand::Settings(cmd) => {
            match cmd {
                BridgeSettingsCommand::LoadFromDisk => {
                    load_settings_into(&mut state.settings);
                    playback.command(PlaybackCommand::SetVolume(state.settings.volume));
                    analysis.command(AnalysisCommand::SetFftSize(state.settings.fft_size));
                }
                BridgeSettingsCommand::SaveToDisk => {
                    save_settings(&state.settings);
                    *settings_dirty = false;
                }
                BridgeSettingsCommand::SetVolume(v) => {
                    let v = v.clamp(0.0, 1.0);
                    state.settings.volume = v;
                    playback.command(PlaybackCommand::SetVolume(v));
                    *settings_dirty = true;
                }
                BridgeSettingsCommand::SetFftSize(size) => {
                    let fft = size.clamp(512, 8192).next_power_of_two();
                    state.settings.fft_size = fft;
                    analysis.command(AnalysisCommand::SetFftSize(fft));
                    *settings_dirty = true;
                }
                BridgeSettingsCommand::SetDbRange(v) => {
                    state.settings.db_range = v.clamp(50.0, 120.0);
                    *settings_dirty = true;
                }
                BridgeSettingsCommand::SetLogScale(v) => {
                    state.settings.log_scale = v;
                    *settings_dirty = true;
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
    match cmd {
        BridgeQueueCommand::Replace { tracks, autoplay } => {
            state.queue = tracks;
            state.selected_queue_index = if state.queue.is_empty() {
                None
            } else {
                Some(0)
            };
            if state.queue.is_empty() {
                playback.command(PlaybackCommand::ClearQueue);
            } else {
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
                if autoplay {
                    playback.command(PlaybackCommand::PlayAt(0));
                    playback.command(PlaybackCommand::Play);
                }
            }
            true
        }
        BridgeQueueCommand::Append(tracks) => {
            if tracks.is_empty() {
                return false;
            }
            if state.queue.is_empty() {
                state.queue.extend(tracks);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
            } else {
                state.queue.extend(tracks.clone());
                playback.command(PlaybackCommand::AddToQueue(tracks));
            }
            true
        }
        BridgeQueueCommand::PlayAt(idx) => {
            if idx < state.queue.len() {
                playback.command(PlaybackCommand::PlayAt(idx));
                playback.command(PlaybackCommand::Play);
                state.selected_queue_index = Some(idx);
                true
            } else {
                let _ = try_send_event(
                    event_tx,
                    BridgeEvent::Error(format!("queue index {idx} out of bounds")),
                );
                false
            }
        }
        BridgeQueueCommand::Remove(idx) => {
            if idx < state.queue.len() {
                state.queue.remove(idx);
                if state.queue.is_empty() {
                    playback.command(PlaybackCommand::ClearQueue);
                    state.selected_queue_index = None;
                } else {
                    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
                    state.selected_queue_index = idx.checked_sub(1);
                }
                true
            } else {
                false
            }
        }
        BridgeQueueCommand::Move { from, to } => {
            if from < state.queue.len() && to < state.queue.len() && from != to {
                let item = state.queue.remove(from);
                state.queue.insert(to, item);
                playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
                state.selected_queue_index = Some(to);
                true
            } else {
                false
            }
        }
        BridgeQueueCommand::Select(sel) => {
            state.selected_queue_index = sel;
            true
        }
        BridgeQueueCommand::Clear => {
            state.queue.clear();
            state.selected_queue_index = None;
            playback.command(PlaybackCommand::ClearQueue);
            true
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
                state.playback = snapshot;
                changed = true;
            }
            PlaybackEvent::TrackChanged(path) => {
                state.analysis.waveform_peaks.clear();
                metadata.request(path.clone());
                analysis.command(AnalysisCommand::SetTrack(path));
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
    let text = format!(
        "volume={:.4}\nfft_size={}\ndb_range={:.2}\nlog_scale={}\n",
        settings.volume,
        settings.fft_size,
        settings.db_range,
        if settings.log_scale { 1 } else { 0 },
    );
    let _ = fs::write(path, text);
}
