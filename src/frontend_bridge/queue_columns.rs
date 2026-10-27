// SPDX-License-Identifier: GPL-3.0-or-later

//! The widths of the queue's columns that the user resized. The UI sends
//! them with [`super::BridgeSettingsCommand::SetQueueColumnWidths`] when a
//! drag ends, they are saved in the settings file as
//! `queue_column_widths=artist:170,album:190,length:76`, and the bridge
//! sends them back in the settings and, at startup, with the window layout,
//! so the queue opens with them. A column left out keeps the UI's default,
//! which follows the font size.

use std::fmt::Write as _;

/// The settings key.
const KEY: &str = "queue_column_widths";
/// The narrowest and widest column kept, in logical pixels. The UI keeps
/// its columns within narrower, font-relative bounds.
pub const MIN_WIDTH: u16 = 16;
pub const MAX_WIDTH: u16 = 4096;

/// The columns' widths in logical pixels; `None` for the default.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct QueueColumnWidths {
    pub artist: Option<u16>,
    pub album: Option<u16>,
    pub length: Option<u16>,
}

impl QueueColumnWidths {
    /// The columns this build knows, by their names in the settings file.
    fn columns(&self) -> [(&'static str, Option<u16>); 3] {
        [
            ("artist", self.artist),
            ("album", self.album),
            ("length", self.length),
        ]
    }

    fn column_mut(&mut self, name: &str) -> Option<&mut Option<u16>> {
        match name {
            "artist" => Some(&mut self.artist),
            "album" => Some(&mut self.album),
            "length" => Some(&mut self.length),
            _ => None,
        }
    }
}

/// A width the user may keep; `None` for 0, which stands for the default,
/// and for widths out of range.
#[must_use]
pub fn checked_width(width: u16) -> Option<u16> {
    (MIN_WIDTH..=MAX_WIDTH).contains(&width).then_some(width)
}

/// The widths as the settings file keeps them, with the entries of columns
/// this build does not know.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueColumnSettings {
    pub widths: QueueColumnWidths,
    /// `name:width` entries of columns this build does not know, such as a
    /// newer build's, kept for it.
    pub other_entries: Vec<String>,
}

/// Whether `entry` is `name:width` with a name and a width that could be a
/// column's, so it is worth keeping for the build that wrote it.
fn is_column_entry(name: &str, width: &str) -> bool {
    !name.is_empty()
        && name
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_')
        && !width.is_empty()
        && width.bytes().all(|byte| byte.is_ascii_digit())
}

fn parse_value(value: &str) -> QueueColumnSettings {
    let mut settings = QueueColumnSettings::default();
    for entry in value.split(',').map(str::trim) {
        let Some((name, width)) = entry.split_once(':') else {
            continue;
        };
        let (name, width) = (name.trim(), width.trim());
        if let Some(column) = settings.widths.column_mut(name) {
            if let Some(width) = width.parse().ok().and_then(checked_width) {
                *column = Some(width);
            }
        } else if is_column_entry(name, width) {
            let entry = format!("{name}:{width}");
            settings
                .other_entries
                .retain(|kept| kept.split_once(':').map(|(kept, _)| kept) != Some(name));
            settings.other_entries.push(entry);
        }
    }
    settings
}

/// Apply the settings line `key=value` to `settings` if it is about the
/// queue's columns. Returns whether it is. Entries that cannot be read, and
/// widths out of range, are ignored.
pub(super) fn parse_setting(settings: &mut QueueColumnSettings, key: &str, value: &str) -> bool {
    if key != KEY {
        return false;
    }
    *settings = parse_value(value);
    true
}

/// Append the settings line of `settings` to `text`, with only the columns
/// the user resized; none if there are none.
pub(super) fn format_settings(settings: &QueueColumnSettings, text: &mut String) {
    let mut entries: Vec<String> = settings
        .widths
        .columns()
        .into_iter()
        .filter_map(|(name, width)| width.map(|width| format!("{name}:{width}")))
        .collect();
    entries.extend(settings.other_entries.iter().cloned());
    if !entries.is_empty() {
        let _ = writeln!(text, "{KEY}={}", entries.join(","));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(value: &str) -> QueueColumnSettings {
        let mut settings = QueueColumnSettings::default();
        assert!(parse_setting(&mut settings, KEY, value));
        settings
    }

    fn format(settings: &QueueColumnSettings) -> String {
        let mut text = String::new();
        format_settings(settings, &mut text);
        text
    }

    #[test]
    fn resized_columns_survive_the_settings_file() {
        let settings = QueueColumnSettings {
            widths: QueueColumnWidths {
                artist: Some(170),
                album: Some(190),
                length: Some(76),
            },
            other_entries: Vec::new(),
        };
        let text = format(&settings);
        assert_eq!(text, "queue_column_widths=artist:170,album:190,length:76\n");
        let (_, value) = text.trim_end().split_once('=').expect("key=value");
        assert_eq!(parse(value), settings);
    }

    #[test]
    fn only_resized_columns_are_saved() {
        assert_eq!(format(&QueueColumnSettings::default()), "");
        let settings = QueueColumnSettings {
            widths: QueueColumnWidths {
                album: Some(240),
                ..QueueColumnWidths::default()
            },
            other_entries: Vec::new(),
        };
        assert_eq!(format(&settings), "queue_column_widths=album:240\n");
    }

    #[test]
    fn widths_out_of_range_and_entries_that_cannot_be_read_are_ignored() {
        let settings =
            parse("artist:0, album:70000,length:-5,artist,:12,album:wide,length:4097,artist:15");
        assert_eq!(settings, QueueColumnSettings::default());
        let settings = parse("artist:16,length:4096");
        assert_eq!(settings.widths.artist, Some(16));
        assert_eq!(settings.widths.length, Some(4096));
        assert_eq!(settings.widths.album, None);
        // The last entry of a column wins.
        assert_eq!(parse("album:100,album:120").widths.album, Some(120));
    }

    #[test]
    fn columns_this_build_does_not_know_are_kept_but_not_used() {
        let settings = parse("genre:120,artist:200,Year:x,comment:80,genre:130");
        assert_eq!(
            settings.widths,
            QueueColumnWidths {
                artist: Some(200),
                ..QueueColumnWidths::default()
            }
        );
        assert_eq!(settings.other_entries, vec!["comment:80", "genre:130"]);
        assert_eq!(
            format(&settings),
            "queue_column_widths=artist:200,comment:80,genre:130\n"
        );
    }

    #[test]
    fn other_settings_are_not_taken_for_the_columns() {
        let mut settings = QueueColumnSettings::default();
        assert!(!parse_setting(&mut settings, "queue_height_ratio", "0.5"));
        assert_eq!(settings, QueueColumnSettings::default());
    }
}
