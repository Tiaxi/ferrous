// SPDX-License-Identifier: GPL-3.0-or-later

//! Prepares each AC3 frame for the decoder: keeps damaged frames from
//! reaching it, and neutralizes the dynamic range words of intact ones.
//!
//! `gst-libav` decodes with the CRC check of `libavcodec` on. That conceals a
//! frame whose CRC fails by repeating the last 256 samples of each channel
//! six times, and the seams of those repeats are a broadband burst across all
//! channels, the LFE included. Decoding such a frame regardless is no better:
//! one flipped byte can decode far above full scale.
//!
//! So a pad probe at the decoder checks each frame itself and replaces a
//! damaged one in place, which leaves the buffer's timestamps and sample
//! count, and with them seeks and gapless handovers, as they are.
//!
//! The decoder also applies a frame's dynamic range words, which compress
//! the audio, until its first flush, but not after: see
//! [`super::ac3_dynamic_range`]. So the probe sets the words of each intact
//! frame to 0, and the frame decodes at the full dynamic range it was mixed
//! at, from the start and after a seek alike.

#[cfg(feature = "gst")]
use std::sync::{Mutex, PoisonError};

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

#[cfg(feature = "gst")]
use super::ac3_dynamic_range::{find_range_flags, RangeFlags, Unparsed};
use super::bitstream::plain_ac3_frame_len;

/// The decoders with a sink pad that takes AC3 frames from `ac3parse`.
#[cfg(feature = "gst")]
const AC3_DECODERS: [&str; 2] = ["avdec_ac3", "avdec_ac3_fixed"];

/// The CRC-16 of A/52 section 7.10, with the polynomial
/// x^16 + x^15 + x^2 + 1, by byte.
const CRC_TABLE: [u16; 256] = {
    let mut table = [0; 256];
    let mut byte = 0;
    while byte < table.len() {
        // Lossless: `byte` is below 256.
        #[allow(clippy::cast_possible_truncation)]
        let mut crc = (byte as u16) << 8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x8000 == 0 {
                crc << 1
            } else {
                (crc << 1) ^ 0x8005
            };
            bit += 1;
        }
        table[byte] = crc;
        byte += 1;
    }
    table
};

fn crc(data: &[u8]) -> u16 {
    data.iter().fold(0, |crc, &byte| {
        let index = crc.to_be_bytes()[0] ^ byte;
        (crc << 8) ^ CRC_TABLE[usize::from(index)]
    })
}

#[derive(Debug, PartialEq, Eq)]
enum Frame {
    Intact,
    Damaged,
    /// Not one AC3 frame: E-AC3, which has its CRC elsewhere, or bytes that
    /// do not parse as a frame of their length.
    Unchecked,
}

/// Multiply `crc`, a remainder of the CRC's generator, by x^-`times`: the
/// generator's constant term lets each step divide by x.
fn divided_by_x(mut crc: u16, times: usize) -> u16 {
    for _ in 0..times {
        crc = if crc & 1 == 0 {
            crc >> 1
        } else {
            // Add the generator, x^16 + 0x8005, then divide by x.
            ((crc ^ 0x8005) >> 1) | 0x8000
        };
    }
    crc
}

/// Set both CRCs of `frame`, a whole AC3 frame (A/52 section 7.10):
/// `crc1`, the two bytes after the sync word, so that the CRC of the first
/// 5/8 of the frame after the sync word is 0, and `crc2`, the last two
/// bytes, so that the CRC of the rest is 0 too.
fn set_crcs(frame: &mut [u8]) {
    let len = frame.len();
    let words = len / 2;
    let five_eighths = ((words >> 1) + (words >> 3)) * 2;
    let rest = &frame[4..five_eighths];
    // The CRC of `crc1` followed by `rest` is (crc1 * x^(8 * rest.len()) +
    // rest) * x^16 modulo the generator, which is 0 for this `crc1`.
    let crc1 = divided_by_x(crc(rest), 8 * rest.len() + 16);
    frame[2..4].copy_from_slice(&crc1.to_be_bytes());
    let crc2 = crc(&frame[five_eighths..len - 2]);
    frame[len - 2..].copy_from_slice(&crc2.to_be_bytes());
}

/// Check `data`, the bytes of one buffer. The CRC of a whole frame after the
/// sync word is 0, as `libavcodec` checks it.
fn check_frame(data: &[u8]) -> Frame {
    if plain_ac3_frame_len(data) != Some(data.len()) {
        return Frame::Unchecked;
    }
    if crc(&data[2..]) == 0 {
        Frame::Intact
    } else {
        Frame::Damaged
    }
}

/// `frame` with no audio, for a damaged frame with no intact one before it
/// in its stream. `libavcodec` conceals this frame whether or not it checks
/// the CRC: block 0 must set a coupling strategy, so a block of zeros fails
/// there, and the decoder repeats its last output. That is silence at the
/// start of a stream, and after a flush or a new stream too, as `gst-libav`
/// resets the decoder at both. The sample count stays, as it would not with
/// a dropped frame.
///
/// The header up to `lfeon` is kept, as it tells the sample rate and the
/// channels; `lfeon` ends in byte 6 at the latest. In the rest of the header
/// a zero flag leaves out what would follow it.
fn silenced(frame: &[u8]) -> Vec<u8> {
    const KEPT_HEADER_BYTES: usize = 7;
    let mut silenced = frame.to_vec();
    silenced[KEPT_HEADER_BYTES..].fill(0);
    silenced
}

