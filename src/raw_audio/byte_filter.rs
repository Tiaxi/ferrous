// SPDX-License-Identifier: GPL-3.0-or-later

//! Byte filter that hands `GStreamer` only the audio frames of a raw AC3/DTS
//! file: no leading partial frame and no appended `APEv2` tag.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
#[cfg(feature = "gst")]
use std::path::PathBuf;
#[cfg(feature = "gst")]
use std::sync::atomic::{AtomicU64, Ordering};

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

use super::apev2::{has_ape_preamble, read_apev2_footer, APE_TAG_HEADER_BYTES_U64};
use super::bitstream::{find_ac3_sync, find_dts_sync};
use super::{is_dts_file, is_raw_surround_file};

/// Byte range of clean audio within a raw surround file: first sync word
/// through end of audio (before `APEv2` tag).  Returns `None` for non-raw
/// surround files or when there is nothing to strip.
pub(crate) fn audio_byte_range(path: &Path) -> Option<(u64, u64)> {
    if !is_raw_surround_file(path) {
        return None;
    }
    let mut file = File::open(path).ok()?;
    let file_len = file.seek(SeekFrom::End(0)).ok()?;

    // --- audio end: subtract APEv2 tag if present ---
    let audio_end = match read_apev2_footer(&mut file, file_len) {
        Some(footer) => {
            let tag_size = u64::from(footer.tag_size);
            let with_header = tag_size.saturating_add(APE_TAG_HEADER_BYTES_U64);
            let total = if file_len
                .checked_sub(with_header)
                .is_some_and(|hs| has_ape_preamble(&mut file, hs))
            {
                with_header
            } else {
                tag_size
            };
            file_len.saturating_sub(total)
        }
        None => file_len,
    };

    // --- audio start: first sync word (skip partial leading frame) ---
    file.seek(SeekFrom::Start(0)).ok()?;
    let scan_len = usize::try_from(audio_end.min(8192)).unwrap_or(8192);
    let mut buf = vec![0u8; scan_len];
    file.read_exact(&mut buf).ok()?;
    let sync_offset = if is_dts_file(path) {
        find_dts_sync(&buf).unwrap_or(0)
    } else {
        find_ac3_sync(&buf).unwrap_or(0)
    };
    let audio_start = sync_offset as u64;

    if audio_start == 0 && audio_end == file_len {
        return None; // nothing to strip
    }
    if audio_start >= audio_end {
        return None;
    }
    Some((audio_start, audio_end))
}

/// Install a `source-setup` handler on playbin that attaches a pad
/// probe to each source element feeding a raw surround file.  The
/// probe strips leading partial-frame bytes (before the first sync
/// word) and trailing `APEv2` tag bytes, so the parser only receives
/// clean, sync-aligned audio data.
#[cfg(feature = "gst")]
pub(crate) fn install_raw_surround_source_probe(playbin: &gst::Element) {
    playbin.connect("source-setup", false, |values| {
        let source = values.get(1)?.get::<gst::Element>().ok()?;
        if !source.has_property("location") {
            return None;
        }
        let location = source.property_value("location").get::<String>().ok()?;
        let path = PathBuf::from(&location);
        let (audio_start, audio_end) = audio_byte_range(&path)?;

        let src_pad = source.static_pad("src")?;
        let bytes_seen = AtomicU64::new(0);
        src_pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, info| {
            let buf_size = match info.buffer() {
                Some(buf) => buf.size() as u64,
                None => return gst::PadProbeReturn::Ok,
            };
            let offset = bytes_seen.fetch_add(buf_size, Ordering::Relaxed);
            let buf_end = offset + buf_size;

            // Entirely outside the audio region → drop.
            if buf_end <= audio_start || offset >= audio_end {
                return gst::PadProbeReturn::Drop;
            }

            // Buffer spans the front boundary (leading partial frame).
            if offset < audio_start {
                let skip = usize::try_from(audio_start - offset).unwrap_or(0);
                let tail = usize::try_from(buf_end.min(audio_end) - audio_start).unwrap_or(0);
                if tail == 0 {
                    return gst::PadProbeReturn::Drop;
                }
                if let Some(buf) = info.buffer_mut() {
                    // Replace with a sub-region copy that skips the
                    // leading bytes — can't just set_size here because
                    // the unwanted bytes are at the front.
                    if let Ok(sub) = buf.copy_region(gst::BufferCopyFlags::all(), skip..skip + tail)
                    {
                        *buf = sub;
                    }
                }
                return gst::PadProbeReturn::Ok;
            }

            // Buffer spans the back boundary (APEv2 tag region).
            if buf_end > audio_end {
                let keep = usize::try_from(audio_end - offset).unwrap_or(0);
                if keep == 0 {
                    return gst::PadProbeReturn::Drop;
                }
                if let Some(buf) = info.buffer_mut() {
                    buf.make_mut().set_size(keep);
                }
            }

            gst::PadProbeReturn::Ok
        });
        None
    });
}

#[cfg(test)]
mod tests {
    use super::audio_byte_range;
    use crate::raw_audio::apev2::build_test_apev2_tag;
    use crate::raw_audio::test_path;
    use std::path::Path;

    #[test]
    fn audio_byte_range_strips_leading_garbage_and_apev2_tag() {
        let path = test_path("audio-byte-range", "ac3");

        // 16 bytes of leading garbage, then AC3 sync word + audio, then tag.
        let leading_garbage = [0xFFu8; 16];
        let ac3_header = [0x0B, 0x77, 0x00, 0x00, 0b00_011010];
        let audio_payload_size: usize = 4096;
        let mut data = Vec::new();
        data.extend_from_slice(&leading_garbage);
        data.extend_from_slice(&ac3_header);
        data.resize(leading_garbage.len() + audio_payload_size, 0x00);
        let tag = build_test_apev2_tag(&[("Title", "Test")], true);
        data.extend_from_slice(&tag);

        std::fs::write(&path, &data).expect("write test file");

        let range = audio_byte_range(&path);
        let (start, end) = range.expect("should return a range");
        assert_eq!(start, leading_garbage.len() as u64);
        assert_eq!(end, (leading_garbage.len() + audio_payload_size) as u64);

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn audio_byte_range_returns_none_for_non_surround_file() {
        assert!(audio_byte_range(Path::new("/tmp/test.flac")).is_none());
    }

    #[test]
    fn audio_byte_range_returns_none_when_nothing_to_strip() {
        // AC3 file with sync word at byte 0 and no APEv2 tag → nothing to strip.
        let path = test_path("audio-byte-range-clean", "ac3");
        let ac3_header = [0x0B, 0x77, 0x00, 0x00, 0b00_011010];
        let mut data = vec![0u8; 4096];
        data[..ac3_header.len()].copy_from_slice(&ac3_header);
        std::fs::write(&path, &data).expect("write test file");

        assert!(audio_byte_range(&path).is_none());

        let _ = std::fs::remove_file(path);
    }
}
