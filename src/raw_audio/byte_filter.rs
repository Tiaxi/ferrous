// SPDX-License-Identifier: GPL-3.0-or-later

//! Byte filter that hands `GStreamer` only the audio frames of a raw AC3/DTS
//! file: no leading partial frame and no appended `APEv2` tag.
//!
//! The filter is a set of probes on the file source. The source offers only
//! push mode, so every reader, format detection included, receives the file
//! through the buffer probe. That probe places each buffer by the byte
//! offset `filesrc` stamps on it, because seeks restart reading anywhere.
//!
//! Downstream, byte positions count from the first frame, as if the file
//! held only the audio: buffer offsets, segments, and the answers to byte
//! queries. Parsers compute byte seeks in those positions, so the probes
//! move them to the file's own positions before they reach `filesrc`.

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

/// Byte position in the audio of `file_position`, clamped to the audio.
fn audio_position(file_position: u64, (audio_start, audio_end): (u64, u64)) -> u64 {
    file_position.clamp(audio_start, audio_end) - audio_start
}

/// Byte position in the file of `audio_position`, clamped to the audio.
fn file_position(audio_position: u64, (audio_start, audio_end): (u64, u64)) -> u64 {
    audio_position.saturating_add(audio_start).min(audio_end)
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
    let kept = match trim(offset, buffer.size(), range, pulled) {
        Trim::Keep => 0..buffer.size(),
        Trim::Drop => return gst::PadProbeReturn::Drop,
        Trim::Slice(kept) => kept,
    };
    // A pulled buffer holds the bytes at the file offset that was asked for.
    let shift = if pulled { 0 } else { range.0 };
    place_buffer(buffer, offset, kept, shift);
    gst::PadProbeReturn::Ok
}

