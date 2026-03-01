use std::collections::{BTreeMap, HashMap, HashSet};
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use std::{ffi::OsStr, path::PathBuf};

use eframe::egui::{
    self, Color32, ColorImage, Pos2, Rect, Sense, Stroke, StrokeKind, TextureHandle,
    TextureOptions, Vec2,
};
use lofty::file::TaggedFileExt;

use crate::analysis::AnalysisSnapshot;
use crate::library::LibrarySnapshot;
use crate::metadata::TrackMetadata;
use crate::playback::{PlaybackSnapshot, PlaybackState};

#[derive(Debug, Clone, Copy)]
pub enum TopPanelAction {
    None,
    OpenFiles,
    AddFiles,
    Previous,
    Next,
    Play,
    Pause,
    Stop,
    SeekTo(Duration),
    SetVolume(f32),
}

#[derive(Default)]
pub struct CenterPanelAction {
    pub queue_play_index: Option<usize>,
    pub queue_select_index: Option<usize>,
    pub queue_move_to: Option<(usize, usize)>,
    pub queue_clear: bool,
    pub queue_remove_index: Option<usize>,
    pub queue_move_up: bool,
    pub queue_move_down: bool,
    pub scan_library_folder: bool,
    pub add_library_track: Option<PathBuf>,
    pub add_library_album_tracks: Option<Vec<PathBuf>>,
    pub play_library_track: Option<PathBuf>,
    pub set_fft_size: Option<usize>,
    pub select_playlist: Option<usize>,
    pub create_playlist: bool,
    pub delete_playlist: bool,
}

#[derive(Debug, Clone)]
pub struct SpectrogramUiSettings {
    pub fft_size: usize,
    pub db_range: f32,
    pub log_scale: bool,
}

impl Default for SpectrogramUiSettings {
    fn default() -> Self {
        Self {
            fft_size: 8192,
            db_range: 90.0,
            log_scale: false,
        }
    }
}

#[derive(Default)]
pub struct SpectrogramCache {
    texture: Option<TextureHandle>,
    width: usize,
    height: usize,
    last_seq: u64,
    write_x: usize,
    written_cols: usize,
    filled: bool,
    fps_last_instant: Option<Instant>,
    fps_accum_frames: u32,
    fps_value: f32,
}

#[derive(Default)]
pub struct CoverArtCache {
    texture: Option<TextureHandle>,
    key: Option<u64>,
}

#[derive(Default)]
pub struct LibraryArtCache {
    thumbs: HashMap<u64, TextureHandle>,
    missing: HashSet<u64>,
}

pub fn draw_top_panel(
    ctx: &egui::Context,
    playback: &PlaybackSnapshot,
    metadata: &TrackMetadata,
    analysis: &AnalysisSnapshot,
) -> TopPanelAction {
    let mut action = TopPanelAction::None;

    egui::TopBottomPanel::top("top_controls").show(ctx, |ui| {
        egui::menu::bar(ui, |ui| {
            ui.label("File");
            ui.label("Edit");
            ui.label("View");
            ui.label("Playback");
            ui.label("Help");
        });
        ui.separator();

        ui.horizontal(|ui| {
            if ui.button("Open").clicked() {
                action = TopPanelAction::OpenFiles;
            }
            if ui.button("Add").clicked() {
                action = TopPanelAction::AddFiles;
            }
            if ui.button("Prev").clicked() {
                action = TopPanelAction::Previous;
            }
            if ui.button("Next").clicked() {
                action = TopPanelAction::Next;
            }
            if ui.button("Play").clicked() {
                action = TopPanelAction::Play;
            }
            if ui.button("Pause").clicked() {
                action = TopPanelAction::Pause;
            }
            if ui.button("Stop").clicked() {
                action = TopPanelAction::Stop;
            }

            ui.separator();

            if let Some(seek) = draw_wave_seekbar(ui, playback, analysis) {
                action = TopPanelAction::SeekTo(seek);
            }

            ui.label(format!(
                "{} / {}",
                format_duration(playback.position),
                format_duration(playback.duration)
            ));
            ui.separator();
            let mut vol = playback.volume.clamp(0.0, 1.0);
            let changed = ui
                .add(
                    egui::Slider::new(&mut vol, 0.0..=1.0)
                        .text("Vol")
                        .clamping(egui::SliderClamping::Always),
                )
                .changed();
            if changed {
                action = TopPanelAction::SetVolume(vol);
            }
        });

        ui.horizontal(|ui| {
            let state = match playback.state {
                PlaybackState::Stopped => "stopped",
                PlaybackState::Playing => "playing",
                PlaybackState::Paused => "paused",
            };
            let title = if metadata.title.is_empty() {
                "No track loaded"
            } else {
                metadata.title.as_str()
            };
            let artist = if metadata.artist.is_empty() {
                ""
            } else {
                metadata.artist.as_str()
            };
            let mut fmt = Vec::new();
            if let Some(sr) = metadata.sample_rate_hz {
                fmt.push(format!("{sr}Hz"));
            }
            if let Some(bits) = metadata.bit_depth {
                fmt.push(format!("{bits}bit"));
            }
            if let Some(ch) = metadata.channels {
                fmt.push(if ch == 1 {
                    "mono".to_string()
                } else if ch == 2 {
                    "stereo".to_string()
                } else {
                    format!("{ch}ch")
                });
            }
            if let Some(br) = metadata.bitrate_kbps {
                fmt.push(format!("{br}kbps"));
            }
            let fmt_str = if fmt.is_empty() {
                String::new()
            } else {
                format!(" | {}", fmt.join(" "))
            };
            ui.label(format!("{state} | {title} {artist}{fmt_str}"));
        });
    });

    action
}

