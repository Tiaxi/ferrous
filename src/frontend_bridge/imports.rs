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
//!
//! File renames reach the bridge through the same channel as import results,
//! ordered by [`ImportGate`]: a result that precedes a rename read the
//! filesystem before it, and a result that follows read it after.

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
    Arc, Condvar, Mutex, MutexGuard, PoisonError,
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

/// What the import worker's results channel delivers to the bridge.
pub(super) enum ImportMessage {
    Finished(ImportResult),
    /// Files renamed on disk, as one map.
    Renamed(Arc<PathRenames>),
}

/// Orders file renames against the import worker's reads of the filesystem,
/// without making either wait for the other's I/O.
///
/// Renames are numbered and run one at a time. A read records the number of
/// the last rename, waiting first for a rename in progress to finish. It then
/// reads without holding anything, and sends its result only if no rename
/// started in the meantime. The check and the send happen under the same lock
/// a rename takes to start, and a rename sends its map under that lock before
/// it counts as finished. So a result that was sent had every earlier rename's
/// map ahead of it in the channel, overlapped no rename, and comes before the
/// maps of later renames: the results channel lists every rename after the
/// results that read the files before it, and before the results that read
/// them after it. A read that a rename overlapped is read again.
///
/// The mutex guards only this bookkeeping and the send on the unbounded
/// channel, never filesystem I/O. An import waits while a rename is in
/// progress, which is bounded by that rename; a rename never waits for an
/// import. The bridge thread only takes the mutex briefly to wake an import
/// whose request it discarded; the UI thread never takes it.
pub(crate) struct ImportGate {
    sequence: Mutex<RenameSequence>,
    /// Signalled when a rename finishes or imports are discarded.
    changed: Condvar,
    messages: Sender<ImportMessage>,
}

#[derive(Default)]
struct RenameSequence {
    /// The number of renames started so far.
    started: u64,
    /// The number of the last rename that finished. A rename is in progress
    /// while it differs from `started`.
    finished: u64,
}

impl RenameSequence {
    fn renaming(&self) -> bool {
        self.started != self.finished
    }
}

/// A read of the filesystem that began once rename `after` had finished.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Read {
    after: u64,
}

#[derive(Debug, PartialEq, Eq)]
enum ReadEnd {
    Sent,
    /// A rename started during the read, so its result was not sent.
    Overlapped,
    /// The bridge has shut down.
    Closed,
}

/// A rename in progress. It finishes when dropped, also if the rename
/// unwinds, so imports never wait for it forever.
struct Renaming<'a> {
    gate: &'a ImportGate,
    number: u64,
    renames: Option<PathRenames>,
}

impl Renaming<'_> {
    /// Send `renames` to the bridge and finish.
    fn finish(mut self, renames: PathRenames) {
        self.renames = Some(renames);
    }
}

impl Drop for Renaming<'_> {
    fn drop(&mut self) {
        let mut sequence = self.gate.lock();
        if let Some(renames) = self.renames.take().filter(|renames| !renames.is_empty()) {
            // The bridge has shut down if this fails; the renames on disk stand.
            let _ = self
                .gate
                .messages
                .send(ImportMessage::Renamed(Arc::new(renames)));
        }
        sequence.finished = self.number;
        drop(sequence);
        self.gate.changed.notify_all();
    }
}

impl ImportGate {
    fn new(messages: Sender<ImportMessage>) -> Self {
        Self {
            sequence: Mutex::new(RenameSequence::default()),
            changed: Condvar::new(),
            messages,
        }
    }

    fn lock(&self) -> MutexGuard<'_, RenameSequence> {
        // Every update is a single assignment, so a panic elsewhere cannot
        // leave the sequence inconsistent.
        self.sequence.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Run `rename`, then send the paths it renamed to the bridge as one
    /// message. Imports that read the filesystem meanwhile read it again.
    /// Waits while another rename is in progress, and runs the rename itself
    /// on the calling thread, so it must not run on the UI or bridge thread.
    pub(crate) fn rename<R>(&self, rename: impl FnOnce() -> (R, PathRenames)) -> R {
        let renaming = self.begin_rename();
        let (result, renames) = rename();
        renaming.finish(renames);
        result
    }

