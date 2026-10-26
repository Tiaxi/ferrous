// SPDX-License-Identifier: GPL-3.0-or-later

//! Queue revisions: the edits the UI has not been sent yet, and the
//! remapping of commands the UI issued against an older queue.
//!
//! Every change to the queue's rows advances the revision. Snapshots carry
//! the edits since the previous snapshot, or the whole queue when that is
//! smaller or the UI needs it. The UI sends index-based commands with the
//! revision its rows show; [`QueueSync::resolve`] carries them across the
//! edits made since, so they act on the rows the user saw.

use std::collections::VecDeque;
use std::path::PathBuf;

use super::BridgeQueueCommand;

/// A change to the queue's rows, in the order the bridge made them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QueueEdit {
    /// `paths` were inserted before row `at`.
    Insert { at: usize, paths: Vec<PathBuf> },
    /// The rows at these indices, ascending and distinct, were removed.
    Remove(Vec<usize>),
    /// The row at `from` was taken out and put back at `to`.
    Move { from: usize, to: usize },
    /// These rows hold new paths, such as after a rename.
    SetPaths(Vec<(usize, PathBuf)>),
    /// The details shown for any row may have changed.
    Refresh,
}

impl QueueEdit {
    /// Apply the edit to `paths`. Returns false, leaving `paths` as it was,
    /// if the edit does not fit them.
    #[cfg(test)]
    pub(crate) fn apply_to(&self, paths: &mut Vec<PathBuf>) -> bool {
        match self {
            Self::Insert {
                at,
                paths: inserted,
            } => {
                if *at > paths.len() {
                    return false;
                }
                paths.splice(*at..*at, inserted.iter().cloned());
            }
            Self::Remove(indices) => {
                if indices.last().is_some_and(|last| *last >= paths.len()) {
                    return false;
                }
                for index in indices.iter().rev() {
                    paths.remove(*index);
                }
            }
            Self::Move { from, to } => {
                if *from >= paths.len() || *to >= paths.len() {
                    return false;
                }
                let path = paths.remove(*from);
                paths.insert(*to, path);
            }
            Self::SetPaths(renamed) => {
                if renamed.iter().any(|(index, _)| *index >= paths.len()) {
                    return false;
                }
                for (index, path) in renamed {
                    paths[*index].clone_from(path);
                }
            }
            Self::Refresh => {}
        }
        true
    }

    /// How many rows the edit sends to the UI, to weigh it against the
    /// whole queue.
    fn weight(&self) -> usize {
        match self {
            Self::Insert { paths, .. } => paths.len(),
            Self::Remove(indices) => indices.len(),
            Self::SetPaths(renamed) => renamed.len(),
            Self::Move { .. } | Self::Refresh => 1,
        }
    }
}

/// The queue changes a snapshot carries.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QueueUpdate {
    /// The whole queue.
    Full(Vec<PathBuf>),
    /// The edits made since `base_revision`, the revision of the update
    /// sent before this one.
    Edits {
        base_revision: u64,
        edits: Vec<QueueEdit>,
    },
}

impl QueueUpdate {
    /// Apply the update to `paths`, the queue at the previous update.
    /// Returns false if it does not fit them.
    #[cfg(test)]
    pub(crate) fn apply_to(&self, paths: &mut Vec<PathBuf>) -> bool {
        match self {
            Self::Full(queue) => {
                paths.clone_from(queue);
                true
            }
            Self::Edits { edits, .. } => edits.iter().all(|edit| edit.apply_to(paths)),
        }
    }
}

/// Below this many rows, edits always travel as edits: the whole queue
/// would not be much smaller.
const MIN_FULL_FALLBACK_ROWS: usize = 256;

/// The edits the journal keeps for remapping stale commands, and the
/// removed indices they may hold in all. A command older than the journal
/// is refused.
const MAX_JOURNAL_EDITS: usize = 1024;
const MAX_JOURNAL_INDICES: usize = 1 << 16;

/// Shown when the UI sends a command against a queue too old to remap.
pub(super) const STALE_QUEUE_COMMAND_MESSAGE: &str =
    "The queue changed before your edit reached it, so nothing was changed. Try again.";
const PLAY_REMOVED_ROW_MESSAGE: &str = "The track you chose to play is no longer in the queue.";
const MOVE_REMOVED_ROW_MESSAGE: &str =
    "The queue changed before the track could be moved, so it was not moved.";

#[derive(Debug, Clone, PartialEq, Eq)]
enum Pending {
    None,
    Full,
    Edits {
        edits: Vec<QueueEdit>,
        weight: usize,
    },
}

