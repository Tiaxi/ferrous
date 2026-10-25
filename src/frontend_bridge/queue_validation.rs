// SPDX-License-Identifier: GPL-3.0-or-later

use super::{commands::validate_queue_details, BridgeState};
use crate::library::{
    read_external_track, track_file_fingerprint, ExternalReadFailures, ExternalTrackCache,
    IndexedTrack, TrackFileFingerprint,
};
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

pub(super) struct ValidationResult {
    generation: u64,
    pub(super) details: Arc<HashMap<PathBuf, IndexedTrack>>,
    pub(super) fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    /// What the files read for this result could not give, for the user.
    pub(super) problem: Option<String>,
}

pub(super) struct QueueValidationWorker {
    tx: Sender<Box<BridgeState>>,
    pub(super) results: Receiver<ValidationResult>,
    generation: Arc<AtomicU64>,
    in_flight: Option<u64>,
    /// How many validations were asked of the worker.
    #[cfg(test)]
    pub(super) requests: usize,
}

impl QueueValidationWorker {
    pub(super) fn new() -> Self {
        Self::with_cache(ExternalTrackCache::new)
    }

    fn with_cache(cache_factory: impl Fn() -> ExternalTrackCache + Send + 'static) -> Self {
        let (tx, rx) = unbounded::<Box<BridgeState>>();
        let (result_tx, results) = unbounded();
        let generation = Arc::new(AtomicU64::new(0));
        let active = Arc::clone(&generation);
        crate::worker_thread::spawn_worker("ferrous-queue-validation", move || {
            let mut cache = None;
            while let Ok(mut state) = rx.recv() {
                for newer in rx.try_iter() {
                    state = newer;
                }
                if active.load(Ordering::Relaxed) != state.queue_detail_generation {
                    continue;
                }
                let request_generation = state.queue_detail_generation;
                let cancelled = || active.load(Ordering::Relaxed) != request_generation;
                let reads = validate_queue_details(&mut state, &cancelled, |pending| {
                    if pending.is_empty() {
                        return HashMap::new();
                    }
                    cache.get_or_insert_with(&cache_factory).load_many(pending)
                });
                let mut failures = ExternalReadFailures::default();
                for (path, fingerprint) in reads {
                    if cancelled() {
                        break;
                    }
                    let read = read_external_track(&path);
                    // Files may change while metadata is read; never cache or publish mismatched data.
                    if track_file_fingerprint(&path) != Some(fingerprint) {
                        continue;
                    }
                    failures.note(&path, &read);
                    let indexed = match &cache {
                        Some(cache) => cache.store(&path, fingerprint, read),
                        None => read.into_indexed(),
                    };
                    state
                        .queue_detail_fingerprints
                        .insert(path.clone(), fingerprint);
                    Arc::make_mut(&mut state.queue_details).insert(path, indexed);
                }
                if active.load(Ordering::Relaxed) == state.queue_detail_generation
                    && result_tx
                        .send(ValidationResult {
                            generation: state.queue_detail_generation,
                            details: state.queue_details,
                            fingerprints: state.queue_detail_fingerprints,
                            problem: failures.problem(),
                        })
                        .is_err()
                {
                    break;
                }
            }
        });
        Self {
            tx,
            results,
            generation,
            in_flight: None,
            #[cfg(test)]
            requests: 0,
        }
    }

    pub(super) fn request(&mut self, state: &BridgeState) {
        if self.in_flight == Some(state.queue_detail_generation) {
            return;
        }
        self.generation
            .store(state.queue_detail_generation, Ordering::Relaxed);
        self.in_flight = Some(state.queue_detail_generation);
        let snapshot = BridgeState {
            library: Arc::clone(&state.library),
            queue: state.queue.clone(),
            queue_details: Arc::clone(&state.queue_details),
            queue_detail_fingerprints: state.queue_detail_fingerprints.clone(),
            queue_detail_generation: state.queue_detail_generation,
            ..BridgeState::default()
        };
        if self.tx.send(Box::new(snapshot)).is_err() {
            self.in_flight = None;
        }
        #[cfg(test)]
        {
            self.requests += 1;
        }
    }

    /// Take `result` if it is for the queue as it is. Returns whether the
    /// details changed, and what the user is shown about the files that
    /// could not be read fully.
    pub(super) fn apply(
        &mut self,
        result: ValidationResult,
        state: &mut BridgeState,
    ) -> (bool, Option<String>) {
        if self.in_flight == Some(result.generation) {
            self.in_flight = None;
        }
        if result.generation != state.queue_detail_generation {
            // The files are read again for the newer queue.
            return (false, None);
        }
        let changed = state.queue_details != result.details;
        state.queue_details = result.details;
        state.queue_detail_fingerprints = result.fingerprints;
        (changed, result.problem)
    }
}

impl Drop for QueueValidationWorker {
    fn drop(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn queue_clear_cancels_validation_while_cache_io_is_blocked() {
        let path = std::env::temp_dir().join(format!(
            "ferrous-blocked-validator-{}.ac3",
            std::process::id()
        ));
        std::fs::write(&path, [0u8; 128]).expect("fixture");
        let (started_tx, started) = crossbeam_channel::bounded(1);
        let (resume, resume_rx) = crossbeam_channel::bounded(1);
        let mut worker = QueueValidationWorker::with_cache(move || {
            started_tx.send(()).expect("cache started");
            resume_rx
                .recv_timeout(std::time::Duration::from_secs(3))
                .expect("resume cache");
            ExternalTrackCache::in_memory()
        });
        let mut state = BridgeState {
            queue: vec![path.clone()],
            queue_detail_generation: 1,
            ..Default::default()
        };
        worker.request(&state);
        started
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("worker blocked in cache");
        // The caller can replace the queue and schedule its validation before I/O resumes.
        state.queue.clear();
        state.queue_detail_generation += 1;
        worker.request(&state);
        resume.send(()).expect("resume worker");
        let result = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("cleared queue result");
        assert_eq!(result.generation, state.queue_detail_generation);
        let _ = worker.apply(result, &mut state);
        assert!(state.queue_details.is_empty());
        assert!(worker.results.try_recv().is_err());
        std::fs::remove_file(path).expect("cleanup fixture");
    }

    #[test]
    fn worker_validates_changed_files_and_rejects_stale_results() {
        let path = std::env::temp_dir().join(format!(
            "ferrous-queue-validator-{}.ac3",
            std::process::id()
        ));
        std::fs::write(&path, [0u8; 128]).expect("fixture");
        let mut state = BridgeState {
            queue: vec![path.clone()],
            queue_detail_generation: 1,
            ..Default::default()
        };
        let mut worker = QueueValidationWorker::with_cache(ExternalTrackCache::in_memory);
        worker.request(&state);
        let first = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("validation");
        assert!(worker.apply(first, &mut state).0);
        assert!(state.queue_details.contains_key(&path));
        let fingerprint = state.queue_detail_fingerprints[&path];
        std::fs::write(&path, [0u8; 256]).expect("changed fixture");
        worker.request(&state);
        let changed = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("revalidation");
        let _ = worker.apply(changed, &mut state);
        assert_ne!(state.queue_detail_fingerprints[&path], fingerprint);
        worker.request(&state);
        let stale = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("old result");
        state.queue.clear();
        state.queue_detail_generation += 1;
        assert_eq!(worker.apply(stale, &mut state), (false, None));
        worker.request(&state);
        let cleared = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("clear result");
        assert!(worker.apply(cleared, &mut state).0);
        assert!(state.queue_details.is_empty());
        std::fs::remove_file(path).expect("remove fixture");
    }
}
