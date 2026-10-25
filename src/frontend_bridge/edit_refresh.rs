// SPDX-License-Identifier: GPL-3.0-or-later

//! Reads the files the tag editor saved again, and updates their library rows,
//! on a worker, so the bridge thread, which also drives playback, never waits
//! for tag reads or the library database.
//!
//! Requests and renames are ordered by one process-wide sequence (see
//! [`next_file_sequence`]): a request takes its number when the bridge sends
//! it, so the worker reads after every rename with a lower number, and a
//! rename takes its number once it has read the files it renamed. When the
//! bridge applies a rename, a refresh with a higher number than the rename,
//! whether still in flight or already applied, read the file later than the
//! rename did, so its row wins over the rename's. A refresh with a lower
//! number may have read a file before it moved, and is ignored for the
//! rename's paths. Which order the bridge handles the rename notification,
//! the save's command, and the refresh's result in does not matter.
//!
//! The library worker's events are numbered from the same sequence, before
//! their rows were read, so a library row read after a rename also wins over
//! the rename's row, and over a refresh row the bridge applied before it.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crossbeam_channel::{unbounded, Receiver, Sender};

use super::imports::RenamedDetails;
use crate::library::{
    next_file_sequence, refresh_indexed_metadata_for_paths, track_file_fingerprint, IndexedTrack,
    LibraryChanges, LibraryTrack, TrackFileFingerprint, TrackRefresh,
};
use crate::playback::PathRenames;

struct RefreshRequest {
    sequence: u64,
    paths: Vec<PathBuf>,
}

pub(super) struct RefreshResult {
    /// The newest request the result answers. The files were read after it
    /// was sent.
    sequence: u64,
    outcome: Result<TrackRefresh, String>,
    fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
}

/// The details of a refresh that are still current.
#[derive(Debug, Default)]
pub(super) struct AcceptedRefresh {
    pub(super) indexed: HashMap<PathBuf, IndexedTrack>,
    pub(super) fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    /// What the user needs to know: the files that could not be read, or why
    /// the refresh failed.
    pub(super) problem: Option<String>,
}

/// A row the bridge applied to a path, with the sequence number it was read
/// after.
struct AppliedRow {
    sequence: u64,
    row: Row,
}

enum Row {
    /// A refresh read the file, in this state.
    Refreshed {
        indexed: IndexedTrack,
        fingerprint: Option<TrackFileFingerprint>,
    },
    /// A library event had this row.
    Library(Arc<LibraryTrack>),
    /// A library event removed the path.
    Removed,
}

pub(super) struct EditRefreshWorker {
    tx: Sender<RefreshRequest>,
    pub(super) results: Receiver<RefreshResult>,
    /// The sequence number of the last request sent.
    last_requested: u64,
    /// The sequence number of the last result accepted.
    last_accepted: u64,
    /// For each path with a refresh in flight, the lowest sequence number
    /// of a result that may update it.
    current_from: HashMap<PathBuf, u64>,
    /// The rows applied while a rename notification may still be on its
    /// way, for renames numbered below them; see [`Self::renamed`]. In the
    /// order they were applied; a row is dropped once a row read after a
    /// number as high as its own is applied after it, since that row is
    /// newer than every rename the dropped one is.
    applied: HashMap<PathBuf, Vec<AppliedRow>>,
}

impl EditRefreshWorker {
    pub(super) fn new() -> Self {
        Self::with_refresh(refresh_indexed_metadata_for_paths)
    }

    pub(super) fn with_refresh(
        refresh: impl Fn(&[PathBuf]) -> Result<TrackRefresh, String> + Send + 'static,
    ) -> Self {
        let (tx, rx) = unbounded::<RefreshRequest>();
        let (result_tx, results) = unbounded();
        crate::worker_thread::spawn_worker("ferrous-edit-refresh", move || {
            while let Ok(first) = rx.recv() {
                // Saves that queued up meanwhile are read in the same pass,
                // after the newest of them was sent.
                let mut sequence = first.sequence;
                let mut paths = first.paths;
                for next in rx.try_iter() {
                    sequence = next.sequence;
                    paths.extend(next.paths);
                }
                paths.sort_unstable();
                paths.dedup();
                let outcome = refresh(&paths);
                let fingerprints = paths
                    .iter()
                    .filter_map(|path| Some((path.clone(), track_file_fingerprint(path)?)))
                    .collect();
                let result = RefreshResult {
                    sequence,
                    outcome,
                    fingerprints,
                };
                if result_tx.send(result).is_err() {
                    break;
                }
            }
        });
        Self {
            tx,
            results,
            last_requested: 0,
            last_accepted: 0,
            current_from: HashMap::new(),
            applied: HashMap::new(),
        }
    }

