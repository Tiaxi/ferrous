// SPDX-License-Identifier: GPL-3.0-or-later

//! Real saves and renames that fail, and the cause the results send beside
//! their unchanged message.

use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde_json::Value;

use super::{
    load_rows_for_paths, rename_rows, save_rows, serialize_rename_response,
    serialize_save_response, RenameRequest, RenameResponse, SaveRequest, SaveResponse, SaveRow,
    TagField,
};
use crate::edit_failure::FailureCause;
use crate::tag_write::write_test_flac;

/// A folder removed with what it holds, whatever the test took from it.
struct Fixture {
    dir: PathBuf,
    restricted: Vec<(PathBuf, fs::Permissions)>,
}

impl Fixture {
    fn new(name: &str) -> Self {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-failure-cause-{name}-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        Self {
            dir: dir.canonicalize().expect("canonical test dir"),
            restricted: Vec::new(),
        }
    }

    /// A track at `name` in the folder: FLAC, or the raw file its extension
    /// says.
    fn track(&self, name: &str, title: &str, number: &str) -> PathBuf {
        let path = self.dir.join(name);
        fs::create_dir_all(path.parent().expect("folder")).expect("create folder");
        if path.extension().is_some_and(|extension| extension == "ac3") {
            fs::write(&path, [0_u8; 128]).expect("write raw track");
        } else {
            write_test_flac(&path, &[("TITLE", title), ("TRACKNUMBER", number)]);
        }
        path
    }

    fn restrict(&mut self, path: &Path, mode: u32) {
        let before = fs::metadata(path).expect("metadata").permissions();
        fs::set_permissions(path, fs::Permissions::from_mode(mode)).expect("chmod");
        self.restricted.push((path.to_path_buf(), before));
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        for (path, permissions) in self.restricted.drain(..).rev() {
            let _ = fs::set_permissions(path, permissions);
        }
        let _ = fs::remove_dir_all(&self.dir);
    }
}

/// Whether the permissions a test sets hold for this user. They don't for
/// root, who can't make these failures.
fn permissions_hold() -> bool {
    let mut probe = Fixture::new("probe");
    let dir = probe.dir.clone();
    probe.restrict(&dir, 0o555);
    let hold = fs::write(dir.join("probe"), b"").is_err();
    if !hold {
        eprintln!("skipping: this user may write whatever the permissions say");
    }
    hold
}

fn denied() -> io::Error {
    io::Error::from_raw_os_error(libc::EACCES)
}

/// Saves a new title to `path`, after `restrict` took what the save needs.
fn failed_save(path: &Path, restrict: impl FnOnce()) -> SaveResponse {
    let mut rows = load_rows_for_paths(&[path.to_path_buf()]).rows;
    let mut row = rows.pop().expect("loaded row");
    row.title = "New".to_string();
    let before = fs::read(path).expect("read track");
    restrict();
    let response = save_rows(SaveRequest {
        rows: vec![SaveRow {
            row,
            changed_fields: Some(vec![TagField::Title]),
        }],
    });
    assert!(!response.results[0].ok);
    if let Ok(after) = fs::read(path) {
        assert_eq!(after, before, "a failed save changed the file");
    }
    response
}

fn sent(response: &SaveResponse) -> Value {
    let bytes = serialize_save_response(response).expect("serialize");
    serde_json::from_slice::<Value>(&bytes).expect("json")["results"][0].clone()
}

fn rename(paths: &[PathBuf]) -> RenameResponse {
    let loaded = load_rows_for_paths(paths);
    assert!(loaded.failures.is_empty(), "{:?}", loaded.failures);
    rename_rows(RenameRequest {
        rows: loaded.rows,
        planned_renames: None,
    })
}

#[test]
fn a_read_only_file_is_the_files_permission() {
    if !permissions_hold() {
        return;
    }
    for name in ["song.flac", "song.ac3"] {
        let mut fixture = Fixture::new("file");
        let path = fixture.track(name, "Old", "1");
        let response = failed_save(&path, || fixture.restrict(&path, 0o444));

        let result = sent(&response);
        assert_eq!(result["cause"], "fileNotWritable", "{name}");
        assert_eq!(
            result["error"],
            format!(
                "failed to open {} for writing: {}",
                path.display(),
                denied()
            )
        );
    }
}

#[test]
fn a_read_only_folder_is_the_folders_permission() {
    if !permissions_hold() {
        return;
    }
    for name in ["album/song.flac", "album/song.ac3"] {
        let mut fixture = Fixture::new("folder");
        let path = fixture.track(name, "Old", "1");
        let folder = fixture.dir.join("album");
        let response = failed_save(&path, || fixture.restrict(&folder, 0o555));

        let result = sent(&response);
        assert_eq!(result["cause"], "folderNotWritable", "{name}");
        assert_eq!(
            result["error"],
            format!(
                "failed to create temporary file in {}: {}",
                folder.display(),
                denied()
            )
        );
    }
}

#[test]
fn an_unreadable_file_is_the_permission_to_read_it() {
    if !permissions_hold() {
        return;
    }
    let mut fixture = Fixture::new("unreadable");
    let path = fixture.track("song.flac", "Old", "1");
    let response = failed_save(&path, || fixture.restrict(&path, 0o000));

    let result = sent(&response);
    assert_eq!(result["cause"], "fileNotReadable");
    assert_eq!(
        result["error"],
        format!(
            "failed to read tag data for {}: {}",
            path.display(),
            denied()
        )
    );
}

