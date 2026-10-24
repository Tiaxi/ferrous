// SPDX-License-Identifier: GPL-3.0-or-later

//! Renames that never replace an existing file.
//!
//! `std::fs::rename` replaces whatever has the target name, so checking for
//! the target first and renaming after can delete a file that another
//! process creates in between. The renames here fail atomically instead when
//! the target name is taken, also by a dangling symbolic link.
//!
//! Linux does this with `renameat2(RENAME_NOREPLACE)`. NFS, and FUSE
//! filesystems without rename2 support, reject the flag with `EINVAL`. On
//! those a regular file is hard-linked to the target, which fails just as
//! atomically when the name is taken, and its old name is then removed. A
//! filesystem that supports neither gets an error rather than a rename that
//! could replace a file.

use std::fs;
use std::io;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::Path;

/// Renames `from` to `to`, failing with [`io::ErrorKind::AlreadyExists`]
/// when `to` exists.
///
/// On a case-insensitive filesystem a different spelling of `from` exists,
/// so a case-only rename fails too; rename through a temporary name instead.
pub(super) fn rename_no_replace(from: &Path, to: &Path) -> io::Result<()> {
    rename_no_replace_with(from, to, renameat2_no_replace)
}

/// [`rename_no_replace`] with the `renameat2` call replaced, so tests can
/// take the path of a filesystem that does not support the flag.
fn rename_no_replace_with(
    from: &Path,
    to: &Path,
    rename: fn(&Path, &Path) -> io::Result<()>,
) -> io::Result<()> {
    match rename(from, to) {
        Err(error) if no_replace_unsupported(&error) => link_then_unlink(from, to),
        result => result,
    }
}

/// Whether `error` means the filesystem or kernel cannot rename without
/// replacing, rather than that this rename failed.
fn no_replace_unsupported(error: &io::Error) -> bool {
    error.raw_os_error() == Some(libc::EINVAL) || error.kind() == io::ErrorKind::Unsupported
}

