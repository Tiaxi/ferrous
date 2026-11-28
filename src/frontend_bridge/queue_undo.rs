// SPDX-License-Identifier: GPL-3.0-or-later

//! The history of queue changes, which Undo walks back and Redo walks
//! forward again.
//!
//! Every change the user makes to the queue's rows is a step here: adding
//! or inserting rows, removing rows, moving rows, clearing the queue and
//! replacing it, also when it was empty. A new change goes on top of the
//! steps to undo and drops the steps to redo. An undo takes the top step,
//! applies its inverse, and leaves the change to redo on top of the steps
//! to redo; a redo makes it again and leaves its undo on top of the steps
//! to undo. Each step has its own ID, and the UI offers only the top step
//! each way: an undo or a redo with another ID is refused, so it never
//! applies to a queue the user did not see. Marking rows to play next is
//! no step of its own; an undo brings back the marks of the rows it
//! restores. Renames keep the steps, with the new paths. The history lives
//! in memory only, and drops its oldest steps past [`MAX_STEPS`] steps or
//! [`MAX_PATHS`] stored paths.

use std::collections::VecDeque;
use std::path::PathBuf;

use crate::playback::{remap_path, PathRenames};

/// Shown when the UI asks to undo a change that is no longer offered.
pub(super) const STALE_UNDO_MESSAGE: &str =
    "Nothing was undone: the queue changed after that change.";

/// Shown when the UI asks to redo a change that is no longer offered.
pub(super) const STALE_REDO_MESSAGE: &str = "Nothing was redone: the queue changed after the undo.";

/// The most restored ranges a snapshot names; past this many, the UI
/// selects only the restored selection.
const MAX_RESTORED_RANGES: usize = 128;

/// The most steps the history keeps, to undo and to redo together.
const MAX_STEPS: usize = 100;

/// The most paths the steps keep together; past it, the oldest steps go,
/// though never the latest, which may hold more on its own.
const MAX_PATHS: usize = 100_000;

/// Sent to the UI as its value. `QueueUndoKind` in
/// `ui/src/BinaryBridgeCodec.h` mirrors it, and refuses a packet with a kind
/// it does not know.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueueUndoKind {
    Cleared = 1,
    Removed = 2,
    Replaced = 3,
    Moved = 4,
    // Rows added or inserted.
    Added = 5,
}

/// What a change did to the queue, for its undo.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum Before {
    /// The whole queue, cleared or replaced; empty when an empty queue was
    /// replaced.
    Queue(Vec<PathBuf>),
    /// The removed rows with their indices before the removal, ascending.
    Rows(Vec<(usize, PathBuf)>),
    /// The added rows with their indices after the addition, ascending.
    Added(Vec<(usize, PathBuf)>),
    /// A reorder: for each row after it, the row it was before, and the
    /// rows that moved, ascending, as they were before.
    Order {
        order: Vec<usize>,
        moved: Vec<usize>,
    },
}

/// A change to the queue, with what undoing it takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct Change {
    pub(super) kind: QueueUndoKind,
    pub(super) before: Before,
    /// The selected row before the change.
    pub(super) selected: Option<usize>,
    /// The row of the track the engine had open before the change, also
    /// while stopped.
    pub(super) current: Option<usize>,
    /// The rows marked to play next before the change, in their order. An
    /// undo brings back the marks of the rows it restores; marking and
    /// unmarking are no changes of their own to undo.
    pub(super) play_next: Vec<usize>,
}

/// What a redone change makes of the queue an undo left.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum After {
    /// The queue a clear or a replacement left; empty for a clear.
    Queue(Vec<PathBuf>),
    /// The rows to remove again, ascending.
    Rows(Vec<usize>),
    /// The rows to add again, with their indices once added, ascending.
    Added(Vec<(usize, PathBuf)>),
    /// The reorder to make again, and the rows it moves, ascending, as
    /// [`Before::Order`] names them.
    Order {
        order: Vec<usize>,
        moved: Vec<usize>,
    },
}

