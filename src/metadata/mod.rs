use std::path::PathBuf;

use crossbeam_channel::{unbounded, Receiver, Sender};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::Accessor;

#[derive(Debug, Clone, Default)]
pub struct TrackMetadata {
    pub title: String,
    pub artist: String,
    pub album: String,
    pub sample_rate_hz: Option<u32>,
    pub bitrate_kbps: Option<u32>,
    pub channels: Option<u8>,
    pub bit_depth: Option<u8>,
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
        let (req_tx, req_rx) = unbounded::<PathBuf>();
        let (event_tx, event_rx) = unbounded::<MetadataEvent>();

        std::thread::spawn(move || {
            while let Ok(path) = req_rx.recv() {
                let mut metadata = TrackMetadata::default();
                metadata.title = path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or_default()
                    .to_owned();

                if let Ok(tagged) = lofty::read_from_path(&path) {
                    let props = tagged.properties();
                    metadata.sample_rate_hz = props.sample_rate();
                    metadata.channels = props.channels().map(|v| v as u8);
                    metadata.bit_depth = props.bit_depth().map(|v| v as u8);
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
                                metadata.cover_art_rgba = Some((
                                    rgba.width() as usize,
                                    rgba.height() as usize,
                                    rgba.into_raw(),
                                ));
                            }
                        }
                    }
                }

                if metadata.cover_art_rgba.is_none() {
                    metadata.cover_art_rgba = load_folder_cover_art(&path);
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
