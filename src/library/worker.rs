// SPDX-License-Identifier: GPL-3.0-or-later

//! The library worker: runs library commands one at a time on its own
//! connection, merges repeated scans, and runs the commands that must not
//! wait between the files of a running scan. It tells the watcher which
//! roots to watch, and takes the changes the watcher saw as scans.
//!
//! A root the watcher was just told about is walked only once its watches
//! are set up: a change made after that is seen by the walk or by the
//! watches, and none made before the walk is missed.

use std::collections::{HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crossbeam_channel::{never, select, Receiver, Sender, TryRecvError};
use rusqlite::Connection;

use super::db::{checkpoint, open_worker_db, OpenedLibrary};
use super::publish::WorkerLibrary;
use super::scan::{
    backfill_search_tags, handle_add_root, handle_rescan_all, handle_rescan_root,
    handle_scan_paths, remove_root_and_purge, rename_root, resolve_root_path, ExternalWrites,
    ScanControl, ScanDirective,
};
use super::schema::{load_roots, needs_search_tag_backfill};
use super::watch::WatchHandle;
use super::{cover_cache, LibraryCommand, LibraryEvent};

/// What the library worker receives, and whom it tells about its roots.
pub(super) struct Inputs {
    /// The service's commands; the worker ends once they close.
    pub(super) commands: Receiver<LibraryCommand>,
    /// The watcher's scans of changed paths, and its status.
    pub(super) changes: Receiver<LibraryCommand>,
    /// Told the roots, once the library is ready and whenever they change.
    pub(super) roots: WatchHandle,
    /// How many of the roots messages the watcher has handled, after each.
    pub(super) roots_watched: Receiver<u64>,
}

/// Runs the library worker until its commands close.
pub(super) fn run(
    db_path: Result<PathBuf, String>,
    busy_timeout: Duration,
    inputs: Inputs,
    event_tx: &Sender<LibraryEvent>,
) {
    let mut library = WorkerLibrary::default();
    let opened = db_path.and_then(|path| open_worker_db(&path, busy_timeout));
    let OpenedLibrary { conn, notice } = match opened {
        Ok(opened) => opened,
        Err(err) => {
            library.status.last_error = Some(err);
            library.publish(event_tx);
            return;
        }
    };
    let mut control = Control::new(inputs.commands);
    control.changes = inputs.changes;
    control.watcher = Some(inputs.roots);
    control.roots_watched = inputs.roots_watched;
    prepare_library(&conn, &mut library, event_tx, &mut control, notice);
    // The watcher starts watching, and rescans the roots, only now.
    control.tell_watcher_roots(&library);
    while let Some(cmd) = control.next_command() {
        handle_command(&conn, cmd, &mut library, event_tx, &mut control);
        control.tell_watcher_roots(&library);
    }
    // Nobody is left to show a failure to; the log stays in the database
    // directory and is applied at the next start.
    if let Err(err) = checkpoint(&conn) {
        eprintln!("[ferrous] could not checkpoint the library database: {err}");
    }
}

/// Publishes the stored library, then finishes the upgrades that existing
/// libraries need, reporting what went wrong.
fn prepare_library(
    conn: &Connection,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut Control,
    notice: Option<String>,
) {
    let mut notices: Vec<String> = notice.into_iter().collect();
    library.begin_reads();
    control.external_writes(conn);
    library.reload(conn);
    library.status.last_error = joined(&notices);
    library.publish(event_tx);

    // Publish the cached library first, then fill new tag columns on this
    // library worker. Existing libraries need no manual rescan after upgrade.
    if needs_search_tag_backfill(conn) {
        match backfill_search_tags(conn, library, event_tx) {
            Ok(problem) => notices.extend(problem),
            Err(err) => notices.push(err),
        }
        finish_preparation(conn, library, event_tx, control, &notices);
    }

    match cover_cache::migrate_legacy_embedded_covers(conn, library, event_tx) {
        Ok(false) => {}
        result => {
            if let Err(err) = result {
                notices.push(err);
            }
            // The migration rewrote cover paths in the database.
            library.reload(conn);
            finish_preparation(conn, library, event_tx, control, &notices);
        }
    }
}

fn finish_preparation(
    conn: &Connection,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut Control,
    notices: &[String],
) {
    if control.external_writes(conn) {
        library.reload(conn);
    }
    library.status.scan_in_progress = false;
    library.status.scan_progress = None;
    library.status.last_error = joined(notices);
    library.publish(event_tx);
}

fn joined(problems: &[String]) -> Option<String> {
    (!problems.is_empty()).then(|| problems.join("; "))
}

/// Runs one command on the library worker and publishes what it changed,
/// with the command's failure, and those of commands run during it, in
/// `last_error`.
///
/// The worker's copy follows its own writes, so the library is read again
/// only after a command that removes rows wholesale, or when another
/// connection wrote to the database.
pub(super) fn handle_command(
    conn: &Connection,
    cmd: LibraryCommand,
    library: &mut WorkerLibrary,
    event_tx: &Sender<LibraryEvent>,
    control: &mut Control,
) {
    library.begin_reads();
    if let LibraryCommand::ReadRows(paths) = cmd {
        // Not a user's command: the error shown stays.
        if let Err(err) = library.read_rows(conn, &paths) {
            library.status.last_error = Some(err);
        }
        library.publish(event_tx);
        return;
    }
    if let LibraryCommand::SetUnwatchedRoots(roots) = cmd {
        library.status.unwatched_roots = roots;
        library.publish(event_tx);
        return;
    }
    library.status.last_error = None;
    control.begin();
    let reload = matches!(&cmd, LibraryCommand::RemoveRoot(_));
    let result = match cmd {
        LibraryCommand::ScanRoot(root) => {
            handle_add_root(conn, &root, "", library, event_tx, control)
        }
        LibraryCommand::AddRoot { path, name } => {
            handle_add_root(conn, &path, &name, library, event_tx, control)
        }
        LibraryCommand::RenameRoot { path, name } => rename_root(conn, &path, &name),
        LibraryCommand::RemoveRoot(root) => remove_root_and_purge(conn, &root),
        LibraryCommand::RescanRoot(root) => {
            handle_rescan_root(conn, &root, library, event_tx, control)
        }
        LibraryCommand::RescanAll => handle_rescan_all(conn, library, event_tx, control),
        LibraryCommand::ScanPaths { root, paths } => {
            handle_scan_paths(conn, &root, &paths, library, event_tx, control)
        }
        LibraryCommand::ReadRows(_) | LibraryCommand::SetUnwatchedRoots(_) => Ok(()),
    };
    let errors: Vec<String> = result.err().into_iter().chain(control.finish()).collect();
    library.status.last_error = joined(&errors);
    if control.shutting_down {
        // Nothing is left to publish to.
        return;
    }

    if control.external_writes(conn) || reload {
        library.reload(conn);
    } else {
        library.set_roots(load_roots(conn));
    }
    library.status.scan_in_progress = false;
    library.status.scan_progress = None;
    library.publish(event_tx);
}

/// The library worker's commands, and what it knows about the command it
/// runs.
pub(super) struct Control {
    commands: Receiver<LibraryCommand>,
    /// The watcher's commands; `never` once the watcher is gone.
    changes: Receiver<LibraryCommand>,
    /// Keeps the channel of a test's control open.
    #[cfg(test)]
    _sender: Option<Sender<LibraryCommand>>,
    queue: CommandQueue,
    /// Set once the service is gone: scans stop, and the worker ends after
    /// the commands that do not scan.
    shutting_down: bool,
    /// Told the roots when they change.
    watcher: Option<WatchHandle>,
    /// The roots the watcher was last told.
    watched_roots: Option<Vec<PathBuf>>,
    /// How many roots messages the watcher was sent, and how many it has
    /// handled; `never` once the watcher is gone.
    roots_sent: u64,
    roots_watched: Receiver<u64>,
    roots_handled: u64,
    /// The roots that the watcher may not watch yet, with the number of the
    /// message that first told it about each.
    unconfirmed: HashMap<PathBuf, u64>,
    /// Roots removed while the command ran, which its scans skip.
    removed: HashSet<PathBuf>,
    /// Failures of the commands run during the command.
    errors: Vec<String>,
    /// Commits of other connections since the snapshot was last read.
    external: ExternalWrites,
}

impl Control {
    fn new(commands: Receiver<LibraryCommand>) -> Self {
        Self {
            commands,
            changes: never(),
            #[cfg(test)]
            _sender: None,
            queue: CommandQueue::default(),
            shutting_down: false,
            watcher: None,
            watched_roots: None,
            roots_sent: 0,
            roots_watched: never(),
            roots_handled: 0,
            unconfirmed: HashMap::new(),
            removed: HashSet::new(),
            errors: Vec::new(),
            external: ExternalWrites::default(),
        }
    }

    /// A control with no commands, for tests that run commands directly.
    #[cfg(test)]
    pub(super) fn detached() -> Self {
        let (sender, commands) = crossbeam_channel::unbounded();
        Self {
            _sender: Some(sender),
            ..Self::new(commands)
        }
    }

    fn begin(&mut self) {
        self.removed.clear();
        self.errors.clear();
    }

    /// The failures of the commands run during the command that ended.
    fn finish(&mut self) -> Vec<String> {
        self.removed.clear();
        std::mem::take(&mut self.errors)
    }

    /// Tells the watcher the roots of `library` if they changed since it
    /// was last told, and the first time.
    fn tell_watcher_roots(&mut self, library: &WorkerLibrary) {
        let Some(watcher) = &self.watcher else {
            return;
        };
        let roots = library.roots();
        if self.watched_roots.as_ref().is_some_and(|told| {
            told.len() == roots.len() && told.iter().zip(roots).all(|(a, b)| *a == b.path)
        }) {
            return;
        }
        let paths: Vec<PathBuf> = roots.iter().map(|root| root.path.clone()).collect();
        watcher.set_roots(paths.clone());
        self.roots_sent += 1;
        let told = self.watched_roots.take().unwrap_or_default();
        self.unconfirmed.retain(|root, _| paths.contains(root));
        for root in paths.iter().filter(|root| !told.contains(root)) {
            self.unconfirmed.insert(root.clone(), self.roots_sent);
        }
        self.watched_roots = Some(paths);
    }

    /// Notes the roots messages the watcher has handled, without waiting.
    fn note_roots_watched(&mut self) {
        loop {
            match self.roots_watched.try_recv() {
                Ok(handled) => self.roots_handled = handled,
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    self.watcher_gone();
                    break;
                }
            }
        }
        let handled = self.roots_handled;
        self.unconfirmed.retain(|_, told_in| *told_in > handled);
    }

    /// Nothing is waited for from a watcher that ended.
    fn watcher_gone(&mut self) {
        self.roots_watched = never();
        self.roots_handled = u64::MAX;
    }

    /// Waits until the watcher has set up the watches of `root`, if it was
    /// told about the root and has not done so yet, so that the scan of
    /// `root` that follows sees every change that the watches miss. Watches
    /// already being set up for other roots are waited for too, since the
    /// watcher handles its messages in order. The commands that arrive
    /// meanwhile are queued as for the running scan, which has `remaining`
    /// left; shutting down ends the wait.
    fn wait_for_watches(&mut self, root: &Path, remaining: &[PathBuf]) {
        self.note_roots_watched();
        let Some(&told_in) = self.unconfirmed.get(root) else {
            return;
        };
        #[cfg(test)]
        super::watch::watch_test_hooks::note_scan_wait(root);
        while self.roots_handled < told_in && !self.shutting_down {
            select! {
                recv(self.roots_watched) -> handled => match handled {
                    Ok(handled) => self.roots_handled = handled,
                    Err(_) => self.watcher_gone(),
                },
                recv(self.commands) -> cmd => match cmd {
                    Ok(cmd) => self
                        .queue
                        .push(resolve_command(cmd), Some(RunningScan { remaining })),
                    Err(_) => self.shutting_down = true,
                },
            }
        }
        let handled = self.roots_handled;
        self.unconfirmed.retain(|_, told_in| *told_in > handled);
    }

    /// Queues the commands that have arrived, merging them with those
    /// queued and with `running`.
    fn receive(&mut self, running: Option<RunningScan<'_>>) {
        loop {
            match self.commands.try_recv() {
                Ok(cmd) => self.queue.push(resolve_command(cmd), running),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    self.shutting_down = true;
                    break;
                }
            }
        }
        loop {
            match self.changes.try_recv() {
                Ok(cmd) => self.queue.push(cmd, running),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    self.changes = never();
                    break;
                }
            }
        }
    }

    /// The next command to run, waiting for one. `None` once the service is
    /// gone and the commands left are run.
    fn next_command(&mut self) -> Option<LibraryCommand> {
        loop {
            self.receive(None);
            if let Some(cmd) = self.queue.pop(self.shutting_down) {
                return Some(cmd);
            }
            if self.shutting_down {
                return None;
            }
            select! {
                recv(self.commands) -> cmd => match cmd {
                    Ok(cmd) => self.queue.push(resolve_command(cmd), None),
                    Err(_) => self.shutting_down = true,
                },
                recv(self.changes) -> cmd => match cmd {
                    Ok(cmd) => self.queue.push(cmd, None),
                    Err(_) => self.changes = never(),
                },
            }
        }
    }

    fn run_priority(
        &mut self,
        conn: &Connection,
        library: &mut WorkerLibrary,
        cmd: LibraryCommand,
    ) {
        let result = match cmd {
            LibraryCommand::RemoveRoot(root) => remove_root_and_purge(conn, &root).map(|()| {
                self.removed.insert(root);
            }),
            LibraryCommand::RenameRoot { path, name } => rename_root(conn, &path, &name),
            LibraryCommand::ReadRows(paths) => library.read_rows(conn, &paths),
            LibraryCommand::SetUnwatchedRoots(roots) => {
                library.status.unwatched_roots = roots;
                Ok(())
            }
            other => {
                // Only removals, renames, row reads and the watcher's status
                // are queued as priority commands.
                self.queue.normal.push_front(other);
                Ok(())
            }
        };
        if let Err(err) = result {
            self.errors.push(err);
        }
    }

    /// Runs the queued commands that must not wait for the running scan,
    /// and publishes what they changed.
    fn run_priority_commands(
        &mut self,
        conn: &Connection,
        library: &mut WorkerLibrary,
        event_tx: &Sender<LibraryEvent>,
    ) {
        if self.queue.priority.is_empty() {
            return;
        }
        let mut removed_any = false;
        while let Some(cmd) = self.queue.priority.pop_front() {
            removed_any |= matches!(cmd, LibraryCommand::RemoveRoot(_));
            self.run_priority(conn, library, cmd);
        }
        if removed_any {
            self.external_writes(conn);
            library.reload(conn);
        } else {
            library.set_roots(load_roots(conn));
        }
        library.status.last_error = joined(&self.errors);
        library.publish(event_tx);
    }
}

