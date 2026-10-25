// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet, VecDeque};
use std::fmt::Write as _;
use std::fs;
use std::io::Read as _;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use crossbeam_channel::{unbounded, Receiver, RecvTimeoutError, Sender, TryRecvError};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::{Accessor, ItemKey};
use rusqlite::{params, Connection, OptionalExtension, TransactionBehavior};
use walkdir::WalkDir;

use crate::file_edit_lock;
use crate::folder_cover::find_folder_cover;
use crate::metadata::EmbeddedCoverCache;
use crate::no_replace::rename_no_replace;
use crate::raw_audio::{
    is_raw_surround_file, probe_raw_surround_technical_details, read_appended_apev2_text_metadata,
};

use super::publish::WorkerLibrary;
use super::schema::{
    init_schema, load_roots, load_snapshot, normalize_root_name, open_library_db, u128_to_i64,
    u64_to_i64, usize_to_f32,
};
use super::{
    IndexedTrack, LibraryEvent, LibraryScanProgress, LibrarySnapshot, LibraryTrack,
    TrackFileFingerprint,
};

#[derive(Debug, Clone, Default)]
struct RootScanProgress {
    discovered: usize,
    processed: usize,
    files_per_second: Option<f32>,
    eta_seconds: Option<f32>,
}

struct ScanProgressReporter {
    previous_index_count: usize,
    smoothed_rate: Option<f32>,
    start: Instant,
    last_emit: Instant,
}

/// Reads files for a scan on worker threads.
struct MetadataWorkerPool {
    task_tx: Option<Sender<MetadataTask>>,
    /// Kept to discard the tasks no worker has started when a scan stops.
    task_rx: Option<Receiver<MetadataTask>>,
    result_rx: Option<Receiver<MetadataResult>>,
    workers: Vec<thread::JoinHandle<()>>,
    pending_tasks: usize,
    max_pending_tasks: usize,
}

impl ScanProgressReporter {
    fn new(previous_index_count: usize) -> Self {
        Self {
            previous_index_count,
            smoothed_rate: None,
            start: Instant::now(),
            last_emit: Instant::now()
                .checked_sub(Duration::from_millis(500))
                .unwrap_or_else(Instant::now),
        }
    }

    /// The progress to report, if it is `force`d or due.
    fn emit(
        &mut self,
        force: bool,
        discovered: usize,
        processed: usize,
    ) -> Option<RootScanProgress> {
        if !force && self.last_emit.elapsed() < Duration::from_millis(180) {
            return None;
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

        self.last_emit = Instant::now();
        Some(RootScanProgress {
            discovered,
            processed,
            files_per_second,
            eta_seconds,
        })
    }
}

impl MetadataWorkerPool {
    fn new(worker_count: usize, covers: Option<&'static EmbeddedCoverCache>) -> Self {
        let max_pending_tasks = worker_count.saturating_mul(256).clamp(512, 8192);
        let inline = Self {
            task_tx: None,
            task_rx: None,
            result_rx: None,
            workers: Vec::new(),
            pending_tasks: 0,
            max_pending_tasks,
        };
        if worker_count <= 1 {
            return inline;
        }

        let (tx_tasks, rx_tasks) = unbounded::<MetadataTask>();
        let (tx_results, rx_results) = unbounded::<MetadataResult>();
        let mut workers = Vec::new();
        for index in 0..worker_count {
            let rx_tasks = rx_tasks.clone();
            let tx_results = tx_results.clone();
            let spawned = thread::Builder::new()
                .name(format!("ferrous-scan-{index}"))
                .spawn(move || {
                    while let Ok(task) = rx_tasks.recv() {
                        let result = read_scan_task(task, covers);
                        if tx_results.send(result).is_err() {
                            break;
                        }
                    }
                });
            match spawned {
                Ok(worker) => workers.push(worker),
                // The scan goes on with the workers that started.
                Err(error) => eprintln!("[ferrous] failed to start a scan worker: {error}"),
            }
        }
        drop(tx_results);
        if workers.is_empty() {
            // The scan reads every file itself, as with one worker.
            return inline;
        }

        Self {
            task_tx: Some(tx_tasks),
            task_rx: Some(rx_tasks),
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

    fn at_capacity(&self) -> bool {
        self.pending_tasks >= self.max_pending_tasks
    }

    fn has_pending(&self) -> bool {
        self.pending_tasks > 0
    }

    /// The next result, or `None` if none arrives within `timeout`, so that
    /// the scan can check for commands while a slow file is read.
    fn next_result(&mut self, timeout: Duration) -> Result<Option<MetadataResult>, String> {
        let Some(result_rx) = self.result_rx.as_ref() else {
            return Ok(None);
        };
        if self.pending_tasks == 0 {
            return Ok(None);
        }
        match result_rx.recv_timeout(timeout) {
            Ok(result) => {
                self.pending_tasks -= 1;
                Ok(Some(result))
            }
            Err(RecvTimeoutError::Timeout) => Ok(None),
            Err(RecvTimeoutError::Disconnected) => {
                Err("metadata scan workers disconnected unexpectedly".to_string())
            }
        }
    }

    /// Ends the workers once they have read every task submitted.
    fn close_tasks(&mut self) {
        self.task_tx = None;
        self.task_rx = None;
    }

    /// Waits for the workers, after [`Self::close_tasks`] and every result.
    fn join(&mut self) {
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}

impl Drop for MetadataWorkerPool {
    /// Stops a scan's reads early: the tasks no worker has started are
    /// discarded, and each worker ends after the file it is reading, without
    /// being waited for. After [`MetadataWorkerPool::join`] there is nothing
    /// left to stop.
    fn drop(&mut self) {
        if let Some(task_rx) = self.task_rx.take() {
            while task_rx.try_recv().is_ok() {}
        }
        self.task_tx = None;
        self.result_rx = None;
    }
}

pub(super) fn handle_add_root(
    conn: &Connection,
    root: &Path,
    name: &str,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    let root = canonicalize_root(root)
        .map_err(|e| format!("cannot add library root '{}': {e}", root.display()))?;
    reject_nested_root(conn, &root)?;
    insert_root(conn, &root, name)?;
    // Reflect the newly-added root in UI state immediately, before scan completion.
    library.set_roots(load_roots(conn));
    run_scans_with(
        conn,
        &[root],
        EmbeddedCoverCache::shared(),
        library,
        event_tx,
        control,
    )
}

pub(super) fn rename_root(conn: &Connection, root: &Path, name: &str) -> Result<(), String> {
    update_root_name(conn, &resolve_root_path(root), name)
}

/// The path a command's root is stored under: canonical while the folder
/// exists, and as given once it is gone.
pub(super) fn resolve_root_path(root: &Path) -> PathBuf {
    if root.exists() {
        root.canonicalize().unwrap_or_else(|_| root.to_path_buf())
    } else {
        root.to_path_buf()
    }
}

pub(super) fn handle_rescan_root(
    conn: &Connection,
    root: &Path,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    let root = resolve_root_path(root);
    let roots = load_roots(conn)
        .into_iter()
        .filter(|known| known.path == root)
        .map(|known| known.path)
        .collect::<Vec<_>>();
    if roots.is_empty() {
        return Err(format!("root '{}' is not configured", root.display()));
    }
    run_scans_with(
        conn,
        &roots,
        EmbeddedCoverCache::shared(),
        library,
        event_tx,
        control,
    )
}

/// Scans `paths`, which changed on disk below `root`; see
/// [`run_scoped_scan`]. A root removed since then is not scanned.
pub(super) fn handle_scan_paths(
    conn: &Connection,
    root: &Path,
    paths: &[PathBuf],
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    let root = resolve_root_path(root);
    if !load_roots(conn).iter().any(|known| known.path == root) {
        return Ok(());
    }
    let covers = EmbeddedCoverCache::shared();
    if paths.contains(&root) {
        return run_scans_with(conn, &[root], covers, library, event_tx, control);
    }
    let scope = outermost_paths_below(&root, paths);
    if scope.is_empty() {
        return Ok(());
    }
    run_scoped_scan(conn, &root, &scope, covers, library, event_tx, control)
}

/// The paths of `paths` strictly below `root` that are not below another of
/// them, sorted.
pub(super) fn outermost_paths_below(root: &Path, paths: &[PathBuf]) -> Vec<PathBuf> {
    let mut below: Vec<&PathBuf> = paths
        .iter()
        .filter(|path| *path != root && path.starts_with(root))
        .collect();
    // Paths sort by component, so a path's descendants follow it.
    below.sort();
    let mut outermost: Vec<PathBuf> = Vec::with_capacity(below.len());
    for path in below {
        if outermost
            .last()
            .is_none_or(|kept: &PathBuf| !path.starts_with(kept))
        {
            outermost.push(path.clone());
        }
    }
    outermost
}

pub(super) fn handle_rescan_all(
    conn: &Connection,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    let roots = load_roots(conn)
        .into_iter()
        .map(|root| root.path)
        .collect::<Vec<_>>();
    if roots.is_empty() {
        return Ok(());
    }
    run_scans_with(
        conn,
        &roots,
        EmbeddedCoverCache::shared(),
        library,
        event_tx,
        control,
    )
}

/// Fills the search tags of rows that have none from their files. Returns
/// the files whose tags could not be read, for the user; their rows stay
/// without search tags, so the next scan reads them again.
pub(super) fn backfill_search_tags(
    conn: &Connection,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
) -> Result<Option<String>, String> {
    backfill_search_tags_using(conn, library, event_tx, |path| {
        let before = track_file_fingerprint(path)?;
        fs::File::open(path).ok()?;
        let read = read_track_catching_panic(path, EmbeddedCoverCache::shared());
        (track_file_fingerprint(path) == Some(before))
            .then(|| read.failure.map_or(Ok(read.indexed.search_tags), Err))
    })
}

/// [`backfill_search_tags`], reading each file with `read_tags`: `None`
/// skips the file, and an error is why its tags could not be read.
fn backfill_search_tags_using(
    conn: &Connection,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    mut read_tags: impl FnMut(&Path) -> Option<Result<super::SearchTags, String>>,
) -> Result<Option<String>, String> {
    let paths = conn
        .prepare("SELECT path FROM tracks WHERE search_tags IS NULL ORDER BY path")
        .map_err(|error| error.to_string())?
        .query_map([], |row| row.get::<_, String>(0))
        .map_err(|error| error.to_string())?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| error.to_string())?;
    let mut failures = ReadFailures::default();
    let mut last_emit = Instant::now();
    for (batch_index, batch) in paths.chunks(256).enumerate() {
        // Metadata I/O must finish before taking a write lock: tag edits and
        // renames remain available while an existing library is backfilled.
        let mut pending = Vec::new();
        for path in batch {
            let path = PathBuf::from(path);
            match read_tags(&path) {
                Some(Ok(tags)) => {
                    let json = serde_json::to_string(&tags).map_err(|error| error.to_string())?;
                    pending.push((path, tags, json));
                }
                Some(Err(failure)) => failures.tags.note(&path, Some(&failure)),
                None => {}
            }
        }
        let tx = conn
            .unchecked_transaction()
            .map_err(|error| error.to_string())?;
        let mut updates = HashMap::new();
        for (path, tags, json) in pending {
            // A concurrent tag edit may have already populated these tags.
            if tx
                .execute(
                    "UPDATE tracks SET search_tags=?2 WHERE path=?1 AND search_tags IS NULL",
                    params![path.to_string_lossy(), json],
                )
                .map_err(|error| error.to_string())?
                > 0
            {
                updates.insert(path, tags);
            }
        }
        tx.commit().map_err(|error| error.to_string())?;
        for (path, tags) in updates {
            library.modify(&path, |track| track.search_tags = tags);
        }
        if last_emit.elapsed() >= scan_snapshot_emit_interval() {
            library.status.scan_in_progress = true;
            library.status.scan_progress = Some(LibraryScanProgress {
                roots_total: library.roots().len(),
                supported_files_discovered: paths.len(),
                supported_files_processed: ((batch_index + 1) * 256).min(paths.len()),
                ..LibraryScanProgress::default()
            });
            library.publish(event_tx);
            last_emit = Instant::now();
        }
    }
    Ok(failures.refresh_problem())
}

/// What a scan does after checking for commands between files.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ScanDirective {
    Continue,
    /// Stop scanning the current root, which was removed, and go on with the
    /// next one.
    SkipRoot,
    /// Stop every scan: the library worker is shutting down.
    StopAll,
}

/// Lets the library worker act on commands while a scan runs.
pub(super) trait ScanControl {
    /// Called between files and batches, while the scan holds no write lock
    /// and the database has everything it committed. May run commands that
    /// must not wait for the scan, such as removing or renaming a root,
    /// updating `library` and publishing it.
    fn poll(
        &mut self,
        conn: &Connection,
        library: &mut WorkerLibrary,
        event_tx: &Sender<LibraryEvent>,
        scanning: &Path,
        remaining: &[PathBuf],
    ) -> ScanDirective;

    /// Whether another connection, such as a tag edit's, committed to the
    /// database since the last call, so that `library` must be read again
    /// before it is published.
    fn external_writes(&mut self, conn: &Connection) -> bool;
}

/// Tells whether another connection committed to the database, with
/// `SQLite`'s `data_version`, which changes only for other connections'
/// commits.
#[derive(Debug, Default)]
pub(super) struct ExternalWrites {
    version: Option<i64>,
}

impl ExternalWrites {
    /// Whether another connection committed since the last call; see
    /// [`ScanControl::external_writes`].
    pub(super) fn since_last_check(&mut self, conn: &Connection) -> bool {
        let Ok(version) = conn.query_row("PRAGMA data_version", [], |row| row.get::<_, i64>(0))
        else {
            return false;
        };
        let changed = self.version.is_some_and(|known| known != version);
        self.version = Some(version);
        changed
    }
}

/// A scan that nothing interrupts.
#[cfg(test)]
pub(super) struct Uninterrupted;

#[cfg(test)]
impl ScanControl for Uninterrupted {
    fn poll(
        &mut self,
        _conn: &Connection,
        _library: &mut WorkerLibrary,
        _event_tx: &Sender<LibraryEvent>,
        _scanning: &Path,
        _remaining: &[PathBuf],
    ) -> ScanDirective {
        ScanDirective::Continue
    }

    fn external_writes(&mut self, _conn: &Connection) -> bool {
        false
    }
}

/// Scans `roots` without interruption; see [`run_scans_with`].
#[cfg(test)]
pub(super) fn run_scans(
    conn: &Connection,
    roots: &[PathBuf],
    covers: Option<&'static EmbeddedCoverCache>,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
) -> Result<(), String> {
    run_scans_with(conn, roots, covers, library, event_tx, &mut Uninterrupted)
}

/// Scans `roots`, caching embedded pictures in `covers`, then removes the
/// entries of `covers` that no track row references. `control` is polled
/// between files; a root it removes is not scanned.
pub(super) fn run_scans_with(
    conn: &Connection,
    roots: &[PathBuf],
    covers: Option<&'static EmbeddedCoverCache>,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    if roots.is_empty() {
        return Ok(());
    }

    let mut run = ScanRun {
        conn,
        covers,
        library,
        event_tx,
        control,
        problems: Vec::new(),
    };
    for (idx, root) in roots.iter().enumerate() {
        let end = run.scan(root, None, (idx, roots.len()), &roots[idx + 1..]);
        if end.stopped_all {
            return scan_result(&run.problems);
        }
    }
    run.remove_unreferenced_covers();
    scan_result(&run.problems)
}

/// Scans the files and folders `scope` of `root`, a configured root, as
/// [`run_scans_with`] scans a whole root: rows of files found there are
/// read again if they changed, and rows of files gone from there are
/// removed. A scope that only a walk of the whole root can scan, because
/// links or hard links reach its files, is scanned by scanning the root.
pub(super) fn run_scoped_scan(
    conn: &Connection,
    root: &Path,
    scope: &[PathBuf],
    covers: Option<&'static EmbeddedCoverCache>,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut dyn ScanControl,
) -> Result<(), String> {
    let mut run = ScanRun {
        conn,
        covers,
        library,
        event_tx,
        control,
        problems: Vec::new(),
    };
    let mut end = run.scan(root, Some(scope), (0, 1), &[]);
    if end.needs_full_scan && !end.stopped_all {
        end = run.scan(root, None, (0, 1), &[]);
        end.changed = true;
    }
    if end.stopped_all {
        return scan_result(&run.problems);
    }
    // Only a scan that wrote or removed rows can leave a cover unreferenced.
    if end.changed {
        run.remove_unreferenced_covers();
    }
    scan_result(&run.problems)
}

/// The scans of one command, and the problems they found.
struct ScanRun<'a> {
    conn: &'a Connection,
    covers: Option<&'static EmbeddedCoverCache>,
    library: &'a mut WorkerLibrary,
    event_tx: &'a Sender<LibraryEvent>,
    control: &'a mut dyn ScanControl,
    problems: Vec<String>,
}

/// How the scan of one root in a [`ScanRun`] ended.
#[derive(Debug, Default)]
struct RootScanEnd {
    /// The worker is shutting down: no other scan runs.
    stopped_all: bool,
    /// See [`RootScanOutcome::needs_full_scan`].
    needs_full_scan: bool,
    /// Whether rows were written or removed.
    changed: bool,
}

impl ScanRun<'_> {
    /// Scans `scope` of `root`, or all of it, as root `position.0` of
    /// `position.1`; `remaining` are the roots scanned after it.
    fn scan(
        &mut self,
        root: &Path,
        scope: Option<&[PathBuf]>,
        position: (usize, usize),
        remaining: &[PathBuf],
    ) -> RootScanEnd {
        // Before the root's reads, and before the commands run between its
        // files, which publish with its rows.
        self.library.begin_reads();
        let conn = self.conn;
        match self
            .control
            .poll(conn, self.library, self.event_tx, root, remaining)
        {
            ScanDirective::Continue => {}
            ScanDirective::SkipRoot => return RootScanEnd::default(),
            ScanDirective::StopAll => {
                return RootScanEnd {
                    stopped_all: true,
                    ..RootScanEnd::default()
                }
            }
        }
        let mut sink = SnapshotSink::new(
            self.library,
            self.event_tx,
            &mut *self.control,
            root,
            remaining,
            position,
        );

        // Roots are independent: a root that fails or cannot be fully read is
        // reported without stopping the remaining scans.
        let result = scan_root_in(conn, root, scope, self.covers, &mut sink);
        let stopped = sink.stopped;
        let committed_any = sink.committed_any;
        let final_progress = sink.final_progress.clone();
        let mut end = RootScanEnd {
            changed: committed_any,
            ..RootScanEnd::default()
        };
        match result {
            Ok(outcome) => {
                // A removed name of a file with other names: only a scan of
                // the root finds the name to index now.
                end.needs_full_scan = outcome.needs_full_scan
                    || (scope.is_some()
                        && outcome
                            .removed
                            .iter()
                            .any(|path| self.library.hard_linked.contains(Path::new(path))));
                if let Some(hard_linked) = outcome.hard_linked {
                    // The roots inside this one, from before nested roots
                    // were refused, are theirs; the scan skipped them.
                    let nested: Vec<PathBuf> = self
                        .library
                        .roots()
                        .iter()
                        .map(|known| known.path.clone())
                        .filter(|known| known != root && known.starts_with(root))
                        .collect();
                    self.library.hard_linked.retain(|path| {
                        !path.starts_with(root)
                            || nested.iter().any(|inner| path.starts_with(inner))
                    });
                    self.library.hard_linked.extend(hard_linked);
                }
                if !outcome.removed.is_empty() {
                    end.changed = true;
                    let stale_set: HashSet<String> = outcome.removed.into_iter().collect();
                    self.library.remove_where(|track| {
                        stale_set.contains(track.path.to_string_lossy().as_ref())
                    });
                }
                self.problems.extend(outcome.problem);
            }
            Err(err) => {
                // The batches committed before the failure stay, and the
                // copy already has them; the failed batch was rolled back.
                // Read the library again so it matches the database
                // whatever the failure left.
                if committed_any {
                    self.control.external_writes(conn);
                    self.library.reload(conn);
                }
                self.problems
                    .push(format!("failed to scan '{}': {err}", root.display()));
            }
        }
        if stopped == Some(ScanDirective::StopAll) {
            end.stopped_all = true;
            return end;
        }

        if self.control.external_writes(conn) {
            self.library.reload(conn);
        }
        let (idx, roots_total) = position;
        self.library.status.scan_in_progress = true;
        self.library.status.scan_progress = Some(LibraryScanProgress {
            current_root: Some(root.to_path_buf()),
            roots_completed: idx + 1,
            roots_total,
            supported_files_discovered: final_progress.discovered,
            supported_files_processed: final_progress.processed,
            files_per_second: final_progress.files_per_second,
            eta_seconds: None,
        });
        self.library.publish(self.event_tx);
        end
    }

    /// Tracks that a scan kept or could not finish still reference their
    /// covers, so the cache cleanup is safe even when a root had problems.
    fn remove_unreferenced_covers(&mut self) {
        if let Err(err) =
            super::cover_cache::remove_unreferenced_embedded_covers(self.conn, self.covers)
        {
            self.problems.push(err);
        }
    }
}

fn scan_result(problems: &[String]) -> Result<(), String> {
    if problems.is_empty() {
        Ok(())
    } else {
        Err(summarize_scan_problems(problems))
    }
}

/// Receives what a scan of one root does: its progress, the rows each batch
/// committed, and the checks for commands between files.
trait ScanSink {
    fn progress(&mut self, conn: &Connection, progress: RootScanProgress);
    fn committed(&mut self, root: &Path, rows: Vec<(MetadataTask, IndexedTrack)>);
    /// Whether the scan of the root must stop now.
    fn should_stop(&mut self, conn: &Connection) -> bool;
}

