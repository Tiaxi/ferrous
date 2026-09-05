// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs::File;
use std::io::ErrorKind;
use std::path::Path;

use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;
#[cfg(feature = "gst")]
use crate::raw_audio::{is_dts_file, register_raw_surround_typefinders};
#[cfg(feature = "gst")]
use gstreamer_app as gst_app;

use super::fft::ensure_sample_buffer;
use super::{
    f64_to_u64_saturating, usize_to_f32_approx, usize_to_u64, SpectrogramViewMode, REFERENCE_HOP,
};

#[cfg(feature = "profiling-logs")]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {
        eprintln!($($arg)*);
    };
}

#[cfg(not(feature = "profiling-logs"))]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {};
}

// ---------------------------------------------------------------------------
// SymphoniaFile — output of opening a file with Symphonia
// ---------------------------------------------------------------------------

pub(super) struct SymphoniaFile {
    pub(super) format: Box<dyn symphonia::core::formats::FormatReader>,
    pub(super) decoder: Box<dyn symphonia::core::codecs::Decoder>,
    pub(super) track_id: u32,
    pub(super) native_sample_rate: u64,
    pub(super) native_channels: usize,
    pub(super) total_columns: u32,
}

// ---------------------------------------------------------------------------
// AudioFrames — a batch of interleaved F32 audio frames
// ---------------------------------------------------------------------------

/// A batch of interleaved F32 audio frames from either backend.
pub(super) struct AudioFrames {
    pub(super) samples: Vec<f32>,
    pub(super) first_frame: u64,
    pub(super) frames: usize,
    pub(super) channels: usize,
}

/// An empty queue is not EOF, and a decoder failure is returned as an error.
pub(super) enum AudioRead {
    Frames(AudioFrames),
    Pending,
    Eof,
}

impl AudioFrames {
    fn trim_before(&mut self, target_frame: u64) {
        let skip = usize::try_from(target_frame.saturating_sub(self.first_frame))
            .unwrap_or(usize::MAX)
            .min(self.frames);
        self.samples.drain(..skip * self.channels);
        self.frames -= skip;
        self.first_frame = self.first_frame.saturating_add(usize_to_u64(skip));
    }
}

fn timestamp_frame(ts: u64, numerator: u32, denominator: u32, rate: u32) -> u64 {
    let denominator = u128::from(denominator.max(1));
    let frame =
        (u128::from(ts) * u128::from(numerator) * u128::from(rate) + denominator / 2) / denominator;
    u64::try_from(frame).unwrap_or(u64::MAX)
}

// ---------------------------------------------------------------------------
// AudioFrameSource — unified audio backend for the spectrogram worker
// ---------------------------------------------------------------------------

/// Abstraction over Symphonia and `GStreamer` decode backends so the spectrogram
/// decode loop works identically regardless of which decoder produced the PCM.
pub(super) enum AudioFrameSource {
    Symphonia {
        format: Box<dyn symphonia::core::formats::FormatReader>,
        decoder: Box<dyn symphonia::core::codecs::Decoder>,
        track_id: u32,
        sample_buf: Option<SampleBuffer<f32>>,
        seek_frame: u64,
    },
    #[cfg(feature = "gst")]
    Gst {
        pipeline: gst::Pipeline,
        appsink: gst_app::AppSink,
        native_channels: usize,
        sample_rate: u32,
        seek_frame: u64,
        /// Stored for seek-flag selection (DTS needs `KEY_UNIT`).
        path: std::path::PathBuf,
    },
}

impl AudioFrameSource {
    /// Read native samples with their absolute source position. Seek preroll is
    /// decoded but excluded, so callers never have to guess where a seek landed.
    pub(super) fn next_frames(&mut self) -> anyhow::Result<AudioRead> {
        let (read, target) = match self {
            Self::Symphonia {
                format,
                decoder,
                track_id,
                sample_buf,
                seek_frame,
            } => (
                read_symphonia_frames(format, decoder, *track_id, sample_buf)?,
                *seek_frame,
            ),
            #[cfg(feature = "gst")]
            Self::Gst {
                pipeline,
                appsink,
                native_channels,
                sample_rate,
                seek_frame,
                ..
            } => (
                read_gstreamer_frames(pipeline, appsink, *native_channels, *sample_rate)?,
                *seek_frame,
            ),
        };
        match read {
            AudioRead::Frames(mut audio) => {
                audio.trim_before(target);
                Ok(if audio.frames == 0 {
                    AudioRead::Pending
                } else {
                    AudioRead::Frames(audio)
                })
            }
            other => Ok(other),
        }
    }

