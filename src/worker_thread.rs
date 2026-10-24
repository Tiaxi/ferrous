// SPDX-License-Identifier: GPL-3.0-or-later

//! Starting the app's long-lived worker threads.

/// Start a named worker thread, and log it if the thread cannot start.
///
/// A worker talks to the rest of the app only through the channels its
/// closure owns. If the thread cannot start, the closure is dropped with
/// those channels, so the worker's clients find them closed, as if the
/// worker had stopped, and report it to the user there.
pub(crate) fn spawn_worker(name: &str, work: impl FnOnce() + Send + 'static) {
    if let Err(error) = std::thread::Builder::new()
        .name(name.to_string())
        .spawn(work)
    {
        eprintln!("[ferrous] failed to start the {name} thread: {error}");
    }
}
