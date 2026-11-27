// SPDX-License-Identifier: GPL-3.0-or-later

//! Find in Queue: the rows of the queue a query of the global search's
//! syntax finds, in queue order.
//!
//! The search runs on the search worker's thread and matches a row of a
//! library track with the fields global search prepared for the track, so
//! that a query finds the same tracks in both. A row of a file outside the
//! library is matched with its own details, prepared once per queue
//! details. Each result names the queue revision it was found in, so the UI
//! takes only results that name its rows, and acts on them through the
//! revision's remapping like any other queue command.

use std::collections::hash_map::DefaultHasher;
use std::collections::HashMap;
use std::hash::{BuildHasher, BuildHasherDefault};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crossbeam_channel::Receiver;

use super::search::{split_search_terms, PreparedSearchLibrary, TrackSearchText};
use super::search_query::{SearchFields, SearchQuery};
use super::BridgeState;
use crate::library::{IndexedTrack, LibrarySnapshot, SearchTags};

/// How many rows a search reads between checks for a newer query.
const CANCEL_POLL_ROWS: usize = 1024;

/// A query to search the queue with, and the queue as it was then.
#[derive(Debug)]
pub(super) struct QueueSearchJob {
    pub(super) seq: u32,
    pub(super) query: String,
    /// The queue revision of `paths`.
    pub(super) revision: u64,
    pub(super) paths: Arc<[PathBuf]>,
    pub(super) details: Arc<HashMap<PathBuf, IndexedTrack>>,
    pub(super) library: Arc<LibrarySnapshot>,
}

/// The rows of the queue at `revision` that the query numbered `seq`
/// found, ascending.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueSearchFrame {
    pub seq: u32,
    pub revision: u64,
    pub rows: Vec<u32>,
}

/// The bridge's side: the queue as the searches read it, copied once per
/// revision, so that typing does not copy it on every key.
#[derive(Default)]
pub(super) struct QueueSearchRequests {
    paths: Option<(u64, Arc<[PathBuf]>)>,
}

impl QueueSearchRequests {
    /// The job that searches the queue of `state` for `query`. An empty
    /// query finds nothing and needs no copy of the queue.
    pub(super) fn job(&mut self, state: &BridgeState, seq: u32, query: &str) -> QueueSearchJob {
        let revision = state.queue_sync.revision();
        let query = query.trim().to_string();
        let paths = if query.is_empty() {
            Arc::default()
        } else {
            match &self.paths {
                Some((copied, paths)) if *copied == revision => Arc::clone(paths),
                _ => {
                    let paths: Arc<[PathBuf]> = state.queue.as_slice().into();
                    self.paths = Some((revision, Arc::clone(&paths)));
                    paths
                }
            }
        };
        QueueSearchJob {
            seq,
            query,
            revision,
            paths,
            details: Arc::clone(&state.queue_details),
            library: Arc::clone(&state.library),
        }
    }

    /// Drop the copy of the queue, once the palette no longer searches it.
    pub(super) fn release(&mut self) {
        self.paths = None;
    }
}

type PathHasher = BuildHasherDefault<DefaultHasher>;

fn hash_path(path: &Path) -> u64 {
    PathHasher::default().hash_one(path)
}

/// The worker's side: where each queue row's fields are.
#[derive(Default)]
pub(super) struct QueueSearchCache {
    /// The library `by_hash` indexes; held so that a newer library is not
    /// built at its address and taken for it.
    library: Option<Arc<LibrarySnapshot>>,
    /// The hash of each library track's path with the track's index,
    /// sorted.
    by_hash: Vec<(u64, usize)>,
    /// The queue details and library `outside` was prepared from.
    details: Option<Arc<HashMap<PathBuf, IndexedTrack>>>,
    /// The fields of rows of files outside the library, by path.
    outside: HashMap<PathBuf, SearchFields>,
}

impl QueueSearchCache {
    /// Index `library` and forget fields prepared from other details, or
    /// for another library, in which a path may have been outside it.
    fn refresh(
        &mut self,
        library: &Arc<LibrarySnapshot>,
        details: &Arc<HashMap<PathBuf, IndexedTrack>>,
    ) {
        let same_library = self
            .library
            .as_ref()
            .is_some_and(|indexed| Arc::ptr_eq(indexed, library));
        if !same_library {
            self.by_hash = library
                .tracks
                .iter()
                .enumerate()
                .map(|(index, track)| (hash_path(&track.path), index))
                .collect();
            self.by_hash.sort_unstable();
            self.library = Some(Arc::clone(library));
            self.outside.clear();
        }
        if !self
            .details
            .as_ref()
            .is_some_and(|prepared| Arc::ptr_eq(prepared, details))
        {
            self.details = Some(Arc::clone(details));
            self.outside.clear();
        }
    }

    /// The index in the library of its track at `path`.
    fn library_index(&self, library: &LibrarySnapshot, path: &Path) -> Option<usize> {
        let hash = hash_path(path);
        let start = self.by_hash.partition_point(|(entry, _)| *entry < hash);
        self.by_hash[start..]
            .iter()
            .take_while(|(entry, _)| *entry == hash)
            .map(|(_, index)| *index)
            .find(|index| {
                library
                    .tracks
                    .get(*index)
                    .is_some_and(|track| track.path == path)
            })
    }
}

