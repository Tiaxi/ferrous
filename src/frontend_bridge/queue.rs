// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::PathBuf;

use crossbeam_channel::Sender;

use crate::playback::{play_next, PlayNextRestore, PlaybackCommand, PlaybackEngine};

use super::queue_sync::QueueEdit;
use super::queue_undo::{
    After, Before, Change, QueueUndoKind, Redo, STALE_REDO_MESSAGE, STALE_UNDO_MESSAGE,
};
use super::{
    send_error, sync_queue_details, BridgeError, BridgeEvent, BridgeQueueCommand, BridgeState,
    ErrorCategory, RowsMove,
};

/// The cost, in rows moved, of the edits a reorder may send the UI as row
/// moves; past it, the UI gets the whole queue.
const MAX_REORDER_EDIT_COST: usize = 1 << 22;
/// The row moves a reorder may always send, whatever the queue's length.
const MIN_REORDER_EDITS: usize = 64;

pub(super) fn handle_queue_command(
    cmd: BridgeQueueCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let cmd = match cmd {
        BridgeQueueCommand::Undo(id) => return undo_queue_change(id, state, playback, event_tx),
        BridgeQueueCommand::Redo(id) => return redo_queue_change(id, state, playback, event_tx),
        BridgeQueueCommand::MoveRows { rows, to } => {
            return move_queue_rows(&rows, to, state, playback);
        }
        // The engine owns the marks; its next snapshot reports them. The
        // rows stay, and so does the history of their changes.
        BridgeQueueCommand::TogglePlayNext(rows) => {
            playback.command(PlaybackCommand::TogglePlayNext(rows));
            return false;
        }
        BridgeQueueCommand::ClearPlayNext => {
            playback.command(PlaybackCommand::SetPlayNext(Vec::new()));
            return false;
        }
        // Rows inserted where the user dropped them go there even when the
        // queue changed while they were read.
        BridgeQueueCommand::InsertAt {
            revision,
            at,
            tracks,
        } => BridgeQueueCommand::InsertAt {
            revision: state.queue_sync.revision(),
            at: state
                .queue_sync
                .follow_gap(revision, at)
                .unwrap_or(state.queue.len()),
            tracks,
        },
        cmd => cmd,
    };
    let (changed, step) = apply_queue_command(cmd, state, playback, event_tx);
    if let HistoryStep::Change(change) = step {
        state.queue_undo.record(change);
    }
    changed
}

/// What a queue command leaves to undo.
enum HistoryStep {
    /// It changed no rows, so the history stays as it is.
    Keep,
    /// The step that undoes the rows it changed; `None` for a change the
    /// history cannot undo, which drops it.
    Change(Option<Change>),
}

