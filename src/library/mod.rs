use std::collections::{HashMap, HashSet};
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{unbounded, Receiver, Sender, TryRecvError};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::Accessor;
use rusqlite::{params, Connection};
use walkdir::WalkDir;

use crate::metadata::cached_embedded_cover_path;
use crate::raw_audio::{
    is_raw_surround_file, probe_raw_surround_technical_details, read_appended_apev2_text_metadata,
};

#[derive(Debug, Clone, Default)]
pub struct LibraryTrack {
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_path: String,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LibraryRoot {
    pub path: PathBuf,
    pub name: String,
}

impl LibraryRoot {
    #[must_use]
    pub fn display_name(&self) -> String {
        let trimmed = self.name.trim();
        if trimmed.is_empty() {
            self.path.to_string_lossy().to_string()
        } else {
            trimmed.to_string()
        }
    }

    #[must_use]
    pub fn search_label(&self) -> String {
        let trimmed = self.name.trim();
        if !trimmed.is_empty() {
            return trimmed.to_string();
        }
        self.path.file_name().map_or_else(
            || self.path.to_string_lossy().to_string(),
            |value| value.to_string_lossy().to_string(),
        )
    }
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySearchTrack {
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_path: String,
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
    pub roots: Vec<LibraryRoot>,
    pub tracks: Vec<LibraryTrack>,
    pub search_revision: u64,
    pub scan_in_progress: bool,
    pub scan_progress: Option<LibraryScanProgress>,
    pub last_error: Option<String>,
}

impl LibrarySnapshot {
    fn bump_search_revision(&mut self) {
        self.search_revision = self.search_revision.saturating_add(1);
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TrackFileFingerprint {
    pub(crate) mtime_ns: i64,
    pub(crate) size_bytes: i64,
}

#[derive(Debug, Clone)]
pub enum LibraryCommand {
    ScanRoot(PathBuf),
    AddRoot { path: PathBuf, name: String },
    RenameRoot { path: PathBuf, name: String },
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
    #[must_use]
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
                        snapshot.bump_search_revision();
                        emit_snapshot(&event_tx, &snapshot);

                        while let Ok(cmd) = cmd_rx.recv() {
                            snapshot.last_error = None;
                            let reload_search_data_on_success = matches!(
                                &cmd,
                                LibraryCommand::RenameRoot { .. } | LibraryCommand::RemoveRoot(_)
                            );
                            let result = match cmd {
                                LibraryCommand::ScanRoot(root) => {
                                    handle_add_root(&conn, &root, "", &mut snapshot, &event_tx)
                                }
                                LibraryCommand::AddRoot { path, name } => {
                                    handle_add_root(&conn, &path, &name, &mut snapshot, &event_tx)
                                }
                                LibraryCommand::RenameRoot { path, name } => {
                                    rename_root(&conn, &path, &name)
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

                            if let Err(err) = &result {
                                snapshot.last_error = Some(err.clone());
                            }

                            load_snapshot(&conn, &mut snapshot);
                            if result.is_ok() && reload_search_data_on_success {
                                snapshot.bump_search_revision();
                            }
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

struct ScanProgressReporter<'a, F>
where
    F: FnMut(RootScanProgress),
{
    previous_index_count: usize,
    smoothed_rate: Option<f32>,
    start: Instant,
    last_emit: Instant,
    on_progress: &'a mut F,
}

struct MetadataWorkerPool {
    task_tx: Option<Sender<MetadataTask>>,
    result_rx: Option<Receiver<MetadataResult>>,
    workers: Vec<thread::JoinHandle<()>>,
    pending_tasks: usize,
    max_pending_tasks: usize,
}

fn emit_snapshot(event_tx: &Sender<LibraryEvent>, snapshot: &LibrarySnapshot) {
    let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));
}

impl<'a, F> ScanProgressReporter<'a, F>
where
    F: FnMut(RootScanProgress),
{
    fn new(previous_index_count: usize, on_progress: &'a mut F) -> Self {
        Self {
            previous_index_count,
            smoothed_rate: None,
            start: Instant::now(),
            last_emit: Instant::now()
                .checked_sub(Duration::from_millis(500))
                .unwrap_or_else(Instant::now),
            on_progress,
        }
    }

    fn emit(&mut self, force: bool, discovered: usize, processed: usize) {
        if !force && self.last_emit.elapsed() < Duration::from_millis(180) {
            return;
        }

        let elapsed = self.start.elapsed().as_secs_f32();
        let files_per_second = if processed >= 4 && elapsed >= 0.8 {
            let instant_rate = usize_to_f32(processed) / elapsed.max(0.001);
            let next = match self.smoothed_rate {
                Some(prev) => prev * 0.75 + instant_rate * 0.25,
                None => instant_rate,
            };
            self.smoothed_rate = Some(next);
            Some(next)
        } else {
            None
        };

        let eta_seconds = if self.previous_index_count > 0 {
            let estimated_total = discovered.max(self.previous_index_count);
            if let Some(rate) = files_per_second {
                if rate >= 0.5 && processed < estimated_total {
                    Some(usize_to_f32(estimated_total.saturating_sub(processed)) / rate)
                } else {
                    None
                }
            } else {
                None
            }
        } else {
            None
        };

        (self.on_progress)(RootScanProgress {
            discovered,
            processed,
            files_per_second,
            eta_seconds,
        });
        self.last_emit = Instant::now();
    }
}

impl MetadataWorkerPool {
    fn new(worker_count: usize) -> Self {
        let max_pending_tasks = worker_count.saturating_mul(256).clamp(512, 8192);
        if worker_count <= 1 {
            return Self {
                task_tx: None,
                result_rx: None,
                workers: Vec::new(),
                pending_tasks: 0,
                max_pending_tasks,
            };
        }

        let (tx_tasks, rx_tasks) = unbounded::<MetadataTask>();
        let (tx_results, rx_results) = unbounded::<MetadataResult>();
        let mut workers = Vec::new();
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

        Self {
            task_tx: Some(tx_tasks),
            result_rx: Some(rx_results),
            workers,
            pending_tasks: 0,
            max_pending_tasks,
        }
    }

    fn submit(&mut self, task: MetadataTask) -> Result<(), MetadataTask> {
        let Some(task_tx) = self.task_tx.as_ref() else {
            return Err(task);
        };
        match task_tx.send(task) {
            Ok(()) => {
                self.pending_tasks = self.pending_tasks.saturating_add(1);
                Ok(())
            }
            Err(err) => Err(err.into_inner()),
        }
    }

    fn drain_ready(&mut self) -> Result<Vec<MetadataResult>, String> {
        let Some(result_rx) = self.result_rx.as_ref() else {
            return Ok(Vec::new());
        };
        let mut ready = Vec::new();
        while self.pending_tasks > 0 {
            match result_rx.try_recv() {
                Ok(result) => {
                    self.pending_tasks -= 1;
                    ready.push(result);
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    return Err("metadata scan workers disconnected unexpectedly".to_string());
                }
            }
        }
        Ok(ready)
    }

    fn wait_for_capacity(&mut self) -> Result<Option<MetadataResult>, String> {
        if self.pending_tasks < self.max_pending_tasks {
            return Ok(None);
        }
        let Some(result_rx) = self.result_rx.as_ref() else {
            return Ok(None);
        };
        match result_rx.recv() {
            Ok(result) => {
                self.pending_tasks -= 1;
                Ok(Some(result))
            }
            Err(_) => Err("metadata scan workers disconnected unexpectedly".to_string()),
        }
    }

    fn finish(&mut self) -> Result<Vec<MetadataResult>, String> {
        if let Some(task_tx) = self.task_tx.take() {
            drop(task_tx);
        }
        let Some(result_rx) = self.result_rx.as_ref() else {
            return Ok(Vec::new());
        };
        let mut results = Vec::new();
        while self.pending_tasks > 0 {
            match result_rx.recv() {
                Ok(result) => {
                    self.pending_tasks -= 1;
                    results.push(result);
                }
                Err(_) => {
                    return Err("metadata scan workers disconnected unexpectedly".to_string());
                }
            }
        }
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
        Ok(results)
    }
}

fn handle_add_root(
    conn: &Connection,
    root: &Path,
    name: &str,
    snapshot: &mut LibrarySnapshot,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    let root = canonicalize_root(root)?;
    insert_root(conn, &root, name)?;
    // Reflect the newly-added root in UI state immediately, before scan completion.
    snapshot.roots = load_roots(conn);
    snapshot.bump_search_revision();
    run_scans(conn, &[root], snapshot, event_tx)
}

fn rename_root(conn: &Connection, root: &Path, name: &str) -> Result<(), String> {
    let root = if root.exists() {
        root.canonicalize().unwrap_or_else(|_| root.to_path_buf())
    } else {
        root.to_path_buf()
    };
    update_root_name(conn, &root, name)
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
        .filter(|known| known.path == root)
        .map(|known| known.path)
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
    let roots = load_roots(conn)
        .into_iter()
        .map(|root| root.path)
        .collect::<Vec<_>>();
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
            snapshot.bump_search_revision();
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
    snapshot.bump_search_revision();
    for (task, indexed) in updates.drain(..) {
        let as_snapshot_track = LibraryTrack {
            path: task.path.clone(),
            root_path: root.to_path_buf(),
            title: indexed.title,
            artist: indexed.artist,
            album: indexed.album,
            cover_path: indexed.cover_path,
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

fn usize_to_i64(value: usize) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn u64_to_i64(value: u64) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn u128_to_i64(value: u128) -> i64 {
    i64::try_from(value).unwrap_or(i64::MAX)
}

fn usize_to_f32(value: usize) -> f32 {
    value.to_string().parse::<f32>().unwrap_or(f32::MAX)
}

fn f64_to_f32(value: f64) -> f32 {
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

fn init_schema(conn: &Connection) -> anyhow::Result<()> {
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

pub(crate) fn track_file_fingerprint(path: &Path) -> Option<TrackFileFingerprint> {
    let metadata = fs::metadata(path).ok()?;
    Some(TrackFileFingerprint {
        mtime_ns: metadata
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |duration| u128_to_i64(duration.as_nanos())),
        size_bytes: u64_to_i64(metadata.len()),
    })
}

pub(crate) fn load_external_track_cache(
    path: &Path,
    fingerprint: TrackFileFingerprint,
) -> Option<IndexedTrack> {
    let conn = open_library_db().ok()?;
    init_schema(&conn).ok()?;
    load_external_track_cache_from_conn(&conn, path, fingerprint)
}

pub(crate) fn load_external_track_caches(
    requests: &[(PathBuf, TrackFileFingerprint)],
) -> HashMap<PathBuf, IndexedTrack> {
    let Ok(conn) = open_library_db() else {
        return HashMap::new();
    };
    if init_schema(&conn).is_err() {
        return HashMap::new();
    }
    load_external_track_caches_from_conn(&conn, requests)
}

fn load_external_track_cache_from_conn(
    conn: &Connection,
    path: &Path,
    fingerprint: TrackFileFingerprint,
) -> Option<IndexedTrack> {
    conn.query_row(
        r"
        SELECT title, artist, album, cover_path, genre, year, track_no, duration_secs
        FROM external_tracks
        WHERE path = ?1
          AND mtime_ns = ?2
          AND size_bytes = ?3
        ",
        params![
            path.to_string_lossy().to_string(),
            fingerprint.mtime_ns,
            fingerprint.size_bytes,
        ],
        |row| {
            Ok(IndexedTrack {
                title: row.get::<_, String>(0)?,
                artist: row.get::<_, String>(1)?,
                album: row.get::<_, String>(2)?,
                cover_path: row.get::<_, String>(3)?,
                genre: row.get::<_, String>(4)?,
                year: row
                    .get::<_, Option<i64>>(5)?
                    .and_then(|v| i32::try_from(v).ok()),
                track_no: row
                    .get::<_, Option<i64>>(6)?
                    .and_then(|v| u32::try_from(v).ok()),
                duration_secs: row.get::<_, Option<f32>>(7)?,
            })
        },
    )
    .ok()
}

fn load_external_track_caches_from_conn(
    conn: &Connection,
    requests: &[(PathBuf, TrackFileFingerprint)],
) -> HashMap<PathBuf, IndexedTrack> {
    let mut loaded = HashMap::with_capacity(requests.len());
    for (path, fingerprint) in requests {
        if let Some(indexed) = load_external_track_cache_from_conn(conn, path, *fingerprint) {
            loaded.insert(path.clone(), indexed);
        }
    }
    loaded
}

#[derive(Debug, Clone, Copy)]
struct ExistingTrackScanState {
    mtime_ns: i64,
    size_bytes: i64,
    has_cover_path: bool,
    suspicious_metadata: bool,
}

fn leading_track_number(input: &str) -> Option<u32> {
    let mut n: u32 = 0;
    let mut saw_digit = false;
    for ch in input.chars() {
        if let Some(d) = ch.to_digit(10) {
            saw_digit = true;
            n = n.saturating_mul(10).saturating_add(d);
        } else {
            break;
        }
    }
    if saw_digit {
        Some(n)
    } else {
        None
    }
}

pub(crate) fn store_external_track_cache(
    path: &Path,
    fingerprint: TrackFileFingerprint,
    indexed: &IndexedTrack,
) -> Result<(), String> {
    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    store_external_track_cache_in_conn(&conn, path, fingerprint, indexed)
}

fn store_external_track_cache_in_conn(
    conn: &Connection,
    path: &Path,
    fingerprint: TrackFileFingerprint,
    indexed: &IndexedTrack,
) -> Result<(), String> {
    conn.execute(
        r"
        INSERT INTO external_tracks(
            path,
            title,
            artist,
            album,
            cover_path,
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
            title = excluded.title,
            artist = excluded.artist,
            album = excluded.album,
            cover_path = excluded.cover_path,
            genre = excluded.genre,
            year = excluded.year,
            track_no = excluded.track_no,
            duration_secs = excluded.duration_secs,
            mtime_ns = excluded.mtime_ns,
            size_bytes = excluded.size_bytes,
            indexed_at = excluded.indexed_at
        ",
        params![
            path.to_string_lossy().to_string(),
            indexed.title.as_str(),
            indexed.artist.as_str(),
            indexed.album.as_str(),
            indexed.cover_path.as_str(),
            indexed.genre.as_str(),
            indexed.year.map(i64::from),
            indexed.track_no.map(i64::from),
            indexed.duration_secs,
            fingerprint.mtime_ns,
            fingerprint.size_bytes,
            unix_ts_i64(),
        ],
    )
    .map_err(|e| format!("failed to store external track cache: {e}"))?;
    Ok(())
}

pub(crate) fn refresh_indexed_metadata_for_paths(
    paths: &[PathBuf],
) -> Result<HashMap<PathBuf, IndexedTrack>, String> {
    let mut conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    let tx = conn
        .transaction()
        .map_err(|e| format!("failed to start metadata refresh transaction: {e}"))?;
    let now = unix_ts_i64();
    let mut refreshed = HashMap::with_capacity(paths.len());

    for path in paths {
        let indexed = read_track_info(path);
        let path_string = path.to_string_lossy().to_string();
        let fingerprint = track_file_fingerprint(path);

        tx.execute(
            r"
            UPDATE tracks
            SET title = ?2,
                artist = ?3,
                album = ?4,
                cover_path = ?5,
                cover_checked = 1,
                genre = ?6,
                year = ?7,
                track_no = ?8,
                duration_secs = ?9,
                mtime_ns = COALESCE(?10, mtime_ns),
                size_bytes = COALESCE(?11, size_bytes),
                indexed_at = ?12
            WHERE path = ?1
            ",
            params![
                path_string,
                indexed.title.as_str(),
                indexed.artist.as_str(),
                indexed.album.as_str(),
                indexed.cover_path.as_str(),
                indexed.genre.as_str(),
                indexed.year.map(i64::from),
                indexed.track_no.map(i64::from),
                indexed.duration_secs,
                fingerprint.map(|value| value.mtime_ns),
                fingerprint.map(|value| value.size_bytes),
                now,
            ],
        )
        .map_err(|e| format!("failed to refresh indexed track metadata: {e}"))?;

        let exists_in_external = tx
            .query_row(
                "SELECT 1 FROM external_tracks WHERE path = ?1 LIMIT 1",
                params![path.to_string_lossy().to_string()],
                |_row| Ok(()),
            )
            .is_ok();
        if exists_in_external {
            if let Some(fingerprint) = fingerprint {
                store_external_track_cache_in_conn(&tx, path, fingerprint, &indexed)?;
            }
        }

        refreshed.insert(path.clone(), indexed);
    }

    tx.commit()
        .map_err(|e| format!("failed to finalize metadata refresh transaction: {e}"))?;
    Ok(refreshed)
}

#[derive(Debug, Clone)]
struct PlannedRename {
    old_path: PathBuf,
    temp_path: Option<PathBuf>,
    new_path: PathBuf,
    finalized: bool,
    already_moved: bool,
}

fn build_temp_rename_path(path: &Path, salt: usize) -> PathBuf {
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let stem = path.file_name().map_or_else(
        || String::from("track"),
        |value| value.to_string_lossy().into_owned(),
    );
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0);
    for attempt in 0..1024usize {
        let candidate = parent.join(format!(
            ".ferrous-rename-{timestamp}-{salt}-{attempt}-{stem}"
        ));
        if !candidate.exists() {
            return candidate;
        }
    }
    parent.join(format!(".ferrous-rename-{timestamp}-{salt}-{stem}"))
}

fn rollback_planned_renames(plans: &[PlannedRename]) {
    for plan in plans.iter().rev() {
        if plan.already_moved {
            continue;
        }
        if plan.finalized {
            let _ = fs::rename(&plan.new_path, &plan.old_path);
        } else if let Some(temp_path) = &plan.temp_path {
            if temp_path.exists() {
                let _ = fs::rename(temp_path, &plan.old_path);
            }
        }
    }
}

fn exact_directory_entry_exists(path: &Path) -> Result<bool, String> {
    let Some(parent) = path.parent() else {
        return Ok(path.exists());
    };
    let Some(file_name) = path.file_name() else {
        return Ok(path.exists());
    };
    let entries = fs::read_dir(parent)
        .map_err(|e| format!("failed to read directory {}: {e}", parent.to_string_lossy()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("failed to read directory entry: {e}"))?;
        if entry.file_name() == file_name {
            return Ok(true);
        }
    }
    Ok(false)
}

fn finalize_staged_rename(
    temp_path: &Path,
    new_path: &Path,
    old_path: &Path,
) -> Result<(), String> {
    const RENAME_ATTEMPTS: usize = 8;
    const VISIBILITY_ATTEMPTS: usize = 8;
    for attempt in 0..RENAME_ATTEMPTS {
        if let Err(e) = fs::rename(temp_path, new_path) {
            if attempt + 1 < RENAME_ATTEMPTS && is_case_only_rename(old_path, new_path) {
                thread::sleep(Duration::from_millis(50));
                continue;
            }
            return Err(format!(
                "failed to rename {} to {}: {e}",
                old_path.to_string_lossy(),
                new_path.to_string_lossy()
            ));
        }

        for visibility_attempt in 0..VISIBILITY_ATTEMPTS {
            let target_exists = exact_directory_entry_exists(new_path)?;
            let temp_exists = exact_directory_entry_exists(temp_path)?;
            if target_exists && !temp_exists {
                return Ok(());
            }
            if !temp_exists && new_path.exists() {
                return Ok(());
            }
            if visibility_attempt + 1 < VISIBILITY_ATTEMPTS {
                thread::sleep(Duration::from_millis(100));
            }
        }

        if attempt + 1 < RENAME_ATTEMPTS {
            thread::sleep(Duration::from_millis(100));
        }
    }
    Err(format!(
        "rename from {} to {} did not finalize on disk",
        old_path.to_string_lossy(),
        new_path.to_string_lossy()
    ))
}

fn paths_refer_to_same_file(left: &Path, right: &Path) -> bool {
    if left == right {
        return true;
    }
    if let (Ok(left_metadata), Ok(right_metadata)) = (fs::metadata(left), fs::metadata(right)) {
        #[cfg(unix)]
        if left_metadata.dev() == right_metadata.dev()
            && left_metadata.ino() == right_metadata.ino()
        {
            return true;
        }
    }
    match (fs::canonicalize(left), fs::canonicalize(right)) {
        (Ok(left_canonical), Ok(right_canonical)) => left_canonical == right_canonical,
        _ => false,
    }
}

fn path_matches_any_source(sources: &HashSet<PathBuf>, target: &Path) -> bool {
    sources
        .iter()
        .any(|source| paths_refer_to_same_file(source, target))
}

fn target_matches_other_source(sources: &HashSet<PathBuf>, current: &Path, target: &Path) -> bool {
    sources.iter().any(|source| {
        !paths_refer_to_same_file(source, current) && paths_refer_to_same_file(source, target)
    })
}

fn is_case_only_rename(current: &Path, target: &Path) -> bool {
    if current == target {
        return false;
    }
    current.to_string_lossy().to_lowercase() == target.to_string_lossy().to_lowercase()
}

#[allow(clippy::too_many_lines)]
pub(crate) fn rename_indexed_metadata_paths(
    renames: &[(PathBuf, PathBuf)],
) -> Result<HashMap<PathBuf, IndexedTrack>, String> {
    let mut conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    let tx = conn
        .transaction()
        .map_err(|e| format!("failed to start rename transaction: {e}"))?;
    let now = unix_ts_i64();
    let mut refreshed = HashMap::with_capacity(renames.len());
    let source_paths = renames
        .iter()
        .map(|(old_path, _)| old_path.clone())
        .collect::<HashSet<_>>();
    let mut claimed_targets = HashSet::with_capacity(renames.len());
    let requires_staged_rename = renames.iter().any(|(old_path, new_path)| {
        old_path != new_path && target_matches_other_source(&source_paths, old_path, new_path)
    });

    if requires_staged_rename {
        let mut planned = Vec::<PlannedRename>::new();
        for (index, (old_path, new_path)) in renames.iter().enumerate() {
            if old_path == new_path {
                let indexed = read_track_info(new_path);
                refreshed.insert(new_path.clone(), indexed);
                continue;
            }
            if !claimed_targets.insert(new_path.clone()) {
                return Err(format!(
                    "multiple files resolve to the same rename target {}",
                    new_path.to_string_lossy()
                ));
            }
            if !old_path.exists() && new_path.exists() {
                planned.push(PlannedRename {
                    old_path: old_path.clone(),
                    temp_path: None,
                    new_path: new_path.clone(),
                    finalized: true,
                    already_moved: true,
                });
                continue;
            }
            if new_path.exists()
                && !path_matches_any_source(&source_paths, new_path)
                && !is_case_only_rename(old_path, new_path)
            {
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    new_path.to_string_lossy()
                ));
            }
            let temp_path = build_temp_rename_path(old_path, index);
            if let Err(e) = fs::rename(old_path, &temp_path) {
                rollback_planned_renames(&planned);
                return Err(format!(
                    "failed to stage rename {} to {}: {e}",
                    old_path.to_string_lossy(),
                    temp_path.to_string_lossy()
                ));
            }
            planned.push(PlannedRename {
                old_path: old_path.clone(),
                temp_path: Some(temp_path),
                new_path: new_path.clone(),
                finalized: false,
                already_moved: false,
            });
        }

        for index in 0..planned.len() {
            if planned[index].already_moved {
                continue;
            }
            if planned[index].new_path.exists()
                && !path_matches_any_source(&source_paths, &planned[index].new_path)
                && !is_case_only_rename(&planned[index].old_path, &planned[index].new_path)
            {
                rollback_planned_renames(&planned);
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    planned[index].new_path.to_string_lossy()
                ));
            }
            let Some(temp_path) = planned[index].temp_path.as_ref() else {
                continue;
            };
            if let Err(e) = finalize_staged_rename(
                temp_path,
                &planned[index].new_path,
                &planned[index].old_path,
            ) {
                rollback_planned_renames(&planned);
                return Err(e);
            }
            planned[index].finalized = true;
        }
    } else {
        let mut completed = Vec::<(PathBuf, PathBuf)>::new();
        for (old_path, new_path) in renames {
            if old_path == new_path {
                let indexed = read_track_info(new_path);
                refreshed.insert(new_path.clone(), indexed);
                continue;
            }
            if !claimed_targets.insert(new_path.clone()) {
                return Err(format!(
                    "multiple files resolve to the same rename target {}",
                    new_path.to_string_lossy()
                ));
            }
            if !old_path.exists() && new_path.exists() {
                continue;
            }
            if new_path.exists()
                && !paths_refer_to_same_file(old_path, new_path)
                && !is_case_only_rename(old_path, new_path)
            {
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    new_path.to_string_lossy()
                ));
            }
            if let Err(e) = fs::rename(old_path, new_path) {
                for (moved_old, moved_new) in completed.iter().rev() {
                    let _ = fs::rename(moved_new, moved_old);
                }
                return Err(format!(
                    "failed to rename {} to {}: {e}",
                    old_path.to_string_lossy(),
                    new_path.to_string_lossy()
                ));
            }
            completed.push((old_path.clone(), new_path.clone()));
        }
    }

    for (old_path, new_path) in renames {
        if old_path == new_path {
            continue;
        }

        let indexed = read_track_info(new_path);
        let fingerprint = track_file_fingerprint(new_path)
            .ok_or_else(|| format!("failed to fingerprint {}", new_path.to_string_lossy()))?;
        let old_path_string = old_path.to_string_lossy().to_string();
        let new_path_string = new_path.to_string_lossy().to_string();

        tx.execute(
            r"
            UPDATE tracks
            SET path = ?2,
                title = ?3,
                artist = ?4,
                album = ?5,
                cover_path = ?6,
                cover_checked = 1,
                genre = ?7,
                year = ?8,
                track_no = ?9,
                duration_secs = ?10,
                mtime_ns = ?11,
                size_bytes = ?12,
                indexed_at = ?13
            WHERE path = ?1
            ",
            params![
                old_path_string,
                new_path_string,
                indexed.title.as_str(),
                indexed.artist.as_str(),
                indexed.album.as_str(),
                indexed.cover_path.as_str(),
                indexed.genre.as_str(),
                indexed.year.map(i64::from),
                indexed.track_no.map(i64::from),
                indexed.duration_secs,
                fingerprint.mtime_ns,
                fingerprint.size_bytes,
                now,
            ],
        )
        .map_err(|e| format!("failed to update renamed track row: {e}"))?;

        let existed_in_external = tx
            .query_row(
                "SELECT 1 FROM external_tracks WHERE path = ?1 LIMIT 1",
                params![old_path.to_string_lossy().to_string()],
                |_row| Ok(()),
            )
            .is_ok();
        if existed_in_external {
            tx.execute(
                "DELETE FROM external_tracks WHERE path = ?1",
                params![old_path.to_string_lossy().to_string()],
            )
            .map_err(|e| format!("failed to delete renamed external track row: {e}"))?;
            store_external_track_cache_in_conn(&tx, new_path, fingerprint, &indexed)?;
        }

        refreshed.insert(new_path.clone(), indexed);
    }

    tx.commit()
        .map_err(|e| format!("failed to finalize rename transaction: {e}"))?;
    Ok(refreshed)
}

fn load_roots(conn: &Connection) -> Vec<LibraryRoot> {
    let mut roots = Vec::new();
    if let Ok(mut stmt) = conn.prepare("SELECT path, name FROM roots ORDER BY path COLLATE NOCASE")
    {
        if let Ok(rows) = stmt.query_map([], |row| {
            Ok(LibraryRoot {
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

fn load_snapshot(conn: &Connection, snapshot: &mut LibrarySnapshot) {
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

fn canonicalize_root(root: &Path) -> Result<PathBuf, String> {
    let root = root
        .canonicalize()
        .map_err(|e| format!("failed to access '{}': {e}", root.display()))?;
    if !root.is_dir() {
        return Err(format!("'{}' is not a directory", root.display()));
    }
    Ok(root)
}

fn normalize_root_name(name: &str) -> String {
    name.trim().to_string()
}

fn insert_root(conn: &Connection, root: &Path, name: &str) -> Result<(), String> {
    let now = unix_ts_i64();
    let root_str = root.to_string_lossy().to_string();
    let trimmed_name = normalize_root_name(name);
    conn.execute(
        "INSERT OR IGNORE INTO roots(path, name, added_at) VALUES (?1, ?2, ?3)",
        params![root_str.clone(), trimmed_name, now],
    )
    .map_err(|e| format!("failed to save root '{}': {e}", root.display()))?;
    if !name.trim().is_empty() {
        update_root_name(conn, root, name)?;
    }
    Ok(())
}

fn update_root_name(conn: &Connection, root: &Path, name: &str) -> Result<(), String> {
    let root_str = root.to_string_lossy().to_string();
    let trimmed_name = normalize_root_name(name);
    let changed = conn
        .execute(
            "UPDATE roots SET name = ?1 WHERE path = ?2",
            params![trimmed_name, root_str],
        )
        .map_err(|e| format!("failed to rename root '{}': {e}", root.display()))?;
    if changed == 0 {
        return Err(format!("root '{}' is not configured", root.display()));
    }
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

fn load_existing_tracks_for_root(
    conn: &Connection,
    root_str: &str,
) -> HashMap<String, ExistingTrackScanState> {
    let mut existing = HashMap::new();
    if let Ok(mut stmt) = conn.prepare(
        r"
        SELECT path, title, track_no, mtime_ns, size_bytes, cover_checked
        FROM tracks
        WHERE root_path = ?1
           OR (root_path = '' AND (path = ?1 OR path LIKE ?1 || '/%'))
        ",
    ) {
        let mapped = stmt.query_map(params![root_str], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, String>(1)?,
                row.get::<_, Option<i64>>(2)?,
                row.get::<_, i64>(3)?,
                row.get::<_, i64>(4)?,
                row.get::<_, i64>(5)?,
            ))
        });
        if let Ok(rows) = mapped {
            for item in rows.flatten() {
                let file_stem = Path::new(&item.0)
                    .file_stem()
                    .map_or_else(String::new, |name| name.to_string_lossy().into_owned());
                let filename_fallback = !item.1.trim().is_empty() && item.1 == file_stem;
                let suspicious_metadata = item.1.trim().is_empty()
                    || (item.2.is_none()
                        && filename_fallback
                        && leading_track_number(&file_stem).is_some());
                existing.insert(
                    item.0,
                    ExistingTrackScanState {
                        mtime_ns: item.3,
                        size_bytes: item.4,
                        has_cover_path: item.5 != 0,
                        suspicious_metadata,
                    },
                );
            }
        }
    }
    existing
}

fn prepare_track_upsert_statement<'conn>(
    tx: &'conn rusqlite::Transaction<'conn>,
) -> Result<rusqlite::CachedStatement<'conn>, String> {
    tx.prepare_cached(
        r"
        INSERT INTO tracks(
            path,
            root_path,
            title,
            artist,
            album,
            cover_path,
            cover_checked,
            genre,
            year,
            track_no,
            duration_secs,
            mtime_ns,
            size_bytes,
            indexed_at
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)
        ON CONFLICT(path) DO UPDATE SET
            root_path=excluded.root_path,
            title=excluded.title,
            artist=excluded.artist,
            album=excluded.album,
            cover_path=excluded.cover_path,
            cover_checked=excluded.cover_checked,
            genre=excluded.genre,
            year=excluded.year,
            track_no=excluded.track_no,
            duration_secs=excluded.duration_secs,
            mtime_ns=excluded.mtime_ns,
            size_bytes=excluded.size_bytes,
            indexed_at=excluded.indexed_at
        ",
    )
    .map_err(|e| format!("failed to prepare track upsert statement: {e}"))
}

fn apply_metadata_result<U>(
    upsert_stmt: &mut rusqlite::CachedStatement<'_>,
    root_str: &str,
    now: i64,
    on_upsert: &mut U,
    result: MetadataResult,
) where
    U: FnMut(&MetadataTask, &IndexedTrack),
{
    let task = result.task;
    let indexed = result.indexed;
    let _ = upsert_stmt.execute(params![
        task.path_string.as_str(),
        root_str,
        indexed.title.as_str(),
        indexed.artist.as_str(),
        indexed.album.as_str(),
        indexed.cover_path.as_str(),
        1_i64,
        indexed.genre.as_str(),
        indexed.year.map(i64::from),
        indexed.track_no.map(i64::from),
        indexed.duration_secs,
        task.mtime_ns,
        task.size_bytes,
        now,
    ]);
    on_upsert(&task, &indexed);
}

#[allow(clippy::too_many_lines)]
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
    insert_root(conn, &root, "")?;

    let root_str = root.to_string_lossy().to_string();
    let existing = load_existing_tracks_for_root(conn, &root_str);
    let previous_index_count = existing.len();
    let mut seen_paths: HashSet<String> = HashSet::new();
    let tx = match conn.unchecked_transaction() {
        Ok(tx) => tx,
        Err(e) => return Err(format!("failed to begin transaction: {e}")),
    };
    let mut upsert_stmt = prepare_track_upsert_statement(&tx)?;

    let mut discovered = 0usize;
    let mut processed = 0usize;
    let mut progress = ScanProgressReporter::new(previous_index_count, on_progress);
    progress.emit(true, discovered, processed);

    let now = unix_ts_i64();
    let mut workers = MetadataWorkerPool::new(scan_worker_count());

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
            progress.emit(false, discovered, processed);
            continue;
        };
        let size_bytes = u64_to_i64(metadata.len());
        let mtime_ns = metadata
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |duration| u128_to_i64(duration.as_nanos()));

        let path_string = path.to_string_lossy().to_string();
        seen_paths.insert(path_string.clone());

        let needs_update = match existing.get(&path_string) {
            Some(existing_state) => {
                existing_state.mtime_ns != mtime_ns
                    || existing_state.size_bytes != size_bytes
                    || !existing_state.has_cover_path
                    || existing_state.suspicious_metadata
            }
            None => true,
        };

        if needs_update {
            let task = MetadataTask {
                path: path.to_path_buf(),
                path_string,
                mtime_ns,
                size_bytes,
            };
            if let Err(task) = workers.submit(task) {
                let indexed = read_track_info(&task.path);
                apply_metadata_result(
                    &mut upsert_stmt,
                    &root_str,
                    now,
                    on_upsert,
                    MetadataResult { task, indexed },
                );
                processed = processed.saturating_add(1);
                progress.emit(false, discovered, processed);
            }
        } else {
            processed = processed.saturating_add(1);
            progress.emit(false, discovered, processed);
        }

        for result in workers.drain_ready()? {
            apply_metadata_result(&mut upsert_stmt, &root_str, now, on_upsert, result);
            processed = processed.saturating_add(1);
            progress.emit(false, discovered, processed);
        }
        if let Some(result) = workers.wait_for_capacity()? {
            apply_metadata_result(&mut upsert_stmt, &root_str, now, on_upsert, result);
            processed = processed.saturating_add(1);
            progress.emit(false, discovered, processed);
        }
    }

    for result in workers.finish()? {
        apply_metadata_result(&mut upsert_stmt, &root_str, now, on_upsert, result);
        processed = processed.saturating_add(1);
        progress.emit(false, discovered, processed);
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

    progress.emit(true, discovered, processed);
    Ok(stale)
}

pub(crate) fn is_supported_audio(path: &Path) -> bool {
    let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
        return false;
    };
    matches!(
        ext.to_ascii_lowercase().as_str(),
        "mp3" | "flac" | "m4a" | "aac" | "ogg" | "opus" | "wav" | "ac3" | "dts"
    )
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct IndexedTrack {
    pub(crate) title: String,
    pub(crate) artist: String,
    pub(crate) album: String,
    pub(crate) cover_path: String,
    pub(crate) genre: String,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
    pub(crate) duration_secs: Option<f32>,
}

pub(crate) fn read_track_info(path: &Path) -> IndexedTrack {
    let mut out = IndexedTrack {
        title: path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or_default()
            .to_owned(),
        artist: String::new(),
        album: String::new(),
        cover_path: String::new(),
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
        out.duration_secs = Some(tagged.properties().duration().as_secs_f32()).filter(|d| *d > 0.0);
    }

    if is_raw_surround_file(path) {
        if let Some(tagged) = read_appended_apev2_text_metadata(path) {
            if let Some(title) = tagged.title {
                out.title = title;
            }
            if let Some(artist) = tagged.artist {
                out.artist = artist;
            }
            if let Some(album) = tagged.album {
                out.album = album;
            }
            if let Some(genre) = tagged.genre {
                out.genre = genre;
            }
            out.year = tagged.year.or(out.year);
            out.track_no = tagged.track_no.or(out.track_no);
        }

        if let Some(details) = probe_raw_surround_technical_details(path) {
            out.duration_secs = details.duration_secs.or(out.duration_secs);
        }

        if out.duration_secs.is_none() {
            tracing::warn!(
                "could not determine duration for raw surround file: {}",
                path.display()
            );
        }
    }

    out.cover_path = find_cover_path_for_track(path);
    if out.cover_path.is_empty() {
        out.cover_path = cached_embedded_cover_path(path).unwrap_or_default();
    }
    out
}

fn find_cover_path_for_track(path: &Path) -> String {
    let Some(dir) = path.parent() else {
        return String::new();
    };
    find_image_in_dir(dir).unwrap_or_default()
}

fn find_image_in_dir(dir: &Path) -> Option<String> {
    let mut candidates = vec![
        "cover.jpg",
        "cover.jpeg",
        "cover.png",
        "folder.jpg",
        "folder.jpeg",
        "folder.png",
        "front.jpg",
        "front.png",
    ]
    .into_iter()
    .map(|name| dir.join(name))
    .collect::<Vec<_>>();

    let Ok(read_dir) = fs::read_dir(dir) else {
        return None;
    };
    for entry in read_dir.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
            continue;
        };
        let ext = ext.to_ascii_lowercase();
        if (ext == "jpg" || ext == "jpeg" || ext == "png")
            && !candidates.iter().any(|candidate| candidate == &path)
        {
            candidates.push(path);
        }
    }