/// What the probe of one decoder knows of its stream.
#[cfg(feature = "gst")]
#[derive(Default)]
struct Concealer {
    /// The last intact frame since the stream started or was flushed, with
    /// its dynamic range words neutralized.
    last_intact: Option<gst::Buffer>,
    /// Frames replaced in this stream.
    concealed: usize,
    /// Intact frames in this stream whose dynamic range words were not
    /// found, and why the last of them was not.
    unparsed: usize,
    unparsed_reason: Option<Unparsed>,
    /// Whether to log `concealed` and `unparsed` at the end of the stream.
    report: bool,
}

/// What the probe does with a frame.
#[cfg(feature = "gst")]
enum Preparation {
    /// Leave it: not a frame it checks, or an intact frame with no word that
    /// changes the gain.
    Keep,
    /// Set its dynamic range words to 0.
    Neutralize(RangeFlags),
    /// Replace it, a damaged frame.
    Replace(gst::Buffer),
}

#[cfg(feature = "gst")]
impl Concealer {
    /// Prepare the frame in `buffer` for the decoder. A damaged frame is
    /// replaced by the last intact frame: every AC3 frame stands on its
    /// own, and the decoder's transform overlaps it with its neighbours, so
    /// it leaves no seams. An intact frame gets its dynamic range words set
    /// to 0, in place if `buffer` is writable.
    fn prepare(&mut self, buffer: &mut gst::Buffer) {
        // The audio before a discontinuity is from elsewhere.
        if buffer.flags().contains(gst::BufferFlags::DISCONT) {
            self.last_intact = None;
        }
        let preparation = {
            let Ok(map) = buffer.map_readable() else {
                return;
            };
            match check_frame(&map) {
                Frame::Unchecked => return,
                Frame::Intact => match find_range_flags(&map) {
                    Ok(flags) if flags.compress(&map) => Preparation::Neutralize(flags),
                    Ok(_) => Preparation::Keep,
                    Err(reason) => {
                        self.unparsed += 1;
                        self.unparsed_reason = Some(reason);
                        Preparation::Keep
                    }
                },
                Frame::Damaged => {
                    self.concealed += 1;
                    let frame = match &self.last_intact {
                        Some(intact) => intact.clone(),
                        None => gst::Buffer::from_mut_slice(silenced(&map)),
                    };
                    match with_timing_of(buffer, &frame) {
                        Some(replacement) => Preparation::Replace(replacement),
                        None => return,
                    }
                }
            }
        };
        match preparation {
            Preparation::Keep => self.last_intact = Some(buffer.clone()),
            Preparation::Neutralize(flags) => {
                if let Ok(mut map) = buffer.make_mut().map_writable() {
                    flags.neutralize(&mut map);
                    set_crcs(&mut map);
                }
                self.last_intact = Some(buffer.clone());
            }
            Preparation::Replace(replacement) => *buffer = replacement,
        }
    }

    /// Nothing from before a flush, from another track or in another format
    /// may stand in for a frame after it: that would play audio from
    /// elsewhere, perhaps with another channel layout or another length.
    fn handle_event(&mut self, event: gst::EventType) {
        match event {
            gst::EventType::FlushStop | gst::EventType::Caps => self.last_intact = None,
            gst::EventType::StreamStart | gst::EventType::Eos => {
                self.last_intact = None;
                let concealed = std::mem::take(&mut self.concealed);
                if self.report && concealed > 0 {
                    eprintln!("[ferrous] concealed {concealed} damaged AC3 frames");
                }
                let unparsed = std::mem::take(&mut self.unparsed);
                if let Some(reason) = self.unparsed_reason.take().filter(|_| self.report) {
                    eprintln!(
                        "[ferrous] {unparsed} AC3 frames kept their dynamic range words: \
                         {reason:?}"
                    );
                }
            }
            _ => {}
        }
    }
}

/// The bytes of `frame` with the timestamps and flags of `original`.
#[cfg(feature = "gst")]
fn with_timing_of(original: &gst::Buffer, frame: &gst::Buffer) -> Option<gst::Buffer> {
    let mut buffer = gst::Buffer::new();
    let target = buffer.get_mut()?;
    original
        .copy_into(
            target,
            gst::BufferCopyFlags::FLAGS
                | gst::BufferCopyFlags::TIMESTAMPS
                | gst::BufferCopyFlags::META,
            ..,
        )
        .ok()?;
    frame
        .copy_into(target, gst::BufferCopyFlags::MEMORY, ..)
        .ok()?;
    Some(buffer)
}

/// Prepare the frames that reach `pad`, the sink pad of a decoder: replace
/// the damaged ones and neutralize the dynamic range words of the others.
///
/// The probe runs on the streaming thread for every frame. It does no I/O,
/// and the lock only orders what that thread does: buffers, serialized
/// events, and the end of a flush.
#[cfg(feature = "gst")]
fn prepare_frames_at(pad: &gst::Pad, report: bool) {
    let concealer = Mutex::new(Concealer {
        report,
        ..Concealer::default()
    });
    pad.add_probe(
        gst::PadProbeType::BUFFER
            | gst::PadProbeType::EVENT_DOWNSTREAM
            | gst::PadProbeType::EVENT_FLUSH,
        move |_pad, info| {
            let mut concealer = concealer.lock().unwrap_or_else(PoisonError::into_inner);
            if let Some(gst::PadProbeData::Buffer(buffer)) = &mut info.data {
                concealer.prepare(buffer);
            } else if let Some(event) = info.event() {
                concealer.handle_event(event.type_());
            }
            gst::PadProbeReturn::Ok
        },
    );
}

