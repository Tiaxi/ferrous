use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{unbounded, Receiver, Sender, TryRecvError};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::Accessor;
use rusqlite::{params, Connection};
use walkdir::WalkDir;

#[derive(Debug, Clone, Default)]
pub struct LibraryTrack {
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySearchTrack {
    pub path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
    pub score: f32,
}

#[derive(Debug, Clone, Default)]
pub struct LibraryScanProgress {
    pub current_root: Option<PathBuf>,
    pub roots_completed: usize,
    pub roots_total: usize,
    pub supported_files_discovered: usize,
    pub supported_files_processed: usize,
    pub files_per_second: Option<f32>,
    pub eta_seconds: Option<f32>,
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySnapshot {
    pub roots: Vec<PathBuf>,
    pub tracks: Vec<LibraryTrack>,
    pub scan_in_progress: bool,
    pub scan_progress: Option<LibraryScanProgress>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone)]
pub enum LibraryCommand {
    ScanRoot(PathBuf),
    AddRoot(PathBuf),
    RemoveRoot(PathBuf),
    RescanRoot(PathBuf),
    RescanAll,
}

#[derive(Debug, Clone)]
pub enum LibraryEvent {
    Snapshot(LibrarySnapshot),
}

pub struct LibraryService {
    tx: Sender<LibraryCommand>,
}

impl LibraryService {
    pub fn new() -> (Self, Receiver<LibraryEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<LibraryCommand>();
        let (event_tx, event_rx) = unbounded::<LibraryEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-library".to_string())
            .spawn(move || {
                let mut snapshot = LibrarySnapshot::default();

                match open_library_db() {
                    Ok(conn) => {
                        if let Err(err) = init_schema(&conn) {
                            snapshot.last_error = Some(format!("library DB init failed: {err}"));
                            emit_snapshot(&event_tx, &snapshot);
                            return;
                        }
                        load_snapshot(&conn, &mut snapshot);
                        emit_snapshot(&event_tx, &snapshot);

                        while let Ok(cmd) = cmd_rx.recv() {
                            snapshot.last_error = None;
                            let result = match cmd {
                                LibraryCommand::ScanRoot(root) | LibraryCommand::AddRoot(root) => {
                                    handle_add_root(&conn, &root, &mut snapshot, &event_tx)
                                }
                                LibraryCommand::RemoveRoot(root) => {
                                    remove_root_and_purge(&conn, &root)
                                }
                                LibraryCommand::RescanRoot(root) => {
                                    handle_rescan_root(&conn, &root, &mut snapshot, &event_tx)
                                }
                                LibraryCommand::RescanAll => {
                                    handle_rescan_all(&conn, &mut snapshot, &event_tx)
                                }
                            };

                            if let Err(err) = result {
                                snapshot.last_error = Some(err);
                            }

                            load_snapshot(&conn, &mut snapshot);
                            snapshot.scan_in_progress = false;
                            snapshot.scan_progress = None;
                            emit_snapshot(&event_tx, &snapshot);
                        }
                    }
                    Err(err) => {
                        snapshot.last_error = Some(format!("library DB open failed: {err}"));
                        emit_snapshot(&event_tx, &snapshot);
                    }
                }
            });

        (Self { tx: cmd_tx }, event_rx)
    }

    pub fn command(&self, cmd: LibraryCommand) {
        let _ = self.tx.send(cmd);
    }
}

#[derive(Debug, Clone, Default)]
struct RootScanProgress {
    discovered: usize,
    processed: usize,
    files_per_second: Option<f32>,
    eta_seconds: Option<f32>,
}

fn emit_snapshot(event_tx: &Sender<LibraryEvent>, snapshot: &LibrarySnapshot) {
    let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));
}

fn handle_add_root(
    conn: &Connection,
    root: &Path,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    let root = canonicalize_root(root)?;
    insert_root(conn, &root)?;
    // Reflect the newly-added root in UI state immediately, before scan completion.
    snapshot.roots = load_roots(conn);
    run_scans(conn, &[root], snapshot, event_tx)
}

