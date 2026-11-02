// SPDX-License-Identifier: GPL-3.0-or-later

//! A seek index for the parser of a raw AC3 file.
//!
//! `ac3parse` knows where a frame lies only once it has parsed it: an
//! accurate seek past the frames it has parsed makes it read every frame up
//! to the target, a third of a second for four minutes of 448 kbit/s audio.
//! A key-unit seek does not help, as AC3 frames carry no time stamp: the
//! parser estimates where the target is from the bitrate and labels the
//! frame it finds there with the target time.
//!
//! Every AC3 frame holds 1,536 samples, and its header tells its length and
//! sample rate. Walking the frame headers of a file from its first frame
//! gives the position and time of every frame, as the parser counts them,
//! and its parser gets an index entry for the first frame of every second.
//! The walk stops where no frame of the stream's sample rate starts, as at
//! damage or an E-AC3 frame; the frames after it are not indexed.
//!
//! Positions count from the first frame, as the parser sees them behind
//! the byte filter (see `byte_filter`).

use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom};
use std::path::Path;

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;
#[cfg(feature = "gst")]
use gstreamer_base as gst_base;
#[cfg(feature = "gst")]
use gstreamer_base::prelude::BaseParseExt;

use super::bitstream::plain_ac3_frame_len;
use super::byte_filter::read_audio_layout;

/// Samples in an AC3 frame.
const SAMPLES_PER_FRAME: u64 = 1_536;
/// Bytes of a frame header that tell its length and sample rate.
const HEADER_LEN: usize = 6;

/// An index entry: the position of a frame, counted from the first frame,
/// and the time of its first sample in nanoseconds.
pub(super) type Entry = (u64, u64);

/// The rate of the samples of the plain AC3 frame whose header `header`
/// starts with: its sample rate code, halved for `bsid` 9 and quartered for
/// 10, as `ac3parse` takes them.
fn ac3_sample_rate(header: &[u8]) -> Option<u64> {
    let rate = match header.get(4)? >> 6 {
        0 => 48_000,
        1 => 44_100,
        2 => 32_000,
        _ => return None,
    };
    match header.get(5)? >> 3 {
        0..=8 => Some(rate),
        9 => Some(rate / 2),
        10 => Some(rate / 4),
        _ => None,
    }
}

/// Frames walked between asks whether the index is still wanted.
const FRAMES_BETWEEN_ASKS: u64 = 256;

/// The index entries of the frames that `reader` reads, from the first
/// frame of a stream whose audio is `audio_len` bytes long. Stops early,
/// with no entries, once `wanted` no longer holds, as when the track was
/// skipped before its walk was done.
pub(super) fn index_entries<R: Read + Seek>(
    reader: &mut BufReader<R>,
    audio_len: u64,
    wanted: &dyn Fn() -> bool,
) -> Vec<Entry> {
    let mut entries = Vec::new();
    let mut position = 0;
    let mut samples = 0;
    let mut stream_rate = None;
    let mut next_entry = 0;
    let mut header = [0; HEADER_LEN];
    let mut frames = 0;
    while position < audio_len && reader.read_exact(&mut header).is_ok() {
        frames += 1;
        if frames % FRAMES_BETWEEN_ASKS == 0 && !wanted() {
            return Vec::new();
        }
        let Some((rate, len)) = ac3_sample_rate(&header).zip(plain_ac3_frame_len(&header)) else {
            break;
        };
        let len = len as u64;
        if *stream_rate.get_or_insert(rate) != rate || position + len > audio_len {
            break;
        }
        if samples >= next_entry {
            let nanos = u128::from(samples) * 1_000_000_000 / u128::from(rate);
            entries.push((position, u64::try_from(nanos).unwrap_or(u64::MAX)));
            next_entry = samples + rate;
        }
        samples += SAMPLES_PER_FRAME;
        position += len;
        let Ok(rest) = i64::try_from(len - HEADER_LEN as u64) else {
            break;
        };
        if reader.seek_relative(rest).is_err() {
            break;
        }
    }
    entries
}