/// Keeps the library worker's copy in step with a scan of one root, and
/// publishes its changes as the scan goes. A scan that changes no row only
/// publishes its progress.
struct SnapshotSink<'a> {
    library: &'a mut WorkerLibrary,
    event_tx: &'a Sender<LibraryEvent>,
    control: &'a mut dyn ScanControl,
    root: &'a Path,
    remaining: &'a [PathBuf],
    /// The root's index in the run, and the number of roots in the run.
    position: (usize, usize),
    emit_interval: Duration,
    min_processed_delta: usize,
    last_emit: Instant,
    last_processed: usize,
    final_progress: RootScanProgress,
    /// Whether a batch was committed.
    committed_any: bool,
    /// Why the scan stopped early, if it did.
    stopped: Option<ScanDirective>,
}

impl<'a> SnapshotSink<'a> {
    fn new(
        library: &'a mut WorkerLibrary,
        event_tx: &'a Sender<LibraryEvent>,
        control: &'a mut dyn ScanControl,
        root: &'a Path,
        remaining: &'a [PathBuf],
        position: (usize, usize),
    ) -> Self {
        let emit_interval = scan_snapshot_emit_interval();
        Self {
            library,
            event_tx,
            control,
            root,
            remaining,
            position,
            emit_interval,
            min_processed_delta: scan_snapshot_min_processed_delta(),
            last_emit: Instant::now()
                .checked_sub(emit_interval)
                .unwrap_or_else(Instant::now),
            last_processed: 0,
            final_progress: RootScanProgress::default(),
            committed_any: false,
            stopped: None,
        }
    }
}

impl ScanSink for SnapshotSink<'_> {
    fn progress(&mut self, conn: &Connection, progress: RootScanProgress) {
        let (idx, roots_total) = self.position;
        self.library.status.scan_in_progress = true;
        self.library.status.scan_progress = Some(LibraryScanProgress {
            current_root: Some(self.root.to_path_buf()),
            roots_completed: idx,
            roots_total,
            supported_files_discovered: progress.discovered,
            supported_files_processed: progress.processed,
            files_per_second: progress.files_per_second,
            eta_seconds: progress.eta_seconds,
        });
        let processed_delta = progress.processed.saturating_sub(self.last_processed);
        let emit_due = progress.processed == 0
            || progress.processed >= progress.discovered
            || processed_delta >= self.min_processed_delta
            || self.last_emit.elapsed() >= self.emit_interval;
        if emit_due {
            // A tag edit or a rename may have changed rows that this copy
            // still has as they were.
            if self.control.external_writes(conn) {
                self.library.reload(conn);
            }
            self.library.publish(self.event_tx);
            self.last_emit = Instant::now();
            self.last_processed = progress.processed;
        }
        self.final_progress = progress;
    }

    fn committed(&mut self, root: &Path, rows: Vec<(MetadataTask, IndexedTrack)>) {
        if rows.is_empty() {
            return;
        }
        self.committed_any = true;
        for (task, indexed) in rows {
            self.library.upsert(LibraryTrack {
                search_tags: indexed.search_tags,
                path: task.path,
                root_path: root.to_path_buf(),
                title: indexed.title,
                artist: indexed.artist,
                album: indexed.album,
                cover_path: indexed.cover_path,
                genre: indexed.genre,
                year: indexed.year,
                track_no: indexed.track_no,
                duration_secs: indexed.duration_secs,
            });
        }
    }

    fn should_stop(&mut self, conn: &Connection) -> bool {
        let directive =
            self.control
                .poll(conn, self.library, self.event_tx, self.root, self.remaining);
        if directive == ScanDirective::Continue {
            return false;
        }
        self.stopped = Some(directive);
        true
    }
}

/// Joins per-root scan problems into one message for the library error state.
fn summarize_scan_problems(problems: &[String]) -> String {
    const SHOWN: usize = 3;
    let mut message = problems
        .iter()
        .take(SHOWN)
        .map(String::as_str)
        .collect::<Vec<_>>()
        .join("; ");
    if problems.len() > SHOWN {
        let _ = write!(
            message,
            "; {} more library roots had problems",
            problems.len() - SHOWN
        );
    }
    message
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

#[cfg(test)]
pub(crate) fn load_external_track_caches(
    requests: &[(PathBuf, TrackFileFingerprint)],
) -> HashMap<PathBuf, IndexedTrack> {
    if requests.is_empty() {
        return HashMap::new();
    }
    let Ok(conn) = open_library_db() else {
        return HashMap::new();
    };
    if init_schema(&conn).is_err() {
        return HashMap::new();
    }
    load_external_track_caches_from_conn(&conn, requests)
}

/// A queue worker owns one connection; schema checks happen once, not on each heartbeat.
pub(crate) struct ExternalTrackCache(Option<Connection>);

impl ExternalTrackCache {
    #[cfg(test)]
    pub(crate) fn in_memory() -> Self {
        let conn = Connection::open_in_memory().expect("fixture DB");
        init_schema(&conn).expect("fixture schema");
        Self(Some(conn))
    }

    pub(crate) fn new() -> Self {
        let conn = open_library_db()
            .ok()
            .filter(|conn| init_schema(conn).is_ok());
        Self(conn)
    }
    pub(crate) fn load_many(
        &self,
        requests: &[(PathBuf, TrackFileFingerprint)],
    ) -> HashMap<PathBuf, IndexedTrack> {
        self.0.as_ref().map_or_else(HashMap::new, |conn| {
            load_external_track_caches_from_conn(conn, requests)
        })
    }
    /// Caches `read` of `path` at `fingerprint`, and returns the details to
    /// show for the file; see [`cache_external_read`].
    pub(crate) fn store(
        &self,
        path: &Path,
        fingerprint: TrackFileFingerprint,
        read: ExternalTrackRead,
    ) -> IndexedTrack {
        cache_external_read(self.0.as_ref(), path, fingerprint, read.0)
    }
}

/// A file read for the external-track cache by [`read_external_track`].
#[derive(Debug)]
pub(crate) struct ExternalTrackRead(TrackRead);

impl ExternalTrackRead {
    /// The details read, for a file that is not cached.
    pub(crate) fn into_indexed(self) -> IndexedTrack {
        self.0.indexed
    }
}

/// Reads `path` for the external-track cache.
pub(crate) fn read_external_track(path: &Path) -> ExternalTrackRead {
    ExternalTrackRead(read_track_catching_panic(
        path,
        EmbeddedCoverCache::shared(),
    ))
}

/// Caches `read` of `path` at `fingerprint` in `conn`, and returns the
/// details to show for the file.
///
/// A read that failed replaces no row: the row keeps the last good read, and
/// its file state, which no longer matches the file, makes the next lookup
/// read the file again. Meanwhile the file shows the row's tags if its tags
/// could not be read, as a refresh keeps a library row, and the row's cover
/// if its picture could not be cached.
fn cache_external_read(
    conn: Option<&Connection>,
    path: &Path,
    fingerprint: TrackFileFingerprint,
    read: TrackRead,
) -> IndexedTrack {
    let Some(conn) = conn else {
        return read.indexed;
    };
    if read.is_complete() {
        let _ = store_external_track_cache_in_conn(conn, path, fingerprint, &read.indexed);
        return read.indexed;
    }
    // The cache is not needed to show the file, so a row that cannot be read
    // counts as none.
    let stored = stored_track(conn, &path.to_string_lossy()).unwrap_or(None);
    let Ok(update) = row_update(read, || Ok::<_, std::convert::Infallible>(stored));
    update.into_indexed()
}

fn load_external_track_cache_from_conn(
    conn: &Connection,
    path: &Path,
    fingerprint: TrackFileFingerprint,
) -> Option<IndexedTrack> {
    conn.query_row(
        r"
        SELECT title, artist, album, cover_path, genre, year, track_no, duration_secs, search_tags
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
        indexed_track_from_row,
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
    row: RowState,
    has_cover_path: bool,
    suspicious_metadata: bool,
}

/// What tells a scan that another connection wrote a library row since the
/// scan read it: an edit's refresh writes the file's new state, and a cover
/// refresh writes a new cover.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct RowState {
    mtime_ns: i64,
    size_bytes: i64,
    cover_path_hash: u64,
}

impl RowState {
    fn new(mtime_ns: i64, size_bytes: i64, cover_path: &str) -> Self {
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        std::hash::Hash::hash(cover_path, &mut hasher);
        Self {
            mtime_ns,
            size_bytes,
            cover_path_hash: std::hash::Hasher::finish(&hasher),
        }
    }
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
            indexed_at,
            search_tags
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
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
            indexed_at = excluded.indexed_at,
            search_tags = excluded.search_tags
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
            serde_json::to_string(&indexed.search_tags).ok(),
        ],
    )
    .map_err(|e| format!("failed to store external track cache: {e}"))?;
    Ok(())
}

/// Rows read again from their files, and the files that could not be read.
#[derive(Debug, Default)]
pub(crate) struct TrackRefresh {
    /// What the index holds for each file after the refresh.
    pub(crate) indexed: HashMap<PathBuf, IndexedTrack>,
    /// The files that could not be read fully, for the user.
    pub(crate) problem: Option<String>,
}

impl TrackRefresh {
    fn with_capacity(capacity: usize) -> Self {
        Self {
            indexed: HashMap::with_capacity(capacity),
            problem: None,
        }
    }
}

/// Reads `paths` again and updates their library and external-track cache
/// rows; see [`row_update`] for files that cannot be read fully.
pub(crate) fn refresh_indexed_metadata_for_paths(
    paths: &[PathBuf],
) -> Result<TrackRefresh, String> {
    let mut conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    refresh_indexed_metadata_in_conn(&mut conn, paths, EmbeddedCoverCache::shared())
}

fn refresh_indexed_metadata_in_conn(
    conn: &mut Connection,
    paths: &[PathBuf],
    covers: Option<&EmbeddedCoverCache>,
) -> Result<TrackRefresh, String> {
    // Writes lock as they start, so the busy timeout covers another writer;
    // see `rename_indexed_metadata_paths_in_conn`.
    let tx = conn
        .transaction_with_behavior(TransactionBehavior::Immediate)
        .map_err(|e| format!("failed to start metadata refresh transaction: {e}"))?;
    let now = unix_ts_i64();
    let mut refreshed = TrackRefresh::with_capacity(paths.len());
    let mut failures = ReadFailures::default();

    for path in paths {
        let read = read_track_catching_panic(path, covers);
        failures.note(path, &read);
        let complete = read.is_complete();
        let path_string = path.to_string_lossy().to_string();
        let fingerprint = track_file_fingerprint(path);
        let update = row_update(read, || stored_track(&tx, &path_string))?;

        if let RowUpdate::Write {
            indexed,
            cover_checked,
        } = &update
        {
            tx.execute(
                r"
                UPDATE tracks
                SET title = ?2,
                    artist = ?3,
                    album = ?4,
                    cover_path = ?5,
                    cover_checked = ?6,
                    genre = ?7,
                    year = ?8,
                    track_no = ?9,
                    duration_secs = ?10,
                    mtime_ns = COALESCE(?11, mtime_ns),
                    size_bytes = COALESCE(?12, size_bytes),
                    indexed_at = ?13,
                    search_tags = ?14
                WHERE path = ?1
                ",
                params![
                    path_string,
                    indexed.title.as_str(),
                    indexed.artist.as_str(),
                    indexed.album.as_str(),
                    indexed.cover_path.as_str(),
                    i64::from(*cover_checked),
                    indexed.genre.as_str(),
                    indexed.year.map(i64::from),
                    indexed.track_no.map(i64::from),
                    indexed.duration_secs,
                    fingerprint.map(|value| value.mtime_ns),
                    fingerprint.map(|value| value.size_bytes),
                    now,
                    serde_json::to_string(&indexed.search_tags).ok(),
                ],
            )
            .map_err(|e| format!("failed to refresh indexed track metadata: {e}"))?;
        }

        let indexed = update.into_indexed();
        if complete && is_in_external_cache(&tx, &path_string) {
            if let Some(fingerprint) = fingerprint {
                store_external_track_cache_in_conn(&tx, path, fingerprint, &indexed)?;
            }
        }
        refreshed.indexed.insert(path.clone(), indexed);
    }

    tx.commit()
        .map_err(|e| format!("failed to finalize metadata refresh transaction: {e}"))?;
    refreshed.problem = failures.refresh_problem();
    Ok(refreshed)
}

fn is_in_external_cache(conn: &Connection, key: &str) -> bool {
    conn.query_row(
        "SELECT 1 FROM external_tracks WHERE path = ?1 LIMIT 1",
        params![key],
        |_row| Ok(()),
    )
    .is_ok()
}

/// The renames made on disk for one request, in order, so that a failure
/// can put every file back. Files are identified by their index in the
/// request. Every rename, including those that put files back, fails rather
/// than replace a file that has the target name.
struct DiskRenameJournal {
    originals: Vec<PathBuf>,
    locations: Vec<PathBuf>,
    moves: Vec<DiskMove>,
    /// Runs just before each rename on disk, so tests can take a name in
    /// between the checks and the rename.
    #[cfg(test)]
    before_move: Option<Box<dyn FnMut(&Path, &Path)>>,
}

struct DiskMove {
    file: usize,
    from: PathBuf,
    to: PathBuf,
}

impl DiskRenameJournal {
    fn new(renames: &[(PathBuf, PathBuf)]) -> Self {
        let originals = renames
            .iter()
            .map(|(old_path, _)| old_path.clone())
            .collect::<Vec<_>>();
        Self {
            locations: originals.clone(),
            originals,
            moves: Vec::new(),
            #[cfg(test)]
            before_move: None,
        }
    }

    #[cfg(test)]
    fn run_before_move(&mut self, from: &Path, to: &Path) {
        if let Some(hook) = &mut self.before_move {
            hook(from, to);
        }
    }

    fn rename(&mut self, file: usize, from: &Path, to: &Path) -> std::io::Result<()> {
        #[cfg(test)]
        self.run_before_move(from, to);
        rename_between_edits(from, to)?;
        self.record(file, from, to);
        Ok(())
    }

    /// Moves `file`, staged at `temp_path`, to `new_path`, its final name.
    fn finalize(
        &mut self,
        file: usize,
        temp_path: &Path,
        new_path: &Path,
        old_path: &Path,
    ) -> Result<(), String> {
        #[cfg(test)]
        self.run_before_move(temp_path, new_path);
        if let Err(e) = finalize_staged_rename(temp_path, new_path, old_path) {
            // The rename can land even when it is not seen to finish.
            if !path_entry_exists(temp_path) && path_entry_exists(new_path) {
                self.record(file, temp_path, new_path);
            }
            return Err(e);
        }
        self.record(file, temp_path, new_path);
        Ok(())
    }

    /// Renames `file` from `old_path` to `new_path` in two steps through a
    /// temporary name. A case-insensitive filesystem cannot rename a file to
    /// another case of its name in one step without replacing, because it
    /// finds the new name taken by the file itself.
    fn rename_through_temp(
        &mut self,
        file: usize,
        old_path: &Path,
        new_path: &Path,
    ) -> Result<(), String> {
        let temp_path = build_temp_rename_path(old_path);
        self.rename(file, old_path, &temp_path)
            .map_err(|e| rename_failure("stage rename", old_path, &temp_path, &e))?;
        self.finalize(file, &temp_path, new_path, old_path)
    }

    fn record(&mut self, file: usize, from: &Path, to: &Path) {
        self.locations[file] = to.to_path_buf();
        self.moves.push(DiskMove {
            file,
            from: from.to_path_buf(),
            to: to.to_path_buf(),
        });
    }

    /// Undoes the recorded renames, newest first, and adds the outcome to
    /// `error`. A file whose earlier name is taken again stays where it is
    /// rather than replacing the file that now has that name.
    fn restore(mut self, error: String) -> String {
        if self.moves.is_empty() {
            return error;
        }
        let mut failures = Vec::new();
        while let Some(DiskMove { file, from, to }) = self.moves.pop() {
            if self.locations[file] != to {
                // Undoing a later rename of this file failed.
                continue;
            }
            let restored = if path_entry_exists(&from) {
                Err(format!("{} already exists", from.to_string_lossy()))
            } else {
                #[cfg(test)]
                self.run_before_move(&to, &from);
                rename_between_edits(&to, &from).map_err(|e| {
                    if e.kind() == std::io::ErrorKind::AlreadyExists {
                        format!("{} already exists", from.to_string_lossy())
                    } else {
                        format!(
                            "failed to rename {} to {}: {e}",
                            to.to_string_lossy(),
                            from.to_string_lossy()
                        )
                    }
                })
            };
            match restored {
                Ok(()) => self.locations[file] = from,
                Err(failure) => failures.push(failure),
            }
        }
        if failures.is_empty() {
            return format!("{error}; the files keep their original names");
        }
        let (stranded, kept): (Vec<_>, Vec<_>) = self
            .originals
            .iter()
            .zip(&self.locations)
            .partition(|(original, location)| original != location);
        let stranded = stranded
            .into_iter()
            .map(|(original, location)| {
                format!(
                    "{} is now at {}",
                    original.to_string_lossy(),
                    location.to_string_lossy()
                )
            })
            .collect::<Vec<_>>();
        let mut message = format!(
            "{error}; restoring the original file names failed ({}); {}",
            failures.join("; "),
            stranded.join("; ")
        );
        if !kept.is_empty() {
            message.push_str("; the other files keep their original names");
        }
        message
    }
}

/// Renames `from` to `to` without replacing anything, while no tag save is
/// editing `from`. A save that waited for the rename then finds no file,
/// instead of writing the old file back under the old name.
fn rename_between_edits(from: &Path, to: &Path) -> std::io::Result<()> {
    let _edit = file_edit_lock::lock(from);
    rename_no_replace(from, to)
}

/// Whether `path` names a directory entry, also a dangling symbolic link,
/// which `Path::exists` reports as missing.
fn path_entry_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

/// Describes a failed rename of `from` to `to`, as `action` (such as
/// "rename"), with a refusal when `to` was taken.
fn rename_failure(action: &str, from: &Path, to: &Path, error: &std::io::Error) -> String {
    if error.kind() == std::io::ErrorKind::AlreadyExists {
        return format!(
            "refusing to overwrite existing file {}",
            to.to_string_lossy()
        );
    }
    format!(
        "failed to {action} {} to {}: {error}",
        from.to_string_lossy(),
        to.to_string_lossy()
    )
}

/// Starts the names that renames stage files under.
const TEMP_RENAME_PREFIX: &str = ".ferrous-rename-";

/// Returns a free temporary name in the directory of `path`. Its length does
/// not depend on the original name, so files whose names are near the
/// 255-byte limit can be staged too, and the `.tmp` extension keeps library
/// scans from indexing it.
fn build_temp_rename_path(path: &Path) -> PathBuf {
    static NEXT_TEMP_NAME: AtomicU64 = AtomicU64::new(0);
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let pid = std::process::id();
    let mut candidate = PathBuf::new();
    for _ in 0..1024 {
        let counter = NEXT_TEMP_NAME.fetch_add(1, Ordering::Relaxed);
        candidate = parent.join(format!("{TEMP_RENAME_PREFIX}{pid}-{counter}.tmp"));
        if !path_entry_exists(&candidate) {
            break;
        }
    }
    // A name still taken after every attempt makes the no-replace rename
    // fail with an error instead of replacing it.
    candidate
}

