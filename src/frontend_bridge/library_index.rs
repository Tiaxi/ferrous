// SPDX-License-Identifier: GPL-3.0-or-later

//! The library's tracks by path, so the bridge thread finds a track without
//! scanning the library.

use std::cell::RefCell;
use std::collections::hash_map::DefaultHasher;
use std::fmt;
use std::hash::{BuildHasher, BuildHasherDefault};
use std::ops::ControlFlow;
use std::path::Path;
use std::sync::{Arc, Weak};

use crate::library::LibrarySnapshot;

type PathHasher = BuildHasherDefault<DefaultHasher>;

/// An index of one library's track paths, built the first time a library is
/// searched and kept until the library is replaced.
#[derive(Default)]
pub(super) struct LibraryPathIndex {
    built: RefCell<Option<BuiltIndex>>,
    #[cfg(test)]
    builds: std::cell::Cell<usize>,
}

struct BuiltIndex {
    /// The library indexed. The weak reference keeps a newer library from
    /// being allocated at its address and taken for it, without keeping its
    /// tracks alive.
    library: Weak<LibrarySnapshot>,
    /// The hash of each track's path with the track's index, sorted.
    by_hash: Vec<(u64, usize)>,
}

impl BuiltIndex {
    fn of(library: &Arc<LibrarySnapshot>) -> Self {
        let mut by_hash = library
            .tracks
            .iter()
            .enumerate()
            .map(|(index, track)| (hash_path(&track.path), index))
            .collect::<Vec<_>>();
        by_hash.sort_unstable();
        Self {
            library: Arc::downgrade(library),
            by_hash,
        }
    }

    fn indexes(&self, library: &Arc<LibrarySnapshot>) -> bool {
        std::ptr::eq(self.library.as_ptr(), Arc::as_ptr(library))
    }
}

fn hash_path(path: &Path) -> u64 {
    PathHasher::default().hash_one(path)
}

impl LibraryPathIndex {
    /// Call `found` with the index in `library.tracks` of each track at
    /// `path`, until it breaks.
    fn search(
        &self,
        library: &Arc<LibrarySnapshot>,
        path: &Path,
        mut found: impl FnMut(usize) -> ControlFlow<()>,
    ) {
        let mut built = self.built.borrow_mut();
        let built = match built.as_mut() {
            Some(built) if built.indexes(library) => built,
            _ => {
                #[cfg(test)]
                self.builds.set(self.builds.get() + 1);
                built.insert(BuiltIndex::of(library))
            }
        };
        let hash = hash_path(path);
        let start = built.by_hash.partition_point(|(entry, _)| *entry < hash);
        for (_, index) in built.by_hash[start..]
            .iter()
            .take_while(|(entry, _)| *entry == hash)
        {
            if library.tracks[*index].path == path && found(*index).is_break() {
                return;
            }
        }
    }

    /// The indexes in `library.tracks` of the tracks at `path`.
    pub(super) fn positions(&self, library: &Arc<LibrarySnapshot>, path: &Path) -> Vec<usize> {
        let mut positions = Vec::new();
        self.search(library, path, |index| {
            positions.push(index);
            ControlFlow::Continue(())
        });
        positions
    }

    /// The index in `library.tracks` of the first track at `path`.
    pub(super) fn position(&self, library: &Arc<LibrarySnapshot>, path: &Path) -> Option<usize> {
        let mut position = None;
        self.search(library, path, |index| {
            position = Some(index);
            ControlFlow::Break(())
        });
        position
    }

    pub(super) fn contains(&self, library: &Arc<LibrarySnapshot>, path: &Path) -> bool {
        self.position(library, path).is_some()
    }

    /// Keep the index for `next`, a copy of `previous` whose tracks have the
    /// same paths in the same order.
    pub(super) fn carry_over(&self, previous: &Arc<LibrarySnapshot>, next: &Arc<LibrarySnapshot>) {
        if let Some(built) = self.built.borrow_mut().as_mut() {
            if built.indexes(previous) {
                built.library = Arc::downgrade(next);
            }
        }
    }

    /// How often an index was built.
    #[cfg(test)]
    pub(super) fn builds(&self) -> usize {
        self.builds.get()
    }
}

/// A copy starts without an index, which it builds when first searched.
impl Clone for LibraryPathIndex {
    fn clone(&self) -> Self {
        Self::default()
    }
}

impl fmt::Debug for LibraryPathIndex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LibraryPathIndex").finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::LibraryTrack;
    use std::path::PathBuf;

    fn library(paths: &[&str]) -> Arc<LibrarySnapshot> {
        Arc::new(LibrarySnapshot {
            tracks: paths
                .iter()
                .map(|path| LibraryTrack {
                    path: PathBuf::from(path),
                    ..LibraryTrack::default()
                })
                .collect(),
            ..LibrarySnapshot::default()
        })
    }

    #[test]
    fn finds_tracks_and_builds_once_per_library() {
        let index = LibraryPathIndex::default();
        let first = library(&["/m/a.flac", "/m/b.flac", "/m/c.flac"]);
        assert_eq!(index.position(&first, Path::new("/m/b.flac")), Some(1));
        assert_eq!(index.position(&first, Path::new("/m/c.flac")), Some(2));
        assert!(!index.contains(&first, Path::new("/m/d.flac")));
        assert_eq!(index.builds(), 1);

        let second = library(&["/m/d.flac", "/m/a.flac"]);
        assert_eq!(index.position(&second, Path::new("/m/a.flac")), Some(1));
        assert!(!index.contains(&second, Path::new("/m/b.flac")));
        assert_eq!(index.builds(), 2);
    }

    #[test]
    fn a_copy_with_the_same_paths_keeps_the_index() {
        let index = LibraryPathIndex::default();
        let first = library(&["/m/a.flac", "/m/b.flac"]);
        assert!(index.contains(&first, Path::new("/m/a.flac")));
        let mut copy = (*first).clone();
        copy.tracks[0].title = "Edited".to_string();
        let copy = Arc::new(copy);
        index.carry_over(&first, &copy);
        assert_eq!(index.position(&copy, Path::new("/m/b.flac")), Some(1));
        assert_eq!(index.builds(), 1);
    }

    #[test]
    fn a_new_library_at_a_freed_address_is_indexed_again() {
        let index = LibraryPathIndex::default();
        let first = library(&["/m/a.flac"]);
        assert!(index.contains(&first, Path::new("/m/a.flac")));
        drop(first);
        // The index's weak reference keeps the old allocation, so this one
        // cannot take its address.
        let second = library(&["/m/b.flac"]);
        assert!(!index.contains(&second, Path::new("/m/a.flac")));
        assert!(index.contains(&second, Path::new("/m/b.flac")));
    }

    #[test]
    fn duplicate_paths_are_all_found() {
        let index = LibraryPathIndex::default();
        let lib = library(&["/m/a.flac", "/m/b.flac", "/m/a.flac"]);
        assert_eq!(index.positions(&lib, Path::new("/m/a.flac")), vec![0, 2]);
    }
}
