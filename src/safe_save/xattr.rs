// SPDX-License-Identifier: GPL-3.0-or-later

//! Extended attribute calls on open files.
//!
//! Every call takes a file descriptor, so the attributes read and written
//! belong to the file that was opened even when its path is replaced.

use std::ffi::{CStr, CString};
use std::fs::File;
use std::io;

/// Names of the extended attributes of `file`, or none when its file system
/// does not support them.
pub(super) fn list(file: &File) -> io::Result<Vec<CString>> {
    let Some(names) = read_sized(|buffer| platform::list(file, buffer))? else {
        return Ok(Vec::new());
    };
    Ok(names
        .split(|&byte| byte == 0)
        .filter(|name| !name.is_empty())
        .filter_map(|name| CString::new(name).ok())
        .collect())
}

/// The value of the attribute `name` of `file`, or `None` when it has none.
pub(super) fn get(file: &File, name: &CStr) -> io::Result<Option<Vec<u8>>> {
    read_sized(|buffer| platform::get(file, name, buffer))
}

/// Set the attribute `name` of `file` to `value`, creating or replacing it.
pub(super) fn set(file: &File, name: &CStr, value: &[u8]) -> io::Result<()> {
    platform::set(file, name, value)
}

/// Remove the attribute `name` of `file`. Removing a missing one succeeds.
pub(super) fn remove(file: &File, name: &CStr) -> io::Result<()> {
    match platform::remove(file, name) {
        Err(err) if is_missing(&err) => Ok(()),
        result => result,
    }
}

/// Run a call that fills a buffer and returns how much it used: first to
/// learn the size, then to read, again when the value grew in between.
/// Returns `None` when the attribute or attribute support is missing.
fn read_sized(mut call: impl FnMut(&mut [u8]) -> io::Result<usize>) -> io::Result<Option<Vec<u8>>> {
    loop {
        let size = match call(&mut []) {
            Ok(size) => size,
            Err(err) if is_missing(&err) => return Ok(None),
            Err(err) => return Err(err),
        };
        let mut buffer = vec![0; size];
        match call(&mut buffer) {
            Ok(used) => {
                buffer.truncate(used);
                return Ok(Some(buffer));
            }
            Err(err) if err.raw_os_error() == Some(libc::ERANGE) => {}
            Err(err) if is_missing(&err) => return Ok(None),
            Err(err) => return Err(err),
        }
    }
}

fn is_missing(err: &io::Error) -> bool {
    matches!(
        err.raw_os_error(),
        Some(libc::ENODATA | libc::ENOTSUP | libc::ENOSYS)
    ) || err.kind() == io::ErrorKind::Unsupported
}

#[cfg(target_os = "linux")]
mod platform {
    use std::ffi::CStr;
    use std::fs::File;
    use std::io;
    use std::os::fd::AsRawFd;

    fn size_result(result: libc::ssize_t) -> io::Result<usize> {
        usize::try_from(result).map_err(|_| io::Error::last_os_error())
    }

    fn unit_result(result: libc::c_int) -> io::Result<()> {
        if result == 0 {
            Ok(())
        } else {
            Err(io::Error::last_os_error())
        }
    }

    pub(super) fn list(file: &File, buffer: &mut [u8]) -> io::Result<usize> {
        // SAFETY: the descriptor is open for the lifetime of `file`, and the
        // kernel writes at most `buffer.len()` bytes to `buffer`, which a
        // zero length makes it leave untouched.
        size_result(unsafe {
            libc::flistxattr(file.as_raw_fd(), buffer.as_mut_ptr().cast(), buffer.len())
        })
    }

    pub(super) fn get(file: &File, name: &CStr, buffer: &mut [u8]) -> io::Result<usize> {
        // SAFETY: as in `list`; `name` is NUL-terminated and outlives the call.
        size_result(unsafe {
            libc::fgetxattr(
                file.as_raw_fd(),
                name.as_ptr(),
                buffer.as_mut_ptr().cast(),
                buffer.len(),
            )
        })
    }

    pub(super) fn set(file: &File, name: &CStr, value: &[u8]) -> io::Result<()> {
        // SAFETY: the descriptor is open, `name` is NUL-terminated, and the
        // kernel reads exactly `value.len()` bytes from `value`. None of them
        // is retained after the call.
        unit_result(unsafe {
            libc::fsetxattr(
                file.as_raw_fd(),
                name.as_ptr(),
                value.as_ptr().cast(),
                value.len(),
                0,
            )
        })
    }

    pub(super) fn remove(file: &File, name: &CStr) -> io::Result<()> {
        // SAFETY: the descriptor is open and `name` is NUL-terminated; the
        // call does not retain either.
        unit_result(unsafe { libc::fremovexattr(file.as_raw_fd(), name.as_ptr()) })
    }
}

/// Ferrous targets Linux; elsewhere the calls report missing support, which
/// reads as a file system without extended attributes.
#[cfg(not(target_os = "linux"))]
mod platform {
    use std::ffi::CStr;
    use std::fs::File;
    use std::io;

    pub(super) fn list(_file: &File, _buffer: &mut [u8]) -> io::Result<usize> {
        Err(io::ErrorKind::Unsupported.into())
    }

    pub(super) fn get(_file: &File, _name: &CStr, _buffer: &mut [u8]) -> io::Result<usize> {
        Err(io::ErrorKind::Unsupported.into())
    }

    pub(super) fn set(_file: &File, _name: &CStr, _value: &[u8]) -> io::Result<()> {
        Err(io::ErrorKind::Unsupported.into())
    }

    pub(super) fn remove(_file: &File, _name: &CStr) -> io::Result<()> {
        Err(io::ErrorKind::Unsupported.into())
    }
}
