use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use crossbeam_channel::Receiver;
use eframe::egui;

use crate::analysis::{AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::library::{LibraryCommand, LibraryEvent, LibraryService, LibrarySnapshot};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot};
use crate::ui::panels::{
    draw_center_panel, draw_footer_panel, draw_top_panel, CenterPanelAction, CoverArtCache,
    LibraryArtCache, SpectrogramCache, SpectrogramUiSettings, TopPanelAction,
};

pub struct FerrousApp {
    playback: PlaybackEngine,
    analysis: AnalysisEngine,
    metadata: MetadataService,
    library: LibraryService,
    playback_rx: Receiver<PlaybackEvent>,
    analysis_rx: Receiver<AnalysisEvent>,
    metadata_rx: Receiver<MetadataEvent>,
    library_rx: Receiver<LibraryEvent>,
    state: AppState,
    last_tick: Instant,
    profile_enabled: bool,
    profile_last: Instant,
    profile_frames: u32,
    last_settings_save: Instant,
}

#[derive(Default)]
struct AppState {
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    library: LibrarySnapshot,
    library_query: String,
    selected_library_root: Option<PathBuf>,
    selected_library_track: Option<PathBuf>,
    expanded_library_groups: HashMap<String, bool>,
    queue: Vec<PathBuf>,
    selected_queue_index: Option<usize>,
    cover_art_cache: CoverArtCache,
    library_art_cache: LibraryArtCache,
    spectro_ui: SpectrogramUiSettings,
    spectrogram_cache: SpectrogramCache,
}

impl FerrousApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        cc.egui_ctx.set_visuals(egui::Visuals::light());

        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (playback, playback_rx) = PlaybackEngine::new(analysis.sender(), analysis.pcm_sender());
        let (metadata, metadata_rx) = MetadataService::new();
        let (library, library_rx) = LibraryService::new();

        let mut app = Self {
            playback,
            analysis,
            metadata,
            library,
            playback_rx,
            analysis_rx,
            metadata_rx,
            library_rx,
            state: AppState {
                playback: PlaybackSnapshot {
                    volume: 1.0,
                    ..PlaybackSnapshot::default()
                },
                ..AppState::default()
            },
            last_tick: Instant::now(),
            profile_enabled: std::env::var_os("FERROUS_PROFILE").is_some(),
            profile_last: Instant::now(),
            profile_frames: 0,
            last_settings_save: Instant::now(),
        };
        app.load_settings();
        app.playback
            .command(PlaybackCommand::SetVolume(app.state.playback.volume));
        app.analysis
            .command(crate::analysis::AnalysisCommand::SetFftSize(
                app.state.spectro_ui.fft_size,
            ));
        app
    }

    fn pump_events(&mut self) {
        for _ in 0..192 {
            let Ok(event) = self.playback_rx.try_recv() else {
                break;
            };
            match event {
                PlaybackEvent::Snapshot(snapshot) => self.state.playback = snapshot,
                PlaybackEvent::TrackChanged(path) => {
                    self.state.spectrogram_cache = SpectrogramCache::default();
                    // New track: clear old precomputed waveform until new one arrives.
                    self.state.analysis.waveform_peaks.clear();
                    self.metadata.request(path.clone());
                    self.analysis
                        .command(crate::analysis::AnalysisCommand::SetTrack(path));
                }
                PlaybackEvent::Seeked => {
                    // Keep existing spectrogram history visible across seeks.
                }
            }
        }

        for _ in 0..8 {
            let Ok(event) = self.analysis_rx.try_recv() else {
                break;
            };
            match event {
                AnalysisEvent::Snapshot(snapshot) => {
                    if snapshot.spectrogram_seq == 0 && snapshot.spectrogram_rows.is_empty() {
                        self.state.analysis.spectrogram_rows.clear();
                    } else if !snapshot.spectrogram_rows.is_empty() {
                        self.state.analysis.spectrogram_rows = snapshot.spectrogram_rows;
                    }

                    self.state.analysis.spectrogram_seq = snapshot.spectrogram_seq;
                    self.state.analysis.sample_rate_hz = snapshot.sample_rate_hz;
                    if !snapshot.waveform_peaks.is_empty() {
                        self.state.analysis.waveform_peaks = snapshot.waveform_peaks;
                    }
                }
            }
        }

        for _ in 0..4 {
            let Ok(event) = self.metadata_rx.try_recv() else {
                break;
            };
            match event {
                MetadataEvent::Loaded(metadata) => self.state.metadata = metadata,
            }
        }

        for _ in 0..4 {
            let Ok(event) = self.library_rx.try_recv() else {
                break;
            };
            match event {
                LibraryEvent::Snapshot(snapshot) => {
                    self.state.library = snapshot;

                    if let Some(root) = self.state.selected_library_root.as_ref() {
                        let still_exists = self.state.library.roots.iter().any(|r| r == root);
                        if !still_exists {
                            self.state.selected_library_root = None;
                        }
                    }

                    if let Some(track_path) = self.state.selected_library_track.as_ref() {
                        let still_exists = self
                            .state
                            .library
                            .tracks
                            .iter()
                            .any(|t| &t.path == track_path);
                        if !still_exists {
                            self.state.selected_library_track = None;
                        }
                    }
                }
            }
        }
    }

    fn tick(&mut self) {
        // Pull fresh playback position.
        self.playback.command(PlaybackCommand::Poll);
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

    fn load_settings(&mut self) {
        let Some(path) = Self::settings_path() else {
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
                        self.state.playback.volume = x.clamp(0.0, 1.0);
                    }
                }
                "fft_size" => {
                    if let Ok(x) = v.parse::<usize>() {
                        let _ = x;
                        self.state.spectro_ui.fft_size = 8192;
                    }
                }
                "db_range" => {
                    if let Ok(x) = v.parse::<f32>() {
                        self.state.spectro_ui.db_range = x.clamp(50.0, 120.0);
                    }
                }
                "log_scale" => {
                    if let Ok(x) = v.parse::<i32>() {
                        self.state.spectro_ui.log_scale = x != 0;
                    }
                }
                _ => {}
            }
        }
    }

    fn save_settings(&mut self) {
        let Some(path) = Self::settings_path() else {
            return;
        };
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let text = format!(
            "volume={:.4}\nfft_size={}\ndb_range={:.2}\nlog_scale={}\n",
            self.state.playback.volume,
            self.state.spectro_ui.fft_size,
            self.state.spectro_ui.db_range,
            if self.state.spectro_ui.log_scale {
                1
            } else {
                0
            },
        );
        let _ = fs::write(path, text);
        self.last_settings_save = Instant::now();
    }
}