/// The fields of a row of a file outside the library: its details, if they
/// were read, and its folder's and file's names, as the library's rows
/// match their path within their root.
fn outside_fields(path: &Path, details: Option<&IndexedTrack>) -> SearchFields {
    let tail = path
        .parent()
        .and_then(Path::file_name)
        .map(Path::new)
        .zip(path.file_name())
        .map_or_else(|| path.to_path_buf(), |(folder, name)| folder.join(name));
    let path_text = tail.to_string_lossy();
    let no_tags = SearchTags::default();
    let text = match details {
        Some(track) => TrackSearchText {
            tags: &track.search_tags,
            title: &track.title,
            artist: &track.artist,
            album: &track.album,
            genre: &track.genre,
            path: &path_text,
            year: track.year,
            track_no: track.track_no,
            root_label: None,
        },
        None => TrackSearchText {
            tags: &no_tags,
            title: "",
            artist: "",
            album: "",
            genre: "",
            path: &path_text,
            year: None,
            track_no: None,
            root_label: None,
        },
    };
    text.fields()
}

/// The newest of the jobs waiting in `jobs`, if any.
pub(super) fn newer_job(jobs: &Receiver<QueueSearchJob>) -> Option<QueueSearchJob> {
    jobs.try_iter().last()
}

/// The rows of `job`'s queue its query finds. `prepared` is global
/// search's index of the job's library. Stops with a newer job from
/// `jobs`, which it returns.
pub(super) fn search_queue(
    job: &QueueSearchJob,
    prepared: Option<&PreparedSearchLibrary>,
    cache: &mut QueueSearchCache,
    jobs: &Receiver<QueueSearchJob>,
) -> Result<QueueSearchFrame, QueueSearchJob> {
    let mut frame = QueueSearchFrame {
        seq: job.seq,
        revision: job.revision,
        rows: Vec::new(),
    };
    let query = SearchQuery::parse(&job.query);
    // Every row is a track; a query for other types finds none.
    if split_search_terms(&job.query).is_empty() || !query.allows_type("track") {
        return Ok(frame);
    }
    cache.refresh(&job.library, &job.details);
    for (row, path) in job.paths.iter().enumerate() {
        if row % CANCEL_POLL_ROWS == 0 && row > 0 {
            if let Some(newer) = newer_job(jobs) {
                return Err(newer);
            }
        }
        let library_fields = prepared.and_then(|prepared| {
            cache
                .library_index(&job.library, path)
                .and_then(|index| prepared.fields_of(index, path))
        });
        let fields = match library_fields {
            Some(fields) => fields,
            None => cache
                .outside
                .entry(path.clone())
                .or_insert_with(|| outside_fields(path, job.details.get(path))),
        };
        if query.matches(fields) {
            frame.rows.push(u32::try_from(row).unwrap_or(u32::MAX));
        }
    }
    if let Some(newer) = newer_job(jobs) {
        return Err(newer);
    }
    Ok(frame)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::{LibraryRoot, LibraryTrack};

    fn library_track(path: &str, title: &str, artist: &str, album: &str) -> LibraryTrack {
        LibraryTrack {
            path: PathBuf::from(path),
            root_path: PathBuf::from("/music"),
            title: title.to_string(),
            artist: artist.to_string(),
            album: album.to_string(),
            ..LibraryTrack::default()
        }
    }

    fn library(tracks: Vec<LibraryTrack>) -> Arc<LibrarySnapshot> {
        Arc::new(LibrarySnapshot::new(
            vec![LibraryRoot {
                path: PathBuf::from("/music"),
                name: "Music".to_string(),
            }],
            tracks,
        ))
    }

    fn job(
        query: &str,
        paths: &[&str],
        library: &Arc<LibrarySnapshot>,
        details: HashMap<PathBuf, IndexedTrack>,
    ) -> QueueSearchJob {
        QueueSearchJob {
            seq: 1,
            query: query.to_string(),
            revision: 7,
            paths: paths.iter().map(PathBuf::from).collect(),
            details: Arc::new(details),
            library: Arc::clone(library),
        }
    }

    /// The rows `query` finds in the queue `paths`, as the worker searches
    /// it, with global search's index of `library`.
    fn found(
        query: &str,
        paths: &[&str],
        library: &Arc<LibrarySnapshot>,
        details: HashMap<PathBuf, IndexedTrack>,
    ) -> Vec<u32> {
        let prepared = super::super::search::prepared_library_for_tests(library);
        let (_tx, jobs) = crossbeam_channel::unbounded();
        search_queue(
            &job(query, paths, library, details),
            Some(&prepared),
            &mut QueueSearchCache::default(),
            &jobs,
        )
        .expect("no newer job")
        .rows
    }

    fn fixture_library() -> Arc<LibrarySnapshot> {
        let mut composed = library_track(
            "/music/Ensemble/Suite/01 Opening.flac",
            "Opening",
            "Ensemble",
            "Suite",
        );
        composed
            .search_tags
            .insert("albumartist", ["Chamber Orchestra"]);
        library(vec![
            composed,
            library_track(
                "/music/Ensemble/Suite/02 Night Café.flac",
                "Night Café",
                "Ensemble",
                "Suite",
            ),
            library_track(
                "/music/Other/Songs/01 Morning.flac",
                "Morning",
                "Other Band",
                "Songs",
            ),
        ])
    }

    const QUEUE: [&str; 4] = [
        "/music/Other/Songs/01 Morning.flac",
        "/music/Ensemble/Suite/01 Opening.flac",
        "/music/Ensemble/Suite/02 Night Café.flac",
        "/music/Other/Songs/01 Morning.flac",
    ];

    #[test]
    fn rows_match_with_global_search_syntax_in_queue_order() {
        let library = fixture_library();
        let none = HashMap::new;
        // Words may match different fields, and accents fold.
        assert_eq!(found("ensemble cafe", &QUEUE, &library, none()), vec![2]);
        // Fields and quoted phrases, the album artist from the tags.
        assert_eq!(
            found(
                "albumartist:\"chamber orchestra\"",
                &QUEUE,
                &library,
                none()
            ),
            vec![1]
        );
        assert_eq!(
            found("artist:ensemble", &QUEUE, &library, none()),
            vec![1, 2]
        );
        assert!(found("\"orchestra chamber\"", &QUEUE, &library, none()).is_empty());
        // The path within its root, and the root's label.
        assert_eq!(found("path:songs", &QUEUE, &library, none()), vec![0, 3]);
        assert_eq!(found("root:music", &QUEUE, &library, none()).len(), 4);
        // Only tracks are in the queue.
        assert_eq!(
            found("type:track suite", &QUEUE, &library, none()),
            vec![1, 2]
        );
        assert!(found("type:album suite", &QUEUE, &library, none()).is_empty());
        assert!(found("   ", &QUEUE, &library, none()).is_empty());
    }

    #[test]
    fn a_file_queued_twice_is_found_in_each_of_its_rows() {
        let library = fixture_library();
        assert_eq!(
            found("morning", &QUEUE, &library, HashMap::new()),
            vec![0, 3]
        );
    }

    #[test]
    fn rows_outside_the_library_match_their_details_and_file_name() {
        let library = fixture_library();
        let outside = "/home/someone/Downloads/Live Set/track 7.ogg";
        let mut details = HashMap::new();
        details.insert(
            PathBuf::from(outside),
            IndexedTrack {
                search_tags: SearchTags::default(),
                title: "Encore".to_string(),
                artist: "Visiting Act".to_string(),
                album: String::new(),
                cover_path: String::new(),
                cover_stamp: 0,
                cover_read_at: crate::library::CoverReadAt::default(),
                genre: String::new(),
                year: None,
                track_no: None,
                duration_secs: None,
            },
        );
        let queue = [QUEUE[0], outside];
        assert_eq!(
            found("encore visiting", &queue, &library, details.clone()),
            vec![1]
        );
        assert_eq!(
            found("path:\"live set\"", &queue, &library, details.clone()),
            vec![1]
        );
        // The folders above it match nothing, as above a library root.
        assert!(found("downloads", &queue, &library, details.clone()).is_empty());
        // Without details, the file's name still matches.
        assert_eq!(found("track 7", &queue, &library, HashMap::new()), vec![1]);
    }

    #[test]
    fn a_newer_query_stops_the_search_and_takes_its_place() {
        let library = fixture_library();
        let paths: Vec<&str> = std::iter::repeat_n(QUEUE[0], CANCEL_POLL_ROWS * 3).collect();
        let prepared = super::super::search::prepared_library_for_tests(&library);
        let (tx, jobs) = crossbeam_channel::unbounded();
        tx.send(job("newer", &paths, &library, HashMap::new()))
            .expect("send");
        let stopped = search_queue(
            &job("morning", &paths, &library, HashMap::new()),
            Some(&prepared),
            &mut QueueSearchCache::default(),
            &jobs,
        )
        .expect_err("the newer query takes its place");
        assert_eq!(stopped.query, "newer");
    }

    #[test]
    fn the_queue_is_copied_once_per_revision_and_names_it() {
        let mut state = BridgeState {
            queue: QUEUE.iter().map(PathBuf::from).collect(),
            ..BridgeState::default()
        };
        let mut requests = QueueSearchRequests::default();
        let first = requests.job(&state, 1, "morning");
        let second = requests.job(&state, 2, " morning ");
        assert!(Arc::ptr_eq(&first.paths, &second.paths));
        assert_eq!(second.query, "morning");
        assert_eq!(first.revision, state.queue_sync.revision());

        state.queue.pop();
        state
            .queue_sync
            .record(super::super::QueueEdit::Remove(vec![3]), state.queue.len());
        let third = requests.job(&state, 3, "morning");
        assert_eq!(third.revision, first.revision + 1);
        assert_eq!(third.paths.len(), 3);
        // An empty query needs no copy.
        assert!(requests.job(&state, 4, "").paths.is_empty());
    }
}
