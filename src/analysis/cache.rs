// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::{params, Connection};

use crate::file_identity::FileId;
use crate::library::{file_id_columns, file_id_from_columns, identity_contradicts};
use crate::playback::{remap_path, PathRenames};

pub(super) const MAX_WAVEFORM_CACHE_TRACKS: usize = 256;
pub(super) const PERSISTENT_WAVEFORM_CACHE_MAX_ROWS: usize = 4096;
pub(super) const PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL: usize = 24;
// Version 2 preserves every sample peak; version 1 could omit transients.
// Version 3 cuts an MP3's encoder delay and padding. A build before it
// writes version 2 into a version 3 table without updating `complete`, so
// its waveforms, untrimmed, are never taken for complete ones.
const WAVEFORM_CACHE_FORMAT_VERSION: i64 = 3;

/// What tells whether a file changed since its waveform was decoded. In
/// memory, every field must match; the saved waveforms compare identities
/// only to break a tie, see [`load_waveform_from_db`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct WaveformSourceStamp {
    pub(super) size_bytes: u64,
    pub(super) modified_secs: u64,
    pub(super) modified_nanos: u32,
    /// What tells the file from another of the same size and time, such as
    /// the other file of a swap.
    pub(super) file_id: Option<FileId>,
}

#[derive(Debug, Clone)]
pub(super) struct WaveformCacheEntry {
    pub(super) stamp: Option<WaveformSourceStamp>,
    pub(super) peaks: Vec<f32>,
}

pub(super) fn usize_to_u64(value: usize) -> u64 {
    u64::try_from(value).unwrap_or(u64::MAX)
}

fn u64_to_i64(value: u64) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn usize_to_i64(value: usize) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

/// Opens the waveform cache, a table of the library database, whose schema
/// owns it; see `crate::library::open_library_database`.
pub(super) fn open_waveform_cache_db() -> anyhow::Result<Connection> {
    let conn = crate::library::open_library_database(Duration::from_millis(250))?;
    remove_empty_waveforms(&conn);
    Ok(conn)
}

/// Prepares a test database as [`open_waveform_cache_db`] prepares the
/// library database.
#[cfg(test)]
pub(super) fn init_waveform_cache_schema(conn: &Connection) -> anyhow::Result<()> {
    crate::library::init_library_schema(conn)?;
    remove_empty_waveforms(conn);
    Ok(())
}

/// Legacy/failed decodes may have written empty entries; treat them as invalid cache rows.
fn remove_empty_waveforms(conn: &Connection) {
    let _ = conn.execute("DELETE FROM waveform_cache WHERE peak_count <= 0", []);
}

/// The saved waveform of `path`, if its file still has `stamp` and the
/// decode that saved it reached the end of the file. Loading it counts as a
/// use, which keeps it from being pruned.
///
/// The file's identity breaks a tie (see [`identity_contradicts`]): when
/// another file has the same size and time, a waveform saved for a file of
/// another identity is not this file's, as after a swap, or after a decode
/// of the other file was saved under this path once the rename had moved
/// the saved waveforms (`crate::library::rename_indexed_metadata_paths`).
/// Otherwise the waveform is this file's, and takes its identity, as does a
/// waveform saved before identities were.
pub(super) fn load_waveform_from_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
) -> Option<Vec<f32>> {
    let path = path.to_string_lossy().to_string();
    let row: (i64, i64, Vec<u8>, Option<FileId>) = conn
        .query_row(
            r"
            SELECT format_version, peak_count, peaks_blob, file_device, file_inode
            FROM waveform_cache
            WHERE path=?1
              AND size_bytes=?2
              AND modified_secs=?3
              AND modified_nanos=?4
              AND complete=1
            ",
            params![
                path,
                u64_to_i64(stamp.size_bytes),
                u64_to_i64(stamp.modified_secs),
                i64::from(stamp.modified_nanos),
            ],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, i64>(1)?,
                    row.get::<_, Vec<u8>>(2)?,
                    file_id_from_columns(row.get(3)?, row.get(4)?),
                ))
            },
        )
        .ok()?;
    if row.0 != WAVEFORM_CACHE_FORMAT_VERSION {
        return None;
    }
    if identity_contradicts(row.3, stamp.file_id, || {
        shares_size_and_time(conn, &path, stamp)
    }) {
        return None;
    }
    let peak_count = usize::try_from(row.1).ok()?;
    if peak_count == 0 {
        return None;
    }
    let peaks = decode_peaks_blob(&row.2, peak_count)?;
    if stamp.file_id.is_some() && row.3 != stamp.file_id {
        adopt_file_id(conn, &path, stamp.file_id);
    }
    touch_waveform_in_db(conn, Path::new(&path));
    Some(peaks)
}