/// Prepare the frames of every AC3 decoder that `bin` plugs from now on, at
/// any depth below it, the ones of gapless handovers included.
#[cfg(feature = "gst")]
fn prepare_in_decoders_of(bin: &gst::Element, report: bool) {
    bin.connect("deep-element-added", false, move |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        if element
            .factory()
            .is_some_and(|factory| AC3_DECODERS.contains(&factory.name().as_str()))
        {
            prepare_frames_at(&element.static_pad("sink")?, report);
        }
        None
    });
}

/// Conceal the damaged AC3 frames of `pipeline`, a bin that analyses audio
/// and plugs its decoder itself, and neutralize the dynamic range words of
/// the others. Call this before the pipeline starts.
#[cfg(feature = "gst")]
pub(crate) fn prepare_ac3_frames(pipeline: &gst::Element) {
    prepare_in_decoders_of(pipeline, false);
}

/// [`prepare_ac3_frames`] for the pipeline that plays a track, the only one
/// of those that decode it that logs how many frames were lost or kept
/// their dynamic range words.
#[cfg(feature = "gst")]
pub(crate) fn prepare_and_report_ac3_frames(playbin: &gst::Element) {
    prepare_in_decoders_of(playbin, true);
}

/// The length of the frames of `TEST_AC3`.
#[cfg(test)]
pub(crate) const AC3_FRAME_LEN: usize = 128;
/// The number of frames of `TEST_AC3`.
#[cfg(test)]
pub(crate) const TEST_AC3_FRAMES: usize = 32;
/// A frame of `TEST_AC3` where short repeats show: 256 samples of the tone
/// do not end near where they start, unlike in frame 16, say.
#[cfg(all(test, feature = "gst"))]
pub(crate) const SEAMED_FRAME: usize = 19;

/// `stream`, in frames of [`AC3_FRAME_LEN`] bytes, with the audio of the
/// frames `damaged` overwritten and their CRC left as it was.
#[cfg(test)]
pub(crate) fn with_damaged_frames(stream: &[u8], damaged: &[usize]) -> Vec<u8> {
    let mut bytes = stream.to_vec();
    for frame in damaged {
        for byte in &mut bytes[frame * AC3_FRAME_LEN + 8..(frame + 1) * AC3_FRAME_LEN - 4] {
            *byte ^= 0xA5;
        }
    }
    bytes
}

/// The raw AC3 fixtures, by name: `TEST_AC3`, and short streams of
/// `FFmpeg`'s encoder in other channel layouts, some with coupling; see
/// `fixtures/README.md`.
#[cfg(test)]
pub(crate) const AC3_LAYOUTS: [(&str, &[u8]); 7] = [
    ("mono", super::TEST_AC3),
    (
        "stereo, coupled",
        include_bytes!("../analysis/fixtures/raw-stereo-coupled.ac3"),
    ),
    ("2.1", include_bytes!("../analysis/fixtures/raw-2.1.ac3")),
    (
        "3/1, coupled",
        include_bytes!("../analysis/fixtures/raw-3.1-coupled.ac3"),
    ),
    ("5.0", include_bytes!("../analysis/fixtures/raw-5.0.ac3")),
    (
        "5.1, coupled",
        include_bytes!("../analysis/fixtures/raw-5.1-coupled.ac3"),
    ),
    (
        "5.1, gated noise",
        include_bytes!("../analysis/fixtures/raw-5.1-gated.ac3"),
    ),
];

/// A dynamic range word that turns the audio down by 12 dB, to 1/4.
#[cfg(test)]
pub(crate) const COMPRESSING_DYNRNG: u8 = 0xC0;

/// Bits written most significant first.
#[cfg(test)]
#[derive(Default)]
struct BitWriter {
    bytes: Vec<u8>,
    bits: usize,
}

#[cfg(test)]
impl BitWriter {
    /// Append the low `count` bits of `value`, after zeros if `count` is
    /// above 32.
    fn put(&mut self, value: u32, count: usize) {
        for bit in (0..count).rev() {
            if self.bits.is_multiple_of(8) {
                self.bytes.push(0);
            }
            if bit < 32 && (value >> bit) & 1 == 1 {
                self.bytes[self.bits / 8] |= 0x80 >> (self.bits % 8);
            }
            self.bits += 1;
        }
    }

    /// Append the bits of `data` from `range`.
    fn copy(&mut self, data: &[u8], range: std::ops::Range<usize>) {
        for index in range {
            self.put(u32::from((data[index / 8] >> (7 - index % 8)) & 1), 1);
        }
    }
}

/// `stream`, plain AC3 frames without dynamic range words, as `FFmpeg`'s
/// encoder writes them, with `dynrng` in every block of each frame, and in
/// dual mono `dynrng2` too, as an encoder that compresses writes them. The
/// encoder leaves no room for the words, so each frame grows to the next
/// bitrate, with zeros after its audio, and gets its CRCs again; it decodes
/// as before but for the words.
#[cfg(test)]
pub(crate) fn with_dynamic_range(stream: &[u8], dynrng: u8) -> Vec<u8> {
    let mut compressed = Vec::with_capacity(stream.len() * 2);
    let mut rest = stream;
    while !rest.is_empty() {
        let len = plain_ac3_frame_len(rest).expect("a plain AC3 frame");
        let (frame, next) = rest.split_at(len);
        rest = next;
        let flags = find_range_flags_for_test(frame);
        let mut bits = BitWriter::default();
        let mut copied = 0;
        for &flag in flags.positions() {
            assert_eq!(frame[flag / 8] & (0x80 >> (flag % 8)), 0, "a word already");
            bits.copy(frame, copied..flag);
            bits.put(0x100 | u32::from(dynrng), 9);
            copied = flag + 1;
        }
        // All but `crc2`, which `set_crcs` writes again.
        bits.copy(frame, copied..(len - 2) * 8);
        let used = bits.bits;
        let mut grown = bits.bytes;
        // The next `frmsizecod` but one has the next bitrate.
        grown[4] += 2;
        let grown_len = plain_ac3_frame_len(&grown).expect("a larger frame size");
        assert!(grown_len * 8 >= used + 16, "room for the words");
        grown.resize(grown_len, 0);
        set_crcs(&mut grown);
        compressed.extend(grown);
    }
    compressed
}

