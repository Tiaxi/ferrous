// SPDX-License-Identifier: GPL-3.0-or-later

use super::{commands::validate_queue_details, BridgeState};
use crate::library::{
    read_track_info, track_file_fingerprint, ExternalTrackCache, IndexedTrack, TrackFileFingerprint,
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
}

pub(super) struct QueueValidationWorker {
    tx: Sender<Box<BridgeState>>,
    pub(super) results: Receiver<ValidationResult>,
    generation: Arc<AtomicU64>,
    in_flight: Option<u64>,
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
        let _ = std::thread::Builder::new()
            .name("ferrous-queue-validation".into())
            .spawn(move || {
                let mut cache = None;
                while let Ok(mut state) = rx.recv() {
                    for newer in rx.try_iter() {
                        state = newer;
                    }
                    let cancelled =
                        || active.load(Ordering::Relaxed) != state.queue_detail_generation;
                    if cancelled() {
                        continue;
                    }
                    let request_generation = state.queue_detail_generation;
                    let (request_tx, requests) = unbounded();
                    validate_queue_details(
                        &mut state,
                        &request_tx,
                        &|| active.load(Ordering::Relaxed) != request_generation,
                        |pending| {
                            if pending.is_empty() {
                                return HashMap::new();
                            }
                            cache.get_or_insert_with(&cache_factory).load_many(pending)
                        },
                    );
                    for request in requests.try_iter() {
                        if active.load(Ordering::Relaxed) != state.queue_detail_generation {
                            break;
                        }
                        let indexed = read_track_info(&request.path);
                        // Files may change while metadata is read; never cache or publish mismatched data.
                        if track_file_fingerprint(&request.path) != Some(request.fingerprint) {
                            continue;
                        }
                        if let Some(cache) = &cache {
                            cache.store(&request.path, request.fingerprint, &indexed);
                        }
                        state
                            .queue_detail_fingerprints
                            .insert(request.path.clone(), request.fingerprint);
                        Arc::make_mut(&mut state.queue_details).insert(request.path, indexed);
                    }
                    if active.load(Ordering::Relaxed) == state.queue_detail_generation
                        && result_tx
                            .send(ValidationResult {
                                generation: state.queue_detail_generation,
                                details: state.queue_details,
                                fingerprints: state.queue_detail_fingerprints,
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
    }

    pub(super) fn apply(&mut self, result: ValidationResult, state: &mut BridgeState) -> bool {
        if self.in_flight == Some(result.generation) {
            self.in_flight = None;
        }
        if result.generation != state.queue_detail_generation {
            return false;
        }
        let changed = state.queue_details != result.details;
        state.queue_details = result.details;
        state.queue_detail_fingerprints = result.fingerprints;
        changed
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
        assert!(worker.apply(first, &mut state));
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
        assert!(!worker.apply(stale, &mut state));
        worker.request(&state);
        let cleared = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(3))
            .expect("clear result");
        assert!(worker.apply(cleared, &mut state));
        assert!(state.queue_details.is_empty());
        std::fs::remove_file(path).expect("remove fixture");
    }
}
