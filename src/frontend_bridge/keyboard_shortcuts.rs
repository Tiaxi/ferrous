// SPDX-License-Identifier: GPL-3.0-or-later

//! The keyboard shortcuts the user changed. The UI sends the whole set with
//! [`super::BridgeSettingsCommand::SetKeyboardShortcuts`], the settings file
//! keeps one line for each changed action, as
//! `shortcut.play_pause=Media Play; Space`, and the bridge sends the set back
//! in the settings section. An action without a line has its default
//! shortcuts, which are never written; an empty value is an action the user
//! left without one.
//!
//! The ids are the UI's, and the values are its key sequences as Qt writes
//! them (`QKeySequence::listToString` in portable text), which the bridge
//! keeps as they are: it does not need to know the actions or read the
//! keys, so the lines of actions this build does not have, such as a newer
//! build's, are kept for it.

use std::collections::BTreeMap;
use std::fmt::Write as _;

/// The start of each settings key; the action's id follows it.
const KEY_PREFIX: &str = "shortcut.";
/// The longest action id, in bytes.
pub const MAX_ID_BYTES: usize = 64;
/// The longest list of key sequences of one action, in bytes.
pub const MAX_SEQUENCES_BYTES: usize = 256;
/// The most actions with changed shortcuts kept.
pub const MAX_ENTRIES: usize = 512;

/// Whether `id` can name an action: 1 to [`MAX_ID_BYTES`] of `a`–`z`,
/// `0`–`9` and `_`.
#[must_use]
pub fn is_valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= MAX_ID_BYTES
        && id
            .bytes()
            .all(|byte| byte.is_ascii_lowercase() || byte.is_ascii_digit() || byte == b'_')
}

/// Whether `sequences` can be an action's shortcuts as the settings file
/// keeps them: at most [`MAX_SEQUENCES_BYTES`], on one line, and without
/// spaces at either end, which the file would lose. Empty is no shortcut.
#[must_use]
pub fn is_valid_sequences(sequences: &str) -> bool {
    sequences.len() <= MAX_SEQUENCES_BYTES
        && !sequences.chars().any(char::is_control)
        && sequences.trim() == sequences
}

/// The changed shortcuts by action id, in the order of their ids.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct KeyboardShortcuts {
    entries: BTreeMap<String, String>,
}

impl KeyboardShortcuts {
    /// The entries that are valid, the last of an id winning, up to
    /// [`MAX_ENTRIES`] ids; `None` if any is not valid or there are more.
    pub fn from_entries<I>(entries: I) -> Option<Self>
    where
        I: IntoIterator<Item = (String, String)>,
    {
        let mut shortcuts = Self::default();
        for (id, sequences) in entries {
            if !shortcuts.insert(id, sequences) {
                return None;
            }
        }
        Some(shortcuts)
    }

    /// Keep `sequences` for `id`. Returns false, keeping nothing, for an
    /// entry that is not valid or a new id past [`MAX_ENTRIES`].
    fn insert(&mut self, id: String, sequences: String) -> bool {
        if !is_valid_id(&id)
            || !is_valid_sequences(&sequences)
            || (self.entries.len() >= MAX_ENTRIES && !self.entries.contains_key(&id))
        {
            return false;
        }
        self.entries.insert(id, sequences);
        true
    }