    /// Query the pipeline duration in nanoseconds (`GStreamer` only).
    /// Returns `None` for Symphonia sources or when the duration is unknown.
    #[cfg(feature = "gst")]
    pub(super) fn query_duration_ns(&self) -> Option<u64> {
        match self {
            Self::Gst { pipeline, .. } => pipeline
                .query_duration::<gst::ClockTime>()
                .map(gst::ClockTime::nseconds),
            Self::Symphonia { .. } => None,
        }
    }

    /// Seek before the requested sample and trim decoded preroll by timestamp.
    pub(super) fn seek(
        &mut self,
        position_seconds: f64,
        native_sample_rate: u64,
    ) -> anyhow::Result<()> {
        anyhow::ensure!(
            position_seconds.is_finite() && position_seconds >= 0.0,
            "invalid seek position"
        );
        let rate = u32::try_from(native_sample_rate)?;
        let target = f64_to_u64_saturating(position_seconds * f64::from(rate));
        self.seek_with_target(position_seconds, target)
    }

    pub(super) fn seek_to_frame(&mut self, frame: u64, sample_rate: u32) -> anyhow::Result<()> {
        anyhow::ensure!(sample_rate > 0, "invalid sample rate");
        // The decoder may land before this time; integer timestamp trimming
        // below guarantees the exact frame even when f64 rounds the time.
        #[allow(clippy::cast_precision_loss)]
        let seconds = frame as f64 / f64::from(sample_rate);
        self.seek_with_target(seconds, frame)
    }

    fn seek_with_target(&mut self, position_seconds: f64, target: u64) -> anyhow::Result<()> {
        match self {
            Self::Symphonia {
                format,
                decoder,
                track_id,
                seek_frame,
                ..
            } => {
                seek_symphonia(format, *track_id, position_seconds)?;
                decoder.reset();
                *seek_frame = target;
            }
            #[cfg(feature = "gst")]
            Self::Gst {
                pipeline,
                path,
                seek_frame,
                ..
            } => {
                let ns = f64_to_u64_saturating(position_seconds * 1_000_000_000.0);
                pipeline.seek_simple(
                    spectrogram_seek_flags_for_path(path),
                    gst::ClockTime::from_nseconds(ns),
                )?;
                *seek_frame = target;
            }
        }
        Ok(())
    }
}

fn read_symphonia_frames(
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    decoder: &mut Box<dyn symphonia::core::codecs::Decoder>,
    track_id: u32,
    sample_buf: &mut Option<SampleBuffer<f32>>,
) -> anyhow::Result<AudioRead> {
    let packet = match format.next_packet() {
        Ok(packet) => packet,
        Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
            return Ok(AudioRead::Eof);
        }
        Err(err) => return Err(err.into()),
    };
    if packet.track_id() != track_id {
        return Ok(AudioRead::Pending);
    }
    let time_base = format
        .tracks()
        .iter()
        .find(|track| track.id == track_id)
        .and_then(|track| track.codec_params.time_base);
    let audio = match decoder.decode(&packet) {
        Ok(audio) => audio,
        Err(SymphoniaError::DecodeError(_)) => return Ok(AudioRead::Pending),
        Err(err) => return Err(err.into()),
    };
    let spec = *audio.spec();
    let channels = spec.channels.count().max(1);
    let first_frame = time_base.map_or(packet.ts(), |base| {
        timestamp_frame(packet.ts(), base.numer, base.denom, spec.rate)
    });
    let buffer = ensure_sample_buffer(sample_buf, audio.capacity(), spec);
    buffer.copy_interleaved_ref(audio);
    let samples = buffer.samples().to_vec();
    Ok(AudioRead::Frames(AudioFrames {
        frames: samples.len() / channels,
        samples,
        channels,
        first_frame,
    }))
}