pub fn draw_center_panel(
    ctx: &egui::Context,
    analysis: &AnalysisSnapshot,
    metadata: &TrackMetadata,
    queue: &[PathBuf],
    playlist_names: &[String],
    active_playlist: usize,
    selected_queue_index: Option<usize>,
    current: Option<&PathBuf>,
    library: &LibrarySnapshot,
    library_query: &mut String,
    selected_library_root: &mut Option<PathBuf>,
    selected_library_track: &mut Option<PathBuf>,
    expanded_library_groups: &mut HashMap<String, bool>,
    spectro_ui: &mut SpectrogramUiSettings,
    cover_art_cache: &mut CoverArtCache,
    library_art_cache: &mut LibraryArtCache,
    spectrogram_cache: &mut SpectrogramCache,
) -> CenterPanelAction {
    let mut action = CenterPanelAction::default();
    egui::CentralPanel::default().show(ctx, |ui| {
        let top_h = ui.available_height();
        ui.allocate_ui(Vec2::new(ui.available_width(), top_h), |ui| {
            let full_w = ui.available_width();
            let mut left_w = (full_w * 0.28).clamp(250.0, 390.0);
            left_w = left_w.min((full_w - 180.0).max(120.0));
            let right_w = (full_w - left_w - 12.0).max(120.0);

            ui.horizontal(|ui| {
                ui.allocate_ui_with_layout(
                    Vec2::new(left_w, top_h),
                    egui::Layout::top_down(egui::Align::Min),
                    |ui| {
                        ui.heading("Library");
                        ui.separator();
                        let cover_h = ui.available_width().max(80.0);
                        draw_cover_art(ui, metadata, cover_art_cache, Vec2::splat(cover_h));

                        ui.separator();
                        ui.horizontal(|ui| {
                            if ui.button("Scan Folder").clicked() && !library.scan_in_progress {
                                action.scan_library_folder = true;
                            }
                            if ui
                                .add_enabled(
                                    selected_library_track.is_some(),
                                    egui::Button::new("Add"),
                                )
                                .clicked()
                            {
                                action.add_library_track = selected_library_track.clone();
                            }
                            if ui
                                .add_enabled(
                                    selected_library_track.is_some(),
                                    egui::Button::new("Play"),
                                )
                                .clicked()
                            {
                                action.play_library_track = selected_library_track.clone();
                            }
                        });

                        ui.horizontal(|ui| {
                            ui.label("Search:");
                            ui.text_edit_singleline(library_query);
                        });
                        let query = library_query.trim().to_lowercase();
                        let selected_root = selected_library_root.clone();
                        let visible_indices: Vec<usize> = library
                            .tracks
                            .iter()
                            .enumerate()
                            .filter_map(|(idx, track)| {
                                if let Some(root) = selected_root.as_ref() {
                                    if !track.path.starts_with(root) {
                                        return None;
                                    }
                                }

                                if query.is_empty() {
                                    return Some(idx);
                                }

                                let hay = format!(
                                    "{} {} {} {}",
                                    track.title,
                                    track.artist,
                                    track.album,
                                    track.path.display()
                                )
                                .to_lowercase();
                                if hay.contains(&query) {
                                    Some(idx)
                                } else {
                                    None
                                }
                            })
                            .collect();
                        ui.label(format!(
                            "Indexed tracks: {}  |  Visible: {}",
                            library.tracks.len(),
                            visible_indices.len()
                        ));
                        if library.scan_in_progress {
                            ui.label(format!("Scanning... {} files", library.scanned_files));
                        }
                        if let Some(err) = library.last_error.as_ref() {
                            ui.colored_label(Color32::from_rgb(200, 70, 70), err);
                        }

                        ui.separator();
                        ui.label("Indexed Folders");
                        egui::ScrollArea::vertical()
                            .auto_shrink([false, false])
                            .max_height((top_h * 0.16).clamp(56.0, 92.0))
                            .show(ui, |ui| {
                                if library.roots.is_empty() {
                                    ui.label("No folders added");
                                } else {
                                    let row_w = ui.available_width().max(90.0);
                                    let max_chars = ((row_w / 6.8).floor() as usize).max(8);
                                    for root in &library.roots {
                                        let root_text =
                                            ellipsize(&root.display().to_string(), max_chars);
                                        let selected = selected_library_root
                                            .as_ref()
                                            .map(|p| p == root)
                                            .unwrap_or(false);
                                        let resp = full_row_text_button(
                                            ui, row_w, 20.0, &root_text, selected, 0.0,
                                        );
                                        if resp.clicked() {
                                            if selected {
                                                *selected_library_root = None;
                                            } else {
                                                *selected_library_root = Some(root.clone());
                                            }
                                        }
                                    }
                                }
                            });

                        ui.separator();
                        ui.label("Library Tree");
                        if visible_indices.is_empty() {
                            ui.label("No tracks indexed");
                        } else {
                            let row_h = 24.0;
                            let tracks_h = ui.available_height().max(120.0);
                            ui.allocate_ui(Vec2::new(ui.available_width(), tracks_h), |ui| {
                                egui::ScrollArea::vertical()
                                    .auto_shrink([false, false])
                                    .show(ui, |ui| {
                                        let row_w = ui.available_width().max(80.0);
                                        let group_chars = ((row_w / 7.6).floor() as usize).max(14);
                                        let track_chars = ((row_w / 8.2).floor() as usize).max(10);

                                        let mut artist_map: BTreeMap<
                                            String,
                                            BTreeMap<String, Vec<usize>>,
                                        > = BTreeMap::new();
                                        for idx in visible_indices.iter().copied() {
                                            let Some(track) = library.tracks.get(idx) else {
                                                continue;
                                            };
                                            let artist = if track.artist.trim().is_empty() {
                                                "Unknown artist".to_string()
                                            } else {
                                                track.artist.trim().to_string()
                                            };
                                            let album = if track.album.trim().is_empty() {
                                                "Unknown album".to_string()
                                            } else {
                                                track.album.trim().to_string()
                                            };
                                            artist_map
                                                .entry(artist)
                                                .or_default()
                                                .entry(album)
                                                .or_default()
                                                .push(idx);
                                        }

                                        let all_key = "__tree_all_music__".to_string();
                                        let all_open = expanded_library_groups
                                            .get(&all_key)
                                            .copied()
                                            .unwrap_or(true);
                                        if full_row_text_button(
                                            ui,
                                            row_w,
                                            row_h,
                                            &format!(
                                                "{} All Music ({})",
                                                if all_open { "v" } else { ">" },
                                                visible_indices.len()
                                            ),
                                            all_open,
                                            0.0,
                                        )
                                        .clicked()
                                        {
                                            expanded_library_groups
                                                .insert(all_key.clone(), !all_open);
                                        }

                                        if !expanded_library_groups
                                            .get(&all_key)
                                            .copied()
                                            .unwrap_or(true)
                                        {
                                            return;
                                        }

                                        for (artist, albums) in artist_map {
                                            let artist_key =
                                                format!("__tree_artist__{}", artist.to_lowercase());
                                            let artist_count: usize =
                                                albums.values().map(|v| v.len()).sum();
                                            let artist_open = expanded_library_groups
                                                .get(&artist_key)
                                                .copied()
                                                .unwrap_or(false);
                                            let artist_label = ellipsize(
                                                &format!(
                                                    "{} {} ({})",
                                                    if artist_open { "v" } else { ">" },
                                                    artist,
                                                    artist_count
                                                ),
                                                group_chars,
                                            );
                                            if full_row_text_button(
                                                ui,
                                                row_w,
                                                row_h,
                                                &artist_label,
                                                artist_open,
                                                0.0,
                                            )
                                            .clicked()
                                            {
                                                expanded_library_groups
                                                    .insert(artist_key.clone(), !artist_open);
                                            }
                                            if !expanded_library_groups
                                                .get(&artist_key)
                                                .copied()
                                                .unwrap_or(false)
                                            {
                                                continue;
                                            }

                                            for (album, indices) in albums {
                                                let album_key = format!(
                                                    "__tree_album__{}|{}",
                                                    artist.to_lowercase(),
                                                    album.to_lowercase()
                                                );
                                                let album_open = expanded_library_groups
                                                    .get(&album_key)
                                                    .copied()
                                                    .unwrap_or(false);
                                                let album_label = ellipsize(
                                                    &format!("  {} ({})", album, indices.len()),
                                                    group_chars,
                                                );
                                                let mut toggle_album = false;
                                                let mut add_album = false;
                                                ui.horizontal(|ui| {
                                                    if full_row_text_button(
                                                        ui,
                                                        18.0,
                                                        row_h,
                                                        if album_open { "v" } else { ">" },
                                                        false,
                                                        0.0,
                                                    )
                                                    .clicked()
                                                    {
                                                        toggle_album = true;
                                                    }
                                                    let resp = full_row_text_button(
                                                        ui,
                                                        (row_w - 22.0).max(60.0),
                                                        row_h,
                                                        &album_label,
                                                        album_open,
                                                        0.0,
                                                    );
                                                    if resp.double_clicked() {
                                                        add_album = true;
                                                    }
                                                });
                                                if toggle_album {
                                                    expanded_library_groups
                                                        .insert(album_key.clone(), !album_open);
                                                }
                                                if add_album {
                                                    let mut paths =
                                                        Vec::with_capacity(indices.len());
                                                    for idx in indices.iter().copied() {
                                                        if let Some(t) = library.tracks.get(idx) {
                                                            paths.push(t.path.clone());
                                                        }
                                                    }
                                                    if !paths.is_empty() {
                                                        action.add_library_album_tracks =
                                                            Some(paths);
                                                    }
                                                }
                                                if !expanded_library_groups
                                                    .get(&album_key)
                                                    .copied()
                                                    .unwrap_or(false)
                                                {
                                                    continue;
                                                }

                                                let first_track = indices
                                                    .first()
                                                    .and_then(|i| library.tracks.get(*i));
                                                if let Some(track) = first_track {
                                                    if let Some(tex_id) =
                                                        ensure_album_thumb_texture_id(
                                                            ui,
                                                            library_art_cache,
                                                            &album_key,
                                                            &track.path,
                                                        )
                                                    {
                                                        ui.horizontal(|ui| {
                                                            ui.add_space(22.0);
                                                            ui.image((tex_id, Vec2::splat(20.0)));
                                                        });
                                                    }
                                                }

                                                for (idx_in_album, idx) in
                                                    indices.iter().copied().enumerate()
                                                {
                                                    let Some(track) = library.tracks.get(idx)
                                                    else {
                                                        continue;
                                                    };
                                                    let title = if track.title.is_empty() {
                                                        track_label(&track.path)
                                                    } else {
                                                        track.title.clone()
                                                    };
                                                    let title = ellipsize(&title, track_chars);
                                                    let duration = track
                                                        .duration_secs
                                                        .map(format_seconds)
                                                        .unwrap_or_else(|| "--:--".to_string());
                                                    let row_text = format!(
                                                        "    {:02}  {}  {}",
                                                        track
                                                            .track_no
                                                            .unwrap_or((idx_in_album + 1) as u32),
                                                        title,
                                                        duration
                                                    );
                                                    let is_selected = selected_library_track
                                                        .as_ref()
                                                        .map(|p| p == &track.path)
                                                        .unwrap_or(false);
                                                    let resp = full_row_text_button(
                                                        ui,
                                                        row_w,
                                                        row_h,
                                                        &row_text,
                                                        is_selected,
                                                        0.0,
                                                    );
                                                    if resp.clicked() {
                                                        *selected_library_track =
                                                            Some(track.path.clone());
                                                    }
                                                    if resp.double_clicked() {
                                                        action.play_library_track =
                                                            Some(track.path.clone());
                                                    }
                                                }
                                            }
                                        }
                                    });
                            });
                        }
                    },
                );

                ui.separator();

                ui.allocate_ui_with_layout(
                    Vec2::new(right_w, top_h),
                    egui::Layout::top_down(egui::Align::Min),
                    |ui| {
                        let total_h = ui.available_height();
                        let split_pad = 6.0;
                        let mut spectro_h: f32 = 400.0;
                        let max_spectro_h = (total_h - split_pad - 120.0).max(80.0);
                        spectro_h = spectro_h.min(max_spectro_h).max(140.0);
                        let playlist_h = (total_h - spectro_h - split_pad).max(80.0);
                        ui.allocate_ui_with_layout(
                            Vec2::new(ui.available_width(), playlist_h),
                            egui::Layout::top_down(egui::Align::Min),
                            |ui| {
                                ui.heading("Playlist");
                                ui.separator();
                                ui.horizontal_wrapped(|ui| {
                                    let active_label = playlist_names
                                        .get(active_playlist)
                                        .cloned()
                                        .unwrap_or_else(|| "Playlist".to_string());
                                    egui::ComboBox::from_id_salt("playlist_select")
                                        .selected_text(active_label)
                                        .show_ui(ui, |ui| {
                                            for (idx, name) in playlist_names.iter().enumerate() {
                                                if ui
                                                    .selectable_label(idx == active_playlist, name)
                                                    .clicked()
                                                {
                                                    action.select_playlist = Some(idx);
                                                }
                                            }
                                        });
                                    if ui.button("+").clicked() {
                                        action.create_playlist = true;
                                    }
                                    if ui
                                        .add_enabled(
                                            playlist_names.len() > 1,
                                            egui::Button::new("-"),
                                        )
                                        .clicked()
                                    {
                                        action.delete_playlist = true;
                                    }
                                    if ui
                                        .add_enabled(
                                            selected_queue_index.is_some(),
                                            egui::Button::new("Up"),
                                        )
                                        .clicked()
                                    {
                                        action.queue_move_up = true;
                                    }
                                    if ui
                                        .add_enabled(
                                            selected_queue_index.is_some(),
                                            egui::Button::new("Down"),
                                        )
                                        .clicked()
                                    {
                                        action.queue_move_down = true;
                                    }
                                    if ui
                                        .add_enabled(
                                            selected_queue_index.is_some(),
                                            egui::Button::new("Remove"),
                                        )
                                        .clicked()
                                    {
                                        action.queue_remove_index = selected_queue_index;
                                    }
                                });
                                ui.separator();
                                let now_playing = if metadata.title.is_empty() {
                                    "No track loaded".to_string()
                                } else if metadata.artist.is_empty() {
                                    metadata.title.clone()
                                } else {
                                    format!("{} - {}", metadata.artist, metadata.title)
                                };
                                ui.label(format!("Now Playing: {now_playing}"));
                                if !metadata.album.is_empty() {
                                    ui.label(format!("Album: {}", metadata.album));
                                }
                                ui.add_space(2.0);
                                ui.horizontal(|ui| {
                                    if ui
                                        .add_enabled(!queue.is_empty(), egui::Button::new("Clear"))
                                        .clicked()
                                    {
                                        action.queue_clear = true;
                                    }
                                    ui.label(format!("Tracks: {}", queue.len()));
                                });
                                ui.separator();
                                let row_h = 22.0;
                                ui.horizontal(|ui| {
                                    ui.add_sized(Vec2::new(34.0, row_h), egui::Label::new("#"));
                                    ui.label("Title");
                                    ui.with_layout(
                                        egui::Layout::right_to_left(egui::Align::Center),
                                        |ui| {
                                            ui.add_sized(
                                                Vec2::new(58.0, row_h),
                                                egui::Label::new("Length"),
                                            );
                                        },
                                    );
                                });
                                ui.separator();
                                let queue_h = ui.available_height().max(60.0);
                                ui.allocate_ui(Vec2::new(ui.available_width(), queue_h), |ui| {
                                    egui::ScrollArea::vertical()
                                        .auto_shrink([false, false])
                                        .show(ui, |ui| {
                                            if queue.is_empty() {
                                                ui.label("Queue is empty");
                                                return;
                                            }

                                            let row_w = ui.available_width().max(120.0);
                                            let max_chars =
                                                ((row_w / 7.2).floor() as usize).max(10);
                                            let drag_id = ui.id().with("playlist_drag_from");
                                            let mut hovered_idx: Option<usize> = None;
                                            for (idx, path) in queue.iter().enumerate() {
                                                let is_current =
                                                    current.map(|p| p == path).unwrap_or(false);
                                                let is_selected = selected_queue_index == Some(idx);
                                                let duration = duration_for_path(library, path)
                                                    .map(format_seconds)
                                                    .unwrap_or_else(|| "--:--".to_string());
                                                let mut text = track_label(path);
                                                if is_current {
                                                    text.push_str("  ▶");
                                                }
                                                text = ellipsize(&text, max_chars);

                                                let button_text = format!(
                                                    "{:02}  {}  {}",
                                                    idx + 1,
                                                    text,
                                                    duration
                                                );
                                                let resp = ui.add_sized(
                                                    Vec2::new(row_w, row_h),
                                                    egui::Button::new(button_text)
                                                        .selected(is_selected)
                                                        .sense(Sense::click_and_drag()),
                                                );
                                                if resp.double_clicked() {
                                                    action.queue_play_index = Some(idx);
                                                } else if resp.clicked() {
                                                    if is_selected {
                                                        action.queue_play_index = Some(idx);
                                                    } else {
                                                        action.queue_select_index = Some(idx);
                                                    }
                                                }
                                                if resp.drag_started() {
                                                    ui.memory_mut(|m| {
                                                        m.data.insert_temp(drag_id, idx)
                                                    });
                                                }
                                                if resp.hovered() {
                                                    hovered_idx = Some(idx);
                                                }
                                            }
                                            if ui.input(|i| i.pointer.any_released()) {
                                                let from = ui.memory_mut(|m| {
                                                    m.data.remove_temp::<usize>(drag_id)
                                                });
                                                if let (Some(from), Some(to)) = (from, hovered_idx)
                                                {
                                                    if from != to {
                                                        action.queue_move_to = Some((from, to));
                                                    }
                                                }
                                            }
                                        });
                                });
                            },
                        );
                        ui.separator();
                        let spectro_h = ui.available_height().max(120.0);
                        ui.allocate_ui_with_layout(
                            Vec2::new(ui.available_width(), spectro_h),
                            egui::Layout::top_down(egui::Align::Min),
                            |ui| {
                                draw_spectrogram(
                                    ui,
                                    spectro_h,
                                    &analysis.spectrogram_rows,
                                    analysis.spectrogram_seq,
                                    analysis.sample_rate_hz,
                                    spectro_ui,
                                    spectrogram_cache,
                                );
                            },
                        );
                    },
                );
            });
        });
    });
    action
}

