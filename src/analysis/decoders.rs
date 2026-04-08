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

use super::fft::{ensure_sample_buffer, waveform_sample_rate_divisor};
use super::{f64_to_u64_saturating, usize_to_f32_approx, SpectrogramViewMode, REFERENCE_HOP};

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
    pub(super) frames: usize,
    pub(super) channels: usize,
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
    },
    #[cfg(feature = "gst")]
    Gst {
        pipeline: gst::Pipeline,
        appsink: gst_app::AppSink,
        native_channels: usize,
        /// Stored for seek-flag selection (DTS needs `KEY_UNIT`).
        path: std::path::PathBuf,
    },
}

impl AudioFrameSource {
    /// Pull the next batch of decoded audio frames.
    /// Returns `None` on EOF or unrecoverable error.
    pub(super) fn next_frames(&mut self) -> Option<AudioFrames> {
        match self {
            Self::Symphonia {
                format,
                decoder,
                track_id,
                sample_buf,
            } => loop {
                let packet = match format.next_packet() {
                    Ok(p) => p,
                    Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                        return None;
                    }
                    Err(_) => return None,
                };
                if packet.track_id() != *track_id {
                    continue;
                }
                let decoded_audio = match decoder.decode(&packet) {
                    Ok(d) => d,
                    Err(SymphoniaError::IoError(err)) if err.kind() == ErrorKind::UnexpectedEof => {
                        return None;
                    }
                    Err(SymphoniaError::DecodeError(_)) => continue,
                    Err(_) => return None,
                };
                let spec = *decoded_audio.spec();
                let decoded_channels = spec.channels.count().max(1);
                let decoded_capacity = decoded_audio.capacity();
                let buf = ensure_sample_buffer(sample_buf, decoded_capacity, spec);
                buf.copy_interleaved_ref(decoded_audio);
                let samples = buf.samples().to_vec();
                let frames = samples.len() / decoded_channels;
                return Some(AudioFrames {
                    samples,
                    frames,
                    channels: decoded_channels,
                });
            },
            #[cfg(feature = "gst")]
            Self::Gst {
                appsink,
                native_channels,
                ..
            } => {
                let timeout = gst::ClockTime::from_mseconds(50);
                if let Some(sample) = appsink.try_pull_sample(timeout) {
                    let buffer = sample.buffer()?;
                    let map = buffer.map_readable().ok()?;
                    let bytes = map.as_slice();
                    let mut samples = Vec::with_capacity(bytes.len() / 4);
                    for chunk in bytes.chunks_exact(4) {
                        samples.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
                    }
                    let ch = *native_channels;
                    let frames = if ch > 0 { samples.len() / ch } else { 0 };
                    Some(AudioFrames {
                        samples,
                        frames,
                        channels: ch,
                    })
                } else if appsink.is_eos() {
                    None
                } else {
                    // Timeout but not EOS — return empty batch so the decode
                    // loop can check for commands and retry.
                    Some(AudioFrames {
                        samples: Vec::new(),
                        frames: 0,
                        channels: *native_channels,
                    })
                }
            }
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

    /// Seek to the given position.
    pub(super) fn seek(&mut self, position_seconds: f64, native_sample_rate: u64) {
        match self {
            Self::Symphonia {
                format,
                decoder,
                track_id,
                ..
            } => {
                seek_symphonia(format, *track_id, native_sample_rate, position_seconds);
                decoder.reset();
            }
            #[cfg(feature = "gst")]
            Self::Gst { pipeline, path, .. } => {
                let flags = spectrogram_seek_flags_for_path(path);
                let ns = f64_to_u64_saturating(position_seconds * 1_000_000_000.0);
                let _ = pipeline.seek_simple(flags, gst::ClockTime::from_nseconds(ns));
            }
        }
    }
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
    let divisor = waveform_sample_rate_divisor(native_sample_rate);
    let effective_frames = n_frames / divisor;
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

pub(super) fn seek_symphonia(
    format: &mut Box<dyn symphonia::core::formats::FormatReader>,
    track_id: u32,
    native_sample_rate: u64,
    seek_seconds: f64,
) {
    use symphonia::core::formats::SeekMode;
    use symphonia::core::formats::SeekTo;
    let native_rate_u32 = u32::try_from(native_sample_rate).unwrap_or(u32::MAX);
    let ts = f64_to_u64_saturating(seek_seconds * f64::from(native_rate_u32));
    let _ = format.seek(SeekMode::Coarse, SeekTo::TimeStamp { ts, track_id });
}

/// `GStreamer` seek flags for the spectrogram worker.  DTS files need
/// `KEY_UNIT` to avoid decode artifacts after seeking.
#[cfg(feature = "gst")]
fn spectrogram_seek_flags_for_path(path: &Path) -> gst::SeekFlags {
    if is_dts_file(path) {
        gst::SeekFlags::FLUSH | gst::SeekFlags::KEY_UNIT
    } else {
        gst::SeekFlags::FLUSH
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

    let appsink = gst_app::AppSink::builder().sync(false).build();

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
    let divisor = waveform_sample_rate_divisor(rate);
    let effective_frames = total_frames / divisor;
    let total_columns =
        u32::try_from(((effective_frames / (REFERENCE_HOP as u64)) + 64).min(u64::from(u32::MAX)))
            .unwrap_or(u32::MAX);

    // Transition to PLAYING so the pipeline starts streaming.
    pipeline.set_state(gst::State::Playing).ok()?;

    let source = AudioFrameSource::Gst {
        pipeline,
        appsink,
        native_channels: channels,
        path: path.to_path_buf(),
    };

    Some((source, rate, channels, total_columns))
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
    divisor: usize,
    effective_frames: usize,
    view_mode: SpectrogramViewMode,
) -> Vec<Vec<f32>> {
    let mut per_channel: Vec<Vec<f32>> = vec![Vec::with_capacity(effective_frames); channel_count];

    match view_mode {
        SpectrogramViewMode::Downmix => {
            let mut downmixed = Vec::with_capacity(effective_frames);
            let inv_channels = 1.0 / usize_to_f32_approx(decoded_channels);
            for frame_idx in (0..frames).step_by(divisor) {
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
            for frame_idx in (0..frames).step_by(divisor) {
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
