// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::PathBuf;
use std::time::Duration;

use rusqlite::{params, Connection};

use super::{LibrarySearchTrack, LibraryTrack};

pub(super) fn open_library_db() -> anyhow::Result<Connection> {
    let db_path = library_db_path()?;
    if let Some(parent) = db_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let conn = Connection::open(db_path)?;
    Ok(conn)
}

fn library_db_path() -> anyhow::Result<PathBuf> {
    if let Some(xdg_home) = std::env::var_os("XDG_DATA_HOME") {
        return Ok(PathBuf::from(xdg_home)
            .join("ferrous")
            .join("library.sqlite3"));
    }
    let home = std::env::var_os("HOME")
        .ok_or_else(|| anyhow::anyhow!("HOME is not set and XDG_DATA_HOME is missing"))?;
    Ok(PathBuf::from(home)
        .join(".local")
        .join("share")
        .join("ferrous")
        .join("library.sqlite3"))
}

pub(super) fn build_fts_query(raw: &str) -> Option<String> {
    let mut terms = Vec::new();
    for part in raw.split_whitespace() {
        let token = part.trim();
        if token.is_empty() {
            continue;
        }
        let escaped = token.replace('"', "\"\"");
        terms.push(format!("\"{escaped}\"*"));
    }
    if terms.is_empty() {
        None
    } else {
        Some(terms.join(" AND "))
    }
}

pub(super) fn usize_to_i64(value: usize) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
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

pub(super) fn f64_to_f32(value: f64) -> f32 {
    value.to_string().parse::<f32>().unwrap_or_else(|_| {
        if value.is_sign_negative() {
            f32::MIN
        } else {
            f32::MAX
        }
    })
}

/// Search library tracks through the `SQLite` FTS index.
///
/// # Errors
///
/// Returns an error when the library database cannot be opened, the query
/// cannot be prepared, or `SQLite` fails while executing it.
pub fn search_tracks_fts(raw_query: &str, limit: usize) -> Result<Vec<LibrarySearchTrack>, String> {
    let Some(query) = build_fts_query(raw_query) else {
        return Ok(Vec::new());
    };
    let limit = usize_to_i64(limit.clamp(1, 5000));

    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    // Search runs on keystrokes and can coincide with long-running scan writes.
    // Keep lock waits short so we can fail fast and use in-memory fallback.
    let _ = conn.busy_timeout(Duration::from_millis(40));

    let mut out = Vec::new();
    let mut stmt = conn
        .prepare(
            r"
            SELECT
                t.path,
                t.root_path,
                t.title,
                t.artist,
                t.album,
                t.cover_path,
                t.genre,
                t.year,
                t.track_no,
                t.duration_secs,
                bm25(tracks_fts) AS rank
            FROM tracks_fts
            JOIN tracks t ON t.rowid = tracks_fts.rowid
            WHERE tracks_fts MATCH ?1
            ORDER BY rank ASC, t.path COLLATE NOCASE
            LIMIT ?2
            ",
        )
        .map_err(|e| format!("failed to prepare search query: {e}"))?;

    let rows = stmt
        .query_map(params![query, limit], |row| {
            Ok(LibrarySearchTrack {
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
                score: row.get::<_, f64>(10).map_or(0.0, f64_to_f32),
            })
        })
        .map_err(|e| format!("failed to execute search query: {e}"))?;

    for row in rows.flatten() {
        out.push(row);
    }

    Ok(out)
}

pub(super) fn init_schema(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(
        r"
        CREATE TABLE IF NOT EXISTS roots (
            path TEXT PRIMARY KEY,
            name TEXT NOT NULL DEFAULT '',
            added_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS tracks (
            path TEXT PRIMARY KEY,
            root_path TEXT NOT NULL,
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
            indexed_at INTEGER NOT NULL
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
            indexed_at INTEGER NOT NULL
        );
        ",
    )?;

    run_schema_migrations(conn);
    create_library_indexes(conn)?;
    rebuild_fts_if_needed(conn);
    Ok(())
}

fn run_schema_migrations(conn: &Connection) {
    // Migrations for existing DBs created before metadata expansion.
    let _ = conn.execute(
        "ALTER TABLE roots ADD COLUMN name TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN root_path TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN genre TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN cover_path TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN cover_checked INTEGER NOT NULL DEFAULT 0",
        [],
    );
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN year INTEGER", []);
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN track_no INTEGER", []);
}

fn create_library_indexes(conn: &Connection) -> anyhow::Result<()> {
    // Build indexes after migrations so pre-existing DBs without root_path can initialize.
    conn.execute_batch(
        r"
        CREATE INDEX IF NOT EXISTS idx_tracks_root_path ON tracks(root_path);
        CREATE INDEX IF NOT EXISTS idx_tracks_root_path_path_nocase
            ON tracks(root_path COLLATE NOCASE, path COLLATE NOCASE);
        CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
        CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album);
        CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title);
        CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);
        CREATE INDEX IF NOT EXISTS idx_external_tracks_indexed_at ON external_tracks(indexed_at);
        CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(
            title,
            artist,
            album,
            genre,
            path UNINDEXED,
            content='tracks',
            content_rowid='rowid',
            tokenize='unicode61 remove_diacritics 2'
        );
        CREATE TRIGGER IF NOT EXISTS tracks_fts_ai AFTER INSERT ON tracks BEGIN
            INSERT INTO tracks_fts(rowid, title, artist, album, genre, path)
            VALUES (new.rowid, new.title, new.artist, new.album, new.genre, new.path);
        END;
        CREATE TRIGGER IF NOT EXISTS tracks_fts_ad AFTER DELETE ON tracks BEGIN
            INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, genre, path)
            VALUES('delete', old.rowid, old.title, old.artist, old.album, old.genre, old.path);
        END;
        CREATE TRIGGER IF NOT EXISTS tracks_fts_au AFTER UPDATE ON tracks BEGIN
            INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, genre, path)
            VALUES('delete', old.rowid, old.title, old.artist, old.album, old.genre, old.path);
            INSERT INTO tracks_fts(rowid, title, artist, album, genre, path)
            VALUES (new.rowid, new.title, new.artist, new.album, new.genre, new.path);
        END;
        ",
    )?;
    Ok(())
}

fn rebuild_fts_if_needed(conn: &Connection) {
    let track_count: i64 = conn
        .query_row("SELECT COUNT(*) FROM tracks", [], |row| row.get(0))
        .unwrap_or(0);
    let fts_count: i64 = conn
        .query_row("SELECT COUNT(*) FROM tracks_fts", [], |row| row.get(0))
        .unwrap_or(0);
    if track_count > 0 && fts_count == 0 {
        let _ = conn.execute("INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild')", []);
    }
}

pub(super) fn load_snapshot(conn: &Connection, snapshot: &mut super::LibrarySnapshot) {
    snapshot.roots = load_roots(conn);
    snapshot.tracks.clear();

    if let Ok(mut stmt) = conn.prepare(
        r"
        SELECT path, root_path, title, artist, album, cover_path, genre, year, track_no, duration_secs
        FROM tracks
        ORDER BY
            root_path COLLATE NOCASE,
            path COLLATE NOCASE
        ",
    ) {
        if let Ok(rows) = stmt.query_map([], |row| {
            Ok(LibraryTrack {
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
        }) {
            for row in rows.flatten() {
                snapshot.tracks.push(row);
            }
        }
    }
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
