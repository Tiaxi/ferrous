// SPDX-License-Identifier: GPL-3.0-or-later

//! Raw AC3/DTS files: path classification, appended `APEv2` tags, frame
//! parsing, and the `GStreamer` setup that playback and analysis share.

#[cfg(any(feature = "gst", test))]
mod ac3_conceal;
mod apev2;
#[cfg(any(feature = "gst", test))]
mod bitstream;
#[cfg(any(feature = "gst", test))]
mod byte_filter;
mod discover;
#[cfg(feature = "gst")]
mod gst_setup;
#[cfg(any(feature = "gst", test))]
mod seek_index;

use std::path::Path;

#[cfg(all(test, feature = "gst"))]
pub(crate) use ac3_conceal::{
    ac3_frame_is_damaged, record_decoder_input, with_damaged_frames, SEAMED_FRAME, TEST_AC3_FRAMES,
};
#[cfg(feature = "gst")]
pub(crate) use ac3_conceal::{conceal_and_report_damaged_ac3_frames, conceal_damaged_ac3_frames};
#[cfg(test)]
pub(crate) use apev2::{build_test_apev2_tag, write_test_apev2_file};
pub(crate) use apev2::{
    edit_appended_apev2_text_items, format_ape_number_pair, read_appended_apev2_text_metadata,
    ApeTextField,
};
#[cfg(feature = "gst")]
pub(crate) use byte_filter::{
    audio_byte_range_of, filter_raw_surround_source, filter_raw_surround_source_to,
    install_raw_surround_source_probe,
};
#[cfg(feature = "gst")]
pub(crate) use discover::estimate_duration_from_bitstream;
pub(crate) use discover::probe_raw_surround_technical_details;
#[cfg(feature = "gst")]
pub(crate) use gst_setup::configure_raw_surround_decoding;
#[cfg(feature = "gst")]
pub(crate) use seek_index::index_raw_ac3_frames;
#[cfg(all(test, feature = "gst"))]
pub(crate) use seek_index::test_hooks::wait_until_indexed;

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
/// 44 DTS frames of 512 samples (about 0.51 s) of a 44.1 kHz mono tone,
/// whose frames fill whole 14-bit words, as those of DTS-CDs do:
/// `GStreamer`'s parser loses most frames that end in a padded word.
#[cfg(test)]
pub(crate) const TEST_DTS_44K: &[u8] = include_bytes!("../analysis/fixtures/raw-tone-44k.dts");
/// The samples of [`TEST_DTS_44K`].
#[cfg(all(test, feature = "gst"))]
pub(crate) const TEST_DTS_44K_FRAMES: usize = 44 * 512;

/// A DTS-CD rip of `stream`, a DTS stream in 16-bit big-endian words: a WAV
/// header that tells 16-bit stereo PCM at 44.1 kHz, digital silence, the
/// stream in the low 14 bits of little-endian words, and an appended
/// `APEv2` tag. Returns the bytes and the range of the stream within them.
///
/// The silence is long enough that the first 4 KiB the file source reads
/// hold only one frame start, as in the rips, whose frames are 4 KiB long:
/// format detection then takes the file for WAV.
#[cfg(test)]
pub(crate) fn ripped_dts_cd_bytes(stream: &[u8]) -> (Vec<u8>, std::ops::Range<usize>) {
    ripped_dts_cd_bytes_in(stream, bitstream::DtsWords::LittleEndian14)
}

