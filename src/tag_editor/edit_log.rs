// SPDX-License-Identifier: GPL-3.0-or-later

//! The diagnostics log lines of tag saves and renames: one line for each
//! save or rename the user makes, and one for each file that failed.

use std::collections::HashSet;
use std::path::Path;

use crate::frontend_bridge::{log_info, log_warning, quoted_path, ErrorCategory};

use super::{RenameResultRow, SaveResultRow, TagField};

/// The most files a line lists by path; the rest are counted.
const LISTED_FILES: usize = 3;

/// The most renames logged one line each; the rest are counted.
const LISTED_RENAMES: usize = 20;

fn field_name(field: TagField) -> &'static str {
    match field {
        TagField::Title => "title",
        TagField::Artist => "artist",
        TagField::Album => "album",
        TagField::AlbumArtist => "album artist",
        TagField::Genre => "genre",
        TagField::Year => "year",
        TagField::TrackNo => "track number",
        TagField::DiscNo => "disc number",
        TagField::TotalTracks => "total tracks",
        TagField::TotalDiscs => "total discs",
        TagField::Comment => "comment",
    }
}

/// The first [`LISTED_FILES`] of `paths`, quoted, and how many more there
/// are.
fn listed(paths: &[&str]) -> String {
    let text = paths
        .iter()
        .take(LISTED_FILES)
        .map(|path| quoted_path(Path::new(path)))
        .collect::<Vec<_>>()
        .join(", ");
    match paths.len().saturating_sub(LISTED_FILES) {
        0 => text,
        more => format!("{text} and {more} more"),
    }
}

fn files(count: usize) -> String {
    if count == 1 {
        "1 file".to_string()
    } else {
        format!("{count} files")
    }
}

/// Logs a save of the tags `fields` of the files in `results`.
pub(super) fn log_tag_save(results: &[SaveResultRow], fields: &HashSet<TagField>) {
    if results.is_empty() {
        return;
    }
    let saved: Vec<&str> = results
        .iter()
        .filter(|result| result.ok)
        .map(|result| result.path.as_str())
        .collect();
    let mut names: Vec<&str> = TagField::ALL
        .into_iter()
        .filter(|field| fields.contains(field))
        .map(field_name)
        .collect();
    if names.len() == TagField::ALL.len() {
        names = vec!["all fields"];
    }
    if !saved.is_empty() {
        log_info(
            ErrorCategory::Tags,
            &format!(
                "Saved the tags of {} ({}): {}.",
                files(saved.len()),
                names.join(", "),
                listed(&saved)
            ),
        );
    }
    for failed in results.iter().filter(|result| !result.ok) {
        log_warning(
            ErrorCategory::Tags,
            &format!(
                "Could not save the tags of {}: {}",
                quoted_path(Path::new(&failed.path)),
                failed.error.as_deref().unwrap_or("unknown error")
            ),
        );
    }
}

/// Logs the renames in `results`; rows the rename left as they were log
/// nothing.
pub(super) fn log_renames(results: &[RenameResultRow]) {
    let mut renamed = 0usize;
    for result in results {
        let new_path = result.new_path.as_deref().unwrap_or_default();
        if !result.ok {
            log_warning(
                ErrorCategory::Tags,
                &format!(
                    "Could not rename {}: {}",
                    quoted_path(Path::new(&result.path)),
                    result.error.as_deref().unwrap_or("unknown error")
                ),
            );
            continue;
        }
        if new_path == result.path {
            continue;
        }
        renamed += 1;
        if renamed <= LISTED_RENAMES {
            log_info(
                ErrorCategory::Tags,
                &format!(
                    "Renamed {} to {}.",
                    quoted_path(Path::new(&result.path)),
                    quoted_path(Path::new(new_path))
                ),
            );
        }
    }
    if renamed > LISTED_RENAMES {
        log_info(
            ErrorCategory::Tags,
            &match renamed - LISTED_RENAMES {
                1 => "Renamed 1 more file.".to_string(),
                more => format!("Renamed {more} more files."),
            },
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend_bridge::test_log;

    fn saved(path: &str, ok: bool) -> SaveResultRow {
        SaveResultRow {
            path: path.to_string(),
            ok,
            error: (!ok).then(|| "the file is read-only".to_string()),
        }
    }

    #[test]
    fn a_tag_save_writes_one_line_and_one_for_each_failure() {
        test_log::clear();
        let results: Vec<SaveResultRow> = (1..=5)
            .map(|n| saved(&format!("/fixture/library/{n:02}.flac"), n != 5))
            .collect();
        log_tag_save(
            &results,
            &[TagField::Artist, TagField::Album].into_iter().collect(),
        );
        assert_eq!(
            test_log::all(),
            [
                "[ferrous] [tags] info: Saved the tags of 4 files (artist, album): \
                 \"/fixture/library/01.flac\", \"/fixture/library/02.flac\", \
                 \"/fixture/library/03.flac\" and 1 more.",
                "[ferrous] [tags] warning: Could not save the tags of \
                 \"/fixture/library/05.flac\": the file is read-only",
            ]
        );
    }

    #[test]
    fn each_rename_writes_a_line_up_to_a_limit() {
        test_log::clear();
        let mut results: Vec<RenameResultRow> = (0..LISTED_RENAMES + 2)
            .map(|n| RenameResultRow {
                path: format!("/fixture/library/{n}.flac"),
                new_path: Some(format!("/fixture/library/{n:02} Title.flac")),
                ok: true,
                error: None,
            })
            .collect();
        // Left as it was: nothing to log.
        results.push(RenameResultRow {
            path: "/fixture/library/kept.flac".to_string(),
            new_path: Some("/fixture/library/kept.flac".to_string()),
            ok: true,
            error: None,
        });
        log_renames(&results);
        let lines = test_log::all();
        assert_eq!(lines.len(), LISTED_RENAMES + 1, "{lines:?}");
        assert_eq!(
            lines[0],
            "[ferrous] [tags] info: Renamed \"/fixture/library/0.flac\" to \
             \"/fixture/library/00 Title.flac\"."
        );
        assert_eq!(
            lines[LISTED_RENAMES],
            "[ferrous] [tags] info: Renamed 2 more files."
        );
    }
}
