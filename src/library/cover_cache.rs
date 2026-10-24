// SPDX-License-Identifier: GPL-3.0-or-later

//! Keeps the embedded-cover cache in step with the cover paths that track
//! rows store.

use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::Path;
use std::time::Instant;

use crossbeam_channel::Sender;
use rusqlite::{params, Connection};

use crate::metadata::EmbeddedCoverCache;

use super::scan::{count_label, find_cover_path_for_track, scan_snapshot_emit_interval};
use super::{emit_snapshot, LibraryEvent, LibraryScanProgress, LibrarySnapshot};

const MIGRATION_BATCH_SIZE: usize = 256;

/// The outcome of re-resolving the cover of a row that points at a legacy
/// cache entry.
#[derive(Debug)]
enum ResolvedCover {
    /// The cover a scan would store for the track now; empty when it has none.
    Cover(String),
    /// The track cannot be read now, such as one on an offline mount.
    Unavailable,
    /// The cover could not be resolved, for example because the new cache
    /// entry could not be written. Rows keep their legacy cover, so the next
    /// startup retries them and the legacy entry stays referenced.
    Failed,
}

#[derive(Debug, Clone, Copy)]
enum CoverTable {
    Tracks,
    ExternalTracks,
}

impl CoverTable {
    const ALL: [Self; 2] = [Self::Tracks, Self::ExternalTracks];

    fn select_cache_rows_sql(self) -> &'static str {
        match self {
            Self::Tracks => {
                "SELECT path, cover_path FROM tracks WHERE substr(cover_path, 1, length(?1)) = ?1"
            }
            Self::ExternalTracks => {
                "SELECT path, cover_path FROM external_tracks \
                 WHERE substr(cover_path, 1, length(?1)) = ?1"
            }
        }
    }

    fn update_cover_sql(self) -> &'static str {
        // The old value guards against overwriting a concurrent re-resolution.
        match self {
            Self::Tracks => {
                "UPDATE tracks SET cover_path = ?3, cover_checked = 1 \
                 WHERE path = ?1 AND cover_path = ?2"
            }
            Self::ExternalTracks => {
                "UPDATE external_tracks SET cover_path = ?3 WHERE path = ?1 AND cover_path = ?2"
            }
        }
    }

    /// For a track that cannot be read now, such as one on an offline mount.
    /// Library rows keep their cover so the next startup retries them; queue
    /// cache rows are dropped and read again when the track is next queued.
    fn unavailable_track_sql(self) -> Option<&'static str> {
        match self {
            Self::Tracks => None,
            Self::ExternalTracks => {
                Some("DELETE FROM external_tracks WHERE path = ?1 AND cover_path = ?2")
            }
        }
    }
}

/// Rows store paths as UTF-8, so entries of a cache directory whose path is
/// not valid UTF-8 cannot be matched to rows.
fn cache_dir_prefix(cache: &EmbeddedCoverCache) -> Option<String> {
    let dir = cache.dir().to_str()?;
    Some(format!("{dir}{}", std::path::MAIN_SEPARATOR))
}

/// `(path, cover_path)` for rows whose cover lies in the cache directory.
fn cache_rows(
    conn: &Connection,
    cache: &EmbeddedCoverCache,
    table: CoverTable,
) -> Result<Vec<(String, String)>, String> {
    let Some(prefix) = cache_dir_prefix(cache) else {
        return Ok(Vec::new());
    };
    let mut stmt = conn
        .prepare(table.select_cache_rows_sql())
        .map_err(|error| error.to_string())?;
    let rows = stmt
        .query_map(params![prefix], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })
        .map_err(|error| error.to_string())?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| error.to_string())?;
    Ok(rows)
}

/// Removes cache entries that no track row references. Runs after each scan.
pub(super) fn remove_unreferenced_embedded_covers(
    conn: &Connection,
    cache: Option<&EmbeddedCoverCache>,
) -> Result<(), String> {
    match cache {
        Some(cache) => remove_unreferenced_entries(conn, cache).map(|_| ()),
        None => Ok(()),
    }
}

