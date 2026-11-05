// SPDX-License-Identifier: GPL-3.0-or-later

//! Where the Qt spectrogram's centered view starts (`centeredDisplayRange` in
//! `ui/src/SpectrogramItem.cpp`), which decode windows must cover. Both are
//! tested against the cases in `fixtures/centered-view.txt`.
//!
//! The view is reckoned in a session's columns, as Qt reckons it: the
//! columns its window shows, the column of the playhead, and the column
//! after the last the session decodes of the track, which the view ends at
//! and shows the whole track by. Whole columns, and the same integer
//! arithmetic, make the decoder and the view decide alike where a track
//! ends and when a window covers it.

use super::column_hop::{column_hop, column_hops, pixels_per_column};
use super::usize_to_u64;

/// The view shows the whole track once its window covers this share of
/// it, in percent of its columns, rounded down, as Qt rounds it.
const WHOLE_TRACK_PERCENT: u64 = 90;

/// How long before the view's left edge a decode window starts, for the
/// STFT to warm up.
pub(super) const WARMUP_SECONDS: f64 = 2.0;

/// How many seconds a view of `width` pixels shows at `zoom`, of a
/// session of audio at `sample_rate`: its columns, as wide as the view
/// draws them ([`pixels_per_column`]), which is a pixel wherever the hop is
/// not the reference hop.
pub(super) fn window_seconds(width: u32, zoom: f32, sample_rate: u32) -> f64 {
    let sample_rate = sample_rate.max(1);
    let hop = column_hop(sample_rate, zoom);
    let pixels_per_column = pixels_per_column(sample_rate, hop, f64::from(zoom));
    // Hops are small; precision loss from usize→f64 is negligible.
    #[allow(clippy::cast_precision_loss)]
    let column_seconds = hop as f64 / f64::from(sample_rate);
    f64::from(width) / pixels_per_column.max(0.01) * column_seconds
}

/// The column after the last that a session decodes of a track of
/// `frames` frames, with `fft_size`-sample windows at `zoom`, of audio at
/// `sample_rate`: where the view of the track ends once the session has
/// decoded its end.
///
/// The STFT's windows are centred on their rows, the first on frame 0,
/// and a row needs its whole window, so the last half window has none of
/// its own. The rows peak-hold down to one a column; at the end of the
/// file a partial column is sent too. So the columns are the rows' frames
/// over the column hop, rounded up, whichever column the session started
/// at: their grid is the track's.
pub(super) fn decoded_end_column(frames: u64, fft_size: usize, zoom: f32, sample_rate: u32) -> u64 {
    let hops = column_hops(fft_size, zoom, sample_rate);
    let stft_hop = usize_to_u64(hops.stft_hop.max(1));
    let Some(beyond) = frames.checked_sub(usize_to_u64(fft_size / 2)) else {
        return 0;
    };
    let rows = beyond / stft_hop + 1;
    (rows * stft_hop).div_ceil(usize_to_u64(hops.effective_hop.max(1)))
}

/// A centered view of a session's columns, as the Qt view reckons it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct CenteredView {
    /// The session's columns a second.
    columns_per_second: f64,
    /// The columns the view shows, `visibleWindowCols` in Qt.
    window: u64,
    /// The column after the last that a session decodes of the track, if
    /// known: where the view ends, and what its window is a share of.
    end: Option<u64>,
}

impl CenteredView {
    /// A view `width` pixels wide at `zoom` of a session of audio at
    /// `sample_rate`, of a track whose columns end at `end` (see
    /// [`decoded_end_column`]), if known. The window is its width over the
    /// pixels a column takes, rounded up, as Qt rounds it.
    pub(super) fn new(width: u32, zoom: f32, sample_rate: u32, end: Option<u64>) -> Self {
        let sample_rate = sample_rate.max(1);
        let hop = column_hop(sample_rate, zoom);
        let pixels_per_column = pixels_per_column(sample_rate, hop, f64::from(zoom)).max(0.01);
        // Hops are small; precision loss from usize→f64 is negligible.
        #[allow(clippy::cast_precision_loss)]
        let columns_per_second = f64::from(sample_rate) / hop as f64;
        let window = super::f64_to_u64_saturating((f64::from(width) / pixels_per_column).ceil());
        Self::of_columns(columns_per_second, window, end)
    }

