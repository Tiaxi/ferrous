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
    let handle = std::thread::Builder::new()
        .name(name.to_string())
        .spawn(move || {
            // Declared first, so it is dropped last, after `work` and all it
            // owns, even when `work` panics.
            let _finished = finished_tx;
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
}
