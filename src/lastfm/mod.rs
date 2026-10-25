// SPDX-License-Identifier: GPL-3.0-or-later

mod queue_file;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, select, unbounded, Receiver, RecvTimeoutError, Sender};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use queue_file::LoadOutcome;

pub const KEYRING_SERVICE: &str = "ferrous.lastfm";
const API_ENDPOINT: &str = "https://ws.audioscrobbler.com/2.0/";
const AUTH_ENDPOINT: &str = "https://www.last.fm/api/auth/";
#[cfg(not(test))]
const REQUEST_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_SCROBBLES_PER_BATCH: usize = 50;
const LASTFM_ERROR_OPERATION_FAILED: i64 = 8;
const LASTFM_ERROR_INVALID_SESSION: i64 = 9;
const LASTFM_ERROR_SERVICE_OFFLINE: i64 = 11;
const LASTFM_ERROR_TEMPORARY: i64 = 16;
const LASTFM_ERROR_RATE_LIMIT: i64 = 29;
/// `ignoredMessage` code for a scrobble refused because the account reached
/// its daily limit. Unlike the other ignore codes, it can succeed later.
const LASTFM_IGNORED_DAILY_LIMIT: i64 = 5;
const UNKNOWN_ALBUM_PLACEHOLDER: &str = "Unknown album";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppCredentials {
    pub api_key: String,
    pub shared_secret: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum AuthState {
    #[default]
    Disconnected,
    AwaitingBrowserApproval,
    Connected,
    ReauthRequired,
    Error,
}

#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct RuntimeState {
    pub enabled: bool,
    pub build_configured: bool,
    pub username: String,
    pub auth_state: AuthState,
    pub pending_scrobble_count: usize,
    pub status_text: String,
    pub auth_url: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PendingAuth {
    pub token: String,
    pub auth_url: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Session {
    pub username: String,
    pub session_key: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct NowPlayingTrack {
    pub artist: String,
    pub track: String,
    pub album: String,
    pub track_number: Option<u32>,
    pub duration_seconds: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ScrobbleEntry {
    pub artist: String,
    pub track: String,
    pub album: String,
    pub track_number: Option<u32>,
    pub duration_seconds: Option<u32>,
    pub timestamp_utc: i64,
}

#[derive(Debug, Clone)]
pub enum Command {
    SetEnabled(bool),
    LoadStoredSession {
        username: String,
    },
    BeginDesktopAuth,
    CompleteDesktopAuth,
    Disconnect {
        clear_queue: bool,
    },
    SendNowPlaying(NowPlayingTrack),
    QueueScrobble(ScrobbleEntry),
    Flush,
    /// Sent by [`Handle::shutdown`]; the sender receives the result of
    /// saving the queue once the service has stopped.
    Shutdown(Sender<Result<(), String>>),
}

/// How the service stopped, as seen by [`Handle::shutdown`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ShutdownOutcome {
    /// The service stopped with its queue saved.
    Stopped,
    /// The service stopped, but pending scrobbles could not be saved.
    SaveFailed(String),
    /// The service did not confirm in time, or had already stopped.
    Unconfirmed,
}

#[derive(Debug, Clone)]
pub enum Event {
    State(RuntimeState),
}

#[derive(Debug, Clone)]
pub struct Handle {
    tx: Sender<Command>,
    requests: Arc<RequestCancellation>,
}

impl Handle {
    fn new(tx: Sender<Command>) -> Self {
        Self {
            tx,
            requests: Arc::new(RequestCancellation::new()),
        }
    }

    pub fn command(&self, command: Command) {
        let _ = self.tx.send(command);
    }

    /// Stops the service once it has handled the commands sent before this
    /// call, without starting network requests for them. Waits up to
    /// `timeout` for the queue to be saved.
    ///
    /// The service stops waiting for a request in progress, which can take
    /// up to 15 s, so a scrobble queued behind it is saved at once. The
    /// scrobbles that request carried stay queued, as after any request
    /// without an answer, and are sent again after the next start.
    #[must_use]
    pub fn shutdown(&self, timeout: Duration) -> ShutdownOutcome {
        self.requests.cancel();
        let (done_tx, done_rx) = bounded(1);
        if self.tx.send(Command::Shutdown(done_tx)).is_err() {
            return ShutdownOutcome::Unconfirmed;
        }
        match done_rx.recv_timeout(timeout) {
            Ok(Ok(())) => ShutdownOutcome::Stopped,
            Ok(Err(error)) => ShutdownOutcome::SaveFailed(error),
            Err(_) => ShutdownOutcome::Unconfirmed,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct ServiceOptions {
    pub queue_path: Option<PathBuf>,
    pub initial_enabled: bool,
}

/// A reply from the Last.fm API, before it is interpreted.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct HttpReply {
    pub(crate) status: u16,
    pub(crate) body: String,
}

/// Tells the service to stop waiting for requests, once it is shutting down.
#[derive(Debug)]
struct RequestCancellation {
    cancelled: AtomicBool,
    /// Wakes a request that is waiting for its answer.
    wake_tx: Sender<()>,
    wake_rx: Receiver<()>,
}

impl RequestCancellation {
    fn new() -> Self {
        let (wake_tx, wake_rx) = bounded(1);
        Self {
            cancelled: AtomicBool::new(false),
            wake_tx,
            wake_rx,
        }
    }

    fn cancel(&self) {
        // Set before the wake-up, so a request starting after the wake-up
        // was taken sees the flag.
        self.cancelled.store(true, Ordering::SeqCst);
        let _ = self.wake_tx.try_send(());
    }

    fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::SeqCst)
    }
}

/// Runs each request of `inner` on a thread of its own and waits for its
/// answer only until the service is cancelled, so shutdown never waits for
/// the network. A request that is abandoned finishes on its own thread, and
/// its answer is dropped.
struct CancellableTransport {
    inner: Arc<dyn Transport>,
    cancellation: Arc<RequestCancellation>,
}

impl Transport for CancellableTransport {
    fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
        const CANCELLED: &str = "the request was abandoned because Ferrous is quitting";
        if self.cancellation.is_cancelled() {
            return Err(CANCELLED.to_string());
        }
        let (reply_tx, reply_rx) = bounded(1);
        let inner = Arc::clone(&self.inner);
        let (url, form) = (url.to_string(), form.to_vec());
        // Dropping the thread's handle detaches it. If it cannot start,
        // `reply_tx` is dropped with the closure, which ends the wait below.
        let _ = crate::worker_thread::spawn_worker("ferrous-lastfm-request", move || {
            let _ = reply_tx.send(inner.post_form(&url, &form));
        });
        select! {
            recv(reply_rx) -> reply => reply
                .unwrap_or_else(|_| Err("the request thread stopped without an answer".to_string())),
            recv(self.cancellation.wake_rx) -> _ => Err(CANCELLED.to_string()),
        }
    }
}

/// Sends requests to the Last.fm API.
pub(crate) trait Transport: Send + Sync {
    /// Posts `form` to `url`. Returns the reply, or a description of why no
    /// reply arrived.
    fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String>;
}

/// Stores the session keys that let Ferrous scrobble to an account.
pub(crate) trait SessionKeyStore: Send {
    /// Returns `Ok(None)` when no key is stored for `username`.
    fn load(&self, username: &str) -> Result<Option<String>, String>;
    fn store(&self, username: &str, session_key: &str) -> Result<(), String>;
    /// Succeeds when no key is stored for `username`.
    fn delete(&self, username: &str) -> Result<(), String>;
}

/// How long to wait before retrying a flush that failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct RetryPolicy {
    pub(crate) initial: Duration,
    pub(crate) max: Duration,
}

impl RetryPolicy {
    const DEFAULT: Self = Self {
        initial: Duration::from_secs(30),
        max: Duration::from_secs(30 * 60),
    };

    /// Delay before retry `attempt` (counted from 1): doubles from `initial`
    /// up to `max`.
    fn delay(self, attempt: u32) -> Duration {
        let doublings = attempt.saturating_sub(1).min(31);
        self.initial.saturating_mul(1 << doublings).min(self.max)
    }
}

/// External dependencies of the service.
pub(crate) struct ServiceBackends {
    pub(crate) transport: Box<dyn Transport>,
    pub(crate) session_keys: Box<dyn SessionKeyStore>,
    pub(crate) credentials: Option<AppCredentials>,
    pub(crate) retry: RetryPolicy,
}

impl ServiceBackends {
    #[cfg(not(test))]
    fn for_build() -> Self {
        Self {
            transport: Box::new(HttpTransport::new()),
            session_keys: Box::new(KeyringSessionKeys),
            credentials: app_credentials(),
            retry: RetryPolicy::DEFAULT,
        }
    }

    /// Test builds never reach the network or the system keyring.
    #[cfg(test)]
    fn for_build() -> Self {
        Self {
            transport: Box::new(test_support::OfflineTransport),
            session_keys: Box::new(test_support::MemorySessionKeys::default()),
            credentials: None,
            retry: RetryPolicy::DEFAULT,
        }
    }
}

#[cfg(not(test))]
struct HttpTransport {
    client: reqwest::blocking::Client,
}

#[cfg(not(test))]
impl HttpTransport {
    fn new() -> Self {
        let client = reqwest::blocking::Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()
            .unwrap_or_else(|_| reqwest::blocking::Client::new());
        Self { client }
    }
}

#[cfg(not(test))]
impl Transport for HttpTransport {
    fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
        let response = self
            .client
            .post(url)
            .form(form)
            .send()
            .map_err(|err| err.to_string())?;
        let status = response.status().as_u16();
        let body = response.text().map_err(|err| err.to_string())?;
        Ok(HttpReply { status, body })
    }
}

