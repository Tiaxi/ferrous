// SPDX-License-Identifier: GPL-3.0-or-later

//! A spelling correction for a search that found nothing: each word of the
//! query that no title, artist or album contains is replaced by the closest
//! word of the library's names, so "merdian" suggests "meridian".

use std::collections::{HashMap, HashSet};

use unicode_normalization::UnicodeNormalization;

use crate::library::LibraryTrack;

use super::search::normalize_for_search;
use super::search_query::field_key;

/// How many tracks the vocabulary reads between checks for a newer query.
const VOCABULARY_CANCEL_POLL_TRACKS: usize = 1024;

/// The fields whose words a correction may come from, and so the fields of
/// a `field:value` term that may be corrected.
const NAME_FIELDS: [&str; 4] = ["title", "artist", "album", "albumartist"];

#[derive(Debug)]
struct VocabularyWord {
    /// Folded as the search folds (see [`normalize_for_search`]).
    folded: String,
    /// Lowercase, with its accents, as the library spells it most often.
    spelling: String,
    /// How many names use it.
    count: u32,
    chars: usize,
}

/// The words of the library's titles, artists and albums.
#[derive(Debug, Default)]
pub(super) struct SearchVocabulary {
    words: Vec<VocabularyWord>,
}

/// The words of `name`: lowercase runs of letters and digits.
fn name_words(name: &str) -> impl Iterator<Item = String> + '_ {
    name.split(|ch: char| !ch.is_alphanumeric())
        .filter(|word| !word.is_empty())
        .map(|word| word.nfc().collect::<String>().to_lowercase())
}

impl SearchVocabulary {
    /// The vocabulary of `tracks`. `newer` is asked between tracks for a
    /// query that replaces the one waiting for it; the build then stops and
    /// returns that query.
    pub(super) fn build<'a, Q>(
        tracks: impl Iterator<Item = &'a LibraryTrack>,
        mut newer: impl FnMut() -> Option<Q>,
    ) -> Result<Self, Q> {
        // Folded word -> (spelling -> uses).
        let mut words: HashMap<String, HashMap<String, u32>> = HashMap::new();
        // Artist and album names repeat on every track: read each once.
        let mut seen: HashSet<&str> = HashSet::new();
        for (index, track) in tracks.enumerate() {
            if index % VOCABULARY_CANCEL_POLL_TRACKS == 0 && index > 0 {
                if let Some(next) = newer() {
                    return Err(next);
                }
            }
            for name in [
                track.title.as_str(),
                track.artist.as_str(),
                track.album.as_str(),
            ] {
                if name.is_empty() || !seen.insert(name) {
                    continue;
                }
                for spelling in name_words(name) {
                    let folded = normalize_for_search(&spelling);
                    if folded.is_empty() {
                        continue;
                    }
                    *words
                        .entry(folded)
                        .or_default()
                        .entry(spelling)
                        .or_default() += 1;
                }
            }
        }
        let mut words: Vec<VocabularyWord> = words
            .into_iter()
            .map(|(folded, spellings)| {
                let count = spellings.values().sum();
                let spelling = spellings
                    .into_iter()
                    .max_by(|a, b| a.1.cmp(&b.1).then_with(|| b.0.cmp(&a.0)))
                    .map(|(spelling, _)| spelling)
                    .unwrap_or_default();
                VocabularyWord {
                    chars: folded.chars().count(),
                    folded,
                    spelling,
                    count,
                }
            })
            .collect();
        // A stable order, so that ties always pick the same word.
        words.sort_unstable_by(|a, b| a.folded.cmp(&b.folded));
        Ok(Self { words })
    }

    /// Whether a word of the library contains `term` (folded).
    fn knows(&self, term: &str) -> bool {
        self.words.iter().any(|word| word.folded.contains(term))
    }

    /// The word closest to `term` (folded), if one is close enough: one edit
    /// for words of up to four letters, two for longer ones. Closer words
    /// win, then the more common, then the shorter.
    fn nearest(&self, term: &str) -> Option<&VocabularyWord> {
        let chars: Vec<char> = term.chars().collect();
        if chars.len() < 3 {
            return None;
        }
        let limit = if chars.len() <= 4 { 1 } else { 2 };
        let mut best: Option<(usize, &VocabularyWord)> = None;
        for word in &self.words {
            if word.chars.abs_diff(chars.len()) > limit {
                continue;
            }
            let bound = best.map_or(limit, |(distance, _)| distance);
            let Some(distance) = edit_distance_within(&chars, &word.folded, bound) else {
                continue;
            };
            let better = match best {
                None => true,
                Some((best_distance, best_word)) => {
                    distance < best_distance
                        || (distance == best_distance
                            && (word.count > best_word.count
                                || (word.count == best_word.count && word.chars < best_word.chars)))
                }
            };
            if better {
                best = Some((distance, word));
            }
        }
        best.map(|(_, word)| word)
    }
}