fn exact_directory_entry_exists(path: &Path) -> Result<bool, String> {
    let Some(parent) = path.parent() else {
        return Ok(path_entry_exists(path));
    };
    let Some(file_name) = path.file_name() else {
        return Ok(path_entry_exists(path));
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
        if let Err(e) = rename_no_replace(temp_path, new_path) {
            // A case-insensitive filesystem can briefly still see the old
            // name, which is the new one in another case.
            if attempt + 1 < RENAME_ATTEMPTS && is_case_only_rename(old_path, new_path) {
                thread::sleep(Duration::from_millis(50));
                continue;
            }
            return Err(rename_failure("rename", old_path, new_path, &e));
        }

        for visibility_attempt in 0..VISIBILITY_ATTEMPTS {
            let target_exists = exact_directory_entry_exists(new_path)?;
            let temp_exists = exact_directory_entry_exists(temp_path)?;
            if target_exists && !temp_exists {
                return Ok(());
            }
            if !temp_exists && path_entry_exists(new_path) {
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

/// Renames the files on disk and moves their library rows to the new paths.
///
/// Either everything succeeds or the database is unchanged and the files are
/// put back under their original names; the error says if that failed.
/// No rename replaces an existing file.
///
/// The rows are refreshed from the renamed files as by
/// [`refresh_indexed_metadata_for_paths`], which the bridge then runs on the
/// new paths and which reports the files that cannot be read to the user.
pub(crate) fn rename_indexed_metadata_paths(
    renames: &[(PathBuf, PathBuf)],
) -> Result<TrackRefresh, String> {
    let mut conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    rename_indexed_metadata_paths_in_conn(&mut conn, renames)
}

fn rename_indexed_metadata_paths_in_conn(
    conn: &mut Connection,
    renames: &[(PathBuf, PathBuf)],
) -> Result<TrackRefresh, String> {
    rename_with_journal(
        conn,
        renames,
        DiskRenameJournal::new(renames),
        EmbeddedCoverCache::shared(),
    )
}

fn rename_with_journal(
    conn: &mut Connection,
    renames: &[(PathBuf, PathBuf)],
    mut journal: DiskRenameJournal,
    covers: Option<&EmbeddedCoverCache>,
) -> Result<TrackRefresh, String> {
    // Take the write lock before any file moves. A deferred transaction that
    // reads first cannot wait for the lock when it then writes: SQLite fails
    // it at once with "database is locked" if another writer holds the lock.
    let tx = conn
        .transaction_with_behavior(TransactionBehavior::Immediate)
        .map_err(|e| format!("failed to start rename transaction: {e}"))?;
    let updated = apply_disk_renames(renames, &mut journal)
        .and_then(|()| update_renamed_track_rows(&tx, renames, covers));
    let committed = match updated {
        Ok(refreshed) => tx
            .commit()
            .map(|()| refreshed)
            .map_err(|e| format!("failed to finalize rename transaction: {e}")),
        Err(error) => {
            drop(tx);
            Err(error)
        }
    };
    // A failed transaction has rolled back by now; put the files back to match its rows.
    committed.map_err(|error| journal.restore(error))
}

/// Renames the files on disk, recording each rename in `journal`. Renames
/// whose targets are other files' sources go through temporary names.
fn apply_disk_renames(
    renames: &[(PathBuf, PathBuf)],
    journal: &mut DiskRenameJournal,
) -> Result<(), String> {
    let source_paths = renames
        .iter()
        .map(|(old_path, _)| old_path.clone())
        .collect::<HashSet<_>>();
    let mut claimed_targets = HashSet::with_capacity(renames.len());
    let requires_staged_rename = renames.iter().any(|(old_path, new_path)| {
        old_path != new_path && target_matches_other_source(&source_paths, old_path, new_path)
    });

    if requires_staged_rename {
        let mut staged = Vec::<(usize, PathBuf)>::new();
        for (index, (old_path, new_path)) in renames.iter().enumerate() {
            if old_path == new_path {
                continue;
            }
            if !claimed_targets.insert(new_path.clone()) {
                return Err(format!(
                    "multiple files resolve to the same rename target {}",
                    new_path.to_string_lossy()
                ));
            }
            if !old_path.exists() && path_entry_exists(new_path) {
                continue;
            }
            if path_entry_exists(new_path) && !path_matches_any_source(&source_paths, new_path) {
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    new_path.to_string_lossy()
                ));
            }
            let temp_path = build_temp_rename_path(old_path);
            journal
                .rename(index, old_path, &temp_path)
                .map_err(|e| rename_failure("stage rename", old_path, &temp_path, &e))?;
            staged.push((index, temp_path));
        }

        for (index, temp_path) in staged {
            let (old_path, new_path) = &renames[index];
            if path_entry_exists(new_path) && !path_matches_any_source(&source_paths, new_path) {
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    new_path.to_string_lossy()
                ));
            }
            journal.finalize(index, &temp_path, new_path, old_path)?;
        }
    } else {
        for (index, (old_path, new_path)) in renames.iter().enumerate() {
            if old_path == new_path {
                continue;
            }
            if !claimed_targets.insert(new_path.clone()) {
                return Err(format!(
                    "multiple files resolve to the same rename target {}",
                    new_path.to_string_lossy()
                ));
            }
            if !old_path.exists() && path_entry_exists(new_path) {
                continue;
            }
            if !path_entry_exists(new_path) {
                journal
                    .rename(index, old_path, new_path)
                    .map_err(|e| rename_failure("rename", old_path, new_path, &e))?;
            } else if paths_refer_to_same_file(old_path, new_path) {
                // The new name is another case of the old one on a
                // case-insensitive filesystem.
                journal.rename_through_temp(index, old_path, new_path)?;
            } else {
                return Err(format!(
                    "refusing to overwrite existing file {}",
                    new_path.to_string_lossy()
                ));
            }
        }
    }
    Ok(())
}

struct RenamedTrackRow<'a> {
    new_path: &'a Path,
    old_key: String,
    new_key: String,
    update: RowUpdate,
    /// Whether the file was read fully, so that its cache row may be replaced.
    complete: bool,
    fingerprint: TrackFileFingerprint,
    /// The file's external-track cache row before the rename.
    external: Option<(IndexedTrack, TrackFileFingerprint)>,
}

/// Reads the renamed files, filling `refreshed` for files that keep their
/// path, and notes the rows the index holds for them before any change.
fn read_renamed_track_rows<'a>(
    tx: &Connection,
    renames: &'a [(PathBuf, PathBuf)],
    covers: Option<&EmbeddedCoverCache>,
    refreshed: &mut HashMap<PathBuf, IndexedTrack>,
    failures: &mut ReadFailures,
) -> Result<Vec<RenamedTrackRow<'a>>, String> {
    let mut rows = Vec::with_capacity(renames.len());
    for (old_path, new_path) in renames {
        let read = read_track_catching_panic(new_path, covers);
        failures.note(new_path, &read);
        let complete = read.is_complete();
        let old_key = old_path.to_string_lossy().to_string();
        let update = row_update(read, || stored_track(tx, &old_key))?;
        if old_path == new_path {
            refreshed.insert(new_path.clone(), update.into_indexed());
            continue;
        }
        let fingerprint = track_file_fingerprint(new_path)
            .ok_or_else(|| format!("failed to fingerprint {}", new_path.to_string_lossy()))?;
        let external = external_row(tx, &old_key)?;
        rows.push(RenamedTrackRow {
            new_path,
            old_key,
            new_key: new_path.to_string_lossy().to_string(),
            update,
            complete,
            fingerprint,
            external,
        });
    }
    Ok(rows)
}

/// Moves the `tracks` and `external_tracks` rows of renamed files to their
/// new paths. Rows keep their rowid and the columns a rename does not
/// change. Swaps, renumbering chains and cycles reuse paths that other rows
/// still hold, so those rows move to a placeholder path first.
fn update_renamed_track_rows(
    tx: &Connection,
    renames: &[(PathBuf, PathBuf)],
    covers: Option<&EmbeddedCoverCache>,
) -> Result<TrackRefresh, String> {
    let now = unix_ts_i64();
    let mut refreshed = TrackRefresh::with_capacity(renames.len());
    let mut failures = ReadFailures::default();
    let rows = read_renamed_track_rows(tx, renames, covers, &mut refreshed.indexed, &mut failures)?;
    let new_keys = rows
        .iter()
        .map(|row| row.new_key.clone())
        .collect::<HashSet<_>>();
    let mut current_keys = Vec::with_capacity(rows.len());
    for (index, row) in rows.iter().enumerate() {
        if !new_keys.contains(&row.old_key) {
            current_keys.push(row.old_key.clone());
            continue;
        }
        // Library paths are absolute, so a relative placeholder cannot collide.
        let placeholder = format!("ferrous-rename-pending/{index}/{}", row.old_key);
        tx.execute(
            "UPDATE tracks SET path = ?2 WHERE path = ?1",
            params![row.old_key, placeholder],
        )
        .map_err(|e| format!("failed to move renamed track row aside: {e}"))?;
        current_keys.push(placeholder);
    }
    for row in rows.iter().filter(|row| row.external.is_some()) {
        tx.execute(
            "DELETE FROM external_tracks WHERE path = ?1",
            params![row.old_key],
        )
        .map_err(|e| format!("failed to delete renamed external track row: {e}"))?;
    }

    for (row, current_key) in rows.into_iter().zip(current_keys) {
        let RenamedTrackRow {
            new_path,
            new_key,
            update,
            complete,
            fingerprint,
            external,
            ..
        } = row;
        match &update {
            RowUpdate::Keep(_) => tx.execute(
                "UPDATE tracks SET path = ?2 WHERE path = ?1",
                params![current_key, new_key],
            ),
            RowUpdate::Write {
                indexed,
                cover_checked,
            } => tx.execute(
                r"
                UPDATE tracks
                SET path = ?2,
                    title = ?3,
                    artist = ?4,
                    album = ?5,
                    cover_path = ?6,
                    cover_checked = ?7,
                    genre = ?8,
                    year = ?9,
                    track_no = ?10,
                    duration_secs = ?11,
                    mtime_ns = ?12,
                    size_bytes = ?13,
                    indexed_at = ?14,
                    search_tags = ?15
                WHERE path = ?1
                ",
                params![
                    current_key,
                    new_key,
                    indexed.title.as_str(),
                    indexed.artist.as_str(),
                    indexed.album.as_str(),
                    indexed.cover_path.as_str(),
                    i64::from(*cover_checked),
                    indexed.genre.as_str(),
                    indexed.year.map(i64::from),
                    indexed.track_no.map(i64::from),
                    indexed.duration_secs,
                    fingerprint.mtime_ns,
                    fingerprint.size_bytes,
                    now,
                    serde_json::to_string(&indexed.search_tags).ok(),
                ],
            ),
        }
        .map_err(|e| format!("failed to update renamed track row: {e}"))?;
        let indexed = update.into_indexed();
        // A cache row that the file could not be read for moves as it is.
        match external {
            Some(_) if complete => {
                store_external_track_cache_in_conn(tx, new_path, fingerprint, &indexed)?;
            }
            Some((cached, cached_fingerprint)) => {
                store_external_track_cache_in_conn(tx, new_path, cached_fingerprint, &cached)?;
            }
            None => {}
        }
        refreshed.indexed.insert(new_path.to_path_buf(), indexed);
    }
    refreshed.problem = failures.refresh_problem();
    Ok(refreshed)
}

/// Adds `paths` to the test process's library database the way a scan of
/// `root` would, without registering `root` for rescans.
#[cfg(test)]
pub(crate) fn index_tracks_for_test(root: &Path, paths: &[PathBuf]) {
    let mut conn = open_library_db().expect("test library db");
    init_schema(&conn).expect("test library schema");
    insert_indexed_tracks(&mut conn, root, paths);
}

#[cfg(test)]
fn insert_indexed_tracks(conn: &mut Connection, root: &Path, paths: &[PathBuf]) {
    // Take the write lock first, as the scan's batches do: a transaction
    // that reads before it writes fails at once if another connection
    // committed since its read.
    let tx = conn
        .transaction_with_behavior(TransactionBehavior::Immediate)
        .expect("index transaction");
    {
        let mut writer = TrackWriter::new(&tx).expect("track statements");
        for path in paths {
            let fingerprint = track_file_fingerprint(path).expect("fixture fingerprint");
            let task = MetadataTask {
                path: path.clone(),
                path_string: path.to_string_lossy().to_string(),
                mtime_ns: fingerprint.mtime_ns,
                size_bytes: fingerprint.size_bytes,
            };
            let read = read_track_catching_panic(path, EmbeddedCoverCache::shared());
            apply_metadata_result(
                &mut writer,
                &root.to_string_lossy(),
                unix_ts_i64(),
                &HashMap::new(),
                MetadataResult { task, read },
            )
            .expect("index fixture");
        }
    }
    tx.commit().expect("commit indexed fixtures");
}

/// Resolves a root to its canonical directory path. Errors omit the path;
/// callers add it with their own context.
fn canonicalize_root(root: &Path) -> Result<PathBuf, String> {
    let root = root.canonicalize().map_err(|e| e.to_string())?;
    if !root.is_dir() {
        return Err("not a directory".to_string());
    }
    Ok(root)
}

/// Rejects a root that contains, or lies inside, another configured root.
/// Overlapping roots would claim the same tracks in turn.
fn reject_nested_root(conn: &Connection, root: &Path) -> Result<(), String> {
    for known in load_roots(conn) {
        if known.path == root {
            continue;
        }
        if root.starts_with(&known.path) {
            return Err(format!(
                "cannot add library root '{}': it is inside library root '{}'",
                root.display(),
                known.path.display()
            ));
        }
        if known.path.starts_with(root) {
            return Err(format!(
                "cannot add library root '{}': it contains library root '{}'; remove that root first",
                root.display(),
                known.path.display()
            ));
        }
    }
    Ok(())
}

/// Returns the string that paths strictly below `root` start with. Used with
/// `substr(path, 1, length(?)) = ?`, which unlike `LIKE` is case-sensitive and
/// treats `_` and `%` literally.
fn descendant_prefix(root: &str) -> String {
    if root.ends_with('/') {
        root.to_string()
    } else {
        format!("{root}/")
    }
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

pub(super) fn remove_root_and_purge(conn: &Connection, root: &Path) -> Result<(), String> {
    let root = resolve_root_path(root);
    let root_str = root.to_string_lossy().to_string();

    // Take the write lock up front, like the library's other write
    // transactions, so a concurrent writer is waited for.
    let tx = rusqlite::Transaction::new_unchecked(conn, TransactionBehavior::Immediate)
        .map_err(|e| format!("failed to begin remove transaction: {e}"))?;

    tx.execute("DELETE FROM roots WHERE path=?1", params![root_str.clone()])
        .map_err(|e| format!("failed to remove root '{}': {e}", root.display()))?;

    // A database from before nested roots were rejected can still have the
    // removed root owning tracks inside a configured inner root. Hand those
    // to the innermost such root instead of deleting them; its next scan
    // keeps them in step with the folder.
    let mut inner_roots = tx
        .prepare("SELECT path FROM roots WHERE substr(path, 1, length(?1)) = ?1")
        .and_then(|mut stmt| {
            stmt.query_map(params![descendant_prefix(&root_str)], |row| {
                row.get::<_, String>(0)
            })?
            .collect::<Result<Vec<_>, _>>()
        })
        .map_err(|e| format!("failed to read library roots: {e}"))?;
    inner_roots.sort_by_key(|path| std::cmp::Reverse(path.len()));
    for inner_str in inner_roots {
        tx.execute(
            r"
            UPDATE tracks SET root_path = ?1
            WHERE (path = ?1 OR substr(path, 1, length(?2)) = ?2)
              AND NOT EXISTS (SELECT 1 FROM roots WHERE roots.path = tracks.root_path)
            ",
            params![inner_str, descendant_prefix(&inner_str)],
        )
        .map_err(|e| format!("failed to move tracks to library root '{inner_str}': {e}"))?;
    }

    // Tracks under the root's path that another configured root owns stay;
    // path matching only collects legacy rows without a valid root.
    tx.execute(
        r"
        DELETE FROM tracks
        WHERE root_path = ?1
           OR ((path = ?1 OR substr(path, 1, length(?2)) = ?2)
               AND NOT EXISTS (SELECT 1 FROM roots WHERE roots.path = tracks.root_path))
        ",
        params![root_str, descendant_prefix(&root_str)],
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

#[derive(Debug)]
struct MetadataResult {
    task: MetadataTask,
    read: TrackRead,
}

/// Read the file of `task` for the index; see [`read_track_catching_panic`].
fn read_scan_task(task: MetadataTask, covers: Option<&EmbeddedCoverCache>) -> MetadataResult {
    let read = read_track_catching_panic(&task.path, covers);
    MetadataResult { task, read }
}

/// How many files failed one way, and the first of them.
#[derive(Default)]
struct FileFailures {
    count: usize,
    first: Option<(PathBuf, String)>,
}

impl FileFailures {
    fn note(&mut self, path: &Path, failure: Option<&String>) {
        let Some(failure) = failure else {
            return;
        };
        self.count += 1;
        self.first
            .get_or_insert_with(|| (path.to_path_buf(), failure.clone()));
    }

    /// `summary` of the failed files, given their count, then the first of
    /// them and why it failed.
    fn describe(&self, summary: impl FnOnce(&str) -> String) -> Option<String> {
        let (path, failure) = self.first.as_ref()?;
        Some(format!(
            "{} (first: '{}': {failure})",
            summary(&count_label(self.count, "file", "files")),
            path.display()
        ))
    }
}

/// The files of a scan or a refresh that could not be read fully.
#[derive(Default)]
struct ReadFailures {
    tags: FileFailures,
    covers: FileFailures,
}

impl ReadFailures {
    fn note(&mut self, path: &Path, read: &TrackRead) {
        self.tags.note(path, read.failure.as_ref());
        self.covers.note(path, read.cover_failure.as_ref());
    }

    /// The failures of a scan of `root`.
    fn problems(&self, root: &Path) -> impl Iterator<Item = String> {
        let root = root.display();
        let tags = self.tags.describe(|files| {
            format!(
                "could not read the tags of {files} in '{root}'; kept the tags indexed before, \
                 or indexed new files by file name"
            )
        });
        let covers = self.covers.describe(|files| {
            format!(
                "could not cache the embedded covers of {files} in '{root}'; kept their previous \
                 covers and will retry at the next scan"
            )
        });
        tags.into_iter().chain(covers)
    }

    /// The failures of a refresh of chosen files, for the user.
    fn refresh_problem(&self) -> Option<String> {
        let tags = self.tags.describe(|files| {
            format!("could not read the tags of {files}; kept the tags indexed before")
        });
        let covers = self.covers.describe(|files| {
            format!(
                "could not cache the embedded covers of {files}; kept their previous covers \
                 and will retry at the next scan"
            )
        });
        let problems: Vec<String> = tags.into_iter().chain(covers).collect();
        (!problems.is_empty()).then(|| problems.join("; "))
    }
}

/// Make scans of chosen files panic or wait, count scans, and tune batches.
#[cfg(test)]
pub(crate) mod scan_test_hooks {
    use std::cell::Cell;
    use std::collections::HashMap;
    use std::path::{Path, PathBuf};
    use std::sync::{Mutex, PoisonError};
    use std::time::Duration;

    use crossbeam_channel::{bounded, Receiver, Sender};

    use super::ScanTuning;

    static PANIC_ON_READ: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
    /// A held read: the file, the sender told when the read waits, and the
    /// receiver that releases it.
    type ReadGate = (PathBuf, Sender<()>, Receiver<()>);

    static READ_GATES: Mutex<Vec<ReadGate>> = Mutex::new(Vec::new());
    static SCANS: Mutex<Option<HashMap<PathBuf, usize>>> = Mutex::new(None);

    thread_local! {
        static TUNING: Cell<Option<ScanTuning>> = const { Cell::new(None) };
    }

    /// Make every scan read of `path` panic.
    pub(crate) fn panic_on_read(path: &Path) {
        PANIC_ON_READ
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push(path.to_path_buf());
    }

    /// Make the next scan read of `path` wait until the returned sender
    /// sends or is dropped. The first receiver gets a message when the read
    /// starts waiting.
    pub(crate) fn hold_read(path: &Path) -> (Receiver<()>, Sender<()>) {
        let (started_tx, started_rx) = bounded(1);
        let (release_tx, release_rx) = bounded(1);
        READ_GATES
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push((path.to_path_buf(), started_tx, release_rx));
        (started_rx, release_tx)
    }

    /// Scans on this thread use `tuning`.
    pub(crate) fn set_tuning(tuning: ScanTuning) {
        TUNING.with(|cell| cell.set(Some(tuning)));
    }

    pub(super) fn tuning() -> Option<ScanTuning> {
        TUNING.with(Cell::get)
    }

    /// How many scans of `root` started.
    pub(crate) fn scans_of(root: &Path) -> usize {
        SCANS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .as_ref()
            .and_then(|scans| scans.get(root).copied())
            .unwrap_or(0)
    }

    pub(super) fn note_scan(root: &Path) {
        *SCANS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get_or_insert_with(HashMap::new)
            .entry(root.to_path_buf())
            .or_default() += 1;
    }

    pub(super) fn before_read(path: &Path) {
        let panics = PANIC_ON_READ
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .iter()
            .any(|chosen| chosen == path);
        assert!(!panics, "test panic while reading {}", path.display());
        let gate = {
            let mut gates = READ_GATES.lock().unwrap_or_else(PoisonError::into_inner);
            gates
                .iter()
                .position(|(chosen, _, _)| chosen == path)
                .map(|index| gates.remove(index))
        };
        if let Some((_, started, release)) = gate {
            let _ = started.send(());
            // A test that fails before it releases the read must not hang.
            let _ = release.recv_timeout(Duration::from_secs(30));
        }
    }
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

pub(super) fn scan_snapshot_emit_interval() -> Duration {
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
    if let Ok(mut stmt) = conn.prepare(&format!(
        "SELECT {EXISTING_TRACK_COLUMNS} FROM tracks WHERE {ROOT_TRACKS_FILTER}"
    )) {
        let mapped = stmt.query_map(
            params![root_str, descendant_prefix(root_str)],
            existing_track_row,
        );
        if let Ok(rows) = mapped {
            existing.extend(rows.flatten().map(existing_track_state));
        }
    }
    existing
}

/// The rows of `root_str` at the paths of `scope` and below them.
fn load_existing_tracks_in_scope(
    conn: &Connection,
    root_str: &str,
    scope: &[PathBuf],
) -> Result<HashMap<String, ExistingTrackScanState>, String> {
    // A range of the primary key: the paths that start with the scope
    // path, followed by a byte below '0', the byte after '/'. The rows of
    // siblings that the range also has, such as `a b` for `a`, are dropped
    // below.
    let mut stmt = conn
        .prepare_cached(&format!(
            "SELECT {EXISTING_TRACK_COLUMNS} FROM tracks \
             WHERE path >= ?3 AND path < ?4 AND ({ROOT_TRACKS_FILTER})"
        ))
        .map_err(|e| format!("failed to read the indexed tracks: {e}"))?;
    let root_prefix = descendant_prefix(root_str);
    let mut existing = HashMap::new();
    for path in scope {
        let path_str = path.to_string_lossy();
        let below = descendant_prefix(&path_str);
        let rows = stmt
            .query_map(
                params![root_str, root_prefix, path_str, format!("{path_str}0")],
                existing_track_row,
            )
            .and_then(Iterator::collect::<Result<Vec<_>, _>>)
            .map_err(|e| format!("failed to read the indexed tracks: {e}"))?;
        existing.extend(
            rows.into_iter()
                .filter(|row| row.0 == path_str || row.0.starts_with(&below))
                .map(existing_track_state),
        );
    }
    Ok(existing)
}

const EXISTING_TRACK_COLUMNS: &str = "path, title, track_no, mtime_ns, size_bytes, \
    cover_checked, duration_secs, search_tags IS NULL, cover_path";

/// The rows of root `?1`, whose descendants start with `?2`, including rows
/// from before tracks had a root.
const ROOT_TRACKS_FILTER: &str = "root_path = ?1 \
    OR (root_path = '' AND (path = ?1 OR substr(path, 1, length(?2)) = ?2))";

type ExistingTrackRow = (
    String,
    String,
    Option<i64>,
    i64,
    i64,
    i64,
    Option<f64>,
    bool,
    String,
);

fn existing_track_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<ExistingTrackRow> {
    Ok((
        row.get(0)?,
        row.get(1)?,
        row.get(2)?,
        row.get(3)?,
        row.get(4)?,
        row.get(5)?,
        row.get(6)?,
        row.get(7)?,
        row.get(8)?,
    ))
}

fn existing_track_state(item: ExistingTrackRow) -> (String, ExistingTrackScanState) {
    let path = Path::new(&item.0);
    let file_stem = path
        .file_stem()
        .map_or_else(String::new, |name| name.to_string_lossy().into_owned());
    let filename_fallback = !item.1.trim().is_empty() && item.1 == file_stem;
    let missing_duration = item.6.is_none() || item.6.is_some_and(|d| d <= 0.0);
    let suspicious_metadata = item.7
        || item.1.trim().is_empty()
        || (item.2.is_none() && filename_fallback && leading_track_number(&file_stem).is_some())
        || (missing_duration && is_raw_surround_file(path));
    let state = ExistingTrackScanState {
        row: RowState::new(item.3, item.4, &item.8),
        has_cover_path: item.5 != 0,
        suspicious_metadata,
    };
    (item.0, state)
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
            indexed_at,
            search_tags
        )
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)
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
            indexed_at=excluded.indexed_at,
            search_tags=excluded.search_tags
        ",
    )
    .map_err(|e| format!("failed to prepare track upsert statement: {e}"))
}

/// The statements a scan writes track rows with.
struct TrackWriter<'conn> {
    upsert: rusqlite::CachedStatement<'conn>,
    stored: rusqlite::CachedStatement<'conn>,
    state: rusqlite::CachedStatement<'conn>,
}

