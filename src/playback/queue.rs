// SPDX-License-Identifier: GPL-3.0-or-later

//! Playback queue navigation shared by the playback backends.
//!
//! The queue distinguishes the committed track, which is audible, from
//! successors staged before the committed track ends. A staged successor
//! changes nothing until the backend commits it when its audio starts; a
//! cancelled successor leaves the queue as if it had never been staged.
//!
//! Files that failed to play are skipped by natural advances until the
//! backend clears them, so a failure cannot repeat in a loop.

use std::collections::{HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use super::play_next::{self, PlayNextRestore};
use super::{PathRenames, RepeatMode, TrackPick};

/// A queue position together with the shuffle navigation and random state
/// that led to it. An advance is planned on a copy, so committing installs
/// the copy and cancelling drops it.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct Cursor {
    index: usize,
    shuffle_history: Vec<usize>,
    shuffle_forward: Vec<usize>,
    shuffle_pool: Vec<usize>,
    rng_state: u64,
    /// How the track at `index` came to play. Repeat-One keeps it, and a
    /// change to the queue or its rules leaves it as it is, so the track
    /// keeps the gain that Auto mode chose for it.
    pick: TrackPick,
    /// The rows marked to play next, in the order they play. A natural
    /// advance and Next go to the first; a row leaves the list when the
    /// cursor goes to it.
    play_next: Arc<[usize]>,
    /// Without shuffle, each marked row the cursor went to, with the row it
    /// went from, latest last, so that Previous goes back through what
    /// played. Shuffle keeps them in its history.
    jumps: Vec<(usize, usize)>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Advance {
    Natural,
    NextManual,
    PreviousManual,
}

impl Cursor {
    fn rebuild_shuffle_pool(&mut self, len: usize) {
        self.shuffle_pool.clear();
        self.shuffle_pool
            .extend((0..len).filter(|index| *index != self.index));
    }

    fn clear_shuffle_navigation(&mut self) {
        self.shuffle_history.clear();
        self.shuffle_forward.clear();
        self.jumps.clear();
    }

    /// Take the row the cursor is at out of the list of rows to play next.
    fn unmark_current(&mut self) {
        if self.play_next.contains(&self.index) {
            let index = self.index;
            self.play_next = self
                .play_next
                .iter()
                .copied()
                .filter(|row| *row != index)
                .collect();
        }
    }

    /// Put each marked row where `remap` says, leaving out those it drops.
    fn remap_play_next(&mut self, remap: impl FnOnce(&[usize]) -> Vec<usize>) {
        if !self.play_next.is_empty() {
            self.play_next = remap(&self.play_next).into();
        }
    }

    /// Carry the jumps to marked rows to where `map` puts their rows. A
    /// jump `map` drops a row of ends the history: Previous reaches the
    /// jumps before it only through it.
    fn remap_jumps(&mut self, map: impl Fn(usize) -> Option<usize>) {
        let mut kept = Vec::with_capacity(self.jumps.len());
        for (from, to) in &self.jumps {
            match (map(*from), map(*to)) {
                (Some(from), Some(to)) => kept.push((from, to)),
                _ => kept.clear(),
            }
        }
        self.jumps = kept;
    }

    /// Restart navigation after the queue order changed. The jumps to
    /// marked rows stay, for the caller to carry with their rows.
    fn reset_navigation(&mut self, len: usize) {
        self.shuffle_history.clear();
        self.shuffle_forward.clear();
        self.rebuild_shuffle_pool(len);
    }

    fn ensure_rng_seed(&mut self) {
        if self.rng_state != 0 {
            return;
        }
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|duration| u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX))
            .unwrap_or(0x9E37_79B9_7F4A_7C15);
        self.rng_state = nanos ^ 0xA5A5_A5A5_5A5A_5A5A;
        if self.rng_state == 0 {
            self.rng_state = 0xD134_2543_DE82_EF95;
        }
    }

    fn next_rand(&mut self, max: usize) -> usize {
        self.ensure_rng_seed();
        self.rng_state = self
            .rng_state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1);
        usize::try_from(self.rng_state % u64::try_from(max).unwrap_or(u64::MAX)).unwrap_or(0)
    }

    /// Move the cursor and return whether a target exists.
    fn advance(&mut self, len: usize, rules: Rules, advance: Advance) -> bool {
        if len == 0 {
            return false;
        }
        match advance {
            // A marked row plays next, also with Repeat-One: marking it
            // asked for it.
            Advance::Natural | Advance::NextManual if !self.play_next.is_empty() => {
                self.advance_play_next(len, rules);
                true
            }
            Advance::Natural if rules.repeat_mode == RepeatMode::One => true,
            Advance::Natural | Advance::NextManual => {
                if rules.shuffle_enabled {
                    // With repeat-all, keep drawing from refreshed pools.
                    // With repeat-off, stop once the pool is exhausted.
                    self.advance_shuffle_or_forward(len, rules)
                } else {
                    self.advance_sequential(len, rules, true)
                }
            }
            Advance::PreviousManual => {
                if rules.shuffle_enabled {
                    if let Some(previous) = self.shuffle_history.pop() {
                        if previous != self.index {
                            self.shuffle_forward.push(self.index);
                        }
                        self.index = previous;
                        self.pick = TrackPick::Shuffle;
                        return true;
                    }
                } else if let Some(&(from, to)) = self.jumps.last() {
                    // Back from a marked row to the row that played before.
                    if to == self.index && from < len {
                        self.jumps.pop();
                        self.index = from;
                        self.pick = TrackPick::Other;
                        self.rebuild_shuffle_pool(len);
                        return true;
                    }
                }
                self.advance_sequential(len, rules, false)
            }
        }
    }

    /// Go to the first marked row, which leaves the list. It is no shuffle
    /// pick, also with shuffle on, and the queue's order goes on after it.
    fn advance_play_next(&mut self, len: usize, rules: Rules) {
        let next = self.play_next[0];
        self.play_next = Arc::from(&self.play_next[1..]);
        if rules.shuffle_enabled {
            if next != self.index {
                self.shuffle_forward.clear();
                self.shuffle_history.push(self.index);
            }
            // It counts as played for the shuffle's round.
            self.shuffle_pool.retain(|row| *row != next);
            self.index = next;
        } else {
            if next != self.index {
                self.jumps.push((self.index, next));
            }
            self.index = next;
            self.rebuild_shuffle_pool(len);
        }
        self.pick = TrackPick::Other;
    }

    fn advance_sequential(&mut self, len: usize, rules: Rules, forward: bool) -> bool {
        let target = if forward {
            if self.index + 1 < len {
                self.index + 1
            } else if rules.repeat_mode == RepeatMode::All {
                0
            } else {
                return false;
            }
        } else if self.index > 0 {
            self.index - 1
        } else if rules.repeat_mode == RepeatMode::All {
            len - 1
        } else {
            return false;
        };
        if target != self.index {
            // The only row, which plays again as it came to play, is no
            // move in the queue's order.
            self.pick = TrackPick::Other;
        }
        self.index = target;
        self.rebuild_shuffle_pool(len);
        true
    }

    fn advance_shuffle_or_forward(&mut self, len: usize, rules: Rules) -> bool {
        if let Some(next) = self.shuffle_forward.pop() {
            if next != self.index {
                self.shuffle_history.push(self.index);
            }
            self.index = next;
            self.pick = TrackPick::Shuffle;
            return true;
        }
        self.advance_shuffle_random(len, rules)
    }

    fn advance_shuffle_random(&mut self, len: usize, rules: Rules) -> bool {
        let allow_repeat_cycle = rules.repeat_mode == RepeatMode::All;
        if len == 1 {
            // The only row plays again, as it came to play: no pick.
            return allow_repeat_cycle || rules.repeat_mode == RepeatMode::One;
        }
        if self.shuffle_pool.is_empty() {
            if !allow_repeat_cycle {
                return false;
            }
            self.rebuild_shuffle_pool(len);
            if self.shuffle_pool.is_empty() {
                return false;
            }
        }
        let pick = self.next_rand(self.shuffle_pool.len());
        let next = self.shuffle_pool.remove(pick);
        if next != self.index {
            self.shuffle_forward.clear();
            self.shuffle_history.push(self.index);
        }
        self.index = next;
        self.pick = TrackPick::Shuffle;
        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Rules {
    repeat_mode: RepeatMode,
    shuffle_enabled: bool,
}

impl Rules {
    /// Whether the natural successor is a random pick rather than a
    /// position derived from the current track.
    fn random_successor(self) -> bool {
        self.shuffle_enabled && self.repeat_mode != RepeatMode::One
    }
}

/// How the pipeline starts a staged successor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Handoff {
    /// The pipeline was given the successor and switches to it without a
    /// gap. The successor is committed when its stream starts.
    Gapless,
    /// The pipeline finishes the current stream first. The successor is
    /// committed and started at end of stream.
    AtEos,
}

/// Compare lowercase file extensions to decide whether two tracks can
/// share a gapless decoder chain.  Conservative: any mismatch (including
/// missing extensions) falls back to the safe EOS-based switch.
pub(crate) fn same_audio_extension(a: &Path, b: &Path) -> bool {
    let ext_a = a
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_lowercase);
    let ext_b = b
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_lowercase);
    match (ext_a, ext_b) {
        (Some(a), Some(b)) => a == b,
        _ => false,
    }
}

/// A successor staged before the track ahead of it ends.
#[derive(Debug, Clone)]
struct PendingAdvance {
    path: PathBuf,
    cursor: Cursor,
    handoff: Handoff,
    staged_spectrogram: bool,
    random_successor: bool,
    /// The successor is the first marked row of the cursor before it.
    marked: bool,
}

/// The successor that `stage_natural` staged.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct StagedAdvance {
    pub(crate) path: PathBuf,
    pub(crate) handoff: Handoff,
    /// Ask analysis to prepare a spectrogram continuation. Only the
    /// gapless successor of the audible track is prepared.
    pub(crate) staged_spectrogram: bool,
    /// How the successor came to play.
    pub(crate) pick: TrackPick,
}

/// A staged successor whose audio has started.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct CommittedAdvance {
    pub(crate) path: PathBuf,
    pub(crate) index: usize,
}

/// The track to start after the pipeline reached end of stream.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct EosSuccessor {
    pub(crate) path: PathBuf,
    pub(crate) index: usize,
    /// Successors dropped or committed here, if any were staged.
    pub(crate) released: Option<CancelledAdvance>,
}

/// Staged successors dropped without being committed.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct CancelledAdvance {
    /// The pipeline had been given a dropped successor, so it must restart
    /// the committed track before playback continues.
    pub(crate) handed_to_pipeline: bool,
    /// Analysis had been asked to prepare a dropped successor.
    pub(crate) staged_spectrogram: bool,
}

impl CancelledAdvance {
    fn from_dropped<'a>(dropped: impl IntoIterator<Item = &'a PendingAdvance>) -> Option<Self> {
        dropped.into_iter().fold(None, |cancelled, pending| {
            let cancelled: Self = cancelled.unwrap_or_default();
            Some(Self {
                handed_to_pipeline: cancelled.handed_to_pipeline
                    || pending.handoff == Handoff::Gapless,
                staged_spectrogram: cancelled.staged_spectrogram || pending.staged_spectrogram,
            })
        })
    }

    pub(crate) fn merge(first: Option<Self>, second: Option<Self>) -> Option<Self> {
        match (first, second) {
            (Some(first), Some(second)) => Some(Self {
                handed_to_pipeline: first.handed_to_pipeline || second.handed_to_pipeline,
                staged_spectrogram: first.staged_spectrogram || second.staged_spectrogram,
            }),
            (cancelled, None) | (None, cancelled) => cancelled,
        }
    }
}