    fn begin_rename(&self) -> Renaming<'_> {
        let mut sequence = self
            .changed
            .wait_while(self.lock(), |sequence| sequence.renaming())
            .unwrap_or_else(PoisonError::into_inner);
        sequence.started = sequence.started.wrapping_add(1);
        Renaming {
            gate: self,
            number: sequence.started,
            renames: None,
        }
    }

    /// Wait until no rename is in progress and begin a read, or return
    /// `None` once the read is cancelled.
    fn begin_read(&self, cancelled: &impl Fn() -> bool) -> Option<Read> {
        let sequence = self
            .changed
            .wait_while(self.lock(), |sequence| sequence.renaming() && !cancelled())
            .unwrap_or_else(PoisonError::into_inner);
        (!cancelled()).then_some(Read {
            after: sequence.started,
        })
    }

    /// Begin a read unless a rename is in progress.
    #[cfg(test)]
    fn try_begin_read(&self) -> Option<Read> {
        let sequence = self.lock();
        (!sequence.renaming()).then_some(Read {
            after: sequence.started,
        })
    }

    /// Send the result of `read` unless a rename started during it.
    fn finish_read(&self, read: Read, result: ImportResult) -> ReadEnd {
        let sequence = self.lock();
        if sequence.started != read.after {
            return ReadEnd::Overlapped;
        }
        let sent = self.messages.send(ImportMessage::Finished(result));
        drop(sequence);
        if sent.is_ok() {
            ReadEnd::Sent
        } else {
            ReadEnd::Closed
        }
    }

    /// Wake imports waiting for a rename so they notice they were cancelled.
    fn wake_readers(&self) {
        // Taking the lock orders this after a waiting import checked for
        // cancellation, so the notification cannot slip in before it waits.
        drop(self.lock());
        self.changed.notify_all();
    }
}

