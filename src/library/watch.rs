// SPDX-License-Identifier: GPL-3.0-or-later

//! The library watcher: watches the roots with inotify on its own thread,
//! and sends the library worker the paths that changed, a few at a time.
//!
//! - Events are collected per root until none has arrived for
//!   [`WatchTuning::quiet`], or for at most [`WatchTuning::max_delay`]
//!   since the first, and then sent as one [`LibraryCommand::ScanPaths`].
//!   The worker merges it with other scans and runs it between commands.
//! - When watching starts, at startup or when it is turned on, every root
//!   is rescanned, so that changes made meanwhile appear.
//! - A root that cannot be watched (a network or FUSE file system, the
//!   inotify watch limit, a folder that is gone) is rescanned every
//!   [`WatchTuning::periodic`] instead, and the worker publishes it, with
//!   the reason, in [`super::LibraryStatus::unwatched_roots`].

mod inotify;
mod mounts;

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{unbounded, Receiver, RecvTimeoutError, Sender, TryRecvError};
use walkdir::WalkDir;

use self::inotify::{Inotify, RawEvent, Waker};
use super::{is_supported_audio, LibraryCommand, UnwatchedRoot};
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
    fn default() -> Self {
        Self {
            quiet: Duration::from_millis(500),
            max_delay: Duration::from_secs(5),
            periodic: Duration::from_secs(30 * 60),
            max_paths: 2048,
        }
    }
}

/// What the watcher is told.
#[derive(Debug)]
enum WatchMessage {
    Enabled(bool),
    /// The configured roots. The first message comes once the library is
    /// ready, and starts watching.
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
    /// receiver gets its commands for the library worker.
    pub(super) fn spawn(tuning: WatchTuning) -> (Self, Receiver<LibraryCommand>) {
        let (commands_tx, commands_rx) = unbounded();
        let (tx, messages) = unbounded();
        let stop = Arc::new(AtomicBool::new(false));
        let (inotify, waker, unavailable) = match Inotify::new().and_then(|inotify| {
            let waker = Waker::new()?;
            Ok((inotify, waker))
        }) {
            Ok((inotify, waker)) => (Some(inotify), Some(Arc::new(waker)), None),
            Err(error) => (None, None, Some(error.to_string())),
        };
        let watcher = Watcher {
            inotify,
            unavailable,
            waker: waker.clone(),
            messages,
            stop: Arc::clone(&stop),
            stopped: false,
            commands: commands_tx,
            tuning,
            enabled: false,
            roots: None,
            caught_up: false,
            table: WatchTable::default(),
            unwatched: BTreeMap::new(),
            reported: Vec::new(),
            pending: Coalescer::default(),
            next_periodic: None,
        };
        let thread =
            crate::worker_thread::spawn_worker("ferrous-library-watch", move || watcher.run());
        (
            Self {
                handle: WatchHandle { tx, waker },
                stop,
                thread,
            },
            commands_rx,
        )
    }

    pub(super) fn handle(&self) -> WatchHandle {
        self.handle.clone()
    }

