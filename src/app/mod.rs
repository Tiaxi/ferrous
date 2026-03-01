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
    draw_center_panel, draw_top_panel, CenterPanelAction, CoverArtCache, LibraryArtCache,
    SpectrogramCache, SpectrogramUiSettings, TopPanelAction,
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
    playlists: Vec<PlaylistModel>,
    active_playlist: usize,
    selected_queue_index: Option<usize>,
    cover_art_cache: CoverArtCache,
    library_art_cache: LibraryArtCache,
    spectro_ui: SpectrogramUiSettings,
    spectrogram_cache: SpectrogramCache,
}

#[derive(Default)]
struct PlaylistModel {
    name: String,
    tracks: Vec<PathBuf>,
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
                playlists: vec![PlaylistModel {
                    name: "Playlist 1".to_string(),
                    tracks: Vec::new(),
                }],
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
                    self.state.spectrogram_cache = SpectrogramCache::default();
                    self.analysis
                        .command(crate::analysis::AnalysisCommand::ResetRealtime);
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

    fn active_playlist_mut(&mut self) -> &mut PlaylistModel {
        let idx = self
            .state
            .active_playlist
            .min(self.state.playlists.len().saturating_sub(1));
        &mut self.state.playlists[idx]
    }

    fn active_playlist(&self) -> &PlaylistModel {
        let idx = self
            .state
            .active_playlist
            .min(self.state.playlists.len().saturating_sub(1));
        &self.state.playlists[idx]
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
                        self.state.spectro_ui.fft_size = x.clamp(256, 2048).next_power_of_two();
                    }
                }
                "floor_cut" => {
                    if let Ok(x) = v.parse::<f32>() {
                        self.state.spectro_ui.floor_cut = x.clamp(0.0, 0.16);
                    }
                }
                "bass_gain_min" => {
                    if let Ok(x) = v.parse::<f32>() {
                        self.state.spectro_ui.bass_gain_min = x.clamp(0.60, 0.95);
                    }
                }
                "highlight_knee" => {
                    if let Ok(x) = v.parse::<f32>() {
                        self.state.spectro_ui.highlight_knee = x.clamp(0.60, 0.90);
                    }
                }
                "active_playlist" => {
                    if let Ok(x) = v.parse::<usize>() {
                        self.state.active_playlist =
                            x.min(self.state.playlists.len().saturating_sub(1));
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
            "volume={:.4}\nfft_size={}\nfloor_cut={:.4}\nbass_gain_min={:.4}\nhighlight_knee={:.4}\nactive_playlist={}\n",
            self.state.playback.volume,
            self.state.spectro_ui.fft_size,
            self.state.spectro_ui.floor_cut,
            self.state.spectro_ui.bass_gain_min,
            self.state.spectro_ui.highlight_knee,
            self.state.active_playlist
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
                self.active_playlist().tracks.len(),
                self.state.analysis.spectrogram_seq
            );
            self.profile_last = Instant::now();
            self.profile_frames = 0;
        }

        self.pump_events();

        let action = draw_top_panel(
            ctx,
            &self.state.playback,
            &self.state.metadata,
            &self.state.analysis,
        );

        match action {
            TopPanelAction::None => {}
            TopPanelAction::OpenFiles => {
                if let Some(paths) = rfd::FileDialog::new()
                    .add_filter("Audio", &["mp3", "flac"])
                    .pick_files()
                {
                    let pl = self.active_playlist_mut();
                    pl.tracks.clear();
                    pl.tracks.extend(paths.clone());
                    self.state.selected_queue_index =
                        if pl.tracks.is_empty() { None } else { Some(0) };
                    self.playback.command(PlaybackCommand::LoadQueue(paths));
                    self.playback.command(PlaybackCommand::Play);
                }
            }
            TopPanelAction::AddFiles => {
                if let Some(paths) = rfd::FileDialog::new()
                    .add_filter("Audio", &["mp3", "flac"])
                    .pick_files()
                {
                    let was_empty = self.active_playlist().tracks.is_empty();
                    if was_empty {
                        let tracks = {
                            let pl = self.active_playlist_mut();
                            pl.tracks.extend(paths.clone());
                            pl.tracks.clone()
                        };
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                        self.playback.command(PlaybackCommand::Play);
                    } else {
                        self.active_playlist_mut().tracks.extend(paths.clone());
                        self.playback.command(PlaybackCommand::AddToQueue(paths));
                    }
                }
            }
            TopPanelAction::Previous => self.playback.command(PlaybackCommand::Previous),
            TopPanelAction::Next => self.playback.command(PlaybackCommand::Next),
            TopPanelAction::Play => self.playback.command(PlaybackCommand::Play),
            TopPanelAction::Pause => self.playback.command(PlaybackCommand::Pause),
            TopPanelAction::Stop => self.playback.command(PlaybackCommand::Stop),
            TopPanelAction::SeekTo(pos) => self.playback.command(PlaybackCommand::Seek(pos)),
            TopPanelAction::SetVolume(v) => self.playback.command(PlaybackCommand::SetVolume(v)),
        }

        let playlist_names: Vec<String> = self
            .state
            .playlists
            .iter()
            .map(|p| p.name.clone())
            .collect();
        let active_playlist = self.state.active_playlist;
        let selected_queue_index = self.state.selected_queue_index;
        let active_tracks = self.active_playlist().tracks.clone();
        let center_action = draw_center_panel(
            ctx,
            &self.state.analysis,
            &self.state.metadata,
            &active_tracks,
            &playlist_names,
            active_playlist,
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
                queue_select_index: Some(index),
                ..
            } => {
                self.state.selected_queue_index = Some(index);
            }
            CenterPanelAction {
                queue_play_index: Some(index),
                ..
            } => {
                self.playback.command(PlaybackCommand::PlayAt(index));
                self.playback.command(PlaybackCommand::Play);
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
                let tracks_len = {
                    let pl = self.active_playlist_mut();
                    pl.tracks.push(path.clone());
                    pl.tracks.len()
                };
                if tracks_len == 1 {
                    let tracks = self.active_playlist().tracks.clone();
                    self.playback.command(PlaybackCommand::LoadQueue(tracks));
                } else {
                    self.playback
                        .command(PlaybackCommand::AddToQueue(vec![path]));
                }
            }
            CenterPanelAction {
                play_library_track: Some(path),
                ..
            } => {
                let pl = self.active_playlist_mut();
                pl.tracks.clear();
                pl.tracks.push(path.clone());
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
                self.active_playlist_mut().tracks.clear();
                self.state.selected_queue_index = None;
                self.playback.command(PlaybackCommand::ClearQueue);
            }
            CenterPanelAction {
                queue_remove_index: Some(idx),
                ..
            } => {
                let mut new_tracks = None;
                {
                    let pl = self.active_playlist_mut();
                    if idx < pl.tracks.len() {
                        pl.tracks.remove(idx);
                        new_tracks = Some(pl.tracks.clone());
                    }
                }
                if let Some(tracks) = new_tracks {
                    self.state.selected_queue_index = idx.checked_sub(1);
                    self.playback.command(PlaybackCommand::LoadQueue(tracks));
                }
            }
            CenterPanelAction {
                queue_move_up: true,
                ..
            } => {
                if let Some(sel) = self.state.selected_queue_index {
                    if sel > 0 {
                        let mut new_tracks = None;
                        {
                            let pl = self.active_playlist_mut();
                            if sel < pl.tracks.len() {
                                pl.tracks.swap(sel - 1, sel);
                                new_tracks = Some(pl.tracks.clone());
                            }
                        }
                        if let Some(tracks) = new_tracks {
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
                    let mut new_tracks = None;
                    {
                        let pl = self.active_playlist_mut();
                        if sel + 1 < pl.tracks.len() {
                            pl.tracks.swap(sel, sel + 1);
                            new_tracks = Some(pl.tracks.clone());
                        }
                    }
                    if let Some(tracks) = new_tracks {
                        self.state.selected_queue_index = Some(sel + 1);
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    }
                }
            }
            CenterPanelAction {
                select_playlist: Some(idx),
                ..
            } => {
                if idx < self.state.playlists.len() {
                    self.state.active_playlist = idx;
                    self.state.selected_queue_index = None;
                    let tracks = self.active_playlist().tracks.clone();
                    if tracks.is_empty() {
                        self.playback.command(PlaybackCommand::ClearQueue);
                    } else {
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    }
                }
            }
            CenterPanelAction {
                create_playlist: true,
                ..
            } => {
                let next_n = self.state.playlists.len() + 1;
                self.state.playlists.push(PlaylistModel {
                    name: format!("Playlist {next_n}"),
                    tracks: Vec::new(),
                });
                self.state.active_playlist = self.state.playlists.len() - 1;
                self.state.selected_queue_index = None;
                self.playback.command(PlaybackCommand::ClearQueue);
            }
            CenterPanelAction {
                delete_playlist: true,
                ..
            } => {
                if self.state.playlists.len() > 1 {
                    let idx = self
                        .state
                        .active_playlist
                        .min(self.state.playlists.len().saturating_sub(1));
                    self.state.playlists.remove(idx);
                    self.state.active_playlist = idx.saturating_sub(1);
                    self.state.selected_queue_index = None;
                    let tracks = self.active_playlist().tracks.clone();
                    if tracks.is_empty() {
                        self.playback.command(PlaybackCommand::ClearQueue);
                    } else {
                        self.playback.command(PlaybackCommand::LoadQueue(tracks));
                    }
                }
            }
            _ => {}
        }

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