fn handle_rescan_root(
    conn: &Connection,
    root: &Path,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    let root = if root.exists() {
        root.canonicalize().unwrap_or_else(|_| root.to_path_buf())
    } else {
        root.to_path_buf()
    };
    let roots = load_roots(conn)
        .into_iter()
        .filter(|known| known == &root)
        .collect::<Vec<_>>();
    if roots.is_empty() {
        return Err(format!("root '{}' is not configured", root.display()));
    }
    run_scans(conn, &roots, snapshot, event_tx)
}

fn handle_rescan_all(
    conn: &Connection,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    let roots = load_roots(conn);
    if roots.is_empty() {
        return Ok(());
    }
    run_scans(conn, &roots, snapshot, event_tx)
}

fn run_scans(
    conn: &Connection,
    roots: &[PathBuf],
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    if roots.is_empty() {
        return Ok(());
    }

    let roots_total = roots.len();
    let snapshot_emit_interval = scan_snapshot_emit_interval();
    let snapshot_min_processed_delta = scan_snapshot_min_processed_delta();
    for (idx, root) in roots.iter().enumerate() {
        let mut final_progress = RootScanProgress::default();
        let mut root_track_indices: HashMap<String, usize> = HashMap::new();
        for (track_index, track) in snapshot.tracks.iter().enumerate() {
            if track.root_path == *root {
                root_track_indices.insert(track.path.to_string_lossy().to_string(), track_index);
            }
        }
        let pending_upserts = std::cell::RefCell::new(Vec::<(MetadataTask, IndexedTrack)>::new());
        let mut last_snapshot_emit = Instant::now()
            .checked_sub(snapshot_emit_interval)
            .unwrap_or_else(Instant::now);
        let mut last_snapshot_processed = 0usize;
        let mut collect_upsert = |task: &MetadataTask, indexed: &IndexedTrack| {
            pending_upserts
                .borrow_mut()
                .push((task.clone(), indexed.clone()));
        };
        let mut on_progress = |progress: RootScanProgress| {
            apply_pending_upserts_for_root(
                snapshot,
                root,
                &mut root_track_indices,
                &pending_upserts,
            );
            final_progress = progress.clone();
            snapshot.scan_in_progress = true;
            snapshot.scan_progress = Some(LibraryScanProgress {
                current_root: Some(root.clone()),
                roots_completed: idx,
                roots_total,
                supported_files_discovered: progress.discovered,
                supported_files_processed: progress.processed,
                files_per_second: progress.files_per_second,
                eta_seconds: progress.eta_seconds,
            });
            let processed_delta = progress.processed.saturating_sub(last_snapshot_processed);
            let emit_due = progress.processed == 0
                || progress.processed >= progress.discovered
                || processed_delta >= snapshot_min_processed_delta
                || last_snapshot_emit.elapsed() >= snapshot_emit_interval;
            if emit_due {
                emit_snapshot(event_tx, snapshot);
                last_snapshot_emit = Instant::now();
                last_snapshot_processed = progress.processed;
            }
        };

        let stale_paths = scan_root(conn, root, &mut on_progress, &mut collect_upsert)?;
        apply_pending_upserts_for_root(snapshot, root, &mut root_track_indices, &pending_upserts);
        if !stale_paths.is_empty() {
            let stale_set: HashSet<String> = stale_paths.into_iter().collect();
            snapshot
                .tracks
                .retain(|track| !stale_set.contains(track.path.to_string_lossy().as_ref()));
        }

        snapshot.scan_in_progress = true;
        snapshot.scan_progress = Some(LibraryScanProgress {
            current_root: Some(root.clone()),
            roots_completed: idx + 1,
            roots_total,
            supported_files_discovered: final_progress.discovered,
            supported_files_processed: final_progress.processed,
            files_per_second: final_progress.files_per_second,
            eta_seconds: None,
        });
        emit_snapshot(event_tx, snapshot);
    }

    Ok(())
}

fn apply_pending_upserts_for_root(
    snapshot: &mut LibrarySnapshot,
    root: &Path,
    root_track_indices: &mut HashMap<String, usize>,
    pending_upserts: &std::cell::RefCell<Vec<(MetadataTask, IndexedTrack)>>,
) {
    let mut updates = pending_upserts.borrow_mut();
    if updates.is_empty() {
        return;
    }
    for (task, indexed) in updates.drain(..) {
        let as_snapshot_track = LibraryTrack {
            path: task.path.clone(),
            root_path: root.to_path_buf(),
            title: indexed.title,
            artist: indexed.artist,
            album: indexed.album,
            genre: indexed.genre,
            year: indexed.year,
            track_no: indexed.track_no,
            duration_secs: indexed.duration_secs,
        };

        if let Some(existing_index) = root_track_indices.get(&task.path_string).copied() {
            if let Some(existing) = snapshot.tracks.get_mut(existing_index) {
                *existing = as_snapshot_track;
            }
            continue;
        }

        let new_index = snapshot.tracks.len();
        snapshot.tracks.push(as_snapshot_track);
        root_track_indices.insert(task.path_string, new_index);
    }
}

