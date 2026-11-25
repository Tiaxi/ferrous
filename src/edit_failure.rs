// SPDX-License-Identifier: GPL-3.0-or-later

//! Why saving or renaming a user's file failed, as a code beside the message.
//!
//! The message says it in English, for the log and as the system's own text.
//! What failed is kept apart from it, so that the cause is never read from
//! the words: [`EditFailure::io`] and [`EditFailure::lofty`] keep the step
//! and the kind of the error where it happens. A failure made from a message
//! alone has no cause, and none is guessed for it.

use std::fmt;
use std::io;

use lofty::error::{ErrorKind, LoftyError};
use serde::Serialize;

/// The step an operating system error stopped. Only steps whose errors can
/// tell the user a cause are listed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EditStep {
    /// Opening the file to read what it holds.
    OpenToRead,
    /// Opening the file to change it.
    OpenToWrite,
    /// Creating the temporary copy in the file's folder.
    CreateTemporary,
    /// Copying, editing, or flushing the temporary copy.
    Write,
    /// Renaming the finished copy over the file.
    Replace,
    /// Renaming a file of a batch, which stops at the first file that fails.
    Rename,
}

/// A cause as the tag editor's results send it, for the UI to put in words.
/// Keep it in step with `failureCauseText` in `ui/src/TagEditorController.cpp`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum FailureCause {
    FolderNotWritable,
    FileNotWritable,
    FileNotReplaceable,
    FileNotReadable,
    ReadOnlyFileSystem,
    NoSpace,
    RenameNotPermitted,
    RenameReadOnlyFileSystem,
    RenameNoSpace,
    /// The file was not renamed, as renaming stopped at another file.
    RenameStoppedElsewhere,
    /// The file was renamed and then given its name back, as renaming
    /// stopped at another file.
    RenameUndone,
    /// The file was renamed, and could not be given its name back when
    /// renaming stopped at another file.
    RenameNotUndone,
}

/// A failed save or rename: its message and, where the system refused a
/// step, that step and the kind of the error.
#[derive(Debug)]
pub(crate) struct EditFailure {
    message: String,
    origin: Option<(EditStep, io::ErrorKind)>,
}

impl EditFailure {
    /// `message`, for `error` at `step`.
    pub(crate) fn io(step: EditStep, error: &io::Error, message: String) -> Self {
        Self {
            message,
            origin: Some((step, error.kind())),
        }
    }

    /// `message`, for `error` from lofty at `step`. Only its I/O errors have
    /// a cause.
    pub(crate) fn lofty(step: EditStep, error: &LoftyError, message: String) -> Self {
        let io_error = match error.kind() {
            ErrorKind::Io(error) => Some(error),
            // lofty writes an Ogg stream's first pages through ogg_pager,
            // whose error holds the system's as its source.
            ErrorKind::OggPage(page) => std::error::Error::source(page)
                .and_then(|source| source.downcast_ref::<io::Error>()),
            _ => None,
        };
        match io_error {
            Some(error) => Self::io(step, error, message),
            None => message.into(),
        }
    }

    /// The same failure with its message changed by `change`.
    pub(crate) fn map_message(self, change: impl FnOnce(String) -> String) -> Self {
        Self {
            message: change(self.message),
            origin: self.origin,
        }
    }

    /// The cause to tell the user, where the step and error are sure of it.
    pub(crate) fn cause(&self) -> Option<FailureCause> {
        use io::ErrorKind::{PermissionDenied, ReadOnlyFilesystem, StorageFull};

        let cause = match self.origin? {
            (EditStep::OpenToRead, PermissionDenied) => FailureCause::FileNotReadable,
            (EditStep::OpenToWrite, PermissionDenied) => FailureCause::FileNotWritable,
            (EditStep::CreateTemporary, PermissionDenied) => FailureCause::FolderNotWritable,
            (EditStep::Replace, PermissionDenied) => FailureCause::FileNotReplaceable,
            (EditStep::Rename, PermissionDenied) => FailureCause::RenameNotPermitted,
            (EditStep::Rename, ReadOnlyFilesystem) => FailureCause::RenameReadOnlyFileSystem,
            (EditStep::Rename, StorageFull) => FailureCause::RenameNoSpace,
            (EditStep::OpenToRead, _) => return None,
            (_, ReadOnlyFilesystem) => FailureCause::ReadOnlyFileSystem,
            (_, StorageFull) => FailureCause::NoSpace,
            _ => return None,
        };
        Some(cause)
    }
}

impl From<String> for EditFailure {
    fn from(message: String) -> Self {
        Self {
            message,
            origin: None,
        }
    }
}

impl From<EditFailure> for String {
    fn from(failure: EditFailure) -> Self {
        failure.message
    }
}