impl ScanControl for Control {
    fn poll(
        &mut self,
        conn: &Connection,
        library: &mut WorkerLibrary,
        event_tx: &Sender<LibraryEvent>,
        scanning: &Path,
        remaining: &[PathBuf],
    ) -> ScanDirective {
        self.receive(Some(RunningScan { remaining }));
        self.run_priority_commands(conn, library, event_tx);
        // An added root is watched before it is walked.
        self.tell_watcher_roots(library);
        if !self.removed.contains(scanning) {
            self.wait_for_watches(scanning, remaining);
            // Those that arrived meanwhile.
            self.run_priority_commands(conn, library, event_tx);
            self.tell_watcher_roots(library);
        }
        if self.shutting_down {
            ScanDirective::StopAll
        } else if self.removed.contains(scanning) {
            ScanDirective::SkipRoot
        } else {
            ScanDirective::Continue
        }
    }

    fn external_writes(&mut self, conn: &Connection) -> bool {
        self.external.since_last_check(conn)
    }
}

/// `cmd` with its root as the database stores it, so that commands for the
/// same root match.
fn resolve_command(cmd: LibraryCommand) -> LibraryCommand {
    match cmd {
        LibraryCommand::ScanRoot(path) => LibraryCommand::AddRoot {
            path: resolve_root_path(&path),
            name: String::new(),
        },
        LibraryCommand::AddRoot { path, name } => LibraryCommand::AddRoot {
            path: resolve_root_path(&path),
            name,
        },
        LibraryCommand::RenameRoot { path, name } => LibraryCommand::RenameRoot {
            path: resolve_root_path(&path),
            name,
        },
        LibraryCommand::RemoveRoot(path) => LibraryCommand::RemoveRoot(resolve_root_path(&path)),
        LibraryCommand::RescanRoot(path) => LibraryCommand::RescanRoot(resolve_root_path(&path)),
        other @ (LibraryCommand::RescanAll
        | LibraryCommand::ReadRows(_)
        | LibraryCommand::ScanPaths { .. }
        | LibraryCommand::SetUnwatchedRoots(_)) => other,
    }
}