/// Result of a manual navigation command.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct ManualMove {
    pub(crate) path: PathBuf,
    pub(crate) index: usize,
    pub(crate) cancelled: Option<CancelledAdvance>,
}

/// Result of removing queue rows.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct Removal {
    /// The committed track's row was removed.
    pub(crate) current_removed: bool,
    pub(crate) cancelled: Option<CancelledAdvance>,
}

/// The play queue: the committed track, the shuffle and repeat rules, and
/// the successors staged ahead of the committed track.
#[cfg_attr(test, derive(Clone))]
pub(crate) struct GaplessQueue {
    queue: Vec<PathBuf>,
    cursor: Cursor,
    repeat_mode: RepeatMode,
    shuffle_enabled: bool,
    /// Successors in the order the pipeline plays them. A short successor
    /// can finish feeding the pipeline before it is audible, so more than
    /// one can be staged.
    pending: VecDeque<PendingAdvance>,
    /// Successors are staged for end of stream instead of being handed to
    /// the pipeline, because a seek in progress would reach them.
    handoffs_held: bool,
    /// Files that failed to play. Natural advances skip rows with these
    /// paths; a renamed row has a new path and is tried again.
    failed: HashSet<PathBuf>,
}

impl GaplessQueue {
    pub(crate) fn new() -> Self {
        Self {
            queue: Vec::new(),
            cursor: Cursor::default(),
            repeat_mode: RepeatMode::Off,
            shuffle_enabled: false,
            pending: VecDeque::new(),
            handoffs_held: false,
            failed: HashSet::new(),
        }
    }

    /// Seed the shuffle's random picks, so a test can repeat them.
    #[cfg(test)]
    pub(crate) fn seed_shuffle(&mut self, seed: u64) {
        self.cursor.rng_state = seed;
    }

    fn rules(&self) -> Rules {
        Rules {
            repeat_mode: self.repeat_mode,
            shuffle_enabled: self.shuffle_enabled,
        }
    }

    pub(crate) fn repeat_mode(&self) -> RepeatMode {
        self.repeat_mode
    }

    pub(crate) fn shuffle_enabled(&self) -> bool {
        self.shuffle_enabled
    }

    pub(crate) fn current(&self) -> Option<PathBuf> {
        self.queue.get(self.cursor.index).cloned()
    }

    pub(crate) fn current_index(&self) -> Option<usize> {
        (!self.queue.is_empty()).then_some(self.cursor.index)
    }

    /// How the committed track came to play. The simulated backend applies
    /// no gain.
    #[cfg(any(test, feature = "gst"))]
    pub(crate) fn current_pick(&self) -> TrackPick {
        self.cursor.pick
    }

    /// The rows marked to play next, in the order they play.
    pub(crate) fn play_next(&self) -> Arc<[usize]> {
        Arc::clone(&self.cursor.play_next)
    }

    /// Mark `rows` to play next, or unmark them when every one is marked;
    /// see [`play_next::toggled`]. Staged successors that no longer follow
    /// are dropped.
    pub(crate) fn toggle_play_next(&mut self, rows: &[usize]) -> Option<CancelledAdvance> {
        let list = play_next::toggled(&self.cursor.play_next, rows, self.queue.len());
        self.set_play_next(list)
    }

    /// Mark `rows` to play next, in this order, in place of the marks.
    pub(crate) fn set_play_next(&mut self, rows: Vec<usize>) -> Option<CancelledAdvance> {
        let list = play_next::sanitized(rows, self.queue.len());
        if *self.cursor.play_next == *list {
            return None;
        }
        self.cursor.play_next = list.into();
        self.revalidate_pending()
    }

    /// Path of the row at `index`.
    #[cfg(feature = "gst")]
    pub(crate) fn path_at(&self, index: usize) -> Option<&Path> {
        self.queue.get(index).map(PathBuf::as_path)
    }

    /// Whether the pipeline was given a successor that has not started.
    /// Once the decoder links that successor, before it is audible, the
    /// pipeline's duration query describes the successor.
    #[cfg(any(test, feature = "gst"))]
    pub(crate) fn successor_handed_to_pipeline(&self) -> bool {
        self.pending
            .iter()
            .any(|pending| pending.handoff == Handoff::Gapless)
    }

    /// Current path of the track at `position` in the pipeline's play
    /// order: 0 for the committed track, then each successor handed to the
    /// pipeline. A rename since the pipeline was given the track shows here.
    #[cfg(any(test, feature = "gst"))]
    pub(crate) fn path_in_pipeline(&self, position: usize) -> Option<PathBuf> {
        let Some(successor) = position.checked_sub(1) else {
            return self.current();
        };
        self.pending
            .iter()
            .filter(|pending| pending.handoff == Handoff::Gapless)
            .nth(successor)
            .map(|pending| pending.path.clone())
    }

    /// Until `release_handoffs`, stage successors for end of stream rather
    /// than for the pipeline. playbin3 applies a seek to a successor it was
    /// given, so a seek holds handoffs until it has been applied.
    pub(crate) fn hold_handoffs(&mut self) {
        self.handoffs_held = true;
    }

    pub(crate) fn release_handoffs(&mut self) {
        self.handoffs_held = false;
    }

    // The simulated backend has no failures.
    #[cfg(any(test, feature = "gst"))]
    /// Skip `path` in natural advances until `clear_failed`.
    pub(crate) fn mark_failed(&mut self, path: PathBuf) {
        self.failed.insert(path);
    }

    // The simulated backend has no failures.
    #[cfg(any(test, feature = "gst"))]
    pub(crate) fn clear_failed(&mut self) {
        self.failed.clear();
    }

    // The simulated backend has no failures.
    #[cfg(any(test, feature = "gst"))]
    /// Move from the committed track, which failed, to its natural
    /// successor that has not failed, dropping staged successors. Returns
    /// `None` and stays when no such track is left; with repeat-one, the
    /// failed track is the only natural successor.
    pub(crate) fn skip_failed_current(&mut self) -> Option<ManualMove> {
        let (cursor, _) = self.natural_successor(&self.cursor)?;
        let path = self.queue.get(cursor.index)?.clone();
        let cancelled = self.cancel_pending();
        self.cursor = cursor;
        Some(ManualMove {
            path,
            index: self.cursor.index,
            cancelled,
        })
    }

    /// Drop every staged successor so the committed track stays current.
    pub(crate) fn cancel_pending(&mut self) -> Option<CancelledAdvance> {
        let cancelled = CancelledAdvance::from_dropped(&self.pending);
        self.pending.clear();
        cancelled
    }

    pub(crate) fn set_queue(&mut self, queue: Vec<PathBuf>) -> Option<CancelledAdvance> {
        let cancelled = self.cancel_pending();
        self.queue = queue;
        self.cursor.index = 0;
        self.cursor.pick = TrackPick::Other;
        self.cursor.play_next = Arc::default();
        self.cursor.jumps.clear();
        self.cursor.reset_navigation(self.queue.len());
        cancelled
    }

    /// Replace the queue with `queue`, in which the committed track is row
    /// `current`, such as when an undo puts removed rows back around it,
    /// with the marks `marks` brings back. Staged successors stay while
    /// they are still the tracks that would follow; the rest are dropped.
    pub(crate) fn restore(
        &mut self,
        queue: Vec<PathBuf>,
        current: usize,
        marks: &PlayNextRestore,
    ) -> Option<CancelledAdvance> {
        let list = play_next::restored(&self.cursor.play_next, marks, queue.len());
        // Rows put back keep the rows around them, and the jumps between.
        let stayed = play_next::reinserted_rows(marks, queue.len());
        self.queue = queue;
        self.cursor.index = current;
        self.cursor.play_next = list.into();
        let random = self.rules().random_successor();
        let mut previous = current;
        let mut marked = self.cursor.play_next.iter().copied();
        let mut rows = Vec::with_capacity(self.pending.len());
        for pending in &self.pending {
            let row = if let Some(stayed) = &stayed {
                // The row it was staged as, among the rows put back. When
                // another row comes next now, such as a row put back that
                // holds the same file, revalidation finds it.
                stayed.get(pending.cursor.index).copied()
            } else if pending.marked {
                marked.find(|row| !self.row_failed(*row))
            } else if random {
                self.queue.iter().position(|path| *path == pending.path)
            } else {
                self.sequential_successor(previous)
            };
            // No row fails the check below, which drops the successor.
            let row = row.unwrap_or(usize::MAX);
            rows.push(row);
            previous = row;
        }
        for (pending, row) in self.pending.iter_mut().zip(rows) {
            pending.cursor.index = row;
        }
        let len = self.queue.len();
        self.for_each_cursor(|cursor| {
            match &stayed {
                Some(stayed) => cursor.remap_jumps(|row| stayed.get(row).copied()),
                None => cursor.jumps.clear(),
            }
            cursor.reset_navigation(len);
        });
        self.revalidate_pending()
    }

    /// Rename entries in place, including staged successors. Indices and
    /// shuffle history stay valid.
    pub(crate) fn remap_paths(&mut self, renames: &PathRenames) {
        let staged = self.pending.iter_mut().map(|pending| &mut pending.path);
        for path in self.queue.iter_mut().chain(staged) {
            super::remap_path(path, renames);
        }
    }

    pub(crate) fn add_to_queue(&mut self, items: Vec<PathBuf>) -> Option<CancelledAdvance> {
        self.queue.extend(items);
        let len = self.queue.len();
        self.for_each_cursor(|cursor| cursor.rebuild_shuffle_pool(len));
        self.revalidate_pending()
    }

    pub(crate) fn clear(&mut self) -> Option<CancelledAdvance> {
        let cancelled = self.cancel_pending();
        self.queue.clear();
        self.cursor = Cursor {
            rng_state: self.cursor.rng_state,
            ..Cursor::default()
        };
        cancelled
    }

    /// Make `idx` current. Returns `None` and keeps any staged successor
    /// when `idx` is out of range.
    pub(crate) fn set_current(&mut self, idx: usize) -> Option<ManualMove> {
        let path = self.queue.get(idx).cloned()?;
        let cancelled = self.cancel_pending();
        self.cursor.index = idx;
        // The user's pick, also with shuffle on.
        self.cursor.pick = TrackPick::Other;
        self.cursor.unmark_current();
        self.cursor.jumps.clear();
        self.cursor.reset_navigation(self.queue.len());
        Some(ManualMove {
            path,
            index: idx,
            cancelled,
        })
    }

    /// Make `idx` current as a jump to a track found in the queue, as
    /// [`Self::set_current`] does, but within the shuffle's round: with
    /// shuffle on, the row counts as played for the round, the rows played
    /// before stay out of it, and Previous goes back to the track that was
    /// current. Without shuffle it is [`Self::set_current`]. Returns `None`
    /// and keeps any staged successor when `idx` is out of range.
    pub(crate) fn jump_to(&mut self, idx: usize) -> Option<ManualMove> {
        if !self.shuffle_enabled {
            return self.set_current(idx);
        }
        let path = self.queue.get(idx).cloned()?;
        let cancelled = self.cancel_pending();
        let cursor = &mut self.cursor;
        if idx != cursor.index {
            cursor.shuffle_forward.clear();
            cursor.shuffle_history.push(cursor.index);
        }
        cursor.shuffle_pool.retain(|row| *row != idx);
        cursor.index = idx;
        // The user's pick, also with shuffle on.
        cursor.pick = TrackPick::Other;
        cursor.unmark_current();
        Some(ManualMove {
            path,
            index: idx,
            cancelled,
        })
    }

