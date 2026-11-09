// SPDX-License-Identifier: GPL-3.0-or-later

//! The errors the bridge shows the user, with what they concern.

use std::fmt;
use std::path::Path;
use std::sync::OnceLock;
use std::time::Duration;

/// What part of Ferrous an error concerns. The values are sent to the UI;
/// `BinaryBridgeCodec::ErrorCategory` in `ui/src/BinaryBridgeCodec.h` and
/// `ui/qml/logic/ErrorUtils.js` mirror them.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ErrorCategory {
    /// The bridge, the UI, and the desktop.
    System = 0,
    /// Playing files, and the queue.
    Playback = 1,
    /// The library, its scans, and album art.
    Library = 2,
    /// The waveform and the spectrogram.
    Analysis = 3,
    /// Tag edits and renames.
    Tags = 4,
    /// Last.fm scrobbling.
    LastFm = 5,
    /// Saving the settings and the session.
    Settings = 6,
}

impl ErrorCategory {
    /// The category's name in the diagnostics log. `errorCategoryLogName`
    /// in `ui/src/BridgeClient.cpp` names the errors it reports the same way.
    #[must_use]
    pub fn log_name(self) -> &'static str {
        match self {
            Self::System => "system",
            Self::Playback => "playback",
            Self::Library => "library",
            Self::Analysis => "analysis",
            Self::Tags => "tags",
            Self::LastFm => "lastfm",
            Self::Settings => "settings",
        }
    }
}

/// How much an error matters. Sent to the UI as its value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ErrorSeverity {
    /// Ferrous worked around it, for example by skipping a file.
    Warning = 0,
    /// Something the user asked for did not happen.
    Error = 1,
}

/// An error for the user.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BridgeError {
    pub category: ErrorCategory,
    pub severity: ErrorSeverity,
    /// The file or folder the error is about, if one; empty otherwise.
    pub subject: String,
    pub message: String,
}

impl BridgeError {
    /// An error of `category`.
    pub fn error(category: ErrorCategory, message: impl Into<String>) -> Self {
        Self {
            category,
            severity: ErrorSeverity::Error,
            subject: String::new(),
            message: message.into(),
        }
    }

    /// A warning of `category`.
    pub fn warning(category: ErrorCategory, message: impl Into<String>) -> Self {
        Self {
            severity: ErrorSeverity::Warning,
            ..Self::error(category, message)
        }
    }

    /// The error, about `subject`.
    #[must_use]
    pub fn about(mut self, subject: impl Into<String>) -> Self {
        self.subject = subject.into();
        self
    }
}

impl fmt::Display for BridgeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

/// Logs a warning about `category` to the diagnostics log only, for what the
/// user need not see in Recent Issues: a condition Ferrous recovers from by
/// itself, a retry, or a fallback that works. The UI copies standard error
/// to the log (`ConsoleTee`).
pub(crate) fn log_warning(category: ErrorCategory, message: &str) {
    log_only(category, "warning", message);
}

/// Logs `message` about `category` to the diagnostics log only, as
/// [`log_warning`] does, for news such as a recovery.
pub(crate) fn log_info(category: ErrorCategory, message: &str) {
    log_only(category, "info", message);
}

/// Whether `FERROUS_LOG=debug` asks for the debug lines of [`log_debug`].
/// Read once: the environment does not change while the app runs.
pub(crate) fn debug_logging() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| {
        std::env::var("FERROUS_LOG").is_ok_and(|level| level.trim().eq_ignore_ascii_case("debug"))
    })
}

/// Logs the line `message` makes about `category`, only with
/// `FERROUS_LOG=debug`: details that would crowd the event trace that
/// [`log_info`] writes. `message` is called only then, so a caller pays for
/// no formatting otherwise.
pub(crate) fn log_debug(category: ErrorCategory, message: impl FnOnce() -> String) {
    if debug_logging() {
        log_only(category, "debug", &message());
    }
}

/// `path` as the log lines quote a path, in double quotes, so that Copy Log
/// Without Paths (`DiagnosticsLog::withoutPaths` in the UI) can tell where
/// it ends.
pub(crate) fn quoted_path(path: &Path) -> String {
    format!("\"{}\"", path.display())
}

/// A playback position or a duration as the log shows it: `m:ss.t`, with
/// hours when there are any.
pub(crate) fn log_position(position: Duration) -> String {
    let tenths = position.as_millis() / 100;
    let (hours, minutes, seconds, tenth) = (
        tenths / 36_000,
        tenths / 600 % 60,
        tenths / 10 % 60,
        tenths % 10,
    );
    if hours > 0 {
        format!("{hours}:{minutes:02}:{seconds:02}.{tenth}")
    } else {
        format!("{minutes}:{seconds:02}.{tenth}")
    }
}

/// Logs `error`, which could not reach Recent Issues, as
/// `BridgeClient::reportError` logs the errors it shows. An error that
/// reaches it is logged there, so only one line is written for each.
pub(crate) fn log_undelivered(error: &BridgeError) {
    let level = match error.severity {
        ErrorSeverity::Warning => "warning",
        ErrorSeverity::Error => "error",
    };
    if error.subject.is_empty() {
        log_only(error.category, level, &error.message);
    } else {
        log_only(
            error.category,
            level,
            &format!("{} ({})", error.message, error.subject),
        );
    }
}

fn log_only(category: ErrorCategory, level: &str, message: &str) {
    // One line each: the log is read line by line.
    let message = message.replace('\n', "\\n").replace('\r', "\\r");
    let line = format!("[ferrous] [{}] {level}: {message}", category.log_name());
    eprintln!("{line}");
    #[cfg(test)]
    test_log::record(line);
}

/// Tests that do not care about the category send system errors.
#[cfg(test)]
impl From<String> for BridgeError {
    fn from(message: String) -> Self {
        Self::error(ErrorCategory::System, message)
    }
}

#[cfg(test)]
impl From<&str> for BridgeError {
    fn from(message: &str) -> Self {
        Self::error(ErrorCategory::System, message)
    }
}

/// What [`log_warning`], [`log_info`] and [`log_debug`] wrote on this
/// thread, for tests
/// that log on their own thread.
#[cfg(test)]
pub(crate) mod test_log {
    use std::cell::RefCell;

    thread_local! {
        static LINES: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    }

    pub(super) fn record(line: String) {
        LINES.with(|lines| lines.borrow_mut().push(line));
    }

    /// Forgets the lines this thread logged so far.
    pub(crate) fn clear() {
        LINES.with(|lines| lines.borrow_mut().clear());
    }

    /// The lines this thread logged so far.
    pub(crate) fn all() -> Vec<String> {
        LINES.with(|lines| lines.borrow().clone())
    }

    /// The lines this thread logged so far that contain `needle`.
    pub(crate) fn lines_containing(needle: &str) -> Vec<String> {
        LINES.with(|lines| {
            lines
                .borrow()
                .iter()
                .filter(|line| line.contains(needle))
                .cloned()
                .collect()
        })
    }
}
