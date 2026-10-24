// SPDX-License-Identifier: GPL-3.0-or-later

//! Year and number edits saved into each tag format: accepted values read
//! back as entered, and refused ones leave the file byte for byte unchanged.

use std::fs;
use std::path::{Path, PathBuf};

use lofty::ape::{ApeItem, ApeTag};
use lofty::config::WriteOptions;
use lofty::mp4::{Atom, AtomData, AtomIdent, Ilst};
use lofty::ogg::VorbisComments;
use lofty::prelude::TagExt;
use lofty::tag::{ItemKey, ItemValue, Tag, TagType};

use super::tests::{dir_entries, edited, load_one, save_json, test_dir};
use super::{SaveResponse, TagEditorRow};
use crate::tag_write::{
    write_test_flac, write_test_monkeys_audio, write_test_mp3_with_id3v23, write_test_mp4,
    write_test_opus, write_test_wavpack, TEST_MP3,
};

/// A file in one tag format, holding a title.
struct Format {
    file_name: &'static str,
    /// The name errors give the tag.
    tag_name: &'static str,
    write: fn(&Path),
}

fn write_id3v24(path: &Path) {
    fs::write(path, TEST_MP3).expect("write MP3");
    let mut tag = Tag::new(TagType::Id3v2);
    tag.insert_text(ItemKey::TrackTitle, "Old".to_string());
    tag.save_to_path(path, WriteOptions::new())
        .expect("write ID3v2.4 tag");
    assert_eq!(&fs::read(path).expect("read")[..4], b"ID3\x04");
}

fn write_mp4(path: &Path) {
    let mut ilst = Ilst::default();
    ilst.insert(Atom::new(
        AtomIdent::Fourcc(*b"\xa9nam"),
        AtomData::UTF8("Old".to_string()),
    ));
    write_test_mp4(path, &ilst);
}

fn write_opus(path: &Path) {
    let mut comments = VorbisComments::new();
    comments.push("TITLE".to_string(), "Old".to_string());
    write_test_opus(path, &comments);
}

fn ape_tag() -> ApeTag {
    let mut tag = ApeTag::new();
    tag.insert(
        ApeItem::new("Title".to_string(), ItemValue::Text("Old".to_string())).expect("APE item"),
    );
    tag
}

const ID3V23: Format = Format {
    file_name: "song.mp3",
    tag_name: "ID3v2.3",
    write: write_test_mp3_with_id3v23,
};
const ID3V24: Format = Format {
    file_name: "song.mp3",
    tag_name: "ID3v2.4",
    write: write_id3v24,
};
const MP4: Format = Format {
    file_name: "song.m4a",
    tag_name: "MP4",
    write: write_mp4,
};
const FLAC: Format = Format {
    file_name: "song.flac",
    tag_name: "Vorbis comments",
    write: |path| write_test_flac(path, &[("TITLE", "Old")]),
};
const OPUS: Format = Format {
    file_name: "song.opus",
    tag_name: "Vorbis comments",
    write: write_opus,
};
const WAVPACK: Format = Format {
    file_name: "song.wv",
    tag_name: "APEv2",
    write: |path| write_test_wavpack(path, &ape_tag()),
};
const MONKEYS_AUDIO: Format = Format {
    file_name: "song.ape",
    tag_name: "APEv2",
    write: |path| write_test_monkeys_audio(path, &ape_tag()),
};
const RAW_AC3: Format = Format {
    file_name: "song.ac3",
    tag_name: "APEv2",
    write: |path| crate::raw_audio::write_test_apev2_file(path, &[("Title", "Old")], true),
};

const FORMATS: [Format; 8] = [
    ID3V23,
    ID3V24,
    MP4,
    FLAC,
    OPUS,
    WAVPACK,
    MONKEYS_AUDIO,
    RAW_AC3,
];

