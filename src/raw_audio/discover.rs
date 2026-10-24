// SPDX-License-Identifier: GPL-3.0-or-later

//! Technical details of raw AC3/DTS files from the `GStreamer` Discoverer,
//! with a bitstream estimate when it reports no duration.

#[cfg(any(feature = "gst", test))]
use std::fs::File;
#[cfg(any(feature = "gst", test))]
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

#[cfg(feature = "gst")]
use gstreamer as gst;
#[cfg(feature = "gst")]
use gstreamer_pbutils as gst_pbutils;

#[cfg(any(feature = "gst", test))]
use super::apev2::{has_ape_preamble, read_apev2_footer, APE_TAG_HEADER_BYTES_U64};
#[cfg(any(feature = "gst", test))]
use super::bitstream::{find_ac3_sync, find_dts_sync, parse_ac3_bitrate, parse_dts_bitrate};
#[cfg(any(feature = "gst", test))]
use super::is_dts_file;
#[cfg(feature = "gst")]
use super::{is_raw_surround_file, raw_surround_format_label, register_raw_surround_typefinders};

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
    register_raw_surround_typefinders();

    let uri = url::Url::from_file_path(path).ok()?.to_string();
    let timeout = gst::ClockTime::from_seconds(10);
    let discoverer = gst_pbutils::Discoverer::new(timeout).ok()?;
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
                 this may be caused by an appended APEv2 tag confusing the demuxer; \
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

/// Estimate duration from the bitstream frame header bitrate and file size,
/// subtracting the `APEv2` tag if present.  Works for AC3 (A/52) and DTS files.
// Duration estimate from byte count — f64 precision loss is negligible.
#[allow(clippy::cast_precision_loss)]
#[cfg(any(feature = "gst", test))]
fn estimate_duration_from_bitstream(path: &Path) -> Option<f32> {
    let mut file = File::open(path).ok()?;
    let file_len = file.seek(SeekFrom::End(0)).ok()?;

    // Determine audio-only size by subtracting the APEv2 tag.
    let apev2_size = read_apev2_footer(&mut file, file_len).map_or(0, |footer| {
        let tag_size = u64::from(footer.tag_size);
        // tag_size covers items + footer; check for a separate header.
        let with_header = tag_size.saturating_add(APE_TAG_HEADER_BYTES_U64);
        if let Some(header_start) = file_len.checked_sub(with_header) {
            if has_ape_preamble(&mut file, header_start) {
                return with_header;
            }
        }
        tag_size
    });
    let audio_bytes = file_len.saturating_sub(apev2_size);
    if audio_bytes == 0 {
        return None;
    }

    // Read enough bytes to scan for the first valid frame header.
    // AC3 files ripped from DVDs may start with a partial frame
    // (up to ~1792 bytes of leftover data from the previous track).
    file.seek(SeekFrom::Start(0)).ok()?;
    let scan_len = usize::try_from(2048u64.min(audio_bytes)).unwrap_or(2048);
    let mut buf = vec![0u8; scan_len];
    file.read_exact(&mut buf).ok()?;

    let bitrate_bps = if is_dts_file(path) {
        let offset = find_dts_sync(&buf)?;
        parse_dts_bitrate(&buf[offset..])?
    } else {
        let offset = find_ac3_sync(&buf)?;
        parse_ac3_bitrate(&buf[offset..])?
    };

    if bitrate_bps == 0 {
        return None;
    }

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
    use super::super::apev2::build_test_apev2_tag;
    use crate::raw_audio::test_path;

    #[test]
    fn estimate_duration_from_ac3_with_apev2() {
        // Build a fake AC3 file: valid header + padding + APEv2 tag
        let path = test_path("ac3-duration", "ac3");

        // AC3 header: 0x0B77, CRC, fscod=00 frmsizecod=26 → index 13 → 320 kbps
        let ac3_header = [0x0B, 0x77, 0x00, 0x00, 0b00_011010];
        let audio_size: usize = 320_000; // 320 kB of audio → 8 seconds at 320 kbps
        let mut data = Vec::with_capacity(audio_size + 200);
        data.extend_from_slice(&ac3_header);
        data.resize(audio_size, 0x00);
        let tag = build_test_apev2_tag(&[("Title", "Test")], true);
        data.extend_from_slice(&tag);

        std::fs::write(&path, &data).expect("write test file");

        let duration = super::estimate_duration_from_bitstream(&path);
        assert!(duration.is_some(), "should estimate duration");
        let d = duration.unwrap();
        // audio_size = 320000 bytes, bitrate = 320000 bps → duration = 320000*8/320000 = 8.0s
        assert!((d - 8.0).abs() < 0.1, "expected ~8.0s, got {d}");

        let _ = std::fs::remove_file(path);
    }
}
