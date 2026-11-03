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

/// The finest summary rows of one tile, filled from its audio in any
/// number of pieces: the decoder's packets, in the order it gives them.
pub(super) struct TileRows {
    level: Vec<[f32; 2]>,
    start: u64,
    channels: usize,
    /// Scratch space for whole rows: the minima and maxima of each half,
    /// then of each quarter, and so on.
    minima: Vec<f32>,
    maxima: Vec<f32>,
}

/// `SUMMARY_FRAMES`, as a count of frames.
const ROW_FRAMES: usize = 64;

impl TileRows {
    /// Empty rows for tile `index` of a file of `channels` channels.
    pub(super) fn new(index: u64, channels: usize) -> Self {
        Self {
            level: vec![[f32::INFINITY, f32::NEG_INFINITY]; tile_rows() * (channels + 1)],
            start: index.saturating_mul(TILE_FRAMES),
            channels,
            minima: Vec::new(),
            maxima: Vec::new(),
        }
    }

    /// Add `samples`, interleaved in `decoded_channels` channels, whose
    /// first frame is `first_frame`. Frames past the tile are left out.
    pub(super) fn push(&mut self, first_frame: u64, samples: &[f32], decoded_channels: usize) {
        let channels = self.channels;
        let width = channels + 1;
        let rows = tile_rows();
        let decoded_channels = decoded_channels.max(1);
        let total = samples.len() / decoded_channels;
        let mut done = 0;
        let mut frame = first_frame;
        while done < total {
            let row =
                usize::try_from(frame.saturating_sub(self.start) / SUMMARY_FRAMES).unwrap_or(rows);
            if row >= rows {
                return;
            }
            // The frames of this row, all at once.
            let in_row = SUMMARY_FRAMES - frame.saturating_sub(self.start) % SUMMARY_FRAMES;
            let count = usize::try_from(in_row)
                .unwrap_or(usize::MAX)
                .min(total - done);
            let block = &samples[done * decoded_channels..(done + count) * decoded_channels];
            let extrema = &mut self.level[row * width..(row + 1) * width];
            if count == ROW_FRAMES && decoded_channels == channels {
                whole_row(
                    extrema,
                    block,
                    channels,
                    [&mut self.minima, &mut self.maxima],
                );
            } else {
                part_row(extrema, block, channels, decoded_channels);
            }
            done += count;
            frame = frame.saturating_add(usize_to_u64(count));
        }
    }