#[cfg(test)]
fn find_range_flags_for_test(frame: &[u8]) -> super::ac3_dynamic_range::RangeFlags {
    super::ac3_dynamic_range::find_range_flags(frame).expect("a frame that parses")
}

/// A frame of silence in the format of `frame`, an AC3 frame of the length
/// its header tells. The header up to `lfeon` is kept; the rest holds no
/// option. The first block sets exponents, and a bit allocation with all
/// SNR offsets 0, which gives no coefficient any bits; the other blocks
/// reuse them.
#[cfg(test)]
pub(crate) fn silent_frame(frame: &[u8]) -> Vec<u8> {
    use super::ac3_dynamic_range::FULL_BANDWIDTH_CHANNELS;
    // `acmod`, the 3 bits after `bsid` and `bsmod`, starts byte 6.
    let acmod = usize::from(frame[6] >> 5);
    let mut lfeon_bit = 51;
    // `cmixlev`, `surmixlev`, and `dsurmod`.
    lfeon_bit += 2 * usize::from(acmod & 1 == 1 && acmod != 1)
        + 2 * usize::from(acmod & 4 != 0)
        + 2 * usize::from(acmod == 2);
    let lfe = frame[lfeon_bit / 8] & (0x80 >> (lfeon_bit % 8)) != 0;
    let channels = FULL_BANDWIDTH_CHANNELS[acmod];
    // Dual mono has a second set of the per-programme fields.
    let programmes = if acmod == 0 { 2 } else { 1 };

    let mut bits = BitWriter::default();
    bits.copy(frame, 0..lfeon_bit + 1);
    for _ in 0..programmes {
        // `dialnorm` -31 dB, and no `compre`, `langcode`, or `audprodie`.
        bits.put(31, 5);
        bits.put(0, 3);
    }
    // No `copyrightb`, `origbs`, `timecod1e`, `timecod2e`, or `addbsie`.
    bits.put(0, 5);
    for block in 0..6 {
        // `blksw` and `dithflag` of each channel, and no `dynrnge`.
        bits.put(0, 2 * channels + programmes);
        if block == 0 {
            // `cplstre` set and `cplinu` clear: no coupling.
            bits.put(0b10, 2);
            if acmod == 2 {
                // `rematstr` and 4 clear `rematflg`.
                bits.put(0b1_0000, 5);
            }
            // `chexpstr` D45, `lfeexpstr` D15, and `chbwcod` 0: 73
            // coefficients, in 6 groups of D45 exponents.
            for _ in 0..channels {
                bits.put(0b11, 2);
            }
            if lfe {
                bits.put(1, 1);
            }
            bits.put(0, 6 * channels);
            // Each group of three exponent deltas codes 0, 0, 0 as 62.
            for _ in 0..channels {
                bits.put(0, 4);
                for _ in 0..6 {
                    bits.put(62, 7);
                }
                // `gainrng`.
                bits.put(0, 2);
            }
            if lfe {
                bits.put(0, 4);
                bits.put(62, 7);
                bits.put(62, 7);
            }
            // `baie`, then the usual decay, gain, and floor codes.
            bits.put(1, 1);
            bits.put(0b0101_0110, 8);
            bits.put(0b100, 3);
            // `snroffste`, with `csnroffst` and each channel's `fsnroffst`
            // and `fgaincod` 0.
            bits.put(1, 1);
            bits.put(0, 6 + 7 * (channels + usize::from(lfe)));
            // No `deltbaie` or `skiple`.
            bits.put(0, 2);
        } else {
            // No `cplstre`, `rematstr`, new exponents, `baie`,
            // `snroffste`, `deltbaie`, or `skiple`.
            let rematrixing = usize::from(acmod == 2);
            bits.put(0, 1 + rematrixing + 2 * channels + usize::from(lfe) + 4);
        }
    }
    let mut silent = bits.bytes;
    silent.resize(frame.len(), 0);
    set_crcs(&mut silent);
    silent
}

/// A 48 kHz frame of the bitrate of `frmsizecod` whose header, up to
/// `lfeon`, tells the channels `acmod` and `lfe`, and is silent.
#[cfg(test)]
pub(crate) fn silent_frame_of(frmsizecod: u8, acmod: u8, lfe: bool) -> Vec<u8> {
    let mut bits = BitWriter::default();
    // The sync word, `crc1`, `fscod` 48 kHz, `frmsizecod`, `bsid` 8, and
    // `bsmod`.
    for byte in [0x0B, 0x77, 0, 0, frmsizecod, 8 << 3] {
        bits.put(u32::from(byte), 8);
    }
    bits.put(u32::from(acmod), 3);
    let mix_fields = 2 * usize::from(acmod & 1 == 1 && acmod != 1)
        + 2 * usize::from(acmod & 4 != 0)
        + 2 * usize::from(acmod == 2);
    bits.put(0, mix_fields);
    bits.put(u32::from(lfe), 1);
    let mut frame = bits.bytes;
    let len = plain_ac3_frame_len(&frame).expect("a frame length");
    frame.resize(len, 0);
    silent_frame(&frame)
}

/// Whether `frame` is one whose CRC fails.
#[cfg(all(test, feature = "gst"))]
pub(crate) fn ac3_frame_is_damaged(frame: &[u8]) -> bool {
    check_frame(frame) == Frame::Damaged
}

