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
pub(super) const SCHEMA_VERSION: i32 = 1;

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

/// The tables of the current schema. A database from before a column was
/// added gets it from [`add_missing_columns`].
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
        search_tags TEXT
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

/// Columns that later versions added, with the definitions that add them.
/// Every other column of [`CREATE_TABLES`] has been there from the start.
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
    add_missing_columns(conn)?;
    // Indexes come after the columns they cover.
    conn.execute_batch(CREATE_INDEXES)?;
    conn.execute_batch(DROP_FULL_TEXT_INDEX)?;
    Ok(())
}

fn add_missing_columns(conn: &Connection) -> anyhow::Result<()> {
    for (table, column, definition) in ADDED_COLUMNS {
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
     track_no, duration_secs, search_tags";

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
}
