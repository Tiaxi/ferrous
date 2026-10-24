// SPDX-License-Identifier: GPL-3.0-or-later

//! Raw AC3/DTS files: path classification, appended `APEv2` tags, frame
//! parsing, and the `GStreamer` setup that playback and analysis share.

mod apev2;
#[cfg(any(feature = "gst", test))]
mod bitstream;
#[cfg(any(feature = "gst", test))]
mod byte_filter;
#[cfg(feature = "gst")]
mod detect;
mod discover;

use std::path::Path;

#[cfg(all(test, feature = "gst"))]
pub(crate) use apev2::build_test_apev2_tag;
#[cfg(test)]
pub(crate) use apev2::write_test_apev2_file;
pub(crate) use apev2::{
    edit_appended_apev2_text_items, format_ape_number_pair, read_appended_apev2_text_metadata,
    ApeTextField,
};
#[cfg(feature = "gst")]
pub(crate) use byte_filter::install_raw_surround_source_probe;
#[cfg(feature = "gst")]
pub(crate) use detect::register_raw_surround_typefinders;
pub(crate) use discover::probe_raw_surround_technical_details;

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
