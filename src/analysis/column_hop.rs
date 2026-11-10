// SPDX-License-Identifier: GPL-3.0-or-later

//! How far apart a spectrogram session's columns are, in samples.
//!
//! A column is a fixed time rather than a fixed number of samples, so a
//! track scrolls at the same pace whatever its sample rate. At zoom 1 a
//! column is 1024 samples of the rate's family base:
//!
//! * rates that are multiples of 11,025 Hz (11.025, 22.05, 44.1, 88.2,
//!   176.4 and 352.8 kHz) are of the 44.1 kHz family, `1024 × rate /
//!   44100` samples: 23.2 ms, 43.07 columns a second;
//! * every other rate (8, 16, 32, 48, 50, 96, 192, 384 kHz, ...) is of the
//!   48 kHz family, `1024 × rate / 48000` samples, rounded: 21.3 ms, 46.875
//!   columns a second.
//!
//! The two families differ by about 9%, as 44.1 and 48 kHz always have:
//! the families' own rates get whole power-of-two hops, and a rate of
//! neither family, such as 32 or 50 kHz, keeps the 48 kHz pace within the
//! rounding of its hop. Zooming divides the reference hop; no column hop
//! is shorter than [`MIN_COLUMN_HOP`] samples, which only rates below 11 kHz
//! reach, at the deepest zooms.

/// A column's samples at zoom 1 at the base rate of either family.
const BASE_HOP: f64 = 1024.0;

/// The fewest samples between columns. It keeps the hop of a very low rate
/// at a deep zoom to a whole number of samples that rounding does not
/// dominate.
pub(super) const MIN_COLUMN_HOP: usize = 16;

/// The zoom levels the backend accepts. The wheel zooms in up to 16 times
/// (`kMaxWheelZoom` in `ui/src/SpectrogramItem.cpp`); only the default zoom
/// that stretches a short track to the view's width
/// ([`super::centered_view::fit_zoom`]) goes deeper, to a column every 16
/// samples at 44.1 and 48 kHz.
pub(super) const MIN_ZOOM: f32 = 0.05;
pub(super) const MAX_ZOOM: f32 = 64.0;

/// The base rate of the family `sample_rate` belongs to.
pub(super) fn family_base_rate(sample_rate: u32) -> u32 {
    if sample_rate.is_multiple_of(11_025) {
        44_100
    } else {
        48_000
    }
}

/// `value` rounded, as a hop of at least [`MIN_COLUMN_HOP`] samples.
fn hop_from_f64(value: f64) -> usize {
    // Hops are positive and far below `usize::MAX`; `as` saturates.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let hop = value.round() as usize;
    hop.max(MIN_COLUMN_HOP)
}

/// The column hop of a session at zoom 1 for audio at `sample_rate`: its
/// family's 1024 samples at the family's base rate. An unknown rate (0)
/// gets 1024.
#[must_use]
pub fn reference_hop(sample_rate: u32) -> usize {
    if sample_rate == 0 {
        return 1024;
    }
    hop_from_f64(BASE_HOP * f64::from(sample_rate) / f64::from(family_base_rate(sample_rate)))
}

/// The column hop of a session at `zoom` for audio at `sample_rate`: the
/// reference hop divided by the zoom, which the backend clamps first.
#[must_use]
pub fn column_hop(sample_rate: u32, zoom: f32) -> usize {
    let zoom = if zoom.is_finite() { zoom } else { 1.0 };
    let zoom = f64::from(zoom.clamp(MIN_ZOOM, MAX_ZOOM));
    // Reference hops are at most a few thousand samples.
    #[allow(clippy::cast_precision_loss)]
    let reference = reference_hop(sample_rate) as f64;
    hop_from_f64(reference / zoom)
}

/// The zoom a view draws data whose columns are `hop` samples apart at,
/// for audio at `sample_rate`, when `zoom` was asked for: one column a
/// pixel wherever the hop is not the reference hop, which is the zoom the
/// hop stands for after its rounding, the zoom clamp and the
/// [`MIN_COLUMN_HOP`] floor; `zoom` itself at the reference hop. Qt draws
/// at this zoom, and the centered window and retention reckon with it.
#[must_use]
pub fn render_zoom(sample_rate: u32, hop: usize, zoom: f64) -> f64 {
    let reference = reference_hop(sample_rate);
    if hop > 0 && hop != reference {
        // Hops are at most a few hundred thousand samples.
        #[allow(clippy::cast_precision_loss)]
        let snapped = reference as f64 / hop as f64;
        snapped
    } else {
        zoom
    }
}

