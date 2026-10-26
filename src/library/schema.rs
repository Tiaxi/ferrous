// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use rusqlite::{params, Connection, OptionalExtension, TransactionBehavior};

use super::LibraryTrack;

/// The schema version this build reads and writes, kept in the database's
/// `PRAGMA user_version`.
///
/// - 0: every database from before schema versions. It may lack columns
///   added since, and has the full-text index `tracks_fts`.
/// - 1: every column, and no full-text index, which nothing queried.
/// - 2: `tracks.cover_stamp`, the stamp of the folder images a row's cover
///   was chosen from. Rows from before have none, so the next scan checks
///   their covers again, once, by the current rule.
/// - 3: the waveform cache, which the analysis worker keeps in the same file,
///   belongs to the schema, and its rows tell whether the decode reached the
///   end of the file (`complete`) and how much of the audio it covered
///   (`coverage_seconds`). Rows from before tell neither, since a decode
///   error could have cut them short, so the next play of each track decodes
///   its waveform again, once. `updated_at` is the time of the last use.
/// - 4: `tracks` and `waveform_cache` rows have the identity of their file
///   (`file_device`, `file_inode`; see [`crate::file_identity::FileId`]), which tells it
///   from another file of the same size and time, and an index finds the
///   rows of a size and time. Rows from before have none, and take that of
///   the file they match next, without being read or decoded again; see
///   [`migrate_to_version_4`].
pub(super) const SCHEMA_VERSION: i32 = 4;

/// How long a connection waits for another connection's write lock.
pub(super) const BUSY_TIMEOUT: Duration = Duration::from_secs(5);

/// Opens the library database without preparing its schema; see
/// [`init_schema`].
pub(super) fn open_library_db() -> anyhow::Result<Connection> {
    open_library_db_at(&crate::paths::library_db_path()?, BUSY_TIMEOUT)
}

pub(super) fn open_library_db_at(
    path: &Path,
    busy_timeout: Duration,
) -> anyhow::Result<Connection> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let conn = Connection::open(path)?;
    conn.busy_timeout(busy_timeout)?;
    Ok(conn)
}

pub(super) fn u64_to_i64(value: u64) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

pub(super) fn u128_to_i64(value: u128) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

pub(super) fn usize_to_f32(value: usize) -> f32 {
    value.to_string().parse::<f32>().unwrap_or(f32::MAX)
}

