// SPDX-License-Identifier: GPL-3.0-or-later

//! The queue rows marked to play next: a list of rows, in the order they
//! play, ahead of the queue's order or a shuffle pick. The list names rows,
//! not paths, so a queue that holds a file twice marks only the row chosen,
//! and each edit of the rows carries the list along.

use std::collections::{HashMap, HashSet};

/// The marks of a queue that an undo or redo brings back; see
/// [`super::PlaybackCommand::RestoreQueue`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub enum PlayNextRestore {
    /// No row of the restored queue is marked.
    #[default]
    None,
    /// These rows of the restored queue are marked, in this order; the
    /// marks of the queue it replaces go with it.
    Rows(Vec<usize>),
    /// The restored queue is the queue before with `rows`, ascending rows
    /// of the restored queue, put back. The rows marked now stay marked,
    /// and each row put back that `before`, the list of the restored queue
    /// before the rows were taken, marks gets its mark back, ahead of the
    /// rows `before` marks after it.
    Reinserted {
        rows: Vec<usize>,
        before: Vec<usize>,
    },
}

/// `rows` of a queue of `len` rows as a list: rows past the queue and
/// repeated rows are left out.
pub(crate) fn sanitized(rows: impl IntoIterator<Item = usize>, len: usize) -> Vec<usize> {
    let mut seen = HashSet::new();
    rows.into_iter()
        .filter(|row| *row < len && seen.insert(*row))
        .collect()
}

/// `list` after the user marked or unmarked `rows` of a queue of `len`
/// rows: when every one of them is marked, they are unmarked; otherwise
/// those not marked join the end of the list in the queue's order, and the
/// marked ones keep their places.
pub(crate) fn toggled(list: &[usize], rows: &[usize], len: usize) -> Vec<usize> {
    let mut rows: Vec<usize> = rows.iter().copied().filter(|row| *row < len).collect();
    rows.sort_unstable();
    rows.dedup();
    if rows.is_empty() {
        return list.to_vec();
    }
    let marked: HashSet<usize> = list.iter().copied().collect();
    if rows.iter().all(|row| marked.contains(row)) {
        let unmarked: HashSet<usize> = rows.into_iter().collect();
        return list
            .iter()
            .copied()
            .filter(|row| !unmarked.contains(row))
            .collect();
    }
    let mut next = list.to_vec();
    next.extend(rows.into_iter().filter(|row| !marked.contains(row)));
    next
}

/// `list` once the rows `removed`, ascending and distinct, are gone.
pub(crate) fn after_removal(list: &[usize], removed: &[usize]) -> Vec<usize> {
    list.iter()
        .filter(|row| removed.binary_search(row).is_err())
        .map(|row| row - removed.partition_point(|removed| removed < row))
        .collect()
}

/// `list` once `count` rows are inserted before row `at`.
pub(crate) fn after_insert(list: &[usize], at: usize, count: usize) -> Vec<usize> {
    list.iter()
        .map(|row| if *row >= at { row + count } else { *row })
        .collect()
}

/// `list` once each row `row` went to `positions[row]`.
pub(crate) fn after_reorder(list: &[usize], positions: &[usize]) -> Vec<usize> {
    list.iter()
        .filter_map(|row| positions.get(*row).copied())
        .collect()
}