/// The scan running when a command arrives.
#[derive(Debug, Clone, Copy)]
struct RunningScan<'a> {
    /// The roots it scans after the one it scans now.
    remaining: &'a [PathBuf],
}

impl RunningScan<'_> {
    /// Whether the scan has yet to start scanning `root`. Only then does it
    /// see every change made to the root since: a scan of the root under way
    /// may have walked past the changed files already.
    fn has_yet_to_start(&self, root: &Path) -> bool {
        self.remaining.iter().any(|queued| queued == root)
    }
}

/// The root a command adds or scans.
fn scanned_root(cmd: &LibraryCommand) -> Option<&Path> {
    match cmd {
        LibraryCommand::ScanRoot(path)
        | LibraryCommand::AddRoot { path, .. }
        | LibraryCommand::RescanRoot(path) => Some(path),
        LibraryCommand::RenameRoot { .. }
        | LibraryCommand::RemoveRoot(_)
        | LibraryCommand::ReadRows(_)
        | LibraryCommand::ScanPaths { .. }
        | LibraryCommand::SetUnwatchedRoots(_)
        | LibraryCommand::RescanAll => None,
    }
}

/// The root of the changed paths that a command scans.
fn changed_paths_root(cmd: &LibraryCommand) -> Option<&Path> {
    match cmd {
        LibraryCommand::ScanPaths { root, .. } => Some(root),
        _ => None,
    }
}