    /// Start a new worker in place of one that stopped. Its requests are
    /// lost; the rows applied are kept for renames still on their way.
    pub(super) fn restart(&mut self) {
        let applied = std::mem::take(&mut self.applied);
        *self = Self::new();
        self.applied = applied;
    }

    /// Ask for `paths` to be read again. Returns the request's sequence
    /// number, or `None` if the worker has stopped.
    pub(super) fn request(&mut self, paths: Vec<PathBuf>) -> Option<u64> {
        let sequence = next_file_sequence();
        for path in &paths {
            self.current_from.insert(path.clone(), sequence);
        }
        self.last_requested = sequence;
        self.tx
            .send(RefreshRequest { sequence, paths })
            .is_ok()
            .then_some(sequence)
    }

    /// Files were renamed on disk by the rename numbered `sequence`, which
    /// read them into `details`.
    ///
    /// - A read in flight of one of their old or new paths that was asked
    ///   for before the rename may have read another file, or none, so it is
    ///   ignored. One asked for after the rename read the files later, so it
    ///   stands and updates the renamed track when it arrives.
    /// - A row already applied for a new path from a read after the rename,
    ///   by a refresh or a library event, is newer than the rename's own, so
    ///   it replaces the rename's row in `details`. Of several, the one
    ///   applied last does, as the library shows it. A path such a read
    ///   found gone is marked gone.
    pub(super) fn renamed(
        &mut self,
        renames: &PathRenames,
        sequence: u64,
        details: &mut RenamedDetails,
    ) {
        for path in renames.keys().chain(renames.values()) {
            if let Some(current_from) = self.current_from.get_mut(path) {
                *current_from = (*current_from).max(sequence);
            }
        }
        for new_path in renames.values() {
            // The rows read after higher numbers come first.
            let Some(applied) = self.applied.get(new_path).and_then(|rows| {
                rows.iter()
                    .take_while(|applied| applied.sequence > sequence)
                    .last()
            }) else {
                continue;
            };
            let (indexed, fingerprint) = match &applied.row {
                Row::Refreshed {
                    indexed,
                    fingerprint,
                } => (indexed.clone(), *fingerprint),
                Row::Library(track) => (IndexedTrack::from(&**track), None),
                Row::Removed => {
                    details.indexed.remove(new_path);
                    details.fingerprints.remove(new_path);
                    details.gone.insert(new_path.clone());
                    continue;
                }
            };
            details.indexed.insert(new_path.clone(), indexed);
            match fingerprint {
                Some(fingerprint) => details.fingerprints.insert(new_path.clone(), fingerprint),
                None => details.fingerprints.remove(new_path),
            };
        }
    }

    /// The bridge applied `changes`, read after `read_after`, while a rename
    /// notification may still be on its way.
    pub(super) fn library_changed(&mut self, changes: &LibraryChanges, read_after: u64) {
        for track in &changes.upserted {
            self.keep(
                track.path.clone(),
                read_after,
                Row::Library(Arc::clone(track)),
            );
        }
        for path in &changes.removed {
            self.keep(path.clone(), read_after, Row::Removed);
        }
    }

    fn keep(&mut self, path: PathBuf, sequence: u64, row: Row) {
        let rows = self.applied.entry(path).or_default();
        rows.retain(|applied| applied.sequence > sequence);
        rows.push(AppliedRow { sequence, row });
    }

    /// Forget the applied rows: no rename that read the files before them
    /// can still arrive.
    pub(super) fn forget_applied(&mut self) {
        self.applied.clear();
    }

    /// How many paths have rows kept for renames on their way.
    #[cfg(test)]
    pub(super) fn kept_paths(&self) -> usize {
        self.applied.len()
    }

    /// Whether every request has been answered.
    #[cfg(test)]
    pub(super) fn is_idle(&self) -> bool {
        self.last_accepted == self.last_requested
    }

