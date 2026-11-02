// SPDX-License-Identifier: GPL-3.0-or-later

//! Starting the app's long-lived worker threads.

use std::thread::JoinHandle;
use std::time::Instant;

use crossbeam_channel::{bounded, Receiver, RecvTimeoutError};

/// A thread that can be waited for with a deadline. Dropping it detaches
/// the thread.
pub(crate) struct JoinableThread {
    handle: JoinHandle<()>,
    /// Closes when the thread's work has returned or unwound, with
    /// everything the work owned already dropped.
    finished: Receiver<()>,
}

impl JoinableThread {
    /// Wait until `deadline` for the thread to end, and join it if it does.
    /// Returns false, leaving the thread running detached, if it is still
    /// running at the deadline. A thread that panicked has ended too.
    pub(crate) fn join_by(self, deadline: Instant) -> bool {
        match self.finished.recv_deadline(deadline) {
            Err(RecvTimeoutError::Disconnected) => {
                // Only thread-local destructors are left, so this is quick.
                let _ = self.handle.join();
                true
            }
            Ok(()) | Err(RecvTimeoutError::Timeout) => false,
        }
    }

    /// Whether the thread is still running.
    #[cfg(test)]
    pub(crate) fn is_running(&self) -> bool {
        !self.handle.is_finished()
    }

    /// A receiver that disconnects when the thread has ended, as
    /// [`JoinableThread::join_by`] sees it, to wait for it without a bound.
    #[cfg(test)]
    pub(crate) fn finished(&self) -> Receiver<()> {
        self.finished.clone()
    }
}

/// Start a named thread that [`JoinableThread::join_by`] can wait for.
///
/// # Errors
///
/// Returns the system's error if the thread cannot start. `work` is dropped
/// then, with the channels it owns.
pub(crate) fn spawn_joinable(
    name: &str,
    work: impl FnOnce() + Send + 'static,
) -> std::io::Result<JoinableThread> {
    let (finished_tx, finished) = bounded::<()>(0);
    // The worker stores cover art where the bridge that starts it does.
    let covers = crate::metadata::CoverCacheBinding::current();
    let handle = std::thread::Builder::new()
        .name(name.to_string())
        .spawn(move || {
            // Declared first, so it is dropped last, after `work` and all it
            // owns, even when `work` panics.
            let _finished = finished_tx;
            let _covers = covers.enter();
            work();
        })?;
    Ok(JoinableThread { handle, finished })
}

/// Start a named worker thread, and log it if the thread cannot start.
///
/// A worker talks to the rest of the app only through the channels its
/// closure owns. If the thread cannot start, the closure is dropped with
/// those channels, so the worker's clients find them closed, as if the
/// worker had stopped, and report it to the user there.
pub(crate) fn spawn_worker(
    name: &str,
    work: impl FnOnce() + Send + 'static,
) -> Option<JoinableThread> {
    spawn_joinable(name, work)
        .inspect_err(|error| eprintln!("[ferrous] failed to start the {name} thread: {error}"))
        .ok()
}

/// How many nice levels below the rest of the app background analysis
/// runs. Under contention a thread ten levels lower gets about a tenth of
/// the CPU time of one at the app's level, which still decodes a
/// spectrogram many times faster than playback needs it.
#[cfg(target_os = "linux")]
const BACKGROUND_NICE_INCREMENT: libc::c_int = 10;

/// The highest nice value, the lowest priority.
#[cfg(target_os = "linux")]
const MAX_NICE: libc::c_int = 19;

/// Run the calling thread at a lower scheduling priority than the rest of
/// the app, for background analysis that must not take CPU time from the
/// UI, render and playback threads when the machine is busy.
///
/// Linux keeps a nice value per thread, so only the calling thread and the
/// threads it starts later run lower. The level is ten below the main
/// thread's, so a thread started by one already lowered, like a helper of
/// the spectrogram worker, runs at the same level rather than ten lower
/// still. Elsewhere this does nothing. A thread that cannot lower its
/// priority runs on at its own, as nothing depends on it.
pub(crate) fn lower_current_thread_priority() {
    #[cfg(target_os = "linux")]
    if let Err(error) = lower_linux_thread_priority() {
        eprintln!("[ferrous] could not lower a background thread's priority: {error}");
    }
}