/// Commands waiting for the library worker. Repeated scans merge, and
/// removing or renaming a root runs before queued scans, and between the
/// files of a running one.
#[derive(Debug, Default)]
struct CommandQueue {
    /// Root removals and renames.
    priority: VecDeque<LibraryCommand>,
    normal: VecDeque<LibraryCommand>,
}

impl CommandQueue {
    /// Queues `cmd`, which arrived while `running` scanned, if anything.
    fn push(&mut self, cmd: LibraryCommand, running: Option<RunningScan<'_>>) {
        match cmd {
            LibraryCommand::ScanRoot(path) => self.push(
                LibraryCommand::AddRoot {
                    path,
                    name: String::new(),
                },
                running,
            ),
            LibraryCommand::AddRoot { path, name } => {
                let queued = self.normal.iter_mut().find_map(|queued| match queued {
                    LibraryCommand::AddRoot {
                        path: queued_path,
                        name: queued_name,
                    } if *queued_path == path => Some(queued_name),
                    _ => None,
                });
                match queued {
                    // Adding a root again changes only its name.
                    Some(queued_name) => {
                        if !name.trim().is_empty() {
                            *queued_name = name;
                        }
                    }
                    None => self
                        .normal
                        .push_back(LibraryCommand::AddRoot { path, name }),
                }
            }
            LibraryCommand::RenameRoot { path, name } => {
                let cmd = LibraryCommand::RenameRoot { path, name };
                if self.normal.iter().any(|queued| {
                    matches!(queued, LibraryCommand::AddRoot { path, .. }
                        if Some(path.as_path()) == renamed_root(&cmd))
                }) {
                    // The root is not added yet; rename it after.
                    self.normal.push_back(cmd);
                } else {
                    self.priority.push_back(cmd);
                }
            }
            // Runs between the files of a scan, so that a copy of the
            // library is corrected soon.
            LibraryCommand::ReadRows(paths) => {
                match self.priority.iter_mut().find_map(|queued| match queued {
                    LibraryCommand::ReadRows(queued) => Some(queued),
                    _ => None,
                }) {
                    Some(queued) => queued.extend(paths),
                    None => self.priority.push_back(LibraryCommand::ReadRows(paths)),
                }
            }
            LibraryCommand::SetUnwatchedRoots(roots) => self.push_watch_status(roots),
            LibraryCommand::ScanPaths { root, paths } => {
                self.push_changed_paths(root, paths, running);
            }
            LibraryCommand::RemoveRoot(path) => {
                // Scans and renames of the root before its removal would be
                // lost with it.
                self.normal.retain(|queued| {
                    scanned_root(queued) != Some(&path)
                        && changed_paths_root(queued) != Some(&path)
                        && renamed_root(queued) != Some(&path)
                });
                self.priority
                    .retain(|queued| renamed_root(queued) != Some(&path));
                if !self
                    .priority
                    .iter()
                    .any(|queued| matches!(queued, LibraryCommand::RemoveRoot(queued) if *queued == path))
                {
                    self.priority.push_back(LibraryCommand::RemoveRoot(path));
                }
            }
            LibraryCommand::RescanRoot(path) => {
                let covered = running.is_some_and(|running| running.has_yet_to_start(&path))
                    || self.normal.iter().any(|queued| {
                        matches!(queued, LibraryCommand::RescanAll)
                            || scanned_root(queued) == Some(&path)
                    });
                if !covered {
                    // The rescan scans the changed paths too.
                    self.normal
                        .retain(|queued| changed_paths_root(queued) != Some(&path));
                    self.normal.push_back(LibraryCommand::RescanRoot(path));
                }
            }
            LibraryCommand::RescanAll => {
                // A running rescan of every root has passed some of them, so
                // only a queued one covers this one.
                if self
                    .normal
                    .iter()
                    .any(|queued| matches!(queued, LibraryCommand::RescanAll))
                {
                    return;
                }
                self.normal.retain(|queued| {
                    !matches!(
                        queued,
                        LibraryCommand::RescanRoot(_) | LibraryCommand::ScanPaths { .. }
                    )
                });
                self.normal.push_back(LibraryCommand::RescanAll);
            }
        }
    }

