// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;
use std::fmt;

use serde::ser::SerializeMap;
use serde::{Deserialize, Deserializer, Serialize, Serializer};

use super::keyed_text::KeyedText;

/// Additional, potentially repeated text tags collected during library
/// scans: the values of each key, in the order they were read. Every key
/// has at least one value.
///
/// The library keeps them for every track, so they are stored compactly
/// (see [`KeyedText`]). They are stored in the database as the JSON of a
/// map from key to values.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct SearchTags(KeyedText);

impl SearchTags {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Each key once, sorted, with its values in order.
    pub fn iter(&self) -> impl Iterator<Item = (&str, impl Iterator<Item = &str> + '_)> + '_ {
        self.0
            .groups()
            .map(|(key, range)| (key, range.map(|index| self.0.pair(index).1)))
    }

    /// The values of `key`, in order.
    pub fn values<'a>(&'a self, key: &'a str) -> impl Iterator<Item = &'a str> + 'a {
        self.0
            .iter()
            .filter(move |(other, _)| *other == key)
            .map(|(_, value)| value)
    }

    /// Gives `key` the values `values`, in place of the ones it had.
    pub fn insert(&mut self, key: &str, values: impl IntoIterator<Item = impl AsRef<str>>) {
        let values: Vec<_> = values.into_iter().collect();
        let pairs: Vec<(&str, &str)> = self
            .0
            .iter()
            .filter(|(other, _)| *other != key)
            .chain(values.iter().map(|value| (key, value.as_ref())))
            .collect();
        self.0 = KeyedText::from_pairs(pairs);
    }

    /// The heap bytes the tags take.
    #[cfg(test)]
    pub(crate) fn heap_bytes(&self) -> usize {
        self.0.heap_bytes()
    }
}

impl From<BTreeMap<String, Vec<String>>> for SearchTags {
    fn from(map: BTreeMap<String, Vec<String>>) -> Self {
        Self(KeyedText::from_pairs(
            map.iter()
                .flat_map(|(key, values)| values.iter().map(move |value| (key, value)))
                .collect(),
        ))
    }
}

impl<const N: usize> From<[(String, Vec<String>); N]> for SearchTags {
    /// The tags of `pairs`; of pairs with the same key, the last is kept.
    fn from(pairs: [(String, Vec<String>); N]) -> Self {
        BTreeMap::from(pairs).into()
    }
}

impl fmt::Debug for SearchTags {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_map()
            .entries(
                self.iter()
                    .map(|(key, values)| (key, values.collect::<Vec<_>>())),
            )
            .finish()
    }
}

impl Serialize for SearchTags {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(None)?;
        for (key, values) in self.iter() {
            map.serialize_entry(key, &values.collect::<Vec<_>>())?;
        }
        map.end()
    }
}

impl<'de> Deserialize<'de> for SearchTags {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        BTreeMap::<String, Vec<String>>::deserialize(deserializer).map(Self::from)
    }
}

/// Collects the tags of a file as it is read.
#[derive(Debug, Default)]
pub(crate) struct SearchTagsBuilder {
    pairs: Vec<(&'static str, String)>,
}

impl SearchTagsBuilder {
    /// Adds `value`, trimmed, to the values of `key`, unless it is empty or
    /// `key` has it already.
    pub(crate) fn add(&mut self, key: &'static str, value: &str) {
        let value = value.trim();
        if value.is_empty()
            || self
                .pairs
                .iter()
                .any(|(other, existing)| *other == key && existing == value)
        {
            return;
        }
        self.pairs.push((key, value.to_string()));
    }

    /// The values of `key`, in the order they were added.
    #[cfg(test)]
    pub(crate) fn values(&self, key: &str) -> Vec<&str> {
        self.pairs
            .iter()
            .filter(|(other, _)| *other == key)
            .map(|(_, value)| value.as_str())
            .collect()
    }