/// A frame as it reached the decoder.
#[cfg(all(test, feature = "gst"))]
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct DecoderFrame {
    pub(crate) pts: Option<gst::ClockTime>,
    pub(crate) duration: Option<gst::ClockTime>,
    pub(crate) bytes: Vec<u8>,
}

/// Record what reaches each AC3 decoder that `bin` plugs from now on. To see
/// the frames after the concealment, connect this after it.
#[cfg(all(test, feature = "gst"))]
pub(crate) fn record_decoder_input(bin: &gst::Element) -> std::sync::Arc<Mutex<Vec<DecoderFrame>>> {
    let frames = std::sync::Arc::<Mutex<Vec<DecoderFrame>>>::default();
    let recorded = std::sync::Arc::clone(&frames);
    bin.connect("deep-element-added", false, move |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        let factory = element.factory()?;
        if AC3_DECODERS.contains(&factory.name().as_str()) {
            let frames = std::sync::Arc::clone(&recorded);
            let pad = element.static_pad("sink")?;
            pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, info| {
                if let Some(buffer) = info.buffer() {
                    let bytes = buffer.map_readable().expect("map frame").to_vec();
                    frames.lock().expect("frames").push(DecoderFrame {
                        pts: buffer.pts(),
                        duration: buffer.duration(),
                        bytes,
                    });
                }
                gst::PadProbeReturn::Ok
            });
        }
        None
    });
    frames
}

#[cfg(test)]
mod tests {
    use super::TEST_AC3_FRAMES;
    use super::{check_frame, crc, silenced, with_damaged_frames, Frame, AC3_FRAME_LEN};
    use crate::raw_audio::TEST_AC3;

    #[test]
    fn the_crc_matches_the_check_value_of_its_parameters() {
        // CRC-16/UMTS: polynomial 0x8005, no reflection, initial value 0.
        assert_eq!(crc(b"123456789"), 0xFEE8);
    }

    #[test]
    fn every_frame_of_a_clean_stream_is_intact() {
        assert_eq!(TEST_AC3.len(), TEST_AC3_FRAMES * AC3_FRAME_LEN);
        for frame in TEST_AC3.chunks(AC3_FRAME_LEN) {
            assert_eq!(check_frame(frame), Frame::Intact);
        }
    }

    #[test]
    fn a_flipped_bit_after_the_sync_word_is_damage() {
        let frame = &TEST_AC3[..AC3_FRAME_LEN];
        // Byte 4 gives the length, so a flip there leaves a frame that does
        // not parse, and the next test covers that.
        for index in (2..4).chain(5..AC3_FRAME_LEN) {
            for bit in 0..8 {
                let mut flipped = frame.to_vec();
                flipped[index] ^= 1 << bit;
                let plain = flipped[5] >> 3 <= 10;
                let expected = if plain {
                    Frame::Damaged
                } else {
                    Frame::Unchecked
                };
                assert_eq!(check_frame(&flipped), expected, "byte {index} bit {bit}");
            }
        }
    }

    #[test]
    fn bytes_that_are_not_one_ac3_frame_are_left_alone() {
        let damaged = with_damaged_frames(&TEST_AC3[..AC3_FRAME_LEN], &[0]);
        assert_eq!(check_frame(&damaged), Frame::Damaged);

        // Shorter and longer than the header says.
        assert_eq!(check_frame(&damaged[..AC3_FRAME_LEN - 1]), Frame::Unchecked);
        assert_eq!(
            check_frame(&[&damaged[..], &[0]].concat()),
            Frame::Unchecked
        );
        // Several frames in one buffer, and too little for a header.
        assert_eq!(check_frame(&damaged.repeat(2)), Frame::Unchecked);
        assert_eq!(check_frame(&damaged[..4]), Frame::Unchecked);
        assert_eq!(check_frame(&[]), Frame::Unchecked);
        // No sync word.
        let mut no_sync = damaged.clone();
        no_sync[0] = 0;
        assert_eq!(check_frame(&no_sync), Frame::Unchecked);
        // A reserved sample rate, and a frame size that does not exist.
        let mut reserved = damaged.clone();
        reserved[4] |= 0xC0;
        assert_eq!(check_frame(&reserved), Frame::Unchecked);
        let mut no_size = damaged;
        no_size[4] = 38;
        assert_eq!(check_frame(&no_size), Frame::Unchecked);
    }

    #[test]
    fn e_ac3_frames_are_left_alone() {
        let mut frame = with_damaged_frames(&TEST_AC3[..AC3_FRAME_LEN], &[0]);
        // A `frmsiz` that gives the length of the frame.
        frame[2] = 0;
        frame[3] = 63;
        for bsid in [11, 16] {
            frame[5] = bsid << 3;
            assert_eq!(check_frame(&frame), Frame::Unchecked, "bsid {bsid}");
        }
    }

    #[test]
    fn the_highest_plain_ac3_bsid_is_checked() {
        let mut frame = with_damaged_frames(&TEST_AC3[..AC3_FRAME_LEN], &[0]);
        frame[5] = 10 << 3;
        assert_eq!(check_frame(&frame), Frame::Damaged);
    }

    #[test]
    fn a_silenced_frame_keeps_its_header_and_length() {
        let frame = &TEST_AC3[..AC3_FRAME_LEN];
        let silenced = silenced(frame);
        assert_eq!(silenced.len(), frame.len());
        assert_eq!(silenced[..7], frame[..7]);
        assert!(silenced[7..].iter().all(|byte| *byte == 0));
    }
}