/// The change an undo undid, with what redoing it takes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct Redo {
    pub(super) kind: QueueUndoKind,
    pub(super) after: After,
    /// The selected row before the undo, which a redone clear, replacement
    /// or addition brings back.
    pub(super) selected: Option<usize>,
    /// The row of the track the engine had open before the undo, where a
    /// redone replacement stops when that track is not in its queue.
    pub(super) current: Option<usize>,
    /// The rows marked to play next before the undo, which a redone
    /// replacement or addition marks again.
    pub(super) play_next: Vec<usize>,
}

impl Change {
    fn paths(&self) -> usize {
        match &self.before {
            Before::Queue(paths) => paths.len(),
            Before::Rows(rows) | Before::Added(rows) => rows.len(),
            Before::Order { .. } => 0,
        }
    }

    fn remap_paths(&mut self, renames: &PathRenames) {
        match &mut self.before {
            Before::Queue(paths) => {
                for path in paths {
                    remap_path(path, renames);
                }
            }
            Before::Rows(rows) | Before::Added(rows) => {
                for (_, path) in rows {
                    remap_path(path, renames);
                }
            }
            // A reorder names rows, not paths.
            Before::Order { .. } => {}
        }
    }
}

impl Redo {
    fn paths(&self) -> usize {
        match &self.after {
            After::Queue(paths) => paths.len(),
            After::Added(rows) => rows.len(),
            After::Rows(_) | After::Order { .. } => 0,
        }
    }

    fn remap_paths(&mut self, renames: &PathRenames) {
        match &mut self.after {
            After::Queue(paths) => {
                for path in paths {
                    remap_path(path, renames);
                }
            }
            After::Added(rows) => {
                for (_, path) in rows {
                    remap_path(path, renames);
                }
            }
            // These name rows, not paths.
            After::Rows(_) | After::Order { .. } => {}
        }
    }
}

/// The undo and the redo the UI may offer, and the rows the latest undo
/// brought back or the latest redo moved.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueUndoState {
    /// 0 when there is nothing to undo.
    pub offer_id: u64,
    pub kind: Option<QueueUndoKind>,
    /// 0 when there is nothing to redo.
    pub redo_id: u64,
    pub redo_kind: Option<QueueUndoKind>,
    pub restored: Option<QueueUndoRestored>,
}

/// The rows an undo brought back, or a redo of a move or an addition moved
/// or added, for the UI to select. It lasts until the rows change again.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueUndoRestored {
    /// The ID of the undone or redone change.
    pub id: u64,
    /// The queue revision the undo left.
    pub revision: u64,
    /// Ascending (start, count) ranges of rows; empty if there are too
    /// many, when the UI selects the restored selection only.
    pub ranges: Vec<(usize, usize)>,
}

/// Which of the steps a cap may not drop.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Latest {
    Undo,
    Redo,
}

#[derive(Debug, Clone, Default)]
pub(super) struct QueueUndo {
    last_id: u64,
    /// The steps to undo, the latest last.
    undo: VecDeque<(u64, Change)>,
    /// The steps to redo, the next to redo last.
    redo: VecDeque<(u64, Redo)>,
    /// The paths the steps keep, together.
    paths: usize,
    restored: Option<QueueUndoRestored>,
}

impl QueueUndo {
    /// Note that the user changed the queue's rows: `change` is the step to
    /// undo it, which drops the steps to redo. A row change without one
    /// would leave the steps for a queue they no longer fit, so it drops
    /// the whole history; every change records its step.
    pub(super) fn record(&mut self, change: Option<Change>) {
        self.restored = None;
        let Some(change) = change else {
            self.forget();
            return;
        };
        for (_, redo) in self.redo.drain(..) {
            self.paths -= redo.paths();
        }
        self.push_undo(change);
    }

    /// The step to undo under `id`, if it is the latest; it leaves the
    /// history.
    pub(super) fn take(&mut self, id: u64) -> Option<Change> {
        if self.undo.back().is_none_or(|(offered, _)| *offered != id) {
            return None;
        }
        let (_, change) = self.undo.pop_back()?;
        self.paths -= change.paths();
        Some(change)
    }