/// The edit distance between `a` and `b` (insertions, deletions,
/// substitutions and swaps of neighbours), if it is at most `bound`.
fn edit_distance_within(a: &[char], b: &str, bound: usize) -> Option<usize> {
    let b: Vec<char> = b.chars().collect();
    if a.len().abs_diff(b.len()) > bound {
        return None;
    }
    let width = b.len() + 1;
    // Three rows of the table: two back, one back and this one.
    let mut before: Vec<usize> = vec![0; width];
    let mut previous: Vec<usize> = (0..width).collect();
    let mut current: Vec<usize> = vec![0; width];
    for i in 1..=a.len() {
        current[0] = i;
        let mut row_min = current[0];
        for j in 1..width {
            let cost = usize::from(a[i - 1] != b[j - 1]);
            let mut value = (previous[j] + 1)
                .min(current[j - 1] + 1)
                .min(previous[j - 1] + cost);
            if i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] {
                value = value.min(before[j - 2] + 1);
            }
            current[j] = value;
            row_min = row_min.min(value);
        }
        if row_min > bound {
            return None;
        }
        std::mem::swap(&mut before, &mut previous);
        std::mem::swap(&mut previous, &mut current);
    }
    let distance = previous[b.len()];
    (distance <= bound).then_some(distance)
}

