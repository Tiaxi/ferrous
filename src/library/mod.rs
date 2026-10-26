// SPDX-License-Identifier: GPL-3.0-or-later

mod cover_cache;
mod db;
mod file_id;
mod publish;
mod scan;
mod schema;
#[cfg(test)]
mod test_fixtures;
mod watch;
mod worker;

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::worker_thread::JoinableThread;

pub(crate) use file_id::{file_id_columns, file_id_from_columns, identity_changed};
#[cfg(test)]
pub(crate) use publish::LibraryMirror;
pub(crate) use publish::{AppliedChanges, LibraryChanges};
#[cfg(test)]
pub(crate) use scan::scan_test_hooks::{hold_read, panic_on_read};
#[cfg(test)]
pub(crate) use scan::{index_tracks_for_test, load_external_track_caches, write_test_apple_double};
pub(crate) use scan::{
    is_audio_track_file, is_supported_audio, read_external_track, read_track_info,
    refresh_cover_paths_for_tracks, refresh_cover_paths_for_tracks_with_override,
    refresh_indexed_metadata_for_paths, rename_indexed_metadata_paths, track_file_fingerprint,
    TrackRefresh,
};
pub(crate) use scan::{read_library_snapshot_from_db, ExternalReadFailures, ExternalTrackCache};

/// Opens the library database, which also holds the analysis worker's
/// waveform cache, and brings its schema up to date as every connection
/// does. `busy_timeout` applies from then on; the migration may wait longer
/// for another connection's.
pub(crate) fn open_library_database(
    busy_timeout: Duration,
) -> anyhow::Result<rusqlite::Connection> {
    let conn = schema::open_library_db()?;
    schema::init_schema(&conn)?;
    conn.busy_timeout(busy_timeout)?;
    Ok(conn)
}

/// Brings the schema of the library database `conn` is open on up to date.
#[cfg(test)]
pub(crate) fn init_library_schema(conn: &rusqlite::Connection) -> anyhow::Result<()> {
    schema::init_schema(conn)
}

/// Additional, potentially repeated text tags collected during library scans.
pub type SearchTags = std::collections::BTreeMap<String, Vec<String>>;

#[derive(Debug, Clone, Default, PartialEq)]
pub struct LibraryTrack {
    pub search_tags: SearchTags,
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_path: String,
    /// The stamp of the folder images `cover_path` was chosen from; see
    /// [`crate::folder_cover::FolderImages::stamp`]. It changes when the
    /// image at `cover_path` is replaced, so views key cached images on it.
    pub cover_stamp: u64,
    /// When the cover was chosen; see [`CoverReadAt`].
    pub cover_read_at: CoverReadAt,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
}

/// When a cover choice was read from disk. Choices reach the bridge from
/// the library worker, artwork apply and the metadata worker, each after
/// its own delay, so the bridge orders them by this, never by arrival.
///
/// It says when, not what: rows that differ only here are equal, so a scan
/// that reads a file again and gets the same row publishes nothing.
#[derive(Debug, Clone, Copy, Default)]
pub struct CoverReadAt(pub Option<std::time::Instant>);

impl PartialEq for CoverReadAt {
    fn eq(&self, _other: &Self) -> bool {
        true
    }
}

impl Eq for CoverReadAt {}

impl CoverReadAt {
    /// Taken before the folder or file is read.
    #[must_use]
    pub fn now() -> Self {
        Self(Some(std::time::Instant::now()))
    }

    /// Whether a cover read at `self` may replace one read at `shown`: it
    /// was read no earlier. A time not known, such as of a row loaded from
    /// the database, is older than any known one.
    #[must_use]
    pub fn is_not_older_than(self, shown: Self) -> bool {
        match (self.0, shown.0) {
            (Some(read), Some(shown)) => read >= shown,
            (None, Some(_)) => false,
            (_, None) => true,
        }
    }
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
    pub cover_stamp: u64,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
    pub score: f32,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct LibraryScanProgress {
    pub current_root: Option<PathBuf>,
    pub roots_completed: usize,
    pub roots_total: usize,
    pub supported_files_discovered: usize,
    pub supported_files_processed: usize,
    pub files_per_second: Option<f32>,
    pub eta_seconds: Option<f32>,
}

/// The library's roots and tracks. Tracks are shared, so a copy of the
/// snapshot copies only pointers, and a change replaces only its tracks.
#[derive(Debug, Clone, Default)]
pub struct LibrarySnapshot {
    pub roots: Vec<LibraryRoot>,
    pub tracks: Vec<Arc<LibraryTrack>>,
    /// Advances whenever the roots or tracks change.
    pub search_revision: u64,
}

impl LibrarySnapshot {
    /// A snapshot of `roots` and `tracks`.
    #[must_use]
    pub fn new(roots: Vec<LibraryRoot>, tracks: Vec<LibraryTrack>) -> Self {
        Self {
            roots,
            tracks: tracks.into_iter().map(Arc::new).collect(),
            search_revision: 0,
        }
    }