    /// A view of `window` columns of a session of `columns_per_second`, of a
    /// track whose columns end at `end`, if known.
    pub(super) fn of_columns(columns_per_second: f64, window: u64, end: Option<u64>) -> Self {
        Self {
            columns_per_second,
            window,
            end: end.filter(|&end| end > 0),
        }
    }

    /// The column the playhead at `position_seconds` is in, as Qt places it.
    pub(super) fn column_at(&self, position_seconds: f64) -> u64 {
        super::f64_to_u64_saturating((position_seconds.max(0.0) * self.columns_per_second).floor())
    }

    /// The seconds column `column` starts at.
    fn seconds_of(&self, column: u64) -> f64 {
        // Column counts are far below f64's exact-integer range.
        #[allow(clippy::cast_precision_loss)]
        let column = column as f64;
        column / self.columns_per_second
    }

    /// The columns the view shows.
    pub(super) fn window_columns(&self) -> u64 {
        self.window
    }

    /// The column at the left edge of the view with the playhead at
    /// `position_seconds`.
    ///
    /// The view centers the playhead, except that it shows the whole track
    /// when its window covers most of its columns, and it ends at the
    /// track's end: once the rest of the track is shorter than half a
    /// window, the left edge stays a window before the end.
    pub(super) fn left_column(&self, position_seconds: f64) -> u64 {
        let centered = self
            .column_at(position_seconds)
            .saturating_sub(self.window / 2);
        let Some(end) = self.end else {
            return centered;
        };
        if self.window == 0 {
            return centered;
        }
        if self.window.saturating_mul(100) / end >= WHOLE_TRACK_PERCENT {
            return 0;
        }
        centered.min(end.saturating_sub(self.window))
    }

    /// The left edge, in seconds, of the view with the playhead at
    /// `position_seconds`.
    pub(super) fn left_edge_seconds(&self, position_seconds: f64) -> f64 {
        self.seconds_of(self.left_column(position_seconds))
    }

    /// Where a decode window for a playhead at `position_seconds` starts,
    /// so that playback from there to the end of the track never shows
    /// columns that were not decoded on the left of the view.
    pub(super) fn decode_start_seconds(&self, position_seconds: f64) -> f64 {
        (self.left_edge_seconds(position_seconds) - WARMUP_SECONDS).max(0.0)
    }
}

#[cfg(test)]
mod tests {
    use super::{column_hop, decoded_end_column, window_seconds, CenteredView};

    /// A view of `window` seconds at 100 columns a second, of a track
    /// `duration` seconds long (0 if unknown).
    fn view_of_seconds(window: f64, duration: f64) -> CenteredView {
        let columns = |seconds: f64| super::super::f64_to_u64_saturating((seconds * 100.0).round());
        CenteredView::of_columns(100.0, columns(window), Some(columns(duration)))
    }