/// Damaged frames through the real parser and decoder.
#[cfg(all(test, feature = "gst"))]
mod pipeline_tests {
    use super::{
        prepare_ac3_frames, record_decoder_input, silenced, with_damaged_frames,
        with_dynamic_range, Concealer, DecoderFrame, AC3_DECODERS, AC3_FRAME_LEN, AC3_LAYOUTS,
        COMPRESSING_DYNRNG, SEAMED_FRAME, TEST_AC3_FRAMES,
    };
    use crate::raw_audio::{
        assert_no_burst, configure_raw_surround_decoding, largest_step, test_path, TEST_AC3,
    };
    use gst::prelude::*;
    use gstreamer as gst;
    use std::sync::{Arc, Mutex};

    const FRAME_SAMPLES: usize = 1536;

    #[derive(Default)]
    struct Decoded {
        frames: Vec<DecoderFrame>,
        samples: Vec<f32>,
    }

    fn record_samples(sink: &gst::Element) -> Arc<Mutex<Vec<f32>>> {
        let samples = Arc::<Mutex<Vec<f32>>>::default();
        let recorded = Arc::clone(&samples);
        let pad = sink.static_pad("sink").expect("sink pad");
        pad.add_probe(gst::PadProbeType::BUFFER, move |_pad, info| {
            if let Some(buffer) = info.buffer() {
                let map = buffer.map_readable().expect("map samples");
                let mut samples = recorded.lock().expect("samples");
                samples.extend(
                    map.chunks_exact(4)
                        .map(|bytes| f32::from_le_bytes(bytes.try_into().expect("4 bytes"))),
                );
            }
            gst::PadProbeReturn::Ok
        });
        samples
    }

    /// `audioconvert`, a filter for floating-point samples, and a sink.
    fn pcm_output() -> [gst::Element; 3] {
        let caps = gst::Caps::builder("audio/x-raw")
            .field("format", "F32LE")
            .build();
        [
            gst::ElementFactory::make("audioconvert")
                .build()
                .expect("audioconvert"),
            gst::ElementFactory::make("capsfilter")
                .property("caps", &caps)
                .build()
                .expect("capsfilter"),
            gst::ElementFactory::make("fakesink")
                .property("sync", false)
                .property("async", false)
                .build()
                .expect("fakesink"),
        ]
    }

    /// Decode `stream` through `decodebin`, as the analysis pipelines do,
    /// with the concealment if `conceal` is set, and record what reached the
    /// decoder.
    fn decode(name: &str, stream: &[u8], conceal: bool) -> Decoded {
        gst::init().expect("gst init");
        configure_raw_surround_decoding();
        let path = test_path(name, "ac3");
        std::fs::write(&path, stream).expect("write fixture");

        let pipeline = gst::Pipeline::new();
        if conceal {
            prepare_ac3_frames(pipeline.upcast_ref());
        }
        let frames = record_decoder_input(pipeline.upcast_ref());
        let source = gst::ElementFactory::make("filesrc")
            .property("location", path.to_string_lossy().to_string())
            .build()
            .expect("filesrc");
        let decodebin = gst::ElementFactory::make("decodebin")
            .build()
            .expect("decodebin");
        let output = pcm_output();
        pipeline
            .add_many([&source, &decodebin])
            .expect("add elements");
        pipeline.add_many(&output).expect("add output");
        source.link(&decodebin).expect("link decodebin");
        gst::Element::link_many(&output).expect("link output");
        let convert_pad = output[0].static_pad("sink").expect("convert pad");
        decodebin.connect_pad_added(move |_, pad| {
            let _ = pad.link(&convert_pad);
        });
        let samples = record_samples(&output[2]);

        pipeline.set_state(gst::State::Playing).expect("play");
        let bus = pipeline.bus().expect("bus");
        let message = bus
            .timed_pop_filtered(
                gst::ClockTime::from_seconds(10),
                &[gst::MessageType::Eos, gst::MessageType::Error],
            )
            .expect("the decoder reaches the end of the stream");
        let _ = pipeline.set_state(gst::State::Null);
        let _ = std::fs::remove_file(&path);
        assert!(
            matches!(message.view(), gst::MessageView::Eos(..)),
            "{message:?}"
        );
        let decoded = Decoded {
            frames: std::mem::take(&mut *frames.lock().expect("frames")),
            samples: std::mem::take(&mut *samples.lock().expect("samples")),
        };
        decoded
    }

    /// Assert that `decoded` has the length and the timing of the clean
    /// stream, and neither a louder sample nor a sharper step than the tone.
    fn assert_sounds_clean(name: &str, decoded: &Decoded, clean: &Decoded) {
        assert_eq!(
            decoded.samples.len(),
            clean.samples.len(),
            "{name}: sample count"
        );
        assert_no_burst(name, &decoded.samples);
        assert_eq!(decoded.frames.len(), clean.frames.len(), "{name}: frames");
        for (index, (frame, clean)) in decoded.frames.iter().zip(&clean.frames).enumerate() {
            assert_eq!(
                (frame.pts, frame.duration),
                (clean.pts, clean.duration),
                "{name}: timing of frame {index}"
            );
        }
    }

    #[test]
    fn the_decoder_turns_a_damaged_frame_into_a_burst_without_concealment() {
        // What the other tests guard against: if this stops holding, their
        // damage no longer shows and they prove nothing.
        let clean = decode("burst-clean", TEST_AC3, false);
        let stream = with_damaged_frames(TEST_AC3, &[SEAMED_FRAME]);
        let damaged = decode("burst-damaged", &stream, false);
        assert_no_burst("clean", &clean.samples);
        let step = largest_step(&damaged.samples);
        assert!(
            step > 10.0 * largest_step(&clean.samples),
            "the largest step is {step}, in a tone with {}",
            largest_step(&clean.samples)
        );
    }