fn remove_unreferenced_entries(
    conn: &Connection,
    cache: &EmbeddedCoverCache,
) -> Result<usize, String> {
    let Some(prefix) = cache_dir_prefix(cache) else {
        return Ok(0);
    };
    // One statement reads both tables from a single database snapshot.
    let referenced = conn
        .prepare(
            r"
            SELECT cover_path FROM tracks WHERE substr(cover_path, 1, length(?1)) = ?1
            UNION
            SELECT cover_path FROM external_tracks WHERE substr(cover_path, 1, length(?1)) = ?1
            ",
        )
        .and_then(|mut stmt| {
            stmt.query_map(params![prefix], |row| row.get::<_, String>(0))?
                .collect::<Result<Vec<_>, _>>()
        })
        .map_err(|error| format!("failed to read embedded cover references: {error}"))?
        .iter()
        .filter_map(|cover_path| cache.entry_name(Path::new(cover_path)))
        .map(str::to_owned)
        .collect::<HashSet<_>>();
    cache.remove_unreferenced(&referenced).map_err(|error| {
        format!(
            "failed to clean the embedded cover cache {}: {error}",
            cache.dir().display()
        )
    })
}

/// Re-resolves rows that still point at per-track PNGs from earlier releases,
/// then removes the PNGs that are no longer referenced. Runs on the library
/// worker at startup and finds nothing to do once every row is migrated.
/// Returns whether any row changed.
pub(super) fn migrate_legacy_embedded_covers(
    conn: &Connection,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<bool, String> {
    match EmbeddedCoverCache::shared() {
        Some(cache) => migrate_and_remove_legacy_entries(conn, cache, snapshot, event_tx),
        None => Ok(false),
    }
}

fn migrate_and_remove_legacy_entries(
    conn: &Connection,
    cache: &EmbeddedCoverCache,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<bool, String> {
    let mut last_emit = Instant::now();
    let mut failures = 0;
    let mut first_failure = None;
    let changed = migrate_legacy_entries(
        conn,
        cache,
        |path| {
            if fs::File::open(path).is_err() {
                return ResolvedCover::Unavailable;
            }
            match resolve_cover(cache, path) {
                Ok(cover_path) => ResolvedCover::Cover(cover_path),
                Err(error) => {
                    failures += 1;
                    first_failure.get_or_insert(error);
                    ResolvedCover::Failed
                }
            }
        },
        |processed, total| {
            if last_emit.elapsed() >= scan_snapshot_emit_interval() {
                snapshot.scan_in_progress = true;
                snapshot.scan_progress = Some(LibraryScanProgress {
                    roots_total: snapshot.roots.len(),
                    supported_files_discovered: total,
                    supported_files_processed: processed,
                    ..LibraryScanProgress::default()
                });
                emit_snapshot(event_tx, snapshot);
                last_emit = Instant::now();
            }
        },
    )?;
    if changed > 0 {
        remove_unreferenced_entries(conn, cache)?;
    }
    match first_failure {
        Some(first_failure) => Err(format!(
            "could not update the embedded covers of {}; kept their previous covers \
             and will retry at the next start ({first_failure})",
            count_label(failures, "track", "tracks")
        )),
        None => Ok(changed > 0),
    }
}

/// The cover a scan would store for `path`, with an embedded picture cached
/// in `cache`. Fails, instead of returning no cover, when the track's tags
/// cannot be read or its picture cannot be cached.
fn resolve_cover(cache: &EmbeddedCoverCache, path: &Path) -> Result<String, String> {
    let folder_cover = find_cover_path_for_track(path);
    if !folder_cover.is_empty() {
        return Ok(folder_cover);
    }
    Ok(cache
        .try_store_track_cover(path)?
        .map(|cover_path| cover_path.to_string_lossy().into_owned())
        .unwrap_or_default())
}

/// Returns how many rows changed.
fn migrate_legacy_entries(
    conn: &Connection,
    cache: &EmbeddedCoverCache,
    mut resolve_cover: impl FnMut(&Path) -> ResolvedCover,
    mut on_progress: impl FnMut(usize, usize),
) -> Result<usize, String> {
    let mut rows_by_path = BTreeMap::<String, Vec<(CoverTable, String)>>::new();
    for table in CoverTable::ALL {
        for (path, cover_path) in cache_rows(conn, cache, table)
            .map_err(|error| format!("failed to read legacy embedded covers: {error}"))?
        {
            if cache.is_legacy_entry(Path::new(&cover_path)) {
                rows_by_path
                    .entry(path)
                    .or_default()
                    .push((table, cover_path));
            }
        }
    }
    if rows_by_path.is_empty() {
        return Ok(0);
    }

    let rows_by_path = rows_by_path.into_iter().collect::<Vec<_>>();
    let total = rows_by_path.len();
    let mut changed = 0;
    for (batch_index, batch) in rows_by_path.chunks(MIGRATION_BATCH_SIZE).enumerate() {
        // Read covers before taking a write lock: tag edits and renames remain
        // available while the cache is migrated.
        let resolved = batch
            .iter()
            .map(|(path, _)| resolve_cover(Path::new(path)))
            .collect::<Vec<_>>();
        let tx = conn
            .unchecked_transaction()
            .map_err(|error| error.to_string())?;
        for ((path, rows), cover_path) in batch.iter().zip(&resolved) {
            for (table, legacy_cover_path) in rows {
                let result = match cover_path {
                    ResolvedCover::Cover(cover_path) => tx.execute(
                        table.update_cover_sql(),
                        params![path, legacy_cover_path, cover_path],
                    ),
                    ResolvedCover::Unavailable => match table.unavailable_track_sql() {
                        Some(sql) => tx.execute(sql, params![path, legacy_cover_path]),
                        None => Ok(0),
                    },
                    ResolvedCover::Failed => Ok(0),
                };
                changed += result.map_err(|error| {
                    format!("failed to migrate embedded cover for {path}: {error}")
                })?;
            }
        }
        tx.commit().map_err(|error| error.to_string())?;
        on_progress(((batch_index + 1) * MIGRATION_BATCH_SIZE).min(total), total);
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::scan::{read_track_info, run_scans};
    use crate::library::schema::init_schema;
    use crate::metadata::{encoded_picture, test_cache, write_wav_with_picture};
    use image::ImageFormat;
    use std::fs;
    use std::path::PathBuf;

    fn db() -> Connection {
        let conn = Connection::open_in_memory().expect("open in-memory db");
        init_schema(&conn).expect("init schema");
        conn
    }

    fn insert_track(conn: &Connection, path: &str, cover_path: &str) {
        conn.execute(
            r"
            INSERT INTO tracks(path, root_path, title, artist, album, cover_path, cover_checked,
                               mtime_ns, size_bytes, indexed_at)
            VALUES (?1, '/music', 'Title', 'Artist', 'Album', ?2, 1, 1, 1, 1)
            ",
            params![path, cover_path],
        )
        .expect("insert track");
    }

    fn insert_external_track(conn: &Connection, path: &str, cover_path: &str) {
        conn.execute(
            r"
            INSERT INTO external_tracks(path, title, artist, album, cover_path,
                                        mtime_ns, size_bytes, indexed_at)
            VALUES (?1, 'Title', 'Artist', 'Album', ?2, 1, 1, 1)
            ",
            params![path, cover_path],
        )
        .expect("insert external track");
    }

    fn cover_of(conn: &Connection, table: &str, path: &str) -> String {
        conn.query_row(
            &format!("SELECT cover_path FROM {table} WHERE path = ?1"),
            params![path],
            |row| row.get(0),
        )
        .expect("cover path")
    }

    fn write_entry(cache: &EmbeddedCoverCache, name: &str) -> PathBuf {
        fs::create_dir_all(cache.dir()).expect("create cache dir");
        let path = cache.dir().join(name);
        fs::write(&path, b"legacy png").expect("write cache entry");
        path
    }

    fn path_string(path: &Path) -> String {
        path.to_string_lossy().into_owned()
    }

    fn fixture_dir(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "ferrous-cover-library-{name}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create fixture dir");
        dir
    }

    #[test]
    fn tracks_sharing_an_embedded_picture_share_one_cache_entry() {
        let dir = fixture_dir("shared-picture");
        let picture = encoded_picture(11, ImageFormat::Jpeg);
        let first = dir.join("01.wav");
        let second = dir.join("02.wav");
        write_wav_with_picture(&first, &picture);
        write_wav_with_picture(&second, &picture);

        let first_cover = read_track_info(&first).cover_path;
        let second_cover = read_track_info(&second).cover_path;

        let cache = EmbeddedCoverCache::shared().expect("test cache directory");
        let expected = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&picture)));
        assert_eq!(first_cover, path_string(&expected));
        assert_eq!(second_cover, first_cover);
        assert_eq!(fs::read(&expected).expect("read cache entry"), picture);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn scans_remove_unreferenced_cache_entries() {
        let conn = db();
        // A private cache: concurrent tests clean the shared one after their
        // scans. Scan workers borrow it, so it lives until the process exits.
        let cache: &'static EmbeddedCoverCache = Box::leak(Box::new(test_cache("scan-cleanup")));
        let referenced = write_entry(cache, &format!("{:x}.png", md5::compute("scan-kept")));
        let orphan = write_entry(cache, &format!("{:x}.jpg", md5::compute("scan-orphan")));
        insert_external_track(&conn, "/elsewhere/a.flac", &path_string(&referenced));
        let root = fixture_dir("scan-cleanup");
        let picture = encoded_picture(13, ImageFormat::Png);
        write_wav_with_picture(&root.join("01.wav"), &picture);
        let scanned = cache
            .dir()
            .join(format!("{:x}.png", md5::compute(&picture)));
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();

        run_scans(
            &conn,
            &[root.clone()],
            Some(cache),
            &mut LibrarySnapshot::default(),
            &event_tx,
        )
        .expect("scan root");

        assert!(referenced.is_file());
        assert!(!orphan.exists());
        assert_eq!(fs::read(&scanned).expect("read scanned entry"), picture);
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn startup_migration_re_reads_embedded_pictures_and_removes_legacy_entries() {
        let conn = db();
        let cache = test_cache("startup-migration");
        let dir = fixture_dir("startup-migration");
        let picture = encoded_picture(12, ImageFormat::Jpeg);
        let track = dir.join("01.wav");
        write_wav_with_picture(&track, &picture);
        let legacy = write_entry(&cache, "0123456789abcdef.png");
        let offline_legacy = write_entry(&cache, "fedcba9876543210.png");
        let offline_track = dir.join("missing.wav");
        insert_track(&conn, &path_string(&track), &path_string(&legacy));
        insert_track(
            &conn,
            &path_string(&offline_track),
            &path_string(&offline_legacy),
        );
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();

        let migrated = migrate_and_remove_legacy_entries(
            &conn,
            &cache,
            &mut LibrarySnapshot::default(),
            &event_tx,
        );

        assert_eq!(migrated, Ok(true));
        let entry = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&picture)));
        assert_eq!(
            cover_of(&conn, "tracks", &path_string(&track)),
            path_string(&entry)
        );
        assert!(entry.is_file());
        assert!(!legacy.exists());
        assert_eq!(
            cover_of(&conn, "tracks", &path_string(&offline_track)),
            path_string(&offline_legacy)
        );
        assert!(offline_legacy.is_file());
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn cleanup_keeps_entries_that_library_or_external_rows_reference() {
        let conn = db();
        let cache = test_cache("library-cleanup");
        let library_entry = write_entry(&cache, &format!("{}.jpg", "1".repeat(32)));
        let external_entry = write_entry(&cache, &format!("{}.png", "2".repeat(32)));
        let orphan = write_entry(&cache, &format!("{}.jpg", "3".repeat(32)));
        let legacy_orphan = write_entry(&cache, "00000000000000aa.png");
        insert_track(&conn, "/music/a.flac", &path_string(&library_entry));
        insert_external_track(&conn, "/elsewhere/b.flac", &path_string(&external_entry));
        insert_track(&conn, "/music/c.flac", "/music/cover.jpg");

        assert_eq!(remove_unreferenced_entries(&conn, &cache), Ok(2));

        assert!(library_entry.is_file());
        assert!(external_entry.is_file());
        assert!(!orphan.exists());
        assert!(!legacy_orphan.exists());
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn migration_re_resolves_rows_that_point_at_legacy_entries() {
        let conn = db();
        let cache = test_cache("legacy-migration");
        let legacy_a = write_entry(&cache, "000000000000000a.png");
        let legacy_b = write_entry(&cache, "000000000000000b.png");
        let legacy_external = write_entry(&cache, "000000000000000c.png");
        let shared_cover = encoded_picture(10, ImageFormat::Jpeg);
        insert_track(&conn, "/music/album/01.flac", &path_string(&legacy_a));
        insert_track(&conn, "/music/album/02.flac", &path_string(&legacy_b));
        insert_track(&conn, "/music/sidecar/01.flac", &path_string(&legacy_b));
        insert_track(&conn, "/music/other/01.flac", "/music/other/cover.jpg");
        insert_external_track(&conn, "/elsewhere/01.flac", &path_string(&legacy_external));
        conn.execute(
            "UPDATE tracks SET cover_checked = 0 WHERE path = '/music/album/02.flac'",
            [],
        )
        .expect("mark track unchecked");

        let mut resolved = Vec::new();
        let migrated = migrate_legacy_entries(
            &conn,
            &cache,
            |path| {
                resolved.push(path.to_path_buf());
                if path.starts_with("/music/sidecar") {
                    ResolvedCover::Cover("/music/sidecar/cover.png".to_string())
                } else {
                    ResolvedCover::Cover(path_string(
                        &cache.store(&shared_cover).expect("store cover"),
                    ))
                }
            },
            |_, _| {},
        )
        .expect("migrate");
        let removed = remove_unreferenced_entries(&conn, &cache).expect("cleanup");

        assert_eq!(migrated, 4);
        assert_eq!(resolved.len(), 4, "each migrated track is read once");
        let shared_entry = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&shared_cover)));
        let shared_entry = path_string(&shared_entry);
        for path in ["/music/album/01.flac", "/music/album/02.flac"] {
            assert_eq!(cover_of(&conn, "tracks", path), shared_entry);
        }
        assert_eq!(
            cover_of(&conn, "tracks", "/music/sidecar/01.flac"),
            "/music/sidecar/cover.png"
        );
        assert_eq!(
            cover_of(&conn, "tracks", "/music/other/01.flac"),
            "/music/other/cover.jpg"
        );
        assert_eq!(
            cover_of(&conn, "external_tracks", "/elsewhere/01.flac"),
            shared_entry
        );
        let unchecked: i64 = conn
            .query_row(
                "SELECT COUNT(*) FROM tracks WHERE cover_checked = 0",
                [],
                |row| row.get(0),
            )
            .expect("count unchecked tracks");
        assert_eq!(unchecked, 0);
        assert_eq!(removed, 3);
        for legacy in [legacy_a, legacy_b, legacy_external] {
            assert!(!legacy.exists());
        }
        assert!(Path::new(&shared_entry).is_file());

        let migrated_again =
            migrate_legacy_entries(&conn, &cache, |_| unreachable!(), |_, _| {}).expect("rerun");
        assert_eq!(migrated_again, 0);
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn migration_keeps_unreadable_library_tracks_and_drops_their_queue_cache_rows() {
        let conn = db();
        let cache = test_cache("legacy-migration-offline");
        let library_legacy = write_entry(&cache, "000000000000000d.png");
        let external_legacy = write_entry(&cache, "000000000000000e.png");
        insert_track(&conn, "/offline/01.flac", &path_string(&library_legacy));
        insert_external_track(&conn, "/offline/02.flac", &path_string(&external_legacy));

        let migrated =
            migrate_legacy_entries(&conn, &cache, |_| ResolvedCover::Unavailable, |_, _| {})
                .expect("migrate");
        let removed = remove_unreferenced_entries(&conn, &cache).expect("cleanup");

        assert_eq!(migrated, 1, "the library row is left unchanged");
        assert_eq!(
            cover_of(&conn, "tracks", "/offline/01.flac"),
            path_string(&library_legacy),
            "an unreadable library track keeps its cover until a later startup"
        );
        let external_rows: i64 = conn
            .query_row("SELECT COUNT(*) FROM external_tracks", [], |row| row.get(0))
            .expect("count external rows");
        assert_eq!(external_rows, 0);
        assert_eq!(removed, 1);
        assert!(library_legacy.is_file());
        assert!(!external_legacy.exists());
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn migration_keeps_legacy_covers_whose_new_entry_cannot_be_written() {
        let conn = db();
        let cache = test_cache("legacy-migration-unwritable");
        let dir = fixture_dir("legacy-migration-unwritable");
        let picture = encoded_picture(13, ImageFormat::Jpeg);
        let track = dir.join("01.wav");
        write_wav_with_picture(&track, &picture);
        let legacy = write_entry(&cache, "00000000000000ee.png");
        insert_track(&conn, &path_string(&track), &path_string(&legacy));
        insert_external_track(&conn, &path_string(&track), &path_string(&legacy));
        // A directory where the new entry belongs makes writing it fail.
        let entry = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&picture)));
        fs::create_dir(&entry).expect("block the new entry");
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();

        let error = migrate_and_remove_legacy_entries(
            &conn,
            &cache,
            &mut LibrarySnapshot::default(),
            &event_tx,
        )
        .expect_err("the failed migration is reported");

        assert!(
            error.contains("1 track") && error.contains("embedded cover cache"),
            "unexpected error: {error}"
        );
        for table in ["tracks", "external_tracks"] {
            assert_eq!(
                cover_of(&conn, table, &path_string(&track)),
                path_string(&legacy),
                "{table} keeps the legacy cover"
            );
        }
        assert!(legacy.is_file());

        // The next start retries the row once the entry can be written.
        fs::remove_dir(&entry).expect("unblock the new entry");
        let migrated = migrate_and_remove_legacy_entries(
            &conn,
            &cache,
            &mut LibrarySnapshot::default(),
            &event_tx,
        );

        assert_eq!(migrated, Ok(true));
        for table in ["tracks", "external_tracks"] {
            assert_eq!(
                cover_of(&conn, table, &path_string(&track)),
                path_string(&entry)
            );
        }
        assert!(entry.is_file());
        assert!(!legacy.exists());
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn migration_keeps_a_cover_that_changed_while_it_was_resolved() {
        let conn = db();
        let cache = test_cache("legacy-migration-race");
        let legacy = write_entry(&cache, "00000000000000dd.png");
        insert_track(&conn, "/music/a.flac", &path_string(&legacy));

        migrate_legacy_entries(
            &conn,
            &cache,
            |path| {
                // A tag edit refreshes the row while the migration reads it.
                conn.execute(
                    "UPDATE tracks SET cover_path = '/music/edited.png' WHERE path = ?1",
                    params![path_string(path)],
                )
                .expect("concurrent refresh");
                ResolvedCover::Cover("/music/stale.png".to_string())
            },
            |_, _| {},
        )
        .expect("migrate");

        assert_eq!(
            cover_of(&conn, "tracks", "/music/a.flac"),
            "/music/edited.png"
        );
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }
}
