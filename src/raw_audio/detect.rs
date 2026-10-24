// SPDX-License-Identifier: GPL-3.0-or-later

//! Format detection setup for `GStreamer`.

use gstreamer as gst;
use std::sync::Once;

use super::bitstream::{find_ac3_sync, find_dts_sync};

/// Disable `GStreamer`'s APE tag handling so that raw surround files with
/// appended `APEv2` tags are not misidentified as `application/x-apetag`.
///
/// We handle `APEv2` tags ourselves via [`read_appended_apev2_text_metadata`].
/// The `GStreamer` APE typefinder + `apedemux` combo causes crashes and decode
/// failures for AC3/DTS files because `apedemux` strips the tag but cannot
/// identify the remaining audio content.
///
/// We also register backup typefinders for AC3/DTS that check the audio sync
/// words at byte 0, ensuring correct type detection even if the built-in
/// typefinders are not installed.
pub(crate) fn register_raw_surround_typefinders() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        // Disable the APE tag typefinder so files are never identified as
        // application/x-apetag.
        {
            use gst::prelude::PluginFeatureExtManual;

            // Disable typefinders whose caps mention APE tags so raw
            // surround files (AC3/DTS) with appended APEv2 tags are never
            // mis-typed as application/x-apetag.  The source-setup byte-
            // level probe already strips APE tags before the typefinder
            // runs, but disabling the typefinder is a defence-in-depth
            // measure.
            //
            // NOTE: `apedemux` itself is left at its default rank.  Other
            // formats (notably MP3) legitimately embed APE tags alongside
            // ID3, and `parsebin` needs `apedemux` to strip them.  For
            // AC3/DTS the source probe handles removal at the byte level.
            for factory in gst::TypeFindFactory::factories() {
                let dominated_by_ape = factory.caps().is_some_and(|c| {
                    c.to_string().contains("apetag") || c.to_string().contains("x-ape")
                });
                if dominated_by_ape {
                    factory.set_rank(gst::Rank::NONE);
                }
            }

            // Demote liba52's a52dec in favour of avdec_ac3 (FFmpeg).
            // a52dec occasionally misparses exponent/scale-factor data in
            // AC3 frames, producing a brief full-bandwidth noise burst
            // (peak values millions of times above full-scale).
            if let Some(factory) = gst::ElementFactory::find("a52dec") {
                factory.set_rank(gst::Rank::NONE);
            }
        }

        // Register AC3/DTS typefinders.  AC3 files ripped from DVDs often
        // start with a partial frame (leftover bytes from the previous
        // track's last frame), so we scan up to 2 kB for the sync word.
        let ac3_caps = gst::Caps::builder("audio/x-ac3").build();
        let _ = gst::TypeFind::register(
            None,
            "ferrous-ac3-typefind",
            gst::Rank::PRIMARY + 256,
            None,
            Some(&ac3_caps),
            |tf| {
                // AC3 frame size is at most 3840 bytes; scan 2 kB which
                // covers any partial leading frame.
                let scan_len: u32 = 2048;
                if let Some(data) = tf.peek(0, scan_len) {
                    if find_ac3_sync(data).is_some() {
                        tf.suggest(
                            gst::TypeFindProbability::Maximum,
                            &gst::Caps::builder("audio/x-ac3").build(),
                        );
                    }
                }
            },
        );

        let dts_caps = gst::Caps::builder("audio/x-dts").build();
        let _ = gst::TypeFind::register(
            None,
            "ferrous-dts-typefind",
            gst::Rank::PRIMARY + 256,
            None,
            Some(&dts_caps),
            |tf| {
                let scan_len: u32 = 8192; // DTS frames can be large
                if let Some(data) = tf.peek(0, scan_len) {
                    if find_dts_sync(data).is_some() {
                        tf.suggest(
                            gst::TypeFindProbability::Maximum,
                            &gst::Caps::builder("audio/x-dts").build(),
                        );
                    }
                }
            },
        );
    });
}
