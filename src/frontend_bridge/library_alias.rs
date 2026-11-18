// SPDX-License-Identifier: GPL-3.0-or-later

//! The library's own path for a file it holds under another spelling.
//!
//! The library keeps a track at the path its scan reached it by, and a scan
//! follows links: a file in a folder that a link inside a library folder
//! leads to is kept under the link's path. A file opened from elsewhere is
//! queued by its canonical path, with every link resolved. The two spell
//! one file differently, and the library's path index, which matches paths
//! exactly, finds nothing for the other spelling.
//!
//! Telling that two paths are one file needs the file system, so a worker
//! does it, off the bridge thread: it resolves the path asked about and
//! the folders of the library's tracks with the same file name, and
//! returns the track whose folder resolves to the same one. The bridge
//! keeps the answers for the library they were found in, and asks again
//! once the library changed.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc, Weak,
};

use crossbeam_channel::{unbounded, Receiver, Sender};

use super::library_index::LibraryPathIndex;
use crate::library::LibrarySnapshot;

struct AliasRequest {
    library: Arc<LibrarySnapshot>,
    paths: Vec<PathBuf>,
    generation: u64,
}

pub(super) struct AliasResult {
    library: Weak<LibrarySnapshot>,
    /// Each path asked about, with the library's path for its file, if the
    /// library has the file.
    found: Vec<(PathBuf, Option<PathBuf>)>,
    generation: u64,
}

/// Finds, on its own thread, the library's paths for paths it does not
/// hold as spelled.
pub(super) struct LibraryAliasWorker {
    tx: Sender<AliasRequest>,
    pub(super) results: Receiver<AliasResult>,
    generation: Arc<AtomicU64>,
    /// What the worker was last asked, until its answer is taken.
    in_flight: Option<(Weak<LibrarySnapshot>, Vec<PathBuf>)>,
}

impl LibraryAliasWorker {
    pub(super) fn new() -> Self {
        let (tx, rx) = unbounded::<AliasRequest>();
        let (result_tx, results) = unbounded();
        let generation = Arc::new(AtomicU64::new(0));
        let active = Arc::clone(&generation);
        crate::worker_thread::spawn_worker("ferrous-library-alias", move || {
            while let Ok(mut request) = rx.recv() {
                for newer in rx.try_iter() {
                    request = newer;
                }
                if active.load(Ordering::Relaxed) != request.generation {
                    continue;
                }
                let found = request
                    .paths
                    .into_iter()
                    .map(|path| {
                        let library_path = library_path_of(&request.library, &path);
                        (path, library_path)
                    })
                    .collect();
                if active.load(Ordering::Relaxed) == request.generation
                    && result_tx
                        .send(AliasResult {
                            library: Arc::downgrade(&request.library),
                            found,
                            generation: request.generation,
                        })
                        .is_err()
                {
                    break;
                }
            }
        });
        Self {
            tx,
            results,
            generation,
            in_flight: None,
        }
    }

    /// Ask for the library's paths for `paths`, unless the worker is
    /// already finding them in `library`.
    pub(super) fn request(&mut self, library: &Arc<LibrarySnapshot>, paths: Vec<PathBuf>) {
        if self.in_flight.as_ref().is_some_and(|(asked, asked_paths)| {
            std::ptr::eq(asked.as_ptr(), Arc::as_ptr(library)) && *asked_paths == paths
        }) {
            return;
        }
        let generation = self.generation.fetch_add(1, Ordering::Relaxed) + 1;
        self.in_flight = Some((Arc::downgrade(library), paths.clone()));
        if self
            .tx
            .send(AliasRequest {
                library: Arc::clone(library),
                paths,
                generation,
            })
            .is_err()
        {
            self.in_flight = None;
        }
    }

    /// Whether `result` answers the latest request. Taking it lets the
    /// worker be asked the same again.
    pub(super) fn take_current(&mut self, result: &AliasResult) -> bool {
        let current = result.generation == self.generation.load(Ordering::Relaxed);
        if current {
            self.in_flight = None;
        }
        current
    }
}