fn draw_cover_art(
    ui: &mut egui::Ui,
    metadata: &TrackMetadata,
    cache: &mut CoverArtCache,
    desired: Vec2,
) {
    let (rect, _) = ui.allocate_exact_size(desired, Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 2.0, Color32::from_gray(30));

    let Some((w, h, rgba)) = metadata.cover_art_rgba.as_ref() else {
        painter.text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            "No cover art",
            egui::TextStyle::Body.resolve(ui.style()),
            Color32::from_gray(170),
        );
        cache.texture = None;
        cache.key = None;
        return;
    };

    let key = cover_art_key(*w, *h, rgba);
    if cache.key != Some(key) || cache.texture.is_none() {
        let image = ColorImage::from_rgba_unmultiplied([*w, *h], rgba);
        cache.texture = Some(ui.ctx().load_texture(
            "cover_art_texture",
            image,
            TextureOptions::LINEAR,
        ));
        cache.key = Some(key);
    }

    if let Some(tex) = cache.texture.as_ref() {
        let img_aspect = *w as f32 / (*h).max(1) as f32;
        let rect_aspect = rect.width() / rect.height().max(1.0);
        let draw_rect = if img_aspect > rect_aspect {
            let draw_h = rect.width() / img_aspect;
            Rect::from_center_size(rect.center(), Vec2::new(rect.width(), draw_h))
        } else {
            let draw_w = rect.height() * img_aspect;
            Rect::from_center_size(rect.center(), Vec2::new(draw_w, rect.height()))
        };
        painter.image(
            tex.id(),
            draw_rect,
            Rect::from_min_max(Pos2::new(0.0, 0.0), Pos2::new(1.0, 1.0)),
            Color32::WHITE,
        );
    }
}