/// The library tables as of version 3. A database from before a column was
/// added gets it from [`add_missing_columns`]; version 4 adds its own
/// columns in [`migrate_to_version_4`].
const CREATE_TABLES: &str = r"
    CREATE TABLE IF NOT EXISTS roots (
        path TEXT PRIMARY KEY,
        name TEXT NOT NULL DEFAULT '',
        added_at INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS tracks (
        path TEXT PRIMARY KEY,
        root_path TEXT NOT NULL DEFAULT '',
        title TEXT NOT NULL,
        artist TEXT NOT NULL,
        album TEXT NOT NULL,
        cover_path TEXT NOT NULL DEFAULT '',
        cover_checked INTEGER NOT NULL DEFAULT 0,
        genre TEXT NOT NULL DEFAULT '',
        year INTEGER,
        track_no INTEGER,
        duration_secs REAL,
        mtime_ns INTEGER NOT NULL,
        size_bytes INTEGER NOT NULL,
        indexed_at INTEGER NOT NULL,
        search_tags TEXT,
        cover_stamp INTEGER
    );

    CREATE TABLE IF NOT EXISTS external_tracks (
        path TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        artist TEXT NOT NULL,
        album TEXT NOT NULL,
        cover_path TEXT NOT NULL DEFAULT '',
        genre TEXT NOT NULL DEFAULT '',
        year INTEGER,
        track_no INTEGER,
        duration_secs REAL,
        mtime_ns INTEGER NOT NULL,
        size_bytes INTEGER NOT NULL,
        indexed_at INTEGER NOT NULL,
        search_tags TEXT
    );
";

/// The analysis worker's waveform cache as of version 3, when it joined the
/// schema. Before, the worker created it when it could, so a database of any
/// earlier version may lack it.
const CREATE_WAVEFORM_CACHE: &str = r"
    CREATE TABLE IF NOT EXISTS waveform_cache (
        path TEXT PRIMARY KEY,
        size_bytes INTEGER NOT NULL,
        modified_secs INTEGER NOT NULL,
        modified_nanos INTEGER NOT NULL,
        format_version INTEGER NOT NULL,
        peak_count INTEGER NOT NULL,
        peaks_blob BLOB NOT NULL,
        updated_at INTEGER NOT NULL,
        complete INTEGER NOT NULL DEFAULT 0,
        coverage_seconds REAL
    );
";

/// Columns that version 3 added to a waveform cache the worker created.
const WAVEFORM_CACHE_ADDED_COLUMNS: &[(&str, &str, &str)] = &[
    ("waveform_cache", "complete", "INTEGER NOT NULL DEFAULT 0"),
    ("waveform_cache", "coverage_seconds", "REAL"),
];

/// The columns of version 4: the identity of each row's file.
const FILE_ID_COLUMNS: &[(&str, &str, &str)] = &[
    ("tracks", "file_device", "INTEGER"),
    ("tracks", "file_inode", "INTEGER"),
    ("waveform_cache", "file_device", "INTEGER"),
    ("waveform_cache", "file_inode", "INTEGER"),
];

/// Columns that later versions added to the library tables, with the
/// definitions that add them. Every other column of [`CREATE_TABLES`] has
/// been there from the start.
const ADDED_COLUMNS: &[(&str, &str, &str)] = &[
    ("roots", "name", "TEXT NOT NULL DEFAULT ''"),
    ("tracks", "root_path", "TEXT NOT NULL DEFAULT ''"),
    ("tracks", "cover_path", "TEXT NOT NULL DEFAULT ''"),
    ("tracks", "cover_checked", "INTEGER NOT NULL DEFAULT 0"),
    ("tracks", "genre", "TEXT NOT NULL DEFAULT ''"),
    ("tracks", "year", "INTEGER"),
    ("tracks", "track_no", "INTEGER"),
    ("tracks", "duration_secs", "REAL"),
    ("tracks", "search_tags", "TEXT"),
    ("tracks", "cover_stamp", "INTEGER"),
    ("external_tracks", "cover_path", "TEXT NOT NULL DEFAULT ''"),
    ("external_tracks", "genre", "TEXT NOT NULL DEFAULT ''"),
    ("external_tracks", "year", "INTEGER"),
    ("external_tracks", "track_no", "INTEGER"),
    ("external_tracks", "duration_secs", "REAL"),
    ("external_tracks", "search_tags", "TEXT"),
];

const CREATE_INDEXES: &str = r"
    CREATE INDEX IF NOT EXISTS idx_tracks_root_path ON tracks(root_path);
    CREATE INDEX IF NOT EXISTS idx_tracks_root_path_path_nocase
        ON tracks(root_path COLLATE NOCASE, path COLLATE NOCASE);
    CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
    CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album);
    CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title);
    CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);
    CREATE INDEX IF NOT EXISTS idx_external_tracks_indexed_at ON external_tracks(indexed_at);
";

/// The full-text index of version 0 and the triggers that kept it in step
/// with `tracks`. It is derived from `tracks` alone, which dropping it
/// leaves as it is.
const DROP_FULL_TEXT_INDEX: &str = r"
    DROP TRIGGER IF EXISTS tracks_fts_ai;
    DROP TRIGGER IF EXISTS tracks_fts_ad;
    DROP TRIGGER IF EXISTS tracks_fts_au;
    DROP TABLE IF EXISTS tracks_fts;