#[cfg(not(test))]
struct KeyringSessionKeys;

#[cfg(not(test))]
impl KeyringSessionKeys {
    fn entry(username: &str) -> Result<keyring::Entry, String> {
        keyring::Entry::new(KEYRING_SERVICE, username).map_err(|err| err.to_string())
    }
}

#[cfg(not(test))]
impl SessionKeyStore for KeyringSessionKeys {
    fn load(&self, username: &str) -> Result<Option<String>, String> {
        stored_session_key(Self::entry(username)?.get_password())
    }

    fn store(&self, username: &str, session_key: &str) -> Result<(), String> {
        Self::entry(username)?
            .set_password(session_key)
            .map_err(|err| err.to_string())
    }

    fn delete(&self, username: &str) -> Result<(), String> {
        session_key_deleted(Self::entry(username)?.delete_credential())
    }
}

/// Interprets a keyring lookup; a missing or blank entry means no key.
fn stored_session_key(lookup: keyring::Result<String>) -> Result<Option<String>, String> {
    match lookup {
        Ok(value) if !value.trim().is_empty() => Ok(Some(value)),
        Ok(_) | Err(keyring::Error::NoEntry) => Ok(None),
        Err(err) => Err(err.to_string()),
    }
}

/// Interprets a keyring deletion; an entry that is already gone is deleted.
fn session_key_deleted(deletion: keyring::Result<()>) -> Result<(), String> {
    match deletion {
        Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
        Err(err) => Err(err.to_string()),
    }
}

struct Service {
    transport: Box<dyn Transport>,
    session_keys: Box<dyn SessionKeyStore>,
    credentials: Option<AppCredentials>,
    retry_policy: RetryPolicy,
    /// Failed flush attempts since the last batch Last.fm fully accepted.
    retry_attempt: u32,
    /// When the next automatic flush is due, if one is scheduled.
    retry_due: Option<Instant>,
    /// Cleared while handling the commands that precede shutdown, so that
    /// shutdown does not wait for the network.
    network_allowed: bool,
    /// The service's own command channel. A flush checks it between batches
    /// so that waiting commands, such as shutdown, go first.
    waiting_commands: Option<Receiver<Command>>,
    /// Entries ignored since a flush last completed, reported when one does.
    flush_ignored: IgnoredScrobbles,
    queue_path: Option<PathBuf>,
    queue: Vec<ScrobbleEntry>,
    /// Problem found while loading the queue file, shown for the session.
    queue_load_notice: Option<String>,
    /// Most recent failure to save the queue, until a save succeeds.
    queue_save_error: Option<String>,
    session: Option<Session>,
    pending_auth: Option<PendingAuth>,
    state: RuntimeState,
}

#[derive(Debug)]
enum RequestError {
    Transport(String),
    /// A JSON-less or unsuccessful HTTP status without a Last.fm error code.
    Http {
        status: u16,
    },
    Api {
        code: i64,
        message: String,
    },
    InvalidResponse(String),
    Keyring(String),
}

/// Entries that Last.fm ignored permanently during a flush.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct IgnoredScrobbles {
    count: usize,
    /// Reason given for the first ignored entry.
    reason: Option<String>,
}

/// What Last.fm did with each entry of an accepted scrobble batch.
#[derive(Debug, Clone, PartialEq, Eq)]
struct ScrobbleOutcome {
    /// Per entry, in request order: whether Last.fm deferred it, so it
    /// stays queued.
    deferred: Vec<bool>,
    /// Entries Last.fm ignored permanently.
    ignored: usize,
    /// Reason given for the first ignored entry.
    ignored_reason: Option<String>,
}

#[must_use]
pub fn app_credentials() -> Option<AppCredentials> {
    let api_key = option_env!("FERROUS_LASTFM_API_KEY")?.trim();
    let shared_secret = option_env!("FERROUS_LASTFM_SHARED_SECRET")?.trim();
    if api_key.is_empty() || shared_secret.is_empty() {
        return None;
    }
    Some(AppCredentials {
        api_key: api_key.to_string(),
        shared_secret: shared_secret.to_string(),
    })
}

#[must_use]
pub fn queue_path(config_base: &Path) -> PathBuf {
    config_base.join("lastfm_queue.json")
}

#[must_use]
pub fn scrobble_threshold_seconds(duration_seconds: u32) -> Option<u32> {
    if duration_seconds <= 30 {
        return None;
    }
    Some((duration_seconds / 2).min(240))
}

#[must_use]
pub(crate) fn normalize_album_for_submission(album: &str) -> String {
    let trimmed = album.trim();
    if trimmed.is_empty() || trimmed.eq_ignore_ascii_case(UNKNOWN_ALBUM_PLACEHOLDER) {
        return String::new();
    }
    trimmed.to_string()
}

#[must_use]
pub fn spawn(options: ServiceOptions) -> (Handle, Receiver<Event>) {
    spawn_with_backends(options, ServiceBackends::for_build)
}

/// Starts the service thread. `backends` runs on that thread, because
/// building the HTTP client starts and waits for a runtime.
pub(crate) fn spawn_with_backends(
    options: ServiceOptions,
    backends: impl FnOnce() -> ServiceBackends + Send + 'static,
) -> (Handle, Receiver<Event>) {
    let (cmd_tx, cmd_rx) = unbounded();
    let (event_tx, event_rx) = unbounded();
    let handle = Handle::new(cmd_tx);
    let cancellation = Arc::clone(&handle.requests);
    crate::worker_thread::spawn_worker("ferrous-lastfm", move || {
        let mut backends = backends();
        backends.transport = Box::new(CancellableTransport {
            inner: Arc::from(backends.transport),
            cancellation,
        });
        let service = Service::new(options, backends);
        run_service_loop(&cmd_rx, &event_tx, service);
    });
    (handle, event_rx)
}

fn run_service_loop(cmd_rx: &Receiver<Command>, event_tx: &Sender<Event>, mut service: Service) {
    service.waiting_commands = Some(cmd_rx.clone());
    service.publish(event_tx);
    loop {
        let received = match service.retry_due {
            Some(due) => cmd_rx.recv_deadline(due),
            None => cmd_rx.recv().map_err(|_| RecvTimeoutError::Disconnected),
        };
        let first = match received {
            Ok(command) => command,
            Err(RecvTimeoutError::Timeout) => {
                service.flush_queue();
                service.publish(event_tx);
                continue;
            }
            Err(RecvTimeoutError::Disconnected) => {
                let _ = service.save_unsaved_queue();
                break;
            }
        };
        // Take everything already waiting, so that a pending shutdown is seen
        // before the commands ahead of it start network requests.
        let mut commands = vec![first];
        commands.extend(cmd_rx.try_iter());
        let shutdown = commands.iter().find_map(|command| match command {
            Command::Shutdown(done) => Some(done.clone()),
            _ => None,
        });
        if shutdown.is_some() {
            service.network_allowed = false;
        }
        for command in commands {
            if matches!(command, Command::Shutdown(_)) {
                break;
            }
            service.handle_command(command);
            service.publish(event_tx);
        }
        if let Some(done) = shutdown {
            let saved = service.save_unsaved_queue();
            service.publish(event_tx);
            let _ = done.send(saved);
            break;
        }
    }
}

impl Service {
    fn new(options: ServiceOptions, backends: ServiceBackends) -> Self {
        let build_configured = backends.credentials.is_some();
        let mut service = Self {
            transport: backends.transport,
            session_keys: backends.session_keys,
            credentials: backends.credentials,
            retry_policy: backends.retry,
            retry_attempt: 0,
            retry_due: None,
            network_allowed: true,
            waiting_commands: None,
            flush_ignored: IgnoredScrobbles::default(),
            queue_path: options.queue_path,
            queue: Vec::new(),
            queue_load_notice: None,
            queue_save_error: None,
            session: None,
            pending_auth: None,
            state: RuntimeState {
                enabled: options.initial_enabled,
                build_configured,
                status_text: if build_configured {
                    String::new()
                } else {
                    "Last.fm not configured in this build.".to_string()
                },
                ..RuntimeState::default()
            },
        };
        service.load_queue();
        service
    }

    fn load_queue(&mut self) {
        let Some(path) = self.queue_path.clone() else {
            return;
        };
        match queue_file::load(&path) {
            LoadOutcome::Loaded(entries) => self.queue = entries,
            LoadOutcome::MovedAside { moved_to, reason } => {
                eprintln!(
                    "Last.fm: scrobble queue {} is not valid ({reason}); moved it to {}",
                    path.display(),
                    moved_to.display()
                );
                self.queue_load_notice = Some(format!(
                    "Saved scrobbles could not be read; the file was kept as {}.",
                    moved_to.display()
                ));
            }
            LoadOutcome::Unavailable { error } => {
                eprintln!(
                    "Last.fm: cannot load scrobble queue {}: {error}",
                    path.display()
                );
                // Saving now would replace a file that may still hold scrobbles.
                self.queue_path = None;
                self.queue_load_notice = Some(format!(
                    "Saved scrobbles in {} could not be read ({error}). New scrobbles \
                     will not be saved until Ferrous restarts.",
                    path.display()
                ));
            }
        }
    }

    /// The state shown in the UI. Queue file problems stay visible above
    /// later status messages.
    fn published_state(&self) -> RuntimeState {
        let mut state = self.state.clone();
        state.pending_scrobble_count = self.queue.len();
        state.status_text = [
            self.queue_load_notice.as_deref(),
            self.queue_save_error.as_deref(),
            Some(self.state.status_text.as_str()),
        ]
        .into_iter()
        .flatten()
        .filter(|text| !text.is_empty())
        .collect::<Vec<_>>()
        .join(" ");
        state
    }

