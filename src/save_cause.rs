// SPDX-License-Identifier: GPL-3.0-or-later

//! Why a file Ferrous keeps for itself (the settings, the queue, Last.fm's
//! scrobbles) could not be saved, as a kind beside the system's own text.
//!
//! The UI puts the kind in words; it never reads the cause from the text.
//! The text stays as the system gave it, for the log and for whoever needs
//! the exact error.

use std::io;

/// A failed save's cause, as the UI tells it. The values are sent to the UI;
/// `QuitSaveCause` in `ui/src/BinaryBridgeCodec.h` mirrors them, and
/// `BridgeClient::quitSaveFailureMap` names them for the UI.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
pub enum SaveCause {
    /// None the user can be told in plain words; the text says it.
    #[default]
    Other = 0,
    /// The system refused to write in the file's folder.
    Permission = 1,
    /// The file system is mounted read-only.
    ReadOnlyFileSystem = 2,
    /// The disk is full.
    NoSpace = 3,
    /// Last.fm was busy, possibly waiting for the network, and did not
    /// save its scrobbles in time.
    Busy = 4,
}

impl SaveCause {
    /// The cause of a system error of `kind`.
    #[must_use]
    pub fn of(kind: io::ErrorKind) -> Self {
        match kind {
            io::ErrorKind::PermissionDenied => Self::Permission,
            io::ErrorKind::ReadOnlyFilesystem => Self::ReadOnlyFileSystem,
            io::ErrorKind::StorageFull => Self::NoSpace,
            _ => Self::Other,
        }
    }
}

/// A file that could not be saved: why, and the system's text for it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SaveProblem {
    pub cause: SaveCause,
    /// What failed, in the system's words, such as "failed to replace
    /// /path/settings.ini: Permission denied (os error 13)"; empty when the
    /// cause says it all.
    pub detail: String,
}

impl SaveProblem {
    /// `detail`, caused by a system error of `kind`.
    #[must_use]
    pub fn io(kind: io::ErrorKind, detail: impl Into<String>) -> Self {
        Self {
            cause: SaveCause::of(kind),
            detail: detail.into(),
        }
    }

    /// `detail`, of no cause the user can be told in plain words.
    #[must_use]
    pub fn other(detail: impl Into<String>) -> Self {
        Self {
            cause: SaveCause::Other,
            detail: detail.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_system_error_has_its_cause_or_none() {
        for (errno, cause) in [
            (libc::EACCES, SaveCause::Permission),
            (libc::EPERM, SaveCause::Permission),
            (libc::EROFS, SaveCause::ReadOnlyFileSystem),
            (libc::ENOSPC, SaveCause::NoSpace),
            // A quota is not the disk's space.
            (libc::EDQUOT, SaveCause::Other),
            (libc::EIO, SaveCause::Other),
            (libc::ENOENT, SaveCause::Other),
        ] {
            let error = io::Error::from_raw_os_error(errno);
            assert_eq!(SaveCause::of(error.kind()), cause, "{error}");
        }
    }
}