    candidates.into_iter().find_map(|candidate| {
        if candidate.is_file() {
            Some(candidate.to_string_lossy().to_string())
        } else {
            None
        }
    })
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| u64_to_i64(duration.as_secs()))
        .unwrap_or(0)
}

pub(crate) fn refresh_cover_paths_for_tracks(paths: &[PathBuf]) -> Result<(), String> {
    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    let now = unix_ts_i64();

    for path in paths {
        let indexed = read_track_info(path);
        conn.execute(
            r"
            UPDATE tracks
            SET cover_path = ?2,
                cover_checked = 1,
                indexed_at = ?3
            WHERE path = ?1
            ",
            params![
                path.to_string_lossy().to_string(),
                indexed.cover_path.as_str(),
                now,
            ],
        )
        .map_err(|e| format!("failed to refresh track cover path: {e}"))?;

        let exists_in_external = conn
            .query_row(
                "SELECT 1 FROM external_tracks WHERE path = ?1 LIMIT 1",
                params![path.to_string_lossy().to_string()],
                |_row| Ok(()),
            )
            .is_ok();
        if exists_in_external {
            let Some(fingerprint) = track_file_fingerprint(path) else {
                continue;
            };
            store_external_track_cache_in_conn(&conn, path, fingerprint, &indexed)?;
        }
    }

    Ok(())
}

pub(crate) fn refresh_cover_paths_for_tracks_with_override(
    paths: &[PathBuf],
    cover_path: &Path,
) -> Result<(), String> {
    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    let now = unix_ts_i64();
    let cover_path_string = cover_path.to_string_lossy().to_string();

    for path in paths {
        let path_string = path.to_string_lossy().to_string();
        conn.execute(
            r"
            UPDATE tracks
            SET cover_path = ?2,
                cover_checked = 1,
                indexed_at = ?3
            WHERE path = ?1
            ",
            params![path_string, cover_path_string.as_str(), now],
        )
        .map_err(|e| format!("failed to refresh track cover path: {e}"))?;

        conn.execute(
            r"
            UPDATE external_tracks
            SET cover_path = ?2,
                indexed_at = ?3
            WHERE path = ?1
            ",
            params![
                path.to_string_lossy().to_string(),
                cover_path_string.as_str(),
                now
            ],
        )
        .map_err(|e| format!("failed to refresh external track cover path: {e}"))?;
    }

    Ok(())
}

pub(crate) fn read_library_snapshot_from_db() -> Result<LibrarySnapshot, String> {
    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    let mut snapshot = LibrarySnapshot::default();
    load_snapshot(&conn, &mut snapshot);
    Ok(snapshot)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::raw_audio::write_test_apev2_file;
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
    fn external_track_cache_invalidates_when_file_fingerprint_changes() {
        let conn = Connection::open_in_memory().expect("open in-memory db");
        init_schema(&conn).expect("init schema");

        let dir = test_dir("external-track-cache");
        fs::create_dir_all(&dir).expect("create cache test dir");
        let path = dir.join("song.flac");
        write_stub(&path, b"version-a");

        let fingerprint_a = track_file_fingerprint(&path).expect("fingerprint a");
        let indexed_a = IndexedTrack {
            title: "Song A".to_string(),
            artist: "Artist".to_string(),
            album: "Album".to_string(),
            cover_path: String::new(),
            genre: "Ambient".to_string(),
            year: Some(2024),
            track_no: Some(3),
            duration_secs: Some(123.0),
        };
        store_external_track_cache_in_conn(&conn, &path, fingerprint_a, &indexed_a)
            .expect("store external cache");

        let loaded_a =
            load_external_track_cache_from_conn(&conn, &path, fingerprint_a).expect("cache hit");
        assert_eq!(loaded_a.title, "Song A");
        assert_eq!(loaded_a.track_no, Some(3));

        std::thread::sleep(Duration::from_millis(2));
        write_stub(&path, b"version-b with different size");
        let fingerprint_b = track_file_fingerprint(&path).expect("fingerprint b");
        assert_ne!(fingerprint_a, fingerprint_b);
        assert!(load_external_track_cache_from_conn(&conn, &path, fingerprint_b).is_none());

        let _ = fs::remove_dir_all(dir);
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
        assert!(is_supported_audio(Path::new("a.ac3")));
        assert!(is_supported_audio(Path::new("a.dts")));
        assert!(!is_supported_audio(Path::new("a.txt")));
        assert!(!is_supported_audio(Path::new("a")));
    }

    #[test]
    fn read_track_info_uses_appended_apev2_for_raw_surround_files() {
        let dir = test_dir("apev2-raw");
        fs::create_dir_all(&dir).expect("create test dir");
        let path = dir.join("01 - The Leper Affinity.dts");
        write_test_apev2_file(
            &path,
            &[
                ("Title", "The Leper Affinity"),
                ("Artist", "Opeth"),
                ("Album", "Blackwater Park"),
                ("Genre", "Progressive death metal"),
                ("Year", "2001"),
                ("Track", "01/8"),
            ],
            true,
        );

        let info = read_track_info(&path);
        assert_eq!(info.title, "The Leper Affinity");
        assert_eq!(info.artist, "Opeth");
        assert_eq!(info.album, "Blackwater Park");
        assert_eq!(info.genre, "Progressive death metal");
        assert_eq!(info.year, Some(2001));
        assert_eq!(info.track_no, Some(1));

        let _ = fs::remove_dir_all(dir);
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
    fn external_track_cache_roundtrips_fresh_metadata() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let path = PathBuf::from("/outside/song.flac");
        let fingerprint = TrackFileFingerprint {
            mtime_ns: 42,
            size_bytes: 1337,
        };
        let indexed = IndexedTrack {
            title: "Outside Song".to_string(),
            artist: "Outside Artist".to_string(),
            album: "Outside Album".to_string(),
            cover_path: "/outside/cover.jpg".to_string(),
            genre: "Ambient".to_string(),
            year: Some(2024),
            track_no: Some(7),
            duration_secs: Some(245.0),
        };

        store_external_track_cache_in_conn(&conn, &path, fingerprint, &indexed)
            .expect("store external cache");
        let loaded = load_external_track_cache_from_conn(&conn, &path, fingerprint)
            .expect("load external cache");

        assert_eq!(loaded, indexed);
    }

    #[test]
    fn external_track_cache_rejects_stale_fingerprint() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let path = PathBuf::from("/outside/song.flac");
        let indexed = IndexedTrack {
            title: "Outside Song".to_string(),
            artist: "Outside Artist".to_string(),
            album: "Outside Album".to_string(),
            cover_path: String::new(),
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: Some(245.0),
        };

        store_external_track_cache_in_conn(
            &conn,
            &path,
            TrackFileFingerprint {
                mtime_ns: 42,
                size_bytes: 1337,
            },
            &indexed,
        )
        .expect("store external cache");

        let stale = load_external_track_cache_from_conn(
            &conn,
            &path,
            TrackFileFingerprint {
                mtime_ns: 99,
                size_bytes: 1337,
            },
        );
        assert!(stale.is_none());
    }

