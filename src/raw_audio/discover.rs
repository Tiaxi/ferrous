// SPDX-License-Identifier: GPL-3.0-or-later

//! Technical details of raw AC3/DTS files from the `GStreamer` Discoverer,
//! with a bitstream estimate when it reports no duration.

use std::path::Path;

#[cfg(feature = "gst")]
use gstreamer as gst;
#[cfg(feature = "gst")]
use gstreamer_pbutils as gst_pbutils;

#[cfg(any(feature = "gst", test))]
use super::bitstream::{parse_ac3_bitrate, parse_dts_bitrate, Codec};
#[cfg(any(feature = "gst", test))]
use super::byte_filter::read_audio_layout;
#[cfg(feature = "gst")]
use super::{
    configure_raw_surround_decoding, filter_raw_surround_source, is_raw_surround_file,
    raw_surround_format_label,
};

#[derive(Debug, Clone, Default, PartialEq)]
pub(crate) struct RawAudioTechnicalDetails {
    pub(crate) duration_secs: Option<f32>,
    pub(crate) sample_rate_hz: Option<u32>,
    pub(crate) bitrate_kbps: Option<u32>,
    pub(crate) channels: Option<u8>,
    pub(crate) bit_depth: Option<u8>,
    pub(crate) current_bitrate_kbps: Option<u32>,
    pub(crate) format_label: String,
}

#[cfg(feature = "gst")]
fn round_f64_to_u32(value: f64) -> Option<u32> {
    if !value.is_finite() || value < 0.0 {
        return None;
    }
    value
        .round()
        .clamp(0.0, f64::from(u32::MAX))
        .to_string()
        .parse::<u32>()
        .ok()
}

#[cfg(feature = "gst")]
pub(crate) fn probe_raw_surround_technical_details(
    path: &Path,
) -> Option<RawAudioTechnicalDetails> {
    if !is_raw_surround_file(path) {
        return None;
    }

    gst::init().ok()?;
    configure_raw_surround_decoding();

    let uri = url::Url::from_file_path(path).ok()?.to_string();
    let timeout = gst::ClockTime::from_seconds(10);
    let discoverer = gst_pbutils::Discoverer::new(timeout).ok()?;
    discoverer.connect_source_setup(|_, source| filter_raw_surround_source(source));
    match discoverer.discover_uri(&uri) {
        Ok(info) => {
            let audio_info = info.audio_streams().into_iter().next()?;

            let bitrate_kbps = kbps_from_bits_per_second(audio_info.bitrate())
                .or_else(|| kbps_from_bits_per_second(audio_info.max_bitrate()));

            let mut details = RawAudioTechnicalDetails {
                duration_secs: info
                    .duration()
                    .map(gst::ClockTime::nseconds)
                    .map(std::time::Duration::from_nanos)
                    .map(|duration| duration.as_secs_f32())
                    .filter(|value| *value > 0.0),
                sample_rate_hz: Some(audio_info.sample_rate()).filter(|value| *value > 0),
                bitrate_kbps,
                channels: u8::try_from(audio_info.channels())
                    .ok()
                    .filter(|value| *value > 0),
                bit_depth: u8::try_from(audio_info.depth())
                    .ok()
                    .filter(|value| *value > 0),
                current_bitrate_kbps: bitrate_kbps,
                format_label: raw_surround_format_label(path),
            };

            if details.duration_secs.is_none() {
                eprintln!(
                    "[ferrous] GStreamer Discoverer returned no duration for {}, \
                     trying bitstream header fallback",
                    path.display()
                );
                details.duration_secs = estimate_duration_from_bitstream(path);
            }

            Some(details)
        }
        Err(err) => {
            eprintln!(
                "[ferrous] GStreamer Discoverer failed for {}: {} — \
                 trying bitstream header fallback",
                path.display(),
                err
            );
            let duration_secs = estimate_duration_from_bitstream(path);
            Some(RawAudioTechnicalDetails {
                duration_secs,
                format_label: raw_surround_format_label(path),
                ..RawAudioTechnicalDetails::default()
            })
        }
    }
}

#[cfg(not(feature = "gst"))]
pub(crate) fn probe_raw_surround_technical_details(
    _path: &Path,
) -> Option<RawAudioTechnicalDetails> {
    None
}

/// Estimate duration from the bitrate in the first frame header and the
/// length of the frames, which excludes a leading partial frame and the
/// `APEv2` tag.  Works for AC3 (A/52) and DTS files.
// Duration estimate from byte count — f64 precision loss is negligible.
#[allow(clippy::cast_precision_loss)]
#[cfg(any(feature = "gst", test))]
fn estimate_duration_from_bitstream(path: &Path) -> Option<f32> {
    let layout = read_audio_layout(path)?;
    let first_frame = layout.first_frame?;
    let header = layout.head.get(usize::try_from(first_frame).ok()?..)?;
    let bitrate_bps = match Codec::of_path(path) {
        Codec::Dts => parse_dts_bitrate(header)?,
        Codec::Ac3 => parse_ac3_bitrate(header)?,
    };
    if bitrate_bps == 0 {
        return None;
    }

    let audio_bytes = layout.audio_end - first_frame;
    let duration = (audio_bytes as f64 * 8.0) / f64::from(bitrate_bps);
    // f64 → f32 for duration display — intentional.
    #[allow(clippy::cast_possible_truncation)]
    let duration = duration as f32;
    (duration > 0.0).then_some(duration)
}

#[cfg(feature = "gst")]
fn kbps_from_bits_per_second(bits_per_second: u32) -> Option<u32> {
    (bits_per_second > 0)
        .then(|| round_f64_to_u32(f64::from(bits_per_second) / 1000.0))
        .flatten()
}

#[cfg(test)]
mod tests {
    use crate::raw_audio::{ripped_surround_bytes, test_path, TEST_AC3, TEST_DTS};

    #[test]
    fn duration_estimate_counts_only_the_frames_of_ripped_files() {
        // 32 frames of 1536 samples at 48 kHz, and 47 frames of 512.
        for (stream, ext, expected) in [(TEST_AC3, "ac3", 1.024), (TEST_DTS, "dts", 0.501)] {
            let path = test_path("duration", ext);
            let (bytes, _) = ripped_surround_bytes(stream);
            std::fs::write(&path, bytes).expect("write test file");

            let duration = super::estimate_duration_from_bitstream(&path).expect("duration");
            assert!(
                (duration - expected).abs() < 0.01,
                "{ext}: expected {expected} s, got {duration} s"
            );
            let _ = std::fs::remove_file(path);
        }
    }

    #[cfg(feature = "gst")]
    #[test]
    fn discoverer_reads_ripped_files() {
        use super::probe_raw_surround_technical_details;

        // Upper case gives the typefinders no extension hint.
        for (stream, ext, length) in [(TEST_AC3, "AC3", 1.024), (TEST_DTS, "DTS", 0.501)] {
            let path = test_path("discover", ext);
            let (bytes, _) = ripped_surround_bytes(stream);
            std::fs::write(&path, bytes).expect("write test file");

            let details = probe_raw_surround_technical_details(&path).expect("details");
            let _ = std::fs::remove_file(path);
            assert_eq!(details.sample_rate_hz, Some(48_000), "{ext}: {details:?}");
            assert_eq!(details.channels, Some(1), "{ext}: {details:?}");
            let duration = details.duration_secs.expect("duration");
            assert!((duration - length).abs() < 0.005, "{ext}: {details:?}");
        }
    }
}
