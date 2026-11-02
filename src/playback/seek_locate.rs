// SPDX-License-Identifier: GPL-3.0-or-later

//! Finds the frame of a FLAC file that an accurate seek starts from.
//!
//! An accurate seek makes `flacparse` start from the last frame it has
//! indexed before the target. A file without a seek table has entries only
//! for what has been parsed, so a seek past it parses, and decodes, every
//! frame up to the target: over half a second for each minute of 24/192
//! 5.1 audio. A key-unit seek would make the parser bisect the file, but
//! it reads the file while it handles the seek, on the thread that seeks,
//! which a read from a network share that stopped answering would keep.
//!
//! The locator bisects the file itself instead, on a thread of its own, for
//! a frame shortly before the target: a FLAC frame header tells the number
//! of its first sample. The engine adds that frame to the parser's index and
//! then seeks. The parser reads nothing until its streaming thread does.
//!
//! The requests for a file are handled in turn by one thread, which ends
//! when none is left, so a file whose read does not return holds one thread
//! however often it is sought. Each request carries a generation; a request
//! that a newer one or a cancellation replaced is skipped, or stops between
//! reads, and its result is not sent.

use std::collections::HashMap;
use std::fs::File;
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Duration;

use crossbeam_channel::Sender;

/// A frame that an accurate seek can start from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct LocatedFrame {
    /// Byte offset of the frame in the file.
    pub(super) offset: u64,
    /// Time of its first sample, in nanoseconds from the first frame, as
    /// the parser stamps it.
    pub(super) nanos: u64,
}

/// The answer to a request: the frame found, or `None` if none was, when
/// the seek reads from where the parser has indexed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct Located {
    pub(super) generation: u64,
    pub(super) frame: Option<LocatedFrame>,
}

struct Job {
    generation: u64,
    target: Duration,
}

struct Shared {
    /// The files that a thread locates frames in, with the request it
    /// handles next.
    files: Mutex<HashMap<PathBuf, Option<Job>>>,
    /// The generation of the newest request or cancellation. Older requests
    /// are not answered.
    latest: AtomicU64,
    answers: Sender<Located>,
}

/// Locates frames on threads of their own; see the module documentation.
pub(super) struct SeekLocator {
    shared: Arc<Shared>,
}

impl SeekLocator {
    /// A locator that sends its answers to `answers`.
    pub(super) fn new(answers: Sender<Located>) -> Self {
        Self {
            shared: Arc::new(Shared {
                files: Mutex::default(),
                latest: AtomicU64::new(0),
                answers,
            }),
        }
    }

    /// Find the frame of the FLAC file at `path` that a seek to `target`
    /// starts from, for the request numbered `generation`, which must be
    /// newer than any before. Replaces the requests not answered yet.
    pub(super) fn request(&self, path: &Path, generation: u64, target: Duration) {
        self.shared.latest.store(generation, Ordering::Relaxed);
        let job = Job { generation, target };
        let mut files = self
            .shared
            .files
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        if let Some(next) = files.get_mut(path) {
            // The file's thread takes it once it is done.
            *next = Some(job);
            return;
        }
        files.insert(path.to_path_buf(), Some(job));
        drop(files);
        let shared = Arc::clone(&self.shared);
        let path = path.to_path_buf();
        let spawned = std::thread::Builder::new()
            .name("ferrous-seek-locate".to_string())
            .spawn({
                let path = path.clone();
                move || locate_requests(&shared, &path)
            });
        if spawned.is_err() {
            // Answered without a frame: the seek reads from the index.
            self.shared
                .files
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .remove(&path);
            let _ = self.shared.answers.send(Located {
                generation,
                frame: None,
            });
        }
    }

    /// Leave the requests up to `generation` unanswered.
    pub(super) fn cancel(&self, generation: u64) {
        self.shared.latest.store(generation, Ordering::Relaxed);
    }
}

/// Handle the requests for the file at `path` until none is left.
fn locate_requests(shared: &Shared, path: &Path) {
    loop {
        let job = {
            let mut files = shared.files.lock().unwrap_or_else(PoisonError::into_inner);
            let Some(job) = files.get_mut(path).and_then(Option::take) else {
                files.remove(path);
                return;
            };
            job
        };
        let current = || shared.latest.load(Ordering::Relaxed) == job.generation;
        if !current() {
            continue;
        }
        let frame = locate_frame(path, job.target, &current);
        if current() {
            let _ = shared.answers.send(Located {
                generation: job.generation,
                frame,
            });
        }
    }
}