fn cover_art_key(w: usize, h: usize, rgba: &[u8]) -> u64 {
    let mut hash = 1469598103934665603u64;
    hash ^= w as u64;
    hash = hash.wrapping_mul(1099511628211);
    hash ^= h as u64;
    hash = hash.wrapping_mul(1099511628211);
    hash ^= rgba.len() as u64;
    hash = hash.wrapping_mul(1099511628211);
    let step = (rgba.len() / 128).max(1);
    for b in rgba.iter().step_by(step).take(128) {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash
}

fn ensure_album_thumb_texture_id(
    ui: &egui::Ui,
    cache: &mut LibraryArtCache,
    group_key: &str,
    track_path: &PathBuf,
) -> Option<egui::TextureId> {
    let key = hash_str(group_key);
    if let Some(tex) = cache.thumbs.get(&key) {
        return Some(tex.id());
    }
    if cache.missing.contains(&key) {
        return None;
    }

    let tagged = match lofty::read_from_path(track_path) {
        Ok(v) => v,
        Err(_) => {
            cache.missing.insert(key);
            return None;
        }
    };
    let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) else {
        cache.missing.insert(key);
        return None;
    };
    let Some(pic) = tag.pictures().first() else {
        return load_album_thumb_from_folder(ui, cache, key, track_path);
    };
    let Ok(img) = image::load_from_memory(pic.data()) else {
        return load_album_thumb_from_folder(ui, cache, key, track_path);
    };
    let rgba = img.thumbnail(24, 24).to_rgba8();
    let image = ColorImage::from_rgba_unmultiplied(
        [rgba.width() as usize, rgba.height() as usize],
        &rgba.into_raw(),
    );
    let tex = ui.ctx().load_texture(
        format!("library_album_thumb_{key}"),
        image,
        TextureOptions::LINEAR,
    );
    if cache.thumbs.len() >= 512 {
        cache.thumbs.clear();
        cache.missing.clear();
    }
    cache.thumbs.insert(key, tex);
    cache.thumbs.get(&key).map(TextureHandle::id)
}