";

/// Brings the schema to [`SCHEMA_VERSION`]. A database already there is
/// left alone, so connections other than the library worker's can call this
/// cheaply each time they open the database.
///
/// The migration runs in one transaction: it either completes, or leaves
/// the database as it was and returns the error.
///
/// # Errors
///
/// Fails if the database cannot be read or written, if it was written by a
/// newer version of Ferrous, or if a migration step fails.
pub(super) fn init_schema(conn: &Connection) -> anyhow::Result<()> {
    if check_version(user_version(conn)?)? {
        return Ok(());
    }
    // Take the write lock first, so that the version read below cannot be
    // stale: another connection may have migrated while this one waited.
    let tx = rusqlite::Transaction::new_unchecked(conn, TransactionBehavior::Immediate)?;
    let version = user_version(&tx)?;
    if check_version(version)? {
        return Ok(());
    }
    if version < 1 {
        migrate_to_version_1(&tx)?;
    }
    if version < 2 {
        migrate_to_version_2(&tx)?;
    }
    if version < 3 {
        migrate_to_version_3(&tx)?;
    }
    if version < 4 {
        migrate_to_version_4(&tx)?;
    }
    tx.pragma_update(None, "user_version", SCHEMA_VERSION)?;
    tx.commit()?;
    Ok(())
}

/// Whether `version` is current. Fails for a version newer than this build
/// knows, whose schema it must not write to.
fn check_version(version: i32) -> anyhow::Result<bool> {
    if version > SCHEMA_VERSION {
        anyhow::bail!(
            "the library database has schema version {version}, from a newer version of \
             Ferrous; this version supports version {SCHEMA_VERSION} and leaves it unchanged"
        );
    }
    Ok(version == SCHEMA_VERSION)
}

pub(super) fn user_version(conn: &Connection) -> rusqlite::Result<i32> {
    conn.pragma_query_value(None, "user_version", |row| row.get(0))
}

/// Version 0 to 1: create what is missing, and drop the full-text index.
fn migrate_to_version_1(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(CREATE_TABLES)?;
    add_missing_columns(conn, ADDED_COLUMNS)?;
    // Indexes come after the columns they cover.
    conn.execute_batch(CREATE_INDEXES)?;
    conn.execute_batch(DROP_FULL_TEXT_INDEX)?;
    Ok(())
}

/// Version 1 to 2: add `tracks.cover_stamp`, NULL in every row, which makes
/// the next scan check each row's cover again; see
/// [`super::LibraryTrack::cover_stamp`]. A database from version 0 already
/// has the column from [`migrate_to_version_1`].
fn migrate_to_version_2(conn: &Connection) -> anyhow::Result<()> {
    add_missing_columns(conn, ADDED_COLUMNS)
}

/// Version 2 to 3: create the waveform cache if the analysis worker never
/// did, or add `complete` and `coverage_seconds` to the one it created: 0
/// and NULL in every row, so each saved waveform is decoded again, once;
/// see [`SCHEMA_VERSION`].
fn migrate_to_version_3(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(CREATE_WAVEFORM_CACHE)?;
    add_missing_columns(conn, WAVEFORM_CACHE_ADDED_COLUMNS)?;
    conn.execute_batch(
        "CREATE INDEX IF NOT EXISTS idx_waveform_cache_updated_at ON waveform_cache(updated_at)",
    )?;
    Ok(())
}