    /// The entries, by id.
    #[must_use]
    pub fn iter(&self) -> impl ExactSizeIterator<Item = (&str, &str)> {
        self.entries
            .iter()
            .map(|(id, sequences)| (id.as_str(), sequences.as_str()))
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    #[cfg(test)]
    pub(super) fn get(&self, id: &str) -> Option<&str> {
        self.entries.get(id).map(String::as_str)
    }
}

/// Apply the settings line `key=value` to `shortcuts` if it is about an
/// action's shortcuts. Returns whether it is. A line whose id or value is
/// not valid, or past [`MAX_ENTRIES`], is dropped.
pub(super) fn parse_setting(shortcuts: &mut KeyboardShortcuts, key: &str, value: &str) -> bool {
    let Some(id) = key.strip_prefix(KEY_PREFIX) else {
        return false;
    };
    shortcuts.insert(id.to_string(), value.to_string());
    true
}

/// Append a settings line for each action of `shortcuts` to `text`.
pub(super) fn format_settings(shortcuts: &KeyboardShortcuts, text: &mut String) {
    for (id, sequences) in shortcuts.iter() {
        let _ = writeln!(text, "{KEY_PREFIX}{id}={sequences}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(text: &str) -> KeyboardShortcuts {
        let mut shortcuts = KeyboardShortcuts::default();
        for line in text.lines() {
            let (key, value) = line.split_once('=').expect("key=value");
            assert!(
                parse_setting(&mut shortcuts, key.trim(), value.trim()),
                "{line}"
            );
        }
        shortcuts
    }

    fn format(shortcuts: &KeyboardShortcuts) -> String {
        let mut text = String::new();
        format_settings(shortcuts, &mut text);
        text
    }

    #[test]
    fn changed_shortcuts_survive_the_settings_file_in_the_order_of_their_ids() {
        let shortcuts = KeyboardShortcuts::from_entries([
            ("play_pause".to_string(), "Media Play; Space".to_string()),
            ("find".to_string(), "Ctrl+K".to_string()),
            // Cleared: no shortcut at all, not the default.
            ("stop".to_string(), String::new()),
            // A key of its own; only the first '=' ends the key.
            ("volume_up".to_string(), "Ctrl+=".to_string()),
        ])
        .expect("valid");
        let text = format(&shortcuts);
        assert_eq!(
            text,
            "shortcut.find=Ctrl+K\nshortcut.play_pause=Media Play; Space\nshortcut.stop=\n\
             shortcut.volume_up=Ctrl+=\n"
        );
        let parsed = parse(&text);
        assert_eq!(parsed, shortcuts);
        assert_eq!(parsed.get("stop"), Some(""));
        assert_eq!(parsed.get("open_files"), None);
        assert_eq!(format(&KeyboardShortcuts::default()), "");
    }

    #[test]
    fn ids_and_sequences_out_of_bounds_are_dropped() {
        let long_id = "a".repeat(MAX_ID_BYTES + 1);
        let long_sequences = "A".repeat(MAX_SEQUENCES_BYTES + 1);
        let shortcuts = parse(&format!(
            "shortcut.=Ctrl+A\nshortcut.Play=Ctrl+B\nshortcut.play-pause=Ctrl+C\n\
             shortcut.{long_id}=Ctrl+D\nshortcut.stop={long_sequences}\nshortcut.next=Ctrl+\u{7}\n\
             shortcut.{}=Ctrl+E\nshortcut.previous={}\n",
            "b".repeat(MAX_ID_BYTES),
            "B".repeat(MAX_SEQUENCES_BYTES),
        ));
        assert_eq!(
            shortcuts.iter().map(|(id, _)| id).collect::<Vec<_>>(),
            vec!["b".repeat(MAX_ID_BYTES).as_str(), "previous"]
        );
        assert!(!is_valid_sequences(" Space"));
        assert!(!is_valid_sequences("Space\n"));
        assert!(is_valid_sequences(""));
        assert!(KeyboardShortcuts::from_entries([("Play".to_string(), String::new())]).is_none());
    }

    #[test]
    fn no_more_than_the_most_entries_are_kept_and_the_last_of_an_id_wins() {
        let mut text = String::new();
        for index in 0..=MAX_ENTRIES {
            let _ = writeln!(text, "shortcut.action_{index:04}=Ctrl+{index}");
        }
        text.push_str("shortcut.action_0000=Ctrl+Z\n");
        let shortcuts = parse(&text);
        assert_eq!(shortcuts.len(), MAX_ENTRIES);
        assert_eq!(shortcuts.get("action_0000"), Some("Ctrl+Z"));
        assert_eq!(shortcuts.get(&format!("action_{MAX_ENTRIES:04}")), None);
        let entries = (0..=MAX_ENTRIES).map(|index| (format!("a{index}"), String::new()));
        assert!(KeyboardShortcuts::from_entries(entries).is_none());
    }

    #[test]
    fn other_settings_are_not_taken_for_shortcuts() {
        let mut shortcuts = KeyboardShortcuts::default();
        assert!(!parse_setting(&mut shortcuts, "shortcuts", "x"));
        assert!(!parse_setting(&mut shortcuts, "volume", "0.5"));
        assert!(shortcuts.is_empty());
    }
}