    #[test]
    fn a_new_format_or_a_discontinuity_ends_what_can_stand_in() {
        gst::init().expect("gst init");
        let stream = with_damaged_frames(TEST_AC3, &[1]);
        let intact = gst::Buffer::from_slice(stream[..AC3_FRAME_LEN].to_vec());
        let damaged_bytes = stream[AC3_FRAME_LEN..2 * AC3_FRAME_LEN].to_vec();
        let damaged = gst::Buffer::from_slice(damaged_bytes.clone());
        let mut discontinuous = damaged.clone();
        discontinuous
            .make_mut()
            .set_flags(gst::BufferFlags::DISCONT);
        // The bytes of `buffer` as the probe leaves them.
        let prepared = |concealer: &mut Concealer, buffer: &gst::Buffer| {
            let mut buffer = buffer.clone();
            concealer.prepare(&mut buffer);
            let map = buffer.map_readable().expect("map frame");
            map.to_vec()
        };

        let mut concealer = Concealer::default();
        assert_eq!(prepared(&mut concealer, &intact), stream[..AC3_FRAME_LEN]);
        assert_eq!(prepared(&mut concealer, &damaged), stream[..AC3_FRAME_LEN]);

        concealer.handle_event(gst::EventType::Caps);
        assert_eq!(prepared(&mut concealer, &damaged), silenced(&damaged_bytes));

        assert_eq!(prepared(&mut concealer, &intact), stream[..AC3_FRAME_LEN]);
        assert_eq!(
            prepared(&mut concealer, &discontinuous),
            silenced(&damaged_bytes)
        );
    }

    #[test]
    fn a_damaged_frame_repeats_the_one_before_it() {
        let clean = decode("middle-clean", TEST_AC3, true);
        let stream = with_damaged_frames(TEST_AC3, &[SEAMED_FRAME]);
        let decoded = decode("middle", &stream, true);
        assert_sounds_clean("middle", &decoded, &clean);
        for (index, (frame, clean)) in decoded.frames.iter().zip(&clean.frames).enumerate() {
            if index == SEAMED_FRAME {
                let before = &decoded.frames[index - 1].bytes;
                assert_eq!(&frame.bytes, before, "frame {index}");
            } else {
                assert_eq!(frame.bytes, clean.bytes, "frame {index}");
            }
        }
    }

    #[test]
    fn damaged_frames_in_a_row_all_repeat_the_last_intact_one() {
        let clean = decode("row-clean", TEST_AC3, true);
        let stream = with_damaged_frames(TEST_AC3, &[12, 13, 14, SEAMED_FRAME]);
        let decoded = decode("row", &stream, true);
        assert_sounds_clean("row", &decoded, &clean);
        for index in [12, 13, 14] {
            assert_eq!(decoded.frames[index].bytes, clean.frames[11].bytes);
        }
        assert_eq!(decoded.frames[15].bytes, clean.frames[15].bytes);
        assert_eq!(
            decoded.frames[SEAMED_FRAME].bytes,
            clean.frames[SEAMED_FRAME - 1].bytes
        );
    }

    #[test]
    fn a_damaged_first_frame_decodes_to_silence() {
        let clean = decode("first-clean", TEST_AC3, true);
        let stream = with_damaged_frames(TEST_AC3, &[0]);
        let decoded = decode("first", &stream, true);
        assert_sounds_clean("first", &decoded, &clean);
        assert_eq!(
            decoded.frames[0].bytes,
            silenced(&stream[..AC3_FRAME_LEN]),
            "no damaged frame reaches the decoder"
        );
        assert!(decoded.samples[..FRAME_SAMPLES]
            .iter()
            .all(|sample| *sample == 0.0));
        assert_eq!(decoded.frames[1].bytes, clean.frames[1].bytes);
    }

    #[test]
    fn damaged_frames_from_the_start_decode_to_silence_until_an_intact_one() {
        let clean = decode("start-clean", TEST_AC3, true);
        let stream = with_damaged_frames(TEST_AC3, &[0, 1, 2]);
        let decoded = decode("start", &stream, true);
        assert_sounds_clean("start", &decoded, &clean);
        assert!(decoded.samples[..3 * FRAME_SAMPLES]
            .iter()
            .all(|sample| *sample == 0.0));
        assert_eq!(decoded.frames[3].bytes, clean.frames[3].bytes);
    }

    #[test]
    fn a_stream_of_only_damaged_frames_decodes_to_silence_of_its_length() {
        let clean = decode("all-clean", TEST_AC3, true);
        let every_frame: Vec<usize> = (0..TEST_AC3_FRAMES).collect();
        let stream = with_damaged_frames(TEST_AC3, &every_frame);
        let decoded = decode("all", &stream, true);
        assert_sounds_clean("all", &decoded, &clean);
        assert!(decoded.samples.iter().all(|sample| *sample == 0.0));
    }

    #[test]
    fn dynamic_range_words_reach_no_decoder_and_the_audio_decodes_as_mixed() {
        for (index, (layout, stream)) in AC3_LAYOUTS.into_iter().enumerate() {
            let plain = decode(&format!("drc-plain-{index}"), stream, true);
            let compressed = with_dynamic_range(stream, COMPRESSING_DYNRNG);
            let decoded = decode(&format!("drc-{index}"), &compressed, true);
            assert!(!plain.samples.is_empty(), "{layout}");
            assert!(
                decoded.samples == plain.samples,
                "{layout}: the samples differ"
            );
        }
    }

