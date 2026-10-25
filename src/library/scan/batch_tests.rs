// SPDX-License-Identifier: GPL-3.0-or-later

//! Scans that write in batches: what an interrupted or failed scan keeps,
//! and other connections writing between batches.

use std::sync::atomic::AtomicUsize;

use super::*;
use crate::library::db::{open_worker_db, OpenedLibrary};
use crate::library::schema::open_library_db_at;
use crate::library::worker::{handle_command, Control};
use crate::library::LibraryCommand;
use crate::tag_write::write_test_flac;

/// A control that calls `on_poll` with each poll's number, from 1, and
/// tells about other connections' writes as the library worker does.
struct TestControl<F> {
    polls: usize,
    on_poll: F,
    external: ExternalWrites,
}

impl<F> TestControl<F>
where
    F: FnMut(&Connection, usize) -> ScanDirective,
{
    fn new(on_poll: F) -> Self {
        Self {
            polls: 0,
            on_poll,
            external: ExternalWrites::default(),
        }
    }
}

impl<F> ScanControl for TestControl<F>
where
    F: FnMut(&Connection, usize) -> ScanDirective,
{
    fn poll(
        &mut self,
        conn: &Connection,
        _library: &mut WorkerLibrary,
        _event_tx: &Sender<LibraryEvent>,
        _scanning: &Path,
        _remaining: &[PathBuf],
    ) -> ScanDirective {
        self.polls += 1;
        (self.on_poll)(conn, self.polls)
    }

    fn external_writes(&mut self, conn: &Connection) -> bool {
        self.external.since_last_check(conn)
    }
}

/// Scans on this thread write every `batch_rows` rows, and read files on
/// this thread in the order the walk finds them.
fn batches_of(batch_rows: usize) {
    scan_test_hooks::set_tuning(ScanTuning {
        batch_rows,
        batch_interval: Duration::from_secs(60),
        workers: 1,
    });
}