fn load_album_thumb_from_folder(
    ui: &egui::Ui,
    cache: &mut LibraryArtCache,
    key: u64,
    track_path: &PathBuf,
) -> Option<egui::TextureId> {
    let Some(dir) = track_path.parent() else {
        cache.missing.insert(key);
        return None;
    };
    let mut candidates = vec![
        "cover.jpg",
        "cover.jpeg",
        "cover.png",
        "folder.jpg",
        "folder.jpeg",
        "folder.png",
        "front.jpg",
        "front.png",
    ]
    .into_iter()
    .map(|n| dir.join(n))
    .collect::<Vec<_>>();

    if let Ok(read_dir) = std::fs::read_dir(dir) {
        for ent in read_dir.flatten() {
            let p = ent.path();
            if !p.is_file() {
                continue;
            }
            let Some(ext) = p.extension().and_then(|e| e.to_str()) else {
                continue;
            };
            let ext = ext.to_ascii_lowercase();
            if ext == "jpg" || ext == "jpeg" || ext == "png" {
                if !candidates.iter().any(|c| c == &p) {
                    candidates.push(p);
                }
            }
        }
    }

    for p in candidates {
        if !p.is_file() {
            continue;
        }
        if let Ok(bytes) = std::fs::read(&p) {
            if let Ok(img) = image::load_from_memory(&bytes) {
                let rgba = img.thumbnail(24, 24).to_rgba8();
                let image = ColorImage::from_rgba_unmultiplied(
                    [rgba.width() as usize, rgba.height() as usize],
                    &rgba.into_raw(),
                );
                let tex = ui.ctx().load_texture(
                    format!("library_album_thumb_{key}"),
                    image,
                    TextureOptions::LINEAR,
                );
                cache.thumbs.insert(key, tex);
                return cache.thumbs.get(&key).map(TextureHandle::id);
            }
        }
    }
    cache.missing.insert(key);
    None
}

fn hash_str(s: &str) -> u64 {
    let mut hash = 1469598103934665603u64;
    for &b in s.as_bytes() {
        hash ^= b as u64;
        hash = hash.wrapping_mul(1099511628211);
    }
    hash
}

fn draw_wave_seekbar(
    ui: &mut egui::Ui,
    playback: &PlaybackSnapshot,
    analysis: &AnalysisSnapshot,
) -> Option<Duration> {
    let desired = Vec2::new(320.0, 24.0);
    let (rect, response) = ui.allocate_exact_size(desired, Sense::click_and_drag());
    let painter = ui.painter_at(rect);
    painter.rect(
        rect,
        3.0,
        Color32::from_gray(18),
        Stroke::new(1.0, Color32::from_gray(70)),
        StrokeKind::Middle,
    );

    let peaks = &analysis.waveform_peaks;
    if !peaks.is_empty() {
        let draw_cols = (rect.width().round() as usize).clamp(64, peaks.len());
        let src_per_col = peaks.len() as f32 / draw_cols as f32;
        let step = rect.width() / draw_cols as f32;
        let center_y = rect.center().y;
        for i in 0..draw_cols {
            let start = (i as f32 * src_per_col) as usize;
            let end = ((i as f32 + 1.0) * src_per_col) as usize;
            let end = end.max(start + 1).min(peaks.len());
            let mut amp = 0.0f32;
            for &v in &peaks[start..end] {
                if v > amp {
                    amp = v;
                }
            }

            let x = rect.left() + (i as f32 + 0.5) * step;
            let h = (amp.clamp(0.0, 1.0) * rect.height() * 0.46).max(1.0);
            painter.line_segment(
                [Pos2::new(x, center_y - h), Pos2::new(x, center_y + h)],
                Stroke::new(1.0, Color32::from_rgb(130, 220, 255)),
            );
        }
    }

    let progress = if playback.duration.is_zero() {
        0.0
    } else {
        (playback.position.as_secs_f32() / playback.duration.as_secs_f32()).clamp(0.0, 1.0)
    };
    let progress_x = rect.left() + rect.width() * progress;
    let progress_rect = Rect::from_min_max(rect.min, Pos2::new(progress_x, rect.max.y));
    painter.rect_filled(
        progress_rect,
        3.0,
        Color32::from_rgba_unmultiplied(90, 180, 255, 24),
    );
    painter.line_segment(
        [
            Pos2::new(progress_x, rect.top()),
            Pos2::new(progress_x, rect.bottom()),
        ],
        Stroke::new(1.0, Color32::from_rgb(150, 210, 255)),
    );

    if response.clicked() || response.dragged() {
        if let Some(pos) = response.interact_pointer_pos() {
            if !playback.duration.is_zero() {
                let t = ((pos.x - rect.left()) / rect.width()).clamp(0.0, 1.0);
                let target = playback.duration.mul_f32(t);
                return Some(target);
            }
        }
    }
    None
}

