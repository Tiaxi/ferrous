// SPDX-License-Identifier: GPL-3.0-or-later

//! The identities of indexed files (see [`FileId`]): a scan reads a file
//! again when another file of the same size and time took its place, and
//! otherwise only stores the identity it finds.

use std::sync::atomic::AtomicUsize;

use super::scan_test_hooks::reads_of;
use super::*;
use crate::library::worker::{handle_command, Control};
use crate::library::LibraryCommand;

fn test_root(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let dir = crate::paths::test_root().join(format!(
        "library-identity-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    let root = dir.join("music");
    fs::create_dir_all(&root).expect("create test root");
    root.canonicalize().expect("canonical test root")
}

/// FLAC files in `root` titled `titles`, which have one length, so that
/// the files have one size, and given one modification time.
fn same_size_and_time(root: &Path, titles: &[&str]) -> Vec<PathBuf> {
    let time = UNIX_EPOCH + Duration::from_secs(1_700_000_000);
    titles
        .iter()
        .map(|title| {
            let path = root.join(format!("{title}.flac"));
            crate::tag_write::write_test_flac(&path, &[("TITLE", title)]);
            fs::File::options()
                .write(true)
                .open(&path)
                .and_then(|file| file.set_modified(time))
                .expect("modification time");
            path
        })
        .collect()
}

fn run(conn: &Connection, library: &mut WorkerLibrary, command: LibraryCommand) {
    let (tx, _rx) = unbounded();
    handle_command(conn, command, library, &tx, &mut Control::detached());
    assert_eq!(library.status.last_error, None);
}

/// A database with `root` added and scanned.
fn scanned(root: &Path) -> (Connection, WorkerLibrary) {
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let mut library = WorkerLibrary::default();
    run(
        &conn,
        &mut library,
        LibraryCommand::AddRoot {
            path: root.to_path_buf(),
            name: String::new(),
        },
    );
    (conn, library)
}

fn rescan(conn: &Connection, library: &mut WorkerLibrary, root: &Path) {
    run(
        conn,
        library,
        LibraryCommand::RescanRoot(root.to_path_buf()),
    );
}

fn stored_file_id(conn: &Connection, path: &Path) -> Option<FileId> {
    conn.query_row(
        "SELECT file_device, file_inode FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| Ok(file_id_from_columns(row.get(0)?, row.get(1)?)),
    )
    .expect("row")
}

fn file_id(path: &Path) -> Option<FileId> {
    Some(FileId::of(&fs::metadata(path).expect("metadata")))
}

fn title(conn: &Connection, path: &Path) -> String {
    conn.query_row(
        "SELECT title FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| row.get(0),
    )
    .expect("row")
}

fn reads(paths: &[PathBuf]) -> Vec<usize> {
    paths.iter().map(|path| reads_of(path)).collect()
}

/// Gives every row an identity of no file, as after the file system was
/// mounted again with other device or inode numbers.
fn forget_identities(conn: &Connection) {
    conn.execute(
        "UPDATE tracks SET file_device = file_device + 1, file_inode = file_inode + 1000003",
        [],
    )
    .expect("other identities");
}

#[test]
fn the_first_scan_after_the_migration_reads_no_file_and_stores_every_identity() {
    let root = test_root("migrated");
    let paths: Vec<PathBuf> = ["Alpha", "Bravo", "Charlie"]
        .iter()
        .map(|title| {
            let path = root.join(format!("{title}.flac"));
            crate::tag_write::write_test_flac(&path, &[("TITLE", title)]);
            path
        })
        .collect();
    let (conn, mut library) = scanned(&root);
    assert_eq!(reads(&paths), [1, 1, 1]);
    // The database as version 3 left it, then migrated.
    conn.execute_batch(
        "DROP INDEX idx_tracks_size_mtime;
         ALTER TABLE tracks DROP COLUMN file_device;
         ALTER TABLE tracks DROP COLUMN file_inode;
         ALTER TABLE waveform_cache DROP COLUMN file_device;
         ALTER TABLE waveform_cache DROP COLUMN file_inode;
         PRAGMA user_version = 3;",
    )
    .expect("version 3");
    init_schema(&conn).expect("migration");
    assert!(paths
        .iter()
        .all(|path| stored_file_id(&conn, path).is_none()));

    rescan(&conn, &mut library, &root);

    assert_eq!(reads(&paths), [1, 1, 1], "no file is read again");
    for path in &paths {
        assert!(file_id(path).is_some());
        assert_eq!(stored_file_id(&conn, path), file_id(path));
    }
    // Nor does the next scan write anything.
    let changes = conn.total_changes();
    rescan(&conn, &mut library, &root);
    assert_eq!(reads(&paths), [1, 1, 1]);
    assert_eq!(conn.total_changes(), changes);
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn identities_the_file_system_did_not_keep_are_replaced_without_reading_the_files() {
    let root = test_root("not-kept");
    let paths: Vec<PathBuf> = ["A", "Bb", "Ccc"]
        .iter()
        .map(|title| {
            let path = root.join(format!("{title}.flac"));
            crate::tag_write::write_test_flac(&path, &[("TITLE", title)]);
            path
        })
        .collect();
    let (conn, mut library) = scanned(&root);
    forget_identities(&conn);

    rescan(&conn, &mut library, &root);

    // Each file has a size of its own, which tells it from any other.
    assert_eq!(reads(&paths), [1, 1, 1]);
    for path in &paths {
        assert_eq!(stored_file_id(&conn, path), file_id(path));
    }
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn files_of_the_same_size_and_time_swapped_outside_the_app_are_read_again() {
    let root = test_root("swapped");
    let paths = same_size_and_time(&root, &["Alpha", "Bravo"]);
    let (alpha, bravo) = (&paths[0], &paths[1]);
    let (conn, mut library) = scanned(&root);
    assert_eq!(title(&conn, alpha), "Alpha");

    let aside = root.join("aside");
    fs::rename(alpha, &aside).expect("aside");
    fs::rename(bravo, alpha).expect("bravo to alpha");
    fs::rename(&aside, bravo).expect("alpha to bravo");
    rescan(&conn, &mut library, &root);

    assert_eq!(title(&conn, alpha), "Bravo");
    assert_eq!(title(&conn, bravo), "Alpha");
    assert_eq!(reads(&paths), [2, 2]);
    assert_eq!(stored_file_id(&conn, alpha), file_id(alpha));
    assert_eq!(stored_file_id(&conn, bravo), file_id(bravo));
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn identities_not_kept_for_files_of_the_same_size_and_time_cost_a_read_not_their_tags() {
    let root = test_root("not-kept-tied");
    let paths = same_size_and_time(&root, &["Alpha", "Bravo"]);
    let (conn, mut library) = scanned(&root);
    forget_identities(&conn);

    rescan(&conn, &mut library, &root);

    // Their identities cannot tell a swap from a file system that did not
    // keep them, so both files are read again, and keep their tags.
    assert_eq!(reads(&paths), [2, 2]);
    assert_eq!(title(&conn, &paths[0]), "Alpha");
    assert_eq!(title(&conn, &paths[1]), "Bravo");
    rescan(&conn, &mut library, &root);
    assert_eq!(reads(&paths), [2, 2], "the new identities are kept");
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn a_rename_through_the_library_keeps_the_identities_of_its_files() {
    let root = test_root("renamed");
    let paths = same_size_and_time(&root, &["Alpha", "Bravo"]);
    let (alpha, bravo) = (paths[0].clone(), paths[1].clone());
    let (mut conn, mut library) = scanned(&root);
    let (alpha_id, bravo_id) = (file_id(&alpha), file_id(&bravo));

    rename_indexed_metadata_paths_in_conn(
        &mut conn,
        &[
            (alpha.clone(), bravo.clone()),
            (bravo.clone(), alpha.clone()),
        ],
    )
    .expect("swap");

    assert_eq!(title(&conn, &bravo), "Alpha");
    assert_eq!(stored_file_id(&conn, &bravo), alpha_id);
    assert_eq!(stored_file_id(&conn, &alpha), bravo_id);
    // So the next scan finds both files unchanged.
    let before = reads(&paths);
    rescan(&conn, &mut library, &root);
    assert_eq!(reads(&paths), before);
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn a_refreshed_row_takes_the_identity_of_the_file_an_edit_saved() {
    let root = test_root("refreshed");
    let path = root.join("Alpha.flac");
    crate::tag_write::write_test_flac(&path, &[("TITLE", "Alpha")]);
    let (mut conn, _library) = scanned(&root);
    // An edit saves the file by replacing it.
    let saved = root.join("saved.tmp");
    crate::tag_write::write_test_flac(&saved, &[("TITLE", "Edited")]);
    fs::rename(&saved, &path).expect("replace");
    assert_ne!(stored_file_id(&conn, &path), file_id(&path));

    refresh_indexed_metadata_in_conn(&mut conn, std::slice::from_ref(&path), None)
        .expect("refresh");

    assert_eq!(title(&conn, &path), "Edited");
    assert_eq!(stored_file_id(&conn, &path), file_id(&path));
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

/// A root with `Alpha.flac` and other files, scanned, and `Bravo.flac`
/// outside it, of Alpha's size and time; Alpha has a saved waveform.
fn with_an_outside_twin(name: &str) -> (PathBuf, PathBuf, PathBuf, Connection, WorkerLibrary) {
    let root = test_root(name);
    let alpha = same_size_and_time(&root, &["Alpha"]).remove(0);
    for title in ["One", "Twelve", "Thirteen", "Fourteen!"] {
        crate::tag_write::write_test_flac(&root.join(format!("{title}.flac")), &[("TITLE", title)]);
    }
    let outside = root.parent().expect("dir").join("outside");
    fs::create_dir_all(&outside).expect("outside");
    let bravo = same_size_and_time(&outside, &["Bravo"]).remove(0);
    let (conn, library) = scanned(&root);
    let (device, inode) = file_id_columns(file_id(&alpha));
    conn.execute(
        "INSERT INTO waveform_cache(path, size_bytes, modified_secs, modified_nanos, \
         format_version, peak_count, peaks_blob, updated_at, complete, file_device, file_inode) \
         VALUES (?1, 0, 0, 0, 3, 1, x'0000003f', 0, 1, ?2, ?3)",
        params![alpha.to_string_lossy(), device, inode],
    )
    .expect("saved waveform");
    (root, alpha, bravo, conn, library)
}

fn swap(a: &Path, b: &Path) {
    let aside = a.with_extension("aside");
    fs::rename(a, &aside).expect("aside");
    fs::rename(b, a).expect("b to a");
    fs::rename(&aside, b).expect("a to b");
}

fn has_saved_waveform(conn: &Connection, path: &Path) -> bool {
    conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM waveform_cache WHERE path = ?1)",
        params![path.to_string_lossy()],
        |row| row.get(0),
    )
    .expect("query")
}

#[test]
fn a_file_swapped_with_one_outside_the_library_is_read_again() {
    let (root, alpha, bravo, conn, mut library) = with_an_outside_twin("outside-twin");
    // While Ferrous was closed, with nothing of the library's size and
    // time to tie with.
    swap(&alpha, &bravo);

    rescan(&conn, &mut library, &root);

    // The other files kept their identities, so Alpha's changed one means
    // another file.
    assert_eq!(title(&conn, &alpha), "Bravo");
    assert_eq!(reads_of(&alpha), 2);
    assert_eq!(stored_file_id(&conn, &alpha), file_id(&alpha));
    assert!(!has_saved_waveform(&conn, &alpha), "the other file's");
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn a_scan_of_changed_paths_leaves_an_identity_it_cannot_judge_to_a_full_scan() {
    let (root, alpha, bravo, conn, mut library) = with_an_outside_twin("outside-twin-scoped");
    let stored = stored_file_id(&conn, &alpha);
    swap(&alpha, &bravo);

    run(
        &conn,
        &mut library,
        LibraryCommand::ScanPaths {
            root: root.clone(),
            paths: vec![alpha.clone()],
        },
    );

    // One path cannot tell a swap from a file system that did not keep the
    // identity: the row is left as it is, which the waveform cache does not
    // take as the library's word for the new identity.
    assert_eq!(reads_of(&alpha), 1);
    assert_eq!(stored_file_id(&conn, &alpha), stored);
    assert!(has_saved_waveform(&conn, &alpha));
    rescan(&conn, &mut library, &root);
    assert_eq!(title(&conn, &alpha), "Bravo");
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}

#[test]
fn a_root_whose_file_system_did_not_keep_identities_is_not_read_again() {
    let (root, alpha, _bravo, conn, mut library) = with_an_outside_twin("not-kept-all");
    let paths: Vec<PathBuf> = fs::read_dir(&root)
        .expect("root")
        .map(|entry| entry.expect("entry").path())
        .collect();
    let before = reads(&paths);
    forget_identities(&conn);

    // A scan of a changed path cannot tell, and leaves the rows.
    run(
        &conn,
        &mut library,
        LibraryCommand::ScanPaths {
            root: root.clone(),
            paths: vec![alpha.clone()],
        },
    );
    assert_eq!(reads(&paths), before);
    assert_ne!(stored_file_id(&conn, &alpha), file_id(&alpha));

    // A full scan finds that no file kept its identity, and stores each
    // file's without reading it; the saved waveform stays.
    rescan(&conn, &mut library, &root);
    assert_eq!(reads(&paths), before);
    for path in &paths {
        assert_eq!(stored_file_id(&conn, path), file_id(path));
    }
    assert!(has_saved_waveform(&conn, &alpha));
    fs::remove_dir_all(root.parent().expect("dir")).expect("cleanup");
}