/// The frame of the FLAC file at `path` that a seek to `target` starts
/// from: one that starts at most about half a second before it. Stops
/// early once `current` no longer holds.
pub(super) fn locate_frame(
    path: &Path,
    target: Duration,
    current: &dyn Fn() -> bool,
) -> Option<LocatedFrame> {
    let file = File::open(path).ok()?;
    let reader = Reader { file: &file, path };
    let stream = StreamInfo::read(&reader)?;
    let target_sample =
        u64::try_from(target.as_nanos() * u128::from(stream.sample_rate) / 1_000_000_000).ok()?;
    let first = stream.frame_at(&reader, stream.first_frame)?;
    let mut best = first;
    let mut low = first.offset;
    let mut high = stream.file_len;
    let close_enough = u64::from(stream.sample_rate / 2);
    while high.saturating_sub(low) > stream.window && current() {
        if target_sample.saturating_sub(best.sample) <= close_enough {
            break;
        }
        let middle = low + (high - low) / 2;
        match stream.frame_at(&reader, middle) {
            Some(frame) if frame.sample <= target_sample => {
                best = frame;
                low = frame.offset + 1;
            }
            _ => high = middle,
        }
    }
    if !current() {
        return None;
    }
    let relative = best.sample.checked_sub(first.sample)?;
    let nanos = u128::from(relative) * 1_000_000_000 / u128::from(stream.sample_rate);
    Some(LocatedFrame {
        offset: best.offset,
        nanos: u64::try_from(nanos).ok()?,
    })
}

/// Reads the file of a request.
struct Reader<'a> {
    file: &'a File,
    path: &'a Path,
}

impl Reader<'_> {
    /// Up to `len` bytes at `offset`; fewer at the end of the file.
    fn read(&self, offset: u64, len: usize) -> Option<Vec<u8>> {
        #[cfg(test)]
        super::file_source::test_support::wait_if_held(
            self.path,
            super::file_source::test_support::Point::Locate,
        );
        #[cfg(not(test))]
        let _ = self.path;
        let mut data = vec![0; len];
        let mut filled = 0;
        while filled < len {
            match self
                .file
                .read_at(&mut data[filled..], offset + filled as u64)
            {
                Ok(0) => break,
                Ok(count) => filled += count,
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                Err(_) => return None,
            }
        }
        data.truncate(filled);
        Some(data)
    }
}

/// A frame header found in the file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FoundFrame {
    offset: u64,
    /// The number of its first sample.
    sample: u64,
}

/// What the STREAMINFO block tells about the frames of a file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct StreamInfo {
    first_frame: u64,
    file_len: u64,
    sample_rate: u32,
    channels: u8,
    bits_per_sample: u8,
    /// The block size of every frame but the last, if all have one.
    fixed_block: Option<u32>,
    /// Bytes read wherever the bisection looks: room for a frame header and
    /// the header of the frame after it.
    window: u64,
}

/// The longest metadata the locator reads before the first frame: a large
/// embedded picture lies there.
const MAX_METADATA: u64 = 64 * 1024 * 1024;