/// Version 3 to 4: add the identity of each row's file to `tracks` and to
/// the waveform cache, which [`migrate_to_version_3`] made sure of, NULL in
/// every row, and index the tracks by size and time, which tell when an
/// identity breaks a tie (see [`super::identity_contradicts`]).
///
/// A row without an identity matches the file at its path by size and time
/// alone, as before, and takes the file's identity then: a scan writes it
/// without reading the file's tags again, and the analysis worker when it
/// loads the saved waveform. So the first scan reads no file, and no
/// waveform is decoded again. That trusts size and time once more for each
/// row, which a swap of two files with the same size and time since the
/// row was written deceives, as it did before; the rows that the library
/// renamed moved with their files.
///
/// Version 3 made the waveform cache. One deleted since, which no build
/// does, is made again, empty, as version 3 made it.
fn migrate_to_version_4(conn: &Connection) -> anyhow::Result<()> {
    if table_columns(conn, "waveform_cache")?.is_empty() {
        migrate_to_version_3(conn)?;
    }
    add_missing_columns(conn, FILE_ID_COLUMNS)?;
    conn.execute_batch(
        "CREATE INDEX IF NOT EXISTS idx_tracks_size_mtime ON tracks(size_bytes, mtime_ns)",
    )?;
    Ok(())
}

/// Add those of `columns`, `(table, column, definition)`, that their tables
/// lack.
fn add_missing_columns(conn: &Connection, columns: &[(&str, &str, &str)]) -> anyhow::Result<()> {
    for (table, column, definition) in columns {
        if !table_columns(conn, table)?.contains(*column) {
            conn.execute_batch(&format!(
                "ALTER TABLE {table} ADD COLUMN {column} {definition}"
            ))?;
        }
    }
    Ok(())
}

fn table_columns(conn: &Connection, table: &str) -> rusqlite::Result<HashSet<String>> {
    let mut stmt = conn.prepare("SELECT name FROM pragma_table_info(?1)")?;
    let columns = stmt
        .query_map(params![table], |row| row.get::<_, String>(0))?
        .collect::<rusqlite::Result<HashSet<_>>>()?;
    Ok(columns)
}

pub(super) fn needs_search_tag_backfill(conn: &Connection) -> bool {
    conn.query_row(
        "SELECT EXISTS(SELECT 1 FROM tracks WHERE search_tags IS NULL)",
        [],
        |row| row.get(0),
    )
    .unwrap_or(false)
}

const TRACK_COLUMNS: &str = "path, root_path, title, artist, album, cover_path, genre, year, \
     track_no, duration_secs, search_tags, cover_stamp";

fn track_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<LibraryTrack> {
    Ok(LibraryTrack {
        search_tags: serde_json::from_str(&row.get::<_, Option<String>>(10)?.unwrap_or_default())
            .unwrap_or_default(),
        path: PathBuf::from(row.get::<_, String>(0)?),
        root_path: PathBuf::from(row.get::<_, String>(1)?),
        title: row.get::<_, String>(2)?,
        artist: row.get::<_, String>(3)?,
        album: row.get::<_, String>(4)?,
        cover_path: row.get::<_, String>(5)?,
        cover_stamp: stamp_from_column(row.get(11)?),
        // A stored row says nothing of when its cover was read.
        cover_read_at: super::CoverReadAt::default(),
        genre: row.get::<_, String>(6)?,
        year: row
            .get::<_, Option<i64>>(7)?
            .and_then(|v| i32::try_from(v).ok()),
        track_no: row
            .get::<_, Option<i64>>(8)?
            .and_then(|v| u32::try_from(v).ok()),
        duration_secs: row.get::<_, Option<f32>>(9)?,
    })
}

/// A `cover_stamp` column value as a stamp; NULL is "not known", zero.
pub(super) fn stamp_from_column(value: Option<i64>) -> u64 {
    value.map_or(0, i64::cast_unsigned)
}

/// A stamp as a `cover_stamp` column value; zero, "not known", is NULL.
pub(super) fn stamp_to_column(stamp: u64) -> Option<i64> {
    (stamp != 0).then(|| stamp.cast_signed())
}

