// SPDX-License-Identifier: GPL-3.0-or-later

//! The library watcher: watches the roots with inotify on its own thread,
//! and sends the library worker the paths that changed, a few at a time.
//!
//! - Events are collected per root until none has arrived for
//!   [`WatchTuning::quiet`], or for at most [`WatchTuning::max_delay`]
//!   since the first, and then sent as one [`LibraryCommand::ScanPaths`].
//!   The worker merges it with other scans and runs it between commands.
//! - A changed audio file is scanned; a changed image that a folder's
//!   cover could be chosen from scans its folder, so the covers of the
//!   tracks there are checked again.
//! - When watching starts, at startup or when it is turned on, every root
//!   is rescanned, so that changes made meanwhile appear.
//! - The worker is told when the roots it sent are watched, and walks a
//!   root it added only then: a change after the watches are set up is
//!   seen by the walk or by them. The worker also passes on the watching
//!   setting, so that it comes in order with the roots.
//! - A root that cannot be watched (a network or FUSE file system, the
//!   inotify watch limit, a folder that is gone) is rescanned every
//!   [`WatchTuning::periodic`] instead, and the worker publishes it, with
//!   the reason, in [`super::LibraryStatus::unwatched_roots`].
//! - When the mount table changes, a root whose folder is now another one,
//!   such as a drive mounted there, or that is back, is watched again and
//!   rescanned. So is a root with other mounts below it, at most once every
//!   [`WatchTuning::mount_rescan`].

mod inotify;
mod mounts;

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs::File;
use std::io;
use std::os::unix::fs::MetadataExt as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{unbounded, Receiver, RecvTimeoutError, Sender, TryRecvError};
use walkdir::WalkDir;

use self::inotify::{Inotify, RawEvent, Waker};
use super::{is_supported_audio, LibraryCommand, UnwatchedRoot};
use crate::folder_cover::is_cover_image;
use crate::worker_thread::JoinableThread;

/// How the watcher batches changes and rescans the roots it cannot watch.
#[derive(Debug, Clone, Copy)]
pub(super) struct WatchTuning {
    /// How long a root's changes wait for more before they are scanned.
    pub(super) quiet: Duration,
    /// The longest a change waits to be scanned while more keep arriving.
    pub(super) max_delay: Duration,
    /// How often a root that is not watched is rescanned.
    pub(super) periodic: Duration,
    /// Above this many changed paths, a root is rescanned instead.
    pub(super) max_paths: usize,
    /// The shortest time between two rescans of a root for mount changes
    /// below it.
    pub(super) mount_rescan: Duration,
}

impl Default for WatchTuning {
    /// - `quiet`, 500 ms: a program saving or copying several files, the
    ///   tag editor's bulk saves included, does so with gaps of a few to a
    ///   few hundred milliseconds, so its changes become one scan.
    /// - `max_delay`, 5 s: a copy that takes minutes shows its first albums
    ///   while it runs, one small scan every few seconds.
    /// - `periodic`, 30 min: a rescan of a network share stats every file
    ///   over the network; changes made elsewhere appear within half an
    ///   hour, or at once with Rescan.
    /// - `max_paths`, 2048: past that, scanning each path separately costs
    ///   about as much as one walk of the root.
    /// - `mount_rescan`, 5 min: a mount change below a root rescans all of
    ///   it, which for a large one on a slow disk takes a minute or more. A
    ///   mount that keeps coming and going, such as a flaky automount, then
    ///   rescans it at most once every 5 minutes instead of all the time.
    ///   Its changes between are merged into the next rescan, not dropped,
    ///   and the root's local changes are still scanned within seconds. A
    ///   root whose own folder is replaced, such as a card swapped at a
    ///   fixed mount point, is rescanned without this wait.
    fn default() -> Self {
        Self {
            quiet: Duration::from_millis(500),
            max_delay: Duration::from_secs(5),
            periodic: Duration::from_secs(30 * 60),
            max_paths: 2048,
            mount_rescan: Duration::from_secs(5 * 60),
        }
    }
}

/// What the watcher is told.
#[derive(Debug)]
enum WatchMessage {
    Enabled(bool),
    /// The configured roots. The first message comes once the library is
    /// ready, and starts watching. Once the watches of these roots are set
    /// up, or the roots are noted as not watched, the watcher sends the
    /// number of these messages it has handled; see
    /// [`WatchService::spawn`].
    Roots(Vec<PathBuf>),
    Stop,
}

/// Sends the watcher messages, and wakes it.
#[derive(Clone)]
pub(super) struct WatchHandle {
    tx: Sender<WatchMessage>,
    waker: Option<Arc<Waker>>,
}

impl WatchHandle {
    /// Tells the watcher the configured roots.
    pub(super) fn set_roots(&self, roots: Vec<PathBuf>) {
        self.send(WatchMessage::Roots(roots));
    }

    /// Turns watching on or off.
    pub(super) fn set_enabled(&self, enabled: bool) {
        self.send(WatchMessage::Enabled(enabled));
    }

    fn send(&self, message: WatchMessage) {
        if self.tx.send(message).is_ok() {
            if let Some(waker) = &self.waker {
                waker.wake();
            }
        }
    }
}

/// The watcher's thread.
pub(super) struct WatchService {
    handle: WatchHandle,
    stop: Arc<AtomicBool>,
    thread: Option<JoinableThread>,
}

impl WatchService {
    /// Starts the watcher, with watching off until it is turned on. The
    /// first receiver gets its commands for the library worker. The second
    /// gets, after each roots message the watcher handled, how many it has
    /// handled; it closes once the watcher ends.
    pub(super) fn spawn(tuning: WatchTuning) -> (Self, Receiver<LibraryCommand>, Receiver<u64>) {
        let (commands_tx, commands_rx) = unbounded();
        let (tx, messages) = unbounded();
        let (roots_watched_tx, roots_watched) = unbounded();
        let stop = Arc::new(AtomicBool::new(false));
        let mut watcher = Watcher::new(tuning, messages, commands_tx, Arc::clone(&stop));
        watcher.roots_watched = Some(roots_watched_tx);
        let waker = watcher.waker.clone();
        let thread =
            crate::worker_thread::spawn_worker("ferrous-library-watch", move || watcher.run());
        (
            Self {
                handle: WatchHandle { tx, waker },
                stop,
                thread,
            },
            commands_rx,
            roots_watched,
        )
    }

