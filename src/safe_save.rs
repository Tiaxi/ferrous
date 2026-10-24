// SPDX-License-Identifier: GPL-3.0-or-later

//! Crash-safe writes of user files.
//!
//! Tag writers rewrite files in place, so an interruption can leave a
//! truncated file and a concurrent reader can observe a half-written one.
//! Every write here goes to a new file in the same directory, which is
//! renamed into place only after it is complete and on disk.
//!
//! A copy that replaces an existing file is created readable by its owner
//! only, so a private file is never exposed while it is written or when the
//! app stops before cleaning up. Before the rename the copy takes over the
//! original's metadata:
//!
//! - The owner and group, where the process may set them. Only root may give
//!   a file to another user, so in a shared, group-writable folder the owner
//!   becomes the user who saves. The group is still kept when that user
//!   belongs to it, so the other members can keep editing the file. The save
//!   goes ahead either way.
//! - Every `user.*` extended attribute and the POSIX access ACL
//!   (`system.posix_acl_access`). The save is refused, naming the attribute,
//!   when one of them cannot be copied. `security.*` attributes are left to
//!   the system, which labels the new file by its policy, and `trusted.*`
//!   attributes need privileges an editor does not have; other file-system
//!   specific attributes are not copied either.
//! - The permissions, including those the access ACL grants. The ACL sets
//!   the group permission bits, so it is applied after the mode, and the save
//!   is refused when the result differs from the original.
//!
//! The replaced path gets a new inode. Other hard links to the file keep its
//! previous contents, and the edited path splits off from them. Hard links
//! are not refused: seeding and library tools hard-link a download into the
//! library on purpose and rely on edits leaving the seeded copy intact.

use std::ffi::{CStr, CString};
use std::fs::{self, File, OpenOptions, Permissions};
use std::io::{self, Seek, SeekFrom, Write};
use std::os::unix::fs::{fchown, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::no_replace::rename_no_replace;

mod xattr;

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

/// The extended attribute holding a file's POSIX access ACL.
const ACCESS_ACL: &CStr = c"system.posix_acl_access";

/// Replace `path` with an edited copy of itself.
///
/// The original is copied to a new, owner-only file in its own directory and
/// `edit` receives that copy, positioned at the start. After `edit` succeeds,
/// the copy takes the original's owner, extended attributes, and permissions,
/// as the [module docs](self) describe, is flushed to disk, and is renamed
/// over the original. The original stays untouched when any step fails, and
/// the temporary copy is removed.
///
/// Symbolic links are resolved first so the link itself survives. Other hard
/// links to the file keep the previous contents.
///
/// # Errors
///
/// Returns a message naming the failed step: when the original is not
/// writable, its directory does not allow new files, or `edit`, the copy,
/// carrying over its metadata, the flush, or the rename fails.
pub(crate) fn rewrite_via_temp_copy<F>(path: &Path, edit: F) -> Result<(), String>
where
    F: FnOnce(&mut File) -> Result<(), String>,
{
    let Writable {
        target,
        mut original,
        metadata,
    } = Writable::open(path)?;
    let mut temp = TempSibling::create_private(&target)?;
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
    temp.commit(&target, Some(&metadata))
}

/// Replace the contents of the existing file `path` with `bytes`, keeping
/// its owner, extended attributes, and permissions. Links are resolved as in
/// [`rewrite_via_temp_copy`], and the file is unchanged when any step fails.
///
/// # Errors
///
/// Returns a message naming the failed step, as [`rewrite_via_temp_copy`]
/// does.
pub(crate) fn replace_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let Writable {
        target,
        original,
        metadata,
    } = Writable::open(path)?;
    drop(original);
    let mut temp = TempSibling::create_private(&target)?;
    temp.write_all(bytes)?;
    temp.commit(&target, Some(&metadata))
}

/// Create the file `path` holding `bytes`. Nothing that has the name
/// `path`, including a dangling link, is replaced, even when it appears
/// while the file is written. File systems without an atomic check, such
/// as NFS, keep a short window; see [`crate::no_replace`].
///
/// # Errors
///
/// Returns a message when something exists at `path`, or when the file
/// cannot be written, flushed, or moved into place.
pub(crate) fn create_file(path: &Path, bytes: &[u8]) -> Result<(), String> {
    create_file_with(path, bytes, || {})
}

