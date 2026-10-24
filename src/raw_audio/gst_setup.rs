// SPDX-License-Identifier: GPL-3.0-or-later

//! Process-wide `GStreamer` setup for raw AC3/DTS files.
//!
//! Format detection is left to `GStreamer`'s own typefinders. They accept
//! leading partial frames, require two consecutive frames, and give lower
//! certainty when the first frame is not at the start. A typefind function
//! sees only bytes, never the file name, and is registered for every file
//! the process opens, so a custom one cannot be limited to `.ac3`/`.dts`
//! files: a typefinder's extension list only moves it to the front of the
//! queue, and its rank only orders it. What made the built-in typefinders
//! lose before was the appended `APEv2` tag, which the tag typefinder
//! reports with maximum certainty. The byte filter hides that tag from
//! every pipeline that reads raw surround files, by path, so no override of
//! the typefinders is needed; see [`super::byte_filter`].

use gstreamer as gst;
use std::sync::Once;

/// Prefer `FFmpeg`'s AC3 decoder.  Call before building a pipeline that can
/// decode AC3.
pub(crate) fn configure_raw_surround_decoding() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        use gst::prelude::PluginFeatureExtManual;

        // Demote liba52's a52dec in favour of avdec_ac3 (FFmpeg).
        // a52dec occasionally misparses exponent/scale-factor data in
        // AC3 frames, producing a brief full-bandwidth noise burst
        // (peak values millions of times above full-scale).
        if let Some(factory) = gst::ElementFactory::find("a52dec") {
            factory.set_rank(gst::Rank::NONE);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::configure_raw_surround_decoding;
    use crate::raw_audio::{
        filter_raw_surround_source, ripped_surround_bytes, test_path, TEST_AC3, TEST_DTS,
    };
    use gst::prelude::*;
    use gstreamer as gst;
    use std::sync::{Arc, Mutex};

    const AC3_SYNC: &[u8] = &[0x0B, 0x77];
    const DTS_SYNC: &[u8] = &[0x7F, 0xFE, 0x80, 0x01];
    /// `GST_TYPE_FIND_POSSIBLE`.
    const TYPE_FIND_POSSIBLE: u32 = 50;

    /// Media type and certainty of `bytes` as found by the `typefind`
    /// element that `decodebin` and `parsebin` use, reading the file in pull
    /// mode. `filtered` adds the byte filter, as the app's pipelines do.
    fn detect(name: &str, ext: &str, bytes: &[u8], filtered: bool) -> Option<(String, u32)> {
        gst::init().expect("gst init");
        configure_raw_surround_decoding();
        let path = test_path(name, ext);
        std::fs::write(&path, bytes).expect("write file");
        let pipeline = gst::Pipeline::new();
        let source = gst::ElementFactory::make("filesrc")
            .property("location", path.to_string_lossy().to_string())
            .build()
            .expect("filesrc");
        if filtered {
            filter_raw_surround_source(&source);
        }
        let typefind = gst::ElementFactory::make("typefind")
            .build()
            .expect("typefind");
        let detected = Arc::new(Mutex::new(None));
        let found = Arc::clone(&detected);
        typefind.connect("have-type", false, move |values| {
            let probability = values.get(1)?.get::<u32>().ok()?;
            let caps = values.get(2)?.get::<gst::Caps>().ok()?;
            let media_type = caps.structure(0)?.name().to_string();
            *found.lock().ok()? = Some((media_type, probability));
            None
        });
        let sink = gst::ElementFactory::make("fakesink")
            .build()
            .expect("fakesink");
        pipeline
            .add_many([&source, &typefind, &sink])
            .expect("add elements");
        gst::Element::link_many([&source, &typefind, &sink]).expect("link elements");
        let _ = pipeline.set_state(gst::State::Paused);
        let _ = pipeline.state(gst::ClockTime::from_seconds(5));
        let _ = pipeline.set_state(gst::State::Null);
        let _ = std::fs::remove_file(&path);
        let detected = detected.lock().expect("detected type").take();
        detected
    }

    fn detected_type_of(name: &str, ext: &str, bytes: &[u8]) -> Option<String> {
        detect(name, ext, bytes, false).map(|(media_type, _)| media_type)
    }

    fn plant(mut bytes: Vec<u8>, at: usize, sync: &[u8]) -> Vec<u8> {
        bytes[at..at + sync.len()].copy_from_slice(sync);
        bytes
    }

    /// Deterministic noise, like compressed audio payload.
    fn noise(len: usize, mut seed: u32) -> Vec<u8> {
        (0..len)
            .map(|_| {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                seed.to_le_bytes()[0]
            })
            .collect()
    }

    /// An MPEG-1 Layer III stream without tags.
    fn untagged_mp3() -> Vec<u8> {
        let mp3 = crate::tag_write::TEST_MP3;
        // Skip the ID3v2 tag: a 10-byte header and a syncsafe size.
        let size = mp3[6..10]
            .iter()
            .fold(0usize, |size, byte| (size << 7) | usize::from(*byte));
        mp3[10 + size..].to_vec()
    }

    /// 20 ADTS frames (AAC LC, 48 kHz, mono) of 200 bytes each.
    fn raw_adts() -> Vec<u8> {
        const FRAME: usize = 200;
        let mut bytes = Vec::new();
        for index in 0..20 {
            let header = [
                0xFF,
                0xF1,
                0x4C,
                0x40 | u8::try_from(FRAME >> 11).expect("length"),
                u8::try_from((FRAME >> 3) & 0xFF).expect("length"),
                u8::try_from((FRAME & 0x07) << 5).expect("length") | 0x1F,
                0xFC,
            ];
            bytes.extend_from_slice(&header);
            bytes.extend(noise(FRAME - header.len(), 0x9E37_79B9 ^ index));
        }
        bytes
    }

    // Compressed audio holds any 2-byte value about once per 64 KiB, so
    // about one file in 30 has the AC3 sync word in its first 2 KiB.
    #[test]
    fn mp3_and_adts_streams_with_sync_bytes_are_not_detected_as_surround() {
        let mp3 = untagged_mp3();
        assert_eq!(
            detected_type_of("mp3", "MP3", &mp3).as_deref(),
            Some("audio/mpeg")
        );
        let adts = raw_adts();
        assert_eq!(
            detected_type_of("adts", "AAC", &adts).as_deref(),
            Some("audio/mpeg")
        );
        for sync in [AC3_SYNC, DTS_SYNC] {
            // Inside the payload of the second frame of each stream.
            assert_eq!(
                detected_type_of("mp3-sync", "MP3", &plant(mp3.clone(), 300, sync)).as_deref(),
                Some("audio/mpeg"),
                "MP3 with {sync:02x?}"
            );
            assert_eq!(
                detected_type_of("adts-sync", "AAC", &plant(adts.clone(), 300, sync)).as_deref(),
                Some("audio/mpeg"),
                "ADTS with {sync:02x?}"
            );
        }
    }

    #[test]
    fn random_data_with_sync_bytes_is_not_detected_as_surround() {
        for (name, ext) in [("noise", "bin"), ("noise-ac3", "ac3"), ("noise-dts", "dts")] {
            for sync in [AC3_SYNC, DTS_SYNC] {
                let noise = plant(noise(16 * 1024, 7), 100, sync);
                let detected = detect(name, ext, &noise, true);
                // GStreamer's DTS typefinder guesses DTS for a lone frame
                // header with its lowest certainty, which nothing else
                // outranks for noise; a real format always does.
                let surround = match detected.as_ref() {
                    Some((media_type, _)) if media_type == "audio/x-ac3" => true,
                    Some((media_type, probability)) if media_type == "audio/x-dts" => {
                        *probability > TYPE_FIND_POSSIBLE
                    }
                    _ => false,
                };
                assert!(
                    !surround,
                    "noise.{ext} with {sync:02x?} detected as {detected:?}"
                );
            }
        }
    }

    #[test]
    fn ripped_surround_files_are_detected_through_the_byte_filter() {
        // Upper-case extensions give the typefinders no hint.
        for (stream, ext, expected) in [
            (TEST_AC3, "ac3", "audio/x-ac3"),
            (TEST_AC3, "AC3", "audio/x-ac3"),
            (TEST_DTS, "dts", "audio/x-dts"),
            (TEST_DTS, "DTS", "audio/x-dts"),
        ] {
            let (bytes, _) = ripped_surround_bytes(stream);
            let detected = detect("ripped", ext, &bytes, true);
            assert_eq!(
                detected.as_ref().map(|(media_type, _)| media_type.as_str()),
                Some(expected),
                "ripped .{ext}: {detected:?}"
            );
        }
    }

    #[test]
    fn monkeys_audio_files_are_still_detected() {
        let mut ape = b"MAC ".to_vec();
        ape.extend_from_slice(&3990u16.to_le_bytes());
        ape.extend(noise(8 * 1024, 11));
        assert_eq!(
            detected_type_of("monkeys-audio", "ape", &ape).as_deref(),
            Some("application/x-ape")
        );
    }
}