    pub(super) fn handle(&self) -> WatchHandle {
        self.handle.clone()
    }

    /// Tells the watcher to stop, also in the middle of adding watches.
    pub(super) fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
        self.handle.send(WatchMessage::Stop);
    }

    /// Waits until `deadline` for the watcher to end; see
    /// [`JoinableThread::join_by`].
    pub(super) fn join_by(&mut self, deadline: Instant) -> bool {
        self.thread
            .take()
            .is_none_or(|thread| thread.join_by(deadline))
    }
}

/// The inotify events a folder is watched for.
const WATCH_MASK: u32 = libc::IN_CREATE
    | libc::IN_DELETE
    | libc::IN_CLOSE_WRITE
    | libc::IN_MOVED_FROM
    | libc::IN_MOVED_TO
    | libc::IN_ATTRIB
    | libc::IN_DELETE_SELF
    | libc::IN_MOVE_SELF
    | libc::IN_ONLYDIR
    | libc::IN_EXCL_UNLINK;

/// Why a root is not watched.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Fallback {
    /// On a file system whose changes inotify misses, of this type.
    FileSystem(String),
    /// Watching every folder would pass `fs.inotify.max_user_watches`.
    WatchLimit,
    /// The folder is gone, or was moved or unmounted; it is watched again
    /// once it is back.
    Missing,
    /// Watching it failed.
    Failed(String),
    /// inotify cannot be used at all.
    Unavailable(String),
}

impl Fallback {
    fn describe(&self, periodic: Duration) -> String {
        let rescanned = format!(
            "Ferrous rescans it at startup and every {}",
            describe_interval(periodic)
        );
        match self {
            Self::FileSystem(fs_type) => format!(
                "it is on a network or FUSE file system ({fs_type}), which does not report \
                 every change; {rescanned}"
            ),
            Self::WatchLimit => format!(
                "it has more folders than the system's inotify watch limit allows \
                 (fs.inotify.max_user_watches); {rescanned}"
            ),
            Self::Missing => format!(
                "the folder is not available; Ferrous watches and rescans it once it is back, \
                 checking every {}",
                describe_interval(periodic)
            ),
            Self::Failed(error) => format!("watching it failed ({error}); {rescanned}"),
            Self::Unavailable(error) => {
                format!("file watching is not available ({error}); {rescanned}")
            }
        }
    }
}

fn describe_interval(interval: Duration) -> String {
    let seconds = interval.as_secs();
    if seconds >= 60 && seconds.is_multiple_of(60) {
        super::scan::count_label(
            usize::try_from(seconds / 60).unwrap_or(usize::MAX),
            "minute",
            "minutes",
        )
    } else {
        super::scan::count_label(
            usize::try_from(seconds.max(1)).unwrap_or(usize::MAX),
            "second",
            "seconds",
        )
    }
}

/// The folders watched, by watch descriptor and by path. A folder reached
/// through several paths (links) has one descriptor for all of them.
#[derive(Debug, Default)]
struct WatchTable {
    paths: HashMap<i32, Vec<PathBuf>>,
    /// Sorted, so that the folders below one follow it.
    wds: BTreeMap<PathBuf, i32>,
}

impl WatchTable {
    fn insert(&mut self, path: PathBuf, wd: i32) {
        if let Some(old) = self.wds.insert(path.clone(), wd) {
            if old == wd {
                return;
            }
            self.unlink(old, &path);
        }
        self.paths.entry(wd).or_default().push(path);
    }

    fn unlink(&mut self, wd: i32, path: &Path) -> bool {
        let Some(paths) = self.paths.get_mut(&wd) else {
            return false;
        };
        paths.retain(|known| known != path);
        if paths.is_empty() {
            self.paths.remove(&wd);
            return true;
        }
        false
    }

    /// Forgets `top` and the folders below it, returning the descriptors
    /// no other path uses.
    fn remove_below(&mut self, top: &Path) -> Vec<i32> {
        self.remove_below_except(top, &[])
    }

    /// Whether `top` or a folder below it is watched.
    fn has_below(&self, top: &Path) -> bool {
        self.wds
            .range(top.to_path_buf()..)
            .next()
            .is_some_and(|(path, _)| path.starts_with(top))
    }

    /// [`WatchTable::remove_below`], keeping the folders below `kept`.
    fn remove_below_except(&mut self, top: &Path, kept: &[PathBuf]) -> Vec<i32> {
        let below: Vec<(PathBuf, i32)> = self
            .wds
            .range(top.to_path_buf()..)
            .take_while(|(path, _)| path.starts_with(top))
            .filter(|(path, _)| !kept.iter().any(|kept| path.starts_with(kept)))
            .map(|(path, wd)| (path.clone(), *wd))
            .collect();
        let mut unused = Vec::new();
        for (path, wd) in below {
            self.wds.remove(&path);
            if self.unlink(wd, &path) {
                unused.push(wd);
            }
        }
        unused
    }

    /// Forgets a descriptor the kernel removed, returning its paths.
    fn forget(&mut self, wd: i32) -> Vec<PathBuf> {
        let paths = self.paths.remove(&wd).unwrap_or_default();
        for path in &paths {
            if self.wds.get(path) == Some(&wd) {
                self.wds.remove(path);
            }
        }
        paths
    }

    #[cfg(test)]
    fn count_below(&self, top: &Path) -> usize {
        self.wds
            .range(top.to_path_buf()..)
            .take_while(|(path, _)| path.starts_with(top))
            .count()
    }
}

/// The changes of one root waiting to be scanned.
#[derive(Debug, Default)]
struct RootChanges {
    paths: BTreeSet<PathBuf>,
    /// Rescan the whole root instead.
    whole: bool,
    first: Option<Instant>,
    last: Option<Instant>,
}