impl<'conn> TrackWriter<'conn> {
    fn new(tx: &'conn rusqlite::Transaction<'conn>) -> Result<Self, String> {
        Ok(Self {
            upsert: prepare_track_upsert_statement(tx)?,
            stored: prepare_library_row_statement(tx)?,
            state: tx
                .prepare_cached(
                    "SELECT mtime_ns, size_bytes, cover_path FROM tracks WHERE path = ?1",
                )
                .map_err(|e| format!("failed to prepare track state statement: {e}"))?,
        })
    }

    /// The row the index has for the file of `task`, whichever root indexed it.
    fn stored(&mut self, task: &MetadataTask) -> Result<Option<IndexedTrack>, String> {
        query_library_row(&mut self.stored, &task.path_string)
    }

    /// Whether the read of `task` may still be written. The scan read the
    /// file without a lock, so meanwhile a rename may have moved it, or an
    /// edit changed it and refreshed its row, or a cover refresh written its
    /// row; their rows are newer than the read. The batch holds the write
    /// lock now, so none of them can happen until it commits.
    ///
    /// `seen` is the row the scan started with.
    fn still_current(
        &mut self,
        task: &MetadataTask,
        seen: Option<&ExistingTrackScanState>,
    ) -> Result<bool, String> {
        let Some(on_disk) = track_file_fingerprint(&task.path) else {
            // Moved or deleted: a rename has its row, or the next scan
            // removes it.
            return Ok(false);
        };
        let row = self
            .state
            .query_row(params![task.path_string], |row| {
                Ok(RowState::new(
                    row.get(0)?,
                    row.get(1)?,
                    &row.get::<_, String>(2)?,
                ))
            })
            .optional()
            .map_err(|e| {
                format!(
                    "failed to read the indexed row of '{}': {e}",
                    task.path.display()
                )
            })?;
        let Some(row) = row else {
            // A new file. If it changed since it was read, its row keeps the
            // older state, so the next scan reads it again.
            return Ok(true);
        };
        let read_state = TrackFileFingerprint {
            mtime_ns: task.mtime_ns,
            size_bytes: task.size_bytes,
        };
        Ok(on_disk == read_state && seen.is_none_or(|seen| seen.row == row))
    }
}

/// Writes the row of `result` unless it is stale (see
/// [`TrackWriter::still_current`]) or the read failed and the row stays as
/// it is (see [`row_update`]). Returns the row written.
fn apply_metadata_result(
    writer: &mut TrackWriter<'_>,
    root_str: &str,
    now: i64,
    existing: &HashMap<String, ExistingTrackScanState>,
    result: MetadataResult,
) -> Result<Option<(MetadataTask, IndexedTrack)>, String> {
    let MetadataResult { task, read } = result;
    if !writer.still_current(&task, existing.get(&task.path_string))? {
        return Ok(None);
    }
    // The row may belong to another root that this one reclaims, so it is
    // looked up by path.
    let RowUpdate::Write {
        indexed,
        cover_checked,
    } = row_update(read, || writer.stored(&task))?
    else {
        return Ok(None);
    };
    writer
        .upsert
        .execute(params![
            task.path_string.as_str(),
            root_str,
            indexed.title.as_str(),
            indexed.artist.as_str(),
            indexed.album.as_str(),
            indexed.cover_path.as_str(),
            i64::from(cover_checked),
            indexed.genre.as_str(),
            indexed.year.map(i64::from),
            indexed.track_no.map(i64::from),
            indexed.duration_secs,
            task.mtime_ns,
            task.size_bytes,
            now,
            serde_json::to_string(&indexed.search_tags).ok(),
        ])
        .map_err(|e| format!("failed to index '{}': {e}", task.path.display()))?;
    Ok(Some((task, indexed)))
}

/// Identity of a file independent of the path that reached it.
#[cfg(unix)]
type FileIdentity = (u64, u64);
#[cfg(not(unix))]
type FileIdentity = PathBuf;

#[cfg(unix)]
fn file_identity(_path: &Path, metadata: &fs::Metadata) -> FileIdentity {
    (metadata.dev(), metadata.ino())
}

#[cfg(not(unix))]
fn file_identity(path: &Path, _metadata: &fs::Metadata) -> FileIdentity {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

/// A supported audio file found by [`LibraryWalk`].
struct ScanCandidate {
    path: PathBuf,
    mtime_ns: i64,
    size_bytes: i64,
}

/// A directory on the walk's current path.
struct WalkAncestor {
    path: PathBuf,
    via_link: bool,
}

enum WalkStep {
    Continue,
    SkipDir,
    Yield(ScanCandidate),
}

/// Walks a library root, following symbolic links, and yields each supported
/// audio file once.
///
/// Files reached only through a link are yielded after the walk, and only
/// when no path without links reached the same file, so the indexed path
/// stays the same across scans. Paths that could not be read are recorded in
/// `kept`: their tracks may still exist and must not be treated as deleted.
///
/// A scoped walk covers only some files and folders of the root. It cannot
/// tell which of the paths that reach a file through links, or through
/// hard links, a walk of the whole root indexes, so it keeps such paths and
/// sets `needs_full_scan` instead.
struct LibraryWalk {
    walk: Option<walkdir::IntoIter>,
    root: PathBuf,
    /// The paths a scoped walk has yet to walk.
    starts: VecDeque<PathBuf>,
    /// The path the current walk started at.
    start: PathBuf,
    scoped: bool,
    needs_full_scan: bool,
    nested_roots: HashSet<PathBuf>,
    ancestors: Vec<WalkAncestor>,
    claimed: HashSet<FileIdentity>,
    linked: VecDeque<(ScanCandidate, FileIdentity)>,
    /// Supported audio files encountered, including unreadable and duplicate ones.
    found: usize,
    kept: HashSet<PathBuf>,
    /// The files found that have other names; see
    /// [`WorkerLibrary::hard_linked`].
    hard_linked: HashSet<PathBuf>,
    read_errors: usize,
    first_read_error: Option<String>,
}

impl LibraryWalk {
    fn new(root: &Path, nested_roots: HashSet<PathBuf>) -> Self {
        Self {
            walk: Some(walk_from(root)),
            root: root.to_path_buf(),
            starts: VecDeque::new(),
            start: root.to_path_buf(),
            scoped: false,
            needs_full_scan: false,
            nested_roots,
            ancestors: Vec::new(),
            claimed: HashSet::new(),
            linked: VecDeque::new(),
            found: 0,
            kept: HashSet::new(),
            hard_linked: HashSet::new(),
            read_errors: 0,
            first_read_error: None,
        }
    }

    /// A walk of the files and folders `scope` of `root`.
    fn scoped(root: &Path, scope: &[PathBuf], nested_roots: HashSet<PathBuf>) -> Self {
        Self {
            walk: None,
            starts: scope.iter().cloned().collect(),
            scoped: true,
            ..Self::new(root, nested_roots)
        }
    }

    /// Starts the walk of the next path of a scoped walk. Returns false when
    /// none is left.
    fn next_walk(&mut self) -> bool {
        self.walk = None;
        while let Some(start) = self.starts.pop_front() {
            if self
                .nested_roots
                .iter()
                .any(|nested| start.starts_with(nested))
            {
                self.kept.insert(start);
                continue;
            }
            if !self.reached_without_links(&start) {
                self.needs_full_scan = true;
                self.kept.insert(start);
                continue;
            }
            self.ancestors.clear();
            self.walk = Some(walk_from(&start));
            self.start = start;
            return true;
        }
        false
    }

    /// Whether `path`, below the canonical root, is reached without links:
    /// it is its own canonical path, or, once it is gone, the closest folder
    /// above it that exists is.
    fn reached_without_links(&self, path: &Path) -> bool {
        let mut path = path;
        loop {
            match path.canonicalize() {
                Ok(canonical) => return canonical == path,
                Err(_) if path != self.root => match path.parent() {
                    Some(parent) => path = parent,
                    None => return false,
                },
                Err(_) => return false,
            }
        }
    }

    /// Keeps the rows at `path` for a scan of the whole root to resolve.
    fn defer_to_full_scan(&mut self, path: &Path) {
        self.needs_full_scan = true;
        self.kept.insert(path.to_path_buf());
    }

    fn visit(&mut self, entry: &walkdir::DirEntry) -> WalkStep {
        let depth = entry.depth();
        if self.scoped && entry.path_is_symlink() {
            self.defer_to_full_scan(entry.path());
            return if entry.file_type().is_dir() {
                WalkStep::SkipDir
            } else {
                WalkStep::Continue
            };
        }
        let via_link = entry.path_is_symlink()
            || depth
                .checked_sub(1)
                .and_then(|parent| self.ancestors.get(parent))
                .is_some_and(|parent| parent.via_link);
        let path = entry.path();
        if entry.file_type().is_dir() {
            if depth > 0 && self.nested_roots.contains(path) {
                // Another root owns this folder (configured before nested
                // roots were rejected); keep its tracks out of this scan.
                self.kept.insert(path.to_path_buf());
                return WalkStep::SkipDir;
            }
            self.ancestors.truncate(depth);
            self.ancestors.push(WalkAncestor {
                path: path.to_path_buf(),
                via_link,
            });
            return WalkStep::Continue;
        }
        if !entry.file_type().is_file() || !is_audio_track_file(path) {
            return WalkStep::Continue;
        }

        self.found = self.found.saturating_add(1);
        let metadata = match fs::metadata(path) {
            Ok(metadata) => metadata,
            Err(err) => {
                self.record_unreadable(path.to_path_buf(), &err);
                return WalkStep::Continue;
            }
        };
        let candidate = ScanCandidate {
            path: path.to_path_buf(),
            mtime_ns: metadata
                .modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map_or(0, |duration| u128_to_i64(duration.as_nanos())),
            size_bytes: u64_to_i64(metadata.len()),
        };
        #[cfg(unix)]
        if metadata.nlink() > 1 {
            if self.scoped {
                self.defer_to_full_scan(path);
                return WalkStep::Continue;
            }
            self.hard_linked.insert(path.to_path_buf());
        }
        let identity = file_identity(path, &metadata);
        if via_link {
            self.linked.push_back((candidate, identity));
            return WalkStep::Continue;
        }
        if self.claim(identity) {
            WalkStep::Yield(candidate)
        } else {
            WalkStep::Continue
        }
    }

    /// Returns whether no earlier path claimed this file.
    fn claim(&mut self, identity: FileIdentity) -> bool {
        self.claimed.insert(identity)
    }

    fn record_walk_error(&mut self, err: &walkdir::Error) {
        if err.loop_ancestor().is_some() {
            // A link back to an ancestor: the walk already covers its files.
            return;
        }
        if self.scoped
            && err.depth() == 0
            && err
                .io_error()
                .is_some_and(|io_err| io_err.kind() == std::io::ErrorKind::NotFound)
        {
            // A changed path that is gone: its tracks were deleted.
            return;
        }
        let path = err.path().map_or_else(
            || {
                // Without a path, the error came from reading the directory
                // one level above the failing entry.
                err.depth()
                    .checked_sub(1)
                    .and_then(|parent| self.ancestors.get(parent))
                    .map_or_else(|| self.start.clone(), |parent| parent.path.clone())
            },
            Path::to_path_buf,
        );
        match err.io_error() {
            Some(io_err) => self.record_unreadable(path, io_err),
            None => self.record_unreadable(path, err),
        }
    }

    fn record_unreadable(&mut self, path: PathBuf, err: &dyn std::fmt::Display) {
        self.read_errors = self.read_errors.saturating_add(1);
        if self.first_read_error.is_none() {
            self.first_read_error = Some(format!("'{}': {err}", path.display()));
        }
        self.kept.insert(path);
    }

    /// Returns whether the walk could not verify `path` because it, or a
    /// folder above it, could not be read or belongs to another root.
    fn keeps(&self, path: &Path) -> bool {
        path.ancestors()
            .any(|ancestor| self.kept.contains(ancestor))
    }
}

impl Iterator for LibraryWalk {
    type Item = ScanCandidate;

    fn next(&mut self) -> Option<ScanCandidate> {
        loop {
            let Some(entry) = self.walk.as_mut().and_then(Iterator::next) else {
                if self.next_walk() {
                    continue;
                }
                break;
            };
            match entry {
                Ok(entry) => match self.visit(&entry) {
                    WalkStep::Continue => {}
                    WalkStep::SkipDir => {
                        if let Some(walk) = self.walk.as_mut() {
                            walk.skip_current_dir();
                        }
                    }
                    WalkStep::Yield(candidate) => return Some(candidate),
                },
                Err(err) => self.record_walk_error(&err),
            }
        }
        while let Some((candidate, identity)) = self.linked.pop_front() {
            if self.claim(identity) {
                return Some(candidate);
            }
        }
        None
    }
}

fn walk_from(path: &Path) -> walkdir::IntoIter {
    WalkDir::new(path)
        .follow_links(true)
        .sort_by_file_name()
        .into_iter()
}

/// Whether `root` looks like a folder whose drive or share is not mounted:
/// it is gone or empty.
fn root_looks_offline(root: &Path) -> bool {
    fs::read_dir(root).map_or(true, |mut entries| entries.next().is_none())
}

/// Result of scanning one root.
struct RootScanOutcome {
    /// Tracks removed from the index because their files are gone.
    removed: Vec<String>,
    /// A condition that kept the scan from verifying tracks, for the user.
    problem: Option<String>,
    /// Whether a scoped scan kept paths that only a scan of the whole root
    /// can index correctly; see [`LibraryWalk`].
    needs_full_scan: bool,
    /// After a complete scan of the whole root, the files found that have
    /// other names.
    hard_linked: Option<HashSet<PathBuf>>,
}

pub(super) fn count_label(count: usize, singular: &str, plural: &str) -> String {
    if count == 1 {
        format!("1 {singular}")
    } else {
        format!("{count} {plural}")
    }
}

/// How a scan batches its writes and reads its files.
#[derive(Debug, Clone, Copy)]
pub(super) struct ScanTuning {
    /// The most rows one transaction writes.
    pub(super) batch_rows: usize,
    /// The longest a read row waits for its batch to be written.
    pub(super) batch_interval: Duration,
    /// Threads reading files; one reads them on the scan's own thread.
    pub(super) workers: usize,
}

impl ScanTuning {
    fn current() -> Self {
        #[cfg(test)]
        if let Some(tuning) = scan_test_hooks::tuning() {
            return tuning;
        }
        Self {
            batch_rows: 256,
            batch_interval: Duration::from_secs(1),
            workers: scan_worker_count(),
        }
    }
}

/// How long a scan waits for a file read before it checks for commands.
const SCAN_POLL_INTERVAL: Duration = Duration::from_millis(50);

/// The state of a scan of one root between files.
struct RootScan<'a> {
    conn: &'a Connection,
    root: &'a Path,
    root_str: &'a str,
    /// The rows of the root when the scan started.
    existing: &'a HashMap<String, ExistingTrackScanState>,
    now: i64,
    tuning: ScanTuning,
    /// Rows read and not yet written.
    batch: Vec<MetadataResult>,
    batch_started: Option<Instant>,
    discovered: usize,
    processed: usize,
    progress: ScanProgressReporter,
    read_failures: ReadFailures,
}

impl RootScan<'_> {
    fn report(&mut self, sink: &mut dyn ScanSink, force: bool) {
        if let Some(progress) = self.progress.emit(force, self.discovered, self.processed) {
            sink.progress(self.conn, progress);
        }
    }

    /// Counts a file whose row is up to date.
    fn skip_unchanged(&mut self, sink: &mut dyn ScanSink) {
        self.processed = self.processed.saturating_add(1);
        self.report(sink, false);
    }

    /// Adds a file read to the batch, and writes the batch if it is due.
    fn accept(&mut self, result: MetadataResult, sink: &mut dyn ScanSink) -> Result<(), String> {
        self.read_failures.note(&result.task.path, &result.read);
        self.batch_started.get_or_insert_with(Instant::now);
        self.batch.push(result);
        self.processed = self.processed.saturating_add(1);
        if self.batch.len() >= self.tuning.batch_rows {
            self.flush(sink)?;
        } else {
            self.flush_if_due(sink)?;
        }
        self.report(sink, false);
        Ok(())
    }

    fn flush_if_due(&mut self, sink: &mut dyn ScanSink) -> Result<(), String> {
        if self
            .batch_started
            .is_some_and(|started| started.elapsed() >= self.tuning.batch_interval)
        {
            self.flush(sink)?;
        }
        Ok(())
    }

    /// Writes the batch in one transaction and puts its rows in the snapshot.
    fn flush(&mut self, sink: &mut dyn ScanSink) -> Result<(), String> {
        self.batch_started = None;
        if self.batch.is_empty() {
            return Ok(());
        }
        let results = std::mem::take(&mut self.batch);
        let written = write_batch(self.conn, self.root_str, self.now, self.existing, results)?;
        sink.committed(self.root, written);
        Ok(())
    }
}

/// Writes the rows of `results` in one transaction, and returns those
/// written; see [`apply_metadata_result`].
fn write_batch(
    conn: &Connection,
    root_str: &str,
    now: i64,
    existing: &HashMap<String, ExistingTrackScanState>,
    results: Vec<MetadataResult>,
) -> Result<Vec<(MetadataTask, IndexedTrack)>, String> {
    // Take the write lock up front: a deferred transaction that reads before
    // it writes cannot wait for another writer, and fails at once.
    let tx = rusqlite::Transaction::new_unchecked(conn, TransactionBehavior::Immediate)
        .map_err(|e| format!("failed to begin a scan batch: {e}"))?;
    let mut written = Vec::with_capacity(results.len());
    {
        let mut writer = TrackWriter::new(&tx)?;
        for result in results {
            written.extend(apply_metadata_result(
                &mut writer,
                root_str,
                now,
                existing,
                result,
            )?);
        }
    }
    tx.commit()
        .map_err(|e| format!("failed to commit a scan batch: {e}"))?;
    Ok(written)
}

/// Removes the rows of files that a complete walk did not find. A row that
/// another connection wrote since the scan started, such as a rename's,
/// stays.
fn delete_stale_rows(
    conn: &Connection,
    stale: Vec<String>,
    existing: &HashMap<String, ExistingTrackScanState>,
) -> Result<Vec<String>, String> {
    if stale.is_empty() {
        return Ok(stale);
    }
    let tx = rusqlite::Transaction::new_unchecked(conn, TransactionBehavior::Immediate)
        .map_err(|e| format!("failed to begin removing deleted tracks: {e}"))?;
    let mut removed = Vec::with_capacity(stale.len());
    for path in stale {
        let Some(seen) = existing.get(&path) else {
            continue;
        };
        let deleted = tx
            .execute(
                "DELETE FROM tracks WHERE path=?1 AND mtime_ns=?2 AND size_bytes=?3",
                params![path, seen.row.mtime_ns, seen.row.size_bytes],
            )
            .map_err(|e| format!("failed to remove deleted track '{path}' from the index: {e}"))?;
        if deleted > 0 {
            removed.push(path);
        }
    }
    tx.commit()
        .map_err(|e| format!("failed to finalize removing deleted tracks: {e}"))?;
    Ok(removed)
}

/// Scans one root; see [`scan_root_in`].
#[cfg(test)]
fn scan_root(
    conn: &Connection,
    root: &Path,
    covers: Option<&'static EmbeddedCoverCache>,
    sink: &mut dyn ScanSink,
) -> Result<RootScanOutcome, String> {
    scan_root_in(conn, root, None, covers, sink)
}