impl Drop for LibraryAliasWorker {
    fn drop(&mut self) {
        self.generation.fetch_add(1, Ordering::Relaxed);
    }
}

/// The path of the track in `library` that is the file at `path`, found
/// by resolving links: a track with the same file name whose folder
/// resolves to the folder of `path`. `None` when `path` cannot be resolved
/// or no track is the file.
fn library_path_of(library: &LibrarySnapshot, path: &Path) -> Option<PathBuf> {
    let file = path.canonicalize().ok()?;
    let (folder, name) = (file.parent()?, file.file_name()?);
    // The folders resolved so far, as tracks of one folder share it.
    let mut resolves_to_folder = HashMap::<&Path, bool>::new();
    library
        .tracks
        .iter()
        .find(|track| {
            track.path.file_name() == Some(name)
                && track.path.parent().is_some_and(|parent| {
                    *resolves_to_folder
                        .entry(parent)
                        .or_insert_with(|| parent.canonicalize().is_ok_and(|c| c == folder))
                })
        })
        .map(|track| track.path.clone())
}

/// The worker's answers, for the library they were found in.
#[derive(Debug, Clone, Default)]
pub(super) struct LibraryAliases {
    library: Weak<LibrarySnapshot>,
    found: HashMap<PathBuf, Option<PathBuf>>,
}

impl LibraryAliases {
    /// The path `library` holds the file at `path` under: `path` itself, or
    /// the path the worker found for it, while `library` still has that
    /// track. An answer found in an earlier library is used until the
    /// worker answers for this one, so the playing track stays marked
    /// while a scan changes the library.
    pub(super) fn library_path<'a>(
        &'a self,
        index: &LibraryPathIndex,
        library: &Arc<LibrarySnapshot>,
        path: &'a Path,
    ) -> Option<&'a Path> {
        if index.contains(library, path) {
            return Some(path);
        }
        self.found
            .get(path)?
            .as_deref()
            .filter(|found| index.contains(library, found))
    }

    /// Whether the worker answered for `path` in `library` itself.
    pub(super) fn is_settled(&self, library: &Arc<LibrarySnapshot>, path: &Path) -> bool {
        self.is_for(library) && self.found.contains_key(path)
    }

    fn is_for(&self, library: &Arc<LibrarySnapshot>) -> bool {
        std::ptr::eq(self.library.as_ptr(), Arc::as_ptr(library))
    }

    /// Keep the answers of `result`. Answers for another library are
    /// replaced, since the bridge asks again for every path it still needs
    /// once the library changes.
    pub(super) fn store(&mut self, result: AliasResult) {
        if !std::ptr::eq(self.library.as_ptr(), result.library.as_ptr()) {
            self.found.clear();
            self.library = result.library;
        }
        self.found.extend(result.found);
    }
}

#[cfg(test)]
pub(super) mod test_support {
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    /// A library folder with a link inside it to an album folder elsewhere,
    /// as a scan keeps it: the album's track under the link's path.
    pub(crate) struct LinkedAlbum {
        pub(crate) base: PathBuf,
        /// The library folder.
        pub(crate) root: PathBuf,
        /// The track as the library holds it, through the link.
        pub(crate) linked: PathBuf,
        /// The same file by its canonical path.
        pub(crate) canonical: PathBuf,
    }

