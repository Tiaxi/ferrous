// SPDX-License-Identifier: GPL-3.0-or-later

//! One edit at a time for each audio file.
//!
//! A tag save reads a file, changes what it read, and replaces the file with
//! the result. When two saves to one file overlap, the one that replaces it
//! last writes what it read before the other's change, and that change is
//! lost. Every path that rewrites or renames an audio file holds the file's
//! [`FileEditGuard`] from its read to its replace, so such edits run one
//! after the other.
//!
//! A thread holds at most one guard at a time. Batch saves take the guards
//! one file after another, so no two threads can wait for each other's
//! guard. Taking a guard waits for the current holder, which is reading and
//! writing a file, so only worker threads take guards, never the UI or
//! bridge thread.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::sync::{Condvar, Mutex, MutexGuard, PoisonError};

/// The files whose guard is held.
static HELD: Mutex<BTreeSet<PathBuf>> = Mutex::new(BTreeSet::new());
static RELEASED: Condvar = Condvar::new();

/// Exclusive permission to edit one file, released when dropped.
#[must_use = "the file is unlocked when the guard is dropped"]
pub(crate) struct FileEditGuard {
    key: PathBuf,
}

/// Wait until no other thread is editing `path`, then hold it.
///
/// A path and the links that resolve to it share a guard, because a rewrite
/// replaces the file a link points to. A path that cannot be resolved, such
/// as a missing file, is locked as given.
pub(crate) fn lock(path: &Path) -> FileEditGuard {
    let key = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    #[cfg(test)]
    test_hooks::enter(&key);
    let mut held = held_files();
    if held.contains(&key) {
        #[cfg(test)]
        test_hooks::notify(&key, test_hooks::Event::Waiting);
        while held.contains(&key) {
            held = RELEASED.wait(held).unwrap_or_else(PoisonError::into_inner);
        }
    }
    held.insert(key.clone());
    FileEditGuard { key }
}

fn held_files() -> MutexGuard<'static, BTreeSet<PathBuf>> {
    // The set stays consistent if a holder panicked: its guard was dropped
    // during unwinding and removed its file.
    HELD.lock().unwrap_or_else(PoisonError::into_inner)
}

impl FileEditGuard {
    /// Marks the point between reading the file and replacing it, where
    /// tests can pause the edit.
    #[cfg(test)]
    pub(crate) fn checkpoint(&self) {
        test_hooks::notify(&self.key, test_hooks::Event::Checkpoint);
        test_hooks::pause(&self.key);
    }
}

impl Drop for FileEditGuard {
    fn drop(&mut self) {
        held_files().remove(&self.key);
        RELEASED.notify_all();
        #[cfg(test)]
        test_hooks::leave();
    }
}

/// Observing and pausing edits of chosen files in tests.
#[cfg(test)]
pub(crate) mod test_hooks {
    use std::cell::Cell;
    use std::collections::BTreeMap;
    use std::path::{Path, PathBuf};
    use std::sync::mpsc::{channel, Receiver, Sender};
    use std::sync::{Mutex, PoisonError};
    use std::time::Duration;

    /// What an edit of an observed file did.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum Event {
        /// An edit started to wait for another edit's guard.
        Waiting,
        /// An edit read the file and has not replaced it yet.
        Checkpoint,
    }

    struct Hook {
        events: Sender<Event>,
        /// When set, the next checkpoint waits for a message on it.
        pause: Option<Receiver<()>>,
    }

    static HOOKS: Mutex<BTreeMap<PathBuf, Hook>> = Mutex::new(BTreeMap::new());

    thread_local! {
        static HOLDING: Cell<bool> = const { Cell::new(false) };
    }

    /// Reports edits of one file until dropped.
    pub(crate) struct Observer {
        key: PathBuf,
        events: Receiver<Event>,
        resume: Sender<()>,
    }

    /// Observe edits of `path`, an existing file. The first edit to reach
    /// its checkpoint stays there until [`Observer::resume`].
    pub(crate) fn pause_first_edit(path: &Path) -> Observer {
        let key = std::fs::canonicalize(path).expect("observed file exists");
        let (events_tx, events) = channel();
        let (resume, pause) = channel();
        let previous = HOOKS.lock().unwrap_or_else(PoisonError::into_inner).insert(
            key.clone(),
            Hook {
                events: events_tx,
                pause: Some(pause),
            },
        );
        assert!(previous.is_none(), "{} is observed twice", key.display());
        Observer {
            key,
            events,
            resume,
        }
    }

    impl Observer {
        /// The next event, failing the test if none comes within a minute.
        pub(crate) fn next_event(&self) -> Event {
            self.events
                .recv_timeout(Duration::from_secs(60))
                .expect("edit event")
        }

        /// Let the paused edit continue.
        pub(crate) fn resume(&self) {
            self.resume.send(()).expect("paused edit is waiting");
        }
    }

    impl Drop for Observer {
        fn drop(&mut self) {
            HOOKS
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .remove(&self.key);
        }
    }

    pub(super) fn notify(key: &Path, event: Event) {
        if let Some(hook) = HOOKS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get(key)
        {
            let _ = hook.events.send(event);
        }
    }

    pub(super) fn pause(key: &Path) {
        let pause = HOOKS
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get_mut(key)
            .and_then(|hook| hook.pause.take());
        if let Some(pause) = pause {
            let _ = pause.recv();
        }
    }

    /// Fails a test whose thread takes a second guard while holding one.
    pub(super) fn enter(key: &Path) {
        assert!(
            !HOLDING.replace(true),
            "{} was locked while another file's guard is held",
            key.display()
        );
    }

    pub(super) fn leave() {
        HOLDING.set(false);
    }
}

#[cfg(test)]
mod tests {
    use super::test_hooks::{pause_first_edit, Event};
    use super::*;
    use std::fs;
    use std::sync::mpsc::channel;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_file(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-file-edit-lock-{name}-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        let path = dir.join("song.flac");
        fs::write(&path, b"audio").expect("write test file");
        path
    }

    #[test]
    fn a_second_edit_of_a_file_waits_for_the_first() {
        let path = test_file("wait");
        let observer = pause_first_edit(&path);
        let (done_tx, done) = channel();

        let first = std::thread::spawn({
            let path = path.clone();
            let done_tx = done_tx.clone();
            move || {
                let guard = lock(&path);
                guard.checkpoint();
                done_tx.send("first").expect("report");
            }
        });
        assert_eq!(observer.next_event(), Event::Checkpoint);
        let second = std::thread::spawn({
            let path = path.clone();
            move || {
                let guard = lock(&path);
                guard.checkpoint();
                done_tx.send("second").expect("report");
            }
        });
        assert_eq!(observer.next_event(), Event::Waiting);

        observer.resume();
        assert_eq!(observer.next_event(), Event::Checkpoint);
        first.join().expect("first edit");
        second.join().expect("second edit");
        assert_eq!(done.try_iter().collect::<Vec<_>>(), ["first", "second"]);
        fs::remove_dir_all(path.parent().expect("test dir")).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn a_link_shares_the_guard_of_its_target() {
        let path = test_file("link");
        let link = path.with_file_name("link.flac");
        std::os::unix::fs::symlink(&path, &link).expect("link");

        let guard = lock(&link);
        assert_eq!(guard.key, fs::canonicalize(&path).expect("canonical"));
        drop(guard);
        // Released guards can be taken again.
        drop(lock(&path));
        drop(lock(&path.with_file_name("missing.flac")));
        fs::remove_dir_all(path.parent().expect("test dir")).expect("remove test dir");
    }
}
