// SPDX-License-Identifier: GPL-3.0-or-later

//! How the library worker publishes its library: the rows that changed
//! since its previous event, never a copy of the whole library, and nothing
//! at all when neither the rows nor the status changed.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crossbeam_channel::Sender;
use rusqlite::Connection;

use super::schema::load_snapshot;
use super::{LibraryEvent, LibraryRoot, LibrarySnapshot, LibraryStatus, LibraryTrack};

/// Changes to the library's roots and tracks. A path is either upserted or
/// removed, never both.
#[derive(Debug, Clone, Default)]
pub struct LibraryChanges {
    /// The roots, when they changed.
    pub roots: Option<Vec<LibraryRoot>>,
    /// Tracks added or changed.
    pub upserted: Vec<Arc<LibraryTrack>>,
    /// Paths whose tracks were removed.
    pub removed: Vec<PathBuf>,
}

impl LibraryChanges {
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.roots.is_none() && !self.has_rows()
    }

    /// Whether any track changed.
    #[must_use]
    pub fn has_rows(&self) -> bool {
        !self.upserted.is_empty() || !self.removed.is_empty()
    }

    /// Adds `newer`, which happened after these changes, so that applying
    /// the result is applying both in order.
    pub fn merge(&mut self, mut newer: Self) {
        if let Some(roots) = newer.roots.take() {
            self.roots = Some(roots);
        }
        if !newer.has_rows() {
            return;
        }
        if !self.has_rows() {
            self.upserted = newer.upserted;
            self.removed = newer.removed;
            return;
        }
        let removed: HashSet<&Path> = newer.removed.iter().map(PathBuf::as_path).collect();
        let mut upserted_at: HashMap<PathBuf, usize> = HashMap::new();
        self.upserted
            .retain(|track| !removed.contains(track.path.as_path()));
        for (index, track) in self.upserted.iter().enumerate() {
            upserted_at.insert(track.path.clone(), index);
        }
        let mut readded = HashSet::new();
        for track in newer.upserted {
            readded.insert(track.path.clone());
            if let Some(&index) = upserted_at.get(&track.path) {
                self.upserted[index] = track;
            } else {
                upserted_at.insert(track.path.clone(), self.upserted.len());
                self.upserted.push(track);
            }
        }
        self.removed.retain(|path| !readded.contains(path));
        let known: HashSet<PathBuf> = self.removed.iter().cloned().collect();
        self.removed.extend(
            newer
                .removed
                .into_iter()
                .filter(|path| !known.contains(path)),
        );
    }
}

/// What [`LibrarySnapshot::apply_changes`] did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AppliedChanges {
    /// Whether the roots or any track changed.
    pub changed: bool,
    /// Whether every track that was kept is where it was, so that an index
    /// of positions only needs the tracks appended from `appended_from`.
    pub positions_kept: bool,
    pub appended_from: usize,
}

impl LibrarySnapshot {
    /// Applies `changes`. `positions` returns the positions of the tracks at
    /// a path as they were before the changes. An upserted track replaces
    /// the first track at its path, and the others at that path are removed,
    /// so a path that a copy got twice by applying changes out of order is
    /// left once.
    pub fn apply_changes(
        &mut self,
        changes: &LibraryChanges,
        positions: impl Fn(&Path) -> Vec<usize>,
    ) -> AppliedChanges {
        let mut any_change = false;
        if let Some(roots) = &changes.roots {
            if self.roots != *roots {
                self.roots.clone_from(roots);
                any_change = true;
            }
        }
        let mut dropped = Vec::new();
        let mut appended = Vec::new();
        for track in &changes.upserted {
            let found = positions(&track.path);
            match found.split_first() {
                Some((&first, rest)) => {
                    let existing = &mut self.tracks[first];
                    if !Arc::ptr_eq(existing, track) && **existing != **track {
                        *existing = Arc::clone(track);
                        any_change = true;
                    }
                    dropped.extend_from_slice(rest);
                }
                None => appended.push(Arc::clone(track)),
            }
        }
        for path in &changes.removed {
            dropped.extend(positions(path));
        }
        let positions_kept = dropped.is_empty();
        if !positions_kept {
            dropped.sort_unstable();
            dropped.dedup();
            let mut dropped = dropped.into_iter().peekable();
            let mut index = 0;
            self.tracks.retain(|_| {
                let keep = dropped.peek() != Some(&index);
                if !keep {
                    dropped.next();
                }
                index += 1;
                keep
            });
            any_change = true;
        }
        let appended_from = self.tracks.len();
        if !appended.is_empty() {
            self.tracks.extend(appended);
            any_change = true;
        }
        if any_change {
            self.bump_search_revision();
        }
        AppliedChanges {
            changed: any_change,
            positions_kept,
            appended_from,
        }
    }
}