/// Collects changes per root until they are due; see [`WatchTuning`].
#[derive(Debug, Default)]
struct Coalescer {
    roots: BTreeMap<PathBuf, RootChanges>,
}

impl Coalescer {
    fn changes(&mut self, root: &Path, now: Instant) -> &mut RootChanges {
        let changes = self.roots.entry(root.to_path_buf()).or_default();
        changes.first.get_or_insert(now);
        changes.last = Some(now);
        changes
    }

    fn add(&mut self, root: &Path, path: PathBuf, now: Instant, max_paths: usize) {
        let changes = self.changes(root, now);
        if changes.whole {
            return;
        }
        changes.paths.insert(path);
        if changes.paths.len() > max_paths {
            changes.whole = true;
            changes.paths.clear();
        }
        #[cfg(test)]
        watch_test_hooks::note_pending(root, changes.paths.len());
    }

    fn add_whole(&mut self, root: &Path, now: Instant) {
        let changes = self.changes(root, now);
        changes.whole = true;
        changes.paths.clear();
    }

    fn forget(&mut self, root: &Path) {
        self.roots.remove(root);
    }

    /// Takes the changes of `root`, to [`Coalescer::restore`] them.
    fn take(&mut self, root: &Path) -> Option<RootChanges> {
        self.roots.remove(root)
    }

    fn restore(&mut self, root: PathBuf, changes: RootChanges) {
        self.roots.insert(root, changes);
    }

    fn due_at(changes: &RootChanges, tuning: &WatchTuning) -> Option<Instant> {
        let (first, last) = (changes.first?, changes.last?);
        Some((last + tuning.quiet).min(first + tuning.max_delay))
    }

    /// When the next root's changes are due.
    fn deadline(&self, tuning: &WatchTuning) -> Option<Instant> {
        self.roots
            .iter()
            .filter(|(root, _)| !held(root))
            .filter_map(|(_, changes)| Self::due_at(changes, tuning))
            .min()
    }

    /// Takes the changes due at `now`.
    fn take_due(&mut self, now: Instant, tuning: &WatchTuning) -> Vec<(PathBuf, RootChanges)> {
        let due: Vec<PathBuf> = self
            .roots
            .iter()
            .filter(|(root, changes)| {
                !held(root) && Self::due_at(changes, tuning).is_some_and(|at| at <= now)
            })
            .map(|(root, _)| root.clone())
            .collect();
        due.into_iter()
            .filter_map(|root| self.roots.remove_entry(&root))
            .collect()
    }
}

/// The rescans of roots asked for by mount changes. For mounts below a
/// root, at most one per root every [`WatchTuning::mount_rescan`]; see
/// there. Changes until a rescan is sent merge into it.
#[derive(Debug, Default)]
struct MountRescans {
    /// When each root was last rescanned for mount changes.
    sent: HashMap<PathBuf, Instant>,
    /// The roots waiting for a rescan, and when it is due.
    due: BTreeMap<PathBuf, Instant>,
}

impl MountRescans {
    /// Asks for a rescan of `root` after a mount change at `now`.
    ///
    /// - If the root's own folder was `replaced`, it is due `quiet` after
    ///   the last replacement: a new card's files make no events, so its
    ///   rescan must not wait, and a root that keeps changing is rescanned
    ///   once it settles.
    /// - Otherwise it is due `quiet` after the first change, so that mounts
    ///   made together, such as several shares at login, are one rescan,
    ///   but not before `mount_rescan` after the last one sent.
    fn request(&mut self, root: &Path, now: Instant, tuning: &WatchTuning, replaced: bool) {
        let at = now + tuning.quiet;
        if replaced {
            self.due.insert(root.to_path_buf(), at);
            return;
        }
        let earliest = self.sent.get(root).map(|sent| *sent + tuning.mount_rescan);
        let at = at.max(earliest.unwrap_or(now));
        self.due.entry(root.to_path_buf()).or_insert(at);
    }

    fn forget(&mut self, root: &Path) {
        self.sent.remove(root);
        self.due.remove(root);
    }

    fn deadline(&self) -> Option<Instant> {
        self.due.values().min().copied()
    }

    /// Takes the rescans due at `now`, and returns the roots of those that
    /// can be sent then, as sent.
    fn take_due(&mut self, now: Instant, can_send: impl Fn(&Path) -> bool) -> Vec<PathBuf> {
        let due: Vec<PathBuf> = self
            .due
            .iter()
            .filter(|(_, at)| **at <= now)
            .map(|(root, _)| root.clone())
            .collect();
        for root in &due {
            self.due.remove(root);
        }
        let sent: Vec<PathBuf> = due.into_iter().filter(|root| can_send(root)).collect();
        for root in &sent {
            self.sent.insert(root.clone(), now);
        }
        sent
    }
}

/// Whether a test holds the changes of `root`.
fn held(root: &Path) -> bool {
    #[cfg(test)]
    {
        watch_test_hooks::is_held(root)
    }
    #[cfg(not(test))]
    {
        let _ = root;
        false
    }
}

/// What a root was when it was last watched.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct RootFolder {
    /// See [`folder_identity`]; `None` if it was gone.
    identity: Option<(u64, u64)>,
    /// The mounts below it, with their types.
    mounts: Vec<(PathBuf, String)>,
}

/// The watcher's state, on its thread.
struct Watcher {
    inotify: Option<Inotify>,
    /// Why inotify cannot be used, once it cannot.
    unavailable: Option<String>,
    waker: Option<Arc<Waker>>,
    /// `/proc/self/mountinfo`, which tells when the mount table changes.
    mounts: Option<File>,
    messages: Receiver<WatchMessage>,
    stop: Arc<AtomicBool>,
    stopped: bool,
    commands: Sender<LibraryCommand>,
    /// Told the number of roots messages handled, after each.
    roots_watched: Option<Sender<u64>>,
    roots_messages: u64,
    tuning: WatchTuning,
    enabled: bool,
    /// The configured roots, once the worker sent them.
    roots: Option<Vec<PathBuf>>,
    /// Whether every root was rescanned since watching was turned on.
    caught_up: bool,
    table: WatchTable,
    /// What each root was when it was last watched.
    root_folders: HashMap<PathBuf, RootFolder>,
    /// The roots that are not watched while watching is on, and why.
    unwatched: BTreeMap<PathBuf, Fallback>,
    /// The mounts below watched roots whose file systems miss changes, by
    /// mount point: their root, and their type. They are not watched, and
    /// are rescanned with the roots that are not watched.
    unwatched_mounts: BTreeMap<PathBuf, (PathBuf, String)>,
    /// What the worker was last told about `unwatched`.
    reported: Vec<UnwatchedRoot>,
    pending: Coalescer,
    mount_rescans: MountRescans,
    /// When the roots that are not watched are rescanned next.
    next_periodic: Option<Instant>,
}