/// Apply `cmd`, which is no undo, redo, move of rows or mark, and say
/// whether the queue or its selection changed and what it leaves to undo.
fn apply_queue_command(
    cmd: BridgeQueueCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> (bool, HistoryStep) {
    let validate_details = !matches!(
        cmd,
        BridgeQueueCommand::Select(_)
            | BridgeQueueCommand::PlayAt(_)
            | BridgeQueueCommand::JumpTo(_)
            | BridgeQueueCommand::Move { .. }
    );
    // Clearing an empty queue, or replacing it with nothing, leaves the
    // queue an earlier change left, so that change can still be undone.
    let keeps_history = state.queue.is_empty()
        && match &cmd {
            BridgeQueueCommand::Clear => true,
            BridgeQueueCommand::Replace { tracks, .. } => tracks.is_empty(),
            _ => false,
        };
    let undoable = undoable_change(&cmd, state);
    let outcome = apply_queue_command_state(cmd, &mut state.queue, &mut state.selected_queue_index);
    let revision = state.queue_sync.revision();
    match outcome.row_change.clone() {
        Some(QueueRowChange::Edit(edit)) => state.queue_sync.record(edit, state.queue.len()),
        Some(QueueRowChange::Reset) => state.queue_sync.reset(),
        None => {}
    }
    if outcome.row_change.is_some() {
        // Until the engine reports it, the playing row moves with its row;
        // an edit that took the row out, or replaced the queue, leaves none.
        state.playback.current_queue_index = state
            .playback
            .current_queue_index
            .and_then(|row| state.queue_sync.follow(revision, row));
    }
    let step = if outcome.row_change.is_some() && !keeps_history {
        HistoryStep::Change(undoable)
    } else {
        HistoryStep::Keep
    };
    if outcome.changed && validate_details {
        sync_queue_details(state);
    }
    for op in &outcome.playback_ops {
        match op {
            QueuePlaybackOp::LoadQueue(tracks) => {
                playback.command(PlaybackCommand::LoadQueue(tracks.clone()));
            }
            QueuePlaybackOp::PlayQueue(tracks) => {
                playback.command(PlaybackCommand::PlayQueue(tracks.clone()));
            }
            QueuePlaybackOp::AddToQueue(tracks) => {
                playback.command(PlaybackCommand::AddToQueue(tracks.clone()));
            }
            QueuePlaybackOp::RemoveMany(indices) => {
                playback.command(PlaybackCommand::RemoveMany(indices.clone()));
            }
            QueuePlaybackOp::RemoveAt(idx) => playback.command(PlaybackCommand::RemoveAt(*idx)),
            QueuePlaybackOp::Move { from, to } => playback.command(PlaybackCommand::MoveQueue {
                from: *from,
                to: *to,
            }),
            QueuePlaybackOp::Insert { at, paths } => {
                playback.command(PlaybackCommand::InsertIntoQueue {
                    at: *at,
                    paths: paths.clone(),
                });
            }
            QueuePlaybackOp::ClearQueue => playback.command(PlaybackCommand::ClearQueue),
            QueuePlaybackOp::PlayAt(idx) => playback.command(PlaybackCommand::StartAt(*idx)),
            QueuePlaybackOp::JumpTo(idx) => playback.command(PlaybackCommand::JumpTo(*idx)),
        }
    }
    if let Some(error) = outcome.error {
        send_error(event_tx, BridgeError::error(ErrorCategory::Playback, error));
    }
    (outcome.changed, step)
}

/// The step that undoes `cmd`, for the history; used only when `cmd`
/// changes the rows. A cleared or replaced queue moves here, since the
/// command replaces it anyway.
fn undoable_change(cmd: &BridgeQueueCommand, state: &mut BridgeState) -> Option<Change> {
    // Before a cleared or replaced queue moves out of `state`.
    let current = current_row(state);
    let play_next = state.queue_sync.play_next().to_vec();
    let len = state.queue.len();
    let (kind, before) = match cmd {
        BridgeQueueCommand::Clear => (
            QueueUndoKind::Cleared,
            Before::Queue(std::mem::take(&mut state.queue)),
        ),
        BridgeQueueCommand::Replace { .. } => (
            QueueUndoKind::Replaced,
            Before::Queue(std::mem::take(&mut state.queue)),
        ),
        BridgeQueueCommand::Remove(index) => (
            QueueUndoKind::Removed,
            removed_rows(&state.queue, std::slice::from_ref(index)),
        ),
        BridgeQueueCommand::RemoveMany(indices) => {
            (QueueUndoKind::Removed, removed_rows(&state.queue, indices))
        }
        BridgeQueueCommand::Append(tracks) => (QueueUndoKind::Added, added_rows(len, tracks)),
        // As `insert_queue_command_outcome` places them.
        BridgeQueueCommand::InsertAt { at, tracks, .. } => {
            (QueueUndoKind::Added, added_rows((*at).min(len), tracks))
        }
        BridgeQueueCommand::Move { from, to } => {
            let (from, to) = (*from, *to);
            let mut order: Vec<usize> = (0..len).collect();
            if from < len && to < len {
                let row = order.remove(from);
                order.insert(to, row);
            }
            (
                QueueUndoKind::Moved,
                Before::Order {
                    order,
                    moved: vec![from],
                },
            )
        }
        _ => return None,
    };
    Some(Change {
        kind,
        before,
        selected: state.selected_queue_index,
        current,
        play_next,
    })
}

/// The rows `tracks` become when added at row `at`.
fn added_rows(at: usize, tracks: &[PathBuf]) -> Before {
    Before::Added((at..).zip(tracks.iter().cloned()).collect())
}

/// The row of the track the engine has open, playing, paused or stopped.
/// A stopped engine reports the track but no row, so the row it last
/// reported stands for it, followed through the edits since; the first row
/// with its path only when that row is lost.
fn current_row(state: &BridgeState) -> Option<usize> {
    let path = state.playback.current.as_ref();
    let holds_path = |row: &usize| path.is_none_or(|path| state.queue.get(*row) == Some(path));
    state
        .playback
        .current_queue_index
        .filter(holds_path)
        .or_else(|| {
            let (revision, row) = state.last_current_row?;
            state.queue_sync.follow(revision, row).filter(holds_path)
        })
        .or_else(|| path.and_then(|path| state.queue.iter().position(|queued| queued == path)))
}

fn removed_rows(queue: &[PathBuf], indices: &[usize]) -> Before {
    let mut rows: Vec<usize> = indices
        .iter()
        .copied()
        .filter(|index| *index < queue.len())
        .collect();
    rows.sort_unstable();
    rows.dedup();
    Before::Rows(
        rows.into_iter()
            .map(|index| (index, queue[index].clone()))
            .collect(),
    )
}

/// Undo the latest change, offered under `id`, if it is still the latest;
/// otherwise tell the user nothing was undone. The selection comes back,
/// and so do the rows it took with their marks; the engine keeps the track
/// it has open when that track is in the restored queue, see
/// [`PlaybackCommand::RestoreQueue`]. Added rows go as Delete takes them.
/// Nothing starts playing that was not. The change can then be redone.
fn undo_queue_change(
    id: u64,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let Some(change) = state.queue_undo.take(id) else {
        // Nothing changed, but the user asked for something that did not happen.
        send_error(
            event_tx,
            BridgeError::warning(ErrorCategory::Playback, STALE_UNDO_MESSAGE),
        );
        return false;
    };
    let kind = change.kind;
    let selected = state.selected_queue_index;
    let current = current_row(state);
    let play_next = state.queue_sync.play_next().to_vec();
    let redo = |after| Redo {
        kind,
        after,
        selected,
        current,
        play_next,
    };
    let restored_rows = match change.before {
        Before::Order { order, moved } => {
            // The rows go back where they were, as a reorder of their own:
            // each row now at `order`'s position goes back to its old row.
            let Some(back) = crate::playback::reorder_positions(&order, state.queue.len()) else {
                return false;
            };
            let redo = redo(After::Order {
                order,
                moved: moved.clone(),
            });
            apply_reorder(state, playback, &back);
            state.selected_queue_index = change.selected.filter(|row| *row < state.queue.len());
            sync_queue_details(state);
            state.queue_undo.offer_redo(redo);
            moved
        }
        Before::Queue(paths) => {
            let replaced = swap_queue(
                state,
                playback,
                paths,
                change.selected,
                change.current,
                change.play_next,
            );
            state.queue_undo.offer_redo(redo(After::Queue(replaced)));
            state.selected_queue_index.into_iter().collect()
        }
        Before::Rows(rows) => {
            let restored: Vec<usize> = rows.iter().map(|(index, _)| *index).collect();
            insert_rows(
                state,
                playback,
                rows,
                change.selected,
                change.current,
                change.play_next,
            );
            state
                .queue_undo
                .offer_redo(redo(After::Rows(restored.clone())));
            restored
        }
        Before::Added(rows) => {
            // The rows as they are now, renamed or not.
            let added: Vec<(usize, PathBuf)> = rows
                .iter()
                .filter_map(|(index, _)| Some((*index, state.queue.get(*index)?.clone())))
                .collect();
            let redo = redo(After::Added(added));
            remove_rows(
                state,
                playback,
                event_tx,
                rows.into_iter().map(|(index, _)| index).collect(),
            );
            state.selected_queue_index = change.selected.filter(|row| *row < state.queue.len());
            state.queue_undo.offer_redo(redo);
            Vec::new()
        }
    };
    state
        .queue_undo
        .restored(id, state.queue_sync.revision(), &restored_rows);
    true
}

/// Make the queue `paths`, as an undo or a redo of a clear or a
/// replacement brings one back, with `selected` and the rows of
/// `play_next` marked; the engine stops on the track it has open, if it is
/// in `paths`, or at `current`. Returns the queue it replaced.
fn swap_queue(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    paths: Vec<PathBuf>,
    selected: Option<usize>,
    current: Option<usize>,
    play_next: Vec<usize>,
) -> Vec<PathBuf> {
    let replaced = std::mem::replace(&mut state.queue, paths);
    state.queue_sync.reset();
    state.playback.current_queue_index = None;
    state.selected_queue_index = selected.filter(|row| *row < state.queue.len());
    match state.queue.len().checked_sub(1) {
        // An empty queue was replaced: undoing that empties it again.
        None => playback.command(PlaybackCommand::ClearQueue),
        Some(last) => {
            let marks = PlayNextRestore::Rows(play_next);
            let marked = play_next::restored(&[], &marks, state.queue.len());
            state.queue_sync.set_play_next(marked.into());
            playback.command(PlaybackCommand::RestoreQueue {
                paths: state.queue.clone(),
                current: current
                    .or(state.selected_queue_index)
                    .unwrap_or(0)
                    .min(last),
                play_next: marks,
            });
        }
    }
    sync_queue_details(state);
    replaced
}

/// Put `rows` back where they were, ascending, as an undo of a removal or
/// a redo of an addition does, with `selected`; the rows marked to play
/// next before, `play_next`, are marked again among them. The engine keeps
/// the track it has open; see [`PlaybackCommand::RestoreQueue`].
fn insert_rows(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    rows: Vec<(usize, PathBuf)>,
    selected: Option<usize>,
    current: Option<usize>,
    play_next: Vec<usize>,
) {
    // The engine's row, also while stopped, and also when an engine
    // snapshot from before an earlier undo named another row; each row put
    // back before it moves it down.
    let engine_row = current_row(state).map(|engine_row| {
        rows.iter().fold(
            engine_row,
            |row, (index, _)| {
                if *index <= row {
                    row + 1
                } else {
                    row
                }
            },
        )
    });
    let marks = PlayNextRestore::Reinserted {
        rows: rows.iter().map(|(index, _)| *index).collect(),
        before: play_next,
    };
    put_back_rows(state, rows, &marks);
    // Until the engine reports it, the playing row moves with its row, so
    // an undo right after this one finds it.
    if state.playback.current_queue_index.is_some() {
        state.playback.current_queue_index = engine_row;
    }
    let Some(last) = state.queue.len().checked_sub(1) else {
        return;
    };
    state.selected_queue_index = selected.filter(|row| *row <= last);
    let current = engine_row
        .or(current)
        .or(state.selected_queue_index)
        .unwrap_or(0)
        .min(last);
    playback.command(PlaybackCommand::RestoreQueue {
        paths: state.queue.clone(),
        current,
        play_next: marks,
    });
    sync_queue_details(state);
}

/// Take out `rows` as Delete does, as an undo of an addition or a redo of
/// a removal does: the selection follows, and when the playing row goes,
/// the next one takes over. Returns the step that undoes the removal.
fn remove_rows(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
    rows: Vec<usize>,
) -> Option<Change> {
    match apply_queue_command(
        BridgeQueueCommand::RemoveMany(rows),
        state,
        playback,
        event_tx,
    ) {
        (_, HistoryStep::Change(change)) => change,
        (_, HistoryStep::Keep) => None,
    }
}

/// Put the removed `rows` back where they were, ascending, and their marks,
/// as `marks` says, as the engine does with them.
fn put_back_rows(state: &mut BridgeState, rows: Vec<(usize, PathBuf)>, marks: &PlayNextRestore) {
    // The rows marked now stay marked, and the rows put back get the marks
    // they had.
    let marked = state.queue_sync.play_next();
    // Ascending, each run of rows goes back where it was.
    let mut rows = rows.into_iter().peekable();
    while let Some((at, path)) = rows.next() {
        let mut paths = vec![path];
        while let Some((_, path)) = rows.next_if(|(index, _)| *index == at + paths.len()) {
            paths.push(path);
        }
        let at = at.min(state.queue.len());
        state.queue.splice(at..at, paths.iter().cloned());
        state
            .queue_sync
            .record(QueueEdit::Insert { at, paths }, state.queue.len());
    }
    let marked = play_next::restored(&marked, marks, state.queue.len());
    state.queue_sync.set_play_next(marked.into());
}

/// Make again the change whose undo left the redo offered under `id`, if it
/// is still the next to redo; otherwise tell the user nothing was redone.
/// Like the change, the redo can be undone. Like an undo, it never starts
/// playback: a removal is made again as it was, and a replaced queue comes
/// back as an undo brings one back, with the selection it had before the
/// undo. A removal or a move takes the selection along, and moved or added
/// rows are selected where they went.
fn redo_queue_change(
    id: u64,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let Some(redo) = state.queue_undo.take_redo(id) else {
        // Nothing changed, but the user asked for something that did not happen.
        send_error(
            event_tx,
            BridgeError::warning(ErrorCategory::Playback, STALE_REDO_MESSAGE),
        );
        return false;
    };
    let kind = redo.kind;
    let selected = state.selected_queue_index;
    let current = current_row(state);
    let play_next = state.queue_sync.play_next().to_vec();
    let change = |before| Change {
        kind,
        before,
        selected,
        current,
        play_next,
    };
    match redo.after {
        After::Rows(rows) => {
            if let Some(change) = remove_rows(state, playback, event_tx, rows) {
                state.queue_undo.redone(change);
            }
            true
        }
        After::Added(rows) => {
            let added: Vec<usize> = rows.iter().map(|(index, _)| *index).collect();
            let change = change(Before::Added(rows.clone()));
            insert_rows(
                state,
                playback,
                rows,
                redo.selected,
                redo.current,
                redo.play_next,
            );
            state.queue_undo.redone(change);
            state
                .queue_undo
                .restored(id, state.queue_sync.revision(), &added);
            true
        }
        After::Order { order, moved } => {
            let Some(positions) = crate::playback::reorder_positions(&order, state.queue.len())
            else {
                return false;
            };
            apply_reorder(state, playback, &order);
            let mut moved_to: Vec<usize> = moved.iter().map(|row| positions[*row]).collect();
            moved_to.sort_unstable();
            state
                .queue_undo
                .redone(change(Before::Order { order, moved }));
            state
                .queue_undo
                .restored(id, state.queue_sync.revision(), &moved_to);
            true
        }
        After::Queue(paths) => {
            let replaced = swap_queue(
                state,
                playback,
                paths,
                redo.selected,
                redo.current,
                redo.play_next,
            );
            state.queue_undo.redone(change(Before::Queue(replaced)));
            true
        }
    }
}

/// Move `rows` as `to` says, as one change the UI can undo. The engine's
/// committed track and the selected row follow their rows.
fn move_queue_rows(
    rows: &[usize],
    to: RowsMove,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
) -> bool {
    let Some((order, moved)) = moved_rows_order(state.queue.len(), rows, to) else {
        return false;
    };
    let selected = state.selected_queue_index;
    let current = current_row(state);
    let play_next = state.queue_sync.play_next().to_vec();
    apply_reorder(state, playback, &order);
    state.queue_undo.record(Some(Change {
        kind: QueueUndoKind::Moved,
        before: Before::Order { order, moved },
        selected,
        current,
        play_next,
    }));
    true
}

/// Put the queue in `order`, which names for each new row the row it was:
/// the rows, the selection, the playing row, the engine and the UI's rows.
fn apply_reorder(state: &mut BridgeState, playback: &PlaybackEngine, order: &[usize]) {
    let Some(positions) = crate::playback::reorder_positions(order, state.queue.len()) else {
        return;
    };
    // The marks follow their rows, also when the UI gets the whole queue.
    let marked = play_next::after_reorder(&state.queue_sync.play_next(), &positions);
    match reorder_edits(order) {
        Some(edits) => {
            for edit in edits {
                state.queue_sync.record(edit, state.queue.len());
            }
        }
        None => state.queue_sync.reset(),
    }
    state.queue_sync.set_play_next(marked.into());
    let mut old = std::mem::take(&mut state.queue);
    state.queue = order
        .iter()
        .map(|index| std::mem::take(&mut old[*index]))
        .collect();
    let follow = |row: Option<usize>| row.and_then(|row| positions.get(row).copied());
    state.selected_queue_index = follow(state.selected_queue_index);
    // Until the engine reports it, the playing row moves with its row.
    state.playback.current_queue_index = follow(state.playback.current_queue_index);
    playback.command(PlaybackCommand::ReorderQueue(order.to_vec()));
}

/// The order of a queue of `len` rows once `rows` move as `to` says, and
/// the rows that move, ascending; `None` if no row moves.
pub(super) fn moved_rows_order(
    len: usize,
    rows: &[usize],
    to: RowsMove,
) -> Option<(Vec<usize>, Vec<usize>)> {
    let mut moved: Vec<usize> = rows.iter().copied().filter(|row| *row < len).collect();
    moved.sort_unstable();
    moved.dedup();
    if moved.is_empty() {
        return None;
    }
    let mut selected = vec![false; len];
    for row in &moved {
        selected[*row] = true;
    }
    let order: Vec<usize> = match to {
        RowsMove::Before(gap) => {
            let gap = gap.min(len);
            let others = |range: std::ops::Range<usize>| {
                range.filter(|row| !selected[*row]).collect::<Vec<_>>()
            };
            let mut order = others(0..gap);
            order.extend_from_slice(&moved);
            order.extend(others(gap..len));
            order
        }
        RowsMove::Up | RowsMove::Down => {
            let mut order: Vec<usize> = (0..len).collect();
            // Each moved row passes the unmoved row beside it, so a run
            // moves as a whole and stops at the end of the queue.
            let mut swap_if = |above: usize, moving_up: bool| {
                let below = above + 1;
                let (moving, other) = if moving_up {
                    (below, above)
                } else {
                    (above, below)
                };
                if selected[moving] && !selected[other] {
                    order.swap(above, below);
                    selected.swap(above, below);
                }
            };
            if to == RowsMove::Up {
                for above in 0..len.saturating_sub(1) {
                    swap_if(above, true);
                }
            } else {
                for above in (0..len.saturating_sub(1)).rev() {
                    swap_if(above, false);
                }
            }
            order
        }
    };
    if order.iter().enumerate().all(|(row, index)| row == *index) {
        return None;
    }
    Some((order, moved))
}

/// Row moves that put a queue in `order`, for the UI, which then keeps its
/// rows, selection and scroll position. `None` if they would cost more than
/// sending the whole queue.
pub(super) fn reorder_edits(order: &[usize]) -> Option<Vec<QueueEdit>> {
    let len = order.len();
    let max_edits = (MAX_REORDER_EDIT_COST / len.max(1)).max(MIN_REORDER_EDITS);
    let mut current: Vec<usize> = (0..len).collect();
    let mut edits = Vec::new();
    let mut row = 0;
    while row < len {
        if current[row] == order[row] {
            row += 1;
            continue;
        }
        // The row that belongs here is further down; it comes up with the
        // rows after it that belong after it.
        let from = row
            + current[row..]
                .iter()
                .position(|index| *index == order[row])?;
        let mut count = 1;
        while from + count < len && row + count < len && current[from + count] == order[row + count]
        {
            count += 1;
        }
        if edits.len() == max_edits {
            return None;
        }
        current[row..from + count].rotate_right(count);
        edits.push(QueueEdit::MoveRows {
            from,
            count,
            to: row,
        });
        row += count;
    }
    Some(edits)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum QueuePlaybackOp {
    LoadQueue(Vec<PathBuf>),
    /// Load the queue and play its first row, in one step of the engine.
    PlayQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    Insert {
        at: usize,
        paths: Vec<PathBuf>,
    },
    RemoveAt(usize),
    RemoveMany(Vec<usize>),
    Move {
        from: usize,
        to: usize,
    },
    ClearQueue,
    /// Play the row from its start, in one step of the engine.
    PlayAt(usize),
    /// Play the row from its start as a jump within the shuffle's round;
    /// see [`PlaybackCommand::JumpTo`].
    JumpTo(usize),
}

/// How a command changed the queue's rows, for the UI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum QueueRowChange {
    Edit(QueueEdit),
    /// Every row was replaced.
    Reset,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub(super) struct QueueCommandOutcome {
    pub(super) changed: bool,
    pub(super) playback_ops: Vec<QueuePlaybackOp>,
    pub(super) error: Option<String>,
    pub(super) row_change: Option<QueueRowChange>,
}

fn replace_queue_command_outcome(
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
    tracks: Vec<PathBuf>,
    autoplay: bool,
) -> QueueCommandOutcome {
    *queue = tracks;
    *selected_queue_index = if queue.is_empty() { None } else { Some(0) };
    // One command, so the engine switches to the first row once and
    // reports it playing, not first stopped or paused there.
    let playback_op = if queue.is_empty() {
        QueuePlaybackOp::ClearQueue
    } else if autoplay {
        QueuePlaybackOp::PlayQueue(queue.clone())
    } else {
        QueuePlaybackOp::LoadQueue(queue.clone())
    };
    QueueCommandOutcome {
        changed: true,
        playback_ops: vec![playback_op],
        error: None,
        row_change: Some(QueueRowChange::Reset),
    }
}

fn append_queue_command_outcome(
    queue: &mut Vec<PathBuf>,
    tracks: Vec<PathBuf>,
) -> QueueCommandOutcome {
    if tracks.is_empty() {
        return QueueCommandOutcome::default();
    }

    let at = queue.len();
    queue.extend(tracks.iter().cloned());
    let playback_ops = vec![if at == 0 {
        QueuePlaybackOp::LoadQueue(queue.clone())
    } else {
        QueuePlaybackOp::AddToQueue(tracks.clone())
    }];
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
        row_change: Some(QueueRowChange::Edit(QueueEdit::Insert {
            at,
            paths: tracks,
        })),
    }
}

fn insert_queue_command_outcome(
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
    at: usize,
    tracks: Vec<PathBuf>,
) -> QueueCommandOutcome {
    if tracks.is_empty() {
        return QueueCommandOutcome::default();
    }
    let at = at.min(queue.len());
    let was_empty = queue.is_empty();
    queue.splice(at..at, tracks.iter().cloned());
    *selected_queue_index =
        selected_queue_index.map(|row| if row >= at { row + tracks.len() } else { row });
    let playback_ops = vec![if was_empty {
        QueuePlaybackOp::LoadQueue(queue.clone())
    } else {
        QueuePlaybackOp::Insert {
            at,
            paths: tracks.clone(),
        }
    }];
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
        row_change: Some(QueueRowChange::Edit(QueueEdit::Insert {
            at,
            paths: tracks,
        })),
    }
}

