// SPDX-License-Identifier: GPL-3.0-or-later

//! The main window's layout. The UI sends it with
//! [`super::BridgeSettingsCommand::SetWindowLayout`], it is saved in the
//! settings file, and the UI reads it back with
//! [`super::ffi::ferrous_ffi_saved_window_layout`] before it shows the
//! window. The UI fits the geometry to the screens it finds then.

use std::fmt::Write as _;

/// The largest window extent kept, in logical pixels: larger than any
/// screen.
const MAX_EXTENT: u32 = 65_535;
/// The farthest a window's corner is kept from the origin of the desktop.
const MAX_COORDINATE: i32 = 1_000_000;
/// The share a pane may take of the space it shares with its neighbour.
const MIN_RATIO: f32 = 0.05;
const MAX_RATIO: f32 = 0.95;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WindowLayout {
    /// Where the window is when it is neither maximized nor fullscreen;
    /// `None` until a window was saved.
    pub geometry: Option<WindowGeometry>,
    pub maximized: bool,
    /// The library pane's width as a share of the window's width; `None`
    /// for the default.
    pub sidebar_width_ratio: Option<f32>,
    /// The queue's height as a share of the height it shares with the
    /// visualization; `None` for the default.
    pub queue_height_ratio: Option<f32>,
    /// 0 for the spectrogram, 1 for the waveform.
    pub visualization_mode: u8,
    /// Whether the queue keeps the current track in view.
    pub follow_current_track: bool,
}

impl Default for WindowLayout {
    fn default() -> Self {
        Self {
            geometry: None,
            maximized: false,
            sidebar_width_ratio: None,
            queue_height_ratio: None,
            visualization_mode: 0,
            follow_current_track: true,
        }
    }
}

/// A window's position and size in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowGeometry {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl WindowGeometry {
    /// The geometry, unless it has no area or lies farther out than any
    /// desktop.
    #[must_use]
    pub fn checked(x: i32, y: i32, width: u32, height: u32) -> Option<Self> {
        let extent = 1..=MAX_EXTENT;
        let coordinate = -MAX_COORDINATE..=MAX_COORDINATE;
        (extent.contains(&width)
            && extent.contains(&height)
            && coordinate.contains(&x)
            && coordinate.contains(&y))
        .then_some(Self {
            x,
            y,
            width,
            height,
        })
    }
}

/// A pane's share, kept between [`MIN_RATIO`] and [`MAX_RATIO`]; `None` for
/// a value that is not a number.
#[must_use]
pub fn checked_ratio(value: f32) -> Option<f32> {
    value.is_finite().then(|| value.clamp(MIN_RATIO, MAX_RATIO))
}

fn parse_flag(value: &str) -> Option<bool> {
    value.parse::<i32>().ok().map(|x| x != 0)
}

fn parse_geometry(value: &str) -> Option<WindowGeometry> {
    let mut parts = value.split(',').map(str::trim);
    let x = parts.next()?.parse().ok()?;
    let y = parts.next()?.parse().ok()?;
    let width = parts.next()?.parse().ok()?;
    let height = parts.next()?.parse().ok()?;
    if parts.next().is_some() {
        return None;
    }
    WindowGeometry::checked(x, y, width, height)
}

/// Apply the settings line `key=value` to `layout` if it is about the
/// window. Returns whether it is; a value that cannot be read is ignored.
pub(super) fn parse_setting(layout: &mut WindowLayout, key: &str, value: &str) -> bool {
    match key {
        "window_geometry" => {
            if let Some(geometry) = parse_geometry(value) {
                layout.geometry = Some(geometry);
            }
        }
        "window_maximized" => {
            if let Some(maximized) = parse_flag(value) {
                layout.maximized = maximized;
            }
        }
        "sidebar_width_ratio" => {
            if let Some(ratio) = value.parse().ok().and_then(checked_ratio) {
                layout.sidebar_width_ratio = Some(ratio);
            }
        }
        "queue_height_ratio" => {
            if let Some(ratio) = value.parse().ok().and_then(checked_ratio) {
                layout.queue_height_ratio = Some(ratio);
            }
        }
        "visualization_mode" => {
            if let Some(mode) = value.parse::<u8>().ok().filter(|mode| *mode <= 1) {
                layout.visualization_mode = mode;
            }
        }
        "follow_current_track" => {
            if let Some(follow) = parse_flag(value) {
                layout.follow_current_track = follow;
            }
        }
        _ => return false,
    }
    true
}

