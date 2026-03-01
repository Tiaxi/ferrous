use std::time::Duration;
use std::{ffi::OsStr, path::PathBuf};

use eframe::egui::{
    self, Color32, ColorImage, Pos2, Rect, Sense, Stroke, StrokeKind, TextureHandle,
    TextureOptions, Vec2,
};

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
}

#[derive(Default)]
pub struct CenterPanelAction {
    pub queue_play_index: Option<usize>,
    pub scan_library_folder: bool,
    pub play_library_track: Option<PathBuf>,
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
            ui.label(format!("{state} | {title} {artist}"));
        });
    });

    action
}

pub fn draw_center_panel(
    ctx: &egui::Context,
    analysis: &AnalysisSnapshot,
    metadata: &TrackMetadata,
    queue: &[PathBuf],
    current: Option<&PathBuf>,
    library: &LibrarySnapshot,
    spectrogram_cache: &mut SpectrogramCache,
) -> CenterPanelAction {
    let mut action = CenterPanelAction::default();
    egui::CentralPanel::default().show(ctx, |ui| {
        let top_h = (ui.available_height() * 0.52).clamp(220.0, 420.0);
        ui.allocate_ui_with_layout(
            Vec2::new(ui.available_width(), top_h),
            egui::Layout::left_to_right(egui::Align::Min),
            |ui| {
                let left_w = 260.0_f32.min(ui.available_width() * 0.35);
                ui.allocate_ui(Vec2::new(left_w, top_h), |ui| {
                    ui.heading("Library");
                    ui.separator();
                    let cover_h = (top_h * 0.58).max(120.0);
                    let (cover_rect, _) = ui.allocate_exact_size(
                        Vec2::new(ui.available_width(), cover_h),
                        Sense::hover(),
                    );
                    let painter = ui.painter_at(cover_rect);
                    painter.rect_filled(cover_rect, 2.0, Color32::from_gray(35));
                    painter.text(
                        cover_rect.center(),
                        egui::Align2::CENTER_CENTER,
                        if metadata.cover_art_rgba.is_some() {
                            "Cover art loaded"
                        } else {
                            "No cover art"
                        },
                        egui::TextStyle::Body.resolve(ui.style()),
                        Color32::from_gray(180),
                    );

                    ui.separator();
                    ui.label(format!("Title: {}", fallback(&metadata.title, "Unknown")));
                    ui.label(format!("Artist: {}", fallback(&metadata.artist, "Unknown")));
                    ui.label(format!("Album: {}", fallback(&metadata.album, "Unknown")));
                    ui.add_space(8.0);
                    ui.horizontal(|ui| {
                        if ui.button("Scan Folder").clicked() && !library.scan_in_progress {
                            action.scan_library_folder = true;
                        }
                        ui.label(format!("Indexed: {}", library.tracks.len()));
                    });
                    if library.scan_in_progress {
                        ui.label(format!("Scanning... {} files", library.scanned_files));
                    }
                    if let Some(err) = library.last_error.as_ref() {
                        ui.colored_label(Color32::from_rgb(200, 70, 70), err);
                    }
                    ui.separator();
                    ui.label("Indexed Folders");
                    egui::ScrollArea::vertical()
                        .max_height((top_h * 0.18).max(50.0))
                        .show(ui, |ui| {
                            if library.roots.is_empty() {
                                ui.label("No folders added");
                                return;
                            }
                            for root in &library.roots {
                                ui.label(root.display().to_string());
                            }
                        });
                    ui.separator();
                    ui.label("Library Tracks");
                    egui::ScrollArea::vertical()
                        .max_height((top_h * 0.34).max(80.0))
                        .show(ui, |ui| {
                            if library.tracks.is_empty() {
                                ui.label("No tracks indexed");
                                return;
                            }
                            for track in &library.tracks {
                                let mut text = if track.artist.is_empty() {
                                    track.title.clone()
                                } else {
                                    format!("{} - {}", track.artist, track.title)
                                };
                                if !track.album.is_empty() {
                                    text.push_str(&format!("  [{}]", track.album));
                                }
                                if let Some(secs) = track.duration_secs {
                                    text.push_str(&format!(
                                        "  {:02}:{:02}",
                                        (secs as u64) / 60,
                                        (secs as u64) % 60
                                    ));
                                }
                                let resp = ui.selectable_label(false, text);
                                if resp.double_clicked() {
                                    action.play_library_track = Some(track.path.clone());
                                }
                            }
                        });
                });

                ui.separator();

                ui.allocate_ui(Vec2::new(ui.available_width(), top_h), |ui| {
                    ui.heading("Playlist");
                    ui.separator();
                    egui::ScrollArea::vertical().show(ui, |ui| {
                        if queue.is_empty() {
                            ui.label("Queue is empty");
                            return;
                        }

                        for (idx, path) in queue.iter().enumerate() {
                            let is_current = current.map(|p| p == path).unwrap_or(false);
                            let mut text = format!("{:02}  {}", idx + 1, track_label(path));
                            if is_current {
                                text.push_str("   ▶");
                            }
                            let resp = ui.selectable_label(is_current, text);
                            if resp.double_clicked() || resp.clicked() {
                                action.queue_play_index = Some(idx);
                            }
                        }
                    });
                });
            },
        );

        ui.separator();

        draw_spectrogram(
            ui,
            &analysis.spectrogram_rows,
            analysis.spectrogram_seq,
            analysis.sample_rate_hz,
            spectrogram_cache,
        );
    });
    action
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
    rows: &[Vec<f32>],
    seq: u64,
    sample_rate_hz: u32,
    cache: &mut SpectrogramCache,
) {
    let desired_h = 360.0_f32.min(ui.available_height().max(180.0));
    let desired = Vec2::new(ui.available_width(), desired_h);
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
    update_spectrogram_texture(cache, rows, seq);

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
}

