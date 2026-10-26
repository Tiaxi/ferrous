// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::PathBuf;

use crossbeam_channel::Sender;

use crate::playback::{PlaybackCommand, PlaybackEngine, PlaybackState};

use super::queue_sync::QueueEdit;
use super::queue_undo::{Before, Change, QueueUndoKind, STALE_UNDO_MESSAGE};
use super::{
    sync_queue_details, try_send_event, BridgeError, BridgeEvent, BridgeQueueCommand, BridgeState,
    ErrorCategory,
};

pub(super) fn handle_queue_command(
    cmd: BridgeQueueCommand,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    if let BridgeQueueCommand::Undo(id) = cmd {
        return undo_queue_change(id, state, playback, event_tx);
    }
    let validate_details = !matches!(
        cmd,
        BridgeQueueCommand::Select(_)
            | BridgeQueueCommand::PlayAt(_)
            | BridgeQueueCommand::Move { .. }
    );
    // Clearing an empty queue leaves the queue an earlier change left, so
    // that change can still be undone.
    let keeps_undo = matches!(cmd, BridgeQueueCommand::Clear) && state.queue.is_empty();
    let undoable = undoable_change(&cmd, state);
    let outcome = apply_queue_command_state(
        cmd,
        &mut state.queue,
        &mut state.selected_queue_index,
        state.playback.state,
    );
    match outcome.row_change.clone() {
        Some(QueueRowChange::Edit(edit)) => state.queue_sync.record(edit, state.queue.len()),
        Some(QueueRowChange::Reset) => state.queue_sync.reset(),
        None => {}
    }
    if outcome.row_change.is_some() && !keeps_undo {
        state.queue_undo.record(undoable);
    }
    if outcome.changed && validate_details {
        sync_queue_details(state);
    }
    for op in &outcome.playback_ops {
        match op {
            QueuePlaybackOp::LoadQueue(tracks) => {
                playback.command(PlaybackCommand::LoadQueue(tracks.clone()));
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
            QueuePlaybackOp::ClearQueue => playback.command(PlaybackCommand::ClearQueue),
            QueuePlaybackOp::PlayAt(idx) => playback.command(PlaybackCommand::PlayAt(*idx)),
            QueuePlaybackOp::Play => playback.command(PlaybackCommand::Play),
        }
    }
    if let Some(error) = outcome.error {
        let _ = try_send_event(
            event_tx,
            BridgeEvent::Error(BridgeError::error(ErrorCategory::Playback, error)),
        );
    }
    outcome.changed
}

/// What `cmd` would take from the queue, for its undo. A cleared or
/// replaced queue moves here, since the command replaces it anyway.
fn undoable_change(cmd: &BridgeQueueCommand, state: &mut BridgeState) -> Option<Change> {
    // Before a cleared or replaced queue moves out of `state`.
    let current = current_row(state);
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
        _ => return None,
    };
    Some(Change {
        kind,
        before,
        selected: state.selected_queue_index,
        current,
    })
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

/// Restore what the change offered under `id` took from the queue, if it
/// is still offered; otherwise tell the user nothing was undone. The
/// selection comes back, and the engine keeps the track it has open when
/// that track is in the restored queue; see
/// [`PlaybackCommand::RestoreQueue`].
fn undo_queue_change(
    id: u64,
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    event_tx: &Sender<BridgeEvent>,
) -> bool {
    let Some(change) = state.queue_undo.take(id) else {
        // Nothing changed, but the user asked for something that did not happen.
        let _ = try_send_event(
            event_tx,
            BridgeEvent::Error(BridgeError::warning(
                ErrorCategory::Playback,
                STALE_UNDO_MESSAGE,
            )),
        );
        return false;
    };
    // The engine's row in the queue that the change left.
    let engine_row = state.playback.current_queue_index;
    let (restored_rows, engine_row) = match change.before {
        Before::Queue(paths) => {
            state.queue = paths;
            state.queue_sync.reset();
            (change.selected.into_iter().collect::<Vec<_>>(), None)
        }
        Before::Rows(rows) => {
            // Each restored row before the engine's row moves it down.
            let engine_row = engine_row.map(|engine_row| {
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
            let restored: Vec<usize> = rows.iter().map(|(index, _)| *index).collect();
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
            (restored, engine_row)
        }
    };
    let Some(last) = state.queue.len().checked_sub(1) else {
        return false;
    };
    state.selected_queue_index = change.selected.filter(|row| *row <= last);
    let current = engine_row
        .or(change.current)
        .or(state.selected_queue_index)
        .unwrap_or(0)
        .min(last);
    playback.command(PlaybackCommand::RestoreQueue {
        paths: state.queue.clone(),
        current,
    });
    sync_queue_details(state);
    state
        .queue_undo
        .restored(id, state.queue_sync.revision(), &restored_rows);
    true
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum QueuePlaybackOp {
    LoadQueue(Vec<PathBuf>),
    AddToQueue(Vec<PathBuf>),
    RemoveAt(usize),
    RemoveMany(Vec<usize>),
    Move { from: usize, to: usize },
    ClearQueue,
    PlayAt(usize),
    Play,
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
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    *queue = tracks;
    *selected_queue_index = if queue.is_empty() { None } else { Some(0) };
    let mut playback_ops = Vec::new();
    if queue.is_empty() {
        playback_ops.push(QueuePlaybackOp::ClearQueue);
    } else {
        playback_ops.push(QueuePlaybackOp::LoadQueue(queue.clone()));
        if autoplay {
            playback_ops.push(QueuePlaybackOp::PlayAt(0));
            if playback_state != PlaybackState::Playing {
                playback_ops.push(QueuePlaybackOp::Play);
            }
        }
    }
    QueueCommandOutcome {
        changed: true,
        playback_ops,
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

fn play_at_queue_command_outcome(
    idx: usize,
    queue_len: usize,
    selected_queue_index: &mut Option<usize>,
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    if idx >= queue_len {
        return QueueCommandOutcome {
            error: Some(format!("queue index {idx} out of bounds")),
            ..QueueCommandOutcome::default()
        };
    }

    let mut playback_ops = vec![QueuePlaybackOp::PlayAt(idx)];
    if playback_state != PlaybackState::Playing {
        playback_ops.push(QueuePlaybackOp::Play);
    }
    *selected_queue_index = Some(idx);
    QueueCommandOutcome {
        changed: true,
        playback_ops,
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
    playback_state: PlaybackState,
) -> QueueCommandOutcome {
    match cmd {
        BridgeQueueCommand::Replace { tracks, autoplay } => replace_queue_command_outcome(
            queue,
            selected_queue_index,
            tracks,
            autoplay,
            playback_state,
        ),
        BridgeQueueCommand::Append(tracks) => append_queue_command_outcome(queue, tracks),
        BridgeQueueCommand::PlayAt(idx) => {
            play_at_queue_command_outcome(idx, queue.len(), selected_queue_index, playback_state)
        }
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
        // `handle_queue_command` undoes changes; see `undo_queue_change`.
        BridgeQueueCommand::Undo(_) => QueueCommandOutcome::default(),
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

    use crate::playback::PlaybackState;

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
            PlaybackState::Playing,
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
        let mut state = BridgeState::default();
        state.queue = vec![p("/missing/a.flac"), p("/missing/b.flac")];
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
            PlaybackState::Stopped,
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
            PlaybackState::Stopped,
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
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::PlayAt(3),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
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
            PlaybackState::Stopped,
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
            PlaybackState::Stopped,
        );
        assert!(!outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/b.flac")]);
        assert_eq!(selected, Some(1));
        assert!(outcome.playback_ops.is_empty());
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_replace_autoplay_loads_and_starts_playback() {
        let mut queue = Vec::new();
        let mut selected = None;
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a.flac"), p("/b.flac")],
                autoplay: true,
            },
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![
                QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")]),
                QueuePlaybackOp::PlayAt(0),
                QueuePlaybackOp::Play,
            ]
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
            PlaybackState::Stopped,
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
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(0),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
    }

    #[test]
    fn queue_remove_middle_track_uses_remove_op_and_keeps_reasonable_selection() {
        let mut queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let mut selected = Some(2);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(1),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert_eq!(queue, vec![p("/a.flac"), p("/c.flac")]);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::RemoveAt(1)]);
    }

    #[test]
    fn queue_remove_out_of_bounds_is_noop() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Remove(3),
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
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
            PlaybackState::Stopped,
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
            PlaybackState::Stopped,
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
            PlaybackState::Stopped,
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
            let update = state.queue_sync.update(&state.queue);
            if let Some(QueueUpdate::Edits { base_revision, .. }) = &update {
                assert_eq!(
                    *base_revision, ui_revision,
                    "an update since the UI's revision"
                );
            }
            if let Some(update) = &update {
                assert!(update.apply_to(&mut ui_queue));
            }
            state.queue_sync.mark_sent();
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
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Clear,
            &mut queue,
            &mut selected,
            PlaybackState::Stopped,
        );
        assert!(outcome.changed);
        assert!(queue.is_empty());
        assert!(selected.is_none());
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::ClearQueue]);
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_play_at_while_playing_skips_redundant_play_op() {
        let mut queue = vec![p("/a.flac"), p("/b.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::PlayAt(1),
            &mut queue,
            &mut selected,
            PlaybackState::Playing,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(1));
        assert_eq!(outcome.playback_ops, vec![QueuePlaybackOp::PlayAt(1)]);
        assert!(outcome.error.is_none());
    }

    #[test]
    fn queue_replace_autoplay_while_playing_skips_redundant_play_op() {
        let mut queue = vec![p("/old.flac")];
        let mut selected = Some(0);
        let outcome = apply_queue_command_state(
            BridgeQueueCommand::Replace {
                tracks: vec![p("/a.flac"), p("/b.flac")],
                autoplay: true,
            },
            &mut queue,
            &mut selected,
            PlaybackState::Playing,
        );
        assert!(outcome.changed);
        assert_eq!(selected, Some(0));
        assert_eq!(
            outcome.playback_ops,
            vec![
                QueuePlaybackOp::LoadQueue(vec![p("/a.flac"), p("/b.flac")]),
                QueuePlaybackOp::PlayAt(0),
            ]
        );
    }
}