    /// The fine levels (spans below `COARSE_SUMMARY_FRAMES`) and the
    /// coarse levels of the tile.
    pub(super) fn finish(self) -> (PyramidTile, PyramidTile) {
        let width = self.channels + 1;
        let mut level = self.level;
        let coarse_level = coarse_level();
        let top_level = tile_rows().ilog2();
        let mut fine = PyramidTile::with_levels(0, coarse_level - 1, self.channels);
        let mut coarse = PyramidTile::with_levels(coarse_level, top_level, self.channels);
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
}

impl PyramidTile {
    /// The fine levels of tile `index` (spans below `COARSE_SUMMARY_FRAMES`)
    /// and its coarse levels.
    pub(super) fn build(pcm: &PcmTile, index: u64, channels: usize) -> (Self, Self) {
        let mut rows = TileRows::new(index, channels);
        for segment in &pcm.segments {
            rows.push(segment.first_frame, &segment.samples, segment.channels);
        }
        rows.finish()
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

    /// The levels, channel count and rows, the rows bit for bit.
    #[cfg(test)]
    pub(super) fn bits(&self) -> (u32, u32, usize, Vec<[u32; 2]>) {
        let rows = self
            .rows
            .iter()
            .map(|pair| [pair[0].to_bits(), pair[1].to_bits()])
            .collect();
        (self.first_level, self.last_level, self.channels, rows)
    }
}

/// Add the frames of `block`, interleaved in `decoded_channels` channels,
/// to the row `extrema` of a file of `channels` channels: each channel's
/// extrema, then those of the mix. A channel the decoder did not give is
/// silent, and one more than the file's is left out.
fn part_row(extrema: &mut [[f32; 2]], block: &[f32], channels: usize, decoded_channels: usize) {
    let divisor = usize_to_f32_approx(channels);
    let (native, mixed) = extrema.split_at_mut(channels);
    let mixed = &mut mixed[0];
    for samples in block.chunks_exact(decoded_channels) {
        let mut sum = 0.0;
        for (channel, pair) in native.iter_mut().enumerate() {
            let value = samples
                .get(channel)
                .copied()
                .unwrap_or(0.0)
                .clamp(-1.0, 1.0);
            pair[0] = pair[0].min(value);
            pair[1] = pair[1].max(value);
            sum += value;
        }
        let value = sum / divisor;
        mixed[0] = mixed[0].min(value);
        mixed[1] = mixed[1].max(value);
    }
}

/// [`part_row`] for a whole row of frames of the file's own channels, as
/// most are. The extrema of a set of samples are the same in any order,
/// and clamping keeps their order, so the row reduces the frames in halves,
/// which the compiler vectorizes, and clamps the extrema; the mix of each
/// frame is the sum of `part_row`, in the same order.
fn whole_row(
    extrema: &mut [[f32; 2]],
    block: &[f32],
    channels: usize,
    scratch: [&mut Vec<f32>; 2],
) {
    #[cfg(target_arch = "x86_64")]
    if std::arch::is_x86_feature_detected!("avx2") {
        // SAFETY: The CPU has AVX2, which is all the function needs. Its
        // operations round as without it, so the rows are the same.
        unsafe { whole_row_avx2(extrema, block, channels, scratch) };
        return;
    }
    whole_row_with(extrema, block, channels, scratch);
}

/// [`whole_row`] with AVX2's wider vectors.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
fn whole_row_avx2(
    extrema: &mut [[f32; 2]],
    block: &[f32],
    channels: usize,
    scratch: [&mut Vec<f32>; 2],
) {
    whole_row_with(extrema, block, channels, scratch);
}

// Inlined so that its body compiles with the features of each caller:
// without it, `whole_row_avx2` calls the code compiled without AVX2.
#[allow(clippy::inline_always)]
#[inline(always)]
fn whole_row_with(
    extrema: &mut [[f32; 2]],
    block: &[f32],
    channels: usize,
    [minima, maxima]: [&mut Vec<f32>; 2],
) {
    let divisor = usize_to_f32_approx(channels);
    let (low, high) = block.split_at(block.len() / 2);
    minima.clear();
    minima.extend(low.iter().zip(high).map(|(a, b)| a.min(*b)));
    maxima.clear();
    maxima.extend(low.iter().zip(high).map(|(a, b)| a.max(*b)));
    let minimum = reduce(minima, channels, f32::min);
    let maximum = reduce(maxima, channels, f32::max);
    for ((pair, &low), &high) in extrema.iter_mut().zip(minimum).zip(maximum) {
        pair[0] = pair[0].min(low.clamp(-1.0, 1.0));
        pair[1] = pair[1].max(high.clamp(-1.0, 1.0));
    }
    let mut mixed = [0.0_f32; ROW_FRAMES];
    // A channel count the compiler knows lets it unroll each frame's sum
    // and work on several frames at once.
    match channels {
        1 => mix_frames::<1>(block, &mut mixed),
        2 => mix_frames::<2>(block, &mut mixed),
        3 => mix_frames::<3>(block, &mut mixed),
        4 => mix_frames::<4>(block, &mut mixed),
        5 => mix_frames::<5>(block, &mut mixed),
        6 => mix_frames::<6>(block, &mut mixed),
        7 => mix_frames::<7>(block, &mut mixed),
        8 => mix_frames::<8>(block, &mut mixed),
        _ => {
            for (mix, samples) in mixed.iter_mut().zip(block.chunks_exact(channels)) {
                *mix = clamped_sum(samples);
            }
        }
    }
    for value in &mut mixed {
        *value /= divisor;
    }
    let (low, high) = mixed.split_at(ROW_FRAMES / 2);
    let mut mix_minima = [0.0_f32; ROW_FRAMES / 2];
    let mut mix_maxima = [0.0_f32; ROW_FRAMES / 2];
    for (((minimum, maximum), a), b) in mix_minima
        .iter_mut()
        .zip(&mut mix_maxima)
        .zip(low)
        .zip(high)
    {
        (*minimum, *maximum) = (a.min(*b), a.max(*b));
    }
    let mix_minimum = reduce(&mut mix_minima, 1, f32::min)[0];
    let mix_maximum = reduce(&mut mix_maxima, 1, f32::max)[0];
    let pair = &mut extrema[channels];
    pair[0] = pair[0].min(mix_minimum);
    pair[1] = pair[1].max(mix_maximum);
}

/// The sum of each frame of `block`, in `C` channels, clamped to full
/// scale, into `mixed`.
fn mix_frames<const C: usize>(block: &[f32], mixed: &mut [f32; ROW_FRAMES]) {
    for (mix, samples) in mixed.iter_mut().zip(block.as_chunks::<C>().0) {
        *mix = clamped_sum(samples);
    }
}

/// The sum of `samples` clamped to full scale, added in order.
fn clamped_sum(samples: &[f32]) -> f32 {
    let mut sum = 0.0;
    for &value in samples {
        sum += value.clamp(-1.0, 1.0);
    }
    sum
}

/// Fold `values`, `width` interleaved columns of a power-of-two count of
/// rows, in halves with `fold` down to one row, which it returns.
fn reduce(values: &mut [f32], width: usize, fold: impl Fn(f32, f32) -> f32) -> &[f32] {
    let mut length = values.len();
    while length > width {
        length /= 2;
        let (low, high) = values[..2 * length].split_at_mut(length);
        for (a, b) in low.iter_mut().zip(&*high) {
            *a = fold(*a, *b);
        }
    }
    &values[..width]
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
    fn whole_rows_equal_rows_summed_frame_by_frame() {
        let mut state = 0x2545_f491_u64;
        for channels in 1..=8 {
            for _ in 0..20 {
                let block: Vec<f32> = (0..ROW_FRAMES * channels)
                    .map(|_| {
                        state = state
                            .wrapping_mul(6_364_136_223_846_793_005)
                            .wrapping_add(1_442_695_040_888_963_407);
                        f32::from(u16::try_from(state >> 48).unwrap()) / 20_000.0 - 1.6
                    })
                    .collect();
                // Rows that hold frames already, and new ones.
                for start in [[f32::INFINITY, f32::NEG_INFINITY], [-0.25, 0.25]] {
                    let mut part = vec![start; channels + 1];
                    part_row(&mut part, &block, channels, channels);
                    let bits = |row: &[[f32; 2]]| {
                        row.iter()
                            .map(|pair| pair.map(f32::to_bits))
                            .collect::<Vec<_>>()
                    };
                    // With the CPU's widest vectors, and without.
                    for whole_row in [whole_row, whole_row_with] {
                        let mut whole = vec![start; channels + 1];
                        let mut scratch = (Vec::new(), Vec::new());
                        whole_row(
                            &mut whole,
                            &block,
                            channels,
                            [&mut scratch.0, &mut scratch.1],
                        );
                        assert_eq!(bits(&whole), bits(&part), "{channels} channels");
                    }
                }
            }
        }
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