    impl Drop for LinkedAlbum {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.base);
        }
    }

    pub(crate) fn linked_album() -> LinkedAlbum {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let base = std::env::temp_dir().join(format!(
            "ferrous-library-alias-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&base).expect("temp dir of the test's own");
        let base = base.canonicalize().expect("canonical temp dir");
        let album = base.join("elsewhere/Album");
        std::fs::create_dir_all(&album).expect("album folder");
        std::fs::write(album.join("01.flac"), b"audio").expect("track");
        let root = base.join("Music");
        std::fs::create_dir_all(&root).expect("library folder");
        std::os::unix::fs::symlink(&album, root.join("Album")).expect("link");
        LinkedAlbum {
            base,
            root: root.clone(),
            linked: root.join("Album/01.flac"),
            canonical: album.join("01.flac"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::test_support::linked_album;
    use super::*;
    use crate::library::LibraryTrack;
    use std::time::Duration;

    fn library_of(paths: &[PathBuf]) -> Arc<LibrarySnapshot> {
        Arc::new(LibrarySnapshot {
            tracks: paths
                .iter()
                .map(|path| {
                    Arc::new(LibraryTrack {
                        path: path.clone(),
                        ..LibraryTrack::default()
                    })
                })
                .collect(),
            ..LibrarySnapshot::default()
        })
    }

    #[test]
    fn finds_a_track_the_library_holds_through_a_link() {
        let album = linked_album();
        let library = library_of(std::slice::from_ref(&album.linked));
        assert_eq!(
            library_path_of(&library, &album.canonical),
            Some(album.linked.clone())
        );
    }

    #[test]
    fn finds_a_track_asked_about_through_a_link() {
        let album = linked_album();
        let library = library_of(std::slice::from_ref(&album.canonical));
        assert_eq!(
            library_path_of(&library, &album.linked),
            Some(album.canonical.clone())
        );
    }

    #[test]
    fn finds_nothing_for_another_file_of_the_same_name() {
        let album = linked_album();
        let other = album.base.join("elsewhere/Other");
        std::fs::create_dir_all(&other).expect("other folder");
        std::fs::write(other.join("01.flac"), b"audio").expect("other track");
        let library = library_of(&[other.join("01.flac")]);
        assert_eq!(library_path_of(&library, &album.canonical), None);
        assert_eq!(
            library_path_of(&library, Path::new("/nonexistent/01.flac")),
            None
        );
    }

    #[test]
    fn worker_answers_the_newest_request_for_its_library() {
        let album = linked_album();
        let library = library_of(std::slice::from_ref(&album.linked));
        let mut worker = LibraryAliasWorker::new();
        worker.request(&library, vec![album.canonical.clone()]);
        let result = worker
            .results
            .recv_timeout(Duration::from_secs(5))
            .expect("answer");
        assert!(worker.take_current(&result));

        let mut aliases = LibraryAliases::default();
        aliases.store(result);
        let index = LibraryPathIndex::default();
        assert!(aliases.is_settled(&library, &album.canonical));
        assert_eq!(
            aliases.library_path(&index, &library, &album.canonical),
            Some(album.linked.as_path())
        );

        // A changed library keeps the answer until the worker answers for
        // it, while the track is still there.
        let changed = library_of(std::slice::from_ref(&album.linked));
        assert!(!aliases.is_settled(&changed, &album.canonical));
        assert_eq!(
            aliases.library_path(&index, &changed, &album.canonical),
            Some(album.linked.as_path())
        );
        let emptied = library_of(&[]);
        assert_eq!(
            aliases.library_path(&index, &emptied, &album.canonical),
            None
        );
    }

    #[test]
    fn an_older_answer_is_not_current() {
        let album = linked_album();
        let library = library_of(std::slice::from_ref(&album.linked));
        let mut worker = LibraryAliasWorker::new();
        worker.request(&library, vec![album.canonical.clone()]);
        let first = worker
            .results
            .recv_timeout(Duration::from_secs(5))
            .expect("first answer");
        let changed = library_of(&[]);
        worker.request(&changed, vec![album.canonical.clone()]);
        assert!(!worker.take_current(&first));
        let latest = worker
            .results
            .recv_timeout(Duration::from_secs(5))
            .expect("latest answer");
        assert!(worker.take_current(&latest));
        assert_eq!(latest.found, vec![(album.canonical.clone(), None)]);
    }
}