impl Watcher {
    fn new(
        tuning: WatchTuning,
        messages: Receiver<WatchMessage>,
        commands: Sender<LibraryCommand>,
        stop: Arc<AtomicBool>,
    ) -> Self {
        let (inotify, waker, unavailable) = match Inotify::new().and_then(|inotify| {
            let waker = Waker::new()?;
            Ok((inotify, waker))
        }) {
            Ok((inotify, waker)) => (Some(inotify), Some(Arc::new(waker)), None),
            Err(error) => (None, None, Some(error.to_string())),
        };
        Self {
            inotify,
            unavailable,
            waker,
            // Without it, mounts are noticed by the periodic checks only.
            mounts: File::open("/proc/self/mountinfo").ok(),
            messages,
            stop,
            stopped: false,
            commands,
            roots_watched: None,
            roots_messages: 0,
            tuning,
            enabled: false,
            roots: None,
            caught_up: false,
            table: WatchTable::default(),
            root_folders: HashMap::new(),
            unwatched: BTreeMap::new(),
            unwatched_mounts: BTreeMap::new(),
            reported: Vec::new(),
            pending: Coalescer::default(),
            mount_rescans: MountRescans::default(),
            next_periodic: None,
        }
    }

    fn run(mut self) {
        let mut events = Vec::new();
        loop {
            self.receive();
            if self.stopping() {
                break;
            }
            self.read_events(&mut events);
            let now = Instant::now();
            self.flush_due(now);
            self.rescan_for_mounts_if_due(now);
            self.rescan_unwatched_if_due(now);
            self.report();
            let deadline = [
                self.pending.deadline(&self.tuning),
                self.mount_rescans.deadline(),
                self.next_periodic,
            ]
            .into_iter()
            .flatten()
            .min();
            if let Some(message) = self.wait(deadline) {
                self.handle(message);
            }
        }
    }

    fn stopping(&self) -> bool {
        self.stopped || self.stop.load(Ordering::Relaxed)
    }

    fn receive(&mut self) {
        loop {
            match self.messages.try_recv() {
                Ok(message) => self.handle(message),
                Err(TryRecvError::Empty) => return,
                Err(TryRecvError::Disconnected) => {
                    self.stopped = true;
                    return;
                }
            }
        }
    }

    /// Waits for events, a message or `deadline`; returns a message
    /// received while waiting without inotify.
    fn wait(&mut self, deadline: Option<Instant>) -> Option<WatchMessage> {
        if let (Some(inotify), Some(waker)) = (&self.inotify, &self.waker) {
            let timeout =
                deadline.map(|deadline| deadline.saturating_duration_since(Instant::now()));
            match inotify.wait(waker, self.mounts.as_ref(), timeout) {
                Ok(true) => self.recheck_mounts(),
                Ok(false) => {}
                Err(error) => self.lose_inotify(&error),
            }
            return None;
        }
        let received = match deadline {
            Some(deadline) => self.messages.recv_deadline(deadline),
            None => self
                .messages
                .recv()
                .map_err(|_| RecvTimeoutError::Disconnected),
        };
        match received {
            Ok(message) => Some(message),
            Err(RecvTimeoutError::Timeout) => None,
            Err(RecvTimeoutError::Disconnected) => {
                self.stopped = true;
                None
            }
        }
    }

    fn handle(&mut self, message: WatchMessage) {
        let roots_told = matches!(message, WatchMessage::Roots(_));
        match message {
            WatchMessage::Stop => self.stopped = true,
            WatchMessage::Enabled(enabled) => self.set_enabled(enabled),
            WatchMessage::Roots(roots) => self.set_roots(&roots),
        }
        self.catch_up();
        if roots_told {
            // The worker walks a root it added once its watches are set up.
            self.roots_messages += 1;
            if let Some(roots_watched) = &self.roots_watched {
                let _ = roots_watched.send(self.roots_messages);
            }
        }
    }

    fn set_enabled(&mut self, enabled: bool) {
        if enabled == self.enabled {
            return;
        }
        self.enabled = enabled;
        if enabled {
            self.caught_up = false;
            for root in self.roots.clone().unwrap_or_default() {
                self.watch_root(&root);
            }
        } else {
            for root in self.roots.clone().unwrap_or_default() {
                self.unwatch_root(&root);
            }
            self.pending = Coalescer::default();
            self.mount_rescans = MountRescans::default();
            self.unwatched.clear();
            self.next_periodic = None;
        }
    }

    fn set_roots(&mut self, roots: &[PathBuf]) {
        let old = self.roots.replace(roots.to_vec()).unwrap_or_default();
        if !self.enabled {
            return;
        }
        let removed: Vec<PathBuf> = old
            .iter()
            .filter(|root| !roots.contains(root))
            .cloned()
            .collect();
        for root in &removed {
            self.unwatch_root(root);
            self.mount_rescans.forget(root);
        }
        for root in roots.iter().filter(|root| !old.contains(root)) {
            self.watch_root(root);
        }
        // A root removed from inside another, from before nested roots were
        // refused, leaves its folders to the outer root.
        for root in roots {
            for inner in removed.iter().filter(|inner| inner.starts_with(root)) {
                if self.watched_root_of(inner).as_deref() == Some(root.as_path()) {
                    let now = Instant::now();
                    self.leave_out_unwatchable_mounts(root, inner, &mounts::mount_table());
                    if self.unwatched_mounts.values().any(|(of, _)| of == root) {
                        self.next_periodic
                            .get_or_insert_with(|| now + self.tuning.periodic);
                    }
                    if let Err(fallback) = self.watch_tree(root, inner) {
                        self.fall_back(root, fallback, now);
                    }
                }
            }
        }
    }