    fn publish(&self, event_tx: &Sender<Event>) {
        let _ = event_tx.send(Event::State(self.published_state()));
    }

    fn handle_command(&mut self, command: Command) {
        match command {
            Command::SetEnabled(enabled) => {
                self.state.enabled = enabled;
                self.flush_queue();
            }
            Command::LoadStoredSession { username } => self.load_session(username),
            Command::BeginDesktopAuth => self.begin_desktop_auth(),
            Command::CompleteDesktopAuth => self.complete_desktop_auth(),
            Command::Disconnect { clear_queue } => self.disconnect(clear_queue),
            Command::SendNowPlaying(track) => self.send_now_playing(&track),
            Command::QueueScrobble(entry) => {
                self.queue.push(entry);
                self.save_queue();
                self.flush_queue();
            }
            Command::Flush => self.flush_queue(),
            // The service loop stops before handing shutdown to a command.
            Command::Shutdown(_) => {}
        }
    }

    fn begin_desktop_auth(&mut self) {
        if !self.network_allowed {
            return;
        }
        let Some(credentials) = self.credentials.as_ref() else {
            self.state.auth_state = AuthState::Error;
            self.state.status_text = "Last.fm not configured in this build.".to_string();
            return;
        };
        match request_token(self.transport.as_ref(), credentials) {
            Ok(token) => {
                let auth_url = format!(
                    "{}?api_key={}&token={}",
                    AUTH_ENDPOINT, credentials.api_key, token
                );
                self.pending_auth = Some(PendingAuth {
                    token,
                    auth_url: auth_url.clone(),
                });
                self.state.auth_state = AuthState::AwaitingBrowserApproval;
                self.state.auth_url = auth_url;
                self.state.status_text =
                    "Authorize Ferrous in your browser, then click Complete Connection."
                        .to_string();
            }
            Err(err) => {
                self.state.auth_state = AuthState::Error;
                self.state.status_text = request_error_message(&err);
            }
        }
    }

    fn complete_desktop_auth(&mut self) {
        if !self.network_allowed {
            return;
        }
        let Some(credentials) = self.credentials.as_ref() else {
            self.state.auth_state = AuthState::Error;
            self.state.status_text = "Last.fm not configured in this build.".to_string();
            return;
        };
        let Some(pending_auth) = self.pending_auth.clone() else {
            self.state.auth_state = AuthState::Error;
            self.state.status_text = "No Last.fm authorization is pending.".to_string();
            return;
        };
        match request_session(self.transport.as_ref(), credentials, &pending_auth.token) {
            Ok(session) => {
                if let Err(err) = self
                    .session_keys
                    .store(&session.username, &session.session_key)
                {
                    self.state.auth_state = AuthState::Error;
                    self.state.status_text = request_error_message(&RequestError::Keyring(err));
                    return;
                }
                self.session = Some(session.clone());
                self.pending_auth = None;
                self.state.username = session.username;
                self.state.auth_state = AuthState::Connected;
                self.state.auth_url.clear();
                self.state.status_text = "Last.fm account connected.".to_string();
                self.retry_attempt = 0;
                self.flush_queue();
            }
            Err(err) => {
                self.state.auth_state = AuthState::Error;
                self.state.status_text = request_error_message(&err);
            }
        }
    }

    fn load_session(&mut self, username: String) {
        if username.trim().is_empty() {
            return;
        }
        match self.session_keys.load(&username) {
            Ok(Some(session_key)) => {
                self.session = Some(Session {
                    username: username.clone(),
                    session_key,
                });
                self.state.username = username;
                self.state.auth_state = AuthState::Connected;
                self.state.status_text.clear();
                self.flush_queue();
            }
            Ok(None) => {
                eprintln!(
                    "Last.fm: no session key found in keyring for user \"{username}\" — \
                     was the keyring cleared or is the keyring daemon unavailable?"
                );
                self.state.username = username;
                self.state.auth_state = AuthState::Disconnected;
                self.state.status_text = "Reconnect Last.fm to resume scrobbling.".to_string();
            }
            Err(err) => {
                let message = request_error_message(&RequestError::Keyring(err));
                eprintln!(
                    "Last.fm: keyring error loading session for user \"{username}\": {message}"
                );
                self.state.username = username;
                self.state.auth_state = AuthState::Error;
                self.state.status_text = message;
            }
        }
    }

    fn disconnect(&mut self, clear_queue: bool) {
        let mut status_text = "Last.fm disconnected.".to_string();
        if !self.state.username.trim().is_empty() {
            if let Err(err) = self.session_keys.delete(&self.state.username) {
                status_text = format!(
                    "Last.fm disconnected, but the session key could not be removed \
                     from the keyring: {err}"
                );
            }
        }
        self.session = None;
        self.pending_auth = None;
        self.retry_attempt = 0;
        self.retry_due = None;
        self.state.username.clear();
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::Disconnected;
        self.state.status_text = status_text;
        if clear_queue {
            self.queue.clear();
            self.save_queue();
        }
    }

    fn send_now_playing(&mut self, track: &NowPlayingTrack) {
        if !self.state.enabled || !self.network_allowed {
            return;
        }
        let Some(credentials) = self.credentials.as_ref() else {
            return;
        };
        let Some(session) = self.session.as_ref() else {
            return;
        };
        if track.artist.trim().is_empty() || track.track.trim().is_empty() {
            return;
        }
        match send_now_playing_request(self.transport.as_ref(), credentials, session, track) {
            Ok(()) => {
                self.state.status_text = format!("Now playing: {} - {}", track.artist, track.track);
                // Last.fm is reachable again; do not wait for the scheduled retry.
                if self.retry_due.is_some() {
                    self.flush_queue();
                }
            }
            Err(err) if is_invalid_session(&err) => {
                self.transition_to_reauth(
                    "Last.fm session expired. Reconnect to resume scrobbling.",
                );
            }
            Err(err) => {
                self.state.status_text = request_error_message(&err);
            }
        }
    }

    /// Submits queued scrobbles in batches. Stops at the first batch that
    /// fails and keeps it queued; only entries that Last.fm accepted or
    /// ignored for good leave the queue. Transient failures schedule a retry.
    /// Between batches, commands that are waiting go first; the flush then
    /// resumes.
    fn flush_queue(&mut self) {
        self.retry_due = None;
        if !self.state.enabled || !self.network_allowed || self.queue.is_empty() {
            return;
        }
        let (Some(credentials), Some(session)) = (self.credentials.clone(), self.session.clone())
        else {
            return;
        };

        while !self.queue.is_empty() {
            let batch_len = self.queue.len().min(MAX_SCROBBLES_PER_BATCH);
            let result = send_scrobble_batch(
                self.transport.as_ref(),
                &credentials,
                &session,
                &self.queue[..batch_len],
            );
            match result {
                Ok(outcome) => {
                    let mut index = 0;
                    self.queue.retain(|_| {
                        let keep = index >= batch_len || outcome.deferred[index];
                        index += 1;
                        keep
                    });
                    self.save_queue();
                    self.flush_ignored.count += outcome.ignored;
                    if self.flush_ignored.reason.is_none() {
                        self.flush_ignored.reason = outcome.ignored_reason;
                    }
                    if outcome.deferred.contains(&true) {
                        self.schedule_retry("Last.fm daily scrobble limit reached");
                        return;
                    }
                    self.retry_attempt = 0;
                    if !self.queue.is_empty() && self.commands_waiting() {
                        // The service loop handles the waiting commands, then
                        // resumes the flush at once.
                        self.retry_due = Some(Instant::now());
                        return;
                    }
                }
                Err(err) if is_invalid_session(&err) => {
                    self.transition_to_reauth(
                        "Last.fm session expired. Reconnect to flush queued scrobbles.",
                    );
                    return;
                }
                Err(err) if is_transient(&err) => {
                    self.schedule_retry(&request_error_message(&err));
                    return;
                }
                Err(err) => {
                    // Last.fm refused the request itself, for example because
                    // the API key is invalid. That says nothing about these
                    // scrobbles, so they stay queued for the next flush.
                    self.state.status_text =
                        format!("{}. Scrobbles stay queued.", request_error_message(&err));
                    return;
                }
            }
        }
        let ignored = std::mem::take(&mut self.flush_ignored);
        self.state.status_text = match ignored.reason {
            None => "Last.fm scrobble queue flushed.".to_string(),
            Some(reason) => format!(
                "Last.fm ignored {} {}: {reason}",
                ignored.count,
                if ignored.count == 1 {
                    "scrobble"
                } else {
                    "scrobbles"
                }
            ),
        };
    }

    fn commands_waiting(&self) -> bool {
        self.waiting_commands
            .as_ref()
            .is_some_and(|commands| !commands.is_empty())
    }

    fn schedule_retry(&mut self, message: &str) {
        self.retry_attempt = self.retry_attempt.saturating_add(1);
        let delay = self.retry_policy.delay(self.retry_attempt);
        self.retry_due = Some(Instant::now() + delay);
        self.state.status_text = format!("{message}; retrying in {}.", describe_delay(delay));
    }

    fn transition_to_reauth(&mut self, message: &str) {
        self.session = None;
        self.pending_auth = None;
        self.retry_attempt = 0;
        self.retry_due = None;
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::ReauthRequired;
        self.state.status_text = message.to_string();
    }