/// Append the settings lines of `layout` to `text`. Values left at their
/// defaults are left out, so the file keeps no geometry until one was
/// saved.
pub(super) fn format_settings(layout: &WindowLayout, text: &mut String) {
    if let Some(geometry) = layout.geometry {
        let _ = writeln!(
            text,
            "window_geometry={},{},{},{}",
            geometry.x, geometry.y, geometry.width, geometry.height
        );
    }
    let _ = writeln!(text, "window_maximized={}", i32::from(layout.maximized));
    if let Some(ratio) = layout.sidebar_width_ratio {
        let _ = writeln!(text, "sidebar_width_ratio={ratio:.4}");
    }
    if let Some(ratio) = layout.queue_height_ratio {
        let _ = writeln!(text, "queue_height_ratio={ratio:.4}");
    }
    let _ = writeln!(text, "visualization_mode={}", layout.visualization_mode);
    let _ = writeln!(
        text,
        "follow_current_track={}",
        i32::from(layout.follow_current_track)
    );
}

/// [`WindowLayout`] as the UI reads it; see `FerrousWindowLayout` in
/// `ui/src/FerrousBridgeFfi.h`, which must match it.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FerrousWindowLayout {
    pub has_geometry: bool,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub maximized: bool,
    /// Negative for the default.
    pub sidebar_width_ratio: f32,
    /// Negative for the default.
    pub queue_height_ratio: f32,
    pub visualization_mode: u8,
    pub follow_current_track: bool,
}

impl From<&WindowLayout> for FerrousWindowLayout {
    fn from(layout: &WindowLayout) -> Self {
        let geometry = layout.geometry.unwrap_or(WindowGeometry {
            x: 0,
            y: 0,
            width: 0,
            height: 0,
        });
        Self {
            has_geometry: layout.geometry.is_some(),
            x: geometry.x,
            y: geometry.y,
            width: geometry.width,
            height: geometry.height,
            maximized: layout.maximized,
            sidebar_width_ratio: layout.sidebar_width_ratio.unwrap_or(-1.0),
            queue_height_ratio: layout.queue_height_ratio.unwrap_or(-1.0),
            visualization_mode: layout.visualization_mode,
            follow_current_track: layout.follow_current_track,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(text: &str) -> WindowLayout {
        let mut layout = WindowLayout::default();
        for line in text.lines() {
            let (key, value) = line.split_once('=').expect("key=value");
            assert!(
                parse_setting(&mut layout, key.trim(), value.trim()),
                "{line}"
            );
        }
        layout
    }

    fn format(layout: &WindowLayout) -> String {
        let mut text = String::new();
        format_settings(layout, &mut text);
        text
    }

    #[test]
    fn a_layout_survives_the_settings_file() {
        let layout = WindowLayout {
            geometry: Some(WindowGeometry {
                x: -1200,
                y: 40,
                width: 1100,
                height: 700,
            }),
            maximized: true,
            sidebar_width_ratio: Some(0.31),
            queue_height_ratio: Some(0.5),
            visualization_mode: 1,
            follow_current_track: false,
        };
        assert_eq!(parse(&format(&layout)), layout);
        assert_eq!(
            parse(&format(&WindowLayout::default())),
            WindowLayout::default()
        );
        assert!(!format(&WindowLayout::default()).contains("window_geometry"));
    }

    #[test]
    fn values_that_cannot_be_read_keep_the_defaults() {
        let layout = parse(
            "window_geometry=10,20,0,700\n\
             window_maximized=yes\n\
             sidebar_width_ratio=NaN\n\
             queue_height_ratio=inf\n\
             visualization_mode=2\n\
             follow_current_track=\n",
        );
        assert_eq!(layout, WindowLayout::default());
        for geometry in [
            "1,2,3",
            "1,2,3,4,5",
            "a,2,3,4",
            "1,2,-3,4",
            "1,2,3,70000",
            "2000000,0,800,600",
        ] {
            assert_eq!(
                parse(&format!("window_geometry={geometry}")).geometry,
                None,
                "{geometry}"
            );
        }
        // Shares beyond what a pane may take are brought back in range.
        let layout = parse("sidebar_width_ratio=1.5\nqueue_height_ratio=-2\n");
        assert_eq!(layout.sidebar_width_ratio, Some(MAX_RATIO));
        assert_eq!(layout.queue_height_ratio, Some(MIN_RATIO));
    }

    #[test]
    fn other_settings_are_not_taken_for_the_layout() {
        let mut layout = WindowLayout::default();
        assert!(!parse_setting(&mut layout, "volume", "0.5"));
        assert!(!parse_setting(&mut layout, "window_unknown", "1"));
        assert_eq!(layout, WindowLayout::default());
    }

    #[test]
    fn the_ffi_layout_has_the_size_the_ui_expects() {
        // static_assert in ui/src/FerrousBridgeFfi.h.
        assert_eq!(std::mem::size_of::<FerrousWindowLayout>(), 36);
    }

    #[test]
    fn the_ffi_layout_marks_defaults() {
        let ffi = FerrousWindowLayout::from(&WindowLayout::default());
        assert!(!ffi.has_geometry);
        assert!(ffi.sidebar_width_ratio < 0.0 && ffi.queue_height_ratio < 0.0);
        assert!(ffi.follow_current_track);
    }
}