pub(super) fn load_snapshot(conn: &Connection, snapshot: &mut super::LibrarySnapshot) {
    snapshot.roots = load_roots(conn);
    snapshot.tracks.clear();

    // The order of `track_order`, which incremental changes keep: paths
    // that `NOCASE` finds equal, such as `song.flac` and `Song.flac`, are
    // ordered by their exact bytes. The index covers the `NOCASE` terms, so
    // only such ties are sorted.
    if let Ok(mut stmt) = conn.prepare(&format!(
        "SELECT {TRACK_COLUMNS} FROM tracks \
         ORDER BY root_path COLLATE NOCASE, path COLLATE NOCASE, \
         root_path COLLATE BINARY, path COLLATE BINARY"
    )) {
        if let Ok(rows) = stmt.query_map([], track_from_row) {
            for row in rows.flatten() {
                snapshot.tracks.push(std::sync::Arc::new(row));
            }
        }
    }
}

/// The rows of those of `paths` that the library has.
pub(super) fn load_tracks(
    conn: &Connection,
    paths: &[PathBuf],
) -> Result<Vec<LibraryTrack>, String> {
    let mut stmt = conn
        .prepare_cached(&format!(
            "SELECT {TRACK_COLUMNS} FROM tracks WHERE path = ?1"
        ))
        .map_err(|error| format!("could not read library tracks: {error}"))?;
    let mut tracks = Vec::new();
    for path in paths {
        if let Some(track) = stmt
            .query_row(params![path.to_string_lossy()], track_from_row)
            .optional()
            .map_err(|error| format!("could not read '{}': {error}", path.display()))?
        {
            tracks.push(track);
        }
    }
    Ok(tracks)
}

pub(super) fn load_roots(conn: &Connection) -> Vec<super::LibraryRoot> {
    let mut roots = Vec::new();
    if let Ok(mut stmt) = conn.prepare("SELECT path, name FROM roots ORDER BY path COLLATE NOCASE")
    {
        if let Ok(rows) = stmt.query_map([], |row| {
            Ok(super::LibraryRoot {
                path: PathBuf::from(row.get::<_, String>(0)?),
                name: normalize_root_name(&row.get::<_, String>(1)?),
            })
        }) {
            for row in rows.flatten() {
                roots.push(row);
            }
        }
    }
    roots
}