    /// Queues the watcher's status, which runs between the files of a scan.
    /// Only the newest matters.
    fn push_watch_status(&mut self, roots: Vec<super::UnwatchedRoot>) {
        self.priority
            .retain(|queued| !matches!(queued, LibraryCommand::SetUnwatchedRoots(_)));
        self.priority
            .push_back(LibraryCommand::SetUnwatchedRoots(roots));
    }

    /// Queues a scan of `paths` below `root`, unless a queued scan, or the
    /// running one, has yet to scan all of `root`. Scans of changed paths
    /// of a root merge.
    fn push_changed_paths(
        &mut self,
        root: PathBuf,
        paths: Vec<PathBuf>,
        running: Option<RunningScan<'_>>,
    ) {
        // A running scan of the root may have walked past the paths already;
        // only one that has yet to start the root covers them.
        let covered = running.is_some_and(|running| running.has_yet_to_start(&root))
            || self.normal.iter().any(|queued| {
                matches!(queued, LibraryCommand::RescanAll)
                    || scanned_root(queued) == Some(root.as_path())
            });
        if covered {
            return;
        }
        let queued = self.normal.iter_mut().find_map(|queued| match queued {
            LibraryCommand::ScanPaths {
                root: queued_root,
                paths: queued_paths,
            } if *queued_root == root => Some(queued_paths),
            _ => None,
        });
        match queued {
            Some(queued) => {
                let known: HashSet<PathBuf> = queued.iter().cloned().collect();
                queued.extend(paths.into_iter().filter(|path| !known.contains(path)));
            }
            None => self
                .normal
                .push_back(LibraryCommand::ScanPaths { root, paths }),
        }
    }

    /// The next command to run. Once shutting down, scans are dropped.
    fn pop(&mut self, shutting_down: bool) -> Option<LibraryCommand> {
        if let Some(cmd) = self.priority.pop_front() {
            return Some(cmd);
        }
        while let Some(cmd) = self.normal.pop_front() {
            if shutting_down
                && matches!(
                    cmd,
                    LibraryCommand::RescanRoot(_)
                        | LibraryCommand::RescanAll
                        | LibraryCommand::ScanPaths { .. }
                )
            {
                continue;
            }
            return Some(cmd);
        }
        None
    }
}

fn renamed_root(cmd: &LibraryCommand) -> Option<&Path> {
    match cmd {
        LibraryCommand::RenameRoot { path, .. } => Some(path),
        _ => None,
    }
}

#[cfg(test)]
mod tests;
