// SPDX-License-Identifier: GPL-3.0-or-later

use realfft::{num_complex::Complex32, RealFftPlanner, RealToComplex};
use symphonia::core::audio::{SampleBuffer, SignalSpec};

use super::{seconds_from_frames, small_usize_to_f32, u32_to_usize, usize_to_u64};

pub(super) struct StftComputer {
    r2c: std::sync::Arc<dyn RealToComplex<f32>>,
    fft_in: Vec<f32>,
    fft_out: Vec<Complex32>,
    pending: Vec<f32>,
    pending_start: usize,
    window: Vec<f32>,
    fft_size: usize,
    hop_size: usize,
}

impl StftComputer {
    pub(super) fn new(fft_size: usize, hop_size: usize) -> Self {
        let mut planner = RealFftPlanner::<f32>::new();
        let r2c = planner.plan_fft_forward(fft_size);
        let fft_in = r2c.make_input_vec();
        let fft_out = r2c.make_output_vec();
        // Blackman-Harris (as in DeaDBeeF spectrogram) gives cleaner bin separation.
        let window = blackman_harris_window(fft_size);

        Self {
            r2c,
            fft_in,
            fft_out,
            pending: Vec::with_capacity(fft_size * 2),
            pending_start: 0,
            window,
            fft_size,
            hop_size,
        }
    }

    pub(super) fn enqueue_samples(&mut self, samples: &[f32], sample_rate_hz: u32) {
        self.compact_pending_if_needed();
        self.pending.extend_from_slice(samples);
        // Keep pending bounded to avoid latency creep: max ~0.5s audio.
        let max_pending = (u32_to_usize(sample_rate_hz) / 2).max(self.fft_size * 4);
        let available = self.pending_available();
        if available > max_pending {
            let drop = available - max_pending;
            self.pending_start = self.pending_start.saturating_add(drop);
            self.compact_pending_if_needed();
        }
    }

    pub(super) fn take_rows(&mut self, max_rows: usize) -> Vec<Vec<f32>> {
        let mut rows = Vec::new();

        while self.pending_available() >= self.fft_size && rows.len() < max_rows {
            for i in 0..self.fft_size {
                self.fft_in[i] = self.pending[self.pending_start + i] * self.window[i];
            }

            if self
                .r2c
                .process(&mut self.fft_in, &mut self.fft_out)
                .is_ok()
            {
                let row: Vec<f32> = self
                    .fft_out
                    .iter()
                    .map(realfft::num_complex::Complex::norm_sqr)
                    .collect();
                rows.push(row);
            }

            let advance = self.hop_size.min(self.pending_available());
            self.pending_start = self.pending_start.saturating_add(advance);
        }

        self.compact_pending_if_needed();

        rows
    }

    #[allow(dead_code)]
    pub(super) fn pending_len(&self) -> usize {
        self.pending_available()
    }

    #[allow(dead_code)]
    pub(super) fn fft_size(&self) -> usize {
        self.fft_size
    }

    #[allow(dead_code)]
    pub(super) fn hop_size(&self) -> usize {
        self.hop_size
    }

    fn pending_available(&self) -> usize {
        self.pending.len().saturating_sub(self.pending_start)
    }

    fn compact_pending_if_needed(&mut self) {
        if self.pending_start == 0 {
            return;
        }
        let should_compact = self.pending_start >= self.fft_size * 8
            || self.pending_start >= self.pending.len().saturating_div(2);
        if should_compact {
            self.pending.drain(0..self.pending_start);
            self.pending_start = 0;
        }
    }
}

pub(super) struct SpectrogramDecimator {
    factor: usize,
    accum: Vec<f32>,
    count: usize,
}

impl SpectrogramDecimator {
    pub(super) fn new(factor: usize) -> Self {
        Self {
            factor: factor.max(1),
            accum: Vec::new(),
            count: 0,
        }
    }

    pub(super) fn push(&mut self, row: Vec<f32>) -> Option<Vec<f32>> {
        if self.accum.is_empty() {
            self.accum = vec![0.0; row.len()];
        }
        if row.len() != self.accum.len() {
            self.accum = vec![0.0; row.len()];
            self.count = 0;
        }

        for (a, v) in self.accum.iter_mut().zip(row) {
            *a += v;
        }
        self.count += 1;

        if self.count < self.factor {
            return None;
        }

        let inv = 1.0 / small_usize_to_f32(self.count);
        let mut out = Vec::with_capacity(self.accum.len());
        for v in &self.accum {
            out.push(v * inv);
        }

        self.accum.fill(0.0);
        self.count = 0;
        Some(out)
    }
}

