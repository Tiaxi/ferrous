// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::{params, Connection};

pub(super) const MAX_WAVEFORM_CACHE_TRACKS: usize = 256;
pub(super) const PERSISTENT_WAVEFORM_CACHE_MAX_ROWS: usize = 4096;
pub(super) const PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL: usize = 24;
const WAVEFORM_CACHE_FORMAT_VERSION: i64 = 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct WaveformSourceStamp {
    pub(super) size_bytes: u64,
    pub(super) modified_secs: u64,
    pub(super) modified_nanos: u32,
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

pub(super) fn open_waveform_cache_db() -> anyhow::Result<Connection> {
    let db_path = waveform_db_path()?;
    if let Some(parent) = db_path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let conn = Connection::open(db_path)?;
    let _ = conn.busy_timeout(Duration::from_millis(250));
    init_waveform_cache_schema(&conn)?;
    Ok(conn)
}

fn waveform_db_path() -> anyhow::Result<PathBuf> {
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

fn init_waveform_cache_schema(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(
        r"
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
        ",
    )?;
    // Legacy/failed decodes may have written empty entries; treat them as invalid cache rows.
    let _ = conn.execute("DELETE FROM waveform_cache WHERE peak_count <= 0", []);
    Ok(())
}

pub(super) fn load_waveform_from_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
) -> Option<Vec<f32>> {
    let path = path.to_string_lossy().to_string();
    let row: (i64, i64, Vec<u8>) = conn
        .query_row(
            r"
            SELECT format_version, peak_count, peaks_blob
            FROM waveform_cache
            WHERE path=?1
              AND size_bytes=?2
              AND modified_secs=?3
              AND modified_nanos=?4
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
                ))
            },
        )
        .ok()?;
    if row.0 != WAVEFORM_CACHE_FORMAT_VERSION {
        return None;
    }
    let peak_count = usize::try_from(row.1).ok()?;
    if peak_count == 0 {
        return None;
    }
    decode_peaks_blob(&row.2, peak_count)
}

pub(super) fn persist_waveform_to_db(
    conn: &Connection,
    path: &Path,
    stamp: WaveformSourceStamp,
    peaks: &[f32],
) -> rusqlite::Result<()> {
    if peaks.is_empty() {
        let _ = conn.execute(
            "DELETE FROM waveform_cache WHERE path = ?1",
            params![path.to_string_lossy().to_string()],
        );
        return Ok(());
    }
    let blob = encode_peaks_blob(peaks);
    let now = unix_ts_i64();
    conn.execute(
        r"
        INSERT INTO waveform_cache(
            path, size_bytes, modified_secs, modified_nanos,
            format_version, peak_count, peaks_blob, updated_at
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(path) DO UPDATE SET
            size_bytes=excluded.size_bytes,
            modified_secs=excluded.modified_secs,
            modified_nanos=excluded.modified_nanos,
            format_version=excluded.format_version,
            peak_count=excluded.peak_count,
            peaks_blob=excluded.peaks_blob,
            updated_at=excluded.updated_at
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
        ],
    )?;
    Ok(())
}

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

#[cfg(test)]
mod tests {
    use super::*;

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
        };
        let peaks = vec![0.1f32, 0.3, 0.9];
        persist_waveform_to_db(&conn, &path, stamp, &peaks).expect("persist");
        let loaded = load_waveform_from_db(&conn, &path, stamp).expect("load");
        assert_eq!(loaded, peaks);
    }
}
