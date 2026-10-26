// SPDX-License-Identifier: GPL-3.0-or-later

//! Saving the settings and the session on a thread of their own.
//!
//! A save serializes the whole session, with the queue, and writes it with
//! `fsync`, which slow or failing storage can hold up for long. The bridge
//! thread also drives playback, so it only hands the state over. One writer
//! saves in the order it was asked; when saves wait, only the newest state
//! of each file is written, as it replaces the older ones.

use crossbeam_channel::{unbounded, Receiver, Sender};

use super::config::{save_session_snapshot, save_settings, SaveFailure, SessionSnapshot};
use super::BridgeSettings;

/// What to save. Each request has a larger `id` than the ones before it.
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

pub(super) struct PersistWorker {
    tx: Sender<PersistRequest>,
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
        let (tx, results) = spawn(save);
        Self {
            tx,
            results,
            last_id: 0,
        }
    }

    /// Replace a worker whose thread has ended. Requests keep counting up.
    pub(super) fn restart(&mut self) {
        (self.tx, self.results) = spawn(save_files);
    }

    /// The `id` of the latest request.
    #[cfg(test)]
    pub(super) fn last_requested(&self) -> u64 {
        self.last_id
    }

    /// Ask for `settings` and `session` to be saved, or either. Returns the
    /// request's `id`, which the outcome that covers it reaches.
    pub(super) fn request(
        &mut self,
        settings: Option<BridgeSettings>,
        session: Option<SessionSnapshot>,
    ) -> Result<u64, PersistStopped> {
        self.last_id += 1;
        self.tx
            .send(PersistRequest {
                id: self.last_id,
                settings,
                session,
            })
            .map_err(|_| PersistStopped)?;
        Ok(self.last_id)
    }
}

fn save_files(request: &PersistRequest) -> PersistOutcome {
    PersistOutcome {
        id: request.id,
        settings: request.settings.as_ref().map(save_settings),
        session: request.session.as_ref().map(save_session_snapshot),
    }
}

fn spawn(
    save: fn(&PersistRequest) -> PersistOutcome,
) -> (Sender<PersistRequest>, Receiver<PersistOutcome>) {
    let (tx, rx) = unbounded::<PersistRequest>();
    let (result_tx, results) = unbounded();
    // The saves go to the config directory of the test that started them.
    #[cfg(test)]
    let config_dir = crate::paths::config_dir_override();
    crate::worker_thread::spawn_worker("ferrous-persist", move || {
        #[cfg(test)]
        crate::paths::set_config_dir_override(config_dir);
        while let Ok(mut request) = rx.recv() {
            for newer in rx.try_iter() {
                request = PersistRequest {
                    id: newer.id,
                    settings: newer.settings.or(request.settings),
                    session: newer.session.or(request.session),
                };
            }
            let outcome = save(&request);
            if result_tx.send(outcome).is_err() {
                break;
            }
        }
    });
    (tx, results)
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