    /// Saves the queue if the last save failed. Every change to the queue
    /// is saved, so it differs from the file only after a failed save.
    fn save_unsaved_queue(&mut self) -> Result<(), String> {
        if self.queue_save_error.is_none() {
            return Ok(());
        }
        self.save_queue();
        match &self.queue_save_error {
            None => Ok(()),
            Some(error) => Err(format!(
                "Last.fm: {} {} lost on quit. {error}",
                self.queue.len(),
                if self.queue.len() == 1 {
                    "scrobble was"
                } else {
                    "scrobbles were"
                }
            )),
        }
    }

    fn save_queue(&mut self) {
        let Some(path) = self.queue_path.as_ref() else {
            return;
        };
        match queue_file::save(path, &self.queue) {
            Ok(()) => self.queue_save_error = None,
            Err(err) => {
                eprintln!(
                    "Last.fm: failed to save scrobble queue {}: {err}",
                    path.display()
                );
                self.queue_save_error = Some(format!(
                    "Pending scrobbles could not be saved to {} ({err}).",
                    path.display()
                ));
            }
        }
    }
}

fn describe_delay(delay: Duration) -> String {
    let seconds = delay.as_secs();
    if seconds >= 120 {
        format!("{} min", seconds / 60)
    } else if seconds >= 1 {
        format!("{seconds} s")
    } else {
        format!("{} ms", delay.as_millis())
    }
}

fn request_token(
    transport: &dyn Transport,
    credentials: &AppCredentials,
) -> Result<String, RequestError> {
    let payload = send_signed_request(transport, credentials, "auth.getToken", Vec::new())?;
    payload
        .get("token")
        .and_then(Value::as_str)
        .filter(|value| !value.trim().is_empty())
        .map(ToString::to_string)
        .ok_or_else(|| {
            RequestError::InvalidResponse("Last.fm token missing from response".to_string())
        })
}

fn request_session(
    transport: &dyn Transport,
    credentials: &AppCredentials,
    token: &str,
) -> Result<Session, RequestError> {
    let payload = send_signed_request(
        transport,
        credentials,
        "auth.getSession",
        vec![("token".to_string(), token.to_string())],
    )?;
    let Some(session) = payload.get("session") else {
        return Err(RequestError::InvalidResponse(
            "Last.fm session missing from response".to_string(),
        ));
    };
    let username = session
        .get("name")
        .and_then(Value::as_str)
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| RequestError::InvalidResponse("Last.fm session name missing".to_string()))?
        .to_string();
    let session_key = session
        .get("key")
        .and_then(Value::as_str)
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| RequestError::InvalidResponse("Last.fm session key missing".to_string()))?
        .to_string();
    Ok(Session {
        username,
        session_key,
    })
}

fn send_now_playing_request(
    transport: &dyn Transport,
    credentials: &AppCredentials,
    session: &Session,
    track: &NowPlayingTrack,
) -> Result<(), RequestError> {
    let album = normalize_album_for_submission(&track.album);
    let mut params = vec![
        ("artist".to_string(), track.artist.clone()),
        ("track".to_string(), track.track.clone()),
        ("sk".to_string(), session.session_key.clone()),
    ];
    if !album.is_empty() {
        params.push(("album".to_string(), album));
    }
    if let Some(track_number) = track.track_number {
        params.push(("trackNumber".to_string(), track_number.to_string()));
    }
    if let Some(duration_seconds) = track.duration_seconds {
        params.push(("duration".to_string(), duration_seconds.to_string()));
    }
    let _ = send_signed_request(transport, credentials, "track.updateNowPlaying", params)?;
    Ok(())
}

fn send_scrobble_batch(
    transport: &dyn Transport,
    credentials: &AppCredentials,
    session: &Session,
    batch: &[ScrobbleEntry],
) -> Result<ScrobbleOutcome, RequestError> {
    let mut params = vec![("sk".to_string(), session.session_key.clone())];
    for (index, track) in batch.iter().enumerate() {
        let suffix = format!("[{index}]");
        let album = normalize_album_for_submission(&track.album);
        params.push((format!("artist{suffix}"), track.artist.clone()));
        params.push((format!("track{suffix}"), track.track.clone()));
        params.push((
            format!("timestamp{suffix}"),
            track.timestamp_utc.to_string(),
        ));
        if !album.is_empty() {
            params.push((format!("album{suffix}"), album));
        }
        if let Some(track_number) = track.track_number {
            params.push((format!("trackNumber{suffix}"), track_number.to_string()));
        }
        if let Some(duration_seconds) = track.duration_seconds {
            params.push((format!("duration{suffix}"), duration_seconds.to_string()));
        }
    }
    let payload = send_signed_request(transport, credentials, "track.scrobble", params)?;
    scrobble_outcome(&payload, batch.len())
}

/// Reads the per-entry results of a `track.scrobble` reply. A reply that does
/// not account for every entry is invalid, so the whole batch stays queued.
fn scrobble_outcome(payload: &Value, batch_len: usize) -> Result<ScrobbleOutcome, RequestError> {
    let invalid = |message: String| Err(RequestError::InvalidResponse(message));
    let Some(scrobbles) = payload.get("scrobbles").filter(|value| value.is_object()) else {
        return invalid("Last.fm scrobble results missing from response".to_string());
    };
    // Last.fm sends a lone object instead of an array for a single result.
    let results = match scrobbles.get("scrobble") {
        Some(Value::Array(results)) => results.iter().collect(),
        Some(result @ Value::Object(_)) => vec![result],
        _ => Vec::new(),
    };
    // Results are in request order, so only one per entry can be matched to
    // the entries.
    if results.len() != batch_len {
        return invalid(format!(
            "Last.fm returned {} scrobble results for {batch_len} scrobbles",
            results.len()
        ));
    }
    // Every result carries an ignore code, 0 when it was accepted.
    let Some(codes) = results
        .iter()
        .map(|result| {
            result
                .get("ignoredMessage")
                .and_then(|message| message.get("code"))
                .and_then(json_integer)
        })
        .collect::<Option<Vec<_>>>()
    else {
        return invalid("Last.fm scrobble result without an ignore code".to_string());
    };
    let accepted = codes.iter().filter(|code| **code == 0).count();
    let attr = scrobbles.get("@attr");
    for (name, expected) in [("accepted", accepted), ("ignored", batch_len - accepted)] {
        let Some(value) = attr.and_then(|attr| attr.get(name)) else {
            continue;
        };
        let count = json_integer(value).and_then(|count| usize::try_from(count).ok());
        if count != Some(expected) {
            return invalid(format!(
                "Last.fm reported {value} {name} scrobbles, but the results show {expected}"
            ));
        }
    }

    let mut outcome = ScrobbleOutcome {
        deferred: vec![false; batch_len],
        ignored: 0,
        ignored_reason: None,
    };
    for (index, (result, code)) in results.into_iter().zip(codes).enumerate() {
        let ignored_message = result.get("ignoredMessage");
        match code {
            0 => {}
            LASTFM_IGNORED_DAILY_LIMIT => outcome.deferred[index] = true,
            _ => {
                outcome.ignored += 1;
                if outcome.ignored_reason.is_none() {
                    let text = ignored_message
                        .and_then(|message| message.get("#text"))
                        .and_then(Value::as_str)
                        .map(str::trim)
                        .filter(|text| !text.is_empty());
                    outcome.ignored_reason = Some(
                        text.map_or_else(|| format!("ignore code {code}"), ToString::to_string),
                    );
                }
            }
        }
    }
    Ok(outcome)
}

fn send_signed_request(
    transport: &dyn Transport,
    credentials: &AppCredentials,
    method: &str,
    mut params: Vec<(String, String)>,
) -> Result<Value, RequestError> {
    params.push(("method".to_string(), method.to_string()));
    params.push(("api_key".to_string(), credentials.api_key.clone()));
    let api_sig = api_signature(&params, &credentials.shared_secret);
    params.push(("api_sig".to_string(), api_sig));
    params.push(("format".to_string(), "json".to_string()));

    let reply = transport
        .post_form(API_ENDPOINT, &params)
        .map_err(RequestError::Transport)?;
    interpret_reply(&reply)
}

/// Last.fm reports API errors as JSON, sometimes with a 4xx status. A reply
/// that is not JSON, such as a proxy error page or a captive portal, says
/// nothing about the request.
fn interpret_reply(reply: &HttpReply) -> Result<Value, RequestError> {
    let Ok(payload) = serde_json::from_str::<Value>(&reply.body) else {
        return Err(if reply.status >= 500 {
            RequestError::Http {
                status: reply.status,
            }
        } else {
            RequestError::InvalidResponse(format!(
                "unexpected non-JSON reply (HTTP {})",
                reply.status
            ))
        });
    };
    if let Some(code) = payload.get("error").and_then(json_integer) {
        let message = payload
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("Last.fm request failed")
            .to_string();
        return Err(RequestError::Api { code, message });
    }
    if !(200..300).contains(&reply.status) {
        return Err(RequestError::Http {
            status: reply.status,
        });
    }
    Ok(payload)
}

/// Reads an integer that Last.fm may send as a number or a string.
fn json_integer(value: &Value) -> Option<i64> {
    value
        .as_i64()
        .or_else(|| value.as_str().and_then(|text| text.trim().parse().ok()))
}

fn api_signature(params: &[(String, String)], shared_secret: &str) -> String {
    let mut pairs = params
        .iter()
        .filter(|(key, _)| key != "format" && key != "callback")
        .collect::<Vec<_>>();
    pairs.sort_by(|a, b| a.0.cmp(&b.0));
    let mut data = String::new();
    for (key, value) in pairs {
        data.push_str(key);
        data.push_str(value);
    }
    data.push_str(shared_secret);
    format!("{:x}", md5::compute(data.as_bytes()))
}

