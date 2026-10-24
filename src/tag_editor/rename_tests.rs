// SPDX-License-Identifier: GPL-3.0-or-later

//! Renames of indexed files through `rename_rows`, against the test process's
//! library database.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use rusqlite::{params, Connection};

use super::{load_rows_for_paths, rename_rows, RenameRequest, RenameResponse};
use crate::library::index_tracks_for_test;
use crate::tag_write::write_test_flac;

/// `(rowid, path, root_path, title, track_no)` for the rows under one root.
type TrackRow = (i64, PathBuf, PathBuf, String, Option<i64>);

fn fixture_dir(name: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos());
    let dir = std::env::temp_dir().join(format!(
        "ferrous-tag-editor-rename-{name}-{}-{nanos}",
        std::process::id()
    ));
    fs::create_dir_all(&dir).expect("create test dir");
    dir.canonicalize().expect("canonical test dir")
}

/// Writes `(file name, title, track number)` fixtures and indexes them.
fn indexed_fixtures(dir: &Path, files: &[(&str, &str, &str)]) -> Vec<PathBuf> {
    let paths = files
        .iter()
        .map(|(file_name, title, track_no)| {
            let path = dir.join(file_name);
            write_test_flac(&path, &[("TITLE", title), ("TRACKNUMBER", track_no)]);
            path
        })
        .collect::<Vec<_>>();
    index_tracks_for_test(dir, &paths);
    paths
}

fn library_db() -> Connection {
    Connection::open(crate::paths::library_db_path().expect("test library path"))
        .expect("test library db")
}

fn track_rows(root: &Path) -> Vec<TrackRow> {
    let conn = library_db();
    let mut stmt = conn
        .prepare(
            "SELECT rowid, path, root_path, title, track_no FROM tracks \
             WHERE root_path = ?1 ORDER BY path",
        )
        .expect("prepare");
    stmt.query_map(params![root.to_string_lossy()], |row| {
        Ok((
            row.get(0)?,
            PathBuf::from(row.get::<_, String>(1)?),
            PathBuf::from(row.get::<_, String>(2)?),
            row.get(3)?,
            row.get(4)?,
        ))
    })
    .expect("query")
    .collect::<Result<_, _>>()
    .expect("rows")
}

fn rename_paths(paths: &[PathBuf]) -> RenameResponse {
    let response = load_rows_for_paths(paths);
    assert!(response.failures.is_empty(), "{:?}", response.failures);
    rename_rows(RenameRequest {
        rows: response.rows,
    })
}

fn file_names(dir: &Path) -> Vec<String> {
    let mut names = fs::read_dir(dir)
        .expect("read dir")
        .map(|entry| {
            entry
                .expect("entry")
                .file_name()
                .to_string_lossy()
                .into_owned()
        })
        .collect::<Vec<_>>();
    names.sort();
    names
}

fn assert_renamed(response: &RenameResponse, expected: &[PathBuf]) {
    assert!(
        response.results.iter().all(|result| result.ok),
        "{:?}",
        response.results
    );
    let mut successful = response.successful_paths.clone();
    successful.sort();
    let mut expected = expected
        .iter()
        .map(|path| path.to_string_lossy().into_owned())
        .collect::<Vec<_>>();
    expected.sort();
    assert_eq!(successful, expected);
}