impl StreamInfo {
    fn read(reader: &Reader<'_>) -> Option<Self> {
        let file_len = reader.file.metadata().ok()?.len();
        // A stream behind an ID3v2 tag reaches the parser without the tag,
        // so the parser's byte positions are not those of the file.
        let mut offset = 0;
        if reader.read(offset, 4)? != b"fLaC" {
            return None;
        }
        offset += 4;
        let mut info = None;
        loop {
            let header = reader.read(offset, 4)?;
            if header.len() < 4 || offset > MAX_METADATA {
                return None;
            }
            let last = header[0] & 0x80 != 0;
            let kind = header[0] & 0x7F;
            let len = u64::from(u32::from_be_bytes([0, header[1], header[2], header[3]]));
            if kind == 0 {
                info = Some(reader.read(offset + 4, 34)?).filter(|block| block.len() == 34);
            }
            offset += 4 + len;
            if last {
                break;
            }
        }
        let block = info?;
        let min_block = u16::from_be_bytes([block[0], block[1]]);
        let max_block = u16::from_be_bytes([block[2], block[3]]);
        let max_frame = u32::from_be_bytes([0, block[7], block[8], block[9]]);
        let sample_rate =
            (u32::from(block[10]) << 12) | (u32::from(block[11]) << 4) | u32::from(block[12] >> 4);
        let channels = ((block[12] >> 1) & 0x07) + 1;
        let bits_per_sample = (((block[12] & 0x01) << 4) | (block[13] >> 4)) + 1;
        if sample_rate == 0 {
            return None;
        }
        // Unknown frame sizes get room for large frames.
        let window = if max_frame == 0 {
            256 * 1024
        } else {
            (4 * u64::from(max_frame)).clamp(64 * 1024, 1024 * 1024)
        };
        Some(Self {
            first_frame: offset,
            file_len,
            sample_rate,
            channels,
            bits_per_sample,
            fixed_block: (min_block == max_block).then_some(u32::from(max_block)),
            window,
        })
    }

    /// The first frame that starts at or after `offset` within the window
    /// read there, and is followed by the next frame of the stream.
    fn frame_at(&self, reader: &Reader<'_>, offset: u64) -> Option<FoundFrame> {
        let data = reader.read(offset, usize::try_from(self.window).ok()?)?;
        let at_end = offset + data.len() as u64 >= self.file_len;
        (0..data.len()).find_map(|start| {
            let header = self.parse_header(&data[start..])?;
            let next = header.next_number();
            let mut later = (start + 2..data.len())
                .filter_map(|later| self.parse_header(&data[later..]))
                .peekable();
            // The last frame of the file has none after it.
            let last = at_end && later.peek().is_none();
            (last || later.any(|following| following.number == next)).then_some(FoundFrame {
                offset: offset + start as u64,
                sample: self.first_sample(&header),
            })
        })
    }

    fn first_sample(&self, header: &FrameHeader) -> u64 {
        if header.variable {
            header.number
        } else {
            header.number * u64::from(self.fixed_block.unwrap_or(header.block_size))
        }
    }

    /// The frame header that `data` starts with, if it is one of this
    /// stream.
    fn parse_header(&self, data: &[u8]) -> Option<FrameHeader> {
        let header = FrameHeader::parse(data)?;
        let matches = header
            .sample_rate
            .is_none_or(|rate| rate == self.sample_rate)
            && header.channels == self.channels
            && header
                .bits_per_sample
                .is_none_or(|bits| bits == self.bits_per_sample)
            && (header.variable || self.fixed_block.is_some());
        matches.then_some(header)
    }
}

/// A FLAC frame header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FrameHeader {
    /// Whether frames vary in block size, so `number` counts samples.
    variable: bool,
    /// The frame number, or with `variable`, the number of its first sample.
    number: u64,
    block_size: u32,
    /// `None` when the stream info gives it.
    sample_rate: Option<u32>,
    channels: u8,
    /// `None` when the stream info gives it.
    bits_per_sample: Option<u8>,
    /// Length of the header in bytes.
    len: usize,
}