impl eframe::App for FerrousApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.profile_frames = self.profile_frames.saturating_add(1);
        if self.profile_enabled && self.profile_last.elapsed() >= Duration::from_secs(1) {
            eprintln!(
                "[ui] fps={} queue={} spectro_seq={}",
                self.profile_frames,
                self.state.queue.len(),
                self.state.analysis.spectrogram_seq
            );
            self.profile_last = Instant::now();
            self.profile_frames = 0;
        }

        self.pump_events();

        let action = draw_top_panel(ctx, &self.state.playback, &self.state.analysis);

        match action {
            TopPanelAction::None => {}
            TopPanelAction::Previous => self.playback.command(PlaybackCommand::Previous),
            TopPanelAction::Next => self.playback.command(PlaybackCommand::Next),
            TopPanelAction::Play => self.playback.command(PlaybackCommand::Play),
            TopPanelAction::Pause => self.playback.command(PlaybackCommand::Pause),
            TopPanelAction::Stop => self.playback.command(PlaybackCommand::Stop),
            TopPanelAction::SeekTo(pos) => self.playback.command(PlaybackCommand::Seek(pos)),
            TopPanelAction::SetVolume(v) => self.playback.command(PlaybackCommand::SetVolume(v)),
        }

        let selected_queue_index = self.state.selected_queue_index;
        let active_tracks = self.state.queue.clone();
        let center_action = draw_center_panel(
            ctx,
            &self.state.analysis,
            &self.state.metadata,
            &active_tracks,
            selected_queue_index,
            self.state.playback.current.as_ref(),
            &self.state.library,
            &mut self.state.library_query,
            &mut self.state.selected_library_root,
            &mut self.state.selected_library_track,
            &mut self.state.expanded_library_groups,
            &mut self.state.spectro_ui,
            &mut self.state.cover_art_cache,
            &mut self.state.library_art_cache,
            &mut self.state.spectrogram_cache,
        );
        match center_action {
            CenterPanelAction {
                queue_play_index: Some(index),
                ..
            } => {
                self.playback.command(PlaybackCommand::PlayAt(index));
                self.playback.command(PlaybackCommand::Play);
                self.state.selected_queue_index = Some(index);
            }
            CenterPanelAction {
                queue_select_index: Some(index),
                ..
            } => {
                self.state.selected_queue_index = Some(index);
            }
            CenterPanelAction {
                scan_library_folder: true,
                ..
            } => {
                if let Some(folder) = rfd::FileDialog::new().pick_folder() {
                    self.library.command(LibraryCommand::ScanRoot(folder));
                }
            }
            CenterPanelAction {
                add_library_track: Some(path),
                ..
            } => {
                self.state.queue.push(path.clone());
                let tracks_len = self.state.queue.len();
                if tracks_len == 1 {
                    let tracks = self.state.queue.clone();
                    self.playback.command(PlaybackCommand::LoadQueue(tracks));
                } else {
                    self.playback
                        .command(PlaybackCommand::AddToQueue(vec![path]));
                }
            }
            CenterPanelAction {
                add_library_album_tracks: Some(paths),
                ..
            } => {
                if paths.is_empty() {
                    // no-op
                } else {
                    let start_idx = self.state.queue.len();
                    self.state.queue.extend(paths);
                    let tracks = self.state.queue.clone();
                    self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    self.playback.command(PlaybackCommand::PlayAt(start_idx));
                    self.playback.command(PlaybackCommand::Play);
                    self.state.selected_queue_index = Some(start_idx);
                }
            }
            CenterPanelAction {
                play_library_track: Some(path),
                ..
            } => {
                self.state.queue.clear();
                self.state.queue.push(path.clone());
                self.playback
                    .command(PlaybackCommand::LoadQueue(vec![path]));
                self.playback.command(PlaybackCommand::Play);
                self.state.selected_queue_index = Some(0);
            }
            CenterPanelAction {
                set_fft_size: Some(size),
                ..
            } => {
                self.analysis
                    .command(crate::analysis::AnalysisCommand::SetFftSize(size));
                self.state.spectrogram_cache = SpectrogramCache::default();
            }
            CenterPanelAction {
                queue_clear: true, ..
            } => {
                self.state.queue.clear();
                self.state.selected_queue_index = None;
                self.playback.command(PlaybackCommand::ClearQueue);
            }
            CenterPanelAction {
                queue_remove_index: Some(idx),
                ..
            } => {
                if idx < self.state.queue.len() {
                    self.state.queue.remove(idx);
                    let new_tracks = self.state.queue.clone();
                    self.state.selected_queue_index = idx.checked_sub(1);
                    self.playback
                        .command(PlaybackCommand::LoadQueue(new_tracks));
                }
            }
            CenterPanelAction {
                queue_move_to: Some((from, to)),
                ..
            } => {
                if from != to {
                    if from < self.state.queue.len() && to < self.state.queue.len() {
                        let item = self.state.queue.remove(from);
                        self.state.queue.insert(to, item);
                        let tracks = self.state.queue.clone();
                        self.state.selected_queue_index = Some(to);
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    }
                }
            }
            CenterPanelAction {
                queue_move_up: true,
                ..
            } => {
                if let Some(sel) = self.state.selected_queue_index {
                    if sel > 0 {
                        if sel < self.state.queue.len() {
                            self.state.queue.swap(sel - 1, sel);
                            let tracks = self.state.queue.clone();
                            self.state.selected_queue_index = Some(sel - 1);
                            self.playback.command(PlaybackCommand::LoadQueue(tracks));
                        }
                    }
                }
            }
            CenterPanelAction {
                queue_move_down: true,
                ..
            } => {
                if let Some(sel) = self.state.selected_queue_index {
                    if sel + 1 < self.state.queue.len() {
                        self.state.queue.swap(sel, sel + 1);
                        let tracks = self.state.queue.clone();
                        self.state.selected_queue_index = Some(sel + 1);
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    }
                }
            }
            _ => {}
        }

        draw_footer_panel(
            ctx,
            &self.state.playback,
            &self.state.metadata,
            &self.state.queue,
            &self.state.library,
        );

        if self.last_tick.elapsed() >= Duration::from_millis(16) {
            self.last_tick = Instant::now();
            self.tick();
        }
        if self.last_settings_save.elapsed() >= Duration::from_secs(2) {
            self.save_settings();
        }
        ctx.request_repaint_after(Duration::from_millis(16));
    }
}