    /// Offer to redo the change an undo just undid, on top of the steps to
    /// redo.
    pub(super) fn offer_redo(&mut self, redo: Redo) {
        self.last_id += 1;
        self.paths += redo.paths();
        self.redo.push_back((self.last_id, redo));
        self.cap(Latest::Redo);
    }

    /// Whether the redo offered under `id` replaces the whole queue, as a
    /// clear or a replacement does.
    pub(super) fn redo_replaces_queue(&self, id: u64) -> bool {
        self.redo
            .back()
            .is_some_and(|(offered, redo)| *offered == id && matches!(redo.after, After::Queue(_)))
    }

    /// The step to redo under `id`, if it is the next; it leaves the
    /// history.
    pub(super) fn take_redo(&mut self, id: u64) -> Option<Redo> {
        if self.redo.back().is_none_or(|(offered, _)| *offered != id) {
            return None;
        }
        let (_, redo) = self.redo.pop_back()?;
        self.paths -= redo.paths();
        Some(redo)
    }

    /// Offer to undo the change a redo just made again, on top of the
    /// steps to undo; the steps left to redo stay.
    pub(super) fn redone(&mut self, change: Change) {
        self.restored = None;
        self.push_undo(change);
    }

    fn push_undo(&mut self, change: Change) {
        self.last_id += 1;
        self.paths += change.paths();
        self.undo.push_back((self.last_id, change));
        self.cap(Latest::Undo);
    }

    /// Drop the oldest steps past either cap: the first changes to undo,
    /// then the last to redo, but never the step just added.
    fn cap(&mut self, latest: Latest) {
        while self.undo.len() + self.redo.len() > MAX_STEPS || self.paths > MAX_PATHS {
            let keep_undo = usize::from(latest == Latest::Undo);
            let keep_redo = usize::from(latest == Latest::Redo);
            if self.undo.len() > keep_undo {
                if let Some((_, change)) = self.undo.pop_front() {
                    self.paths -= change.paths();
                }
            } else if self.redo.len() > keep_redo {
                if let Some((_, redo)) = self.redo.pop_front() {
                    self.paths -= redo.paths();
                }
            } else {
                // The latest step alone.
                break;
            }
        }
    }

    /// Note that the undo or redo of `id` restored `rows`, ascending, leaving
    /// `revision`.
    pub(super) fn restored(&mut self, id: u64, revision: u64, rows: &[usize]) {
        let mut ranges: Vec<(usize, usize)> = Vec::new();
        for &row in rows {
            match ranges.last_mut() {
                Some((start, count)) if *start + *count == row => *count += 1,
                _ => ranges.push((row, 1)),
            }
        }
        if ranges.len() > MAX_RESTORED_RANGES {
            ranges.clear();
        }
        self.restored = Some(QueueUndoRestored {
            id,
            revision,
            ranges,
        });
    }

    /// Follow renamed files, so an undo or a redo brings back their new
    /// paths.
    pub(super) fn remap_paths(&mut self, renames: &PathRenames) {
        for (_, change) in &mut self.undo {
            change.remap_paths(renames);
        }
        for (_, redo) in &mut self.redo {
            redo.remap_paths(renames);
        }
    }

    /// Drop the whole history, for a queue loaded from the saved session.
    pub(super) fn forget(&mut self) {
        self.undo.clear();
        self.redo.clear();
        self.paths = 0;
        self.restored = None;
    }

