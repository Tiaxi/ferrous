// SPDX-License-Identifier: GPL-3.0-or-later

//! Queue additions and replacements, applied in the order they arrive.
//!
//! Imports expand files, folders, and playlists on a worker thread. Every
//! change to the queue's contents waits behind the changes that arrived before
//! it, so a later append is neither overwritten by an earlier replacement nor
//! applied ahead of an earlier import. A replacement discards the changes still
//! waiting, because it would overwrite them. Commands that address queue rows
//! by index (play, remove, move, select) are not queued here: they refer to
//! the queue the user can see, so they apply immediately.

use super::commands::{
    expand_import_paths_cancellable, format_import_warning, ImportExpandOutcome,
};
use super::BridgeQueueCommand;
use crate::playback::{remap_path, PathRenames};
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};

/// A change to the queue's contents.
#[derive(Debug, Clone)]
pub(super) enum QueueRequest {
    /// Files, folders, or playlists for the worker to expand.
    Import { paths: Vec<PathBuf>, replace: bool },
    /// Library tracks that replace (and start) or extend the queue. A
    /// selection that matched no tracks leaves the queue unchanged.
    Tracks { tracks: Vec<PathBuf>, replace: bool },
    /// A `Replace` or `Append` queue command.
    Command(BridgeQueueCommand),
}

/// A change ready to apply, with any warning to show for it.
#[derive(Debug, Clone)]
pub(super) struct QueueChange {
    pub(super) command: Option<BridgeQueueCommand>,
    pub(super) warning: Option<String>,
}

struct PendingChange {
    id: u64,
    /// `None` while the worker is expanding the request.
    change: Option<QueueChange>,
}

struct ImportJob {
    paths: Vec<PathBuf>,
    replace: bool,
    id: u64,
    generation: u64,
}

pub(super) struct ImportResult {
    outcome: ImportExpandOutcome,
    replace: bool,
    id: u64,
    generation: u64,
}

pub(super) struct ImportWorker {
    tx: Sender<ImportJob>,
    pub(super) results: Receiver<ImportResult>,
    /// Advanced when pending changes are discarded, so the worker stops
    /// expanding them and their results are ignored.
    generation: Arc<AtomicU64>,
    next_id: u64,
    pending: VecDeque<PendingChange>,
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
                                id: job.id,
                                generation: job.generation,
                            })
                            .is_err()
                    {
                        break;
                    }
                }
            });
        Self::with_channels(tx, results, generation)
    }

    fn with_channels(
        tx: Sender<ImportJob>,
        results: Receiver<ImportResult>,
        generation: Arc<AtomicU64>,
    ) -> Self {
        Self {
            tx,
            results,
            generation,
            next_id: 0,
            pending: VecDeque::new(),
        }
    }

    /// Queue `request` behind earlier changes and return the changes that can
    /// apply now, in order.
    pub(super) fn submit(&mut self, request: QueueRequest) -> Result<Vec<QueueChange>, String> {
        let replace = match &request {
            QueueRequest::Tracks { tracks, .. } if tracks.is_empty() => return Ok(Vec::new()),
            QueueRequest::Import { replace, .. } | QueueRequest::Tracks { replace, .. } => *replace,
            QueueRequest::Command(command) => matches!(command, BridgeQueueCommand::Replace { .. }),
        };
        if replace {
            self.discard_pending();
        }
        self.next_id = self.next_id.wrapping_add(1);
        let id = self.next_id;
        let change = match request {
            QueueRequest::Import { paths, replace } => {
                self.tx
                    .send(ImportJob {
                        paths,
                        replace,
                        id,
                        generation: self.generation.load(Ordering::Relaxed),
                    })
                    .map_err(|_| "import worker unavailable".to_string())?;
                None
            }
            QueueRequest::Tracks { tracks, replace } => Some(QueueChange {
                command: tracks_command(tracks, replace),
                warning: None,
            }),
            QueueRequest::Command(command) => Some(QueueChange {
                command: Some(command),
                warning: None,
            }),
        };
        self.pending.push_back(PendingChange { id, change });
        Ok(self.take_ready())
    }

    /// Record a finished import and return the changes that can apply now.
    pub(super) fn complete(&mut self, result: ImportResult) -> Vec<QueueChange> {
        if result.generation != self.generation.load(Ordering::Relaxed) {
            return Vec::new();
        }
        let Some(pending) = self
            .pending
            .iter_mut()
            .find(|pending| pending.id == result.id)
        else {
            return Vec::new();
        };
        let warning = format_import_warning(
            if result.replace { "open" } else { "append" },
            &result.outcome,
        );
        pending.change = Some(QueueChange {
            command: tracks_command(result.outcome.tracks, result.replace),
            warning,
        });
        self.take_ready()
    }

    /// Drop every change that has not been applied yet.
    pub(super) fn discard_pending(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
        self.pending.clear();
    }

    /// Rename tracks in changes that are waiting to apply. Imports still
    /// expanding are left alone: their paths may already have the new names.
    pub(super) fn remap_pending_paths(&mut self, renames: &PathRenames) {
        let commands = self
            .pending
            .iter_mut()
            .filter_map(|pending| pending.change.as_mut()?.command.as_mut());
        for command in commands {
            if let BridgeQueueCommand::Replace { tracks, .. } | BridgeQueueCommand::Append(tracks) =
                command
            {
                for path in tracks {
                    remap_path(path, renames);
                }
            }
        }
    }

    /// Remove the resolved changes at the front, which no longer wait on
    /// anything.
    fn take_ready(&mut self) -> Vec<QueueChange> {
        let mut ready = Vec::new();
        while let Some(change) = self
            .pending
            .front_mut()
            .and_then(|pending| pending.change.take())
        {
            self.pending.pop_front();
            ready.push(change);
        }
        ready
    }
}

