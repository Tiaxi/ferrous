// SPDX-License-Identifier: GPL-3.0-or-later

//! Folding text for searching and sorting names.
//!
//! [`fold`] spells text with lowercase base letters: it decomposes it
//! (NFKD), drops the combining marks, lowercases it, and spells the letters
//! that have no decomposition, such as `ø`, `æ`, `ß` and `ł`, with the base
//! letters they are written with in ASCII. Searches compare folded text, and
//! [`CollationKey`] sorts by it, with numbers in their natural order.

use std::cmp::Ordering;

use unicode_normalization::char::is_combining_mark;
use unicode_normalization::UnicodeNormalization;

/// `text` in lowercase base letters, for comparisons that ignore case and
/// accents: "Øystein Sørensen" folds to "oystein sorensen", and "Straße" to
/// "strasse".
#[must_use]
pub(crate) fn fold(text: &str) -> String {
    if text.is_ascii() {
        return text.to_ascii_lowercase();
    }
    let mut out = String::with_capacity(text.len());
    for ch in text.nfkd().filter(|ch| !is_combining_mark(*ch)) {
        if ch.is_ascii() {
            out.push(ch.to_ascii_lowercase());
            continue;
        }
        for lower in ch.to_lowercase().filter(|ch| !is_combining_mark(*ch)) {
            match base_letters(lower) {
                Some(spelled) => out.push_str(spelled),
                None => out.push(lower),
            }
        }
    }
    out
}

/// The base letters of a lowercase letter that NFKD does not decompose.
fn base_letters(ch: char) -> Option<&'static str> {
    Some(match ch {
        'ß' => "ss",
        'æ' => "ae",
        'œ' => "oe",
        'ø' => "o",
        'ł' => "l",
        'đ' | 'ð' => "d",
        'þ' => "th",
        'ħ' => "h",
        'ŧ' => "t",
        'ı' => "i",
        'ς' => "σ",
        _ => return None,
    })
}

/// The sort key of a name: its [`fold`], compared with runs of digits as
/// numbers, so "Disc 2" sorts before "Disc 10" and "Émile" with "Emile".
///
/// Names that fold to the same text compare equal; sort them by their own
/// text after the key for a stable order.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub(crate) struct CollationKey(String);

impl CollationKey {
    #[must_use]
    pub(crate) fn new(text: &str) -> Self {
        Self(fold(text))
    }
}

impl Ord for CollationKey {
    fn cmp(&self, other: &Self) -> Ordering {
        natural_cmp(&self.0, &other.0)
    }
}

impl PartialOrd for CollationKey {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

/// Compares two names by their [`CollationKey`], then by their own text.
#[cfg(test)]
fn collate(a: &str, b: &str) -> Ordering {
    CollationKey::new(a)
        .cmp(&CollationKey::new(b))
        .then_with(|| a.cmp(b))
}

/// Compares `a` and `b` character by character, and each run of ASCII
/// digits that both have at the same place as a number. Equal numbers with
/// fewer leading zeros sort first, so only equal strings compare equal.
///
/// A run of digits sorts where its first digit would: after the characters
/// below `'0'` and before those above `'9'`, so the order is total.
fn natural_cmp(a: &str, b: &str) -> Ordering {
    let a = a.as_bytes();
    let b = b.as_bytes();
    let (mut ia, mut ib) = (0, 0);
    while ia < a.len() && ib < b.len() {
        if a[ia].is_ascii_digit() && b[ib].is_ascii_digit() {
            let (na, ea) = digit_run(a, ia);
            let (nb, eb) = digit_run(b, ib);
            let order = na
                .len()
                .cmp(&nb.len())
                .then_with(|| na.cmp(nb))
                .then_with(|| (ea - ia).cmp(&(eb - ib)));
            if order != Ordering::Equal {
                return order;
            }
            (ia, ib) = (ea, eb);
            continue;
        }
        // Comparing UTF-8 bytes compares the characters' code points.
        match a[ia].cmp(&b[ib]) {
            Ordering::Equal => {
                ia += 1;
                ib += 1;
            }
            order => return order,
        }
    }
    (a.len() - ia).cmp(&(b.len() - ib))
}

/// The digits of the run of digits that starts at `start`, without leading
/// zeros, and the index after the run.
fn digit_run(text: &[u8], start: usize) -> (&[u8], usize) {
    let end = text[start..]
        .iter()
        .position(|byte| !byte.is_ascii_digit())
        .map_or(text.len(), |len| start + len);
    let digits = &text[start..end];
    let zeros = digits.iter().take_while(|&&digit| digit == b'0').count();
    (&digits[zeros..], end)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sorted(names: &[&str]) -> Vec<String> {
        let mut names: Vec<String> = names.iter().map(ToString::to_string).collect();
        names.sort_by(|a, b| collate(a, b));
        names
    }

    #[test]
    fn letters_without_a_decomposition_fold_to_their_base_letters() {
        assert_eq!(fold("Øystein Sørensen"), "oystein sorensen");
        assert_eq!(fold("Æther Œuvre"), "aether oeuvre");
        assert_eq!(fold("Straße STRASSE ẞ"), "strasse strasse ss");
        assert_eq!(fold("Łódź"), "lodz");
        assert_eq!(fold("Þórður Đorđe"), "thordur dorde");
        assert_eq!(fold("Jónsi Björk Beyoncé"), "jonsi bjork beyonce");
        assert_eq!(fold("Ｆｕｌｌ ﬁ"), "full fi");
        assert_eq!(fold("ΣΊΣΥΦΟΣ"), fold("σίσυφος"));
    }

    #[test]
    fn accented_names_sort_with_their_base_letters() {
        assert_eq!(
            sorted(&[
                "Zappa",
                "Émilie Simon",
                "Ørjan",
                "Æsop Rock",
                "Eels",
                "Oasis",
                "Abba"
            ]),
            [
                "Abba",
                "Æsop Rock",
                "Eels",
                "Émilie Simon",
                "Oasis",
                "Ørjan",
                "Zappa"
            ]
        );
        assert_eq!(
            sorted(&["Łona", "Lorde", "Lykke Li", "ÉLAN", "élan", "Elan"]),
            ["Elan", "ÉLAN", "élan", "Łona", "Lorde", "Lykke Li"]
        );
    }

    #[test]
    fn numbers_sort_in_their_natural_order() {
        assert_eq!(
            sorted(&["Disc 10", "Disc 2", "disc 1", "Disc 02", "Disc 1b", "Disc"]),
            ["Disc", "disc 1", "Disc 1b", "Disc 2", "Disc 02", "Disc 10"]
        );
        assert_eq!(
            sorted(&["Track 100", "Track 99", "Track 0099", "Track 9"]),
            ["Track 9", "Track 99", "Track 0099", "Track 100"]
        );
    }

    #[test]
    fn the_order_is_total() {
        let names = [
            "a1", "a01", "a/", "a:", "a", "a1b", "a10", "a 1", "A1", "á1", "a0", "a00", "1", "01",
            "", "/", ":", "ß", "ss", "Ss",
        ];
        for a in names {
            for b in names {
                let ab = collate(a, b);
                assert_eq!(ab, collate(b, a).reverse(), "{a:?} {b:?}");
                assert_eq!(ab == Ordering::Equal, a == b, "{a:?} {b:?}");
                for c in names {
                    if ab != Ordering::Greater && collate(b, c) != Ordering::Greater {
                        assert_ne!(collate(a, c), Ordering::Greater, "{a:?} {b:?} {c:?}");
                    }
                }
            }
        }
    }
}
