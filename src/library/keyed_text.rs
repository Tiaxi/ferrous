// SPDX-License-Identifier: GPL-3.0-or-later

//! Compact storage for short texts by key, such as a track's tags and its
//! search fields, which the library keeps for every track.
//!
//! The pairs sit in two allocations: their texts one after another in one
//! string, and one 8-byte entry per pair. A key the library and search
//! commonly use is stored as its number in [`KNOWN_KEYS`]; any other key is
//! stored in the text, before its value.

use std::fmt;

/// The keys the tag reader and the search index use, sorted.
const KNOWN_KEYS: [&str; 17] = [
    "album",
    "albumartist",
    "artist",
    "comment",
    "composer",
    "conductor",
    "date",
    "disc",
    "genre",
    "label",
    "lyrics",
    "path",
    "performer",
    "root",
    "title",
    "track",
    "year",
];

/// The key of an [`Entry`] that is not in [`KNOWN_KEYS`].
const OTHER_KEY: u8 = u8::MAX;

/// One pair of a [`KeyedText`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct Entry {
    /// The key's number in [`KNOWN_KEYS`], or [`OTHER_KEY`]: the text of
    /// the pair then starts with the key's `key_len` bytes.
    key: u8,
    key_len: u16,
    /// Where the pair's text ends; it starts where the previous one ends.
    end: u32,
}

/// The number of `key` in [`KNOWN_KEYS`]. Search looks fields up by key for
/// every hit, so this is a match rather than a search of the table.
fn known_key(key: &str) -> Option<u8> {
    Some(match key {
        "album" => 0,
        "albumartist" => 1,
        "artist" => 2,
        "comment" => 3,
        "composer" => 4,
        "conductor" => 5,
        "date" => 6,
        "disc" => 7,
        "genre" => 8,
        "label" => 9,
        "lyrics" => 10,
        "path" => 11,
        "performer" => 12,
        "root" => 13,
        "title" => 14,
        "track" => 15,
        "year" => 16,
        _ => return None,
    })
}

/// `(key, value)` pairs, sorted by key, with the values of a key in the
/// order they were given. Pairs with equal keys and values compare equal.
#[derive(Clone, Default, PartialEq, Eq, Hash)]
pub(crate) struct KeyedText {
    text: Box<str>,
    entries: Box<[Entry]>,
}

impl KeyedText {
    /// The pairs of `pairs`, sorted by key; pairs with equal keys keep their
    /// order. A pair is left out if the text would pass 4 GiB, or if its
    /// key is neither known nor shorter than 64 KiB.
    pub(crate) fn from_pairs<K: AsRef<str>, V: AsRef<str>>(mut pairs: Vec<(K, V)>) -> Self {
        pairs.sort_by(|a, b| a.0.as_ref().cmp(b.0.as_ref()));
        let text_len: usize = pairs
            .iter()
            .map(|(key, value)| {
                let key = key.as_ref();
                value.as_ref().len() + known_key(key).map_or(key.len(), |_| 0)
            })
            .sum();
        let mut text = String::with_capacity(text_len);
        let mut entries = Vec::with_capacity(pairs.len());
        for (key, value) in &pairs {
            let (key, value) = (key.as_ref(), value.as_ref());
            let (code, key_text) = match known_key(key) {
                Some(code) => (code, ""),
                None => (OTHER_KEY, key),
            };
            let Ok(key_len) = u16::try_from(key_text.len()) else {
                continue;
            };
            let Ok(end) = u32::try_from(text.len() + key_text.len() + value.len()) else {
                continue;
            };
            text.push_str(key_text);
            text.push_str(value);
            entries.push(Entry {
                key: code,
                key_len,
                end,
            });
        }
        Self {
            text: text.into_boxed_str(),
            entries: entries.into_boxed_slice(),
        }
    }

    /// How many pairs there are.
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The key and value of pair `index`, which is less than [`Self::len`].
    pub(crate) fn pair(&self, index: usize) -> (&str, &str) {
        let entry = self.entries[index];
        let start = index
            .checked_sub(1)
            .map_or(0, |previous| self.entries[previous].end as usize);
        let end = entry.end as usize;
        if entry.key == OTHER_KEY {
            let value_start = start + usize::from(entry.key_len);
            (&self.text[start..value_start], &self.text[value_start..end])
        } else {
            (KNOWN_KEYS[usize::from(entry.key)], &self.text[start..end])
        }
    }

