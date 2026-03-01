use std::path::PathBuf;

use crossbeam_channel::{unbounded, Receiver, Sender};
use lofty::file::TaggedFileExt;
use lofty::prelude::Accessor;

#[derive(Debug, Clone, Default)]
pub struct TrackMetadata {
    pub title: String,
    pub artist: String,
    pub album: String,
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
                    if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
                        metadata.title = tag
                            .title()
                            .map(|v| v.into_owned())
                            .unwrap_or_else(|| "Unknown title".to_string());
                        metadata.artist = tag
                            .artist()
                            .map(|v| v.into_owned())
                            .unwrap_or_else(|| "Unknown artist".to_string());
                        metadata.album = tag
                            .album()
                            .map(|v| v.into_owned())
                            .unwrap_or_else(|| "Unknown album".to_string());

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

                let _ = event_tx.send(MetadataEvent::Loaded(metadata));
            }
        });

        (Self { tx: req_tx }, event_rx)
    }

    pub fn request(&self, path: PathBuf) {
        let _ = self.tx.send(path);
    }
}