/// The paths changed since the worker's previous event.
#[derive(Debug, Default)]
struct Pending {
    roots: bool,
    /// In the order they changed; an entry is current only while it is in
    /// `upserted_set`.
    upserted: Vec<PathBuf>,
    upserted_set: HashSet<PathBuf>,
    removed: HashSet<PathBuf>,
}

impl Pending {
    fn upserted(&mut self, path: &Path) {
        self.removed.remove(path);
        if self.upserted_set.insert(path.to_path_buf()) {
            self.upserted.push(path.to_path_buf());
        }
    }

    fn removed(&mut self, path: &Path) {
        self.upserted_set.remove(path);
        self.removed.insert(path.to_path_buf());
    }
}

/// The library worker's copy of the library, which matches the database,
/// and the changes it has yet to publish.
#[derive(Debug, Default)]
pub(super) struct WorkerLibrary {
    snapshot: LibrarySnapshot,
    /// The position of each track in `snapshot`; paths are unique.
    positions: HashMap<PathBuf, usize>,
    pub(super) status: LibraryStatus,
    pending: Pending,
    /// The status of the previous event, if there was one.
    published_status: Option<LibraryStatus>,
}

impl WorkerLibrary {
    /// The paths of the tracks, sorted.
    #[cfg(test)]
    pub(super) fn paths(&self) -> Vec<PathBuf> {
        let mut paths: Vec<PathBuf> = self.positions.keys().cloned().collect();
        paths.sort();
        paths
    }

    #[cfg(test)]
    pub(super) fn tracks(&self) -> &[Arc<LibraryTrack>] {
        &self.snapshot.tracks
    }

    pub(super) fn roots(&self) -> &[LibraryRoot] {
        &self.snapshot.roots
    }

    pub(super) fn set_roots(&mut self, roots: Vec<LibraryRoot>) {
        if self.snapshot.roots != roots {
            self.snapshot.roots = roots;
            self.snapshot.bump_search_revision();
            self.pending.roots = true;
        }
    }

    /// Adds or replaces the track at `track.path`, if it differs.
    pub(super) fn upsert(&mut self, track: LibraryTrack) {
        self.upsert_shared(Arc::new(track), false);
    }

    /// Adds or replaces the track at `track.path`. A `forced` track is
    /// published even if the worker already has it.
    fn upsert_shared(&mut self, track: Arc<LibraryTrack>, forced: bool) {
        if let Some(&index) = self.positions.get(&track.path) {
            let existing = &mut self.snapshot.tracks[index];
            if **existing != *track {
                *existing = track;
            } else if !forced {
                return;
            }
            let path = existing.path.clone();
            self.pending.upserted(&path);
        } else {
            self.positions
                .insert(track.path.clone(), self.snapshot.tracks.len());
            self.pending.upserted(&track.path);
            self.snapshot.tracks.push(track);
        }
        self.snapshot.bump_search_revision();
    }

    /// Changes the track at `path`, if there is one.
    pub(super) fn modify(&mut self, path: &Path, change: impl FnOnce(&mut LibraryTrack)) {
        let Some(&index) = self.positions.get(path) else {
            return;
        };
        let mut track = (*self.snapshot.tracks[index]).clone();
        change(&mut track);
        self.upsert(track);
    }

    /// Removes the tracks for which `remove` is true.
    pub(super) fn remove_where(&mut self, mut remove: impl FnMut(&LibraryTrack) -> bool) {
        let mut removed = Vec::new();
        self.snapshot.tracks.retain(|track| {
            let gone = remove(track);
            if gone {
                removed.push(track.path.clone());
            }
            !gone
        });
        if removed.is_empty() {
            return;
        }
        for path in &removed {
            self.pending.removed(path);
        }
        self.reindex();
        self.snapshot.bump_search_revision();
    }

    fn reindex(&mut self) {
        self.positions = self
            .snapshot
            .tracks
            .iter()
            .enumerate()
            .map(|(index, track)| (track.path.clone(), index))
            .collect();
    }

