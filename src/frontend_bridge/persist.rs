// SPDX-License-Identifier: GPL-3.0-or-later

//! Saving the settings and the session on a thread of their own.
//!
//! A save serializes the whole session, with the queue, and writes it with
//! `fsync`, which slow or failing storage can hold up for long. The bridge
//! thread also drives playback, so it only hands the state over. One writer
//! saves in the order it was asked. A request replaces the state of each
//! file that waits to be written, so however long a save takes, at most one
//! state of each file waits besides the one being written.

use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};

#[cfg(test)]
use crossbeam_channel::Sender;
use crossbeam_channel::{unbounded, Receiver};

use super::config::{save_session_snapshot, save_settings, SaveFailure, SessionSnapshot};
use super::BridgeSettings;

/// What to save. `id` is that of the newest request it carries; each request
/// has a larger `id` than the ones before it.
#[derive(Default)]
struct PersistRequest {
    id: u64,
    settings: Option<BridgeSettings>,
    session: Option<SessionSnapshot>,
}

/// How the requests up to `id` went: the result of the newest save of each
/// file that one of them asked for.
#[derive(Debug)]
pub(super) struct PersistOutcome {
    pub(super) id: u64,
    pub(super) settings: Option<Result<(), SaveFailure>>,
    pub(super) session: Option<Result<(), SaveFailure>>,
}

impl PersistOutcome {
    /// Whether every save it reports succeeded.
    pub(super) fn saved(&self) -> bool {
        [&self.settings, &self.session]
            .into_iter()
            .flatten()
            .all(Result::is_ok)
    }
}

/// The worker has stopped, so nothing it was asked to save was saved.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct PersistStopped;

/// What waits for the writer, shared with its thread.
#[derive(Default)]
struct Pending {
    /// The newest state of each file not yet handed to the writer.
    request: PersistRequest,
    /// Set by the bridge when it drops the worker, and by the writer when
    /// its thread ends.
    closed: bool,
}

#[derive(Default)]
struct Shared {
    pending: Mutex<Pending>,
    wake: Condvar,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Pending> {
        // The lock is only held to swap values, which cannot panic halfway.
        self.pending.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn close(&self) {
        self.lock().closed = true;
        self.wake.notify_all();
    }
}

/// Closes the writer's side when its thread ends, also by a panic, so
/// requests after it are refused rather than left unanswered.
struct WriterGuard(Arc<Shared>);

impl Drop for WriterGuard {
    fn drop(&mut self) {
        self.0.close();
    }
}

pub(super) struct PersistWorker {
    shared: Arc<Shared>,
    pub(super) results: Receiver<PersistOutcome>,
    /// The `id` of the latest request.
    last_id: u64,
}

impl PersistWorker {
    pub(super) fn new() -> Self {
        Self::with_saver(save_files)
    }

    /// A worker that saves with `save`, which a test can hold up.
    fn with_saver(save: fn(&PersistRequest) -> PersistOutcome) -> Self {
        let (shared, results) = spawn(save);
        Self {
            shared,
            results,
            last_id: 0,
        }
    }

    /// Replace a worker whose thread has ended. Requests keep counting up.
    pub(super) fn restart(&mut self) {
        self.shared.close();
        (self.shared, self.results) = spawn(save_files);
    }

    /// The `id` of the latest request.
    #[cfg(test)]
    pub(super) fn last_requested(&self) -> u64 {
        self.last_id
    }

    /// How many states of the settings and of the session wait for the
    /// writer, besides those it is writing.
    #[cfg(test)]
    fn pending_snapshots(&self) -> (usize, usize) {
        let pending = self.shared.lock();
        (
            usize::from(pending.request.settings.is_some()),
            usize::from(pending.request.session.is_some()),
        )
    }