    pub(super) fn state(&self) -> QueueUndoState {
        let (offer_id, kind) = self
            .undo
            .back()
            .map_or((0, None), |(id, change)| (*id, Some(change.kind)));
        let (redo_id, redo_kind) = self
            .redo
            .back()
            .map_or((0, None), |(id, redo)| (*id, Some(redo.kind)));
        QueueUndoState {
            offer_id,
            kind,
            redo_id,
            redo_kind,
            restored: self.restored.clone(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    fn cleared(paths: &[&str]) -> Change {
        Change {
            kind: QueueUndoKind::Cleared,
            before: Before::Queue(paths.iter().map(|path| p(path)).collect()),
            selected: Some(0),
            current: None,
            play_next: Vec::new(),
        }
    }

    /// The `Name = value` members of the enum that `declaration` opens,
    /// without `...Last`, which names another member. Any other line fails,
    /// so that no member escapes the comparison.
    fn enum_values<'a>(source: &'a str, declaration: &str) -> Vec<(&'a str, u8)> {
        let (_, body) = source.split_once(declaration).expect("the enum");
        let (body, _) = body.split_once('}').expect("the end of the enum");
        body.lines()
            .map(|line| line.split("//").next().unwrap_or_default().trim())
            .filter(|line| !line.is_empty())
            .map(|line| {
                let member = line.trim_end_matches(',');
                member.split_once(" = ").unwrap_or_else(|| panic!("{line}"))
            })
            .filter(|(name, _)| !name.ends_with("Last"))
            .map(|(name, value)| (name, value.parse().unwrap_or_else(|_| panic!("{value}"))))
            .collect()
    }

    #[test]
    fn enum_values_read_commented_members_and_refuse_others() {
        let source = "enum Kind {\n    // Why.\n    A = 1, // Note.\n    KindLast = A,\n};";
        assert_eq!(enum_values(source, "enum Kind {"), vec![("A", 1)]);
        for member in ["B,", "B = next,"] {
            let source = format!("enum Kind {{\n    {member}\n}}");
            assert!(std::panic::catch_unwind(|| enum_values(&source, "enum Kind {")).is_err());
        }
    }

    #[test]
    fn the_kinds_are_the_ones_the_qt_decoder_knows() {
        let header = include_str!("../../ui/src/BinaryBridgeCodec.h");
        let qt: Vec<(&str, u8)> = enum_values(header, "enum QueueUndoKind : quint8 {")
            .into_iter()
            .map(|(name, value)| (name.strip_prefix("QueueUndo").expect("the prefix"), value))
            .collect();
        let mut kinds = vec![("None", 0)];
        kinds.extend(enum_values(
            include_str!("queue_undo.rs"),
            "pub enum QueueUndoKind {",
        ));
        assert_eq!(qt, kinds);
        let (last, _) = kinds.last().expect("the kinds");
        assert!(
            header.contains(&format!("QueueUndoLast = QueueUndo{last},")),
            "the decoder must accept {last}"
        );
    }

    fn added(rows: &[(usize, &str)]) -> Change {
        Change {
            kind: QueueUndoKind::Added,
            before: Before::Added(rows.iter().map(|(row, path)| (*row, p(path))).collect()),
            selected: None,
            current: None,
            play_next: Vec::new(),
        }
    }

    fn redo_of(kind: QueueUndoKind, paths: &[&str]) -> Redo {
        Redo {
            kind,
            after: After::Queue(paths.iter().map(|path| p(path)).collect()),
            selected: None,
            current: None,
            play_next: Vec::new(),
        }
    }

    /// The kinds the UI is offered to undo and to redo.
    fn offered(undo: &QueueUndo) -> (Option<QueueUndoKind>, Option<QueueUndoKind>) {
        let state = undo.state();
        (state.kind, state.redo_kind)
    }

    #[test]
    fn undo_takes_the_steps_latest_first_and_redo_gives_them_back_in_order() {
        use QueueUndoKind::{Added, Cleared, Replaced};

        let mut undo = QueueUndo::default();
        assert_eq!(undo.state(), QueueUndoState::default());
        undo.record(Some(added(&[(0, "/a.flac")])));
        let first = undo.state().offer_id;
        undo.record(Some(cleared(&["/a.flac"])));
        let second = undo.state().offer_id;
        assert_ne!(first, second);
        assert_eq!(offered(&undo), (Some(Cleared), None));

        // Only the latest step is offered; an older one is refused.
        assert_eq!(undo.take(first), None);
        assert_eq!(
            undo.take(second).map(|change| change.before),
            Some(Before::Queue(vec![p("/a.flac")]))
        );
        assert_eq!(undo.take(second), None, "an undo applies once");
        undo.offer_redo(redo_of(Cleared, &[]));
        assert_eq!(offered(&undo), (Some(Added), Some(Cleared)));
        assert_eq!(undo.state().offer_id, first, "the step below is next");
        assert!(undo.take(first).is_some());
        undo.offer_redo(redo_of(Added, &["/a.flac"]));
        assert_eq!(offered(&undo), (None, Some(Added)));

        // Redo gives them back in the order they were made, each under
        // an ID no step had, and leaves each to undo again.
        let redo_add = undo.state().redo_id;
        assert!(redo_add > second);
        assert_eq!(undo.take_redo(first), None, "an undo's ID is no redo's");
        assert_eq!(undo.take_redo(redo_add).map(|redo| redo.kind), Some(Added));
        undo.redone(added(&[(0, "/a.flac")]));
        assert_eq!(offered(&undo), (Some(Added), Some(Cleared)));
        let redo_clear = undo.state().redo_id;
        assert_eq!(undo.take_redo(redo_add), None, "a redo applies once");
        assert!(undo.take_redo(redo_clear).is_some());
        undo.redone(cleared(&["/a.flac"]));
        assert_eq!(offered(&undo), (Some(Cleared), None));

        // An undo, then a new change: the step to redo goes.
        let id = undo.state().offer_id;
        assert!(undo.take(id).is_some());
        undo.offer_redo(redo_of(Cleared, &[]));
        let redo = undo.state().redo_id;
        undo.record(Some(cleared(&["/b.flac"])));
        assert_eq!(offered(&undo), (Some(Cleared), None));
        assert_eq!(undo.take_redo(redo), None);
        undo.record(Some(Change {
            kind: Replaced,
            ..cleared(&[])
        }));
        assert_eq!(offered(&undo), (Some(Replaced), None));
        assert_eq!(undo.undo.len(), 3, "a replaced empty queue is a step");
    }

    #[test]
    fn a_row_change_without_a_step_drops_the_whole_history_and_so_does_a_saved_session() {
        let mut undo = QueueUndo::default();
        undo.record(Some(cleared(&["/a.flac"])));
        undo.record(Some(cleared(&["/b.flac"])));
        let id = undo.state().offer_id;
        assert!(undo.take(id).is_some());
        undo.offer_redo(redo_of(QueueUndoKind::Cleared, &[]));
        undo.record(None);
        assert_eq!(undo.state(), QueueUndoState::default());
        assert_eq!(undo.paths, 0);

        undo.record(Some(cleared(&["/a.flac"])));
        undo.offer_redo(redo_of(QueueUndoKind::Cleared, &["/b.flac"]));
        undo.forget();
        assert_eq!(undo.state(), QueueUndoState::default());
        assert_eq!(undo.paths, 0);
    }

    #[test]
    fn the_history_drops_its_oldest_steps_past_the_step_cap() {
        let mut undo = QueueUndo::default();
        for step in 0..MAX_STEPS + 5 {
            undo.record(Some(added(&[(step, "/a.flac")])));
        }
        assert_eq!(undo.undo.len(), MAX_STEPS);
        let first = |undo: &QueueUndo| match &undo.undo.front().expect("a step").1.before {
            Before::Added(rows) => rows[0].0,
            other => panic!("{other:?}"),
        };
        assert_eq!(first(&undo), 5, "the oldest five went");
        assert_eq!(undo.paths, MAX_STEPS);

        // Undone steps count too: the steps to undo and to redo stay
        // within the cap together.
        for _ in 0..3 {
            let id = undo.state().offer_id;
            assert!(undo.take(id).is_some());
            undo.offer_redo(redo_of(QueueUndoKind::Added, &["/a.flac"]));
        }
        assert_eq!(undo.undo.len() + undo.redo.len(), MAX_STEPS);
        assert_eq!(undo.paths, MAX_STEPS);
        undo.record(Some(added(&[(0, "/b.flac")])));
        assert_eq!((undo.undo.len(), undo.redo.len()), (MAX_STEPS - 2, 0));
        assert_eq!(undo.paths, MAX_STEPS - 2);
    }

    fn queue_of(len: usize) -> Vec<PathBuf> {
        (0..len).map(|row| p(&format!("/{row}.flac"))).collect()
    }

    fn cleared_queue(len: usize) -> Change {
        Change {
            before: Before::Queue(queue_of(len)),
            ..cleared(&[])
        }
    }

    #[test]
    fn the_history_drops_its_oldest_steps_past_the_path_cap_but_keeps_the_latest() {
        let mut undo = QueueUndo::default();
        let quarter = MAX_PATHS / 4;
        for _ in 0..4 {
            undo.record(Some(cleared_queue(quarter)));
        }
        assert_eq!((undo.undo.len(), undo.paths), (4, MAX_PATHS));
        // One more path drops the oldest step.
        undo.record(Some(added(&[(0, "/a.flac")])));
        assert_eq!((undo.undo.len(), undo.paths), (4, 3 * quarter + 1));

        // A step past the cap on its own stays, alone.
        undo.record(Some(cleared_queue(MAX_PATHS + 1)));
        assert_eq!((undo.undo.len(), undo.paths), (1, MAX_PATHS + 1));
        assert_eq!(undo.state().kind, Some(QueueUndoKind::Cleared));

        // So does the redo its undo leaves, though the step below it goes.
        let mut undo = QueueUndo::default();
        undo.record(Some(added(&[(0, "/a.flac")])));
        undo.record(Some(cleared(&["/b.flac"])));
        let id = undo.state().offer_id;
        assert!(undo.take(id).is_some());
        undo.offer_redo(Redo {
            after: After::Queue(queue_of(MAX_PATHS)),
            ..redo_of(QueueUndoKind::Replaced, &[])
        });
        assert_eq!(offered(&undo), (None, Some(QueueUndoKind::Replaced)));
        assert_eq!(undo.paths, MAX_PATHS);
    }

    #[test]
    fn renames_reach_the_paths_of_every_step_both_ways() {
        let mut undo = QueueUndo::default();
        undo.record(Some(Change {
            kind: QueueUndoKind::Removed,
            before: Before::Rows(vec![(1, p("/old.flac")), (3, p("/kept.flac"))]),
            selected: Some(1),
            current: Some(0),
            play_next: Vec::new(),
        }));
        undo.record(Some(added(&[(0, "/old.flac")])));
        undo.record(Some(cleared(&["/kept.flac", "/old.flac"])));
        undo.record(Some(added(&[(2, "/old.flac")])));
        let id = undo.state().offer_id;
        assert!(undo.take(id).is_some());
        undo.offer_redo(Redo {
            after: After::Added(vec![(2, p("/old.flac"))]),
            ..redo_of(QueueUndoKind::Added, &[])
        });
        let id = undo.state().offer_id;
        assert!(undo.take(id).is_some());
        undo.offer_redo(redo_of(QueueUndoKind::Replaced, &["/old.flac"]));

        undo.remap_paths(&PathRenames::from([(p("/old.flac"), p("/new.flac"))]));
        let redos: Vec<After> = undo
            .redo
            .iter()
            .map(|(_, redo)| redo.after.clone())
            .collect();
        assert_eq!(
            redos,
            [
                After::Added(vec![(2, p("/new.flac"))]),
                After::Queue(vec![p("/new.flac")]),
            ]
        );
        let undos: Vec<Before> = undo
            .undo
            .iter()
            .map(|(_, change)| change.before.clone())
            .collect();
        assert_eq!(
            undos,
            [
                Before::Rows(vec![(1, p("/new.flac")), (3, p("/kept.flac"))]),
                Before::Added(vec![(0, p("/new.flac"))]),
            ]
        );
    }

    #[test]
    fn restored_rows_travel_as_ranges_until_the_rows_change() {
        let mut undo = QueueUndo::default();
        undo.restored(4, 9, &[1, 2, 3, 7]);
        let restored = undo.state().restored.expect("restored");
        assert_eq!((restored.id, restored.revision), (4, 9));
        assert_eq!(restored.ranges, vec![(1, 3), (7, 1)]);
        undo.record(None);
        assert_eq!(undo.state().restored, None);

        let scattered: Vec<usize> = (0..=MAX_RESTORED_RANGES).map(|row| row * 2).collect();
        undo.restored(5, 10, &scattered);
        assert!(undo.state().restored.expect("restored").ranges.is_empty());
    }
}