/// The zoom Qt draws data whose columns are `hop` samples apart at, of
/// audio at `sample_rate`, at `render_zoom` ([`render_zoom`]): the canvas
/// pixels a column takes at a device pixel ratio of 1. It is the render
/// zoom times the hop over the reference hop, in `f64`, in that order, as
/// `effectiveZoomLocked` in `ui/src/SpectrogramItem.cpp` computes it, by
/// calling this. Wherever the hop is not the reference hop it is 1 but for
/// rounding, which can leave it a bit below: at 8 kHz at zoom 0.57, `171 /
/// 300 × 300 / 171` is 0.9999999999999999, and a view 1680 pixels wide
/// shows 1681 columns.
#[must_use]
pub fn effective_zoom(sample_rate: u32, hop: usize, render_zoom: f64) -> f64 {
    // Hops are at most a few hundred thousand samples.
    #[allow(clippy::cast_precision_loss)]
    let (hop, reference) = (hop as f64, reference_hop(sample_rate) as f64);
    render_zoom * hop / reference
}

/// The pixels a column of data `hop` samples apart takes at
/// [`render_zoom`]: 1 wherever the hop is not the reference hop.
#[must_use]
pub fn pixels_per_column(sample_rate: u32, hop: usize, zoom: f64) -> f64 {
    if hop > 0 && hop != reference_hop(sample_rate) {
        // Exactly, rather than the rounding of `reference / hop × hop /
        // reference`.
        1.0
    } else {
        zoom
    }
}

/// The STFT hop and output decimation of a session.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct ColumnHops {
    /// Samples between the STFT's windows.
    pub(super) stft_hop: usize,
    /// STFT rows per output column, at least 1. Fractional intervals keep
    /// the columns on the exact grid of `effective_hop`.
    pub(super) output_interval: f64,
    /// Samples between output columns: [`column_hop`].
    pub(super) effective_hop: usize,
}