    /// Rescans every root once watching is on and the roots are known, so
    /// that changes made while Ferrous was closed, or watching was off,
    /// appear. Roots added later are scanned by the command adding them.
    fn catch_up(&mut self) {
        if !self.enabled || self.caught_up || self.roots.is_none() || self.stopping() {
            return;
        }
        self.caught_up = true;
        // A root that is gone is rescanned once it is back.
        for root in self.roots.iter().flatten().filter(|root| root.is_dir()) {
            let _ = self.commands.send(LibraryCommand::RescanRoot(root.clone()));
        }
    }

    /// After the mount table changed: watches again the roots whose folder
    /// is another one now, such as a drive mounted there, that are back or
    /// gone, or that have other mounts below them, and rescans what changed.
    fn recheck_mounts(&mut self) {
        if !self.enabled {
            return;
        }
        let table = mounts::mount_table();
        let now = Instant::now();
        for root in self.roots.clone().unwrap_or_default() {
            let folder = RootFolder {
                identity: folder_identity(&root),
                mounts: mounts::mounts_below(&table, &root),
            };
            let before = self.root_folders.get(&root).cloned().unwrap_or_default();
            if before == folder {
                continue;
            }
            let pending = self.pending.take(&root);
            self.unwatch_root(&root);
            self.watch_root(&root);
            if folder.identity.is_none() {
                continue;
            }
            // The changes seen are still scanned when due, and those seen
            // next through the new watches too.
            if let Some(pending) = pending {
                self.pending.restore(root.clone(), pending);
            }
            // Events of the replaced watches may be lost: rescan all of it,
            // as after lost events, not only where the mounts changed.
            let replaced = folder.identity != before.identity;
            self.mount_rescans
                .request(&root, now, &self.tuning, replaced);
        }
    }

    /// Sends the rescans that mount changes asked for, once due.
    fn rescan_for_mounts_if_due(&mut self, now: Instant) {
        let enabled = self.enabled;
        // A root gone meanwhile is rescanned once it is back.
        for root in self
            .mount_rescans
            .take_due(now, |root| enabled && root.is_dir())
        {
            let _ = self.commands.send(LibraryCommand::RescanRoot(root));
        }
    }

    /// Watches `root` and the folders below it, or notes why it cannot be.
    fn watch_root(&mut self, root: &Path) {
        #[cfg(test)]
        watch_test_hooks::before_watching(root);
        self.unwatched.remove(root);
        let table = mounts::mount_table();
        let fallback = self.try_watch_root(root, &table);
        self.root_folders.insert(
            root.to_path_buf(),
            RootFolder {
                identity: folder_identity(root),
                mounts: mounts::mounts_below(&table, root),
            },
        );
        #[cfg(test)]
        watch_test_hooks::note_watched(root, self.table.count_below(root));
        let unwatched_mounts = self.unwatched_mounts.values().any(|(of, _)| of == root);
        if let Some(fallback) = fallback {
            self.unwatched.insert(root.to_path_buf(), fallback);
        }
        if self.unwatched.contains_key(root) || unwatched_mounts {
            self.next_periodic
                .get_or_insert_with(|| Instant::now() + self.tuning.periodic);
        }
    }

    fn try_watch_root(&mut self, root: &Path, table: &[(PathBuf, String)]) -> Option<Fallback> {
        if !root.is_dir() {
            return Some(Fallback::Missing);
        }
        if let Some(fs_type) = mounts::mount_type_of(table, root) {
            if mounts::misses_changes(&fs_type) {
                return Some(Fallback::FileSystem(fs_type));
            }
        }
        if self.inotify.is_none() {
            return Some(Fallback::Unavailable(
                self.unavailable.clone().unwrap_or_default(),
            ));
        }
        self.leave_out_unwatchable_mounts(root, root, table);
        match self.watch_tree(root, root) {
            Ok(()) => None,
            Err(fallback) => {
                self.unwatch_root(root);
                Some(fallback)
            }
        }
    }

    /// Leaves out of the watches of `root` the mounts at or below `top`, one
    /// of its folders, whose file systems miss changes; they are rescanned
    /// instead. The outermost ones suffice. A mount at `top` matters when
    /// `top` is a removed inner root's folder: one at `root` itself that
    /// misses changes leaves the whole root unwatched before this.
    fn leave_out_unwatchable_mounts(
        &mut self,
        root: &Path,
        top: &Path,
        table: &[(PathBuf, String)],
    ) {
        let nested = self.nested_roots(root);
        for (mount_point, fs_type) in mounts::mounts_at_or_below(table, top) {
            if mounts::misses_changes(&fs_type)
                && !nested.iter().any(|inner| mount_point.starts_with(inner))
                && !self
                    .unwatched_mounts
                    .keys()
                    .any(|outer| mount_point.starts_with(outer))
            {
                self.unwatched_mounts
                    .insert(mount_point, (root.to_path_buf(), fs_type));
            }
        }
    }

    /// The configured roots inside `root`, from before nested roots were
    /// refused. Their folders are theirs, not `root`'s.
    fn nested_roots(&self, root: &Path) -> Vec<PathBuf> {
        self.roots
            .iter()
            .flatten()
            .filter(|other| *other != root && other.starts_with(root))
            .cloned()
            .collect()
    }