fn play_at_queue_command_outcome(
    op: fn(usize) -> QueuePlaybackOp,
    idx: usize,
    queue_len: usize,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    if idx >= queue_len {
        return QueueCommandOutcome {
            error: Some(format!("queue index {idx} out of bounds")),
            ..QueueCommandOutcome::default()
        };
    }

    *selected_queue_index = Some(idx);
    QueueCommandOutcome {
        changed: true,
        // One command, so the engine reports the row playing, not first
        // stopped or paused there.
        playback_ops: vec![op(idx)],
        ..QueueCommandOutcome::default()
    }
}

fn remove_queue_command_outcome(
    idx: usize,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    if idx >= queue.len() {
        return QueueCommandOutcome::default();
    }

    queue.remove(idx);
    let playback_ops = if queue.is_empty() {
        *selected_queue_index = None;
        vec![QueuePlaybackOp::ClearQueue]
    } else {
        *selected_queue_index = selected_queue_index.and_then(|sel| match sel.cmp(&idx) {
            std::cmp::Ordering::Equal => Some(sel.min(queue.len().saturating_sub(1))),
            std::cmp::Ordering::Greater => Some(sel - 1),
            std::cmp::Ordering::Less => Some(sel),
        });
        vec![QueuePlaybackOp::RemoveAt(idx)]
    };
    QueueCommandOutcome {
        changed: true,
        playback_ops,
        error: None,
        row_change: Some(QueueRowChange::Edit(QueueEdit::Remove(vec![idx]))),
    }
}

