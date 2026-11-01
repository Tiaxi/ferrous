// SPDX-License-Identifier: GPL-3.0-or-later

//! Keeps damaged AC3 frames from reaching the decoder.
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

#[cfg(feature = "gst")]
use std::sync::{Mutex, PoisonError};

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

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
    /// The last intact frame since the stream started or was flushed.
    last_intact: Option<gst::Buffer>,
    /// Frames replaced in this stream.
    concealed: usize,
    /// Whether to log `concealed` at the end of the stream.
    report: bool,
}

#[cfg(feature = "gst")]
impl Concealer {
    /// What replaces the frame in `buffer`, if it is damaged: the last
    /// intact frame. Every AC3 frame stands on its own, and the decoder's
    /// transform overlaps it with its neighbours, so it leaves no seams.
    fn replacement(&mut self, buffer: &gst::Buffer) -> Option<gst::Buffer> {
        let map = buffer.map_readable().ok()?;
        match check_frame(&map) {
            Frame::Unchecked => None,
            Frame::Intact => {
                self.last_intact = Some(buffer.clone());
                None
            }
            Frame::Damaged => {
                self.concealed += 1;
                let frame = match &self.last_intact {
                    Some(intact) => intact.clone(),
                    None => gst::Buffer::from_mut_slice(silenced(&map)),
                };
                with_timing_of(buffer, &frame)
            }
        }
    }

    /// Nothing from before a flush or from another track may stand in for
    /// a frame after it: that would play audio from elsewhere, perhaps with
    /// another channel layout.
    fn handle_event(&mut self, event: gst::EventType) {
        match event {
            gst::EventType::FlushStop => self.last_intact = None,
            gst::EventType::StreamStart | gst::EventType::Eos => {
                self.last_intact = None;
                let concealed = std::mem::take(&mut self.concealed);
                if self.report && concealed > 0 {
                    eprintln!("[ferrous] concealed {concealed} damaged AC3 frames");
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

/// Replace the damaged frames that reach `pad`, the sink pad of a decoder.
///
/// The probe runs on the streaming thread for every frame. It does no I/O,
/// and the lock only orders what that thread does: buffers, serialized
/// events, and the end of a flush.
#[cfg(feature = "gst")]
fn conceal_frames_at(pad: &gst::Pad, report: bool) {
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
            if let Some(buffer) = info.buffer() {
                if let Some(replacement) = concealer.replacement(buffer) {
                    info.data = Some(gst::PadProbeData::Buffer(replacement));
                }
            } else if let Some(event) = info.event() {
                concealer.handle_event(event.type_());
            }
            gst::PadProbeReturn::Ok
        },
    );
}

/// Conceal damaged frames in every AC3 decoder that `bin` plugs from now on,
/// at any depth below it, the ones of gapless handovers included.
#[cfg(feature = "gst")]
fn conceal_in_decoders_of(bin: &gst::Element, report: bool) {
    bin.connect("deep-element-added", false, move |values| {
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        if element
            .factory()
            .is_some_and(|factory| AC3_DECODERS.contains(&factory.name().as_str()))
        {
            conceal_frames_at(&element.static_pad("sink")?, report);
        }
        None
    });
}

/// Conceal the damaged AC3 frames of `pipeline`, a bin that analyses audio
/// and plugs its decoder itself. Call this before the pipeline starts.
#[cfg(feature = "gst")]
pub(crate) fn conceal_damaged_ac3_frames(pipeline: &gst::Element) {
    conceal_in_decoders_of(pipeline, false);
}

/// [`conceal_damaged_ac3_frames`] for the pipeline that plays a track, the
/// only one of those that decode it that logs how many frames were lost.
#[cfg(feature = "gst")]
pub(crate) fn conceal_and_report_damaged_ac3_frames(playbin: &gst::Element) {
    conceal_in_decoders_of(playbin, true);
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
        conceal_damaged_ac3_frames, record_decoder_input, silenced, with_damaged_frames,
        DecoderFrame, AC3_DECODERS, AC3_FRAME_LEN, SEAMED_FRAME, TEST_AC3_FRAMES,
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
            conceal_damaged_ac3_frames(pipeline.upcast_ref());
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
            conceal_damaged_ac3_frames(pipeline.upcast_ref());
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
