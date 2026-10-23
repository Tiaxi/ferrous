// SPDX-License-Identifier: GPL-3.0-or-later

//! Crash-safe rewrites of user audio files.
//!
//! Tag writers rewrite files in place, so an interruption can leave a
//! truncated file and a concurrent reader can observe a half-written one.
//! [`rewrite_via_temp_copy`] edits a copy in the same directory and renames
//! it over the original only after the copy is complete and on disk.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

/// Replace `path` with an edited copy of itself.
///
/// The original is copied to a new file in its own directory and `edit`
/// receives that copy, positioned at the start. After `edit` succeeds, the
/// copy takes the original's permissions, is flushed to disk, and is renamed
/// over the original. The original stays untouched when any step fails, and
/// the temporary copy is removed.
///
/// Symbolic links are resolved first so the link itself survives. Other hard
/// links to the file keep the previous contents, and extended attributes and
/// ownership are not copied.
///
/// # Errors
///
/// Returns a message naming the failed step: when the original is not
/// writable, its directory does not allow new files, or `edit`, the copy,
/// the flush, or the rename fails.
pub(crate) fn rewrite_via_temp_copy<F>(path: &Path, edit: F) -> Result<(), String>
where
    F: FnOnce(&mut File) -> Result<(), String>,
{
    let target = fs::canonicalize(path)
        .map_err(|err| format!("failed to resolve {}: {err}", path.display()))?;
    // Opening for writing keeps the permission check an in-place write had:
    // read-only files and file systems are rejected before anything changes.
    let mut original = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&target)
        .map_err(|err| format!("failed to open {} for writing: {err}", target.display()))?;
    let permissions = original
        .metadata()
        .map_err(|err| format!("failed to read metadata for {}: {err}", target.display()))?
        .permissions();
    let parent = target
        .parent()
        .ok_or_else(|| format!("missing parent directory for {}", target.display()))?;

    let mut temp = TempSibling::create(&target)?;
    io::copy(&mut original, &mut temp.file).map_err(|err| {
        format!(
            "failed to copy {} to {}: {err}",
            target.display(),
            temp.path.display()
        )
    })?;
    drop(original);
    temp.file
        .seek(SeekFrom::Start(0))
        .map_err(|err| format!("failed to rewind {}: {err}", temp.path.display()))?;
    edit(&mut temp.file)?;
    temp.file.set_permissions(permissions).map_err(|err| {
        format!(
            "failed to set permissions on {}: {err}",
            temp.path.display()
        )
    })?;
    temp.file
        .sync_all()
        .map_err(|err| format!("failed to flush {}: {err}", temp.path.display()))?;
    temp.persist(&target)?;
    File::open(parent)
        .and_then(|dir| dir.sync_all())
        .map_err(|err| format!("failed to flush directory {}: {err}", parent.display()))
}

/// A new file next to the target that is removed unless persisted.
struct TempSibling {
    path: PathBuf,
    file: File,
    persisted: bool,
}