/// Scans one root, or the files and folders `scope` of it: walks it, reads
/// the files that changed on worker threads, and writes their rows in
/// batches of short transactions.
///
/// Other connections can write between batches, and a scan that stops early
/// or fails keeps every batch it committed. Rows of files that are gone are
/// removed only after a complete walk. A scan that `sink` stops returns an
/// outcome with nothing removed and no problem.
#[allow(clippy::too_many_lines)]
fn scan_root_in(
    conn: &Connection,
    root: &Path,
    scope: Option<&[PathBuf]>,
    covers: Option<&'static EmbeddedCoverCache>,
    sink: &mut dyn ScanSink,
) -> Result<RootScanOutcome, String> {
    let root = canonicalize_root(root)?;
    insert_root(conn, &root, "")?;
    #[cfg(test)]
    scan_test_hooks::note_scan(&root);

    let root_str = root.to_string_lossy().to_string();
    let existing = match scope {
        Some(scope) => load_existing_tracks_in_scope(conn, &root_str, scope)?,
        None => load_existing_tracks_for_root(conn, &root_str),
    };
    let previous_index_count = existing.len();
    let nested_roots = load_roots(conn)
        .into_iter()
        .map(|known| known.path)
        .filter(|path| path != &root && path.starts_with(&root))
        .collect();
    let mut seen_paths: HashSet<String> = HashSet::new();
    let tuning = ScanTuning::current();
    let mut scan = RootScan {
        conn,
        root: &root,
        root_str: &root_str,
        existing: &existing,
        now: unix_ts_i64(),
        tuning,
        batch: Vec::new(),
        batch_started: None,
        discovered: 0,
        processed: 0,
        progress: ScanProgressReporter::new(previous_index_count),
        read_failures: ReadFailures::default(),
    };
    scan.report(sink, true);

    let mut workers = MetadataWorkerPool::new(tuning.workers, covers);
    let mut walk = match scope {
        Some(scope) => LibraryWalk::scoped(&root, scope, nested_roots),
        None => LibraryWalk::new(&root, nested_roots),
    };

    let complete = 'scan: {
        for ScanCandidate {
            path,
            mtime_ns,
            size_bytes,
        } in walk.by_ref()
        {
            if sink.should_stop(conn) {
                break 'scan false;
            }
            scan.discovered = scan.discovered.saturating_add(1);

            let path_string = path.to_string_lossy().to_string();
            seen_paths.insert(path_string.clone());

            let existing_state = existing.get(&path_string);
            let needs_update = existing_state.is_none_or(|existing_state| {
                existing_state.row.mtime_ns != mtime_ns
                    || existing_state.row.size_bytes != size_bytes
                    || !existing_state.has_cover_path
                    || existing_state.suspicious_metadata
            });

            if needs_update {
                let task = MetadataTask {
                    path,
                    path_string,
                    mtime_ns,
                    size_bytes,
                };
                if let Err(task) = workers.submit(task) {
                    scan.accept(read_scan_task(task, covers), sink)?;
                }
            } else {
                scan.skip_unchanged(sink);
            }

            for result in workers.drain_ready()? {
                scan.accept(result, sink)?;
            }
            while workers.at_capacity() {
                if sink.should_stop(conn) {
                    break 'scan false;
                }
                match workers.next_result(SCAN_POLL_INTERVAL)? {
                    Some(result) => scan.accept(result, sink)?,
                    None => scan.flush_if_due(sink)?,
                }
            }
            scan.flush_if_due(sink)?;
        }

        workers.close_tasks();
        while workers.has_pending() {
            if sink.should_stop(conn) {
                break 'scan false;
            }
            match workers.next_result(SCAN_POLL_INTERVAL)? {
                Some(result) => scan.accept(result, sink)?,
                None => scan.flush_if_due(sink)?,
            }
        }
        workers.join();
        scan.flush(sink)?;
        true
    };
    if !complete {
        // The rows read since the last batch are dropped with the workers'
        // remaining reads; the next scan reads those files again.
        return Ok(RootScanOutcome {
            removed: Vec::new(),
            problem: None,
            needs_full_scan: false,
            hard_linked: None,
        });
    }

    // A root that previously had tracks but now has no audio files at all is
    // most likely an unmounted or offline folder, not an emptied library. A
    // scoped walk does not see the whole root, so it looks at the folder.
    let keep_all = walk.found == 0
        && previous_index_count > 0
        && (scope.is_none() || root_looks_offline(&root));
    let stale: Vec<String> = if keep_all {
        Vec::new()
    } else {
        existing
            .keys()
            .filter(|p| !seen_paths.contains(*p) && !walk.keeps(Path::new(p)))
            .cloned()
            .collect()
    };
    let removed = delete_stale_rows(conn, stale, &existing)?;

    scan.report(sink, true);

    let walk_problem = match walk.first_read_error {
        Some(first_error) => Some(format!(
            "could not read {} in '{}'; kept the tracks indexed there ({first_error})",
            count_label(walk.read_errors, "folder or file", "folders or files"),
            root.display()
        )),
        None if keep_all => Some(format!(
            "found no audio files in '{}'; kept its {} in case the folder is offline. \
             Remove the root to drop them",
            root.display(),
            count_label(previous_index_count, "indexed track", "indexed tracks")
        )),
        None => None,
    };
    let problems: Vec<String> = walk_problem
        .into_iter()
        .chain(scan.read_failures.problems(&root))
        .collect();
    Ok(RootScanOutcome {
        removed,
        problem: (!problems.is_empty()).then(|| problems.join("; ")),
        needs_full_scan: walk.needs_full_scan,
        hard_linked: scope.is_none().then_some(walk.hard_linked),
    })
}

/// Whether `path` is a track: a file with a supported audio extension that
/// is not an `AppleDouble` sidecar. Track discovery (library walks, imports,
/// album membership) uses this.
///
/// It reads files named `._*`, so call it only from worker threads; the
/// bridge and UI threads use [`is_supported_audio`].
pub(crate) fn is_audio_track_file(path: &Path) -> bool {
    is_supported_audio(path) && !is_apple_double_sidecar(path)
}

/// Whether `path` is an `AppleDouble` file, which macOS writes as `._<name>`
/// next to `<name>` to keep its metadata on file systems that cannot. It
/// holds no audio, though it has the audio file's extension.
///
/// Only files with such a name are read, to check for the `AppleDouble` magic
/// number, so a track that merely starts with `._` is still a track.
fn is_apple_double_sidecar(path: &Path) -> bool {
    const APPLE_DOUBLE_MAGIC: [u8; 4] = 0x0005_1607_u32.to_be_bytes();
    let has_sidecar_name = path
        .file_name()
        .is_some_and(|name| name.as_encoded_bytes().starts_with(b"._"));
    if !has_sidecar_name {
        return false;
    }
    let mut magic = [0; 4];
    fs::File::open(path)
        .and_then(|mut file| file.read_exact(&mut magic))
        .is_ok_and(|()| magic == APPLE_DOUBLE_MAGIC)
}

/// An `AppleDouble` file as macOS writes one next to a file it copies: the
/// header and no entries.
#[cfg(test)]
pub(crate) fn write_test_apple_double(path: &Path) {
    let mut bytes = vec![0x00, 0x05, 0x16, 0x07, 0x00, 0x02, 0x00, 0x00];
    bytes.extend_from_slice(b"Mac OS X        ");
    bytes.extend_from_slice(&[0, 0]);
    fs::write(path, bytes).expect("write AppleDouble file");
}

/// Whether `path` has a supported audio extension. It reads nothing, so the
/// bridge thread can use it; discovery uses [`is_audio_track_file`].
pub(crate) fn is_supported_audio(path: &Path) -> bool {
    let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
        return false;
    };
    matches!(
        ext.to_ascii_lowercase().as_str(),
        "mp3" | "flac" | "m4a" | "aac" | "ogg" | "opus" | "wav" | "ac3" | "dts"
    )
}

fn add_search_tag(tags: &mut super::SearchTags, key: &str, value: &str) {
    let value = value.trim();
    if value.is_empty() {
        return;
    }
    let values = tags.entry(key.to_string()).or_default();
    if !values.iter().any(|existing| existing == value) {
        values.push(value.to_string());
    }
}

fn collect_search_tags(tag: &lofty::tag::Tag, tags: &mut super::SearchTags) {
    for (key, item) in [
        ("title", ItemKey::TrackTitle),
        ("artist", ItemKey::TrackArtist),
        ("artist", ItemKey::TrackArtists),
        ("album", ItemKey::AlbumTitle),
        ("albumartist", ItemKey::AlbumArtist),
        ("albumartist", ItemKey::AlbumArtists),
        ("genre", ItemKey::Genre),
        ("composer", ItemKey::Composer),
        ("conductor", ItemKey::Conductor),
        ("performer", ItemKey::Performer),
        ("label", ItemKey::Label),
        ("label", ItemKey::Publisher),
        ("comment", ItemKey::Comment),
        ("lyrics", ItemKey::Lyrics),
        ("lyrics", ItemKey::UnsyncLyrics),
        ("date", ItemKey::RecordingDate),
        ("date", ItemKey::OriginalReleaseDate),
        ("disc", ItemKey::DiscNumber),
    ] {
        for value in tag.get_strings(item) {
            add_search_tag(tags, key, value);
        }
    }
}

pub(crate) fn read_track_info(path: &Path) -> IndexedTrack {
    read_track_info_with_cover_cache(path, EmbeddedCoverCache::shared()).indexed
}

/// A file read for the index by [`read_track_info_with_cover_cache`].
#[derive(Debug)]
struct TrackRead {
    indexed: IndexedTrack,
    /// Why the tags could not be read, if reading the file panicked, in the
    /// tag reader or elsewhere.
    failure: Option<String>,
    /// Why the embedded picture could not be cached; `indexed` has no cover
    /// then, though the file has one.
    cover_failure: Option<String>,
}

impl TrackRead {
    /// Whether the read got all the file has. Only such reads replace rows
    /// of the external-track cache.
    fn is_complete(&self) -> bool {
        self.failure.is_none() && self.cover_failure.is_none()
    }
}

/// Read `path` for the index, caching an embedded picture in `covers`.
///
/// A panic while reading the file, in the tag reader or elsewhere, fails the
/// read of that file alone, which the caller then reports.
fn read_track_catching_panic(path: &Path, covers: Option<&EmbeddedCoverCache>) -> TrackRead {
    crate::panic_guard::catch(&format!("reading '{}'", path.display()), || {
        #[cfg(test)]
        scan_test_hooks::before_read(path);
        read_track_info_with_cover_cache(path, covers)
    })
    .unwrap_or_else(|failure| TrackRead {
        indexed: untagged_track(path),
        failure: Some(failure),
        cover_failure: None,
    })
}

/// How a file read again updates the index row of the file.
#[derive(Debug)]
enum RowUpdate {
    /// The tags could not be read, and the row stays as it is: it holds the
    /// tags of the last good read, which the file name would replace, and its
    /// old file state makes the next scan read the file again.
    Keep(IndexedTrack),
    /// Write `indexed` to the row. Without `cover_checked`, the picture could
    /// not be cached, and `indexed` has the cover the row had, which also
    /// keeps that cache entry from the cleanup; the next scan retries it.
    Write {
        indexed: IndexedTrack,
        cover_checked: bool,
    },
}

impl RowUpdate {
    /// What the row holds after the update.
    fn into_indexed(self) -> IndexedTrack {
        match self {
            Self::Keep(indexed) | Self::Write { indexed, .. } => indexed,
        }
    }
}

/// How `read` updates the row of its file. `stored` looks up the row the
/// index has for the file, if any; only a failed read needs it.
fn row_update<E>(
    read: TrackRead,
    stored: impl FnOnce() -> Result<Option<IndexedTrack>, E>,
) -> Result<RowUpdate, E> {
    if read.is_complete() {
        return Ok(RowUpdate::Write {
            indexed: read.indexed,
            cover_checked: true,
        });
    }
    let stored = stored()?;
    if read.failure.is_some() {
        if let Some(stored) = stored {
            return Ok(RowUpdate::Keep(stored));
        }
    }
    // A picture that could not be cached is not "no cover".
    let mut indexed = read.indexed;
    let cover_checked = read.cover_failure.is_none();
    if !cover_checked {
        indexed.cover_path = stored.map(|stored| stored.cover_path).unwrap_or_default();
    }
    Ok(RowUpdate::Write {
        indexed,
        cover_checked,
    })
}

fn prepare_library_row_statement(
    conn: &Connection,
) -> Result<rusqlite::CachedStatement<'_>, String> {
    conn.prepare_cached(
        r"
        SELECT title, artist, album, cover_path, genre, year, track_no, duration_secs, search_tags
        FROM tracks
        WHERE path = ?1
        ",
    )
    .map_err(|e| format!("failed to prepare track row statement: {e}"))
}

fn query_library_row(
    statement: &mut rusqlite::CachedStatement<'_>,
    key: &str,
) -> Result<Option<IndexedTrack>, String> {
    statement
        .query_row(params![key], indexed_track_from_row)
        .optional()
        .map_err(|e| format!("failed to read the indexed row of '{key}': {e}"))
}

/// The library row of the file at `key`.
fn library_row(conn: &Connection, key: &str) -> Result<Option<IndexedTrack>, String> {
    query_library_row(&mut prepare_library_row_statement(conn)?, key)
}

/// The external-track cache row of the file at `key`, whatever file state it
/// was read at, and that state.
fn external_row(
    conn: &Connection,
    key: &str,
) -> Result<Option<(IndexedTrack, TrackFileFingerprint)>, String> {
    conn.query_row(
        r"
        SELECT title, artist, album, cover_path, genre, year, track_no, duration_secs, search_tags,
               mtime_ns, size_bytes
        FROM external_tracks
        WHERE path = ?1
        ",
        params![key],
        |row| {
            let fingerprint = TrackFileFingerprint {
                mtime_ns: row.get(9)?,
                size_bytes: row.get(10)?,
            };
            Ok((indexed_track_from_row(row)?, fingerprint))
        },
    )
    .optional()
    .map_err(|e| format!("failed to read the cached row of '{key}': {e}"))
}

/// The row the index has for the file at `key`: its library row, or else its
/// external-track cache row.
fn stored_track(conn: &Connection, key: &str) -> Result<Option<IndexedTrack>, String> {
    if let Some(row) = library_row(conn, key)? {
        return Ok(Some(row));
    }
    Ok(external_row(conn, key)?.map(|(indexed, _)| indexed))
}