/// How a structural edit moves rows, for remapping stale indices.
#[derive(Debug, Clone)]
enum RowMap {
    Insert {
        at: usize,
        count: usize,
    },
    Remove(Vec<usize>),
    Move {
        from: usize,
        to: usize,
    },
    /// Every row was replaced.
    Reset,
}

impl RowMap {
    /// Where the row at `index` before the edit is after it, if it is
    /// still in the queue.
    fn map(&self, index: usize) -> Option<usize> {
        match self {
            Self::Insert { at, count } => Some(if index >= *at { index + count } else { index }),
            Self::Remove(indices) => match indices.binary_search(&index) {
                Ok(_) => None,
                Err(removed_before) => Some(index - removed_before),
            },
            Self::Move { from, to } => Some(if index == *from {
                *to
            } else if *from < index && index <= *to {
                index - 1
            } else if *to <= index && index < *from {
                index + 1
            } else {
                index
            }),
            Self::Reset => None,
        }
    }

    fn indices(&self) -> usize {
        match self {
            Self::Remove(indices) => indices.len(),
            _ => 1,
        }
    }
}

/// The queue's revision, the changes not yet sent to the UI, and the
/// journal of recent structural edits.
#[derive(Debug, Clone)]
pub(super) struct QueueSync {
    revision: u64,
    sent_revision: u64,
    pending: Pending,
    /// Structural edits with the revision each produced, oldest first.
    journal: VecDeque<(u64, RowMap)>,
    journal_indices: usize,
    /// Commands issued at a revision below this predate the journal.
    journal_floor: u64,
}

impl Default for QueueSync {
    /// The UI has no queue yet: the first snapshot sends it whole. Its
    /// revision is 1, so the UI's 0 means it has none.
    fn default() -> Self {
        Self {
            revision: 1,
            sent_revision: 0,
            pending: Pending::Full,
            journal: VecDeque::new(),
            journal_indices: 0,
            journal_floor: 1,
        }
    }
}

impl QueueSync {
    pub(super) fn revision(&self) -> u64 {
        self.revision
    }

    /// Note `edit`, which left `queue_len` rows in the queue.
    pub(super) fn record(&mut self, edit: QueueEdit, queue_len: usize) {
        self.revision += 1;
        let row_map = match &edit {
            QueueEdit::Insert { at, paths } => Some(RowMap::Insert {
                at: *at,
                count: paths.len(),
            }),
            QueueEdit::Remove(indices) => Some(RowMap::Remove(indices.clone())),
            QueueEdit::Move { from, to } => Some(RowMap::Move {
                from: *from,
                to: *to,
            }),
            QueueEdit::SetPaths(_) | QueueEdit::Refresh => None,
        };
        if let Some(row_map) = row_map {
            self.push_journal(row_map);
        }
        match &mut self.pending {
            Pending::Full => {}
            Pending::None => {
                let weight = edit.weight();
                self.pending = Pending::Edits {
                    edits: vec![edit],
                    weight,
                };
            }
            Pending::Edits { edits, weight } => {
                // One refresh covers any number in a row.
                if edit == QueueEdit::Refresh && edits.last() == Some(&QueueEdit::Refresh) {
                    return;
                }
                *weight += edit.weight();
                edits.push(edit);
                if *weight > queue_len.max(MIN_FULL_FALLBACK_ROWS) {
                    self.pending = Pending::Full;
                }
            }
        }
    }

    /// Note that every row was replaced.
    pub(super) fn reset(&mut self) {
        self.revision += 1;
        // No command can be carried across a reset, so older edits are of
        // no further use.
        self.journal.clear();
        self.journal_indices = 0;
        self.push_journal(RowMap::Reset);
        self.pending = Pending::Full;
    }

    /// Send the whole queue with the next snapshot, for a UI that lost
    /// track of it.
    pub(super) fn request_full(&mut self) {
        self.pending = Pending::Full;
    }

    #[cfg(test)]
    pub(super) fn has_pending(&self) -> bool {
        self.pending != Pending::None
    }

    /// The update the next snapshot carries, if the UI's queue is behind.
    pub(super) fn update(&self, queue: &[PathBuf]) -> Option<QueueUpdate> {
        match &self.pending {
            Pending::None => None,
            Pending::Full => Some(QueueUpdate::Full(queue.to_vec())),
            Pending::Edits { edits, .. } => Some(QueueUpdate::Edits {
                base_revision: self.sent_revision,
                edits: edits.clone(),
            }),
        }
    }