/// The nice value of the thread `tid`, 0 for the calling thread. Linux
/// applies `PRIO_PROCESS` to the thread whose ID it is given, not to the
/// whole process; the process ID is the main thread's.
#[cfg(target_os = "linux")]
fn linux_thread_nice(tid: libc::id_t) -> std::io::Result<libc::c_int> {
    // SAFETY: errno is the calling thread's own; clearing it tells the
    // nice value -1 from a failed call.
    unsafe { *libc::__errno_location() = 0 };
    // SAFETY: plain system call, no pointers.
    let nice = unsafe { libc::getpriority(libc::PRIO_PROCESS, tid) };
    if nice == -1 {
        let error = std::io::Error::last_os_error();
        if error.raw_os_error() != Some(0) {
            return Err(error);
        }
    }
    Ok(nice)
}

#[cfg(target_os = "linux")]
fn lower_linux_thread_priority() -> std::io::Result<()> {
    let app = linux_thread_nice(std::process::id())?;
    let current = linux_thread_nice(0)?;
    let lower = app.saturating_add(BACKGROUND_NICE_INCREMENT).min(MAX_NICE);
    if lower <= current {
        return Ok(());
    }
    // SAFETY: plain system call on the calling thread, no pointers.
    if unsafe { libc::setpriority(libc::PRIO_PROCESS, 0, lower) } == -1 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn join_by_waits_for_a_thread_that_ends_in_time() {
        let (release_tx, release_rx) = bounded::<()>(0);
        let thread = spawn_joinable("test-joinable", move || {
            let _ = release_rx.recv();
        })
        .expect("spawn");
        assert!(thread.is_running());
        drop(release_tx);
        assert!(thread.join_by(Instant::now() + Duration::from_secs(10)));
    }

    #[test]
    fn join_by_gives_up_on_a_thread_that_outlives_the_deadline() {
        let (release_tx, release_rx) = bounded::<()>(0);
        let thread = spawn_joinable("test-joinable", move || {
            let _ = release_rx.recv();
        })
        .expect("spawn");
        assert!(!thread.join_by(Instant::now() + Duration::from_millis(20)));
        drop(release_tx);
    }

    #[test]
    fn a_thread_that_panics_has_ended() {
        let thread =
            spawn_joinable("test-joinable", || panic!("test panic in a worker")).expect("spawn");
        assert!(thread.join_by(Instant::now() + Duration::from_secs(10)));
    }

    #[test]
    fn the_work_is_dropped_before_the_thread_counts_as_ended() {
        let (owned_tx, owned_rx) = bounded::<()>(0);
        let thread = spawn_joinable("test-joinable", move || {
            let _owned = owned_tx;
        })
        .expect("spawn");
        assert!(thread.join_by(Instant::now() + Duration::from_secs(10)));
        assert_eq!(
            owned_rx.try_recv(),
            Err(crossbeam_channel::TryRecvError::Disconnected)
        );
    }

    /// The calling thread's nice value.
    #[cfg(target_os = "linux")]
    fn thread_nice() -> libc::c_int {
        // SAFETY: plain system call on the calling thread, no pointers.
        unsafe { libc::getpriority(libc::PRIO_PROCESS, 0) }
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn lowering_a_threads_priority_leaves_the_other_threads_alone() {
        let own = thread_nice();
        let (before, after) = std::thread::spawn(|| {
            let before = thread_nice();
            lower_current_thread_priority();
            (before, thread_nice())
        })
        .join()
        .expect("thread");
        assert_eq!(after, (before + BACKGROUND_NICE_INCREMENT).min(MAX_NICE));
        assert_eq!(thread_nice(), own);
        // Threads it starts later inherit it, and lowering one of them
        // again keeps it at the same level.
        let nested = std::thread::spawn(|| {
            lower_current_thread_priority();
            std::thread::spawn(|| {
                lower_current_thread_priority();
                thread_nice()
            })
            .join()
            .expect("inner thread")
        })
        .join()
        .expect("thread");
        assert_eq!(nested, (own + BACKGROUND_NICE_INCREMENT).min(MAX_NICE));
    }
}
