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
//! filesystem before it, and a result that follows read it after. An import's
//! input paths follow the renames that finish before it reads them.

use super::commands::{
    expand_import_paths_cancellable, format_import_warning, ImportExpandOutcome,
};
use super::BridgeQueueCommand;
use crate::library::{IndexedTrack, TrackFileFingerprint};
use crate::playback::{remap_path, PathRenames};
use crossbeam_channel::{unbounded, Receiver, Sender};
use std::collections::{HashMap, VecDeque};
use std::ops::ControlFlow;
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
    /// The inputs as the filesystem was after rename `base`.
    paths: Vec<PathBuf>,
    replace: bool,
    id: u64,
    generation: u64,
    base: u64,
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
    /// Files renamed on disk by rename `number`, as one map.
    Renamed {
        number: u64,
        renames: Arc<PathRenames>,
        details: RenamedDetails,
    },
}

/// What a rename read from the files it renamed, so that the bridge does
/// not read them again.
#[derive(Debug, Default)]
pub(crate) struct RenamedDetails {
    /// What the library index holds for each renamed file, by new path.
    pub(crate) indexed: HashMap<PathBuf, IndexedTrack>,
    /// The state of each renamed file at its new path.
    pub(crate) fingerprints: HashMap<PathBuf, TrackFileFingerprint>,
    /// The files that could not be read fully, for the user.
    pub(crate) problem: Option<String>,
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
/// An import's input paths follow renames the same way its results do. The
/// bridge submits them as it knows the filesystem, after the last rename whose
/// map it has applied, and the job records that rename's number as its base.
/// The gate keeps the maps of finished renames in a log, and before each read
/// the import renames its inputs by every map logged since its base or its
/// previous read, in order and once each. So a file the import was given, or
/// found during a read that a rename then overlapped, is read again under its
/// new name. Bases never decrease and the worker imports in order, so the log
/// drops maps up to a job's base when the job starts, and maps the bridge has
/// applied while no import is pending.
///
/// Two inputs do not follow renames:
/// - A playlist's entries are text the app does not rewrite. An entry naming
///   a file renamed before the playlist is read no longer exists, and is
///   reported as missing like any other missing entry.
/// - Paths from outside the bridge, such as an external drag and drop,
///   describe the filesystem when they were sent, not after the bridge's last
///   rename. One sent after a rename whose map the bridge has not yet applied
///   gets that map again; one sent from a UI that has not yet shown a rename
///   misses it. Both windows last only while a message is in transit, and
///   are accepted.
///
/// The mutex guards only this bookkeeping and the send on the unbounded
/// channel, never filesystem I/O. An import waits while a rename is in
/// progress, which is bounded by that rename; a rename never waits for an
/// import. The bridge thread only takes the mutex briefly, to wake an import
/// whose request it discarded and to drop maps from the log; the UI thread
/// never takes it.
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
    /// The maps of finished renames that changed something, by number, that
    /// a pending import may not have applied to its inputs yet.
    log: VecDeque<(u64, Arc<PathRenames>)>,
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
    renames: Option<(PathRenames, RenamedDetails)>,
}

impl Renaming<'_> {
    /// Send `renames` and `details` to the bridge and finish.
    fn finish(mut self, renames: PathRenames, details: RenamedDetails) {
        self.renames = Some((renames, details));
    }
}