/// Whether a file other than that of `path`, with a saved waveform or in
/// the library, has the size and time of `stamp`, so that only identities
/// tell the files apart. A failed query counts as one, which only costs a
/// decode.
fn shares_size_and_time(conn: &Connection, path: &str, stamp: WaveformSourceStamp) -> bool {
    // The library's modification time, in nanoseconds since the epoch.
    let mtime_ns =
        u128::from(stamp.modified_secs) * 1_000_000_000 + u128::from(stamp.modified_nanos);
    conn.query_row(
        r"
        SELECT EXISTS(
            SELECT 1 FROM waveform_cache
            WHERE size_bytes = ?2 AND modified_secs = ?3 AND modified_nanos = ?4
              AND path <> ?1
        ) OR EXISTS(
            SELECT 1 FROM tracks WHERE size_bytes = ?2 AND mtime_ns = ?5 AND path <> ?1
        )
        ",
        params![
            path,
            u64_to_i64(stamp.size_bytes),
            u64_to_i64(stamp.modified_secs),
            i64::from(stamp.modified_nanos),
            i64::try_from(mtime_ns).unwrap_or(i64::MAX),
        ],
        |row| row.get(0),
    )
    .unwrap_or(true)
}

/// Give the saved waveform of `path` the identity of the file it matched.
/// A failed write leaves the old one, which only costs a decode if another
/// file gets the same size and time.
fn adopt_file_id(conn: &Connection, path: &str, file_id: Option<FileId>) {
    let (device, inode) = file_id_columns(file_id);
    let _ = conn.execute(
        "UPDATE waveform_cache SET file_device = ?2, file_inode = ?3 WHERE path = ?1",
        params![path, device, inode],
    );
}

/// Record a use of the saved waveform of `path`; pruning keeps the waveforms
/// used last. A failed write only makes the waveform likelier to be pruned.
pub(super) fn touch_waveform_in_db(conn: &Connection, path: &Path) {
    let _ = conn.execute(
        "UPDATE waveform_cache SET updated_at = ?2 WHERE path = ?1 AND updated_at < ?2",
        params![path.to_string_lossy().to_string(), unix_ts_i64()],
    );
}

/// Save a waveform whose decode reached the end of the file, covering
/// `coverage_seconds` of audio.
pub(super) fn persist_waveform_to_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
    peaks: &[f32],
    coverage_seconds: f32,
) -> rusqlite::Result<()> {
    if peaks.is_empty() {
        delete_waveform_from_db(conn, path);
        return Ok(());
    }
    let blob = encode_peaks_blob(peaks);
    let now = unix_ts_i64();
    let (device, inode) = file_id_columns(stamp.file_id);
    conn.execute(
        r"
        INSERT INTO waveform_cache(
            path, size_bytes, modified_secs, modified_nanos,
            format_version, peak_count, peaks_blob, updated_at,
            complete, coverage_seconds, file_device, file_inode
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 1, ?9, ?10, ?11)
        ON CONFLICT(path) DO UPDATE SET
            size_bytes=excluded.size_bytes,
            modified_secs=excluded.modified_secs,
            modified_nanos=excluded.modified_nanos,
            format_version=excluded.format_version,
            peak_count=excluded.peak_count,
            peaks_blob=excluded.peaks_blob,
            updated_at=excluded.updated_at,
            complete=excluded.complete,
            coverage_seconds=excluded.coverage_seconds,
            file_device=excluded.file_device,
            file_inode=excluded.file_inode
        ",
        params![
            path.to_string_lossy().to_string(),
            u64_to_i64(stamp.size_bytes),
            u64_to_i64(stamp.modified_secs),
            i64::from(stamp.modified_nanos),
            WAVEFORM_CACHE_FORMAT_VERSION,
            usize_to_i64(peaks.len()),
            blob,
            now,
            f64::from(coverage_seconds),
            device,
            inode,
        ],
    )?;
    Ok(())
}