fn spectrogram_color(value: f32) -> Color32 {
    let t = (value.clamp(0.0, 1.0) * 0.94).clamp(0.0, 1.0);
    // Inferno-ish gradient to match DeaDBeeF/Audition style.
    gradient(
        t,
        &[
            (0.00, [0x00, 0x00, 0x00]),
            (0.10, [0x0d, 0x0a, 0x6b]),
            (0.28, [0x3b, 0x0f, 0x70]),
            (0.48, [0x7a, 0x15, 0x6a]),
            (0.66, [0xb5, 0x2c, 0x4d]),
            (0.80, [0xd9, 0x55, 0x22]),
            (0.91, [0xee, 0x97, 0x16]),
            (0.97, [0xf3, 0xcf, 0x3a]),
            (1.00, [0xfa, 0xec, 0xc2]),
        ],
    )
}

fn format_duration(d: Duration) -> String {
    let secs = d.as_secs();
    let m = secs / 60;
    let s = secs % 60;
    format!("{m:02}:{s:02}")
}

fn fallback<'a>(value: &'a str, alt: &'a str) -> &'a str {
    if value.is_empty() {
        alt
    } else {
        value
    }
}

fn track_label(path: &PathBuf) -> String {
    path.file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("unknown")
        .to_owned()
}

fn gradient(t: f32, stops: &[(f32, [u8; 3])]) -> Color32 {
    for window in stops.windows(2) {
        let (t0, c0) = window[0];
        let (t1, c1) = window[1];
        if t >= t0 && t <= t1 {
            let span = (t1 - t0).max(1e-6);
            let u = (t - t0) / span;
            let r = c0[0] as f32 + (c1[0] as f32 - c0[0] as f32) * u;
            let g = c0[1] as f32 + (c1[1] as f32 - c0[1] as f32) * u;
            let b = c0[2] as f32 + (c1[2] as f32 - c0[2] as f32) * u;
            return Color32::from_rgb(r as u8, g as u8, b as u8);
        }
    }

    let [r, g, b] = stops.last().map(|(_, c)| *c).unwrap_or([0, 0, 0]);
    Color32::from_rgb(r, g, b)
}

fn build_column_pixels(height: usize, row: &[f32]) -> Vec<Color32> {
    let src_bins = row.len().max(1);
    let mut col = vec![Color32::BLACK; height];
    for y in 0..height {
        // Linear frequency axis from 0..Nyquist.
        let yf = y as f32 / (height.saturating_sub(1).max(1)) as f32;
        let src_yf = (1.0 - yf) * (src_bins.saturating_sub(1)) as f32;
        let src_y0 = src_yf.floor() as usize;
        let src_y1 = (src_y0 + 1).min(src_bins - 1);
        let frac = (src_yf - src_y0 as f32).clamp(0.0, 1.0);
        let v0 = row[src_y0.min(src_bins - 1)];
        let v1 = row[src_y1];
        let v = (v0 * (1.0 - frac) + v1 * frac).clamp(0.0, 1.0);
        col[y] = spectrogram_color(v);
    }
    col
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
        TextureOptions::LINEAR,
    ));
}

fn update_spectrogram_texture(cache: &mut SpectrogramCache, rows: &[Vec<f32>], seq: u64) {
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
                TextureOptions::LINEAR,
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
        let img = build_column_strip(cache.height, &incoming[..first_chunk]);
        tex.set_partial([cache.write_x, 0], img, TextureOptions::LINEAR);
        cache.write_x += first_chunk;
        if cache.write_x >= cache.width {
            cache.write_x = 0;
            cache.filled = true;
        }
        cache.written_cols = (cache.written_cols + first_chunk).min(cache.width);
    }

    let remaining = incoming.len().saturating_sub(first_chunk);
    if remaining > 0 {
        let img = build_column_strip(cache.height, &incoming[first_chunk..]);
        tex.set_partial([0, 0], img, TextureOptions::LINEAR);
        cache.write_x = remaining.min(cache.width);
        cache.filled = true;
        cache.written_cols = cache.width;
    }

    cache.last_seq = seq;
}

fn build_column_strip(height: usize, rows: &[Vec<f32>]) -> ColorImage {
    let w = rows.len();
    let mut pixels = vec![Color32::BLACK; w * height];
    for (x, row) in rows.iter().enumerate() {
        let col = build_column_pixels(height, row);
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