/// The hops of a session with `fft_size`-sample windows at `zoom` for
/// audio at `sample_rate`.
///
/// The STFT runs at an eighth of the window, or every column when columns
/// are closer, and the output peak-holds its rows down to one a column.
/// Zoomed in, where each column needs a window of its own, the STFT steps
/// by a column as long as its windows then still cover all the audio; when
/// they would leave gaps, as a small window at a high rate does, it keeps
/// the eighth-window hop and peak-holds instead.
pub(super) fn column_hops(fft_size: usize, zoom: f32, sample_rate: u32) -> ColumnHops {
    let effective_hop = column_hop(sample_rate, zoom);
    let overlapped = (fft_size / 8).max(64);
    let stft_hop = if zoom > 1.0 && effective_hop <= fft_size {
        effective_hop
    } else {
        overlapped.min(effective_hop)
    };
    // Both hops are small; precision loss is negligible.
    #[allow(clippy::cast_precision_loss)]
    let output_interval = (effective_hop as f64 / stft_hop as f64).max(1.0);
    ColumnHops {
        stft_hop,
        output_interval,
        effective_hop,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FFT_SIZES: [usize; 5] = [512, 1024, 2048, 4096, 8192];

    fn columns_per_second(sample_rate: u32, zoom: f32) -> f64 {
        #[allow(clippy::cast_precision_loss)]
        let hop = column_hop(sample_rate, zoom) as f64;
        f64::from(sample_rate) / hop
    }

    #[test]
    fn each_family_gets_power_of_two_reference_hops() {
        for (rate, hop) in [
            (44_100, 1024),
            (88_200, 2048),
            (176_400, 4096),
            (352_800, 8192),
            (48_000, 1024),
            (96_000, 2048),
            (192_000, 4096),
            (384_000, 8192),
        ] {
            assert_eq!(reference_hop(rate), hop, "{rate} Hz");
        }
    }

    #[test]
    fn columns_are_as_long_within_each_family_at_every_zoom() {
        for zoom in [1.0, 4.0, 2.5, 0.5, 0.1] {
            for (family, base) in [
                (&[44_100, 88_200, 176_400, 352_800][..], 44_100.0),
                (&[48_000, 96_000, 192_000, 384_000][..], 48_000.0),
            ] {
                let pace = columns_per_second(family[0], zoom);
                // Where the base rate's hop is a whole number of samples,
                // the hops of a family are it times a power of two, so the
                // paces are equal to the last bit; elsewhere they are
                // within the rounding of the shortest hop to a sample.
                let base_hop = 1024.0 / f64::from(zoom);
                let tolerance = if (base_hop - base_hop.round()).abs() < 1e-9 {
                    1e-9
                } else {
                    pace * 0.5 / base_hop.floor()
                };
                for &rate in family {
                    let other = columns_per_second(rate, zoom);
                    assert!(
                        (other - pace).abs() < tolerance,
                        "{rate} Hz at zoom {zoom}: {other} columns a second, {pace} at {}",
                        family[0]
                    );
                }
                // The family's pace: its base rate's 1024 samples a column.
                if (zoom - 1.0).abs() < f32::EPSILON {
                    assert!((pace - base / 1024.0).abs() < 1e-9, "{pace}");
                }
            }
        }
        // Zoomed in four times: four times as many columns.
        assert!((columns_per_second(96_000, 4.0) - 4.0 * 46.875).abs() < 1e-9);
        assert!((columns_per_second(88_200, 4.0) - 4.0 * 44_100.0 / 1024.0).abs() < 1e-9);
    }

    #[test]
    fn other_rates_keep_their_family_pace() {
        for (rate, hop, pace) in [
            // Multiples of 11.025 kHz are of the 44.1 kHz family.
            (22_050, 512, 44_100.0 / 1024.0),
            (11_025, 256, 44_100.0 / 1024.0),
            // The rest are of the 48 kHz family, rounded to a whole sample.
            (32_000, 683, 46.875),
            (24_000, 512, 46.875),
            (16_000, 341, 46.875),
            (8_000, 171, 46.875),
            (50_000, 1067, 46.875),
            (37_800, 806, 46.875),
        ] {
            assert_eq!(reference_hop(rate), hop, "{rate} Hz");
            let actual = columns_per_second(rate, 1.0);
            assert!(
                (actual / pace - 1.0).abs() < 0.002,
                "{rate} Hz: {actual} columns a second"
            );
        }
        // An unknown rate keeps the hop of 44.1 and 48 kHz.
        assert_eq!(reference_hop(0), 1024);
    }

    #[test]
    fn the_effective_zoom_keeps_the_rounding_of_its_order_of_operations() {
        // 171 / 300 × 300 / 171 is a bit below 1: the view shows one
        // column more than its pixels.
        assert_eq!(column_hop(8_000, 0.57), 300);
        let render = render_zoom(8_000, 300, 0.57);
        assert_eq!(render, 171.0 / 300.0);
        let zoom = effective_zoom(8_000, 300, render);
        assert_eq!(zoom, 0.999_999_999_999_999_9);
        assert!(zoom < 1.0);
        assert_eq!((1_680.0 / zoom).ceil(), 1_681.0);
        // At the reference hop, the zoom itself.
        assert_eq!(effective_zoom(48_000, 1_024, 2.5), 2.5);
        // Where the hop is a power of two times the reference, exactly 1.
        assert_eq!(
            effective_zoom(48_000, 512, render_zoom(48_000, 512, 2.0)),
            1.0
        );
    }

    #[test]
    fn low_rates_keep_hops_of_whole_samples_at_deep_zooms() {
        // 8 kHz at zoom 16 would be 10.7 samples a column.
        assert_eq!(column_hop(8_000, 16.0), MIN_COLUMN_HOP);
        assert_eq!(column_hop(11_025, 16.0), MIN_COLUMN_HOP);
        // 44.1 kHz is well clear of the floor.
        assert_eq!(column_hop(44_100, 16.0), 64);
        // Rates no real file has still get a hop.
        assert_eq!(reference_hop(1), MIN_COLUMN_HOP);
    }

    #[test]
    fn zoom_is_clamped_to_what_the_backend_accepts() {
        assert_eq!(column_hop(48_000, 256.0), 16);
        assert_eq!(column_hop(48_000, 0.001), 20_480);
        assert_eq!(column_hop(96_000, 256.0), 32);
        // 384 kHz zoomed out all the way is more than 16 bits.
        assert_eq!(column_hop(384_000, 0.001), 163_840);
        assert_eq!(column_hop(48_000, f32::NAN), 1024);
    }

    #[test]
    fn views_draw_a_column_a_pixel_at_hops_other_than_the_reference() {
        // 8 kHz at 16x is floored at 16 samples, which stands for 10.7x.
        let hop = column_hop(8_000, 16.0);
        assert_eq!(hop, MIN_COLUMN_HOP);
        assert!((render_zoom(8_000, hop, 16.0) - 171.0 / 16.0).abs() < 1e-12);
        assert!((pixels_per_column(8_000, hop, 16.0) - 1.0).abs() < 1e-12);
        // 32 kHz at 4x: 683 / 4 rounds to 171, which stands for 3.994x.
        let hop = column_hop(32_000, 4.0);
        assert_eq!(hop, 171);
        assert!((render_zoom(32_000, hop, 4.0) - 683.0 / 171.0).abs() < 1e-12);
        assert!((pixels_per_column(32_000, hop, 4.0) - 1.0).abs() < 1e-12);
        // At the reference hop the zoom asked for is drawn.
        assert!((render_zoom(48_000, 1024, 1.0002) - 1.0002).abs() < 1e-12);
        assert!((pixels_per_column(48_000, 1024, 1.0002) - 1.0002).abs() < 1e-12);
        for rate in [8_000, 22_050, 32_000, 44_100, 96_000, 384_000] {
            for zoom in [0.05, 0.3, 2.5, 3.815, 16.0] {
                let hop = column_hop(rate, zoom);
                assert!(
                    (pixels_per_column(rate, hop, f64::from(zoom)) - 1.0).abs() < 1e-12,
                    "{rate} at {zoom}"
                );
            }
        }
    }

    #[test]
    fn the_stft_and_decimation_give_the_column_hop() {
        for rate in [
            8_000, 22_050, 32_000, 44_100, 48_000, 96_000, 192_000, 384_000,
        ] {
            for fft in FFT_SIZES {
                for zoom in [0.05, 0.1, 0.37, 0.8, 1.0, 1.5, 3.815, 4.0, 16.0] {
                    let hops = column_hops(fft, zoom, rate);
                    assert!(hops.stft_hop >= 1);
                    assert!(hops.output_interval >= 1.0, "{rate} {fft} {zoom}: {hops:?}");
                    #[allow(
                        clippy::cast_possible_truncation,
                        clippy::cast_sign_loss,
                        clippy::cast_precision_loss
                    )]
                    let effective = (hops.stft_hop as f64 * hops.output_interval).round() as usize;
                    assert_eq!(effective, column_hop(rate, zoom), "{rate} {fft} {zoom}");
                    assert_eq!(hops.effective_hop, effective);
                    // No audio between two windows goes unseen.
                    assert!(hops.stft_hop <= fft, "{rate} {fft} {zoom}: {hops:?}");
                }
            }
        }
    }

    #[test]
    fn the_stft_hops_of_44_and_48_khz_stay_where_windows_cover_their_columns() {
        // The hops before the reference hop followed the rate.
        for rate in [44_100, 48_000] {
            assert_eq!(column_hops(8192, 1.0, rate).stft_hop, 1024);
            assert_eq!(column_hops(2048, 1.0, rate).stft_hop, 256);
            assert_eq!(column_hops(8192, 2.0, rate).stft_hop, 512);
            assert_eq!(column_hops(8192, 4.0, rate).stft_hop, 256);
            assert_eq!(column_hops(8192, 16.0, rate).stft_hop, 64);
            assert_eq!(column_hops(8192, 0.5, rate).stft_hop, 1024);
            assert_eq!(column_hops(2048, 3.815, rate).stft_hop, 268);
            assert!((column_hops(8192, 0.8, rate).output_interval - 1.25).abs() < 1e-12);
        }
    }

    #[test]
    fn a_zoomed_in_small_window_at_a_high_rate_peak_holds() {
        // A 512-sample window at 192 kHz zoomed in 1.5 times: columns 2731
        // samples apart. Windows a column apart would see a fifth of the
        // audio; the eighth-window hop sees all of it.
        let hops = column_hops(512, 1.5, 192_000);
        assert_eq!(hops.effective_hop, 2731);
        assert_eq!(hops.stft_hop, 64);
        // A window as long as the column steps by the column.
        let hops = column_hops(4096, 1.5, 192_000);
        assert_eq!(hops.stft_hop, 2731);
        assert!((hops.output_interval - 1.0).abs() < f64::EPSILON);
    }
}