    pub(crate) fn remove_many(&mut self, indices: &[usize]) -> Removal {
        let len = self.queue.len();
        let mut removed: Vec<usize> = indices.iter().copied().filter(|i| *i < len).collect();
        removed.sort_unstable();
        removed.dedup();
        if removed.is_empty() {
            return Removal::default();
        }
        let current_removed = removed.binary_search(&self.cursor.index).is_ok();
        let cancelled = if current_removed {
            self.cancel_pending()
        } else {
            self.pending
                .iter()
                .position(|pending| removed.binary_search(&pending.cursor.index).is_ok())
                .and_then(|cut| self.drop_pending_from(cut))
        };
        for pending in &mut self.pending {
            pending.cursor.index -= removed.partition_point(|r| *r < pending.cursor.index);
        }
        self.for_each_cursor(|cursor| {
            cursor.remap_play_next(|list| play_next::after_removal(list, &removed));
            cursor.remap_jumps(|row| {
                removed
                    .binary_search(&row)
                    .is_err()
                    .then(|| row - removed.partition_point(|removed| *removed < row))
            });
        });
        self.cursor.index =
            super::remove_queue_indices(&mut self.queue, Some(self.cursor.index), &removed)
                .unwrap_or(0);
        if current_removed {
            // The row that takes the removed one's place, in queue order,
            // which starts.
            self.cursor.pick = TrackPick::Other;
            self.cursor.unmark_current();
        }
        let len = self.queue.len();
        self.for_each_cursor(|cursor| cursor.reset_navigation(len));
        Removal {
            current_removed,
            cancelled: CancelledAdvance::merge(cancelled, self.revalidate_pending()),
        }
    }

    pub(crate) fn move_item(&mut self, from: usize, to: usize) -> Option<CancelledAdvance> {
        let len = self.queue.len();
        if from >= len || to >= len || from == to {
            return None;
        }
        let item = self.queue.remove(from);
        self.queue.insert(to, item);
        let remap = |index: usize| {
            if index == from {
                to
            } else if from < index && to >= index {
                index - 1
            } else if from > index && to <= index {
                index + 1
            } else {
                index
            }
        };
        self.for_each_cursor(|cursor| {
            cursor.index = remap(cursor.index);
            cursor.remap_play_next(|list| list.iter().map(|row| remap(*row)).collect());
            cursor.remap_jumps(|row| Some(remap(row)));
            cursor.reset_navigation(len);
        });
        self.revalidate_pending()
    }

    /// Put the rows in `order`, which names for each new row the row it was
    /// before. Every cursor follows its row, so the committed track keeps
    /// its identity even when the queue holds its path more than once.
    /// Returns `None`, changing nothing, unless `order` names every row once.
    pub(crate) fn reorder(&mut self, order: &[usize]) -> Option<CancelledAdvance> {
        let len = self.queue.len();
        let positions = super::reorder_positions(order, len)?;
        let mut old = std::mem::take(&mut self.queue);
        self.queue = order
            .iter()
            .map(|index| std::mem::take(&mut old[*index]))
            .collect();
        self.for_each_cursor(|cursor| {
            if let Some(position) = positions.get(cursor.index) {
                cursor.index = *position;
            }
            cursor.remap_play_next(|list| play_next::after_reorder(list, &positions));
            cursor.remap_jumps(|row| positions.get(row).copied());
            cursor.reset_navigation(len);
        });
        self.revalidate_pending()
    }

    /// Insert `items` before row `at`, or after the last row when `at` is
    /// past it. Rows from `at` on, and their cursors, move down.
    pub(crate) fn insert(&mut self, at: usize, items: Vec<PathBuf>) -> Option<CancelledAdvance> {
        if items.is_empty() {
            return None;
        }
        let at = at.min(self.queue.len());
        let count = items.len();
        let was_empty = self.queue.is_empty();
        self.queue.splice(at..at, items);
        let len = self.queue.len();
        self.for_each_cursor(|cursor| {
            if !was_empty && cursor.index >= at {
                cursor.index += count;
            }
            cursor.remap_play_next(|list| play_next::after_insert(list, at, count));
            cursor.remap_jumps(|row| Some(if row >= at { row + count } else { row }));
            cursor.reset_navigation(len);
        });
        self.revalidate_pending()
    }

    pub(crate) fn set_repeat_mode(&mut self, mode: RepeatMode) -> Option<CancelledAdvance> {
        self.repeat_mode = mode;
        self.revalidate_pending()
    }

    pub(crate) fn set_shuffle_enabled(&mut self, enabled: bool) -> Option<CancelledAdvance> {
        if self.shuffle_enabled == enabled {
            return None;
        }
        self.shuffle_enabled = enabled;
        let len = self.queue.len();
        self.for_each_cursor(|cursor| {
            cursor.clear_shuffle_navigation();
            if enabled {
                cursor.rebuild_shuffle_pool(len);
            } else {
                cursor.shuffle_pool.clear();
            }
        });
        self.revalidate_pending()
    }

    pub(crate) fn next_manual(&mut self) -> Option<ManualMove> {
        self.move_manually(Advance::NextManual)
    }

    pub(crate) fn previous_manual(&mut self) -> Option<ManualMove> {
        self.move_manually(Advance::PreviousManual)
    }

    /// Move from the committed track. A staged successor is dropped only
    /// when the move happens.
    fn move_manually(&mut self, advance: Advance) -> Option<ManualMove> {
        let mut cursor = self.cursor.clone();
        if !cursor.advance(self.queue.len(), self.rules(), advance) {
            return None;
        }
        // A marked row that Previous goes back to plays, so it leaves the
        // list too.
        cursor.unmark_current();
        let path = self.queue.get(cursor.index).cloned()?;
        let cancelled = self.cancel_pending();
        self.cursor = cursor;
        Some(ManualMove {
            path,
            index: self.cursor.index,
            cancelled,
        })
    }

    /// Cursor of the track the pipeline is fed last: the newest staged
    /// successor, or the committed track.
    fn feeding(&self) -> &Cursor {
        self.pending
            .back()
            .map_or(&self.cursor, |pending| &pending.cursor)
    }

    /// The cursor of the natural successor of `from` that has not failed,
    /// and whether it is a marked row.
    ///
    /// The search ends once no row that a natural advance can still reach
    /// is left unfailed. A shuffled advance can revisit a failed row when
    /// it refreshes its pool, so a fixed number of advances is no bound.
    fn natural_successor(&self, from: &Cursor) -> Option<(Cursor, bool)> {
        let len = self.queue.len();
        let rules = self.rules();
        let mut cursor = from.clone();
        // While a reachable row has not failed, this many advances reach
        // one: the marked rows, the shuffle navigation and pool of `from`,
        // then at most two refreshed pools, which leave out only the row
        // they start from. A sequential advance needs at most `len`.
        let steps =
            from.play_next.len() + from.shuffle_forward.len() + from.shuffle_pool.len() + 2 * len;
        for _ in 0..steps {
            let marked = !cursor.play_next.is_empty();
            if !self.reachable_row_not_failed(&cursor)
                || !cursor.advance(len, rules, Advance::Natural)
            {
                return None;
            }
            if !self.row_failed(cursor.index) {
                if marked {
                    return Some((cursor, true));
                }
                if !rules.shuffle_enabled && cursor.index == from.index {
                    // Past failed rows, back to the row it started from,
                    // which plays again as it came to play.
                    cursor.pick = from.pick;
                }
                return Some((cursor, false));
            }
        }
        None
    }

    fn row_failed(&self, index: usize) -> bool {
        self.queue
            .get(index)
            .is_none_or(|path| self.failed.contains(path))
    }

    /// Whether natural advances from `cursor` can reach a row that has
    /// not failed.
    fn reachable_row_not_failed(&self, cursor: &Cursor) -> bool {
        let not_failed = |index: &usize| !self.row_failed(*index);
        if cursor.play_next.iter().any(not_failed) {
            return true;
        }
        match self.repeat_mode {
            RepeatMode::One => not_failed(&cursor.index),
            RepeatMode::All => (0..self.queue.len()).any(|index| not_failed(&index)),
            RepeatMode::Off if self.shuffle_enabled => cursor
                .shuffle_forward
                .iter()
                .chain(&cursor.shuffle_pool)
                .any(not_failed),
            RepeatMode::Off => (cursor.index + 1..self.queue.len()).any(|index| not_failed(&index)),
        }
    }

    /// Stage the natural successor of the track the pipeline is fed last.
    /// `handoff` receives the finishing and successor paths.
    pub(crate) fn stage_natural(
        &mut self,
        handoff: impl FnOnce(&Path, &Path) -> Handoff,
    ) -> Option<StagedAdvance> {
        let rules = self.rules();
        let feeding = self.feeding();
        let finishing = self.queue.get(feeding.index)?.clone();
        let (cursor, marked) = self.natural_successor(feeding)?;
        let path = self.queue.get(cursor.index)?.clone();
        let handoff = if self.handoffs_held {
            Handoff::AtEos
        } else {
            handoff(&finishing, &path)
        };
        let staged_spectrogram = handoff == Handoff::Gapless && self.pending.is_empty();
        let pick = cursor.pick;
        self.pending.push_back(PendingAdvance {
            path: path.clone(),
            cursor,
            handoff,
            staged_spectrogram,
            random_successor: rules.random_successor(),
            marked,
        });
        Some(StagedAdvance {
            path,
            handoff,
            staged_spectrogram,
            pick,
        })
    }

    /// Commit the oldest staged successor when the pipeline started it
    /// without a gap. Returns `None` when no such successor is staged.
    pub(crate) fn commit_gapless(&mut self) -> Option<CommittedAdvance> {
        if self.pending.front()?.handoff != Handoff::Gapless {
            return None;
        }
        let pending = self.pending.pop_front()?;
        self.cursor = pending.cursor;
        Some(CommittedAdvance {
            path: pending.path,
            index: self.cursor.index,
        })
    }

    /// Commit the track to start after end of stream: the oldest staged
    /// successor, or the natural successor when none was staged, such as
    /// a track appended after the pipeline asked for one.
    pub(crate) fn take_eos_successor(&mut self) -> Option<EosSuccessor> {
        if let Some(pending) = self.pending.pop_front() {
            let released = CancelledAdvance::from_dropped(
                std::iter::once(&pending).chain(self.pending.iter()),
            );
            let _ = self.cancel_pending();
            self.cursor = pending.cursor;
            return Some(EosSuccessor {
                path: pending.path,
                index: self.cursor.index,
                released,
            });
        }
        let (cursor, _) = self.natural_successor(&self.cursor)?;
        let path = self.queue.get(cursor.index)?.clone();
        self.cursor = cursor;
        Some(EosSuccessor {
            path,
            index: self.cursor.index,
            released: None,
        })
    }

    fn for_each_cursor(&mut self, mut apply: impl FnMut(&mut Cursor)) {
        apply(&mut self.cursor);
        for pending in &mut self.pending {
            apply(&mut pending.cursor);
        }
    }

    /// Keep the staged successors that the current queue, marks and
    /// settings would still choose, in order, and drop the rest. Those kept
    /// take the marks left after them.
    ///
    /// A successor that would now come to play another way, such as the
    /// next row in the queue's order once it is marked, or another row
    /// that holds its file, stays when it is still the same file by the
    /// same pick, the pick the gain stage was given with it: it takes the
    /// cursor it would be staged with now, and so do the successors after
    /// it.
    fn revalidate_pending(&mut self) -> Option<CancelledAdvance> {
        let rules = self.rules();
        let marks = Arc::clone(&self.cursor.play_next);
        let mut previous = self.cursor.index;
        // Where the marks left after each successor start.
        let mut rest = 0;
        let mut restaged = false;
        let mut cut = None;
        for position in 0..self.pending.len() {
            let pending = &self.pending[position];
            // The first marked row that has not failed comes next, if any.
            let first_mark = marks[rest..]
                .iter()
                .position(|row| !self.row_failed(*row))
                .map(|skipped| rest + skipped);
            let follows = if let Some(mark) = first_mark {
                rest = mark + 1;
                pending.marked && pending.cursor.index == marks[mark]
            } else {
                rest = marks.len();
                !pending.marked
                    && if rules.random_successor() {
                        pending.random_successor
                    } else {
                        self.sequential_successor(previous) == Some(pending.cursor.index)
                    }
            };
            let in_place = self.queue.get(pending.cursor.index) == Some(&pending.path);
            if !in_place {
                cut = Some(position);
                break;
            }
            if follows && !restaged {
                let pending = &mut self.pending[position];
                if pending.cursor.play_next[..] != marks[rest..] {
                    pending.cursor.play_next = Arc::from(&marks[rest..]);
                }
            } else if self.restage(position) {
                restaged = true;
            } else {
                cut = Some(position);
                break;
            }
            previous = self.pending[position].cursor.index;
        }
        self.drop_pending_from(cut?)
    }

