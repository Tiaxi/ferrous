// SPDX-License-Identifier: GPL-3.0-or-later

//! The undo of the latest destructive queue change.
//!
//! Clearing the queue, removing rows, replacing a queue that had rows, and
//! moving rows keep what they changed here, under a new ID that the UI
//! offers to undo.
//! The undo applies only to the queue that change left: any later change to
//! the rows withdraws it, and a later destructive change replaces it. An
//! undo with an ID that is no longer offered is refused, so it never
//! applies to a queue the user did not see. Renames keep the offer, with
//! the new paths.

use std::path::PathBuf;

use crate::playback::{remap_path, PathRenames};

/// Shown when the UI asks to undo a change that is no longer offered.
pub(super) const STALE_UNDO_MESSAGE: &str =
    "Nothing was undone: the queue changed after that change.";

/// The most restored ranges a snapshot names; past this many, the UI
/// selects only the restored selection.
const MAX_RESTORED_RANGES: usize = 128;

/// Sent to the UI as its value. `QueueUndoKind` in
/// `ui/src/BinaryBridgeCodec.h` mirrors it, and refuses a packet with a kind
/// it does not know.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueueUndoKind {
    Cleared = 1,
    Removed = 2,
    Replaced = 3,
    Moved = 4,
}

/// What a destructive change took from the queue.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum Before {
    /// The whole queue, cleared or replaced.
    Queue(Vec<PathBuf>),
    /// The removed rows with their indices before the removal, ascending.
    Rows(Vec<(usize, PathBuf)>),
    /// A reorder: for each row after it, the row it was before, and the
    /// rows that moved, ascending, as they were before.
    Order {
        order: Vec<usize>,
        moved: Vec<usize>,
    },
}

/// A destructive change, with what to restore.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct Change {
    pub(super) kind: QueueUndoKind,
    pub(super) before: Before,
    /// The selected row before the change.
    pub(super) selected: Option<usize>,
    /// The row of the track the engine had open before the change, also
    /// while stopped.
    pub(super) current: Option<usize>,
}

impl Change {
    fn is_empty(&self) -> bool {
        match &self.before {
            Before::Queue(paths) => paths.is_empty(),
            Before::Rows(rows) => rows.is_empty(),
            Before::Order { moved, .. } => moved.is_empty(),
        }
    }
}

/// The undo the UI may offer, and the rows the latest undo brought back.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueUndoState {
    /// 0 when there is nothing to undo.
    pub offer_id: u64,
    pub kind: Option<QueueUndoKind>,
    pub restored: Option<QueueUndoRestored>,
}

/// The rows an undo brought back, for the UI to select. It lasts until the
/// rows change again.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QueueUndoRestored {
    /// The ID of the undone change.
    pub id: u64,
    /// The queue revision the undo left.
    pub revision: u64,
    /// Ascending (start, count) ranges of rows; empty if there are too
    /// many, when the UI selects the restored selection only.
    pub ranges: Vec<(usize, usize)>,
}

#[derive(Debug, Clone, Default)]
pub(super) struct QueueUndo {
    last_id: u64,
    offered: Option<(u64, Change)>,
    restored: Option<QueueUndoRestored>,
}

impl QueueUndo {
    /// Note that the queue's rows changed. `change` is what a destructive
    /// change took, which the UI can now undo; any other change withdraws
    /// the undo.
    pub(super) fn record(&mut self, change: Option<Change>) {
        self.restored = None;
        self.offered = change.filter(|change| !change.is_empty()).map(|change| {
            self.last_id += 1;
            (self.last_id, change)
        });
    }

    /// The change offered under `id`, which is no longer offered.
    pub(super) fn take(&mut self, id: u64) -> Option<Change> {
        if self
            .offered
            .as_ref()
            .is_some_and(|(offered, _)| *offered == id)
        {
            return self.offered.take().map(|(_, change)| change);
        }
        None
    }

    /// Note that the undo of `id` restored `rows`, ascending, leaving
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

    /// Follow renamed files, so an undo brings back their new paths.
    pub(super) fn remap_paths(&mut self, renames: &PathRenames) {
        let Some((_, change)) = self.offered.as_mut() else {
            return;
        };
        match &mut change.before {
            Before::Queue(paths) => {
                for path in paths {
                    remap_path(path, renames);
                }
            }
            Before::Rows(rows) => {
                for (_, path) in rows {
                    remap_path(path, renames);
                }
            }
            // A reorder names rows, not paths.
            Before::Order { .. } => {}
        }
    }

    /// Drop any undo, for a queue loaded from the saved session.
    pub(super) fn forget(&mut self) {
        self.offered = None;
        self.restored = None;
    }

    pub(super) fn state(&self) -> QueueUndoState {
        let (offer_id, kind) = self
            .offered
            .as_ref()
            .map_or((0, None), |(id, change)| (*id, Some(change.kind)));
        QueueUndoState {
            offer_id,
            kind,
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

    #[test]
    fn only_the_latest_destructive_change_is_offered() {
        let mut undo = QueueUndo::default();
        assert_eq!(undo.state().offer_id, 0);
        undo.record(Some(cleared(&["/a.flac"])));
        let first = undo.state().offer_id;
        assert_ne!(first, 0);
        assert_eq!(undo.state().kind, Some(QueueUndoKind::Cleared));

        undo.record(Some(cleared(&["/b.flac", "/c.flac"])));
        let second = undo.state().offer_id;
        assert_ne!(second, first);
        assert_eq!(undo.take(first), None, "an older change is not offered");
        assert_eq!(
            undo.take(second).map(|change| change.before),
            Some(Before::Queue(vec![p("/b.flac"), p("/c.flac")]))
        );
        assert_eq!(undo.take(second), None, "an undo applies once");
        assert_eq!(undo.state().offer_id, 0);
    }

    #[test]
    fn another_change_withdraws_the_offer_and_an_empty_one_offers_nothing() {
        let mut undo = QueueUndo::default();
        undo.record(Some(cleared(&["/a.flac"])));
        let id = undo.state().offer_id;
        undo.record(None);
        assert_eq!(undo.take(id), None);
        undo.record(Some(cleared(&[])));
        assert_eq!(undo.state(), QueueUndoState::default());
    }

    #[test]
    fn renames_reach_the_offered_paths() {
        let mut undo = QueueUndo::default();
        undo.record(Some(Change {
            kind: QueueUndoKind::Removed,
            before: Before::Rows(vec![(1, p("/old.flac")), (3, p("/kept.flac"))]),
            selected: Some(1),
            current: Some(0),
        }));
        let id = undo.state().offer_id;
        undo.remap_paths(&PathRenames::from([(p("/old.flac"), p("/new.flac"))]));
        let change = undo.take(id).expect("still offered");
        assert_eq!(
            change.before,
            Before::Rows(vec![(1, p("/new.flac")), (3, p("/kept.flac"))])
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