    /// Every pair, in order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = (&str, &str)> + '_ {
        (0..self.len()).map(|index| self.pair(index))
    }

    /// The value of the first pair with `key`.
    pub(crate) fn get(&self, key: &str) -> Option<&str> {
        let code = known_key(key);
        (0..self.len())
            .find(|&index| {
                let entry = self.entries[index];
                match code {
                    Some(code) => entry.key == code,
                    None => entry.key == OTHER_KEY && self.pair(index).0 == key,
                }
            })
            .map(|index| self.pair(index).1)
    }

    /// The pairs grouped by key: each key once, in order, with the range of
    /// its pairs.
    pub(crate) fn groups(&self) -> impl Iterator<Item = (&str, std::ops::Range<usize>)> + '_ {
        let mut start = 0;
        std::iter::from_fn(move || {
            if start >= self.len() {
                return None;
            }
            let key = self.pair(start).0;
            let end = (start + 1..self.len())
                .find(|&index| self.pair(index).0 != key)
                .unwrap_or(self.len());
            let range = start..end;
            start = end;
            Some((key, range))
        })
    }

    /// The heap bytes the pairs take: their text and entries.
    #[cfg(test)]
    pub(crate) fn heap_bytes(&self) -> usize {
        self.text.len() + std::mem::size_of_val::<[Entry]>(&self.entries)
    }
}

impl fmt::Debug for KeyedText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_keys_are_sorted_and_fit_a_byte() {
        assert!(KNOWN_KEYS.windows(2).all(|pair| pair[0] < pair[1]));
        for (index, key) in KNOWN_KEYS.iter().enumerate() {
            assert_eq!(known_key(key).map(usize::from), Some(index), "{key}");
        }
        assert_eq!(known_key("Title"), None);
        assert!(KNOWN_KEYS.len() < usize::from(OTHER_KEY));
        assert_eq!(std::mem::size_of::<Entry>(), 8);
    }

    #[test]
    fn keeps_pairs_sorted_by_key_with_values_in_order_and_unknown_keys() {
        let text = KeyedText::from_pairs(vec![
            ("title", "Second"),
            ("mood", "calm"),
            ("artist", "B"),
            ("title", "First"),
            ("", "empty key"),
            ("comment", ""),
            ("Album", "upper case is not a known key"),
            ("title", "Third"),
        ]);
        assert_eq!(
            text.iter().collect::<Vec<_>>(),
            [
                ("", "empty key"),
                ("Album", "upper case is not a known key"),
                ("artist", "B"),
                ("comment", ""),
                ("mood", "calm"),
                ("title", "Second"),
                ("title", "First"),
                ("title", "Third"),
            ]
        );
        assert_eq!(text.get("title"), Some("Second"));
        assert_eq!(text.get("mood"), Some("calm"));
        assert_eq!(text.get(""), Some("empty key"));
        assert_eq!(text.get("comment"), Some(""));
        assert_eq!(text.get("album"), None);
        assert_eq!(text.get("year"), None);
        assert_eq!(
            text.groups()
                .map(|(key, range)| (key, range.len()))
                .collect::<Vec<_>>(),
            [
                ("", 1),
                ("Album", 1),
                ("artist", 1),
                ("comment", 1),
                ("mood", 1),
                ("title", 3)
            ]
        );
        // Known keys take no text; others are stored before their value.
        let texts = [
            "empty key",
            "Albumupper case is not a known key",
            "B",
            "",
            "moodcalm",
            "SecondFirstThird",
        ];
        assert_eq!(text.heap_bytes(), 8 * 8 + texts.concat().len());
    }

    #[test]
    fn equal_pairs_compare_equal_whatever_their_order_of_keys() {
        let a = KeyedText::from_pairs(vec![("genre", "x"), ("mood", "y"), ("genre", "z")]);
        let b = KeyedText::from_pairs(vec![("mood", "y"), ("genre", "x"), ("genre", "z")]);
        let c = KeyedText::from_pairs(vec![("mood", "y"), ("genre", "z"), ("genre", "x")]);
        assert_eq!(a, b);
        assert_ne!(a, c);
        assert_ne!(
            KeyedText::from_pairs(vec![("moo", "dy")]),
            KeyedText::from_pairs(vec![("mood", "y")])
        );
        assert!(KeyedText::from_pairs(Vec::<(&str, &str)>::new()).is_empty());
        assert_eq!(
            KeyedText::from_pairs(Vec::<(&str, &str)>::new()),
            KeyedText::default()
        );
    }

    #[test]
    fn leaves_out_a_key_too_long_to_store() {
        let long = "k".repeat(usize::from(u16::MAX) + 1);
        let text = KeyedText::from_pairs(vec![(long.as_str(), "v"), ("title", "kept")]);
        assert_eq!(text.iter().collect::<Vec<_>>(), [("title", "kept")]);
    }
}