    /// Watches `top`, a folder of `root`, and the folders below it, through
    /// links as scans follow them. Folders that cannot be read or are gone
    /// are skipped, as scans skip them, and so are other roots and mounts
    /// that are not watched.
    fn watch_tree(&mut self, root: &Path, top: &Path) -> Result<(), Fallback> {
        let skipped: Vec<PathBuf> = self
            .nested_roots(root)
            .into_iter()
            .chain(self.unwatched_mounts.keys().cloned())
            .collect();
        let walk = WalkDir::new(top)
            .follow_links(true)
            .into_iter()
            .filter_entry(|entry| {
                entry.file_type().is_dir() && !skipped.iter().any(|other| entry.path() == other)
            });
        for entry in walk {
            if self.stop.load(Ordering::Relaxed) {
                return Ok(());
            }
            let Ok(entry) = entry else {
                continue;
            };
            let Some(inotify) = &self.inotify else {
                return Err(Fallback::Unavailable(
                    self.unavailable.clone().unwrap_or_default(),
                ));
            };
            match add_watch(inotify, entry.path()) {
                Ok(wd) => self.table.insert(entry.into_path(), wd),
                Err(error) if error.raw_os_error() == Some(libc::ENOSPC) => {
                    return Err(Fallback::WatchLimit);
                }
                Err(error)
                    if entry.depth() > 0
                        && matches!(
                            error.raw_os_error(),
                            Some(libc::ENOENT | libc::ENOTDIR | libc::EACCES)
                        ) => {}
                Err(error) => return Err(Fallback::Failed(error.to_string())),
            }
        }
        Ok(())
    }

    /// Stops watching `root`, but not the roots inside it.
    fn unwatch_root(&mut self, root: &Path) {
        let nested = self.nested_roots(root);
        for wd in self.table.remove_below_except(root, &nested) {
            if let Some(inotify) = &self.inotify {
                inotify.remove_watch(wd);
            }
        }
        self.pending.forget(root);
        self.unwatched.remove(root);
        self.unwatched_mounts.retain(|_, (of, _)| of != root);
        self.root_folders.remove(root);
        #[cfg(test)]
        watch_test_hooks::note_watched(root, 0);
    }

    /// Falls back for `root`, whose watches cannot be completed, and
    /// rescans what they missed.
    fn fall_back(&mut self, root: &Path, fallback: Fallback, now: Instant) {
        self.unwatch_root(root);
        self.unwatched.insert(root.to_path_buf(), fallback);
        self.next_periodic
            .get_or_insert_with(|| now + self.tuning.periodic);
        self.pending.add_whole(root, now);
    }

    /// After events were lost: watches the folders made meanwhile, forgets
    /// those gone, and rescans the root.
    fn resync_root(&mut self, root: &Path, now: Instant) {
        let nested = self.nested_roots(root);
        let before = self.table.remove_below_except(root, &nested);
        match self.watch_tree(root, root) {
            Ok(()) => {
                for wd in before {
                    // Watched again, the folder has the same descriptor.
                    if !self.table.paths.contains_key(&wd) {
                        if let Some(inotify) = &self.inotify {
                            inotify.remove_watch(wd);
                        }
                    }
                }
                self.pending.add_whole(root, now);
            }
            Err(fallback) => {
                for wd in before {
                    if let Some(inotify) = &self.inotify {
                        inotify.remove_watch(wd);
                    }
                }
                self.fall_back(root, fallback, now);
            }
        }
        #[cfg(test)]
        watch_test_hooks::note_watched(root, self.table.count_below(root));
    }

    /// Gives up on inotify after it failed: every root falls back.
    fn lose_inotify(&mut self, error: &io::Error) {
        eprintln!("[ferrous] library folders are no longer watched: {error}");
        self.inotify = None;
        self.unavailable = Some(error.to_string());
        self.table = WatchTable::default();
        self.unwatched_mounts.clear();
        if self.enabled {
            for root in self.roots.clone().unwrap_or_default() {
                self.unwatched
                    .entry(root)
                    .or_insert_with(|| Fallback::Unavailable(error.to_string()));
            }
            self.next_periodic
                .get_or_insert_with(|| Instant::now() + self.tuning.periodic);
        }
    }

    fn read_events(&mut self, events: &mut Vec<RawEvent>) {
        let Some(inotify) = &mut self.inotify else {
            return;
        };
        events.clear();
        if let Err(error) = inotify.read_events(events) {
            self.lose_inotify(&error);
            return;
        }
        let now = Instant::now();
        for event in events.drain(..) {
            self.apply_event(&event, now);
        }
    }

    /// The configured root that `path` is in, if it is watched.
    fn watched_root_of(&self, path: &Path) -> Option<PathBuf> {
        self.roots
            .iter()
            .flatten()
            .filter(|root| path.starts_with(root))
            .max_by_key(|root| root.components().count())
            .filter(|root| !self.unwatched.contains_key(*root))
            .cloned()
    }

    fn apply_event(&mut self, event: &RawEvent, now: Instant) {
        if !self.enabled {
            return;
        }
        if event.mask & libc::IN_Q_OVERFLOW != 0 {
            // Events were lost, those of new folders too.
            for root in self.roots.clone().unwrap_or_default() {
                if !self.unwatched.contains_key(&root) {
                    self.resync_root(&root, now);
                }
            }
            return;
        }
        if event.mask & libc::IN_IGNORED != 0 {
            let paths = self.table.forget(event.wd);
            self.lose_roots_among(&paths);
            return;
        }
        let Some(folders) = self.table.paths.get(&event.wd).cloned() else {
            return;
        };
        match &event.name {
            Some(name) => {
                for folder in folders {
                    self.apply_entry_event(event.mask, folder.join(name), now);
                }
            }
            None => {
                if event.mask & (libc::IN_DELETE_SELF | libc::IN_MOVE_SELF) != 0 {
                    // A folder below a root is reported by its parent too.
                    self.lose_roots_among(&folders);
                }
            }
        }
    }

    /// Falls back for the roots among `folders`, whose watches are gone:
    /// they were deleted, moved or unmounted.
    fn lose_roots_among(&mut self, folders: &[PathBuf]) {
        for folder in folders {
            if self.roots.iter().flatten().any(|root| root == folder) {
                self.unwatch_root(folder);
                self.unwatched.insert(folder.clone(), Fallback::Missing);
                self.next_periodic
                    .get_or_insert_with(|| Instant::now() + self.tuning.periodic);
            }
        }
    }

