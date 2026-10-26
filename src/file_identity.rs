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
//!
//! A save replaces one directory entry, so a watch follows it only when it
//! watches that entry. Other hard links to the file keep the old inode, and
//! so does a reader that opened the file through one of them. Paths are
//! compared resolved, as a save resolves symbolic links and replaces the
//! file they point to.

#[cfg(any(test, feature = "gst"))]
use std::collections::VecDeque;
use std::fs::Metadata;
use std::os::unix::fs::MetadataExt;
use std::path::Path;
#[cfg(any(test, feature = "gst"))]
use std::path::PathBuf;
#[cfg(any(test, feature = "gst"))]
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

    /// The identity of device number `device` and inode number `inode`,
    /// as [`Self::numbers`] gave them.
    pub(crate) fn from_numbers(device: u64, inode: u64) -> Self {
        Self { device, inode }
    }

    /// The device and inode numbers, for storing the identity.
    pub(crate) fn numbers(self) -> (u64, u64) {
        (self.device, self.inode)
    }
}

#[cfg(any(test, feature = "gst"))]
/// Identity of the file at `path` now, or `None` when there is none.
pub(crate) fn file_id(path: &Path) -> Option<FileId> {
    std::fs::metadata(path)
        .ok()
        .map(|metadata| FileId::of(&metadata))
}

#[cfg(any(test, feature = "gst"))]
/// A path with its symbolic links resolved, as a save resolves them.
#[derive(Debug)]
pub(crate) struct ResolvedPath(PathBuf);

#[cfg(any(test, feature = "gst"))]
impl ResolvedPath {
    /// Resolve `path`, which reads the file system. A path that cannot be
    /// resolved is kept as it is.
    pub(crate) fn of(path: &Path) -> Self {
        Self(std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf()))
    }
}

#[cfg(any(test, feature = "gst"))]
/// Replacements kept for watches that start after them; see
/// [`WatchedFile::new`]. They only need to cover the time a reader takes to
/// read an identity, so a few are plenty.
const RECENT_REPLACEMENTS: usize = 64;

#[cfg(any(test, feature = "gst"))]
static REGISTRY: Mutex<Registry> = Mutex::new(Registry {
    replacements: 0,
    recent: VecDeque::new(),
    next_watch: 0,
    watches: Vec::new(),
});

#[cfg(any(test, feature = "gst"))]
struct Registry {
    /// Replacements recorded so far.
    replacements: u64,
    /// The latest replacements.
    recent: VecDeque<Replacement>,
    next_watch: u64,
    watches: Vec<Watch>,
}

#[cfg(any(test, feature = "gst"))]
/// A save replaced the file `old` at the resolved `path` with `new`.
struct Replacement {
    number: u64,
    path: PathBuf,
    old: FileId,
    new: FileId,
}

#[cfg(any(test, feature = "gst"))]
impl Replacement {
    /// The identity that a watch of `file` at `path` has after this.
    fn apply(&self, path: &Path, file: FileId) -> FileId {
        if file == self.old && path == self.path {
            self.new
        } else {
            file
        }
    }
}

#[cfg(any(test, feature = "gst"))]
/// A watched file: the resolved path of the directory entry that a reader
/// opened, and the identity of the file the reader reads.
struct Watch {
    key: u64,
    path: PathBuf,
    file: FileId,
}

#[cfg(any(test, feature = "gst"))]
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