    /// Give the successor at `position` the cursor that the track before it
    /// would stage it with now, when that is the same file by the same
    /// pick. Returns whether it did.
    fn restage(&mut self, position: usize) -> bool {
        let rules = self.rules();
        let from = position
            .checked_sub(1)
            .map_or(&self.cursor, |before| &self.pending[before].cursor);
        let Some((cursor, marked)) = self.natural_successor(from) else {
            return false;
        };
        // The pipeline plays the file it was given, so another row that
        // holds it can take its place.
        if self.queue.get(cursor.index) != Some(&self.pending[position].path)
            || cursor.pick != self.pending[position].cursor.pick
        {
            return false;
        }
        let pending = &mut self.pending[position];
        pending.cursor = cursor;
        pending.marked = marked;
        pending.random_successor = rules.random_successor();
        true
    }

    /// Drop the successors from `cut` onwards. The pipeline can drop a
    /// successor it was given only by restarting the committed track, which
    /// drops every staged successor, so dropping one such successor drops
    /// them all.
    fn drop_pending_from(&mut self, cut: usize) -> Option<CancelledAdvance> {
        let cancelled = CancelledAdvance::from_dropped(self.pending.range(cut..));
        if cancelled.is_some_and(|cancelled| cancelled.handed_to_pipeline) {
            return self.cancel_pending();
        }
        self.pending.truncate(cut);
        cancelled
    }

    /// The row that a sequential natural advance from `index` reaches,
    /// skipping failed files like `natural_successor`.
    fn sequential_successor(&self, index: usize) -> Option<usize> {
        let len = self.queue.len();
        let mut index = index;
        for _ in 0..len {
            index = if index >= len {
                return None;
            } else if self.repeat_mode == RepeatMode::One {
                index
            } else if index + 1 < len {
                index + 1
            } else if self.repeat_mode == RepeatMode::All {
                0
            } else {
                return None;
            };
            if !self.failed.contains(&self.queue[index]) {
                return Some(index);
            }
            if self.repeat_mode == RepeatMode::One {
                return None;
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use std::path::{Path, PathBuf};

    use super::{
        same_audio_extension, Advance, CancelledAdvance, GaplessQueue, Handoff, PathRenames,
        PlayNextRestore, RepeatMode, TrackPick,
    };

    fn paths(count: usize) -> Vec<PathBuf> {
        (0..count)
            .map(|index| PathBuf::from(format!("/fixture/{index}.flac")))
            .collect()
    }

    fn queue_of(count: usize) -> (GaplessQueue, Vec<PathBuf>) {
        let paths = paths(count);
        let mut queue = GaplessQueue::new();
        assert_eq!(queue.set_queue(paths.clone()), None);
        (queue, paths)
    }

    fn gapless(_: &Path, _: &Path) -> Handoff {
        Handoff::Gapless
    }

    const HANDED: Option<CancelledAdvance> = Some(CancelledAdvance {
        handed_to_pipeline: true,
        staged_spectrogram: true,
    });

    fn committed_index(queue: &mut GaplessQueue) -> Option<usize> {
        queue.commit_gapless().map(|committed| committed.index)
    }

    #[test]
    fn staged_successor_changes_nothing_until_committed() {
        let (mut queue, paths) = queue_of(3);
        let staged = queue.stage_natural(gapless).expect("successor");
        assert_eq!(staged.path, paths[1]);
        assert!(staged.staged_spectrogram);
        assert_eq!(queue.current_index(), Some(0));
        assert_eq!(queue.current(), Some(paths[0].clone()));

        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (paths[1].clone(), 1));
        assert!(!queue.successor_handed_to_pipeline());
        assert_eq!(queue.current_index(), Some(1));
        assert!(queue.pending.is_empty());
    }

    #[test]
    fn each_track_keeps_how_it_came_to_play() {
        let (mut queue, _) = queue_of(6);
        queue.seed_shuffle(7);
        assert_eq!(queue.current_pick(), TrackPick::Other);
        // In the queue's order.
        assert_eq!(queue.stage_natural(gapless).unwrap().pick, TrackPick::Other);
        queue.commit_gapless();
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // Turning shuffle on leaves the current track as it came to play;
        // the next one is a shuffle pick.
        queue.set_shuffle_enabled(true);
        assert_eq!(queue.current_pick(), TrackPick::Other);
        assert_eq!(
            queue.stage_natural(gapless).unwrap().pick,
            TrackPick::Shuffle
        );
        queue.commit_gapless();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);

        // Repeat-One plays it again as it first came to play.
        queue.set_repeat_mode(RepeatMode::One);
        let current = queue.current_index();
        let again = queue.stage_natural(gapless).unwrap();
        assert_eq!(again.pick, TrackPick::Shuffle);
        assert_eq!(committed_index(&mut queue), current);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);

        // Turning shuffle off, or editing the queue, leaves it too; the
        // next track in order is not a shuffle pick.
        queue.set_repeat_mode(RepeatMode::All);
        queue.set_shuffle_enabled(false);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        let current = queue.current_index().unwrap();
        let other = (current + 3) % 6;
        queue.move_item(other, (other + 1) % 6);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        assert_eq!(queue.stage_natural(gapless).unwrap().pick, TrackPick::Other);
        queue.commit_gapless();
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // A successor dropped before it starts leaves the current track's.
        queue.set_shuffle_enabled(true);
        assert_eq!(
            queue.stage_natural(gapless).unwrap().pick,
            TrackPick::Shuffle
        );
        assert_eq!(queue.cancel_pending(), HANDED);
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // A shuffle pick that starts at end of stream keeps its pick.
        let staged = queue.stage_natural(|_, _| Handoff::AtEos).unwrap();
        assert_eq!(staged.pick, TrackPick::Shuffle);
        queue.take_eos_successor().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
    }

