// SPDX-License-Identifier: GPL-3.0-or-later

//! Turning panics into errors where the app cannot let them end a thread.
//!
//! Tag and audio parsers read the user's files, and a malformed file can make
//! one panic: lofty 0.24 indexes past the end of a `COVERART` picture shorter
//! than 8 bytes, for example. Panics unwind, so the code that reads such files
//! catches them and reports the file like any other unreadable file, instead
//! of letting the panic end a worker thread or abort the process at the FFI
//! boundary.

use std::any::Any;
use std::io;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

use lofty::error::LoftyError;
use lofty::file::TaggedFile;

/// The message a panic was raised with.
pub(crate) fn panic_message(payload: &(dyn Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("unknown panic")
}

/// Run `body`, and log and return the panic's message if it panics.
///
/// The caller must discard whatever `body` was changing when it panicked, or
/// only share data with it that a panic cannot leave half updated; that is
/// what makes asserting unwind safety here sound.
pub(crate) fn catch<T>(what: &str, body: impl FnOnce() -> T) -> Result<T, String> {
    catch_unwind(AssertUnwindSafe(body)).map_err(|payload| {
        let message = format!(
            "internal error in {what}: {}",
            panic_message(payload.as_ref())
        );
        eprintln!("[ferrous] {message}");
        message
    })
}

/// Read the tags of `path`, reporting a panic in the tag reader as an error.
pub(crate) fn read_tags(path: &Path) -> lofty::error::Result<TaggedFile> {
    guard_tag_read(path, || lofty::read_from_path(path))
}

/// Read the tags of `path`, keeping a panic in the tag reader apart from an
/// ordinary read error: the outer error is the panic's message.
///
/// A panic means the reader failed on data it should have handled, not that
/// the file has no tags it understands.
pub(crate) fn read_tags_catching_panic(
    path: &Path,
) -> Result<lofty::error::Result<TaggedFile>, String> {
    catch_tag_read(path, || lofty::read_from_path(path))
}

/// Run a lofty read of `path`, reporting a panic in it as an error.
pub(crate) fn guard_tag_read<T>(
    path: &Path,
    read: impl FnOnce() -> lofty::error::Result<T>,
) -> lofty::error::Result<T> {
    catch_tag_read(path, read).unwrap_or_else(|_| {
        Err(LoftyError::from(io::Error::new(
            io::ErrorKind::InvalidData,
            "the tag reader failed on malformed tag data",
        )))
    })
}

fn catch_tag_read<T>(path: &Path, read: impl FnOnce() -> T) -> Result<T, String> {
    catch(&format!("the tag reader for '{}'", path.display()), read)
}

/// Panics on demand at named points, to test what catches them.
#[cfg(test)]
pub(crate) mod test_hooks {
    use std::cell::Cell;

    thread_local! {
        static PANIC_AT: Cell<Option<&'static str>> = const { Cell::new(None) };
    }

    /// Make the next [`checkpoint`] named `point` on this thread panic.
    pub(crate) fn panic_at(point: &'static str) {
        PANIC_AT.with(|cell| cell.set(Some(point)));
    }

    pub(crate) fn checkpoint(point: &str) {
        if PANIC_AT.with(Cell::get) == Some(point) {
            PANIC_AT.with(|cell| cell.set(None));
            panic!("test panic at {point}");
        }
    }
}

/// Files that make a parser panic.
#[cfg(test)]
pub(crate) mod fixtures {
    use crate::vorbis_comment_scan::fixtures::{comment_block, flac};

    /// A FLAC file with a `COVERART` picture too short for lofty 0.24, which
    /// panics on any read of it.
    pub(crate) fn short_coverart_flac() -> Vec<u8> {
        flac(&[&comment_block(&[b"TITLE=Broken", b"COVERART=AAAA"])])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_becomes_an_error_with_its_message() {
        let result: Result<(), String> = catch("the test", || panic!("boom"));
        assert_eq!(result, Err("internal error in the test: boom".to_string()));
        assert_eq!(catch("the test", || 7), Ok(7));
    }

    #[test]
    fn a_tag_reader_panic_is_a_read_error() {
        let dir = tempfile_dir("panic-guard-coverart");
        let path = dir.join("broken.flac");
        std::fs::write(&path, fixtures::short_coverart_flac()).expect("write fixture");
        // The fixture must still make lofty panic, or this test proves nothing.
        assert!(std::panic::catch_unwind(|| lofty::read_from_path(&path).is_ok()).is_err());

        let Err(error) = read_tags(&path) else {
            panic!("the read fails");
        };
        assert!(error.to_string().contains("malformed tag data"), "{error}");
        let _ = std::fs::remove_dir_all(dir);
    }

    fn tempfile_dir(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("ferrous-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("create test dir");
        dir
    }
}