    /// Ask for `settings` and `session` to be saved, or either, in place of
    /// the states of those files that still wait. Returns the request's
    /// `id`, which the outcome that covers it reaches.
    pub(super) fn request(
        &mut self,
        settings: Option<BridgeSettings>,
        session: Option<SessionSnapshot>,
    ) -> Result<u64, PersistStopped> {
        let mut pending = self.shared.lock();
        if pending.closed {
            return Err(PersistStopped);
        }
        self.last_id += 1;
        pending.request.id = self.last_id;
        if settings.is_some() {
            pending.request.settings = settings;
        }
        if session.is_some() {
            pending.request.session = session;
        }
        drop(pending);
        self.shared.wake.notify_one();
        Ok(self.last_id)
    }
}

impl Drop for PersistWorker {
    fn drop(&mut self) {
        // The writer saves what waits, then ends.
        self.shared.close();
    }
}

fn save_files(request: &PersistRequest) -> PersistOutcome {
    PersistOutcome {
        id: request.id,
        settings: request.settings.as_ref().map(save_settings),
        session: request.session.as_ref().map(save_session_snapshot),
    }
}

/// The next request for the writer, once there is one, or `None` once the
/// worker is closed and nothing waits.
fn next_request(shared: &Shared) -> Option<PersistRequest> {
    let mut pending = shared.lock();
    loop {
        if pending.request.settings.is_some() || pending.request.session.is_some() {
            return Some(std::mem::take(&mut pending.request));
        }
        if pending.closed {
            return None;
        }
        pending = shared
            .wake
            .wait(pending)
            .unwrap_or_else(PoisonError::into_inner);
    }
}

fn spawn(save: fn(&PersistRequest) -> PersistOutcome) -> (Arc<Shared>, Receiver<PersistOutcome>) {
    let shared = Arc::new(Shared::default());
    let writer = Arc::clone(&shared);
    let (result_tx, results) = unbounded();
    // The saves go to the config directory of the test that started them.
    #[cfg(test)]
    let config_dir = crate::paths::config_dir_override();
    let started = crate::worker_thread::spawn_worker("ferrous-persist", move || {
        #[cfg(test)]
        crate::paths::set_config_dir_override(config_dir);
        let guard = WriterGuard(writer);
        while let Some(request) = next_request(&guard.0) {
            if result_tx.send(save(&request)).is_err() {
                break;
            }
        }
    });
    if started.is_none() {
        // Its closure, with the results' sender, was dropped: the bridge
        // finds the worker stopped.
        shared.close();
    }
    (shared, results)
}

/// A worker whose saves wait until the test opens [`STALL_GATE`], as on
/// storage that does not answer.
#[cfg(test)]
pub(super) fn stalled_worker() -> PersistWorker {
    PersistWorker::with_saver(|request| {
        let _ = STALL_GATE.1.recv();
        save_files(request)
    })
}

/// Opens the saves of [`stalled_worker`], one per message.
#[cfg(test)]
pub(super) static STALL_GATE: std::sync::LazyLock<(Sender<()>, Receiver<()>)> =
    std::sync::LazyLock::new(unbounded);

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    fn session(path: &str) -> SessionSnapshot {
        SessionSnapshot {
            queue: vec![path.into()],
            ..SessionSnapshot::default()
        }
    }

    #[test]
    fn saves_land_in_order_and_the_latest_state_wins() {
        let _config = crate::paths::TestConfigDir::new();
        let mut worker = PersistWorker::new();
        let mut last = 0;
        for index in 0..20 {
            last = worker
                .request(None, Some(session(&format!("/music/{index}.flac"))))
                .expect("request");
        }
        let settings = BridgeSettings {
            volume: 0.25,
            ..BridgeSettings::default()
        };
        last = last.max(worker.request(Some(settings), None).expect("request"));
        let mut answered = 0;
        while answered < last {
            let outcome = worker
                .results
                .recv_timeout(Duration::from_secs(10))
                .expect("an outcome");
            assert!(outcome.id > answered, "outcomes come in order");
            assert!(outcome.saved(), "{outcome:?}");
            answered = outcome.id;
        }
        let text = std::fs::read_to_string(super::super::config::session_path().unwrap())
            .expect("the session");
        let saved = super::super::config::parse_session_text(&text).expect("parse");
        assert_eq!(
            saved.queue,
            vec![std::path::PathBuf::from("/music/19.flac")]
        );
        let mut loaded = BridgeSettings::default();
        super::super::config::load_settings_into(&mut loaded);
        assert!((loaded.volume - 0.25).abs() < 1e-4);
    }

