// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs::File;
use std::io::ErrorKind;
use std::path::Path;

use symphonia::core::audio::{AudioBuffer, AudioBufferRef, Signal};
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::conv::IntoSample;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;
use symphonia::core::sample::Sample;

#[cfg(feature = "gst")]
use gst::prelude::*;
#[cfg(feature = "gst")]
use gstreamer as gst;

#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;
#[cfg(feature = "gst")]
use crate::raw_audio::{configure_raw_surround_decoding, filter_raw_surround_source, is_dts_file};
#[cfg(feature = "gst")]
use gstreamer_app as gst_app;

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

/// How analysis reads files with Symphonia: with gapless trimming, so that
/// an MP3's encoder delay and padding (from its LAME or `FFmpeg` tag) and an
/// Ogg Vorbis file's pre-skip are cut, as playback cuts them, and frame 0 is
/// the first sample that plays. Symphonia 0.5 has no trimming for AAC.
pub(super) fn analysis_format_options() -> FormatOptions {
    FormatOptions {
        enable_gapless: true,
        ..FormatOptions::default()
    }
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

    /// The whole frames in the batch.
    pub(super) fn analysis_frames(&self) -> usize {
        self.frames.min(self.samples.len() / self.channels.max(1))
    }

    /// Append analysis channel `channel`'s samples from frame `first` of the
    /// batch's [`Self::analysis_frames`] on, whatever the decoder's channel
    /// count: a decoder may produce fewer or more channels than the file's
    /// header said, or change its count mid-stream. Every channel gets every
    /// frame, so their STFTs stay in step: a mono stream fills every channel,
    /// as playback plays it on every speaker, other missing channels are
    /// silent, and extra ones are left out. A downmix averages every decoded
    /// channel.
    pub(super) fn extend_analysis_channel(
        &self,
        out: &mut Vec<f32>,
        channel: usize,
        view_mode: SpectrogramViewMode,
        first: usize,
    ) {
        let decoded_channels = self.channels.max(1);
        let end = self.analysis_frames() * decoded_channels;
        let start = (first * decoded_channels).min(end);
        // Preserve the native sample grid. Skipping samples here aliases ultrasonic
        // energy into the audible spectrum and loses phase at packet boundaries.
        let frames = self.samples[start..end].chunks_exact(decoded_channels);
        match view_mode {
            SpectrogramViewMode::Downmix => {
                let inv_channels = 1.0 / usize_to_f32_approx(decoded_channels);
                out.extend(frames.map(|frame| frame.iter().sum::<f32>() * inv_channels));
            }
            SpectrogramViewMode::PerChannel => {
                let source = if decoded_channels == 1 {
                    Some(0)
                } else {
                    (channel < decoded_channels).then_some(channel)
                };
                match source {
                    Some(source) => out.extend(frames.map(|frame| frame[source])),
                    None => out.resize(out.len() + frames.len(), 0.0),
                }
            }
        }
    }
}

/// Normalize timestamped packets for a continuous STFT timeline. Missing audio
/// advances time as bounded silent batches; overlaps never repeat old frames.
/// Waveform extrema deliberately use the original packets to retain empty gaps.
pub(super) struct AudioTimeline {
    next_frame: u64,
    pending: Option<AudioFrames>,
}

impl AudioTimeline {
    pub(super) fn new(next_frame: u64) -> Self {
        Self {
            next_frame,
            pending: None,
        }
    }

