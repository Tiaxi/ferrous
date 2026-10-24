// SPDX-License-Identifier: GPL-3.0-or-later

//! Byte filter that hands `GStreamer` only the audio frames of a raw AC3/DTS
//! file: no leading partial frame and no appended `APEv2` tag.
//!
//! The filter is a pair of probes on the file source. The source offers only
//! push mode, so every reader, format detection included, receives the file
//! through the buffer probe. That probe places each buffer by the byte
//! offset `filesrc` stamps on it, because seeks restart reading anywhere.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::ops::Range;
use std::path::Path;

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

use super::apev2::appended_tag_len;
use super::bitstream::{first_frame_offset, Codec};
use super::is_raw_surround_file;

/// Bytes read from the start of a file to find its first frame: room for a
/// leading partial frame and two of the longest frames after it.
const FRAME_SCAN_BYTES: u64 = 16 * 1024;

/// Where the frames of a raw surround file lie.
pub(super) struct AudioLayout {
    pub(super) file_len: u64,
    /// Offset of the first frame, if one was found.
    pub(super) first_frame: Option<u64>,
    /// End of the frames: the start of the appended tag, or the file end.
    pub(super) audio_end: u64,
    /// The first bytes of the file, up to [`FRAME_SCAN_BYTES`] and not past
    /// the end of the frames.
    pub(super) head: Vec<u8>,
}

pub(super) fn read_audio_layout(path: &Path) -> Option<AudioLayout> {
    let mut file = File::open(path).ok()?;
    let file_len = file.seek(SeekFrom::End(0)).ok()?;
    let audio_end = file_len - appended_tag_len(&mut file, file_len);
    file.seek(SeekFrom::Start(0)).ok()?;
    let mut head = Vec::new();
    file.take(audio_end.min(FRAME_SCAN_BYTES))
        .read_to_end(&mut head)
        .ok()?;
    let first_frame = first_frame_offset(&head, Codec::of_path(path)).map(|offset| offset as u64);
    Some(AudioLayout {
        file_len,
        first_frame,
        audio_end,
        head,
    })
}

/// Byte range of clean audio within a raw surround file: first frame
/// through end of audio (before `APEv2` tag).  Returns `None` for non-raw
/// surround files or when there is nothing to strip.
pub(crate) fn audio_byte_range(path: &Path) -> Option<(u64, u64)> {
    if !is_raw_surround_file(path) {
        return None;
    }
    let layout = read_audio_layout(path)?;
    let audio_start = layout.first_frame.unwrap_or(0);
    if audio_start == 0 && layout.audio_end == layout.file_len {
        return None; // nothing to strip
    }
    (audio_start < layout.audio_end).then_some((audio_start, layout.audio_end))
}

/// What the filter does with one buffer from the source.
#[derive(Debug, PartialEq, Eq)]
enum Trim {
    Keep,
    Drop,
    /// Keep only these bytes of the buffer.
    Slice(Range<usize>),
}

/// Decide what to keep of `len` bytes read at `offset`, given the audio
/// range of the file.
///
/// Pushed buffers form one stream, so both ends can be cut. The source
/// offers no pull mode, but should a reader pull anyway: a pulled buffer must
/// hold the bytes at the offset that was asked for, so it keeps the leading
/// partial frame for the parser to skip, and dropping one ends the stream.
fn trim(offset: u64, len: usize, (audio_start, audio_end): (u64, u64), pulled: bool) -> Trim {
    let end = offset.saturating_add(len as u64);
    let keep_start = if pulled {
        offset
    } else {
        offset.max(audio_start)
    };
    let keep_end = end.min(audio_end);
    if keep_start >= keep_end {
        return Trim::Drop;
    }
    if keep_start == offset && keep_end == end {
        return Trim::Keep;
    }
    // Both bounds lie within the buffer, so they fit its length.
    let to_index = |position: u64| usize::try_from(position - offset).unwrap_or(len);
    Trim::Slice(to_index(keep_start)..to_index(keep_end))
}