    pub(super) fn set_enabled(&self, enabled: bool) {
        self.handle.send(WatchMessage::Enabled(enabled));
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
        let below: Vec<(PathBuf, i32)> = self
            .wds
            .range(top.to_path_buf()..)
            .take_while(|(path, _)| path.starts_with(top))
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

/// The watcher's state, on its thread.
struct Watcher {
    inotify: Option<Inotify>,
    /// Why inotify cannot be used, once it cannot.
    unavailable: Option<String>,
    waker: Option<Arc<Waker>>,
    messages: Receiver<WatchMessage>,
    stop: Arc<AtomicBool>,
    stopped: bool,
    commands: Sender<LibraryCommand>,
    tuning: WatchTuning,
    enabled: bool,
    /// The configured roots, once the worker sent them.
    roots: Option<Vec<PathBuf>>,
    /// Whether every root was rescanned since watching was turned on.
    caught_up: bool,
    table: WatchTable,
    /// The roots that are not watched while watching is on, and why.
    unwatched: BTreeMap<PathBuf, Fallback>,
    /// What the worker was last told about `unwatched`.
    reported: Vec<UnwatchedRoot>,
    pending: Coalescer,
    /// When the roots that are not watched are rescanned next.
    next_periodic: Option<Instant>,
}

impl Watcher {
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
            self.rescan_unwatched_if_due(now);
            self.report();
            let deadline = [self.pending.deadline(&self.tuning), self.next_periodic]
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
            if let Err(error) = inotify.wait(waker, timeout) {
                self.lose_inotify(&error);
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
        match message {
            WatchMessage::Stop => self.stopped = true,
            WatchMessage::Enabled(enabled) => self.set_enabled(enabled),
            WatchMessage::Roots(roots) => self.set_roots(&roots),
        }
        self.catch_up();
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
            self.unwatched.clear();
            self.next_periodic = None;
        }
    }

    fn set_roots(&mut self, roots: &[PathBuf]) {
        let old = self.roots.replace(roots.to_vec()).unwrap_or_default();
        if !self.enabled {
            return;
        }
        for root in old.iter().filter(|root| !roots.contains(root)) {
            self.unwatch_root(root);
        }
        for root in roots.iter().filter(|root| !old.contains(root)) {
            self.watch_root(root);
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
        if self.roots.as_ref().is_some_and(|roots| !roots.is_empty()) {
            let _ = self.commands.send(LibraryCommand::RescanAll);
        }
    }

    /// Watches `root` and the folders below it, or notes why it cannot be.
    fn watch_root(&mut self, root: &Path) {
        self.unwatched.remove(root);
        let fallback = self.try_watch_root(root);
        #[cfg(test)]
        watch_test_hooks::note_watched(root, self.table.count_below(root));
        if let Some(fallback) = fallback {
            self.unwatched.insert(root.to_path_buf(), fallback);
            self.next_periodic
                .get_or_insert_with(|| Instant::now() + self.tuning.periodic);
        }
    }

    fn try_watch_root(&mut self, root: &Path) -> Option<Fallback> {
        if !root.is_dir() {
            return Some(Fallback::Missing);
        }
        if let Some(fs_type) = mounts::file_system_type(root) {
            if mounts::misses_changes(&fs_type) {
                return Some(Fallback::FileSystem(fs_type));
            }
        }
        if self.inotify.is_none() {
            return Some(Fallback::Unavailable(
                self.unavailable.clone().unwrap_or_default(),
            ));
        }
        match self.watch_tree(root, root) {
            Ok(()) => None,
            Err(fallback) => {
                self.unwatch_root(root);
                Some(fallback)
            }
        }
    }

    /// Watches `top`, a folder of `root`, and the folders below it. Folders
    /// that cannot be read or are gone are skipped, as scans skip them.
    fn watch_tree(&mut self, root: &Path, top: &Path) -> Result<(), Fallback> {
        let other_roots: Vec<PathBuf> = self
            .roots
            .iter()
            .flatten()
            .filter(|other| *other != root && other.starts_with(root))
            .cloned()
            .collect();
        let walk = WalkDir::new(top)
            .follow_links(true)
            .into_iter()
            .filter_entry(|entry| {
                entry.file_type().is_dir() && !other_roots.iter().any(|other| entry.path() == other)
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

    fn unwatch_root(&mut self, root: &Path) {
        for wd in self.table.remove_below(root) {
            if let Some(inotify) = &self.inotify {
                inotify.remove_watch(wd);
            }
        }
        self.pending.forget(root);
        self.unwatched.remove(root);
        #[cfg(test)]
        watch_test_hooks::note_watched(root, 0);
    }

    /// Gives up on inotify after it failed: every root falls back.
    fn lose_inotify(&mut self, error: &io::Error) {
        eprintln!("[ferrous] library folders are no longer watched: {error}");
        self.inotify = None;
        self.unavailable = Some(error.to_string());
        self.table = WatchTable::default();
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
            // Events were lost: rescan every watched root.
            for root in self.roots.clone().unwrap_or_default() {
                if !self.unwatched.contains_key(&root) {
                    self.pending.add_whole(&root, now);
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
        if mask & libc::IN_ISDIR != 0 {
            if mask & libc::IN_MOVED_FROM != 0 {
                for wd in self.table.remove_below(&path) {
                    if let Some(inotify) = &self.inotify {
                        inotify.remove_watch(wd);
                    }
                }
            }
            if mask & (libc::IN_CREATE | libc::IN_MOVED_TO) != 0 {
                if let Err(fallback) = self.watch_tree(&root, &path) {
                    self.unwatch_root(&root);
                    self.unwatched.insert(root.clone(), fallback);
                    self.next_periodic
                        .get_or_insert_with(|| now + self.tuning.periodic);
                    // Rescan what the watches missed.
                    self.pending.add_whole(&root, now);
                    return;
                }
            }
            self.pending.add(&root, path, now, self.tuning.max_paths);
            return;
        }
        // A link, possibly to a folder, made or moved here.
        let new_link = mask & (libc::IN_CREATE | libc::IN_MOVED_TO) != 0
            && path
                .symlink_metadata()
                .is_ok_and(|meta| meta.file_type().is_symlink());
        if is_supported_audio(&path) || new_link {
            self.pending.add(&root, path, now, self.tuning.max_paths);
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

    /// Rescans the roots that are not watched, and watches again those that
    /// were gone and are back.
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
        if !self.unwatched.is_empty() {
            self.next_periodic.get_or_insert(now + self.tuning.periodic);
        }
    }

    /// Tells the worker the roots that are not watched, when they change.
    fn report(&mut self) {
        let unwatched: Vec<UnwatchedRoot> = self
            .unwatched
            .iter()
            .map(|(root, fallback)| UnwatchedRoot {
                path: root.clone(),
                reason: fallback.describe(self.tuning.periodic),
            })
            .collect();
        if unwatched != self.reported {
            let _ = self
                .commands
                .send(LibraryCommand::SetUnwatchedRoots(unwatched.clone()));
            self.reported = unwatched;
        }
    }
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
    static FS_TYPES: Mutex<Vec<(PathBuf, String)>> = Mutex::new(Vec::new());
    static HELD: Mutex<Vec<PathBuf>> = Mutex::new(Vec::new());
    static PENDING: Mutex<Vec<(PathBuf, Sender<usize>)>> = Mutex::new(Vec::new());
    static WATCHED: Mutex<Vec<(PathBuf, Sender<usize>)>> = Mutex::new(Vec::new());

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

    /// `path`, and the folders below it, are on a file system of `fs_type`.
    pub(crate) fn set_fs_type(path: &Path, fs_type: &str) {
        lock(&FS_TYPES).push((path.to_path_buf(), fs_type.to_string()));
    }

    pub(super) fn fs_type(path: &Path) -> Option<String> {
        lock(&FS_TYPES)
            .iter()
            .find(|(top, _)| path.starts_with(top))
            .map(|(_, fs_type)| fs_type.clone())
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
}

#[cfg(test)]
mod service_tests;
#[cfg(test)]
mod tests;
