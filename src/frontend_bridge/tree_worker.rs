// SPDX-License-Identifier: GPL-3.0-or-later

use super::{library_tree::LibraryTreeCache, LibrarySortMode};
use crate::library::LibrarySnapshot;
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::collections::HashSet;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

struct TreeRequest {
    library: Arc<LibrarySnapshot>,
    sort_mode: LibrarySortMode,
    expanded: HashSet<String>,
    generation: u64,
}

pub(super) struct TreeResult {
    pub(super) bytes: Arc<Vec<u8>>,
    pub(super) expanded: HashSet<String>,
    pub(super) counts: (usize, usize),
    generation: u64,
}

pub(super) struct TreeWorker {
    tx: Sender<TreeRequest>,
    pub(super) results: Receiver<TreeResult>,
    generation: Arc<AtomicU64>,
}

impl TreeWorker {
    pub(super) fn new() -> Self {
        let (tx, rx) = unbounded::<TreeRequest>();
        let (result_tx, results) = unbounded();
        let generation = Arc::new(AtomicU64::new(0));
        let active = Arc::clone(&generation);
        let _ = std::thread::Builder::new()
            .name("ferrous-library-tree".into())
            .spawn(move || {
                let mut cached: Option<(Arc<LibrarySnapshot>, LibraryTreeCache)> = None;
                while let Ok(mut request) = rx.recv() {
                    for newer in rx.try_iter() {
                        request = newer;
                    }
                    if active.load(Ordering::Relaxed) != request.generation {
                        continue;
                    }
                    let (cached_library, cache) = cached.get_or_insert_with(|| {
                        (
                            Arc::clone(&request.library),
                            LibraryTreeCache::new(&request.library),
                        )
                    });
                    if !Arc::ptr_eq(cached_library, &request.library) {
                        *cache = LibraryTreeCache::new(&request.library);
                        *cached_library = Arc::clone(&request.library);
                    }
                    if !request.library.scan_in_progress {
                        cache.retain_expanded_keys(&mut request.expanded);
                    }
                    let bytes = cache.build(request.sort_mode, Some(&request.expanded));
                    if active.load(Ordering::Relaxed) == request.generation
                        && result_tx
                            .send(TreeResult {
                                bytes: Arc::new(bytes),
                                expanded: request.expanded,
                                counts: cache.counts,
                                generation: request.generation,
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
        }
    }

    pub(super) fn request(
        &self,
        library: Arc<LibrarySnapshot>,
        sort_mode: LibrarySortMode,
        expanded: HashSet<String>,
    ) {
        let generation = self.generation.fetch_add(1, Ordering::Relaxed) + 1;
        let _ = self.tx.send(TreeRequest {
            library,
            sort_mode,
            expanded,
            generation,
        });
    }

    pub(super) fn is_current(&self, result: &TreeResult) -> bool {
        result.generation == self.generation.load(Ordering::Relaxed)
    }
}

impl Drop for TreeWorker {
    fn drop(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn newer_expansion_invalidates_ready_tree_results() {
        let worker = TreeWorker::new();
        let library = Arc::new(LibrarySnapshot::default());
        worker.request(Arc::clone(&library), LibrarySortMode::Year, HashSet::new());
        let first = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(2))
            .expect("first result");
        assert!(worker.is_current(&first));
        worker.request(
            library,
            LibrarySortMode::Title,
            HashSet::from(["restored".into()]),
        );
        assert!(!worker.is_current(&first));
        let latest = worker
            .results
            .recv_timeout(std::time::Duration::from_secs(2))
            .expect("latest result");
        assert!(worker.is_current(&latest));
        assert!(latest.expanded.contains("restored"));
    }
}
