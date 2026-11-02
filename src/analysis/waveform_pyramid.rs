// SPDX-License-Identifier: GPL-3.0-or-later

use super::waveform_service::{PcmTile, TILE_FRAMES};
use super::{usize_to_f32_approx, usize_to_u64};

pub(super) const SUMMARY_FRAMES: u64 = 64;
/// The finest span of the coarse levels, which the waveform service keeps
/// for the whole track; finer levels share an LRU cache.
pub(super) const COARSE_SUMMARY_FRAMES: u64 = 1_024;

/// Min/max summaries of one PCM tile at power-of-two spans, from
/// `first_level` (`SUMMARY_FRAMES << first_level` frames a row) to
/// `last_level`.
pub(super) struct PyramidTile {
    // The levels in order, concatenated; each doubles the frame span of the
    // one before. Every row contains native channel extrema followed by the
    // arithmetic mix, preserving phase cancellation.
    rows: Vec<[f32; 2]>,
    channels: usize,
    first_level: u32,
    last_level: u32,
}

impl PyramidTile {
    /// The fine levels of tile `index` (spans below `COARSE_SUMMARY_FRAMES`)
    /// and its coarse levels.
    pub(super) fn build(pcm: &PcmTile, index: u64, channels: usize) -> (Self, Self) {
        let width = channels + 1;
        let start = index.saturating_mul(TILE_FRAMES);
        let rows = tile_rows();
        let mut level = vec![[f32::INFINITY, f32::NEG_INFINITY]; rows * width];
        for segment in &pcm.segments {
            for (offset, samples) in segment.samples.chunks_exact(segment.channels).enumerate() {
                let frame = segment.first_frame.saturating_add(usize_to_u64(offset));
                let row =
                    usize::try_from(frame.saturating_sub(start) / SUMMARY_FRAMES).unwrap_or(rows);
                let Some(extrema) = level.get_mut(row * width..(row + 1) * width) else {
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
        let coarse_level = coarse_level();
        let top_level = rows.ilog2();
        let mut fine = Self::with_levels(0, coarse_level - 1, channels);
        let mut coarse = Self::with_levels(coarse_level, top_level, channels);
        for number in 0..=top_level {
            if number > 0 {
                level = halve(&level, width);
            }
            let part = if number < coarse_level {
                &mut fine
            } else {
                &mut coarse
            };
            part.rows.extend_from_slice(&level);
        }
        (fine, coarse)
    }

    /// The coarse levels of the tile whose fine levels are `fine`.
    pub(super) fn coarse_from(fine: &Self) -> Self {
        let width = fine.row_width();
        let mut level = fine.rows(SUMMARY_FRAMES << fine.last_level).0.to_vec();
        let mut coarse = Self::with_levels(fine.last_level + 1, tile_rows().ilog2(), fine.channels);
        for _ in coarse.first_level..=coarse.last_level {
            level = halve(&level, width);
            coarse.rows.extend_from_slice(&level);
        }
        coarse
    }

    fn with_levels(first_level: u32, last_level: u32, channels: usize) -> Self {
        let rows = (first_level..=last_level)
            .map(|level| tile_rows() >> level)
            .sum::<usize>();
        Self {
            rows: Vec::with_capacity(rows * (channels + 1)),
            channels,
            first_level,
            last_level,
        }
    }

    /// The rows of the coarsest level whose span does not exceed
    /// `frames_per_point`, with that span. Spans below this tile's finest
    /// level give the finest.
    pub(super) fn rows(&self, frames_per_point: u64) -> (&[[f32; 2]], u64) {
        let wanted = (frames_per_point / SUMMARY_FRAMES).max(1).ilog2();
        let level = wanted.clamp(self.first_level, self.last_level);
        debug_assert!(
            level <= wanted,
            "a {frames_per_point}-frame point asked for coarser rows"
        );
        let width = self.row_width();
        let offset = (self.first_level..level)
            .map(|level| (tile_rows() >> level) * width)
            .sum::<usize>();
        let length = (tile_rows() >> level) * width;
        (&self.rows[offset..offset + length], SUMMARY_FRAMES << level)
    }

    pub(super) fn row_width(&self) -> usize {
        self.channels + 1
    }

    pub(super) fn bytes(&self) -> usize {
        self.rows.capacity() * std::mem::size_of::<[f32; 2]>() + std::mem::size_of::<Self>()
    }
}

/// The level above `level`: each row the extrema of two.
fn halve(level: &[[f32; 2]], width: usize) -> Vec<[f32; 2]> {
    level
        .chunks_exact(width * 2)
        .flat_map(|pair| {
            (0..width).map(move |channel| {
                [
                    pair[channel][0].min(pair[channel + width][0]),
                    pair[channel][1].max(pair[channel + width][1]),
                ]
            })
        })
        .collect()
}

/// Rows of the finest level of a tile.
fn tile_rows() -> usize {
    usize::try_from(TILE_FRAMES / SUMMARY_FRAMES).unwrap_or(256)
}

/// The level whose rows span `COARSE_SUMMARY_FRAMES`.
fn coarse_level() -> u32 {
    (COARSE_SUMMARY_FRAMES / SUMMARY_FRAMES).ilog2()
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
        let (summary, _) = PyramidTile::build(&pcm, 0, 2);
        let (rows, _) = summary.rows(64);
        assert_eq!(&rows[..3], &[[0.5, 0.5], [-0.5, -0.5], [0.0, 0.0]]);
        assert!(rows[3..6].iter().all(|pair| pair[0] > pair[1]));
        let (coarse, _) = summary.rows(256);
        assert_eq!(&coarse[..3], &[[-0.25, 0.5], [-0.5, 0.25], [0.0, 0.0]]);
    }

    #[test]
    fn coarse_levels_continue_the_fine_ones_and_keep_every_peak() {
        let channels = 2;
        let samples: Vec<f32> = (0..TILE_FRAMES * 2)
            .map(|sample| {
                f32::from(u16::try_from((sample * 7_919) % 2_001).unwrap()) / 1_000.0 - 1.0
            })
            .collect();
        let pcm = PcmTile {
            segments: vec![AudioFrames {
                samples: samples.clone(),
                first_frame: 0,
                frames: usize::try_from(TILE_FRAMES).unwrap(),
                channels,
            }],
        };
        let (fine, coarse) = PyramidTile::build(&pcm, 0, channels);
        let (finest, span) = fine.rows(64);
        assert_eq!((finest.len(), span), (256 * 3, 64));
        let (top_fine, span) = fine.rows(COARSE_SUMMARY_FRAMES);
        assert_eq!((top_fine.len(), span), (32 * 3, 512));
        let (first_coarse, span) = coarse.rows(COARSE_SUMMARY_FRAMES);
        assert_eq!((first_coarse.len(), span), (16 * 3, 1_024));
        for (row, pair) in first_coarse.chunks_exact(3).zip(top_fine.chunks_exact(6)) {
            for column in 0..3 {
                assert_eq!(row[column][0], pair[column][0].min(pair[column + 3][0]));
                assert_eq!(row[column][1], pair[column][1].max(pair[column + 3][1]));
            }
        }
        let derived = PyramidTile::coarse_from(&fine);
        assert_eq!(
            derived.rows, coarse.rows,
            "coarse levels from the fine ones"
        );
        assert_eq!(
            (derived.first_level, derived.last_level),
            (coarse.first_level, coarse.last_level)
        );
        let (whole, span) = coarse.rows(TILE_FRAMES * 4);
        assert_eq!(span, TILE_FRAMES);
        for (channel, extrema) in whole.iter().take(channels).enumerate() {
            let values = samples.iter().skip(channel).step_by(channels);
            let minimum = values.clone().copied().fold(f32::INFINITY, f32::min);
            let maximum = values.copied().fold(f32::NEG_INFINITY, f32::max);
            assert_eq!(*extrema, [minimum, maximum]);
        }
    }
}