#[cfg(feature = "gst")]
fn filter_buffer(info: &mut gst::PadProbeInfo, range: (u64, u64)) -> gst::PadProbeReturn {
    let pulled = info.mask.contains(gst::PadProbeType::PULL);
    let Some(buffer) = info.buffer_mut() else {
        return gst::PadProbeReturn::Ok;
    };
    let offset = buffer.offset();
    // Without an offset the buffer's place in the file is unknown. Passing
    // it on loses no audio; the parser skips tag bytes it cannot sync to.
    if offset == gst::format::Buffers::OFFSET_NONE {
        return gst::PadProbeReturn::Ok;
    }
    match trim(offset, buffer.size(), range, pulled) {
        Trim::Keep => gst::PadProbeReturn::Ok,
        Trim::Drop => gst::PadProbeReturn::Drop,
        Trim::Slice(kept) => {
            slice_buffer(buffer, offset, kept);
            gst::PadProbeReturn::Ok
        }
    }
}

#[cfg(feature = "gst")]
fn slice_buffer(buffer: &mut gst::Buffer, offset: u64, kept: Range<usize>) {
    let start = offset + kept.start as u64;
    let end = offset + kept.end as u64;
    let buffer = buffer.make_mut();
    if buffer.resize(kept).is_ok() {
        buffer.set_offset(start);
        buffer.set_offset_end(end);
    }
}

/// Filter the buffers of a file source when it reads a raw surround file
/// with a leading partial frame or an appended tag.
///
/// Use this for every pipeline that reads such files. Hidden tags also keep
/// format detection from taking the file for an `APEv2` tag stream.
#[cfg(feature = "gst")]
pub(crate) fn filter_raw_surround_source(source: &gst::Element) {
    if !source.has_property("location") {
        return;
    }
    let Ok(Some(location)) = source.property_value("location").get::<Option<String>>() else {
        return;
    };
    let Some(range) = audio_byte_range(Path::new(&location)) else {
        return;
    };
    let Some(pad) = source.static_pad("src") else {
        return;
    };
    pad.add_probe(gst::PadProbeType::QUERY_UPSTREAM, move |_pad, info| {
        answer_query(info, range)
    });
    pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, info| {
        filter_buffer(info, range)
    });
}

/// Answer queries about the source as if the file held only the frames.
///
/// The scheduling answer is the one `filesrc` gives, without pull mode.
/// Readers that pull call the source's read function directly to detect the
/// format, which bypasses pad probes, and a pulled read cannot drop the
/// leading partial frame. Pushed buffers all pass the filter, so format
/// detection sees no tag and a stream that starts with a whole frame.
///
/// The byte length leaves out the tag, which parsers would otherwise count
/// as audio when they estimate the duration.
#[cfg(feature = "gst")]
fn answer_query(
    info: &mut gst::PadProbeInfo,
    (audio_start, audio_end): (u64, u64),
) -> gst::PadProbeReturn {
    let Some(query) = info.query_mut() else {
        return gst::PadProbeReturn::Ok;
    };
    match query.view_mut() {
        gst::QueryViewMut::Scheduling(scheduling) => {
            scheduling.set(gst::SchedulingFlags::SEEKABLE, 1, -1, 0);
            scheduling.add_scheduling_modes([gst::PadMode::Push]);
            gst::PadProbeReturn::Handled
        }
        gst::QueryViewMut::Duration(duration) if duration.format() == gst::Format::Bytes => {
            duration.set(gst::format::Bytes::from_u64(audio_end - audio_start));
            gst::PadProbeReturn::Handled
        }
        _ => gst::PadProbeReturn::Ok,
    }
}

/// Filter the source of every file that `playbin` opens.
#[cfg(feature = "gst")]
pub(crate) fn install_raw_surround_source_probe(playbin: &gst::Element) {
    playbin.connect("source-setup", false, |values| {
        if let Some(source) = values
            .get(1)
            .and_then(|value| value.get::<gst::Element>().ok())
        {
            filter_raw_surround_source(&source);
        }
        None
    });
}

#[cfg(test)]
mod tests {
    use super::{audio_byte_range, trim, Trim};
    use crate::raw_audio::{ripped_surround_bytes, test_path, TEST_AC3, TEST_DTS};
    use std::path::Path;

    #[test]
    fn audio_byte_range_strips_leading_partial_frame_and_apev2_tag() {
        for (stream, ext) in [(TEST_AC3, "ac3"), (TEST_DTS, "DTS")] {
            let path = test_path("audio-byte-range", ext);
            let (bytes, audio) = ripped_surround_bytes(stream);
            std::fs::write(&path, &bytes).expect("write test file");

            assert_eq!(
                audio_byte_range(&path),
                Some((audio.start as u64, audio.end as u64)),
                "{ext}"
            );
            let _ = std::fs::remove_file(path);
        }
    }