    /// Reads the library from the database, and keeps the changes as the
    /// rows that differ from the worker's copy.
    pub(super) fn reload(&mut self, conn: &Connection) {
        let mut fresh = LibrarySnapshot::default();
        load_snapshot(conn, &mut fresh);
        self.set_roots(fresh.roots);
        let present: HashSet<PathBuf> = fresh
            .tracks
            .iter()
            .map(|track| track.path.clone())
            .collect();
        self.remove_where(|track| !present.contains(&track.path));
        for track in fresh.tracks {
            self.upsert_shared(track, false);
        }
    }

    /// Reads the rows of `paths` from the database and publishes each,
    /// present or removed, even if the worker's copy already matched it.
    pub(super) fn read_rows(&mut self, conn: &Connection, paths: &[PathBuf]) -> Result<(), String> {
        let rows = super::schema::load_tracks(conn, paths)?;
        let found: HashSet<PathBuf> = rows.iter().map(|track| track.path.clone()).collect();
        for path in paths {
            if !found.contains(path) {
                if self.positions.contains_key(path) {
                    self.remove_where(|track| track.path == *path);
                } else {
                    self.pending.removed(path);
                }
            }
        }
        for track in rows {
            self.upsert_shared(Arc::new(track), true);
        }
        Ok(())
    }

    fn take_changes(&mut self) -> LibraryChanges {
        let pending = std::mem::take(&mut self.pending);
        let mut upserted_set = pending.upserted_set;
        let upserted = pending
            .upserted
            .into_iter()
            .filter(|path| upserted_set.remove(path))
            .filter_map(|path| {
                self.positions
                    .get(&path)
                    .map(|&index| Arc::clone(&self.snapshot.tracks[index]))
            })
            .collect();
        let mut removed: Vec<PathBuf> = pending.removed.into_iter().collect();
        removed.sort_unstable();
        LibraryChanges {
            roots: pending.roots.then(|| self.snapshot.roots.clone()),
            upserted,
            removed,
        }
    }

    /// Sends the changes since the previous event with the status, unless
    /// there are none and the status is the same.
    pub(super) fn publish(&mut self, event_tx: &Sender<LibraryEvent>) {
        let changes = self.take_changes();
        if changes.is_empty() && self.published_status.as_ref() == Some(&self.status) {
            return;
        }
        self.published_status = Some(self.status.clone());
        let _ = event_tx.send(LibraryEvent {
            changes,
            status: self.status.clone(),
        });
    }
}

/// The library as a receiver of the worker's events has it, for tests.
#[cfg(test)]
#[derive(Debug, Clone, Default)]
pub(crate) struct LibraryMirror {
    pub(crate) snapshot: LibrarySnapshot,
    pub(crate) status: LibraryStatus,
    /// How many events were applied, and how many tracks they carried.
    pub(crate) events: usize,
    pub(crate) rows_received: usize,
}

#[cfg(test)]
impl LibraryMirror {
    pub(crate) fn apply(&mut self, event: &LibraryEvent) {
        let mut positions: HashMap<PathBuf, Vec<usize>> = HashMap::new();
        for (index, track) in self.snapshot.tracks.iter().enumerate() {
            positions.entry(track.path.clone()).or_default().push(index);
        }
        self.snapshot.apply_changes(&event.changes, |path| {
            positions.get(path).cloned().unwrap_or_default()
        });
        self.status = event.status.clone();
        self.events += 1;
        self.rows_received += event.changes.upserted.len();
    }

    /// Applies events until the library satisfies `wanted`, waiting up to
    /// 20 s.
    pub(crate) fn wait_for(
        &mut self,
        events: &crossbeam_channel::Receiver<LibraryEvent>,
        mut wanted: impl FnMut(&Self) -> bool,
    ) -> &Self {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
        loop {
            match events.recv_deadline(deadline) {
                Ok(event) => {
                    self.apply(&event);
                    if wanted(self) {
                        return self;
                    }
                }
                Err(error) => panic!("the library never got there: {error}"),
            }
        }
    }

    /// Applies the events already sent.
    pub(crate) fn drain(&mut self, events: &crossbeam_channel::Receiver<LibraryEvent>) {
        while let Ok(event) = events.try_recv() {
            self.apply(&event);
        }
    }

    pub(crate) fn idle_with_tracks(&self, count: usize) -> bool {
        !self.status.scan_in_progress && self.snapshot.tracks.len() == count
    }

    pub(crate) fn paths(&self) -> Vec<PathBuf> {
        let mut paths: Vec<PathBuf> = self
            .snapshot
            .tracks
            .iter()
            .map(|track| track.path.clone())
            .collect();
        paths.sort();
        paths
    }
}

#[cfg(test)]
mod tests;