/// `query` with each of its words that the library has nowhere replaced by
/// the closest word it has, or `None` when no word needed and found a
/// correction. Quoted phrases and filters other than names stay as they
/// are.
pub(super) fn corrected_query(query: &str, vocabulary: &SearchVocabulary) -> Option<String> {
    let mut corrected_any = false;
    let mut tokens = Vec::new();
    for token in query.split_whitespace() {
        let (prefix, value) = match token.split_once(':') {
            Some((name, value)) if field_key(name).is_some() => {
                if !field_key(name).is_some_and(|field| NAME_FIELDS.contains(&field)) {
                    tokens.push(token.to_string());
                    continue;
                }
                (Some(name), value)
            }
            _ => (None, token),
        };
        let folded = normalize_for_search(value);
        if token.contains('"') || folded.is_empty() || vocabulary.knows(&folded) {
            tokens.push(token.to_string());
            continue;
        }
        let word = vocabulary.nearest(&folded)?;
        corrected_any = true;
        tokens.push(match prefix {
            Some(name) => format!("{name}:{}", word.spelling),
            None => word.spelling.clone(),
        });
    }
    corrected_any.then(|| tokens.join(" "))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn track(title: &str, artist: &str, album: &str) -> LibraryTrack {
        LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: PathBuf::from(format!("/fixture/{title}.flac")),
            root_path: PathBuf::from("/fixture"),
            title: title.to_string(),
            artist: artist.to_string(),
            album: album.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            cover_read_at: crate::library::CoverReadAt::default(),
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        }
    }

    fn vocabulary(tracks: &[LibraryTrack]) -> SearchVocabulary {
        SearchVocabulary::build(tracks.iter(), || None::<()>).expect("not cancelled")
    }

    fn fixture() -> Vec<LibraryTrack> {
        vec![
            track("Morning Tide Field", "The Quiet Meridian", "Small Hours"),
            track("Lantern Signal", "The Quiet Meridian", "Small Hours"),
            track("Meridian Line", "The Quiet Meridian", "Grey Lines"),
            track("Ørnen", "Sølvi Ørnulf", "Fjord"),
            track("Café Lumière", "Lumen Drift", "Afterimage"),
        ]
    }

    #[test]
    fn corrects_a_misspelt_word_to_the_closest_name_word() {
        let words = vocabulary(&fixture());
        assert_eq!(
            corrected_query("merdian", &words).as_deref(),
            Some("meridian")
        );
        // A swap of neighbours is one edit.
        assert_eq!(
            corrected_query("meirdian", &words).as_deref(),
            Some("meridian")
        );
        assert_eq!(
            corrected_query("quiet merdian", &words).as_deref(),
            Some("quiet meridian")
        );
        assert_eq!(
            corrected_query("artist:merdian", &words).as_deref(),
            Some("artist:meridian")
        );
    }

    #[test]
    fn suggests_nothing_for_known_words_far_words_and_other_filters() {
        let words = vocabulary(&fixture());
        assert_eq!(corrected_query("meridian", &words), None);
        assert_eq!(
            corrected_query("merid", &words),
            None,
            "a prefix of a word is known"
        );
        assert_eq!(corrected_query("xylophone", &words), None);
        assert_eq!(
            corrected_query("merdian zzzzzz", &words),
            None,
            "every unknown word needs a correction"
        );
        assert_eq!(corrected_query("genre:merdian", &words), None);
        assert_eq!(corrected_query("\"merdian\"", &words), None);
        assert_eq!(corrected_query("mx", &words), None, "too short to correct");
    }

    #[test]
    fn keeps_the_library_spelling_with_its_accents() {
        let words = vocabulary(&fixture());
        assert_eq!(
            corrected_query("ornul", &words),
            None,
            "a prefix of ørnulf, folded"
        );
        assert_eq!(corrected_query("ornulv", &words).as_deref(), Some("ørnulf"));
        assert_eq!(
            corrected_query("lumiere cafx", &words).as_deref(),
            Some("lumiere café")
        );
    }

    #[test]
    fn short_words_allow_one_edit_and_long_words_two() {
        let words = vocabulary(&[track("Fjord", "Hour", "Eventide")]);
        assert_eq!(corrected_query("hoxr", &words).as_deref(), Some("hour"));
        assert_eq!(corrected_query("hxxr", &words), None);
        assert_eq!(
            corrected_query("evxntxde", &words).as_deref(),
            Some("eventide")
        );
        assert_eq!(corrected_query("exxntxde", &words), None);
    }

    #[test]
    fn prefers_the_more_common_word_on_a_tie() {
        let words = vocabulary(&[
            track("Lines", "A", "One"),
            track("Lanes", "B", "Two"),
            track("Lanes Again", "C", "Three"),
        ]);
        assert_eq!(corrected_query("lunes", &words).as_deref(), Some("lanes"));
    }

    #[test]
    fn the_build_stops_for_a_newer_query() {
        let tracks: Vec<_> = (0..3000)
            .map(|i| track(&format!("Song {i}"), "A", "B"))
            .collect();
        let mut polls = 0;
        let result = SearchVocabulary::build(tracks.iter(), || {
            polls += 1;
            Some(7)
        });
        assert_eq!(result.err(), Some(7));
        assert_eq!(polls, 1);
    }

    #[test]
    fn edit_distance_counts_swaps_once_and_respects_the_bound() {
        let chars = |text: &str| text.chars().collect::<Vec<_>>();
        assert_eq!(edit_distance_within(&chars("abcd"), "abcd", 2), Some(0));
        assert_eq!(edit_distance_within(&chars("abcd"), "abdc", 2), Some(1));
        assert_eq!(edit_distance_within(&chars("abcd"), "axcx", 2), Some(2));
        assert_eq!(edit_distance_within(&chars("abcd"), "xxxx", 2), None);
        assert_eq!(edit_distance_within(&chars("ab"), "abcde", 2), None);
    }
}
