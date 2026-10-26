// SPDX-License-Identifier: GPL-3.0-or-later

//! Encoding of STFT power as the spectrogram's byte levels.

use std::cell::RefCell;

// Shared with SpectrogramItem::buildPrecomputedDbRemapLocked: zero means
// silence/below-floor; 1..=255 encodes -150..=0 dB relative to full scale.
// The user's dB range is applied by that remap, not here.
const PRECOMPUTED_DB_RANGE: f32 = 150.0;

/// Compute the expected peak power dB for a full-scale sine in a
/// Blackman-Harris-windowed FFT of the given size.  This is used to
/// normalise the STFT output so that a full-scale signal maps to u8≈255
/// regardless of FFT size.
///
/// For BH4 window: sum(w) = N × a0 (a0 = 0.35875).
/// Peak bin magnitude = sum(w) / 2, power = (N × a0 / 2)².
#[allow(clippy::cast_precision_loss)]
fn stft_peak_power_db(fft_size: usize) -> f64 {
    20.0 * (fft_size as f64 * 0.35875 / 2.0).log10()
}

/// The level of one bin's power, `peak_db` being the FFT size's
/// [`stft_peak_power_db`]. [`SpectrumQuantizer`] gives the same levels
/// without a logarithm per bin.
fn power_level(v: f32, peak_db: f64) -> u8 {
    let range = f64::from(PRECOMPUTED_DB_RANGE);
    if v.is_nan() || v <= 0.0 {
        return 0;
    }
    let db = (10.0 / std::f64::consts::LN_10) * f64::from(v).ln();
    // Normalise for FFT size: anchor at the BH4 peak power so that a
    // full-scale signal maps to u8≈255 for any FFT size.
    if db < peak_db - range {
        return 0;
    }
    let xdb = (db + range - peak_db).clamp(0.0, range);
    let scaled = 1.0 + (xdb / range) * 254.0;
    // Value is clamped to 0.0..=255.0, so truncation and sign loss are impossible.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let result = scaled.round().clamp(0.0, 255.0) as u8;
    result
}

/// Bits of mantissa that pick a bucket along with the exponent. A bucket of
/// normal numbers spans a ratio of at most 1 + 1/16, less than the 0.59 dB
/// (a ratio of 1.146) between levels, so it holds at most one threshold.
const BUCKET_MANTISSA_BITS: u32 = 4;
const BUCKET_SHIFT: u32 = f32::MANTISSA_DIGITS - 1 - BUCKET_MANTISSA_BITS;
/// Enough buckets for every non-negative `f32` up to infinity.
const BUCKETS: usize = 1 << (8 + BUCKET_MANTISSA_BITS);

/// Encodes STFT power as byte levels for one FFT size, giving exactly the
/// levels of [`power_level`].
///
/// The level only grows with the power: a logarithm's error is far below the
/// relative spacing of `f32` values, so it orders them as they are ordered,
/// and the arithmetic after it is monotonic too. So level `k` is the number
/// of thresholds at or below the power, the threshold of each level being
/// the least `f32` with that level or more, which [`Self::new`] finds by
/// bisecting over the bit patterns of positive `f32`s, as they sort in the
/// order of their values.
struct SpectrumQuantizer {
    fft_size: usize,
    /// `thresholds[k - 1]` is the least power encoded as level `k` or more.
    thresholds: [f32; 255],
    /// The level of the least power of each bucket of `f32` bit patterns.
    bucket_levels: Box<[u8; BUCKETS]>,
}

impl SpectrumQuantizer {
    fn new(fft_size: usize) -> Self {
        let peak_db = stft_peak_power_db(fft_size);
        let level = |bits: u32| power_level(f32::from_bits(bits), peak_db);
        let infinity = f32::INFINITY.to_bits();
        let mut thresholds = [f32::INFINITY; 255];
        // The least positive `f32`; the levels of zero and below are zero.
        let mut low = 1;
        for (wanted, threshold) in (1..=u8::MAX).zip(&mut thresholds) {
            // Infinity has level 255, so the search always ends on a value.
            let mut high = infinity;
            while low < high {
                let mid = low + (high - low) / 2;
                if level(mid) >= wanted {
                    high = mid;
                } else {
                    low = mid + 1;
                }
            }
            *threshold = f32::from_bits(low);
        }
        let mut bucket_levels = Box::new([0; BUCKETS]);
        for (bucket, bucket_level) in (0_u32..).zip(bucket_levels.iter_mut()) {
            let least = f32::from_bits(bucket << BUCKET_SHIFT);
            let count = thresholds.partition_point(|&threshold| threshold <= least);
            *bucket_level = u8::try_from(count).unwrap_or(u8::MAX);
        }
        Self {
            fft_size,
            thresholds,
            bucket_levels,
        }
    }