impl FrameHeader {
    /// The header that `data` starts with: its sync code, valid fields and
    /// CRC-8.
    fn parse(data: &[u8]) -> Option<Self> {
        if data.len() < 6 || data[0] != 0xFF || data[1] & 0xFE != 0xF8 {
            return None;
        }
        let variable = data[1] & 0x01 != 0;
        let block_code = data[2] >> 4;
        let rate_code = data[2] & 0x0F;
        let channel_code = data[3] >> 4;
        let size_code = (data[3] >> 1) & 0x07;
        if block_code == 0 || rate_code == 0x0F || channel_code > 10 || size_code == 3 {
            return None;
        }
        if data[3] & 0x01 != 0 {
            return None;
        }
        let (number, mut len) = utf8_number(&data[4..], if variable { 7 } else { 6 })?;
        len += 4;
        let block_size = match block_code {
            1 => 192,
            2..=5 => 576 << (block_code - 2),
            6 => {
                let size = u32::from(*data.get(len)?) + 1;
                len += 1;
                size
            }
            7 => {
                let size =
                    u32::from(u16::from_be_bytes([*data.get(len)?, *data.get(len + 1)?])) + 1;
                len += 2;
                size
            }
            _ => 256 << (block_code - 8),
        };
        let sample_rate = match rate_code {
            0 => None,
            1 => Some(88_200),
            2 => Some(176_400),
            3 => Some(192_000),
            4 => Some(8_000),
            5 => Some(16_000),
            6 => Some(22_050),
            7 => Some(24_000),
            8 => Some(32_000),
            9 => Some(44_100),
            10 => Some(48_000),
            11 => Some(96_000),
            12 => {
                let rate = u32::from(*data.get(len)?) * 1_000;
                len += 1;
                Some(rate)
            }
            _ => {
                let value = u32::from(u16::from_be_bytes([*data.get(len)?, *data.get(len + 1)?]));
                len += 2;
                Some(if rate_code == 13 { value } else { value * 10 })
            }
        };
        if crc8(data.get(..len)?) != *data.get(len)? {
            return None;
        }
        Some(Self {
            variable,
            number,
            block_size,
            sample_rate,
            channels: if channel_code < 8 {
                channel_code + 1
            } else {
                2
            },
            bits_per_sample: match size_code {
                0 => None,
                1 => Some(8),
                2 => Some(12),
                4 => Some(16),
                5 => Some(20),
                6 => Some(24),
                _ => Some(32),
            },
            len: len + 1,
        })
    }

    /// The number in the header of the frame after this one.
    fn next_number(&self) -> u64 {
        if self.variable {
            self.number + u64::from(self.block_size)
        } else {
            self.number + 1
        }
    }
}

/// The number coded as UTF-8 at the start of `data`, of at most `max_len`
/// bytes, and its length.
fn utf8_number(data: &[u8], max_len: usize) -> Option<(u64, usize)> {
    let first = *data.first()?;
    let len = match first.leading_ones() {
        0 => return Some((u64::from(first), 1)),
        1 => return None,
        ones => usize::try_from(ones).ok()?,
    };
    if len > max_len {
        return None;
    }
    let mut value = u64::from(first & (0x7F >> len));
    for byte in data.get(1..len)? {
        if byte & 0xC0 != 0x80 {
            return None;
        }
        value = (value << 6) | u64::from(byte & 0x3F);
    }
    Some((value, len))
}

/// CRC-8 of a FLAC frame header: polynomial 0x07, initial value 0.
fn crc8(data: &[u8]) -> u8 {
    data.iter().fold(0, |crc, byte| {
        (0..8).fold(crc ^ byte, |crc, _| {
            if crc & 0x80 != 0 {
                (crc << 1) ^ 0x07
            } else {
                crc << 1
            }
        })
    })
}

#[cfg(test)]
mod tests {
    use super::{crc8, locate_frame, utf8_number, FrameHeader};
    use gst::prelude::*;
    use gstreamer as gst;
    use std::time::Duration;

    /// 60 s of noise at 48 kHz in 24-bit stereo, encoded by `flacenc`
    /// without a seek table, in a file of its own.
    fn noise_flac(name: &str) -> std::path::PathBuf {
        gst::init().expect("GStreamer");
        let path = std::env::temp_dir().join(format!(
            "ferrous-seek-locate-{name}-{}.flac",
            std::process::id()
        ));
        let pipeline = gst::parse::launch(&format!(
            "audiotestsrc wave=white-noise volume=0.1 num-buffers=2813 samplesperbuffer=1024 \
             ! audio/x-raw,rate=48000,channels=2,format=S24LE ! audioconvert \
             ! flacenc seekpoints=0 ! filesink location=\"{}\"",
            path.display()
        ))
        .expect("encoder");
        pipeline.set_state(gst::State::Playing).expect("encode");
        let message = pipeline.bus().expect("bus").timed_pop_filtered(
            gst::ClockTime::from_seconds(30),
            &[gst::MessageType::Eos, gst::MessageType::Error],
        );
        pipeline.set_state(gst::State::Null).expect("stop");
        assert!(
            matches!(
                message.as_ref().map(|message| message.view()),
                Some(gst::MessageView::Eos(_))
            ),
            "{message:?}"
        );
        path
    }