/// A save replaced the file `old` at `path`, resolved, with its edited copy
/// `new`. Watches of `old` at `path` become `new`.
pub(crate) fn replaced(path: &Path, old: FileId, new: FileId) {
    // Only the engine watches files.
    #[cfg(not(any(test, feature = "gst")))]
    let _ = (path, old, new);
    #[cfg(any(test, feature = "gst"))]
    {
        let mut registry = registry();
        registry.replacements += 1;
        let replacement = Replacement {
            number: registry.replacements,
            path: path.to_path_buf(),
            old,
            new,
        };
        for watch in &mut registry.watches {
            watch.file = replacement.apply(&watch.path, watch.file);
        }
        registry.recent.push_back(replacement);
        if registry.recent.len() > RECENT_REPLACEMENTS {
            registry.recent.pop_front();
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
    /// Watch `found`, an identity read at `path` after `since` was taken. A
    /// save that replaced `found` there after that, while it was read, is
    /// followed too.
    pub(crate) fn new(path: ResolvedPath, found: FileId, since: Since) -> Self {
        let mut registry = registry();
        let file = registry
            .recent
            .iter()
            .filter(|replacement| replacement.number > since.0)
            .fold(found, |file, replacement| replacement.apply(&path.0, file));
        let key = registry.next_watch;
        registry.next_watch += 1;
        registry.watches.push(Watch {
            key,
            path: path.0,
            file,
        });
        Self { key }
    }

    /// Identity of the file now, after the saves that replaced it.
    pub(crate) fn current(&self) -> Option<FileId> {
        registry()
            .watches
            .iter()
            .find(|watch| watch.key == self.key)
            .map(|watch| watch.file)
    }

    /// The watched entry was renamed to `path`, so saves there replace it.
    pub(crate) fn renamed(&self, path: ResolvedPath) {
        if let Some(watch) = registry()
            .watches
            .iter_mut()
            .find(|watch| watch.key == self.key)
        {
            watch.path = path.0;
        }
    }
}

#[cfg(any(test, feature = "gst"))]
impl Drop for WatchedFile {
    fn drop(&mut self) {
        registry().watches.retain(|watch| watch.key != self.key);
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

    /// A resolved path no file has.
    fn entry(name: &str) -> PathBuf {
        Path::new("/nonexistent/ferrous-file-identity").join(name)
    }

    fn watch(name: &str, found: FileId, since: Since) -> WatchedFile {
        WatchedFile::new(ResolvedPath(entry(name)), found, since)
    }

    #[test]
    fn watched_files_follow_every_save_that_replaces_them() {
        let (opened, saved, saved_again) = (fake(1), fake(2), fake(3));
        let watched = watch("a", opened, since_now());
        assert_eq!(watched.current(), Some(opened));
        replaced(&entry("a"), opened, saved);
        replaced(&entry("a"), saved, saved_again);
        assert_eq!(watched.current(), Some(saved_again));
    }

    #[test]
    fn a_save_while_the_identity_was_read_is_followed() {
        let (opened, saved, other) = (fake(11), fake(12), fake(13));
        let since = since_now();
        // Saved after `since`, before the reader watched what it found.
        replaced(&entry("b"), opened, saved);
        assert_eq!(watch("b", opened, since).current(), Some(saved));
        // Only at the path it was read from.
        assert_eq!(watch("b-link", opened, since).current(), Some(opened));
        // Saves before `since` were seen by the read.
        replaced(&entry("b"), other, opened);
        let since = since_now();
        assert_eq!(watch("b", other, since).current(), Some(other));
    }

    #[test]
    fn saves_of_other_hard_links_leave_the_watch_alone() {
        let (opened, saved, saved_again) = (fake(31), fake(32), fake(33));
        let watched = watch("c", opened, since_now());
        // Another link to the same file splits off from it.
        replaced(&entry("c-link"), opened, saved);
        assert_eq!(watched.current(), Some(opened));
        // Renamed, the watched entry is replaced by saves at its new path.
        watched.renamed(ResolvedPath(entry("c-renamed")));
        replaced(&entry("c"), opened, saved);
        assert_eq!(watched.current(), Some(opened));
        replaced(&entry("c-renamed"), opened, saved_again);
        assert_eq!(watched.current(), Some(saved_again));
    }

    #[test]
    fn other_files_and_dropped_watches_are_left_alone() {
        let (opened, other, saved) = (fake(21), fake(22), fake(23));
        let watched = watch("d", opened, since_now());
        replaced(&entry("d"), other, saved);
        assert_eq!(watched.current(), Some(opened));
        let key = watched.key;
        drop(watched);
        assert!(registry().watches.iter().all(|watch| watch.key != key));
    }
}
