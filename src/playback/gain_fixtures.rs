// SPDX-License-Identifier: GPL-3.0-or-later

//! Fixtures with `ReplayGain` tags for the tests of the gain stage: audio
//! that `GStreamer` encodes, tagged by `lofty` as a tagger would.

use std::path::{Path, PathBuf};

use gstreamer as gst;
use gstreamer::prelude::*;
use lofty::config::WriteOptions;
use lofty::ogg::VorbisComments;
use lofty::prelude::TagExt;
use lofty::tag::{ItemKey, Tag, TagType};

/// A mono PCM WAV file of `samples` at `rate` Hz, with `bits` of 16 or 24
/// bits per sample.
pub(crate) fn wav_bytes(rate: u32, bits: u16, samples: &[i32]) -> Vec<u8> {
    let sample_size = u32::from(bits / 8);
    let data_len = u32::try_from(samples.len()).unwrap() * sample_size;
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"RIFF");
    bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
    bytes.extend_from_slice(b"WAVEfmt ");
    bytes.extend_from_slice(&16u32.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&rate.to_le_bytes());
    bytes.extend_from_slice(&(rate * sample_size).to_le_bytes());
    bytes.extend_from_slice(&(bits / 8).to_le_bytes());
    bytes.extend_from_slice(&bits.to_le_bytes());
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&data_len.to_le_bytes());
    for sample in samples {
        bytes.extend_from_slice(&sample.to_le_bytes()[..usize::from(bits / 8)]);
    }
    bytes
}

/// The AAC encoder that this system has: FFmpeg's from the libav plugin,
/// which CI and the package's build install, or the FDK's.
fn aac_encoder() -> &'static str {
    gst::init().unwrap();
    ["avenc_aac", "fdkaacenc"]
        .into_iter()
        .find(|name| gst::ElementFactory::find(name).is_some())
        .expect("an AAC encoder: install gstreamer1-plugin-libav")
}

/// The formats a fixture can be encoded in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Encoding {
    Wav,
    Flac,
    Vorbis,
    Opus,
    Mp3,
    M4a,
}

impl Encoding {
    fn pipeline(self) -> String {
        match self {
            Self::Wav => "wavenc".to_owned(),
            Self::Flac => "flacenc seekpoints=0".to_owned(),
            Self::Vorbis => "vorbisenc ! oggmux".to_owned(),
            Self::Opus => "opusenc ! oggmux".to_owned(),
            Self::Mp3 => "lamemp3enc".to_owned(),
            Self::M4a => format!("{} ! mp4mux", aac_encoder()),
        }
    }
}

/// Encode the WAV file `wav` as `encoding` into `dir/name`.
pub(crate) fn encode(dir: &Path, name: &str, wav: &[u8], encoding: Encoding) -> PathBuf {
    let source = dir.join(format!("{name}.source.wav"));
    std::fs::write(&source, wav).unwrap();
    let path = dir.join(name);
    gst::init().unwrap();
    let pipeline = gst::parse::launch(&format!(
        "filesrc location=\"{}\" ! wavparse ! audioconvert ! audioresample ! {} \
         ! filesink location=\"{}\"",
        source.display(),
        encoding.pipeline(),
        path.display()
    ))
    .expect("encoder pipeline");
    pipeline.set_state(gst::State::Playing).unwrap();
    let message = pipeline.bus().unwrap().timed_pop_filtered(
        gst::ClockTime::from_seconds(30),
        &[gst::MessageType::Eos, gst::MessageType::Error],
    );
    pipeline.set_state(gst::State::Null).unwrap();
    assert!(
        matches!(
            message.as_ref().map(|message| message.view()),
            Some(gst::MessageView::Eos(_))
        ),
        "{message:?}"
    );
    std::fs::remove_file(source).unwrap();
    path
}

/// The `ReplayGain` tags a tagger such as rsgain writes.
pub(crate) fn replay_gain_items<'a>(
    track_gain: &'a str,
    track_peak: &'a str,
    album_gain: &'a str,
    album_peak: &'a str,
) -> [(ItemKey, &'a str); 4] {
    [
        (ItemKey::ReplayGainTrackGain, track_gain),
        (ItemKey::ReplayGainTrackPeak, track_peak),
        (ItemKey::ReplayGainAlbumGain, album_gain),
        (ItemKey::ReplayGainAlbumPeak, album_peak),
    ]
}

/// Write `items` into the file at `path` as a tag of `tag_type`.
pub(crate) fn write_tag(path: &Path, tag_type: TagType, items: &[(ItemKey, &str)]) {
    let mut tag = Tag::new(tag_type);
    for (key, value) in items {
        assert!(
            tag.insert_text(*key, (*value).to_owned()),
            "{tag_type:?} has no {key:?}"
        );
    }
    tag.save_to_path(path, WriteOptions::default())
        .unwrap_or_else(|error| panic!("tagging {}: {error}", path.display()));
}

/// Write an Opus file's R128 gains, as Q7.8 numbers of dB.
pub(crate) fn write_r128(path: &Path, track_gain: i16, album_gain: i16) {
    let mut comments = VorbisComments::default();
    comments.insert("R128_TRACK_GAIN".to_owned(), track_gain.to_string());
    comments.insert("R128_ALBUM_GAIN".to_owned(), album_gain.to_string());
    comments
        .save_to_path(path, WriteOptions::default())
        .unwrap_or_else(|error| panic!("tagging {}: {error}", path.display()));
}