    /// Note that a snapshot with [`Self::update`] reached the relay.
    pub(super) fn mark_sent(&mut self) {
        self.sent_revision = self.revision;
        self.pending = Pending::None;
    }

    fn push_journal(&mut self, row_map: RowMap) {
        self.journal_indices += row_map.indices();
        self.journal.push_back((self.revision, row_map));
        while self.journal.len() > MAX_JOURNAL_EDITS || self.journal_indices > MAX_JOURNAL_INDICES {
            let Some((revision, dropped)) = self.journal.pop_front() else {
                break;
            };
            self.journal_indices -= dropped.indices();
            self.journal_floor = self.journal_floor.max(revision);
        }
    }

    /// Where the row at `index` at `revision` is now, if it is still in
    /// the queue and the journal reaches back to `revision`.
    pub(super) fn follow(&self, revision: u64, index: usize) -> Option<usize> {
        if revision > self.revision || revision < self.journal_floor {
            return None;
        }
        self.map_index(revision, index)
    }

    /// Where the row at `index` at `revision` is now, if it is still in
    /// the queue.
    fn map_index(&self, revision: u64, index: usize) -> Option<usize> {
        self.journal
            .iter()
            .filter(|(edit_revision, _)| *edit_revision > revision)
            .try_fold(index, |index, (_, row_map)| row_map.map(index))
    }

    /// The command `command`, issued against the queue at `revision`, as
    /// it applies to the queue now. `Ok(None)` drops it without a word: its
    /// rows are gone, which is what removing them wanted, or it only
    /// selected a row. `Err` holds what to tell the user.
    pub(super) fn resolve(
        &self,
        revision: u64,
        command: BridgeQueueCommand,
    ) -> Result<Option<BridgeQueueCommand>, String> {
        if revision == self.revision {
            return Ok(Some(command));
        }
        if revision > self.revision || revision < self.journal_floor {
            return match command {
                BridgeQueueCommand::Select(_) => Ok(None),
                _ => Err(STALE_QUEUE_COMMAND_MESSAGE.to_string()),
            };
        }
        let map = |index| self.map_index(revision, index);
        Ok(Some(match command {
            BridgeQueueCommand::PlayAt(index) => {
                BridgeQueueCommand::PlayAt(map(index).ok_or(PLAY_REMOVED_ROW_MESSAGE)?)
            }
            BridgeQueueCommand::Select(Some(index)) => match map(index) {
                Some(index) => BridgeQueueCommand::Select(Some(index)),
                None => return Ok(None),
            },
            BridgeQueueCommand::Remove(index) => match map(index) {
                Some(index) => BridgeQueueCommand::Remove(index),
                None => return Ok(None),
            },
            BridgeQueueCommand::RemoveMany(indices) => {
                let indices: Vec<usize> = indices.into_iter().filter_map(map).collect();
                if indices.is_empty() {
                    return Ok(None);
                }
                BridgeQueueCommand::RemoveMany(indices)
            }
            BridgeQueueCommand::Move { from, to } => {
                let Some(moved_to) = self.map_move(revision, from, to) else {
                    return Err(MOVE_REMOVED_ROW_MESSAGE.to_string());
                };
                if moved_to.0 == moved_to.1 {
                    return Ok(None);
                }
                BridgeQueueCommand::Move {
                    from: moved_to.0,
                    to: moved_to.1,
                }
            }
            other => other,
        }))
    }