#[test]
fn a_failure_of_unknown_cause_sends_none() {
    let fixture = Fixture::new("unknown");
    let missing = fixture.track("missing.flac", "Old", "1");
    let garbage = fixture.track("garbage.flac", "Old", "1");
    let refused = fixture.track("refused.flac", "Old", "1");
    let mut rows = load_rows_for_paths(&[missing.clone(), garbage.clone(), refused]).rows;
    fs::remove_file(&missing).expect("remove track");
    // Its words say permission was denied, which is no cause.
    fs::write(&garbage, b"Permission denied (os error 13)").expect("write garbage");
    rows[2].track_no = "0".to_string();

    let response = save_rows(SaveRequest {
        rows: rows
            .into_iter()
            .map(|row| SaveRow {
                row,
                changed_fields: Some(vec![TagField::TrackNo]),
            })
            .collect(),
    });

    let bytes = serialize_save_response(&response).expect("serialize");
    let sent: Value = serde_json::from_slice(&bytes).expect("json");
    let results = sent["results"].as_array().expect("results");
    assert_eq!(results.len(), 3);
    for result in results {
        assert_eq!(result["ok"], false, "{result}");
        assert!(result["error"].is_string(), "{result}");
        assert!(result.get("cause").is_none(), "{result}");
    }
}

#[test]
fn a_rename_in_a_read_only_folder_is_a_permission() {
    if !permissions_hold() {
        return;
    }
    // Renamed at once, and, for a swap of their names, through a staged one.
    for (names, action) in [
        (["a.flac", "b.flac"], "failed to rename "),
        (
            ["02 - Two.flac", "01 - One.flac"],
            "failed to stage rename ",
        ),
    ] {
        let mut fixture = Fixture::new("rename");
        let paths = [
            fixture.track(&format!("album/{}", names[0]), "One", "1"),
            fixture.track(&format!("album/{}", names[1]), "Two", "2"),
        ];
        let folder = fixture.dir.join("album");
        fixture.restrict(&folder, 0o555);

        let response = rename(&paths);

        // The first file's rename fails, and the second is left as it was.
        assert_eq!(response.results.len(), 2);
        for path in &paths {
            assert!(path.exists());
        }
        let failed = &response.results[0];
        assert!(!failed.ok);
        assert_eq!(failed.path, paths[0].to_string_lossy());
        assert_eq!(failed.cause, Some(FailureCause::RenameNotPermitted));
        let error = failed.error.as_deref().expect("error");
        assert!(error.starts_with(action), "{error}");
        let left = &response.results[1];
        assert!(!left.ok);
        assert_eq!(left.cause, Some(FailureCause::RenameStoppedElsewhere));
        assert_eq!(
            left.error.as_deref(),
            Some(
                format!(
                    "not renamed, because renaming stopped at {}",
                    paths[0].display()
                )
                .as_str()
            )
        );
        let bytes = serialize_rename_response(&response).expect("serialize");
        let sent: Value = serde_json::from_slice(&bytes).expect("json");
        assert_eq!(sent["results"][0]["cause"], "renameNotPermitted");
    }
}

#[test]
fn a_failed_rename_blames_its_file_and_tells_what_became_of_the_others() {
    if !permissions_hold() {
        return;
    }
    let mut fixture = Fixture::new("rename-restore");
    let paths = [
        fixture.track("writable/a.flac", "One", "1"),
        fixture.track("read-only/b.flac", "Two", "2"),
        fixture.track("writable/c.flac", "Three", "3"),
    ];
    let folder = fixture.dir.join("read-only");
    fixture.restrict(&folder, 0o555);

    let response = rename(&paths);

    // The first file is renamed, the second's rename fails, and the batch
    // stops there: the first is given its name back, the third is left as
    // it was (#741).
    for path in &paths {
        assert!(path.exists());
    }
    let stopped_at = paths[1].display();
    let expected = [
        (
            format!("renamed back to its original name, because renaming stopped at {stopped_at}"),
            FailureCause::RenameUndone,
        ),
        (
            format!(
                "failed to rename {stopped_at} to {}: {}",
                folder.join("02 - Two.flac").display(),
                denied()
            ),
            FailureCause::RenameNotPermitted,
        ),
        (
            format!("not renamed, because renaming stopped at {stopped_at}"),
            FailureCause::RenameStoppedElsewhere,
        ),
    ];
    assert_eq!(response.results.len(), expected.len());
    assert!(response.successful_paths.is_empty());
    for ((path, result), (error, cause)) in paths.iter().zip(&response.results).zip(&expected) {
        assert_eq!(result.path, path.to_string_lossy());
        assert!(!result.ok, "{result:?}");
        assert_eq!(result.error.as_deref(), Some(error.as_str()));
        assert_eq!(result.cause, Some(*cause));
    }
    let bytes = serialize_rename_response(&response).expect("serialize");
    let sent: Value = serde_json::from_slice(&bytes).expect("json");
    assert_eq!(sent["results"][0]["cause"], "renameUndone");
    assert_eq!(sent["results"][2]["cause"], "renameStoppedElsewhere");
}

#[test]
fn a_refused_rename_sends_no_cause() {
    let fixture = Fixture::new("rename-refused");
    let path = fixture.track("a.flac", "One", "1");
    fixture.track("01 - One.flac", "Other", "9");

    let response = rename(&[path]);

    assert!(!response.results[0].ok);
    let bytes = serialize_rename_response(&response).expect("serialize");
    let sent: Value = serde_json::from_slice(&bytes).expect("json");
    assert!(sent["results"][0].get("cause").is_none(), "{sent}");
}