    #[test]
    fn audio_byte_range_skips_sync_bytes_in_the_partial_frame() {
        // The partial frame holds a sync word whose frame is not followed
        // by another one.
        let (mut bytes, audio) = ripped_surround_bytes(TEST_AC3);
        bytes[10..12].copy_from_slice(&[0x0B, 0x77]);
        bytes[14] = TEST_AC3[4];
        bytes[15] = TEST_AC3[5];
        let path = test_path("audio-byte-range-false-sync", "ac3");
        std::fs::write(&path, &bytes).expect("write test file");

        assert_eq!(
            audio_byte_range(&path),
            Some((audio.start as u64, audio.end as u64))
        );
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn audio_byte_range_returns_none_for_non_surround_file() {
        assert!(audio_byte_range(Path::new("/tmp/test.flac")).is_none());
    }

    #[test]
    fn audio_byte_range_returns_none_when_nothing_to_strip() {
        let path = test_path("audio-byte-range-clean", "ac3");
        std::fs::write(&path, TEST_AC3).expect("write test file");

        assert!(audio_byte_range(&path).is_none());

        let _ = std::fs::remove_file(path);
    }

    const RANGE: (u64, u64) = (100, 1000);

    #[test]
    fn pulled_buffers_are_cut_at_the_audio_end_only() {
        assert_eq!(trim(0, 4096, RANGE, true), Trim::Slice(0..1000));
        assert_eq!(trim(0, 50, RANGE, true), Trim::Keep);
        assert_eq!(trim(500, 400, RANGE, true), Trim::Keep);
        assert_eq!(trim(900, 4096, RANGE, true), Trim::Slice(0..100));
        assert_eq!(trim(1000, 32, RANGE, true), Trim::Drop);
        assert_eq!(trim(4000, 32, RANGE, true), Trim::Drop);
    }

    #[test]
    fn pushed_buffers_are_cut_at_both_ends() {
        assert_eq!(trim(0, 50, RANGE, false), Trim::Drop);
        assert_eq!(trim(0, 100, RANGE, false), Trim::Drop);
        assert_eq!(trim(0, 4096, RANGE, false), Trim::Slice(100..1000));
        assert_eq!(trim(50, 100, RANGE, false), Trim::Slice(50..100));
        assert_eq!(trim(100, 900, RANGE, false), Trim::Keep);
        assert_eq!(trim(600, 4096, RANGE, false), Trim::Slice(0..400));
        assert_eq!(trim(1000, 1, RANGE, false), Trim::Drop);
    }

    #[test]
    fn buffers_are_placed_by_their_offset_not_by_arrival() {
        // A seek back reads the start again after the end was read.
        for pulled in [true, false] {
            assert_eq!(trim(4000, 32, RANGE, pulled), Trim::Drop);
            assert_eq!(trim(200, 300, RANGE, pulled), Trim::Keep);
        }
    }
}

#[cfg(all(test, feature = "gst"))]
mod gst_tests {
    use super::install_raw_surround_source_probe;
    use crate::playback::pipeline_trace::PipelineTrace;
    use crate::raw_audio::{configure_raw_surround_decoding, ripped_surround_bytes, test_path};
    use crate::raw_audio::{TEST_AC3, TEST_DTS};
    use gst::prelude::*;
    use gstreamer as gst;
    use std::collections::VecDeque;
    use std::ops::Range;
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Mutex};
    use std::time::Duration;

    /// A buffer that left a source after the filter.
    #[derive(Debug)]
    struct Delivered {
        path: PathBuf,
        offset: u64,
        bytes: Vec<u8>,
    }

    /// What playback from a seek target, or through a queue, produced.
    #[derive(Debug, Default)]
    struct Run {
        /// Decoded buffers as (start, duration).
        decoded: Vec<(Duration, Duration)>,
        delivered: Vec<Delivered>,
    }

    impl Run {
        fn first_start(&self) -> Duration {
            self.decoded
                .first()
                .map_or(Duration::MAX, |(start, _)| *start)
        }

        fn last_end(&self) -> Duration {
            self.decoded
                .last()
                .map_or(Duration::ZERO, |(start, length)| *start + *length)
        }

