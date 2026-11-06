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

/// How many vocabulary words a correction compares between checks for a
/// newer query.
const MATCH_CANCEL_POLL_WORDS: usize = 4096;

/// The fields whose words a correction may come from, and so the fields of
/// a `field:value` term that may be corrected.
const NAME_FIELDS: [&str; 4] = ["title", "artist", "album", "albumartist"];

/// The most edits a correction makes to one word.
const MAX_EDITS: usize = 2;

#[derive(Debug)]
struct VocabularyWord {
    /// Folded as the search folds (see [`normalize_for_search`]).
    folded: String,
    /// The folded word's characters, for the edit distance.
    chars: Vec<char>,
    /// See [`histogram`].
    histogram: Histogram,
    /// Lowercase, with its accents, as the library spells it most often.
    spelling: String,
    /// How many names use it.
    count: u32,
}

/// The words of the library's titles, artists and albums, made once per
/// library and kept with its search index.
#[derive(Debug, Default)]
pub(super) struct SearchVocabulary {
    /// In the order of their folded text, so that ties always pick the
    /// same word.
    words: Vec<VocabularyWord>,
    /// The indices of the words of each length in characters, in order.
    by_length: Vec<Vec<u32>>,
}

/// How many characters of a word fall in each of 32 buckets, by code
/// point. An insertion or a deletion changes the counts by 1, a
/// substitution by at most 2 and a swap not at all, so two words are at
/// least half the counts' difference apart: most words of a library are
/// ruled out before their edit distance is worked out. Characters sharing
/// a bucket only make the difference smaller.
type Histogram = [u8; 32];

fn histogram(chars: &[char]) -> Histogram {
    let mut counts = [0u8; 32];
    for &ch in chars {
        let bucket = &mut counts[(u32::from(ch) % 32) as usize];
        *bucket = bucket.saturating_add(1);
    }
    counts
}

/// The least edits that can turn a word with the counts `a` into one with
/// the counts `b`.
fn histogram_bound(a: &Histogram, b: &Histogram) -> usize {
    let difference: usize = a
        .iter()
        .zip(b)
        .map(|(x, y)| usize::from(x.abs_diff(*y)))
        .sum();
    difference.div_ceil(2)
}

/// The words of `name`: lowercase runs of letters and digits.
fn name_words(name: &str) -> impl Iterator<Item = String> + '_ {
    name.split(|ch: char| !ch.is_alphanumeric())
        .filter(|word| !word.is_empty())
        .map(|word| word.nfc().collect::<String>().to_lowercase())
}

/// Asks `newer` for a query that replaces the one being corrected every
/// [`MATCH_CANCEL_POLL_WORDS`] words compared.
struct Poll<'a, Q> {
    newer: &'a mut dyn FnMut() -> Option<Q>,
    compared: usize,
}

