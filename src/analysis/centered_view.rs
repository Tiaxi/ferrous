// SPDX-License-Identifier: GPL-3.0-or-later

//! Where the Qt spectrogram's centered view starts (`centeredDisplayRange` in
//! `ui/src/SpectrogramItem.cpp`), which decode windows must cover. Both are
//! tested against the cases in `fixtures/centered-view.txt`.

/// The view shows the whole track once its window covers this share of it.
const WHOLE_TRACK_SHARE: f64 = 0.9;

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

#[cfg(test)]
mod tests {
    use super::visible_left_edge_seconds;

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
