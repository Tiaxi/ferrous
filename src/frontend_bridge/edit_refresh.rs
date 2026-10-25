// SPDX-License-Identifier: GPL-3.0-or-later

//! Reads the files the tag editor saved again, and updates their library rows,
//! on a worker, so the bridge thread, which also drives playback, never waits
//! for tag reads or the library database.
//!
//! Every request has a generation. A result updates a path only if no newer
//! request for the path is pending and no rename has moved the path since the
//! request, so an older read never overwrites newer details. A read that a
//! rename made stale is asked for again under the file's new path, so the
//! details shown always come from a read after both the rename and the save,
//! in whichever order the bridge learns of them.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::library::{
    refresh_indexed_metadata_for_paths, track_file_fingerprint, IndexedTrack, TrackFileFingerprint,
    TrackRefresh,
};
use crate::playback::PathRenames;

struct RefreshRequest {
    generation: u64,
    paths: Vec<PathBuf>,
}

pub(super) struct RefreshResult {
    /// The newest request the result answers.
    generation: u64,
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

pub(super) struct EditRefreshWorker {
    tx: Sender<RefreshRequest>,
    pub(super) results: Receiver<RefreshResult>,
    next_generation: u64,
    /// The generation of the last request sent.
    last_requested: u64,
    /// The generation of the last result accepted.
    last_accepted: u64,
    /// For each path with a refresh in flight, the oldest generation whose
    /// result may update it.
    current_from: HashMap<PathBuf, u64>,
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
                // Saves that queued up meanwhile are read in the same pass.
                let mut generation = first.generation;
                let mut paths = first.paths;
                for next in rx.try_iter() {
                    generation = next.generation;
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
                    generation,
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
            next_generation: 0,
            last_requested: 0,
            last_accepted: 0,
            current_from: HashMap::new(),
        }
    }

    /// Ask for `paths` to be read again. Returns `false` if the worker has
    /// stopped.
    pub(super) fn request(&mut self, paths: Vec<PathBuf>) -> bool {
        let generation = self.advance();
        for path in &paths {
            self.current_from.insert(path.clone(), generation);
        }
        self.last_requested = generation;
        self.tx.send(RefreshRequest { generation, paths }).is_ok()
    }

    /// Files were renamed on disk. A read in flight of any of their old or
    /// new paths may have read another file, or none, so it is ignored.
    /// Returns the paths, under their new names, that must be read again:
    /// the read may also hold a save the rename's own read missed, as when
    /// a file is saved under its new name before the bridge applies the
    /// rename. Request them once the rename is applied.
    #[must_use]
    pub(super) fn renamed(&mut self, renames: &PathRenames) -> Vec<PathBuf> {
        if self.current_from.is_empty() {
            return Vec::new();
        }
        let generation = self.advance();
        let new_paths = renames.values().collect::<HashSet<_>>();
        let mut read_again = Vec::new();
        for path in renames.keys().chain(renames.values()) {
            let Some(current_from) = self.current_from.get_mut(path) else {
                continue;
            };
            *current_from = generation;
            // The request meant the file that had the path, now under its
            // new name, or, if it was sent once the rename was on disk, the
            // file that has it now.
            if let Some(new_path) = renames.get(path) {
                read_again.push(new_path.clone());
            }
            if new_paths.contains(path) {
                read_again.push(path.clone());
            }
        }
        read_again.sort_unstable();
        read_again.dedup();
        read_again
    }

    /// Whether every request has been answered.
    #[cfg(test)]
    pub(super) fn is_idle(&self) -> bool {
        self.last_accepted == self.last_requested
    }

    /// The part of `result` that is still current.
    pub(super) fn accept(&mut self, result: RefreshResult) -> AcceptedRefresh {
        let RefreshResult {
            generation,
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
                .is_some_and(|current_from| *current_from <= generation)
        };
        indexed.retain(|path, _| is_current(path));
        fingerprints.retain(|path, _| indexed.contains_key(path));
        self.last_accepted = generation;
        if generation == self.last_requested {
            // Nothing is in flight any more.
            self.current_from.clear();
        } else {
            for path in indexed.keys() {
                self.current_from.remove(path);
            }
        }
        AcceptedRefresh {
            indexed,
            fingerprints,
            problem,
        }
    }

    fn advance(&mut self) -> u64 {
        self.next_generation = self.next_generation.wrapping_add(1);
        self.next_generation
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
            genre: String::new(),
            year: None,
            track_no: None,
            duration_secs: None,
        }
    }

    fn result(generation: u64, entries: &[(&str, &str)]) -> RefreshResult {
        RefreshResult {
            generation,
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

    #[test]
    fn an_older_result_for_a_path_with_a_newer_request_is_ignored() {
        let mut worker = idle_worker();
        assert!(worker.request(vec![PathBuf::from("/m/a.flac"), PathBuf::from("/m/b.flac")]));
        assert!(worker.request(vec![PathBuf::from("/m/a.flac")]));

        // The first read, which began before the second save of a.flac.
        let accepted = worker.accept(result(1, &[("/m/a.flac", "old"), ("/m/b.flac", "b")]));
        assert_eq!(
            accepted.indexed.keys().collect::<Vec<_>>(),
            vec![&PathBuf::from("/m/b.flac")]
        );
        let accepted = worker.accept(result(2, &[("/m/a.flac", "new")]));
        assert_eq!(accepted.indexed[&PathBuf::from("/m/a.flac")].title, "new");
    }

    #[test]
    fn a_result_read_before_a_rename_is_ignored() {
        let mut worker = idle_worker();
        assert!(worker.request(vec![PathBuf::from("/m/a.flac"), PathBuf::from("/m/c.flac")]));
        // a and b swap names while a is read.
        let read_again = worker.renamed(&PathRenames::from([
            (PathBuf::from("/m/a.flac"), PathBuf::from("/m/b.flac")),
            (PathBuf::from("/m/b.flac"), PathBuf::from("/m/a.flac")),
        ]));
        // Either file may hold the save the read was for.
        assert_eq!(
            read_again,
            vec![PathBuf::from("/m/a.flac"), PathBuf::from("/m/b.flac")]
        );
        let accepted = worker.accept(result(1, &[("/m/a.flac", "a before"), ("/m/c.flac", "c")]));
        assert_eq!(
            accepted.indexed.keys().collect::<Vec<_>>(),
            vec![&PathBuf::from("/m/c.flac")]
        );
        // A save after the rename is read and applied.
        assert!(worker.request(vec![PathBuf::from("/m/a.flac")]));
        let accepted = worker.accept(result(3, &[("/m/a.flac", "a after")]));
        assert_eq!(
            accepted.indexed[&PathBuf::from("/m/a.flac")].title,
            "a after"
        );
    }

    #[test]
    fn a_failed_refresh_is_reported() {
        let mut worker = idle_worker();
        assert!(worker.request(vec![PathBuf::from("/m/a.flac")]));
        let accepted = worker.accept(RefreshResult {
            generation: 1,
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
        assert!(worker.request(vec![PathBuf::from("/m/a.flac")]));
        let first = started
            .recv_timeout(Duration::from_secs(10))
            .expect("first read");
        assert_eq!(first, vec![PathBuf::from("/m/a.flac")]);
        for name in ["c", "b", "c", "d"] {
            assert!(worker.request(vec![PathBuf::from(format!("/m/{name}.flac"))]));
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