pub(super) fn blackman_harris_window(size: usize) -> Vec<f32> {
    let n = small_usize_to_f32(size);
    (0..size)
        .map(|i| {
            let phase = (2.0 * std::f32::consts::PI * small_usize_to_f32(i)) / n;
            0.35875 - 0.48829 * phase.cos() + 0.14128 * (2.0 * phase).cos()
                - 0.01168 * (3.0 * phase).cos()
        })
        .collect()
}

pub(super) struct WaveformAccumulator {
    peaks: Vec<f32>,
    bucket_peak: f32,
    bucket_count: u64,
    covered_frames: u64,
    block_size: u64,
    max_points: usize,
    sample_rate_hz: u64,
    last_preview_emit: std::time::Instant,
}

impl WaveformAccumulator {
    pub(super) fn new(max_points: usize, estimated_frames: u64, sample_rate_hz: u64) -> Self {
        Self {
            peaks: Vec::with_capacity(max_points),
            bucket_peak: 0.0,
            bucket_count: 0,
            covered_frames: 0,
            block_size: (estimated_frames / usize_to_u64(max_points.max(1))).max(1),
            max_points,
            sample_rate_hz,
            last_preview_emit: std::time::Instant::now(),
        }
    }

    pub(super) fn push_sample<F>(
        &mut self,
        amp: f32,
        sample_stride: usize,
        on_update: &mut F,
    ) -> bool
    where
        F: FnMut(Vec<f32>, f32, bool) -> bool,
    {
        if amp > self.bucket_peak {
            self.bucket_peak = amp;
        }
        let sample_stride = usize_to_u64(sample_stride);
        self.bucket_count = self.bucket_count.saturating_add(sample_stride);
        self.covered_frames = self.covered_frames.saturating_add(sample_stride);

        if self.bucket_count < self.block_size {
            return true;
        }

        self.peaks.push(self.bucket_peak.clamp(0.0, 1.0));
        self.bucket_peak = 0.0;
        self.bucket_count = 0;
        while self.peaks.len() > self.max_points {
            self.peaks = fold_waveform_peaks(&self.peaks);
            self.block_size = self.block_size.saturating_mul(2).max(1);
        }
        if self.peaks.len() < 12
            || self.last_preview_emit.elapsed() < std::time::Duration::from_millis(240)
        {
            return true;
        }

        self.last_preview_emit = std::time::Instant::now();
        on_update(
            self.peaks.clone(),
            seconds_from_frames(self.covered_frames, self.sample_rate_hz),
            false,
        )
    }

    pub(super) fn finish(mut self) -> Vec<f32> {
        if self.bucket_count > 0 {
            self.peaks.push(self.bucket_peak.clamp(0.0, 1.0));
        }
        reduce_waveform_peaks(&self.peaks, self.max_points)
    }
}

pub(super) fn ensure_sample_buffer(
    sample_buf: &mut Option<SampleBuffer<f32>>,
    capacity: usize,
    spec: SignalSpec,
) -> &mut SampleBuffer<f32> {
    let capacity_u64 = usize_to_u64(capacity);
    if sample_buf
        .as_ref()
        .is_none_or(|buffer| buffer.capacity() < capacity)
    {
        *sample_buf = Some(SampleBuffer::<f32>::new(capacity_u64, spec));
    }

    sample_buf
        .as_mut()
        .expect("sample buffer is initialized above")
}

pub(super) fn waveform_sample_rate_divisor(sample_rate_hz: u64) -> u64 {
    const TARGET_48KHZ: u64 = 48_000;
    const TARGET_44K1HZ: u64 = 44_100;

    if sample_rate_hz <= TARGET_48KHZ {
        return 1;
    }
    if sample_rate_hz.is_multiple_of(TARGET_48KHZ) {
        return sample_rate_hz / TARGET_48KHZ;
    }
    if sample_rate_hz.is_multiple_of(TARGET_44K1HZ) {
        return sample_rate_hz / TARGET_44K1HZ;
    }
    1
}

/// Return the maximum absolute sample value across `channels` interleaved
/// channels starting at `base` in `samples`.  Used by the waveform decoder
/// so the seekbar peak represents the loudest channel, not just channel 0.
pub(super) fn peak_across_channels(samples: &[f32], base: usize, channels: usize) -> f32 {
    (0..channels)
        .map(|ch| samples.get(base + ch).map_or(0.0, |s| s.abs()))
        .fold(0.0_f32, f32::max)
}

pub(super) fn fold_waveform_peaks(peaks: &[f32]) -> Vec<f32> {
    let mut reduced = Vec::with_capacity(peaks.len().div_ceil(2));
    for chunk in peaks.chunks(2) {
        let mut peak = 0.0f32;
        for &value in chunk {
            if value > peak {
                peak = value;
            }
        }
        reduced.push(peak);
    }
    reduced
}