/// Every form the tag editor accepts, which every tag but `ID3v2.3` stores.
const ISO_DATES: [&str; 8] = [
    "1997",
    "1997-05",
    "1997-05-12",
    "1997-05-12T10",
    "1997-05-12T10:30",
    "1997-05-12T10:30:45",
    "2000-02-29",
    "0999",
];
const ID3V23_DATES: [&str; 4] = ["1997", "1997-05-12", "1997-05-12T10:30", "2000-02-29"];
const ID3V23_ONLY_REFUSED: [&str; 4] = ["1997-05", "1997-05-12T10", "1997-05-12T10:30:45", "0999"];
const REFUSED_DATES: [&str; 9] = [
    "1997-13-01",
    "1997-99-99",
    "1997-04-31",
    "1997-02-29",
    "1997-05-00",
    "1997-05-12T24:00",
    "1997-05-12T10:30:60",
    "May 1997",
    "19970512",
];

/// A new file of `format` in its own directory, loaded as the editor shows it.
fn fixture(format: &Format, name: &str) -> (PathBuf, TagEditorRow) {
    let dir = test_dir(name);
    let path = dir.join(format.file_name);
    (format.write)(&path);
    let row = load_one(&path);
    (path, row)
}

fn save_field(row: &TagEditorRow, field: &str) -> SaveResponse {
    save_json(&[edited(row, &[field])])
}

/// Saves `edit` into a new file of `format` and checks that it is refused
/// with an error naming the tag, and that the file is unchanged.
fn assert_refused(format: &Format, field: &str, edit: impl FnOnce(&mut TagEditorRow)) -> String {
    let (path, mut row) = fixture(format, "refused");
    let before = fs::read(&path).expect("read fixture");
    edit(&mut row);

    let response = save_field(&row, field);

    assert!(
        response.successful_paths.is_empty(),
        "{:?}",
        response.results
    );
    assert!(!response.results[0].ok);
    let error = response.results[0].error.clone().unwrap_or_default();
    assert!(
        error.contains(&format!("for {}:", format.tag_name)),
        "{}: {error}",
        format.file_name
    );
    assert_eq!(fs::read(&path).expect("read result"), before, "{error}");
    let dir = path.parent().expect("test dir");
    assert_eq!(dir_entries(dir), [format.file_name]);
    fs::remove_dir_all(dir).expect("remove test dir");
    error
}

/// Saves `edit` into a new file of `format` and returns the row read back.
fn saved(format: &Format, field: &str, edit: impl FnOnce(&mut TagEditorRow)) -> TagEditorRow {
    let (path, mut row) = fixture(format, "accepted");
    edit(&mut row);

    let response = save_field(&row, field);

    assert!(
        response.results.iter().all(|result| result.ok),
        "{}: {:?}",
        format.file_name,
        response.results
    );
    let reloaded = load_one(&path);
    assert_eq!(reloaded.title, "Old", "{}", format.file_name);
    fs::remove_dir_all(path.parent().expect("test dir")).expect("remove test dir");
    reloaded
}

#[test]
fn accepted_years_read_back_exactly_in_every_format() {
    for format in &FORMATS {
        let dates: &[&str] = if format.tag_name == "ID3v2.3" {
            &ID3V23_DATES
        } else {
            &ISO_DATES
        };
        for date in dates {
            let row = saved(format, "year", |row| row.year = (*date).to_string());
            assert_eq!(row.year, *date, "{}", format.file_name);
        }
    }
}

#[test]
fn refused_years_leave_every_format_unchanged() {
    for format in &FORMATS {
        for date in REFUSED_DATES {
            let error = assert_refused(format, "year", |row| row.year = date.to_string());
            assert!(
                error.starts_with(&format!("invalid year '{date}'")),
                "{error}"
            );
        }
    }
    for date in ID3V23_ONLY_REFUSED {
        let error = assert_refused(&ID3V23, "year", |row| row.year = date.to_string());
        assert!(error.contains("YYYY, YYYY-MM-DD, or YYYY-MM-DDTHH:MM") || error.contains("1000"));
    }
}