    /// A move of the row at `from` to `to`, issued at `revision`, as a move
    /// in the queue now. The row lands next to the row that was at `to`:
    /// after it when it moved down, before it when it moved up.
    fn map_move(&self, revision: u64, from: usize, to: usize) -> Option<(usize, usize)> {
        let new_from = self.map_index(revision, from)?;
        let anchor = self.map_index(revision, to)?;
        let after_anchor = from < to;
        let new_to = match (after_anchor, new_from < anchor) {
            (true, true) | (false, false) => anchor,
            (true, false) => anchor + 1,
            (false, true) => anchor - 1,
        };
        Some((new_from, new_to))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    fn sent() -> QueueSync {
        let mut sync = QueueSync::default();
        sync.mark_sent();
        sync
    }

    #[test]
    fn the_first_update_is_the_whole_queue_at_revision_one() {
        let mut sync = QueueSync::default();
        assert_eq!(sync.revision(), 1);
        let queue = vec![p("/a.flac")];
        assert_eq!(sync.update(&queue), Some(QueueUpdate::Full(queue)));
        sync.mark_sent();
        assert!(!sync.has_pending());
        assert_eq!(sync.update(&[]), None);
    }

    #[test]
    fn each_edit_advances_the_revision_and_travels_since_the_last_update() {
        let mut sync = sent();
        let insert = QueueEdit::Insert {
            at: 0,
            paths: vec![p("/a.flac"), p("/b.flac")],
        };
        sync.record(insert.clone(), 2);
        sync.record(QueueEdit::Move { from: 0, to: 1 }, 2);
        assert_eq!(sync.revision(), 3);
        assert_eq!(
            sync.update(&[]),
            Some(QueueUpdate::Edits {
                base_revision: 1,
                edits: vec![insert, QueueEdit::Move { from: 0, to: 1 }],
            })
        );
        sync.mark_sent();
        sync.record(QueueEdit::Remove(vec![1]), 1);
        assert_eq!(
            sync.update(&[]),
            Some(QueueUpdate::Edits {
                base_revision: 3,
                edits: vec![QueueEdit::Remove(vec![1])],
            })
        );
    }

    #[test]
    fn refreshes_in_a_row_travel_once() {
        let mut sync = sent();
        sync.record(QueueEdit::Refresh, 3);
        sync.record(QueueEdit::Refresh, 3);
        assert_eq!(sync.revision(), 3);
        assert_eq!(
            sync.update(&[]),
            Some(QueueUpdate::Edits {
                base_revision: 1,
                edits: vec![QueueEdit::Refresh],
            })
        );
    }

    #[test]
    fn edits_larger_than_the_queue_fall_back_to_the_whole_queue() {
        let mut sync = sent();
        let queue: Vec<PathBuf> = (0..1000).map(|i| p(&format!("/{i}.flac"))).collect();
        sync.record(
            QueueEdit::Insert {
                at: 0,
                paths: queue.clone(),
            },
            1000,
        );
        // One edit as large as the queue still travels as an edit.
        assert!(matches!(
            sync.update(&queue),
            Some(QueueUpdate::Edits { .. })
        ));
        sync.record(QueueEdit::Remove((0..999).collect()), 1);
        assert_eq!(
            sync.update(&queue[999..]),
            Some(QueueUpdate::Full(queue[999..].to_vec()))
        );
        // A reset always sends the whole queue.
        sync.mark_sent();
        sync.reset();
        assert_eq!(sync.update(&[]), Some(QueueUpdate::Full(Vec::new())));
    }

    #[test]
    fn a_full_resync_keeps_the_revision() {
        let mut sync = sent();
        sync.request_full();
        assert_eq!(sync.revision(), 1);
        assert_eq!(
            sync.update(&[p("/a.flac")]),
            Some(QueueUpdate::Full(vec![p("/a.flac")]))
        );
    }

    #[test]
    fn edits_apply_to_paths_and_refuse_ones_that_do_not_fit() {
        let mut paths = vec![p("/a"), p("/b"), p("/c"), p("/d")];
        assert!(QueueEdit::Remove(vec![0, 2]).apply_to(&mut paths));
        assert_eq!(paths, vec![p("/b"), p("/d")]);
        assert!(QueueEdit::Insert {
            at: 1,
            paths: vec![p("/x")]
        }
        .apply_to(&mut paths));
        assert_eq!(paths, vec![p("/b"), p("/x"), p("/d")]);
        assert!(QueueEdit::Move { from: 2, to: 0 }.apply_to(&mut paths));
        assert_eq!(paths, vec![p("/d"), p("/b"), p("/x")]);
        assert!(QueueEdit::SetPaths(vec![(1, p("/y"))]).apply_to(&mut paths));
        assert_eq!(paths, vec![p("/d"), p("/y"), p("/x")]);
        for edit in [
            QueueEdit::Remove(vec![3]),
            QueueEdit::Insert {
                at: 4,
                paths: vec![p("/z")],
            },
            QueueEdit::Move { from: 0, to: 3 },
            QueueEdit::SetPaths(vec![(3, p("/z"))]),
        ] {
            assert!(!edit.apply_to(&mut paths), "{edit:?}");
            assert_eq!(paths, vec![p("/d"), p("/y"), p("/x")]);
        }
    }

    #[test]
    fn current_commands_pass_unchanged() {
        let sync = sent();
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::Remove(0)),
            Ok(Some(BridgeQueueCommand::Remove(0)))
        ));
    }

    #[test]
    fn stale_indices_follow_their_rows() {
        // [a, b, c, d] at revision 1.
        let mut sync = sent();
        // An import appends; another inserts at the top; d moves first.
        sync.record(
            QueueEdit::Insert {
                at: 4,
                paths: vec![p("/e")],
            },
            5,
        );
        sync.record(
            QueueEdit::Insert {
                at: 0,
                paths: vec![p("/x")],
            },
            6,
        );
        // [x, a, b, c, d, e] → [d, x, a, b, c, e]
        sync.record(QueueEdit::Move { from: 4, to: 0 }, 6);
        // Removing b: [d, x, a, c, e]
        sync.record(QueueEdit::Remove(vec![3]), 5);

        let resolve = |command| sync.resolve(1, command);
        assert!(matches!(
            resolve(BridgeQueueCommand::PlayAt(1)),
            Err(message) if message == PLAY_REMOVED_ROW_MESSAGE
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::PlayAt(3)),
            Ok(Some(BridgeQueueCommand::PlayAt(0)))
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::Select(Some(0))),
            Ok(Some(BridgeQueueCommand::Select(Some(2))))
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::Select(Some(1))),
            Ok(None)
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::Select(None)),
            Ok(Some(BridgeQueueCommand::Select(None)))
        ));
        assert!(matches!(resolve(BridgeQueueCommand::Remove(1)), Ok(None)));
        assert!(matches!(
            resolve(BridgeQueueCommand::RemoveMany(vec![3, 1, 2])),
            Ok(Some(BridgeQueueCommand::RemoveMany(indices))) if indices == vec![0, 3]
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::RemoveMany(vec![1])),
            Ok(None)
        ));
        // c up before a, which is at 2 now: [d, x, c, a, e].
        assert!(matches!(
            resolve(BridgeQueueCommand::Move { from: 2, to: 0 }),
            Ok(Some(BridgeQueueCommand::Move { from: 3, to: 2 }))
        ));
        // a down after d, which moved above it: [d, a, x, c, e].
        assert!(matches!(
            resolve(BridgeQueueCommand::Move { from: 0, to: 3 }),
            Ok(Some(BridgeQueueCommand::Move { from: 2, to: 1 }))
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::Move { from: 1, to: 0 }),
            Err(message) if message == MOVE_REMOVED_ROW_MESSAGE
        ));
        assert!(matches!(
            resolve(BridgeQueueCommand::Clear),
            Ok(Some(BridgeQueueCommand::Clear))
        ));
    }

    #[test]
    fn a_replaced_queue_drops_or_refuses_older_commands() {
        let mut sync = sent();
        sync.reset();
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::Remove(0)),
            Ok(None)
        ));
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::RemoveMany(vec![0, 1])),
            Ok(None)
        ));
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::Select(Some(0))),
            Ok(None)
        ));
        assert!(sync.resolve(1, BridgeQueueCommand::PlayAt(0)).is_err());
        assert!(sync
            .resolve(1, BridgeQueueCommand::Move { from: 0, to: 1 })
            .is_err());
        assert!(matches!(
            sync.resolve(2, BridgeQueueCommand::Remove(0)),
            Ok(Some(BridgeQueueCommand::Remove(0)))
        ));
    }

    #[test]
    fn commands_older_than_the_journal_are_refused() {
        let mut sync = sent();
        for _ in 0..=MAX_JOURNAL_EDITS {
            sync.record(QueueEdit::Move { from: 0, to: 1 }, 2);
        }
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::Remove(0)),
            Err(message) if message == STALE_QUEUE_COMMAND_MESSAGE
        ));
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::Select(Some(0))),
            Ok(None)
        ));
        let recent = sync.revision() - 1;
        assert!(matches!(
            sync.resolve(recent, BridgeQueueCommand::Remove(0)),
            Ok(Some(BridgeQueueCommand::Remove(1)))
        ));
        // A revision the bridge never had is refused too.
        assert!(sync
            .resolve(sync.revision() + 1, BridgeQueueCommand::Remove(0))
            .is_err());
    }

    #[test]
    fn refreshes_and_renames_keep_indices() {
        let mut sync = sent();
        sync.record(QueueEdit::Refresh, 2);
        sync.record(QueueEdit::SetPaths(vec![(1, p("/renamed"))]), 2);
        assert!(matches!(
            sync.resolve(1, BridgeQueueCommand::PlayAt(1)),
            Ok(Some(BridgeQueueCommand::PlayAt(1)))
        ));
    }
}