/// Forget the saved waveform of `path`. A failed write leaves it to the
/// stamp check.
fn delete_waveform_from_db(conn: &Connection, path: &Path) {
    let _ = conn.execute(
        "DELETE FROM waveform_cache WHERE path = ?1",
        params![path.to_string_lossy().to_string()],
    );
}

/// Keep the `max_rows` waveforms used last.
pub(super) fn prune_persistent_waveform_cache(
    conn: &Connection,
    max_rows: usize,
) -> rusqlite::Result<()> {
    conn.execute(
        r"
        DELETE FROM waveform_cache
        WHERE path IN (
            SELECT path
            FROM waveform_cache
            ORDER BY updated_at DESC
            LIMIT -1 OFFSET ?1
        )
        ",
        params![usize_to_i64(max_rows)],
    )?;
    Ok(())
}

fn encode_peaks_blob(peaks: &[f32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(peaks.len() * 4);
    for &v in peaks {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

fn decode_peaks_blob(blob: &[u8], peak_count: usize) -> Option<Vec<f32>> {
    if blob.len() != peak_count.checked_mul(4)? {
        return None;
    }
    let mut out = Vec::with_capacity(peak_count);
    for chunk in blob.chunks_exact(4) {
        out.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Some(out)
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| u64_to_i64(duration.as_secs()))
        .unwrap_or(0)
}

pub(super) fn touch_waveform_cache_lru(lru: &mut VecDeque<PathBuf>, path: &Path) {
    if let Some(pos) = lru.iter().position(|p| p == path) {
        lru.remove(pos);
    }
    lru.push_back(path.to_path_buf());
}

pub(super) fn insert_waveform_cache_entry(
    cache: &mut HashMap<PathBuf, WaveformCacheEntry>,
    lru: &mut VecDeque<PathBuf>,
    path: &Path,
    entry: WaveformCacheEntry,
) {
    if entry.peaks.is_empty() {
        cache.remove(path);
        if let Some(pos) = lru.iter().position(|p| p == path) {
            lru.remove(pos);
        }
        return;
    }
    let owned_path = path.to_path_buf();
    cache.insert(owned_path.clone(), entry);
    touch_waveform_cache_lru(lru, &owned_path);

    while cache.len() > MAX_WAVEFORM_CACHE_TRACKS {
        let Some(evicted) = lru.pop_front() else {
            break;
        };
        cache.remove(&evicted);
    }
}

/// Keep each cached waveform with its file when files are renamed. All
/// entries move at once, so in a swap neither file takes the other's, and
/// an entry at a path that another file was renamed to is dropped.
pub(super) fn remap_waveform_cache_entries(
    cache: &mut HashMap<PathBuf, WaveformCacheEntry>,
    lru: &mut VecDeque<PathBuf>,
    renames: &PathRenames,
) {
    let moved: Vec<_> = renames
        .iter()
        .filter_map(|(old, new)| Some((new.clone(), cache.remove(old)?)))
        .collect();
    for new in renames.values() {
        cache.remove(new);
    }
    cache.extend(moved);
    // Keep the latest use of each path that still has an entry.
    let mut kept = HashSet::new();
    let mut order: Vec<_> = lru
        .drain(..)
        .rev()
        .map(|mut path| {
            remap_path(&mut path, renames);
            path
        })
        .filter(|path| cache.contains_key(path) && kept.insert(path.clone()))
        .collect();
    order.reverse();
    lru.extend(order);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn waveform_cache_rejects_sample_skipping_format() {
        let conn = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&conn).expect("schema");
        let stamp = WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: None,
        };
        let path = Path::new("fixture.wav");
        persist_waveform_to_db(&conn, path, stamp, &[0.5], 1.0).expect("cache fixture");
        conn.execute("UPDATE waveform_cache SET format_version = 1", [])
            .expect("legacy version");
        assert!(load_waveform_from_db(&conn, path, stamp).is_none());
    }

    #[test]
    fn a_waveform_an_older_build_wrote_is_decoded_again() {
        let conn = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&conn).expect("schema");
        let stamp = WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: None,
        };
        let path = Path::new("/music/a.mp3");
        persist_waveform_to_db(&conn, path, stamp, &[0.5], 1.0).expect("save");
        assert!(load_waveform_from_db(&conn, path, stamp).is_some());
        // An older build's upsert, which knows neither `complete` nor
        // `coverage_seconds`, over the complete row: version 2 peaks of the
        // untrimmed audio.
        conn.execute(
            "INSERT INTO waveform_cache(path, size_bytes, modified_secs, modified_nanos, \
             format_version, peak_count, peaks_blob, updated_at) \
             VALUES (?1, 10, 1, 0, 2, 1, x'0000803e', 100) \
             ON CONFLICT(path) DO UPDATE SET format_version = excluded.format_version, \
             peak_count = excluded.peak_count, peaks_blob = excluded.peaks_blob, \
             updated_at = excluded.updated_at",
            params![path.to_string_lossy().to_string()],
        )
        .expect("older build's write");

        assert!(load_waveform_from_db(&conn, path, stamp).is_none());
    }

    #[test]
    fn cached_waveforms_follow_their_files_through_renames() {
        let (a, b, c, d) = (
            PathBuf::from("/music/a.flac"),
            PathBuf::from("/music/b.flac"),
            PathBuf::from("/music/c.flac"),
            PathBuf::from("/music/d.flac"),
        );
        // The same stamp, so only the path tells the entries apart.
        let entry = |peak| WaveformCacheEntry {
            stamp: Some(WaveformSourceStamp {
                size_bytes: 10,
                modified_secs: 1,
                modified_nanos: 0,
                file_id: None,
            }),
            peaks: vec![peak],
        };
        let mut cache = HashMap::new();
        let mut lru = VecDeque::new();
        for (path, peak) in [(&a, 0.1), (&b, 0.2), (&c, 0.3), (&d, 0.4)] {
            insert_waveform_cache_entry(&mut cache, &mut lru, path, entry(peak));
        }

        // A and B swap; D is renamed onto C, whose file it replaces.
        let renames = PathRenames::from([
            (a.clone(), b.clone()),
            (b.clone(), a.clone()),
            (d.clone(), c.clone()),
        ]);
        remap_waveform_cache_entries(&mut cache, &mut lru, &renames);

        let peaks = |path: &PathBuf| cache.get(path).map(|entry| entry.peaks.clone());
        assert_eq!(peaks(&a), Some(vec![0.2]));
        assert_eq!(peaks(&b), Some(vec![0.1]));
        assert_eq!(peaks(&c), Some(vec![0.4]));
        assert_eq!(peaks(&d), None);
        assert_eq!(lru, VecDeque::from([b, a, c]));
    }

    #[test]
    fn peaks_blob_roundtrip() {
        let peaks = vec![0.0f32, 0.25, 0.5, 1.0];
        let blob = encode_peaks_blob(&peaks);
        let decoded_peaks = decode_peaks_blob(&blob, peaks.len()).expect("decode");
        assert_eq!(decoded_peaks, peaks);
    }

    #[test]
    fn waveform_cache_persist_and_load_roundtrip() {
        let conn = Connection::open_in_memory().expect("in-memory db");
        init_waveform_cache_schema(&conn).expect("schema");
        let path = PathBuf::from("/tmp/test_track.flac");
        let stamp = WaveformSourceStamp {
            size_bytes: 1234,
            modified_secs: 100,
            modified_nanos: 55,
            file_id: None,
        };
        let peaks = vec![0.1f32, 0.3, 0.9];
        persist_waveform_to_db(&conn, &path, stamp, &peaks, 3.0).expect("persist");
        let loaded = load_waveform_from_db(&conn, &path, stamp).expect("load");
        assert_eq!(loaded, peaks);
    }

    fn updated_at(conn: &Connection, path: &Path) -> i64 {
        conn.query_row(
            "SELECT updated_at FROM waveform_cache WHERE path = ?1",
            params![path.to_string_lossy().to_string()],
            |row| row.get(0),
        )
        .expect("cached row")
    }

    #[test]
    fn a_load_counts_as_a_use_so_pruning_keeps_waveforms_played_often() {
        let conn = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&conn).expect("schema");
        let stamp = WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: None,
        };
        let (played, newer) = (
            Path::new("/music/played.flac"),
            Path::new("/music/newer.flac"),
        );
        persist_waveform_to_db(&conn, played, stamp, &[0.5], 1.0).expect("played");
        persist_waveform_to_db(&conn, newer, stamp, &[0.5], 1.0).expect("newer");
        // The played track was saved long ago, the other one later.
        conn.execute(
            "UPDATE waveform_cache SET updated_at = 100 WHERE path = ?1",
            params![played.to_string_lossy().to_string()],
        )
        .expect("old save");
        conn.execute(
            "UPDATE waveform_cache SET updated_at = 200 WHERE path = ?1",
            params![newer.to_string_lossy().to_string()],
        )
        .expect("newer save");

        assert!(load_waveform_from_db(&conn, played, stamp).is_some());

        assert!(updated_at(&conn, played) >= unix_ts_i64() - 60);
        prune_persistent_waveform_cache(&conn, 1).expect("prune");
        assert!(load_waveform_from_db(&conn, played, stamp).is_some());
        assert!(load_waveform_from_db(&conn, newer, stamp).is_none());
    }

    #[test]
    fn a_waveform_not_known_to_reach_the_end_of_its_file_is_decoded_again() {
        let conn = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&conn).expect("schema");
        let stamp = WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: None,
        };
        let path = Path::new("/music/legacy.flac");
        persist_waveform_to_db(&conn, path, stamp, &[0.5], 1.0).expect("save");
        // A row from before schema version 3 tells neither.
        conn.execute(
            "UPDATE waveform_cache SET complete = 0, coverage_seconds = NULL, updated_at = 100",
            [],
        )
        .expect("legacy row");

        assert!(load_waveform_from_db(&conn, path, stamp).is_none());
        assert_eq!(updated_at(&conn, path), 100, "not a use");

        persist_waveform_to_db(&conn, path, stamp, &[0.25], 2.5).expect("decoded again");
        let (complete, coverage): (i64, f64) = conn
            .query_row(
                "SELECT complete, coverage_seconds FROM waveform_cache",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .expect("row");
        assert_eq!((complete, coverage), (1, 2.5));
        assert_eq!(load_waveform_from_db(&conn, path, stamp), Some(vec![0.25]));
    }

    fn stamp_of(inode: Option<u64>) -> WaveformSourceStamp {
        WaveformSourceStamp {
            size_bytes: 10,
            modified_secs: 1,
            modified_nanos: 0,
            file_id: inode.map(|inode| FileId::from_numbers(1, inode)),
        }
    }

    fn stored_file_id(conn: &Connection, path: &Path) -> Option<FileId> {
        conn.query_row(
            "SELECT file_device, file_inode FROM waveform_cache WHERE path = ?1",
            params![path.to_string_lossy().to_string()],
            |row| Ok(file_id_from_columns(row.get(0)?, row.get(1)?)),
        )
        .expect("row")
    }

    #[test]
    fn a_saved_waveform_takes_the_identity_of_the_file_it_alone_matches() {
        let path = Path::new("/music/a.flac");
        // Saved before identities were, or with one the file system did not
        // keep, as after it was mounted again.
        for saved in [None, Some(7)] {
            let conn = Connection::open_in_memory().expect("database");
            init_waveform_cache_schema(&conn).expect("schema");
            persist_waveform_to_db(&conn, path, stamp_of(saved), &[0.5], 1.0).expect("save");

            assert_eq!(
                load_waveform_from_db(&conn, path, stamp_of(Some(8))),
                Some(vec![0.5]),
                "{saved:?}"
            );
            assert_eq!(stored_file_id(&conn, path), stamp_of(Some(8)).file_id);
        }
    }

    #[test]
    fn a_waveform_of_another_file_is_not_taken_when_its_size_and_time_tie() {
        let conn = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&conn).expect("schema");
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        persist_waveform_to_db(&conn, a, stamp_of(Some(7)), &[0.5], 1.0).expect("a");
        persist_waveform_to_db(&conn, b, stamp_of(Some(8)), &[0.25], 1.0).expect("b");

        // The file at `a` is the one saved at `b`, as after a swap.
        assert_eq!(load_waveform_from_db(&conn, a, stamp_of(Some(8))), None);
        assert_eq!(stored_file_id(&conn, a), stamp_of(Some(7)).file_id);
        assert_eq!(
            load_waveform_from_db(&conn, a, stamp_of(Some(7))),
            Some(vec![0.5])
        );
        // A file of unknown identity is matched by size and time alone.
        assert_eq!(
            load_waveform_from_db(&conn, a, stamp_of(None)),
            Some(vec![0.5])
        );
        assert_eq!(stored_file_id(&conn, a), stamp_of(Some(7)).file_id);
    }
}