fn is_invalid_session(error: &RequestError) -> bool {
    matches!(
        error,
        RequestError::Api {
            code: LASTFM_ERROR_INVALID_SESSION,
            ..
        }
    )
}

/// Whether a failure is expected to clear up by itself, so that a flush
/// should be retried on a timer.
fn is_transient(error: &RequestError) -> bool {
    match error {
        RequestError::Transport(_) | RequestError::InvalidResponse(_) => true,
        RequestError::Http { status } => *status >= 500,
        RequestError::Api { code, .. } => matches!(
            *code,
            LASTFM_ERROR_OPERATION_FAILED
                | LASTFM_ERROR_SERVICE_OFFLINE
                | LASTFM_ERROR_TEMPORARY
                | LASTFM_ERROR_RATE_LIMIT
        ),
        RequestError::Keyring(_) => false,
    }
}

fn request_error_message(error: &RequestError) -> String {
    match error {
        RequestError::Transport(message) => format!("Last.fm transport error: {message}"),
        RequestError::Http { status } => format!("Last.fm HTTP error {status}"),
        RequestError::Api { code, message } => format!("Last.fm API error {code}: {message}"),
        RequestError::InvalidResponse(message) => format!("Last.fm response error: {message}"),
        RequestError::Keyring(message) => format!("Last.fm keyring error: {message}"),
    }
}