/// An [`IndexedTrack`] from the columns `title, artist, album, cover_path,
/// genre, year, track_no, duration_secs, search_tags`, in that order.
fn indexed_track_from_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<IndexedTrack> {
    Ok(IndexedTrack {
        search_tags: serde_json::from_str(&row.get::<_, Option<String>>(8)?.unwrap_or_default())
            .unwrap_or_default(),
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
}

/// Like [`read_track_info`], but caches an embedded picture in `covers`, and
/// also returns why the tags could not be read if the tag reader panicked,
/// and why the picture could not be cached.
///
/// A file whose tags lofty cannot read, or that has none, is indexed by its
/// name with no failure: raw surround files, for one, are read that way and
/// get their tags from an appended `APEv2` tag.
fn read_track_info_with_cover_cache(path: &Path, covers: Option<&EmbeddedCoverCache>) -> TrackRead {
    let mut out = untagged_track(path);

    let (tagged, failure) = match crate::panic_guard::read_tags_catching_panic(path) {
        Ok(read) => (read.ok(), None),
        Err(failure) => (None, Some(failure)),
    };
    if let Some(tagged) = &tagged {
        for tag in tagged.tags() {
            collect_search_tags(tag, &mut out.search_tags);
        }
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
            for (key, value) in [
                ("albumartist", tagged.album_artist.as_deref()),
                ("comment", tagged.comment.as_deref()),
            ] {
                if let Some(value) = value {
                    add_search_tag(&mut out.search_tags, key, value);
                }
            }
            if let Some(disc) = tagged.disc_no {
                add_search_tag(&mut out.search_tags, "disc", &disc.to_string());
            }
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
            eprintln!(
                "[ferrous] could not determine duration for raw surround file: {}",
                path.display()
            );
        }
    }

    out.cover_path = find_cover_path_for_track(path);
    let mut cover_failure = None;
    // A reader that panicked read no picture.
    if let (true, Some(cache), Some(tagged)) = (out.cover_path.is_empty(), covers, &tagged) {
        match cache.try_store_tagged_cover(tagged) {
            Ok(cover_path) => {
                out.cover_path = cover_path
                    .map(|cover_path| cover_path.to_string_lossy().into_owned())
                    .unwrap_or_default();
            }
            Err(error) => cover_failure = Some(error),
        }
    }
    TrackRead {
        indexed: out,
        failure,
        cover_failure,
    }
}

/// The index entry of a file without readable tags: its name as the title.
fn untagged_track(path: &Path) -> IndexedTrack {
    IndexedTrack {
        search_tags: crate::library::SearchTags::default(),
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
    }
}

pub(super) fn find_cover_path_for_track(path: &Path) -> String {
    path.parent()
        .and_then(find_folder_cover)
        .map(|cover| cover.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| u64_to_i64(duration.as_secs()))
        .unwrap_or(0)
}

/// Reads the covers of `paths` again, after their pictures changed, and
/// updates their library and external-track cache rows; see [`row_update`]
/// for files that cannot be read fully.
pub(crate) fn refresh_cover_paths_for_tracks(paths: &[PathBuf]) -> Result<TrackRefresh, String> {
    let conn = open_library_db().map_err(|e| format!("failed to open library db: {e}"))?;
    init_schema(&conn).map_err(|e| format!("failed to initialize library db schema: {e}"))?;
    refresh_cover_paths_in_conn(&conn, paths, EmbeddedCoverCache::shared())
}

fn refresh_cover_paths_in_conn(
    conn: &Connection,
    paths: &[PathBuf],
    covers: Option<&EmbeddedCoverCache>,
) -> Result<TrackRefresh, String> {
    let now = unix_ts_i64();
    let mut refreshed = TrackRefresh::with_capacity(paths.len());
    let mut failures = ReadFailures::default();

    for path in paths {
        let read = read_track_catching_panic(path, covers);
        failures.note(path, &read);
        let complete = read.is_complete();
        let path_string = path.to_string_lossy().to_string();
        let update = row_update(read, || stored_track(conn, &path_string))?;
        if let RowUpdate::Write {
            indexed,
            cover_checked,
        } = &update
        {
            conn.execute(
                r"
                UPDATE tracks
                SET cover_path = ?2,
                    cover_checked = ?3,
                    indexed_at = ?4
                WHERE path = ?1
                ",
                params![
                    path_string,
                    indexed.cover_path.as_str(),
                    i64::from(*cover_checked),
                    now,
                ],
            )
            .map_err(|e| format!("failed to refresh track cover path: {e}"))?;
        }

        let indexed = update.into_indexed();
        if complete && is_in_external_cache(conn, &path_string) {
            if let Some(fingerprint) = track_file_fingerprint(path) {
                store_external_track_cache_in_conn(conn, path, fingerprint, &indexed)?;
            }
        }
        refreshed.indexed.insert(path.clone(), indexed);
    }

    refreshed.problem = failures.refresh_problem();
    Ok(refreshed)
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

/// Feeds a scan's progress and the rows it writes to closures.
#[cfg(test)]
struct ClosureSink<'a, F, U> {
    on_progress: &'a mut F,
    on_upsert: &'a mut U,
}

#[cfg(test)]
impl<F, U> ScanSink for ClosureSink<'_, F, U>
where
    F: FnMut(RootScanProgress),
    U: FnMut(&MetadataTask, &IndexedTrack),
{
    fn progress(&mut self, _conn: &Connection, progress: RootScanProgress) {
        (self.on_progress)(progress);
    }

    fn committed(&mut self, _root: &Path, rows: Vec<(MetadataTask, IndexedTrack)>) {
        for (task, indexed) in &rows {
            (self.on_upsert)(task, indexed);
        }
    }

    fn should_stop(&mut self, _conn: &Connection) -> bool {
        false
    }
}

/// Scans `root` uninterrupted, reporting its progress and the rows it
/// writes to closures.
#[cfg(test)]
fn scan_root_observed<F, U>(
    conn: &Connection,
    root: &Path,
    covers: Option<&'static EmbeddedCoverCache>,
    on_progress: &mut F,
    on_upsert: &mut U,
) -> Result<RootScanOutcome, String>
where
    F: FnMut(RootScanProgress),
    U: FnMut(&MetadataTask, &IndexedTrack),
{
    scan_root(
        conn,
        root,
        covers,
        &mut ClosureSink {
            on_progress,
            on_upsert,
        },
    )
}

#[cfg(test)]
mod batch_tests;
#[cfg(test)]
mod refresh_tests;
#[cfg(test)]
mod scope_tests;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::LibraryCommand;
    use crate::raw_audio::write_test_apev2_file;
    use std::io::Write;
    use std::sync::atomic::{AtomicBool, Ordering};

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
            search_tags: crate::library::SearchTags::default(),
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
    fn external_track_cache_roundtrips_fresh_metadata() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let path = PathBuf::from("/outside/song.flac");
        let fingerprint = TrackFileFingerprint {
            mtime_ns: 42,
            size_bytes: 1337,
        };
        let indexed = IndexedTrack {
            search_tags: crate::library::SearchTags::default(),
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
            search_tags: crate::library::SearchTags::default(),
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
            search_tags: crate::library::SearchTags::default(),
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
    fn scan_root_skips_apple_double_sidecars_but_keeps_dot_underscore_tracks() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("apple-double");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        write_stub(&root.join("01.mp3"), crate::tag_write::TEST_MP3);
        write_test_apple_double(&root.join("._01.mp3"));
        write_stub(&root.join("._live.mp3"), crate::tag_write::TEST_MP3);

        let mut last = RootScanProgress::default();
        scan_root(
            &conn,
            &root,
            &mut |progress| last = progress,
            &mut |_, _| {},
        )
        .expect("scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        let mut paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
        paths.sort();
        assert_eq!(paths, [root.join("._live.mp3"), root.join("01.mp3")]);
        assert_eq!(last.discovered, 2);
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn scan_root_removes_rows_indexed_from_apple_double_sidecars() {
        let mut conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("apple-double-stale");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let track = root.join("01.mp3");
        let sidecar = root.join("._01.mp3");
        write_stub(&track, crate::tag_write::TEST_MP3);
        write_test_apple_double(&sidecar);
        // As a scan before sidecars were skipped indexed them.
        insert_indexed_tracks(&mut conn, &root, &[track.clone(), sidecar.clone()]);

        let outcome = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        let paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
        assert_eq!(paths, [track]);
        assert_eq!(outcome.removed, [sidecar.to_string_lossy().to_string()]);
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn only_a_dot_underscore_file_with_the_apple_double_header_is_a_sidecar() {
        let root = test_dir("apple-double-name");
        fs::create_dir_all(&root).expect("mkdir");
        let sidecar = root.join("._01.mp3");
        write_test_apple_double(&sidecar);
        let header_without_name = root.join("01.mp3");
        write_test_apple_double(&header_without_name);
        let track_with_name = root.join("._live.mp3");
        write_stub(&track_with_name, crate::tag_write::TEST_MP3);
        let sidecar_text = root.join("._notes.txt");
        write_test_apple_double(&sidecar_text);

        assert!(!is_audio_track_file(&sidecar));
        assert!(is_audio_track_file(&header_without_name));
        assert!(is_audio_track_file(&track_with_name));
        assert!(!is_audio_track_file(&sidecar_text));
        // Unreadable, so not known to be a sidecar.
        assert!(is_audio_track_file(&root.join("._missing.mp3")));
        assert!(is_apple_double_sidecar(&sidecar));
        assert!(!is_apple_double_sidecar(&header_without_name));
        assert!(!is_apple_double_sidecar(&track_with_name));
        assert!(!is_apple_double_sidecar(&root.join("._missing.mp3")));
        fs::remove_dir_all(root).expect("cleanup");
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
    fn scan_picks_a_recognised_cover_name_in_any_case_over_other_images() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("cover-case");
        fs::create_dir_all(&root).expect("mkdir");
        let mp3 = root.join("song.mp3");
        let cover = root.join("Cover.JPG");
        write_stub(&mp3, b"not-real-mp3");
        for image in ["a.jpg", "back.jpg", "booklet01.png"] {
            write_stub(&root.join(image), b"not-real-jpg");
        }
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
        write_stub(&root.join("song2.mp3"), b"not-real-mp3");

        let _ = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("initial scan");

        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 2);

        fs::remove_file(&mp3).expect("remove mp3");
        let outcome = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("rescan");
        assert_eq!(outcome.removed, [mp3.to_string_lossy().to_string()]);
        assert_eq!(outcome.problem, None);
        assert_eq!(indexed_paths(&conn), [root.join("song2.mp3")]);

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
            vec![super::super::LibraryRoot {
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

    /// Scans with the shared cover cache, as the library worker does.
    fn scan_root<F, U>(
        conn: &Connection,
        root: &Path,
        on_progress: &mut F,
        on_upsert: &mut U,
    ) -> Result<RootScanOutcome, String>
    where
        F: FnMut(RootScanProgress),
        U: FnMut(&MetadataTask, &IndexedTrack),
    {
        scan_root_observed(
            conn,
            root,
            EmbeddedCoverCache::shared(),
            on_progress,
            on_upsert,
        )
    }

    /// Runs `cmd` as the library worker does, with nothing queued.
    fn handle_command(
        conn: &Connection,
        cmd: LibraryCommand,
        snapshot: &mut WorkerLibrary,
        event_tx: &Sender<LibraryEvent>,
    ) {
        super::super::worker::handle_command(
            conn,
            cmd,
            snapshot,
            event_tx,
            &mut super::super::worker::Control::detached(),
        );
    }

    fn indexed_paths(conn: &Connection) -> Vec<PathBuf> {
        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(conn, &mut snapshot);
        let mut paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
        paths.sort();
        paths
    }

    fn scan(conn: &Connection, root: &Path) -> RootScanOutcome {
        scan_root(conn, root, &mut |_| {}, &mut |_, _| {}).expect("scan")
    }

    #[test]
    fn a_file_that_panics_its_reader_is_indexed_by_name_and_reported() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("reader-panic");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let names = ["a.mp3", "broken.mp3", "c.mp3", "d.mp3"];
        for name in names {
            write_stub(&root.join(name), b"not-real-mp3");
        }
        let broken = root.join("broken.mp3");
        scan_test_hooks::panic_on_read(&broken);

        let outcome = scan(&conn, &root);

        let expected: Vec<PathBuf> = names.iter().map(|name| root.join(name)).collect();
        assert_eq!(indexed_paths(&conn), expected);
        let problem = outcome.problem.expect("the unreadable file is reported");
        assert!(
            problem.contains("could not read the tags of 1 file"),
            "{problem}"
        );
        assert!(problem.contains(&broken.display().to_string()), "{problem}");
        assert!(problem.contains("test panic"), "{problem}");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn a_tag_reader_panic_keeps_the_indexed_tags_and_is_reported() {
        use crate::vorbis_comment_scan::fixtures::{comment_block, flac};

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("tag-reader-panic");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let saved = root.join("track.flac");
        let tags: [&[u8]; 3] = [
            b"TITLE=Saved Title",
            b"ARTIST=Saved Artist",
            b"ALBUM=Saved Album",
        ];
        fs::write(&saved, flac(&[&comment_block(&tags)])).expect("write track");
        assert!(scan(&conn, &root).problem.is_none());
        let saved_tags = |conn: &Connection| {
            let mut snapshot = LibrarySnapshot::default();
            load_snapshot(conn, &mut snapshot);
            snapshot
                .tracks
                .iter()
                .map(|track| {
                    (
                        track.path.clone(),
                        track.title.clone(),
                        track.artist.clone(),
                        track.album.clone(),
                    )
                })
                .collect::<Vec<_>>()
        };
        let indexed = vec![(
            saved.clone(),
            "Saved Title".to_string(),
            "Saved Artist".to_string(),
            "Saved Album".to_string(),
        )];
        assert_eq!(saved_tags(&conn), indexed);

        // The same tags with a `COVERART` picture that makes lofty panic, and
        // a new file that does too.
        let broken_tags: [&[u8]; 4] = [tags[0], tags[1], tags[2], b"COVERART=AAAA"];
        fs::write(&saved, flac(&[&comment_block(&broken_tags)])).expect("break track");
        let new = root.join("new.flac");
        fs::write(&new, crate::panic_guard::fixtures::short_coverart_flac()).expect("write new");

        let problem = scan(&conn, &root)
            .problem
            .expect("the unreadable files are reported");

        assert!(
            problem.contains("could not read the tags of 2 files"),
            "{problem}"
        );
        assert!(problem.contains("the tag reader for"), "{problem}");
        let mut expected = vec![(new, "new".to_string(), String::new(), String::new())];
        expected.extend(indexed);
        assert_eq!(saved_tags(&conn), expected);
        // The row keeps its old file state, so the next scan tries again.
        let problem = scan(&conn, &root).problem.expect("still reported");
        assert!(
            problem.contains("could not read the tags of 1 file"),
            "{problem}"
        );
        let _ = fs::remove_dir_all(root);
    }

    /// A private cover cache: concurrent tests clean the shared one after
    /// their scans. Scan workers borrow it, so it lives until the process
    /// exits.
    fn private_cover_cache(name: &str) -> &'static EmbeddedCoverCache {
        Box::leak(Box::new(crate::metadata::test_cache(name)))
    }

    /// The row's `(cover_path, cover_checked)`.
    fn cover_row(conn: &Connection, path: &Path) -> (String, bool) {
        conn.query_row(
            "SELECT cover_path, cover_checked FROM tracks WHERE path = ?1",
            params![path.to_string_lossy()],
            |row| Ok((row.get(0)?, row.get::<_, i64>(1)? != 0)),
        )
        .expect("track row")
    }

    fn snapshot_cover(snapshot: &WorkerLibrary, path: &Path) -> String {
        snapshot
            .tracks()
            .iter()
            .find(|track| track.path == path)
            .map(|track| track.cover_path.clone())
            .expect("snapshot track")
    }

    fn scan_with_cache(
        conn: &Connection,
        root: &Path,
        cache: &'static EmbeddedCoverCache,
        snapshot: &mut WorkerLibrary,
    ) -> Result<(), String> {
        let (event_tx, _event_rx) = unbounded();
        run_scans(
            conn,
            &[root.to_path_buf()],
            Some(cache),
            snapshot,
            &event_tx,
        )
    }

    #[test]
    fn a_cover_that_cannot_be_cached_is_reported_once_and_retried_by_the_next_scan() {
        use crate::metadata::{encoded_picture, write_wav_with_picture};

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let cache = private_cover_cache("scan-blocked-entry");
        let root = test_dir("scan-blocked-cover");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let picture = encoded_picture(21, image::ImageFormat::Png);
        let tracks = [root.join("01.wav"), root.join("02.wav")];
        for track in &tracks {
            write_wav_with_picture(track, &picture);
        }
        let without_picture = root.join("03.mp3");
        write_stub(&without_picture, b"not-real-mp3");
        // A directory where the entry belongs makes writing it fail.
        let entry = cache
            .dir()
            .join(format!("{:x}.png", md5::compute(&picture)));
        fs::create_dir_all(&entry).expect("block the entry");
        let mut snapshot = WorkerLibrary::default();

        let error = scan_with_cache(&conn, &root, cache, &mut snapshot)
            .expect_err("the uncached covers are reported");

        assert!(
            error.contains("could not cache the embedded covers of 2 files")
                && error.contains("embedded cover cache"),
            "{error}"
        );
        assert_eq!(error.matches("could not cache").count(), 1, "{error}");
        for track in &tracks {
            assert_eq!(
                cover_row(&conn, track),
                (String::new(), false),
                "left due for a retry"
            );
        }
        assert_eq!(
            cover_row(&conn, &without_picture),
            (String::new(), true),
            "a file without a picture is checked"
        );

        // The files are unchanged; the next scan retries their covers.
        fs::remove_dir(&entry).expect("unblock the entry");
        scan_with_cache(&conn, &root, cache, &mut snapshot).expect("scan caches the covers");

        let entry_string = entry.to_string_lossy().into_owned();
        for track in &tracks {
            assert_eq!(cover_row(&conn, track), (entry_string.clone(), true));
            assert_eq!(snapshot_cover(&snapshot, track), entry_string);
        }
        assert_eq!(fs::read(&entry).expect("read entry"), picture);
        let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn a_scan_that_cannot_cache_a_changed_cover_keeps_the_legacy_entry_of_the_row() {
        use crate::metadata::{encoded_picture, write_wav_with_picture};

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let cache = private_cover_cache("scan-legacy-entry");
        let root = test_dir("scan-legacy-cover");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let picture = encoded_picture(22, image::ImageFormat::Jpeg);
        let track = root.join("01.wav");
        write_wav_with_picture(&track, &picture);
        // The startup migration kept this per-track PNG because the new entry
        // could not be written; the file has changed since it was indexed.
        fs::create_dir_all(cache.dir()).expect("create cache dir");
        let legacy = cache.dir().join("0123456789abcdef.png");
        fs::write(&legacy, b"legacy png").expect("write legacy entry");
        let legacy_string = legacy.to_string_lossy().into_owned();
        conn.execute(
            "INSERT INTO tracks(path, root_path, title, artist, album, cover_path, cover_checked, \
                                mtime_ns, size_bytes, indexed_at) \
             VALUES (?1, ?2, 'Old', '', '', ?3, 1, 1, 1, 1)",
            params![
                track.to_string_lossy(),
                root.to_string_lossy(),
                legacy_string
            ],
        )
        .expect("indexed row");
        let entry = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&picture)));
        fs::create_dir(&entry).expect("block the new entry");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);

        let error = scan_with_cache(&conn, &root, cache, &mut snapshot)
            .expect_err("the uncached cover is reported");

        assert!(
            error.contains("could not cache the embedded covers of 1 file"),
            "{error}"
        );
        assert_eq!(cover_row(&conn, &track), (legacy_string.clone(), false));
        assert_eq!(snapshot_cover(&snapshot, &track), legacy_string);
        assert!(legacy.is_file(), "the cleanup keeps the entry the row uses");

        fs::remove_dir(&entry).expect("unblock the new entry");
        scan_with_cache(&conn, &root, cache, &mut snapshot).expect("scan caches the cover");

        let entry_string = entry.to_string_lossy().into_owned();
        assert_eq!(cover_row(&conn, &track), (entry_string.clone(), true));
        assert_eq!(snapshot_cover(&snapshot, &track), entry_string);
        assert!(entry.is_file());
        assert!(!legacy.exists(), "the unreferenced legacy entry is removed");
        let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn a_root_reclaiming_a_track_keeps_its_cover_when_the_picture_cannot_be_cached() {
        use crate::metadata::{encoded_picture, write_wav_with_picture};

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let cache = private_cover_cache("scan-reclaimed-entry");
        let outer = test_dir("scan-reclaimed-cover");
        fs::create_dir_all(outer.join("inner")).expect("mkdir inner");
        let outer = outer.canonicalize().expect("canonical outer");
        let inner = outer.join("inner");
        let picture = encoded_picture(23, image::ImageFormat::Jpeg);
        let track = inner.join("01.wav");
        write_wav_with_picture(&track, &picture);
        // A database from before nested roots were rejected: the outer root
        // still owns the inner folder's track, whose legacy entry the startup
        // migration kept, and the inner root was added without a rescan.
        fs::create_dir_all(cache.dir()).expect("create cache dir");
        let legacy = cache.dir().join("fedcba9876543210.png");
        fs::write(&legacy, b"legacy png").expect("write legacy entry");
        let legacy_string = legacy.to_string_lossy().into_owned();
        insert_root(&conn, &outer, "").expect("outer root");
        insert_root(&conn, &inner, "").expect("legacy inner root");
        conn.execute(
            "INSERT INTO tracks(path, root_path, title, artist, album, cover_path, cover_checked, \
                                mtime_ns, size_bytes, indexed_at) \
             VALUES (?1, ?2, 'Old', '', '', ?3, 1, 1, 1, 1)",
            params![
                track.to_string_lossy(),
                outer.to_string_lossy(),
                legacy_string
            ],
        )
        .expect("row owned by the outer root");
        let entry = cache
            .dir()
            .join(format!("{:x}.jpg", md5::compute(&picture)));
        fs::create_dir(&entry).expect("block the new entry");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);

        let error = scan_with_cache(&conn, &inner, cache, &mut snapshot)
            .expect_err("the uncached cover is reported");

        assert!(
            error.contains("could not cache the embedded covers of 1 file"),
            "{error}"
        );
        assert_eq!(cover_row(&conn, &track), (legacy_string.clone(), false));
        assert_eq!(snapshot_cover(&snapshot, &track), legacy_string);
        assert!(legacy.is_file(), "the cleanup keeps the entry the row uses");

        fs::remove_dir(&entry).expect("unblock the new entry");
        scan_with_cache(&conn, &inner, cache, &mut snapshot).expect("scan caches the cover");

        let entry_string = entry.to_string_lossy().into_owned();
        assert_eq!(cover_row(&conn, &track), (entry_string.clone(), true));
        assert!(entry.is_file());
        assert!(!legacy.exists(), "the unreferenced legacy entry is removed");
        let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
        let _ = fs::remove_dir_all(outer);
    }

    fn insert_legacy_track(conn: &Connection, path: &Path) {
        conn.execute(
            "INSERT INTO tracks(path,root_path,title,artist,album,mtime_ns,size_bytes,indexed_at) \
             VALUES (?1,'','Legacy','Artist','Album',0,0,0)",
            params![path.to_string_lossy()],
        )
        .expect("legacy row");
    }

    #[test]
    fn root_paths_match_exactly_when_removing_and_scanning() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let base = test_dir("exact-prefix");
        for name in ["a_b", "aXb", "A_B"] {
            fs::create_dir_all(base.join(name)).expect("mkdir root");
            write_stub(&base.join(name).join("song.mp3"), name.as_bytes());
        }
        let base = base.canonicalize().expect("canonical base");
        for name in ["a_b", "aXb", "A_B"] {
            scan(&conn, &base.join(name));
        }
        // Rows from databases that predate per-track roots.
        insert_legacy_track(&conn, &base.join("a_b/legacy.mp3"));
        insert_legacy_track(&conn, &base.join("aXb/legacy.mp3"));
        insert_legacy_track(&conn, &base.join("A_B/legacy.mp3"));

        // `LIKE 'a_b/%'` matched the other two roots' legacy rows.
        let outcome = scan(&conn, &base.join("a_b"));
        assert_eq!(
            outcome.removed,
            [base.join("a_b/legacy.mp3").to_string_lossy().to_string()]
        );

        remove_root_and_purge(&conn, &base.join("a_b")).expect("remove a_b");

        assert_eq!(
            indexed_paths(&conn),
            [
                base.join("A_B/legacy.mp3"),
                base.join("A_B/song.mp3"),
                base.join("aXb/legacy.mp3"),
                base.join("aXb/song.mp3"),
            ]
        );
        let _ = fs::remove_dir_all(base);
    }

    #[test]
    fn add_root_rejects_nested_roots() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let base = test_dir("nested-add");
        fs::create_dir_all(base.join("music/inner")).expect("mkdir inner");
        fs::create_dir_all(base.join("music-2")).expect("mkdir sibling");
        let base = base.canonicalize().expect("canonical base");
        let mut snapshot = WorkerLibrary::default();
        let (tx, _rx) = unbounded();
        let add = |snapshot: &mut WorkerLibrary, path: &Path| {
            handle_add_root(&conn, path, "", snapshot, &tx, &mut Uninterrupted)
        };

        add(&mut snapshot, &base.join("music")).expect("add root");
        add(&mut snapshot, &base.join("music")).expect("re-adding a root rescans it");
        add(&mut snapshot, &base.join("music-2")).expect("sibling with a shared prefix");
        let inside = add(&mut snapshot, &base.join("music/inner")).expect_err("inside a root");
        assert!(inside.contains("is inside library root"), "{inside}");
        let contains = add(&mut snapshot, &base).expect_err("contains a root");
        assert!(contains.contains("contains library root"), "{contains}");

        let roots: Vec<PathBuf> = load_roots(&conn).into_iter().map(|r| r.path).collect();
        assert_eq!(roots, [base.join("music"), base.join("music-2")]);
        let _ = fs::remove_dir_all(base);
    }

    #[test]
    fn scan_root_keeps_tracks_when_a_previously_populated_root_is_empty() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("emptied-root").join("mount");
        fs::create_dir_all(&root).expect("mkdir");
        let root = root.canonicalize().expect("canonical root");
        let song = root.join("song.mp3");
        write_stub(&song, b"not-real-mp3");
        scan(&conn, &root);

        // An unmounted mount point is an empty directory.
        fs::remove_file(&song).expect("remove song");
        let outcome = scan(&conn, &root);

        assert!(outcome.removed.is_empty());
        let problem = outcome.problem.expect("empty root is reported");
        assert!(problem.contains("found no audio files"), "{problem}");
        assert!(problem.contains("1 indexed track"), "{problem}");
        assert_eq!(indexed_paths(&conn), [song]);

        let _ = fs::remove_dir_all(root.parent().expect("fixture parent"));
    }

    #[test]
    fn scan_root_without_previous_tracks_accepts_an_empty_root() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("new-empty-root");
        fs::create_dir_all(&root).expect("mkdir");

        let outcome = scan(&conn, &root);

        assert!(outcome.removed.is_empty());
        assert_eq!(outcome.problem, None);
        let _ = fs::remove_dir_all(root);
    }

    #[cfg(unix)]
    #[test]
    fn scan_root_keeps_tracks_in_unreadable_folders() {
        use std::os::unix::fs::PermissionsExt;

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("unreadable");
        fs::create_dir_all(root.join("open")).expect("mkdir open");
        fs::create_dir_all(root.join("locked")).expect("mkdir locked");
        let root = root.canonicalize().expect("canonical root");
        let open_song = root.join("open/song.mp3");
        let locked_song = root.join("locked/song.mp3");
        write_stub(&open_song, b"open");
        write_stub(&locked_song, b"locked");
        scan(&conn, &root);
        assert_eq!(indexed_paths(&conn).len(), 2);

        let locked = root.join("locked");
        fs::set_permissions(&locked, fs::Permissions::from_mode(0o000)).expect("lock folder");
        if fs::read_dir(&locked).is_ok() {
            // Root and CAP_DAC_OVERRIDE ignore directory permissions; the
            // dangling-link test covers unreadable paths in that case.
            fs::set_permissions(&locked, fs::Permissions::from_mode(0o755)).expect("unlock");
            let _ = fs::remove_dir_all(root);
            eprintln!("skipping: permission-denied folders are readable for this user");
            return;
        }
        let outcome = scan(&conn, &root);
        fs::set_permissions(&locked, fs::Permissions::from_mode(0o755)).expect("unlock");

        assert!(outcome.removed.is_empty());
        let problem = outcome.problem.expect("unreadable folder is reported");
        assert!(
            problem.contains("could not read 1 folder or file"),
            "{problem}"
        );
        assert!(problem.contains(&locked.display().to_string()), "{problem}");
        assert_eq!(indexed_paths(&conn), [locked_song, open_song]);

        let _ = fs::remove_dir_all(root);
    }

    #[cfg(unix)]
    #[test]
    fn scan_root_follows_links_and_keeps_tracks_behind_dangling_links() {
        use std::os::unix::fs::symlink;

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let base = test_dir("dangling-link");
        let root = base.join("root");
        let external = base.join("external");
        fs::create_dir_all(root.join("local")).expect("mkdir local");
        fs::create_dir_all(&external).expect("mkdir external");
        let root = root.canonicalize().expect("canonical root");
        let local_song = root.join("local/song.mp3");
        write_stub(&local_song, b"local");
        write_stub(&external.join("song.mp3"), b"external");
        symlink(&external, root.join("linked")).expect("link folder");
        let linked_song = root.join("linked/song.mp3");
        scan(&conn, &root);
        assert_eq!(
            indexed_paths(&conn),
            [linked_song.clone(), local_song.clone()]
        );

        // The link target disappears, as when the drive it points to is not
        // mounted, and the local track is deleted.
        fs::remove_dir_all(&external).expect("remove link target");
        fs::remove_file(&local_song).expect("remove local song");
        write_stub(&root.join("local/other.mp3"), b"other");
        let outcome = scan(&conn, &root);

        assert_eq!(outcome.removed, [local_song.to_string_lossy().to_string()]);
        let problem = outcome.problem.expect("dangling link is reported");
        assert!(
            problem.contains("could not read 1 folder or file"),
            "{problem}"
        );
        assert!(
            problem.contains(&root.join("linked").display().to_string()),
            "{problem}"
        );
        assert_eq!(
            indexed_paths(&conn),
            [linked_song, root.join("local/other.mp3")]
        );

        let _ = fs::remove_dir_all(base);
    }

    #[cfg(unix)]
    #[test]
    fn scan_root_indexes_each_file_once_and_prefers_paths_without_links() {
        use std::os::unix::fs::symlink;

        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let base = test_dir("links-once");
        let root = base.join("root");
        fs::create_dir_all(root.join("Albums/Album")).expect("mkdir album");
        fs::create_dir_all(base.join("external")).expect("mkdir external");
        let root = root.canonicalize().expect("canonical root");
        let song = root.join("Albums/Album/song.mp3");
        write_stub(&song, b"song");
        write_stub(&base.join("external/other.mp3"), b"other");
        // "0 Links" sorts before "Albums", so the walk reaches the linked
        // paths first.
        symlink(root.join("Albums"), root.join("0 Links")).expect("link folder");
        symlink(&song, root.join("0 single.mp3")).expect("link file");
        symlink(base.join("external"), root.join("External")).expect("link external");
        symlink(root.join("Albums"), root.join("Albums/Album/loop")).expect("link loop");

        let mut upserts = 0usize;
        let outcome = scan_root(&conn, &root, &mut |_| {}, &mut |_, _| upserts += 1).expect("scan");

        assert_eq!(outcome.problem, None);
        assert_eq!(upserts, 2);
        let expected = [
            root.join("Albums/Album/song.mp3"),
            root.join("External/other.mp3"),
        ];
        assert_eq!(indexed_paths(&conn), expected);

        upserts = 0;
        let outcome =
            scan_root(&conn, &root, &mut |_| {}, &mut |_, _| upserts += 1).expect("rescan");
        assert_eq!(upserts, 0);
        assert!(outcome.removed.is_empty());
        assert_eq!(indexed_paths(&conn), expected);

        let _ = fs::remove_dir_all(base);
    }

    #[test]
    fn rescan_all_scans_every_root_and_reports_failures() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let missing = test_dir("rescan-a-missing");
        let healthy = test_dir("rescan-b-healthy");
        fs::create_dir_all(&missing).expect("mkdir missing");
        fs::create_dir_all(&healthy).expect("mkdir healthy");
        let missing = missing.canonicalize().expect("canonical missing");
        let healthy = healthy.canonicalize().expect("canonical healthy");
        write_stub(&missing.join("kept.mp3"), b"kept");
        write_stub(&healthy.join("old.mp3"), b"old");
        scan(&conn, &missing);
        scan(&conn, &healthy);
        assert!(load_roots(&conn).first().is_some_and(|r| r.path == missing));

        fs::remove_dir_all(&missing).expect("remove first root");
        write_stub(&healthy.join("new.mp3"), b"new");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);
        let (tx, _rx) = unbounded();
        let err = handle_rescan_all(&conn, &mut snapshot, &tx, &mut Uninterrupted)
            .expect_err("failure is reported");

        assert!(
            err.contains(&format!("failed to scan '{}'", missing.display())),
            "{err}"
        );
        let expected = [
            missing.join("kept.mp3"),
            healthy.join("new.mp3"),
            healthy.join("old.mp3"),
        ];
        assert_eq!(indexed_paths(&conn), expected);
        let mut snapshot_paths: Vec<PathBuf> =
            snapshot.tracks().iter().map(|t| t.path.clone()).collect();
        snapshot_paths.sort();
        assert_eq!(snapshot_paths, expected);

        let _ = fs::remove_dir_all(healthy);
    }

    #[test]
    fn scan_problems_are_summarized_per_root() {
        let problems = ["a".to_string(), "b".to_string()];
        assert_eq!(summarize_scan_problems(&problems), "a; b");
        let problems: Vec<String> = (1..=5).map(|n| n.to_string()).collect();
        assert_eq!(
            summarize_scan_problems(&problems),
            "1; 2; 3; 2 more library roots had problems"
        );
    }

    #[test]
    fn existing_nested_roots_keep_their_own_tracks() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let outer = test_dir("nested-legacy");
        fs::create_dir_all(outer.join("inner")).expect("mkdir inner");
        let outer = outer.canonicalize().expect("canonical outer");
        let inner = outer.join("inner");
        write_stub(&outer.join("outer.mp3"), b"outer");
        write_stub(&inner.join("inner.mp3"), b"inner");
        // Before nested roots were rejected, the outer root claimed the
        // inner root's tracks.
        scan(&conn, &outer);
        insert_root(&conn, &inner, "").expect("legacy nested root");
        scan(&conn, &inner);

        let mut upserts = 0usize;
        let outcome =
            scan_root(&conn, &outer, &mut |_| {}, &mut |_, _| upserts += 1).expect("rescan outer");
        assert_eq!(upserts, 0);
        assert!(outcome.removed.is_empty());
        let root_of = |path: &Path| -> String {
            conn.query_row(
                "SELECT root_path FROM tracks WHERE path=?1",
                params![path.to_string_lossy()],
                |row| row.get(0),
            )
            .expect("track row")
        };
        assert_eq!(root_of(&inner.join("inner.mp3")), inner.to_string_lossy());

        remove_root_and_purge(&conn, &outer).expect("remove outer");
        assert_eq!(indexed_paths(&conn), [inner.join("inner.mp3")]);
        let _ = fs::remove_dir_all(outer);
    }

    fn root_of(conn: &Connection, path: &Path) -> String {
        conn.query_row(
            "SELECT root_path FROM tracks WHERE path=?1",
            params![path.to_string_lossy()],
            |row| row.get(0),
        )
        .expect("track row")
    }

    fn snapshot_paths(snapshot: &WorkerLibrary) -> Vec<PathBuf> {
        snapshot.paths()
    }

    /// The library as the events the library worker published have it.
    fn last_published(rx: &Receiver<LibraryEvent>) -> crate::library::LibraryMirror {
        let mut mirror = crate::library::LibraryMirror::default();
        mirror.drain(rx);
        assert!(mirror.events > 0, "a published event");
        mirror
    }

    #[test]
    fn removing_an_outer_root_hands_tracks_to_configured_inner_roots() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let outer = test_dir("remove-outer");
        fs::create_dir_all(outer.join("inner/deepest")).expect("mkdir inner roots");
        let outer = outer.canonicalize().expect("canonical outer");
        let inner = outer.join("inner");
        let deepest = inner.join("deepest");
        write_stub(&outer.join("outer.mp3"), b"outer");
        write_stub(&inner.join("inner.mp3"), b"inner");
        write_stub(&deepest.join("deep.mp3"), b"deep");
        // Before nested roots were rejected, the outer root claimed every
        // track, and the inner roots were added without being rescanned.
        scan(&conn, &outer);
        insert_root(&conn, &inner, "").expect("legacy inner root");
        insert_root(&conn, &deepest, "").expect("legacy innermost root");
        insert_legacy_track(&conn, &inner.join("legacy.mp3"));

        remove_root_and_purge(&conn, &outer).expect("remove outer");

        assert_eq!(
            indexed_paths(&conn),
            [
                deepest.join("deep.mp3"),
                inner.join("inner.mp3"),
                inner.join("legacy.mp3"),
            ]
        );
        assert_eq!(
            root_of(&conn, &inner.join("inner.mp3")),
            inner.to_string_lossy()
        );
        assert_eq!(
            root_of(&conn, &inner.join("legacy.mp3")),
            inner.to_string_lossy()
        );
        assert_eq!(
            root_of(&conn, &deepest.join("deep.mp3")),
            deepest.to_string_lossy()
        );
        let roots: Vec<PathBuf> = load_roots(&conn).into_iter().map(|r| r.path).collect();
        assert_eq!(roots, [inner.clone(), deepest]);

        // The inner root's own scan keeps the tracks it was handed.
        let outcome = scan(&conn, &inner);
        assert_eq!(
            outcome.removed,
            [inner.join("legacy.mp3").to_string_lossy().to_string()]
        );
        assert_eq!(indexed_paths(&conn).len(), 2);
        let _ = fs::remove_dir_all(outer);
    }

    #[test]
    fn an_inner_root_claiming_tracks_replaces_their_snapshot_entries() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let outer = test_dir("claim-snapshot");
        fs::create_dir_all(outer.join("inner")).expect("mkdir inner");
        let outer = outer.canonicalize().expect("canonical outer");
        let inner = outer.join("inner");
        write_stub(&outer.join("outer.mp3"), b"outer");
        write_stub(&inner.join("one.mp3"), b"one");
        write_stub(&inner.join("two.mp3"), b"two");
        scan(&conn, &outer);
        insert_root(&conn, &inner, "").expect("legacy inner root");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);
        let (tx, _rx) = unbounded();

        handle_rescan_root(&conn, &inner, &mut snapshot, &tx, &mut Uninterrupted)
            .expect("rescan inner");

        let expected = [
            inner.join("one.mp3"),
            inner.join("two.mp3"),
            outer.join("outer.mp3"),
        ];
        assert_eq!(indexed_paths(&conn), expected);
        assert_eq!(snapshot_paths(&snapshot), expected);
        for track in snapshot.tracks() {
            assert_eq!(
                track.root_path.to_string_lossy(),
                root_of(&conn, &track.path),
                "{}",
                track.path.display()
            );
        }
        let _ = fs::remove_dir_all(outer);
    }

    #[test]
    fn a_failed_track_write_rolls_back_its_root_and_is_reported() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let failing = test_dir("write-fails-a");
        let healthy = test_dir("write-fails-b");
        fs::create_dir_all(&failing).expect("mkdir failing");
        fs::create_dir_all(&healthy).expect("mkdir healthy");
        let failing = failing.canonicalize().expect("canonical failing");
        let healthy = healthy.canonicalize().expect("canonical healthy");
        write_stub(&failing.join("changed.mp3"), b"old");
        scan(&conn, &failing);
        write_stub(&failing.join("changed.mp3"), b"changed");
        for name in ["new-1.mp3", "rejected.mp3", "new-2.mp3"] {
            write_stub(&failing.join(name), name.as_bytes());
        }
        write_stub(&healthy.join("healthy.mp3"), b"healthy");
        insert_root(&conn, &healthy, "").expect("healthy root");
        // Stands in for any statement failure, such as a locked database.
        conn.execute_batch(
            "CREATE TEMP TRIGGER reject_track BEFORE INSERT ON tracks \
             WHEN NEW.path LIKE '%/rejected.mp3' \
             BEGIN SELECT RAISE(ABORT, 'injected write failure'); END",
        )
        .expect("inject failure");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);
        let (tx, rx) = unbounded();

        handle_command(&conn, LibraryCommand::RescanAll, &mut snapshot, &tx);

        let published = last_published(&rx);
        let error = published.status.last_error.as_deref().unwrap_or_default();
        assert!(
            error.contains(&format!("failed to scan '{}'", failing.display()))
                && error.contains("injected write failure"),
            "unexpected error: {error}"
        );
        let expected = [failing.join("changed.mp3"), healthy.join("healthy.mp3")];
        assert_eq!(indexed_paths(&conn), expected);
        assert_eq!(published.paths(), expected);
        let size: i64 = conn
            .query_row(
                "SELECT size_bytes FROM tracks WHERE path=?1",
                params![failing.join("changed.mp3").to_string_lossy()],
                |row| row.get(0),
            )
            .expect("changed track");
        assert_eq!(size, 3, "the re-indexed track was rolled back");
        let _ = fs::remove_dir_all(failing);
        let _ = fs::remove_dir_all(healthy);
    }

    #[test]
    fn a_failed_stale_track_removal_keeps_the_scanned_rows_and_is_reported() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("stale-delete-fails");
        fs::create_dir_all(&root).expect("mkdir root");
        let root = root.canonicalize().expect("canonical root");
        write_stub(&root.join("kept.mp3"), b"kept");
        write_stub(&root.join("deleted.mp3"), b"deleted");
        scan(&conn, &root);
        fs::remove_file(root.join("deleted.mp3")).expect("delete track");
        write_stub(&root.join("added.mp3"), b"added");
        conn.execute_batch(
            "CREATE TEMP TRIGGER reject_delete BEFORE DELETE ON tracks \
             BEGIN SELECT RAISE(ABORT, 'injected delete failure'); END",
        )
        .expect("inject failure");
        let mut snapshot = WorkerLibrary::default();
        snapshot.reload(&conn);
        let (tx, rx) = unbounded();

        handle_command(
            &conn,
            LibraryCommand::RescanRoot(root.clone()),
            &mut snapshot,
            &tx,
        );

        let published = last_published(&rx);
        let error = published.status.last_error.as_deref().unwrap_or_default();
        assert!(
            error.contains("injected delete failure"),
            "unexpected error: {error}"
        );
        // The scanned rows stay in their committed batch; nothing was removed.
        let expected = [
            root.join("added.mp3"),
            root.join("deleted.mp3"),
            root.join("kept.mp3"),
        ];
        assert_eq!(indexed_paths(&conn), expected);
        assert_eq!(published.paths(), expected);
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn collects_all_repeated_and_credit_tags() {
        use lofty::tag::{ItemValue, Tag, TagItem, TagType};
        let mut tag = Tag::new(TagType::VorbisComments);
        for (key, value) in [
            (ItemKey::TrackArtist, "First"),
            (ItemKey::TrackArtist, "Second"),
            (ItemKey::Genre, "Rock"),
            (ItemKey::Genre, "Ambient"),
            (ItemKey::Composer, "Composer"),
            (ItemKey::Conductor, "Conductor"),
            (ItemKey::Performer, "Performer"),
            (ItemKey::Label, "Label"),
            (ItemKey::AlbumArtist, "Album Artist"),
            (ItemKey::Comment, "Comment"),
            (ItemKey::Lyrics, "Lyrics"),
        ] {
            tag.push_unchecked(TagItem::new(key, ItemValue::Text(value.into())));
        }
        let mut tags = super::super::SearchTags::default();
        collect_search_tags(&tag, &mut tags);
        collect_search_tags(&tag, &mut tags);
        assert_eq!(tags["artist"], ["First", "Second"]);
        assert_eq!(tags["genre"], ["Rock", "Ambient"]);
        for key in [
            "composer",
            "conductor",
            "performer",
            "label",
            "albumartist",
            "comment",
            "lyrics",
        ] {
            assert_eq!(tags[key].len(), 1, "{key}");
        }
    }

    #[test]
    fn search_tags_round_trip_and_backfill_without_file_changes() {
        use lofty::config::WriteOptions;
        use lofty::tag::{ItemValue, Tag, TagExt, TagItem, TagType};
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("search-tags");
        fs::create_dir_all(&root).expect("directory");
        let path = root.join("song.wav");
        let mut wave = Vec::new();
        wave.extend_from_slice(b"RIFF");
        wave.extend_from_slice(&2036_u32.to_le_bytes());
        wave.extend_from_slice(b"WAVEfmt ");
        wave.extend_from_slice(&16_u32.to_le_bytes());
        wave.extend_from_slice(&1_u16.to_le_bytes());
        wave.extend_from_slice(&1_u16.to_le_bytes());
        wave.extend_from_slice(&1000_u32.to_le_bytes());
        wave.extend_from_slice(&2000_u32.to_le_bytes());
        wave.extend_from_slice(&2_u16.to_le_bytes());
        wave.extend_from_slice(&16_u16.to_le_bytes());
        wave.extend_from_slice(b"data");
        wave.extend_from_slice(&2000_u32.to_le_bytes());
        wave.resize(2044, 0);
        write_stub(&path, &wave);
        let mut tag = Tag::new(TagType::Id3v2);
        tag.set_title("Song".into());
        tag.insert_text(ItemKey::AlbumArtist, "Orchestra".into());
        tag.insert_text(ItemKey::Comment, "Anniversary remaster".into());
        tag.push_unchecked(TagItem::new(
            ItemKey::TrackArtist,
            ItemValue::Text("First".into()),
        ));
        tag.push_unchecked(TagItem::new(
            ItemKey::TrackArtist,
            ItemValue::Text("Second".into()),
        ));
        tag.save_to_path(&path, WriteOptions::new())
            .expect("tag fixture");
        scan_root(&conn, &root, &mut |_| {}, &mut |_, _| {}).expect("scan");
        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        let tags = snapshot.tracks[0].search_tags.clone();
        assert_eq!(tags["albumartist"], ["Orchestra"]);
        assert_eq!(tags["comment"], ["Anniversary remaster"]);
        assert!(tags["artist"].iter().any(|value| value.contains("Second")));
        let indexed = read_track_info(&path);
        let fingerprint = track_file_fingerprint(&path).expect("fingerprint");
        store_external_track_cache_in_conn(&conn, &path, fingerprint, &indexed).expect("cache");
        assert_eq!(
            load_external_track_cache_from_conn(&conn, &path, fingerprint)
                .expect("cached")
                .search_tags,
            tags
        );
        conn.execute("UPDATE tracks SET search_tags=NULL", [])
            .expect("simulate legacy rows");
        assert!(super::super::schema::needs_search_tag_backfill(&conn));
        let (tx, _rx) = unbounded();
        backfill_search_tags(&conn, &mut WorkerLibrary::default(), &tx).expect("backfill");
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks[0].search_tags, tags);
        assert!(!super::super::schema::needs_search_tag_backfill(&conn));
        assert_eq!(track_file_fingerprint(&path), Some(fingerprint));
        let mut updates = 0;
        scan_root(&conn, &root, &mut |_| {}, &mut |_, _| updates += 1).expect("unchanged rescan");
        assert_eq!(updates, 0);
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn search_tag_backfill_preserves_offline_roots() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let root = test_dir("offline-root");
        conn.execute("INSERT INTO tracks(path,root_path,title,artist,album,mtime_ns,size_bytes,indexed_at) VALUES (?1,?2,'Song','Artist','Album',0,0,0)",
            params![root.join("song.flac").to_string_lossy(), root.to_string_lossy()]).expect("legacy offline row");
        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        let (tx, _rx) = unbounded();
        backfill_search_tags(&conn, &mut WorkerLibrary::default(), &tx).expect("skip offline root");
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 1);
        assert!(super::super::schema::needs_search_tag_backfill(&conn));
    }
    #[test]
    fn backfill_reads_without_write_locks_and_preserves_concurrent_edits() {
        let root = test_dir("backfill-locks");
        fs::create_dir_all(&root).expect("directory");
        let db = root.join("library.sqlite3");
        let conn = Connection::open(&db).expect("db");
        init_schema(&conn).expect("schema");
        conn.execute("INSERT INTO tracks(path,root_path,title,artist,album,mtime_ns,size_bytes,indexed_at) VALUES ('/fixture/song','/fixture','Song','Artist','Album',0,0,0)", []).expect("legacy row");
        let other = Connection::open(&db).expect("writer");
        other.busy_timeout(Duration::ZERO).expect("no lock waits");
        let mut snapshot = LibrarySnapshot::default();
        load_snapshot(&conn, &mut snapshot);
        let (tx, _rx) = unbounded();
        let edited = super::super::SearchTags::from([("comment".into(), vec!["Edited".into()])]);
        backfill_search_tags_using(&conn, &mut WorkerLibrary::default(), &tx, |_| {
            other
                .execute(
                    "UPDATE tracks SET search_tags=?1",
                    params![serde_json::to_string(&edited).expect("json")],
                )
                .expect("tag edit can write during metadata read");
            Some(Ok(super::super::SearchTags::from([(
                "comment".into(),
                vec!["Old".into()],
            )])))
        })
        .expect("backfill");
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks[0].search_tags, edited);
        drop(other);
        drop(conn);
        fs::remove_dir_all(root).expect("cleanup");
    }

    fn rename_fixture(name: &str, titles: &[&str]) -> (PathBuf, Vec<PathBuf>) {
        let dir = test_dir(name);
        fs::create_dir_all(&dir).expect("create test dir");
        let dir = dir.canonicalize().expect("canonical test dir");
        let paths = titles
            .iter()
            .enumerate()
            .map(|(index, title)| {
                let path = dir.join(format!("{:02} - {title}.flac", index + 1));
                crate::tag_write::write_test_flac(&path, &[("TITLE", title)]);
                path
            })
            .collect();
        (dir, paths)
    }

    /// `(rowid, path, root_path, title)` for every library row, by path.
    fn track_rows(conn: &Connection) -> Vec<(i64, PathBuf, PathBuf, String)> {
        let mut stmt = conn
            .prepare("SELECT rowid, path, root_path, title FROM tracks ORDER BY path")
            .expect("prepare");
        stmt.query_map([], |row| {
            Ok((
                row.get(0)?,
                PathBuf::from(row.get::<_, String>(1)?),
                PathBuf::from(row.get::<_, String>(2)?),
                row.get(3)?,
            ))
        })
        .expect("query")
        .collect::<Result<_, _>>()
        .expect("rows")
    }

    fn file_names(dir: &Path) -> Vec<String> {
        let mut names = fs::read_dir(dir)
            .expect("read dir")
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

    fn rename_conn() -> Connection {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        conn
    }

    /// Set by the busy handler of `rename_waits_for_another_writer`.
    static RENAME_WAITED_FOR_LOCK: AtomicBool = AtomicBool::new(false);

    fn note_rename_lock_wait(_attempt: i32) -> bool {
        RENAME_WAITED_FOR_LOCK.store(true, Ordering::SeqCst);
        std::thread::yield_now();
        true
    }

    #[test]
    fn rename_waits_for_another_writer_instead_of_failing() {
        let (dir, paths) = rename_fixture("rename-busy", &["One", "Two"]);
        let db = dir.join("library.sqlite3");
        let mut conn = Connection::open(&db).expect("db");
        init_schema(&conn).expect("schema");
        insert_indexed_tracks(&mut conn, &dir, &paths);
        RENAME_WAITED_FOR_LOCK.store(false, Ordering::SeqCst);
        conn.busy_handler(Some(note_rename_lock_wait))
            .expect("busy handler");
        // Another writer, such as a library scan, holds the write lock until
        // the rename waits for it.
        let writer = Connection::open(&db).expect("writer");
        writer.execute_batch("BEGIN IMMEDIATE").expect("write lock");
        let holder = std::thread::spawn(move || {
            let deadline = Instant::now() + Duration::from_secs(10);
            while !RENAME_WAITED_FOR_LOCK.load(Ordering::SeqCst) && Instant::now() < deadline {
                std::thread::yield_now();
            }
            writer
                .execute_batch("ROLLBACK")
                .expect("release write lock");
        });

        let renamed = dir.join("03 - Renamed.flac");
        let result = rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[(paths[0].clone(), renamed.clone())],
        );
        holder.join().expect("lock holder");

        // A transaction that reads before it writes cannot wait for the lock,
        // and fails with "database is locked".
        let refreshed = result.expect("rename waits for the write lock");
        assert!(RENAME_WAITED_FOR_LOCK.load(Ordering::SeqCst));
        assert_eq!(refreshed.indexed[&renamed].title, "One");
        assert_eq!(
            file_names(&dir)[..2],
            ["02 - Two.flac", "03 - Renamed.flac"]
        );
        drop(conn);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_swaps_indexed_tracks_and_keeps_their_rows() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-swap", &["One", "Two"]);
        let (one, two) = (paths[0].clone(), paths[1].clone());
        insert_indexed_tracks(&mut conn, &dir, &paths);
        let before = track_rows(&conn);

        let refreshed = rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[(one.clone(), two.clone()), (two.clone(), one.clone())],
        )
        .expect("swap");

        assert_eq!(refreshed.indexed[&one].title, "Two");
        assert_eq!(refreshed.indexed[&two].title, "One");
        // Each row follows its file: same rowid and root, new path.
        assert_eq!(
            track_rows(&conn),
            [
                (before[1].0, one.clone(), dir.clone(), "Two".to_string()),
                (before[0].0, two.clone(), dir.clone(), "One".to_string()),
            ]
        );
        assert_eq!(file_names(&dir), ["01 - One.flac", "02 - Two.flac"]);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_rotates_a_cycle_of_indexed_tracks() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-cycle", &["One", "Two", "Three"]);
        insert_indexed_tracks(&mut conn, &dir, &paths);
        let before = track_rows(&conn);
        let renames = (0..3)
            .map(|index| (paths[index].clone(), paths[(index + 1) % 3].clone()))
            .collect::<Vec<_>>();

        rename_indexed_metadata_paths_in_conn(&mut conn, &renames).expect("rotate");

        assert_eq!(
            track_rows(&conn),
            [
                (
                    before[2].0,
                    paths[0].clone(),
                    dir.clone(),
                    "Three".to_string()
                ),
                (
                    before[0].0,
                    paths[1].clone(),
                    dir.clone(),
                    "One".to_string()
                ),
                (
                    before[1].0,
                    paths[2].clone(),
                    dir.clone(),
                    "Two".to_string()
                ),
            ]
        );
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_swaps_external_track_cache_rows() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-external", &["One", "Two"]);
        let (one, two) = (paths[0].clone(), paths[1].clone());
        for path in &paths {
            let fingerprint = track_file_fingerprint(path).expect("fingerprint");
            store_external_track_cache_in_conn(&conn, path, fingerprint, &read_track_info(path))
                .expect("cache row");
        }

        rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[(one.clone(), two.clone()), (two.clone(), one.clone())],
        )
        .expect("swap");

        let mut stmt = conn
            .prepare("SELECT path, title FROM external_tracks ORDER BY path")
            .expect("prepare");
        let rows = stmt
            .query_map([], |row| {
                Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
            })
            .expect("query")
            .collect::<Result<Vec<_>, _>>()
            .expect("rows");
        assert_eq!(
            rows,
            [
                (one.to_string_lossy().into_owned(), "Two".to_string()),
                (two.to_string_lossy().into_owned(), "One".to_string()),
            ]
        );
        drop(stmt);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_restores_files_when_a_database_update_fails() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-db-failure", &["One", "Two"]);
        let (one, two) = (paths[0].clone(), paths[1].clone());
        insert_indexed_tracks(&mut conn, &dir, &paths);
        conn.execute(
            &format!(
                "CREATE TRIGGER fail_rename BEFORE UPDATE OF path ON tracks \
                 WHEN new.path = '{}' BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                two.to_string_lossy()
            ),
            [],
        )
        .expect("failure trigger");
        let before = track_rows(&conn);
        let bytes = paths
            .iter()
            .map(|path| fs::read(path).expect("read"))
            .collect::<Vec<_>>();

        let error = rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[(one.clone(), two.clone()), (two.clone(), one.clone())],
        )
        .expect_err("injected failure");

        assert!(error.contains("injected failure"), "{error}");
        assert!(
            error.ends_with("the files keep their original names"),
            "{error}"
        );
        assert_eq!(fs::read(&one).expect("read"), bytes[0]);
        assert_eq!(fs::read(&two).expect("read"), bytes[1]);
        assert_eq!(file_names(&dir), ["01 - One.flac", "02 - Two.flac"]);
        assert_eq!(track_rows(&conn), before);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_restores_staged_files_when_a_later_target_is_taken() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-staged-conflict", &["One", "Two", "Three"]);
        let taken = dir.join("taken.flac");
        write_stub(&taken, b"unrelated");
        let bytes = paths
            .iter()
            .map(|path| fs::read(path).expect("read"))
            .collect::<Vec<_>>();

        let error = rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[
                (paths[0].clone(), paths[1].clone()),
                (paths[1].clone(), paths[0].clone()),
                (paths[2].clone(), taken.clone()),
            ],
        )
        .expect_err("taken target");

        assert!(
            error.starts_with("refusing to overwrite existing file"),
            "{error}"
        );
        for (path, original) in paths.iter().zip(&bytes) {
            assert_eq!(&fs::read(path).expect("read"), original);
        }
        assert_eq!(fs::read(&taken).expect("read"), b"unrelated");
        assert_eq!(
            file_names(&dir),
            [
                "01 - One.flac",
                "02 - Two.flac",
                "03 - Three.flac",
                "taken.flac"
            ]
        );
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_restore_reports_files_it_cannot_put_back() {
        let (dir, paths) = rename_fixture("rename-restore-failure", &["One"]);
        let moved = dir.join("02 - Moved.flac");
        let original = fs::read(&paths[0]).expect("read");
        let renames = [(paths[0].clone(), moved.clone())];
        let mut journal = DiskRenameJournal::new(&renames);
        journal.rename(0, &paths[0], &moved).expect("rename");
        write_stub(&paths[0], b"newcomer");

        let error = journal.restore("injected failure".to_string());

        assert!(
            error.starts_with("injected failure; restoring the original file names failed ("),
            "{error}"
        );
        assert!(
            error.ends_with(&format!(
                "{} is now at {}",
                paths[0].to_string_lossy(),
                moved.to_string_lossy()
            )),
            "{error}"
        );
        assert_eq!(fs::read(&paths[0]).expect("read"), b"newcomer");
        assert_eq!(fs::read(&moved).expect("read"), original);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    /// Creates `blocker` at `path`: a file with `bytes`, or a dangling
    /// symbolic link when `bytes` is `None`.
    fn create_blocker(path: &Path, bytes: Option<&[u8]>) {
        match bytes {
            Some(bytes) => write_stub(path, bytes),
            None => std::os::unix::fs::symlink(path.with_extension("missing"), path)
                .expect("dangling symlink"),
        }
    }

    fn assert_blocker(path: &Path, bytes: Option<&[u8]>) {
        match bytes {
            Some(bytes) => assert_eq!(fs::read(path).expect("read blocker"), bytes),
            None => assert!(fs::symlink_metadata(path)
                .expect("blocker link")
                .is_symlink()),
        }
    }

    /// A journal that creates a blocker at `blocked` just before the first
    /// rename to that name.
    fn journal_blocking(
        renames: &[(PathBuf, PathBuf)],
        blocked: &Path,
        bytes: Option<&'static [u8]>,
    ) -> DiskRenameJournal {
        let mut journal = DiskRenameJournal::new(renames);
        let blocked = blocked.to_path_buf();
        let mut created = false;
        journal.before_move = Some(Box::new(move |_from: &Path, to: &Path| {
            if to == blocked && !created {
                created = true;
                create_blocker(&blocked, bytes);
            }
        }));
        journal
    }

    #[test]
    fn rename_refuses_a_different_file_whose_name_differs_only_in_case() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-case-collision", &["One", "Two", "Three"]);
        let lower = dir.join("01 - one.flac");
        crate::tag_write::write_test_flac(&lower, &[("TITLE", "Other")]);
        if file_names(&dir).len() != 4 {
            eprintln!("skipping: {} is case-insensitive", dir.display());
            fs::remove_dir_all(dir).expect("cleanup");
            return;
        }
        insert_indexed_tracks(&mut conn, &dir, &paths);
        let before = track_rows(&conn);
        let bytes = [&paths[0], &lower].map(|path| fs::read(path).expect("read"));

        // On its own, and with a swap that stages every rename.
        for renames in [
            vec![(paths[0].clone(), lower.clone())],
            vec![
                (paths[0].clone(), lower.clone()),
                (paths[1].clone(), paths[2].clone()),
                (paths[2].clone(), paths[1].clone()),
            ],
        ] {
            let error = rename_indexed_metadata_paths_in_conn(&mut conn, &renames)
                .expect_err("case collision");

            assert!(
                error.starts_with(&format!(
                    "refusing to overwrite existing file {}",
                    lower.to_string_lossy()
                )),
                "{error}"
            );
            assert_eq!(fs::read(&paths[0]).expect("read"), bytes[0]);
            assert_eq!(fs::read(&lower).expect("read"), bytes[1]);
            assert_eq!(
                file_names(&dir),
                [
                    "01 - One.flac",
                    "01 - one.flac",
                    "02 - Two.flac",
                    "03 - Three.flac"
                ]
            );
            assert_eq!(track_rows(&conn), before);
        }
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_changes_only_the_case_of_a_name() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-case-only", &["One"]);
        insert_indexed_tracks(&mut conn, &dir, &paths);
        let before = track_rows(&conn);
        let bytes = fs::read(&paths[0]).expect("read");
        let upper = dir.join("01 - ONE.flac");

        rename_indexed_metadata_paths_in_conn(&mut conn, &[(paths[0].clone(), upper.clone())])
            .expect("case-only rename");

        assert_eq!(file_names(&dir), ["01 - ONE.flac"]);
        assert_eq!(fs::read(&upper).expect("read"), bytes);
        assert_eq!(
            track_rows(&conn),
            [(before[0].0, upper, dir.clone(), "One".to_string())]
        );
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_through_temp_moves_a_file_in_two_steps_and_restores_it() {
        let (dir, paths) = rename_fixture("rename-through-temp", &["One"]);
        let bytes = fs::read(&paths[0]).expect("read");
        let renamed = dir.join("01 - ONE.flac");
        let renames = [(paths[0].clone(), renamed.clone())];
        let mut journal = DiskRenameJournal::new(&renames);

        journal
            .rename_through_temp(0, &paths[0], &renamed)
            .expect("two-step rename");

        assert_eq!(file_names(&dir), ["01 - ONE.flac"]);
        assert_eq!(fs::read(&renamed).expect("read"), bytes);
        assert_eq!(journal.moves.len(), 2);
        let error = journal.restore("injected failure".to_string());
        assert_eq!(
            error,
            "injected failure; the files keep their original names"
        );
        assert_eq!(file_names(&dir), ["01 - One.flac"]);
        assert_eq!(fs::read(&paths[0]).expect("read"), bytes);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    /// A 240-byte file name: valid, but too long to take a prefix that pushes
    /// it past the 255-byte limit.
    fn long_file_name(letter: char) -> String {
        format!("{}.flac", letter.to_string().repeat(235))
    }

    #[test]
    fn temporary_rename_names_are_short_hidden_and_not_audio() {
        let dir = test_dir("rename-temp-name");
        let path = dir.join(long_file_name('a'));

        let first = build_temp_rename_path(&path);
        let second = build_temp_rename_path(&path);

        assert_ne!(first, second);
        for temp in [&first, &second] {
            assert_eq!(temp.parent(), Some(dir.as_path()));
            let name = temp.file_name().expect("name").to_string_lossy();
            assert!(name.starts_with(TEMP_RENAME_PREFIX), "{name}");
            assert!(name.len() < 64, "{name}");
            assert!(!is_supported_audio(temp), "{name}");
        }
    }

    #[test]
    fn rename_changes_the_case_of_a_240_byte_name_in_two_steps() {
        let (dir, _) = rename_fixture("rename-long-case", &[]);
        let (lower, upper) = (long_file_name('a'), long_file_name('A'));
        let (old_path, new_path) = (dir.join(&lower), dir.join(&upper));
        crate::tag_write::write_test_flac(&old_path, &[("TITLE", "One")]);
        let bytes = fs::read(&old_path).expect("read");
        let renames = [(old_path.clone(), new_path.clone())];
        let mut journal = DiskRenameJournal::new(&renames);

        journal
            .rename_through_temp(0, &old_path, &new_path)
            .expect("two-step rename");

        assert_eq!(file_names(&dir), [upper]);
        assert_eq!(fs::read(&new_path).expect("read"), bytes);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_swaps_tracks_with_240_byte_names() {
        let mut conn = rename_conn();
        let (dir, _) = rename_fixture("rename-long-swap", &[]);
        let (one, two) = (dir.join(long_file_name('a')), dir.join(long_file_name('b')));
        crate::tag_write::write_test_flac(&one, &[("TITLE", "One")]);
        crate::tag_write::write_test_flac(&two, &[("TITLE", "Two")]);
        insert_indexed_tracks(&mut conn, &dir, &[one.clone(), two.clone()]);

        let refreshed = rename_indexed_metadata_paths_in_conn(
            &mut conn,
            &[(one.clone(), two.clone()), (two.clone(), one.clone())],
        )
        .expect("swap");

        assert_eq!(refreshed.indexed[&one].title, "Two");
        assert_eq!(refreshed.indexed[&two].title, "One");
        assert_eq!(file_names(&dir), [long_file_name('a'), long_file_name('b')]);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_does_not_replace_another_name_of_the_same_file() {
        let mut conn = rename_conn();
        let (dir, paths) = rename_fixture("rename-alias", &["One"]);
        let alias = dir.join("alias.flac");
        std::os::unix::fs::symlink(&paths[0], &alias).expect("symlink");
        let bytes = fs::read(&paths[0]).expect("read");

        let error =
            rename_indexed_metadata_paths_in_conn(&mut conn, &[(paths[0].clone(), alias.clone())])
                .expect_err("alias");

        assert!(
            error.starts_with(&format!(
                "refusing to overwrite existing file {}",
                alias.to_string_lossy()
            )),
            "{error}"
        );
        assert!(
            error.ends_with("the files keep their original names"),
            "{error}"
        );
        assert_eq!(file_names(&dir), ["01 - One.flac", "alias.flac"]);
        assert_eq!(fs::read(&paths[0]).expect("read"), bytes);
        assert!(fs::symlink_metadata(&alias).expect("alias").is_symlink());
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn rename_never_replaces_a_file_created_at_the_target() {
        for (kind, blocker) in [("file", Some(&b"newcomer"[..])), ("symlink", None)] {
            let mut conn = rename_conn();
            let (dir, paths) =
                rename_fixture(&format!("rename-target-race-{kind}"), &["One", "Two"]);
            insert_indexed_tracks(&mut conn, &dir, &paths);
            let before = track_rows(&conn);
            let bytes = paths
                .iter()
                .map(|path| fs::read(path).expect("read"))
                .collect::<Vec<_>>();
            let target = dir.join("03 - Taken.flac");
            let mut expected_names = vec![
                "01 - One.flac".to_string(),
                "02 - Two.flac".to_string(),
                "03 - Taken.flac".to_string(),
            ];
            expected_names.sort();

            // A plain rename after another that succeeds, and the final step
            // of a staged rename.
            for renames in [
                vec![
                    (paths[0].clone(), dir.join("00 - First.flac")),
                    (paths[1].clone(), target.clone()),
                ],
                vec![
                    (paths[0].clone(), paths[1].clone()),
                    (paths[1].clone(), target.clone()),
                ],
            ] {
                let journal = journal_blocking(&renames, &target, blocker);

                let error =
                    rename_with_journal(&mut conn, &renames, journal, EmbeddedCoverCache::shared())
                        .expect_err("target taken");

                assert!(
                    error.starts_with(&format!(
                        "refusing to overwrite existing file {}",
                        target.to_string_lossy()
                    )),
                    "{kind}: {error}"
                );
                assert!(
                    error.ends_with("the files keep their original names"),
                    "{kind}: {error}"
                );
                assert_blocker(&target, blocker);
                for (path, original) in paths.iter().zip(&bytes) {
                    assert_eq!(&fs::read(path).expect("read"), original, "{kind}");
                }
                assert_eq!(file_names(&dir), expected_names, "{kind}");
                assert_eq!(track_rows(&conn), before, "{kind}");
                fs::remove_file(&target).expect("remove blocker");
            }
            fs::remove_dir_all(dir).expect("cleanup");
        }
    }

    #[test]
    fn rename_restore_never_replaces_a_file_created_at_an_original_name() {
        for (kind, blocker) in [("file", Some(&b"newcomer"[..])), ("symlink", None)] {
            let mut conn = rename_conn();
            let (dir, paths) =
                rename_fixture(&format!("rename-restore-race-{kind}"), &["One", "Two"]);
            insert_indexed_tracks(&mut conn, &dir, &paths);
            let renamed = [dir.join("03 - One.flac"), dir.join("04 - Two.flac")];
            conn.execute(
                &format!(
                    "CREATE TRIGGER fail_rename BEFORE UPDATE OF path ON tracks \
                     WHEN new.path = '{}' BEGIN SELECT RAISE(ABORT, 'injected failure'); END",
                    renamed[1].to_string_lossy()
                ),
                [],
            )
            .expect("failure trigger");
            let before = track_rows(&conn);
            let bytes = paths
                .iter()
                .map(|path| fs::read(path).expect("read"))
                .collect::<Vec<_>>();
            let renames = [
                (paths[0].clone(), renamed[0].clone()),
                (paths[1].clone(), renamed[1].clone()),
            ];
            // Only putting the first file back renames it to its original name.
            let journal = journal_blocking(&renames, &paths[0], blocker);

            let error =
                rename_with_journal(&mut conn, &renames, journal, EmbeddedCoverCache::shared())
                    .expect_err("injected failure");

            assert_eq!(
                error,
                format!(
                    "failed to update renamed track row: injected failure; \
                     restoring the original file names failed \
                     ({} already exists); {} is now at {}; \
                     the other files keep their original names",
                    paths[0].to_string_lossy(),
                    paths[0].to_string_lossy(),
                    renamed[0].to_string_lossy()
                ),
                "{kind}"
            );
            assert_blocker(&paths[0], blocker);
            assert_eq!(fs::read(&renamed[0]).expect("read"), bytes[0], "{kind}");
            assert_eq!(fs::read(&paths[1]).expect("read"), bytes[1], "{kind}");
            assert_eq!(
                file_names(&dir),
                ["01 - One.flac", "02 - Two.flac", "03 - One.flac"],
                "{kind}"
            );
            assert_eq!(track_rows(&conn), before, "{kind}");
            fs::remove_dir_all(dir).expect("cleanup");
        }
    }
}