    #[test]
    fn located_frames_start_shortly_before_their_targets() {
        let path = noise_flac("targets");
        let bytes = std::fs::read(&path).expect("read fixture");
        for millis in [0, 700, 15_250, 30_000, 47_777, 59_900] {
            let target = Duration::from_millis(millis);
            let frame = locate_frame(&path, target, &|| true).expect("a frame");
            let header = FrameHeader::parse(&bytes[usize::try_from(frame.offset).unwrap()..])
                .unwrap_or_else(|| panic!("no frame header at {} for {target:?}", frame.offset));
            // Fixed blocks of 4608 samples at 48 kHz.
            assert!(!header.variable);
            assert_eq!(
                frame.nanos,
                header.number * 4608 * 1_000_000_000 / 48_000,
                "{target:?}"
            );
            let start = Duration::from_nanos(frame.nanos);
            assert!(start <= target, "{start:?} after {target:?}");
            assert!(
                target - start < Duration::from_millis(600),
                "{start:?} for {target:?}"
            );
        }
        // Past the end: the last frames.
        let frame = locate_frame(&path, Duration::from_secs(90), &|| true).expect("a frame");
        assert!(frame.nanos > 59_000_000_000, "{frame:?}");
        // A request that no longer applies is not answered.
        assert_eq!(
            locate_frame(&path, Duration::from_secs(30), &|| false),
            None
        );
        std::fs::remove_file(path).expect("remove fixture");
    }

    /// A header with its CRC.
    fn with_crc(mut header: Vec<u8>) -> Vec<u8> {
        header.push(crc8(&header));
        header
    }

    #[test]
    fn crc8_matches_the_flac_reference() {
        // The check value of CRC-8 with polynomial 0x07.
        assert_eq!(crc8(b"123456789"), 0xF4);
    }

    #[test]
    fn utf8_numbers_decode_as_flac_codes_them() {
        assert_eq!(utf8_number(&[0x7F], 6), Some((0x7F, 1)));
        assert_eq!(utf8_number(&[0xC2, 0x80], 6), Some((0x80, 2)));
        assert_eq!(utf8_number(&[0xE0, 0xA0, 0x80], 6), Some((0x800, 3)));
        // A continuation byte cannot start a number.
        assert_eq!(utf8_number(&[0x80], 6), None);
        // Seven bytes code a sample number, not a frame number.
        let seven = [0xFE, 0x82, 0x80, 0x80, 0x80, 0x80, 0x80];
        assert_eq!(utf8_number(&seven, 6), None);
        assert_eq!(utf8_number(&seven, 7), Some((1 << 31, 7)));
    }

    #[test]
    fn frame_headers_parse_with_their_sample_numbers() {
        // Fixed blocks of 4096 at 44.1 kHz, stereo, 16 bits, frame 2.
        let fixed = with_crc(vec![0xFF, 0xF8, 0xC9, 0x18, 0x02]);
        let header = FrameHeader::parse(&fixed).expect("fixed header");
        assert!(!header.variable);
        assert_eq!(
            (header.number, header.block_size, header.sample_rate),
            (2, 4096, Some(44_100))
        );
        assert_eq!((header.channels, header.bits_per_sample), (2, Some(16)));
        assert_eq!(header.next_number(), 3);

        // Variable blocks of 1000 (coded after the number), at sample
        // 0x800, at 48 kHz, 5.1, 24 bits.
        let variable = with_crc(vec![0xFF, 0xF9, 0x7A, 0x5C, 0xE0, 0xA0, 0x80, 0x03, 0xE7]);
        let header = FrameHeader::parse(&variable).expect("variable header");
        assert!(header.variable);
        assert_eq!((header.number, header.block_size), (0x800, 1000));
        assert_eq!((header.channels, header.bits_per_sample), (6, Some(24)));
        assert_eq!(header.next_number(), 0x800 + 1000);

        // A wrong CRC, a reserved bit and an invalid rate code fail.
        let mut damaged = fixed.clone();
        *damaged.last_mut().unwrap() ^= 1;
        assert_eq!(FrameHeader::parse(&damaged), None);
        assert_eq!(
            FrameHeader::parse(&with_crc(vec![0xFF, 0xF8, 0xC9, 0x19, 0x02])),
            None
        );
        assert_eq!(
            FrameHeader::parse(&with_crc(vec![0xFF, 0xF8, 0xCF, 0x18, 0x02])),
            None
        );
    }
}
