// SPDX-License-Identifier: GPL-3.0-or-later

//! The errors the bridge shows the user, with what they concern.

use std::fmt;

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
