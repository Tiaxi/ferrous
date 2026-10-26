// SPDX-License-Identifier: GPL-3.0-or-later

use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

use rusqlite::params;
use rusqlite::types::Value;

use super::*;
use crate::library::schema::{user_version, SCHEMA_VERSION};
use crate::library::test_fixtures::{
    build_version_0_library, library_rows, schema_objects, table_rows,
};

const SHORT_WAIT: Duration = Duration::from_millis(50);

/// A new, empty folder under the test process's root.
fn test_folder(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let folder = crate::paths::test_root().join(format!(
        "library-db-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&folder);
    fs::create_dir_all(&folder).expect("test folder");
    folder
}

fn entries(folder: &Path) -> Vec<String> {
    let mut names = fs::read_dir(folder)
        .expect("read folder")
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

fn journal_mode(conn: &Connection) -> String {
    conn.pragma_query_value(None, "journal_mode", |row| row.get(0))
        .expect("journal mode")
}

fn full_text_objects(conn: &Connection) -> Vec<String> {
    schema_objects(conn)
        .into_iter()
        .map(|(_, name, _)| name)
        .filter(|name| name.starts_with("tracks_fts"))
        .collect()
}

#[test]
fn a_version_0_library_migrates_without_losing_a_row_and_a_second_run_changes_nothing() {
    let folder = test_folder("migrate-v0");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 1500);
    let before = {
        let conn = Connection::open(&path).expect("open fixture");
        assert_eq!(user_version(&conn).expect("version"), 0);
        assert!(
            !full_text_objects(&conn).is_empty(),
            "the fixture has the index"
        );
        let fts_rows: i64 = conn
            .query_row("SELECT COUNT(*) FROM tracks_fts", [], |row| row.get(0))
            .expect("index rows");
        assert_eq!(fts_rows, 4500, "the old schema code indexed every track");
        library_rows(&conn)
    };
    assert_eq!(before[1].len(), 4500);
    assert!(before.iter().all(|rows| !rows.is_empty()));

    let OpenedLibrary { conn, notice } = open_worker_db(&path, SHORT_WAIT).expect("migrate");

    assert_eq!(notice, None);
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
    assert_eq!(journal_mode(&conn), "wal");
    assert_eq!(full_text_objects(&conn), Vec::<String>::new());
    // Every track gets a cover stamp, unknown until the next scan checks
    // its cover; see `migrate_to_version_2`.
    // Every track and saved waveform has an unknown file identity, which
    // it takes from the file it matches next; see `migrate_to_version_4`.
    let mut before = before;
    for row in &mut before[1] {
        row.extend([Value::Null, Value::Null, Value::Null]);
    }
    // Every saved waveform is decoded again, once, since nothing tells
    // whether its decode reached the end of the file; see
    // `migrate_to_version_3`.
    for row in &mut before[3] {
        row.extend([Value::Integer(0), Value::Null, Value::Null, Value::Null]);
    }
    assert_eq!(library_rows(&conn), before, "every row and cover is kept");
    let indexes: Vec<String> = schema_objects(&conn)
        .into_iter()
        .filter(|(kind, _, _)| kind == "index")
        .map(|(_, name, _)| name)
        .filter(|name| name.starts_with("idx_"))
        .collect();
    assert_eq!(
        indexes,
        [
            "idx_external_tracks_indexed_at",
            "idx_tracks_album",
            "idx_tracks_artist",
            "idx_tracks_genre",
            "idx_tracks_root_path",
            "idx_tracks_root_path_path_nocase",
            "idx_tracks_size_mtime",
            "idx_tracks_title",
            "idx_waveform_cache_updated_at",
        ]
    );
    let schema = schema_objects(&conn);
    let integrity: String = conn
        .query_row("PRAGMA integrity_check", [], |row| row.get(0))
        .expect("integrity check");
    assert_eq!(integrity, "ok");
    drop(conn);

    let OpenedLibrary { conn, notice } = open_worker_db(&path, SHORT_WAIT).expect("reopen");
    assert_eq!(notice, None);
    assert_eq!(conn.total_changes(), 0, "a second migration writes nothing");
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
    assert_eq!(schema_objects(&conn), schema);
    assert_eq!(library_rows(&conn), before);
    drop(conn);
    assert_eq!(entries(&folder), ["library.sqlite3"], "nothing moved aside");
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_version_0_library_without_the_waveform_cache_gets_it_and_keeps_its_rows() {
    let folder = test_folder("migrate-v0-no-waveforms");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 40);
    let library_tables = |conn: &Connection| {
        ["roots", "tracks", "external_tracks"].map(|table| table_rows(conn, table))
    };
    let mut before = {
        let conn = Connection::open(&path).expect("open fixture");
        // The analysis worker may never have made it, as when another
        // writer held the database.
        conn.execute_batch("DROP TABLE waveform_cache")
            .expect("no waveform cache");
        library_tables(&conn)
    };
    assert!(before.iter().all(|rows| !rows.is_empty()));

    let OpenedLibrary { conn, notice } = open_worker_db(&path, SHORT_WAIT).expect("migrate");

    assert_eq!(notice, None);
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
    for row in &mut before[1] {
        row.extend([Value::Null, Value::Null, Value::Null]);
    }
    assert_eq!(library_tables(&conn), before);
    assert!(table_rows(&conn, "waveform_cache").is_empty());
    let columns: Vec<String> = conn
        .prepare("SELECT name FROM pragma_table_info('waveform_cache')")
        .expect("prepare")
        .query_map([], |row| row.get(0))
        .expect("columns")
        .collect::<rusqlite::Result<_>>()
        .expect("column names");
    assert!(
        ["complete", "coverage_seconds", "file_device", "file_inode"]
            .iter()
            .all(|column| columns.iter().any(|name| name == column))
    );
    drop(conn);

    let OpenedLibrary { conn, .. } = open_worker_db(&path, SHORT_WAIT).expect("reopen");
    assert_eq!(conn.total_changes(), 0, "a second migration writes nothing");
    drop(conn);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_library_from_before_metadata_columns_gets_them_and_keeps_its_rows() {
    let folder = test_folder("migrate-legacy");
    let path = folder.join("library.sqlite3");
    let conn = Connection::open(&path).expect("legacy database");
    conn.execute_batch(
        r"
        CREATE TABLE roots (path TEXT PRIMARY KEY, added_at INTEGER NOT NULL);
        CREATE TABLE tracks (
            path TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            artist TEXT NOT NULL,
            album TEXT NOT NULL,
            mtime_ns INTEGER NOT NULL,
            size_bytes INTEGER NOT NULL,
            indexed_at INTEGER NOT NULL
        );
        INSERT INTO roots VALUES ('/music', 1);
        INSERT INTO tracks VALUES ('/music/a.flac', 'A', 'Artist', 'Album', 2, 3, 4);
        ",
    )
    .expect("legacy rows");
    drop(conn);

    let OpenedLibrary { conn, .. } = open_worker_db(&path, SHORT_WAIT).expect("migrate");

    let row = conn
        .query_row(
            "SELECT path, root_path, title, genre, cover_checked, year, duration_secs, \
             search_tags FROM tracks",
            [],
            |row| {
                (0..8)
                    .map(|column| row.get::<_, Value>(column))
                    .collect::<rusqlite::Result<Vec<_>>>()
            },
        )
        .expect("migrated row");
    assert_eq!(
        row,
        [
            Value::Text("/music/a.flac".to_string()),
            Value::Text(String::new()),
            Value::Text("A".to_string()),
            Value::Text(String::new()),
            Value::Integer(0),
            Value::Null,
            Value::Null,
            Value::Null,
        ]
    );
    let name: String = conn
        .query_row("SELECT name FROM roots WHERE path = '/music'", [], |row| {
            row.get(0)
        })
        .expect("root");
    assert_eq!(name, "");
    drop(conn);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_failed_migration_is_reported_and_leaves_the_database_as_it_was() {
    let folder = test_folder("migrate-fails");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 20);
    let conn = Connection::open(&path).expect("fixture");
    // Any failing step: here the index name is taken by a table.
    conn.execute_batch("DROP INDEX idx_tracks_genre; CREATE TABLE idx_tracks_genre (value TEXT);")
        .expect("block a migration step");
    let before = library_rows(&conn);
    let schema = schema_objects(&conn);
    drop(conn);

    let error = open_worker_db(&path, SHORT_WAIT)
        .err()
        .expect("the migration fails");

    assert!(error.contains("idx_tracks_genre"), "{error}");
    let conn = Connection::open(&path).expect("reopen");
    assert_eq!(user_version(&conn).expect("version"), 0);
    assert_eq!(schema_objects(&conn), schema, "the migration rolled back");
    assert_eq!(library_rows(&conn), before);
    drop(conn);
    assert!(
        entries(&folder)
            .iter()
            .all(|name| name.starts_with("library.sqlite3") && !name.contains("damaged")),
        "{:?}",
        entries(&folder)
    );
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_database_from_a_newer_version_is_reported_and_left_alone() {
    let folder = test_folder("newer");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 5);
    let conn = Connection::open(&path).expect("fixture");
    conn.pragma_update(None, "user_version", SCHEMA_VERSION + 1)
        .expect("newer version");
    let before = library_rows(&conn);
    let schema = schema_objects(&conn);
    drop(conn);

    let error = open_worker_db(&path, SHORT_WAIT)
        .err()
        .expect("a newer schema is refused");

    assert!(error.contains("newer version of Ferrous"), "{error}");
    let conn = Connection::open(&path).expect("reopen");
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION + 1);
    assert_eq!(schema_objects(&conn), schema);
    assert_eq!(library_rows(&conn), before);
    drop(conn);
    assert_eq!(entries(&folder), ["library.sqlite3"]);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_file_that_is_not_a_database_is_moved_aside_and_reported() {
    let folder = test_folder("not-a-database");
    let path = folder.join("library.sqlite3");
    let garbage = b"this is not an SQLite database, but the user's file all the same".repeat(100);
    fs::write(&path, &garbage).expect("damaged file");

    let OpenedLibrary { conn, notice } =
        open_worker_db(&path, SHORT_WAIT).expect("a new library starts");

    let notice = notice.expect("the move is reported");
    let names = entries(&folder);
    let kept = names
        .iter()
        .find(|name| name.starts_with("library.sqlite3.damaged-"))
        .unwrap_or_else(|| panic!("no damaged copy in {names:?}"));
    let kept = folder.join(kept);
    assert!(notice.contains(&kept.display().to_string()), "{notice}");
    assert!(
        notice.contains("add your library folders again"),
        "{notice}"
    );
    assert_eq!(
        fs::read(kept.join("library.sqlite3")).expect("kept copy"),
        garbage
    );
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
    assert_eq!(table_rows(&conn, "tracks"), Vec::<Vec<_>>::new());
    drop(conn);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_truncated_database_is_moved_aside() {
    let folder = test_folder("truncated");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 400);
    let bytes = fs::read(&path).expect("fixture bytes");
    let truncated = &bytes[..bytes.len() / 3];
    fs::write(&path, truncated).expect("truncate");

    let OpenedLibrary { notice, .. } =
        open_worker_db(&path, SHORT_WAIT).expect("a new library starts");

    assert!(notice.is_some_and(|notice| notice.contains("damaged")));
    let kept = entries(&folder)
        .into_iter()
        .find(|name| name.starts_with("library.sqlite3.damaged-"))
        .expect("damaged copy");
    assert_eq!(
        fs::read(folder.join(kept).join("library.sqlite3")).expect("kept copy"),
        truncated
    );
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn a_locked_database_is_reported_and_not_moved() {
    let folder = test_folder("locked");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 10);
    let holder = Connection::open(&path).expect("other program");
    holder
        .execute_batch("BEGIN EXCLUSIVE")
        .expect("exclusive lock");

    let error = open_worker_db(&path, SHORT_WAIT)
        .err()
        .expect("the lock is reported");

    assert!(error.contains("locked"), "{error}");
    holder.execute_batch("ROLLBACK").expect("release");
    let rows = table_rows(&holder, "tracks").len();
    assert_eq!(rows, 30);
    drop(holder);
    assert_eq!(entries(&folder), ["library.sqlite3"]);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[cfg(unix)]
#[test]
fn an_unreadable_database_is_reported_and_not_moved() {
    use std::os::unix::fs::PermissionsExt;

    let folder = test_folder("unreadable");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 10);
    let original = fs::read(&path).expect("fixture bytes");
    fs::set_permissions(&path, fs::Permissions::from_mode(0o000)).expect("chmod");
    if fs::File::open(&path).is_ok() {
        // Running with permission checks off, as root does; nothing to test.
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).expect("chmod back");
        fs::remove_dir_all(folder).expect("cleanup");
        return;
    }

    let result = open_worker_db(&path, SHORT_WAIT);

    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).expect("chmod back");
    let error = result.err().expect("the failure is reported");
    assert!(
        error.contains("could not open the library database"),
        "{error}"
    );
    assert_eq!(entries(&folder), ["library.sqlite3"]);
    assert_eq!(fs::read(&path).expect("file kept"), original);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn every_connection_reads_and_writes_while_another_holds_a_read_transaction() {
    let folder = test_folder("wal-readers");
    let path = folder.join("library.sqlite3");
    build_version_0_library(&path, 10);
    let OpenedLibrary { conn: worker, .. } = open_worker_db(&path, SHORT_WAIT).expect("open");
    // A reader in the middle of reading, such as the library snapshot.
    worker.execute_batch("BEGIN").expect("read transaction");
    let tracks: i64 = worker
        .query_row("SELECT COUNT(*) FROM tracks", [], |row| row.get(0))
        .expect("read");
    assert_eq!(tracks, 30);

    // A tag edit's connection writes without waiting, and the reader keeps
    // its snapshot until it ends.
    let editor = open_library_db_at(&path, Duration::ZERO).expect("editor");
    init_schema(&editor).expect("schema is current");
    assert_eq!(journal_mode(&editor), "wal");
    editor
        .execute("UPDATE tracks SET title = 'Edited' WHERE rowid = 1", [])
        .expect("write beside a reader");
    let title: String = worker
        .query_row("SELECT title FROM tracks WHERE rowid = 1", [], |row| {
            row.get(0)
        })
        .expect("read");
    assert_ne!(title, "Edited");
    worker.execute_batch("COMMIT").expect("end read");

    // A writer of the library and a reader of the waveform cache in the same file.
    worker.execute_batch("BEGIN IMMEDIATE").expect("write lock");
    let waveforms = Connection::open(&path).expect("analysis connection");
    waveforms.busy_timeout(Duration::ZERO).expect("no waits");
    let cached: i64 = waveforms
        .query_row("SELECT COUNT(*) FROM waveform_cache", [], |row| row.get(0))
        .expect("read beside a writer");
    assert_eq!(cached, 1);
    worker.execute_batch("ROLLBACK").expect("release");

    checkpoint(&worker).expect("checkpoint");
    drop((worker, editor, waveforms));
    let conn = Connection::open(&path).expect("reopen");
    let title: String = conn
        .query_row(
            "SELECT title FROM tracks WHERE rowid = ?1",
            params![1],
            |row| row.get(0),
        )
        .expect("read");
    assert_eq!(title, "Edited");
    drop(conn);
    fs::remove_dir_all(folder).expect("cleanup");
}

#[test]
fn timestamps_are_utc_dates_and_times() {
    let at = |secs: u64| utc_timestamp(UNIX_EPOCH + Duration::from_secs(secs));
    assert_eq!(at(0), "19700101-000000");
    assert_eq!(at(951_868_799), "20000229-235959");
    assert_eq!(at(1_792_933_445), "20261025-130405");
}

#[test]
fn moving_aside_takes_the_side_files_and_never_reuses_a_folder() {
    let folder = test_folder("move-twice");
    let path = folder.join("library.sqlite3");
    let now = UNIX_EPOCH + Duration::from_secs(1_792_933_445);
    fs::write(&path, b"first").expect("first");
    fs::write(folder.join("library.sqlite3-wal"), b"log").expect("log");
    fs::write(folder.join("library.sqlite3-journal"), b"journal").expect("journal");
    let first = move_aside(&path, now).expect("first move");
    assert_eq!(
        fs::read(first.join("library.sqlite3-wal")).expect("log"),
        b"log"
    );
    assert_eq!(
        fs::read(first.join("library.sqlite3-journal")).expect("journal"),
        b"journal"
    );
    fs::write(&path, b"second").expect("second");
    let second = move_aside(&path, now).expect("second move");

    assert_ne!(first, second);
    assert_eq!(
        fs::read(first.join("library.sqlite3")).expect("first"),
        b"first"
    );
    assert_eq!(
        fs::read(second.join("library.sqlite3")).expect("second"),
        b"second"
    );
    fs::remove_dir_all(folder).expect("cleanup");
}

/// The oldest `SQLite` the library can use, 3.51.3.
const MIN_SQLITE_VERSION: i32 = 3_051_003;

#[test]
fn the_linked_sqlite_is_at_least_3_51_3() {
    assert!(
        rusqlite::version_number() >= MIN_SQLITE_VERSION,
        "SQLite {} is linked, but the library needs 3.51.3 or later (#218): 3.51.0 and 3.51.1 \
         can deadlock when one thread closes a write-ahead-logging connection while another \
         opens the same file, and versions before 3.51.3 can corrupt a database when the log \
         is reset",
        rusqlite::version()
    );
}

/// Names the database that `wal_close_open_stress` uses, in the child process
/// that `closing_and_opening_connections_never_deadlock` starts.
const WAL_STRESS_DB_ENV: &str = "FERROUS_TEST_WAL_STRESS_DB";
/// How long the child process closes and opens connections.
const WAL_STRESS_RUN: Duration = Duration::from_secs(1);
/// How long the child process may take before it counts as deadlocked.
const WAL_STRESS_LIMIT: Duration = Duration::from_secs(30);

/// `SQLite` 3.51.1 could deadlock when one thread closed the last connection to
/// a file in write-ahead logging while another thread opened the file (#218).
/// A deadlocked thread holds `SQLite`'s process-wide lock and would stop every
/// later test, so the stress runs in a child process, which is killed if it
/// does not finish in time.
#[test]
fn closing_and_opening_connections_never_deadlock() {
    let folder = test_folder("wal-close-open");
    let path = folder.join("library.sqlite3");
    let opened = open_worker_db(&path, SHORT_WAIT).expect("open");
    assert!(opened.notice.is_none(), "{:?}", opened.notice);
    drop(opened);

    let module = module_path!()
        .split_once("::")
        .map_or(module_path!(), |(_, module)| module);
    let name = format!("{module}::wal_close_open_stress");
    let mut child = Command::new(std::env::current_exe().expect("test binary"))
        .args(["--exact", &name, "--ignored"])
        .env(WAL_STRESS_DB_ENV, &path)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("start the stress");
    let started = Instant::now();
    while child.try_wait().expect("stress status").is_none() {
        if started.elapsed() > WAL_STRESS_LIMIT {
            let _ = child.kill();
            let _ = child.wait();
            panic!(
                "closing and opening connections to one database did not finish in {:?}; \
                 SQLite {} deadlocked (#218)",
                WAL_STRESS_LIMIT,
                rusqlite::version()
            );
        }
        thread::sleep(SHORT_WAIT);
    }
    let output = child.wait_with_output().expect("stress output");
    let stdout = String::from_utf8_lossy(&output.stdout);
    assert!(
        output.status.success() && stdout.contains(&format!("test {name} ... ok")),
        "{stdout}{}",
        String::from_utf8_lossy(&output.stderr)
    );
    fs::remove_dir_all(folder).expect("cleanup");
}

/// The child process's part of `closing_and_opening_connections_never_deadlock`.
/// One thread opens, reads, and closes a connection, so that each close is the
/// last one and takes down the write-ahead log, while another thread opens and
/// closes the file without reading it. One opener is enough to deadlock
/// 3.51.1 within a second, and keeps the load on the other tests low.
#[test]
#[ignore = "run in a child process by closing_and_opening_connections_never_deadlock"]
fn wal_close_open_stress() {
    let Some(path) = std::env::var_os(WAL_STRESS_DB_ENV).map(PathBuf::from) else {
        return;
    };
    let stop = Arc::new(AtomicBool::new(false));
    let closer = {
        let (path, stop) = (path.clone(), Arc::clone(&stop));
        thread::spawn(move || {
            let mut closes = 0_u64;
            while !stop.load(Ordering::Relaxed) {
                let conn = open_library_db_at(&path, SHORT_WAIT).expect("open");
                let _: i64 = conn
                    .query_row("SELECT COUNT(*) FROM tracks", [], |row| row.get(0))
                    .expect("read");
                drop(conn);
                closes += 1;
            }
            closes
        })
    };
    let opener = {
        let stop = Arc::clone(&stop);
        thread::spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                drop(Connection::open(&path).expect("open"));
            }
        })
    };
    thread::sleep(WAL_STRESS_RUN);
    stop.store(true, Ordering::Relaxed);
    let closes = closer.join().expect("closer");
    opener.join().expect("opener");
    assert!(closes > 0, "no connection closed");
}