#[cfg(all(target_os = "linux", target_env = "gnu"))]
fn renameat2_no_replace(from: &Path, to: &Path) -> io::Result<()> {
    use std::ffi::CString;
    use std::os::unix::ffi::OsStrExt;

    let from = CString::new(from.as_os_str().as_bytes())?;
    let to = CString::new(to.as_os_str().as_bytes())?;
    // SAFETY: `from` and `to` are NUL-terminated strings that outlive the
    // call, and `AT_FDCWD` resolves relative paths against the working
    // directory, as `std::fs::rename` does. The call does not retain them.
    let result = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            from.as_ptr(),
            libc::AT_FDCWD,
            to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

#[cfg(not(all(target_os = "linux", target_env = "gnu")))]
fn renameat2_no_replace(_from: &Path, _to: &Path) -> io::Result<()> {
    Err(io::ErrorKind::Unsupported.into())
}

/// Moves the regular file `from` to `to` by hard-linking it to `to`, which
/// fails if `to` exists, and then removing `from`.
fn link_then_unlink(from: &Path, to: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(from)?;
    if !metadata.is_file() {
        return Err(unsupported(from, to, "it is not a regular file"));
    }
    if let Err(error) = fs::hard_link(from, to) {
        if error.kind() == io::ErrorKind::AlreadyExists {
            return Err(error);
        }
        return Err(unsupported(
            from,
            to,
            &format!("hard links failed too: {error}"),
        ));
    }
    let Err(error) = fs::remove_file(from) else {
        return Ok(());
    };
    // Take back the link so the file keeps only its old name, unless
    // something else has taken the new name since.
    let undone = fs::symlink_metadata(to).is_ok_and(|linked| same_inode(&linked, &metadata))
        && fs::remove_file(to).is_ok();
    let outcome = if undone {
        format!("it keeps the name {}", from.display())
    } else {
        format!(
            "it now has both names {} and {}",
            from.display(),
            to.display()
        )
    };
    Err(io::Error::new(
        error.kind(),
        format!(
            "linked {} to {} but could not remove the old name: {error}; {outcome}",
            from.display(),
            to.display()
        ),
    ))
}

#[cfg(unix)]
fn same_inode(left: &fs::Metadata, right: &fs::Metadata) -> bool {
    left.dev() == right.dev() && left.ino() == right.ino()
}

#[cfg(not(unix))]
fn same_inode(_left: &fs::Metadata, _right: &fs::Metadata) -> bool {
    false
}

fn unsupported(from: &Path, to: &Path, reason: &str) -> io::Error {
    io::Error::new(
        io::ErrorKind::Unsupported,
        format!(
            "cannot rename {} to {} without risking replacing a file: the filesystem \
             does not support renaming without replacing, and {reason}",
            from.display(),
            to.display()
        ),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-no-replace-{name}-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        dir
    }

    fn flag_rejected(_from: &Path, _to: &Path) -> io::Result<()> {
        Err(io::Error::from_raw_os_error(libc::EINVAL))
    }

    #[test]
    fn renames_to_a_free_name() {
        let dir = test_dir("free");
        let (from, to) = (dir.join("a"), dir.join("b"));
        fs::write(&from, b"a").expect("write");

        rename_no_replace(&from, &to).expect("rename");

        assert!(!from.exists());
        assert_eq!(fs::read(&to).expect("read"), b"a");
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn refuses_to_replace_a_file_or_a_dangling_symlink() {
        let dir = test_dir("taken");
        let from = dir.join("a");
        let (file, link) = (dir.join("file"), dir.join("link"));
        fs::write(&from, b"a").expect("write");
        fs::write(&file, b"file").expect("write");
        std::os::unix::fs::symlink(dir.join("missing"), &link).expect("symlink");

        for (target, kind) in [(&file, "file"), (&link, "symlink")] {
            let error = rename_no_replace(&from, target).expect_err(kind);
            assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{kind}");
        }

        assert_eq!(fs::read(&from).expect("read"), b"a");
        assert_eq!(fs::read(&file).expect("read"), b"file");
        assert!(fs::symlink_metadata(&link).expect("link").is_symlink());
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn falls_back_to_a_hard_link_when_the_flag_is_unsupported() {
        let dir = test_dir("fallback");
        let (from, to, taken) = (dir.join("a"), dir.join("b"), dir.join("c"));
        fs::write(&from, b"a").expect("write");
        fs::write(&taken, b"c").expect("write");

        let error = rename_no_replace_with(&from, &taken, flag_rejected).expect_err("taken");
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        assert_eq!(fs::read(&taken).expect("read"), b"c");

        rename_no_replace_with(&from, &to, flag_rejected).expect("rename");
        assert!(!from.exists());
        assert_eq!(fs::read(&to).expect("read"), b"a");
        assert_eq!(fs::metadata(&to).expect("metadata").nlink(), 1);
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn refuses_when_neither_the_flag_nor_a_hard_link_is_available() {
        let dir = test_dir("unsupported");
        // Only regular files can move by hard link.
        let (from, to) = (dir.join("a"), dir.join("b"));
        fs::create_dir(&from).expect("dir");

        let error = rename_no_replace_with(&from, &to, flag_rejected).expect_err("unsupported");

        assert_eq!(error.kind(), io::ErrorKind::Unsupported);
        assert!(
            error
                .to_string()
                .contains("does not support renaming without replacing"),
            "{error}"
        );
        assert!(from.is_dir());
        assert!(fs::symlink_metadata(&to).is_err());
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn other_rename_errors_do_not_fall_back() {
        let dir = test_dir("other-error");
        let error = rename_no_replace(&dir.join("missing"), &dir.join("b")).expect_err("missing");
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert!(no_replace_unsupported(&io::Error::from_raw_os_error(
            libc::EINVAL
        )));
        assert!(no_replace_unsupported(&io::Error::from_raw_os_error(
            libc::ENOSYS
        )));
        assert!(!no_replace_unsupported(&io::Error::from_raw_os_error(
            libc::EEXIST
        )));
        fs::remove_dir_all(dir).expect("cleanup");
    }
}