#[test]
fn a_mixed_selection_saves_each_file_that_can_store_the_year() {
    let dir = test_dir("mixed-year");
    let mut rows = Vec::new();
    for format in [&ID3V23, &FLAC, &MP4] {
        let path = dir.join(format!(
            "{}-{}",
            format.tag_name.replace(' ', "-"),
            format.file_name
        ));
        (format.write)(&path);
        let mut row = load_one(&path);
        row.year = "1997-05".to_string();
        rows.push(edited(&row, &["year"]));
    }
    let id3v23_path = dir.join("ID3v2.3-song.mp3");
    let before = fs::read(&id3v23_path).expect("read fixture");

    let response = save_json(&rows);

    let failures = response
        .results
        .iter()
        .filter(|result| !result.ok)
        .collect::<Vec<_>>();
    assert_eq!(failures.len(), 1, "{:?}", response.results);
    assert_eq!(failures[0].path, id3v23_path.to_string_lossy());
    assert_eq!(
        failures[0].error.as_deref(),
        Some(
            "invalid year '1997-05' for ID3v2.3: it stores only YYYY, YYYY-MM-DD, or \
             YYYY-MM-DDTHH:MM"
        )
    );
    assert_eq!(fs::read(&id3v23_path).expect("read result"), before);
    assert_eq!(response.successful_paths.len(), 2);
    for path in &response.successful_paths {
        assert_eq!(load_one(Path::new(path)).year, "1997-05");
    }
    fs::remove_dir_all(dir).expect("remove test dir");
}

#[test]
fn refused_numbers_leave_every_format_unchanged() {
    for format in &FORMATS {
        // Vorbis comments keep any other track text as a label.
        let not_numbers = if KEEPS_LABELS
            .iter()
            .any(|keeps| keeps.file_name == format.file_name)
        {
            [].as_slice()
        } else {
            [("trackNo", "1.5"), ("trackNo", "-1")].as_slice()
        };
        for &(field, value) in [
            ("trackNo", "3/12"),
            ("trackNo", "0"),
            ("trackNo", "4294967296"),
            ("discNo", "1/2"),
            ("discNo", "00"),
            ("totalTracks", "x"),
            ("totalTracks", "B"),
            ("totalDiscs", "0"),
        ]
        .iter()
        .chain(not_numbers)
        {
            let error = assert_refused(format, field, |row| match field {
                "trackNo" => row.track_no = value.to_string(),
                "discNo" => row.disc_no = value.to_string(),
                "totalTracks" => row.total_tracks = value.to_string(),
                _ => row.total_discs = value.to_string(),
            });
            assert!(error.contains(&format!("'{value}'")), "{error}");
        }
    }
    let error = assert_refused(&MP4, "trackNo", |row| row.track_no = "65536".to_string());
    assert!(error.contains("from 1 to 65535"), "{error}");
}

#[test]
fn accepted_numbers_read_back_in_every_format() {
    for format in &FORMATS {
        let row = saved(format, "trackNo", |row| row.track_no = "07".to_string());
        assert_eq!(row.track_no, "07", "{}", format.file_name);
        let row = saved(format, "totalTracks", |row| {
            row.total_tracks = "12".to_string();
        });
        assert_eq!(row.total_tracks, "12", "{}", format.file_name);
        let row = saved(format, "discNo", |row| row.disc_no = "2".to_string());
        assert_eq!(row.disc_no, "2", "{}", format.file_name);
    }
}

#[test]
fn an_empty_number_clears_it() {
    for format in &FORMATS {
        let (path, mut row) = fixture(format, "clear-number");
        row.track_no = "3".to_string();
        assert!(
            save_field(&row, "trackNo").results[0].ok,
            "{}",
            format.file_name
        );
        let mut row = load_one(&path);
        assert_eq!(row.track_no, "03", "{}", format.file_name);

        row.track_no = " ".to_string();
        assert!(
            save_field(&row, "trackNo").results[0].ok,
            "{}",
            format.file_name
        );
        assert_eq!(load_one(&path).track_no, "", "{}", format.file_name);
        fs::remove_dir_all(path.parent().expect("test dir")).expect("remove test dir");
    }
}

