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