fn test_dir(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let dir = crate::paths::test_root().join(format!(
        "library-batches-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(dir.join("music")).expect("create test dir");
    dir.canonicalize().expect("canonical test dir")
}

/// A root with stub tracks `a.mp3`, `b.mp3`, …, and an indexed track
/// `gone.mp3` whose file is gone.
fn root_with_gone_track(conn: &Connection, dir: &Path, tracks: usize) -> (PathBuf, Vec<PathBuf>) {
    let root = dir.join("music");
    let paths: Vec<PathBuf> = (b'a'..)
        .take(tracks)
        .map(|letter| root.join(format!("{}.mp3", char::from(letter))))
        .collect();
    for path in &paths {
        fs::write(path, path.to_string_lossy().as_bytes()).expect("track");
    }
    insert_root(conn, &root, "").expect("root");
    conn.execute(
        "INSERT INTO tracks(path, root_path, title, artist, album, cover_checked, mtime_ns, \
                            size_bytes, indexed_at, search_tags) \
         VALUES (?1, ?2, 'Gone', '', '', 1, 1, 1, 1, '{}')",
        params![
            root.join("gone.mp3").to_string_lossy(),
            root.to_string_lossy()
        ],
    )
    .expect("gone track");
    (root, paths)
}

fn db_paths(conn: &Connection) -> Vec<PathBuf> {
    let mut stmt = conn
        .prepare("SELECT path FROM tracks ORDER BY path")
        .expect("prepare");
    stmt.query_map([], |row| row.get::<_, String>(0))
        .expect("query")
        .map(|path| PathBuf::from(path.expect("path")))
        .collect()
}

fn snapshot_paths(library: &WorkerLibrary) -> Vec<PathBuf> {
    library.paths()
}

/// The library as the events published so far have it.
fn last_published(rx: &Receiver<LibraryEvent>) -> crate::library::LibraryMirror {
    let mut mirror = crate::library::LibraryMirror::default();
    mirror.drain(rx);
    assert!(mirror.events > 0, "a published event");
    mirror
}

fn title_of(conn: &Connection, path: &Path) -> String {
    conn.query_row(
        "SELECT title FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| row.get(0),
    )
    .expect("track row")
}

#[test]
fn a_stopped_scan_keeps_its_committed_batches_and_removes_nothing() {
    batches_of(2);
    let dir = test_dir("stopped");
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let (root, paths) = root_with_gone_track(&conn, &dir, 6);
    let mut snapshot = WorkerLibrary::default();
    snapshot.reload(&conn);
    let (tx, rx) = unbounded();
    // Stop as soon as the first batch is in the database.
    let mut control = TestControl::new(|conn: &Connection, _| {
        if db_paths(conn).len() > 2 {
            ScanDirective::SkipRoot
        } else {
            ScanDirective::Continue
        }
    });

    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut control,
    )
    .expect("a stopped scan is no failure");

    let expected = vec![paths[0].clone(), paths[1].clone(), root.join("gone.mp3")];
    assert_eq!(
        db_paths(&conn),
        expected,
        "one batch, and the gone track kept"
    );
    assert_eq!(snapshot_paths(&snapshot), expected);
    assert_eq!(last_published(&rx).paths(), expected);
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_failed_batch_keeps_the_batches_before_it_and_is_reported() {
    batches_of(2);
    let dir = test_dir("failed");
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let (root, paths) = root_with_gone_track(&conn, &dir, 6);
    // Stands in for any statement failure, such as a full disk.
    conn.execute_batch(
        "CREATE TEMP TRIGGER reject_track BEFORE INSERT ON tracks \
         WHEN NEW.path LIKE '%/d.mp3' \
         BEGIN SELECT RAISE(ABORT, 'injected write failure'); END",
    )
    .expect("inject failure");
    let mut snapshot = WorkerLibrary::default();
    snapshot.reload(&conn);
    let (tx, rx) = unbounded();

    handle_command(
        &conn,
        LibraryCommand::RescanRoot(root.clone()),
        &mut snapshot,
        &tx,
        &mut Control::detached(),
    );

    let published = last_published(&rx);
    let error = published.status.last_error.as_deref().unwrap_or_default();
    assert!(
        error.contains(&format!("failed to scan '{}'", root.display()))
            && error.contains("injected write failure"),
        "{error}"
    );
    // The batch of c and d rolled back; the gone track stays, since the walk
    // did not finish.
    let expected = vec![paths[0].clone(), paths[1].clone(), root.join("gone.mp3")];
    assert_eq!(db_paths(&conn), expected);
    assert_eq!(published.paths(), expected);
    let _ = fs::remove_dir_all(dir);
}

/// A library database in write-ahead mode, as the library worker opens it.
fn worker_db(dir: &Path) -> (Connection, PathBuf) {
    let path = dir.join("library.sqlite3");
    let OpenedLibrary { conn, notice } =
        open_worker_db(&path, Duration::from_secs(5)).expect("library database");
    assert_eq!(notice, None);
    (conn, path)
}

/// FLAC files titled `titles` in `dir/music`, named `01 - <title>.flac`, ….
fn flac_tracks(dir: &Path, titles: &[&str]) -> Vec<PathBuf> {
    titles
        .iter()
        .enumerate()
        .map(|(index, title)| {
            let path = dir
                .join("music")
                .join(format!("{:02} - {title}.flac", index + 1));
            write_test_flac(&path, &[("TITLE", title)]);
            path
        })
        .collect()
}

#[test]
fn a_rename_between_batches_does_not_wait_for_the_scan() {
    batches_of(2);
    let dir = test_dir("rename-between");
    let (conn, db) = worker_db(&dir);
    let root = dir.join("music");
    let paths = flac_tracks(&dir, &["One", "Two", "Three", "Four", "Five"]);
    let renamed = root.join("99 - Renamed.flac");
    let mut snapshot = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    let rename_count = std::cell::Cell::new(0);
    let mut control = TestControl::new(|conn: &Connection, _| {
        if rename_count.get() == 0 && db_paths(conn).len() >= 2 {
            // A tag editor's connection, which must not wait at all.
            let mut editor = open_library_db_at(&db, Duration::ZERO).expect("editor");
            rename_indexed_metadata_paths_in_conn(
                &mut editor,
                &[(paths[0].clone(), renamed.clone())],
            )
            .expect("the scan holds no lock between batches");
            rename_count.set(rename_count.get() + 1);
        }
        ScanDirective::Continue
    });

    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut control,
    )
    .expect("scan");

    assert_eq!(rename_count.get(), 1);
    let mut expected = paths[1..].to_vec();
    expected.push(renamed.clone());
    assert_eq!(db_paths(&conn), expected);
    assert!(renamed.is_file() && !paths[0].exists());
    assert_eq!(title_of(&conn, &renamed), "One");
    assert_eq!(last_published(&rx).paths(), expected);
    drop(conn);
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_file_renamed_after_the_scan_read_it_gets_no_row_under_its_old_name() {
    batches_of(3);
    let dir = test_dir("rename-read");
    let (conn, db) = worker_db(&dir);
    let root = dir.join("music");
    let paths = flac_tracks(&dir, &["One", "Two", "Three", "Four"]);
    insert_root(&conn, &root, "").expect("root");
    let mut existing = Connection::open(&db).expect("indexer");
    insert_indexed_tracks(&mut existing, &root, &paths[..1]);
    drop(existing);
    write_test_flac(&paths[0], &[("TITLE", "One, edited")]);
    let renamed = root.join("99 - Renamed.flac");
    let mut snapshot = WorkerLibrary::default();
    snapshot.reload(&conn);
    let (tx, rx) = unbounded();
    let mut control = TestControl::new(|_: &Connection, poll| {
        // Poll 3 comes after the scan read the first file, before its batch.
        if poll == 3 {
            let mut editor = open_library_db_at(&db, Duration::ZERO).expect("editor");
            rename_indexed_metadata_paths_in_conn(
                &mut editor,
                &[(paths[0].clone(), renamed.clone())],
            )
            .expect("rename");
        }
        ScanDirective::Continue
    });

    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut control,
    )
    .expect("scan");

    let mut expected = paths[1..].to_vec();
    expected.push(renamed.clone());
    assert_eq!(db_paths(&conn), expected, "no row for the old name");
    assert_eq!(title_of(&conn, &renamed), "One, edited");
    assert_eq!(last_published(&rx).paths(), expected);
    drop(conn);
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn an_edit_during_a_scan_is_not_overwritten_by_the_scans_older_read() {
    batches_of(10);
    let dir = test_dir("edit-during");
    let (conn, db) = worker_db(&dir);
    let root = dir.join("music");
    let paths = flac_tracks(&dir, &["One", "Two", "Three"]);
    let mut snapshot = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut Uninterrupted,
    )
    .expect("first scan");
    write_test_flac(&paths[1], &[("TITLE", "Two, changed outside")]);
    let mut control = TestControl::new(|_: &Connection, poll| {
        // Poll 4 comes after the scan read the second file, before its batch.
        if poll == 4 {
            std::thread::sleep(Duration::from_millis(5));
            write_test_flac(&paths[1], &[("TITLE", "Two, edited in Ferrous")]);
            let mut editor = open_library_db_at(&db, Duration::ZERO).expect("editor");
            refresh_indexed_metadata_in_conn(&mut editor, &paths[1..2], None).expect("refresh");
        }
        ScanDirective::Continue
    });

    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut control,
    )
    .expect("rescan");

    assert_eq!(title_of(&conn, &paths[1]), "Two, edited in Ferrous");
    let published = last_published(&rx);
    let track = published
        .snapshot
        .tracks
        .iter()
        .find(|track| track.path == paths[1])
        .expect("published track");
    assert_eq!(track.title, "Two, edited in Ferrous");
    drop(conn);
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_cover_refresh_during_a_scan_is_not_overwritten_by_the_scans_older_read() {
    batches_of(10);
    let dir = test_dir("cover-during");
    let (conn, db) = worker_db(&dir);
    let root = dir.join("music");
    let paths = flac_tracks(&dir, &["One", "Two"]);
    let mut snapshot = WorkerLibrary::default();
    let (tx, _rx) = unbounded();
    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut Uninterrupted,
    )
    .expect("first scan");
    std::thread::sleep(Duration::from_millis(5));
    write_test_flac(&paths[0], &[("TITLE", "One, changed")]);
    let mut control = TestControl::new(|_: &Connection, poll| {
        // Poll 3 comes after the scan read the first file, before its batch.
        if poll == 3 {
            let editor = open_library_db_at(&db, Duration::ZERO).expect("editor");
            editor
                .execute(
                    "UPDATE tracks SET cover_path = '/covers/applied.jpg' WHERE path = ?1",
                    params![paths[0].to_string_lossy()],
                )
                .expect("apply album art");
        }
        ScanDirective::Continue
    });

    run_scans_with(
        &conn,
        std::slice::from_ref(&root),
        None,
        &mut snapshot,
        &tx,
        &mut control,
    )
    .expect("rescan");

    let cover: String = conn
        .query_row(
            "SELECT cover_path FROM tracks WHERE path = ?1",
            params![paths[0].to_string_lossy()],
            |row| row.get(0),
        )
        .expect("row");
    assert_eq!(cover, "/covers/applied.jpg");
    drop(conn);
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn removing_gone_tracks_keeps_a_row_written_since_the_scan_started() {
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    for (path, mtime) in [("/music/gone.mp3", 1), ("/music/renamed-in.mp3", 7)] {
        conn.execute(
            "INSERT INTO tracks(path, root_path, title, artist, album, mtime_ns, size_bytes, \
                                indexed_at) VALUES (?1, '/music', '', '', '', ?2, 1, 1)",
            params![path, mtime],
        )
        .expect("row");
    }
    // The scan started with both rows at state 1; the second was rewritten.
    let existing = ["/music/gone.mp3", "/music/renamed-in.mp3"]
        .into_iter()
        .map(|path| {
            (
                path.to_string(),
                ExistingTrackScanState {
                    row: RowState::new(1, 1, "", 0),
                    has_cover_path: true,
                    suspicious_metadata: false,
                    cover_stamp: 0,
                    cover_in_folder: false,
                },
            )
        })
        .collect::<HashMap<_, _>>();

    let removed = delete_stale_rows(
        &conn,
        vec![
            "/music/gone.mp3".to_string(),
            "/music/renamed-in.mp3".to_string(),
        ],
        &existing,
    )
    .expect("remove");

    assert_eq!(removed, ["/music/gone.mp3"]);
    assert_eq!(db_paths(&conn), [PathBuf::from("/music/renamed-in.mp3")]);
}
