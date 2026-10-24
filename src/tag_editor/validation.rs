// SPDX-License-Identifier: GPL-3.0-or-later

//! Checks that edited values can be stored by the tag they are written to.
//!
//! A value a tag cannot hold is refused before the file is touched. Writers
//! otherwise shorten it, drop it, or, for MP4 numbers, write an atom that
//! makes the file unreadable.

use lofty::tag::TagType;

use super::TagField;

/// The tag an edit is written to, as far as its checks differ.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct TagTarget {
    /// The name errors use, such as `MP4`.
    name: &'static str,
    date: DateStorage,
    /// The largest track or disc number, or total, the tag stores.
    max_number: u32,
    /// Whether the tag keeps any track or disc text, such as `A1` or
    /// `Side A`, and reads it back exactly. Only Vorbis comments do: lofty
    /// drops a label from `ID3v2` `TRCK` and `MP4` `trkn`, the `ApeTag::from`
    /// conversion in the `APEv2` save path drops it before writing, and the
    /// raw AC3/DTS reader takes only numbers.
    keeps_labels: bool,
}

/// How a tag stores the recording date.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DateStorage {
    /// The text as given: `ID3v2.4` `TDRC`, MP4 `©day`, Vorbis `DATE`, and
    /// `APEv2` `Year`.
    Text,
    /// `ID3v2.3` frames: `TYER` holds the year, written without leading
    /// zeros, `TDAT` the day and month, and `TIME` the hour and minute.
    Id3v23Frames,
}

/// The parts a Year value gives, from the year down to the second.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Precision {
    Year,
    Month,
    Day,
    Hour,
    Minute,
    Second,
}

const TEXT_DATE_FORMS: &str =
    "YYYY, YYYY-MM, YYYY-MM-DD, YYYY-MM-DDTHH, YYYY-MM-DDTHH:MM, or YYYY-MM-DDTHH:MM:SS";
const ID3V23_DATE_FORMS: &str = "YYYY, YYYY-MM-DD, or YYYY-MM-DDTHH:MM";

impl TagTarget {
    /// The `APEv2` tag the tag editor writes itself into raw AC3 and DTS files.
    pub(super) const RAW_APEV2: Self = Self::text("APEv2", u32::MAX);

    const fn text(name: &'static str, max_number: u32) -> Self {
        Self {
            name,
            date: DateStorage::Text,
            max_number,
            keeps_labels: false,
        }
    }

    /// The target for a lofty tag of `tag_type`. `id3v23` tells whether an
    /// `ID3v2` tag is written as version 2.3.
    pub(super) fn for_tag(tag_type: TagType, id3v23: bool) -> Self {
        match tag_type {
            TagType::Id3v2 if id3v23 => Self {
                name: "ID3v2.3",
                date: DateStorage::Id3v23Frames,
                max_number: u32::MAX,
                keeps_labels: false,
            },
            TagType::Id3v2 => Self::text("ID3v2.4", u32::MAX),
            // `trkn` and `disk` hold 16-bit numbers.
            TagType::Mp4Ilst => Self::text("MP4", u32::from(u16::MAX)),
            TagType::VorbisComments => Self {
                keeps_labels: true,
                ..Self::text("Vorbis comments", u32::MAX)
            },
            TagType::Ape => Self::text("APEv2", u32::MAX),
            _ => Self::text("this tag", u32::MAX),
        }
    }