    #[test]
    fn external_track_cache_batch_load_returns_only_matching_rows() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let path_a = PathBuf::from("/outside/a.flac");
        let path_b = PathBuf::from("/outside/b.flac");
        let fingerprint_a = TrackFileFingerprint {
            mtime_ns: 10,
            size_bytes: 100,
        };
        let fingerprint_b = TrackFileFingerprint {
            mtime_ns: 20,
            size_bytes: 200,
        };
        let indexed_a = IndexedTrack {
            title: "Track A".to_string(),
            artist: "Artist A".to_string(),
            album: "Album A".to_string(),
            cover_path: String::new(),
            genre: String::new(),
            year: None,
            track_no: Some(1),
            duration_secs: Some(111.0),
        };

        store_external_track_cache_in_conn(&conn, &path_a, fingerprint_a, &indexed_a)
            .expect("store external cache");

        let loaded = load_external_track_caches_from_conn(
            &conn,
            &[
                (path_a.clone(), fingerprint_a),
                (path_b.clone(), fingerprint_b),
            ],
        );

        assert_eq!(loaded.len(), 1);
        assert_eq!(loaded.get(&path_a), Some(&indexed_a));
        assert!(!loaded.contains_key(&path_b));
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
    fn scan_root_persists_cover_path_from_album_directory() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("cover-path");
        fs::create_dir_all(&root).expect("mkdir");