    fn bump_search_revision(&mut self) {
        self.search_revision = self.search_revision.saturating_add(1);
    }
}

/// The library in the database at `path`, as the worker reads it, for tests.
#[cfg(test)]
pub(crate) fn schema_load_snapshot_for_test(
    path: &std::path::Path,
    snapshot: &mut LibrarySnapshot,
) {
    let conn = rusqlite::Connection::open(path).expect("open the test library");
    schema::load_snapshot(&conn, snapshot);
}

/// `tracks` as a snapshot holds them, for tests.
#[cfg(test)]
pub(crate) fn shared_tracks(tracks: Vec<LibraryTrack>) -> Vec<Arc<LibraryTrack>> {
    tracks.into_iter().map(Arc::new).collect()
}

/// What the library worker is doing, apart from the rows.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LibraryStatus {
    pub scan_in_progress: bool,
    pub scan_progress: Option<LibraryScanProgress>,
    pub last_error: Option<String>,
    /// The roots that changes on disk are not watched for, sorted by path.
    pub unwatched_roots: Vec<UnwatchedRoot>,
    /// The problems the latest scan of each root found, sorted by path.
    /// A root's problem lasts until a scan of the whole root finds none.
    pub root_problems: Vec<RootProblem>,
}

impl LibraryStatus {
    /// Record what the latest scan of `root` found: `problem`, or none.
    pub(crate) fn set_root_problem(&mut self, root: &std::path::Path, problem: Option<String>) {
        let index = self
            .root_problems
            .binary_search_by(|known| known.path.as_path().cmp(root));
        match (index, problem) {
            (Ok(index), Some(problem)) => self.root_problems[index].problem = problem,
            (Ok(index), None) => {
                self.root_problems.remove(index);
            }
            (Err(index), Some(problem)) => self.root_problems.insert(
                index,
                RootProblem {
                    path: root.to_path_buf(),
                    problem,
                },
            ),
            (Err(_), None) => {}
        }
    }

    /// Forget the problems of roots that are not in `roots`.
    pub(crate) fn retain_root_problems(&mut self, roots: &[LibraryRoot]) {
        self.root_problems
            .retain(|known| roots.iter().any(|root| root.path == known.path));
    }
}

/// What the latest scan of a library root could not do, for the user.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RootProblem {
    pub path: PathBuf,
    pub problem: String,
}

