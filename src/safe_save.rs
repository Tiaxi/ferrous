// SPDX-License-Identifier: GPL-3.0-or-later

//! Crash-safe writes of user files.
//!
//! Tag writers rewrite files in place, so an interruption can leave a
//! truncated file and a concurrent reader can observe a half-written one.
//! Every write here goes to a new file in the same directory, which is
//! renamed into place only after it is complete and on disk.

use std::fs::{self, File, OpenOptions, Permissions};
use std::io::{self, Seek, SeekFrom, Write};
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
    let Writable {
        target,
        mut original,
        permissions,
    } = Writable::open(path)?;
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
    temp.commit(&target, Some(permissions))
}

/// Replace the contents of the existing file `path` with `bytes`, keeping
/// its permissions. Links are resolved as in [`rewrite_via_temp_copy`], and
/// the file is unchanged when any step fails.
///
/// # Errors
///
/// Returns a message naming the failed step, as [`rewrite_via_temp_copy`]
/// does.
pub(crate) fn replace_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let Writable {
        target,
        original,
        permissions,
    } = Writable::open(path)?;
    drop(original);
    let mut temp = TempSibling::create(&target)?;
    temp.write_all(bytes)?;
    temp.commit(&target, Some(permissions))
}

/// Create the file `path` holding `bytes`. Nothing that already exists at
/// `path`, including a dangling link, is replaced.
///
/// # Errors
///
/// Returns a message when something exists at `path`, or when the file
/// cannot be written, flushed, or moved into place.
pub(crate) fn create_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut temp = TempSibling::create(path)?;
    temp.write_all(bytes)?;
    // The rename would replace an existing file, so check right before it.
    // Something created in between is the only case this cannot catch.
    match fs::symlink_metadata(path) {
        Ok(_) => return Err(format!("{} already exists", path.display())),
        Err(err) if err.kind() == io::ErrorKind::NotFound => {}
        Err(err) => return Err(format!("failed to check {}: {err}", path.display())),
    }
    temp.commit(path, None)
}

/// Check, without changing anything, that [`rewrite_via_temp_copy`] could
/// replace `path` now: the file opens for writing and its directory accepts
/// the temporary copy.
///
/// # Errors
///
/// Returns the message the rewrite would fail with for those steps.
pub(crate) fn check_rewritable(path: &Path) -> Result<(), String> {
    let writable = Writable::open(path)?;
    TempSibling::create(&writable.target).map(drop)
}

/// An existing file opened the way a rewrite needs it.
struct Writable {
    /// `path` with links resolved.
    target: PathBuf,
    original: File,
    permissions: Permissions,
}

impl Writable {
    fn open(path: &Path) -> Result<Self, String> {
        let target = fs::canonicalize(path)
            .map_err(|err| format!("failed to resolve {}: {err}", path.display()))?;
        // Opening for writing keeps the permission check an in-place write
        // had: read-only files and file systems are rejected before anything
        // changes.
        let original = OpenOptions::new()
            .read(true)
            .write(true)
            .open(&target)
            .map_err(|err| format!("failed to open {} for writing: {err}", target.display()))?;
        let permissions = original
            .metadata()
            .map_err(|err| format!("failed to read metadata for {}: {err}", target.display()))?
            .permissions();
        Ok(Self {
            target,
            original,
            permissions,
        })
    }
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
        // copy as audio or as a cover image.
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

    fn write_all(&mut self, bytes: &[u8]) -> Result<(), String> {
        self.file
            .write_all(bytes)
            .map_err(|err| format!("failed to write {}: {err}", self.path.display()))
    }

    /// Flush the file and rename it to `target`, with `permissions` if given.
    fn commit(mut self, target: &Path, permissions: Option<Permissions>) -> Result<(), String> {
        if let Some(permissions) = permissions {
            self.file.set_permissions(permissions).map_err(|err| {
                format!(
                    "failed to set permissions on {}: {err}",
                    self.path.display()
                )
            })?;
        }
        self.file
            .sync_all()
            .map_err(|err| format!("failed to flush {}: {err}", self.path.display()))?;
        fs::rename(&self.path, target).map_err(|err| {
            format!(
                "failed to replace {} with {}: {err}",
                target.display(),
                self.path.display()
            )
        })?;
        self.persisted = true;
        let parent = target
            .parent()
            .ok_or_else(|| format!("missing parent directory for {}", target.display()))?;
        File::open(parent)
            .and_then(|dir| dir.sync_all())
            .map_err(|err| format!("failed to flush directory {}: {err}", parent.display()))
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

    #[test]
    fn replacing_a_file_keeps_its_permissions_and_leaves_no_copy() {
        let dir = test_dir("replace-file");
        let path = dir.join("cover.jpg");
        fs::write(&path, b"old cover").expect("write original");
        #[cfg(unix)]
        fs::set_permissions(&path, fs::Permissions::from_mode(0o640)).expect("chmod");

        replace_file(&path, b"new cover").expect("replace");

        assert_eq!(fs::read(&path).expect("read result"), b"new cover");
        assert_eq!(dir_entries(&dir), vec!["cover.jpg".to_string()]);
        #[cfg(unix)]
        assert_eq!(
            fs::metadata(&path).expect("metadata").permissions().mode() & 0o777,
            0o640
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn replacing_a_missing_file_creates_nothing() {
        let dir = test_dir("replace-missing");

        replace_file(&dir.join("cover.jpg"), b"cover").expect_err("nothing to replace");

        assert!(dir_entries(&dir).is_empty());
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn creating_a_file_writes_it_once_and_never_replaces_anything() {
        let dir = test_dir("create");
        let path = dir.join("cover.png");

        create_file(&path, b"first").expect("create");
        let error = create_file(&path, b"second").expect_err("file exists");

        assert!(error.contains("already exists"), "{error}");
        assert_eq!(fs::read(&path).expect("read result"), b"first");
        assert_eq!(dir_entries(&dir), vec!["cover.png".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn creating_a_file_keeps_a_dangling_link_with_its_name() {
        let dir = test_dir("create-dangling");
        let link = dir.join("cover.png");
        std::os::unix::fs::symlink(dir.join("missing.png"), &link).expect("create symlink");

        create_file(&link, b"cover").expect_err("link exists");

        assert!(fs::symlink_metadata(&link)
            .expect("link metadata")
            .file_type()
            .is_symlink());
        assert_eq!(dir_entries(&dir), vec!["cover.png".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn checking_a_writable_file_changes_nothing() {
        let dir = test_dir("check");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");

        check_rewritable(&path).expect("writable");

        assert_eq!(fs::read(&path).expect("read original"), b"original");
        assert_eq!(dir_entries(&dir), vec!["song.flac".to_string()]);
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
        let error = check_rewritable(&path).expect_err("read-only file is rejected");
        assert!(error.contains("for writing"), "{error}");
        assert_eq!(fs::read(&path).expect("read original"), b"original");
        assert_eq!(dir_entries(&dir), vec!["song.flac".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
