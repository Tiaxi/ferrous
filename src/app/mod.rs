use std::path::PathBuf;
use std::time::{Duration, Instant};

use crossbeam_channel::Receiver;
use eframe::egui;

use crate::analysis::{AnalysisEngine, AnalysisEvent, AnalysisSnapshot};
use crate::metadata::{MetadataEvent, MetadataService, TrackMetadata};
use crate::playback::{PlaybackCommand, PlaybackEngine, PlaybackEvent, PlaybackSnapshot};
use crate::ui::panels::{draw_center_panel, draw_top_panel, SpectrogramCache, TopPanelAction};

pub struct FerrousApp {
    playback: PlaybackEngine,
    analysis: AnalysisEngine,
    metadata: MetadataService,
    playback_rx: Receiver<PlaybackEvent>,
    analysis_rx: Receiver<AnalysisEvent>,
    metadata_rx: Receiver<MetadataEvent>,
    state: AppState,
    last_tick: Instant,
    profile_enabled: bool,
    profile_last: Instant,
    profile_frames: u32,
}

#[derive(Default)]
struct AppState {
    playback: PlaybackSnapshot,
    analysis: AnalysisSnapshot,
    metadata: TrackMetadata,
    queue: Vec<PathBuf>,
    spectrogram_cache: SpectrogramCache,
}

impl FerrousApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        cc.egui_ctx.set_visuals(egui::Visuals::light());

        let (analysis, analysis_rx) = AnalysisEngine::new();
        let (playback, playback_rx) = PlaybackEngine::new(analysis.sender(), analysis.pcm_sender());
        let (metadata, metadata_rx) = MetadataService::new();

        Self {
            playback,
            analysis,
            metadata,
            playback_rx,
            analysis_rx,
            metadata_rx,
            state: AppState::default(),
            last_tick: Instant::now(),
            profile_enabled: std::env::var_os("FERROUS_PROFILE").is_some(),
            profile_last: Instant::now(),
            profile_frames: 0,
        }
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
    }

    fn tick(&mut self) {
        // Pull fresh playback position.
        self.playback.command(PlaybackCommand::Poll);
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
                    self.state.queue.clear();
                    self.state.queue.extend(paths.clone());
                    self.playback.command(PlaybackCommand::LoadQueue(paths));
                    self.playback.command(PlaybackCommand::Play);
                }
            }
            TopPanelAction::AddFiles => {
                if let Some(paths) = rfd::FileDialog::new()
                    .add_filter("Audio", &["mp3", "flac"])
                    .pick_files()
                {
                    if self.state.queue.is_empty() {
                        self.state.queue.extend(paths.clone());
                        self.playback
                            .command(PlaybackCommand::LoadQueue(self.state.queue.clone()));
                        self.playback.command(PlaybackCommand::Play);
                    } else {
                        self.state.queue.extend(paths.clone());
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
        }

        let play_index = draw_center_panel(
            ctx,
            &self.state.analysis,
            &self.state.metadata,
            &self.state.queue,
            self.state.playback.current.as_ref(),
            &mut self.state.spectrogram_cache,
        );
        if let Some(index) = play_index {
            self.playback.command(PlaybackCommand::PlayAt(index));
            self.playback.command(PlaybackCommand::Play);
        }

        if self.last_tick.elapsed() >= Duration::from_millis(16) {
            self.last_tick = Instant::now();
            self.tick();
        }
        ctx.request_repaint_after(Duration::from_millis(16));
    }
}