    pub(crate) fn build(self) -> SearchTags {
        SearchTags(KeyedText::from_pairs(self.pairs))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn map(pairs: &[(&str, &[&str])]) -> BTreeMap<String, Vec<String>> {
        pairs
            .iter()
            .map(|(key, values)| {
                (
                    (*key).to_string(),
                    values.iter().map(|value| (*value).to_string()).collect(),
                )
            })
            .collect()
    }

    #[test]
    fn round_trips_the_json_of_a_map_from_key_to_values() {
        let source = map(&[
            ("artist", &["First", "Second"]),
            ("comment", &["Remastered"]),
            ("disc", &["1"]),
            // Keys are free-form; one the scanner does not write stays.
            ("mood", &["calm", "warm"]),
            ("title", &["Song"]),
        ]);
        let json = serde_json::to_string(&source).expect("map json");
        let tags: SearchTags = serde_json::from_str(&json).expect("parse tags");
        assert_eq!(serde_json::to_string(&tags).expect("tags json"), json);
        assert_eq!(tags, SearchTags::from(source.clone()));
        assert_eq!(
            tags.iter()
                .map(|(key, values)| (key.to_string(), values.map(String::from).collect()))
                .collect::<BTreeMap<_, Vec<_>>>(),
            source
        );
        assert_eq!(
            tags.values("artist").collect::<Vec<_>>(),
            ["First", "Second"]
        );
        assert_eq!(tags.values("mood").collect::<Vec<_>>(), ["calm", "warm"]);
        assert_eq!(tags.values("album").count(), 0);

        let empty: SearchTags = serde_json::from_str("{}").expect("parse empty");
        assert!(empty.is_empty());
        assert_eq!(serde_json::to_string(&empty).expect("empty json"), "{}");
        assert!(serde_json::from_str::<SearchTags>("").is_err());
    }

    #[test]
    fn a_key_without_values_is_not_kept() {
        let tags = SearchTags::from(map(&[("comment", &[]), ("title", &["Song"])]));
        assert_eq!(tags, SearchTags::from(map(&[("title", &["Song"])])));
        assert_eq!(
            serde_json::to_string(&tags).expect("json"),
            r#"{"title":["Song"]}"#
        );
    }

    #[test]
    fn insert_replaces_the_values_of_a_key() {
        let mut tags = SearchTags::from(map(&[("comment", &["Old"]), ("title", &["Song"])]));
        tags.insert("comment", ["New", "Newer"]);
        tags.insert("mood", ["calm"]);
        assert_eq!(
            tags,
            SearchTags::from(map(&[
                ("comment", &["New", "Newer"]),
                ("mood", &["calm"]),
                ("title", &["Song"]),
            ]))
        );
        tags.insert("comment", Vec::<String>::new());
        assert_eq!(tags.values("comment").count(), 0);
    }

    #[test]
    fn equality_compares_keys_and_values_in_order() {
        let tags = SearchTags::from(map(&[("artist", &["A", "B"])]));
        assert_eq!(tags, SearchTags::from(map(&[("artist", &["A", "B"])])));
        assert_ne!(tags, SearchTags::from(map(&[("artist", &["B", "A"])])));
        assert_ne!(tags, SearchTags::from(map(&[("artist", &["A"])])));
        assert_ne!(tags, SearchTags::from(map(&[("albumartist", &["A", "B"])])));
        assert_ne!(tags, SearchTags::default());
    }

    #[test]
    fn the_builder_trims_and_skips_empty_and_repeated_values() {
        let mut builder = SearchTagsBuilder::default();
        for (key, value) in [
            ("title", " Song "),
            ("artist", "B"),
            ("artist", "A"),
            ("artist", "B"),
            ("comment", "  "),
            ("title", "Song"),
        ] {
            builder.add(key, value);
        }
        assert_eq!(builder.values("artist"), ["B", "A"]);
        assert_eq!(
            builder.build(),
            SearchTags::from(map(&[("artist", &["B", "A"]), ("title", &["Song"])]))
        );
    }
}
