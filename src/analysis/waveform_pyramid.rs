// SPDX-License-Identifier: GPL-3.0-or-later

use super::waveform_service::{PcmTile, TILE_FRAMES};
use super::{usize_to_f32_approx, usize_to_u64};

pub(super) const SUMMARY_FRAMES: u64 = 64;

pub(super) struct PyramidTile {
    // Each level doubles the frame span. Every row contains native channel
    // extrema followed by the arithmetic mix, preserving phase cancellation.
    levels: Vec<Vec<[f32; 2]>>,
    channels: usize,
}

impl PyramidTile {
    pub(super) fn new(pcm: &PcmTile, index: u64, channels: usize) -> Self {
        let width = channels + 1;
        let start = index.saturating_mul(TILE_FRAMES);
        let rows = usize::try_from(TILE_FRAMES / SUMMARY_FRAMES).unwrap_or(256);
        let mut base = vec![[f32::INFINITY, f32::NEG_INFINITY]; rows * width];
        for segment in &pcm.segments {
            for (offset, samples) in segment.samples.chunks_exact(segment.channels).enumerate() {
                let frame = segment.first_frame.saturating_add(usize_to_u64(offset));
                let row =
                    usize::try_from(frame.saturating_sub(start) / SUMMARY_FRAMES).unwrap_or(rows);
                let Some(extrema) = base.get_mut(row * width..(row + 1) * width) else {
                    continue;
                };
                let mut sum = 0.0;
                for (channel, pair) in extrema.iter_mut().take(channels).enumerate() {
                    let value = samples
                        .get(channel)
                        .copied()
                        .unwrap_or(0.0)
                        .clamp(-1.0, 1.0);
                    pair[0] = pair[0].min(value);
                    pair[1] = pair[1].max(value);
                    sum += value;
                }
                let mixed = sum / usize_to_f32_approx(channels);
                extrema[channels][0] = extrema[channels][0].min(mixed);
                extrema[channels][1] = extrema[channels][1].max(mixed);
            }
        }
        let mut levels = vec![base];
        while levels.last().is_some_and(|level| level.len() > width) {
            let previous = levels.last().expect("base level exists");
            let mut next = Vec::with_capacity(previous.len() / 2);
            for pair in previous.chunks_exact(width * 2) {
                for channel in 0..width {
                    next.push([
                        pair[channel][0].min(pair[channel + width][0]),
                        pair[channel][1].max(pair[channel + width][1]),
                    ]);
                }
            }
            levels.push(next);
        }
        Self { levels, channels }
    }

    pub(super) fn rows(&self, frames_per_point: u64) -> (&[[f32; 2]], u64) {
        let level = usize::try_from((frames_per_point / SUMMARY_FRAMES).max(1).ilog2())
            .unwrap_or(0)
            .min(self.levels.len() - 1);
        (&self.levels[level], SUMMARY_FRAMES << level)
    }

    pub(super) fn row_width(&self) -> usize {
        self.channels + 1
    }

    pub(super) fn bytes(&self) -> usize {
        self.levels
            .iter()
            .map(|level| level.capacity() * std::mem::size_of::<[f32; 2]>())
            .sum::<usize>()
            + self.levels.capacity() * std::mem::size_of::<Vec<[f32; 2]>>()
    }
}

#[cfg(test)]
mod tests {
    use super::super::decoders::AudioFrames;
    use super::*;

    #[test]
    fn summary_keeps_timestamp_gaps_empty_and_mixes_before_aggregation() {
        let pcm = PcmTile {
            segments: vec![
                AudioFrames {
                    samples: vec![0.5, -0.5],
                    first_frame: 0,
                    frames: 1,
                    channels: 2,
                },
                AudioFrames {
                    samples: vec![-0.25, 0.25],
                    first_frame: 128,
                    frames: 1,
                    channels: 2,
                },
            ],
        };
        let summary = PyramidTile::new(&pcm, 0, 2);
        let (rows, _) = summary.rows(64);
        assert_eq!(&rows[..3], &[[0.5, 0.5], [-0.5, -0.5], [0.0, 0.0]]);
        assert!(rows[3..6].iter().all(|pair| pair[0] > pair[1]));
        let (coarse, _) = summary.rows(256);
        assert_eq!(&coarse[..3], &[[-0.25, 0.5], [-0.5, 0.25], [0.0, 0.0]]);
    }
}
