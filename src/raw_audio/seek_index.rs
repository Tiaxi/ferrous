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
//! At 48 and 32 kHz, every frame of a stream with one bitrate has the same
//! length, so the frame that holds a time lies at a known byte position.
//! Once the middle and the last frame of a file lie where that length puts
//! them, its parser gets an index entry every second, and an accurate seek
//! starts from the one before its target. Other streams, such as those at
//! 44.1 kHz, whose frames alternate between two lengths, are not indexed.
//!
//! Positions count from the first frame, as the parser sees them behind
//! the byte filter (see `byte_filter`).

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
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
/// Bytes of the frame header that every frame of an indexed stream shares:
/// the sync word, the CRC, which differs, and the sample rate, bitrate and
/// stream version.
const HEADER_LEN: usize = 6;

/// The frames of a raw AC3 file whose frames all have the same length.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct FrameGrid {
    /// File offset of the first frame.
    first_frame: u64,
    frame_len: u64,
    sample_rate: u64,
    /// Whole frames from the first frame to the end of the audio.
    frames: u64,
}

impl FrameGrid {
    /// The grid of `header`, the first frame of a stream whose audio is
    /// `audio_len` bytes long from it, if the frames of such a stream all
    /// have one length.
    fn of_header(header: &[u8], first_frame: u64, audio_len: u64) -> Option<Self> {
        let sample_rate = match header.get(4)? >> 6 {
            0 => 48_000,
            2 => 32_000,
            // 44.1 kHz frames alternate between two lengths.
            _ => return None,
        };
        let frame_len = u64::try_from(plain_ac3_frame_len(header)?).ok()?;
        let frames = audio_len / frame_len;
        (frames > 0).then_some(Self {
            first_frame,
            frame_len,
            sample_rate,
            frames,
        })
    }

    /// File offsets of the frames that must lie on the grid for it to hold:
    /// the middle and the last frame.
    fn checked_frames(&self) -> [u64; 2] {
        [self.frames / 2, self.frames - 1].map(|frame| self.first_frame + frame * self.frame_len)
    }

    /// The index entries of the grid: the position of every frame that
    /// starts a second, counted from the first frame, and its time in
    /// nanoseconds.
    pub(super) fn entries(&self) -> impl Iterator<Item = (u64, u64)> + '_ {
        let frames_per_second = (self.sample_rate / SAMPLES_PER_FRAME).max(1);
        (0..self.frames)
            .step_by(usize::try_from(frames_per_second).unwrap_or(1))
            .map(|frame| {
                let nanos = u128::from(frame) * u128::from(SAMPLES_PER_FRAME) * 1_000_000_000
                    / u128::from(self.sample_rate);
                (
                    frame * self.frame_len,
                    u64::try_from(nanos).unwrap_or(u64::MAX),
                )
            })
    }
}

/// The frame grid of the raw AC3 file at `path`, if its frames all have the
/// length of the first: the middle and the last frame are checked.
pub(super) fn ac3_frame_grid(path: &Path) -> Option<FrameGrid> {
    let layout = read_audio_layout(path)?;
    let first_frame = layout.first_frame?;
    let header = layout
        .head
        .get(usize::try_from(first_frame).ok()?..)?
        .get(..HEADER_LEN)?;
    let grid = FrameGrid::of_header(header, first_frame, layout.audio_end - first_frame)?;
    let mut file = File::open(path).ok()?;
    for offset in grid.checked_frames() {
        let mut other = [0; HEADER_LEN];
        file.seek(SeekFrom::Start(offset)).ok()?;
        file.read_exact(&mut other).ok()?;
        // The CRC (bytes 2 and 3) differs from frame to frame.
        let same = other[..2] == header[..2] && other[4..] == header[4..];
        if !same {
            return None;
        }
    }
    Some(grid)
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
                        let Some(grid) = ac3_frame_grid(&path) else {
                            return;
                        };
                        let Some(parser) = parser
                            .upgrade()
                            .and_then(|parser| parser.downcast::<gst_base::BaseParse>().ok())
                        else {
                            return;
                        };
                        for (offset, nanos) in grid.entries() {
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
    use super::{ac3_frame_grid, FrameGrid};
    use crate::raw_audio::{ripped_surround_bytes, test_path, TEST_AC3};

    /// The length of the frames of `TEST_AC3`: 32 kbit/s at 48 kHz.
    const FRAME_LEN: u64 = 128;

    fn written(name: &str, bytes: &[u8]) -> std::path::PathBuf {
        let path = test_path(name, "ac3");
        std::fs::write(&path, bytes).expect("write fixture");
        path
    }

    #[test]
    fn a_stream_of_one_frame_length_is_indexed_every_second_from_its_first_frame() {
        let stream = TEST_AC3.repeat(3);
        let (bytes, audio) = ripped_surround_bytes(&stream);
        let path = written("grid-ripped", &bytes);
        let grid = ac3_frame_grid(&path).expect("grid");
        let _ = std::fs::remove_file(&path);
        assert_eq!(
            grid,
            FrameGrid {
                first_frame: audio.start as u64,
                frame_len: FRAME_LEN,
                sample_rate: 48_000,
                frames: stream.len() as u64 / FRAME_LEN,
            }
        );
        // 31 frames of 32 ms make the first 0.992 s.
        let entries: Vec<_> = grid.entries().collect();
        assert_eq!(
            entries,
            [
                (0, 0),
                (31 * FRAME_LEN, 992_000_000),
                (62 * FRAME_LEN, 1_984_000_000),
                (93 * FRAME_LEN, 2_976_000_000),
            ]
        );
    }

    #[test]
    fn a_stream_whose_frames_move_off_the_grid_is_not_indexed() {
        // A frame cut short in the middle moves every frame after it.
        let mut bytes = TEST_AC3.repeat(3);
        bytes.drain(40 * FRAME_LEN as usize..40 * FRAME_LEN as usize + 10);
        let path = written("grid-cut", &bytes);
        assert_eq!(ac3_frame_grid(&path), None);
        let _ = std::fs::remove_file(&path);

        // A stream that is not AC3 at all.
        let path = written("grid-noise", &[0x55; 4_096]);
        assert_eq!(ac3_frame_grid(&path), None);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_stream_at_44_1_khz_is_not_indexed() {
        let mut bytes = TEST_AC3.to_vec();
        // fscod 1: 44.1 kHz, whose frames alternate between two lengths.
        for frame in bytes.chunks_exact_mut(FRAME_LEN as usize) {
            frame[4] = (frame[4] & 0x3F) | 0x40;
        }
        let header = &bytes[..6];
        assert_eq!(FrameGrid::of_header(header, 0, bytes.len() as u64), None);
    }
}
