// SPDX-License-Identifier: GPL-3.0-or-later

//! What a save before quitting could not save, as the UI puts it in words.

use crate::save_cause::{SaveCause, SaveProblem};

/// What could not be saved before quitting, and why: the files it concerns,
/// one cause the UI can tell in plain words, and the system's text. The UI
/// builds its sentences from these; it never reads the text for them.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct QuitSaveFailure {
    pub settings: bool,
    pub queue: bool,
    /// Last.fm's scrobbles that are kept only in memory.
    pub scrobbles: bool,
    /// The cause the failed saves share; [`SaveCause::Other`] when they
    /// differ.
    pub cause: SaveCause,
    /// The system's text of each failed save, each once, in order.
    pub details: Vec<String>,
}

/// What a failed save before quitting concerns. Sent to the UI as a bit
/// each; `QuitSaveFailure` in `ui/src/BinaryBridgeCodec.h` mirrors them.
pub(super) const QUIT_SAVE_SETTINGS: u8 = 1;
pub(super) const QUIT_SAVE_QUEUE: u8 = 1 << 1;
pub(super) const QUIT_SAVE_SCROBBLES: u8 = 1 << 2;

impl QuitSaveFailure {
    /// The settings could not be saved, for `problem`.
    pub(super) fn settings(problem: SaveProblem) -> Self {
        Self {
            settings: true,
            ..Self::of(problem)
        }
    }

    /// The queue could not be saved, for `problem`.
    pub(super) fn queue(problem: SaveProblem) -> Self {
        Self {
            queue: true,
            ..Self::of(problem)
        }
    }

    /// Neither the settings nor the queue could be saved, for `problem`.
    pub(super) fn settings_and_queue(problem: SaveProblem) -> Self {
        Self {
            settings: true,
            queue: true,
            ..Self::of(problem)
        }
    }

    /// Last.fm's scrobbles could not be saved, for `problem`.
    pub(super) fn scrobbles(problem: SaveProblem) -> Self {
        Self {
            scrobbles: true,
            ..Self::of(problem)
        }
    }

    fn of(problem: SaveProblem) -> Self {
        Self {
            cause: problem.cause,
            details: if problem.detail.is_empty() {
                Vec::new()
            } else {
                vec![problem.detail]
            },
            ..Self::default()
        }
    }

    /// This failure and `other` together: what either concerns, the cause
    /// they share, and the text of both.
    #[must_use]
    pub(super) fn and(mut self, other: Self) -> Self {
        self.settings |= other.settings;
        self.queue |= other.queue;
        self.scrobbles |= other.scrobbles;
        if self.cause != other.cause {
            self.cause = SaveCause::Other;
        }
        for detail in other.details {
            if !self.details.contains(&detail) {
                self.details.push(detail);
            }
        }
        self
    }

    /// The failures in `failures` together, or `None` for none.
    pub(super) fn joined(failures: impl IntoIterator<Item = Self>) -> Option<Self> {
        failures.into_iter().reduce(Self::and)
    }

    /// What it concerns, as the bits the UI reads.
    pub(super) fn subjects(&self) -> u8 {
        [
            (self.settings, QUIT_SAVE_SETTINGS),
            (self.queue, QUIT_SAVE_QUEUE),
            (self.scrobbles, QUIT_SAVE_SCROBBLES),
        ]
        .into_iter()
        .filter_map(|(set, bit)| set.then_some(bit))
        .fold(0, |subjects, bit| subjects | bit)
    }

    /// The system's text, a line for each failed save.
    pub(super) fn detail(&self) -> String {
        self.details.join("\n")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::ErrorKind;

    #[test]
    fn failures_together_concern_both_and_keep_a_cause_only_they_share() {
        let settings = QuitSaveFailure::settings(SaveProblem::io(
            ErrorKind::PermissionDenied,
            "failed to replace /c/settings.ini: Permission denied (os error 13)",
        ));
        let queue = QuitSaveFailure::queue(SaveProblem::io(
            ErrorKind::PermissionDenied,
            "failed to replace /c/session.ini: Permission denied (os error 13)",
        ));
        let both = settings.clone().and(queue);
        assert!(both.settings && both.queue && !both.scrobbles);
        assert_eq!(both.subjects(), QUIT_SAVE_SETTINGS | QUIT_SAVE_QUEUE);
        assert_eq!(both.cause, SaveCause::Permission);
        assert_eq!(
            both.detail(),
            "failed to replace /c/settings.ini: Permission denied (os error 13)\n\
             failed to replace /c/session.ini: Permission denied (os error 13)"
        );

        // Last.fm's busy cause is not the settings' permission: no cause.
        let all = both.and(QuitSaveFailure::scrobbles(SaveProblem {
            cause: SaveCause::Busy,
            detail: String::new(),
        }));
        assert_eq!(all.subjects(), 0b111);
        assert_eq!(all.cause, SaveCause::Other);
        assert_eq!(all.details.len(), 2, "a cause without text adds no line");

        // The same text twice, as when there is no config folder, is shown once.
        let twice = QuitSaveFailure::joined([settings.clone(), settings.clone()]).unwrap();
        assert_eq!(twice.details, settings.details);
        assert_eq!(QuitSaveFailure::joined([]), None);
    }
}