/// Keep the `kept` bytes of a buffer read at `offset`, placed `shift` bytes
/// earlier.
#[cfg(feature = "gst")]
fn place_buffer(buffer: &mut gst::Buffer, offset: u64, kept: Range<usize>, shift: u64) {
    let whole = kept.len() == buffer.size();
    if whole && shift == 0 {
        return;
    }
    // A kept byte of a pushed buffer lies at or after the first frame.
    let start = offset + kept.start as u64 - shift;
    let end = offset + kept.end as u64 - shift;
    let buffer = buffer.make_mut();
    if whole || buffer.resize(kept).is_ok() {
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
    pad.add_probe(gst::PadProbeType::EVENT_BOTH, move |_pad, info| {
        place_event(info, range);
        gst::PadProbeReturn::Ok
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
/// as audio when they estimate the duration. Byte positions that `filesrc`
/// answers with are moved into the audio once it has answered.
#[cfg(feature = "gst")]
fn answer_query(info: &mut gst::PadProbeInfo, range: (u64, u64)) -> gst::PadProbeReturn {
    let answered = info.mask.contains(gst::PadProbeType::PULL);
    let Some(query) = info.query_mut() else {
        return gst::PadProbeReturn::Ok;
    };
    let in_audio = |position: gst::GenericFormattedValue| match position {
        gst::GenericFormattedValue::Bytes(position) => {
            position.map(|position| gst::format::Bytes::from_u64(audio_position(*position, range)))
        }
        _ => None,
    };
    match query.view_mut() {
        gst::QueryViewMut::Scheduling(scheduling) => {
            scheduling.set(gst::SchedulingFlags::SEEKABLE, 1, -1, 0);
            scheduling.add_scheduling_modes([gst::PadMode::Push]);
            gst::PadProbeReturn::Handled
        }
        gst::QueryViewMut::Duration(duration) if duration.format() == gst::Format::Bytes => {
            duration.set(gst::format::Bytes::from_u64(range.1 - range.0));
            gst::PadProbeReturn::Handled
        }
        gst::QueryViewMut::Position(position)
            if answered && position.format() == gst::Format::Bytes =>
        {
            position.set(in_audio(position.result()));
            gst::PadProbeReturn::Ok
        }
        gst::QueryViewMut::Seeking(seeking)
            if answered && seeking.format() == gst::Format::Bytes =>
        {
            let (seekable, start, end) = seeking.result();
            seeking.set(seekable, in_audio(start), in_audio(end));
            gst::PadProbeReturn::Ok
        }
        _ => gst::PadProbeReturn::Ok,
    }
}

/// Move byte seeks on their way to `filesrc` into the file, and the byte
/// segments it starts from into the audio.
#[cfg(feature = "gst")]
fn place_event(info: &mut gst::PadProbeInfo, range: (u64, u64)) {
    let placed = match info.event().map(|event| event.view()) {
        Some(gst::EventView::Seek(seek)) => seek_in_file(seek, range),
        Some(gst::EventView::Segment(segment)) => segment_in_audio(segment, range),
        _ => None,
    };
    if let Some(event) = placed {
        info.data = Some(gst::PadProbeData::Event(event));
    }
}

#[cfg(feature = "gst")]
fn seek_in_file(seek: &gst::event::Seek, range: (u64, u64)) -> Option<gst::Event> {
    let (rate, flags, start_type, start, stop_type, stop) = seek.get();
    if start.format() != gst::Format::Bytes {
        return None;
    }
    let (start_type, start) = file_seek_position(start_type, start.value(), range);
    let (stop_type, stop) = file_seek_position(stop_type, stop.value(), range);
    // Parsers seek by bytes without trick modes, so no interval is lost.
    Some(
        gst::event::Seek::builder(rate, flags, start_type, start, stop_type, stop)
            .seqnum(seek.seqnum())
            .running_time_offset(seek.running_time_offset())
            .build(),
    )
}

/// Where in the file a byte seek to `position` in the audio goes. Seeks from
/// the end of the audio become seeks from the start of the file, which
/// continues past the audio.
#[cfg(feature = "gst")]
fn file_seek_position(
    seek_type: gst::SeekType,
    position: i64,
    range: (u64, u64),
) -> (gst::SeekType, Option<gst::format::Bytes>) {
    let in_audio = match seek_type {
        gst::SeekType::Set => u64::try_from(position).ok(),
        gst::SeekType::End => Some((range.1 - range.0).saturating_add_signed(position)),
        // The seek leaves this position as it is.
        gst::SeekType::None => return (seek_type, None),
    };
    match in_audio {
        Some(position) => (
            gst::SeekType::Set,
            Some(gst::format::Bytes::from_u64(file_position(position, range))),
        ),
        None => (seek_type, None),
    }
}

#[cfg(feature = "gst")]
fn segment_in_audio(event: &gst::event::Segment, range: (u64, u64)) -> Option<gst::Event> {
    let mut segment = event
        .segment()
        .downcast_ref::<gst::format::Bytes>()?
        .clone();
    let in_audio = |position: Option<gst::format::Bytes>| {
        position.map(|position| gst::format::Bytes::from_u64(audio_position(*position, range)))
    };
    segment.set_start(in_audio(segment.start()));
    segment.set_stop(in_audio(segment.stop()));
    segment.set_time(in_audio(segment.time()));
    segment.set_position(in_audio(segment.position()));
    segment.set_duration(
        segment
            .duration()
            .map(|_| gst::format::Bytes::from_u64(range.1 - range.0)),
    );
    Some(
        gst::event::Segment::builder(&segment)
            .seqnum(event.seqnum())
            .running_time_offset(event.running_time_offset())
            .build(),
    )
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
    use super::{audio_byte_range, audio_position, file_position, trim, Trim};
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

    #[test]
    fn audio_positions_count_from_the_first_frame() {
        assert_eq!(audio_position(0, RANGE), 0);
        assert_eq!(audio_position(100, RANGE), 0);
        assert_eq!(audio_position(600, RANGE), 500);
        assert_eq!(audio_position(1000, RANGE), 900);
        assert_eq!(audio_position(4000, RANGE), 900);

        assert_eq!(file_position(0, RANGE), 100);
        assert_eq!(file_position(500, RANGE), 600);
        assert_eq!(file_position(900, RANGE), 1000);
        assert_eq!(file_position(4000, RANGE), 1000);
        assert_eq!(file_position(u64::MAX, RANGE), 1000);
    }
}

#[cfg(all(test, feature = "gst"))]
mod gst_tests {
    use super::{
        file_seek_position, filter_raw_surround_source, install_raw_surround_source_probe,
    };
    use crate::playback::pipeline_trace::PipelineTrace;
    use crate::playback::source_activation::race::{
        race_type_finding_with_activation, ActivationRace,
    };
    use crate::playback::source_activation::SourceActivation;
    use crate::raw_audio::{build_test_apev2_tag, test_path, TEST_AC3, TEST_DTS};
    use crate::raw_audio::{configure_raw_surround_decoding, ripped_surround_bytes_after_junk};
    use gst::prelude::*;
    use gstreamer as gst;
    use std::collections::VecDeque;
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

    impl Delivered {
        fn end(&self) -> u64 {
            self.offset + self.bytes.len() as u64
        }

        /// Check that the buffer holds the bytes of `audio` at its offset,
        /// which counts from the first frame: no byte from before the first
        /// frame or from the tag.
        fn check_audio(&self, audio: &[u8]) -> Result<(), String> {
            let (start, end) = (self.offset, self.end());
            let expected = usize::try_from(start)
                .ok()
                .zip(usize::try_from(end).ok())
                .and_then(|(start, end)| audio.get(start..end));
            if expected == Some(self.bytes.as_slice()) {
                return Ok(());
            }
            Err(format!(
                "the bytes that left the source as {start}..{end} are not the audio there, of {} bytes",
                audio.len()
            ))
        }
    }

    /// A decoded buffer.
    #[derive(Debug, PartialEq)]
    struct Decoded {
        start: Duration,
        length: Duration,
    }

    /// A compressed frame that left the parser, before the decoder.
    #[derive(Debug, PartialEq)]
    struct Parsed {
        start: Option<gst::ClockTime>,
        bytes: Vec<u8>,
    }

    /// The parsers of the formats under test.
    const PARSERS: [&str; 2] = [AC3.elements[0], DTS.elements[0]];

    /// What playback from a seek target, or through a queue, produced.
    #[derive(Debug, Default)]
    struct Run {
        decoded: Vec<Decoded>,
        parsed: Vec<Parsed>,
        delivered: Vec<Delivered>,
    }

    impl Run {
        fn first_start(&self) -> Duration {
            self.decoded
                .first()
                .map_or(Duration::MAX, |decoded| decoded.start)
        }

        fn last_end(&self) -> Duration {
            self.decoded
                .last()
                .map_or(Duration::ZERO, |decoded| decoded.start + decoded.length)
        }

        fn total_decoded(&self) -> Duration {
            self.decoded.iter().map(|decoded| decoded.length).sum()
        }

        /// Assert that every file, given with its audio, was read, and that
        /// only its audio left its source, at offsets that count from the
        /// first frame.
        fn assert_only_audio_delivered(&self, files: &[(PathBuf, Vec<u8>)]) {
            for (path, audio) in files {
                let name = path.display();
                let delivered: Vec<_> = self.delivered.iter().filter(|d| d.path == *path).collect();
                assert!(!delivered.is_empty(), "{name}: nothing left the source");
                for buffer in delivered {
                    if let Err(err) = buffer.check_audio(audio) {
                        panic!("{name}: {err}");
                    }
                }
            }
        }
    }

    /// `playbin3` with the app's source filter, a fake audio sink, and
    /// recorders for what the sources deliver, what the parser frames and
    /// what is decoded. It activates its sources as the playback backend
    /// does.
    struct Player {
        playbin: gst::Element,
        decoded: Arc<Mutex<Vec<Decoded>>>,
        parsed: Arc<Mutex<Vec<Parsed>>>,
        delivered: Arc<Mutex<Vec<Delivered>>>,
        trace: PipelineTrace,
        activation: Arc<SourceActivation>,
    }

    impl Player {
        fn new() -> Self {
            gst::init().expect("gst init");
            configure_raw_surround_decoding();
            let playbin = gst::ElementFactory::make("playbin3")
                .build()
                .expect("playbin3");
            install_raw_surround_source_probe(&playbin);
            let activation = SourceActivation::connect(&playbin);
            let player = Self {
                trace: PipelineTrace::new(&playbin),
                playbin,
                decoded: Arc::default(),
                parsed: Arc::default(),
                delivered: Arc::default(),
                activation,
            };
            player.record_delivered_buffers();
            player.record_parsed_frames();
            player
                .playbin
                .set_property("audio-sink", player.decoded_sink());
            player
        }

        /// Record what leaves each source after the filter. A flushing seek
        /// starts a new run: what its source delivered before is dropped
        /// once the end of the flush leaves the source, in stream order.
        fn record_delivered_buffers(&self) {
            let delivered = Arc::clone(&self.delivered);
            // Connected after the filter, so this probe runs after the filter's.
            self.playbin.connect("source-setup", false, move |values| {
                let source = values.get(1)?.get::<gst::Element>().ok()?;
                let path = PathBuf::from(source.property::<Option<String>>("location")?);
                let delivered = Arc::clone(&delivered);
                source.static_pad("src")?.add_probe(
                    gst::PadProbeType::BUFFER | gst::PadProbeType::EVENT_FLUSH,
                    move |_pad, info| {
                        if is_flush_stop(info) {
                            if let Ok(mut delivered) = delivered.lock() {
                                delivered.retain(|buffer| buffer.path != path);
                            }
                        } else if let Some(buffer) = info.buffer() {
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

        /// Record the frames that leave the parser. A flushing seek starts a
        /// new run once the end of the flush leaves the parser, before the
        /// first frame from the seek target.
        fn record_parsed_frames(&self) {
            let parsed = Arc::clone(&self.parsed);
            self.playbin
                .connect("deep-element-added", false, move |values| {
                    let element = values.get(2)?.get::<gst::Element>().ok()?;
                    if element
                        .factory()
                        .is_none_or(|factory| !PARSERS.contains(&factory.name().as_str()))
                    {
                        return None;
                    }
                    let parsed = Arc::clone(&parsed);
                    element.static_pad("src")?.add_probe(
                        gst::PadProbeType::BUFFER | gst::PadProbeType::EVENT_FLUSH,
                        move |_pad, info| {
                            if is_flush_stop(info) {
                                if let Ok(mut parsed) = parsed.lock() {
                                    parsed.clear();
                                }
                            } else if let Some(buffer) = info.buffer() {
                                if let (Ok(map), Ok(mut parsed)) =
                                    (buffer.map_readable(), parsed.lock())
                                {
                                    parsed.push(Parsed {
                                        start: buffer.pts(),
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

        /// A sink that records what is decoded. A flushing seek starts a
        /// new run once the end of the flush reaches the sink, before the
        /// first buffer from the seek target.
        fn decoded_sink(&self) -> gst::Element {
            let decoded = Arc::clone(&self.decoded);
            let sink = gst::ElementFactory::make("fakesink")
                .property("sync", false)
                .build()
                .expect("fakesink");
            sink.static_pad("sink").expect("fakesink pad").add_probe(
                gst::PadProbeType::BUFFER | gst::PadProbeType::EVENT_FLUSH,
                move |_pad, info| {
                    if is_flush_stop(info) {
                        if let Ok(mut decoded) = decoded.lock() {
                            decoded.clear();
                        }
                    } else if let Some(buffer) = info.buffer() {
                        if let (Some(start), Some(length), Ok(mut decoded)) =
                            (buffer.pts(), buffer.duration(), decoded.lock())
                        {
                            decoded.push(Decoded {
                                start: Duration::from_nanos(start.nseconds()),
                                length: Duration::from_nanos(length.nseconds()),
                            });
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
            self.activation
                .set_state(&self.playbin, state)
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
            let parsed = self.parsed.lock().map_or(0, |parsed| parsed.len());
            let delivered = self.delivered.lock().map_or(0, |delivered| delivered.len());
            let details = format!(
                "  since the last seek: {decoded} buffers decoded, {parsed} frames parsed, \
                 {delivered} left the sources\n"
            );
            self.trace.report(what, &details)
        }

        fn take_run(&self) -> Run {
            Run {
                decoded: std::mem::take(&mut *self.decoded.lock().expect("decoded")),
                parsed: std::mem::take(&mut *self.parsed.lock().expect("parsed")),
                delivered: std::mem::take(&mut *self.delivered.lock().expect("delivered")),
            }
        }

        fn finish<T>(self, name: &str, result: Result<T, String>) -> T {
            let _ = self.set_state(gst::State::Null);
            result.unwrap_or_else(|err| panic!("{name}: {err}"))
        }
    }

    fn is_flush_stop(info: &gst::PadProbeInfo) -> bool {
        info.event()
            .is_some_and(|event| event.type_() == gst::EventType::FlushStop)
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
        play_gapless_on(Player::new(), paths)
    }

    fn play_gapless_on(player: Player, paths: &[PathBuf]) -> Run {
        player.set_path(&paths[0]);
        let queue = Mutex::new(paths[1..].iter().cloned().collect::<VecDeque<_>>());
        let activation = Arc::clone(&player.activation);
        player
            .playbin
            .connect("about-to-finish", false, move |values| {
                let playbin = values.first()?.get::<gst::Element>().ok()?;
                let next = queue.lock().ok()?.pop_front()?;
                activation.hand_over(|| playbin.set_property("uri", file_uri(&next)));
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
    /// reads it in several 64 KiB pulls, with its audio.
    fn write_ripped_file(name: &str, fixture: &[u8], repeats: usize) -> (PathBuf, Vec<u8>) {
        write_ripped_file_after_junk(name, 0, fixture, repeats)
    }

    /// [`write_ripped_file`] with `junk_len` bytes of junk before the
    /// leading partial frame.
    fn write_ripped_file_after_junk(
        name: &str,
        junk_len: usize,
        fixture: &[u8],
        repeats: usize,
    ) -> (PathBuf, Vec<u8>) {
        let audio = fixture.repeat(repeats);
        let (bytes, range) = ripped_surround_bytes_after_junk(junk_len, &audio);
        assert_eq!(bytes[range], audio[..]);
        let path = test_path(name, name);
        std::fs::write(&path, &bytes).expect("write fixture");
        (path, audio)
    }

    /// Junk before the first frame, for the filter to trim: more than one
    /// read of the file source, and neither whole frames nor whole fixtures,
    /// so that playback from the wrong place plays other samples.
    const LONG_JUNK: usize = 5000;

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

        /// The frames of `parsed` from the one that plays `target` on.
        fn frames_from<'a>(&self, target: Duration, parsed: &'a [Parsed]) -> Vec<&'a Parsed> {
            parsed
                .iter()
                .skip_while(|frame| {
                    frame.start.is_none_or(|start| {
                        Duration::from_nanos(start.nseconds()) + self.frame_length <= target
                    })
                })
                .collect()
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

    /// Seeks in a ripped file must play what the same seeks play in a file
    /// that starts with the first frame: the same compressed frames, from
    /// the target to the end of the stream. That file ends with a tag, so
    /// the filter reads it in push mode too, but moves no byte position.
    ///
    /// The frames are compared before the decoder, whose output depends on
    /// what it decoded before, such as the dither of the AC3 decoder. For a
    /// target it has indexed no frame near, the parser scans from the last
    /// frame it indexed before the target, which depends on how the reads
    /// split the file, and the decoder drops what comes before the target.
    fn assert_seeks_play_what_they_play_without_the_junk(format: &Format) {
        require_elements(format.elements);
        let name = format.name;
        let repeats = format.repeats();
        let length = format.length(repeats);
        let ripped = write_ripped_file_after_junk(name, LONG_JUNK, format.fixture, repeats);
        let tagged = test_path("tagged", name);
        let mut bytes = ripped.1.clone();
        bytes.extend_from_slice(&build_test_apev2_tag(&[("Title", "Tone")], true));
        std::fs::write(&tagged, &bytes).expect("write fixture");

        let targets = [length * 3 / 5, length / 4];
        let runs = play_from(&ripped.0, format.seek_flags, &targets);
        let tagged_runs = play_from(&tagged, format.seek_flags, &targets);
        for path in [&ripped.0, &tagged] {
            let _ = std::fs::remove_file(path);
        }

        let slack = format.frame_length * 2;
        for ((run, tagged_run), target) in runs.iter().zip(&tagged_runs).zip(targets) {
            assert!(
                run.first_start() <= target + slack && run.first_start() + slack >= target,
                "{name}: playback after seeking to {target:?} starts at {:?}",
                run.first_start()
            );
            // The decoders round the last timestamp or clip the last frame.
            assert!(
                run.last_end() <= length + Duration::from_micros(1)
                    && run.last_end() + format.frame_length >= length,
                "{name}: playback after seeking to {target:?} ends at {:?} of {length:?}",
                run.last_end()
            );
            assert_same_frames(
                &format.frames_from(target, &run.parsed),
                &format.frames_from(target, &tagged_run.parsed),
            )
            .unwrap_or_else(|err| panic!("{name}: playback after seeking to {target:?} {err}"));
            assert!(
                run.decoded == tagged_run.decoded,
                "{name}: playback after seeking to {target:?} decoded {} buffers from {:?} to {:?}, \
                 without the junk {} buffers from {:?} to {:?}",
                run.decoded.len(),
                run.first_start(),
                run.last_end(),
                tagged_run.decoded.len(),
                tagged_run.first_start(),
                tagged_run.last_end()
            );
            run.assert_only_audio_delivered(std::slice::from_ref(&ripped));
        }
    }

    /// Check that the parser framed the same compressed frames, with the
    /// same timestamps, as it did without the junk.
    fn assert_same_frames(parsed: &[&Parsed], without_junk: &[&Parsed]) -> Result<(), String> {
        if parsed.is_empty() {
            return Err("parsed no frames".to_owned());
        }
        if let Some(index) = (0..parsed.len().max(without_junk.len()))
            .find(|&index| parsed.get(index) != without_junk.get(index))
        {
            let describe = |frame: Option<&&Parsed>| {
                frame.map_or_else(
                    || "nothing".to_owned(),
                    |frame| format!("{} bytes at {:?}", frame.bytes.len(), frame.start),
                )
            };
            return Err(format!(
                "parsed {} frames, without the junk {}; frame {index} differs: {}, without the junk {}",
                parsed.len(),
                without_junk.len(),
                describe(parsed.get(index)),
                describe(without_junk.get(index)),
            ));
        }
        Ok(())
    }

    fn assert_gapless_queue_plays_every_frame(format: &Format) {
        require_elements(format.elements);
        let name = format.name;
        let repeats = format.repeats();
        let first = write_ripped_file(name, format.fixture, repeats);
        // A file with nothing to strip is read without the filter.
        let clean = (test_path("clean", name), format.fixture.repeat(repeats));
        std::fs::write(&clean.0, &clean.1).expect("write fixture");
        let last = write_ripped_file_after_junk(name, LONG_JUNK, format.fixture, repeats + 3);
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
    fn a_flushing_seek_starts_a_new_run_of_what_is_decoded() {
        // A run must not depend on when the test thread looks after the
        // seek: under load, playback from the target can be well under way
        // by then.
        require_elements(AC3.elements);
        let repeats = AC3.repeats();
        let (path, _) = write_ripped_file(AC3.name, AC3.fixture, repeats);
        let target = AC3.length(repeats) * 3 / 5;
        let player = Player::new();
        player.set_path(&path);
        let result = (|| {
            player.set_state(gst::State::Paused)?;
            player.wait_for(gst::MessageType::AsyncDone)?;
            let nanos = u64::try_from(target.as_nanos()).expect("target fits");
            player
                .playbin
                .seek_simple(AC3.seek_flags, gst::ClockTime::from_nseconds(nanos))
                .map_err(|err| err.to_string())?;
            player.wait_for(gst::MessageType::AsyncDone)?;
            Ok(player.take_run())
        })();
        let run = player.finish("seek while paused", result);
        let _ = std::fs::remove_file(&path);

        // Only the preroll from the target, not the one from the start.
        let slack = AC3.frame_length * 2;
        assert!(
            !run.decoded.is_empty() && run.first_start() + slack >= target,
            "decoded {:?} for a seek to {target:?}",
            run.decoded
        );
    }

    #[test]
    fn ripped_files_find_their_type_only_once_their_sources_are_activated() {
        // The filter offers the file in push mode only, so urisourcebin
        // finds its type on the streaming thread of the file source. When
        // that happens while the activating thread still changes the state
        // of urisourcebin, the parsebin it then sets up can deadlock with
        // that thread (GStreamer issue 4472). The hold must stop the
        // streaming thread before: for the first file, activated by the
        // state change, and for the successor, activated by the handover in
        // about-to-finish.
        require_elements(AC3.elements);
        let repeats = AC3.repeats();
        let first = write_ripped_file(AC3.name, AC3.fixture, repeats);
        let second = write_ripped_file(AC3.name, AC3.fixture, repeats);
        let player = Player::new();
        let races = [&first, &second]
            .map(|(path, _)| race_type_finding_with_activation(&player.playbin, path));
        let run = play_gapless_on(player, &[first.0.clone(), second.0.clone()]);
        for (path, _) in [&first, &second] {
            let _ = std::fs::remove_file(path);
        }

        for (race, (path, _)) in races.iter().zip([&first, &second]) {
            assert_eq!(
                *race.lock().unwrap(),
                Some(ActivationRace::TypeFindingHeld),
                "{}",
                path.display()
            );
        }
        run.assert_only_audio_delivered(&[first, second]);
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

    #[test]
    fn ripped_ac3_seeks_play_what_they_play_without_the_junk() {
        assert_seeks_play_what_they_play_without_the_junk(&AC3);
    }

    #[test]
    fn ripped_dts_seeks_play_what_they_play_without_the_junk() {
        assert_seeks_play_what_they_play_without_the_junk(&DTS);
    }

    #[test]
    fn byte_seeks_and_queries_count_from_the_first_frame() {
        gst::init().expect("gst init");
        for format in [&AC3, &DTS] {
            let (path, audio) =
                write_ripped_file_after_junk(format.name, LONG_JUNK, format.fixture, 4);
            let reader = ByteReader::new(&path);
            let result = reader.check(&audio);
            let _ = reader.pipeline.set_state(gst::State::Null);
            let _ = std::fs::remove_file(&path);
            result.unwrap_or_else(|err| panic!("{}: {err}", format.name));
        }
    }

    #[test]
    fn byte_seeks_go_to_the_file_position_of_their_audio_position() {
        let range = (100, 1000);
        let seek = |seek_type, position| {
            let (seek_type, position) = file_seek_position(seek_type, position, range);
            (seek_type, position.map(|position| *position))
        };
        assert_eq!(seek(gst::SeekType::Set, 0), (gst::SeekType::Set, Some(100)));
        assert_eq!(
            seek(gst::SeekType::Set, 400),
            (gst::SeekType::Set, Some(500))
        );
        assert_eq!(seek(gst::SeekType::Set, -1), (gst::SeekType::Set, None));
        assert_eq!(
            seek(gst::SeekType::End, 0),
            (gst::SeekType::Set, Some(1000))
        );
        assert_eq!(
            seek(gst::SeekType::End, -300),
            (gst::SeekType::Set, Some(700))
        );
        assert_eq!(
            seek(gst::SeekType::End, -5000),
            (gst::SeekType::Set, Some(100))
        );
        assert_eq!(seek(gst::SeekType::None, -1), (gst::SeekType::None, None));
    }

    /// `filesrc` with the filter, read by a fake sink that records what
    /// reaches it since the last flush.
    struct ByteReader {
        pipeline: gst::Pipeline,
        sink_pad: gst::Pad,
        delivered: Arc<Mutex<Vec<Delivered>>>,
    }

    impl ByteReader {
        fn new(path: &Path) -> Self {
            let source = gst::ElementFactory::make("filesrc")
                .property("location", path.to_string_lossy().to_string())
                .build()
                .expect("filesrc");
            filter_raw_surround_source(&source);
            let sink = gst::ElementFactory::make("fakesink")
                .property("sync", false)
                .build()
                .expect("fakesink");
            let pipeline = gst::Pipeline::new();
            pipeline.add_many([&source, &sink]).expect("add elements");
            source.link(&sink).expect("link elements");
            let sink_pad = sink.static_pad("sink").expect("fakesink pad");
            let delivered: Arc<Mutex<Vec<Delivered>>> = Arc::default();
            let recorded = Arc::clone(&delivered);
            let path = path.to_owned();
            sink_pad.add_probe(
                gst::PadProbeType::BUFFER | gst::PadProbeType::EVENT_FLUSH,
                move |_pad, info| {
                    if is_flush_stop(info) {
                        if let Ok(mut recorded) = recorded.lock() {
                            recorded.clear();
                        }
                    } else if let Some(buffer) = info.buffer() {
                        if let (Ok(map), Ok(mut recorded)) =
                            (buffer.map_readable(), recorded.lock())
                        {
                            recorded.push(Delivered {
                                path: path.clone(),
                                offset: buffer.offset(),
                                bytes: map.to_vec(),
                            });
                        }
                    }
                    gst::PadProbeReturn::Ok
                },
            );
            Self {
                pipeline,
                sink_pad,
                delivered,
            }
        }

        fn wait_for(&self, wanted: gst::MessageType) -> Result<(), String> {
            let bus = self.pipeline.bus().expect("bus");
            let message = bus
                .timed_pop_filtered(
                    gst::ClockTime::from_seconds(10),
                    &[wanted, gst::MessageType::Error],
                )
                .ok_or_else(|| format!("timed out waiting for {wanted:?}"))?;
            match message.view() {
                gst::MessageView::Error(err) => Err(format!("{} ({:?})", err.error(), err.debug())),
                _ => Ok(()),
            }
        }

        fn seek(&self, start_type: gst::SeekType, start: i64) -> Result<(), String> {
            let seek = gst::event::Seek::new(
                1.0,
                gst::SeekFlags::FLUSH,
                start_type,
                gst::GenericFormattedValue::new(gst::Format::Bytes, start),
                gst::SeekType::None,
                gst::GenericFormattedValue::new(gst::Format::Bytes, -1),
            );
            self.pipeline
                .send_event(seek)
                .then_some(())
                .ok_or_else(|| format!("seek to {start_type:?} {start} failed"))
        }

        fn position(&self) -> Option<u64> {
            self.sink_pad
                .peer_query_position::<gst::format::Bytes>()
                .map(|position| *position)
        }

        /// What reached the sink since the last flush: the bytes, which must
        /// be the audio at their offsets, as one range of the audio.
        fn read(&self, audio: &[u8]) -> Result<std::ops::Range<u64>, String> {
            let delivered = self.delivered.lock().map_err(|err| err.to_string())?;
            let mut read: Option<std::ops::Range<u64>> = None;
            for buffer in delivered.iter() {
                buffer.check_audio(audio)?;
                read = match read {
                    None => Some(buffer.offset..buffer.end()),
                    Some(read) if read.end == buffer.offset => Some(read.start..buffer.end()),
                    Some(read) => {
                        return Err(format!(
                            "bytes {}..{} follow bytes {read:?}",
                            buffer.offset,
                            buffer.end()
                        ))
                    }
                };
            }
            read.ok_or_else(|| "nothing reached the sink".to_string())
        }

        /// Answer byte queries and seek by bytes, from the start and from
        /// the end of the audio, as a parser does.
        fn check(&self, audio: &[u8]) -> Result<(), String> {
            let len = audio.len() as u64;
            self.pipeline
                .set_state(gst::State::Paused)
                .map_err(|err| err.to_string())?;
            self.wait_for(gst::MessageType::AsyncDone)?;
            let mut seeking = gst::query::Seeking::new(gst::Format::Bytes);
            if !self.sink_pad.peer_query(&mut seeking) {
                return Err("no seeking answer".to_string());
            }
            let seeking = seeking.result();
            let bytes = |position| gst::GenericFormattedValue::Bytes(Some(position));
            let wanted = (
                true,
                bytes(gst::format::Bytes::ZERO),
                bytes(gst::format::Bytes::from_u64(len)),
            );
            if seeking != wanted {
                return Err(format!("seeking answer {seeking:?}, not {wanted:?}"));
            }
            let duration = self
                .sink_pad
                .peer_query_duration::<gst::format::Bytes>()
                .map(|duration| *duration);
            if duration != Some(len) {
                return Err(format!(
                    "byte duration {duration:?} of {len} bytes of audio"
                ));
            }
            let read = self.read(audio)?;
            if read.start != 0 || self.position() != Some(read.end) {
                return Err(format!(
                    "prerolled bytes {read:?} at byte position {:?}",
                    self.position()
                ));
            }

            // Paused, the source stops after the first buffer from the target.
            let target = len / 3;
            self.seek(gst::SeekType::Set, i64::try_from(target).expect("target"))?;
            self.wait_for(gst::MessageType::AsyncDone)?;
            let read = self.read(audio)?;
            if read.start != target || self.position() != Some(read.end) {
                return Err(format!(
                    "seeking to byte {target} prerolled bytes {read:?} at byte position {:?}",
                    self.position()
                ));
            }
            self.pipeline
                .set_state(gst::State::Playing)
                .map_err(|err| err.to_string())?;
            self.wait_for(gst::MessageType::Eos)?;
            let read = self.read(audio)?;
            if read != (target..len) {
                return Err(format!("seeking to byte {target} read bytes {read:?}"));
            }

            let from_end = len / 5;
            self.seek(
                gst::SeekType::End,
                -i64::try_from(from_end).expect("target"),
            )?;
            self.wait_for(gst::MessageType::Eos)?;
            let read = self.read(audio)?;
            if read != (len - from_end..len) {
                return Err(format!(
                    "seeking to {from_end} bytes before the end read bytes {read:?}"
                ));
            }
            Ok(())
        }
    }
}
