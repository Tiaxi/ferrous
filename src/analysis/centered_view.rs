// SPDX-License-Identifier: GPL-3.0-or-later

//! Where the Qt spectrogram's centered view starts (`centeredDisplayRange` in
//! `ui/src/SpectrogramItem.cpp`), which decode windows must cover. Both are
//! tested against the cases in `fixtures/centered-view.txt`.

use super::column_hop::{column_hop, reference_hop};

/// The view shows the whole track once its window covers this share of it.
const WHOLE_TRACK_SHARE: f64 = 0.9;

/// How long before the view's left edge a decode window starts, for the
/// STFT to warm up.
pub(super) const WARMUP_SECONDS: f64 = 2.0;

/// How many seconds a view of `width` pixels shows at `zoom`, of a
/// session of audio at `sample_rate`: its columns, `zoom` pixels a
/// reference hop apart.
pub(super) fn window_seconds(width: u32, zoom: f32, sample_rate: u32) -> f64 {
    let sample_rate = sample_rate.max(1);
    // Hop sizes are small; precision loss from usize→f64 is negligible.
    #[allow(clippy::cast_precision_loss)]
    let (hop, reference) = (
        column_hop(sample_rate, zoom) as f64,
        reference_hop(sample_rate) as f64,
    );
    let effective_zoom = f64::from(zoom) * hop / reference;
    let columns_per_second = f64::from(sample_rate) / hop;
    f64::from(width) / effective_zoom.max(0.01) / columns_per_second
}

/// The left edge, in seconds, of a centered view `window_seconds` wide with
/// the playhead at `position_seconds`, in a track `duration_seconds` long (0
/// if unknown).
///
/// The view centers the playhead, except that it shows the whole track when
/// its window covers most of it, and it ends at the track's end: once the
/// rest of the track is shorter than half a window, the left edge stays a
/// window before the end.
pub(super) fn visible_left_edge_seconds(
    position_seconds: f64,
    window_seconds: f64,
    duration_seconds: f64,
) -> f64 {
    let centered_left = (position_seconds - window_seconds * 0.5).max(0.0);
    if duration_seconds <= 0.0 || window_seconds <= 0.0 {
        return centered_left;
    }
    if window_seconds >= duration_seconds * WHOLE_TRACK_SHARE {
        return 0.0;
    }
    centered_left.min((duration_seconds - window_seconds).max(0.0))
}

/// Where a decode window for a playhead at `position_seconds` starts, so
/// that playback from there to the end of the track never shows columns
/// that were not decoded on the left of the view.
pub(super) fn decode_start_seconds(
    position_seconds: f64,
    window_seconds: f64,
    duration_seconds: f64,
) -> f64 {
    (visible_left_edge_seconds(position_seconds, window_seconds, duration_seconds) - WARMUP_SECONDS)
        .max(0.0)
}

#[cfg(test)]
mod tests {
    use super::{decode_start_seconds, visible_left_edge_seconds, window_seconds};

    #[test]
    fn decode_windows_start_before_the_view() {
        // Mid track, half a window before the playhead, and warm-up.
        assert!((decode_start_seconds(100.0, 20.0, 400.0) - 88.0).abs() < 1e-9);
        // A window before the end of the track, near it.
        assert!((decode_start_seconds(399.0, 20.0, 400.0) - 378.0).abs() < 1e-9);
        // An unknown length ends no view.
        assert!((decode_start_seconds(399.0, 20.0, 0.0) - 387.0).abs() < 1e-9);
        // Never before the start of the track.
        assert!(decode_start_seconds(5.0, 20.0, 400.0).abs() < 1e-9);
    }

    #[test]
    fn a_window_is_as_wide_as_the_columns_the_view_shows() {
        // 1920 pixels of a column each, at 46.875 columns a second, at
        // every rate of the 48 kHz family.
        for rate in [48_000, 96_000, 192_000, 384_000] {
            let seconds = window_seconds(1920, 1.0, rate);
            assert!((seconds - 40.96).abs() < 1e-9, "{rate}: {seconds}");
            // Zoomed in twice, on columns half as far apart: a pixel a column.
            let seconds = window_seconds(1920, 2.0, rate);
            assert!((seconds - 20.48).abs() < 1e-9, "{rate}: {seconds}");
        }
        // The 44.1 kHz family, at 43.07 columns a second.
        for rate in [44_100, 88_200, 176_400] {
            let seconds = window_seconds(1920, 1.0, rate);
            assert!(
                (seconds - 1920.0 * 1024.0 / 44_100.0).abs() < 1e-9,
                "{rate}: {seconds}"
            );
        }
    }

    #[test]
    fn matches_the_cases_shared_with_the_qt_view() {
        let cases = include_str!("fixtures/centered-view.txt");
        let mut checked = 0;
        for line in cases
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
        {
            let values: Vec<f64> = line
                .split_whitespace()
                .map(|value| value.parse().expect("a number"))
                .collect();
            let [position, window, duration, left] = values[..] else {
                panic!("four values in {line:?}");
            };
            let actual = visible_left_edge_seconds(position, window, duration);
            assert!((actual - left).abs() < 1e-9, "{line}: left edge {actual}");
            checked += 1;
        }
        assert!(checked >= 9, "only {checked} cases");
    }

    #[test]
    fn an_unknown_duration_centers_the_playhead() {
        assert!((visible_left_edge_seconds(30.0, 10.0, 0.0) - 25.0).abs() < 1e-9);
        assert!(visible_left_edge_seconds(3.0, 10.0, 0.0).abs() < 1e-9);
    }
}