    fn apply_entry_event(&mut self, mask: u32, path: PathBuf, now: Instant) {
        let Some(root) = self.watched_root_of(&path) else {
            return;
        };
        let made = mask & (libc::IN_CREATE | libc::IN_MOVED_TO) != 0;
        let gone = mask & (libc::IN_DELETE | libc::IN_MOVED_FROM) != 0;
        if mask & libc::IN_ISDIR != 0 {
            if mask & libc::IN_MOVED_FROM != 0 {
                self.forget_watches_below(&path);
            }
            if made {
                if let Err(fallback) = self.watch_tree(&root, &path) {
                    self.fall_back(&root, fallback, now);
                    return;
                }
            }
            self.pending.add(&root, path, now, self.tuning.max_paths);
            return;
        }
        if made
            && path
                .symlink_metadata()
                .is_ok_and(|meta| meta.file_type().is_symlink())
        {
            // A link, which scans follow: one to a folder is watched as the
            // folder.
            if path.is_dir() {
                if let Err(fallback) = self.watch_tree(&root, &path) {
                    self.fall_back(&root, fallback, now);
                    return;
                }
            }
            self.pending.add(&root, path, now, self.tuning.max_paths);
            return;
        }
        if gone && self.table.has_below(&path) {
            // A link to a folder, gone with the tracks reached through it.
            self.forget_watches_below(&path);
            self.pending.add(&root, path, now, self.tuning.max_paths);
            return;
        }
        if is_supported_audio(&path) {
            self.pending.add(&root, path, now, self.tuning.max_paths);
        } else if is_cover_image(&path) {
            // An image added, removed, renamed or rewritten can change the
            // cover of every track in its folder, so the folder is scanned.
            if let Some(folder) = path.parent() {
                self.pending
                    .add(&root, folder.to_path_buf(), now, self.tuning.max_paths);
            }
        }
    }

    fn forget_watches_below(&mut self, path: &Path) {
        for wd in self.table.remove_below(path) {
            if let Some(inotify) = &self.inotify {
                inotify.remove_watch(wd);
            }
        }
    }

    fn flush_due(&mut self, now: Instant) {
        for (root, changes) in self.pending.take_due(now, &self.tuning) {
            let command = if changes.whole {
                LibraryCommand::RescanRoot(root)
            } else {
                LibraryCommand::ScanPaths {
                    root,
                    paths: changes.paths.into_iter().collect(),
                }
            };
            let _ = self.commands.send(command);
        }
    }

    /// Rescans the roots and mounts that are not watched, and watches again
    /// the roots that were gone and are back.
    fn rescan_unwatched_if_due(&mut self, now: Instant) {
        if self.next_periodic.is_none_or(|at| at > now) {
            return;
        }
        self.next_periodic = None;
        let unwatched: Vec<(PathBuf, Fallback)> = self
            .unwatched
            .iter()
            .map(|(root, fallback)| (root.clone(), fallback.clone()))
            .collect();
        for (root, fallback) in unwatched {
            if fallback == Fallback::Missing {
                if !root.is_dir() {
                    self.next_periodic.get_or_insert(now + self.tuning.periodic);
                    continue;
                }
                self.watch_root(&root);
            }
            let _ = self.commands.send(LibraryCommand::RescanRoot(root));
        }
        let mut mounts: BTreeMap<PathBuf, Vec<PathBuf>> = BTreeMap::new();
        for (mount_point, (root, _)) in &self.unwatched_mounts {
            mounts
                .entry(root.clone())
                .or_default()
                .push(mount_point.clone());
        }
        for (root, paths) in mounts {
            let _ = self
                .commands
                .send(LibraryCommand::ScanPaths { root, paths });
        }
        if !self.unwatched.is_empty() || !self.unwatched_mounts.is_empty() {
            self.next_periodic.get_or_insert(now + self.tuning.periodic);
        }
    }

    /// Tells the worker the roots that are not watched, or have mounts that
    /// are not, when they change.
    fn report(&mut self) {
        let periodic = self.tuning.periodic;
        let mut reasons: BTreeMap<PathBuf, String> = self
            .unwatched
            .iter()
            .map(|(root, fallback)| (root.clone(), fallback.describe(periodic)))
            .collect();
        let mut mounts: BTreeMap<&Path, Vec<(&Path, &str)>> = BTreeMap::new();
        for (mount_point, (root, fs_type)) in &self.unwatched_mounts {
            mounts.entry(root).or_default().push((mount_point, fs_type));
        }
        for (root, mounts) in mounts {
            reasons
                .entry(root.to_path_buf())
                .or_insert_with(|| describe_mounts(&mounts, periodic));
        }
        let unwatched: Vec<UnwatchedRoot> = reasons
            .into_iter()
            .map(|(path, reason)| UnwatchedRoot { path, reason })
            .collect();
        if unwatched != self.reported {
            let _ = self
                .commands
                .send(LibraryCommand::SetUnwatchedRoots(unwatched.clone()));
            self.reported = unwatched;
        }
    }
}

/// Why the mounts `mounts` below a root are not watched.
fn describe_mounts(mounts: &[(&Path, &str)], periodic: Duration) -> String {
    let list = mounts
        .iter()
        .map(|(mount_point, fs_type)| format!("{} ({fs_type})", mount_point.display()))
        .collect::<Vec<_>>()
        .join(", ");
    let interval = describe_interval(periodic);
    if mounts.len() == 1 {
        format!(
            "{list} is on a network or FUSE file system, which does not report every change; \
             Ferrous rescans it at startup and every {interval}"
        )
    } else {
        format!(
            "{list} are on network or FUSE file systems, which do not report every change; \
             Ferrous rescans them at startup and every {interval}"
        )
    }
}

/// The device and inode of the folder at `path`, which change when another
/// file system is mounted there.
fn folder_identity(path: &Path) -> Option<(u64, u64)> {
    std::fs::metadata(path)
        .ok()
        .filter(std::fs::Metadata::is_dir)
        .map(|metadata| (metadata.dev(), metadata.ino()))
}