fn open_library_db() -> anyhow::Result<Connection> {
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

fn build_fts_query(raw: &str) -> Option<String> {
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
    let limit = limit.clamp(1, 5000) as i64;

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
                t.title,
                t.artist,
                t.album,
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
                title: row.get::<_, String>(1)?,
                artist: row.get::<_, String>(2)?,
                album: row.get::<_, String>(3)?,
                genre: row.get::<_, String>(4)?,
                year: row
                    .get::<_, Option<i64>>(5)?
                    .and_then(|v| i32::try_from(v).ok()),
                track_no: row
                    .get::<_, Option<i64>>(6)?
                    .and_then(|v| u32::try_from(v).ok()),
                duration_secs: row.get::<_, Option<f32>>(7)?,
                score: row.get::<_, f64>(8).map_or(0.0, |v| v as f32),
            })
        })
        .map_err(|e| format!("failed to execute search query: {e}"))?;

    for row in rows.flatten() {
        out.push(row);
    }

    Ok(out)
}

fn init_schema(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(
        r"
        CREATE TABLE IF NOT EXISTS roots (
            path TEXT PRIMARY KEY,
            added_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS tracks (
            path TEXT PRIMARY KEY,
            root_path TEXT NOT NULL,
            title TEXT NOT NULL,
            artist TEXT NOT NULL,
            album TEXT NOT NULL,
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

    // Migrations for existing DBs created before metadata expansion.
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN root_path TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute(
        "ALTER TABLE tracks ADD COLUMN genre TEXT NOT NULL DEFAULT ''",
        [],
    );
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN year INTEGER", []);
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN track_no INTEGER", []);

    // Build indexes after migrations so pre-existing DBs without root_path can initialize.
    conn.execute_batch(
        r"
        CREATE INDEX IF NOT EXISTS idx_tracks_root_path ON tracks(root_path);
        CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
        CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album);
        CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title);
        CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);
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

    let track_count: i64 = conn
        .query_row("SELECT COUNT(*) FROM tracks", [], |row| row.get(0))
        .unwrap_or(0);
    let fts_count: i64 = conn
        .query_row("SELECT COUNT(*) FROM tracks_fts", [], |row| row.get(0))
        .unwrap_or(0);
    if track_count > 0 && fts_count == 0 {
        let _ = conn.execute("INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild')", []);
    }

    Ok(())
}

fn load_roots(conn: &Connection) -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if let Ok(mut stmt) = conn.prepare("SELECT path FROM roots ORDER BY path COLLATE NOCASE") {
        if let Ok(rows) = stmt.query_map([], |row| row.get::<_, String>(0)) {
            for row in rows.flatten() {
                roots.push(PathBuf::from(row));
            }
        }
    }
    roots
}

fn load_snapshot(conn: &Connection, snapshot: &mut LibrarySnapshot) {
    snapshot.roots = load_roots(conn);
    snapshot.tracks.clear();

    if let Ok(mut stmt) = conn.prepare(
        r"
        SELECT path, root_path, title, artist, album, genre, year, track_no, duration_secs
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
                genre: row.get::<_, String>(5)?,
                year: row
                    .get::<_, Option<i64>>(6)?
                    .and_then(|v| i32::try_from(v).ok()),
                track_no: row
                    .get::<_, Option<i64>>(7)?
                    .and_then(|v| u32::try_from(v).ok()),
                duration_secs: row.get::<_, Option<f32>>(8)?,
            })
        }) {
            for row in rows.flatten() {
                snapshot.tracks.push(row);
            }
        }
    }
}

fn canonicalize_root(root: &Path) -> Result<PathBuf, String> {
    let root = root
        .canonicalize()
        .map_err(|e| format!("failed to access '{}': {e}", root.display()))?;
    if !root.is_dir() {
        return Err(format!("'{}' is not a directory", root.display()));
    }
    Ok(root)
}