/// The index entries of the raw AC3 file at `path`, which this reads from
/// its first frame to the end of its audio while `wanted` holds.
pub(super) fn ac3_index(path: &Path, wanted: &dyn Fn() -> bool) -> Option<Vec<Entry>> {
    let layout = read_audio_layout(path)?;
    let first_frame = layout.first_frame?;
    let mut file = File::open(path).ok()?;
    file.seek(SeekFrom::Start(first_frame)).ok()?;
    let mut reader = BufReader::with_capacity(256 * 1024, file);
    let entries = index_entries(&mut reader, layout.audio_end - first_frame, wanted);
    (!entries.is_empty()).then_some(entries)
}

/// Give the AC3 parsers of `bin` an index of the frames of the raw AC3
/// files they parse. Call this before the pipeline starts.
///
/// A parser asks upstream for its file once it receives data. The file is
/// read on a thread of its own, since a read from a network share can take
/// long, and the parser is indexed when that thread is done; a seek before
/// then reads the frames, as it would without an index.
#[cfg(feature = "gst")]
pub(crate) fn index_raw_ac3_frames(bin: &gst::Element) {
    bin.connect("deep-element-added", false, |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        if element
            .factory()
            .is_some_and(|factory| factory.name() == "ac3parse")
        {
            index_once_parsing(&element);
        }
        None
    });
}

/// Index `parser` once it receives its first data, which tells its file.
#[cfg(feature = "gst")]
fn index_once_parsing(parser: &gst::Element) {
    let Some(pad) = parser.static_pad("sink") else {
        return;
    };
    let parser = parser.downgrade();
    pad.add_probe(
        gst::PadProbeType::BUFFER | gst::PadProbeType::BUFFER_LIST,
        move |pad, _| {
            let mut query = gst::query::Uri::new();
            let path = pad
                .peer_query(&mut query)
                .then(|| query.uri())
                .flatten()
                .and_then(|uri| gst::glib::filename_from_uri(&uri).ok())
                .map(|(path, _)| path)
                .filter(|path| super::is_raw_surround_file(path) && !super::is_dts_file(path));
            if let Some(path) = path {
                let parser = parser.clone();
                #[cfg(test)]
                let indexing = test_hooks::Indexing::start(&path);
                let _ = std::thread::Builder::new()
                    .name("ferrous-ac3-index".to_string())
                    .spawn(move || {
                        #[cfg(test)]
                        let _indexing = indexing;
                        // Not once the parser is gone.
                        let wanted = || parser.upgrade().is_some();
                        let Some(entries) = ac3_index(&path, &wanted) else {
                            return;
                        };
                        let Some(parser) = parser
                            .upgrade()
                            .and_then(|parser| parser.downcast::<gst_base::BaseParse>().ok())
                        else {
                            return;
                        };
                        for (offset, nanos) in entries {
                            parser.add_index_entry(
                                offset,
                                gst::ClockTime::from_nseconds(nanos),
                                true,
                                true,
                            );
                        }
                    });
            }
            gst::PadProbeReturn::Remove
        },
    );
}

/// The parsers being indexed, so a test can wait for those of its file.
#[cfg(all(test, feature = "gst"))]
pub(crate) mod test_hooks {
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;
    use std::time::{Duration, Instant};

