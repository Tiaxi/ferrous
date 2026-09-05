// SPDX-License-Identifier: GPL-3.0-or-later

use super::commands::{expand_import_paths_cancellable, ImportExpandOutcome};
use super::{BridgeCommand, BridgeLibraryCommand, BridgeQueueCommand};
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

struct ImportJob {
    paths: Vec<PathBuf>,
    replace: bool,
    generation: u64,
}

pub(super) struct ImportResult {
    pub(super) outcome: ImportExpandOutcome,
    pub(super) replace: bool,
    generation: u64,
}

pub(super) struct ImportWorker {
    tx: Sender<ImportJob>,
    pub(super) results: Receiver<ImportResult>,
    generation: Arc<AtomicU64>,
}

impl ImportWorker {
    pub(super) fn new() -> Self {
        let (tx, rx) = unbounded::<ImportJob>();
        let (result_tx, results) = unbounded();
        let generation = Arc::new(AtomicU64::new(0));
        let active = Arc::clone(&generation);
        let _ = std::thread::Builder::new()
            .name("ferrous-import".into())
            .spawn(move || {
                while let Ok(job) = rx.recv() {
                    let cancelled = || active.load(Ordering::Relaxed) != job.generation;
                    if cancelled() {
                        continue;
                    }
                    let outcome = expand_import_paths_cancellable(job.paths, &cancelled);
                    if !cancelled()
                        && result_tx
                            .send(ImportResult {
                                outcome,
                                replace: job.replace,
                                generation: job.generation,
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

    pub(super) fn is_current(&self, result: &ImportResult) -> bool {
        self.generation.load(Ordering::Relaxed) == result.generation
    }

    pub(super) fn request(&self, command: &BridgeCommand) -> Option<Result<(), String>> {
        let (paths, replace) = match command {
            BridgeCommand::Library(BridgeLibraryCommand::AddTrack(path)) => {
                (vec![path.clone()], false)
            }
            BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(path)) => {
                (vec![path.clone()], true)
            }
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(paths)) => {
                (paths.clone(), false)
            }
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceWithAlbum(paths)) => {
                (paths.clone(), true)
            }
            _ => {
                if replaces_queue(command) {
                    self.generation.fetch_add(1, Ordering::Relaxed);
                }
                return None;
            }
        };
        if replace {
            self.generation.fetch_add(1, Ordering::Relaxed);
        }
        Some(
            self.tx
                .send(ImportJob {
                    paths,
                    replace,
                    generation: self.generation.load(Ordering::Relaxed),
                })
                .map_err(|_| "import worker unavailable".to_string()),
        )
    }
}

impl Drop for ImportWorker {
    fn drop(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
    }
}

fn replaces_queue(command: &BridgeCommand) -> bool {
    matches!(
        command,
        BridgeCommand::Shutdown
            | BridgeCommand::Queue(BridgeQueueCommand::Clear | BridgeQueueCommand::Replace { .. })
            | BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceAlbumByKey { .. }
                    | BridgeLibraryCommand::ReplaceArtistByKey { .. }
                    | BridgeLibraryCommand::ReplaceRootByPath { .. }
                    | BridgeLibraryCommand::ReplaceAllTracks
            )
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn import_requests_only_enqueue_and_replacements_cancel_previous_work() {
        let (tx, jobs) = unbounded();
        let (_, results) = unbounded();
        let worker = ImportWorker {
            tx,
            results,
            generation: Arc::new(AtomicU64::new(0)),
        };
        let import = |path: &str| {
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(vec![PathBuf::from(path)]))
        };
        worker
            .request(&import("/unreadable/first.m3u"))
            .expect("import")
            .expect("queued");
        worker
            .request(&import("/unreadable/second.m3u"))
            .expect("import")
            .expect("queued");
        let first = jobs.try_recv().expect("first job");
        let second = jobs.try_recv().expect("second job");
        assert_eq!(first.paths, vec![PathBuf::from("/unreadable/first.m3u")]);
        assert_eq!(second.paths, vec![PathBuf::from("/unreadable/second.m3u")]);
        assert_eq!(first.generation, second.generation);
        let result = ImportResult {
            outcome: ImportExpandOutcome::default(),
            replace: false,
            generation: first.generation,
        };
        assert!(worker.is_current(&result));
        assert!(worker
            .request(&BridgeCommand::Playback(
                super::super::BridgePlaybackCommand::Pause
            ))
            .is_none());
        assert!(worker.is_current(&result));
        assert!(worker
            .request(&BridgeCommand::Queue(BridgeQueueCommand::Clear))
            .is_none());
        assert!(!worker.is_current(&result));
        worker
            .request(&BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(
                PathBuf::from("/new.m3u"),
            )))
            .expect("import")
            .expect("queued");
        let replacement = jobs.try_recv().expect("replacement");
        assert!(replacement.replace);
        assert!(replacement.generation > second.generation);
    }
}