    #[test]
    fn the_only_row_plays_again_as_it_came_to_play() {
        let (mut queue, _) = queue_of(1);
        queue.set_shuffle_enabled(true);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(queue.stage_natural(gapless).unwrap().pick, TrackPick::Other);
        // Repeat-One keeps that replay, which keeps the first pass's pick.
        assert_eq!(queue.set_repeat_mode(RepeatMode::One), None);
        queue.commit_gapless().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Other);
        assert_eq!(queue.next_manual().unwrap().index, 0);
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // A shuffle pick left as the only row, with shuffle off, plays again
        // as a shuffle pick too.
        let (mut queue, _) = queue_of(3);
        queue.set_shuffle_enabled(true);
        let picked = queue.next_manual().unwrap().index;
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        let others: Vec<usize> = (0..3).filter(|row| *row != picked).collect();
        assert!(!queue.remove_many(&others).current_removed);
        queue.set_shuffle_enabled(false);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(
            queue.stage_natural(gapless).unwrap().pick,
            TrackPick::Shuffle
        );
        assert_eq!(queue.set_repeat_mode(RepeatMode::One), None);
        queue.commit_gapless().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(queue.previous_manual().unwrap().index, 0);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);

        // So does one whose other rows failed: the search steps past them
        // back to it.
        let (mut queue, paths) = queue_of(3);
        queue.set_shuffle_enabled(true);
        let picked = queue.next_manual().unwrap().index;
        for (row, path) in paths.iter().enumerate() {
            if row != picked {
                queue.mark_failed(path.clone());
            }
        }
        queue.set_shuffle_enabled(false);
        queue.set_repeat_mode(RepeatMode::All);
        let replay = queue.stage_natural(gapless).unwrap();
        assert_eq!(replay.path, paths[picked]);
        assert_eq!(replay.pick, TrackPick::Shuffle);
        assert_eq!(queue.set_repeat_mode(RepeatMode::One), None);
        queue.commit_gapless().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
    }

    #[test]
    fn a_track_the_user_picks_is_no_shuffle_pick_also_with_shuffle_on() {
        let (mut queue, paths) = queue_of(6);
        queue.set_shuffle_enabled(true);
        queue.next_manual().unwrap();
        assert_eq!(
            queue.current_pick(),
            TrackPick::Shuffle,
            "Next picks at random"
        );

        queue.set_current(3).unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Other);

        queue.next_manual().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        let _ = queue.set_queue(paths.clone());
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // Removing the current track plays the row in its place.
        queue.next_manual().unwrap();
        let removal = queue.remove_many(&[queue.current_index().unwrap()]);
        assert!(removal.current_removed);
        assert_eq!(queue.current_pick(), TrackPick::Other);

        queue.next_manual().unwrap();
        let _ = queue.clear();
        assert_eq!(queue.current_pick(), TrackPick::Other);
    }

    #[test]
    fn steps_through_shuffle_history_are_shuffle_picks_and_steps_in_order_are_not() {
        let (mut queue, _) = queue_of(6);
        // Previous from the first row wraps to the last.
        queue.set_repeat_mode(RepeatMode::All);
        queue.set_shuffle_enabled(true);
        queue.set_current(2).unwrap();
        let picked = queue.next_manual().unwrap().index;
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);

        // Back through the history, then forward again.
        assert_eq!(queue.previous_manual().unwrap().index, 2);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        assert_eq!(queue.next_manual().unwrap().index, picked);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);

        // Previous with no history left steps back in the queue's order.
        assert_eq!(queue.previous_manual().unwrap().index, 2);
        let in_order = queue.previous_manual().unwrap();
        assert_eq!(in_order.index, 1);
        assert_eq!(queue.current_pick(), TrackPick::Other);

        queue.set_shuffle_enabled(false);
        queue.next_manual().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Other);
        queue.set_shuffle_enabled(true);
        queue.next_manual().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        queue.set_shuffle_enabled(false);
        queue.previous_manual().unwrap();
        assert_eq!(queue.current_pick(), TrackPick::Other);
    }

    #[test]
    fn manual_navigation_inside_the_window_starts_from_the_audible_track() {
        let (mut queue, paths) = queue_of(3);
        queue.stage_natural(gapless);
        let next = queue.next_manual().expect("next");
        assert_eq!((next.path, next.index), (paths[1].clone(), 1));
        assert_eq!(next.cancelled, HANDED);
        assert!(queue.pending.is_empty());

        queue.stage_natural(gapless);
        let previous = queue.previous_manual().expect("previous");
        assert_eq!((previous.path, previous.index), (paths[0].clone(), 0));
        assert_eq!(previous.cancelled, HANDED);
    }

    #[test]
    fn navigation_that_does_not_move_keeps_the_staged_successor() {
        let (mut queue, paths) = queue_of(2);
        queue.set_current(1);
        queue.set_repeat_mode(RepeatMode::One);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[1]);
        assert_eq!(queue.next_manual(), None);
        assert_eq!(queue.set_current(9), None);
        assert_eq!(committed_index(&mut queue), Some(1));
    }

    #[test]
    fn cancelled_shuffle_advance_leaves_navigation_as_before() {
        let (mut queue, _) = queue_of(8);
        queue.set_shuffle_enabled(true);
        queue.set_repeat_mode(RepeatMode::All);
        queue.next_manual();
        queue.next_manual();
        let before = queue.cursor.clone();

        let first = queue.stage_natural(gapless).expect("shuffle successor");
        assert_eq!(queue.cancel_pending(), HANDED);
        assert_eq!(queue.cursor, before);
        let second = queue.stage_natural(gapless).expect("shuffle successor");
        assert_eq!(second.path, first.path, "the random state is restored too");

        let mut sequential = queue.clone();
        let _ = sequential.cancel_pending();
        let expected = sequential.take_eos_successor().expect("natural successor");
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!(committed.index, expected.index);
        assert_eq!(queue.cursor, sequential.cursor);
    }

    #[test]
    fn previous_after_committed_shuffle_advance_returns_to_history() {
        let (mut queue, paths) = queue_of(3);
        queue.set_shuffle_enabled(true);
        let staged = queue.stage_natural(gapless).expect("successor");
        queue.commit_gapless();
        assert_ne!(staged.path, paths[0]);
        assert_eq!(queue.previous_manual().unwrap().path, paths[0]);
        assert_eq!(queue.next_manual().unwrap().path, staged.path);
    }

    #[test]
    fn removing_other_rows_keeps_and_remaps_the_staged_successor() {
        let (mut queue, paths) = queue_of(5);
        queue.set_current(1);
        queue.stage_natural(gapless);
        let removal = queue.remove_many(&[0, 4]);
        assert!(!removal.current_removed);
        assert_eq!(removal.cancelled, None);
        assert_eq!(queue.current_index(), Some(0));
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (paths[2].clone(), 1));
    }

    #[test]
    fn removing_the_staged_successor_cancels_it() {
        let (mut queue, paths) = queue_of(3);
        queue.stage_natural(gapless);
        let removal = queue.remove_many(&[1]);
        assert!(!removal.current_removed);
        assert_eq!(removal.cancelled, HANDED);
        assert_eq!(queue.current(), Some(paths[0].clone()));
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[2]);
    }

    #[test]
    fn removing_the_current_row_cancels_and_moves_to_the_next_row() {
        let (mut queue, paths) = queue_of(3);
        queue.stage_natural(gapless);
        let removal = queue.remove_many(&[0]);
        assert!(removal.current_removed);
        assert_eq!(removal.cancelled, HANDED);
        assert_eq!(queue.current(), Some(paths[1].clone()));
        assert_eq!(queue.remove_many(&[7]), super::Removal::default());
    }

    #[test]
    fn moving_rows_keeps_a_successor_that_still_follows() {
        let (mut queue, paths) = queue_of(4);
        queue.stage_natural(gapless);
        assert_eq!(queue.move_item(3, 2), None);
        assert_eq!(queue.move_item(0, 0), None);
        assert_eq!(committed_index(&mut queue), Some(1));
        assert_eq!(queue.current(), Some(paths[1].clone()));

        let (mut queue, paths) = queue_of(4);
        queue.set_current(1);
        queue.stage_natural(gapless);
        assert_eq!(queue.move_item(0, 3), None, "both rows shift");
        assert_eq!(queue.current_index(), Some(0));
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (paths[2].clone(), 1));

        let (mut queue, _) = queue_of(2);
        queue.stage_natural(gapless);
        assert_eq!(
            queue.move_item(0, 1),
            HANDED,
            "nothing follows the last row"
        );
    }

    #[test]
    fn restoring_rows_around_the_current_track_keeps_a_successor_that_still_follows() {
        // Rows 0 and 3 come back around the committed track, which was row
        // 0 of the shorter queue; its successor still follows it.
        let short = vec![PathBuf::from("/b.flac"), PathBuf::from("/c.flac")];
        let mut queue = GaplessQueue::new();
        queue.set_queue(short);
        queue.stage_natural(gapless);
        let restored = ["/a.flac", "/b.flac", "/c.flac", "/d.flac"]
            .map(PathBuf::from)
            .to_vec();
        assert_eq!(
            queue.restore(restored.clone(), 1, &PlayNextRestore::None),
            None
        );
        assert_eq!(queue.current(), Some(restored[1].clone()));
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (restored[2].clone(), 2));

        // A row restored between the committed track and its successor
        // follows it now, so the successor goes.
        let mut queue = GaplessQueue::new();
        queue.set_queue(vec![PathBuf::from("/a.flac"), PathBuf::from("/c.flac")]);
        queue.stage_natural(gapless);
        assert_eq!(
            queue.restore(restored[..3].to_vec(), 0, &PlayNextRestore::None),
            HANDED
        );
        assert_eq!(queue.stage_natural(gapless).unwrap().path, restored[1]);

        // With repeat-one, the committed track follows itself wherever its
        // row is now.
        let mut queue = GaplessQueue::new();
        queue.set_queue(vec![PathBuf::from("/b.flac")]);
        queue.set_repeat_mode(RepeatMode::One);
        queue.stage_natural(gapless);
        assert_eq!(
            queue.restore(restored.clone(), 1, &PlayNextRestore::None),
            None
        );
        assert_eq!(committed_index(&mut queue), Some(1));
    }

    #[test]
    fn reordering_rows_keeps_the_committed_row_and_a_successor_that_still_follows() {
        // The committed track is the second of two rows with its path.
        let mut queue = GaplessQueue::new();
        let a = PathBuf::from("/a.flac");
        let rows = vec![
            a.clone(),
            PathBuf::from("/b.flac"),
            a.clone(),
            PathBuf::from("/c.flac"),
        ];
        queue.set_queue(rows.clone());
        queue.set_current(2);
        queue.stage_natural(gapless);
        // The first A moves after C; the committed A and its successor C
        // stay together.
        assert_eq!(queue.reorder(&[1, 2, 3, 0]), None);
        assert_eq!(queue.current_index(), Some(1));
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (rows[3].clone(), 2));

        // B moves between the committed track and its successor: the
        // successor goes, and B follows now.
        let (mut queue, paths) = queue_of(4);
        queue.stage_natural(gapless);
        assert_eq!(queue.reorder(&[0, 2, 1, 3]), HANDED);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[2]);

        // With repeat-one, the committed track follows itself wherever its
        // row goes.
        let (mut queue, _) = queue_of(3);
        queue.set_repeat_mode(RepeatMode::One);
        queue.stage_natural(gapless);
        assert_eq!(queue.reorder(&[2, 1, 0]), None);
        assert_eq!(committed_index(&mut queue), Some(2));

        // An order that does not name every row once changes nothing.
        let (mut queue, paths) = queue_of(3);
        assert_eq!(queue.reorder(&[0, 0, 1]), None);
        assert_eq!(queue.reorder(&[0, 1]), None);
        assert_eq!(queue.reorder(&[0, 1, 5]), None);
        assert_eq!(queue.queue, paths);
    }

    #[test]
    fn inserting_rows_moves_the_committed_row_down_and_drops_a_successor_they_displace() {
        let (mut queue, paths) = queue_of(3);
        queue.set_current(1);
        queue.stage_natural(gapless);
        let new = vec![PathBuf::from("/x.flac"), PathBuf::from("/y.flac")];
        // Before the committed track: its successor still follows it.
        assert_eq!(queue.insert(0, new.clone()), None);
        assert_eq!(queue.current_index(), Some(3));
        assert_eq!(queue.current(), Some(paths[1].clone()));
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (paths[2].clone(), 4));

        // Right after it: the inserted rows follow now.
        let (mut queue, _) = queue_of(3);
        queue.stage_natural(gapless);
        assert_eq!(queue.insert(1, new.clone()), HANDED);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, new[0]);
        // Past the end, after the last row.
        assert_eq!(queue.insert(99, vec![PathBuf::from("/z.flac")]), None);
        assert_eq!(queue.queue.last(), Some(&PathBuf::from("/z.flac")));
        assert_eq!(queue.insert(0, Vec::new()), None);
    }

    #[test]
    fn moving_another_row_after_the_current_track_cancels_the_successor() {
        let (mut queue, paths) = queue_of(3);
        queue.stage_natural(gapless);
        assert_eq!(queue.move_item(2, 1), HANDED);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[2]);
    }

    #[test]
    fn repeat_and_shuffle_changes_cancel_only_a_different_successor() {
        let (mut queue, paths) = queue_of(2);
        queue.stage_natural(gapless);
        assert_eq!(queue.set_repeat_mode(RepeatMode::All), None);
        assert_eq!(queue.set_shuffle_enabled(false), None);
        assert_eq!(queue.set_repeat_mode(RepeatMode::One), HANDED);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[0]);
        assert_eq!(queue.set_repeat_mode(RepeatMode::Off), HANDED);

        queue.stage_natural(gapless);
        assert_eq!(queue.set_shuffle_enabled(true), HANDED);
    }

    #[test]
    fn appending_after_the_last_track_retargets_a_wrapped_successor() {
        let (mut queue, paths) = queue_of(2);
        queue.set_current(1);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[0]);
        assert_eq!(
            queue.add_to_queue(vec![PathBuf::from("/fixture/new.flac")]),
            HANDED
        );

        let (mut queue, _) = queue_of(3);
        queue.stage_natural(gapless);
        assert_eq!(
            queue.add_to_queue(vec![PathBuf::from("/fixture/new.flac")]),
            None
        );
        assert_eq!(committed_index(&mut queue), Some(1));
    }

    #[test]
    fn repeat_one_and_duplicate_files_commit_by_queue_position() {
        let (mut queue, paths) = queue_of(2);
        queue.set_repeat_mode(RepeatMode::One);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[0]);
        assert_eq!(committed_index(&mut queue), Some(0));

        let duplicate = PathBuf::from("/fixture/same.flac");
        let mut queue = GaplessQueue::new();
        queue.set_queue(vec![duplicate.clone(), duplicate.clone()]);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, duplicate);
        assert_eq!(committed_index(&mut queue), Some(1));
        assert_eq!(queue.stage_natural(gapless), None);
    }

    #[test]
    fn short_successors_chain_and_commit_in_order() {
        let (mut queue, paths) = queue_of(3);
        assert!(queue.stage_natural(gapless).unwrap().staged_spectrogram);
        let second = queue
            .stage_natural(gapless)
            .expect("successor of the staged track");
        assert_eq!(second.path, paths[2]);
        assert!(!second.staged_spectrogram);

        let first = queue.commit_gapless().expect("first commit");
        assert_eq!(first.index, 1);
        assert!(queue.successor_handed_to_pipeline());
        assert_eq!(committed_index(&mut queue), Some(2));
        assert_eq!(queue.commit_gapless(), None);
    }

    #[test]
    fn invalid_successor_drops_the_successors_behind_it() {
        let (mut queue, paths) = queue_of(4);
        queue.stage_natural(gapless);
        queue.stage_natural(|_, _| Handoff::AtEos);
        let removal = queue.remove_many(&[3]);
        assert_eq!(removal.cancelled, None, "unrelated row");
        let removal = queue.remove_many(&[2]);
        assert_eq!(
            removal.cancelled,
            Some(CancelledAdvance {
                handed_to_pipeline: false,
                staged_spectrogram: false,
            }),
            "a successor not given to the pipeline drops alone"
        );
        assert_eq!(committed_index(&mut queue), Some(1));
        assert_eq!(queue.current(), Some(paths[1].clone()));
    }

    #[test]
    fn held_handoffs_stage_successors_for_end_of_stream() {
        let (mut queue, paths) = queue_of(3);
        queue.stage_natural(gapless);
        queue.hold_handoffs();
        assert_eq!(queue.cancel_pending(), HANDED);
        let staged = queue.stage_natural(gapless).expect("successor");
        assert_eq!(staged.handoff, Handoff::AtEos);
        assert!(!staged.staged_spectrogram);
        queue.release_handoffs();
        assert_eq!(queue.commit_gapless(), None);
        assert_eq!(queue.take_eos_successor().unwrap().path, paths[1]);
        assert_eq!(
            queue.stage_natural(gapless).unwrap().handoff,
            Handoff::Gapless
        );
    }

    #[test]
    fn end_of_stream_starts_the_staged_successor_or_a_late_one() {
        let (mut queue, paths) = queue_of(3);
        let staged = queue.stage_natural(|_, _| Handoff::AtEos).unwrap();
        assert!(!staged.staged_spectrogram);
        assert!(!queue.successor_handed_to_pipeline());
        assert_eq!(queue.commit_gapless(), None);
        let successor = queue.take_eos_successor().expect("staged successor");
        assert_eq!((successor.path, successor.index), (paths[1].clone(), 1));
        assert_eq!(successor.released, Some(CancelledAdvance::default()));

        let (mut queue, paths) = queue_of(1);
        assert_eq!(queue.stage_natural(gapless), None);
        queue.add_to_queue(vec![PathBuf::from("/fixture/late.flac")]);
        let successor = queue.take_eos_successor().expect("appended successor");
        assert_eq!(successor.index, 1);
        assert_eq!(successor.released, None);
        assert_ne!(successor.path, paths[0]);
        assert_eq!(queue.take_eos_successor(), None);
    }

    #[test]
    fn remapped_queue_keeps_position_shuffle_repeat_one_and_staged_successors() {
        let (mut queue, paths) = queue_of(4);
        let renamed_current = PathBuf::from("/fixture/renamed-1.flac");
        let renamed_next = PathBuf::from("/fixture/renamed-2.flac");
        queue.set_current(1);
        queue.set_shuffle_enabled(true);
        let cursor = queue.cursor.clone();

        queue.remap_paths(&PathRenames::from([
            (paths[1].clone(), renamed_current.clone()),
            (paths[2].clone(), renamed_next.clone()),
        ]));
        assert_eq!(
            queue.queue,
            vec![
                paths[0].clone(),
                renamed_current.clone(),
                renamed_next.clone(),
                paths[3].clone()
            ]
        );
        assert_eq!(queue.cursor, cursor);
        queue.set_shuffle_enabled(false);
        queue.set_repeat_mode(RepeatMode::One);
        let successor = queue.clone().take_eos_successor().unwrap();
        assert_eq!(successor.path, renamed_current);
        queue.set_repeat_mode(RepeatMode::Off);

        let staged = queue.stage_natural(gapless).expect("successor");
        assert_eq!(staged.path, renamed_next);
        let renamed_again = PathBuf::from("/fixture/renamed-again-2.flac");
        queue.remap_paths(&PathRenames::from([(renamed_next, renamed_again.clone())]));
        assert_eq!(queue.add_to_queue(Vec::new()), None, "still the successor");
        let committed = queue.commit_gapless().expect("commit");
        assert_eq!((committed.path, committed.index), (renamed_again, 2));
    }

    #[test]
    fn natural_advances_skip_failed_files_until_cleared() {
        let (mut queue, paths) = queue_of(4);
        queue.mark_failed(paths[1].clone());
        let staged = queue.stage_natural(gapless).expect("successor");
        assert_eq!(staged.path, paths[2]);
        assert_eq!(queue.add_to_queue(Vec::new()), None, "still the successor");
        assert_eq!(committed_index(&mut queue), Some(2));

        queue.set_current(0);
        assert_eq!(queue.clone().take_eos_successor().unwrap().index, 2);
        let skipped = queue.skip_failed_current().expect("next playable track");
        assert_eq!((skipped.path, skipped.index), (paths[2].clone(), 2));

        queue.clear_failed();
        queue.set_current(0);
        assert_eq!(queue.stage_natural(gapless).unwrap().path, paths[1]);
    }

    #[test]
    fn failed_file_renamed_later_is_tried_at_its_new_path() {
        let (mut queue, paths) = queue_of(3);
        let renamed = PathBuf::from("/fixture/renamed-1.flac");
        queue.mark_failed(paths[1].clone());
        queue.remap_paths(&PathRenames::from([(paths[1].clone(), renamed.clone())]));
        assert_eq!(queue.stage_natural(gapless).unwrap().path, renamed);
    }

    #[test]
    fn queue_of_failed_files_has_no_successor_in_any_repeat_mode() {
        let (mut queue, paths) = queue_of(2);
        queue.mark_failed(paths[0].clone());
        queue.mark_failed(paths[1].clone());
        for mode in [RepeatMode::Off, RepeatMode::All, RepeatMode::One] {
            queue.set_repeat_mode(mode);
            for shuffle in [false, true] {
                queue.set_shuffle_enabled(shuffle);
                assert_eq!(queue.stage_natural(gapless), None, "{mode:?} {shuffle}");
                assert_eq!(
                    queue.clone().take_eos_successor(),
                    None,
                    "{mode:?} {shuffle}"
                );
                assert_eq!(queue.skip_failed_current(), None, "{mode:?} {shuffle}");
            }
        }
        assert_eq!(queue.current_index(), Some(0));

        // Repeat-one replays only the current track; it does not move on.
        let (mut queue, paths) = queue_of(2);
        queue.set_repeat_mode(RepeatMode::One);
        queue.mark_failed(paths[0].clone());
        assert_eq!(queue.skip_failed_current(), None);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(queue.skip_failed_current().unwrap().index, 1);
    }

    #[test]
    fn pipeline_positions_name_the_tracks_by_their_current_paths() {
        let (mut queue, paths) = queue_of(4);
        queue.stage_natural(gapless);
        queue.stage_natural(gapless);
        // A successor staged for end of stream is not in the pipeline.
        queue.stage_natural(|_, _| Handoff::AtEos);
        let renamed = PathBuf::from("/fixture/renamed-1.flac");
        queue.remap_paths(&PathRenames::from([
            (paths[0].clone(), paths[1].clone()),
            (paths[1].clone(), renamed.clone()),
        ]));
        assert_eq!(queue.path_in_pipeline(0), Some(paths[1].clone()));
        assert_eq!(queue.path_in_pipeline(1), Some(renamed));
        assert_eq!(queue.path_in_pipeline(2), Some(paths[2].clone()));
        assert_eq!(queue.path_in_pipeline(3), None);
    }

    #[test]
    fn shuffle_repeat_all_finds_the_only_track_left_through_refreshed_pools() {
        // Some picks revisit a failed track after the pool is refreshed,
        // before they reach the current track again.
        for seed in 1..100 {
            let (mut queue, paths) = queue_of(3);
            queue.set_repeat_mode(RepeatMode::All);
            queue.set_shuffle_enabled(true);
            queue.seed_shuffle(seed);
            queue.mark_failed(paths[1].clone());
            queue.mark_failed(paths[2].clone());
            assert_eq!(
                queue
                    .clone()
                    .stage_natural(gapless)
                    .map(|staged| staged.path),
                Some(paths[0].clone()),
                "seed {seed}"
            );
            assert_eq!(
                queue.take_eos_successor().map(|next| next.index),
                Some(0),
                "seed {seed}"
            );
        }
    }

    /// Every queue of up to four rows, with every set of failed rows, from
    /// every row and with partly used shuffle navigation: the natural
    /// successor is the first row not failed that natural advances reach,
    /// and there is none exactly when every reachable row has failed.
    #[test]
    fn skipping_failed_rows_stops_exactly_when_every_reachable_row_failed() {
        for len in 1..=4_usize {
            for failed_rows in 0..1_u32 << len {
                for mode in [RepeatMode::Off, RepeatMode::All, RepeatMode::One] {
                    for shuffle in [false, true] {
                        for seed in 1..=8 {
                            for start in 0..len {
                                for moves in 0..3 {
                                    check_skip_search(
                                        len,
                                        failed_rows,
                                        (mode, shuffle),
                                        (seed, start, moves),
                                    );
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    fn check_skip_search(
        len: usize,
        failed_rows: u32,
        (mode, shuffle): (RepeatMode, bool),
        (seed, start, moves): (u64, usize, usize),
    ) {
        let (mut queue, paths) = queue_of(len);
        queue.set_shuffle_enabled(shuffle);
        queue.seed_shuffle(seed);
        queue.set_current(start);
        // Navigate first, so that the shuffle has history, forward steps,
        // and a partly used pool.
        for step in 0..moves {
            if step % 2 == 0 {
                queue.next_manual();
            } else {
                queue.previous_manual();
            }
        }
        queue.set_repeat_mode(mode);
        let failed = |index: usize| failed_rows & (1 << index) != 0;
        for (index, path) in paths.iter().enumerate() {
            if failed(index) {
                queue.mark_failed(path.clone());
            }
        }
        let case = format!(
            "len {len} failed {failed_rows:b} {mode:?} shuffle {shuffle} seed {seed} \
             start {start} moves {moves}"
        );

        // The rows that natural advances reach, ignoring failures, until
        // they end or have gone around the queue more than often enough.
        let mut cursor = queue.cursor.clone();
        let mut reached = Vec::new();
        while reached.len() < 8 * len && cursor.advance(len, queue.rules(), Advance::Natural) {
            reached.push(cursor.index);
        }
        let expected = reached.into_iter().find(|index| !failed(*index));
        let reachable_not_failed = match mode {
            RepeatMode::One => !failed(queue.cursor.index),
            RepeatMode::All => (0..len).any(|index| !failed(index)),
            RepeatMode::Off => expected.is_some(),
        };
        assert_eq!(expected.is_some(), reachable_not_failed, "{case}");

        let found = queue.natural_successor(&queue.cursor);
        assert_eq!(found.map(|(cursor, _)| cursor.index), expected, "{case}");
    }

    #[test]
    fn batched_removal_keeps_current_and_rebuilds_shuffle_for_survivors() {
        let (mut queue, paths) = queue_of(6);
        queue.set_current(3);
        queue.set_shuffle_enabled(true);
        queue.set_repeat_mode(RepeatMode::All);
        let removal = queue.remove_many(&[0, 2, 5]);
        assert!(!removal.current_removed);
        assert_eq!(queue.current(), Some(paths[3].clone()));
        assert_eq!(queue.current_index(), Some(1));
        assert_eq!(
            queue.queue,
            vec![paths[1].clone(), paths[3].clone(), paths[4].clone()]
        );
        for _ in 0..10 {
            let next = queue.next_manual().expect("surviving shuffle track");
            assert!(queue.queue.contains(&next.path));
        }
        assert!(queue.remove_many(&[0, 1, 2]).current_removed);
        assert_eq!(queue.current_index(), None);
    }

    #[test]
    fn repeat_all_loops_to_start_on_natural_advance() {
        let (mut queue, paths) = queue_of(2);
        queue.set_current(1);
        queue.set_repeat_mode(RepeatMode::All);
        assert_eq!(queue.take_eos_successor().unwrap().path, paths[0]);
    }

    #[test]
    fn shuffle_previous_returns_to_history() {
        let (mut queue, paths) = queue_of(3);
        queue.set_shuffle_enabled(true);
        let next = queue.next_manual().expect("shuffle next").path;
        assert_ne!(next, paths[0]);
        assert_eq!(queue.previous_manual().unwrap().path, paths[0]);
        assert_eq!(queue.next_manual().unwrap().path, next);
    }

    #[test]
    fn shuffle_repeat_off_stops_when_pool_exhausted() {
        let (mut queue, paths) = queue_of(2);
        queue.set_shuffle_enabled(true);
        assert_eq!(queue.take_eos_successor().unwrap().path, paths[1]);
        assert_eq!(queue.take_eos_successor(), None);
    }

    fn marks(queue: &GaplessQueue) -> Vec<usize> {
        queue.play_next().to_vec()
    }

    fn staged_index(queue: &mut GaplessQueue) -> Option<usize> {
        let staged = queue.stage_natural(gapless)?;
        Some(queue.pending.back()?.cursor.index).filter(|_| !staged.path.as_os_str().is_empty())
    }

    #[test]
    fn marked_rows_play_next_in_order_and_the_order_goes_on_after_the_last() {
        let (mut queue, paths) = queue_of(8);
        assert_eq!(queue.toggle_play_next(&[5]), None);
        assert_eq!(queue.toggle_play_next(&[2]), None);
        assert_eq!(marks(&queue), vec![5, 2]);

        // The natural successor is the first marked row, which leaves the
        // list only once it starts. It is no shuffle pick.
        let staged = queue.stage_natural(gapless).expect("the first marked row");
        assert_eq!(
            (staged.path, staged.pick),
            (paths[5].clone(), TrackPick::Other)
        );
        assert_eq!(marks(&queue), vec![5, 2]);
        assert_eq!(committed_index(&mut queue), Some(5));
        assert_eq!(marks(&queue), vec![2]);
        assert_eq!(queue.current_pick(), TrackPick::Other);

        // At end of stream too, and then the queue's order goes on after
        // the last marked row that played.
        let staged = queue.stage_natural(|_, _| Handoff::AtEos).expect("row 2");
        assert_eq!(staged.path, paths[2]);
        assert_eq!(queue.take_eos_successor().map(|eos| eos.index), Some(2));
        assert!(marks(&queue).is_empty());
        assert_eq!(staged_index(&mut queue), Some(3));
        queue.cancel_pending();

        // Next goes to the first marked row too.
        queue.toggle_play_next(&[7, 0]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(0));
        assert_eq!(marks(&queue), vec![7]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(7));
        assert_eq!(queue.next_manual(), None, "the queue ends after row 7");
    }

    #[test]
    fn with_shuffle_a_marked_row_is_no_shuffle_pick_and_counts_as_played() {
        let (mut queue, paths) = queue_of(6);
        queue.seed_shuffle(11);
        queue.set_shuffle_enabled(true);
        queue.toggle_play_next(&[4]);
        let staged = queue.stage_natural(gapless).expect("the marked row");
        assert_eq!(
            (staged.path, staged.pick),
            (paths[4].clone(), TrackPick::Other)
        );
        assert_eq!(committed_index(&mut queue), Some(4));
        assert_eq!(queue.current_pick(), TrackPick::Other);
        // The shuffle's round counts it as played, and goes on with picks.
        assert!(!queue.cursor.shuffle_pool.contains(&4));
        let mut played = vec![0, 4];
        while let Some(next) = queue.stage_natural(gapless) {
            assert_eq!(next.pick, TrackPick::Shuffle);
            played.push(committed_index(&mut queue).expect("a shuffle pick"));
        }
        played.sort_unstable();
        assert_eq!(played, vec![0, 1, 2, 3, 4, 5], "each row once");

        // Previous goes back through what played, the marked row too.
        let (mut queue, _) = queue_of(6);
        queue.seed_shuffle(3);
        queue.set_shuffle_enabled(true);
        queue.toggle_play_next(&[5]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(5));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );
    }

    #[test]
    fn a_jump_with_shuffle_on_counts_as_played_and_previous_goes_back() {
        let (mut queue, paths) = queue_of(6);
        queue.seed_shuffle(7);
        queue.set_shuffle_enabled(true);
        assert!(queue.next_manual().is_some());
        let before = queue.current_index().expect("a row plays");
        let target = (0..6)
            .find(|row| *row != before && *row != 0)
            .expect("a row");
        // A staged pick is dropped.
        assert!(queue.stage_natural(gapless).is_some());
        let jump = queue.jump_to(target).expect("the row");
        assert_eq!((jump.path, jump.index), (paths[target].clone(), target));
        assert_eq!(jump.cancelled, HANDED);
        assert_eq!(queue.current_pick(), TrackPick::Other, "the user's pick");
        assert!(queue.shuffle_enabled());

        // The round goes on without the rows that played, the jump's row
        // among them, each row once.
        let mut played = vec![0, before, target];
        while let Some(next) = queue.stage_natural(gapless) {
            assert_eq!(next.pick, TrackPick::Shuffle);
            played.push(committed_index(&mut queue).expect("a shuffle pick"));
        }
        played.sort_unstable();
        assert_eq!(played, vec![0, 1, 2, 3, 4, 5]);

        // Previous goes back from the jump to the track that was current.
        let (mut queue, _) = queue_of(6);
        queue.seed_shuffle(7);
        queue.set_shuffle_enabled(true);
        queue.jump_to(4).expect("row 4");
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );
    }

    #[test]
    fn a_jump_leaves_the_marks_and_without_shuffle_is_a_pick_of_the_row() {
        let (mut queue, _) = queue_of(6);
        queue.toggle_play_next(&[3, 5]);
        assert_eq!(queue.jump_to(3).map(|jump| jump.index), Some(3));
        assert_eq!(marks(&queue), vec![5], "the row leaves the marks");
        // Without shuffle, the queue's order goes on after it, after the
        // marked row.
        assert_eq!(queue.next_manual().map(|next| next.index), Some(5));
        assert_eq!(queue.jump_to(1).map(|jump| jump.index), Some(1));
        assert_eq!(queue.next_manual().map(|next| next.index), Some(2));
        // A row past the end keeps what is staged.
        assert!(queue.stage_natural(gapless).is_some());
        assert!(queue.jump_to(6).is_none());
        assert_eq!(committed_index(&mut queue), Some(3));
    }

    #[test]
    fn without_shuffle_previous_goes_back_from_a_marked_row_to_the_one_before() {
        let (mut queue, _) = queue_of(8);
        queue.set_current(1);
        queue.toggle_play_next(&[6]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(6));
        assert_eq!(queue.next_manual().map(|next| next.index), Some(7));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(6)
        );
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(1)
        );
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );
    }

    #[test]
    fn previous_goes_back_from_a_marked_row_across_edits_that_keep_both_rows() {
        // Row 0 plays, then marked row 2; removing row 3 keeps both.
        let (mut queue, _) = queue_of(4);
        queue.toggle_play_next(&[2]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(2));
        queue.remove_many(&[3]);
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );

        // Moves and insertions carry the rows along.
        let (mut queue, paths) = queue_of(6);
        queue.set_current(1);
        queue.toggle_play_next(&[4]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(4));
        queue.insert(0, vec![paths[5].clone()]);
        queue.move_item(6, 0);
        // Rows 1 and 4 are rows 3 and 6 now.
        assert_eq!(queue.current_index(), Some(6));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(3)
        );

        // With the row it went from gone, Previous goes back in order.
        let (mut queue, _) = queue_of(6);
        queue.set_current(1);
        queue.toggle_play_next(&[4]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(4));
        queue.remove_many(&[1]);
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(2)
        );

        // An undone removal puts the rows back around the jump.
        let (mut queue, paths) = queue_of(4);
        queue.toggle_play_next(&[2]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(2));
        queue.remove_many(&[1, 3]);
        let undo = PlayNextRestore::Reinserted {
            rows: vec![1, 3],
            before: Vec::new(),
        };
        assert_eq!(queue.restore(paths, 2, &undo), None);
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );

        // A, then marked D, then marked B, then C and D in order: removing
        // B ends the history, so Previous from D goes to C, not to A.
        let (mut queue, _) = queue_of(4);
        queue.toggle_play_next(&[3]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(3));
        queue.toggle_play_next(&[1]);
        assert_eq!(queue.next_manual().map(|next| next.index), Some(1));
        assert_eq!(queue.next_manual().map(|next| next.index), Some(2));
        assert_eq!(queue.next_manual().map(|next| next.index), Some(3));
        queue.remove_many(&[1]);
        assert_eq!(queue.current_index(), Some(2));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(1)
        );
    }

    #[test]
    fn an_undo_keeps_the_staged_copy_of_a_marked_file_that_it_was_staged_as() {
        // Rows 2 and 3 hold the same file, and both are marked.
        let duplicate = PathBuf::from("/fixture/same.flac");
        let mut rows = paths(2);
        rows.extend([duplicate.clone(), duplicate.clone()]);
        let mut queue = GaplessQueue::new();
        queue.set_queue(rows.clone());
        queue.toggle_play_next(&[2, 3]);
        assert_eq!(staged_index(&mut queue), Some(2));
        // Removing row 2 drops it; row 3, now row 2, is staged.
        assert_eq!(queue.remove_many(&[2]).cancelled, HANDED);
        assert_eq!(staged_index(&mut queue), Some(2));

        // The undo puts row 2 back ahead of it, and row 2 plays next. The
        // pipeline was given the same file, so it keeps the handoff.
        let undo = PlayNextRestore::Reinserted {
            rows: vec![2],
            before: vec![2, 3],
        };
        assert_eq!(queue.restore(rows.clone(), 0, &undo), None);
        assert_eq!(committed_index(&mut queue), Some(2));
        assert_eq!(marks(&queue), vec![3]);

        // Unmarked, row 3 plays in order after row 2, and Previous goes
        // back through both rows.
        assert_eq!(queue.toggle_play_next(&[3]), None);
        assert_eq!(staged_index(&mut queue), Some(3));
        assert_eq!(committed_index(&mut queue), Some(3));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(2)
        );
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );

        // With Repeat-One, the marked row 3 plays after row 2, and
        // Previous goes back through both rows as well.
        let mut queue = GaplessQueue::new();
        queue.set_queue(rows.clone());
        queue.set_repeat_mode(RepeatMode::One);
        queue.toggle_play_next(&[2, 3]);
        assert_eq!(staged_index(&mut queue), Some(2));
        assert_eq!(queue.remove_many(&[2]).cancelled, HANDED);
        assert_eq!(staged_index(&mut queue), Some(2));
        assert_eq!(queue.restore(rows, 0, &undo), None);
        assert_eq!(committed_index(&mut queue), Some(2));
        assert_eq!(staged_index(&mut queue), Some(3));
        assert_eq!(committed_index(&mut queue), Some(3));
        let previous = [queue.previous_manual(), queue.previous_manual()];
        assert_eq!(
            previous.map(|previous| previous.map(|p| p.index)),
            [Some(2), Some(0)]
        );
    }

    #[test]
    fn a_staged_file_stays_when_another_row_with_it_comes_to_play_next() {
        // Rows 1 and 2 hold the same file; row 1 is staged in order, and
        // marking row 2 makes it come next. The pipeline was given that
        // file, so it keeps the handoff, and row 2 plays.
        let duplicate = PathBuf::from("/fixture/same.flac");
        let mut queue = GaplessQueue::new();
        queue.set_queue(vec![
            PathBuf::from("/fixture/0.flac"),
            duplicate.clone(),
            duplicate,
        ]);
        assert_eq!(staged_index(&mut queue), Some(1));
        assert_eq!(queue.toggle_play_next(&[2]), None);
        assert_eq!(committed_index(&mut queue), Some(2));
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(0)
        );

        // A different file coming next still drops the staged one.
        let (mut queue, _) = queue_of(3);
        assert_eq!(staged_index(&mut queue), Some(1));
        assert_eq!(queue.toggle_play_next(&[2]), HANDED);
    }

    #[test]
    fn repeat_one_plays_a_marked_row_and_the_playing_row_can_be_marked() {
        let (mut queue, paths) = queue_of(4);
        queue.set_repeat_mode(RepeatMode::One);
        queue.toggle_play_next(&[2]);
        assert_eq!(
            queue.stage_natural(gapless).map(|staged| staged.path),
            Some(paths[2].clone())
        );
        assert_eq!(committed_index(&mut queue), Some(2));
        // Then Repeat-One repeats the row that played.
        assert_eq!(staged_index(&mut queue), Some(2));
        queue.cancel_pending();

        // The playing row, marked, plays again next, as no shuffle pick.
        queue.set_repeat_mode(RepeatMode::Off);
        queue.set_shuffle_enabled(true);
        queue.toggle_play_next(&[2]);
        let again = queue.stage_natural(gapless).expect("the playing row again");
        assert_eq!(
            (again.path, again.pick),
            (paths[2].clone(), TrackPick::Other)
        );
        assert_eq!(committed_index(&mut queue), Some(2));
        assert!(marks(&queue).is_empty());
    }

    #[test]
    fn changing_the_marks_drops_a_staged_successor_that_no_longer_follows() {
        let (mut queue, paths) = queue_of(8);
        // A successor in the queue's order, given to the pipeline, gives
        // way to a marked row.
        assert_eq!(staged_index(&mut queue), Some(1));
        assert_eq!(queue.toggle_play_next(&[5]), HANDED);
        assert!(queue.pending.is_empty());
        assert_eq!(staged_index(&mut queue), Some(5));

        // Marking another row keeps the staged one, which takes the new
        // mark along to commit.
        assert_eq!(queue.toggle_play_next(&[3]), None);
        assert_eq!(queue.pending.len(), 1);
        assert_eq!(committed_index(&mut queue), Some(5));
        assert_eq!(marks(&queue), vec![3]);

        // Unmarking the staged row drops it; so does clearing the marks.
        assert_eq!(staged_index(&mut queue), Some(3));
        assert_eq!(queue.toggle_play_next(&[3]), HANDED);
        queue.toggle_play_next(&[7]);
        assert_eq!(staged_index(&mut queue), Some(7));
        assert_eq!(queue.set_play_next(Vec::new()), HANDED);
        assert_eq!(staged_index(&mut queue), Some(6));
        queue.cancel_pending();

        // A shuffle pick gives way to a marked row as well, and a marked
        // row to another that goes ahead of it.
        queue.set_shuffle_enabled(true);
        assert!(queue.stage_natural(gapless).is_some());
        assert_eq!(queue.toggle_play_next(&[0]), HANDED);
        assert_eq!(
            queue.stage_natural(gapless).map(|staged| staged.path),
            Some(paths[0].clone())
        );
        assert_eq!(queue.set_play_next(vec![2, 0]), HANDED);
        assert_eq!(staged_index(&mut queue), Some(2));
        // An edit that leaves the staged row first keeps it.
        assert_eq!(queue.move_item(6, 7), None);
        assert_eq!(committed_index(&mut queue), Some(2));
        assert_eq!(marks(&queue), vec![0]);
    }

    /// Whether each staged successor has the cursor that the track before
    /// it would stage it with now.
    fn staged_as_now(queue: &GaplessQueue) -> bool {
        let mut from = &queue.cursor;
        queue.pending.iter().all(|pending| {
            let now = queue.natural_successor(from);
            from = &pending.cursor;
            now.is_some_and(|(cursor, marked)| cursor == pending.cursor && marked == pending.marked)
        })
    }

    #[test]
    fn marking_or_unmarking_a_staged_successor_that_follows_anyway_keeps_it() {
        // Without shuffle: the next row in the queue's order, and one more
        // staged after it, stay staged as the row is marked and unmarked.
        let (mut queue, _) = queue_of(6);
        queue.set_current(1);
        assert_eq!(staged_index(&mut queue), Some(2));
        assert_eq!(staged_index(&mut queue), Some(3));
        assert_eq!(queue.toggle_play_next(&[2]), None);
        assert_eq!(queue.pending.len(), 2);
        assert!(queue.pending[0].marked && staged_as_now(&queue));
        assert_eq!(queue.toggle_play_next(&[2]), None);
        assert!(!queue.pending[0].marked && staged_as_now(&queue));
        // Marked again, it plays as the marked row, the queue's order goes
        // on after it, and Previous goes back through it.
        assert_eq!(queue.toggle_play_next(&[2]), None);
        assert_eq!(committed_index(&mut queue), Some(2));
        assert!(marks(&queue).is_empty());
        assert_eq!(queue.current_pick(), TrackPick::Other);
        assert_eq!(committed_index(&mut queue), Some(3));
        assert_eq!(queue.cursor.jumps, vec![(1, 2)]);
        assert_eq!(queue.previous_manual().map(|moved| moved.index), Some(2));
        assert_eq!(queue.previous_manual().map(|moved| moved.index), Some(1));

        // Repeat-One: the playing row, staged again, stays staged as it is
        // marked to play again and unmarked, by the same pick.
        for shuffle in [false, true] {
            let (mut queue, _) = queue_of(4);
            queue.seed_shuffle(5);
            queue.set_shuffle_enabled(shuffle);
            queue.set_repeat_mode(RepeatMode::One);
            assert_eq!(staged_index(&mut queue), Some(0));
            assert_eq!(queue.toggle_play_next(&[0]), None, "shuffle {shuffle}");
            assert!(queue.pending[0].marked && staged_as_now(&queue));
            assert_eq!(queue.toggle_play_next(&[0]), None, "shuffle {shuffle}");
            assert!(!queue.pending[0].marked && staged_as_now(&queue));
        }
    }

    #[test]
    fn a_staged_successor_that_would_come_to_play_by_another_pick_is_dropped() {
        // The gain stage was given the staged successor's pick, which Auto
        // mode levels it by: a shuffle pick that is marked would be no
        // shuffle pick, so it is staged again.
        let (mut queue, _) = queue_of(6);
        queue.seed_shuffle(9);
        queue.set_shuffle_enabled(true);
        let staged = queue.stage_natural(gapless).expect("a shuffle pick");
        assert_eq!(staged.pick, TrackPick::Shuffle);
        let row = queue.pending[0].cursor.index;
        assert_eq!(queue.toggle_play_next(&[row]), HANDED);
        let marked = queue.stage_natural(gapless).expect("the marked row");
        assert_eq!(
            (queue.pending[0].cursor.index, marked.pick),
            (row, TrackPick::Other)
        );
        // Unmarked, a shuffle pick follows instead, drawn when staged.
        assert_eq!(queue.toggle_play_next(&[row]), HANDED);

        // Repeat-One plays a shuffle pick again as a shuffle pick, and as
        // no shuffle pick once it is marked to play again.
        let (mut queue, _) = queue_of(4);
        queue.seed_shuffle(9);
        queue.set_shuffle_enabled(true);
        let picked = queue.next_manual().expect("a shuffle pick").index;
        queue.set_repeat_mode(RepeatMode::One);
        assert_eq!(queue.current_pick(), TrackPick::Shuffle);
        assert_eq!(staged_index(&mut queue), Some(picked));
        assert_eq!(queue.toggle_play_next(&[picked]), HANDED);
        assert_eq!(
            queue.stage_natural(gapless).map(|staged| staged.pick),
            Some(TrackPick::Other)
        );
    }

    #[test]
    fn marks_follow_their_rows_through_edits_and_a_file_queued_twice() {
        let mut queue = GaplessQueue::new();
        let twice = PathBuf::from("/fixture/twice.flac");
        let mut rows = paths(6);
        rows[4] = twice.clone();
        rows[1] = twice.clone();
        queue.set_queue(rows);
        // Only the row chosen is marked, not the other row of its file.
        queue.toggle_play_next(&[4, 2]);
        assert_eq!(marks(&queue), vec![2, 4]);
        queue.remove_many(&[1, 3]);
        assert_eq!(marks(&queue), vec![1, 2]);
        queue.insert(0, paths(2));
        assert_eq!(marks(&queue), vec![3, 4]);
        queue.move_item(4, 0);
        assert_eq!(marks(&queue), vec![4, 0]);
        // Row 0 to the end, the others up.
        queue.reorder(&[1, 2, 3, 4, 5, 0]);
        assert_eq!(marks(&queue), vec![3, 5]);
        queue.add_to_queue(paths(1));
        assert_eq!(marks(&queue), vec![3, 5]);
        queue.remove_many(&[5]);
        assert_eq!(marks(&queue), vec![3]);
        // A replaced queue has none.
        queue.set_queue(paths(3));
        assert!(marks(&queue).is_empty());
    }

    #[test]
    fn a_row_leaves_the_marks_when_playback_goes_to_it_and_the_rest_stay() {
        let (mut queue, _) = queue_of(6);
        queue.toggle_play_next(&[2, 4]);
        // Playing another row by hand keeps the marks.
        queue.set_current(5);
        assert_eq!(marks(&queue), vec![2, 4]);
        // Playing a marked row by hand, or going back to one, unmarks it.
        queue.set_current(4);
        assert_eq!(marks(&queue), vec![2]);
        queue.set_current(3);
        assert_eq!(
            queue.previous_manual().map(|previous| previous.index),
            Some(2)
        );
        assert!(marks(&queue).is_empty());
        // Removing the playing row starts the row that takes its place.
        queue.toggle_play_next(&[3, 5]);
        let removal = queue.remove_many(&[2]);
        assert!(removal.current_removed);
        assert_eq!(queue.current_index(), Some(2));
        assert_eq!(marks(&queue), vec![4]);
    }

    #[test]
    fn a_failed_marked_row_is_passed_over() {
        let (mut queue, paths) = queue_of(6);
        queue.toggle_play_next(&[4]);
        queue.toggle_play_next(&[2]);
        queue.mark_failed(paths[4].clone());
        assert_eq!(staged_index(&mut queue), Some(2));
        assert_eq!(committed_index(&mut queue), Some(2));
        assert!(marks(&queue).is_empty());
    }

    #[test]
    fn a_restored_queue_brings_back_the_marks_of_the_rows_it_restores() {
        let (mut queue, paths) = queue_of(5);
        queue.toggle_play_next(&[3, 1]);
        assert_eq!(marks(&queue), vec![1, 3]);
        // Rows 1 and 2 removed, then put back by an undo.
        queue.remove_many(&[1, 2]);
        assert_eq!(marks(&queue), vec![1]);
        let marks_back = PlayNextRestore::Reinserted {
            rows: vec![1, 2],
            before: vec![1, 3],
        };
        assert_eq!(queue.restore(paths.clone(), 0, &marks_back), None);
        assert_eq!(marks(&queue), vec![1, 3]);
        // A staged marked successor that still comes first stays.
        assert_eq!(staged_index(&mut queue), Some(1));
        assert_eq!(
            queue.restore(paths.clone(), 0, &PlayNextRestore::Rows(vec![1, 3])),
            None
        );
        assert_eq!(committed_index(&mut queue), Some(1));
        assert_eq!(
            queue.restore(paths, 1, &PlayNextRestore::Rows(vec![4])),
            None
        );
        assert_eq!(marks(&queue), vec![4]);
    }

    #[test]
    fn clearing_or_replacing_the_queue_cancels_staged_successors() {
        let (mut queue, _) = queue_of(2);
        queue.stage_natural(gapless);
        assert_eq!(queue.clear(), HANDED);
        assert_eq!(queue.current(), None);
        queue.set_queue(paths(2));
        queue.stage_natural(gapless);
        assert_eq!(queue.set_queue(paths(3)), HANDED);
        assert_eq!(queue.current_index(), Some(0));
    }

    #[test]
    fn same_audio_extension_matches_same_codecs() {
        assert!(same_audio_extension(
            Path::new("/tmp/a.flac"),
            Path::new("/tmp/b.flac")
        ));
        assert!(same_audio_extension(
            Path::new("/tmp/a.AC3"),
            Path::new("/tmp/b.ac3")
        ));
    }

    #[test]
    fn same_audio_extension_rejects_different_codecs() {
        assert!(!same_audio_extension(
            Path::new("/tmp/a.flac"),
            Path::new("/tmp/b.ac3")
        ));
        assert!(!same_audio_extension(
            Path::new("/tmp/a.ac3"),
            Path::new("/tmp/b.dts")
        ));
    }

    #[test]
    fn same_audio_extension_rejects_missing_extension() {
        assert!(!same_audio_extension(
            Path::new("/tmp/noext"),
            Path::new("/tmp/b.flac")
        ));
        assert!(!same_audio_extension(
            Path::new("/tmp/a.flac"),
            Path::new("/tmp/noext")
        ));
    }
}