        fn total_decoded(&self) -> Duration {
            self.decoded.iter().map(|(_, length)| *length).sum()
        }

        /// Assert that every file was read and that no byte outside its
        /// audio left its source.
        fn assert_only_audio_delivered(&self, files: &[(PathBuf, Range<usize>)]) {
            for (path, audio) in files {
                let name = path.display();
                let delivered: Vec<_> = self.delivered.iter().filter(|d| d.path == *path).collect();
                assert!(!delivered.is_empty(), "{name}: nothing left the source");
                for Delivered { offset, bytes, .. } in delivered {
                    let end = offset + bytes.len() as u64;
                    assert!(
                        *offset >= audio.start as u64,
                        "{name}: bytes {offset}..{end} before the first frame at {} reached the parser",
                        audio.start
                    );
                    assert!(
                        end <= audio.end as u64,
                        "{name}: bytes {offset}..{end} after the audio end {} reached the parser",
                        audio.end
                    );
                    assert!(
                        !bytes.windows(8).any(|window| window == b"APETAGEX"),
                        "{name}: APEv2 tag bytes reached the parser"
                    );
                }
            }
        }
    }

    /// `playbin3` with the app's source filter, a fake audio sink, and
    /// recorders for what the sources deliver and what is decoded.
    struct Player {
        playbin: gst::Element,
        decoded: Arc<Mutex<Vec<(Duration, Duration)>>>,
        delivered: Arc<Mutex<Vec<Delivered>>>,
        trace: PipelineTrace,
    }

    impl Player {
        fn new() -> Self {
            gst::init().expect("gst init");
            configure_raw_surround_decoding();
            let playbin = gst::ElementFactory::make("playbin3")
                .build()
                .expect("playbin3");
            install_raw_surround_source_probe(&playbin);
            let player = Self {
                trace: PipelineTrace::new(&playbin),
                playbin,
                decoded: Arc::default(),
                delivered: Arc::default(),
            };
            player.record_delivered_buffers();
            player
                .playbin
                .set_property("audio-sink", player.decoded_sink());
            player
        }

        fn record_delivered_buffers(&self) {
            let delivered = Arc::clone(&self.delivered);
            // Connected after the filter, so this probe runs after the filter's.
            self.playbin.connect("source-setup", false, move |values| {
                let source = values.get(1)?.get::<gst::Element>().ok()?;
                let path = PathBuf::from(source.property::<Option<String>>("location")?);
                let delivered = Arc::clone(&delivered);
                source.static_pad("src")?.add_probe(
                    gst::PadProbeType::BUFFER,
                    move |_pad, info| {
                        if let Some(buffer) = info.buffer() {
                            if let (Ok(map), Ok(mut delivered)) =
                                (buffer.map_readable(), delivered.lock())
                            {
                                delivered.push(Delivered {
                                    path: path.clone(),
                                    offset: buffer.offset(),
                                    bytes: map.to_vec(),
                                });
                            }
                        }
                        gst::PadProbeReturn::Ok
                    },
                );
                None
            });
        }

        fn decoded_sink(&self) -> gst::Element {
            let decoded = Arc::clone(&self.decoded);
            let sink = gst::ElementFactory::make("fakesink")
                .property("sync", false)
                .build()
                .expect("fakesink");
            sink.static_pad("sink").expect("fakesink pad").add_probe(
                gst::PadProbeType::BUFFER,
                move |_pad, info| {
                    if let Some(buffer) = info.buffer() {
                        if let (Some(start), Some(length), Ok(mut decoded)) =
                            (buffer.pts(), buffer.duration(), decoded.lock())
                        {
                            decoded.push((
                                Duration::from_nanos(start.nseconds()),
                                Duration::from_nanos(length.nseconds()),
                            ));
                        }
                    }
                    gst::PadProbeReturn::Ok
                },
            );
            sink
        }

        fn set_path(&self, path: &Path) {
            self.playbin.set_property("uri", file_uri(path));
        }

        fn set_state(&self, state: gst::State) -> Result<(), String> {
            self.playbin
                .set_state(state)
                .map(|_| ())
                .map_err(|err| err.to_string())
        }