/// [`create_file`] that calls `before_rename` once the file is complete, so
/// tests can create something at `path` right before it is moved there.
fn create_file_with(path: &Path, bytes: &[u8], before_rename: impl FnOnce()) -> Result<(), String> {
    let mut temp = TempSibling::create(path)?;
    temp.write_all(bytes)?;
    temp.file
        .sync_all()
        .map_err(|err| format!("failed to flush {}: {err}", temp.path.display()))?;
    before_rename();
    rename_no_replace(&temp.path, path).map_err(|err| {
        if err.kind() == io::ErrorKind::AlreadyExists {
            format!("{} already exists", path.display())
        } else {
            format!(
                "failed to create {} from {}: {err}",
                path.display(),
                temp.path.display()
            )
        }
    })?;
    temp.persisted = true;
    let parent = path
        .parent()
        .ok_or_else(|| format!("missing parent directory for {}", path.display()))?;
    File::open(parent)
        .and_then(|dir| dir.sync_all())
        .map_err(|err| format!("failed to flush directory {}: {err}", parent.display()))
}

/// Check, without changing anything, that [`rewrite_via_temp_copy`] could
/// replace `path` now: the file opens for writing, its metadata can be read,
/// and its directory accepts the temporary copy.
///
/// # Errors
///
/// Returns the message the rewrite would fail with for those steps.
pub(crate) fn check_rewritable(path: &Path) -> Result<(), String> {
    let writable = Writable::open(path)?;
    TempSibling::create_private(&writable.target).map(drop)
}

/// An existing file opened the way a rewrite needs it.
struct Writable {
    /// `path` with links resolved.
    target: PathBuf,
    original: File,
    metadata: KeptMetadata,
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
        let metadata = KeptMetadata::read(&original, &target)?;
        Ok(Self {
            target,
            original,
            metadata,
        })
    }
}

/// The metadata of a replaced file that its replacement takes over.
struct KeptMetadata {
    /// Permission bits, including set-id and sticky bits.
    mode: u32,
    uid: u32,
    gid: u32,
    /// The copied extended attributes, as names and values.
    xattrs: Vec<(CString, Vec<u8>)>,
}

impl KeptMetadata {
    fn read(file: &File, path: &Path) -> Result<Self, String> {
        let metadata = file
            .metadata()
            .map_err(|err| format!("failed to read metadata for {}: {err}", path.display()))?;
        let names = xattr::list(file).map_err(|err| {
            format!(
                "failed to list extended attributes of {}: {err}",
                path.display()
            )
        })?;
        let mut xattrs = Vec::new();
        for name in names.into_iter().filter(|name| is_copied(name)) {
            let value = xattr::get(file, &name).map_err(|err| {
                format!(
                    "failed to read extended attribute {} of {}: {err}",
                    name.to_string_lossy(),
                    path.display()
                )
            })?;
            // An attribute removed since it was listed has nothing to keep.
            if let Some(value) = value {
                xattrs.push((name, value));
            }
        }
        Ok(Self {
            mode: metadata.mode() & 0o7777,
            uid: metadata.uid(),
            gid: metadata.gid(),
            xattrs,
        })
    }

    fn access_acl(&self) -> Option<&[u8]> {
        self.xattrs
            .iter()
            .find(|(name, _)| name.as_c_str() == ACCESS_ACL)
            .map(|(_, value)| value.as_slice())
    }

    /// Give `copy` this metadata before it replaces `target`.
    fn apply(&self, copy: &File, target: &Path) -> Result<(), String> {
        keep_owner(copy, self.uid, self.gid).map_err(|err| {
            format!(
                "failed to keep the owner and group of {}: {err}",
                target.display()
            )
        })?;
        // Writing attributes needs write access, which the owner-only mode
        // the copy has until now grants.
        for (name, value) in &self.xattrs {
            if name.as_c_str() != ACCESS_ACL {
                xattr::set(copy, name, value).map_err(|err| xattr_error(name, target, &err))?;
            }
        }
        copy.set_permissions(Permissions::from_mode(self.mode))
            .map_err(|err| {
                format!(
                    "failed to keep the permissions of {}: {err}",
                    target.display()
                )
            })?;
        // The access ACL sets the group permission bits from its mask, so it
        // comes after the mode. Without one the copy may still have an ACL
        // that a default ACL of the directory gave it.
        match self.access_acl() {
            Some(acl) => xattr::set(copy, ACCESS_ACL, acl),
            None => xattr::remove(copy, ACCESS_ACL),
        }
        .map_err(|err| xattr_error(ACCESS_ACL, target, &err))?;
        self.check_permissions(copy, target)
    }