/// Replace the queue and start playback, or append; nothing without tracks.
fn tracks_command(tracks: Vec<PathBuf>, replace: bool) -> Option<BridgeQueueCommand> {
    if tracks.is_empty() {
        None
    } else if replace {
        Some(BridgeQueueCommand::Replace {
            tracks,
            autoplay: true,
        })
    } else {
        Some(BridgeQueueCommand::Append(tracks))
    }
}

impl Drop for ImportWorker {
    fn drop(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    /// A pipeline whose jobs the test completes by hand.
    fn manual_worker() -> (ImportWorker, Receiver<ImportJob>) {
        let (tx, jobs) = unbounded();
        let (_, results) = unbounded();
        (
            ImportWorker::with_channels(tx, results, Arc::new(AtomicU64::new(0))),
            jobs,
        )
    }

    fn finish(job: &ImportJob, tracks: Vec<PathBuf>) -> ImportResult {
        let mut outcome = ImportExpandOutcome::default();
        outcome.tracks = tracks;
        ImportResult {
            outcome,
            replace: job.replace,
            id: job.id,
            generation: job.generation,
        }
    }

    fn commands(changes: Vec<QueueChange>) -> Vec<BridgeQueueCommand> {
        changes
            .into_iter()
            .filter_map(|change| change.command)
            .collect()
    }

    fn tracks_of(command: &BridgeQueueCommand) -> (&'static str, Vec<PathBuf>) {
        match command {
            BridgeQueueCommand::Replace { tracks, autoplay } => {
                assert!(autoplay, "replacements start playback");
                ("replace", tracks.clone())
            }
            BridgeQueueCommand::Append(tracks) => ("append", tracks.clone()),
            other => panic!("unexpected queue command {other:?}"),
        }
    }

    #[test]
    fn changes_wait_behind_an_earlier_import_and_apply_in_order() {
        let (mut worker, jobs) = manual_worker();
        let ready = worker
            .submit(QueueRequest::Import {
                paths: vec![p("/import/track.flac")],
                replace: true,
            })
            .expect("queued");
        assert!(ready.is_empty(), "the import has not been expanded yet");
        let ready = worker
            .submit(QueueRequest::Tracks {
                tracks: vec![p("/album/1.flac"), p("/album/2.flac")],
                replace: false,
            })
            .expect("queued");
        assert!(ready.is_empty(), "the append waits for the earlier import");

        let job = jobs.try_recv().expect("import job");
        let applied = commands(worker.complete(finish(&job, vec![p("/import/track.flac")])));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![
                ("replace", vec![p("/import/track.flac")]),
                ("append", vec![p("/album/1.flac"), p("/album/2.flac")]),
            ]
        );
        assert!(worker.pending.is_empty());
    }

