//! Fuzzy relevance scoring for iTunes artwork search results.
//!
//! Computes a combined album + artist similarity score using Jaro-Winkler,
//! so results with both a matching album title *and* a matching artist sort
//! above albums that merely share a common title.

/// Normalize a string for comparison: trim, Unicode case-fold, collapse runs
/// of whitespace to a single space.
fn normalize(s: &str) -> String {
    s.trim()
        .to_lowercase()
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
}

/// Return a 0.0–1.0 relevance score for an iTunes candidate against
/// the wanted album/artist.
///
/// When both wanted fields are present, weight is split 50/50 between
/// album and artist similarity.  If one of the wanted fields is empty
/// (after normalization), the other carries the full weight.
#[must_use]
pub fn itunes_relevance_score(
    candidate_album: &str,
    candidate_artist: &str,
    wanted_album: &str,
    wanted_artist: &str,
) -> f64 {
    let ca = normalize(candidate_album);
    let cr = normalize(candidate_artist);
    let wa = normalize(wanted_album);
    let wr = normalize(wanted_artist);

    let have_album = !wa.is_empty();
    let have_artist = !wr.is_empty();

    match (have_album, have_artist) {
        (true, true) => {
            let album_sim = strsim::jaro_winkler(&ca, &wa);
            let artist_sim = strsim::jaro_winkler(&cr, &wr);
            album_sim * 0.5 + artist_sim * 0.5
        }
        (true, false) => strsim::jaro_winkler(&ca, &wa),
        (false, true) => strsim::jaro_winkler(&cr, &wr),
        (false, false) => 0.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_match_scores_near_one() {
        let score =
            itunes_relevance_score("Peace and Love", "Dadawah", "Peace and Love", "Dadawah");
        assert!(score > 0.99, "exact match should be ~1.0, got {score}");
    }

    #[test]
    fn same_album_different_artist_scores_lower() {
        let correct =
            itunes_relevance_score("Peace and Love", "Dadawah", "Peace and Love", "Dadawah");
        let wrong_artist = itunes_relevance_score(
            "Peace and Love",
            "Florida Georgia Line",
            "Peace and Love",
            "Dadawah",
        );
        assert!(
            correct > wrong_artist,
            "correct artist should score higher: {correct} vs {wrong_artist}"
        );
    }

    #[test]
    fn dadawah_ranks_above_irrelevant_same_title() {
        let dadawah =
            itunes_relevance_score("Peace and Love", "Dadawah", "Peace and Love", "Dadawah");
        let other =
            itunes_relevance_score("Peace and Love", "Bob Sinclar", "Peace and Love", "Dadawah");
        assert!(
            dadawah > other,
            "Dadawah should rank above Bob Sinclar: {dadawah} vs {other}"
        );
    }

    #[test]
    fn empty_wanted_artist_scores_on_album_only() {
        let score = itunes_relevance_score("Peace and Love", "Dadawah", "Peace and Love", "");
        assert!(score > 0.99, "album-only match should be ~1.0, got {score}");
    }

    #[test]
    fn empty_candidate_strings_score_low() {
        let score = itunes_relevance_score("", "", "Peace and Love", "Dadawah");
        assert!(
            score < 0.5,
            "empty candidates should score low, got {score}"
        );
    }

    #[test]
    fn case_and_whitespace_insensitive() {
        let score = itunes_relevance_score(
            "  peace  AND  love  ",
            " DADAWAH ",
            "Peace and Love",
            "Dadawah",
        );
        assert!(
            score > 0.99,
            "should be case/whitespace insensitive, got {score}"
        );
    }

    #[test]
    fn subtitle_variant_scores_high() {
        let score = itunes_relevance_score(
            "Peace and Love - Wadadasow",
            "Dadawah",
            "Peace and Love",
            "Dadawah",
        );
        // Jaro-Winkler handles length differences well; the album part
        // shares a long prefix, so we expect a decent score.
        assert!(
            score > 0.8,
            "subtitle variant should still score high, got {score}"
        );
    }
}