    /// Confirm that `copy` grants exactly what the original did.
    fn check_permissions(&self, copy: &File, target: &Path) -> Result<(), String> {
        let mode = copy
            .metadata()
            .map_err(|err| format!("failed to read metadata for {}: {err}", target.display()))?
            .mode()
            & 0o7777;
        if mode != self.mode {
            return Err(format!(
                "failed to keep the permissions of {}: the copy got mode {mode:04o} instead of {:04o}",
                target.display(),
                self.mode
            ));
        }
        let acl = xattr::get(copy, ACCESS_ACL).map_err(|err| {
            format!(
                "failed to read the access control list of {}: {err}",
                target.display()
            )
        })?;
        if acl.as_deref() != self.access_acl() {
            return Err(format!(
                "failed to keep the access control list of {}",
                target.display()
            ));
        }
        Ok(())
    }
}

/// Whether a replacement copies the extended attribute `name`; see the
/// module docs for those it leaves out.
fn is_copied(name: &CStr) -> bool {
    name.to_bytes().starts_with(b"user.") || name == ACCESS_ACL
}

fn xattr_error(name: &CStr, target: &Path, err: &io::Error) -> String {
    format!(
        "failed to keep extended attribute {} of {}: {err}",
        name.to_string_lossy(),
        target.display()
    )
}

/// Give `copy` the owner `uid` and group `gid` as far as the process may.
///
/// Only a privileged process may give a file to another user, so the owner
/// can stay the process's user. The group is then still kept when the user
/// belongs to it. Either refusal leaves the copy usable and is not an error.
fn keep_owner(copy: &File, uid: u32, gid: u32) -> io::Result<()> {
    let current = copy.metadata()?;
    if current.uid() == uid && current.gid() == gid {
        return Ok(());
    }
    match fchown(copy, Some(uid), Some(gid)) {
        Err(err) if is_not_permitted(&err) => {}
        result => return result,
    }
    if current.gid() == gid {
        return Ok(());
    }
    match fchown(copy, None, Some(gid)) {
        Err(err) if is_not_permitted(&err) => Ok(()),
        result => result,
    }
}

/// Whether `err` refuses an ownership change the process may not make. In a
/// user namespace, such as a Flatpak sandbox, IDs from outside it are
/// invalid there (`EINVAL`) rather than forbidden.
fn is_not_permitted(err: &io::Error) -> bool {
    matches!(err.raw_os_error(), Some(libc::EPERM | libc::EINVAL))
}

/// A new file next to the target that is removed unless persisted.
struct TempSibling {
    path: PathBuf,
    file: File,
    persisted: bool,
}

impl TempSibling {
    /// Create a file with the permissions any new file gets.
    fn create(target: &Path) -> Result<Self, String> {
        // The mode new files get from `File::create`, before the umask.
        Self::create_with_mode(target, 0o666)
    }

    /// Create a file only its owner can open, for a copy of an existing file
    /// that may be private.
    fn create_private(target: &Path) -> Result<Self, String> {
        Self::create_with_mode(target, 0o600)
    }