impl Drop for Renaming<'_> {
    fn drop(&mut self) {
        let mut sequence = self.gate.lock();
        if let Some((renames, details)) = self
            .renames
            .take()
            .filter(|(renames, _)| !renames.is_empty())
        {
            let renames = Arc::new(renames);
            let sent = self.gate.messages.send(ImportMessage::Renamed {
                number: self.number,
                renames: Arc::clone(&renames),
                details,
            });
            // The bridge has shut down if this fails; the renames on disk
            // stand, and no import is left to follow them.
            if sent.is_ok() {
                sequence.log.push_back((self.number, renames));
            }
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
        // No update can be left half done by a panic, so a panic elsewhere
        // cannot leave the sequence inconsistent.
        self.sequence.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Run `rename`, then send the paths it renamed to the bridge as one
    /// message. Imports that read the filesystem meanwhile read it again.
    /// Waits while another rename is in progress, and runs the rename itself
    /// on the calling thread, so it must not run on the UI or bridge thread.
    #[cfg(test)]
    pub(crate) fn rename<R>(&self, rename: impl FnOnce() -> (R, PathRenames)) -> R {
        self.rename_with_details(|| {
            let (result, renames) = rename();
            (result, renames, RenamedDetails::default())
        })
    }

    /// [`Self::rename`], also sending what the rename read from the renamed
    /// files.
    pub(crate) fn rename_with_details<R>(
        &self,
        rename: impl FnOnce() -> (R, PathRenames, RenamedDetails),
    ) -> R {
        let renaming = self.begin_rename();
        let (result, renames, details) = rename();
        renaming.finish(renames, details);
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
    /// `None` once the read is cancelled. Also returns, in order, the maps of
    /// the renames that finished after rename `cursor`, which the read must
    /// apply to its inputs.
    fn begin_read(
        &self,
        cursor: u64,
        cancelled: &impl Fn() -> bool,
    ) -> Option<(Read, Vec<Arc<PathRenames>>)> {
        let sequence = self
            .changed
            .wait_while(self.lock(), |sequence| sequence.renaming() && !cancelled())
            .unwrap_or_else(PoisonError::into_inner);
        if cancelled() {
            return None;
        }
        // Every logged rename has finished, so none is after `started`.
        let renames = sequence
            .log
            .iter()
            .filter(|(number, _)| *number > cursor)
            .map(|(_, renames)| Arc::clone(renames))
            .collect();
        Some((
            Read {
                after: sequence.started,
            },
            renames,
        ))
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

    /// Drop the maps of renames up to `number`, which no pending import needs.
    fn forget_renames_through(&self, number: u64) {
        let mut sequence = self.lock();
        while sequence
            .log
            .front()
            .is_some_and(|(renamed, _)| *renamed <= number)
        {
            sequence.log.pop_front();
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
    /// The number of the last rename whose map was applied here.
    renamed: u64,
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
        crate::worker_thread::spawn_worker("ferrous-import", move || {
            while let Ok(job) = rx.recv() {
                let expand = |paths: Vec<PathBuf>, cancelled: &dyn Fn() -> bool| {
                    // A panic fails this import alone, which still completes,
                    // so the changes queued behind it are not held up.
                    let count = paths.len();
                    crate::panic_guard::catch("importing files", || {
                        expand_import_paths_cancellable(paths, &cancelled)
                    })
                    .unwrap_or_else(|_| ImportExpandOutcome::all_unreadable(count))
                };
                if run_import(&reader, job, &active, expand).is_break() {
                    return;
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
            renamed: 0,
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
                        base: self.renamed,
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
        let ready = self.take_ready();
        self.forget_renames_if_idle();
        ready
    }

    /// Drop every change that has not been applied yet.
    pub(super) fn discard_pending(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
        self.gate.wake_readers();
        self.pending.clear();
        self.forget_renames_if_idle();
    }

    /// Rename tracks in changes that are waiting to apply, for rename
    /// `number`. Call it when `results` delivers the renames: imports without
    /// a result by then send it later, so they read the files after the
    /// renames and are left alone.
    pub(super) fn remap_pending_paths(&mut self, number: u64, renames: &PathRenames) {
        self.renamed = number;
        self.forget_renames_if_idle();
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

    /// With no import pending, every later import starts after the last
    /// rename applied here, so the gate's log no longer needs its maps.
    fn forget_renames_if_idle(&mut self) {
        if self.pending.is_empty() {
            self.gate.forget_renames_through(self.renamed);
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

/// Expand `job` with `expand` and send its result. Before each read, the
/// job's inputs follow the renames that finished since its base or its
/// previous read. Breaks once the bridge has shut down.
fn run_import(
    gate: &ImportGate,
    job: ImportJob,
    active: &AtomicU64,
    mut expand: impl FnMut(Vec<PathBuf>, &dyn Fn() -> bool) -> ImportExpandOutcome,
) -> ControlFlow<()> {
    let ImportJob {
        mut paths,
        replace,
        id,
        generation,
        base,
    } = job;
    let cancelled = || active.load(Ordering::Relaxed) != generation;
    // Later jobs start from this base or a later one.
    gate.forget_renames_through(base);
    let mut cursor = base;
    // A rename that started during the read may have moved files the read
    // already saw, or not yet saw, so the result is not sent and the paths
    // are read again under their new names. This is an optimistic
    // concurrency check on input that changed while it was read, not a
    // retry of a failure.
    while let Some((read, renames)) = gate.begin_read(cursor, &cancelled) {
        for renames in &renames {
            for path in &mut paths {
                remap_path(path, renames);
            }
        }
        cursor = read.after;
        let outcome = expand(paths.clone(), &cancelled);
        if cancelled() {
            break;
        }
        let result = ImportResult {
            outcome,
            replace,
            id,
            generation,
        };
        match gate.finish_read(read, result) {
            ReadEnd::Sent => break,
            ReadEnd::Overlapped => {}
            ReadEnd::Closed => return ControlFlow::Break(()),
        }
    }
    ControlFlow::Continue(())
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
    use std::cell::RefCell;
    use std::collections::HashSet;
    use std::time::Duration;

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
                ImportMessage::Renamed {
                    number, renames, ..
                } => {
                    worker.remap_pending_paths(number, &renames);
                }
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
        rename_on_disk(&worker, &[("/album/old.flac", "/album/new.flac")]);
        assert!(deliver(&mut worker).is_empty(), "waits for the slow import");
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
                ImportMessage::Renamed { .. } => "renamed",
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
                gate.begin_read(0, &|| {
                    let _ = checked_tx.send(());
                    false
                })
            })
        };
        // The reader checks for cancellation under the gate's lock before it
        // waits, and the rename needs that lock to finish.
        checked.recv().expect("the reader checked for cancellation");
        let map = renames(&[("/album/01.flac", "/album/02.flac")]);
        renaming.finish(map.clone(), RenamedDetails::default());
        let (read, logged) = reader
            .join()
            .expect("reader thread")
            .expect("not cancelled");
        assert_eq!(read, Read { after: 1 });
        assert_eq!(
            logged.iter().map(|renames| &**renames).collect::<Vec<_>>(),
            vec![&map],
            "the read applies the rename to its inputs"
        );
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
        let (done_tx, done) = unbounded();
        let reader = {
            let gate = Arc::clone(&gate);
            let active = Arc::clone(&worker.generation);
            std::thread::spawn(move || {
                let read = gate.begin_read(0, &|| {
                    let cancelled = active.load(Ordering::Relaxed) != job.generation;
                    let _ = checked_tx.send(cancelled);
                    cancelled
                });
                let _ = done_tx.send(read.map(|(read, _)| read));
            })
        };
        // The reader checks under the gate's lock just before it waits, and
        // discarding takes that lock to wake it.
        assert_eq!(checked.recv(), Ok(false), "the reader waits for the rename");
        worker.discard_pending();
        // Without the wakeup the reader would wait for the rename this thread
        // holds, so fail after a generous deadline instead of hanging, and
        // finish the rename before joining.
        let woke = done.recv_timeout(Duration::from_secs(10));
        drop(renaming);
        reader.join().expect("reader thread");
        assert_eq!(
            woke,
            Ok(None),
            "the import is dropped while the rename is still in progress"
        );
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

    /// Files on a pretend disk.
    struct Disk(RefCell<HashSet<PathBuf>>);

    impl Disk {
        fn new(files: &[&str]) -> Self {
            Self(RefCell::new(files.iter().map(|file| p(file)).collect()))
        }

        /// Rename files through `gate` as the tag editor does, all at once.
        fn rename(&self, gate: &ImportGate, pairs: &[(&str, &str)]) {
            gate.rename(|| {
                let renames = renames(pairs);
                let mut files = self.0.borrow_mut();
                let moved: Vec<_> = renames
                    .iter()
                    .filter(|(old, _)| files.remove(*old))
                    .map(|(_, new)| new.clone())
                    .collect();
                files.extend(moved);
                ((), renames)
            });
        }

        /// Expand `paths` as the worker does: the ones on disk are tracks.
        fn expand(&self, paths: Vec<PathBuf>) -> ImportExpandOutcome {
            let files = self.0.borrow();
            let mut outcome = ImportExpandOutcome::default();
            outcome.tracks = paths
                .into_iter()
                .filter(|path| files.contains(path))
                .collect();
            outcome
        }
    }

    /// Run the next job as the worker does, reading with `expand`, and
    /// return the inputs of each read.
    fn import_next(
        worker: &ImportWorker,
        jobs: &Receiver<ImportJob>,
        mut expand: impl FnMut(&[Vec<PathBuf>]) -> ImportExpandOutcome,
    ) -> Vec<Vec<PathBuf>> {
        let job = jobs.try_recv().expect("job");
        let mut reads = Vec::new();
        let flow = run_import(&worker.gate, job, &worker.generation, |paths, _| {
            reads.push(paths);
            expand(&reads)
        });
        assert!(flow.is_continue(), "the bridge is listening");
        reads
    }

    fn logged(worker: &ImportWorker) -> Vec<u64> {
        worker
            .gate
            .lock()
            .log
            .iter()
            .map(|(number, _)| *number)
            .collect()
    }

    fn submit_import(worker: &mut ImportWorker, paths: &[&str]) {
        worker
            .submit(QueueRequest::Import {
                paths: paths.iter().map(|path| p(path)).collect(),
                replace: false,
            })
            .expect("queued");
    }

    #[test]
    fn a_file_renamed_during_its_read_is_read_again_under_its_new_name() {
        for pairs in OVERLAPPING {
            let before: Vec<_> = pairs.iter().map(|(old, _)| *old).collect();
            let disk = Disk::new(&before);
            let (mut worker, jobs) = manual_worker();
            submit_import(&mut worker, &before);
            let reads = import_next(&worker, &jobs, |reads| {
                let found = disk.expand(reads.last().cloned().unwrap_or_default());
                if reads.len() == 1 {
                    // The read found the files under their old names.
                    disk.rename(&worker.gate, pairs);
                }
                found
            });
            let before: Vec<_> = before.into_iter().map(p).collect();
            let after: Vec<_> = pairs.iter().map(|(_, new)| p(new)).collect();
            assert_eq!(reads, vec![before, after.clone()], "{pairs:?}");
            let applied = commands(deliver(&mut worker));
            assert_eq!(
                applied.iter().map(tracks_of).collect::<Vec<_>>(),
                vec![("append", after)],
                "{pairs:?}"
            );
        }
    }

    #[test]
    fn an_input_renamed_while_its_import_waits_is_read_under_its_new_name() {
        let disk = Disk::new(&["/slow.flac", "/music/old.flac"]);
        let (mut worker, jobs) = manual_worker();
        submit_import(&mut worker, &["/slow.flac"]);
        submit_import(&mut worker, &["/music/old.flac"]);
        let slow = import_next(&worker, &jobs, |reads| {
            if reads.len() == 1 {
                disk.rename(&worker.gate, &[("/music/old.flac", "/music/new.flac")]);
            }
            disk.expand(vec![p("/slow.flac")])
        });
        assert_eq!(slow.len(), 2, "the rename overlapped the slow read");
        let waited = import_next(&worker, &jobs, |reads| {
            disk.expand(reads.last().cloned().unwrap_or_default())
        });
        assert_eq!(waited, vec![vec![p("/music/new.flac")]]);
        let applied = commands(deliver(&mut worker));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![
                ("append", vec![p("/slow.flac")]),
                ("append", vec![p("/music/new.flac")]),
            ]
        );
    }

    #[test]
    fn inputs_follow_each_rename_once() {
        let disk = Disk::new(&["/album/01.flac", "/album/02.flac"]);
        let (mut worker, jobs) = manual_worker();
        submit_import(&mut worker, &["/album/01.flac", "/album/02.flac"]);
        // Renumber before the first read, then swap during it.
        disk.rename(&worker.gate, OVERLAPPING[0]);
        let swap = [
            ("/album/02.flac", "/album/03.flac"),
            ("/album/03.flac", "/album/02.flac"),
        ];
        let reads = import_next(&worker, &jobs, |reads| {
            let found = disk.expand(reads.last().cloned().unwrap_or_default());
            if reads.len() == 1 {
                disk.rename(&worker.gate, &swap);
            }
            found
        });
        assert_eq!(
            reads,
            vec![
                vec![p("/album/02.flac"), p("/album/03.flac")],
                vec![p("/album/03.flac"), p("/album/02.flac")],
            ]
        );
        assert_eq!(
            message_kinds(&worker),
            vec!["renamed", "renamed", "finished"]
        );
    }

    #[test]
    fn the_bridge_forgets_renames_once_no_import_is_pending() {
        let disk = Disk::new(&["/music/a.flac"]);
        let (mut worker, jobs) = manual_worker();
        disk.rename(&worker.gate, &[("/music/a.flac", "/music/b.flac")]);
        assert_eq!(logged(&worker), vec![1]);
        assert!(deliver(&mut worker).is_empty());
        assert!(logged(&worker).is_empty(), "nothing is pending");

        submit_import(&mut worker, &["/music/b.flac"]);
        disk.rename(&worker.gate, &[("/music/b.flac", "/music/c.flac")]);
        assert!(deliver(&mut worker).is_empty());
        assert_eq!(logged(&worker), vec![2], "the pending import needs it");
        let reads = import_next(&worker, &jobs, |reads| {
            disk.expand(reads.last().cloned().unwrap_or_default())
        });
        assert_eq!(reads, vec![vec![p("/music/c.flac")]]);
        assert_eq!(logged(&worker), vec![2]);
        let applied = commands(deliver(&mut worker));
        assert_eq!(
            applied.iter().map(tracks_of).collect::<Vec<_>>(),
            vec![("append", vec![p("/music/c.flac")])]
        );
        assert!(logged(&worker).is_empty(), "the import finished");
    }

    #[test]
    fn an_import_forgets_renames_up_to_its_base_when_it_starts() {
        let disk = Disk::new(&["/music/a.flac", "/music/x.flac"]);
        let (mut worker, jobs) = manual_worker();
        submit_import(&mut worker, &["/music/a.flac"]);
        disk.rename(&worker.gate, &[("/music/a.flac", "/music/b.flac")]);
        assert!(deliver(&mut worker).is_empty());
        // Submitted after the bridge applied rename 1.
        submit_import(&mut worker, &["/music/x.flac"]);
        let first = import_next(&worker, &jobs, |reads| {
            disk.expand(reads.last().cloned().unwrap_or_default())
        });
        assert_eq!(first, vec![vec![p("/music/b.flac")]]);
        assert_eq!(logged(&worker), vec![1], "the first import started from 0");
        let second = import_next(&worker, &jobs, |reads| {
            disk.expand(reads.last().cloned().unwrap_or_default())
        });
        assert_eq!(second, vec![vec![p("/music/x.flac")]]);
        assert!(logged(&worker).is_empty(), "the second started from 1");
    }

    #[test]
    fn a_discarded_import_lets_the_bridge_forget_renames() {
        let (mut worker, _jobs) = manual_worker();
        submit_import(&mut worker, &["/music/a.flac"]);
        rename_on_disk(&worker, &[("/music/a.flac", "/music/b.flac")]);
        assert!(deliver(&mut worker).is_empty());
        assert_eq!(logged(&worker), vec![1]);
        worker.discard_pending();
        assert!(logged(&worker).is_empty());
    }
}