/// The list of a restored queue of `len` rows, given `list`, the list of
/// the queue it replaces, and what to bring back.
pub(crate) fn restored(list: &[usize], restore: &PlayNextRestore, len: usize) -> Vec<usize> {
    match restore {
        PlayNextRestore::None => Vec::new(),
        PlayNextRestore::Rows(rows) => sanitized(rows.iter().copied(), len),
        PlayNextRestore::Reinserted { rows, before } => {
            let put_back: HashSet<usize> = rows.iter().copied().filter(|row| *row < len).collect();
            // The rows that stayed, in order: the queue now is the restored
            // one without the rows put back.
            let stayed: Vec<usize> = (0..len).filter(|row| !put_back.contains(row)).collect();
            let marked: Vec<usize> = list
                .iter()
                .filter_map(|row| stayed.get(*row).copied())
                .collect();
            let marked_now: HashSet<usize> = marked.iter().copied().collect();
            // Each row put back goes ahead of the first row that came after
            // it in `before` and is marked now, or after them all. From the
            // end, so rows put back in a run keep their order once reversed.
            let mut ahead_of: HashMap<Option<usize>, Vec<usize>> = HashMap::new();
            let mut anchor = None;
            for row in sanitized(before.iter().copied(), len).into_iter().rev() {
                if put_back.contains(&row) {
                    ahead_of.entry(anchor).or_default().push(row);
                } else if marked_now.contains(&row) {
                    anchor = Some(row);
                }
            }
            let mut put_back_ahead_of = |anchor: Option<usize>| {
                let mut rows = ahead_of.remove(&anchor).unwrap_or_default();
                rows.reverse();
                rows
            };
            let mut next = Vec::with_capacity(marked.len() + rows.len());
            for row in marked {
                next.extend(put_back_ahead_of(Some(row)));
                next.push(row);
            }
            next.extend(put_back_ahead_of(None));
            next
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        after_insert, after_removal, after_reorder, restored, sanitized, toggled, PlayNextRestore,
    };

    #[test]
    fn marking_appends_in_queue_order_and_unmarks_only_when_all_are_marked() {
        // Selected from the bottom up, the rows still join in queue order.
        assert_eq!(toggled(&[], &[7, 2, 5], 10), vec![2, 5, 7]);
        // A later press adds to the end.
        assert_eq!(toggled(&[2, 5, 7], &[1], 10), vec![2, 5, 7, 1]);
        // A mixed selection marks the unmarked rows, and the marked ones
        // keep their places.
        assert_eq!(toggled(&[5, 2], &[3, 5, 2, 4], 10), vec![5, 2, 3, 4]);
        // Every row marked: they are unmarked, and the rest keep order.
        assert_eq!(toggled(&[5, 2, 3, 4], &[2, 4], 10), vec![5, 3]);
        // Rows past the queue, and repeats, count for nothing.
        assert_eq!(toggled(&[1], &[1, 1, 12], 10), Vec::<usize>::new());
        assert_eq!(toggled(&[1], &[12], 10), vec![1]);
    }

    #[test]
    fn edits_carry_the_marks_with_their_rows() {
        assert_eq!(after_removal(&[6, 1, 4, 3], &[1, 3]), vec![4, 2]);
        assert_eq!(after_insert(&[6, 1, 4], 4, 2), vec![8, 1, 6]);
        // Row 0 moved to the end of four rows.
        assert_eq!(after_reorder(&[0, 3], &[3, 0, 1, 2]), vec![3, 2]);
        assert_eq!(sanitized([3, 9, 3, 1], 5), vec![3, 1]);
    }

    #[test]
    fn restored_rows_get_their_marks_back_in_their_old_places() {
        // Before: rows 2, 5 and 8 marked, in the order 5, 2, 8. Rows 2 and
        // 3 were removed, so 5 and 8 are rows 3 and 6 now.
        let restore = PlayNextRestore::Reinserted {
            rows: vec![2, 3],
            before: vec![5, 2, 8],
        };
        assert_eq!(restored(&[3, 6], &restore, 10), vec![5, 2, 8]);
        // Since, row 5 played and row 1 was marked: row 2 goes ahead of 8.
        assert_eq!(restored(&[6, 1], &restore, 10), vec![2, 8, 1]);
        // Nothing marked after it is marked now: it goes to the end.
        let last = PlayNextRestore::Reinserted {
            rows: vec![9],
            before: vec![9, 4],
        };
        assert_eq!(restored(&[0], &last, 10), vec![0, 9]);
        // A replaced queue comes back with its own marks only.
        assert_eq!(
            restored(&[0, 1], &PlayNextRestore::Rows(vec![4, 2, 40]), 5),
            vec![4, 2]
        );
        assert_eq!(
            restored(&[0, 1], &PlayNextRestore::None, 5),
            Vec::<usize>::new()
        );
    }
}
