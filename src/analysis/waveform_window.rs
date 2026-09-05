// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::Path;

use symphonia::core::audio::SampleBuffer;
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::formats::{SeekMode, SeekTo};

use super::decoders::{open_audio_file, open_symphonia_file};
use super::f64_to_u64_saturating;
use super::fft::ensure_sample_buffer;

const MAX_WINDOW_POINTS: usize = 65_536;

#[derive(Debug, Clone)]
pub(crate) struct WaveformWindow {
    pub(crate) sample_rate_hz: u32,
    pub(crate) channel_count: u16,
    pub(crate) start_seconds: f64,
    pub(crate) end_seconds: f64,
    pub(crate) frames_per_point: u32,
    /// Interleaved `[minimum, maximum]` pairs for every point and channel.
    /// At sample resolution `minimum == maximum == sample`.
    pub(crate) extrema: Vec<f32>,
}

struct WindowAccumulator {
    start_frame: u64,
    end_frame: u64,
    frames_per_point: u64,
    channels: usize,
    minima: Vec<f32>,
    maxima: Vec<f32>,
    seen: Vec<bool>,
}

impl WindowAccumulator {
    fn new(start_frame: u64, end_frame: u64, max_points: usize, channels: usize) -> Self {
        let requested_frames = end_frame.saturating_sub(start_frame).max(1);
        let point_limit = u64::try_from(max_points.clamp(1, MAX_WINDOW_POINTS)).unwrap_or(1);
        let frames_per_point = requested_frames.div_ceil(point_limit).max(1);
        // Anchor every aggregation bin to the track's absolute sample grid.
        // Sliding windows with the same resolution must describe their shared
        // samples identically or cache handoffs make the waveform pulse.
        let remainder = start_frame % frames_per_point;
        let aligned_start_frame = if remainder == 0 {
            start_frame
        } else {
            start_frame.saturating_add(frames_per_point - remainder)
        }
        .min(end_frame.saturating_sub(1));
        let aligned_frames = end_frame.saturating_sub(aligned_start_frame).max(1);
        let point_count_u64 = aligned_frames.div_ceil(frames_per_point);
        let point_count = usize::try_from(point_count_u64)
            .unwrap_or(MAX_WINDOW_POINTS)
            .min(MAX_WINDOW_POINTS);
        let value_count = point_count.saturating_mul(channels);
        Self {
            start_frame: aligned_start_frame,
            end_frame,
            frames_per_point,
            channels,
            minima: vec![f32::INFINITY; value_count],
            maxima: vec![f32::NEG_INFINITY; value_count],
            seen: vec![false; value_count],
        }
    }

    fn push_interleaved(&mut self, first_frame: u64, samples: &[f32], decoded_channels: usize) {
        if decoded_channels == 0 {
            return;
        }
        for (local_frame, frame) in samples.chunks_exact(decoded_channels).enumerate() {
            let frame_index =
                first_frame.saturating_add(u64::try_from(local_frame).unwrap_or(u64::MAX));
            if frame_index < self.start_frame || frame_index >= self.end_frame {
                continue;
            }
            let point_u64 = (frame_index - self.start_frame) / self.frames_per_point;
            let Ok(point) = usize::try_from(point_u64) else {
                continue;
            };
            for channel in 0..self.channels {
                let value = frame.get(channel).copied().unwrap_or(0.0).clamp(-1.0, 1.0);
                let index = point.saturating_mul(self.channels).saturating_add(channel);
                let Some(minimum) = self.minima.get_mut(index) else {
                    continue;
                };
                *minimum = minimum.min(value);
                if let Some(maximum) = self.maxima.get_mut(index) {
                    *maximum = maximum.max(value);
                }
                if let Some(seen) = self.seen.get_mut(index) {
                    *seen = true;
                }
            }
        }
    }

    fn finish(self, sample_rate_hz: u32) -> WaveformWindow {
        let mut extrema = Vec::with_capacity(self.minima.len().saturating_mul(2));
        for ((minimum, maximum), seen) in self.minima.into_iter().zip(self.maxima).zip(self.seen) {
            if seen {
                extrema.push(minimum);
                extrema.push(maximum);
            } else {
                extrema.push(0.0);
                extrema.push(0.0);
            }
        }
        let sample_rate = u64::from(sample_rate_hz.max(1));
        WaveformWindow {
            sample_rate_hz,
            channel_count: u16::try_from(self.channels).unwrap_or(u16::MAX),
            start_seconds: frame_seconds(self.start_frame, sample_rate),
            end_seconds: frame_seconds(self.end_frame, sample_rate),
            frames_per_point: u32::try_from(self.frames_per_point).unwrap_or(u32::MAX),
            extrema,
        }
    }
}