    /// Check an edited Year value: a year or an ISO 8601 date and time such
    /// as `1997-05-12`, which must exist and which this tag must store
    /// exactly. Returns the trimmed text to store; empty clears the date.
    pub(super) fn validated_year(self, value: &str) -> Result<String, String> {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            return Ok(String::new());
        }
        let forms = match self.date {
            DateStorage::Text => TEXT_DATE_FORMS,
            DateStorage::Id3v23Frames => ID3V23_DATE_FORMS,
        };
        let invalid =
            |reason: String| format!("invalid year '{trimmed}' for {}: {reason}", self.name);
        let date = DateParts::parse(trimmed).ok_or_else(|| invalid(format!("use {forms}")))?;
        date.check_calendar().map_err(invalid)?;
        if self.date == DateStorage::Id3v23Frames {
            if !matches!(
                date.precision(),
                Precision::Year | Precision::Day | Precision::Minute
            ) {
                return Err(invalid(format!("it stores only {forms}")));
            }
            if date.year < 1000 {
                return Err(invalid("it cannot store years before 1000".to_string()));
            }
        }
        Ok(trimmed.to_string())
    }

    /// Check an edited track or disc number, or total: empty clears it, and
    /// anything else must be a whole number from 1 to what this tag stores.
    /// Leading zeros are allowed. Where the tag keeps track text, a track or
    /// disc may be any other label, such as `A1` or `Side A`, stored as typed;
    /// a total is a count and stays a number.
    pub(super) fn validated_number(self, field: TagField, value: &str) -> Result<(), String> {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            return Ok(());
        }
        let label = number_label(field);
        let invalid =
            |reason: String| format!("invalid {label} '{trimmed}' for {}: {reason}", self.name);
        let total = total_label(field);
        if let Some(total_label) = total {
            if trimmed.contains('/') {
                return Err(invalid(format!(
                    "enter only the {} number here and the total under {total_label}",
                    label.to_lowercase()
                )));
            }
        }
        let keeps_label = total.is_some() && self.keeps_labels;
        let max = self.max_number;
        if !trimmed.bytes().all(|byte| byte.is_ascii_digit()) {
            if keeps_label {
                return Ok(());
            }
            if total.is_some() && trimmed.chars().any(char::is_alphabetic) {
                return Err(invalid(format!(
                    "it stores only whole numbers, so use one from 1 to {max}, or leave it empty"
                )));
            }
            return Err(invalid(format!(
                "use a whole number from 1 to {max}, or leave it empty"
            )));
        }
        // All digits, so the only parse failure is a number above `u32::MAX`.
        match trimmed.parse::<u32>() {
            Ok(number) if (1..=max).contains(&number) => Ok(()),
            _ if keeps_label => Err(invalid(format!(
                "a number must be from 1 to {max}; any other label, such as A1 or Side A, \
                 is stored as typed"
            ))),
            _ => Err(invalid(format!(
                "use a whole number from 1 to {max}, or leave it empty"
            ))),
        }
    }
}

/// The field's name in the tag editor.
fn number_label(field: TagField) -> &'static str {
    match field {
        TagField::TrackNo => "Track",
        TagField::DiscNo => "Disc",
        TagField::TotalTracks => "Tracks",
        TagField::TotalDiscs => "Discs",
        _ => "number",
    }
}

/// The label of the field that holds the total for `field`.
fn total_label(field: TagField) -> Option<&'static str> {
    match field {
        TagField::TrackNo => Some(number_label(TagField::TotalTracks)),
        TagField::DiscNo => Some(number_label(TagField::TotalDiscs)),
        _ => None,
    }
}

/// A date and time in the extended ISO 8601 form lofty writes: a four-digit
/// year, then optionally `-MM`, `-DD`, `THH`, `:MM`, and `:SS`, each only
/// after the one before it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct DateParts {
    year: u16,
    month: Option<u8>,
    day: Option<u8>,
    hour: Option<u8>,
    minute: Option<u8>,
    second: Option<u8>,
}

impl DateParts {
    fn parse(text: &str) -> Option<Self> {
        let bytes = text.as_bytes();
        let digits = |start: usize, len: usize| -> Option<u16> {
            let part = bytes.get(start..start + len)?;
            part.iter().all(u8::is_ascii_digit).then(|| {
                part.iter()
                    .fold(0_u16, |value, digit| value * 10 + u16::from(*digit - b'0'))
            })
        };
        // Each later part is a separator and two digits at a fixed offset.
        let part = |index: usize| -> Option<Option<u8>> {
            const SEPARATORS: [u8; 5] = [b'-', b'-', b'T', b':', b':'];
            let start = 4 + 3 * index;
            if bytes.len() <= start {
                return Some(None);
            }
            if bytes[start] != SEPARATORS[index] {
                return None;
            }
            let value = digits(start + 1, 2)?;
            u8::try_from(value).ok().map(Some)
        };
        if ![4, 7, 10, 13, 16, 19].contains(&bytes.len()) {
            return None;
        }
        Some(Self {
            year: digits(0, 4)?,
            month: part(0)?,
            day: part(1)?,
            hour: part(2)?,
            minute: part(3)?,
            second: part(4)?,
        })
    }

    fn precision(&self) -> Precision {
        [
            (self.second, Precision::Second),
            (self.minute, Precision::Minute),
            (self.hour, Precision::Hour),
            (self.day, Precision::Day),
            (self.month, Precision::Month),
        ]
        .into_iter()
        .find_map(|(part, precision)| part.map(|_| precision))
        .unwrap_or(Precision::Year)
    }