/// The formats whose tag keeps any track text, such as `A1` or `Side A`, exactly.
const KEEPS_LABELS: [Format; 2] = [FLAC, OPUS];
/// The formats whose tag would drop a label: lofty reads no number back.
const DROPS_LABELS: [Format; 6] = [ID3V23, ID3V24, MP4, WAVPACK, MONKEYS_AUDIO, RAW_AC3];

#[test]
fn track_and_disc_labels_read_back_exactly_in_vorbis_comments() {
    for format in &KEEPS_LABELS {
        for label in ["A1", "B2", "A-1", "1A", "A3a", "Side A", "CD1"] {
            let row = saved(format, "trackNo", |row| row.track_no = label.to_string());
            assert_eq!(row.track_no, label, "{}", format.file_name);
        }
        for label in ["A", "Side B"] {
            let row = saved(format, "discNo", |row| row.disc_no = label.to_string());
            assert_eq!(row.disc_no, label, "{}", format.file_name);
        }
    }
}

#[test]
fn vorbis_comments_still_refuse_a_slash_or_an_out_of_range_number() {
    for format in &KEEPS_LABELS {
        let error = assert_refused(format, "trackNo", |row| row.track_no = "3/12".to_string());
        assert_eq!(
            error,
            "invalid Track '3/12' for Vorbis comments: enter only the track number here and the \
             total under Tracks"
        );
        for value in ["0", "4294967296"] {
            let error = assert_refused(format, "trackNo", |row| row.track_no = value.to_string());
            assert_eq!(
                error,
                format!(
                    "invalid Track '{value}' for Vorbis comments: a number must be from 1 to \
                     4294967295; any other label, such as A1 or Side A, is stored as typed"
                )
            );
        }
    }
}

#[test]
fn labels_are_refused_where_the_tag_drops_them() {
    for format in &DROPS_LABELS {
        for label in ["A1", "B2", "A-1", "Side A"] {
            let error = assert_refused(format, "trackNo", |row| row.track_no = label.to_string());
            assert!(
                error.starts_with(&format!(
                    "invalid Track '{label}' for {}: it stores only whole numbers, so use one from 1 to ",
                    format.tag_name
                )),
                "{error}"
            );
        }
        let error = assert_refused(format, "discNo", |row| row.disc_no = "A".to_string());
        assert!(error.contains("stores only whole numbers"), "{error}");
    }
}

#[test]
fn a_mixed_selection_saves_a_label_only_where_the_tag_keeps_it() {
    let dir = test_dir("mixed-label");
    let mut rows = Vec::new();
    let mut files = Vec::new();
    let formats = KEEPS_LABELS
        .iter()
        .map(|format| (format, true))
        .chain(DROPS_LABELS.iter().map(|format| (format, false)));
    for (index, (format, keeps)) in formats.enumerate() {
        let path = dir.join(format!("{index}-{}", format.file_name));
        (format.write)(&path);
        let mut row = load_one(&path);
        row.track_no = "A-1".to_string();
        rows.push(edited(&row, &["trackNo"]));
        let bytes = fs::read(&path).expect("read fixture");
        files.push((path, keeps, bytes));
    }

    let response = save_json(&rows);

    assert_eq!(response.results.len(), files.len());
    for ((path, keeps, before), result) in files.iter().zip(&response.results) {
        assert_eq!(result.path, path.to_string_lossy());
        assert_eq!(result.ok, *keeps, "{result:?}");
        if *keeps {
            assert_eq!(load_one(path).track_no, "A-1", "{}", path.display());
        } else {
            let error = result.error.as_deref().unwrap_or_default();
            assert!(error.contains("stores only whole numbers"), "{error}");
            assert_eq!(&fs::read(path).expect("read result"), before, "{error}");
        }
    }
    assert_eq!(response.successful_paths.len(), KEEPS_LABELS.len());
    fs::remove_dir_all(dir).expect("remove test dir");
}