pub(crate) fn decode_waveform_window(
    path: &Path,
    start_seconds: f64,
    end_seconds: f64,
    max_points: usize,
) -> anyhow::Result<WaveformWindow> {
    decode_waveform_window_cancellable(path, start_seconds, end_seconds, max_points, &|| false)
}

pub(crate) fn decode_waveform_window_cancellable(
    path: &Path,
    start_seconds: f64,
    end_seconds: f64,
    max_points: usize,
    cancelled: &impl Fn() -> bool,
) -> anyhow::Result<WaveformWindow> {
    anyhow::ensure!(start_seconds.is_finite() && end_seconds.is_finite());
    anyhow::ensure!(start_seconds >= 0.0 && end_seconds > start_seconds);
    anyhow::ensure!(max_points > 0);

    anyhow::ensure!(!cancelled(), "waveform decode cancelled");
    if let Some(mut file) = open_symphonia_file(path) {
        let sample_rate_hz = u32::try_from(file.native_sample_rate)
            .unwrap_or(u32::MAX)
            .max(1);
        let channels = file.native_channels.clamp(1, usize::from(u16::MAX));
        let start_frame = f64_to_u64_saturating(start_seconds * f64::from(sample_rate_hz));
        let end_frame = f64_to_u64_saturating(end_seconds * f64::from(sample_rate_hz))
            .max(start_frame.saturating_add(1));
        let mut accumulator = WindowAccumulator::new(start_frame, end_frame, max_points, channels);

        let _ = file.format.seek(
            SeekMode::Accurate,
            SeekTo::TimeStamp {
                ts: start_frame,
                track_id: file.track_id,
            },
        );
        file.decoder.reset();
        let mut sample_buffer: Option<SampleBuffer<f32>> = None;
        loop {
            anyhow::ensure!(!cancelled(), "waveform decode cancelled");
            let Ok(packet) = file.format.next_packet() else {
                break;
            };
            if packet.track_id() != file.track_id {
                continue;
            }
            if packet.ts() >= end_frame {
                break;
            }
            let decoded = match file.decoder.decode(&packet) {
                Ok(decoded) => decoded,
                Err(SymphoniaError::DecodeError(_)) => continue,
                Err(_) => break,
            };
            let spec = *decoded.spec();
            let decoded_channels = spec.channels.count().max(1);
            let buffer = ensure_sample_buffer(&mut sample_buffer, decoded.capacity(), spec);
            buffer.copy_interleaved_ref(decoded);
            accumulator.push_interleaved(packet.ts(), buffer.samples(), decoded_channels);
        }
        return Ok(accumulator.finish(sample_rate_hz));
    }

    decode_fallback_window(path, start_seconds, end_seconds, max_points, cancelled)
}

fn frame_seconds(frame: u64, sample_rate: u64) -> f64 {
    let sample_rate = sample_rate.max(1);
    let seconds = frame / sample_rate;
    let remainder = frame % sample_rate;
    let nanos = remainder.saturating_mul(1_000_000_000) / sample_rate;
    std::time::Duration::new(seconds, u32::try_from(nanos).unwrap_or(999_999_999)).as_secs_f64()
}