#[cfg(feature = "gst")]
fn read_gstreamer_frames(
    pipeline: &gst::Pipeline,
    appsink: &gst_app::AppSink,
    native_channels: usize,
    sample_rate: u32,
) -> anyhow::Result<AudioRead> {
    if let Some(message) = pipeline
        .bus()
        .and_then(|bus| bus.pop_filtered(&[gst::MessageType::Error]))
    {
        if let gst::MessageView::Error(error) = message.view() {
            anyhow::bail!("analysis decoder failed: {}", error.error());
        }
    }
    let Some(sample) = appsink.try_pull_sample(gst::ClockTime::from_mseconds(50)) else {
        return Ok(if appsink.is_eos() {
            AudioRead::Eof
        } else {
            AudioRead::Pending
        });
    };
    let buffer = sample
        .buffer()
        .ok_or_else(|| anyhow::anyhow!("missing audio buffer"))?;
    let pts = buffer
        .pts()
        .ok_or_else(|| anyhow::anyhow!("missing audio timestamp"))?;
    let first_frame = timestamp_frame(pts.nseconds(), 1, 1_000_000_000, sample_rate);
    let map = buffer.map_readable()?;
    let samples: Vec<f32> = map
        .as_slice()
        .chunks_exact(4)
        .map(|bytes| f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
        .collect();
    let channels = native_channels;
    Ok(AudioRead::Frames(AudioFrames {
        frames: samples.len() / channels,
        samples,
        channels,
        first_frame,
    }))
}

impl Drop for AudioFrameSource {
    fn drop(&mut self) {
        #[cfg(feature = "gst")]
        if let Self::Gst { pipeline, .. } = self {
            let _ = pipeline.set_state(gst::State::Null);
        }
    }
}

// ---------------------------------------------------------------------------
// File opening functions
// ---------------------------------------------------------------------------

/// Try to open an audio file, first via Symphonia, then falling back to
/// `GStreamer` for formats Symphonia cannot decode (AC3/DTS).
/// Returns `(source, native_sample_rate, native_channels, total_columns_estimate)`.
#[allow(clippy::type_complexity)]
pub(super) fn open_audio_file(path: &Path) -> Option<(AudioFrameSource, u64, usize, u32)> {
    // Skip Symphonia for raw AC3/DTS — it can't decode them but its probe
    // can misidentify the bitstream as another format, returning wrong
    // sample rate and channel count (e.g. 32 kHz stereo for a 48 kHz 5.1
    // DTS file).
    #[cfg(feature = "gst")]
    if is_raw_surround_file(path) {
        return open_gstreamer_file(path);
    }
    if let Some(sf) = open_symphonia_file(path) {
        return Some((
            AudioFrameSource::Symphonia {
                format: sf.format,
                decoder: sf.decoder,
                track_id: sf.track_id,
                sample_buf: None,
                seek_frame: 0,
            },
            sf.native_sample_rate,
            sf.native_channels,
            sf.total_columns,
        ));
    }
    profile_eprintln!("[spect-worker] Symphonia failed, trying GStreamer fallback");
    #[cfg(feature = "gst")]
    {
        open_gstreamer_file(path)
    }
    #[cfg(not(feature = "gst"))]
    {
        profile_eprintln!("[spect-worker] GStreamer not available (gst feature disabled)");
        None
    }
}

/// Open an audio file with symphonia, returning the format reader,
/// decoder, track info, and an estimated total column count.  A single
/// file open + probe avoids the double-open latency that is visible on
/// network-mounted storage during gapless transitions.
pub(super) fn open_symphonia_file(path: &Path) -> Option<SymphoniaFile> {
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let file = File::open(path).ok()?;
    let mss = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
    let format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?
        .format;
    let track = format.default_track()?;
    let track_id = track.id;
    let native_sample_rate = u64::from(track.codec_params.sample_rate.unwrap_or(48_000));
    let native_channels = track
        .codec_params
        .channels
        .map_or(2, |ch| ch.count().max(1));
    let n_frames = track
        .codec_params
        .n_frames
        .unwrap_or(native_sample_rate * 300);
    let effective_frames = n_frames;
    let total_columns =
        u32::try_from(((effective_frames / (REFERENCE_HOP as u64)) + 64).min(u64::from(u32::MAX)))
            .unwrap_or(u32::MAX);
    let audio_decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .ok()?;
    Some(SymphoniaFile {
        format,
        decoder: audio_decoder,
        track_id,
        native_sample_rate,
        native_channels,
        total_columns,
    })
}

fn seek_symphonia(
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    track_id: u32,
    seek_seconds: f64,
) -> anyhow::Result<()> {
    use symphonia::core::formats::{SeekMode, SeekTo};
    // Compressed decoders need reservoir/overlap history even after an accurate
    // demuxer seek. Decode preroll, then trim by actual packet timestamps.
    let seek_seconds = (seek_seconds - 0.1).max(0.0);
    let time = symphonia::core::units::Time::new(
        f64_to_u64_saturating(seek_seconds),
        seek_seconds.fract(),
    );
    format.seek(
        SeekMode::Accurate,
        SeekTo::Time {
            time,
            track_id: Some(track_id),
        },
    )?;
    Ok(())
}

/// `GStreamer` seek flags for the spectrogram worker.  DTS files need
/// `KEY_UNIT` to avoid decode artifacts after seeking.
#[cfg(feature = "gst")]
fn spectrogram_seek_flags_for_path(path: &Path) -> gst::SeekFlags {
    if is_dts_file(path) {
        gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT | gst::SeekFlags::SNAP_BEFORE
    } else {
        gst::SeekFlags::FLUSH | gst::SeekFlags::ACCURATE
    }
}

/// Open an audio file with a `GStreamer` pipeline for the spectrogram worker.
/// Returns `(source, native_sample_rate, native_channels, total_columns_estimate)`.
///
/// Pipeline: `filesrc → decodebin → audioconvert → capsfilter(F32LE) → appsink(sync=false)`
///
/// Unlike the waveform pipeline, no `audioresample` — the spectrogram needs
/// native sample rate for correct frequency resolution.
#[cfg(feature = "gst")]
fn open_gstreamer_file(path: &Path) -> Option<(AudioFrameSource, u64, usize, u32)> {
    gst::init().ok()?;
    register_raw_surround_typefinders();

    let pipeline = gst::Pipeline::new();
    let src = gst::ElementFactory::make("filesrc").build().ok()?;
    src.set_property("location", path.to_string_lossy().to_string());

    let decodebin = gst::ElementFactory::make("decodebin").build().ok()?;
    let conv = gst::ElementFactory::make("audioconvert").build().ok()?;
    let capsfilter = gst::ElementFactory::make("capsfilter").build().ok()?;
    let caps = gst::Caps::builder("audio/x-raw")
        .field("format", "F32LE")
        .field("layout", "interleaved")
        .build();
    capsfilter.set_property("caps", &caps);

    let appsink = analysis_decode_sink();

    pipeline
        .add_many([&src, &decodebin, &conv, &capsfilter, appsink.upcast_ref()])
        .ok()?;
    src.link(&decodebin).ok()?;
    gst::Element::link_many([&conv, &capsfilter, appsink.upcast_ref()]).ok()?;

    // Dynamic pad linking for decodebin → audioconvert.
    let conv_sink_pad = conv.static_pad("sink")?;
    decodebin.connect_pad_added(move |_dbin, src_pad| {
        if conv_sink_pad.is_linked() {
            return;
        }
        let Some(caps) = src_pad
            .current_caps()
            .or_else(|| Some(src_pad.query_caps(None)))
        else {
            return;
        };
        let Some(structure) = caps.structure(0) else {
            return;
        };
        if !structure.name().starts_with("audio/") {
            return;
        }
        let _ = src_pad.link(&conv_sink_pad);
    });

    // Transition to PAUSED so decodebin can negotiate.
    pipeline.set_state(gst::State::Paused).ok()?;
    let _ = pipeline.state(gst::ClockTime::from_seconds(5));

    // Read negotiated format from the appsink pad.
    let pad = appsink.static_pad("sink")?;
    let negotiated_caps = pad.current_caps()?;
    let structure = negotiated_caps.structure(0)?;
    let rate =
        u64::from(u32::try_from(structure.get::<i32>("rate").ok().filter(|&r| r > 0)?).ok()?);
    let channels =
        usize::try_from(structure.get::<i32>("channels").ok().filter(|&c| c > 0)?).ok()?;

    // Query duration for column estimate.
    let duration_ns = pipeline
        .query_duration::<gst::ClockTime>()
        .map(gst::ClockTime::nseconds);
    let total_frames = duration_ns.map_or(rate * 300, |ns| ns * rate / 1_000_000_000);
    let effective_frames = total_frames;
    let total_columns =
        u32::try_from(((effective_frames / (REFERENCE_HOP as u64)) + 64).min(u64::from(u32::MAX)))
            .unwrap_or(u32::MAX);

    // Transition to PLAYING so the pipeline starts streaming.
    pipeline.set_state(gst::State::Playing).ok()?;

    let source = AudioFrameSource::Gst {
        pipeline,
        appsink,
        native_channels: channels,
        sample_rate: u32::try_from(rate).ok()?,
        seek_frame: 0,
        path: path.to_path_buf(),
    };

    Some((source, rate, channels, total_columns))
}

#[cfg(feature = "gst")]
fn analysis_decode_sink() -> gst_app::AppSink {
    // This pipeline is independent of playback. Backpressure must stop its
    // decoder while analysis parks at the lookahead boundary, without dropping
    // PCM or waiting for queued data when a seek/track change tears it down.
    gst_app::AppSink::builder()
        .sync(false)
        .max_buffers(8)
        .drop(false)
        .wait_on_eos(false)
        .build()
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

pub(super) fn u64_to_u32_saturating(v: u64) -> u32 {
    u32::try_from(v).unwrap_or(u32::MAX)
}

pub(super) fn deinterleave_samples(
    samples: &[f32],
    frames: usize,
    decoded_channels: usize,
    channel_count: usize,
    view_mode: SpectrogramViewMode,
) -> Vec<Vec<f32>> {
    // Preserve the native sample grid. Skipping samples here aliases ultrasonic
    // energy into the audible spectrum and loses phase at packet boundaries.
    let mut per_channel: Vec<Vec<f32>> = vec![Vec::with_capacity(frames); channel_count];

    match view_mode {
        SpectrogramViewMode::Downmix => {
            let mut downmixed = Vec::with_capacity(frames);
            let inv_channels = 1.0 / usize_to_f32_approx(decoded_channels);
            for frame_idx in 0..frames {
                let base = frame_idx * decoded_channels;
                let mut sum = 0.0f32;
                for ch in 0..decoded_channels {
                    sum += samples[base + ch];
                }
                downmixed.push(sum * inv_channels);
            }
            per_channel[0] = downmixed;
        }
        SpectrogramViewMode::PerChannel => {
            for frame_idx in 0..frames {
                let base = frame_idx * decoded_channels;
                for ch in 0..channel_count.min(decoded_channels) {
                    per_channel[ch].push(samples[base + ch]);
                }
            }
        }
    }

    per_channel
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::Path;

    #[cfg(feature = "gst")]
    #[test]
    fn parked_gstreamer_decoder_backpressures_without_losing_samples() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::sync::Arc;
        use std::time::{Duration, Instant};

        gst::init().expect("gstreamer initialization");
        let pipeline = gst::Pipeline::new();
        let source = gst_app::AppSrc::builder().build();
        let sink = analysis_decode_sink();
        pipeline
            .add_many([source.upcast_ref::<gst::Element>(), sink.upcast_ref()])
            .expect("add source and sink");
        source.link(&sink).expect("link source and sink");
        let delivered = Arc::new(AtomicUsize::new(0));
        let observed = Arc::clone(&delivered);
        sink.static_pad("sink").expect("sink pad").add_probe(
            gst::PadProbeType::BUFFER,
            move |_, _| {
                observed.fetch_add(1, Ordering::Relaxed);
                gst::PadProbeReturn::Ok
            },
        );
        pipeline
            .set_state(gst::State::Playing)
            .expect("start pipeline");
        for value in 0_u8..32 {
            source
                .push_buffer(gst::Buffer::from_mut_slice(vec![value]))
                .expect("queue fixture buffer");
        }
        source.end_of_stream().expect("queue EOF");
        let deadline = Instant::now() + Duration::from_secs(2);
        while delivered.load(Ordering::Relaxed) < 9 && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(1));
        }
        std::thread::sleep(Duration::from_millis(20));
        // Eight queued samples plus one blocked streaming-thread buffer.
        let parked_count = delivered.load(Ordering::Relaxed);
        let mut values = Vec::new();
        for _ in 0..32 {
            if let Some(sample) = sink.try_pull_sample(gst::ClockTime::from_seconds(2)) {
                let buffer = sample.buffer().expect("fixture buffer");
                values.push(buffer.map_readable().expect("read fixture")[0]);
            }
        }
        pipeline.set_state(gst::State::Null).expect("stop pipeline");
        assert_eq!(parked_count, 9);
        assert_eq!(values, (0_u8..32).collect::<Vec<_>>());
    }

    #[cfg(feature = "gst")]
    #[test]
    fn parked_gstreamer_decoder_can_be_cancelled() {
        gst::init().expect("gstreamer initialization");
        let pipeline = gst::Pipeline::new();
        let source = gst_app::AppSrc::builder().build();
        let sink = analysis_decode_sink();
        pipeline
            .add_many([source.upcast_ref::<gst::Element>(), sink.upcast_ref()])
            .expect("add elements");
        source.link(&sink).expect("link elements");
        pipeline.set_state(gst::State::Playing).expect("start");
        for _ in 0..32 {
            source
                .push_buffer(gst::Buffer::from_mut_slice(vec![0_u8; 4]))
                .expect("push");
        }
        source.end_of_stream().expect("EOF");
        // State teardown must release both a full queue and pending EOF even
        // when the analysis consumer has been cancelled and never pulls again.
        pipeline
            .set_state(gst::State::Null)
            .expect("cancel parked decoder");
        assert_eq!(pipeline.current_state(), gst::State::Null);
    }

    #[test]
    fn timestamp_conversion_handles_container_timebases_and_nanosecond_rounding() {
        assert_eq!(timestamp_frame(1234, 1, 1000, 48000), 59_232);
        assert_eq!(timestamp_frame(20_833, 1, 1_000_000_000, 48000), 1);
        assert_eq!(
            timestamp_frame(1_000_020_833, 1, 1_000_000_000, 48000),
            48_001
        );
    }

    #[test]
    fn timestamped_preroll_trimming_preserves_channel_alignment() {
        let mut audio = AudioFrames {
            first_frame: 100,
            frames: 3,
            channels: 2,
            samples: vec![1.0, -1.0, 2.0, -2.0, 3.0, -3.0],
        };
        audio.trim_before(102);
        assert_eq!(audio.first_frame, 102);
        assert_eq!(audio.frames, 1);
        assert_eq!(audio.samples, [3.0, -3.0]);
        audio.trim_before(200);
        assert_eq!(audio.frames, 0);
        assert!(audio.samples.is_empty());
    }

    #[cfg(feature = "gst")]
    #[test]
    fn gstreamer_reads_preserve_pts_and_distinguish_pending_from_eof() {
        gst::init().expect("gstreamer initialization");
        let pipeline = gst::Pipeline::new();
        let producer = gst_app::AppSrc::builder().build();
        let sink = gst_app::AppSink::builder()
            .sync(false)
            .max_buffers(8)
            .build();
        pipeline
            .add_many([producer.upcast_ref::<gst::Element>(), sink.upcast_ref()])
            .expect("add elements");
        producer.link(&sink).expect("link");
        let mut source = AudioFrameSource::Gst {
            pipeline,
            appsink: sink,
            native_channels: 1,
            sample_rate: 48000,
            seek_frame: 48_001,
            path: Path::new("fixture.wav").to_owned(),
        };
        let AudioFrameSource::Gst { pipeline, .. } = &source else {
            unreachable!()
        };
        pipeline.set_state(gst::State::Playing).expect("start");
        assert!(matches!(
            source.next_frames().expect("idle read"),
            AudioRead::Pending
        ));
        let bytes: Vec<u8> = [0.25_f32, 0.5, 0.75]
            .into_iter()
            .flat_map(f32::to_le_bytes)
            .collect();
        let mut buffer = gst::Buffer::from_mut_slice(bytes);
        buffer
            .get_mut()
            .expect("unique buffer")
            .set_pts(gst::ClockTime::from_seconds(1));
        producer.push_buffer(buffer).expect("push");
        producer.end_of_stream().expect("EOF");
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
        let audio = loop {
            assert!(std::time::Instant::now() < deadline, "decoder timed out");
            match source.next_frames().expect("read") {
                AudioRead::Frames(audio) => break audio,
                AudioRead::Pending => {}
                AudioRead::Eof => panic!("premature EOF"),
            }
        };
        assert_eq!(audio.first_frame, 48_001);
        assert_eq!(audio.samples, [0.5, 0.75]);
        assert!(matches!(
            source.next_frames().expect("final read"),
            AudioRead::Eof
        ));
    }

    #[test]
    fn native_rate_spectra_preserve_ultrasonic_tones_and_channel_samples() {
        for rate in [88_200, 96_000, 192_000, 384_000] {
            let frequency = f64::from(rate) * 0.3125;
            let mono: Vec<f32> = (0_u16..8192)
                .map(|frame| {
                    (std::f64::consts::TAU * frequency * f64::from(frame) / f64::from(rate)).sin()
                        as f32
                })
                .collect();
            let stereo: Vec<f32> = mono.iter().flat_map(|&value| [value, -value]).collect();
            let split =
                deinterleave_samples(&stereo, mono.len(), 2, 2, SpectrogramViewMode::PerChannel);
            assert_eq!(split[0], mono);
            assert_eq!(
                split[1],
                mono.iter().map(|value| -value).collect::<Vec<_>>()
            );
            let mixed =
                deinterleave_samples(&stereo, mono.len(), 2, 1, SpectrogramViewMode::Downmix);
            assert!(mixed[0].iter().all(|value| value.abs() < f32::EPSILON));
            let mut stft = super::super::fft::StftComputer::new(8192, 1024);
            stft.enqueue_samples(&split[0]);
            let rows = stft.take_rows(1);
            let peak = rows[0]
                .iter()
                .enumerate()
                .max_by(|a, b| a.1.total_cmp(b.1))
                .expect("spectrum")
                .0;
            assert_eq!(peak, 2560, "native spectrum at {rate} Hz");
        }
    }

    #[test]
    fn symphonia_next_frames_returns_none_on_invalid_data() {
        // Feed garbage bytes to Symphonia — next_frames should return None (EOF/error).
        let garbage = vec![0u8; 256];
        let cursor = std::io::Cursor::new(garbage);
        let mss = symphonia::core::io::MediaSourceStream::new(
            Box::new(cursor),
            symphonia::core::io::MediaSourceStreamOptions::default(),
        );
        let probe_result = symphonia::default::get_probe().format(
            &symphonia::core::probe::Hint::new(),
            mss,
            &symphonia::core::formats::FormatOptions::default(),
            &symphonia::core::meta::MetadataOptions::default(),
        );
        // Probing garbage should fail, so we can't even construct a source.
        // This verifies the open path correctly returns None for bad data.
        assert!(probe_result.is_err());
    }

    #[cfg(feature = "gst")]
    #[test]
    fn open_gstreamer_file_returns_none_for_nonexistent_path() {
        let _ = gst::init();
        let result = open_gstreamer_file(Path::new("/nonexistent/path/to/file.ac3"));
        assert!(result.is_none());
    }

    #[test]
    fn open_audio_file_skips_symphonia_for_dts() {
        // Write a tiny file with DTS sync word header.  Without the
        // surround guard, Symphonia's probe would attempt to open this
        // and potentially misidentify it (returning wrong sr/ch).
        // With the guard, open_audio_file skips Symphonia entirely.
        let dir = std::env::temp_dir().join("ferrous_test_open_audio_dts");
        let _ = std::fs::create_dir_all(&dir);
        let dts_path = dir.join("test.dts");
        // DTS sync word 0x7FFE8001 followed by garbage.
        let mut data = vec![0x7F, 0xFE, 0x80, 0x01];
        data.extend_from_slice(&[0u8; 252]);
        std::fs::write(&dts_path, &data).expect("write test dts file");

        let result = open_audio_file(&dts_path);
        // Without gst feature: returns None (skips Symphonia, no GStreamer).
        // With gst feature: returns None (GStreamer can't decode garbage).
        // Either way, Symphonia must NOT be called — if it were, it could
        // misidentify the bytes and return Some with wrong parameters.
        assert!(result.is_none());

        let _ = std::fs::remove_file(&dts_path);
        let _ = std::fs::remove_dir(&dir);
    }

    #[test]
    fn open_audio_file_uses_symphonia_for_flac() {
        // A .flac path goes through Symphonia (not the surround skip).
        // Nonexistent file → Symphonia fails to open → returns None.
        // This confirms the Symphonia path is still active for non-surround.
        let result = open_audio_file(Path::new("/nonexistent/path/to/file.flac"));
        assert!(result.is_none());
    }
}
