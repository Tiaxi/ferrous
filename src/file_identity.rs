// SPDX-License-Identifier: GPL-3.0-or-later

//! Identity of files that a reader holds open, across the app's own saves.
//!
//! A file's device and inode identify it while it is renamed. A save
//! replaces the file with an edited copy instead (see [`crate::safe_save`]),
//! which has a new inode, while a reader that opened the file before keeps
//! reading the old inode, the same audio with the old tags. A reader that
//! later compares the file it opened with the file at a path watches it
//! with [`WatchedFile`]. Every replacement by a save is recorded with
//! [`replaced`], so the watched identity follows the app's saves, but not a
//! replacement made by anything else.

use std::collections::VecDeque;
use std::fs::Metadata;
use std::os::unix::fs::MetadataExt;
#[cfg(any(test, feature = "gst"))]
use std::path::Path;
use std::sync::{Mutex, MutexGuard, PoisonError};

/// Identity of a file, which a rename keeps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct FileId {
    device: u64,
    inode: u64,
}

impl FileId {
    pub(crate) fn of(metadata: &Metadata) -> Self {
        Self {
            device: metadata.dev(),
            inode: metadata.ino(),
        }
    }
}

#[cfg(any(test, feature = "gst"))]
/// Identity of the file at `path` now, or `None` when there is none.
pub(crate) fn file_id(path: &Path) -> Option<FileId> {
    std::fs::metadata(path)
        .ok()
        .map(|metadata| FileId::of(&metadata))
}

/// Replacements kept for watches that start after them; see
/// [`WatchedFile::new`]. They only need to cover the time a reader takes to
/// read an identity, so a few are plenty.
const RECENT_REPLACEMENTS: usize = 64;

static REGISTRY: Mutex<Registry> = Mutex::new(Registry {
    replacements: 0,
    recent: VecDeque::new(),
    #[cfg(any(test, feature = "gst"))]
    next_watch: 0,
    watches: Vec::new(),
});

struct Registry {
    /// Replacements recorded so far.
    replacements: u64,
    /// The latest replacements, with their number.
    recent: VecDeque<(u64, FileId, FileId)>,
    #[cfg(any(test, feature = "gst"))]
    next_watch: u64,
    /// Key and current identity of each watched file.
    watches: Vec<(u64, FileId)>,
}

fn registry() -> MutexGuard<'static, Registry> {
    // Each change of the registry is complete before anything can panic.
    REGISTRY.lock().unwrap_or_else(PoisonError::into_inner)
}

#[cfg(any(test, feature = "gst"))]
/// A point in the sequence of replacements; see [`WatchedFile::new`].
#[derive(Debug, Clone, Copy)]
pub(crate) struct Since(u64);

#[cfg(any(test, feature = "gst"))]
/// The point after the replacements recorded so far.
pub(crate) fn since_now() -> Since {
    Since(registry().replacements)
}

/// A save replaced the file `old` with its edited copy `new`. Watched files
/// that are `old` become `new`.
pub(crate) fn replaced(old: FileId, new: FileId) {
    let mut registry = registry();
    registry.replacements += 1;
    let number = registry.replacements;
    registry.recent.push_back((number, old, new));
    if registry.recent.len() > RECENT_REPLACEMENTS {
        registry.recent.pop_front();
    }
    for (_, file) in &mut registry.watches {
        if *file == old {
            *file = new;
        }
    }
}

#[cfg(any(test, feature = "gst"))]
/// A file that a reader opened. Its identity follows the replacements by
/// saves while this is kept.
#[derive(Debug)]
pub(crate) struct WatchedFile {
    key: u64,
}

#[cfg(any(test, feature = "gst"))]
impl WatchedFile {
    /// Watch `found`, an identity read after `since` was taken. A save that
    /// replaced `found` after that, while it was read, is followed too.
    pub(crate) fn new(found: FileId, since: Since) -> Self {
        let mut registry = registry();
        let mut file = found;
        for &(_, old, new) in registry
            .recent
            .iter()
            .filter(|(number, _, _)| *number > since.0)
        {
            if file == old {
                file = new;
            }
        }
        let key = registry.next_watch;
        registry.next_watch += 1;
        registry.watches.push((key, file));
        Self { key }
    }

    /// Identity of the file now, after the saves that replaced it.
    pub(crate) fn current(&self) -> Option<FileId> {
        registry()
            .watches
            .iter()
            .find(|(key, _)| *key == self.key)
            .map(|(_, file)| *file)
    }
}

#[cfg(any(test, feature = "gst"))]
impl Drop for WatchedFile {
    fn drop(&mut self) {
        registry().watches.retain(|(key, _)| *key != self.key);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Identities no file has, so tests running at the same time cannot
    /// replace them.
    fn fake(inode: u64) -> FileId {
        FileId {
            device: u64::MAX,
            inode,
        }
    }

    #[test]
    fn watched_files_follow_every_save_that_replaces_them() {
        let (opened, saved, saved_again) = (fake(1), fake(2), fake(3));
        let watched = WatchedFile::new(opened, since_now());
        assert_eq!(watched.current(), Some(opened));
        replaced(opened, saved);
        replaced(saved, saved_again);
        assert_eq!(watched.current(), Some(saved_again));
    }

    #[test]
    fn a_save_while_the_identity_was_read_is_followed() {
        let (opened, saved, other) = (fake(11), fake(12), fake(13));
        let since = since_now();
        // Saved after `since`, before the reader watched what it found.
        replaced(opened, saved);
        assert_eq!(WatchedFile::new(opened, since).current(), Some(saved));
        // Saves before `since` were seen by the read.
        replaced(other, opened);
        let since = since_now();
        assert_eq!(WatchedFile::new(other, since).current(), Some(other));
    }

    #[test]
    fn other_files_and_dropped_watches_are_left_alone() {
        let (opened, other, saved) = (fake(21), fake(22), fake(23));
        let watched = WatchedFile::new(opened, since_now());
        replaced(other, saved);
        assert_eq!(watched.current(), Some(opened));
        let key = watched.key;
        drop(watched);
        assert!(registry().watches.iter().all(|(watch, _)| *watch != key));
    }
}