fn decode_fallback_window(
    path: &Path,
    start_seconds: f64,
    end_seconds: f64,
    max_points: usize,
    cancelled: &impl Fn() -> bool,
) -> anyhow::Result<WaveformWindow> {
    let (mut source, sample_rate, channels, _) =
        open_audio_file(path).ok_or_else(|| anyhow::anyhow!("unsupported audio file"))?;
    let sample_rate_hz = u32::try_from(sample_rate).unwrap_or(u32::MAX).max(1);
    let channels = channels.clamp(1, usize::from(u16::MAX));
    let start_frame = f64_to_u64_saturating(start_seconds * f64::from(sample_rate_hz));
    let end_frame = f64_to_u64_saturating(end_seconds * f64::from(sample_rate_hz))
        .max(start_frame.saturating_add(1));
    let mut accumulator = WindowAccumulator::new(start_frame, end_frame, max_points, channels);
    source.seek(start_seconds, sample_rate);
    let mut next_frame = start_frame;
    while next_frame < end_frame {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        let Some(frames) = source.next_frames() else {
            break;
        };
        accumulator.push_interleaved(next_frame, &frames.samples, frames.channels);
        next_frame = next_frame.saturating_add(u64::try_from(frames.frames).unwrap_or(u64::MAX));
    }
    Ok(accumulator.finish(sample_rate_hz))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_test_wave(samples: &[i16], sample_rate: u32, channels: u16) -> std::path::PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!(
            "ferrous-waveform-window-{}-{}.wav",
            std::process::id(),
            samples.len()
        ));
        let data_bytes = u32::try_from(samples.len().saturating_mul(2)).unwrap();
        let mut file = std::fs::File::create(&path).unwrap();
        file.write_all(b"RIFF").unwrap();
        file.write_all(&(36 + data_bytes).to_le_bytes()).unwrap();
        file.write_all(b"WAVEfmt ").unwrap();
        file.write_all(&16_u32.to_le_bytes()).unwrap();
        file.write_all(&1_u16.to_le_bytes()).unwrap();
        file.write_all(&channels.to_le_bytes()).unwrap();
        file.write_all(&sample_rate.to_le_bytes()).unwrap();
        let byte_rate = sample_rate * u32::from(channels) * 2;
        file.write_all(&byte_rate.to_le_bytes()).unwrap();
        file.write_all(&(channels * 2).to_le_bytes()).unwrap();
        file.write_all(&16_u16.to_le_bytes()).unwrap();
        file.write_all(b"data").unwrap();
        file.write_all(&data_bytes.to_le_bytes()).unwrap();
        for sample in samples {
            file.write_all(&sample.to_le_bytes()).unwrap();
        }
        path
    }

    #[test]
    fn cancelled_window_does_not_open_the_source() {
        let error = decode_waveform_window_cancellable(
            Path::new("/missing/cancelled.wav"),
            0.0,
            1.0,
            100,
            &|| true,
        )
        .expect_err("cancel before opening");
        assert!(error.to_string().contains("cancelled"));
    }

    #[test]
    fn window_decode_observes_cancellation_between_packets() {
        let path = write_test_wave(&vec![100; 48_003], 48_000, 1);
        let checks = std::cell::Cell::new(0);
        let result = decode_waveform_window_cancellable(&path, 0.0, 1.0, 100, &|| {
            checks.set(checks.get() + 1);
            checks.get() > 2
        });
        std::fs::remove_file(path).expect("remove fixture");
        assert!(result
            .expect_err("cancel during decode")
            .to_string()
            .contains("cancelled"));
        assert_eq!(checks.get(), 3);
    }

    #[test]
    fn window_decoder_preserves_signed_samples_at_sample_resolution() {
        let samples = [-16_384, 8_192, 16_384, -8_192];
        let path = write_test_wave(&samples, 4, 1);
        let window = decode_waveform_window(&path, 0.0, 1.0, 4).unwrap();
        let _ = std::fs::remove_file(path);

        assert_eq!(window.sample_rate_hz, 4);
        assert_eq!(window.channel_count, 1);
        assert_eq!(window.frames_per_point, 1);
        assert_eq!(window.extrema.len(), 8);
        assert!((window.extrema[0] + 0.5).abs() < 0.001);
        assert!((window.extrema[2] - 0.25).abs() < 0.001);
    }

    #[test]
    fn window_decoder_peak_holds_minimum_and_maximum_per_channel() {
        let samples = [
            -16_384, 4_096, 8_192, -12_288, 16_384, -4_096, -8_192, 12_288,
        ];
        let path = write_test_wave(&samples, 4, 2);
        let window = decode_waveform_window(&path, 0.0, 1.0, 2).unwrap();
        let _ = std::fs::remove_file(path);

        assert_eq!(window.channel_count, 2);
        assert_eq!(window.frames_per_point, 2);
        assert_eq!(window.extrema.len(), 8);
        assert!((window.extrema[0] + 0.5).abs() < 0.001);
        assert!((window.extrema[1] - 0.25).abs() < 0.001);
        assert!((window.extrema[2] + 0.375).abs() < 0.001);
        assert!((window.extrema[3] - 0.125).abs() < 0.001);
    }

    #[test]
    fn overlapping_windows_keep_shared_bins_sample_aligned() {
        let samples = (0_i16..300_i16)
            .map(|sample| sample.saturating_mul(100).saturating_sub(15_000))
            .collect::<Vec<_>>();
        let path = write_test_wave(&samples, 100, 1);
        let first = decode_waveform_window(&path, 0.03, 2.03, 50).unwrap();
        let second = decode_waveform_window(&path, 0.07, 2.07, 50).unwrap();
        let _ = std::fs::remove_file(path);

        assert_eq!(first.frames_per_point, 4);
        assert_eq!(second.frames_per_point, 4);
        assert!((first.start_seconds - 0.04).abs() < 0.000_001);
        assert!((second.start_seconds - 0.08).abs() < 0.000_001);
        assert_eq!(first.extrema[2..98], second.extrema[0..96]);
    }
}