        let mp3 = root.join("song.mp3");
        let cover = root.join("cover.jpg");
        write_stub(&mp3, b"not-real-mp3");
        write_stub(&cover, b"not-real-jpg");

        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 1);
        assert_eq!(
            snapshot.tracks[0].cover_path,
            cover.to_string_lossy().to_string()
        );

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn scan_root_backfills_missing_cover_path_without_file_changes() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("cover-backfill");
        fs::create_dir_all(&root).expect("mkdir");

        let mp3 = root.join("song.mp3");
        let cover = root.join("cover.jpg");
        write_stub(&mp3, b"not-real-mp3");
        write_stub(&cover, b"not-real-jpg");

        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("initial scan");

        let mp3_path = mp3.to_string_lossy().to_string();
        conn.execute(
            "UPDATE tracks SET cover_path='', cover_checked=0 WHERE path=?1",
            params![mp3_path.as_str()],
        )
        .expect("clear cover path");

        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("rescan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 1);
        assert_eq!(
            snapshot.tracks[0].cover_path,
            cover.to_string_lossy().to_string()
        );

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn scan_root_reprocesses_suspicious_filename_fallback_rows_without_file_changes() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("suspicious-fallback-rescan");
        fs::create_dir_all(&root).expect("mkdir");

        let flac = root.join("08 - Example.flac");
        write_stub(&flac, b"not-real-flac");

        let mut upserted = 0usize;
        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {
            upserted += 1;
        })
        .expect("initial scan");
        assert_eq!(upserted, 1);

        conn.execute(
            "UPDATE tracks SET title=?2, track_no=NULL WHERE path=?1",
            params![flac.to_string_lossy().to_string(), "08 - Example"],
        )
        .expect("inject suspicious row");

        upserted = 0;
        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {
            upserted += 1;
        })
        .expect("rescan");
        assert_eq!(upserted, 1);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn scan_root_does_not_reprocess_unchanged_tracks_without_cover_after_backfill() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("cover-no-repeat");
        fs::create_dir_all(&root).expect("mkdir");

        let mp3 = root.join("song.mp3");
        write_stub(&mp3, b"not-real-mp3");

        let mut first_upserts = 0usize;
        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {
            first_upserts = first_upserts.saturating_add(1);
        })
        .expect("initial scan");
        assert_eq!(first_upserts, 1);

        let mut second_upserts = 0usize;
        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {
            second_upserts = second_upserts.saturating_add(1);
        })
        .expect("rescan");
        assert_eq!(second_upserts, 0);

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
            vec![LibraryRoot {
                path: root_b.canonicalize().expect("canon b"),
                name: String::new(),
            }]
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