    fn level(&self, power: f32) -> u8 {
        if power.is_nan() || power <= 0.0 {
            return 0;
        }
        // A positive power's sign bit is clear, so its bucket is in range.
        let bucket = (power.to_bits() >> BUCKET_SHIFT) as usize & (BUCKETS - 1);
        let mut level = self.bucket_levels[bucket];
        while let Some(&threshold) = self.thresholds.get(usize::from(level)) {
            if threshold > power {
                break;
            }
            level += 1;
        }
        level
    }

    /// Append the levels of a row's first `bins` bins, zero past its end.
    fn append_row(&self, out: &mut Vec<u8>, row: &[f32], bins: usize) {
        out.extend(row.iter().take(bins).map(|&power| self.level(power)));
        out.extend(std::iter::repeat_n(0, bins.saturating_sub(row.len())));
    }
}

thread_local! {
    /// The quantizer of the FFT size this thread last encoded, which a
    /// decode thread keeps for its session.
    static QUANTIZER: RefCell<Option<SpectrumQuantizer>> = const { RefCell::new(None) };
}

/// Append the levels of an STFT row's first `bins` bins for `fft_size`,
/// zero past the row's end.
pub(super) fn append_quantized_row(out: &mut Vec<u8>, row: &[f32], bins: usize, fft_size: usize) {
    QUANTIZER.with_borrow_mut(|cached| {
        if cached
            .as_ref()
            .is_some_and(|quantizer| quantizer.fft_size != fft_size)
        {
            *cached = None;
        }
        cached
            .get_or_insert_with(|| SpectrumQuantizer::new(fft_size))
            .append_row(out, row, bins);
    });
}

