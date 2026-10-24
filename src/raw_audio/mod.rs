// SPDX-License-Identifier: GPL-3.0-or-later

//! Raw AC3/DTS files: path classification, appended `APEv2` tags, frame
//! parsing, and the `GStreamer` setup that playback and analysis share.

mod apev2;
#[cfg(any(feature = "gst", test))]
mod bitstream;
#[cfg(any(feature = "gst", test))]
mod byte_filter;
mod discover;
#[cfg(feature = "gst")]
mod gst_setup;

use std::path::Path;

#[cfg(test)]
pub(crate) use apev2::{build_test_apev2_tag, write_test_apev2_file};
pub(crate) use apev2::{
    edit_appended_apev2_text_items, format_ape_number_pair, read_appended_apev2_text_metadata,
    ApeTextField,
};
#[cfg(feature = "gst")]
pub(crate) use byte_filter::{filter_raw_surround_source, install_raw_surround_source_probe};
pub(crate) use discover::probe_raw_surround_technical_details;
#[cfg(feature = "gst")]
pub(crate) use gst_setup::configure_raw_surround_decoding;

pub(crate) fn is_raw_surround_file(path: &Path) -> bool {
    matches!(raw_surround_extension(path).as_deref(), Some("ac3" | "dts"))
}

#[cfg(any(feature = "gst", test))]
pub(crate) fn is_dts_file(path: &Path) -> bool {
    matches!(raw_surround_extension(path).as_deref(), Some("dts"))
}

#[cfg(any(feature = "gst", test))]
pub(crate) fn same_surround_extension(a: &Path, b: &Path) -> bool {
    is_raw_surround_file(a)
        && is_raw_surround_file(b)
        && raw_surround_extension(a) == raw_surround_extension(b)
}

fn raw_surround_extension(path: &Path) -> Option<String> {
    let ext = path.extension().and_then(|value| value.to_str())?;
    Some(ext.to_ascii_lowercase())
}

pub(crate) fn raw_surround_format_label(path: &Path) -> String {
    match raw_surround_extension(path).as_deref().unwrap_or_default() {
        "ac3" => "AC3".to_string(),
        "dts" => "DTS".to_string(),
        _ => String::new(),
    }
}

/// 32 AC3 frames (1.024 s) of a 48 kHz mono tone; see `fixtures/README.md`.
#[cfg(test)]
pub(crate) const TEST_AC3: &[u8] = include_bytes!("../analysis/fixtures/raw-tone.ac3");
/// 47 DTS frames (about 0.5 s) of a 48 kHz mono tone.
#[cfg(test)]
pub(crate) const TEST_DTS: &[u8] = include_bytes!("../analysis/fixtures/raw-tone.dts");

/// A raw surround stream as ripped from a disc: the end of the previous
/// track's last frame, the stream, and an appended `APEv2` tag. Returns the
/// bytes and the range of the stream within them.
#[cfg(test)]
pub(crate) fn ripped_surround_bytes(stream: &[u8]) -> (Vec<u8>, std::ops::Range<usize>) {
    let partial_frame = &stream[stream.len() - 100..];
    let comment = "Ripped from disc. ".repeat(40);
    let mut bytes = partial_frame.to_vec();
    bytes.extend_from_slice(stream);
    let audio = partial_frame.len()..bytes.len();
    bytes.extend_from_slice(&build_test_apev2_tag(
        &[("Title", "Tone"), ("Comment", &comment)],
        true,
    ));
    (bytes, audio)
}

#[cfg(test)]
fn test_path(name: &str, ext: &str) -> std::path::PathBuf {
    let mut path = std::env::temp_dir();
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|value| value.as_nanos())
        .unwrap_or(0);
    path.push(format!(
        "ferrous-raw-audio-{name}-{}-{nanos}.{ext}",
        std::process::id()
    ));
    path
}

#[cfg(test)]
mod tests {
    use super::raw_surround_format_label;
    use std::path::Path;

    #[test]
    fn raw_surround_format_labels_match_extensions() {
        assert_eq!(raw_surround_format_label(Path::new("a.ac3")), "AC3");
        assert_eq!(raw_surround_format_label(Path::new("a.dts")), "DTS");
    }

    #[test]
    fn same_surround_extension_matches_identical_formats() {
        use super::same_surround_extension;

        assert!(same_surround_extension(
            Path::new("/a/track.ac3"),
            Path::new("/b/other.ac3")
        ));
        assert!(same_surround_extension(
            Path::new("/a/track.dts"),
            Path::new("/b/other.dts")
        ));
    }

    #[test]
    fn same_surround_extension_rejects_different_surround_formats() {
        use super::same_surround_extension;

        assert!(!same_surround_extension(
            Path::new("/a/track.ac3"),
            Path::new("/b/other.dts")
        ));
    }

    #[test]
    fn same_surround_extension_rejects_surround_vs_non_surround() {
        use super::same_surround_extension;

        assert!(!same_surround_extension(
            Path::new("/a/track.ac3"),
            Path::new("/b/other.flac")
        ));
    }

    #[test]
    fn same_surround_extension_rejects_non_surround_pair() {
        use super::same_surround_extension;

        // Neither file is a raw surround file.
        assert!(!same_surround_extension(
            Path::new("/a/track.flac"),
            Path::new("/b/other.flac")
        ));
    }
}