impl fmt::Display for EditFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cause_of(step: EditStep, errno: i32) -> Option<FailureCause> {
        let error = io::Error::from_raw_os_error(errno);
        EditFailure::io(step, &error, error.to_string()).cause()
    }

    #[test]
    fn each_step_and_system_error_has_its_cause_or_none() {
        use EditStep as Step;
        use FailureCause as Cause;

        let expected = |step, errno| match (step, errno) {
            (Step::OpenToRead, libc::EACCES | libc::EPERM) => Some(Cause::FileNotReadable),
            (Step::OpenToWrite, libc::EACCES | libc::EPERM) => Some(Cause::FileNotWritable),
            (Step::CreateTemporary, libc::EACCES | libc::EPERM) => Some(Cause::FolderNotWritable),
            (Step::Replace, libc::EACCES | libc::EPERM) => Some(Cause::FileNotReplaceable),
            (Step::Rename, libc::EACCES | libc::EPERM) => Some(Cause::RenameNotPermitted),
            (Step::Rename, libc::EROFS) => Some(Cause::RenameReadOnlyFileSystem),
            (Step::Rename, libc::ENOSPC) => Some(Cause::RenameNoSpace),
            (Step::OpenToRead, _) => None,
            (_, libc::EROFS) => Some(Cause::ReadOnlyFileSystem),
            (_, libc::ENOSPC) => Some(Cause::NoSpace),
            _ => None,
        };
        for step in [
            Step::OpenToRead,
            Step::OpenToWrite,
            Step::CreateTemporary,
            Step::Write,
            Step::Replace,
            Step::Rename,
        ] {
            // A quota is not the disk's space, and the others say nothing
            // that the user could be told for sure.
            for errno in [
                libc::EACCES,
                libc::EPERM,
                libc::EROFS,
                libc::ENOSPC,
                libc::EDQUOT,
                libc::EIO,
                libc::ENOENT,
                libc::EEXIST,
                libc::EBUSY,
            ] {
                assert_eq!(
                    cause_of(step, errno),
                    expected(step, errno),
                    "{step:?}, errno {errno}"
                );
            }
        }
    }

    #[test]
    fn the_wording_of_a_message_has_no_effect_on_its_cause() {
        let full = io::Error::from_raw_os_error(libc::ENOSPC);
        for message in [
            "",
            "Laitteella ei ole tilaa",
            "failed to create temporary file in /m: Permission denied (os error 13)",
        ] {
            let failure = EditFailure::io(EditStep::Write, &full, message.to_string());
            assert_eq!(failure.cause(), Some(FailureCause::NoSpace), "{message}");
            assert_eq!(failure.to_string(), message);
            // The same words alone tell nothing.
            assert_eq!(EditFailure::from(message.to_string()).cause(), None);
        }
        let reworded = EditFailure::io(EditStep::Write, &full, "full".to_string())
            .map_message(|message| format!("{message}; the files keep their original names"));
        assert_eq!(reworded.cause(), Some(FailureCause::NoSpace));
        assert_eq!(
            reworded.to_string(),
            "full; the files keep their original names"
        );
    }

    #[test]
    fn only_io_errors_from_lofty_have_a_cause() {
        let full = LoftyError::from(io::Error::from_raw_os_error(libc::ENOSPC));
        let failure = EditFailure::lofty(EditStep::Write, &full, full.to_string());
        assert_eq!(failure.cause(), Some(FailureCause::NoSpace));

        let full_page = LoftyError::new(ErrorKind::OggPage(
            io::Error::from_raw_os_error(libc::ENOSPC).into(),
        ));
        let failure = EditFailure::lofty(EditStep::Write, &full_page, full_page.to_string());
        assert_eq!(failure.cause(), Some(FailureCause::NoSpace));

        let other = LoftyError::new(ErrorKind::UnsupportedTag);
        let failure = EditFailure::lofty(EditStep::Write, &other, other.to_string());
        assert_eq!(failure.cause(), None);
    }

    #[test]
    fn the_results_send_each_cause_as_its_code() {
        use FailureCause as Cause;

        // The codes the UI's test of the same list turns into words.
        for (cause, code) in [
            (Cause::FolderNotWritable, "folderNotWritable"),
            (Cause::FileNotWritable, "fileNotWritable"),
            (Cause::FileNotReplaceable, "fileNotReplaceable"),
            (Cause::FileNotReadable, "fileNotReadable"),
            (Cause::ReadOnlyFileSystem, "readOnlyFileSystem"),
            (Cause::NoSpace, "noSpace"),
            (Cause::RenameNotPermitted, "renameNotPermitted"),
            (Cause::RenameReadOnlyFileSystem, "renameReadOnlyFileSystem"),
            (Cause::RenameNoSpace, "renameNoSpace"),
            (Cause::RenameStoppedElsewhere, "renameStoppedElsewhere"),
            (Cause::RenameUndone, "renameUndone"),
            (Cause::RenameNotUndone, "renameNotUndone"),
        ] {
            assert_eq!(serde_json::to_value(cause).expect("serialize"), code);
        }
    }
}
