// SPDX-License-Identifier: GPL-3.0-or-later

//! The library worker: runs library commands one at a time on its own
//! connection, merges repeated scans, and runs the commands that must not
//! wait between the files of a running scan.

use std::collections::{HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender, TryRecvError};
use rusqlite::Connection;

use super::db::{checkpoint, open_worker_db, OpenedLibrary};
use super::publish::WorkerLibrary;
use super::scan::{
    backfill_search_tags, handle_add_root, handle_rescan_all, handle_rescan_root,
    remove_root_and_purge, rename_root, resolve_root_path, ExternalWrites, ScanControl,
    ScanDirective,
};
use super::schema::{load_roots, needs_search_tag_backfill};
use super::{cover_cache, LibraryCommand, LibraryEvent};

/// Runs the library worker until `commands` closes.
pub(super) fn run(
    db_path: Result<PathBuf, String>,
    busy_timeout: Duration,
    commands: Receiver<LibraryCommand>,
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
    let mut control = Control::new(commands);
    prepare_library(&conn, &mut library, event_tx, &mut control, notice);
    while let Some(cmd) = control.next_command() {
        handle_command(&conn, cmd, &mut library, event_tx, &mut control);
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
    library.status.last_error = None;
    control.begin(&cmd);
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
        LibraryCommand::ReadRows(_) => Ok(()),
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
    /// Keeps the channel of a test's control open.
    #[cfg(test)]
    _sender: Option<Sender<LibraryCommand>>,
    queue: CommandQueue,
    /// Set once the service is gone: scans stop, and the worker ends after
    /// the commands that do not scan.
    shutting_down: bool,
    /// Whether the command running scans every root.
    running_all: bool,
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
            #[cfg(test)]
            _sender: None,
            queue: CommandQueue::default(),
            shutting_down: false,
            running_all: false,
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

    fn begin(&mut self, cmd: &LibraryCommand) {
        self.running_all = matches!(cmd, LibraryCommand::RescanAll);
        self.removed.clear();
        self.errors.clear();
    }

    /// The failures of the commands run during the command that ended.
    fn finish(&mut self) -> Vec<String> {
        self.running_all = false;
        self.removed.clear();
        std::mem::take(&mut self.errors)
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
            match self.commands.recv() {
                Ok(cmd) => self.queue.push(resolve_command(cmd), None),
                Err(_) => self.shutting_down = true,
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
            other => {
                // Only removals, renames and row reads are queued as priority
                // commands.
                self.queue.normal.push_front(other);
                Ok(())
            }
        };
        if let Err(err) = result {
            self.errors.push(err);
        }
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
        self.receive(Some(RunningScan {
            all: self.running_all,
            scanning,
            remaining,
        }));
        if !self.queue.priority.is_empty() {
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
        LibraryCommand::RescanAll => LibraryCommand::RescanAll,
        LibraryCommand::ReadRows(paths) => LibraryCommand::ReadRows(paths),
    }
}

/// The scan running when a command arrives.
#[derive(Debug, Clone, Copy)]
struct RunningScan<'a> {
    /// Whether it scans every root.
    all: bool,
    scanning: &'a Path,
    /// The roots it scans after `scanning`.
    remaining: &'a [PathBuf],
}

impl RunningScan<'_> {
    /// Whether the scan has yet to finish `root`.
    fn covers(&self, root: &Path) -> bool {
        self.scanning == root || self.remaining.iter().any(|queued| queued == root)
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
        | LibraryCommand::RescanAll => None,
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
            LibraryCommand::RemoveRoot(path) => {
                // Scans and renames of the root before its removal would be
                // lost with it.
                self.normal.retain(|queued| {
                    scanned_root(queued) != Some(&path) && renamed_root(queued) != Some(&path)
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
                let covered = running.is_some_and(|running| running.covers(&path))
                    || self.normal.iter().any(|queued| {
                        matches!(queued, LibraryCommand::RescanAll)
                            || scanned_root(queued) == Some(&path)
                    });
                if !covered {
                    self.normal.push_back(LibraryCommand::RescanRoot(path));
                }
            }
            LibraryCommand::RescanAll => {
                if running.is_some_and(|running| running.all)
                    || self
                        .normal
                        .iter()
                        .any(|queued| matches!(queued, LibraryCommand::RescanAll))
                {
                    return;
                }
                self.normal
                    .retain(|queued| !matches!(queued, LibraryCommand::RescanRoot(_)));
                self.normal.push_back(LibraryCommand::RescanAll);
            }
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
                    LibraryCommand::RescanRoot(_) | LibraryCommand::RescanAll
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