    #[test]
    fn without_the_probe_the_decoder_applies_dynamic_range_words_from_the_start() {
        // What the test before guards against. The words compress by 12 dB,
        // a power of 2, which scales every sample exactly: the decoder
        // reads each word where it was written, in every block.
        for (index, (layout, stream)) in AC3_LAYOUTS.into_iter().enumerate() {
            let plain = decode(&format!("drc-applied-plain-{index}"), stream, false);
            let compressed = with_dynamic_range(stream, COMPRESSING_DYNRNG);
            let applied = decode(&format!("drc-applied-{index}"), &compressed, false);
            let quarter: Vec<f32> = plain.samples.iter().map(|sample| sample / 4.0).collect();
            assert!(!quarter.is_empty(), "{layout}");
            assert!(applied.samples == quarter, "{layout}: not a quarter");
        }
    }

    #[test]
    fn a_clean_stream_passes_unchanged() {
        let plain = decode("clean-plain", TEST_AC3, false);
        let concealed = decode("clean-concealed", TEST_AC3, true);
        assert_eq!(concealed.frames, plain.frames);
        assert_eq!(concealed.samples, plain.samples);
    }

    /// A decoder in a nested bin, as the bins that plug one have it, fed
    /// directly so that a test decides what happens between its frames.
    struct Decoder {
        pipeline: gst::Pipeline,
        pad: gst::Pad,
        samples: Arc<Mutex<Vec<f32>>>,
        next_frame: u64,
    }

    impl Decoder {
        fn start() -> Self {
            gst::init().expect("gst init");
            let pipeline = gst::Pipeline::new();
            prepare_ac3_frames(pipeline.upcast_ref());
            let bin = gst::Bin::new();
            pipeline.add(&bin).expect("add bin");
            let decoder = gst::ElementFactory::make(AC3_DECODERS[0])
                .build()
                .expect("decoder");
            let output = pcm_output();
            bin.add(&decoder).expect("add decoder");
            bin.add_many(&output).expect("add output");
            decoder.link(&output[0]).expect("link decoder");
            gst::Element::link_many(&output).expect("link output");
            let samples = record_samples(&output[2]);
            pipeline.set_state(gst::State::Playing).expect("play");
            let decoder = Self {
                pipeline,
                pad: decoder.static_pad("sink").expect("decoder pad"),
                samples,
                next_frame: 0,
            };
            decoder.start_track("first");
            decoder
        }

        fn send(&self, event: gst::Event) {
            assert!(self.pad.send_event(event), "the decoder takes the event");
        }

        fn start_segment(&self) {
            let segment = gst::FormattedSegment::<gst::ClockTime>::new();
            self.send(gst::event::Segment::new(&segment));
        }

        fn start_track(&self, id: &str) {
            self.send(gst::event::StreamStart::new(id));
            let caps = gst::Caps::builder("audio/x-ac3")
                .field("framed", true)
                .field("rate", 48_000i32)
                .field("channels", 1i32)
                .build();
            self.send(gst::event::Caps::new(&caps));
            self.start_segment();
        }

        /// Start a track on the same decoder, as a gapless handover does.
        /// Forget what was decoded before: the decoder restarts its
        /// overlap, so the audio jumps there with or without damage.
        fn next_track(&self, id: &str) {
            self.start_track(id);
            self.samples.lock().expect("samples").clear();
        }

        /// Flush, and forget what was decoded before, as a seek leaves it.
        fn flush(&self) {
            self.send(gst::event::FlushStart::new());
            self.send(gst::event::FlushStop::new(true));
            self.samples.lock().expect("samples").clear();
            self.start_segment();
        }

        /// Push `stream` in frames, each 32 ms after the last.
        fn push(&mut self, stream: &[u8]) {
            for frame in stream.chunks(AC3_FRAME_LEN) {
                let mut buffer = gst::Buffer::from_slice(frame.to_vec());
                let buffer_mut = buffer.get_mut().expect("new buffer");
                buffer_mut.set_pts(gst::ClockTime::from_mseconds(32 * self.next_frame));
                buffer_mut.set_duration(gst::ClockTime::from_mseconds(32));
                self.next_frame += 1;
                self.pad.chain(buffer).expect("the decoder takes the frame");
            }
        }

        /// End the stream and return all that was decoded.
        fn finish(self) -> Vec<f32> {
            self.send(gst::event::Eos::new());
            let _ = self.pipeline.set_state(gst::State::Null);
            let samples = std::mem::take(&mut *self.samples.lock().expect("samples"));
            samples
        }
    }

    #[test]
    fn a_damaged_frame_after_a_flush_decodes_to_silence() {
        let mut decoder = Decoder::start();
        decoder.push(&TEST_AC3[..SEAMED_FRAME * AC3_FRAME_LEN]);
        decoder.flush();
        decoder.push(&with_damaged_frames(TEST_AC3, &[0]));
        let decoded = decoder.finish();
        assert_eq!(decoded.len(), TEST_AC3_FRAMES * FRAME_SAMPLES);
        assert!(decoded[..FRAME_SAMPLES].iter().all(|s| *s == 0.0));
        assert_no_burst("flush", &decoded);
    }

    #[test]
    fn damaged_frames_at_the_start_of_a_track_on_a_kept_decoder_decode_to_silence() {
        let mut decoder = Decoder::start();
        decoder.push(&TEST_AC3[..SEAMED_FRAME * AC3_FRAME_LEN]);
        decoder.next_track("second");
        decoder.push(&with_damaged_frames(TEST_AC3, &[0, 1]));
        let decoded = decoder.finish();
        assert_eq!(decoded.len(), TEST_AC3_FRAMES * FRAME_SAMPLES);
        assert!(decoded[..2 * FRAME_SAMPLES].iter().all(|s| *s == 0.0));
        assert_no_burst("handover", &decoded);
    }
}