pub(super) struct ImportWorker {
    tx: Sender<ImportJob>,
    pub(super) results: Receiver<ImportMessage>,
    gate: Arc<ImportGate>,
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
        let gate = Arc::new(ImportGate::new(result_tx));
        let generation = Arc::new(AtomicU64::new(0));
        let active = Arc::clone(&generation);
        let reader = Arc::clone(&gate);
        let _ = std::thread::Builder::new()
            .name("ferrous-import".into())
            .spawn(move || {
                while let Ok(job) = rx.recv() {
                    let cancelled = || active.load(Ordering::Relaxed) != job.generation;
                    // A rename that started during the read may have moved
                    // files the read already saw, or not yet saw, so the
                    // result is not sent and the paths are read again. This
                    // is an optimistic concurrency check on input that
                    // changed while it was read, not a retry of a failure.
                    while let Some(read) = reader.begin_read(&cancelled) {
                        let outcome =
                            expand_import_paths_cancellable(job.paths.clone(), &cancelled);
                        if cancelled() {
                            break;
                        }
                        let result = ImportResult {
                            outcome,
                            replace: job.replace,
                            id: job.id,
                            generation: job.generation,
                        };
                        match reader.finish_read(read, result) {
                            ReadEnd::Sent => break,
                            ReadEnd::Overlapped => {}
                            ReadEnd::Closed => return,
                        }
                    }
                }
            });
        Self::with_channels(tx, results, gate, generation)
    }

    fn with_channels(
        tx: Sender<ImportJob>,
        results: Receiver<ImportMessage>,
        gate: Arc<ImportGate>,
        generation: Arc<AtomicU64>,
    ) -> Self {
        Self {
            tx,
            results,
            gate,
            generation,
            next_id: 0,
            pending: VecDeque::new(),
        }
    }

    /// The gate that orders file renames against this worker's reads.
    pub(super) fn gate(&self) -> Arc<ImportGate> {
        Arc::clone(&self.gate)
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
        self.gate.wake_readers();
        self.pending.clear();
    }

    /// Rename tracks in changes that are waiting to apply. Call it when
    /// `results` delivers the renames: imports without a result by then send
    /// it later, so they read the files after the renames and are left alone.
    pub(super) fn remap_pending_paths(&mut self, renames: &PathRenames) {
        for pending in &mut self.pending {
            let Some(change) = pending.change.as_mut() else {
                continue;
            };
            if let Some(
                BridgeQueueCommand::Replace { tracks, .. } | BridgeQueueCommand::Append(tracks),
            ) = change.command.as_mut()
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
        self.gate.wake_readers();
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
        let (result_tx, results) = unbounded();
        (
            ImportWorker::with_channels(
                tx,
                results,
                Arc::new(ImportGate::new(result_tx)),
                Arc::new(AtomicU64::new(0)),
            ),
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

    /// Expand `job` as the worker does, finding `tracks` on disk.
    fn read(worker: &ImportWorker, job: &ImportJob, tracks: Vec<PathBuf>) {
        let gate = worker.gate();
        let read = gate.try_begin_read().expect("no rename in progress");
        assert_eq!(gate.finish_read(read, finish(job, tracks)), ReadEnd::Sent);
    }

    fn renames(pairs: &[(&str, &str)]) -> PathRenames {
        pairs.iter().map(|(old, new)| (p(old), p(new))).collect()
    }

    /// Rename files as the tag editor does.
    fn rename_on_disk(worker: &ImportWorker, pairs: &[(&str, &str)]) {
        worker.gate().rename(|| ((), renames(pairs)));
    }

    /// Handle the results channel as the bridge does, returning the changes
    /// that reach the queue.
    fn deliver(worker: &mut ImportWorker) -> Vec<QueueChange> {
        let mut applied = Vec::new();
        while let Ok(message) = worker.results.try_recv() {
            match message {
                ImportMessage::Finished(result) => applied.extend(worker.complete(result)),
                ImportMessage::Renamed(renames) => worker.remap_pending_paths(&renames),
            }
        }
        applied
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
        worker.remap_pending_paths(&renames(&[("/album/old.flac", "/album/new.flac")]));
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

    /// Renumbering `01 -> 02, 02 -> 03` and swapping `a <-> b` both rename a
    /// file to another selected file's old name.
    const OVERLAPPING: [&[(&str, &str)]; 2] = [
        &[
            ("/album/01.flac", "/album/02.flac"),
            ("/album/02.flac", "/album/03.flac"),
        ],
        &[
            ("/album/a.flac", "/album/b.flac"),
            ("/album/b.flac", "/album/a.flac"),
        ],
    ];

    #[test]
    fn an_import_that_reads_after_overlapping_renames_keeps_its_paths() {
        for pairs in OVERLAPPING {
            let (mut worker, jobs) = manual_worker();
            worker
                .submit(QueueRequest::Import {
                    paths: vec![p("/album")],
                    replace: false,
                })
                .expect("queued");
            rename_on_disk(&worker, pairs);
            let after: Vec<_> = pairs.iter().map(|(_, new)| p(new)).collect();
            read(&worker, &jobs.try_recv().expect("job"), after.clone());
            let applied = commands(deliver(&mut worker));
            assert_eq!(
                applied.iter().map(tracks_of).collect::<Vec<_>>(),
                vec![("append", after)],
                "{pairs:?}"
            );
        }
    }

    #[test]
    fn an_import_waiting_behind_another_is_renamed_once() {
        for pairs in OVERLAPPING {
            let (mut worker, jobs) = manual_worker();
            for path in ["/slow.m3u", "/album"] {
                worker
                    .submit(QueueRequest::Import {
                        paths: vec![p(path)],
                        replace: false,
                    })
                    .expect("queued");
            }
            let slow = jobs.try_recv().expect("slow job");
            let album = jobs.try_recv().expect("album job");
            let before: Vec<_> = pairs.iter().map(|(old, _)| p(old)).collect();
            read(&worker, &album, before);
            rename_on_disk(&worker, pairs);
            assert!(deliver(&mut worker).is_empty(), "waits for the slow import");
            read(&worker, &slow, vec![p("/slow.flac")]);
            let applied = commands(deliver(&mut worker));
            let after: Vec<_> = pairs.iter().map(|(_, new)| p(new)).collect();
            assert_eq!(
                applied.iter().map(tracks_of).collect::<Vec<_>>(),
                vec![("append", vec![p("/slow.flac")]), ("append", after)],
                "{pairs:?}"
            );
        }
    }

    #[test]
    fn renames_skip_an_import_submitted_after_them() {
        let (mut worker, jobs) = manual_worker();
        rename_on_disk(&worker, &[("/music/a.flac", "/music/b.flac")]);
        // A new file now has the old name.
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/music/a.flac")],
                replace: false,
            })
            .expect("queued");
        read(
            &worker,
            &jobs.try_recv().expect("job"),
            vec![p("/music/a.flac")],
        );
        let applied = commands(deliver(&mut worker));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![("append", vec![p("/music/a.flac")])]
        );
    }

    fn message_kinds(worker: &ImportWorker) -> Vec<&'static str> {
        worker
            .results
            .try_iter()
            .map(|message| match message {
                ImportMessage::Finished(_) => "finished",
                ImportMessage::Renamed(_) => "renamed",
            })
            .collect()
    }

    #[test]
    fn a_rename_during_a_read_finishes_and_the_import_reads_again() {
        for pairs in OVERLAPPING {
            let (mut worker, jobs) = manual_worker();
            worker
                .submit(QueueRequest::Import {
                    paths: vec![p("/album")],
                    replace: false,
                })
                .expect("queued");
            let job = jobs.try_recv().expect("job");
            let gate = worker.gate();
            let read = gate.try_begin_read().expect("no rename in progress");
            // On the reading thread itself: the rename does not wait for it.
            rename_on_disk(&worker, pairs);
            let before: Vec<_> = pairs.iter().map(|(old, _)| p(old)).collect();
            assert_eq!(
                gate.finish_read(read, finish(&job, before)),
                ReadEnd::Overlapped,
                "{pairs:?}"
            );
            let after: Vec<_> = pairs.iter().map(|(_, new)| p(new)).collect();
            let reread = gate.try_begin_read().expect("the rename finished");
            assert_eq!(
                gate.finish_read(reread, finish(&job, after.clone())),
                ReadEnd::Sent
            );
            let applied = commands(deliver(&mut worker));
            assert_eq!(
                applied.iter().map(tracks_of).collect::<Vec<_>>(),
                vec![("append", after)],
                "{pairs:?}"
            );
        }
    }

    #[test]
    fn an_import_waits_for_a_rename_in_progress() {
        let (mut worker, jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/album")],
                replace: false,
            })
            .expect("queued");
        let job = jobs.try_recv().expect("job");
        let gate = worker.gate();
        let renaming = gate.begin_rename();
        assert_eq!(gate.try_begin_read(), None);

        let (checked_tx, checked) = unbounded();
        let reader = {
            let gate = Arc::clone(&gate);
            std::thread::spawn(move || {
                gate.begin_read(&|| {
                    let _ = checked_tx.send(());
                    false
                })
            })
        };
        // The reader checks for cancellation under the gate's lock before it
        // waits, and the rename needs that lock to finish.
        checked.recv().expect("the reader checked for cancellation");
        renaming.finish(renames(&[("/album/01.flac", "/album/02.flac")]));
        let read = reader
            .join()
            .expect("reader thread")
            .expect("not cancelled");
        assert_eq!(read, Read { after: 1 });
        assert_eq!(
            gate.finish_read(read, finish(&job, vec![p("/album/02.flac")])),
            ReadEnd::Sent
        );
        assert_eq!(message_kinds(&worker), vec!["renamed", "finished"]);
    }

    #[test]
    fn discarding_pending_changes_wakes_an_import_waiting_for_a_rename() {
        let (mut worker, jobs) = manual_worker();
        worker
            .submit(QueueRequest::Import {
                paths: vec![p("/album")],
                replace: false,
            })
            .expect("queued");
        let job = jobs.try_recv().expect("job");
        let gate = worker.gate();
        let renaming = gate.begin_rename();

        let (checked_tx, checked) = unbounded();
        let reader = {
            let gate = Arc::clone(&gate);
            let active = Arc::clone(&worker.generation);
            std::thread::spawn(move || {
                gate.begin_read(&|| {
                    let cancelled = active.load(Ordering::Relaxed) != job.generation;
                    let _ = checked_tx.send(cancelled);
                    cancelled
                })
            })
        };
        // The reader checks under the gate's lock just before it waits, and
        // discarding takes that lock to wake it.
        assert_eq!(checked.recv(), Ok(false), "the reader waits for the rename");
        worker.discard_pending();
        assert_eq!(
            reader.join().expect("reader thread"),
            None,
            "the import is dropped while the rename is still in progress"
        );
        drop(renaming);
        assert!(worker.results.is_empty());
    }

    #[test]
    fn a_rename_that_panics_still_finishes() {
        let (worker, _jobs) = manual_worker();
        let gate = worker.gate();
        let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            gate.rename::<()>(|| panic!("the rename failed"));
        }));
        assert!(panicked.is_err());
        assert!(worker.results.is_empty(), "nothing to report");
        assert_eq!(gate.try_begin_read(), Some(Read { after: 1 }));
        rename_on_disk(&worker, &[("/music/a.flac", "/music/b.flac")]);
        assert_eq!(gate.try_begin_read(), Some(Read { after: 2 }));
        assert_eq!(message_kinds(&worker), vec!["renamed"]);
    }

    #[test]
    fn a_rename_that_changes_nothing_sends_nothing() {
        let (worker, _jobs) = manual_worker();
        assert_eq!(worker.gate().rename(|| (7, PathRenames::new())), 7);
        assert!(worker.results.is_empty());
        assert_eq!(worker.gate().try_begin_read(), Some(Read { after: 1 }));
    }
}