    fn create_with_mode(target: &Path, mode: u32) -> Result<Self, String> {
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
            .mode(mode)
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

    /// Flush the file and rename it to `target`, with the metadata of the
    /// file it replaces if given.
    fn commit(mut self, target: &Path, replaced: Option<&KeptMetadata>) -> Result<(), String> {
        if let Some(replaced) = replaced {
            replaced.apply(&self.file, target)?;
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

    fn mode_of(path: &Path) -> u32 {
        fs::metadata(path).expect("metadata").mode() & 0o7777
    }

    /// Whether `err` means the test file system lacks the attribute
    /// support a test needs, so the test cannot run there.
    fn unsupported(err: &io::Error) -> bool {
        matches!(err.raw_os_error(), Some(libc::ENOTSUP | libc::ENOSYS))
    }

    fn skip(test: &str, dir: PathBuf, reason: &str) {
        eprintln!(
            "skipping {test}: {} {reason}; set TMPDIR to a file system that supports it",
            dir.display()
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// An ACL in the kernel's `system.posix_acl_*` encoding that gives the
    /// owner read and write, the group read, the user `uid` read, and
    /// others nothing. The mask allows read and write.
    fn acl_value(uid: u32) -> Vec<u8> {
        const VERSION: u32 = 2;
        const UNDEFINED_ID: u32 = u32::MAX;
        let entries: [(u16, u16, u32); 5] = [
            (0x01, 6, UNDEFINED_ID), // owner
            (0x02, 4, uid),          // named user
            (0x04, 4, UNDEFINED_ID), // owning group
            (0x10, 6, UNDEFINED_ID), // mask
            (0x20, 0, UNDEFINED_ID), // others
        ];
        let mut value = VERSION.to_le_bytes().to_vec();
        for (tag, perm, id) in entries {
            value.extend_from_slice(&tag.to_le_bytes());
            value.extend_from_slice(&perm.to_le_bytes());
            value.extend_from_slice(&id.to_le_bytes());
        }
        value
    }

    /// Groups of this process other than its primary group.
    fn supplementary_groups() -> Vec<u32> {
        let status = fs::read_to_string("/proc/self/status").unwrap_or_default();
        let primary = status
            .lines()
            .find_map(|line| line.strip_prefix("Gid:"))
            .and_then(|ids| ids.split_whitespace().nth(1))
            .and_then(|id| id.parse::<u32>().ok());
        status
            .lines()
            .find_map(|line| line.strip_prefix("Groups:"))
            .map(|ids| {
                ids.split_whitespace()
                    .filter_map(|id| id.parse().ok())
                    .filter(|&id| Some(id) != primary)
                    .collect()
            })
            .unwrap_or_default()
    }

    #[test]
    fn copy_of_a_private_file_is_never_readable_by_others() {
        let dir = test_dir("private");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fs::set_permissions(&path, Permissions::from_mode(0o600)).expect("chmod");

        rewrite_via_temp_copy(&path, |file| {
            let mode = file.metadata().map_err(|err| err.to_string())?.mode();
            assert_eq!(mode & 0o077, 0, "copy mode {mode:o}");
            replace_contents(file, b"edited")
        })
        .expect("rewrite");

        assert_eq!(mode_of(&path), 0o600);
        assert_eq!(fs::read(&path).expect("read result"), b"edited");
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn rewrite_keeps_user_attributes_and_the_mode() {
        let dir = test_dir("xattr");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fs::set_permissions(&path, Permissions::from_mode(0o640)).expect("chmod");
        let file = File::open(&path).expect("open original");
        match xattr::set(&file, c"user.review", b"keep") {
            Ok(()) => {}
            Err(err) if unsupported(&err) => {
                return skip(
                    "rewrite_keeps_user_attributes_and_the_mode",
                    dir,
                    "does not support user extended attributes",
                );
            }
            Err(err) => panic!("set user.review: {err}"),
        }
        xattr::set(&file, c"user.empty", b"").expect("set user.empty");
        drop(file);

        rewrite_via_temp_copy(&path, |file| replace_contents(file, b"edited")).expect("rewrite");
        replace_file(&path, b"replaced").expect("replace");

        let file = File::open(&path).expect("open result");
        assert_eq!(
            xattr::get(&file, c"user.review").expect("get user.review"),
            Some(b"keep".to_vec())
        );
        assert_eq!(
            xattr::get(&file, c"user.empty").expect("get user.empty"),
            Some(Vec::new())
        );
        assert_eq!(mode_of(&path), 0o640);
        assert_eq!(fs::read(&path).expect("read result"), b"replaced");
        assert_eq!(dir_entries(&dir), vec!["song.flac".to_string()]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn rewrite_keeps_the_access_acl_and_the_mode_it_sets() {
        let dir = test_dir("acl");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fs::set_permissions(&path, Permissions::from_mode(0o600)).expect("chmod");
        let file = File::open(&path).expect("open original");
        let acl = acl_value(file.metadata().expect("metadata").uid());
        match xattr::set(&file, ACCESS_ACL, &acl) {
            Ok(()) => {}
            Err(err) if unsupported(&err) => {
                return skip(
                    "rewrite_keeps_the_access_acl_and_the_mode_it_sets",
                    dir,
                    "does not support POSIX ACLs",
                );
            }
            Err(err) => panic!("set access ACL: {err}"),
        }
        let original_acl = xattr::get(&file, ACCESS_ACL).expect("get original ACL");
        drop(file);
        // The mask entry sets the group bits.
        assert_eq!(mode_of(&path), 0o660);

        rewrite_via_temp_copy(&path, |file| replace_contents(file, b"edited")).expect("rewrite");

        let file = File::open(&path).expect("open result");
        assert_eq!(
            xattr::get(&file, ACCESS_ACL).expect("get ACL"),
            original_acl
        );
        assert_eq!(mode_of(&path), 0o660);
        assert_eq!(fs::read(&path).expect("read result"), b"edited");
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn rewrite_drops_an_acl_the_directory_default_would_add() {
        let dir = test_dir("default-acl");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fs::set_permissions(&path, Permissions::from_mode(0o644)).expect("chmod");
        let directory = File::open(&dir).expect("open test dir");
        let uid = directory.metadata().expect("metadata").uid();
        match xattr::set(&directory, c"system.posix_acl_default", &acl_value(uid)) {
            Ok(()) => {}
            Err(err) if unsupported(&err) => {
                return skip(
                    "rewrite_drops_an_acl_the_directory_default_would_add",
                    dir,
                    "does not support POSIX ACLs",
                );
            }
            Err(err) => panic!("set default ACL: {err}"),
        }
        drop(directory);

        rewrite_via_temp_copy(&path, |file| {
            // The copy inherits the default ACL, limited to the owner.
            let mode = file.metadata().map_err(|err| err.to_string())?.mode();
            assert_eq!(mode & 0o077, 0, "copy mode {mode:o}");
            replace_contents(file, b"edited")
        })
        .expect("rewrite");

        let file = File::open(&path).expect("open result");
        assert_eq!(xattr::get(&file, ACCESS_ACL).expect("get ACL"), None);
        assert_eq!(mode_of(&path), 0o644);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn rewrite_keeps_a_group_the_user_belongs_to() {
        let Some(&group) = supplementary_groups().first() else {
            eprintln!(
                "skipping rewrite_keeps_a_group_the_user_belongs_to: \
                 the process belongs to no supplementary group"
            );
            return;
        };
        let dir = test_dir("group");
        let path = dir.join("song.flac");
        fs::write(&path, b"original").expect("write original");
        fchown(
            &File::open(&path).expect("open original"),
            None,
            Some(group),
        )
        .expect("chgrp");
        fs::set_permissions(&path, Permissions::from_mode(0o664)).expect("chmod");

        rewrite_via_temp_copy(&path, |file| replace_contents(file, b"edited")).expect("rewrite");

        let metadata = fs::metadata(&path).expect("metadata");
        assert_eq!(metadata.gid(), group);
        assert_eq!(metadata.mode() & 0o7777, 0o664);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn foreign_owner_still_keeps_the_group() {
        let Some(&group) = supplementary_groups().first() else {
            eprintln!(
                "skipping foreign_owner_still_keeps_the_group: \
                 the process belongs to no supplementary group"
            );
            return;
        };
        let dir = test_dir("foreign-owner");
        let copy = File::create(dir.join("copy")).expect("create copy");
        let own = copy.metadata().expect("metadata").uid();
        if own == 0 {
            // Root may give the file away, so there is no refusal to test.
            return skip(
                "foreign_owner_still_keeps_the_group",
                dir,
                "is owned by root",
            );
        }

        keep_owner(&copy, own + 1, group).expect("keep owner");

        let metadata = copy.metadata().expect("metadata");
        assert_eq!(metadata.uid(), own);
        assert_eq!(metadata.gid(), group);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn rewriting_a_hard_link_splits_it_from_the_other_links() {
        let dir = test_dir("hard-link");
        let path = dir.join("song.flac");
        let seeded = dir.join("seeded.flac");
        fs::write(&path, b"original").expect("write original");
        fs::hard_link(&path, &seeded).expect("hard link");

        rewrite_via_temp_copy(&path, |file| replace_contents(file, b"edited")).expect("rewrite");

        assert_eq!(fs::read(&path).expect("read edited"), b"edited");
        assert_eq!(fs::read(&seeded).expect("read seeded"), b"original");
        assert_ne!(
            fs::metadata(&path).expect("metadata").ino(),
            fs::metadata(&seeded).expect("metadata").ino()
        );
        assert_eq!(
            dir_entries(&dir),
            vec!["seeded.flac".to_string(), "song.flac".to_string()]
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn created_file_gets_the_default_mode() {
        let dir = test_dir("create-mode");
        let path = dir.join("cover.jpg");
        let reference = dir.join("reference");
        File::create(&reference).expect("create reference");

        create_file(&path, b"cover").expect("create");

        assert_eq!(mode_of(&path), mode_of(&reference));
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

    #[test]
    fn creating_a_file_keeps_what_appears_at_its_name_while_it_is_written() {
        let dir = test_dir("create-race");
        let path = dir.join("cover.png");

        let error = create_file_with(&path, b"ferrous", || {
            fs::write(&path, b"other program").expect("write competing file");
        })
        .expect_err("name was taken");

        assert_eq!(error, format!("{} already exists", path.display()));
        assert_eq!(fs::read(&path).expect("read result"), b"other program");
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
