// SPDX-License-Identifier: GPL-3.0-or-later

//! On-disk storage for scrobbles that Last.fm has not accepted yet.

use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use super::ScrobbleEntry;

/// How many numbered names to try before giving up on moving a file aside.
const MAX_ASIDE_CANDIDATES: u32 = 1000;

#[derive(Debug)]
pub(super) enum LoadOutcome {
    /// The file held a valid queue, or there was no file.
    Loaded(Vec<ScrobbleEntry>),
    /// The file was not a valid queue. It was renamed, unchanged, to
    /// `moved_to` so that later saves cannot overwrite it.
    MovedAside { moved_to: PathBuf, reason: String },
    /// The file could not be read or moved aside. Saving must stay off so
    /// that the file is not overwritten.
    Unavailable { error: String },
}

pub(super) fn load(path: &Path) -> LoadOutcome {
    let bytes = match fs::read(path) {
        Ok(bytes) => bytes,
        // Nothing exists at the path, so there is nothing to preserve.
        Err(err)
            if matches!(
                err.kind(),
                io::ErrorKind::NotFound | io::ErrorKind::NotADirectory
            ) =>
        {
            return LoadOutcome::Loaded(Vec::new());
        }
        Err(err) => {
            return LoadOutcome::Unavailable {
                error: err.to_string(),
            }
        }
    };
    match serde_json::from_slice::<Vec<ScrobbleEntry>>(&bytes) {
        Ok(entries) => LoadOutcome::Loaded(entries),
        Err(parse_error) => match move_aside(path) {
            Ok(moved_to) => LoadOutcome::MovedAside {
                moved_to,
                reason: parse_error.to_string(),
            },
            Err(err) => LoadOutcome::Unavailable {
                error: format!("{parse_error}; moving it aside failed: {err}"),
            },
        },
    }
}

/// Replaces the queue file atomically, or removes it when `entries` is empty.
pub(super) fn save(path: &Path, entries: &[ScrobbleEntry]) -> io::Result<()> {
    if entries.is_empty() {
        return match fs::remove_file(path) {
            Err(err) if err.kind() != io::ErrorKind::NotFound => Err(err),
            _ => Ok(()),
        };
    }
    let text = serde_json::to_vec_pretty(entries).map_err(io::Error::other)?;
    write_atomically(path, &text)
}

fn write_atomically(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let parent = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(parent)?;
    let temporary = sibling_path(path, ".tmp");
    let written = write_synced(&temporary, bytes).and_then(|()| fs::rename(&temporary, path));
    if written.is_err() {
        let _ = fs::remove_file(&temporary);
        return written;
    }
    // Make the rename itself durable. The new contents are already on disk,
    // so a directory that cannot be synced is not treated as a failure.
    if let Ok(directory) = File::open(parent) {
        let _ = directory.sync_all();
    }
    Ok(())
}

fn write_synced(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let mut file = File::create(path)?;
    file.write_all(bytes)?;
    file.sync_all()
}

fn move_aside(path: &Path) -> io::Result<PathBuf> {
    for index in 0..MAX_ASIDE_CANDIDATES {
        let suffix = if index == 0 {
            ".corrupt".to_string()
        } else {
            format!(".corrupt-{index}")
        };
        let candidate = sibling_path(path, &suffix);
        if fs::symlink_metadata(&candidate).is_ok() {
            continue;
        }
        fs::rename(path, &candidate)?;
        return Ok(candidate);
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "every name for the moved-aside copy is taken",
    ))
}

fn sibling_path(path: &Path, suffix: &str) -> PathBuf {
    let mut name = path
        .file_name()
        .map_or_else(OsString::new, ToOwned::to_owned);
    name.push(suffix);
    path.with_file_name(name)
}