#[test]
fn renaming_an_indexed_track_moves_its_row() {
    let dir = fixture_dir("plain");
    let paths = indexed_fixtures(&dir, &[("01 - Old.flac", "New", "1")]);
    let bytes = fs::read(&paths[0]).expect("read");
    let before = track_rows(&dir);
    let renamed = dir.join("01 - New.flac");

    let response = rename_paths(&paths);

    assert_renamed(&response, std::slice::from_ref(&renamed));
    assert_eq!(fs::read(&renamed).expect("read"), bytes);
    assert_eq!(file_names(&dir), ["01 - New.flac"]);
    assert_eq!(
        track_rows(&dir),
        [(
            before[0].0,
            renamed,
            dir.clone(),
            "New".to_string(),
            Some(1)
        )]
    );
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn swapping_two_indexed_tracks_keeps_files_and_rows_consistent() {
    let dir = fixture_dir("swap");
    // Each file is named after the other's tags.
    let paths = indexed_fixtures(
        &dir,
        &[("01 - One.flac", "Two", "2"), ("02 - Two.flac", "One", "1")],
    );
    let bytes = paths
        .iter()
        .map(|path| fs::read(path).expect("read"))
        .collect::<Vec<_>>();
    let before = track_rows(&dir);

    let response = rename_paths(&paths);

    assert_renamed(&response, &paths);
    assert_eq!(file_names(&dir), ["01 - One.flac", "02 - Two.flac"]);
    assert_eq!(fs::read(&paths[0]).expect("read"), bytes[1]);
    assert_eq!(fs::read(&paths[1]).expect("read"), bytes[0]);
    // Each row follows its file and now matches the tags under its path.
    assert_eq!(
        track_rows(&dir),
        [
            (
                before[1].0,
                paths[0].clone(),
                dir.clone(),
                "One".to_string(),
                Some(1)
            ),
            (
                before[0].0,
                paths[1].clone(),
                dir.clone(),
                "Two".to_string(),
                Some(2)
            ),
        ]
    );
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn renumbering_a_chain_of_indexed_tracks_moves_each_row_once() {
    let dir = fixture_dir("chain");
    let paths = indexed_fixtures(
        &dir,
        &[
            ("01 - Song.flac", "Song", "2"),
            ("02 - Song.flac", "Song", "3"),
            ("03 - Song.flac", "Song", "4"),
        ],
    );
    let before = track_rows(&dir);
    let renamed = ["02 - Song.flac", "03 - Song.flac", "04 - Song.flac"].map(|name| dir.join(name));

    let response = rename_paths(&paths);

    assert_renamed(&response, &renamed);
    assert_eq!(
        file_names(&dir),
        ["02 - Song.flac", "03 - Song.flac", "04 - Song.flac"]
    );
    let expected = before
        .iter()
        .zip(&renamed)
        .zip(2..)
        .map(|((row, path), track_no)| {
            (
                row.0,
                path.clone(),
                dir.clone(),
                "Song".to_string(),
                Some(track_no),
            )
        })
        .collect::<Vec<_>>();
    assert_eq!(track_rows(&dir), expected);
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn a_failed_database_update_puts_swapped_files_back() {
    let dir = fixture_dir("swap-failure");
    let paths = indexed_fixtures(
        &dir,
        &[("01 - One.flac", "Two", "2"), ("02 - Two.flac", "One", "1")],
    );
    let bytes = paths
        .iter()
        .map(|path| fs::read(path).expect("read"))
        .collect::<Vec<_>>();
    let before = track_rows(&dir);
    // Fails only the update that gives a row this fixture's second path.
    library_db()
        .execute(
            &format!(
                "CREATE TRIGGER rename_tests_fail_swap BEFORE UPDATE OF path ON tracks \
                 WHEN new.path = '{}' BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                paths[1].to_string_lossy()
            ),
            [],
        )
        .expect("failure trigger");

    let response = rename_paths(&paths);

    library_db()
        .execute("DROP TRIGGER rename_tests_fail_swap", [])
        .expect("drop failure trigger");
    assert!(response.successful_paths.is_empty());
    assert_eq!(response.results.len(), 2);
    for result in &response.results {
        assert!(!result.ok, "{result:?}");
        let error = result.error.as_deref().unwrap_or_default();
        assert!(error.contains("injected failure"), "{error}");
        assert!(
            error.ends_with("the files keep their original names"),
            "{error}"
        );
    }
    assert_eq!(file_names(&dir), ["01 - One.flac", "02 - Two.flac"]);
    for (path, original) in paths.iter().zip(&bytes) {
        assert_eq!(&fs::read(path).expect("read"), original);
    }
    assert_eq!(track_rows(&dir), before);
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn a_different_file_whose_name_differs_only_in_case_is_not_replaced() {
    let dir = fixture_dir("case-collision");
    let paths = indexed_fixtures(
        &dir,
        &[
            ("01 - Song.flac", "song", "1"),
            ("01 - song.flac", "Other", "2"),
        ],
    );
    if file_names(&dir).len() != 2 {
        eprintln!("skipping: {} is case-insensitive", dir.display());
        fs::remove_dir_all(dir).expect("cleanup");
        return;
    }
    let bytes = paths
        .iter()
        .map(|path| fs::read(path).expect("read"))
        .collect::<Vec<_>>();
    let before = track_rows(&dir);

    // Renaming the first file to its tags' name would replace the second.
    let response = rename_paths(&paths[..1]);

    assert!(response.successful_paths.is_empty());
    assert_eq!(response.results.len(), 1);
    let result = &response.results[0];
    assert!(!result.ok, "{result:?}");
    assert_eq!(
        result.error.as_deref(),
        Some(format!("target already exists: {}", paths[1].to_string_lossy()).as_str())
    );
    assert_eq!(file_names(&dir), ["01 - Song.flac", "01 - song.flac"]);
    for (path, original) in paths.iter().zip(&bytes) {
        assert_eq!(&fs::read(path).expect("read"), original);
    }
    assert_eq!(track_rows(&dir), before);
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn a_case_only_rename_of_one_file_still_works() {
    let dir = fixture_dir("case-only");
    let paths = indexed_fixtures(&dir, &[("01 - song.flac", "Song", "1")]);
    let bytes = fs::read(&paths[0]).expect("read");
    let before = track_rows(&dir);
    let renamed = dir.join("01 - Song.flac");

    let response = rename_paths(&paths);

    assert_renamed(&response, std::slice::from_ref(&renamed));
    assert_eq!(file_names(&dir), ["01 - Song.flac"]);
    assert_eq!(fs::read(&renamed).expect("read"), bytes);
    assert_eq!(
        track_rows(&dir),
        [(
            before[0].0,
            renamed,
            dir.clone(),
            "Song".to_string(),
            Some(1)
        )]
    );
    fs::remove_dir_all(dir).expect("cleanup");
}

#[test]
fn a_dangling_symlink_at_the_target_is_not_replaced() {
    let dir = fixture_dir("dangling-target");
    let paths = indexed_fixtures(&dir, &[("01 - Old.flac", "New", "1")]);
    let bytes = fs::read(&paths[0]).expect("read");
    let target = dir.join("01 - New.flac");
    std::os::unix::fs::symlink(dir.join("missing.flac"), &target).expect("symlink");

    let response = rename_paths(&paths);

    assert!(response.successful_paths.is_empty());
    assert_eq!(
        response.results[0].error.as_deref(),
        Some(format!("target already exists: {}", target.to_string_lossy()).as_str())
    );
    assert_eq!(fs::read(&paths[0]).expect("read"), bytes);
    assert!(fs::symlink_metadata(&target).expect("link").is_symlink());
    fs::remove_dir_all(dir).expect("cleanup");
}