    /// The files of the parsers whose indexing has not ended.
    static INDEXING: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
    /// The files of the parsers whose indexing ended.
    static INDEXED: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());

    /// The indexing of a parser of a file, until it is dropped.
    pub(super) struct Indexing(PathBuf);

    impl Indexing {
        pub(super) fn start(path: &Path) -> Self {
            INDEXING.lock().expect("indexing").push(path.to_path_buf());
            Self(path.to_path_buf())
        }
    }

    impl Drop for Indexing {
        fn drop(&mut self) {
            let mut indexing = INDEXING.lock().expect("indexing");
            if let Some(at) = indexing.iter().position(|path| *path == self.0) {
                indexing.swap_remove(at);
            }
            INDEXED.lock().expect("indexed").push(self.0.clone());
        }
    }

    /// Wait until a parser of the file at `path` has been indexed and none
    /// is being indexed.
    pub(crate) fn wait_until_indexed(path: &Path) {
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            let indexed = INDEXED.lock().expect("indexed").iter().any(|p| p == path);
            let indexing = INDEXING.lock().expect("indexing").iter().any(|p| p == path);
            if indexed && !indexing {
                return;
            }
            assert!(
                Instant::now() < deadline,
                "{} was not indexed",
                path.display()
            );
            std::thread::sleep(Duration::from_millis(5));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{ac3_index, index_entries, Entry};
    use crate::raw_audio::{ripped_surround_bytes, test_path, TEST_AC3};
    use std::io::{BufReader, Cursor};

    /// The length of the frames of `TEST_AC3`: 32 kbit/s at 48 kHz.
    const FRAME_LEN: u64 = 128;

    /// The entries of `bytes`, a stream that starts with a frame.
    fn entries_of(bytes: &[u8]) -> Vec<Entry> {
        index_entries(
            &mut BufReader::new(Cursor::new(bytes)),
            bytes.len() as u64,
            &|| true,
        )
    }

    /// A plain AC3 frame of `frmsizecod` at `fscod` with `bsid`, silent but
    /// for its header, whose CRC the walk does not check.
    fn frame(fscod: u8, frmsizecod: u8, bsid: u8) -> Vec<u8> {
        let header = [0x0B, 0x77, 0, 0, (fscod << 6) | frmsizecod, bsid << 3];
        let len = super::plain_ac3_frame_len(&header).expect("frame length");
        let mut frame = header.to_vec();
        frame.resize(len, 0);
        frame
    }

    /// The entries of a stream of `frames`: the first frame of each second,
    /// at `rate`.
    fn expected(frames: &[Vec<u8>], rate: u64) -> Vec<Entry> {
        let (mut position, mut samples, mut next) = (0, 0, 0);
        let mut entries = Vec::new();
        for frame in frames {
            if samples >= next {
                entries.push((position, samples * 1_000_000_000 / rate));
                next = samples + rate;
            }
            position += frame.len() as u64;
            samples += 1_536;
        }
        entries
    }

    #[test]
    fn a_stream_is_indexed_every_second_from_its_first_frame() {
        let stream = TEST_AC3.repeat(3);
        let (bytes, _) = ripped_surround_bytes(&stream);
        let path = test_path("index-ripped", "ac3");
        std::fs::write(&path, &bytes).expect("write fixture");
        let entries = ac3_index(&path, &|| true).expect("entries");
        let _ = std::fs::remove_file(&path);
        // 32 frames of 32 ms make the first 1.024 s; the tag is not read.
        assert_eq!(
            entries,
            [
                (0, 0),
                (32 * FRAME_LEN, 1_024_000_000),
                (64 * FRAME_LEN, 2_048_000_000),
            ]
        );
    }

    #[test]
    fn a_stream_whose_bitrate_changes_is_indexed_by_its_frames() {
        // 32 kbit/s, then 64 kbit/s frames twice as long, then 32 kbit/s:
        // the first, middle and last frames all are 32 kbit/s frames that
        // lie where 32 kbit/s frames throughout would put them.
        let mut frames = vec![frame(0, 0, 8); 320];
        frames.extend(vec![frame(0, 8, 8); 160]);
        frames.extend(vec![frame(0, 0, 8); 1_280]);
        let bytes = frames.concat();
        let units = bytes.len() as u64 / FRAME_LEN;
        assert_eq!(
            &bytes[(units / 2 * FRAME_LEN) as usize..][..6],
            &frames[0][..6]
        );
        let entries = entries_of(&bytes);
        assert_eq!(entries, expected(&frames, 48_000));
        // An entry in the last section is 5.12 s earlier than a single
        // bitrate would place it.
        let (position, nanos) = entries[40];
        assert_eq!(position / FRAME_LEN * 32_000_000, nanos + 5_120_000_000);
    }

    #[test]
    fn half_and_quarter_rate_streams_are_timed_at_their_own_rates() {
        for (bsid, rate) in [(9, 24_000), (10, 12_000)] {
            let frames = vec![frame(0, 0, bsid); 100];
            let entries = entries_of(&frames.concat());
            assert_eq!(entries, expected(&frames, rate), "bsid {bsid}");
            // 1,536 samples at the lower rate last 64 or 128 ms.
            let frames_to_second = rate.div_ceil(1_536);
            assert_eq!(
                entries[1].1,
                frames_to_second * 1_536 * 1_000_000_000 / rate,
                "bsid {bsid}"
            );
        }
    }

    #[test]
    fn a_stream_at_44_1_khz_is_indexed_through_frames_of_two_lengths() {
        let frames: Vec<_> = (0..200)
            .map(|index| frame(1, if index % 3 == 0 { 1 } else { 0 }, 8))
            .collect();
        assert_ne!(frames[0].len(), frames[1].len());
        let entries = entries_of(&frames.concat());
        assert_eq!(entries, expected(&frames, 44_100));
    }

    /// Counts the bytes read through it.
    struct Counted<'a> {
        inner: Cursor<&'a [u8]>,
        read: std::rc::Rc<std::cell::Cell<usize>>,
    }

    impl std::io::Read for Counted<'_> {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            let count = self.inner.read(buf)?;
            self.read.set(self.read.get() + count);
            Ok(count)
        }
    }

    impl std::io::Seek for Counted<'_> {
        fn seek(&mut self, position: std::io::SeekFrom) -> std::io::Result<u64> {
            self.inner.seek(position)
        }
    }

    #[test]
    fn the_walk_ends_before_the_end_of_the_file_once_the_index_is_not_wanted() {
        let stream = TEST_AC3.repeat(64);
        let read = std::rc::Rc::default();
        let mut reader = BufReader::with_capacity(
            4_096,
            Counted {
                inner: Cursor::new(&stream),
                read: std::rc::Rc::clone(&read),
            },
        );
        let asks = std::cell::Cell::new(0);
        let entries = index_entries(&mut reader, stream.len() as u64, &|| {
            asks.set(asks.get() + 1);
            asks.get() < 2
        });
        assert!(entries.is_empty(), "{entries:?}");
        // Two asks, 512 frames of 128 bytes, and a buffer.
        assert!(
            read.get() <= 512 * 128 + 4_096,
            "read {} of {} bytes",
            read.get(),
            stream.len()
        );
    }

    #[test]
    fn the_walk_stops_where_the_frames_do() {
        let stream = TEST_AC3.repeat(3);
        // A frame cut short at 40: no frame starts where the next should.
        let mut cut = stream.clone();
        cut.drain(40 * FRAME_LEN as usize..40 * FRAME_LEN as usize + 10);
        assert_eq!(entries_of(&cut), entries_of(&stream)[..2]);

        // A change of sample rate, and an E-AC3 frame of 256 bytes.
        let mut eac3 = vec![0x0B, 0x77, 0x00, 0x7F, 0x00, 16 << 3];
        eac3.resize(256, 0);
        for other in [frame(2, 0, 8), eac3] {
            let mut frames = vec![frame(0, 0, 8); 40];
            frames.push(other);
            frames.extend(vec![frame(0, 0, 8); 40]);
            assert_eq!(
                entries_of(&frames.concat()),
                expected(&frames[..40], 48_000)
            );
        }

        // A stream that is not AC3 at all.
        assert!(entries_of(&[0x55; 4_096]).is_empty());
    }
}
