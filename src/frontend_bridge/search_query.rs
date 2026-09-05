// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::BTreeMap;

use super::search::normalize_for_search;

#[derive(Debug)]
pub(super) struct SearchTerm {
    pub(super) field: Option<&'static str>,
    pub(super) value: String,
}

#[derive(Debug)]
pub(super) struct SearchQuery {
    pub(super) terms: Vec<SearchTerm>,
}

pub(super) fn field_key(name: &str) -> Option<&'static str> {
    match name.to_ascii_lowercase().as_str() {
        "title" => Some("title"),
        "artist" => Some("artist"),
        "album" => Some("album"),
        "albumartist" | "album_artist" | "album-artist" => Some("albumartist"),
        "genre" => Some("genre"),
        "year" => Some("year"),
        "date" => Some("date"),
        "composer" => Some("composer"),
        "conductor" => Some("conductor"),
        "performer" => Some("performer"),
        "label" | "publisher" => Some("label"),
        "comment" => Some("comment"),
        "lyrics" => Some("lyrics"),
        "path" | "filename" => Some("path"),
        "root" => Some("root"),
        "track" => Some("track"),
        "disc" => Some("disc"),
        _ => None,
    }
}

impl SearchQuery {
    pub(super) fn parse(raw: &str) -> Self {
        let mut tokens = Vec::new();
        let mut token = String::new();
        let mut quoted = false;
        for ch in raw.chars() {
            if ch == '"' {
                quoted = !quoted;
            } else if ch.is_whitespace() && !quoted {
                if !token.is_empty() {
                    tokens.push(std::mem::take(&mut token));
                }
            } else {
                token.push(ch);
            }
        }
        if !token.is_empty() {
            tokens.push(token);
        }
        let terms = tokens
            .into_iter()
            .map(|token| {
                if let Some((name, value)) = token.split_once(':') {
                    if let Some(field) = field_key(name) {
                        return SearchTerm {
                            field: Some(field),
                            value: normalize_for_search(value.trim()),
                        };
                    }
                }
                SearchTerm {
                    field: None,
                    value: normalize_for_search(token.trim()),
                }
            })
            .collect();
        Self { terms }
    }

    pub(super) fn matches(&self, fields: &BTreeMap<String, String>) -> bool {
        self.matches_with(fields, |_| true, &[])
    }

    pub(super) fn matches_with(
        &self,
        fields: &BTreeMap<String, String>,
        include: impl Fn(&str) -> bool,
        extra: &[(&str, &str)],
    ) -> bool {
        !self.terms.is_empty()
            && self.terms.iter().all(|term| {
                !term.value.is_empty()
                    && (fields
                        .iter()
                        .any(|(field, value)| include(field) && term.matches(field, value))
                        || extra
                            .iter()
                            .any(|(field, value)| term.matches(field, value)))
            })
    }

    pub(super) fn name_terms(&self, field: &str) -> Vec<String> {
        self.terms
            .iter()
            .filter(|term| term.field.is_none_or(|key| key == field))
            .map(|term| term.value.clone())
            .collect()
    }
}

impl SearchTerm {
    pub(super) fn matches(&self, field: &str, value: &str) -> bool {
        if self.field.is_some_and(|key| key != field) || self.value.is_empty() {
            return false;
        }
        if self.field.is_some() && matches!(field, "year" | "track" | "disc") {
            value.split_whitespace().any(|part| part == self.value)
        } else {
            value.contains(&self.value)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_fields_and_quoted_phrases_without_interpreting_unknown_prefixes() {
        let fields = BTreeMap::from([
            ("albumartist".into(), "sigur ros".into()),
            ("year".into(), "1997".into()),
            ("comment".into(), "anniversary remaster".into()),
            ("path".into(), "/music/live:oslo.flac".into()),
        ]);
        assert!(
            SearchQuery::parse("album_artist:\"Sigur Rós\" year:1997 comment:remaster")
                .matches(&fields)
        );
        assert!(SearchQuery::parse("live:oslo").matches(&fields));
        assert!(!SearchQuery::parse("year:199").matches(&fields));
        assert!(!SearchQuery::parse("artist:remaster").matches(&fields));
        assert!(!SearchQuery::parse("comment:").matches(&fields));
        assert!(!SearchQuery::parse("\"ros sigur\"").matches(&fields));
        assert!(!SearchQuery::parse("\"\"").matches(&fields));
    }
}