        fn wait_for(&self, wanted: gst::MessageType) -> Result<(), String> {
            let bus = self.playbin.bus().expect("bus");
            let message = bus
                .timed_pop_filtered(
                    gst::ClockTime::from_seconds(10),
                    &[wanted, gst::MessageType::Error],
                )
                .ok_or_else(|| self.report(&format!("timed out waiting for {wanted:?}")))?;
            match message.view() {
                gst::MessageView::Error(err) => Err(format!("{} ({:?})", err.error(), err.debug())),
                _ => Ok(()),
            }
        }

        /// Describe what the player recorded and what its pipeline does,
        /// for a wait for `what` that timed out.
        fn report(&self, what: &str) -> String {
            let decoded = self.decoded.lock().map_or(0, |decoded| decoded.len());
            let delivered = self.delivered.lock().map_or(0, |delivered| delivered.len());
            let details = format!(
                "  since the last seek: {decoded} buffers decoded, {delivered} left the sources\n"
            );
            self.trace.report(what, &details)
        }

        fn clear(&self) {
            self.decoded.lock().expect("decoded").clear();
            self.delivered.lock().expect("delivered").clear();
        }

        fn take_run(&self) -> Run {
            Run {
                decoded: std::mem::take(&mut *self.decoded.lock().expect("decoded")),
                delivered: std::mem::take(&mut *self.delivered.lock().expect("delivered")),
            }
        }

        fn finish<T>(self, name: &str, result: Result<T, String>) -> T {
            let _ = self.playbin.set_state(gst::State::Null);
            result.unwrap_or_else(|err| panic!("{name}: {err}"))
        }
    }

    fn file_uri(path: &Path) -> String {
        url::Url::from_file_path(path)
            .expect("file URI")
            .to_string()
    }

    /// Preroll, then for each target: seek, play to the end of the stream,
    /// and record what was decoded and what left the source.
    fn play_from(path: &Path, seek_flags: gst::SeekFlags, targets: &[Duration]) -> Vec<Run> {
        let player = Player::new();
        player.set_path(path);
        let result = (|| {
            player.set_state(gst::State::Paused)?;
            player.wait_for(gst::MessageType::AsyncDone)?;
            let mut runs = Vec::new();
            for (index, target) in targets.iter().enumerate() {
                let nanos = u64::try_from(target.as_nanos()).expect("target fits");
                player
                    .playbin
                    .seek_simple(seek_flags, gst::ClockTime::from_nseconds(nanos))
                    .map_err(|err| err.to_string())?;
                // The flushing seek has completed; anything recorded so far
                // belongs to the preroll or the previous run.
                player.clear();
                if index == 0 {
                    player.set_state(gst::State::Playing)?;
                }
                player.wait_for(gst::MessageType::Eos)?;
                runs.push(player.take_run());
            }
            Ok(runs)
        })();
        player.finish(&path.display().to_string(), result)
    }

    /// Play `paths` back to back, switching on `about-to-finish` as the
    /// playback backend does for files with the same extension.
    fn play_gapless(paths: &[PathBuf]) -> Run {
        let player = Player::new();
        player.set_path(&paths[0]);
        let queue = Mutex::new(paths[1..].iter().cloned().collect::<VecDeque<_>>());
        player
            .playbin
            .connect("about-to-finish", false, move |values| {
                let playbin = values.first()?.get::<gst::Element>().ok()?;
                let next = queue.lock().ok()?.pop_front()?;
                playbin.set_property("uri", file_uri(&next));
                None
            });
        let result = (|| {
            player.set_state(gst::State::Playing)?;
            player.wait_for(gst::MessageType::Eos)?;
            Ok(player.take_run())
        })();
        player.finish("gapless queue", result)
    }

    fn require_elements(elements: [&str; 2]) {
        gst::init().expect("gst init");
        for element in elements {
            assert!(
                gst::ElementFactory::find(element).is_some(),
                "install the GStreamer good and libav plugins: missing {element}"
            );
        }
    }

    /// A ripped file holding `fixture` repeated, long enough that a parser
    /// reads it in several 64 KiB pulls, with the range of its audio.
    fn write_ripped_file(name: &str, fixture: &[u8], repeats: usize) -> (PathBuf, Range<usize>) {
        let (bytes, audio) = ripped_surround_bytes(&fixture.repeat(repeats));
        let path = test_path(name, name);
        std::fs::write(&path, &bytes).expect("write fixture");
        (path, audio)
    }