pub(super) fn reduce_waveform_peaks(peaks: &[f32], max_points: usize) -> Vec<f32> {
    if peaks.len() <= max_points || max_points == 0 {
        return peaks.to_vec();
    }

    let mut reduced = Vec::with_capacity(max_points);
    for i in 0..max_points {
        let idx = i.saturating_mul(peaks.len()) / max_points;
        reduced.push(peaks[idx.min(peaks.len() - 1)]);
    }
    reduced
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spectrogram_decimator_averages_rows() {
        let mut decimator = SpectrogramDecimator::new(2);
        let first = decimator.push(vec![2.0, 4.0]);
        assert!(first.is_none());
        let second = decimator.push(vec![4.0, 6.0]).expect("averaged row");
        assert_eq!(second, vec![3.0, 5.0]);
    }

    #[test]
    fn stft_computer_produces_rows_from_samples() {
        let mut stft = StftComputer::new(512, 128);
        let mut samples = Vec::new();
        for i in 0..4096usize {
            let x = (2.0 * std::f32::consts::PI * 440.0 * (small_usize_to_f32(i) / 48_000.0)).sin();
            samples.push(x);
        }
        stft.enqueue_samples(&samples, 48_000);
        let rows = stft.take_rows(4);
        assert!(!rows.is_empty());
        assert_eq!(rows[0].len(), 257);
    }

    #[test]
    fn stft_computer_keeps_row_count_with_chunked_input() {
        let mut stft = StftComputer::new(8, 4);
        let input: Vec<f32> = (0u16..24).map(f32::from).collect();
        let mut rows = 0usize;

        for chunk in input.chunks(3) {
            stft.enqueue_samples(chunk, 48_000);
            rows += stft.take_rows(1).len();
        }
        rows += stft.take_rows(64).len();

        assert_eq!(rows, 5);
        assert_eq!(stft.pending_len(), 4);
    }

    #[test]
    fn stft_computer_no_sample_loss_with_large_packet() {
        // FFT 512, hop 256: a typical audio packet of 4096 samples should
        // produce (4096 - 512) / 256 + 1 = 15 rows when drained one row
        // at a time (the pattern used by session_drain_stft_rows).
        let mut stft = StftComputer::new(512, 256);
        let samples: Vec<f32> = (0u32..4096).map(|i| (i as f32).sin()).collect();
        stft.enqueue_samples(&samples, 44_100);

        let mut rows = 0usize;
        loop {
            let batch = stft.take_rows(1);
            if batch.is_empty() {
                break;
            }
            rows += batch.len();
        }

        // Exact expected: floor((4096 - 512) / 256) + 1 = 15
        assert_eq!(rows, 15);
    }

    #[test]
    fn waveform_sample_rate_divisor_targets_common_high_rate_multiples() {
        assert_eq!(waveform_sample_rate_divisor(44_100), 1);
        assert_eq!(waveform_sample_rate_divisor(48_000), 1);
        assert_eq!(waveform_sample_rate_divisor(88_200), 2);
        assert_eq!(waveform_sample_rate_divisor(96_000), 2);
        assert_eq!(waveform_sample_rate_divisor(176_400), 4);
        assert_eq!(waveform_sample_rate_divisor(192_000), 4);
        assert_eq!(waveform_sample_rate_divisor(384_000), 8);
    }

    #[test]
    fn waveform_sample_rate_divisor_leaves_non_matching_rates_untouched() {
        assert_eq!(waveform_sample_rate_divisor(32_000), 1);
        assert_eq!(waveform_sample_rate_divisor(44_000), 1);
        assert_eq!(waveform_sample_rate_divisor(50_000), 1);
        assert_eq!(waveform_sample_rate_divisor(64_000), 1);
    }

    #[test]
    fn peak_across_channels_returns_max_absolute_value() {
        // 3-channel interleaved: frame at base=0 has channels [0.2, -0.8, 0.5]
        let samples = [0.2_f32, -0.8, 0.5, 0.1, 0.3, -0.1];
        assert!((peak_across_channels(&samples, 0, 3) - 0.8).abs() < f32::EPSILON);
        assert!((peak_across_channels(&samples, 3, 3) - 0.3).abs() < f32::EPSILON);
    }

    #[test]
    fn peak_across_channels_mono_returns_abs_sample() {
        let samples = [-0.6_f32, 0.3, -0.9];
        assert!((peak_across_channels(&samples, 0, 1) - 0.6).abs() < f32::EPSILON);
        assert!((peak_across_channels(&samples, 2, 1) - 0.9).abs() < f32::EPSILON);
    }

    #[test]
    fn peak_across_channels_handles_out_of_bounds() {
        let samples = [0.5_f32, 0.3];
        // Requesting 4 channels but only 2 samples — out-of-bounds channels
        // should contribute 0.0, not panic.
        assert!((peak_across_channels(&samples, 0, 4) - 0.5).abs() < f32::EPSILON);
    }
}