impl TempSibling {
    fn create(target: &Path) -> Result<Self, String> {
        let parent = target
            .parent()
            .ok_or_else(|| format!("missing parent directory for {}", target.display()))?;
        let name = target
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        // The `.tmp` extension keeps library scans from treating a leftover
        // copy as audio.
        let path = parent.join(format!(
            ".{name}.ferrous-{}-{}-{nanos}.tmp",
            std::process::id(),
            TEMP_COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)
            .map_err(|err| {
                format!(
                    "failed to create temporary file in {}: {err}",
                    parent.display()
                )
            })?;
        Ok(Self {
            path,
            file,
            persisted: false,
        })
    }

    fn persist(mut self, target: &Path) -> Result<(), String> {
        fs::rename(&self.path, target).map_err(|err| {
            format!(
                "failed to replace {} with {}: {err}",
                target.display(),
                self.path.display()
            )
        })?;
        self.persisted = true;
        Ok(())
    }
}

impl Drop for TempSibling {
    fn drop(&mut self) {
        if !self.persisted {
            // Only this process created the file; nothing else can depend on it.
            let _ = fs::remove_file(&self.path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-safe-save-{name}-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        dir
    }

    fn dir_entries(dir: &Path) -> Vec<String> {
        let mut names = fs::read_dir(dir)
            .expect("read test dir")
            .map(|entry| {
                entry
                    .expect("dir entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        names.sort();
        names
    }

    fn replace_contents(file: &mut File, bytes: &[u8]) -> Result<(), String> {
        file.set_len(0).map_err(|err| err.to_string())?;
        file.rewind().map_err(|err| err.to_string())?;
        file.write_all(bytes).map_err(|err| err.to_string())
    }

    #[test]
    fn edits_a_copy_and_replaces_the_original() {
        let dir = test_dir("replace");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        #[cfg(unix)]
        fs::set_permissions(&path, fs::Permissions::from_mode(0o640)).expect("chmod");
        let mut reader = File::open(&path).expect("open reader");

        rewrite_via_temp_copy(&path, |file| {
            let mut copied = Vec::new();
            file.read_to_end(&mut copied)
                .map_err(|err| err.to_string())?;
            assert_eq!(copied, b"original");
            // The original is untouched until the edited copy is complete.
            assert_eq!(fs::read(&path).expect("read original"), b"original");
            replace_contents(file, b"edited")
        })
        .expect("rewrite");

        assert_eq!(fs::read(&path).expect("read result"), b"edited");
        assert_eq!(dir_entries(&dir), vec!["song.flac".to_string()]);
        #[cfg(unix)]
        assert_eq!(
            fs::metadata(&path).expect("metadata").permissions().mode() & 0o777,
            0o640
        );
        // A reader that opened the file before the save, such as playback,
        // keeps reading the complete previous contents.
        let mut previous = Vec::new();
        reader.read_to_end(&mut previous).expect("read old handle");
        assert_eq!(previous, b"original");
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn failed_edit_keeps_the_original_and_removes_the_copy() {
        let dir = test_dir("failure");
        let path = dir.join("song.mp3");
        fs::write(&path, b"original").expect("write original");

        let error = rewrite_via_temp_copy(&path, |file| {
            replace_contents(file, b"partial")?;
            Err("disk full".to_string())
        })
        .expect_err("edit failure is reported");

        assert_eq!(error, "disk full");
        assert_eq!(fs::read(&path).expect("read original"), b"original");
        assert_eq!(dir_entries(&dir), vec!["song.mp3".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_path_updates_the_target_and_keeps_the_link() {
        let dir = test_dir("symlink");
        let target = dir.join("real.flac");
        let link = dir.join("link.flac");
        fs::write(&target, b"original").expect("write target");
        std::os::unix::fs::symlink(&target, &link).expect("create symlink");

        rewrite_via_temp_copy(&link, |file| replace_contents(file, b"edited")).expect("rewrite");

        assert!(fs::symlink_metadata(&link)
            .expect("link metadata")
            .file_type()
            .is_symlink());
        assert_eq!(fs::read(&target).expect("read target"), b"edited");
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn read_only_file_is_rejected_without_changes() {
        let dir = test_dir("read-only");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fs::set_permissions(&path, fs::Permissions::from_mode(0o444)).expect("chmod");
        // Root ignores file permissions, so the check only holds for normal users.
        if OpenOptions::new().write(true).open(&path).is_ok() {
            fs::remove_dir_all(dir).expect("remove test dir");
            return;
        }

        let mut edited = false;
        let error = rewrite_via_temp_copy(&path, |_| {
            edited = true;
            Ok(())
        })
        .expect_err("read-only file is rejected");

        assert!(error.contains("for writing"), "{error}");
        assert!(!edited);
        assert_eq!(fs::read(&path).expect("read original"), b"original");
        assert_eq!(dir_entries(&dir), vec!["song.flac".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