    #[test]
    fn resolved_changes_apply_immediately_when_nothing_is_pending() {
        let (mut worker, _jobs) = manual_worker();
        let applied = commands(
            worker
                .submit(QueueRequest::Tracks {
                    tracks: vec![p("/album/1.flac")],
                    replace: false,
                })
                .expect("applied"),
        );
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![("append", vec![p("/album/1.flac")])]
        );
        assert!(worker
            .submit(QueueRequest::Tracks {
                tracks: Vec::new(),
                replace: true,
            })
            .expect("no-op")
            .is_empty());
    }

    #[test]
    fn imports_complete_in_submission_order_even_when_results_arrive_reversed() {
        let (mut worker, jobs) = manual_worker();
        for path in ["/first.m3u", "/second.m3u"] {
            worker
                .submit(QueueRequest::Import {
                    paths: vec![p(path)],
                    replace: false,
                })
                .expect("queued");
        }
        let first = jobs.try_recv().expect("first job");
        let second = jobs.try_recv().expect("second job");
        assert_eq!(first.paths, vec![p("/first.m3u")]);
        assert!(worker
            .complete(finish(&second, vec![p("/2.flac")]))
            .is_empty());
        let applied = commands(worker.complete(finish(&first, vec![p("/1.flac")])));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![
                ("append", vec![p("/1.flac")]),
                ("append", vec![p("/2.flac")])
            ]
        );
    }

    #[test]
    fn a_replacement_discards_an_append_that_arrived_before_it() {
        let (mut worker, jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/big-folder")],
                replace: false,
            })
            .expect("queued");
        let append = jobs.try_recv().expect("append job");
        let applied = commands(
            worker
                .submit(QueueRequest::Tracks {
                    tracks: vec![p("/album/1.flac")],
                    replace: true,
                })
                .expect("applied"),
        );
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![("replace", vec![p("/album/1.flac")])]
        );
        assert!(
            worker
                .complete(finish(&append, vec![p("/big-folder/1.flac")]))
                .is_empty(),
            "the discarded append must not land after the replacement"
        );
        assert_ne!(
            append.generation,
            worker.generation.load(Ordering::Relaxed),
            "the worker stops expanding discarded imports"
        );
    }

    #[test]
    fn an_import_replacement_discards_earlier_pending_changes() {
        let (mut worker, jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/slow.m3u")],
                replace: false,
            })
            .expect("queued");
        worker
            .submit(QueueRequest::Tracks {
                tracks: vec![p("/album/1.flac")],
                replace: false,
            })
            .expect("queued");
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/track.flac")],
                replace: true,
            })
            .expect("queued");
        let _discarded = jobs.try_recv().expect("discarded job");
        let replacement = jobs.try_recv().expect("replacement job");
        let applied = commands(worker.complete(finish(&replacement, vec![p("/track.flac")])));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![("replace", vec![p("/track.flac")])]
        );
    }

    #[test]
    fn a_failed_import_reports_its_warning_without_blocking_later_changes() {
        let (mut worker, jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/missing.flac")],
                replace: false,
            })
            .expect("queued");
        worker
            .submit(QueueRequest::Command(BridgeQueueCommand::Append(vec![p(
                "/later.flac",
            )])))
            .expect("queued");
        let job = jobs.try_recv().expect("job");
        let mut result = finish(&job, Vec::new());
        result.outcome = super::super::commands::expand_import_paths_cancellable(
            vec![p("/definitely/missing/ferrous-import.flac")],
            &|| false,
        );
        let changes = worker.complete(result);
        assert_eq!(changes.len(), 2);
        assert!(changes[0].command.is_none());
        assert!(changes[0]
            .warning
            .as_deref()
            .is_some_and(|warning| warning.contains("1 missing")));
        assert!(matches!(
            &changes[1].command,
            Some(BridgeQueueCommand::Append(tracks)) if tracks == &vec![p("/later.flac")]
        ));
    }

    #[test]
    fn renames_reach_changes_waiting_behind_an_import() {
        let (mut worker, _jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/slow.m3u")],
                replace: false,
            })
            .expect("queued");
        worker
            .submit(QueueRequest::Tracks {
                tracks: vec![p("/album/old.flac"), p("/album/other.flac")],
                replace: false,
            })
            .expect("queued");
        worker.remap_pending_paths(&PathRenames::from([(
            p("/album/old.flac"),
            p("/album/new.flac"),
        )]));
        let waiting = worker
            .pending
            .back()
            .and_then(|pending| pending.change.clone());
        assert!(matches!(
            waiting.and_then(|change| change.command),
            Some(BridgeQueueCommand::Append(tracks))
                if tracks == vec![p("/album/new.flac"), p("/album/other.flac")]
        ));
    }
}