/// The encoding before [`SpectrumQuantizer`], kept verbatim as the
/// reference its levels must match.
#[cfg(test)]
pub(super) fn reference_level(v: f32, fft_size: usize) -> u8 {
    let range = f64::from(PRECOMPUTED_DB_RANGE);
    if v.is_nan() || v <= 0.0 {
        return 0;
    }
    let db = (10.0 / std::f64::consts::LN_10) * f64::from(v).ln();
    // Normalise for FFT size: anchor at the BH4 peak power so that a
    // full-scale signal maps to u8≈255 for any FFT size.
    let peak_db = stft_peak_power_db(fft_size);
    if db < peak_db - range {
        return 0;
    }
    let xdb = (db + range - peak_db).clamp(0.0, range);
    let scaled = 1.0 + (xdb / range) * 254.0;
    // Value is clamped to 0.0..=255.0, so truncation and sign loss are impossible.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let result = scaled.round().clamp(0.0, 255.0) as u8;
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every FFT size the settings allow, and some they do not.
    const FFT_SIZES: [usize; 8] = [256, 512, 1_024, 2_048, 4_096, 8_192, 16_384, 1_000];

    fn assert_matches(quantizer: &SpectrumQuantizer, fft_size: usize, power: f32) {
        assert_eq!(
            quantizer.level(power),
            reference_level(power, fft_size),
            "power {power:e} ({:#010x}) at FFT {fft_size}",
            power.to_bits()
        );
    }

    #[test]
    fn levels_match_the_reference_at_every_threshold_and_bucket_edge() {
        for fft_size in FFT_SIZES {
            let quantizer = SpectrumQuantizer::new(fft_size);
            let edges = quantizer
                .thresholds
                .iter()
                .map(|threshold| threshold.to_bits())
                .chain((0..u32::try_from(BUCKETS).unwrap()).map(|bucket| bucket << BUCKET_SHIFT));
            for bits in edges {
                for near in bits.saturating_sub(3)..=bits.saturating_add(3) {
                    assert_matches(&quantizer, fft_size, f32::from_bits(near));
                    assert_matches(&quantizer, fft_size, -f32::from_bits(near));
                }
            }
            for special in [
                0.0,
                -0.0,
                f32::MIN_POSITIVE,
                f32::from_bits(1),
                f32::MAX,
                f32::INFINITY,
                f32::NEG_INFINITY,
                f32::NAN,
                -f32::NAN,
                f32::from_bits(0x7fc0_1234),
                f32::from_bits(0xffff_ffff),
            ] {
                assert_matches(&quantizer, fft_size, special);
            }
        }
    }

    #[test]
    fn every_level_has_a_threshold_where_the_reference_reaches_it() {
        for fft_size in FFT_SIZES {
            let quantizer = SpectrumQuantizer::new(fft_size);
            for (level, &threshold) in (1..=u8::MAX).zip(&quantizer.thresholds) {
                let below = f32::from_bits(threshold.to_bits() - 1);
                assert!(reference_level(threshold, fft_size) >= level);
                assert!(reference_level(below, fft_size) < level);
            }
        }
    }

    #[test]
    fn levels_match_the_reference_for_random_bit_patterns() {
        let mut state = 0x2545_f491_4f6c_dd1d_u64;
        for fft_size in FFT_SIZES {
            let quantizer = SpectrumQuantizer::new(fft_size);
            for _ in 0..200_000 {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                let bits = u32::try_from(state >> 32).unwrap();
                assert_matches(&quantizer, fft_size, f32::from_bits(bits));
            }
        }
    }

    #[test]
    fn encoding_preserves_full_dynamic_range_and_silence() {
        for fft_size in [512, 1_024, 2_048, 4_096, 8_192] {
            let quantizer = SpectrumQuantizer::new(fft_size);
            for silent in [0.0, -1.0, f32::NAN] {
                assert_eq!(quantizer.level(silent), 0);
            }
            let mut previous = 0;
            for db in [-160.0, -150.0, -140.0, -132.0, -90.0, -50.0, 0.0] {
                // FFT powers in this test remain well within f32's range.
                #[allow(clippy::cast_possible_truncation)]
                let power = 10.0_f64.powf((stft_peak_power_db(fft_size) + db) / 10.0) as f32;
                let encoded = quantizer.level(power);
                assert!(encoded >= previous);
                if db < -150.0 {
                    assert_eq!(encoded, 0);
                } else if db > -150.0 {
                    let decoded = (f64::from(encoded) - 1.0) / 254.0 * 150.0 - 150.0;
                    assert!((decoded - db).abs() < 0.3, "{fft_size}: {db} -> {decoded}");
                }
                previous = encoded;
            }
            assert_eq!(previous, 255);
        }
    }

    #[test]
    fn a_thread_rebuilds_its_quantizer_when_the_fft_size_changes() {
        // At 8192 this power is 6 dB below the peak; at 512 it is above it.
        #[allow(clippy::cast_possible_truncation)]
        let power = 10.0_f64.powf(stft_peak_power_db(8_192) / 10.0) as f32 / 4.0;
        let mut levels = Vec::new();
        for fft_size in [8_192, 512, 8_192] {
            append_quantized_row(&mut levels, &[power, 0.0], 3, fft_size);
        }
        let at = |fft_size| reference_level(power, fft_size);
        assert_eq!(levels, [at(8_192), 0, 0, at(512), 0, 0, at(8_192), 0, 0]);
        assert_ne!(at(8_192), at(512));
    }

    /// Every `f32` at every FFT size the settings allow. Run with
    /// `cargo test --release -- --ignored every_f32`.
    #[test]
    #[ignore = "exhaustive; takes about a minute in a release build"]
    fn every_f32_matches_the_reference() {
        let sizes = [512, 1_024, 2_048, 4_096, 8_192];
        std::thread::scope(|scope| {
            for fft_size in sizes {
                scope.spawn(move || {
                    let quantizer = SpectrumQuantizer::new(fft_size);
                    for bits in 0..=u32::MAX {
                        let power = f32::from_bits(bits);
                        assert_eq!(
                            quantizer.level(power),
                            reference_level(power, fft_size),
                            "bits {bits:#010x} at FFT {fft_size}"
                        );
                    }
                });
            }
        });
    }
}