fn draw_spectrogram(
    ui: &mut egui::Ui,
    desired_h: f32,
    rows: &[Vec<f32>],
    seq: u64,
    sample_rate_hz: u32,
    settings: &SpectrogramUiSettings,
    cache: &mut SpectrogramCache,
) {
    update_spectrogram_fps(cache);

    let desired = Vec2::new(ui.available_width(), desired_h.max(120.0));
    let (rect, _) = ui.allocate_exact_size(desired, Sense::hover());
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, 4.0, Color32::from_gray(12));

    let no_texture_yet = cache.texture.is_none() || cache.written_cols == 0;
    if (rows.is_empty() || rows[0].is_empty()) && no_texture_yet {
        painter.text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            "spectrogram unavailable",
            egui::TextStyle::Body.resolve(ui.style()),
            Color32::from_gray(120),
        );
        return;
    }

    let ppp = ui.ctx().pixels_per_point().max(1.0);
    let tex_w = (rect.width() * ppp).round().max(256.0) as usize;
    let tex_h = (rect.height() * ppp).round().max(128.0) as usize;
    ensure_spectrogram_texture(ui, cache, tex_w, tex_h);
    update_spectrogram_texture(cache, rows, seq, sample_rate_hz, settings);

    if let Some(tex) = cache.texture.as_ref() {
        paint_ring_texture(&painter, rect, tex, cache);
    }

    painter.rect_stroke(
        rect,
        4.0,
        Stroke::new(1.0, Color32::from_gray(50)),
        StrokeKind::Middle,
    );

    let max_khz = (sample_rate_hz as f32 / 2000.0).max(1.0);
    painter.text(
        Pos2::new(rect.right() - 4.0, rect.top() + 4.0),
        egui::Align2::RIGHT_TOP,
        format!("{max_khz:.1} kHz"),
        egui::TextStyle::Small.resolve(ui.style()),
        Color32::from_gray(175),
    );
    painter.text(
        Pos2::new(rect.right() - 4.0, rect.center().y),
        egui::Align2::RIGHT_CENTER,
        format!("{:.1} kHz", max_khz * 0.5),
        egui::TextStyle::Small.resolve(ui.style()),
        Color32::from_gray(150),
    );
    painter.text(
        Pos2::new(rect.right() - 4.0, rect.bottom() - 4.0),
        egui::Align2::RIGHT_BOTTOM,
        "0.0 kHz",
        egui::TextStyle::Small.resolve(ui.style()),
        Color32::from_gray(130),
    );
    painter.text(
        Pos2::new(rect.left() + 4.0, rect.top() + 4.0),
        egui::Align2::LEFT_TOP,
        format!("{:.0} fps", cache.fps_value),
        egui::TextStyle::Small.resolve(ui.style()),
        Color32::from_gray(95),
    );
}

fn full_row_text_button(
    ui: &mut egui::Ui,
    row_w: f32,
    row_h: f32,
    text: &str,
    selected: bool,
    indent: f32,
) -> egui::Response {
    let size = Vec2::new(row_w.max(1.0), row_h.max(1.0));
    let (rect, response) = ui.allocate_exact_size(size, Sense::click());
    let rect = response.rect.intersect(rect);
    let bg = if selected {
        ui.style().visuals.selection.bg_fill
    } else if response.hovered() {
        ui.style().visuals.widgets.hovered.weak_bg_fill
    } else {
        ui.style().visuals.widgets.inactive.weak_bg_fill
    };
    ui.painter().rect_filled(rect, 2.0, bg);

    let text_color = if selected {
        ui.style().visuals.selection.stroke.color
    } else {
        ui.style().visuals.text_color()
    };
    let text_pos = Pos2::new(rect.left() + 6.0 + indent.max(0.0), rect.center().y);
    ui.painter().text(
        text_pos,
        egui::Align2::LEFT_CENTER,
        text,
        egui::TextStyle::Body.resolve(ui.style()),
        text_color,
    );
    response
}

fn update_spectrogram_fps(cache: &mut SpectrogramCache) {
    let now = Instant::now();
    if let Some(last) = cache.fps_last_instant {
        cache.fps_accum_frames = cache.fps_accum_frames.saturating_add(1);
        let elapsed = now.duration_since(last).as_secs_f32();
        if elapsed >= 0.5 {
            cache.fps_value = cache.fps_accum_frames as f32 / elapsed;
            cache.fps_accum_frames = 0;
            cache.fps_last_instant = Some(now);
        }
    } else {
        cache.fps_last_instant = Some(now);
        cache.fps_accum_frames = 0;
        cache.fps_value = 0.0;
    }
}

const DDB_GRADIENT_TABLE_SIZE: usize = 2048;
const DDB_MIN_FREQ_HZ: f32 = 25.0;
const DDB_NUM_COLORS: usize = 7;
const DDB_GRADIENT_COLORS_16: [[u16; 3]; DDB_NUM_COLORS] = [
    [65535, 65535, 65535],
    [65535, 65535, 65535],
    [65535, 63479, 0],
    [62194, 13878, 0],
    [45232, 0, 23387],
    [12336, 0, 29555],
    [1027, 256, 18247],
];

fn format_duration(d: Duration) -> String {
    let secs = d.as_secs();
    let m = secs / 60;
    let s = secs % 60;
    format!("{m:02}:{s:02}")
}

fn format_seconds(secs: f32) -> String {
    if !secs.is_finite() || secs < 0.0 {
        return "--:--".to_string();
    }
    let secs = secs.round() as u64;
    let m = secs / 60;
    let s = secs % 60;
    format!("{m:02}:{s:02}")
}

fn track_label(path: &PathBuf) -> String {
    path.file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("unknown")
        .to_owned()
}

fn duration_for_path(library: &LibrarySnapshot, path: &PathBuf) -> Option<f32> {
    library
        .tracks
        .iter()
        .find(|t| &t.path == path)
        .and_then(|t| t.duration_secs)
}

fn ellipsize(s: &str, max_chars: usize) -> String {
    if s.chars().count() <= max_chars {
        return s.to_owned();
    }
    if max_chars <= 1 {
        return "…".to_string();
    }
    let take_n = max_chars.saturating_sub(1);
    let mut out = String::new();
    for ch in s.chars().take(take_n) {
        out.push(ch);
    }
    out.push('…');
    out
}