/// A library root whose changes on disk the library does not see as they
/// happen, and that it rescans instead.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnwatchedRoot {
    pub path: PathBuf,
    /// Why, and what the library does instead, for the user.
    pub reason: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TrackFileFingerprint {
    pub(crate) mtime_ns: i64,
    pub(crate) size_bytes: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LibraryCommand {
    ScanRoot(PathBuf),
    AddRoot {
        path: PathBuf,
        name: String,
    },
    RenameRoot {
        path: PathBuf,
        name: String,
    },
    RemoveRoot(PathBuf),
    RescanRoot(PathBuf),
    RescanAll,
    /// Read the rows of these paths again and publish them whatever the
    /// worker last published, so that a copy of the library that applied
    /// changes out of order matches the database again.
    ReadRows(Vec<PathBuf>),
    /// Scan these files and folders below `root` again: they changed on
    /// disk. Sent by the library's watcher.
    ScanPaths {
        root: PathBuf,
        paths: Vec<PathBuf>,
    },
    /// The roots the watcher does not watch; published as
    /// [`LibraryStatus::unwatched_roots`].
    SetUnwatchedRoots(Vec<UnwatchedRoot>),
}

static FILE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

/// The next number of one process-wide sequence that orders reads of the
/// files against renames: a rename takes its number after it has read the
/// renamed files, and a read takes one before it begins. The bridge takes a
/// number for each edit refresh request it sends, and the library worker one
/// for each command it runs and each root its scans read.
pub(crate) fn next_file_sequence() -> u64 {
    FILE_SEQUENCE.fetch_add(1, Ordering::SeqCst) + 1
}

/// What changed in the library since the worker's previous event. The
/// first event has every row.
#[derive(Debug, Clone, Default)]
pub struct LibraryEvent {
    pub changes: LibraryChanges,
    pub status: LibraryStatus,
    /// A [`next_file_sequence`] number taken before the rows in `changes`
    /// were read, from the files or the database.
    pub read_after: u64,
}

#[cfg(test)]
impl LibraryEvent {
    /// An event that adds `library`'s roots and tracks, as a worker's first
    /// event does.
    pub(crate) fn with_library(library: LibrarySnapshot) -> Self {
        Self {
            changes: LibraryChanges {
                roots: Some(library.roots),
                upserted: library.tracks,
                removed: Vec::new(),
            },
            status: LibraryStatus::default(),
            read_after: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct IndexedTrack {
    pub(crate) search_tags: SearchTags,
    pub(crate) title: String,
    pub(crate) artist: String,
    pub(crate) album: String,
    pub(crate) cover_path: String,
    /// See [`LibraryTrack::cover_stamp`].
    pub(crate) cover_stamp: u64,
    /// See [`CoverReadAt`].
    pub(crate) cover_read_at: CoverReadAt,
    pub(crate) genre: String,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
    pub(crate) duration_secs: Option<f32>,
}

impl From<&LibraryTrack> for IndexedTrack {
    fn from(track: &LibraryTrack) -> Self {
        Self {
            search_tags: track.search_tags.clone(),
            title: track.title.clone(),
            artist: track.artist.clone(),
            album: track.album.clone(),
            cover_path: track.cover_path.clone(),
            cover_stamp: track.cover_stamp,
            cover_read_at: track.cover_read_at,
            genre: track.genre.clone(),
            year: track.year,
            track_no: track.track_no,
            duration_secs: track.duration_secs,
        }
    }
}

/// How long dropping the [`LibraryService`] waits for its worker to stop a
/// scan, checkpoint the database, and close it, and for its watcher to end.
pub(crate) const LIBRARY_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);

pub struct LibraryService {
    tx: Option<Sender<LibraryCommand>>,
    /// Wakes the worker when the watching setting changes.
    watching: Sender<()>,
    thread: Option<JoinableThread>,
    watcher: watch::WatchService,
}

impl LibraryService {
    /// The library service, which watches its roots for changes once
    /// [`LibraryService::set_watching`] turns watching on.
    #[must_use]
    pub fn new() -> (Self, Receiver<LibraryEvent>) {
        let db_path = crate::paths::library_db_path()
            .map_err(|err| format!("could not open the library database: {err}"));
        Self::spawn(
            db_path,
            schema::BUSY_TIMEOUT,
            watch::WatchTuning::default(),
            || {},
        )
    }

    /// A service on the database at `path`, scanning as the app does.
    #[cfg(test)]
    pub(crate) fn at_database(path: PathBuf) -> (Self, Receiver<LibraryEvent>) {
        Self::spawn(
            Ok(path),
            schema::BUSY_TIMEOUT,
            watch::WatchTuning::default(),
            || {},
        )
    }

    /// A service on the database at `path`, whose scans use `tuning`.
    #[cfg(test)]
    fn with_database(
        path: PathBuf,
        busy_timeout: Duration,
        tuning: scan::ScanTuning,
    ) -> (Self, Receiver<LibraryEvent>) {
        Self::with_watch_tuning(path, busy_timeout, tuning, watch::WatchTuning::default())
    }

    /// A service on the database at `path`, whose scans use `tuning` and
    /// whose watcher uses `watching`.
    #[cfg(test)]
    fn with_watch_tuning(
        path: PathBuf,
        busy_timeout: Duration,
        tuning: scan::ScanTuning,
        watching: watch::WatchTuning,
    ) -> (Self, Receiver<LibraryEvent>) {
        Self::spawn(Ok(path), busy_timeout, watching, move || {
            scan::scan_test_hooks::set_tuning(tuning);
        })
    }

    /// Starts the worker, which runs `configure` first, and the watcher.
    fn spawn(
        db_path: Result<PathBuf, String>,
        busy_timeout: Duration,
        watching: watch::WatchTuning,
        configure: impl FnOnce() + Send + 'static,
    ) -> (Self, Receiver<LibraryEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<LibraryCommand>();
        let (event_tx, event_rx) = unbounded::<LibraryEvent>();
        let (watcher, changes, roots_watched) = watch::WatchService::spawn(watching);
        let roots = watcher.handle();
        let (watching_tx, watching_rx) = unbounded::<()>();

        let thread = crate::worker_thread::spawn_worker("ferrous-library", move || {
            configure();
            worker::run(
                db_path,
                busy_timeout,
                worker::Inputs {
                    commands: cmd_rx,
                    changes,
                    roots,
                    roots_watched,
                    watching: watching_rx,
                },
                &event_tx,
            );
        });

        (
            Self {
                tx: Some(cmd_tx),
                watching: watching_tx,
                thread,
                watcher,
            },
            event_rx,
        )
    }

    pub fn command(&self, cmd: LibraryCommand) {
        if let Some(tx) = &self.tx {
            let _ = tx.send(cmd);
        }
    }

    /// Turns watching the roots for changes on or off. Turning it on, the
    /// first time too, also rescans every root, so that changes made while
    /// it was off appear.
    pub fn set_watching(&self, enabled: bool) {
        self.watcher.set_enabled(enabled);
        let _ = self.watching.send(());
    }
}

impl Drop for LibraryService {
    /// Stops the watcher and closes the command channel, which stops a
    /// running scan between files, and waits a bounded time for the worker
    /// to checkpoint and close the database, and for the watcher to end.
    fn drop(&mut self) {
        self.watcher.stop();
        self.tx = None;
        let deadline = Instant::now() + LIBRARY_SHUTDOWN_TIMEOUT;
        if let Some(thread) = self.thread.take() {
            if !thread.join_by(deadline) {
                eprintln!(
                    "[ferrous] the library worker did not stop within {} s",
                    LIBRARY_SHUTDOWN_TIMEOUT.as_secs()
                );
            }
        }
        if !self.watcher.join_by(deadline) {
            eprintln!(
                "[ferrous] the library watcher did not stop within {} s",
                LIBRARY_SHUTDOWN_TIMEOUT.as_secs()
            );
        }
    }
}