fn insert_root(conn: &Connection, root: &Path) -> Result<(), String> {
    let now = unix_ts_i64();
    let root_str = root.to_string_lossy().to_string();
    conn.execute(
        "INSERT OR IGNORE INTO roots(path, added_at) VALUES (?1, ?2)",
        params![root_str, now],
    )
    .map_err(|e| format!("failed to save root '{}': {e}", root.display()))?;
    Ok(())
}

fn remove_root_and_purge(conn: &Connection, root: &Path) -> Result<(), String> {
    let root = if root.exists() {
        root.canonicalize().unwrap_or_else(|_| root.to_path_buf())
    } else {
        root.to_path_buf()
    };
    let root_str = root.to_string_lossy().to_string();

    let tx = conn
        .unchecked_transaction()
        .map_err(|e| format!("failed to begin remove transaction: {e}"))?;

    tx.execute("DELETE FROM roots WHERE path=?1", params![root_str.clone()])
        .map_err(|e| format!("failed to remove root '{}': {e}", root.display()))?;

    tx.execute(
        r"
        DELETE FROM tracks
        WHERE root_path = ?1
           OR path = ?1
           OR path LIKE ?1 || '/%'
        ",
        params![root_str],
    )
    .map_err(|e| format!("failed to purge root tracks '{}': {e}", root.display()))?;

    tx.commit()
        .map_err(|e| format!("failed to finalize root removal '{}': {e}", root.display()))?;

    Ok(())
}

#[derive(Debug, Clone)]
struct MetadataTask {
    path: PathBuf,
    path_string: String,
    mtime_ns: i64,
    size_bytes: i64,
}

#[derive(Debug, Clone)]
struct MetadataResult {
    task: MetadataTask,
    indexed: IndexedTrack,
}

fn scan_worker_count() -> usize {
    const MAX_SCAN_WORKERS: usize = 32;
    if let Ok(raw) = std::env::var("FERROUS_SCAN_WORKERS") {
        if let Ok(parsed) = raw.trim().parse::<usize>() {
            return parsed.clamp(1, MAX_SCAN_WORKERS);
        }
    }
    let cores = std::thread::available_parallelism()
        .map(std::num::NonZeroUsize::get)
        .unwrap_or(1);
    if cores <= 2 {
        return cores.clamp(1, MAX_SCAN_WORKERS);
    }
    (cores.saturating_mul(2)).clamp(2, 24)
}

fn scan_snapshot_emit_interval() -> Duration {
    std::env::var("FERROUS_LIBRARY_SNAPSHOT_EMIT_MS")
        .ok()
        .and_then(|raw| raw.trim().parse::<u64>().ok())
        .map_or(Duration::from_millis(2500), |ms| {
            Duration::from_millis(ms.clamp(100, 10_000))
        })
}

fn scan_snapshot_min_processed_delta() -> usize {
    std::env::var("FERROUS_LIBRARY_SNAPSHOT_MIN_DELTA")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(256, |delta| delta.clamp(8, 8192))
}