    #[test]
    fn decode_windows_start_before_the_view() {
        // Mid track, half a window before the playhead, and warm-up.
        let view = view_of_seconds(20.0, 400.0);
        assert!((view.decode_start_seconds(100.0) - 88.0).abs() < 1e-9);
        // A window before the end of the track, near it.
        assert!((view.decode_start_seconds(399.0) - 378.0).abs() < 1e-9);
        // An unknown length ends no view.
        let unknown = view_of_seconds(20.0, 0.0);
        assert!((unknown.decode_start_seconds(399.0) - 387.0).abs() < 1e-9);
        // Never before the start of the track.
        assert!(view.decode_start_seconds(5.0).abs() < 1e-9);
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
            assert_eq!(
                CenteredView::new(1920, 2.0, rate, None).window_columns(),
                1920
            );
        }
        // 8 kHz at 16x is floored at 16 samples a column, a pixel each, as
        // Qt draws them: 3440 columns of 2 ms.
        let seconds = window_seconds(3440, 16.0, 8_000);
        assert!(
            (seconds - 3440.0 * 16.0 / 8_000.0).abs() < 1e-9,
            "{seconds}"
        );
        // 32 kHz at 4x: 171 samples a column, a pixel each.
        let seconds = window_seconds(1920, 4.0, 32_000);
        assert!(
            (seconds - 1920.0 * 171.0 / 32_000.0).abs() < 1e-9,
            "{seconds}"
        );
        // The 44.1 kHz family, at 43.07 columns a second.
        for rate in [44_100, 88_200, 176_400] {
            let seconds = window_seconds(1920, 1.0, rate);
            assert!(
                (seconds - 1920.0 * 1024.0 / 44_100.0).abs() < 1e-9,
                "{rate}: {seconds}"
            );
        }
        // At the reference hop, a column is as wide as the zoom, and the
        // view shows the columns its pixels reach into: 1001 pixels at
        // 1.0004 are 1000.6 columns, which Qt rounds up.
        assert_eq!(column_hop(48_000, 1.0004), 1024);
        assert_eq!(
            CenteredView::new(1001, 1.0004, 48_000, None).window_columns(),
            1001
        );
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
            let actual = view_of_seconds(window, duration).left_edge_seconds(position);
            assert!((actual - left).abs() < 1e-9, "{line}: left edge {actual}");
            checked += 1;
        }
        assert!(checked >= 12, "only {checked} cases");
    }

    #[test]
    fn a_view_shows_the_whole_track_at_90_percent_of_its_columns_rounded_down() {
        // 1680 of 1866 columns is 90.03%, which Qt shows the whole track
        // at; 1679 is 89.98%, which it does not. In seconds, 1680 columns
        // at 8 kHz are 35.91 s, short of 90% of the 40 s file the 1866
        // columns are of: the decoder decides by the columns too.
        for (window, left) in [(1_679, 1_024 - 839), (1_680, 0), (1_681, 0)] {
            let view = CenteredView::of_columns(8_000.0 / 171.0, window, Some(1_866));
            assert_eq!(view.left_column(21.9), left, "{window}");
        }
        let view = CenteredView::new(1_680, 1.0, 8_000, Some(1_866));
        assert_eq!(view.window_columns(), 1_680);
        assert_eq!(view.left_column(21.9), 0);
        assert!(window_seconds(1_680, 1.0, 8_000) < 0.9 * 40.0);
    }

    #[test]
    fn an_unknown_duration_centers_the_playhead() {
        let view = CenteredView::of_columns(100.0, 1_000, None);
        assert!((view.left_edge_seconds(30.0) - 25.0).abs() < 1e-9);
        assert!(view.left_edge_seconds(3.0).abs() < 1e-9);
    }

    #[test]
    fn the_decoded_end_is_the_rows_frames_over_the_column_hop_rounded_up() {
        // 40 s at 8 kHz with 2048-sample windows, a row a column of 171
        // frames: the rows whose windows fit.
        assert_eq!(decoded_end_column(320_000, 2_048, 1.0, 8_000), 1_866);
        // At 44.1 kHz, four rows of 256 frames a column: 6887 rows end in
        // a partial column, which is sent too.
        assert_eq!(decoded_end_column(1_764_000, 2_048, 1.0, 44_100), 1_722);
        // With 6888 rows, the last column is whole.
        assert_eq!(decoded_end_column(1_764_096, 2_048, 1.0, 44_100), 1_722);
        assert_eq!(decoded_end_column(1_764_352, 2_048, 1.0, 44_100), 1_723);
        // Shorter than half a window: no row.
        assert_eq!(decoded_end_column(1_023, 2_048, 1.0, 44_100), 0);
        assert_eq!(decoded_end_column(1_024, 2_048, 1.0, 44_100), 1);
    }
}
