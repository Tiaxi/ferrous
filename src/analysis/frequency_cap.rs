// SPDX-License-Identifier: GPL-3.0-or-later

//! The band a spectrogram shows: up to Nyquist, or with the frequency cap
//! on, up to the audible band of the track's rate family.
//!
//! The cap is half the family's base rate: 22,050 Hz for the 44.1 kHz
//! family (multiples of 11,025 Hz) and 24,000 Hz for every other rate. A
//! rate whose Nyquist is at or below its cap, such as 44.1, 48 or 32 kHz,
//! shows its whole spectrum either way. A session sends only the bins it
//! shows, so a capped 96 kHz track takes half the bytes a column and a
//! capped 192 kHz track a quarter.

use super::column_hop::family_base_rate;

/// The top of the band the spectrogram shows with the cap on, in Hz, for
/// audio at `sample_rate`.
#[must_use]
pub(super) fn frequency_cap_hz(sample_rate: u32) -> u32 {
    family_base_rate(sample_rate) / 2
}

/// The bins of an `fft_size`-point spectrum of audio at `sample_rate` that
/// a session sends: all `fft_size / 2 + 1` of them, or with `capped`, those
/// from 0 Hz up to the cap. A bin is `sample_rate / fft_size` Hz wide, so
/// the top bin sent is the highest at or below the cap. An unknown rate
/// (0) sends them all.
#[must_use]
pub(super) fn shown_bins(fft_size: usize, sample_rate: u32, capped: bool) -> usize {
    let all = fft_size / 2 + 1;
    if !capped || sample_rate == 0 {
        return all;
    }
    let fft = u64::try_from(fft_size).unwrap_or(u64::MAX);
    let top_bin =
        u64::from(frequency_cap_hz(sample_rate)).saturating_mul(fft) / u64::from(sample_rate);
    usize::try_from(top_bin)
        .map_or(all, |top_bin| top_bin.saturating_add(1))
        .min(all)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_cap_is_half_the_rate_familys_base_rate() {
        for (rate, cap) in [
            (44_100, 22_050),
            (88_200, 22_050),
            (176_400, 22_050),
            (352_800, 22_050),
            (22_050, 22_050),
            (48_000, 24_000),
            (96_000, 24_000),
            (192_000, 24_000),
            (384_000, 24_000),
            (32_000, 24_000),
            // Of neither family.
            (50_000, 24_000),
        ] {
            assert_eq!(frequency_cap_hz(rate), cap, "{rate} Hz");
        }
    }

    /// The top frequency the bins sent reach, in Hz.
    fn top_hz(fft_size: usize, sample_rate: u32, capped: bool) -> f64 {
        let bins = shown_bins(fft_size, sample_rate, capped);
        #[allow(clippy::cast_precision_loss)]
        let top = (bins - 1) as f64 * f64::from(sample_rate) / fft_size as f64;
        top
    }

    #[test]
    fn capped_sessions_send_the_bins_up_to_the_cap() {
        // (rate, bins at FFT 2048 with the cap, top frequency)
        for (rate, bins, top) in [
            // At or below the cap: the whole spectrum.
            (44_100, 1_025, 22_050.0),
            (48_000, 1_025, 24_000.0),
            (32_000, 1_025, 16_000.0),
            (22_050, 1_025, 11_025.0),
            // Above it: up to the family's cap.
            (88_200, 513, 22_050.0),
            (96_000, 513, 24_000.0),
            (176_400, 257, 22_050.0),
            (192_000, 257, 24_000.0),
            (352_800, 129, 22_050.0),
            (384_000, 129, 24_000.0),
        ] {
            assert_eq!(shown_bins(2_048, rate, true), bins, "{rate} Hz");
            assert!((top_hz(2_048, rate, true) - top).abs() < 1e-9, "{rate} Hz");
        }
        // A rate of neither family: the highest bin at or below 24 kHz.
        assert_eq!(shown_bins(2_048, 50_000, true), 984);
        let top = top_hz(2_048, 50_000, true);
        assert!(
            top <= 24_000.0 && top > 24_000.0 - 50_000.0 / 2_048.0,
            "{top}"
        );
    }

    #[test]
    fn every_fft_size_caps_at_the_same_frequency() {
        for fft_size in [512, 1_024, 2_048, 4_096, 8_192] {
            for rate in [96_000, 192_000, 88_200, 176_400] {
                let cap = f64::from(frequency_cap_hz(rate));
                assert!((top_hz(fft_size, rate, true) - cap).abs() < 1e-9);
            }
            assert_eq!(shown_bins(fft_size, 96_000, true), fft_size / 4 + 1);
            assert_eq!(shown_bins(fft_size, 192_000, true), fft_size / 8 + 1);
        }
    }

    #[test]
    fn without_the_cap_or_a_rate_every_bin_is_sent() {
        for rate in [44_100, 96_000, 192_000, 50_000] {
            assert_eq!(shown_bins(2_048, rate, false), 1_025);
        }
        assert_eq!(shown_bins(2_048, 0, true), 1_025);
    }
}
