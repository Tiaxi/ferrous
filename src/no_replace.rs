// SPDX-License-Identifier: GPL-3.0-or-later

//! Renames that never replace an existing file.
//!
//! `std::fs::rename` replaces whatever has the target name, so checking for
//! the target first and renaming after can delete a file that another
//! process creates in between. The renames here fail atomically instead when
//! the target name is taken, also by a dangling symbolic link.
//!
//! Linux does this with `renameat2(RENAME_NOREPLACE)`. NFS, and FUSE or
//! network mounts without rename2 support (rclone, gvfs SMB), reject the flag
//! with `EINVAL` or `ENOSYS`. On those the target is checked and then renamed
//! with a plain rename, which is refused when anything has the target name.
//! A file that another process creates at the target between the check and
//! the rename can still be replaced there, as before Ferrous used the flag.
//! The source is never deleted.

use std::fs;
use std::io;
use std::path::Path;

/// Renames `from` to `to`, failing with [`io::ErrorKind::AlreadyExists`]
/// when `to` exists.
///
/// On a case-insensitive filesystem a different spelling of `from` exists,
/// so a case-only rename fails too; rename through a temporary name instead.
pub(crate) fn rename_no_replace(from: &Path, to: &Path) -> io::Result<()> {
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
        Err(error) if no_replace_unsupported(&error) => check_then_rename(from, to),
        result => result,
    }
}

/// Whether `error` means the filesystem or kernel cannot rename without
/// replacing, rather than that this rename failed.
fn no_replace_unsupported(error: &io::Error) -> bool {
    matches!(error.raw_os_error(), Some(libc::EINVAL | libc::ENOSYS))
        || error.kind() == io::ErrorKind::Unsupported
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

/// Renames `from` to `to` unless something already has the name `to`. Used
/// where the filesystem cannot refuse atomically; see the module docs for the
/// window between the check and the rename.
fn check_then_rename(from: &Path, to: &Path) -> io::Result<()> {
    match fs::symlink_metadata(to) {
        Ok(_) => Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            format!("{} already exists", to.display()),
        )),
        Err(error) if error.kind() == io::ErrorKind::NotFound => fs::rename(from, to),
        Err(error) => Err(error),
    }
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

    fn flag_missing(_from: &Path, _to: &Path) -> io::Result<()> {
        Err(io::Error::from_raw_os_error(libc::ENOSYS))
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
    fn falls_back_to_a_checked_rename_when_the_flag_is_unsupported() {
        for (rename, kind) in [
            (
                flag_rejected as fn(&Path, &Path) -> io::Result<()>,
                "EINVAL",
            ),
            (flag_missing, "ENOSYS"),
        ] {
            let dir = test_dir("fallback");
            let (from, to) = (dir.join("a"), dir.join("b"));
            fs::write(&from, b"a").expect("write");

            rename_no_replace_with(&from, &to, rename).expect(kind);

            assert!(fs::symlink_metadata(&from).is_err(), "{kind}");
            assert_eq!(fs::read(&to).expect("read"), b"a", "{kind}");
            fs::remove_dir_all(dir).expect("cleanup");
        }
    }

    #[test]
    fn fallback_refuses_a_file_or_a_dangling_symlink_at_the_target() {
        let dir = test_dir("fallback-taken");
        let from = dir.join("a");
        let (file, link) = (dir.join("file"), dir.join("link"));
        fs::write(&from, b"a").expect("write");
        fs::write(&file, b"file").expect("write");
        std::os::unix::fs::symlink(dir.join("missing"), &link).expect("symlink");

        for (target, kind) in [(&file, "file"), (&link, "symlink")] {
            let error = rename_no_replace_with(&from, target, flag_rejected).expect_err(kind);
            assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{kind}");
        }

        assert_eq!(fs::read(&from).expect("read"), b"a");
        assert_eq!(fs::read(&file).expect("read"), b"file");
        let link_target = fs::read_link(&link).expect("link");
        assert_eq!(link_target, dir.join("missing"));
        fs::remove_dir_all(dir).expect("cleanup");
    }

    #[test]
    fn fallback_keeps_the_source_when_the_rename_fails() {
        let dir = test_dir("fallback-error");
        let from = dir.join("a");
        fs::write(&from, b"a").expect("write");

        let error = rename_no_replace_with(&from, &dir.join("missing").join("b"), flag_rejected)
            .expect_err("missing directory");

        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert_eq!(fs::read(&from).expect("read"), b"a");
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