    struct Format {
        /// Also the extension; upper case gives the typefinders no hint.
        name: &'static str,
        fixture: &'static [u8],
        fixture_frames: u32,
        frame_length: Duration,
        /// Matches `seek_flags_for_path` in the playback backend.
        seek_flags: gst::SeekFlags,
        elements: [&'static str; 2],
    }

    const AC3: Format = Format {
        name: "AC3",
        fixture: TEST_AC3,
        fixture_frames: 32,
        frame_length: Duration::from_millis(32),
        seek_flags: gst::SeekFlags::FLUSH,
        elements: ["ac3parse", "avdec_ac3"],
    };

    const DTS: Format = Format {
        name: "dts",
        fixture: TEST_DTS,
        fixture_frames: 47,
        frame_length: Duration::from_nanos(512 * 1_000_000_000 / 48_000),
        seek_flags: gst::SeekFlags::FLUSH.union(gst::SeekFlags::KEY_UNIT),
        elements: ["dcaparse", "avdec_dca"],
    };

    impl Format {
        fn repeats(&self) -> usize {
            (256 * 1024_usize).div_ceil(self.fixture.len())
        }

        fn length(&self, repeats: usize) -> Duration {
            self.frame_length * self.fixture_frames * u32::try_from(repeats).expect("repeat count")
        }
    }

    fn assert_playback_survives_seeks(format: &Format) {
        require_elements(format.elements);
        let name = format.name;
        let repeats = format.repeats();
        let length = format.length(repeats);
        let file = write_ripped_file(name, format.fixture, repeats);
        let path = file.0.clone();

        let forward = length * 3 / 5;
        let runs = play_from(&path, format.seek_flags, &[forward, Duration::ZERO]);
        let _ = std::fs::remove_file(&path);

        // Seeking may land on the frame before the target.
        let slack = format.frame_length * 2;
        for (run, target) in runs.iter().zip([forward, Duration::ZERO]) {
            assert!(
                run.first_start() <= target + slack && run.first_start() + slack >= target,
                "{name}: playback after seeking to {target:?} starts at {:?}",
                run.first_start()
            );
            assert!(
                run.last_end() + slack >= length,
                "{name}: playback after seeking to {target:?} stopped at {:?} of {length:?}",
                run.last_end()
            );
            assert!(
                run.total_decoded() + slack >= run.last_end() - run.first_start(),
                "{name}: playback after seeking to {target:?} dropped audio: {:?} decoded",
                run.total_decoded()
            );
            run.assert_only_audio_delivered(std::slice::from_ref(&file));
        }
    }

    fn assert_gapless_queue_plays_every_frame(format: &Format) {
        require_elements(format.elements);
        let name = format.name;
        let repeats = format.repeats();
        let first = write_ripped_file(name, format.fixture, repeats);
        // A file with nothing to strip is read without the filter.
        let clean = (test_path("clean", name), 0..format.fixture.len() * repeats);
        std::fs::write(&clean.0, format.fixture.repeat(repeats)).expect("write fixture");
        let last = write_ripped_file(name, format.fixture, repeats + 3);
        // The last track twice, as Repeat-One queues it.
        let paths = [&first, &clean, &last, &last].map(|(path, _)| path.clone());
        let run = play_gapless(&paths);
        for (path, _) in [&first, &clean, &last] {
            let _ = std::fs::remove_file(path);
        }

        let expected = format.length(repeats) * 2 + format.length(repeats + 3) * 2;
        let decoded = run.total_decoded();
        assert!(
            decoded + format.frame_length * 3 >= expected
                && decoded <= expected + format.frame_length,
            "{name}: decoded {decoded:?} of a {expected:?} queue"
        );
        run.assert_only_audio_delivered(&[first, clean, last]);
    }

    #[test]
    fn ripped_ac3_plays_to_the_end_after_seeking_forward_and_back() {
        assert_playback_survives_seeks(&AC3);
    }

    #[test]
    fn ripped_dts_plays_to_the_end_after_seeking_forward_and_back() {
        assert_playback_survives_seeks(&DTS);
    }

    #[test]
    fn ripped_ac3_plays_every_frame_through_gapless_and_repeated_tracks() {
        assert_gapless_queue_plays_every_frame(&AC3);
    }

    #[test]
    fn ripped_dts_plays_every_frame_through_gapless_and_repeated_tracks() {
        assert_gapless_queue_plays_every_frame(&DTS);
    }
}