/// [`ripped_dts_cd_bytes`] with the stream in the words of `layout`.
#[cfg(test)]
fn ripped_dts_cd_bytes_in(
    stream: &[u8],
    layout: bitstream::DtsWords,
) -> (Vec<u8>, std::ops::Range<usize>) {
    const SILENCE: usize = 3900;
    let stream = bitstream::repack_dts(stream, layout);
    let data_len = u32::try_from(SILENCE + stream.len()).expect("data length");
    let mut bytes = b"RIFF".to_vec();
    bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
    bytes.extend_from_slice(b"WAVEfmt ");
    // PCM, 2 channels, 44.1 kHz, 176,400 bytes a second, 4 bytes a frame,
    // 16 bits.
    for field in [16_u32, 1 | (2 << 16), 44_100, 176_400, 4 | (16 << 16)] {
        bytes.extend_from_slice(&field.to_le_bytes());
    }
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&data_len.to_le_bytes());
    bytes.resize(bytes.len() + SILENCE, 0);
    let audio = bytes.len()..bytes.len() + stream.len();
    bytes.extend_from_slice(&stream);
    bytes.extend_from_slice(&build_test_apev2_tag(&[("Title", "Tone")], true));
    (bytes, audio)
}

/// The share of the power of the mono `samples` that a tone of `frequency`
/// Hz at `rate` Hz carries: near 1 for the tone of a fixture, near 0 for
/// noise. Whole cycles of the tone give the most exact share.
#[cfg(all(test, feature = "gst"))]
pub(crate) fn tone_share(samples: &[f32], rate: u32, frequency: f64) -> f64 {
    let step = std::f64::consts::TAU * frequency / f64::from(rate);
    let (mut re, mut im, mut power) = (0.0, 0.0, 0.0);
    for (index, sample) in samples.iter().enumerate() {
        let (sample, phase) = (f64::from(*sample), step * index as f64);
        re += sample * phase.cos();
        im += sample * phase.sin();
        power += sample * sample;
    }
    2.0 * (re * re + im * im) / (samples.len() as f64 * power)
}

/// The largest change between adjacent samples of the mono `samples`.
#[cfg(all(test, feature = "gst"))]
pub(crate) fn largest_step(samples: &[f32]) -> f32 {
    samples
        .windows(2)
        .fold(0.0, |step, pair| step.max((pair[1] - pair[0]).abs()))
}

/// Assert that `samples` of a decoded `TEST_AC3` hold no concealment burst.
///
/// The tone peaks at 1/8 of full scale and is 440 Hz at 48 kHz, so it steps
/// by at most 0.008. A burst need not be louder: what gives it away is the
/// step at each seam of the short repeats, of up to twice the tone's level.
/// The bounds leave room for the tone to jump in phase where a frame repeats,
/// which the overlap of the decoder's transform spreads over a block.
#[cfg(all(test, feature = "gst"))]
pub(crate) fn assert_no_burst(name: &str, samples: &[f32]) {
    const MAX_PEAK: f32 = 0.2;
    const MAX_STEP: f32 = 0.02;
    let peak = samples
        .iter()
        .fold(0.0, |peak: f32, sample| peak.max(sample.abs()));
    assert!(peak <= MAX_PEAK, "{name}: peak {peak}");
    let step = largest_step(samples);
    assert!(step <= MAX_STEP, "{name}: a step of {step}");
}

/// A raw surround stream as ripped from a disc: the end of the previous
/// track's last frame, the stream, and an appended `APEv2` tag. Returns the
/// bytes and the range of the stream within them.
#[cfg(test)]
pub(crate) fn ripped_surround_bytes(stream: &[u8]) -> (Vec<u8>, std::ops::Range<usize>) {
    ripped_surround_bytes_after_junk(0, stream)
}

/// [`ripped_surround_bytes`] with `junk_len` zero bytes before the partial
/// frame, which the filter trims with it.
#[cfg(test)]
pub(crate) fn ripped_surround_bytes_after_junk(
    junk_len: usize,
    stream: &[u8],
) -> (Vec<u8>, std::ops::Range<usize>) {
    let partial_frame = &stream[stream.len() - 100..];
    let comment = "Ripped from disc. ".repeat(40);
    let mut bytes = vec![0; junk_len];
    bytes.extend_from_slice(partial_frame);
    bytes.extend_from_slice(stream);
    let audio = junk_len + partial_frame.len()..bytes.len();
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