fn scan_root<F, U>(
    conn: &Connection,
    root: &Path,
    on_progress: &mut F,
    on_upsert: &mut U,
) -> Result<Vec<String>, String>
where
    F: FnMut(RootScanProgress),
    U: FnMut(&MetadataTask, &IndexedTrack),
{
    let root = canonicalize_root(root)?;
    insert_root(conn, &root)?;

    let root_str = root.to_string_lossy().to_string();
    let mut existing: HashMap<String, (i64, i64)> = HashMap::new();
    if let Ok(mut stmt) = conn.prepare(
        r"
        SELECT path, mtime_ns, size_bytes
        FROM tracks
        WHERE root_path = ?1
           OR (root_path = '' AND (path = ?1 OR path LIKE ?1 || '/%'))
        ",
    ) {
        let mapped = stmt.query_map(params![root_str.clone()], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, i64>(2)?,
            ))
        });
        if let Ok(rows) = mapped {
            for item in rows.flatten() {
                existing.insert(item.0, (item.1, item.2));
            }
        }
    }

    let previous_index_count = existing.len();
    let mut seen_paths: HashSet<String> = HashSet::new();
    let tx = match conn.unchecked_transaction() {
        Ok(tx) => tx,
        Err(e) => return Err(format!("failed to begin transaction: {e}")),
    };
    let mut upsert_stmt = tx
        .prepare_cached(
            r"
            INSERT INTO tracks(
                path,
                root_path,
                title,
                artist,
                album,
                genre,
                year,
                track_no,
                duration_secs,
                mtime_ns,
                size_bytes,
                indexed_at
            )
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)
            ON CONFLICT(path) DO UPDATE SET
                root_path=excluded.root_path,
                title=excluded.title,
                artist=excluded.artist,
                album=excluded.album,
                genre=excluded.genre,
                year=excluded.year,
                track_no=excluded.track_no,
                duration_secs=excluded.duration_secs,
                mtime_ns=excluded.mtime_ns,
                size_bytes=excluded.size_bytes,
                indexed_at=excluded.indexed_at
            ",
        )
        .map_err(|e| format!("failed to prepare track upsert statement: {e}"))?;

    let mut discovered = 0usize;
    let mut processed = 0usize;
    let mut smoothed_rate = None::<f32>;
    let start = Instant::now();
    let mut last_emit = Instant::now()
        .checked_sub(Duration::from_millis(500))
        .unwrap_or_else(Instant::now);

    let mut emit_progress = |force: bool, discovered: usize, processed: usize| {
        if !force && last_emit.elapsed() < Duration::from_millis(180) {
            return;
        }

        let elapsed = start.elapsed().as_secs_f32();
        let files_per_second = if processed >= 4 && elapsed >= 0.8 {
            let instant_rate = processed as f32 / elapsed.max(0.001);
            let next = match smoothed_rate {
                Some(prev) => prev * 0.75 + instant_rate * 0.25,
                None => instant_rate,
            };
            smoothed_rate = Some(next);
            Some(next)
        } else {
            None
        };

        let eta_seconds = if previous_index_count > 0 {
            let estimated_total = discovered.max(previous_index_count);
            if let Some(rate) = files_per_second {
                if rate >= 0.5 && processed < estimated_total {
                    Some((estimated_total - processed) as f32 / rate)
                } else {
                    None
                }
            } else {
                None
            }
        } else {
            None
        };

        on_progress(RootScanProgress {
            discovered,
            processed,
            files_per_second,
            eta_seconds,
        });
        last_emit = Instant::now();
    };

    emit_progress(true, discovered, processed);

    let now = unix_ts_i64();
    let worker_count = scan_worker_count();
    let max_pending_tasks = worker_count.saturating_mul(256).clamp(512, 8192);
    let mut workers = Vec::new();
    let mut task_tx: Option<Sender<MetadataTask>> = None;
    let mut result_rx: Option<Receiver<MetadataResult>> = None;
    let mut pending_metadata_tasks = 0usize;

    if worker_count > 1 {
        let (tx_tasks, rx_tasks) = unbounded::<MetadataTask>();
        let (tx_results, rx_results) = unbounded::<MetadataResult>();
        for _ in 0..worker_count {
            let rx_tasks = rx_tasks.clone();
            let tx_results = tx_results.clone();
            workers.push(thread::spawn(move || {
                while let Ok(task) = rx_tasks.recv() {
                    let indexed = read_track_info(&task.path);
                    if tx_results.send(MetadataResult { task, indexed }).is_err() {
                        break;
                    }
                }
            }));
        }
        drop(rx_tasks);
        drop(tx_results);
        task_tx = Some(tx_tasks);
        result_rx = Some(rx_results);
    }

    macro_rules! apply_metadata_result {
        ($result:expr) => {{
            let result = $result;
            let task = result.task;
            let indexed = result.indexed;
            let _ = upsert_stmt.execute(params![
                task.path_string.as_str(),
                root_str.as_str(),
                indexed.title.as_str(),
                indexed.artist.as_str(),
                indexed.album.as_str(),
                indexed.genre.as_str(),
                indexed.year.map(i64::from),
                indexed.track_no.map(i64::from),
                indexed.duration_secs,
                task.mtime_ns,
                task.size_bytes,
                now,
            ]);
            on_upsert(&task, &indexed);
            processed = processed.saturating_add(1);
            emit_progress(false, discovered, processed);
        }};
    }

    for entry in WalkDir::new(&root)
        .follow_links(false)
        .into_iter()
        .filter_map(std::result::Result::ok)
    {
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        if !is_supported_audio(path) {
            continue;
        }

        discovered = discovered.saturating_add(1);

        let Ok(metadata) = fs::metadata(path) else {
            processed = processed.saturating_add(1);
            emit_progress(false, discovered, processed);
            continue;
        };
        let size_bytes = metadata.len() as i64;
        let mtime_ns = metadata
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |d| d.as_nanos() as i64);

        let path_string = path.to_string_lossy().to_string();
        seen_paths.insert(path_string.clone());

        let needs_update = match existing.get(&path_string) {
            Some((old_mtime, old_size)) => *old_mtime != mtime_ns || *old_size != size_bytes,
            None => true,
        };

        if needs_update {
            let task = MetadataTask {
                path: path.to_path_buf(),
                path_string,
                mtime_ns,
                size_bytes,
            };
            if let Some(task_tx_ref) = task_tx.as_ref() {
                match task_tx_ref.send(task) {
                    Ok(()) => {
                        pending_metadata_tasks = pending_metadata_tasks.saturating_add(1);
                    }
                    Err(err) => {
                        let task = err.into_inner();
                        let indexed = read_track_info(&task.path);
                        apply_metadata_result!(MetadataResult { task, indexed });
                    }
                }
            } else {
                let indexed = read_track_info(&task.path);
                apply_metadata_result!(MetadataResult { task, indexed });
            }
        } else {
            processed = processed.saturating_add(1);
            emit_progress(false, discovered, processed);
        }

        if let Some(result_rx_ref) = result_rx.as_ref() {
            while pending_metadata_tasks > 0 {
                match result_rx_ref.try_recv() {
                    Ok(result) => {
                        pending_metadata_tasks -= 1;
                        apply_metadata_result!(result);
                    }
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        return Err("metadata scan workers disconnected unexpectedly".to_string());
                    }
                }
            }
            if pending_metadata_tasks >= max_pending_tasks {
                match result_rx_ref.recv() {
                    Ok(result) => {
                        pending_metadata_tasks -= 1;
                        apply_metadata_result!(result);
                    }
                    Err(_) => {
                        return Err("metadata scan workers disconnected unexpectedly".to_string());
                    }
                }
            }
        }
    }

    if let Some(task_tx) = task_tx.take() {
        drop(task_tx);
    }
    if let Some(result_rx_ref) = result_rx.as_ref() {
        while pending_metadata_tasks > 0 {
            match result_rx_ref.recv() {
                Ok(result) => {
                    pending_metadata_tasks -= 1;
                    apply_metadata_result!(result);
                }
                Err(_) => {
                    return Err("metadata scan workers disconnected unexpectedly".to_string());
                }
            }
        }
    }
    for worker in workers {
        let _ = worker.join();
    }
    drop(upsert_stmt);

    let stale: Vec<String> = existing
        .into_keys()
        .filter(|p| !seen_paths.contains(p))
        .collect();
    for p in &stale {
        let _ = tx.execute("DELETE FROM tracks WHERE path=?1", params![p]);
    }

    tx.commit()
        .map_err(|e| format!("failed to finalize scan transaction: {e}"))?;

    emit_progress(true, discovered, processed);
    Ok(stale)
}