    /// Check that the date exists, in the proleptic Gregorian calendar.
    fn check_calendar(&self) -> Result<(), String> {
        const MONTHS: [&str; 12] = [
            "January",
            "February",
            "March",
            "April",
            "May",
            "June",
            "July",
            "August",
            "September",
            "October",
            "November",
            "December",
        ];
        if let Some(month) = self.month {
            if !(1..=12).contains(&month) {
                return Err(format!("there is no month {month:02}"));
            }
            if let Some(day) = self.day {
                let days = days_in_month(self.year, month);
                if !(1..=days).contains(&day) {
                    return Err(format!(
                        "{} {:04} has no day {day:02}",
                        MONTHS[usize::from(month - 1)],
                        self.year
                    ));
                }
            }
        }
        for (value, limit, name) in [
            (self.hour, 23, "hour"),
            (self.minute, 59, "minute"),
            (self.second, 59, "second"),
        ] {
            if let Some(value) = value.filter(|value| *value > limit) {
                return Err(format!("there is no {name} {value:02}"));
            }
        }
        Ok(())
    }
}

fn days_in_month(year: u16, month: u8) -> u8 {
    match month {
        2 if year.is_multiple_of(4) && (!year.is_multiple_of(100) || year.is_multiple_of(400)) => {
            29
        }
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const TEXT_TARGETS: [(TagType, bool); 4] = [
        (TagType::Id3v2, false),
        (TagType::Mp4Ilst, false),
        (TagType::VorbisComments, false),
        (TagType::Ape, false),
    ];

    fn id3v23() -> TagTarget {
        TagTarget::for_tag(TagType::Id3v2, true)
    }

    #[test]
    fn text_tags_accept_every_iso_form_and_trim_it() {
        for (tag_type, version) in TEXT_TARGETS {
            let target = TagTarget::for_tag(tag_type, version);
            for value in [
                "",
                "1997",
                " 1997 ",
                "0000",
                "0999",
                "1997-05",
                "1997-05-12",
                "2000-02-29",
                "1996-02-29",
                "1997-12-31T23",
                "1997-05-12T10:30",
                "1997-05-12T00:00:00",
                "1997-05-12T23:59:59",
            ] {
                assert_eq!(
                    target.validated_year(value),
                    Ok(value.trim().to_string()),
                    "{tag_type:?} {value}"
                );
            }
        }
        assert_eq!(
            TagTarget::RAW_APEV2.validated_year("1997-05"),
            Ok("1997-05".to_string())
        );
    }

    #[test]
    fn year_text_must_have_the_iso_shape() {
        for value in [
            "May 1997",
            "1997/05/12",
            "12.05.1997",
            "97",
            "19970",
            "1997-5-12",
            "1997-05-12x",
            "19970512",
            "1997-05-12 10:30",
            "1997-05-12T10:30:45Z",
            "1997-05-12T10:30:45.5",
            "+997",
            "1997-0a",
        ] {
            let error = TagTarget::for_tag(TagType::VorbisComments, false)
                .validated_year(value)
                .expect_err(value);
            assert!(error.contains("for Vorbis comments"), "{error}");
            assert!(error.contains(TEXT_DATE_FORMS), "{error}");
        }
    }

    #[test]
    fn impossible_dates_are_refused_for_every_tag() {
        let mut targets = TEXT_TARGETS
            .iter()
            .map(|(tag_type, version)| TagTarget::for_tag(*tag_type, *version))
            .collect::<Vec<_>>();
        targets.extend([id3v23(), TagTarget::RAW_APEV2]);
        for target in targets {
            for (value, reason) in [
                ("1997-13-01", "there is no month 13"),
                ("1997-00-10", "there is no month 00"),
                ("1997-99-99", "there is no month 99"),
                ("1997-04-31", "April 1997 has no day 31"),
                ("1997-02-29", "February 1997 has no day 29"),
                ("1900-02-29", "February 1900 has no day 29"),
                ("1997-05-00", "May 1997 has no day 00"),
                ("1997-05-12T24:00", "there is no hour 24"),
                ("1997-05-12T10:60", "there is no minute 60"),
                ("1997-05-12T10:30:61", "there is no second 61"),
            ] {
                let error = target.validated_year(value).expect_err(value);
                assert!(error.contains(reason), "{error}");
                assert!(error.contains(target.name), "{error}");
            }
        }
    }

    #[test]
    fn id3v23_accepts_only_what_its_frames_store() {
        for value in ["1997", "1000", "1997-05-12", "2000-02-29T23:59"] {
            assert_eq!(id3v23().validated_year(value), Ok(value.to_string()));
        }
        for value in ["1997-05", "1997-05-12T10", "1997-05-12T10:30:45"] {
            let error = id3v23().validated_year(value).expect_err(value);
            assert_eq!(
                error,
                format!("invalid year '{value}' for ID3v2.3: it stores only {ID3V23_DATE_FORMS}")
            );
        }
        let error = id3v23().validated_year("0999").expect_err("short year");
        assert!(error.contains("before 1000"), "{error}");
    }

    #[test]
    fn numbers_are_positive_whole_numbers_within_the_tag_range() {
        let vorbis = TagTarget::for_tag(TagType::VorbisComments, false);
        let mp4 = TagTarget::for_tag(TagType::Mp4Ilst, false);
        for field in [
            TagField::TrackNo,
            TagField::DiscNo,
            TagField::TotalTracks,
            TagField::TotalDiscs,
        ] {
            for value in ["", "  ", "7", " 07 ", "70000"] {
                assert_eq!(vorbis.validated_number(field, value), Ok(()), "{value}");
            }
            assert_eq!(mp4.validated_number(field, "65535"), Ok(()));
            // Values with letters are covered with the labels below.
            for value in ["0", "00", "-1", "+5", "1.5", "1 2", "4294967296"] {
                assert_eq!(
                    mp4.validated_number(field, value),
                    Err(format!(
                        "invalid {} '{value}' for MP4: use a whole number from 1 to 65535, or \
                         leave it empty",
                        number_label(field)
                    )),
                    "{value}"
                );
            }
            let error = mp4.validated_number(field, "65536").expect_err("too large");
            assert_eq!(
                error,
                format!(
                    "invalid {} '65536' for MP4: use a whole number from 1 to 65535, or leave it empty",
                    number_label(field)
                )
            );
        }
        // An all-digit track or disc in Vorbis comments is still a number.
        for field in [TagField::TrackNo, TagField::DiscNo] {
            for value in ["0", "00", "4294967296"] {
                assert_eq!(
                    vorbis.validated_number(field, value),
                    Err(format!(
                        "invalid {} '{value}' for Vorbis comments: a number must be from 1 to \
                         4294967295; any other label, such as A1 or Side A, is stored as typed",
                        number_label(field)
                    ))
                );
            }
        }
    }

    #[test]
    fn vorbis_comments_keep_any_track_and_disc_label() {
        let vorbis = TagTarget::for_tag(TagType::VorbisComments, false);
        for field in [TagField::TrackNo, TagField::DiscNo] {
            for value in [
                "A", "A1", " B2 ", "b12", "AA1", "A-1", "1A", "A3a", "Side A", "CD1", "1.5", "-1",
            ] {
                assert_eq!(vorbis.validated_number(field, value), Ok(()), "{value}");
            }
        }
        let mut targets = [
            (TagType::Id3v2, false),
            (TagType::Id3v2, true),
            (TagType::Mp4Ilst, false),
            (TagType::Ape, false),
        ]
        .map(|(tag_type, version)| TagTarget::for_tag(tag_type, version))
        .to_vec();
        targets.push(TagTarget::RAW_APEV2);
        for target in targets {
            for value in ["A1", "A-1", "Side A"] {
                assert_eq!(
                    target.validated_number(TagField::TrackNo, value),
                    Err(format!(
                        "invalid Track '{value}' for {}: it stores only whole numbers, so use \
                         one from 1 to {}, or leave it empty",
                        target.name, target.max_number
                    ))
                );
            }
        }
        // A total counts tracks or discs, so it is a number in every tag.
        for field in [TagField::TotalTracks, TagField::TotalDiscs] {
            for value in ["B", "A-1", "1.5"] {
                assert_eq!(
                    vorbis.validated_number(field, value),
                    Err(format!(
                        "invalid {} '{value}' for Vorbis comments: use a whole number from 1 \
                         to 4294967295, or leave it empty",
                        number_label(field)
                    ))
                );
            }
        }
    }

    #[test]
    fn a_number_with_its_total_points_to_the_total_field() {
        let target = TagTarget::for_tag(TagType::Id3v2, false);
        assert_eq!(
            target.validated_number(TagField::TrackNo, "3/12"),
            Err("invalid Track '3/12' for ID3v2.4: enter only the track number here and the total under Tracks"
                .to_string())
        );
        assert_eq!(
            target.validated_number(TagField::DiscNo, "1/2"),
            Err("invalid Disc '1/2' for ID3v2.4: enter only the disc number here and the total under Discs"
                .to_string())
        );
        let error = target
            .validated_number(TagField::TotalTracks, "3/12")
            .expect_err("total with a slash");
        assert!(error.contains("use a whole number"), "{error}");
        let vorbis = TagTarget::for_tag(TagType::VorbisComments, false);
        assert_eq!(
            vorbis.validated_number(TagField::TrackNo, "A/1"),
            Err(
                "invalid Track 'A/1' for Vorbis comments: enter only the track number here and \
                 the total under Tracks"
                    .to_string()
            )
        );
    }
}
