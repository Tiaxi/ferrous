// SPDX-License-Identifier: GPL-3.0-or-later

mod cover_cache;
mod db;
mod publish;
mod scan;
mod schema;
#[cfg(test)]
mod test_fixtures;
mod worker;

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::worker_thread::JoinableThread;

pub(crate) use publish::LibraryChanges;
#[cfg(test)]
pub(crate) use publish::LibraryMirror;
#[cfg(test)]
pub(crate) use scan::scan_test_hooks::hold_read;
#[cfg(test)]
pub(crate) use scan::{index_tracks_for_test, load_external_track_caches, write_test_apple_double};
pub(crate) use scan::{
    is_audio_track_file, is_supported_audio, read_external_track, read_track_info,
    refresh_cover_paths_for_tracks, refresh_cover_paths_for_tracks_with_override,
    refresh_indexed_metadata_for_paths, rename_indexed_metadata_paths, track_file_fingerprint,
    TrackRefresh,
};
pub(crate) use scan::{read_library_snapshot_from_db, ExternalTrackCache};

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
}

/// What changed in the library since the worker's previous event. The
/// first event has every row.
#[derive(Debug, Clone, Default)]
pub struct LibraryEvent {
    pub changes: LibraryChanges,
    pub status: LibraryStatus,
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
    pub(crate) genre: String,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
    pub(crate) duration_secs: Option<f32>,
}

/// How long dropping the [`LibraryService`] waits for its worker to stop a
/// scan, checkpoint the database, and close it.
pub(crate) const LIBRARY_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);

pub struct LibraryService {
    tx: Option<Sender<LibraryCommand>>,
    thread: Option<JoinableThread>,
}

impl LibraryService {
    #[must_use]
    pub fn new() -> (Self, Receiver<LibraryEvent>) {
        let db_path = crate::paths::library_db_path()
            .map_err(|err| format!("could not open the library database: {err}"));
        Self::spawn(db_path, schema::BUSY_TIMEOUT, || {})
    }

    /// A service on the database at `path`, whose scans use `tuning`.
    #[cfg(test)]
    fn with_database(
        path: PathBuf,
        busy_timeout: Duration,
        tuning: scan::ScanTuning,
    ) -> (Self, Receiver<LibraryEvent>) {
        Self::spawn(Ok(path), busy_timeout, move || {
            scan::scan_test_hooks::set_tuning(tuning);
        })
    }

    /// Starts the worker, which runs `configure` first.
    fn spawn(
        db_path: Result<PathBuf, String>,
        busy_timeout: Duration,
        configure: impl FnOnce() + Send + 'static,
    ) -> (Self, Receiver<LibraryEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<LibraryCommand>();
        let (event_tx, event_rx) = unbounded::<LibraryEvent>();

        let thread = crate::worker_thread::spawn_worker("ferrous-library", move || {
            configure();
            worker::run(db_path, busy_timeout, cmd_rx, &event_tx);
        });

        (
            Self {
                tx: Some(cmd_tx),
                thread,
            },
            event_rx,
        )
    }

    pub fn command(&self, cmd: LibraryCommand) {
        if let Some(tx) = &self.tx {
            let _ = tx.send(cmd);
        }
    }
}

impl Drop for LibraryService {
    /// Closes the command channel, which stops a running scan between
    /// files, and waits a bounded time for the worker to checkpoint and
    /// close the database.
    fn drop(&mut self) {
        self.tx = None;
        if let Some(thread) = self.thread.take() {
            if !thread.join_by(Instant::now() + LIBRARY_SHUTDOWN_TIMEOUT) {
                eprintln!(
                    "[ferrous] the library worker did not stop within {} s",
                    LIBRARY_SHUTDOWN_TIMEOUT.as_secs()
                );
            }
        }
    }
}