fn is_supported_audio(path: &Path) -> bool {
    let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
        return false;
    };
    matches!(
        ext.to_ascii_lowercase().as_str(),
        "mp3" | "flac" | "m4a" | "aac" | "ogg" | "opus" | "wav"
    )
}

#[derive(Debug, Clone)]
struct IndexedTrack {
    title: String,
    artist: String,
    album: String,
    genre: String,
    year: Option<i32>,
    track_no: Option<u32>,
    duration_secs: Option<f32>,
}

fn read_track_info(path: &Path) -> IndexedTrack {
    let mut out = IndexedTrack {
        title: path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or_default()
            .to_owned(),
        artist: String::new(),
        album: String::new(),
        genre: String::new(),
        year: None,
        track_no: None,
        duration_secs: None,
    };

    if let Ok(tagged) = lofty::read_from_path(path) {
        if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
            if let Some(title) = tag.title() {
                out.title = title.into_owned();
            }
            if let Some(artist) = tag.artist() {
                out.artist = artist.into_owned();
            }
            if let Some(album) = tag.album() {
                out.album = album.into_owned();
            }
            if let Some(genre) = tag.genre() {
                out.genre = genre.into_owned();
            }
            out.year = tag.date().map(|v| i32::from(v.year));
            out.track_no = tag.track();
        }
        out.duration_secs = Some(tagged.properties().duration().as_secs_f32());
    }
    out
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn test_dir(name: &str) -> PathBuf {
        let mut p = std::env::temp_dir();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        p.push(format!(
            "ferrous-libtest-{name}-{}-{nanos}",
            std::process::id()
        ));
        p
    }

    fn write_stub(path: &Path, bytes: &[u8]) {
        fs::File::create(path)
            .and_then(|mut f| f.write_all(bytes))
            .expect("write file");
    }

    #[test]
    fn supported_audio_extensions_are_detected() {
        assert!(is_supported_audio(Path::new("a.mp3")));
        assert!(is_supported_audio(Path::new("a.flac")));
        assert!(is_supported_audio(Path::new("a.m4a")));
        assert!(is_supported_audio(Path::new("a.aac")));
        assert!(is_supported_audio(Path::new("a.ogg")));
        assert!(is_supported_audio(Path::new("a.opus")));
        assert!(is_supported_audio(Path::new("a.wav")));
        assert!(!is_supported_audio(Path::new("a.txt")));
        assert!(!is_supported_audio(Path::new("a")));
    }

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

    #[test]
    fn scan_root_indexes_supported_files_only() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("index");
        fs::create_dir_all(&root).expect("mkdir");

        let mp3 = root.join("song1.mp3");
        let flac = root.join("song2.flac");
        let m4a = root.join("song3.m4a");
        let txt = root.join("notes.txt");
        write_stub(&mp3, b"not-real-mp3");
        write_stub(&flac, b"not-real-flac");
        write_stub(&m4a, b"not-real-m4a");
        write_stub(&txt, b"ignore me");

        let mut last = RootScanProgress::default();
        let _ = scan_root(
            &conn,
            &root,
            &mut |progress| {
                last = progress;
            },
            &mut |_, _| {},
        )
        .expect("scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);

        let paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
        assert!(paths.iter().any(|p| p.ends_with("song1.mp3")));
        assert!(paths.iter().any(|p| p.ends_with("song2.flac")));
        assert!(paths.iter().any(|p| p.ends_with("song3.m4a")));
        assert!(!paths.iter().any(|p| p.ends_with("notes.txt")));
        assert_eq!(last.discovered, 3);
        assert_eq!(last.processed, 3);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn scan_root_removes_stale_deleted_tracks() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("stale");
        fs::create_dir_all(&root).expect("mkdir");
        let mp3 = root.join("song1.mp3");
        write_stub(&mp3, b"not-real-mp3");

        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("initial scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 1);

        fs::remove_file(&mp3).expect("remove mp3");
        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("rescan");
        load_snapshot(&conn, &mut snapshot);
        assert!(snapshot.tracks.is_empty());

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn remove_root_purges_only_target_root_tracks() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root_a = test_dir("remove-a");
        let root_b = test_dir("remove-b");
        fs::create_dir_all(&root_a).expect("mkdir a");
        fs::create_dir_all(&root_b).expect("mkdir b");

        let a_track = root_a.join("a.mp3");
        let b_track = root_b.join("b.mp3");
        write_stub(&a_track, b"a");
        write_stub(&b_track, b"b");

        let _ = scan_root(&conn, &root_a, &mut |_| {}, &mut |_, _| {}).expect("scan a");
        let _ = scan_root(&conn, &root_b, &mut |_| {}, &mut |_, _| {}).expect("scan b");

        let root_a_canon = root_a.canonicalize().expect("canon a");
        remove_root_and_purge(&conn, &root_a_canon).expect("remove root a");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);

        assert_eq!(
            snapshot.roots,
            vec![root_b.canonicalize().expect("canon b")]
        );
        assert_eq!(snapshot.tracks.len(), 1);
        assert!(snapshot
            .tracks
            .iter()
            .all(|t| t.path.starts_with(root_b.canonicalize().expect("canon b"))));

        let _ = fs::remove_dir_all(root_a);
        let _ = fs::remove_dir_all(root_b);
    }
}
