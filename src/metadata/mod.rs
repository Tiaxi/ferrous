use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};

use crossbeam_channel::{unbounded, Receiver, Sender};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::Accessor;

#[derive(Debug, Clone, Default)]
pub struct TrackMetadata {
    pub source_path: Option<String>,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub sample_rate_hz: Option<u32>,
    pub bitrate_kbps: Option<u32>,
    pub channels: Option<u8>,
    pub bit_depth: Option<u8>,
    pub cover_art_path: Option<String>,
    pub cover_art_rgba: Option<(usize, usize, Vec<u8>)>,
}

#[derive(Debug, Clone)]
pub enum MetadataEvent {
    Loaded(TrackMetadata),
}

pub struct MetadataService {
    tx: Sender<PathBuf>,
}

impl MetadataService {
    pub fn new() -> (Self, Receiver<MetadataEvent>) {
        Self::new_with_delay(std::time::Duration::ZERO)
    }

    pub(crate) fn new_with_delay(delay: std::time::Duration) -> (Self, Receiver<MetadataEvent>) {
        let (req_tx, req_rx) = unbounded::<PathBuf>();
        let (event_tx, event_rx) = unbounded::<MetadataEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-metadata".to_string())
            .spawn(move || {
                while let Ok(path) = req_rx.recv() {
                    let mut metadata = TrackMetadata {
                        source_path: Some(path.to_string_lossy().to_string()),
                        title: path
                            .file_stem()
                            .and_then(|s| s.to_str())
                            .unwrap_or_default()
                            .to_owned(),
                        ..TrackMetadata::default()
                    };

                    if let Ok(tagged) = lofty::read_from_path(&path) {
                        let props = tagged.properties();
                        metadata.sample_rate_hz = props.sample_rate();
                        metadata.channels = props.channels();
                        metadata.bit_depth = props.bit_depth();
                        metadata.bitrate_kbps = props.audio_bitrate();

                        if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
                            metadata.title = tag.title().map_or_else(
                                || "Unknown title".to_string(),
                                std::borrow::Cow::into_owned,
                            );
                            metadata.artist = tag.artist().map_or_else(
                                || "Unknown artist".to_string(),
                                std::borrow::Cow::into_owned,
                            );
                            metadata.album = tag.album().map_or_else(
                                || "Unknown album".to_string(),
                                std::borrow::Cow::into_owned,
                            );

                            if let Some(pic) = tag.pictures().first() {
                                if let Ok(img) = image::load_from_memory(pic.data()) {
                                    let rgba = img.to_rgba8();
                                    let width = rgba.width() as usize;
                                    let height = rgba.height() as usize;
                                    let raw = rgba.into_raw();
                                    metadata.cover_art_path =
                                        cache_embedded_cover_png(&path, width, height, &raw);
                                    metadata.cover_art_rgba = Some((width, height, raw));
                                }
                            }
                        }
                    }

                    if metadata.cover_art_rgba.is_none() {
                        metadata.cover_art_rgba = load_folder_cover_art(&path);
                    }

                    if !delay.is_zero() {
                        std::thread::sleep(delay);
                    }
                    let _ = event_tx.send(MetadataEvent::Loaded(metadata));
                }
            });

        (Self { tx: req_tx }, event_rx)
    }

    pub fn request(&self, path: PathBuf) {
        let _ = self.tx.send(path);
    }
}

fn load_folder_cover_art(track_path: &PathBuf) -> Option<(usize, usize, Vec<u8>)> {
    let dir = track_path.parent()?;
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
            if (ext == "jpg" || ext == "jpeg" || ext == "png")
                && !candidates.iter().any(|c| c == &p)
            {
                candidates.push(p);
            }
        }
    }

    for p in candidates {
        if !p.is_file() {
            continue;
        }
        if let Ok(bytes) = std::fs::read(&p) {
            if let Ok(img) = image::load_from_memory(&bytes) {
                let rgba = img.to_rgba8();
                return Some((
                    rgba.width() as usize,
                    rgba.height() as usize,
                    rgba.into_raw(),
                ));
            }
        }
    }
    None
}

fn cover_cache_dir() -> Option<PathBuf> {
    let cache_base = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| Path::new(&home).join(".cache")))?;
    Some(cache_base.join("ferrous").join("embedded_covers"))
}

fn cache_embedded_cover_png(
    track_path: &Path,
    width: usize,
    height: usize,
    rgba: &[u8],
) -> Option<String> {
    let cache_dir = cover_cache_dir()?;
    if std::fs::create_dir_all(&cache_dir).is_err() {
        return None;
    }

    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    track_path.hash(&mut hasher);
    let key = hasher.finish();
    let out_path = cache_dir.join(format!("{key:016x}.png"));

    if !out_path.is_file() {
        let dims_match = image::RgbaImage::from_raw(width as u32, height as u32, rgba.to_vec())?;
        if dims_match
            .save_with_format(&out_path, image::ImageFormat::Png)
            .is_err()
        {
            return None;
        }
    }

    Some(out_path.to_string_lossy().to_string())
}