fn deadbeef_gradient_table() -> &'static [Color32] {
    static TABLE: OnceLock<Vec<Color32>> = OnceLock::new();
    TABLE.get_or_init(|| {
        let mut out = vec![Color32::BLACK; DDB_GRADIENT_TABLE_SIZE];
        let num_segments = DDB_NUM_COLORS.saturating_sub(1).max(1);
        for (i, px) in out.iter_mut().enumerate() {
            let position = i as f32 / DDB_GRADIENT_TABLE_SIZE as f32;
            let m = num_segments as f32 * position;
            let n = m.floor() as usize;
            let f = (m - n as f32).clamp(0.0, 1.0);

            let idx0 = n.min(DDB_NUM_COLORS - 1);
            let idx1 = (n + 1).min(DDB_NUM_COLORS - 1);
            let c0 = DDB_GRADIENT_COLORS_16[idx0];
            let c1 = DDB_GRADIENT_COLORS_16[idx1];

            let r16 = c0[0] as f32 + (c1[0] as f32 - c0[0] as f32) * f;
            let g16 = c0[1] as f32 + (c1[1] as f32 - c0[1] as f32) * f;
            let b16 = c0[2] as f32 + (c1[2] as f32 - c0[2] as f32) * f;
            let scale = 255.0 / 65535.0;

            *px = Color32::from_rgb(
                (r16 * scale).round().clamp(0.0, 255.0) as u8,
                (g16 * scale).round().clamp(0.0, 255.0) as u8,
                (b16 * scale).round().clamp(0.0, 255.0) as u8,
            );
        }
        out
    })
}

fn build_column_pixels(
    height: usize,
    row: &[f32],
    sample_rate_hz: u32,
    settings: &SpectrogramUiSettings,
) -> Vec<Color32> {
    let src_bins = row.len();
    let mut col = vec![Color32::BLACK; height];
    if src_bins == 0 {
        return col;
    }
    let gradient = deadbeef_gradient_table();
    let db_range = settings.db_range.clamp(50.0, 120.0);
    let (log_index, low_res_end) = if settings.log_scale {
        let (idx, low_res_end) = build_log_index(height, src_bins, sample_rate_hz);
        (Some(idx), low_res_end)
    } else {
        (None, 0)
    };

    for y in 0..height {
        // DeaDBeeF computes i from low->high while drawing y from bottom->top.
        let i = height.saturating_sub(1).saturating_sub(y);
        let mut x_db = db_value_for_row(row, i, height, log_index.as_deref(), settings.log_scale);

        if settings.log_scale && i <= low_res_end {
            x_db = interpolate_low_res_log_bin(row, x_db, i, height, log_index.as_deref());
        }
        // Same bias as DeaDBeeF: x += db_range - 63.
        x_db = (x_db + db_range - 63.0).clamp(0.0, db_range);

        let mut color_index = DDB_GRADIENT_TABLE_SIZE as i32
            - ((DDB_GRADIENT_TABLE_SIZE as f32 / db_range) * x_db).round() as i32;
        color_index = color_index.clamp(0, DDB_GRADIENT_TABLE_SIZE as i32 - 1);
        col[y] = gradient[color_index as usize];
    }
    col
}

fn build_log_index(height: usize, src_bins: usize, sample_rate_hz: u32) -> (Vec<usize>, usize) {
    if height == 0 {
        return (Vec::new(), 0);
    }
    let nyquist = (sample_rate_hz as f32 * 0.5).max(DDB_MIN_FREQ_HZ * 1.1);
    let log_scale = (nyquist.log2() - DDB_MIN_FREQ_HZ.log2()) / height as f32;
    let freq_res =
        (sample_rate_hz as f32 / (2.0 * src_bins.saturating_sub(1).max(1) as f32)).max(1.0);

    let mut out = vec![0usize; height];
    let mut low_res_end = 0usize;
    let mut prev = None;
    for (i, idx) in out.iter_mut().enumerate() {
        let freq = 2.0_f32.powf(i as f32 * log_scale + DDB_MIN_FREQ_HZ.log2());
        let bin = (freq / freq_res).round() as isize;
        let clamped = bin.clamp(0, src_bins.saturating_sub(1) as isize) as usize;
        *idx = clamped;
        if i > 0 && prev == Some(clamped) {
            low_res_end = i;
        }
        prev = Some(clamped);
    }
    (out, low_res_end)
}

fn db_value_for_row(
    row: &[f32],
    i: usize,
    height: usize,
    log_index: Option<&[usize]>,
    log_scale: bool,
) -> f32 {
    let src_bins = row.len();
    let (bin0, bin1, bin2) = if log_scale {
        let idx = log_index.unwrap_or(&[]);
        if idx.is_empty() {
            (0i32, 0i32, 0i32)
        } else {
            let b0 = idx[i.saturating_sub(1)] as i32;
            let b1 = idx[i.min(idx.len() - 1)] as i32;
            let b2 = idx[(i + 1).min(idx.len() - 1)] as i32;
            (b0, b1, b2)
        }
    } else {
        let mut ratio = (src_bins as f32 / height.max(1) as f32).round() as i32;
        ratio = ratio.clamp(0, 1023);
        let i = i as i32;
        ((i - 1) * ratio, i * ratio, (i + 1) * ratio)
    };

    let mut index0 = bin0 + ((bin1 - bin0) as f32 / 2.0).round() as i32;
    if index0 == bin0 {
        index0 = bin1;
    }
    let mut index1 = bin1 + ((bin2 - bin1) as f32 / 2.0).round() as i32;
    if index1 == bin2 {
        index1 = bin1;
    }
    index0 = index0.clamp(0, src_bins.saturating_sub(1) as i32);
    index1 = index1.clamp(0, src_bins.saturating_sub(1) as i32);

    let f = spectrogram_get_value(row, index0 as usize, index1 as usize);
    if f > 0.0 {
        10.0 * f.log10()
    } else {
        -200.0
    }
}

fn spectrogram_get_value(row: &[f32], start: usize, end: usize) -> f32 {
    if row.is_empty() {
        return 0.0;
    }
    let end = end.min(row.len() - 1);
    if start >= end {
        return row[end].max(0.0);
    }
    let mut value = 0.0f32;
    for &v in &row[start..end] {
        if v > value {
            value = v;
        }
    }
    value
}