    /// The part of `result` that is still current.
    pub(super) fn accept(&mut self, result: RefreshResult) -> AcceptedRefresh {
        let RefreshResult {
            sequence,
            outcome,
            mut fingerprints,
        } = result;
        let (mut indexed, problem) = match outcome {
            Ok(refresh) => (refresh.indexed, refresh.problem),
            Err(error) => (HashMap::new(), Some(error)),
        };
        let is_current = |path: &Path| {
            self.current_from
                .get(path)
                .is_some_and(|current_from| *current_from <= sequence)
        };
        indexed.retain(|path, _| is_current(path));
        fingerprints.retain(|path, _| indexed.contains_key(path));
        self.last_accepted = sequence;
        if sequence == self.last_requested {
            // Nothing is in flight any more.
            self.current_from.clear();
        } else {
            for path in indexed.keys() {
                self.current_from.remove(path);
            }
        }
        for (path, row) in &indexed {
            self.keep(
                path.clone(),
                sequence,
                Row::Refreshed {
                    indexed: row.clone(),
                    fingerprint: fingerprints.get(path).copied(),
                },
            );
        }
        AcceptedRefresh {
            indexed,
            fingerprints,
            problem,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::SearchTags;
    use std::time::Duration;

    fn indexed(title: &str) -> IndexedTrack {
        IndexedTrack {
            search_tags: SearchTags::default(),
            title: title.to_string(),
            artist: String::new(),
            album: String::new(),
            cover_path: String::new(),
            cover_stamp: 0,
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        }
    }

    fn result(sequence: u64, entries: &[(&str, &str)]) -> RefreshResult {
        RefreshResult {
            sequence,
            outcome: Ok(TrackRefresh {
                indexed: entries
                    .iter()
                    .map(|(path, title)| (PathBuf::from(path), indexed(title)))
                    .collect(),
                problem: None,
            }),
            fingerprints: HashMap::new(),
        }
    }

    /// A worker whose own results are left unread, for results made by hand.
    fn idle_worker() -> EditRefreshWorker {
        EditRefreshWorker::with_refresh(|_| Err("unread".to_string()))
    }

    fn request(worker: &mut EditRefreshWorker, paths: &[&str]) -> u64 {
        worker
            .request(paths.iter().map(PathBuf::from).collect())
            .expect("the worker takes requests")
    }

    fn swap(a: &str, b: &str) -> PathRenames {
        PathRenames::from([
            (PathBuf::from(a), PathBuf::from(b)),
            (PathBuf::from(b), PathBuf::from(a)),
        ])
    }

    fn titles(details: &RenamedDetails) -> HashMap<&Path, &str> {
        details
            .indexed
            .iter()
            .map(|(path, row)| (path.as_path(), row.title.as_str()))
            .collect()
    }

    #[test]
    fn an_older_result_for_a_path_with_a_newer_request_is_ignored() {
        let mut worker = idle_worker();
        let first = request(&mut worker, &["/m/a.flac", "/m/b.flac"]);
        let second = request(&mut worker, &["/m/a.flac"]);

        // The first read, which began before the second save of a.flac.
        let accepted = worker.accept(result(first, &[("/m/a.flac", "old"), ("/m/b.flac", "b")]));
        assert_eq!(
            accepted.indexed.keys().collect::<Vec<_>>(),
            vec![&PathBuf::from("/m/b.flac")]
        );
        let accepted = worker.accept(result(second, &[("/m/a.flac", "new")]));
        assert_eq!(accepted.indexed[&PathBuf::from("/m/a.flac")].title, "new");
    }

    #[test]
    fn a_result_asked_for_before_a_rename_is_ignored_for_its_paths() {
        let mut worker = idle_worker();
        let before = request(&mut worker, &["/m/a.flac", "/m/c.flac"]);
        // a and b swap names while a is read.
        let mut details = RenamedDetails::default();
        worker.renamed(
            &swap("/m/a.flac", "/m/b.flac"),
            next_file_sequence(),
            &mut details,
        );
        let accepted = worker.accept(result(
            before,
            &[("/m/a.flac", "a before"), ("/m/c.flac", "c")],
        ));
        assert_eq!(
            accepted.indexed.keys().collect::<Vec<_>>(),
            vec![&PathBuf::from("/m/c.flac")]
        );
        // A save after the rename is read and applied.
        let after = request(&mut worker, &["/m/a.flac"]);
        let accepted = worker.accept(result(after, &[("/m/a.flac", "a after")]));
        assert_eq!(
            accepted.indexed[&PathBuf::from("/m/a.flac")].title,
            "a after"
        );
    }

    #[test]
    fn a_result_asked_for_after_a_rename_stands_in_whatever_order_they_arrive() {
        // The rename read its files first; a save of b followed it.
        let renamed_at = next_file_sequence();
        let renames = swap("/m/a.flac", "/m/b.flac");
        let rename_rows = || RenamedDetails {
            indexed: HashMap::from([
                (PathBuf::from("/m/a.flac"), indexed("b's file")),
                (PathBuf::from("/m/b.flac"), indexed("a's file")),
            ]),
            ..RenamedDetails::default()
        };

        // The save's result is still in flight when the rename arrives.
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/b.flac"]);
        let mut details = rename_rows();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(titles(&details)[Path::new("/m/b.flac")], "a's file");
        let accepted = worker.accept(result(saved, &[("/m/b.flac", "a's file, saved")]));
        assert_eq!(
            accepted.indexed[&PathBuf::from("/m/b.flac")].title,
            "a's file, saved"
        );

        // The save's result was applied before the rename arrives.
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/b.flac"]);
        let _ = worker.accept(result(saved, &[("/m/b.flac", "a's file, saved")]));
        let mut details = rename_rows();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(
            titles(&details),
            HashMap::from([
                (Path::new("/m/a.flac"), "b's file"),
                (Path::new("/m/b.flac"), "a's file, saved"),
            ])
        );

        // Rows forgotten once no rename can arrive are not used.
        worker.forget_applied();
        let mut details = rename_rows();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(titles(&details)[Path::new("/m/b.flac")], "a's file");
    }

    #[test]
    fn a_row_accepted_before_a_rename_was_read_gives_way_to_the_rename() {
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/b.flac"]);
        let _ = worker.accept(result(saved, &[("/m/b.flac", "before the rename")]));
        let mut details = RenamedDetails {
            indexed: HashMap::from([(PathBuf::from("/m/b.flac"), indexed("renamed"))]),
            ..RenamedDetails::default()
        };
        worker.renamed(
            &PathRenames::from([(PathBuf::from("/m/a.flac"), PathBuf::from("/m/b.flac"))]),
            next_file_sequence(),
            &mut details,
        );
        assert_eq!(titles(&details)[Path::new("/m/b.flac")], "renamed");
    }

    fn library_rows(upserted: &[(&str, &str)], removed: &[&str]) -> LibraryChanges {
        LibraryChanges {
            roots: None,
            upserted: upserted
                .iter()
                .map(|(path, title)| {
                    Arc::new(LibraryTrack {
                        path: PathBuf::from(path),
                        title: (*title).to_string(),
                        ..LibraryTrack::default()
                    })
                })
                .collect(),
            removed: removed.iter().map(PathBuf::from).collect(),
        }
    }

    fn fingerprint(mtime_ns: i64) -> TrackFileFingerprint {
        TrackFileFingerprint {
            mtime_ns,
            size_bytes: 1,
        }
    }

    #[test]
    fn a_library_row_read_after_a_rename_wins_over_older_rows() {
        let renamed_at = next_file_sequence();
        let renames = swap("/m/a.flac", "/m/b.flac");
        let rename_rows = || RenamedDetails {
            indexed: HashMap::from([
                (PathBuf::from("/m/a.flac"), indexed("b's file")),
                (PathBuf::from("/m/b.flac"), indexed("a's file")),
            ]),
            fingerprints: HashMap::from([(PathBuf::from("/m/b.flac"), fingerprint(1))]),
            ..RenamedDetails::default()
        };

        // A save of b is applied, then a scan's newer row for it.
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/b.flac"]);
        let mut refreshed = result(saved, &[("/m/b.flac", "a's file, saved")]);
        refreshed
            .fingerprints
            .insert(PathBuf::from("/m/b.flac"), fingerprint(2));
        let _ = worker.accept(refreshed);
        worker.library_changed(
            &library_rows(&[("/m/b.flac", "a's file, scanned")], &[]),
            next_file_sequence(),
        );
        let mut details = rename_rows();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(
            titles(&details),
            HashMap::from([
                (Path::new("/m/a.flac"), "b's file"),
                (Path::new("/m/b.flac"), "a's file, scanned"),
            ])
        );
        // The scan did not stat the file.
        assert!(details.fingerprints.is_empty());

        // A scan's row without a save wins over the rename's own.
        let mut worker = idle_worker();
        worker.library_changed(
            &library_rows(&[("/m/a.flac", "b's file, scanned")], &[]),
            next_file_sequence(),
        );
        let mut details = rename_rows();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(
            titles(&details)[Path::new("/m/a.flac")],
            "b's file, scanned"
        );
    }

    #[test]
    fn a_library_row_read_before_a_rename_leaves_the_newer_rows() {
        // A scan read b before the rename, and its row reaches the bridge
        // after a save's that followed the rename.
        let scanned_from = next_file_sequence();
        let renamed_at = next_file_sequence();
        let renames = swap("/m/a.flac", "/m/b.flac");
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/b.flac"]);
        let _ = worker.accept(result(saved, &[("/m/b.flac", "a's file, saved")]));
        worker.library_changed(
            &library_rows(&[("/m/a.flac", "a"), ("/m/b.flac", "b")], &[]),
            scanned_from,
        );
        let mut details = RenamedDetails {
            indexed: HashMap::from([
                (PathBuf::from("/m/a.flac"), indexed("b's file")),
                (PathBuf::from("/m/b.flac"), indexed("a's file")),
            ]),
            ..RenamedDetails::default()
        };
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(
            titles(&details),
            HashMap::from([
                (Path::new("/m/a.flac"), "b's file"),
                (Path::new("/m/b.flac"), "a's file, saved"),
            ])
        );
        assert!(details.gone.is_empty());
    }

    #[test]
    fn a_path_a_library_event_removed_after_a_rename_is_gone() {
        let renamed_at = next_file_sequence();
        let renames = PathRenames::from([(PathBuf::from("/m/a.flac"), PathBuf::from("/m/c.flac"))]);
        let mut worker = idle_worker();
        let saved = request(&mut worker, &["/m/c.flac"]);
        let _ = worker.accept(result(saved, &[("/m/c.flac", "saved")]));
        worker.library_changed(&library_rows(&[], &["/m/c.flac"]), next_file_sequence());
        let mut details = RenamedDetails {
            indexed: HashMap::from([(PathBuf::from("/m/c.flac"), indexed("renamed"))]),
            fingerprints: HashMap::from([(PathBuf::from("/m/c.flac"), fingerprint(1))]),
            ..RenamedDetails::default()
        };
        worker.renamed(&renames, renamed_at, &mut details);
        assert!(details.indexed.is_empty());
        assert!(details.fingerprints.is_empty());
        assert_eq!(
            details.gone,
            std::collections::HashSet::from([PathBuf::from("/m/c.flac")])
        );

        // A save after the removal, applied last, is shown.
        let saved = request(&mut worker, &["/m/c.flac"]);
        let _ = worker.accept(result(saved, &[("/m/c.flac", "saved again")]));
        let mut details = RenamedDetails::default();
        worker.renamed(&renames, renamed_at, &mut details);
        assert_eq!(titles(&details)[Path::new("/m/c.flac")], "saved again");
        assert!(details.gone.is_empty());
    }

    #[test]
    fn a_failed_refresh_is_reported() {
        let mut worker = idle_worker();
        let sequence = request(&mut worker, &["/m/a.flac"]);
        let accepted = worker.accept(RefreshResult {
            sequence,
            outcome: Err("failed to open library db".to_string()),
            fingerprints: HashMap::new(),
        });
        assert!(accepted.indexed.is_empty());
        assert_eq!(
            accepted.problem.as_deref(),
            Some("failed to open library db")
        );
    }

    #[test]
    fn saves_that_queue_up_are_read_in_one_pass() {
        let (started_tx, started) = crossbeam_channel::unbounded();
        let (release, release_rx) = crossbeam_channel::unbounded::<()>();
        let mut worker = EditRefreshWorker::with_refresh(move |paths| {
            started_tx.send(paths.to_vec()).expect("report the read");
            release_rx
                .recv_timeout(Duration::from_secs(10))
                .expect("release the read");
            Ok(TrackRefresh {
                indexed: paths
                    .iter()
                    .map(|path| (path.clone(), indexed("read")))
                    .collect(),
                problem: None,
            })
        });
        assert!(worker.request(vec![PathBuf::from("/m/a.flac")]).is_some());
        let first = started
            .recv_timeout(Duration::from_secs(10))
            .expect("first read");
        assert_eq!(first, vec![PathBuf::from("/m/a.flac")]);
        for name in ["c", "b", "c", "d"] {
            assert!(worker
                .request(vec![PathBuf::from(format!("/m/{name}.flac"))])
                .is_some());
        }
        release.send(()).expect("release the first read");
        let second = started
            .recv_timeout(Duration::from_secs(10))
            .expect("second read");
        assert_eq!(
            second,
            ["b", "c", "d"]
                .iter()
                .map(|name| PathBuf::from(format!("/m/{name}.flac")))
                .collect::<Vec<_>>()
        );
        release.send(()).expect("release the second read");
        for _ in 0..2 {
            let result = worker
                .results
                .recv_timeout(Duration::from_secs(10))
                .expect("result");
            let _ = worker.accept(result);
        }
        assert!(worker.current_from.is_empty());
    }
}