    pub(super) fn read(
        &mut self,
        next: impl FnOnce() -> anyhow::Result<AudioRead>,
    ) -> anyhow::Result<AudioRead> {
        let mut frames = match self
            .pending
            .take()
            .map_or_else(next, |frames| Ok(AudioRead::Frames(frames)))?
        {
            AudioRead::Frames(frames) => frames,
            other => return Ok(other),
        };
        frames.trim_before(self.next_frame);
        if frames.frames == 0 {
            return Ok(AudioRead::Pending);
        }
        if frames.first_frame > self.next_frame {
            let gap =
                usize::try_from((frames.first_frame - self.next_frame).min(4096)).unwrap_or(4096);
            let silence = AudioFrames {
                samples: vec![0.0; gap * frames.channels],
                first_frame: self.next_frame,
                frames: gap,
                channels: frames.channels,
            };
            self.next_frame += usize_to_u64(gap);
            self.pending = Some(frames);
            return Ok(AudioRead::Frames(silence));
        }
        self.next_frame = frames
            .first_frame
            .saturating_add(usize_to_u64(frames.frames));
        Ok(AudioRead::Frames(frames))
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
        seek_frame: u64,
        /// The file and format to read with `GStreamer` if Symphonia fails
        /// or ends before decoding any audio; cleared once it decodes some.
        #[cfg(feature = "gst")]
        fallback: Option<GstFallback>,
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

/// What a Symphonia source needs to read its file with `GStreamer` instead.
#[cfg(feature = "gst")]
pub(super) struct GstFallback {
    path: std::path::PathBuf,
    sample_rate: u64,
    channels: usize,
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
                seek_frame,
                #[cfg(feature = "gst")]
                fallback,
            } => {
                let read = read_symphonia_frames(format, decoder, *track_id);
                #[cfg(feature = "gst")]
                if matches!(read, Ok(AudioRead::Frames(_))) {
                    *fallback = None;
                } else if matches!(read, Err(_) | Ok(AudioRead::Eof)) {
                    if let Some(source) = fallback
                        .take()
                        .and_then(|fallback| fallback.open(*seek_frame))
                    {
                        *self = source;
                        return self.next_frames();
                    }
                }
                (read?, *seek_frame)
            }
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
    let samples = interleave_f32(&audio);
    Ok(AudioRead::Frames(AudioFrames {
        frames: samples.len() / channels,
        samples,
        channels,
        first_frame,
    }))
}

/// Convert decoded audio to interleaved F32 samples in one pass, as
/// `SampleBuffer::copy_interleaved_ref` would but straight into the batch.
pub(super) fn interleave_f32(audio: &AudioBufferRef) -> Vec<f32> {
    fn interleave<S: Sample + IntoSample<f32>>(buffer: &AudioBuffer<S>) -> Vec<f32> {
        let channels = buffer.spec().channels.count();
        let mut samples = vec![0.0; buffer.frames() * channels];
        for channel in 0..channels {
            for (frame, &sample) in samples.chunks_exact_mut(channels).zip(buffer.chan(channel)) {
                frame[channel] = sample.into_sample();
            }
        }
        samples
    }
    match audio {
        AudioBufferRef::U8(buffer) => interleave(buffer),
        AudioBufferRef::U16(buffer) => interleave(buffer),
        AudioBufferRef::U24(buffer) => interleave(buffer),
        AudioBufferRef::U32(buffer) => interleave(buffer),
        AudioBufferRef::S8(buffer) => interleave(buffer),
        AudioBufferRef::S16(buffer) => interleave(buffer),
        AudioBufferRef::S24(buffer) => interleave(buffer),
        AudioBufferRef::S32(buffer) => interleave(buffer),
        AudioBufferRef::F32(buffer) => interleave(buffer),
        AudioBufferRef::F64(buffer) => interleave(buffer),
    }
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
    // The decoder may change its output mid-stream, as an MP3 that goes
    // from stereo to mono does; each buffer has the format of its caps.
    let (channels, rate) =
        sample
            .caps()
            .and_then(|caps| caps.structure(0))
            .map_or((None, None), |structure| {
                (
                    structure
                        .get::<i32>("channels")
                        .ok()
                        .and_then(|channels| usize::try_from(channels).ok())
                        .filter(|&channels| channels > 0),
                    structure
                        .get::<i32>("rate")
                        .ok()
                        .and_then(|rate| u32::try_from(rate).ok()),
                )
            });
    if let Some(rate) = rate.filter(|&rate| rate != sample_rate) {
        // Columns are laid out at the rate the session planned for.
        anyhow::bail!("the sample rate changes from {sample_rate} Hz to {rate} Hz mid-stream");
    }
    let channels = channels.unwrap_or(native_channels);
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
                seek_frame: 0,
                #[cfg(feature = "gst")]
                fallback: Some(GstFallback {
                    path: path.to_path_buf(),
                    sample_rate: sf.native_sample_rate,
                    channels: sf.native_channels,
                }),
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

#[cfg(feature = "gst")]
impl GstFallback {
    /// The file read with `GStreamer` from `seek_frame`, if it has the format
    /// Symphonia reported, which the caller has sized its work for.
    fn open(self, seek_frame: u64) -> Option<AudioFrameSource> {
        profile_eprintln!(
            "[analysis] Symphonia decoded nothing of {}, trying GStreamer",
            self.path.display()
        );
        let (mut source, rate, channels, _) = open_gstreamer_file(&self.path)?;
        if rate != self.sample_rate || channels != self.channels {
            return None;
        }
        if seek_frame > 0 {
            source
                .seek_to_frame(seek_frame, u32::try_from(rate).ok()?)
                .ok()?;
        }
        Some(source)
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
            &analysis_format_options(),
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
pub(super) fn open_gstreamer_file(path: &Path) -> Option<(AudioFrameSource, u64, usize, u32)> {
    gst::init().ok()?;
    configure_raw_surround_decoding();

    open_gstreamer_pipeline(path, gst::Pipeline::new())
}

#[cfg(feature = "gst")]
fn open_gstreamer_pipeline(
    path: &Path,
    pipeline: gst::Pipeline,
) -> Option<(AudioFrameSource, u64, usize, u32)> {
    let shutdown = super::gst_lifecycle::PipelineShutdown::new(&pipeline);
    let src = gst::ElementFactory::make("filesrc").build().ok()?;
    src.set_property("location", path.to_string_lossy().to_string());
    filter_raw_surround_source(&src);

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

    shutdown.disarm(); // AudioFrameSource::drop now owns pipeline shutdown.
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

/// Split interleaved frames into `channel_count` analysis channels; see
/// [`AudioFrames::extend_analysis_channel`].
#[cfg(test)]
pub(super) fn deinterleave_samples(
    samples: &[f32],
    frames: usize,
    decoded_channels: usize,
    channel_count: usize,
    view_mode: SpectrogramViewMode,
) -> Vec<Vec<f32>> {
    let audio = AudioFrames {
        samples: samples.to_vec(),
        first_frame: 0,
        frames,
        channels: decoded_channels,
    };
    (0..channel_count)
        .map(|channel| {
            let mut samples = Vec::new();
            audio.extend_analysis_channel(&mut samples, channel, view_mode, 0);
            samples
        })
        .collect()
}

/// The split before [`AudioFrames::extend_analysis_channel`], kept verbatim
/// as the reference the STFT input must match:
///
/// Split interleaved frames of `decoded_channels` into `channel_count`
/// channels of `frames` samples each, whatever the decoder's channel count:
/// a decoder may produce fewer or more channels than the file's header
/// said, or change its count mid-stream. Every channel gets every frame, so
/// their STFTs stay in step: a mono stream fills every channel, as playback
/// plays it on every speaker, other missing channels are silent, and extra
/// ones are left out.
#[cfg(test)]
pub(super) fn reference_deinterleave_samples(
    samples: &[f32],
    frames: usize,
    decoded_channels: usize,
    channel_count: usize,
    view_mode: SpectrogramViewMode,
) -> Vec<Vec<f32>> {
    let decoded_channels = decoded_channels.max(1);
    let frames = frames.min(samples.len() / decoded_channels);
    let frame_samples = |frame: usize| &samples[frame * decoded_channels..][..decoded_channels];
    // Preserve the native sample grid. Skipping samples here aliases ultrasonic
    // energy into the audible spectrum and loses phase at packet boundaries.
    let mut per_channel: Vec<Vec<f32>> = vec![Vec::with_capacity(frames); channel_count];

    match view_mode {
        SpectrogramViewMode::Downmix => {
            let inv_channels = 1.0 / usize_to_f32_approx(decoded_channels);
            if let Some(downmixed) = per_channel.first_mut() {
                downmixed.extend(
                    (0..frames)
                        .map(|frame| frame_samples(frame).iter().sum::<f32>() * inv_channels),
                );
            }
        }
        SpectrogramViewMode::PerChannel => {
            for (ch, channel) in per_channel.iter_mut().enumerate() {
                let source = if decoded_channels == 1 {
                    Some(0)
                } else {
                    (ch < decoded_channels).then_some(ch)
                };
                match source {
                    Some(source) => {
                        channel.extend((0..frames).map(|frame| frame_samples(frame)[source]));
                    }
                    None => channel.resize(frames, 0.0),
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

    #[test]
    fn timeline_fills_gaps_in_bounded_batches_without_reading_ahead() {
        let mut timeline = AudioTimeline::new(0);
        let mut reads = 0;
        let mut samples = Vec::new();
        for expected_frames in [4096, 4096, 1808, 2] {
            let read = timeline
                .read(|| {
                    reads += 1;
                    assert_eq!(reads, 1, "pending packet must be retained through the gap");
                    Ok(AudioRead::Frames(AudioFrames {
                        samples: vec![0.5, -0.5, 0.25, -0.25],
                        first_frame: 10_000,
                        frames: 2,
                        channels: 2,
                    }))
                })
                .expect("normalize gap");
            let AudioRead::Frames(frames) = read else {
                panic!("expected audio");
            };
            assert_eq!(frames.frames, expected_frames);
            assert_eq!(frames.first_frame * 2, usize_to_u64(samples.len()));
            samples.extend(frames.samples);
        }
        assert_eq!(samples.len(), 20_004);
        assert!(samples[..20_000].iter().all(|&sample| sample == 0.0));
        assert_eq!(&samples[20_000..], &[0.5, -0.5, 0.25, -0.25]);
        assert!(matches!(
            timeline.read(|| Ok(AudioRead::Eof)).unwrap(),
            AudioRead::Eof
        ));
    }

    #[test]
    fn timeline_trims_overlap_and_resets_pending_audio_for_a_seek_or_new_file() {
        let mut timeline = AudioTimeline::new(42);
        let read = timeline
            .read(|| {
                Ok(AudioRead::Frames(AudioFrames {
                    samples: vec![0.0, 0.25, 0.5, 0.75],
                    first_frame: 40,
                    frames: 4,
                    channels: 1,
                }))
            })
            .unwrap();
        let AudioRead::Frames(frames) = read else {
            panic!("expected audio");
        };
        assert_eq!(frames.first_frame, 42);
        assert_eq!(frames.samples, [0.5, 0.75]);
        assert!(matches!(
            timeline
                .read(|| Ok(AudioRead::Frames(AudioFrames {
                    samples: vec![1.0],
                    first_frame: 0,
                    frames: 1,
                    channels: 1,
                })))
                .unwrap(),
            AudioRead::Pending
        ));
        timeline
            .read(|| {
                Ok(AudioRead::Frames(AudioFrames {
                    samples: vec![1.0],
                    first_frame: 100_000,
                    frames: 1,
                    channels: 1,
                }))
            })
            .unwrap();
        timeline = AudioTimeline::new(0);
        let read = timeline
            .read(|| {
                Ok(AudioRead::Frames(AudioFrames {
                    samples: vec![-0.5],
                    first_frame: 0,
                    frames: 1,
                    channels: 1,
                }))
            })
            .unwrap();
        let AudioRead::Frames(frames) = read else {
            panic!("expected new source");
        };
        assert_eq!(frames.first_frame, 0);
        assert_eq!(frames.samples, [-0.5]);
    }

    #[cfg(feature = "gst")]
    #[test]
    fn failed_gstreamer_open_stops_pipeline_and_success_transfers_shutdown() {
        gst::init().expect("gstreamer initialization");
        let pipeline = gst::Pipeline::new();
        assert!(open_gstreamer_pipeline(
            Path::new("/missing/analysis-startup.wav"),
            pipeline.clone()
        )
        .is_none());
        assert_eq!(pipeline.current_state(), gst::State::Null);

        let path =
            super::super::waveform_window::tests::write_test_wave(&vec![100; 12_017], 48_000, 1);
        let pipeline = gst::Pipeline::new();
        let (source, _, _, _) = open_gstreamer_pipeline(&path, pipeline.clone()).expect("open wav");
        assert_ne!(pipeline.current_state(), gst::State::Null);
        drop(source);
        assert_eq!(pipeline.current_state(), gst::State::Null);
        std::fs::remove_file(path).expect("remove fixture");
    }

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

    #[cfg(feature = "gst")]
    #[test]
    fn ripped_surround_files_decode_completely_for_analysis() {
        use crate::raw_audio::{ripped_surround_bytes, TEST_AC3, TEST_DTS};
        use std::time::{Duration, Instant};

        // A leading partial frame and an appended APEv2 tag; upper case
        // gives the typefinders no extension hint.
        for (stream, ext, expected_frames) in
            [(TEST_AC3, "AC3", 32 * 1536), (TEST_DTS, "dts", 47 * 512)]
        {
            let path = std::env::temp_dir().join(format!(
                "ferrous-analysis-ripped-{}.{ext}",
                std::process::id()
            ));
            std::fs::write(&path, ripped_surround_bytes(stream).0).expect("write fixture");
            let (mut source, rate, channels, _) =
                open_audio_file(&path).unwrap_or_else(|| panic!("open {ext}"));
            assert_eq!((rate, channels), (48_000, 1), "{ext}");

            let mut decoded = 0;
            let deadline = Instant::now() + Duration::from_secs(10);
            loop {
                match source.next_frames().expect("decode") {
                    AudioRead::Frames(frames) => decoded += frames.frames,
                    AudioRead::Pending => assert!(Instant::now() < deadline, "{ext} stalled"),
                    AudioRead::Eof => break,
                }
            }
            drop(source);
            std::fs::remove_file(&path).expect("remove fixture");
            assert_eq!(decoded, expected_frames, "{ext}");
        }
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

    #[test]
    fn decoded_audio_interleaves_as_a_sample_buffer_would_for_every_format() {
        use symphonia::core::audio::{AsAudioBufferRef, AudioBuffer, Channels, SampleBuffer};
        use symphonia::core::conv::FromSample;
        use symphonia::core::sample::{i24, u24};

        fn check<S>(capacity: usize, frames: usize, channels: Channels)
        where
            S: Sample + FromSample<f64> + IntoSample<f32>,
            AudioBuffer<S>: AsAudioBufferRef,
        {
            let spec = symphonia::core::audio::SignalSpec::new(44_100, channels);
            let mut decoded = AudioBuffer::<S>::new(usize_to_u64(capacity), spec);
            decoded.render_reserved(Some(frames));
            for channel in 0..channels.count() {
                for (frame, sample) in decoded.chan_mut(channel).iter_mut().enumerate() {
                    let phase = f64::from(u32::try_from(frame * 7 + channel * 3).unwrap());
                    *sample = S::from_sample((phase * 0.37).sin() * 0.999);
                }
            }
            let mut expected = SampleBuffer::<f32>::new(usize_to_u64(capacity), spec);
            expected.copy_interleaved_ref(decoded.as_audio_buffer_ref());
            let actual = interleave_f32(&decoded.as_audio_buffer_ref());
            assert!(
                actual
                    .iter()
                    .map(|sample| sample.to_bits())
                    .eq(expected.samples().iter().map(|sample| sample.to_bits())),
                "{} frames of {} channels",
                frames,
                channels.count()
            );
        }

        let stereo = Channels::FRONT_LEFT | Channels::FRONT_RIGHT;
        let surround = stereo
            | Channels::FRONT_CENTRE
            | Channels::LFE1
            | Channels::REAR_LEFT
            | Channels::REAR_RIGHT;
        for (capacity, frames, channels) in [
            (1_152, 1_152, stereo),
            (4_096, 1_000, surround),
            (64, 64, Channels::FRONT_LEFT),
            (16, 0, surround),
        ] {
            check::<u8>(capacity, frames, channels);
            check::<u16>(capacity, frames, channels);
            check::<u24>(capacity, frames, channels);
            check::<u32>(capacity, frames, channels);
            check::<i8>(capacity, frames, channels);
            check::<i16>(capacity, frames, channels);
            check::<i24>(capacity, frames, channels);
            check::<i32>(capacity, frames, channels);
            check::<f32>(capacity, frames, channels);
            check::<f64>(capacity, frames, channels);
        }
    }

    #[test]
    fn every_channel_gets_every_frame_whatever_the_decoder_produces() {
        let per_channel = |samples: &[f32], decoded, planned| {
            deinterleave_samples(
                samples,
                samples.len() / decoded,
                decoded,
                planned,
                SpectrogramViewMode::PerChannel,
            )
        };
        // Mono plays on every speaker.
        assert_eq!(
            per_channel(&[0.1, 0.2, 0.3], 1, 2),
            [vec![0.1, 0.2, 0.3], vec![0.1, 0.2, 0.3]]
        );
        // Channels the decoder does not produce are silent, and the ones a
        // view has no room for are left out.
        assert_eq!(
            per_channel(&[0.1, -0.1, 0.2, -0.2], 2, 3),
            [vec![0.1, 0.2], vec![-0.1, -0.2], vec![0.0, 0.0]]
        );
        assert_eq!(
            per_channel(&[0.1, -0.1, 0.5, 0.2, -0.2, 0.5], 3, 2),
            [vec![0.1, 0.2], vec![-0.1, -0.2]]
        );
        assert_eq!(
            deinterleave_samples(
                &[0.25, 0.75, 0.5, 1.0],
                2,
                2,
                1,
                SpectrogramViewMode::Downmix
            ),
            [vec![0.5, 0.75]]
        );
    }

    /// The frame of the loudest sample `source` decodes, and how many it
    /// decodes.
    fn loudest_frame(mut source: AudioFrameSource) -> (u64, u64) {
        let mut loudest = (0.0_f32, 0);
        let mut frames = 0;
        for _ in 0..100_000 {
            match source.next_frames().expect("decode") {
                AudioRead::Frames(audio) => {
                    for (offset, frame) in audio.samples.chunks_exact(audio.channels).enumerate() {
                        let peak = frame.iter().fold(0.0_f32, |peak, &s| peak.max(s.abs()));
                        if peak > loudest.0 {
                            loudest = (peak, audio.first_frame + usize_to_u64(offset));
                        }
                    }
                    frames = audio.first_frame + usize_to_u64(audio.frames);
                }
                AudioRead::Pending => {}
                AudioRead::Eof => return (loudest.1, frames),
            }
        }
        panic!("no end of file");
    }

    /// A copy of `gapless-click.mp3` that no other test shares: tests run in
    /// parallel, and each removes its copy.
    fn gapless_click_mp3() -> std::path::PathBuf {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "ferrous-decoders-gapless-click-{}-{}.mp3",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::write(&path, include_bytes!("fixtures/gapless-click.mp3")).expect("fixture");
        path
    }

    /// Whether `GStreamer` has a decoder for MPEG-1 Layer III audio, which
    /// some installations leave out.
    #[cfg(feature = "gst")]
    fn gstreamer_decodes_mp3() -> bool {
        gst::init().expect("GStreamer");
        let caps = gst::Caps::builder("audio/mpeg")
            .field("mpegversion", 1)
            .field("layer", 3)
            .build();
        gst::ElementFactory::factories_with_type(
            gst::ElementFactoryType::DECODER | gst::ElementFactoryType::MEDIA_AUDIO,
            gst::Rank::MARGINAL,
        )
        .iter()
        .any(|factory| factory.can_sink_any_caps(&caps))
    }

    /// `gapless-click.mp3` is 0.4 s of 44.1 kHz audio with a click at 0.2 s,
    /// frame 8,820. Its LAME tag gives the encoder's delay, 1,105 frames
    /// with the decoder's, and padding, which playback cuts.
    #[test]
    fn mp3_encoder_delay_and_padding_are_cut_as_playback_cuts_them() {
        let path = gapless_click_mp3();
        let (source, rate, _, _) = open_audio_file(&path).expect("open");
        assert!(matches!(source, AudioFrameSource::Symphonia { .. }));
        assert_eq!(rate, 44_100);
        let (click, frames) = loudest_frame(source);
        assert!(click.abs_diff(8_820) <= 4, "the click is at frame {click}");
        assert_eq!(frames, 17_640);

        // A seek lands on the same timeline.
        let (mut source, ..) = open_audio_file(&path).expect("open");
        source.seek_to_frame(4_410, 44_100).expect("seek");
        let (click, _) = loudest_frame(source);
        assert!(click.abs_diff(8_820) <= 4, "after a seek at frame {click}");
        std::fs::remove_file(path).expect("remove fixture");
    }

    /// `GStreamer`, which plays the file, puts the click at the same frame.
    #[cfg(feature = "gst")]
    #[test]
    fn gstreamer_cuts_the_same_mp3_encoder_delay() {
        if !gstreamer_decodes_mp3() {
            eprintln!(
                "SKIPPED: GStreamer has no MP3 decoder here, so playback's trimming is unknown"
            );
            return;
        }
        let path = gapless_click_mp3();
        let (source, ..) = open_gstreamer_file(&path).expect("open");
        let (click, frames) = loudest_frame(source);
        assert!(click.abs_diff(8_820) <= 4, "the click is at frame {click}");
        assert_eq!(frames, 17_640);
        std::fs::remove_file(path).expect("remove fixture");
    }
}
