// SPDX-License-Identifier: GPL-3.0-or-later

//! The few inotify calls the watcher needs, over `libc`, and a waker that
//! interrupts its wait.

use std::ffi::{CString, OsString};
use std::fs::File;
use std::io::{self, Read as _, Write as _};
use std::os::fd::{AsRawFd as _, FromRawFd as _, OwnedFd};
use std::os::unix::ffi::{OsStrExt as _, OsStringExt as _};
use std::path::Path;
use std::time::Duration;

/// One inotify event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct RawEvent {
    pub(super) wd: i32,
    pub(super) mask: u32,
    /// The name of the entry in the watched folder, for events about one.
    pub(super) name: Option<OsString>,
}

/// An inotify instance, read without blocking.
pub(super) struct Inotify {
    file: File,
}

impl Inotify {
    pub(super) fn new() -> io::Result<Self> {
        // SAFETY: inotify_init1 takes no pointers, and these flags are valid.
        let fd = unsafe { libc::inotify_init1(libc::IN_NONBLOCK | libc::IN_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: `fd` is a new descriptor that nothing else owns or closes.
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        Ok(Self {
            file: File::from(fd),
        })
    }

    /// Watches `path` for `mask`, returning its watch descriptor. A path
    /// already watched, through any name, has the same descriptor.
    pub(super) fn add_watch(&self, path: &Path, mask: u32) -> io::Result<i32> {
        let path = CString::new(path.as_os_str().as_bytes())?;
        // SAFETY: the descriptor stays open while `self` lives, and `path` is
        // a NUL-terminated string that outlives the call.
        let wd = unsafe { libc::inotify_add_watch(self.file.as_raw_fd(), path.as_ptr(), mask) };
        if wd < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(wd)
        }
    }

    /// Stops watching `wd`. A descriptor the kernel already removed, with
    /// its folder, is ignored.
    pub(super) fn remove_watch(&self, wd: i32) {
        // SAFETY: inotify_rm_watch takes no pointers; an unknown descriptor
        // only makes it fail with EINVAL.
        unsafe {
            libc::inotify_rm_watch(self.file.as_raw_fd(), wd);
        }
    }

    /// Waits until there are events, `waker` is woken, or `timeout` passes.
    pub(super) fn wait(&self, waker: &Waker, timeout: Option<Duration>) -> io::Result<()> {
        let mut fds = [
            libc::pollfd {
                fd: self.file.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
            libc::pollfd {
                fd: waker.file.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            },
        ];
        let timeout_ms = timeout.map_or(-1, |timeout| {
            // Rounded up, so that a wait for a deadline never ends before it.
            let ms = timeout.as_nanos().div_ceil(1_000_000);
            i32::try_from(ms).unwrap_or(i32::MAX)
        });
        // SAFETY: `fds` is an array of two initialized pollfd structures that
        // lives until the call returns, and its length is passed with it.
        let ready = unsafe { libc::poll(fds.as_mut_ptr(), 2, timeout_ms) };
        if ready < 0 {
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
        waker.reset();
        Ok(())
    }

    /// Appends the events waiting to `out`.
    pub(super) fn read_events(&mut self, out: &mut Vec<RawEvent>) -> io::Result<()> {
        // Room for many events; one event needs at most 16 + NAME_MAX + 1.
        let mut buffer = vec![0_u8; 64 * 1024];
        loop {
            match self.file.read(&mut buffer) {
                Ok(0) => return Ok(()),
                Ok(read) => parse_events(&buffer[..read], out),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(()),
                Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
                Err(error) => return Err(error),
            }
        }
    }
}

/// Parses `struct inotify_event` records: a descriptor, a mask, a cookie
/// and a name length, in native byte order, then the NUL-padded name.
pub(super) fn parse_events(bytes: &[u8], out: &mut Vec<RawEvent>) {
    const HEADER: usize = 16;
    let field = |at: usize| {
        bytes
            .get(at..at + 4)
            .and_then(|field| <[u8; 4]>::try_from(field).ok())
    };
    let mut at = 0;
    while at + HEADER <= bytes.len() {
        let (Some(wd), Some(mask), Some(len)) = (field(at), field(at + 4), field(at + 12)) else {
            return;
        };
        let len = usize::try_from(u32::from_ne_bytes(len)).unwrap_or(usize::MAX);
        let Some(name) = bytes.get(at + HEADER..(at + HEADER).saturating_add(len)) else {
            return;
        };
        let name = &name[..name
            .iter()
            .position(|&byte| byte == 0)
            .unwrap_or(name.len())];
        out.push(RawEvent {
            wd: i32::from_ne_bytes(wd),
            mask: u32::from_ne_bytes(mask),
            name: (!name.is_empty()).then(|| OsString::from_vec(name.to_vec())),
        });
        at += HEADER + len;
    }
}

/// Wakes [`Inotify::wait`] from another thread.
pub(crate) struct Waker {
    file: File,
}

impl Waker {
    pub(super) fn new() -> io::Result<Self> {
        // SAFETY: eventfd takes no pointers, and these flags are valid.
        let fd = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: `fd` is a new descriptor that nothing else owns or closes.
        let fd = unsafe { OwnedFd::from_raw_fd(fd) };
        Ok(Self {
            file: File::from(fd),
        })
    }

    /// Wakes the waiting thread, or the next wait. Never blocks.
    pub(crate) fn wake(&self) {
        // A full counter already wakes the waiter, so a failed write loses
        // nothing.
        let _ = (&self.file).write(&1_u64.to_ne_bytes());
    }

    fn reset(&self) {
        let mut count = [0_u8; 8];
        let _ = (&self.file).read(&mut count);
    }
}
