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

use super::{PathRenames, RepeatMode};

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
    }

    /// Restart navigation after the queue order changed.
    fn reset_navigation(&mut self, len: usize) {
        self.clear_shuffle_navigation();
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
                        return true;
                    }
                }
                self.advance_sequential(len, rules, false)
            }
        }
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
            return true;
        }
        self.advance_shuffle_random(len, rules)
    }

    fn advance_shuffle_random(&mut self, len: usize, rules: Rules) -> bool {
        let allow_repeat_cycle = rules.repeat_mode == RepeatMode::All;
        if len == 1 {
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
}

/// The successor that `stage_natural` staged.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct StagedAdvance {
    pub(crate) path: PathBuf,
    pub(crate) handoff: Handoff,
    /// Ask analysis to prepare a spectrogram continuation. Only the
    /// gapless successor of the audible track is prepared.
    pub(crate) staged_spectrogram: bool,
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

    /// Path of the row at `index`.
    #[cfg(any(test, feature = "gst"))]
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
        let cursor = self.natural_successor(&self.cursor)?;
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
        self.cursor.reset_navigation(self.queue.len());
        cancelled
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
        self.cursor.reset_navigation(self.queue.len());
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
        self.cursor.index =
            super::remove_queue_indices(&mut self.queue, Some(self.cursor.index), &removed)
                .unwrap_or(0);
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

    /// The cursor of the natural successor of `from` that has not failed.
    ///
    /// The search ends once no row that a natural advance can still reach
    /// is left unfailed. A shuffled advance can revisit a failed row when
    /// it refreshes its pool, so a fixed number of advances is no bound.
    fn natural_successor(&self, from: &Cursor) -> Option<Cursor> {
        let len = self.queue.len();
        let rules = self.rules();
        let mut cursor = from.clone();
        // While a reachable row has not failed, this many advances reach
        // one: the shuffle navigation and pool of `from`, then at most two
        // refreshed pools, which leave out only the row they start from. A
        // sequential advance needs at most `len`.
        let steps = from.shuffle_forward.len() + from.shuffle_pool.len() + 2 * len;
        for _ in 0..steps {
            if !self.reachable_row_not_failed(&cursor)
                || !cursor.advance(len, rules, Advance::Natural)
            {
                return None;
            }
            if !self.row_failed(cursor.index) {
                return Some(cursor);
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
        let cursor = self.natural_successor(feeding)?;
        let path = self.queue.get(cursor.index)?.clone();
        let handoff = if self.handoffs_held {
            Handoff::AtEos
        } else {
            handoff(&finishing, &path)
        };
        let staged_spectrogram = handoff == Handoff::Gapless && self.pending.is_empty();
        self.pending.push_back(PendingAdvance {
            path: path.clone(),
            cursor,
            handoff,
            staged_spectrogram,
            random_successor: rules.random_successor(),
        });
        Some(StagedAdvance {
            path,
            handoff,
            staged_spectrogram,
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
        let cursor = self.natural_successor(&self.cursor)?;
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

    /// Keep the staged successors that the current queue and settings
    /// would still choose, in order, and drop the rest.
    fn revalidate_pending(&mut self) -> Option<CancelledAdvance> {
        let rules = self.rules();
        let mut previous = self.cursor.index;
        let cut = self.pending.iter().position(|pending| {
            let valid = self.queue.get(pending.cursor.index) == Some(&pending.path)
                && if rules.random_successor() {
                    pending.random_successor
                } else {
                    self.sequential_successor(previous) == Some(pending.cursor.index)
                };
            previous = pending.cursor.index;
            !valid
        })?;
        self.drop_pending_from(cut)
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
        RepeatMode,
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
        assert_eq!(found.map(|cursor| cursor.index), expected, "{case}");
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