fn add_watch(inotify: &Inotify, path: &Path) -> io::Result<i32> {
    #[cfg(test)]
    if let Some(errno) = watch_test_hooks::watch_failure(path) {
        return Err(io::Error::from_raw_os_error(errno));
    }
    inotify.add_watch(path, WATCH_MASK)
}

/// Make watches fail, set file system types, hold changes, and observe the
/// watcher, for tests. Each is keyed by a test's own folder.
#[cfg(test)]
pub(crate) mod watch_test_hooks {
    use std::path::{Path, PathBuf};
    use std::sync::{Mutex, PoisonError};

    use crossbeam_channel::{unbounded, Receiver, Sender};

    static FAILURES: Mutex<Vec<(PathBuf, i32)>> = Mutex::new(Vec::new());
    static MOUNTS: Mutex<Vec<(PathBuf, String)>> = Mutex::new(Vec::new());
    static HELD: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
    static PENDING: Mutex<Vec<(PathBuf, Sender<usize>)>> = Mutex::new(Vec::new());
    static WATCHED: Mutex<Vec<(PathBuf, Sender<usize>)>> = Mutex::new(Vec::new());
    /// A held root: the sender told when watching it waits, and the
    /// receiver that releases it.
    static HELD_WATCHING: Mutex<Vec<(PathBuf, Sender<()>, Receiver<()>)>> = Mutex::new(Vec::new());
    static DEFERRED_WALKS: Mutex<Vec<(PathBuf, Sender<()>)>> = Mutex::new(Vec::new());

    fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
        mutex.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Watching `path`, or a folder below it, fails with `errno`.
    pub(crate) fn fail_watches(path: &Path, errno: i32) {
        lock(&FAILURES).push((path.to_path_buf(), errno));
    }

    pub(super) fn watch_failure(path: &Path) -> Option<i32> {
        lock(&FAILURES)
            .iter()
            .find(|(failing, _)| path.starts_with(failing))
            .map(|(_, errno)| *errno)
    }

    /// A file system of `fs_type` is mounted at `path`, as far as watchers
    /// can tell.
    pub(crate) fn set_fs_type(path: &Path, fs_type: &str) {
        lock(&MOUNTS).push((path.to_path_buf(), fs_type.to_string()));
    }

    /// Unmounts what [`set_fs_type`] mounted at `path`.
    pub(crate) fn unmount_fake(path: &Path) {
        lock(&MOUNTS).retain(|(mount_point, _)| mount_point != path);
    }

    pub(super) fn fake_mounts() -> Vec<(PathBuf, String)> {
        lock(&MOUNTS).clone()
    }

    /// Keeps the changes of `root` from being scanned until the guard is
    /// dropped and the watcher next wakes.
    pub(crate) fn hold_changes(root: &Path) -> HeldChanges {
        lock(&HELD).push(root.to_path_buf());
        HeldChanges(root.to_path_buf())
    }

    pub(crate) struct HeldChanges(PathBuf);

    impl Drop for HeldChanges {
        fn drop(&mut self) {
            lock(&HELD).retain(|root| *root != self.0);
        }
    }

    pub(super) fn is_held(root: &Path) -> bool {
        lock(&HELD).iter().any(|held| held == root)
    }

    /// Receives the number of changed paths of `root` waiting, whenever
    /// one is added.
    pub(crate) fn observe_pending(root: &Path) -> Receiver<usize> {
        let (tx, rx) = unbounded();
        lock(&PENDING).push((root.to_path_buf(), tx));
        rx
    }

    pub(super) fn note_pending(root: &Path, count: usize) {
        for (observed, tx) in lock(&PENDING).iter() {
            if observed == root {
                let _ = tx.send(count);
            }
        }
    }

    /// Receives the number of folders of `root` watched, whenever watches
    /// of the root are added or removed.
    pub(crate) fn observe_watched(root: &Path) -> Receiver<usize> {
        let (tx, rx) = unbounded();
        lock(&WATCHED).push((root.to_path_buf(), tx));
        rx
    }

    pub(super) fn note_watched(root: &Path, count: usize) {
        for (observed, tx) in lock(&WATCHED).iter() {
            if observed == root {
                let _ = tx.send(count);
            }
        }
    }

    /// Makes the watcher wait before it watches `root`, until the guard is
    /// dropped. [`HeldWatching::reached`] gets a message each time it
    /// waits.
    pub(crate) fn hold_watching(root: &Path) -> HeldWatching {
        let (reached_tx, reached) = unbounded();
        let (release, release_rx) = unbounded();
        lock(&HELD_WATCHING).push((root.to_path_buf(), reached_tx, release_rx));
        HeldWatching {
            root: root.to_path_buf(),
            reached,
            _release: release,
        }
    }

    pub(crate) struct HeldWatching {
        root: PathBuf,
        pub(crate) reached: Receiver<()>,
        /// Dropped with the guard, which releases the watcher.
        _release: Sender<()>,
    }

    impl Drop for HeldWatching {
        fn drop(&mut self) {
            lock(&HELD_WATCHING).retain(|(root, _, _)| *root != self.root);
        }
    }

    pub(super) fn before_watching(root: &Path) {
        let held = lock(&HELD_WATCHING)
            .iter()
            .find(|(held, _, _)| held == root)
            .map(|(_, reached, release)| (reached.clone(), release.clone()));
        if let Some((reached, release)) = held {
            let _ = reached.send(());
            // Returns once the guard is dropped.
            let _ = release.recv();
        }
    }

    /// Receives a message whenever the worker leaves the walk of `root` for
    /// when its watches are set up.
    pub(crate) fn observe_deferred_walks(root: &Path) -> Receiver<()> {
        let (tx, rx) = unbounded();
        lock(&DEFERRED_WALKS).push((root.to_path_buf(), tx));
        rx
    }

    pub(in crate::library) fn note_walk_deferred(root: &Path) {
        for (observed, tx) in lock(&DEFERRED_WALKS).iter() {
            if observed == root {
                let _ = tx.send(());
            }
        }
    }
}

#[cfg(test)]
mod service_tests;
#[cfg(test)]
mod tests;
