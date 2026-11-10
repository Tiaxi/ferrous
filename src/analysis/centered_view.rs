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

use super::column_hop::{
    column_hop, column_hops, effective_zoom, pixels_per_column, reference_hop, render_zoom,
    MAX_ZOOM,
};
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
#[must_use]
pub fn decoded_end_column(frames: u64, fft_size: usize, zoom: f32, sample_rate: u32) -> u64 {
    let hops = column_hops(fft_size, zoom, sample_rate);
    let stft_hop = usize_to_u64(hops.stft_hop.max(1));
    let Some(beyond) = frames.checked_sub(usize_to_u64(fft_size / 2)) else {
        return 0;
    };
    let rows = beyond / stft_hop + 1;
    (rows * stft_hop).div_ceil(usize_to_u64(hops.effective_hop.max(1)))
}

/// The zoom whose columns of a track of `frames` frames, with
/// `fft_size`-sample windows, of audio at `sample_rate`, fill a view `width`
/// pixels wide: the deepest zoom, up to [`MAX_ZOOM`], at which all the
/// columns a session decodes of the track ([`decoded_end_column`]) fit in
/// the view. It is the spectrogram's default zoom for a track shorter than
/// the view when the user has it stretch such tracks.
///
/// 1 when the track's columns at zoom 1 fill the view already, when it has
/// no columns at all, and when its length, the rate or the width is unknown
/// (0). Deeper zooms draw a column a pixel ([`pixels_per_column`]), and
/// hops are whole samples, so the columns fill the view to within the
/// columns one more sample a hop takes off; a track too short to fill it at
/// [`MAX_ZOOM`] gets that zoom.
#[must_use]
pub fn fit_zoom(frames: u64, fft_size: usize, sample_rate: u32, width: u32) -> f32 {
    if frames == 0 || fft_size == 0 || sample_rate == 0 || width == 0 {
        return 1.0;
    }
    let width = u64::from(width);
    let at_default = decoded_end_column(frames, fft_size, 1.0, sample_rate);
    if at_default == 0 || at_default >= width {
        return 1.0;
    }
    let reference = reference_hop(sample_rate);
    let deepest = column_hop(sample_rate, MAX_ZOOM);
    // Hops are at most a few thousand samples, and zooms between 1 and
    // MAX_ZOOM, which f32 holds as the backend does.
    #[allow(clippy::cast_precision_loss, clippy::cast_possible_truncation)]
    let zoom_of = |hop: usize| (reference as f64 / hop as f64) as f32;
    let fits =
        |hop: usize| decoded_end_column(frames, fft_size, zoom_of(hop), sample_rate) <= width;
    // The columns are about the frames past the first half window over the
    // hop: start there, and step to the shortest hop whose columns fit.
    let beyond = frames.saturating_sub(usize_to_u64(fft_size / 2));
    let estimate = usize::try_from(beyond / width).unwrap_or(usize::MAX);
    let mut hop = estimate.clamp(deepest, reference);
    while hop < reference && !fits(hop) {
        hop += 1;
    }
    while hop > deepest && fits(hop - 1) {
        hop -= 1;
    }
    if hop >= reference {
        return 1.0;
    }
    zoom_of(hop).min(MAX_ZOOM)
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
    /// zoom Qt draws the session's columns at ([`effective_zoom`] of
    /// [`render_zoom`]), rounded up, as `centeredViewLocked` computes it
    /// at a device pixel ratio of 1. Qt draws data at the zoom the session
    /// decodes at, which is `zoom`.
    pub(super) fn new(width: u32, zoom: f32, sample_rate: u32, end: Option<u64>) -> Self {
        let sample_rate = sample_rate.max(1);
        let hop = column_hop(sample_rate, zoom);
        let zoom = effective_zoom(
            sample_rate,
            hop,
            render_zoom(sample_rate, hop, f64::from(zoom)),
        );
        // Hops are small; precision loss from usize→f64 is negligible.
        #[allow(clippy::cast_precision_loss)]
        let columns_per_second = f64::from(sample_rate) / hop as f64;
        let window = super::f64_to_u64_saturating((f64::from(width) / zoom.max(0.01)).ceil());
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
    use super::{column_hop, decoded_end_column, fit_zoom, window_seconds, CenteredView};
    use crate::analysis::column_hop::{reference_hop, MAX_ZOOM};

    /// The columns a session at `zoom` decodes of a track.
    fn columns_at(frames: u64, fft_size: usize, zoom: f32, rate: u32) -> u64 {
        decoded_end_column(frames, fft_size, zoom, rate)
    }

    #[test]
    fn a_short_track_fills_the_view_at_its_fit_zoom() {
        for (rate, fft_size, width) in [
            (48_000, 8192, 1_000),
            (44_100, 8192, 1_000),
            (48_000, 2048, 1_337),
            (96_000, 8192, 1_920),
            (44_100, 512, 640),
        ] {
            let frames = u64::from(rate); // one second
            let zoom = fit_zoom(frames, fft_size, rate, width);
            assert!(zoom > 1.0, "{rate} Hz, {width} px: {zoom}");
            let columns = columns_at(frames, fft_size, zoom, rate);
            assert!(
                columns <= u64::from(width),
                "{rate} Hz: {columns} > {width}"
            );
            // A column a sample longer would leave more of the view empty:
            // the hop is the shortest whose columns fit.
            let hop = column_hop(rate, zoom);
            assert!(hop < reference_hop(rate));
            #[allow(clippy::cast_precision_loss, clippy::cast_possible_truncation)]
            let finer = (reference_hop(rate) as f64 / (hop - 1) as f64) as f32;
            assert!(
                zoom >= MAX_ZOOM || columns_at(frames, fft_size, finer, rate) > u64::from(width),
                "{rate} Hz: a hop of {} samples fits too",
                hop - 1
            );
        }
        // 1 s at 48 kHz in 1000 px: 44 samples a column, 998 columns.
        let zoom = fit_zoom(48_000, 8192, 48_000, 1_000);
        assert_eq!(column_hop(48_000, zoom), 44);
        assert_eq!(columns_at(48_000, 8192, zoom, 48_000), 998);
    }

    #[test]
    fn a_track_that_fills_the_view_or_has_no_known_length_keeps_zoom_1() {
        // 30 s at zoom 1 is 1406 columns, wider than 1000 px.
        assert!((fit_zoom(30 * 48_000, 8192, 48_000, 1_000) - 1.0).abs() < f32::EPSILON);
        // Exactly as wide as the view.
        let frames = 1_000 * 1_024 + 4_096;
        assert!(columns_at(frames, 8192, 1.0, 48_000) >= 1_000);
        assert!((fit_zoom(frames, 8192, 48_000, 1_000) - 1.0).abs() < f32::EPSILON);
        for (frames, fft_size, rate, width) in [
            (0, 8192, 48_000, 1_000),
            (48_000, 0, 48_000, 1_000),
            (48_000, 8192, 0, 1_000),
            (48_000, 8192, 48_000, 0),
            // Shorter than half a window: no columns.
            (4_000, 8192, 48_000, 1_000),
        ] {
            assert!(
                (fit_zoom(frames, fft_size, rate, width) - 1.0).abs() < f32::EPSILON,
                "{frames} frames"
            );
        }
    }

    #[test]
    fn a_track_too_short_to_fill_the_view_gets_the_deepest_zoom() {
        // A fifth of a second at 48 kHz is under 600 columns a 16 samples.
        let zoom = fit_zoom(9_600, 2048, 48_000, 3_840);
        assert!((zoom - MAX_ZOOM).abs() < f32::EPSILON);
        assert_eq!(column_hop(48_000, zoom), 16);
        // 8 kHz cannot go below its floor of 16 samples a column.
        let zoom = fit_zoom(8_000, 512, 8_000, 3_840);
        assert_eq!(column_hop(8_000, zoom), 16);
        assert!(zoom < MAX_ZOOM);
    }

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
    fn a_window_has_the_columns_qt_computes_for_it() {
        // 8 kHz at zoom 0.57, 300 samples a column: Qt draws them at
        // 171 / 300 × 300 / 171, a bit below 1, and a view 1680 pixels wide
        // shows 1681 columns. Of the 1867 a 560,640-frame file decodes to,
        // that is 90.04%: the view shows the whole track at 38.4 s, which
        // 1680 columns, 89.98%, would not.
        let end = decoded_end_column(560_640, 2_048, 0.57, 8_000);
        assert_eq!(end, 1_867);
        let view = CenteredView::new(1_680, 0.57, 8_000, Some(end));
        assert_eq!(view.window_columns(), 1_681);
        assert_eq!(view.left_column(38.4), 0);
        let narrower = CenteredView::new(1_679, 0.57, 8_000, Some(end));
        assert_eq!(narrower.window_columns(), 1_680);
        assert_eq!(narrower.left_column(38.4), 1_024 - 840);
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

    #[test]
    fn the_decoded_end_needs_the_sessions_zoom_not_only_its_hop() {
        // At the reference hop, a zoom just above 1 runs the STFT a column
        // apart and zoom 1 an eighth of a window apart, so a session's end
        // is not the hop's: chunks carry it rather than leave the UI to
        // reckon it from the hop (issue 436).
        // 60 s at 32 kHz: columns of 683 frames, rows of 256 at zoom 1.
        assert_eq!(column_hop(32_000, 1.0), 683);
        assert_eq!(column_hop(32_000, 1.0003), 683);
        assert_eq!(decoded_end_column(1_920_000, 2_048, 1.0, 32_000), 2_811);
        assert_eq!(decoded_end_column(1_920_000, 2_048, 1.0003, 32_000), 2_810);
    }
}
