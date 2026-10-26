// SPDX-License-Identifier: GPL-3.0-or-later

//! Scans of the paths that changed below a root.

use std::sync::atomic::AtomicUsize;

use super::*;
use crate::library::worker::{handle_command, Control};
use crate::library::LibraryCommand;

fn test_root(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let dir = crate::paths::test_root().join(format!(
        "library-scope-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    let root = dir.join("music");
    fs::create_dir_all(&root).expect("create test root");
    root.canonicalize().expect("canonical test root")
}

fn write(path: &Path) {
    fs::create_dir_all(path.parent().expect("parent")).expect("folder");
    fs::write(path, path.to_string_lossy().as_bytes()).expect("track");
}

/// A database with `root` scanned, and the worker's copy of it.
fn scanned(
    root: &Path,
) -> (
    Connection,
    WorkerLibrary,
    Receiver<LibraryEvent>,
    Sender<LibraryEvent>,
) {
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    handle_command(
        &conn,
        LibraryCommand::AddRoot {
            path: root.to_path_buf(),
            name: String::new(),
        },
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    assert_eq!(library.status.last_error, None);
    (conn, library, rx, tx)
}

fn scan_paths(
    conn: &Connection,
    library: &mut WorkerLibrary,
    tx: &Sender<LibraryEvent>,
    root: &Path,
    paths: &[PathBuf],
) {
    handle_command(
        conn,
        LibraryCommand::ScanPaths {
            root: root.to_path_buf(),
            paths: paths.to_vec(),
        },
        library,
        tx,
        &mut Control::detached(),
    );
}

fn indexed(conn: &Connection) -> Vec<PathBuf> {
    let mut snapshot = LibrarySnapshot::default();
    load_snapshot(conn, &mut snapshot);
    let mut paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
    paths.sort();
    paths
}

#[test]
fn a_scan_of_changed_paths_adds_and_removes_only_their_tracks() {
    let root = test_root("changes");
    for name in ["a/1.mp3", "a/2.mp3", "a b/3.mp3", "b/4.mp3"] {
        write(&root.join(name));
    }
    let (conn, mut library, rx, tx) = scanned(&root);
    // Changes the scan is not told about stay as they are.
    fs::remove_file(root.join("b/4.mp3")).expect("remove");
    write(&root.join("b/5.mp3"));
    // Changes it is told about: a folder that is gone, and a new file.
    fs::remove_dir_all(root.join("a")).expect("remove folder");
    write(&root.join("c/6.mp3"));

    scan_paths(
        &conn,
        &mut library,
        &tx,
        &root,
        &[root.join("a"), root.join("c/6.mp3"), root.join("c")],
    );

    let expected = vec![
        root.join("a b/3.mp3"),
        root.join("b/4.mp3"),
        root.join("c/6.mp3"),
    ];
    assert_eq!(indexed(&conn), expected);
    assert_eq!(library.paths(), expected);
    let mut mirror = crate::library::LibraryMirror::default();
    mirror.drain(&rx);
    assert_eq!(mirror.paths(), expected);
    assert_eq!(library.status.last_error, None);
    assert!(!library.status.scan_in_progress);
}

#[test]
fn a_scan_of_changed_paths_reads_no_file_whose_row_is_current() {
    let root = test_root("current");
    let paths: Vec<PathBuf> = (0..20)
        // No leading number: a stub named so is read again by every scan.
        .map(|index| root.join(format!("album/t{index:02}.mp3")))
        .collect();
    for path in &paths {
        write(path);
    }
    let (conn, mut library, _rx, tx) = scanned(&root);
    // As after the tag editor saved them and refreshed their rows.
    for path in &paths {
        scan_test_hooks::panic_on_read(path);
    }

    scan_paths(&conn, &mut library, &tx, &root, &paths);

    assert_eq!(library.status.last_error, None, "no file was read");
    assert_eq!(indexed(&conn), paths);
}

#[test]
fn changed_paths_reached_through_a_link_scan_the_whole_root() {
    let root = test_root("link");
    write(&root.join("real/1.mp3"));
    std::os::unix::fs::symlink(root.join("real"), root.join("alias")).expect("link");
    let (conn, mut library, _rx, tx) = scanned(&root);
    assert_eq!(indexed(&conn), vec![root.join("real/1.mp3")]);
    let scans = scan_test_hooks::scans_of(&root);
    write(&root.join("real/2.mp3"));

    // The change seen through the link.
    scan_paths(&conn, &mut library, &tx, &root, &[root.join("alias/2.mp3")]);

    assert_eq!(
        indexed(&conn),
        vec![root.join("real/1.mp3"), root.join("real/2.mp3")],
        "indexed under the path without links, as a scan of the root does"
    );
    assert_eq!(scan_test_hooks::scans_of(&root), scans + 2);
    assert_eq!(library.status.last_error, None);
}

#[test]
fn a_scan_of_changed_paths_in_an_emptied_root_keeps_its_tracks() {
    let root = test_root("offline");
    write(&root.join("a/1.mp3"));
    write(&root.join("2.mp3"));
    let (conn, mut library, _rx, tx) = scanned(&root);
    // An unmounted drive leaves its mount point empty.
    fs::remove_dir_all(root.join("a")).expect("remove");
    fs::remove_file(root.join("2.mp3")).expect("remove");

    scan_paths(&conn, &mut library, &tx, &root, &[root.join("a")]);

    assert_eq!(
        indexed(&conn),
        vec![root.join("2.mp3"), root.join("a/1.mp3")]
    );
    assert!(library
        .status
        .last_error
        .as_deref()
        .is_some_and(|error| error.contains("in case the folder is offline")));
}

#[test]
fn changed_paths_of_a_removed_root_are_not_scanned() {
    let root = test_root("removed");
    write(&root.join("1.mp3"));
    let (conn, mut library, _rx, tx) = scanned(&root);
    handle_command(
        &conn,
        LibraryCommand::RemoveRoot(root.clone()),
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    let scans = scan_test_hooks::scans_of(&root);

    scan_paths(&conn, &mut library, &tx, &root, &[root.join("1.mp3")]);

    assert_eq!(scan_test_hooks::scans_of(&root), scans);
    assert!(indexed(&conn).is_empty());
    assert!(load_roots(&conn).is_empty(), "the root is not added back");
    assert_eq!(library.status.last_error, None);
}

#[test]
fn changed_paths_keep_only_the_outermost_below_the_root() {
    let root = Path::new("/music");
    let paths = [
        PathBuf::from("/music/a/1.flac"),
        PathBuf::from("/music/a"),
        PathBuf::from("/music/a b/2.flac"),
        PathBuf::from("/elsewhere/3.flac"),
        PathBuf::from("/music/b/c/4.flac"),
        PathBuf::from("/music/b/c"),
    ];
    assert_eq!(
        outermost_paths_below(root, &paths),
        vec![
            PathBuf::from("/music/a"),
            PathBuf::from("/music/a b/2.flac"),
            PathBuf::from("/music/b/c"),
        ]
    );
}

#[test]
fn removing_the_indexed_name_of_a_hard_linked_file_indexes_its_other_name() {
    let root = test_root("hard-link");
    write(&root.join("a.mp3"));
    fs::hard_link(root.join("a.mp3"), root.join("b.mp3")).expect("hard link");
    let (conn, mut library, _rx, tx) = scanned(&root);
    assert_eq!(
        indexed(&conn),
        vec![root.join("a.mp3")],
        "one name per file"
    );

    fs::remove_file(root.join("a.mp3")).expect("remove");
    scan_paths(&conn, &mut library, &tx, &root, &[root.join("a.mp3")]);

    assert_eq!(indexed(&conn), vec![root.join("b.mp3")]);
    assert_eq!(library.paths(), vec![root.join("b.mp3")]);
    assert_eq!(library.status.last_error, None);
}

#[test]
fn a_root_keeps_its_scan_problem_until_a_scan_of_the_whole_root_finds_none() {
    let root = test_root("root-problem");
    write(&root.join("a/1.mp3"));
    write(&root.join("2.mp3"));
    let (conn, mut library, _rx, tx) = scanned(&root);
    assert!(library.status.root_problems.is_empty());
    // An unmounted drive leaves its mount point empty.
    fs::rename(&root, root.with_extension("away")).expect("unmount");
    fs::create_dir_all(&root).expect("empty mount point");

    handle_command(
        &conn,
        LibraryCommand::RescanRoot(root.clone()),
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    let problems = library.status.root_problems.clone();
    assert_eq!(problems.len(), 1);
    assert_eq!(problems[0].path, root);
    assert!(problems[0]
        .problem
        .contains("in case the folder is offline"));

    // Another command, and a scan of part of the root that finds nothing
    // wrong, leave the problem shown.
    fs::remove_dir_all(&root).expect("unmount");
    fs::rename(root.with_extension("away"), &root).expect("mount");
    handle_command(
        &conn,
        LibraryCommand::RenameRoot {
            path: root.clone(),
            name: "Music".to_string(),
        },
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    scan_paths(&conn, &mut library, &tx, &root, &[root.join("a")]);
    assert_eq!(library.status.root_problems, problems);

    // A scan of the whole root that finds nothing wrong clears it.
    handle_command(
        &conn,
        LibraryCommand::RescanRoot(root.clone()),
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    assert!(library.status.root_problems.is_empty());

    // The problem of a removed root goes with it.
    fs::remove_dir_all(root.join("a")).expect("remove");
    fs::remove_file(root.join("2.mp3")).expect("remove");
    handle_command(
        &conn,
        LibraryCommand::RescanRoot(root.clone()),
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    assert_eq!(library.status.root_problems.len(), 1);
    handle_command(
        &conn,
        LibraryCommand::RemoveRoot(root.clone()),
        &mut library,
        &tx,
        &mut Control::detached(),
    );
    assert!(library.status.root_problems.is_empty());
}