fn interpolate_low_res_log_bin(
    row: &[f32],
    v0_db: f32,
    i: usize,
    height: usize,
    log_index: Option<&[usize]>,
) -> f32 {
    let Some(log_index) = log_index else {
        return v0_db;
    };
    if log_index.is_empty() || i >= log_index.len() {
        return v0_db;
    }
    let target = log_index[i];

    let mut j = 0usize;
    while i + j < height && log_index[i + j] == target {
        j += 1;
    }
    let next_i = (i + j).min(height.saturating_sub(1));
    let mut v1_db = {
        let v1 = row[log_index[next_i].min(row.len().saturating_sub(1))];
        if v1 > 0.0 {
            10.0 * v1.log10()
        } else {
            -200.0
        }
    };

    let mut k: isize = 0;
    let mut span = j;
    while i as isize + k >= 0 && log_index[(i as isize + k) as usize] == target {
        span += 1;
        k -= 1;
    }
    if span <= 1 {
        return v0_db;
    }
    let mu = (1.0 / (span - 1) as f32) * (((-k) - 1) as f32);
    v1_db = v1_db.clamp(-200.0, 200.0);
    linear_interpolate(v0_db, v1_db, mu.clamp(0.0, 1.0))
}

fn linear_interpolate(y1: f32, y2: f32, mu: f32) -> f32 {
    y1 * (1.0 - mu) + y2 * mu
}

fn ensure_spectrogram_texture(
    ui: &egui::Ui,
    cache: &mut SpectrogramCache,
    width: usize,
    height: usize,
) {
    let needs_recreate = cache.texture.is_none() || cache.width != width || cache.height != height;
    if !needs_recreate {
        return;
    }

    cache.width = width;
    cache.height = height;
    cache.write_x = 0;
    cache.written_cols = 0;
    cache.filled = false;
    cache.last_seq = 0;

    let image = ColorImage::new([width, height], Color32::BLACK);
    cache.texture = Some(ui.ctx().load_texture(
        "spectrogram_texture",
        image,
        TextureOptions::NEAREST,
    ));
}

fn update_spectrogram_texture(
    cache: &mut SpectrogramCache,
    rows: &[Vec<f32>],
    seq: u64,
    sample_rate_hz: u32,
    settings: &SpectrogramUiSettings,
) {
    if cache.width == 0 || cache.height == 0 {
        return;
    }

    if seq < cache.last_seq {
        cache.last_seq = 0;
        cache.write_x = 0;
        cache.written_cols = 0;
        cache.filled = false;
        if let Some(tex) = cache.texture.as_mut() {
            tex.set(
                ColorImage::new([cache.width, cache.height], Color32::BLACK),
                TextureOptions::NEAREST,
            );
        }
    }

    if rows.is_empty() || rows[0].is_empty() {
        cache.last_seq = seq;
        return;
    }

    let seq_delta = seq.saturating_sub(cache.last_seq) as usize;
    let mut new_cols = rows.len().min(seq_delta);
    if new_cols == 0 {
        return;
    }
    new_cols = new_cols.min(cache.width);

    let start = rows.len().saturating_sub(new_cols);
    let Some(tex) = cache.texture.as_mut() else {
        return;
    };

    let incoming = &rows[start..];

    // Upload as wide strips (1-2 calls) instead of one call per column.
    let first_chunk = incoming
        .len()
        .min(cache.width.saturating_sub(cache.write_x));
    if first_chunk > 0 {
        let img = build_column_strip(
            cache.height,
            &incoming[..first_chunk],
            sample_rate_hz,
            settings,
        );
        tex.set_partial([cache.write_x, 0], img, TextureOptions::NEAREST);
        cache.write_x += first_chunk;
        if cache.write_x >= cache.width {
            cache.write_x = 0;
            cache.filled = true;
        }
        cache.written_cols = (cache.written_cols + first_chunk).min(cache.width);
    }

    let remaining = incoming.len().saturating_sub(first_chunk);
    if remaining > 0 {
        let img = build_column_strip(
            cache.height,
            &incoming[first_chunk..],
            sample_rate_hz,
            settings,
        );
        tex.set_partial([0, 0], img, TextureOptions::NEAREST);
        cache.write_x = remaining.min(cache.width);
        cache.filled = true;
        cache.written_cols = cache.width;
    }

    cache.last_seq = seq;
}

fn build_column_strip(
    height: usize,
    rows: &[Vec<f32>],
    sample_rate_hz: u32,
    settings: &SpectrogramUiSettings,
) -> ColorImage {
    let w = rows.len();
    let mut pixels = vec![Color32::BLACK; w * height];
    for (x, row) in rows.iter().enumerate() {
        let col = build_column_pixels(height, row, sample_rate_hz, settings);
        for (y, px) in col.into_iter().enumerate() {
            pixels[y * w + x] = px;
        }
    }

    ColorImage {
        size: [w, height],
        pixels,
    }
}

fn paint_ring_texture(
    painter: &egui::Painter,
    rect: Rect,
    tex: &TextureHandle,
    cache: &SpectrogramCache,
) {
    if !cache.filled {
        let fill = cache.written_cols as f32 / cache.width.max(1) as f32;
        if fill <= 0.0 {
            return;
        }
        let fill_w = rect.width() * fill;
        let dst = Rect::from_min_max(
            Pos2::new(rect.right() - fill_w, rect.top()),
            rect.right_bottom(),
        );
        painter.image(
            tex.id(),
            dst,
            Rect::from_min_max(Pos2::new(0.0, 0.0), Pos2::new(fill, 1.0)),
            Color32::WHITE,
        );
        return;
    }

    let texel = 1.0 / cache.width.max(1) as f32;
    let split = cache.write_x as f32 / cache.width as f32;
    let left_w = rect.width() * (1.0 - split);
    let left_rect = Rect::from_min_size(rect.min, Vec2::new(left_w, rect.height()));
    let right_rect = Rect::from_min_max(
        Pos2::new(left_rect.right(), rect.top()),
        Pos2::new(rect.right(), rect.bottom()),
    );

    // Oldest..newest (left segment).
    painter.image(
        tex.id(),
        left_rect,
        Rect::from_min_max(
            Pos2::new((split + texel).min(1.0), 0.0),
            Pos2::new(1.0, 1.0),
        ),
        Color32::WHITE,
    );
    // Wrapped tail.
    painter.image(
        tex.id(),
        right_rect,
        Rect::from_min_max(
            Pos2::new(0.0, 0.0),
            Pos2::new((split - texel).max(0.0), 1.0),
        ),
        Color32::WHITE,
    );
}