impl<Q> Poll<'_, Q> {
    fn compared_one(&mut self) -> Result<(), Q> {
        self.compared += 1;
        #[cfg(test)]
        tests::COMPARED.with(|compared| compared.set(compared.get() + 1));
        if self.compared.is_multiple_of(MATCH_CANCEL_POLL_WORDS) {
            if let Some(next) = (self.newer)() {
                return Err(next);
            }
        }
        Ok(())
    }
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
                let chars: Vec<char> = folded.chars().collect();
                VocabularyWord {
                    histogram: histogram(&chars),
                    chars,
                    folded,
                    spelling,
                    count,
                }
            })
            .collect();
        words.sort_unstable_by(|a, b| a.folded.cmp(&b.folded));
        let mut by_length: Vec<Vec<u32>> = Vec::new();
        for (index, word) in words.iter().enumerate() {
            let length = word.chars.len();
            if by_length.len() <= length {
                by_length.resize_with(length + 1, Vec::new);
            }
            by_length[length].push(u32::try_from(index).unwrap_or(u32::MAX));
        }
        Ok(Self { words, by_length })
    }

    /// The words of `min_length` to `max_length` characters, in order.
    fn words_of_length(
        &self,
        min_length: usize,
        max_length: usize,
    ) -> impl Iterator<Item = &VocabularyWord> {
        self.by_length
            .iter()
            .enumerate()
            .skip(min_length)
            .take_while(move |(length, _)| *length <= max_length)
            .flat_map(|(_, indices)| indices.iter())
            .filter_map(|&index| self.words.get(index as usize))
    }

    /// Whether a word of the library contains `term` (folded).
    fn knows<Q>(&self, term: &str, poll: &mut Poll<'_, Q>) -> Result<bool, Q> {
        let length = term.chars().count();
        for word in self.words_of_length(length, usize::MAX) {
            poll.compared_one()?;
            if word.folded.contains(term) {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// The word closest to `term` (folded), if one is close enough: one edit
    /// for words of up to four letters, two for longer ones. Closer words
    /// win, then the more common, then the shorter.
    fn nearest<Q>(&self, term: &str, poll: &mut Poll<'_, Q>) -> Result<Option<&VocabularyWord>, Q> {
        let chars: Vec<char> = term.chars().collect();
        if chars.len() < 3 {
            return Ok(None);
        }
        let limit = if chars.len() <= 4 { 1 } else { MAX_EDITS };
        let counts = histogram(&chars);
        let mut scratch = EditScratch::default();
        let mut best: Option<(usize, &VocabularyWord)> = None;
        for word in self.words_of_length(chars.len().saturating_sub(limit), chars.len() + limit) {
            poll.compared_one()?;
            let bound = best.map_or(limit, |(distance, _)| distance);
            if histogram_bound(&counts, &word.histogram) > bound {
                continue;
            }
            let Some(distance) = scratch.distance_within(&chars, &word.chars, bound) else {
                continue;
            };
            let better = match best {
                None => true,
                Some((best_distance, best_word)) => {
                    distance < best_distance
                        || (distance == best_distance
                            && (word.count > best_word.count
                                || (word.count == best_word.count
                                    && word.chars.len() < best_word.chars.len())))
                }
            };
            if better {
                best = Some((distance, word));
            }
        }
        Ok(best.map(|(_, word)| word))
    }
}

/// The rows of an edit distance table, kept between comparisons.
#[derive(Default)]
struct EditScratch {
    before: Vec<usize>,
    previous: Vec<usize>,
    current: Vec<usize>,
}

impl EditScratch {
    /// The edit distance between `a` and `b` (insertions, deletions,
    /// substitutions and swaps of neighbours), if it is at most `bound`.
    /// Only the cells within `bound` of the diagonal are computed; the
    /// others are more than `bound` anyway.
    fn distance_within(&mut self, a: &[char], b: &[char], bound: usize) -> Option<usize> {
        if a.len().abs_diff(b.len()) > bound {
            return None;
        }
        let over = bound + 1;
        let width = b.len() + 1;
        for row in [&mut self.before, &mut self.previous, &mut self.current] {
            row.clear();
            row.resize(width, over);
        }
        for (j, cell) in self.previous.iter_mut().enumerate().take(bound + 1) {
            *cell = j;
        }
        for i in 1..=a.len() {
            let low = i.saturating_sub(bound).max(1);
            let high = (i + bound).min(b.len());
            self.current[0] = if i <= bound { i } else { over };
            if low > 1 {
                self.current[low - 1] = over;
            }
            let mut row_min = self.current[0];
            for j in low..=high {
                let cost = usize::from(a[i - 1] != b[j - 1]);
                let mut value = (self.previous[j] + 1)
                    .min(self.current[j - 1] + 1)
                    .min(self.previous[j - 1] + cost);
                if i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] {
                    value = value.min(self.before[j - 2] + 1);
                }
                let value = value.min(over);
                self.current[j] = value;
                row_min = row_min.min(value);
            }
            if high < b.len() {
                self.current[high + 1] = over;
            }
            if row_min > bound {
                return None;
            }
            std::mem::swap(&mut self.before, &mut self.previous);
            std::mem::swap(&mut self.previous, &mut self.current);
        }
        let distance = self.previous[b.len()];
        (distance <= bound).then_some(distance)
    }
}

/// `query` with each of its words that the library has nowhere replaced by
/// the closest word it has, or `None` when no word needed and found a
/// correction. Quoted phrases and filters other than names stay as they
/// are. `newer` is asked every few thousand words compared for a query
/// that replaces this one; the correction then stops and returns it.
pub(super) fn corrected_query<Q>(
    query: &str,
    vocabulary: &SearchVocabulary,
    newer: &mut dyn FnMut() -> Option<Q>,
) -> Result<Option<String>, Q> {
    let mut poll = Poll { newer, compared: 0 };
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
        if token.contains('"') || folded.is_empty() || vocabulary.knows(&folded, &mut poll)? {
            tokens.push(token.to_string());
            continue;
        }
        let Some(word) = vocabulary.nearest(&folded, &mut poll)? else {
            return Ok(None);
        };
        corrected_any = true;
        tokens.push(match prefix {
            Some(name) => format!("{name}:{}", word.spelling),
            None => word.spelling.clone(),
        });
    }
    Ok(corrected_any.then(|| tokens.join(" ")))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::path::PathBuf;

    thread_local! {
        /// How many words corrections on this thread compared.
        pub(super) static COMPARED: Cell<usize> = const { Cell::new(0) };
    }

    /// `query` corrected, with no newer query.
    fn correct(query: &str, vocabulary: &SearchVocabulary) -> Option<String> {
        corrected_query(query, vocabulary, &mut || None::<()>).expect("no newer query")
    }

    fn edit_distance_within(a: &[char], b: &str, bound: usize) -> Option<usize> {
        let b: Vec<char> = b.chars().collect();
        EditScratch::default().distance_within(a, &b, bound)
    }

    /// The whole table, for comparison.
    fn reference_distance(a: &[char], b: &[char]) -> usize {
        let mut table = vec![vec![0usize; b.len() + 1]; a.len() + 1];
        for (i, row) in table.iter_mut().enumerate() {
            row[0] = i;
        }
        for (j, cell) in table[0].iter_mut().enumerate() {
            *cell = j;
        }
        for i in 1..=a.len() {
            for j in 1..=b.len() {
                let cost = usize::from(a[i - 1] != b[j - 1]);
                let mut value = (table[i - 1][j] + 1)
                    .min(table[i][j - 1] + 1)
                    .min(table[i - 1][j - 1] + cost);
                if i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] {
                    value = value.min(table[i - 2][j - 2] + 1);
                }
                table[i][j] = value;
            }
        }
        table[a.len()][b.len()]
    }

    /// Made-up words of `alphabet`, from a fixed seed.
    fn made_up_words(
        alphabet: &[u8],
        count: usize,
        lengths: std::ops::Range<u64>,
        seed: u64,
    ) -> Vec<String> {
        let mut state = seed;
        let mut random = move || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            state
        };
        let mut words = HashSet::new();
        let mut ordered = Vec::new();
        while ordered.len() < count {
            let length = lengths.start + random() % (lengths.end - lengths.start);
            let word: String = (0..length)
                .map(|_| alphabet[(random() % alphabet.len() as u64) as usize] as char)
                .collect();
            if words.insert(word.clone()) {
                ordered.push(word);
            }
        }
        ordered
    }

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
        assert_eq!(correct("merdian", &words).as_deref(), Some("meridian"));
        // A swap of neighbours is one edit.
        assert_eq!(correct("meirdian", &words).as_deref(), Some("meridian"));
        assert_eq!(
            correct("quiet merdian", &words).as_deref(),
            Some("quiet meridian")
        );
        assert_eq!(
            correct("artist:merdian", &words).as_deref(),
            Some("artist:meridian")
        );
    }

    #[test]
    fn suggests_nothing_for_known_words_far_words_and_other_filters() {
        let words = vocabulary(&fixture());
        assert_eq!(correct("meridian", &words), None);
        assert_eq!(
            correct("merid", &words),
            None,
            "a prefix of a word is known"
        );
        assert_eq!(correct("xylophone", &words), None);
        assert_eq!(
            correct("merdian zzzzzz", &words),
            None,
            "every unknown word needs a correction"
        );
        assert_eq!(correct("genre:merdian", &words), None);
        assert_eq!(correct("\"merdian\"", &words), None);
        assert_eq!(correct("mx", &words), None, "too short to correct");
    }

    #[test]
    fn keeps_the_library_spelling_with_its_accents() {
        let words = vocabulary(&fixture());
        assert_eq!(correct("ornul", &words), None, "a prefix of ørnulf, folded");
        assert_eq!(correct("ornulv", &words).as_deref(), Some("ørnulf"));
        assert_eq!(
            correct("lumiere cafx", &words).as_deref(),
            Some("lumiere café")
        );
    }

    #[test]
    fn short_words_allow_one_edit_and_long_words_two() {
        let words = vocabulary(&[track("Fjord", "Hour", "Eventide")]);
        assert_eq!(correct("hoxr", &words).as_deref(), Some("hour"));
        assert_eq!(correct("hxxr", &words), None);
        assert_eq!(correct("evxntxde", &words).as_deref(), Some("eventide"));
        assert_eq!(correct("exxntxde", &words), None);
    }

    #[test]
    fn prefers_the_more_common_word_on_a_tie() {
        let words = vocabulary(&[
            track("Lines", "A", "One"),
            track("Lanes", "B", "Two"),
            track("Lanes Again", "C", "Three"),
        ]);
        assert_eq!(correct("lunes", &words).as_deref(), Some("lanes"));
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
    #[test]
    fn the_banded_distance_matches_the_whole_table() {
        let words = made_up_words(b"abc", 300, 0..7, 0x9e37_79b9_7f4a_7c15);
        let mut scratch = EditScratch::default();
        for a in &words {
            let a: Vec<char> = a.chars().collect();
            for b in &words {
                let b: Vec<char> = b.chars().collect();
                let distance = reference_distance(&a, &b);
                for bound in 0..=3 {
                    let expected = (distance <= bound).then_some(distance);
                    assert_eq!(
                        scratch.distance_within(&a, &b, bound),
                        expected,
                        "{a:?} {b:?} within {bound}"
                    );
                }
            }
        }
    }

    #[test]
    fn the_histogram_bound_never_exceeds_the_distance() {
        let words = made_up_words(b"abcdxyz", 200, 0..8, 0x51_7cc1_b727_220a);
        for a in &words {
            let a: Vec<char> = a.chars().collect();
            for b in &words {
                let b: Vec<char> = b.chars().collect();
                assert!(
                    histogram_bound(&histogram(&a), &histogram(&b)) <= reference_distance(&a, &b)
                );
            }
        }
        // Accented and other letters share buckets: the bound only gets
        // smaller.
        let a: Vec<char> = "ørnulf".chars().collect();
        let b: Vec<char> = "ornulv".chars().collect();
        assert!(histogram_bound(&histogram(&a), &histogram(&b)) <= reference_distance(&a, &b));
    }

    #[test]
    fn a_newer_query_stops_the_correction_within_one_poll() {
        let titles = made_up_words(b"abcdef", 30_000, 3..13, 0x2545_f491_4f6c_dd1d);
        let tracks: Vec<_> = titles.iter().map(|title| track(title, "", "")).collect();
        let words = vocabulary(&tracks);
        let query = made_up_words(b"abcdef", 8, 8..9, 7).join(" ");
        // Without a newer query, it compares far more than one poll's worth.
        COMPARED.with(|compared| compared.set(0));
        let _ = correct(&query, &words);
        let whole = COMPARED.with(Cell::get);
        assert!(whole > 4 * MATCH_CANCEL_POLL_WORDS, "{whole} comparisons");
        // A newer query at the first poll stops it there.
        COMPARED.with(|compared| compared.set(0));
        let mut polls = 0;
        let result = corrected_query(&query, &words, &mut || {
            polls += 1;
            Some(9)
        });
        assert_eq!(result, Err(9));
        assert_eq!(polls, 1);
        assert_eq!(COMPARED.with(Cell::get), MATCH_CANCEL_POLL_WORDS);
    }

    /// 8 misspelt words against a vocabulary of 100,000 words, with
    /// letters from the whole alphabet (most comparisons end early) and
    /// from six letters (many words are close, so few do).
    /// `cargo test --release --lib -- --ignored correction_benchmark --nocapture`
    #[test]
    #[ignore = "a benchmark"]
    fn correction_benchmark_100k_words() {
        for alphabet in [&b"abcdefghijklmnopqrstuvwxyz"[..], &b"abcdef"[..]] {
            let titles = made_up_words(alphabet, 100_000, 3..13, 0x2545_f491_4f6c_dd1d);
            let tracks: Vec<_> = titles.iter().map(|title| track(title, "", "")).collect();
            let words = vocabulary(&tracks);
            let query = made_up_words(alphabet, 8, 8..9, 7).join(" ");
            let runs = 5;
            COMPARED.with(|compared| compared.set(0));
            let started = std::time::Instant::now();
            for _ in 0..runs {
                std::hint::black_box(correct(&query, &words));
            }
            let milliseconds = started.elapsed().as_secs_f64() * 1000.0 / f64::from(runs);
            let compared = COMPARED.with(Cell::get) / runs as usize;
            eprintln!(
                "{} letters: correction of 8 unknown words against {} words: {milliseconds:.1} ms, \
                 {compared} words compared, a newer query waits at most {:.2} ms",
                alphabet.len(),
                titles.len(),
                milliseconds * MATCH_CANCEL_POLL_WORDS as f64 / compared as f64
            );
        }
    }
}