    /// A worker whose saves wait until the test sends on the returned gate.
    fn gated_worker() -> PersistWorker {
        PersistWorker::with_saver(|request| {
            let _ = GATE.1.recv();
            save_files(request)
        })
    }

    static GATE: std::sync::LazyLock<(Sender<()>, Receiver<()>)> =
        std::sync::LazyLock::new(unbounded);

    #[test]
    fn a_stalled_save_keeps_one_waiting_state_of_each_file() {
        let _config = crate::paths::TestConfigDir::new();
        let mut worker = gated_worker();
        // The writer takes the first request and stalls in it, as on a
        // drive that does not answer.
        worker
            .request(None, Some(session("/music/first.flac")))
            .expect("request");
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        while worker.pending_snapshots() != (0, 0) {
            assert!(std::time::Instant::now() < deadline, "the writer took it");
            std::thread::yield_now();
        }
        for index in 0..100 {
            worker
                .request(
                    Some(BridgeSettings {
                        volume: 0.5,
                        ..BridgeSettings::default()
                    }),
                    Some(session(&format!("/music/{index}.flac"))),
                )
                .expect("request");
            assert_eq!(worker.pending_snapshots(), (1, 1), "after {index}");
        }
        // Settings and the session are replaced on their own.
        let last = worker
            .request(None, Some(session("/music/newest.flac")))
            .expect("request");
        assert_eq!(worker.pending_snapshots(), (1, 1));

        GATE.0.send(()).expect("open");
        GATE.0.send(()).expect("open");
        let mut answered = 0;
        while answered < last {
            let outcome = worker
                .results
                .recv_timeout(Duration::from_secs(10))
                .expect("an outcome");
            assert!(outcome.saved(), "{outcome:?}");
            answered = outcome.id;
        }
        assert_eq!(answered, last);
        let text = std::fs::read_to_string(super::super::config::session_path().unwrap())
            .expect("the session");
        let saved = super::super::config::parse_session_text(&text).expect("parse");
        assert_eq!(
            saved.queue,
            vec![std::path::PathBuf::from("/music/newest.flac")]
        );
        let mut loaded = BridgeSettings::default();
        super::super::config::load_settings_into(&mut loaded);
        assert!((loaded.volume - 0.5).abs() < 1e-4);
    }

    #[test]
    fn a_worker_whose_writer_ended_refuses_requests() {
        let mut worker = PersistWorker::with_saver(|_| panic!("test panic in the writer"));
        worker
            .request(Some(BridgeSettings::default()), None)
            .expect("request");
        // Its results close, which the bridge takes for a stopped worker.
        assert!(worker
            .results
            .recv_timeout(Duration::from_secs(10))
            .is_err());
        assert_eq!(
            worker.request(Some(BridgeSettings::default()), None),
            Err(PersistStopped)
        );
        // A restarted worker takes them again.
        let _config = crate::paths::TestConfigDir::new();
        worker.restart();
        let id = worker
            .request(Some(BridgeSettings::default()), None)
            .expect("request");
        let outcome = worker
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("an outcome");
        assert_eq!(outcome.id, id);
        assert!(outcome.saved());
    }

    #[test]
    fn a_failed_save_is_reported_with_its_file() {
        let _config = crate::paths::TestConfigDir::new();
        let path = super::super::config::session_path().expect("session path");
        // Nothing can be renamed over a directory.
        std::fs::create_dir_all(&path).expect("block the session");
        let mut worker = PersistWorker::new();
        let id = worker
            .request(Some(BridgeSettings::default()), Some(session("/a.flac")))
            .expect("request");
        let outcome = worker
            .results
            .recv_timeout(Duration::from_secs(10))
            .expect("an outcome");
        assert_eq!(outcome.id, id);
        assert!(!outcome.saved());
        assert!(matches!(outcome.settings, Some(Ok(()))));
        let failure = outcome.session.expect("the session").expect_err("failed");
        assert_eq!(failure.identity.0, path);
    }
}