#[cfg(test)]
pub(crate) mod test_support {
    use std::collections::{HashMap, VecDeque};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Mutex, PoisonError};

    use crossbeam_channel::{bounded, unbounded, Receiver, Sender};

    use super::{
        AppCredentials, Command, Event, Handle, HttpReply, RetryPolicy, ServiceBackends,
        ServiceOptions, SessionKeyStore, Transport,
    };

    /// Starts a service, connected with a stored session, that sends its
    /// requests through `transport` and saves its queue to `queue_path`.
    pub(crate) fn spawn_connected(
        transport: impl Transport + 'static,
        queue_path: PathBuf,
    ) -> (Handle, Receiver<Event>) {
        let (handle, events) = super::spawn_with_backends(
            ServiceOptions {
                queue_path: Some(queue_path),
                initial_enabled: true,
            },
            move || ServiceBackends {
                transport: Box::new(transport),
                session_keys: Box::new(MemorySessionKeys::with_key("tester", "session")),
                credentials: Some(AppCredentials {
                    api_key: "api-key".to_string(),
                    shared_secret: "secret".to_string(),
                }),
                retry: RetryPolicy::DEFAULT,
            },
        );
        handle.command(Command::LoadStoredSession {
            username: "tester".to_string(),
        });
        (handle, events)
    }

    /// Holds every request until the test releases it, as a server that
    /// does not answer would.
    pub(crate) struct BlockingTransport {
        started: Sender<()>,
        release: Receiver<()>,
        answered: Arc<AtomicUsize>,
    }

    /// The test's side of a [`BlockingTransport`].
    pub(crate) struct BlockedRequests {
        /// Gets a message when a request starts.
        pub(crate) started: Receiver<()>,
        /// Lets one request answer, with a transport error.
        pub(crate) release: Sender<()>,
        answered: Arc<AtomicUsize>,
    }

    impl BlockedRequests {
        /// How many requests have answered.
        pub(crate) fn answered(&self) -> usize {
            self.answered.load(Ordering::SeqCst)
        }
    }

    impl BlockingTransport {
        pub(crate) fn new() -> (Self, BlockedRequests) {
            let (started_tx, started) = unbounded();
            let (release, release_rx) = bounded(0);
            let answered = Arc::new(AtomicUsize::new(0));
            (
                Self {
                    started: started_tx,
                    release: release_rx,
                    answered: Arc::clone(&answered),
                },
                BlockedRequests {
                    started,
                    release,
                    answered,
                },
            )
        }
    }

    impl Transport for BlockingTransport {
        fn post_form(&self, _url: &str, _form: &[(String, String)]) -> Result<HttpReply, String> {
            let _ = self.started.send(());
            let _ = self.release.recv();
            self.answered.fetch_add(1, Ordering::SeqCst);
            Err("the test released the request".to_string())
        }
    }

    /// A handle whose commands go to the returned receiver instead of a
    /// running service.
    pub(crate) fn command_handle() -> (Handle, Receiver<Command>) {
        let (tx, rx) = unbounded();
        (Handle::new(tx), rx)
    }

    pub(crate) struct OfflineTransport;

    impl Transport for OfflineTransport {
        fn post_form(&self, _url: &str, _form: &[(String, String)]) -> Result<HttpReply, String> {
            Err("network access is disabled in tests".to_string())
        }
    }

    /// Answers requests with scripted replies, in order, and records them.
    #[derive(Clone, Default)]
    pub(crate) struct ScriptedTransport {
        replies: Arc<Mutex<VecDeque<Result<HttpReply, String>>>>,
        requests: Arc<Mutex<Vec<Vec<(String, String)>>>>,
    }

    impl ScriptedTransport {
        pub(crate) fn with_replies(
            replies: impl IntoIterator<Item = Result<HttpReply, String>>,
        ) -> Self {
            let transport = Self::default();
            transport
                .replies
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .extend(replies);
            transport
        }

        pub(crate) fn requests(&self) -> Vec<Vec<(String, String)>> {
            self.requests
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .clone()
        }
    }

    impl Transport for ScriptedTransport {
        fn post_form(&self, _url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
            self.requests
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .push(form.to_vec());
            self.replies
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .pop_front()
                .unwrap_or_else(|| Err("no scripted reply left".to_string()))
        }
    }

    #[derive(Clone, Default)]
    pub(crate) struct MemorySessionKeys {
        keys: Arc<Mutex<HashMap<String, String>>>,
        failure: Option<String>,
    }

    impl MemorySessionKeys {
        pub(crate) fn with_key(username: &str, session_key: &str) -> Self {
            let store = Self::default();
            store
                .keys
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .insert(username.to_string(), session_key.to_string());
            store
        }

        pub(crate) fn failing(message: &str) -> Self {
            Self {
                failure: Some(message.to_string()),
                ..Self::default()
            }
        }

        fn check(&self) -> Result<(), String> {
            self.failure.clone().map_or(Ok(()), Err)
        }
    }

    impl SessionKeyStore for MemorySessionKeys {
        fn load(&self, username: &str) -> Result<Option<String>, String> {
            self.check()?;
            Ok(self
                .keys
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .get(username)
                .cloned())
        }

        fn store(&self, username: &str, session_key: &str) -> Result<(), String> {
            self.check()?;
            self.keys
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .insert(username.to_string(), session_key.to_string());
            Ok(())
        }

        fn delete(&self, username: &str) -> Result<(), String> {
            self.check()?;
            self.keys
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .remove(username);
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use super::test_support::{MemorySessionKeys, ScriptedTransport};
    use super::*;

    const USERNAME: &str = "tester";

    fn scratch_dir(name: &str) -> PathBuf {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let dir = crate::paths::test_root().join(format!(
            "lastfm-{name}-{}",
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create scratch directory");
        dir
    }

    fn entry(index: usize) -> ScrobbleEntry {
        ScrobbleEntry {
            artist: format!("Artist {index}"),
            track: format!("Track {index}"),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
            timestamp_utc: 1_700_000_000 + i64::try_from(index).unwrap_or(0),
        }
    }

    fn entries(range: std::ops::Range<usize>) -> Vec<ScrobbleEntry> {
        range.map(entry).collect()
    }

    fn reply(status: u16, body: &str) -> Result<HttpReply, String> {
        Ok(HttpReply {
            status,
            body: body.to_string(),
        })
    }

    /// A `track.scrobble` reply with one result per ignore code.
    fn scrobble_reply(ignore_codes: &[i64]) -> Result<HttpReply, String> {
        let results = ignore_codes
            .iter()
            .map(|code| {
                serde_json::json!({
                    "ignoredMessage": {
                        "code": code.to_string(),
                        "#text": if *code == 0 { "" } else { "Ignored for testing" },
                    },
                })
            })
            .collect::<Vec<_>>();
        reply(
            200,
            &serde_json::json!({ "scrobbles": { "scrobble": results } }).to_string(),
        )
    }

    fn accepted_reply(count: usize) -> Result<HttpReply, String> {
        scrobble_reply(&vec![0; count])
    }

    fn backends(transport: &ScriptedTransport, keys: MemorySessionKeys) -> ServiceBackends {
        ServiceBackends {
            transport: Box::new(transport.clone()),
            session_keys: Box::new(keys),
            credentials: Some(AppCredentials {
                api_key: "api-key".to_string(),
                shared_secret: "secret".to_string(),
            }),
            retry: RetryPolicy::DEFAULT,
        }
    }

    /// An enabled service with a stored session and `queued` pending.
    fn connected_service(
        transport: &ScriptedTransport,
        queue_path: Option<PathBuf>,
        queued: Vec<ScrobbleEntry>,
    ) -> Service {
        let mut service = Service::new(
            ServiceOptions {
                queue_path,
                initial_enabled: true,
            },
            backends(transport, MemorySessionKeys::with_key(USERNAME, "session")),
        );
        service.handle_command(Command::LoadStoredSession {
            username: USERNAME.to_string(),
        });
        assert_eq!(service.state.auth_state, AuthState::Connected);
        service.queue = queued;
        service
    }

    fn scrobble_requests(transport: &ScriptedTransport) -> Vec<Vec<(String, String)>> {
        transport
            .requests()
            .into_iter()
            .filter(|form| {
                form.iter()
                    .any(|(key, value)| key == "method" && value == "track.scrobble")
            })
            .collect()
    }

    fn batch_size(form: &[(String, String)]) -> usize {
        form.iter()
            .filter(|(key, _)| key.starts_with("artist["))
            .count()
    }

    fn read_queue_file(path: &Path) -> Vec<ScrobbleEntry> {
        serde_json::from_slice(&fs::read(path).expect("read queue file")).expect("parse queue")
    }

    #[test]
    fn scrobble_threshold_matches_lastfm_rule() {
        assert_eq!(scrobble_threshold_seconds(30), None);
        assert_eq!(scrobble_threshold_seconds(31), Some(15));
        assert_eq!(scrobble_threshold_seconds(400), Some(200));
        assert_eq!(scrobble_threshold_seconds(1000), Some(240));
    }

    #[test]
    fn api_signature_ignores_format_and_callback() {
        let params = vec![
            ("method".to_string(), "auth.getSession".to_string()),
            ("token".to_string(), "abc".to_string()),
            ("api_key".to_string(), "xyz".to_string()),
            ("format".to_string(), "json".to_string()),
            (
                "callback".to_string(),
                "https://example.invalid".to_string(),
            ),
        ];
        let left = api_signature(&params, "secret");
        let right = api_signature(
            &params
                .into_iter()
                .filter(|(key, _)| key != "format" && key != "callback")
                .collect::<Vec<_>>(),
            "secret",
        );
        assert_eq!(left, right);
    }

    #[test]
    fn normalize_album_for_submission_omits_missing_album_values() {
        assert!(normalize_album_for_submission("").is_empty());
        assert!(normalize_album_for_submission("   ").is_empty());
        assert!(normalize_album_for_submission("Unknown album").is_empty());
        assert!(normalize_album_for_submission("unknown ALBUM").is_empty());
    }

    #[test]
    fn normalize_album_for_submission_preserves_real_album_values() {
        assert_eq!(
            normalize_album_for_submission("Music Has the Right to Children"),
            "Music Has the Right to Children"
        );
    }

    #[test]
    fn transient_failures_keep_the_batch_and_schedule_a_retry() {
        let cases = [
            ("transport error", Err("connection reset".to_string())),
            (
                "5xx HTML page",
                reply(503, "<html><body>Service Unavailable</body></html>"),
            ),
            ("5xx without body", reply(502, "")),
            (
                "captive portal",
                reply(200, "<html><title>Sign in to Wi-Fi</title></html>"),
            ),
            ("JSON without scrobble results", reply(200, "{}")),
            (
                "operation failed",
                reply(200, r#"{"error":8,"message":"Operation failed"}"#),
            ),
            (
                "service offline",
                reply(200, r#"{"error":11,"message":"Service Offline"}"#),
            ),
            (
                "temporary error",
                reply(503, r#"{"error":16,"message":"Temporary error"}"#),
            ),
            (
                "rate limit",
                reply(429, r#"{"error":29,"message":"Rate limit exceeded"}"#),
            ),
        ];
        for (name, failure) in cases {
            let transport = ScriptedTransport::with_replies([failure]);
            let mut service = connected_service(&transport, None, entries(0..3));

            service.handle_command(Command::Flush);

            assert_eq!(service.queue, entries(0..3), "{name}");
            assert!(service.retry_due.is_some(), "{name}");
            assert_eq!(service.state.auth_state, AuthState::Connected, "{name}");
            assert_eq!(scrobble_requests(&transport).len(), 1, "{name}");
            let state = service.published_state();
            assert_eq!(state.pending_scrobble_count, 3, "{name}");
            assert!(
                state.status_text.contains("retrying in"),
                "{name}: {}",
                state.status_text
            );
        }
    }

    #[test]
    fn flush_stops_at_the_first_failed_batch() {
        let dir = scratch_dir("flush-stops");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([
            accepted_reply(MAX_SCROBBLES_PER_BATCH),
            reply(503, "<html>Bad gateway</html>"),
        ]);
        let mut service = connected_service(&transport, Some(queue_path.clone()), entries(0..120));

        service.handle_command(Command::Flush);

        let sizes = scrobble_requests(&transport)
            .iter()
            .map(|form| batch_size(form))
            .collect::<Vec<_>>();
        assert_eq!(sizes, vec![50, 50]);
        assert_eq!(service.queue, entries(50..120));
        assert_eq!(read_queue_file(&queue_path), entries(50..120));
        assert!(service.retry_due.is_some());
    }

    #[test]
    fn refused_requests_keep_the_batch_without_a_timer() {
        for (code, message) in [(6, "Invalid parameters"), (10, "Invalid API key")] {
            let transport = ScriptedTransport::with_replies([reply(
                400,
                &serde_json::json!({ "error": code, "message": message }).to_string(),
            )]);
            let mut service = connected_service(&transport, None, entries(0..120));

            service.handle_command(Command::Flush);

            assert_eq!(scrobble_requests(&transport).len(), 1, "code {code}");
            assert_eq!(service.queue, entries(0..120), "code {code}");
            assert!(service.retry_due.is_none(), "code {code}");
            let status = service.published_state().status_text;
            assert!(status.contains(message), "{status}");
            assert!(status.contains("Scrobbles stay queued"), "{status}");
        }
    }

    #[test]
    fn invalid_session_keeps_the_queue_and_requires_reconnecting() {
        let transport = ScriptedTransport::with_replies([reply(
            403,
            r#"{"error":9,"message":"Invalid session key"}"#,
        )]);
        let mut service = connected_service(&transport, None, entries(0..3));

        service.handle_command(Command::Flush);

        assert_eq!(service.state.auth_state, AuthState::ReauthRequired);
        assert_eq!(service.queue, entries(0..3));
        assert!(service.retry_due.is_none());
    }

    #[test]
    fn flush_drops_only_accepted_and_permanently_ignored_entries() {
        let dir = scratch_dir("flush-ignored");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[0, 1, 5])]);
        let mut service = connected_service(&transport, Some(queue_path.clone()), entries(0..3));

        service.handle_command(Command::Flush);

        // The entry deferred by the daily limit stays queued for a retry.
        assert_eq!(service.queue, vec![entry(2)]);
        assert_eq!(read_queue_file(&queue_path), vec![entry(2)]);
        assert!(service.retry_due.is_some());
        assert!(service
            .published_state()
            .status_text
            .contains("daily scrobble limit"));
    }

    #[test]
    fn permanently_ignored_entries_are_reported() {
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[0, 3])]);
        let mut service = connected_service(&transport, None, entries(0..2));

        service.handle_command(Command::Flush);

        assert!(service.queue.is_empty());
        assert!(service.retry_due.is_none());
        assert_eq!(
            service.published_state().status_text,
            "Last.fm ignored 1 scrobble: Ignored for testing"
        );
    }

    #[test]
    fn single_scrobble_result_object_is_matched_to_its_entry() {
        let deferred = r##"{"scrobbles":{"scrobble":{"ignoredMessage":{"code":"5","#text":""}}}}"##;
        let transport = ScriptedTransport::with_replies([reply(200, deferred)]);
        let mut service = connected_service(&transport, None, entries(0..1));

        service.handle_command(Command::Flush);

        assert_eq!(service.queue, entries(0..1));
        assert!(service.retry_due.is_some());
    }

    #[test]
    fn successful_flush_submits_batches_and_removes_the_queue_file() {
        let dir = scratch_dir("flush-success");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([
            accepted_reply(50),
            accepted_reply(50),
            accepted_reply(20),
        ]);
        let mut service = connected_service(&transport, Some(queue_path.clone()), entries(0..119));

        service.handle_command(Command::QueueScrobble(entry(119)));

        let sizes = scrobble_requests(&transport)
            .iter()
            .map(|form| batch_size(form))
            .collect::<Vec<_>>();
        assert_eq!(sizes, vec![50, 50, 20]);
        assert!(service.queue.is_empty());
        assert!(!queue_path.exists());
        assert!(service.retry_due.is_none());
        assert_eq!(
            service.published_state().status_text,
            "Last.fm scrobble queue flushed."
        );
    }

    #[test]
    fn retry_delay_doubles_up_to_the_limit() {
        let policy = RetryPolicy::DEFAULT;
        assert_eq!(policy.delay(1), Duration::from_secs(30));
        assert_eq!(policy.delay(2), Duration::from_secs(60));
        assert_eq!(policy.delay(3), Duration::from_secs(120));
        assert_eq!(policy.delay(7), Duration::from_secs(30 * 60));
        assert_eq!(policy.delay(u32::MAX), Duration::from_secs(30 * 60));
    }

    #[test]
    fn repeated_failures_back_off_until_a_batch_succeeds() {
        let transport = ScriptedTransport::with_replies([
            Err("offline".to_string()),
            Err("offline".to_string()),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, entries(0..1));

        service.handle_command(Command::Flush);
        service.handle_command(Command::Flush);
        let due = service.retry_due.expect("retry scheduled");
        assert_eq!(service.retry_attempt, 2);
        assert!(due >= Instant::now() + Duration::from_secs(50));
        assert!(service
            .published_state()
            .status_text
            .contains("retrying in 60 s"));

        service.handle_command(Command::Flush);
        assert!(service.queue.is_empty());
        assert_eq!(service.retry_attempt, 0);
        assert!(service.retry_due.is_none());
    }

    #[test]
    fn scheduled_retry_flushes_the_backlog_without_a_new_scrobble() {
        let transport =
            ScriptedTransport::with_replies([Err("offline".to_string()), accepted_reply(1)]);
        let mut backends = backends(&transport, MemorySessionKeys::with_key(USERNAME, "key"));
        backends.retry = RetryPolicy {
            initial: Duration::from_millis(20),
            max: Duration::from_millis(50),
        };
        let (handle, events) = spawn_with_backends(
            ServiceOptions {
                queue_path: None,
                initial_enabled: true,
            },
            move || backends,
        );
        handle.command(Command::LoadStoredSession {
            username: USERNAME.to_string(),
        });
        handle.command(Command::QueueScrobble(entry(0)));

        let deadline = Instant::now() + Duration::from_secs(5);
        let mut flushed = false;
        while let Ok(Event::State(state)) = events.recv_deadline(deadline) {
            if state.status_text == "Last.fm scrobble queue flushed." {
                assert_eq!(state.pending_scrobble_count, 0);
                flushed = true;
                break;
            }
        }
        assert!(flushed, "the retry timer did not flush the backlog");
        assert_eq!(scrobble_requests(&transport).len(), 2);
        assert_eq!(
            handle.shutdown(Duration::from_secs(1)),
            ShutdownOutcome::Stopped
        );
    }

    #[test]
    fn reaching_last_fm_again_flushes_a_waiting_backlog() {
        let transport = ScriptedTransport::with_replies([
            Err("offline".to_string()),
            reply(200, r#"{"nowplaying":{}}"#),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, entries(0..1));
        service.handle_command(Command::Flush);
        assert!(service.retry_due.is_some());

        service.handle_command(Command::SendNowPlaying(NowPlayingTrack {
            artist: "Artist".to_string(),
            track: "Track".to_string(),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
        }));

        assert!(service.queue.is_empty());
        assert!(service.retry_due.is_none());
    }

    #[test]
    fn truncated_queue_file_is_moved_aside_and_preserved() {
        let dir = scratch_dir("truncated-queue");
        let queue_path = dir.join("lastfm_queue.json");
        let truncated = br#"[{"artist":"Artist","track":"Track","album":"","#;
        fs::write(&queue_path, truncated).expect("write truncated queue");
        let disabled = || ServiceOptions {
            queue_path: Some(queue_path.clone()),
            initial_enabled: false,
        };
        let transport = ScriptedTransport::default();

        let mut service = Service::new(
            disabled(),
            backends(&transport, MemorySessionKeys::default()),
        );

        let kept = dir.join("lastfm_queue.json.corrupt");
        assert!(service.queue.is_empty());
        assert_eq!(fs::read(&kept).expect("kept copy"), truncated);
        assert!(service
            .published_state()
            .status_text
            .contains("could not be read"));

        service.handle_command(Command::QueueScrobble(entry(1)));
        assert_eq!(read_queue_file(&queue_path), vec![entry(1)]);
        assert_eq!(fs::read(&kept).expect("kept copy"), truncated);

        // A second unreadable file does not replace the first copy.
        fs::write(&queue_path, b"not json").expect("write second corrupt queue");
        let service = Service::new(
            disabled(),
            backends(&transport, MemorySessionKeys::default()),
        );
        assert!(service.queue.is_empty());
        assert_eq!(fs::read(&kept).expect("first copy"), truncated);
        assert_eq!(
            fs::read(dir.join("lastfm_queue.json.corrupt-1")).expect("second copy"),
            b"not json"
        );
    }

    #[test]
    fn unreadable_queue_file_is_never_overwritten() {
        let dir = scratch_dir("unreadable-queue");
        // A directory in place of the file fails to read but cannot be parsed
        // or replaced either.
        let queue_path = dir.join("lastfm_queue.json");
        fs::create_dir(&queue_path).expect("create directory at queue path");
        fs::write(queue_path.join("keep.txt"), b"keep").expect("write marker");
        let transport = ScriptedTransport::default();
        let mut service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.clone()),
                initial_enabled: false,
            },
            backends(&transport, MemorySessionKeys::default()),
        );

        service.handle_command(Command::QueueScrobble(entry(0)));

        let state = service.published_state();
        assert_eq!(state.pending_scrobble_count, 1);
        assert!(
            state.status_text.contains("could not be read"),
            "{}",
            state.status_text
        );
        assert!(queue_path.is_dir());
        assert_eq!(
            fs::read(queue_path.join("keep.txt")).expect("marker"),
            b"keep"
        );
    }

    #[test]
    fn queue_save_failure_is_shown_and_entries_stay_queued() {
        let dir = scratch_dir("save-failure");
        let blocker = dir.join("not-a-directory");
        fs::write(&blocker, b"").expect("write blocking file");
        let transport = ScriptedTransport::default();
        let mut service = Service::new(
            ServiceOptions {
                queue_path: Some(blocker.join("lastfm_queue.json")),
                initial_enabled: false,
            },
            backends(&transport, MemorySessionKeys::default()),
        );

        service.handle_command(Command::QueueScrobble(entry(0)));

        let state = service.published_state();
        assert_eq!(state.pending_scrobble_count, 1);
        assert!(
            state.status_text.contains("could not be saved"),
            "{}",
            state.status_text
        );
    }

    #[test]
    fn queue_save_replaces_the_file_without_leaving_a_temporary_file() {
        let dir = scratch_dir("atomic-save");
        let queue_path = dir.join("lastfm_queue.json");
        queue_file::save(&queue_path, &entries(0..2)).expect("first save");
        queue_file::save(&queue_path, &entries(0..3)).expect("second save");

        assert_eq!(read_queue_file(&queue_path), entries(0..3));
        let names = fs::read_dir(&dir)
            .expect("list directory")
            .map(|item| item.expect("entry").file_name())
            .collect::<Vec<_>>();
        assert_eq!(names, vec![std::ffi::OsString::from("lastfm_queue.json")]);
    }

    #[test]
    fn missing_keyring_entry_means_no_session_key() {
        assert_eq!(stored_session_key(Err(keyring::Error::NoEntry)), Ok(None));
        assert_eq!(stored_session_key(Ok("  ".to_string())), Ok(None));
        assert_eq!(
            stored_session_key(Ok("key".to_string())),
            Ok(Some("key".to_string()))
        );
        assert!(stored_session_key(Err(keyring::Error::NoStorageAccess("locked".into()))).is_err());
        assert_eq!(session_key_deleted(Err(keyring::Error::NoEntry)), Ok(()));
        assert!(
            session_key_deleted(Err(keyring::Error::NoStorageAccess("locked".into()))).is_err()
        );
    }

    #[test]
    fn missing_session_key_disconnects_instead_of_failing() {
        let transport = ScriptedTransport::default();
        let load = |keys| {
            let mut service = Service::new(ServiceOptions::default(), backends(&transport, keys));
            service.handle_command(Command::LoadStoredSession {
                username: USERNAME.to_string(),
            });
            service.state
        };

        let missing = load(MemorySessionKeys::default());
        assert_eq!(missing.auth_state, AuthState::Disconnected);
        assert_eq!(missing.username, USERNAME);

        let failing = load(MemorySessionKeys::failing("keyring locked"));
        assert_eq!(failing.auth_state, AuthState::Error);
        assert!(failing.status_text.contains("keyring locked"));
    }

    #[test]
    fn shutdown_saves_pending_scrobbles_without_network_requests() {
        let dir = scratch_dir("shutdown");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([accepted_reply(1)]);
        let service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.clone()),
                initial_enabled: true,
            },
            backends(&transport, MemorySessionKeys::with_key(USERNAME, "key")),
        );
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, _event_rx) = unbounded();
        let (done_tx, done_rx) = bounded(1);
        for command in [
            Command::LoadStoredSession {
                username: USERNAME.to_string(),
            },
            Command::QueueScrobble(entry(0)),
            Command::Shutdown(done_tx),
        ] {
            cmd_tx.send(command).expect("queue command");
        }

        run_service_loop(&cmd_rx, &event_tx, service);

        assert_eq!(done_rx.try_recv(), Ok(Ok(())));
        assert!(transport.requests().is_empty());
        assert_eq!(read_queue_file(&queue_path), vec![entry(0)]);
    }

    #[test]
    fn shutdown_saves_a_scrobble_queued_behind_a_request_that_does_not_answer() {
        let dir = scratch_dir("blocked-request");
        let queue_path = dir.join("lastfm_queue.json");
        let (transport, blocked) = test_support::BlockingTransport::new();
        let (handle, _events) = test_support::spawn_connected(transport, queue_path.clone());
        handle.command(Command::QueueScrobble(entry(0)));
        blocked
            .started
            .recv_timeout(Duration::from_secs(5))
            .expect("the scrobble is sent");
        // Waits behind the request.
        handle.command(Command::QueueScrobble(entry(1)));

        assert_eq!(
            handle.shutdown(Duration::from_secs(5)),
            ShutdownOutcome::Stopped
        );

        assert_eq!(read_queue_file(&queue_path), entries(0..2));
        assert_eq!(blocked.answered(), 0, "the request is still running");
        blocked.release.send(()).expect("release the request");
    }

    #[test]
    fn requests_after_shutdown_fail_without_reaching_the_transport() {
        let transport = ScriptedTransport::with_replies([accepted_reply(1)]);
        let cancellation = Arc::new(RequestCancellation::new());
        let cancellable = CancellableTransport {
            inner: Arc::new(transport.clone()),
            cancellation: Arc::clone(&cancellation),
        };
        cancellation.cancel();

        assert!(cancellable.post_form(API_ENDPOINT, &[]).is_err());
        assert!(cancellable.post_form(API_ENDPOINT, &[]).is_err());
        assert!(transport.requests().is_empty());
    }

    #[test]
    fn requests_before_shutdown_get_their_answers() {
        let transport = ScriptedTransport::with_replies([accepted_reply(1)]);
        let cancellable = CancellableTransport {
            inner: Arc::new(transport.clone()),
            cancellation: Arc::new(RequestCancellation::new()),
        };

        assert_eq!(cancellable.post_form(API_ENDPOINT, &[]), accepted_reply(1));
        assert_eq!(transport.requests().len(), 1);
    }

    #[test]
    fn shutdown_reports_a_service_that_is_gone() {
        let (handle, commands) = test_support::command_handle();
        drop(commands);
        assert_eq!(
            handle.shutdown(Duration::from_millis(10)),
            ShutdownOutcome::Unconfirmed
        );
    }

    #[test]
    fn malformed_scrobble_results_keep_the_batch() {
        let code = |code: &str| serde_json::json!({ "ignoredMessage": { "code": code } });
        let results = |results: Vec<Value>, attr: Option<Value>| {
            let mut scrobbles = serde_json::json!({ "scrobble": results });
            if let Some(attr) = attr {
                scrobbles["@attr"] = attr;
            }
            reply(
                200,
                &serde_json::json!({ "scrobbles": scrobbles }).to_string(),
            )
        };
        let cases = [
            ("no results", reply(200, r#"{"scrobbles":{}}"#)),
            ("short list", results(vec![code("0"), code("0")], None)),
            // The missing result could not be matched to the deferred entry.
            ("short list with a deferral", results(vec![code("5")], None)),
            (
                "result without a code",
                results(
                    vec![code("0"), code("0"), serde_json::json!({ "track": {} })],
                    None,
                ),
            ),
            (
                "accepted count disagrees",
                results(
                    vec![code("0"), code("0"), code("0")],
                    Some(serde_json::json!({ "accepted": 2, "ignored": 0 })),
                ),
            ),
            (
                "ignored count disagrees",
                results(
                    vec![code("0"), code("0"), code("1")],
                    Some(serde_json::json!({ "accepted": "2", "ignored": "0" })),
                ),
            ),
        ];
        for (name, malformed) in cases {
            let transport = ScriptedTransport::with_replies([malformed]);
            let mut service = connected_service(&transport, None, entries(0..3));

            service.handle_command(Command::Flush);

            assert_eq!(service.queue, entries(0..3), "{name}");
            assert!(service.retry_due.is_some(), "{name}");
            let status = service.published_state().status_text;
            assert!(status.contains("response error"), "{name}: {status}");
        }

        // Counts that agree with the results are accepted.
        let transport = ScriptedTransport::with_replies([results(
            vec![code("0"), code("1"), code("0")],
            Some(serde_json::json!({ "accepted": "2", "ignored": 1 })),
        )]);
        let mut service = connected_service(&transport, None, entries(0..3));
        service.handle_command(Command::Flush);
        assert!(service.queue.is_empty());
    }

    #[test]
    fn a_server_that_always_answers_malformed_is_retried_with_backoff() {
        let malformed = || reply(200, r#"{"scrobbles":{}}"#);
        let transport = ScriptedTransport::with_replies([malformed(), malformed(), malformed()]);
        let mut service = connected_service(&transport, None, entries(0..1));

        for attempt in 1..=3 {
            let before = Instant::now();
            service.handle_command(Command::Flush);
            assert_eq!(service.retry_attempt, attempt);
            let due = service.retry_due.expect("retry scheduled");
            assert!(due >= before + RetryPolicy::DEFAULT.delay(attempt));
        }
        assert_eq!(scrobble_requests(&transport).len(), 3);
        assert_eq!(service.queue, entries(0..1));
    }

    /// Sends commands to the service while it waits for a request, as if
    /// they arrived during the request. Each request sends the next group.
    struct InterruptingTransport {
        inner: ScriptedTransport,
        commands: Sender<Command>,
        during_requests: std::sync::Mutex<std::collections::VecDeque<Vec<Command>>>,
    }

    impl Transport for InterruptingTransport {
        fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
            let group = self
                .during_requests
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .pop_front();
            for command in group.into_iter().flatten() {
                self.commands.send(command).expect("send command");
            }
            self.inner.post_form(url, form)
        }
    }

    /// Runs a connected service with `queued` pending until it shuts down.
    /// Its first command is a flush, and requests send `during_requests`.
    fn run_interrupted_flush(
        transport: &ScriptedTransport,
        queue_path: &Path,
        queued: Vec<ScrobbleEntry>,
        during_requests: Vec<Vec<Command>>,
    ) -> Vec<RuntimeState> {
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, event_rx) = unbounded();
        let mut backends = backends(transport, MemorySessionKeys::with_key(USERNAME, "key"));
        backends.transport = Box::new(InterruptingTransport {
            inner: transport.clone(),
            commands: cmd_tx.clone(),
            during_requests: std::sync::Mutex::new(during_requests.into()),
        });
        let mut service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.to_path_buf()),
                initial_enabled: true,
            },
            backends,
        );
        service.handle_command(Command::LoadStoredSession {
            username: USERNAME.to_string(),
        });
        service.queue = queued;
        cmd_tx.send(Command::Flush).expect("queue flush");

        run_service_loop(&cmd_rx, &event_tx, service);

        event_rx
            .try_iter()
            .map(|Event::State(state)| state)
            .collect()
    }

    #[test]
    fn shutdown_during_a_multi_batch_flush_stops_after_the_current_batch() {
        let dir = scratch_dir("flush-shutdown");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([accepted_reply(50), accepted_reply(50)]);
        let (done_tx, done_rx) = bounded(1);

        run_interrupted_flush(
            &transport,
            &queue_path,
            entries(0..120),
            vec![vec![
                Command::QueueScrobble(entry(120)),
                Command::Shutdown(done_tx),
            ]],
        );

        assert_eq!(scrobble_requests(&transport).len(), 1);
        assert_eq!(done_rx.try_recv(), Ok(Ok(())));
        assert_eq!(read_queue_file(&queue_path), entries(50..121));
    }

    #[test]
    fn a_scrobble_during_a_multi_batch_flush_is_handled_before_the_next_batch() {
        let dir = scratch_dir("flush-new-scrobble");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([
            accepted_reply(50),
            accepted_reply(50),
            accepted_reply(21),
        ]);
        let (done_tx, done_rx) = bounded(1);

        let states = run_interrupted_flush(
            &transport,
            &queue_path,
            entries(0..120),
            vec![
                vec![Command::QueueScrobble(entry(120))],
                Vec::new(),
                vec![Command::Shutdown(done_tx)],
            ],
        );

        let sizes = scrobble_requests(&transport)
            .iter()
            .map(|form| batch_size(form))
            .collect::<Vec<_>>();
        // Handled after the flush, the new scrobble would need a fourth batch.
        assert_eq!(sizes, vec![50, 50, 21]);
        assert_eq!(
            states.last().map(|state| state.status_text.as_str()),
            Some("Last.fm scrobble queue flushed.")
        );
        assert_eq!(done_rx.try_recv(), Ok(Ok(())));
        assert!(!queue_path.exists());
    }

    #[test]
    fn shutdown_saves_a_queue_whose_last_save_failed() {
        let dir = scratch_dir("shutdown-unsaved");
        let config_dir = dir.join("config");
        let queue_path = config_dir.join("lastfm_queue.json");
        // A file in place of the directory makes saving fail.
        fs::write(&config_dir, b"").expect("write blocking file");
        let mut service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.clone()),
                initial_enabled: false,
            },
            backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );
        service.handle_command(Command::QueueScrobble(entry(0)));
        assert!(service.queue_save_error.is_some());

        // The directory is writable again, but no command has saved since.
        fs::remove_file(&config_dir).expect("remove blocking file");
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, _event_rx) = unbounded();
        let (done_tx, done_rx) = bounded(1);
        cmd_tx
            .send(Command::Shutdown(done_tx))
            .expect("queue shutdown");

        run_service_loop(&cmd_rx, &event_tx, service);

        assert_eq!(done_rx.try_recv(), Ok(Ok(())));
        assert_eq!(read_queue_file(&queue_path), vec![entry(0)]);
    }

    #[test]
    fn shutdown_reports_scrobbles_it_could_not_save() {
        let dir = scratch_dir("shutdown-save-failure");
        let config_dir = dir.join("config");
        fs::write(&config_dir, b"").expect("write blocking file");
        let (handle, _events) = spawn_with_backends(
            ServiceOptions {
                queue_path: Some(config_dir.join("lastfm_queue.json")),
                initial_enabled: false,
            },
            || backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );
        handle.command(Command::QueueScrobble(entry(0)));

        match handle.shutdown(Duration::from_secs(5)) {
            ShutdownOutcome::SaveFailed(message) => {
                assert!(message.contains("1 scrobble was lost"), "{message}");
                assert!(message.contains("could not be saved"), "{message}");
            }
            other => panic!("expected a save failure, got {other:?}"),
        }
    }

    #[test]
    fn backends_are_built_on_the_service_thread() {
        let (built_on_tx, built_on_rx) = bounded(1);
        let (handle, _events) = spawn_with_backends(ServiceOptions::default(), move || {
            let _ = built_on_tx.send(std::thread::current().name().map(str::to_owned));
            backends(&ScriptedTransport::default(), MemorySessionKeys::default())
        });

        assert_eq!(
            built_on_rx.recv_timeout(Duration::from_secs(5)),
            Ok(Some("ferrous-lastfm".to_string()))
        );
        assert_eq!(
            handle.shutdown(Duration::from_secs(5)),
            ShutdownOutcome::Stopped
        );
    }
}