pub(super) fn normalize_root_name(name: &str) -> String {
    name.trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init_schema_migrates_legacy_tracks_before_index_creation() {
        let conn = Connection::open_in_memory().expect("db");
        conn.execute_batch(
            r"
            CREATE TABLE tracks (
                path TEXT PRIMARY KEY,
                title TEXT NOT NULL,
                artist TEXT NOT NULL,
                album TEXT NOT NULL,
                mtime_ns INTEGER NOT NULL,
                size_bytes INTEGER NOT NULL,
                indexed_at INTEGER NOT NULL
            );
            ",
        )
        .expect("legacy schema");

        init_schema(&conn).expect("schema migration");

        let mut has_root_path = false;
        let mut stmt = conn
            .prepare("SELECT name FROM pragma_table_info('tracks')")
            .expect("pragma table_info");
        let rows = stmt
            .query_map([], |row| row.get::<_, String>(0))
            .expect("query columns");
        for name in rows.flatten() {
            if name == "root_path" {
                has_root_path = true;
                break;
            }
        }
        assert!(
            has_root_path,
            "tracks.root_path should exist after migration"
        );
    }

    /// Takes a current database back to version 3: without the identities
    /// of files and the index by size and time.
    fn back_to_version_3(conn: &Connection) {
        conn.execute_batch(
            "DROP INDEX idx_tracks_size_mtime;
             ALTER TABLE tracks DROP COLUMN file_device;
             ALTER TABLE tracks DROP COLUMN file_inode;
             ALTER TABLE waveform_cache DROP COLUMN file_device;
             ALTER TABLE waveform_cache DROP COLUMN file_inode;
             PRAGMA user_version = 3;",
        )
        .expect("version 3 schema");
    }

    /// The `CREATE` statements of the schema.
    fn schema_sql(conn: &Connection) -> Vec<String> {
        conn.prepare("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY name")
            .expect("prepare")
            .query_map([], |row| row.get(0))
            .expect("schema")
            .collect::<rusqlite::Result<_>>()
            .expect("schema rows")
    }

    #[test]
    fn version_1_rows_get_an_unknown_cover_stamp_so_the_next_scan_checks_them() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        back_to_version_3(&conn);
        // Version 1 is the same schema without `tracks.cover_stamp`.
        conn.execute_batch("ALTER TABLE tracks DROP COLUMN cover_stamp; PRAGMA user_version = 1;")
            .expect("version 1 schema");
        conn.execute(
            "INSERT INTO tracks(path, title, artist, album, cover_path, cover_checked, \
             mtime_ns, size_bytes, indexed_at) \
             VALUES ('/music/a.flac', 'A', 'Artist', 'Album', '/music/back.jpg', 1, 1, 2, 3)",
            [],
        )
        .expect("version 1 row");

        init_schema(&conn).expect("migration");

        assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
        let (cover, stamp): (String, Option<i64>) = conn
            .query_row("SELECT cover_path, cover_stamp FROM tracks", [], |row| {
                Ok((row.get(0)?, row.get(1)?))
            })
            .expect("migrated row");
        assert_eq!(cover, "/music/back.jpg", "the cover stays until a scan");
        assert_eq!(stamp, None);
        let mut snapshot = super::super::LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks[0].cover_stamp, 0, "a stamp no folder has");
    }

    /// The waveform cache as the analysis worker created it before version
    /// 3, outside the schema.
    const VERSION_2_WAVEFORM_CACHE: &str = r"
        CREATE TABLE IF NOT EXISTS waveform_cache (
            path TEXT PRIMARY KEY,
            size_bytes INTEGER NOT NULL,
            modified_secs INTEGER NOT NULL,
            modified_nanos INTEGER NOT NULL,
            format_version INTEGER NOT NULL,
            peak_count INTEGER NOT NULL,
            peaks_blob BLOB NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_waveform_cache_updated_at
            ON waveform_cache(updated_at);
    ";

    /// A version 2 database, with the waveform cache if `waveforms`, holding
    /// a track and, then, two waveforms.
    fn version_2_database(waveforms: bool) -> Connection {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        back_to_version_3(&conn);
        conn.execute_batch("DROP TABLE waveform_cache; PRAGMA user_version = 2;")
            .expect("version 2 schema");
        conn.execute(
            "INSERT INTO tracks(path, title, artist, album, mtime_ns, size_bytes, indexed_at, \
             cover_stamp) VALUES ('/music/a.flac', 'A', 'Artist', 'Album', 1, 2, 3, 77)",
            [],
        )
        .expect("track row");
        if waveforms {
            conn.execute_batch(VERSION_2_WAVEFORM_CACHE)
                .expect("waveform cache");
            for (path, updated_at) in [("/music/a.flac", 100), ("/music/b.flac", 200)] {
                conn.execute(
                    "INSERT INTO waveform_cache(path, size_bytes, modified_secs, \
                     modified_nanos, format_version, peak_count, peaks_blob, updated_at) \
                     VALUES (?1, 10, 1, 0, 2, 1, x'0000003f', ?2)",
                    params![path, updated_at],
                )
                .expect("waveform row");
            }
        }
        conn
    }

    fn rows(conn: &Connection, table: &str) -> Vec<Vec<rusqlite::types::Value>> {
        let mut stmt = conn
            .prepare(&format!("SELECT * FROM {table} ORDER BY rowid"))
            .expect("prepare");
        let columns = stmt.column_count();
        stmt.query_map([], |row| {
            (0..columns)
                .map(|column| row.get(column))
                .collect::<rusqlite::Result<Vec<_>>>()
        })
        .expect("query")
        .collect::<rusqlite::Result<Vec<_>>>()
        .expect("rows")
    }

    #[test]
    fn version_2_waveforms_are_kept_and_marked_for_one_more_decode() {
        use rusqlite::types::Value;
        let conn = version_2_database(true);
        let tracks = rows(&conn, "tracks");
        let mut waveforms = rows(&conn, "waveform_cache");

        init_schema(&conn).expect("migration");

        assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
        let mut tracks = tracks;
        for row in &mut tracks {
            row.extend([Value::Null, Value::Null]);
        }
        assert_eq!(rows(&conn, "tracks"), tracks);
        // Not known to be complete, of unknown coverage, and of a file of
        // unknown identity.
        for row in &mut waveforms {
            row.extend([Value::Integer(0), Value::Null, Value::Null, Value::Null]);
        }
        assert_eq!(rows(&conn, "waveform_cache"), waveforms);

        // A second run changes nothing.
        let schema = schema_sql(&conn);
        let changes = conn.total_changes();
        init_schema(&conn).expect("second run");
        assert_eq!(conn.total_changes(), changes);
        assert_eq!(rows(&conn, "waveform_cache"), waveforms);
        assert_eq!(schema_sql(&conn), schema);
    }

    /// A version 1 database: one without `tracks.cover_stamp`, holding a
    /// track and, with `waveforms`, the analysis worker's cache with a
    /// waveform. A version 1 library may lack the cache, as when its
    /// creation timed out behind another writer.
    fn version_1_database(waveforms: bool) -> Connection {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        back_to_version_3(&conn);
        conn.execute_batch(
            "DROP TABLE waveform_cache; ALTER TABLE tracks DROP COLUMN cover_stamp; \
             PRAGMA user_version = 1;",
        )
        .expect("version 1 schema");
        conn.execute(
            "INSERT INTO tracks(path, title, artist, album, mtime_ns, size_bytes, indexed_at) \
             VALUES ('/music/a.flac', 'A', 'Artist', 'Album', 1, 2, 3)",
            [],
        )
        .expect("track row");
        if waveforms {
            conn.execute_batch(VERSION_2_WAVEFORM_CACHE)
                .expect("waveform cache");
            conn.execute(
                "INSERT INTO waveform_cache(path, size_bytes, modified_secs, modified_nanos, \
                 format_version, peak_count, peaks_blob, updated_at) \
                 VALUES ('/music/a.flac', 10, 1, 0, 2, 1, x'0000003f', 100)",
                [],
            )
            .expect("waveform row");
        }
        conn
    }

    #[test]
    fn a_version_1_database_migrates_with_or_without_the_waveform_cache() {
        use rusqlite::types::Value;
        for waveforms in [false, true] {
            let conn = version_1_database(waveforms);
            let mut tracks = rows(&conn, "tracks");
            let mut saved = if waveforms {
                rows(&conn, "waveform_cache")
            } else {
                Vec::new()
            };

            init_schema(&conn).unwrap_or_else(|error| panic!("waveforms {waveforms}: {error:#}"));

            assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
            for row in &mut tracks {
                row.extend([Value::Null, Value::Null, Value::Null]);
            }
            assert_eq!(rows(&conn, "tracks"), tracks, "waveforms {waveforms}");
            for row in &mut saved {
                row.extend([Value::Integer(0), Value::Null, Value::Null, Value::Null]);
            }
            assert_eq!(
                rows(&conn, "waveform_cache"),
                saved,
                "waveforms {waveforms}"
            );
            let columns = table_columns(&conn, "waveform_cache").expect("columns");
            assert!(columns.contains("complete") && columns.contains("coverage_seconds"));

            let changes = conn.total_changes();
            init_schema(&conn).expect("second run");
            assert_eq!(conn.total_changes(), changes, "a second run writes nothing");
        }
    }

    #[test]
    fn a_version_2_database_without_waveforms_gets_the_waveform_cache() {
        let conn = version_2_database(false);

        init_schema(&conn).expect("migration");

        assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
        let columns = table_columns(&conn, "waveform_cache").expect("columns");
        assert!(columns.contains("complete") && columns.contains("coverage_seconds"));
        assert!(columns.contains("file_device") && columns.contains("file_inode"));
        let index: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE name = 'idx_waveform_cache_updated_at'",
                [],
                |row| row.get(0),
            )
            .expect("index");
        assert_eq!(index, 1);
        assert_eq!(rows(&conn, "tracks").len(), 1);
    }

    /// A version 3 database holding a track and two waveforms, one of a file
    /// that is no track. Without `waveforms`, it lacks the waveform cache,
    /// as no build leaves a version 3 database.
    fn version_3_database(waveforms: bool) -> Connection {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        back_to_version_3(&conn);
        conn.execute(
            "INSERT INTO tracks(path, title, artist, album, mtime_ns, size_bytes, indexed_at, \
             cover_stamp) VALUES ('/music/a.flac', 'A', 'Artist', 'Album', 1, 2, 3, 77)",
            [],
        )
        .expect("track row");
        for (path, complete) in [("/music/a.flac", 1), ("/elsewhere/b.flac", 0)] {
            conn.execute(
                "INSERT INTO waveform_cache(path, size_bytes, modified_secs, modified_nanos, \
                 format_version, peak_count, peaks_blob, updated_at, complete, \
                 coverage_seconds) VALUES (?1, 10, 1, 0, 3, 1, x'0000003f', 100, ?2, 2.5)",
                params![path, complete],
            )
            .expect("waveform row");
        }
        if !waveforms {
            conn.execute_batch("DROP TABLE waveform_cache;")
                .expect("no waveform cache");
        }
        conn
    }

    #[test]
    fn a_version_3_database_keeps_every_row_with_an_unknown_file_identity() {
        use rusqlite::types::Value;
        for waveforms in [true, false] {
            let conn = version_3_database(waveforms);
            let mut tracks = rows(&conn, "tracks");
            let mut saved = if waveforms {
                rows(&conn, "waveform_cache")
            } else {
                Vec::new()
            };

            init_schema(&conn).unwrap_or_else(|error| panic!("waveforms {waveforms}: {error:#}"));

            assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
            // Each row takes the identity of the file it matches next.
            for row in tracks.iter_mut().chain(&mut saved) {
                row.extend([Value::Null, Value::Null]);
            }
            assert_eq!(rows(&conn, "tracks"), tracks, "waveforms {waveforms}");
            assert_eq!(rows(&conn, "waveform_cache"), saved);
            let columns = table_columns(&conn, "waveform_cache").expect("columns");
            assert!(columns.contains("complete") && columns.contains("file_inode"));
            let index: i64 = conn
                .query_row(
                    "SELECT COUNT(*) FROM sqlite_master WHERE name = 'idx_tracks_size_mtime'",
                    [],
                    |row| row.get(0),
                )
                .expect("index");
            assert_eq!(index, 1);

            let schema = schema_sql(&conn);
            let changes = conn.total_changes();
            init_schema(&conn).expect("second run");
            assert_eq!(conn.total_changes(), changes, "a second run writes nothing");
            assert_eq!(schema_sql(&conn), schema);
            assert_eq!(rows(&conn, "tracks"), tracks);
        }
    }

    #[test]
    fn the_version_4_step_is_idempotent() {
        let conn = version_3_database(true);
        let tx = conn.unchecked_transaction().expect("transaction");
        migrate_to_version_4(&tx).expect("first");
        let schema = schema_sql(&tx);
        let rows_after = rows(&tx, "waveform_cache");
        migrate_to_version_4(&tx).expect("second");
        assert_eq!(schema_sql(&tx), schema);
        assert_eq!(rows(&tx, "waveform_cache"), rows_after);
    }
}