fn move_queue_command_outcome(
    from: usize,
    to: usize,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    if from >= queue.len() || to >= queue.len() || from == to {
        return QueueCommandOutcome::default();
    }

    let item = queue.remove(from);
    queue.insert(to, item);
    *selected_queue_index = selected_queue_index.map(|sel| {
        if sel == from {
            to
        } else if from < sel && to >= sel {
            sel - 1
        } else if from > sel && to <= sel {
            sel + 1
        } else {
            sel
        }
    });
    QueueCommandOutcome {
        changed: true,
        playback_ops: vec![QueuePlaybackOp::Move { from, to }],
        error: None,
        row_change: Some(QueueRowChange::Edit(QueueEdit::Move { from, to })),
    }
}

pub(super) fn apply_queue_command_state(
    cmd: BridgeQueueCommand,
    queue: &mut Vec<PathBuf>,
    selected_queue_index: &mut Option<usize>,
) -> QueueCommandOutcome {
    match cmd {
        BridgeQueueCommand::Replace { tracks, autoplay } => {
            replace_queue_command_outcome(queue, selected_queue_index, tracks, autoplay)
        }
        BridgeQueueCommand::Append(tracks) => append_queue_command_outcome(queue, tracks),
        BridgeQueueCommand::InsertAt { at, tracks, .. } => {
            insert_queue_command_outcome(queue, selected_queue_index, at, tracks)
        }
        BridgeQueueCommand::PlayAt(idx) => play_at_queue_command_outcome(
            QueuePlaybackOp::PlayAt,
            idx,
            queue.len(),
            selected_queue_index,
        ),
        BridgeQueueCommand::JumpTo(idx) => play_at_queue_command_outcome(
            QueuePlaybackOp::JumpTo,
            idx,
            queue.len(),
            selected_queue_index,
        ),
        BridgeQueueCommand::RemoveMany(indices) => {
            let mut removed: Vec<usize> = indices
                .iter()
                .copied()
                .filter(|index| *index < queue.len())
                .collect();
            removed.sort_unstable();
            removed.dedup();
            *selected_queue_index =
                crate::playback::remove_queue_indices(queue, *selected_queue_index, &indices);
            if removed.is_empty() {
                return QueueCommandOutcome::default();
            }
            QueueCommandOutcome {
                changed: true,
                playback_ops: vec![if queue.is_empty() {
                    QueuePlaybackOp::ClearQueue
                } else {
                    QueuePlaybackOp::RemoveMany(indices)
                }],
                error: None,
                row_change: Some(QueueRowChange::Edit(QueueEdit::Remove(removed))),
            }
        }
        BridgeQueueCommand::Remove(idx) => {
            remove_queue_command_outcome(idx, queue, selected_queue_index)
        }
        BridgeQueueCommand::Move { from, to } => {
            move_queue_command_outcome(from, to, queue, selected_queue_index)
        }
        BridgeQueueCommand::Select(sel) => {
            let normalized = sel.filter(|idx| *idx < queue.len());
            let changed = *selected_queue_index != normalized;
            *selected_queue_index = normalized;
            QueueCommandOutcome {
                changed,
                ..QueueCommandOutcome::default()
            }
        }
        // `handle_queue_command` undoes and redoes changes, moves rows and
        // sends marks to the engine; see `undo_queue_change`,
        // `redo_queue_change` and `move_queue_rows`.
        BridgeQueueCommand::Undo(_)
        | BridgeQueueCommand::Redo(_)
        | BridgeQueueCommand::MoveRows { .. }
        | BridgeQueueCommand::TogglePlayNext(_)
        | BridgeQueueCommand::ClearPlayNext => QueueCommandOutcome::default(),
        BridgeQueueCommand::Clear => {
            queue.clear();
            *selected_queue_index = None;
            QueueCommandOutcome {
                changed: true,
                playback_ops: vec![QueuePlaybackOp::ClearQueue],
                error: None,
                row_change: Some(QueueRowChange::Reset),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use super::super::BridgeQueueCommand;
    use super::*;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    #[test]
    fn batch_removal_keeps_order_and_emits_one_playback_operation() {
        let mut queue = (0..8)
            .map(|index| p(&format!("/{index}.flac")))
            .collect::<Vec<_>>();
        let mut selected = Some(4);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::RemoveMany(vec![1, 3, 3, 5, 99]),
            &mut queue,
            &mut selected,
        );
        assert_eq!(
            queue,
            vec![
                p("/0.flac"),
                p("/2.flac"),
                p("/4.flac"),
                p("/6.flac"),
                p("/7.flac")
            ]
        );
        assert_eq!(selected, Some(2));
        assert_eq!(outcome.playback_ops.len(), 1);
        assert!(matches!(
            &outcome.playback_ops[0],
            QueuePlaybackOp::RemoveMany(_)
        ));
    }

    #[test]
    fn selection_playback_and_reorder_do_not_request_metadata_validation() {
        let (analysis_tx, _) = crossbeam_channel::unbounded();

        let (playback, _) = PlaybackEngine::new(analysis_tx);
        let (event_tx, _) = crossbeam_channel::unbounded();
        let mut state = BridgeState {
            queue: vec![p("/missing/a.flac"), p("/missing/b.flac")],
            ..BridgeState::default()
        };
        for command in [
            BridgeQueueCommand::Select(Some(1)),
            BridgeQueueCommand::PlayAt(0),
            BridgeQueueCommand::Move { from: 0, to: 1 },
        ] {
            assert!(handle_queue_command(
                command, &mut state, &playback, &event_tx
            ));
            assert!(!state.queue_detail_validation_requested);
        }
    }

    #[test]
    fn queue_append_into_empty_loads_full_queue() {
        let mut queue = Vec::new();
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(vec![p("/a.flac"), p("/b.flac")]),
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")])]
        );
    }

    #[test]
    fn queue_append_empty_is_noop() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(Vec::new()),
            &mut queue,
            &mut selected,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac")]);
        assert_eq!(selected, Some(0));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_play_at_out_of_bounds_emits_error() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = None;
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::PlayAt(3), &mut queue, &mut selected);
        assert!(!outcome.changed);
        assert_eq!(
            outcome.error.as_deref(),
            Some("queue index 3 out of bounds")
        );
        assert!(outcome.playback_ops.is_empty());
    }

    #[test]
    fn queue_move_updates_selection_and_uses_move_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Move { from: 0, to: 2 },
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/b.flac"), p("/c.flac"), p("/a.flac")]);
        assert_eq!(selected, Some(2));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::Move { from: 0, to: 2 }]
        );
    }

    #[test]
    fn queue_move_invalid_indices_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Move { from: 2, to: 0 },
            &mut queue,
            &mut selected,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_replace_autoplay_loads_and_plays_in_one_engine_command() {
        // Whatever plays, the engine gets one command, so it switches to the
        // first row once and reports it playing straight away.
        let mut queue = vec![p("/old.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a.flac"), p("/b.flac")],
                autoplay: true,
            },
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::PlayQueue(vec![p("/a.flac"), p("/b.flac")])]
        );
    }

    #[test]
    fn queue_append_non_empty_uses_add_to_queue_op() {
        let mut queue = vec![p("/a.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Append(vec![p("/b.flac"), p("/c.flac")]),
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::AddToQueue(vec![
                p("/b.flac"),
                p("/c.flac")
            ])]
        );
    }

    #[test]
    fn queue_remove_last_track_clears_selection_and_playback_queue() {
        let mut queue = vec![p("/only.flac")];
        let mut selected = Some(0);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(0), &mut queue, &mut selected);
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
    }

    #[test]
    fn queue_remove_middle_track_uses_remove_op_and_keeps_reasonable_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(2);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(1), &mut queue, &mut selected);
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::RemoveAt(1)]);
    }

    #[test]
    fn queue_remove_out_of_bounds_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Remove(3), &mut queue, &mut selected);
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(selected, Some(0));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_updates_state_without_playback_ops() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(1)),
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_same_index_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(1)),
            &mut queue,
            &mut selected,
        );
        assert!(!outcome.changed);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_select_out_of_bounds_clears_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Select(Some(9)),
            &mut queue,
            &mut selected,
        );
        assert!(outcome.changed);
        assert!(selected.is_none());
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn each_queue_edit_reaches_the_ui_as_its_delta() {
        use super::super::{QueueEdit, QueueUpdate};

        let (analysis_tx, _) = crossbeam_channel::unbounded();
        let (playback, _) = PlaybackEngine::new(analysis_tx);
        let (event_tx, _) = crossbeam_channel::unbounded();
        let mut state = BridgeState::default();
        let run = |state: &mut BridgeState, command| {
            handle_queue_command(command, state, &playback, &event_tx);
        };
        let mut ui_queue = Vec::new();
        let mut ui_revision = 0;
        // What the UI's queue becomes with the update a snapshot carries now.
        let mut sync_ui = |state: &mut BridgeState| {
            let update = state
                .queue_sync
                .update(&state.queue, state.selected_queue_index);
            if let Some(QueueUpdate::Edits { base_revision, .. }) = &update {
                assert_eq!(
                    *base_revision, ui_revision,
                    "an update since the UI's revision"
                );
            }
            if let Some(update) = &update {
                assert!(update.apply_to(&mut ui_queue));
            }
            state.queue_sync.mark_sent(state.selected_queue_index);
            ui_revision = state.queue_sync.revision();
            assert_eq!(ui_queue, state.queue);
            update
        };

        run(
            &mut state,
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a"), p("/b"), p("/c"), p("/d")],
                autoplay: false,
            },
        );
        assert!(matches!(sync_ui(&mut state), Some(QueueUpdate::Full(_))));

        let edits = |update: Option<QueueUpdate>| match update {
            Some(QueueUpdate::Edits { edits, .. }) => edits,
            other => panic!("expected edits, got {other:?}"),
        };
        run(&mut state, BridgeQueueCommand::Append(vec![p("/e")]));
        assert_eq!(
            edits(sync_ui(&mut state)),
            vec![QueueEdit::Insert {
                at: 4,
                paths: vec![p("/e")]
            }]
        );
        run(&mut state, BridgeQueueCommand::Move { from: 4, to: 0 });
        run(&mut state, BridgeQueueCommand::Remove(1));
        assert_eq!(
            edits(sync_ui(&mut state)),
            vec![
                QueueEdit::Move { from: 4, to: 0 },
                QueueEdit::Remove(vec![1])
            ]
        );
        run(&mut state, BridgeQueueCommand::RemoveMany(vec![3, 0, 3, 9]));
        assert_eq!(
            edits(sync_ui(&mut state)),
            vec![QueueEdit::Remove(vec![0, 3])]
        );
        assert_eq!(state.queue, vec![p("/b"), p("/c")]);

        // Selecting and playing a row change no rows.
        let revision = state.queue_sync.revision();
        run(&mut state, BridgeQueueCommand::Select(Some(1)));
        run(&mut state, BridgeQueueCommand::PlayAt(0));
        run(&mut state, BridgeQueueCommand::Move { from: 1, to: 1 });
        assert_eq!(state.queue_sync.revision(), revision);
        assert!(!state.queue_sync.has_pending());

        run(&mut state, BridgeQueueCommand::Clear);
        assert_eq!(sync_ui(&mut state), Some(QueueUpdate::Full(Vec::new())));
    }

    #[test]
    fn queue_clear_empties_state_and_emits_clear_queue_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(1);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::Clear, &mut queue, &mut selected);
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_play_at_plays_the_row_in_one_engine_command() {
        // Whatever plays: the engine reports the row playing straight away.
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome =
            apply_queue_command_state(BridgeQueueCommand::PlayAt(1), &mut queue, &mut selected);
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::PlayAt(1)]);
        assert!(outcome.error.is_none());
    }

    /// The rows of `order` as letters, for readable expectations.
    fn letters(order: &[usize]) -> String {
        order
            .iter()
            .map(|index| char::from(b'a' + u8::try_from(*index).expect("a letter")))
            .collect()
    }

    fn moved(len: usize, rows: &[usize], to: RowsMove) -> Option<String> {
        moved_rows_order(len, rows, to).map(|(order, _)| letters(&order))
    }

    #[test]
    fn rows_move_together_before_the_drop_row_wherever_it_is() {
        // abcdefgh, with c, d and g moving.
        let rows = [6, 2, 3, 2];
        // Before, after and between the moving rows; at both ends.
        assert_eq!(
            moved(8, &rows, RowsMove::Before(1)).as_deref(),
            Some("acdgbefh")
        );
        assert_eq!(
            moved(8, &rows, RowsMove::Before(8)).as_deref(),
            Some("abefhcdg")
        );
        assert_eq!(
            moved(8, &rows, RowsMove::Before(99)).as_deref(),
            Some("abefhcdg")
        );
        assert_eq!(
            moved(8, &rows, RowsMove::Before(5)).as_deref(),
            Some("abecdgfh")
        );
        assert_eq!(
            moved(8, &rows, RowsMove::Before(0)).as_deref(),
            Some("cdgabefh")
        );
        // Dropped on a moving row, or between them: the rows close up there.
        assert_eq!(
            moved(8, &rows, RowsMove::Before(3)).as_deref(),
            Some("abcdgefh")
        );
        assert_eq!(
            moved(8, &rows, RowsMove::Before(6)).as_deref(),
            Some("abefcdgh")
        );
        // A contiguous block dropped onto itself does not move.
        assert_eq!(moved(8, &[2, 3], RowsMove::Before(3)), None);
        assert_eq!(moved(8, &[2, 3], RowsMove::Before(4)), None);
        assert_eq!(
            moved(8, &[2, 3], RowsMove::Before(6)).as_deref(),
            Some("abefcdgh")
        );
        // Rows past the end are ignored.
        assert_eq!(moved(3, &[9], RowsMove::Before(0)), None);
        assert_eq!(
            moved_rows_order(8, &rows, RowsMove::Before(1)).map(|(_, moved)| moved),
            Some(vec![2, 3, 6])
        );
    }

    #[test]
    fn up_and_down_move_each_run_by_one_row_and_stop_at_the_ends() {
        let rows = [2, 3, 6];
        assert_eq!(moved(8, &rows, RowsMove::Up).as_deref(), Some("acdbegfh"));
        assert_eq!(moved(8, &rows, RowsMove::Down).as_deref(), Some("abecdfhg"));
        // A run at the top stays; the run below it closes up to it.
        assert_eq!(moved(5, &[0, 1, 3], RowsMove::Up).as_deref(), Some("abdce"));
        assert_eq!(moved(5, &[0, 1], RowsMove::Up), None);
        assert_eq!(moved(5, &[3, 4], RowsMove::Down), None);
        assert_eq!(moved(5, &[1, 4], RowsMove::Down).as_deref(), Some("acbde"));
    }

    #[test]
    fn a_reorder_reaches_the_ui_as_few_row_moves() {
        let paths: Vec<PathBuf> = (0..12).map(|index| p(&format!("/{index}"))).collect();
        let check = |order: &[usize]| {
            let edits = reorder_edits(order).expect("row moves");
            let mut ui = paths.clone();
            for edit in &edits {
                assert!(edit.apply_to(&mut ui), "{edit:?} fits");
            }
            let expected: Vec<PathBuf> = order.iter().map(|index| paths[*index].clone()).collect();
            assert_eq!(ui, expected);
            edits.len()
        };
        // Gathering two runs takes a move for each.
        let (order, _) =
            moved_rows_order(12, &[2, 3, 7, 8, 9], RowsMove::Before(11)).expect("move");
        assert!(check(&order) <= 2);
        let (order, _) = moved_rows_order(12, &[0, 5, 10], RowsMove::Up).expect("move");
        assert!(check(&order) <= 2);
        // Any order comes out right.
        let mut seed = 0x2545_f491_u64;
        for _ in 0..50 {
            let mut order: Vec<usize> = (0..12).collect();
            for index in (1..order.len()).rev() {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                let other = usize::try_from(seed % (index as u64 + 1)).expect("index");
                order.swap(index, other);
            }
            check(&order);
        }
        assert_eq!(check(&(0..12).collect::<Vec<_>>()), 0);
    }

    #[test]
    fn a_reorder_too_scattered_for_row_moves_sends_the_whole_queue() {
        // Reversing a long queue would take a move per row.
        let order: Vec<usize> = (0..100_000).rev().collect();
        assert!(reorder_edits(&order).is_none());
    }

    #[test]
    fn rows_inserted_into_the_queue_go_before_the_row_and_move_the_selection() {
        let mut queue = vec![p("/a"), p("/b"), p("/c")];
        let mut selected = Some(1);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::InsertAt {
                revision: 0,
                at: 1,
                tracks: vec![p("/x"), p("/y")],
            },
            &mut queue,
            &mut selected,
        );
        assert_eq!(queue, vec![p("/a"), p("/x"), p("/y"), p("/b"), p("/c")]);
        assert_eq!(selected, Some(3));
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::Insert {
                at: 1,
                paths: vec![p("/x"), p("/y")]
            }]
        );
        assert_eq!(
            outcome.row_change,
            Some(QueueRowChange::Edit(QueueEdit::Insert {
                at: 1,
                paths: vec![p("/x"), p("/y")]
            }))
        );
        // Into an empty queue, the engine loads it.
        let mut queue = Vec::new();
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::InsertAt {
                revision: 0,
                at: 4,
                tracks: vec![p("/x")],
            },
            &mut queue,
            &mut None,
        );
        assert_eq!(
            outcome.playback_ops,
            vec![QueuePlaybackOp::LoadQueue(vec![p("/x")])]
        );
    }
}
