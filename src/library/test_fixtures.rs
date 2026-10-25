// SPDX-License-Identifier: GPL-3.0-or-later

//! Library databases as earlier versions of Ferrous left them, for the
//! migration tests.

use std::path::{Path, PathBuf};

use rusqlite::types::Value;
use rusqlite::{params, Connection};

/// The schema code of Ferrous before schema versions (commit c106602),
/// unchanged, which every existing library database went through.
mod version_0 {
    use rusqlite::Connection;

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
        let _ = conn.execute("ALTER TABLE tracks ADD COLUMN search_tags TEXT", []);
        let _ = conn.execute(
            "ALTER TABLE external_tracks ADD COLUMN search_tags TEXT",
            [],
        );
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
}

/// The waveform cache table that the analysis worker keeps in the same file.
const WAVEFORM_CACHE_TABLE: &str = r"
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

/// The tables a library database holds.
pub(super) const TABLES: [&str; 4] = ["roots", "tracks", "external_tracks", "waveform_cache"];

/// Builds a version 0 library database at `path` with the schema code of
/// Ferrous before schema versions, and fills it the way years of use would:
/// named and unnamed roots, `tracks` rows with and without optional fields,
/// covers from folders and from the embedded-cover cache, search tags, and
/// rows from before tracks had a root; external tracks; and waveforms.
/// Returns the roots, with the number of tracks under each.
pub(super) fn build_version_0_library(
    path: &Path,
    tracks_per_root: usize,
) -> Vec<(PathBuf, usize)> {
    let conn = Connection::open(path).expect("open version 0 database");
    version_0::init_schema(&conn).expect("version 0 schema");
    conn.execute_batch(WAVEFORM_CACHE_TABLE)
        .expect("waveform cache table");
    let roots = [
        (PathBuf::from("/music/Library"), "Main library"),
        (PathBuf::from("/mnt/nas/Flac"), ""),
        (
            PathBuf::from("/home/user/Music – Ålbüms"),
            "  Spaced name  ",
        ),
    ];
    let tx = conn.unchecked_transaction().expect("fixture transaction");
    for (index, (root, name)) in roots.iter().enumerate() {
        tx.execute(
            "INSERT INTO roots(path, name, added_at) VALUES (?1, ?2, ?3)",
            params![
                root.to_string_lossy(),
                name,
                1_700_000_000 + i64::try_from(index).unwrap_or(0)
            ],
        )
        .expect("root row");
        for track in 0..tracks_per_root {
            insert_track(&tx, root, index, track);
        }
    }
    for track in 0..tracks_per_root / 4 {
        tx.execute(
            "INSERT INTO external_tracks(path, title, artist, album, cover_path, genre, year, \
                 track_no, duration_secs, mtime_ns, size_bytes, indexed_at, search_tags) \
             VALUES (?1, ?2, 'Outside Artist', 'Outside Album', ?3, 'Jazz', ?4, ?5, ?6, ?7, ?8, \
                 1700000500, ?9)",
            params![
                format!("/downloads/outside {track}.flac"),
                format!("Outside {track}"),
                if track.is_multiple_of(2) {
                    format!("/downloads/cover {track}.jpg")
                } else {
                    String::new()
                },
                (!track.is_multiple_of(3)).then_some(1990 + i64::try_from(track % 30).unwrap_or(0)),
                (!track.is_multiple_of(5)).then_some(i64::try_from(track % 12 + 1).unwrap_or(1)),
                (!track.is_multiple_of(7))
                    .then_some(180.5_f64 + f64::from(u32::try_from(track % 90).unwrap_or(0))),
                1_600_000_000_000_000_000_i64 + i64::try_from(track).unwrap_or(0),
                4_000_000 + i64::try_from(track).unwrap_or(0),
                (!track.is_multiple_of(4)).then_some(r#"{"comment":["outside"]}"#),
            ],
        )
        .expect("external track row");
    }
    for track in 0..tracks_per_root / 8 {
        let peaks: Vec<u8> = (0..512_u32)
            .map(|peak| {
                u8::try_from((peak * 7 + u32::try_from(track).unwrap_or(0)) % 256).unwrap_or(0)
            })
            .collect();
        tx.execute(
            "INSERT INTO waveform_cache(path, size_bytes, modified_secs, modified_nanos, \
                 format_version, peak_count, peaks_blob, updated_at) \
             VALUES (?1, 1234, 1700000000, 5, 2, 128, ?2, 1700000600)",
            params![format!("/music/Library/Artist {track}/01.flac"), peaks],
        )
        .expect("waveform row");
    }
    tx.commit().expect("commit fixture");
    roots
        .iter()
        .map(|(root, _)| (root.clone(), tracks_per_root))
        .collect()
}

fn insert_track(conn: &Connection, root: &Path, root_index: usize, track: usize) {
    let artist = format!("Artist {} ü", track % 37);
    let album = format!("Album {}", track % 113);
    let path = root
        .join(&artist)
        .join(&album)
        .join(format!("{:02} - Track {track} ★.flac", track % 20 + 1));
    let cover_path = match track % 4 {
        0 => root
            .join(&artist)
            .join(&album)
            .join("cover.jpg")
            .to_string_lossy()
            .into_owned(),
        1 => format!("/home/user/.cache/ferrous/embedded-covers/{track:032x}.png"),
        2 => String::new(),
        _ => root
            .join(&artist)
            .join(&album)
            .join("Folder.PNG")
            .to_string_lossy()
            .into_owned(),
    };
    // Tracks from before rows had a root have an empty one.
    let root_path = if track.is_multiple_of(50) {
        String::new()
    } else {
        root.to_string_lossy().into_owned()
    };
    let search_tags = match track % 3 {
        0 => Some(format!(
            r#"{{"composer":["Composer {track}"],"comment":["note"]}}"#
        )),
        1 => Some("{}".to_string()),
        _ => None,
    };
    conn.execute(
        "INSERT INTO tracks(path, root_path, title, artist, album, cover_path, cover_checked, \
             genre, year, track_no, duration_secs, mtime_ns, size_bytes, indexed_at, search_tags) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)",
        params![
            path.to_string_lossy(),
            root_path,
            format!("Track {track} – “quoted”"),
            artist,
            album,
            cover_path,
            i64::from(track % 4 != 2),
            ["Rock", "", "Électronique", "Hip-Hop"][track % 4],
            (!track.is_multiple_of(6)).then_some(1960 + i64::try_from(track % 60).unwrap_or(0)),
            (!track.is_multiple_of(9)).then_some(i64::try_from(track % 20 + 1).unwrap_or(1)),
            (!track.is_multiple_of(11))
                .then_some(60.25_f64 + f64::from(u32::try_from(track % 400).unwrap_or(0))),
            1_650_000_000_000_000_000_i64 + i64::try_from(track * 1_000 + root_index).unwrap_or(0),
            3_000_000 + i64::try_from(track).unwrap_or(0),
            1_700_000_100 + i64::try_from(track).unwrap_or(0),
            search_tags,
        ],
    )
    .expect("track row");
}

/// Every row of `table`, with its rowid, in rowid order.
pub(super) fn table_rows(conn: &Connection, table: &str) -> Vec<Vec<Value>> {
    let mut stmt = conn
        .prepare(&format!("SELECT rowid, * FROM {table} ORDER BY rowid"))
        .expect("prepare table dump");
    let columns = stmt.column_count();
    stmt.query_map([], |row| {
        (0..columns)
            .map(|column| row.get::<_, Value>(column))
            .collect::<rusqlite::Result<Vec<_>>>()
    })
    .expect("dump table")
    .collect::<rusqlite::Result<Vec<_>>>()
    .expect("table rows")
}

/// The rows of every table of a library database.
pub(super) fn library_rows(conn: &Connection) -> Vec<Vec<Vec<Value>>> {
    TABLES.iter().map(|table| table_rows(conn, table)).collect()
}

/// `(type, name, sql)` of every schema object, by name.
pub(super) fn schema_objects(conn: &Connection) -> Vec<(String, String, Option<String>)> {
    let mut stmt = conn
        .prepare("SELECT type, name, sql FROM sqlite_master ORDER BY name")
        .expect("prepare schema dump");
    stmt.query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .expect("dump schema")
        .collect::<rusqlite::Result<Vec<_>>>()
        .expect("schema objects")
}
