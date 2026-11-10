// SPDX-License-Identifier: GPL-3.0-or-later

mod queue_file;

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crossbeam_channel::{bounded, select, unbounded, Receiver, RecvTimeoutError, Sender};
use serde::{Deserialize, Serialize};
use serde_json::Value;

use queue_file::LoadOutcome;

use crate::frontend_bridge::{log_info, log_undelivered, log_warning, BridgeError, ErrorCategory};

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
/// The longest wait a `Retry-After` can ask for that is kept to; a longer
/// one is cut to this, so a bad header cannot stop scrobbling for good.
const MAX_REQUESTED_WAIT: Duration = Duration::from_secs(24 * 60 * 60);
/// `ignoredMessage` code for a scrobble refused because the account reached
/// its daily limit. Unlike the other ignore codes, it can succeed later.
const LASTFM_IGNORED_DAILY_LIMIT: i64 = 5;
/// How many tracks a line of the diagnostics log names; the rest are
/// counted.
const LOGGED_TRACKS: usize = 3;

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
    /// Whether queued scrobbles exist only in memory, because the queue
    /// file could not be read or saved, so quitting would lose them.
    pub unsaved_scrobbles: bool,
    /// How many [`Command::QueueScrobble`] the service has queued and tried
    /// to save. Published before the scrobble is sent to Last.fm, so it
    /// matches [`Handle::scrobbles_sent`] unless one waits behind a request.
    pub scrobbles_received: u64,
    /// What went wrong since the previous state, for the user. The status
    /// text shows the latest; these are shown once each.
    pub problems: Vec<Problem>,
    /// What became of the most recent [`Command::QueueScrobble`].
    pub latest_scrobble: Option<LatestScrobble>,
}

/// What became of the most recent scrobble the service received.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LatestScrobble {
    /// Its [`RuntimeState::scrobbles_received`] count: the `n`th scrobble
    /// sent, as [`Handle::queue_scrobble`] numbers them.
    pub ordinal: u64,
    pub delivery: ScrobbleDelivery,
    /// Why it is waiting or failed, for the user; empty otherwise.
    pub message: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum ScrobbleDelivery {
    /// Queued and not yet answered, or held while scrobbling is paused or
    /// Last.fm is not connected.
    #[default]
    Pending,
    /// Last.fm could not be reached, or deferred it; it stays queued and is
    /// sent again later.
    Waiting,
    Accepted,
    /// Last.fm ignored it, or refused the request; the message says which.
    Failed,
}

/// A Last.fm failure the user is told about in Recent Issues: one the user
/// must act on, or one that loses or refuses scrobbles. Temporary failures,
/// which the service retries, are only logged.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Problem {
    pub message: String,
    /// Whether the service works around it, for example by keeping the
    /// scrobbles in memory.
    pub warning: bool,
}

impl Problem {
    /// The error Recent Issues shows for it.
    pub(crate) fn into_bridge_error(self) -> BridgeError {
        if self.warning {
            BridgeError::warning(ErrorCategory::LastFm, self.message)
        } else {
            BridgeError::error(ErrorCategory::LastFm, self.message)
        }
    }
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
    /// [`Handle::set_paused`] changed the pause; sent by it. Resuming sends
    /// the scrobbles that waited.
    PauseChanged,
    /// Save scrobbles that exist only in memory, before the app quits, and
    /// answer with [`Event::SavedForQuit`].
    SaveForQuit(u32),
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
    /// The answer to [`Command::SaveForQuit`] with that request ID: what
    /// quitting would lose, if the queue still could not be saved.
    SavedForQuit {
        request_id: u32,
        result: Result<(), String>,
    },
}

#[derive(Debug, Clone)]
pub struct Handle {
    tx: Sender<Command>,
    requests: Arc<RequestCancellation>,
    /// How many [`Command::QueueScrobble`] reached the service's channel.
    scrobbles_sent: Arc<AtomicU64>,
    /// Whether scrobbling is paused; see [`Handle::set_paused`].
    paused: Arc<AtomicBool>,
}

impl Handle {
    fn new(tx: Sender<Command>) -> Self {
        Self {
            tx,
            requests: Arc::new(RequestCancellation::new()),
            scrobbles_sent: Arc::new(AtomicU64::new(0)),
            paused: Arc::new(AtomicBool::new(false)),
        }
    }

    /// Pause or resume scrobbling for this run of the app. While paused,
    /// nothing new is sent to Last.fm: neither now playing updates nor
    /// queued scrobbles, which stay queued and are sent on resuming. The
    /// service sees the pause before it starts its next request, even with
    /// commands queued before this call; a request already under way may
    /// finish.
    pub fn set_paused(&self, paused: bool) {
        self.paused.store(paused, Ordering::SeqCst);
        self.command(Command::PauseChanged);
    }

    pub fn command(&self, command: Command) {
        let scrobble = matches!(command, Command::QueueScrobble(_));
        if self.tx.send(command).is_ok() && scrobble {
            self.scrobbles_sent.fetch_add(1, Ordering::SeqCst);
        }
    }

    /// Sends `entry` to be queued and returns its ordinal, which
    /// [`LatestScrobble::ordinal`] reports, or `None` if the service is
    /// gone. The ordinals match while one thread sends the scrobbles.
    #[must_use]
    pub fn queue_scrobble(&self, entry: ScrobbleEntry) -> Option<u64> {
        self.tx.send(Command::QueueScrobble(entry)).ok()?;
        Some(self.scrobbles_sent.fetch_add(1, Ordering::SeqCst) + 1)
    }

    /// Whether scrobbling is paused; see [`Self::set_paused`].
    #[must_use]
    pub fn is_paused(&self) -> bool {
        self.paused.load(Ordering::SeqCst)
    }

    /// How many scrobbles were sent to the service. Those beyond
    /// [`RuntimeState::scrobbles_received`] are not saved yet.
    #[must_use]
    pub fn scrobbles_sent(&self) -> u64 {
        self.scrobbles_sent.load(Ordering::SeqCst)
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
    /// How long the reply's `Retry-After` asks to wait before the next
    /// request, if it has one that can be read; see [`parse_retry_after`].
    pub(crate) retry_after: Option<Duration>,
    /// Why the body could not be read, if it could not, for example
    /// because the connection closed before its end; `body` is empty then.
    /// The status and `Retry-After` still say most.
    pub(crate) body_error: Option<String>,
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

struct HttpTransport {
    client: reqwest::blocking::Client,
}

impl HttpTransport {
    #[cfg(not(test))]
    fn new() -> Self {
        let client = reqwest::blocking::Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()
            .unwrap_or_else(|_| reqwest::blocking::Client::new());
        Self { client }
    }
}

impl Transport for HttpTransport {
    fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
        let response = self
            .client
            .post(url)
            .form(form)
            .send()
            .map_err(|err| err.to_string())?;
        let status = response.status().as_u16();
        let retry_after = response
            .headers()
            .get(reqwest::header::RETRY_AFTER)
            .and_then(|value| value.to_str().ok())
            .and_then(|value| parse_retry_after(value, std::time::SystemTime::now()));
        // A body cut off keeps the status and `Retry-After`, so a rate
        // limit's wait is kept to whatever came after them.
        let (body, body_error) = match response.text() {
            Ok(body) => (body, None),
            Err(err) => (String::new(), Some(err.to_string())),
        };
        Ok(HttpReply {
            status,
            body,
            retry_after,
            body_error,
        })
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
    /// Set by a temporary failure, until a request goes through again with
    /// nothing left to retry: what Last.fm answered for the scrobbles sent
    /// meanwhile, for the line that logs the recovery.
    recovering: Option<Answered>,
    /// Until when Last.fm asked for no requests, and why; see
    /// [`asks_to_wait`]. Only what the user asks for, such as reconnecting,
    /// is sent before then.
    hold: Option<Hold>,
    /// Cleared while handling the commands that precede shutdown, so that
    /// shutdown does not wait for the network.
    network_allowed: bool,
    /// The service's own command channel. A flush checks it between batches
    /// so that waiting commands, such as shutdown, go first.
    waiting_commands: Option<Receiver<Command>>,
    /// Entries ignored since a flush last completed, reported when one does.
    flush_ignored: IgnoredScrobbles,
    /// How many of the queue's first entries the diagnostics log has
    /// already listed as sent. Sending them again, as a retry does, is
    /// logged without listing them, so that an outage does not flood the
    /// log.
    listed_scrobbles: usize,
    queue_path: Option<PathBuf>,
    queue: Vec<ScrobbleEntry>,
    /// Problem found while loading the queue file, shown for the session.
    queue_load_notice: Option<String>,
    /// Why the queue file could not be read at startup. Saving stays off
    /// while this is set, so that a file that may still hold scrobbles is
    /// not replaced, and every queued scrobble exists only in memory.
    queue_unreadable: Option<String>,
    /// Most recent failure to save the queue, until a save succeeds.
    queue_save_error: Option<String>,
    session: Option<Session>,
    pending_auth: Option<PendingAuth>,
    state: RuntimeState,
    /// Failures not yet published; see [`RuntimeState::problems`].
    problems: Vec<Problem>,
    /// Whether [`RuntimeState::latest_scrobble`] is still queued. Queued, it
    /// is the queue's last entry: entries are only appended, and leave the
    /// queue in order.
    latest_queued: bool,
    /// Whether scrobbling is paused for this run of the app, shared with
    /// the [`Handle`], which sets it; see [`Handle::set_paused`]. Never
    /// saved.
    paused: Arc<AtomicBool>,
    /// The authentication state the diagnostics log last told; see
    /// [`Service::log_auth_change`].
    logged_auth_state: AuthState,
}

#[derive(Debug)]
enum RequestError {
    Transport(String),
    /// An unsuccessful HTTP status without a Last.fm error code.
    Http {
        status: u16,
        retry_after: Option<Duration>,
    },
    Api {
        code: i64,
        message: String,
        retry_after: Option<Duration>,
    },
    InvalidResponse(String),
    /// A reply whose body could not be read, for example because the
    /// connection closed before its end. Its `Retry-After` still counts.
    UnreadableReply {
        status: u16,
        error: String,
        retry_after: Option<Duration>,
    },
    Keyring(String),
}

/// What Last.fm answered for the scrobbles sent since a temporary failure.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct Answered {
    accepted: usize,
    ignored: usize,
}

/// A wait Last.fm asked for; see [`Service::hold`].
#[derive(Debug, Clone, PartialEq, Eq)]
struct Hold {
    until: Instant,
    /// The failure that asked for it, for the latest scrobble's state.
    message: String,
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
    /// Per entry, in request order: why Last.fm ignored it for good, if it
    /// did.
    ignored_reasons: Vec<Option<String>>,
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

/// The album sent with a track: empty, and so left out of the request,
/// when the track has none.
#[must_use]
pub(crate) fn normalize_album_for_submission(album: &str) -> String {
    album.trim().to_owned()
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
    let paused = Arc::clone(&handle.paused);
    crate::worker_thread::spawn_worker("ferrous-lastfm", move || {
        let mut backends = backends();
        backends.transport = Box::new(CancellableTransport {
            inner: Arc::from(backends.transport),
            cancellation,
        });
        let mut service = Service::new(options, backends);
        service.paused = paused;
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
            match command {
                Command::Shutdown(_) => break,
                Command::SaveForQuit(request_id) => {
                    let result = service.save_for_quit();
                    service.publish(event_tx);
                    let _ = event_tx.send(Event::SavedForQuit { request_id, result });
                }
                Command::QueueScrobble(entry) => {
                    // Whether the scrobble is saved is told before it is
                    // sent, which can wait for the network.
                    service.queue_scrobble(entry);
                    service.publish(event_tx);
                    service.flush_queue();
                    service.publish(event_tx);
                }
                command => {
                    service.handle_command(command);
                    service.publish(event_tx);
                }
            }
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
            recovering: None,
            hold: None,
            network_allowed: true,
            waiting_commands: None,
            flush_ignored: IgnoredScrobbles::default(),
            listed_scrobbles: 0,
            queue_path: options.queue_path,
            queue: Vec::new(),
            queue_load_notice: None,
            queue_unreadable: None,
            queue_save_error: None,
            session: None,
            pending_auth: None,
            problems: Vec::new(),
            latest_queued: false,
            paused: Arc::new(AtomicBool::new(false)),
            logged_auth_state: AuthState::Disconnected,
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
                let notice = format!(
                    "Saved scrobbles could not be read; the file was kept as {}.",
                    moved_to.display()
                );
                self.report(&notice, true);
                self.queue_load_notice = Some(notice);
            }
            LoadOutcome::Unavailable { error } => {
                eprintln!(
                    "Last.fm: cannot load scrobble queue {}: {error}",
                    path.display()
                );
                // Saving now would replace a file that may still hold scrobbles.
                self.queue_unreadable = Some(error.clone());
                let notice = format!(
                    "Saved scrobbles in {} could not be read ({error}). New scrobbles \
                     are kept in memory only; Ferrous tries to save them again when it quits.",
                    path.display()
                );
                self.report(&notice, false);
                self.queue_load_notice = Some(notice);
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
        state.unsaved_scrobbles = self.queue_save_error.is_some()
            || (self.queue_unreadable.is_some() && !self.queue.is_empty());
        state
    }

    /// Send the state, with the problems not sent yet, which Recent Issues
    /// shows and logs. When nothing receives it any more, they are logged
    /// here instead.
    fn publish(&mut self, event_tx: &Sender<Event>) {
        self.log_auth_change();
        let mut state = self.published_state();
        state.problems = std::mem::take(&mut self.problems);
        if let Err(unsent) = event_tx.send(Event::State(state)) {
            if let Event::State(state) = unsent.into_inner() {
                for problem in state.problems {
                    log_undelivered(&problem.into_bridge_error());
                }
            }
        }
    }

    /// Log the authentication state once it changed, one line per change.
    /// The line never holds the session key, the token, or the address of
    /// the browser's approval page, which has the token.
    fn log_auth_change(&mut self) {
        let now = self.state.auth_state;
        if now == self.logged_auth_state {
            return;
        }
        self.logged_auth_state = now;
        let line = match now {
            AuthState::Disconnected => "Authentication: disconnected.",
            AuthState::AwaitingBrowserApproval => {
                "Authentication: waiting for approval in the browser."
            }
            AuthState::Connected => "Authentication: connected.",
            AuthState::ReauthRequired => {
                "Authentication: Last.fm no longer accepts the session; reconnecting is needed."
            }
            AuthState::Error => "Authentication: failed; the reason is in Recent Issues.",
        };
        log_info(ErrorCategory::LastFm, line);
    }

    /// Tell the user about `message` with the next state; see
    /// [`RuntimeState::problems`].
    fn report(&mut self, message: &str, warning: bool) {
        self.problems.push(Problem {
            message: message.to_string(),
            warning,
        });
    }

    /// Show `message` as the status, and tell the user about it.
    fn fail(&mut self, message: String, warning: bool) {
        self.report(&message, warning);
        self.state.status_text = message;
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
                self.queue_scrobble(entry);
                self.flush_queue();
            }
            // A pause is already in effect when its PauseChanged arrives;
            // resuming sends what waited.
            Command::Flush | Command::PauseChanged => self.flush_queue(),
            // The service loop answers these itself.
            Command::SaveForQuit(_) | Command::Shutdown(_) => {}
        }
    }

    /// Queue `entry` and save the queue.
    fn queue_scrobble(&mut self, entry: ScrobbleEntry) {
        self.queue.push(entry);
        self.save_queue();
        self.state.scrobbles_received += 1;
        self.state.latest_scrobble = Some(LatestScrobble {
            ordinal: self.state.scrobbles_received,
            delivery: ScrobbleDelivery::Pending,
            message: String::new(),
        });
        self.latest_queued = true;
    }

    /// Records what became of the latest scrobble, if it is still queued,
    /// and whether it still is.
    fn set_latest_delivery(&mut self, delivery: ScrobbleDelivery, message: &str, queued: bool) {
        if !self.latest_queued {
            return;
        }
        if let Some(latest) = self.state.latest_scrobble.as_mut() {
            latest.delivery = delivery;
            latest.message = message.to_string();
        }
        self.latest_queued = queued;
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
                self.fail(request_error_message(&err), false);
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
                    self.fail(request_error_message(&RequestError::Keyring(err)), false);
                    return;
                }
                self.session = Some(session.clone());
                self.pending_auth = None;
                self.state.username = session.username;
                self.state.auth_state = AuthState::Connected;
                self.state.auth_url.clear();
                self.state.status_text = "Last.fm account connected.".to_string();
                self.retry_attempt = 0;
                // The user asked for this, so it need not wait for a rate
                // limit: the queue is sent now.
                self.hold = None;
                self.flush_queue();
            }
            Err(err) => {
                self.state.auth_state = AuthState::Error;
                self.fail(request_error_message(&err), false);
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
            // The log is shared in bug reports, so it never names the
            // account.
            Ok(None) => {
                log_warning(
                    ErrorCategory::LastFm,
                    "No session key was found in the keyring for the saved account. Was the \
                     keyring cleared, or is the keyring daemon unavailable?",
                );
                self.state.username = username;
                self.state.auth_state = AuthState::Disconnected;
                self.fail("Reconnect Last.fm to resume scrobbling.".to_string(), true);
            }
            Err(err) => {
                // The keyring's own error may name its entry, the account.
                let message = request_error_message(&RequestError::Keyring(err))
                    .replace(username.as_str(), "the saved account");
                log_warning(
                    ErrorCategory::LastFm,
                    &format!("The saved account's session key could not be read: {message}"),
                );
                self.state.username = username;
                self.state.auth_state = AuthState::Error;
                self.fail(message, false);
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
                self.report(&status_text, true);
            }
        }
        self.session = None;
        self.pending_auth = None;
        self.retry_attempt = 0;
        self.retry_due = None;
        self.recovering = None;
        self.state.username.clear();
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::Disconnected;
        self.state.status_text = status_text;
        if clear_queue {
            self.queue.clear();
            self.listed_scrobbles = 0;
            self.latest_queued = false;
            self.save_queue();
        }
    }

    fn send_now_playing(&mut self, track: &NowPlayingTrack) {
        if !self.state.enabled || self.is_paused() || !self.network_allowed {
            return;
        }
        let holding = self.holding();
        let Some(credentials) = self.credentials.as_ref() else {
            return;
        };
        let Some(session) = self.session.as_ref() else {
            return;
        };
        if track.artist.trim().is_empty() || track.track.trim().is_empty() {
            return;
        }
        if holding {
            log_warning(
                ErrorCategory::LastFm,
                "The now-playing update was not sent: Last.fm asked Ferrous to wait.",
            );
            return;
        }
        let logged = logged_track(&track.artist, &track.track);
        log_info(
            ErrorCategory::LastFm,
            &format!("Sending now playing: {logged}."),
        );
        match send_now_playing_request(self.transport.as_ref(), credentials, session, track) {
            Ok(ignored) => {
                // The failures are logged where they are handled.
                let result = match ignored {
                    None => format!("Last.fm accepted now playing: {logged}."),
                    Some(reason) => format!("Last.fm ignored now playing: {logged} ({reason})."),
                };
                log_info(ErrorCategory::LastFm, &result);
                self.state.status_text = format!("Now playing: {} - {}", track.artist, track.track);
                // Last.fm is reachable again; do not wait for the scheduled retry.
                if self.retry_due.is_some() {
                    self.flush_queue();
                } else {
                    self.log_recovery();
                }
            }
            Err(err) if is_invalid_session(&err) => {
                self.transition_to_reauth(
                    "Last.fm session expired. Reconnect to resume scrobbling.",
                );
            }
            // It would be stale by the time a retry went through, so it is
            // dropped; the user need not act on it.
            Err(err) if is_transient(&err) => {
                let message = request_error_message(&err);
                let logged = match self.hold_after(&err, &message) {
                    Some(wait) => format!(
                        "{message}; the now-playing update was not sent, and nothing is sent \
                         for {}.",
                        describe_delay(wait)
                    ),
                    None => format!("{message}; the now-playing update was not sent."),
                };
                log_warning(ErrorCategory::LastFm, &logged);
                self.recovering.get_or_insert_with(Answered::default);
                self.state.status_text = message;
            }
            Err(err) => {
                self.fail(request_error_message(&err), true);
            }
        }
    }

    /// Whether scrobbling is paused now. Read before each request, so that a
    /// pause stops the requests after the one under way, whatever commands
    /// are queued before it.
    fn is_paused(&self) -> bool {
        self.paused.load(Ordering::SeqCst)
    }

    /// Submits queued scrobbles in batches. Stops at the first batch that
    /// fails and keeps it queued; only entries that Last.fm accepted or
    /// ignored for good leave the queue. Transient failures schedule a retry.
    /// Between batches, commands that are waiting go first; the flush then
    /// resumes.
    fn flush_queue(&mut self) {
        if self.holding() {
            // What is queued waits for the end of the wait Last.fm asked
            // for, when the service loop flushes again.
            if let Some(hold) = self.hold.clone() {
                if !self.queue.is_empty() {
                    self.retry_due = Some(hold.until);
                    self.set_latest_delivery(ScrobbleDelivery::Waiting, &hold.message, true);
                }
            }
            return;
        }
        self.retry_due = None;
        if !self.state.enabled || self.is_paused() || !self.network_allowed || self.queue.is_empty()
        {
            return;
        }
        let (Some(credentials), Some(session)) = (self.credentials.clone(), self.session.clone())
        else {
            return;
        };

        while !self.queue.is_empty() {
            // A pause stops the flush before its next batch; resuming
            // flushes again.
            if self.is_paused() {
                return;
            }
            let batch_len = self.queue.len().min(MAX_SCROBBLES_PER_BATCH);
            log_info(
                ErrorCategory::LastFm,
                &scrobbles_sent_line(
                    &self.queue[..batch_len],
                    self.listed_scrobbles.min(batch_len),
                ),
            );
            self.listed_scrobbles = self.listed_scrobbles.max(batch_len);
            // A failure is logged with its retry, or in Recent Issues.
            let result = send_scrobble_batch(
                self.transport.as_ref(),
                &credentials,
                &session,
                &self.queue[..batch_len],
            );
            match result {
                Ok(outcome) => {
                    if !self.take_batch_outcome(batch_len, outcome) {
                        return;
                    }
                }
                Err(err) if is_invalid_session(&err) => {
                    let message = "Last.fm session expired. Reconnect to flush queued scrobbles.";
                    self.set_latest_delivery(ScrobbleDelivery::Failed, message, true);
                    self.transition_to_reauth(message);
                    return;
                }
                Err(err) if is_transient(&err) => {
                    let message = request_error_message(&err);
                    self.set_latest_delivery(ScrobbleDelivery::Waiting, &message, true);
                    self.schedule_retry(&message, Some(&err));
                    return;
                }
                Err(err) => {
                    // Last.fm refused the request itself, for example because
                    // the API key is invalid. That says nothing about these
                    // scrobbles, so they stay queued for the next flush.
                    let message =
                        format!("{}. Scrobbles stay queued.", request_error_message(&err));
                    self.set_latest_delivery(ScrobbleDelivery::Failed, &message, true);
                    self.fail(message, false);
                    return;
                }
            }
        }
        self.log_recovery();
        let ignored = std::mem::take(&mut self.flush_ignored);
        match ignored.reason {
            None => self.state.status_text = "Last.fm scrobble queue flushed.".to_string(),
            Some(reason) => self.fail(
                format!(
                    "Last.fm ignored {} {}: {reason}",
                    ignored.count,
                    if ignored.count == 1 {
                        "scrobble"
                    } else {
                        "scrobbles"
                    }
                ),
                true,
            ),
        }
    }

    /// Takes Last.fm's answer to the batch of the queue's first
    /// `batch_len` entries. Returns whether the flush goes on now.
    fn take_batch_outcome(&mut self, batch_len: usize, outcome: ScrobbleOutcome) -> bool {
        log_info(
            ErrorCategory::LastFm,
            &scrobbles_answered_line(&self.queue[..batch_len], &outcome),
        );
        // The latest scrobble is the queue's last entry.
        if batch_len == self.queue.len() {
            let last = batch_len - 1;
            if outcome.deferred[last] {
                self.set_latest_delivery(
                    ScrobbleDelivery::Waiting,
                    "Last.fm daily scrobble limit reached",
                    true,
                );
            } else if let Some(reason) = &outcome.ignored_reasons[last] {
                self.set_latest_delivery(ScrobbleDelivery::Failed, reason, false);
            } else {
                self.set_latest_delivery(ScrobbleDelivery::Accepted, "", false);
            }
        }
        if let Some(answered) = self.recovering.as_mut() {
            let deferred = outcome
                .deferred
                .iter()
                .filter(|deferred| **deferred)
                .count();
            answered.ignored += outcome.ignored;
            answered.accepted += batch_len - deferred - outcome.ignored;
        }
        let mut index = 0;
        self.queue.retain(|_| {
            let keep = index >= batch_len || outcome.deferred[index];
            index += 1;
            keep
        });
        // The deferred entries, all listed, now lead the queue.
        let deferred = outcome
            .deferred
            .iter()
            .filter(|deferred| **deferred)
            .count();
        self.listed_scrobbles = deferred + self.listed_scrobbles.saturating_sub(batch_len);
        self.save_queue();
        self.flush_ignored.count += outcome.ignored;
        if self.flush_ignored.reason.is_none() {
            self.flush_ignored.reason = outcome.ignored_reason;
        }
        if outcome.deferred.contains(&true) {
            // The latest scrobble, in this batch or behind it,
            // waits for the retry too.
            self.set_latest_delivery(
                ScrobbleDelivery::Waiting,
                "Last.fm daily scrobble limit reached",
                true,
            );
            self.schedule_retry("Last.fm daily scrobble limit reached", None);
            return false;
        }
        self.retry_attempt = 0;
        if !self.queue.is_empty() && self.commands_waiting() {
            // The service loop handles the waiting commands, then
            // resumes the flush at once.
            self.retry_due = Some(Instant::now());
            return false;
        }
        true
    }

    fn commands_waiting(&self) -> bool {
        self.waiting_commands
            .as_ref()
            .is_some_and(|commands| !commands.is_empty())
    }

    /// Retry the flush after a temporary failure, `message`, which `error`
    /// caused if a request failed. The scrobbles stay queued until Last.fm
    /// takes them, so the user need not act: the failure is logged, and the
    /// status shows when the next retry is. The retry waits as long as the
    /// reply's `Retry-After` asks, and after a rate limit nothing but what
    /// the user asks for is sent before then.
    fn schedule_retry(&mut self, message: &str, error: Option<&RequestError>) {
        self.retry_attempt = self.retry_attempt.saturating_add(1);
        let delay = error
            .and_then(requested_wait)
            .unwrap_or_else(|| self.retry_policy.delay(self.retry_attempt));
        let due = Instant::now() + delay;
        self.retry_due = Some(due);
        if error.is_some_and(asks_to_wait) {
            self.hold = Some(Hold {
                until: due,
                message: message.to_string(),
            });
        }
        self.recovering.get_or_insert_with(Answered::default);
        let status = format!("{message}; retrying in {}.", describe_delay(delay));
        log_warning(ErrorCategory::LastFm, &status);
        self.state.status_text = status;
    }

    /// Hold back requests after a request other than a flush failed with
    /// `error`, if it asks to wait; see [`asks_to_wait`]. Returns the wait.
    /// The queue, if any, is flushed again once it is over.
    fn hold_after(&mut self, error: &RequestError, message: &str) -> Option<Duration> {
        if !asks_to_wait(error) {
            return None;
        }
        let wait = requested_wait(error).unwrap_or_else(|| {
            self.retry_policy
                .delay(self.retry_attempt.saturating_add(1))
        });
        let until = Instant::now() + wait;
        self.hold = Some(Hold {
            until,
            message: message.to_string(),
        });
        if !self.queue.is_empty() {
            self.retry_due = Some(self.retry_due.map_or(until, |due| due.max(until)));
        }
        Some(wait)
    }

    /// Whether a wait Last.fm asked for is still going on. Forgets one that
    /// is over.
    fn holding(&mut self) -> bool {
        if self
            .hold
            .as_ref()
            .is_some_and(|hold| Instant::now() < hold.until)
        {
            return true;
        }
        self.hold = None;
        false
    }

    /// Log, once, that requests go through again after a temporary failure,
    /// with what Last.fm answered for the scrobbles sent since.
    fn log_recovery(&mut self) {
        let Some(Answered { accepted, ignored }) = self.recovering.take() else {
            return;
        };
        let scrobbles = |count: usize| {
            if count == 1 {
                "1 queued scrobble".to_string()
            } else {
                format!("{count} queued scrobbles")
            }
        };
        let message = match (accepted, ignored) {
            (0, 0) => "Last.fm is reachable again.".to_string(),
            (accepted, 0) => {
                format!(
                    "Last.fm is reachable again; it accepted {}.",
                    scrobbles(accepted)
                )
            }
            (accepted, ignored) => format!(
                "Last.fm is reachable again; it accepted {} and ignored {ignored}.",
                scrobbles(accepted)
            ),
        };
        log_info(ErrorCategory::LastFm, &message);
    }

    fn transition_to_reauth(&mut self, message: &str) {
        self.session = None;
        self.pending_auth = None;
        self.retry_attempt = 0;
        self.retry_due = None;
        self.recovering = None;
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::ReauthRequired;
        self.fail(message.to_string(), false);
    }

    /// [`Self::try_save_unsaved_queue`], as the service stops: fails with
    /// the scrobbles that are lost.
    fn save_unsaved_queue(&mut self) -> Result<(), String> {
        self.try_save_unsaved_queue().map_err(|problem| {
            format!(
                "Last.fm: {} lost on quit. {problem}",
                self.unsaved_scrobbles_were()
            )
        })
    }

    /// [`Self::try_save_unsaved_queue`], before the app quits: fails with
    /// what quitting would lose, so that the user can retry or quit anyway.
    fn save_for_quit(&mut self) -> Result<(), String> {
        self.try_save_unsaved_queue().map_err(|problem| {
            format!(
                "Last.fm: {} not saved and would be lost on quit. {problem}",
                self.unsaved_scrobbles_were()
            )
        })
    }

    fn unsaved_scrobbles_were(&self) -> String {
        format!(
            "{} {}",
            self.queue.len(),
            if self.queue.len() == 1 {
                "scrobble was"
            } else {
                "scrobbles were"
            }
        )
    }

    /// Saves the queue if it is not on disk: after a failed save, or while
    /// the queue file is unreadable. Every change to the queue is saved, so
    /// it differs from the file only then. Fails with why the scrobbles
    /// that exist only in memory still cannot be saved.
    fn try_save_unsaved_queue(&mut self) -> Result<(), String> {
        if self.queue_unreadable.is_some() {
            self.save_beside_unreadable_queue();
        } else if self.queue_save_error.is_some() {
            self.save_queue();
        }
        let problem = match (&self.queue_unreadable, &self.queue_save_error) {
            (_, Some(error)) => error.clone(),
            (Some(error), None) if !self.queue.is_empty() => format!(
                "Saved scrobbles in {} could not be read ({error}), so new ones were not saved.",
                self.queue_path
                    .as_deref()
                    .map_or_else(String::new, |path| path.display().to_string())
            ),
            _ => return Ok(()),
        };
        Err(problem)
    }

    /// Reads the queue file that could not be read at startup again, and
    /// saves the scrobbles held in memory after the ones it holds. The file
    /// is replaced only once its contents were read or moved aside, so
    /// nothing in it is lost. While it is still unreadable, the scrobbles
    /// stay in memory only.
    fn save_beside_unreadable_queue(&mut self) {
        if self.queue.is_empty() {
            return;
        }
        let Some(path) = self.queue_path.clone() else {
            return;
        };
        let mut merged = match queue_file::load(&path) {
            LoadOutcome::Loaded(saved) => saved,
            LoadOutcome::MovedAside { moved_to, reason } => {
                eprintln!(
                    "Last.fm: scrobble queue {} is not valid ({reason}); moved it to {}",
                    path.display(),
                    moved_to.display()
                );
                Vec::new()
            }
            LoadOutcome::Unavailable { error } => {
                eprintln!(
                    "Last.fm: still cannot load scrobble queue {}: {error}",
                    path.display()
                );
                self.queue_unreadable = Some(error);
                return;
            }
        };
        merged.extend_from_slice(&self.queue);
        match queue_file::save(&path, &merged) {
            Ok(()) => {
                self.queue = merged;
                // The saved scrobbles now come first, and were never
                // listed; the ones in memory are listed again with them.
                self.listed_scrobbles = 0;
                self.queue_unreadable = None;
                self.queue_save_error = None;
            }
            // The file was read but not replaced, so only the scrobbles in
            // memory are unsaved.
            Err(err) => self.record_save_error(&path, &err),
        }
    }

    fn save_queue(&mut self) {
        if self.queue_unreadable.is_some() {
            return;
        }
        let Some(path) = self.queue_path.clone() else {
            return;
        };
        match queue_file::save(&path, &self.queue) {
            Ok(()) => self.queue_save_error = None,
            Err(err) => self.record_save_error(&path, &err),
        }
    }

    fn record_save_error(&mut self, path: &Path, err: &std::io::Error) {
        let error = format!(
            "Pending scrobbles could not be saved to {} ({err}).",
            path.display()
        );
        // A save that keeps failing the same way is told once, and Recent
        // Issues logs it then; the failures after it are only logged.
        if self.queue_save_error.as_ref() == Some(&error) {
            log_warning(ErrorCategory::LastFm, &format!("{error} Again."));
        } else {
            self.report(&error, false);
        }
        self.queue_save_error = Some(error);
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

/// Sends `track` as now playing. Returns why Last.fm ignored it, if it did.
fn send_now_playing_request(
    transport: &dyn Transport,
    credentials: &AppCredentials,
    session: &Session,
    track: &NowPlayingTrack,
) -> Result<Option<String>, RequestError> {
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
    let payload = send_signed_request(transport, credentials, "track.updateNowPlaying", params)?;
    Ok(payload
        .get("nowplaying")
        .and_then(|nowplaying| nowplaying.get("ignoredMessage"))
        .and_then(ignored_reason))
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
        ignored_reasons: vec![None; batch_len],
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
                let reason = ignored_message
                    .and_then(ignored_reason)
                    .unwrap_or_else(|| format!("ignore code {code}"));
                if outcome.ignored_reason.is_none() {
                    outcome.ignored_reason = Some(reason.clone());
                }
                outcome.ignored_reasons[index] = Some(reason);
            }
        }
    }
    Ok(outcome)
}

/// Why Last.fm ignored a scrobble or a now-playing update, from its
/// `ignoredMessage`; `None` for one it accepted, with code 0.
fn ignored_reason(ignored_message: &Value) -> Option<String> {
    let code = ignored_message.get("code").and_then(json_integer)?;
    if code == 0 {
        return None;
    }
    let text = ignored_message
        .get("#text")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|text| !text.is_empty());
    Some(text.map_or_else(|| format!("ignore code {code}"), ToString::to_string))
}

/// `artist – title`, as the diagnostics log names a track.
fn logged_track(artist: &str, title: &str) -> String {
    format!("{artist} – {title}")
}

/// The first [`LOGGED_TRACKS`] of `items`, then how many more there are.
fn logged_list(items: impl ExactSizeIterator<Item = String>) -> String {
    let count = items.len();
    let mut listed = items.take(LOGGED_TRACKS).collect::<Vec<_>>();
    if count > LOGGED_TRACKS {
        listed.push(format!("+{} more", count - LOGGED_TRACKS));
    }
    listed.join("; ")
}

/// `timestamp`, seconds since the epoch, as `2023-11-14T22:13:20Z`.
fn logged_utc_time(timestamp: i64) -> String {
    let of_day = timestamp.rem_euclid(86_400);
    let (year, month, day) = crate::library::civil_from_days(timestamp.div_euclid(86_400));
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}Z",
        of_day / 3_600,
        of_day % 3_600 / 60,
        of_day % 60
    )
}

fn scrobbles_count(count: usize) -> String {
    if count == 1 {
        "1 scrobble".to_string()
    } else {
        format!("{count} scrobbles")
    }
}

/// The diagnostics log's line for sending `batch` to Last.fm, whose first
/// `listed` entries an earlier line listed. It names the other tracks and
/// when they were played, and nothing of the request itself, whose session
/// key and signature must not be logged.
fn scrobbles_sent_line(batch: &[ScrobbleEntry], listed: usize) -> String {
    let count = scrobbles_count(batch.len());
    let new = &batch[listed.min(batch.len())..];
    if new.is_empty() {
        return format!("Sending {count} again.");
    }
    let tracks = logged_list(new.iter().map(|entry| {
        format!(
            "{} at {}",
            logged_track(&entry.artist, &entry.track),
            logged_utc_time(entry.timestamp_utc)
        )
    }));
    if listed == 0 {
        format!("Sending {count}: {tracks}.")
    } else {
        format!("Sending {count}, {listed} of them again: {tracks}.")
    }
}

/// The diagnostics log's line for Last.fm's answer, `outcome`, to `batch`.
fn scrobbles_answered_line(batch: &[ScrobbleEntry], outcome: &ScrobbleOutcome) -> String {
    let deferred = outcome
        .deferred
        .iter()
        .filter(|deferred| **deferred)
        .count();
    let accepted = batch.len() - deferred - outcome.ignored;
    let mut line = if accepted == batch.len() {
        format!("Last.fm accepted {}", scrobbles_count(accepted))
    } else {
        format!(
            "Last.fm accepted {accepted} of {}",
            scrobbles_count(batch.len())
        )
    };
    let mut parts = Vec::new();
    if outcome.ignored > 0 {
        let ignored = batch
            .iter()
            .zip(&outcome.ignored_reasons)
            .filter_map(|(entry, reason)| {
                reason.as_ref().map(|reason| {
                    format!("{}: {reason}", logged_track(&entry.artist, &entry.track))
                })
            })
            .collect::<Vec<_>>();
        parts.push(format!(
            "ignored {} ({})",
            outcome.ignored,
            logged_list(ignored.into_iter())
        ));
    }
    if deferred > 0 {
        parts.push(format!(
            "deferred {deferred} (daily scrobble limit reached)"
        ));
    }
    if let Some((last, rest)) = parts.split_last() {
        for part in rest {
            line.push_str(", ");
            line.push_str(part);
        }
        line.push_str(" and ");
        line.push_str(last);
    }
    line.push('.');
    line
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

/// Last.fm reports API errors as JSON, sometimes with a 4xx status, and its
/// error code says most. Otherwise the HTTP status decides, whatever the
/// body: an unsuccessful one is an HTTP error, so a 403 with an HTML page is
/// as permanent as one with no body. A successful status with a body that is
/// not JSON, such as a captive portal's page, is an invalid response, and one
/// whose body cannot be read is an unreadable reply that keeps the reply's
/// `Retry-After`.
fn interpret_reply(reply: &HttpReply) -> Result<Value, RequestError> {
    let payload = reply
        .body_error
        .is_none()
        .then(|| serde_json::from_str::<Value>(&reply.body).ok())
        .flatten();
    if let Some(payload) = &payload {
        if let Some(code) = payload.get("error").and_then(json_integer) {
            let message = payload
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("Last.fm request failed")
                .to_string();
            return Err(RequestError::Api {
                code,
                message,
                retry_after: reply.retry_after,
            });
        }
    }
    if !(200..300).contains(&reply.status) {
        return Err(RequestError::Http {
            status: reply.status,
            retry_after: reply.retry_after,
        });
    }
    payload.ok_or_else(|| match &reply.body_error {
        Some(error) => RequestError::UnreadableReply {
            status: reply.status,
            error: error.clone(),
            retry_after: reply.retry_after,
        },
        None => RequestError::InvalidResponse(format!(
            "unexpected non-JSON reply (HTTP {})",
            reply.status
        )),
    })
}

/// How long a `Retry-After` header with `value` asks to wait, at `now`:
/// whole seconds, or an HTTP date in the IMF-fixdate form, such as
/// `Sun, 06 Nov 1994 08:49:37 GMT`, which senders must use. A date that has
/// passed asks for no wait. Anything else is not read.
fn parse_retry_after(value: &str, now: std::time::SystemTime) -> Option<Duration> {
    let value = value.trim();
    if !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit()) {
        return Some(Duration::from_secs(value.parse().unwrap_or(u64::MAX)));
    }
    let at = std::time::UNIX_EPOCH + Duration::from_secs(parse_imf_fixdate(value)?);
    Some(at.duration_since(now).unwrap_or(Duration::ZERO))
}

/// The seconds since the Unix epoch of an IMF-fixdate.
fn parse_imf_fixdate(value: &str) -> Option<u64> {
    const MONTHS: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ];
    let (_weekday, rest) = value.split_once(", ")?;
    let fields: Vec<&str> = rest.split(' ').collect();
    let [day, month, year, time, "GMT"] = fields.as_slice() else {
        return None;
    };
    let number = |text: &str, digits: usize| {
        (text.len() == digits && text.bytes().all(|byte| byte.is_ascii_digit()))
            .then(|| text.parse::<u64>().ok())
            .flatten()
    };
    let day = number(day, 2)?;
    let month = MONTHS.iter().position(|name| name == month)? as u64 + 1;
    let year = number(year, 4)?;
    let [hour, minute, second] = time.split(':').collect::<Vec<_>>()[..] else {
        return None;
    };
    let (hour, minute, second) = (number(hour, 2)?, number(minute, 2)?, number(second, 2)?);
    if !(1..=31).contains(&day) || hour > 23 || minute > 59 || second > 60 || year < 1970 {
        return None;
    }
    // Days from the epoch to the date, counting years from March so that
    // the leap day comes last.
    let (year, month) = if month <= 2 {
        (year - 1, month + 9)
    } else {
        (year, month - 3)
    };
    let era = year / 400;
    let year_of_era = year % 400;
    let day_of_year = (153 * month + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    let days = (era * 146_097 + day_of_era).checked_sub(719_468)?;
    Some(days * 86_400 + hour * 3_600 + minute * 60 + second)
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

/// Whether `error` asks for no requests for a while: Last.fm's rate limit,
/// or a temporary failure with a `Retry-After`. Until the wait is over,
/// nothing is sent but what the user asks for.
fn asks_to_wait(error: &RequestError) -> bool {
    matches!(error, RequestError::Http { status: 429, .. })
        || matches!(
            error,
            RequestError::Api {
                code: LASTFM_ERROR_RATE_LIMIT,
                ..
            }
        )
        || (is_transient(error) && requested_wait(error).is_some())
}

/// The wait the reply that failed with `error` asked for, at most
/// [`MAX_REQUESTED_WAIT`].
fn requested_wait(error: &RequestError) -> Option<Duration> {
    match error {
        RequestError::Http { retry_after, .. }
        | RequestError::Api { retry_after, .. }
        | RequestError::UnreadableReply { retry_after, .. } => {
            retry_after.map(|wait| wait.min(MAX_REQUESTED_WAIT))
        }
        _ => None,
    }
}

/// Whether a failure is expected to clear up by itself, so that a flush
/// should be retried on a timer.
fn is_transient(error: &RequestError) -> bool {
    match error {
        RequestError::Transport(_)
        | RequestError::InvalidResponse(_)
        | RequestError::UnreadableReply { .. } => true,
        RequestError::Http { status, .. } => is_temporary_status(*status),
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

/// Whether an HTTP status says the server cannot answer now but may later:
/// a server error, or too many requests.
fn is_temporary_status(status: u16) -> bool {
    status >= 500 || status == 429
}

fn request_error_message(error: &RequestError) -> String {
    match error {
        RequestError::Transport(message) => format!("Last.fm transport error: {message}"),
        RequestError::Http { status, .. } => format!("Last.fm HTTP error {status}"),
        RequestError::Api { code, message, .. } => format!("Last.fm API error {code}: {message}"),
        RequestError::InvalidResponse(message) => format!("Last.fm response error: {message}"),
        RequestError::UnreadableReply { status, error, .. } => {
            format!("Last.fm response error: could not read the reply (HTTP {status}): {error}")
        }
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

    /// The parameters of one request.
    type RequestParams = Vec<(String, String)>;

    /// Answers requests with scripted replies, in order, and records them.
    #[derive(Clone, Default)]
    pub(crate) struct ScriptedTransport {
        replies: Arc<Mutex<VecDeque<Result<HttpReply, String>>>>,
        requests: Arc<Mutex<Vec<RequestParams>>>,
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
    use crate::frontend_bridge::test_log;

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
            retry_after: None,
            body_error: None,
        })
    }

    /// A reply with a `Retry-After` that asks to wait `seconds`.
    fn reply_retry_after(status: u16, body: &str, seconds: u64) -> Result<HttpReply, String> {
        Ok(HttpReply {
            status,
            body: body.to_string(),
            retry_after: Some(Duration::from_secs(seconds)),
            body_error: None,
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
            ("429 without a Last.fm error", reply(429, "{}")),
            (
                "429 HTML page",
                reply(429, "<html>Too Many Requests</html>"),
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
            // Recent Issues is not told; the diagnostics log is.
            assert!(
                service.problems.is_empty(),
                "{name}: {:?}",
                service.problems
            );
            let logged = format!("[ferrous] [lastfm] warning: {}", state.status_text);
            assert!(
                !test_log::lines_containing(&logged).is_empty(),
                "{name}: {logged}"
            );
        }
    }

    #[test]
    fn temporary_failures_are_logged_until_the_scrobbles_go_through() {
        let transport = ScriptedTransport::with_replies([
            reply(503, "<html>Service Unavailable</html>"),
            reply(503, "<html>Service Unavailable</html>"),
            accepted_reply(2),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, entries(0..2));
        let recovered = "[ferrous] [lastfm] info: Last.fm is reachable again";

        // Each failure is logged with the next retry's delay, and none
        // reaches Recent Issues, also after the first of the run.
        service.handle_command(Command::Flush);
        service.handle_command(Command::Flush);
        assert!(service.problems.is_empty(), "{:?}", service.problems);
        for delay in ["30 s", "60 s"] {
            let line =
                format!("[ferrous] [lastfm] warning: Last.fm HTTP error 503; retrying in {delay}.");
            assert_eq!(test_log::lines_containing(&line).len(), 1, "{line}");
        }
        assert_eq!(service.queue, entries(0..2));
        assert!(test_log::lines_containing(recovered).is_empty());

        // The retry sends them, and the recovery is logged once, with them.
        service.handle_command(Command::Flush);
        assert!(service.queue.is_empty());
        assert!(service.retry_due.is_none());
        assert!(service.problems.is_empty(), "{:?}", service.problems);
        assert_eq!(
            test_log::lines_containing(recovered),
            vec![format!("{recovered}; it accepted 2 queued scrobbles.")]
        );
        service.handle_command(Command::QueueScrobble(entry(2)));
        assert!(service.queue.is_empty());
        assert_eq!(test_log::lines_containing(recovered).len(), 1);
    }

    #[test]
    fn a_recovery_tells_the_accepted_scrobbles_from_the_ignored_ones() {
        let transport = ScriptedTransport::with_replies([
            Err("offline".to_string()),
            scrobble_reply(&[0, 1, 0]),
        ]);
        let mut service = connected_service(&transport, None, entries(0..3));

        service.handle_command(Command::Flush);
        service.handle_command(Command::Flush);

        assert!(service.queue.is_empty());
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] info: Last.fm is reachable again"),
            vec![
                "[ferrous] [lastfm] info: Last.fm is reachable again; it accepted 2 queued \
                 scrobbles and ignored 1."
                    .to_string()
            ]
        );
        // The ignored one is still told to the user.
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        assert!(service.problems[0].message.contains("ignored 1 scrobble"));
    }

    #[test]
    fn unsuccessful_statuses_without_last_fm_json_are_permanent_whatever_the_body() {
        for (status, body) in [
            (401, "<html><body>Unauthorized</body></html>"),
            (403, "\u{0}garbage{"),
            (404, ""),
        ] {
            let transport = ScriptedTransport::with_replies([reply(status, body)]);
            let mut service = connected_service(&transport, None, entries(0..2));

            service.handle_command(Command::Flush);

            // Kept for the next flush, but not retried on a timer, and the
            // user is told.
            assert_eq!(service.queue, entries(0..2), "{status}");
            assert!(service.retry_due.is_none(), "{status}");
            assert_eq!(
                service.problems.len(),
                1,
                "{status}: {:?}",
                service.problems
            );
            assert!(!service.problems[0].warning, "{status}");
            assert!(
                service.problems[0]
                    .message
                    .contains(&format!("Last.fm HTTP error {status}")),
                "{:?}",
                service.problems
            );

            let transport = ScriptedTransport::with_replies([reply(status, body)]);
            let mut service = connected_service(&transport, None, Vec::new());
            service.handle_command(Command::SendNowPlaying(now_playing_track()));
            assert_eq!(
                service.problems,
                vec![Problem {
                    message: format!("Last.fm HTTP error {status}"),
                    warning: true,
                }],
                "{status}"
            );
        }
        // Successful, server-error and too-many-requests statuses with such
        // a body are still temporary.
        for (status, body) in [
            (200, "<html>Sign in</html>"),
            (502, "<html>"),
            (429, "slow"),
        ] {
            let error = interpret_reply(&HttpReply {
                status,
                body: body.to_string(),
                retry_after: None,
                body_error: None,
            })
            .expect_err("not a reply");
            assert!(is_transient(&error), "{status}: {error:?}");
        }
    }

    /// Ends the wait Last.fm asked `service` for, as time passing would.
    fn end_hold(service: &mut Service) {
        let past = Instant::now() - Duration::from_millis(1);
        service.hold.as_mut().expect("a hold").until = past;
        service.retry_due = Some(past);
    }

    #[test]
    fn a_rate_limit_holds_back_every_automatic_request_until_its_retry_after() {
        let transport = ScriptedTransport::with_replies([
            reply_retry_after(429, "<html>Too Many Requests</html>", 120),
            accepted_reply(3),
        ]);
        let mut service = connected_service(&transport, None, entries(0..2));

        service.handle_command(Command::Flush);
        let due = service.retry_due.expect("a retry");
        assert!(due >= Instant::now() + Duration::from_secs(110));
        assert_eq!(service.hold.as_ref().map(|hold| hold.until), Some(due));
        assert!(service
            .published_state()
            .status_text
            .contains("retrying in 2 min"));

        // A new scrobble queues behind the wait, and neither it, a flush, a
        // resume nor a now-playing update sends anything.
        service.handle_command(Command::QueueScrobble(entry(2)));
        service.handle_command(Command::Flush);
        set_paused(&mut service, true);
        set_paused(&mut service, false);
        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        assert_eq!(transport.requests().len(), 1);
        assert_eq!(service.queue, entries(0..3));
        assert_eq!(service.retry_due, Some(due));
        assert_eq!(latest(&service).1, ScrobbleDelivery::Waiting);
        assert!(service.problems.is_empty(), "{:?}", service.problems);

        // Once it is over, the retry sends the queue.
        end_hold(&mut service);
        service.flush_queue();
        assert!(service.queue.is_empty());
        assert_eq!(scrobble_requests(&transport).len(), 2);
        assert_eq!(batch_size(&scrobble_requests(&transport)[1]), 3);
        assert_eq!(service.hold, None);
    }

    #[test]
    fn last_fm_s_rate_limit_code_holds_back_requests_for_the_backoff_delay() {
        let transport = ScriptedTransport::with_replies([reply(
            200,
            r#"{"error":29,"message":"Rate limit exceeded"}"#,
        )]);
        let mut service = connected_service(&transport, None, entries(0..1));

        service.handle_command(Command::Flush);
        let due = service.retry_due.expect("a retry");
        assert!(due >= Instant::now() + Duration::from_secs(25));
        assert_eq!(service.hold.as_ref().map(|hold| hold.until), Some(due));
        service.handle_command(Command::QueueScrobble(entry(1)));
        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        assert_eq!(transport.requests().len(), 1);
        assert_eq!(service.queue, entries(0..2));
    }

    #[test]
    fn a_rate_limited_now_playing_update_holds_back_the_queue() {
        let transport = ScriptedTransport::with_replies([reply_retry_after(
            503,
            r#"{"error":16,"message":"Temporary error"}"#,
            600,
        )]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::QueueScrobble(entry(0)));

        assert_eq!(transport.requests().len(), 1);
        assert_eq!(service.queue, entries(0..1));
        let due = service
            .retry_due
            .expect("the queue is flushed after the wait");
        assert!(due >= Instant::now() + Duration::from_secs(590));
        assert!(service.problems.is_empty(), "{:?}", service.problems);
        assert_eq!(
            test_log::lines_containing(
                "[ferrous] [lastfm] warning: Last.fm API error 16: Temporary error; the \
                 now-playing update was not sent, and nothing is sent for 10 min."
            )
            .len(),
            1
        );
    }

    #[test]
    fn a_wait_longer_than_a_day_is_cut_to_a_day() {
        let transport =
            ScriptedTransport::with_replies([reply_retry_after(429, "", 3 * 24 * 60 * 60)]);
        let mut service = connected_service(&transport, None, entries(0..1));

        service.handle_command(Command::Flush);

        let due = service.retry_due.expect("a retry");
        assert!(due <= Instant::now() + MAX_REQUESTED_WAIT);
        assert!(due >= Instant::now() + MAX_REQUESTED_WAIT - Duration::from_secs(10));
    }

    #[test]
    fn authentication_changes_are_logged_once_each_without_secrets() {
        crate::frontend_bridge::test_log::clear();
        let transport = ScriptedTransport::with_replies([
            reply(200, r#"{"token":"fixture-token-123"}"#),
            reply(
                200,
                r#"{"session":{"name":"tester","key":"fixture-session-key-456"}}"#,
            ),
        ]);
        let mut service = Service::new(
            ServiceOptions {
                queue_path: None,
                initial_enabled: true,
            },
            backends(&transport, MemorySessionKeys::default()),
        );
        let (event_tx, _event_rx) = crossbeam_channel::unbounded();
        service.publish(&event_tx);
        service.handle_command(Command::BeginDesktopAuth);
        assert_eq!(service.state.auth_state, AuthState::AwaitingBrowserApproval);
        service.publish(&event_tx);
        service.publish(&event_tx);
        service.handle_command(Command::CompleteDesktopAuth);
        assert_eq!(service.state.auth_state, AuthState::Connected);
        service.publish(&event_tx);
        service.handle_command(Command::Disconnect { clear_queue: false });
        service.publish(&event_tx);

        let lines = crate::frontend_bridge::test_log::lines_containing("Authentication");
        assert_eq!(
            lines,
            [
                "waiting for approval in the browser.",
                "connected.",
                "disconnected.",
            ]
            .map(|state| format!("[ferrous] [lastfm] info: Authentication: {state}"))
        );
        for line in crate::frontend_bridge::test_log::all() {
            for secret in [
                "fixture-token-123",
                "fixture-session-key-456",
                "secret",
                "api-key",
            ] {
                assert!(!line.contains(secret), "{secret} in {line}");
            }
        }
    }

    #[test]
    fn reconnecting_sends_the_queue_during_a_rate_limit() {
        let transport = ScriptedTransport::with_replies([
            reply_retry_after(429, "", 3600),
            reply(200, r#"{"session":{"name":"tester","key":"new-key"}}"#),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, entries(0..1));
        service.handle_command(Command::Flush);
        assert!(service.hold.is_some());

        // The user asked for it, so it does not wait.
        service.pending_auth = Some(PendingAuth {
            token: "token".to_string(),
            auth_url: String::new(),
        });
        service.handle_command(Command::CompleteDesktopAuth);

        assert_eq!(service.state.auth_state, AuthState::Connected);
        assert!(service.queue.is_empty());
        assert_eq!(scrobble_requests(&transport).len(), 2);
    }

    /// Serves one request on loopback with `response`, then closes the
    /// connection. Returns the URL to post to.
    fn serve_once(response: &'static [u8]) -> (String, std::thread::JoinHandle<()>) {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let url = format!("http://{}/2.0/", listener.local_addr().expect("address"));
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("a request");
            // Read the request's head and body before answering.
            let mut request = Vec::new();
            let mut chunk = [0u8; 4096];
            loop {
                let read = stream.read(&mut chunk).expect("read the request");
                request.extend_from_slice(&chunk[..read]);
                let text = String::from_utf8_lossy(&request);
                if let Some(head_end) = text.find("\r\n\r\n") {
                    let length = text[..head_end]
                        .lines()
                        .find_map(|line| {
                            let (name, value) = line.split_once(':')?;
                            name.eq_ignore_ascii_case("content-length")
                                .then(|| value.trim().parse::<usize>().ok())
                                .flatten()
                        })
                        .unwrap_or(0);
                    if request.len() >= head_end + 4 + length {
                        break;
                    }
                }
                if read == 0 {
                    break;
                }
            }
            stream.write_all(response).expect("answer");
            // Closing here cuts the body short of its Content-Length.
        });
        (url, server)
    }

    /// Posts to a loopback server that answers with `response`, and returns
    /// the reply as [`HttpTransport`] reads it.
    fn loopback_reply(response: &'static [u8]) -> HttpReply {
        let (url, server) = serve_once(response);
        let transport = HttpTransport {
            client: reqwest::blocking::Client::builder()
                .no_proxy()
                .timeout(Duration::from_secs(10))
                .build()
                .expect("client"),
        };

        let reply = transport
            .post_form(
                &url,
                &[("method".to_string(), "track.scrobble".to_string())],
            )
            .expect("the status and headers arrived");
        server.join().expect("server");
        reply
    }

    #[test]
    fn a_rate_limit_whose_body_is_cut_off_still_holds_back_requests() {
        let reply = loopback_reply(
            b"HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n\
              Content-Type: application/json\r\nContent-Length: 200\r\n\
              Connection: close\r\n\r\n{\"error\":29,\"mess",
        );

        assert_eq!(reply.status, 429);
        assert_eq!(reply.retry_after, Some(Duration::from_secs(120)));
        assert!(reply.body_error.is_some(), "{reply:?}");
        assert!(reply.body.is_empty());

        // The service keeps to the wait it asked for.
        let scripted = ScriptedTransport::with_replies([Ok(reply)]);
        let mut service = connected_service(&scripted, None, entries(0..1));
        service.handle_command(Command::Flush);
        let hold = service.hold.clone().expect("a hold");
        assert!(hold.until >= Instant::now() + Duration::from_secs(110));
        assert_eq!(service.retry_due, Some(hold.until));
        service.handle_command(Command::QueueScrobble(entry(1)));
        assert_eq!(scripted.requests().len(), 1);
        assert!(service.problems.is_empty(), "{:?}", service.problems);
    }

    #[test]
    fn a_successful_reply_whose_body_is_cut_off_keeps_its_retry_after() {
        let reply = loopback_reply(
            b"HTTP/1.1 200 OK\r\nRetry-After: 120\r\n\
              Content-Type: application/json\r\nContent-Length: 200\r\n\
              Connection: close\r\n\r\n{\"scrobbles\":{\"@attr",
        );

        assert_eq!(reply.status, 200);
        assert_eq!(reply.retry_after, Some(Duration::from_secs(120)));
        assert!(reply.body_error.is_some(), "{reply:?}");
        assert!(reply.body.is_empty());

        // The flush waits as long as the reply asked, and nothing automatic
        // is sent before then: not a new scrobble, a flush nor a now-playing
        // update.
        let scripted = ScriptedTransport::with_replies([Ok(reply), accepted_reply(2)]);
        let mut service = connected_service(&scripted, None, entries(0..1));
        service.handle_command(Command::Flush);
        let hold = service.hold.clone().expect("a hold");
        assert!(hold.until >= Instant::now() + Duration::from_secs(110));
        assert_eq!(service.retry_due, Some(hold.until));
        service.handle_command(Command::QueueScrobble(entry(1)));
        service.handle_command(Command::Flush);
        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        assert_eq!(scripted.requests().len(), 1);
        assert_eq!(service.queue, entries(0..2));
        assert!(service.problems.is_empty(), "{:?}", service.problems);

        // Once it is over, the retry sends the queue.
        end_hold(&mut service);
        service.flush_queue();
        assert!(service.queue.is_empty());
        assert_eq!(scrobble_requests(&scripted).len(), 2);
        assert_eq!(service.hold, None);
    }

    #[test]
    fn a_successful_reply_whose_body_is_cut_off_is_retried() {
        let error = interpret_reply(&HttpReply {
            status: 200,
            body: String::new(),
            retry_after: None,
            body_error: Some("connection closed".to_string()),
        })
        .expect_err("nothing to read");
        assert!(is_transient(&error));
        assert_eq!(
            request_error_message(&error),
            "Last.fm response error: could not read the reply (HTTP 200): connection closed"
        );
    }

    #[test]
    fn retry_after_is_read_as_seconds_or_as_an_http_date() {
        let now = std::time::UNIX_EPOCH + Duration::from_secs(784_111_777); // Sun, 06 Nov 1994 08:49:37 GMT
        assert_eq!(
            parse_retry_after("120", now),
            Some(Duration::from_secs(120))
        );
        assert_eq!(parse_retry_after(" 0 ", now), Some(Duration::ZERO));
        assert_eq!(
            parse_retry_after("Sun, 06 Nov 1994 08:51:37 GMT", now),
            Some(Duration::from_secs(120))
        );
        assert_eq!(
            parse_retry_after("Tue, 01 Mar 2000 00:00:00 GMT", std::time::UNIX_EPOCH),
            Some(Duration::from_secs(951_868_800))
        );
        // A date that has passed asks for no wait.
        assert_eq!(
            parse_retry_after("Sat, 05 Nov 1994 08:49:37 GMT", now),
            Some(Duration::ZERO)
        );
        for unreadable in [
            "",
            "-5",
            "1.5",
            "soon",
            "Sunday, 06-Nov-94 08:49:37 GMT",
            "Sun, 06 Nov 1994 08:49:37 UTC",
            "Sun, 32 Nov 1994 08:49:37 GMT",
            "Sun, 06 Foo 1994 08:49:37 GMT",
        ] {
            assert_eq!(parse_retry_after(unreadable, now), None, "{unreadable}");
        }
    }

    #[test]
    fn a_now_playing_update_that_fails_for_now_is_only_logged() {
        let transport = ScriptedTransport::with_replies([
            reply(502, ""),
            Err("timed out".to_string()),
            reply(200, r#"{"nowplaying":{}}"#),
        ]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::SendNowPlaying(now_playing_track()));

        assert!(service.problems.is_empty(), "{:?}", service.problems);
        // The status shows the latest; the log has both.
        assert_eq!(
            service.published_state().status_text,
            "Last.fm transport error: timed out"
        );
        for message in [
            "Last.fm HTTP error 502",
            "Last.fm transport error: timed out",
        ] {
            let line = format!(
                "[ferrous] [lastfm] warning: {message}; the now-playing update was not sent."
            );
            assert_eq!(test_log::lines_containing(&line).len(), 1, "{line}");
        }
        // Not sent again: it would be stale.
        assert_eq!(transport.requests().len(), 2);
        assert_eq!(service.state.auth_state, AuthState::Connected);

        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] info: Last.fm is reachable again."),
            vec!["[ferrous] [lastfm] info: Last.fm is reachable again.".to_string()]
        );
    }

    #[test]
    fn a_now_playing_update_last_fm_refuses_is_reported() {
        let transport = ScriptedTransport::with_replies([reply(
            400,
            r#"{"error":6,"message":"Invalid parameters"}"#,
        )]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));

        assert_eq!(
            service.problems,
            vec![Problem {
                message: "Last.fm API error 6: Invalid parameters".to_string(),
                warning: true,
            }]
        );
    }

    #[test]
    fn a_now_playing_update_with_an_expired_session_asks_to_reconnect() {
        let transport = ScriptedTransport::with_replies([reply(
            403,
            r#"{"error":9,"message":"Invalid session key"}"#,
        )]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));

        assert_eq!(service.state.auth_state, AuthState::ReauthRequired);
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        assert!(!service.problems[0].warning);
        assert!(service.problems[0].message.contains("Reconnect"));
    }

    /// What became of the latest scrobble.
    fn latest(service: &Service) -> (u64, ScrobbleDelivery, String) {
        let latest = service
            .published_state()
            .latest_scrobble
            .expect("a latest scrobble");
        (latest.ordinal, latest.delivery, latest.message)
    }

    fn now_playing_track() -> NowPlayingTrack {
        NowPlayingTrack {
            artist: "Artist 0".to_string(),
            track: "Track 0".to_string(),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
        }
    }

    #[test]
    fn the_latest_scrobble_is_reported_accepted_once_last_fm_takes_it() {
        let transport = ScriptedTransport::with_replies([accepted_reply(1), accepted_reply(1)]);
        let mut service = connected_service(&transport, None, Vec::new());
        assert_eq!(service.published_state().latest_scrobble, None);

        service.handle_command(Command::QueueScrobble(entry(0)));
        assert_eq!(
            latest(&service),
            (1, ScrobbleDelivery::Accepted, String::new())
        );
        service.handle_command(Command::QueueScrobble(entry(1)));
        assert_eq!(
            latest(&service),
            (2, ScrobbleDelivery::Accepted, String::new())
        );
        assert!(service.queue.is_empty());
    }

    #[test]
    fn the_latest_scrobble_waits_while_last_fm_cannot_be_reached_and_is_accepted_later() {
        let transport = ScriptedTransport::with_replies([
            Err("connection reset".to_string()),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));
        let (ordinal, delivery, message) = latest(&service);
        assert_eq!((ordinal, delivery), (1, ScrobbleDelivery::Waiting));
        assert!(message.contains("connection reset"), "{message}");
        assert_eq!(service.queue, entries(0..1));
        assert!(service.retry_due.is_some());

        service.handle_command(Command::Flush);
        assert_eq!(
            latest(&service),
            (1, ScrobbleDelivery::Accepted, String::new())
        );
    }

    #[test]
    fn the_latest_scrobble_behind_a_failing_backlog_waits() {
        let transport = ScriptedTransport::with_replies([reply(503, "<html>Bad gateway</html>")]);
        let mut service =
            connected_service(&transport, None, entries(0..MAX_SCROBBLES_PER_BATCH + 5));

        service.handle_command(Command::QueueScrobble(entry(99)));
        assert_eq!(latest(&service).1, ScrobbleDelivery::Waiting);
    }

    #[test]
    fn the_latest_scrobble_deferred_by_the_daily_limit_waits() {
        let transport =
            ScriptedTransport::with_replies([scrobble_reply(&[LASTFM_IGNORED_DAILY_LIMIT])]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));
        let (_, delivery, message) = latest(&service);
        assert_eq!(delivery, ScrobbleDelivery::Waiting);
        assert!(message.contains("daily"), "{message}");
        assert_eq!(service.queue, entries(0..1));
    }

    #[test]
    fn the_latest_scrobble_behind_a_batch_the_daily_limit_deferred_waits() {
        let mut codes = vec![0; MAX_SCROBBLES_PER_BATCH];
        codes[0] = LASTFM_IGNORED_DAILY_LIMIT;
        let transport = ScriptedTransport::with_replies([scrobble_reply(&codes)]);
        let mut service = connected_service(&transport, None, entries(0..MAX_SCROBBLES_PER_BATCH));

        service.handle_command(Command::QueueScrobble(entry(99)));
        let (_, delivery, message) = latest(&service);
        assert_eq!(delivery, ScrobbleDelivery::Waiting);
        assert!(message.contains("daily"), "{message}");
        assert_eq!(scrobble_requests(&transport).len(), 1);
        assert!(service.retry_due.is_some());
    }

    #[test]
    fn a_scrobble_last_fm_ignores_is_reported_failed_with_the_reason() {
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[1])]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));
        assert_eq!(
            latest(&service),
            (
                1,
                ScrobbleDelivery::Failed,
                "Ignored for testing".to_string()
            )
        );
        assert!(service.queue.is_empty());
        // As before, the failure is also told to the user.
        assert!(
            service
                .problems
                .iter()
                .any(|problem| problem.message.contains("ignored 1 scrobble")),
            "{:?}",
            service.problems
        );
    }

    #[test]
    fn a_scrobble_whose_request_last_fm_refuses_is_reported_failed_and_stays_queued() {
        let transport = ScriptedTransport::with_replies([reply(
            403,
            r#"{"error":10,"message":"Invalid API key"}"#,
        )]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));
        let (_, delivery, message) = latest(&service);
        assert_eq!(delivery, ScrobbleDelivery::Failed);
        assert!(message.contains("Invalid API key"), "{message}");
        assert_eq!(service.queue, entries(0..1));
        // Retrying would not help, so the user is told.
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        assert!(!service.problems[0].warning);
        assert!(service.problems[0].message.contains("Invalid API key"));
    }

    #[test]
    fn a_scrobble_sent_with_an_expired_session_is_reported_failed() {
        let transport = ScriptedTransport::with_replies([reply(
            403,
            r#"{"error":9,"message":"Invalid session key"}"#,
        )]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));
        let (_, delivery, message) = latest(&service);
        assert_eq!(delivery, ScrobbleDelivery::Failed);
        assert!(message.contains("expired"), "{message}");
        assert_eq!(service.state.auth_state, AuthState::ReauthRequired);
    }

    /// Pauses or resumes `service` as [`Handle::set_paused`] does.
    fn set_paused(service: &mut Service, paused: bool) {
        service.paused.store(paused, Ordering::SeqCst);
        service.handle_command(Command::PauseChanged);
    }

    /// Scrobbles queued before the pause, also those queued while Last.fm
    /// could not be reached, count as scrobbled: their plays reached the
    /// scrobble point before the pause. They wait while paused and are sent
    /// on resuming.
    #[test]
    fn scrobbles_queued_before_the_pause_count_as_scrobbled_and_wait_for_the_resume() {
        let transport = ScriptedTransport::with_replies([accepted_reply(2)]);
        let mut service = connected_service(&transport, None, entries(0..1));

        set_paused(&mut service, true);
        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::QueueScrobble(entry(1)));
        service.handle_command(Command::Flush);
        assert!(
            transport.requests().is_empty(),
            "{:?}",
            transport.requests()
        );
        assert!(service.retry_due.is_none());
        // Held, not lost.
        assert_eq!(service.queue, entries(0..2));
        assert_eq!(latest(&service).1, ScrobbleDelivery::Pending);

        // Resuming sends what was held.
        set_paused(&mut service, false);
        assert_eq!(scrobble_requests(&transport).len(), 1);
        assert!(service.queue.is_empty());
        assert_eq!(
            latest(&service),
            (1, ScrobbleDelivery::Accepted, String::new())
        );
    }

    #[test]
    fn a_new_service_scrobbles_whatever_the_previous_one_paused() {
        let transport = ScriptedTransport::default();
        let mut service = connected_service(&transport, None, Vec::new());
        set_paused(&mut service, true);
        assert!(service.is_paused());

        // As after a restart: the pause is not saved.
        let service = connected_service(&transport, None, Vec::new());
        assert!(!service.is_paused());
        let (handle, _commands) = test_support::command_handle();
        assert!(!handle.is_paused());
    }

    /// Answers like a [`ScriptedTransport`], and pauses scrobbling while
    /// its first request is under way, as the user may.
    struct PausingTransport {
        inner: ScriptedTransport,
        paused: Arc<AtomicBool>,
    }

    impl Transport for PausingTransport {
        fn post_form(&self, url: &str, form: &[(String, String)]) -> Result<HttpReply, String> {
            self.paused.store(true, Ordering::SeqCst);
            self.inner.post_form(url, form)
        }
    }

    #[test]
    fn a_pause_during_a_flush_stops_it_before_the_next_batch() {
        let transport = ScriptedTransport::with_replies([
            accepted_reply(MAX_SCROBBLES_PER_BATCH),
            accepted_reply(MAX_SCROBBLES_PER_BATCH),
        ]);
        let paused = Arc::new(AtomicBool::new(false));
        let mut service = Service::new(
            ServiceOptions {
                queue_path: None,
                initial_enabled: true,
            },
            ServiceBackends {
                transport: Box::new(PausingTransport {
                    inner: transport.clone(),
                    paused: Arc::clone(&paused),
                }),
                ..backends(&transport, MemorySessionKeys::with_key(USERNAME, "session"))
            },
        );
        service.paused = paused;
        service.handle_command(Command::LoadStoredSession {
            username: USERNAME.to_string(),
        });
        service.queue = entries(0..MAX_SCROBBLES_PER_BATCH + 10);

        service.handle_command(Command::Flush);
        // The batch under way finished; the next did not start.
        assert_eq!(scrobble_requests(&transport).len(), 1);
        assert_eq!(
            service.queue,
            entries(MAX_SCROBBLES_PER_BATCH..MAX_SCROBBLES_PER_BATCH + 10)
        );
        assert!(service.retry_due.is_none());
    }

    #[test]
    fn a_pause_stops_requests_queued_behind_the_one_under_way() {
        let dir = scratch_dir("pause-behind-a-request");
        let queue_path = dir.join("lastfm_queue.json");
        let (transport, requests) = test_support::BlockingTransport::new();
        let (handle, events) = test_support::spawn_connected(transport, queue_path.clone());

        // A now playing update is under way.
        handle.command(Command::SendNowPlaying(now_playing_track()));
        requests
            .started
            .recv_timeout(Duration::from_secs(10))
            .expect("the now playing request starts");
        // Behind it: a scrobble and another update, then the pause.
        handle.command(Command::QueueScrobble(entry(0)));
        handle.command(Command::SendNowPlaying(now_playing_track()));
        handle.set_paused(true);
        assert!(handle.is_paused());
        // The request under way finishes.
        requests.release.send(()).expect("release the request");

        // Once the service has handled every command before this one, no
        // other request has started: one would wait for a release here.
        handle.command(Command::SaveForQuit(7));
        loop {
            match events.recv_timeout(Duration::from_secs(10)) {
                Ok(Event::SavedForQuit { request_id: 7, .. }) => break,
                Ok(_) => {}
                Err(err) => panic!("the service did not answer: {err}"),
            }
        }
        assert_eq!(requests.answered(), 1);
        assert!(
            requests.started.try_recv().is_err(),
            "a request started while paused"
        );
        // The scrobble waits, saved.
        assert_eq!(read_queue_file(&queue_path), entries(0..1));

        // Resuming sends it.
        handle.set_paused(false);
        requests
            .started
            .recv_timeout(Duration::from_secs(10))
            .expect("the scrobble is sent on resuming");
        requests.release.send(()).expect("release the request");
        assert_eq!(
            handle.shutdown(Duration::from_secs(10)),
            ShutdownOutcome::Stopped
        );
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
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        assert!(!service.problems[0].warning);
        assert!(service.problems[0].message.contains("Reconnect"));
    }

    #[test]
    fn flush_drops_only_accepted_and_permanently_ignored_entries() {
        let dir = scratch_dir("flush-ignored");
        let queue_path = dir.join("lastfm_queue.json");
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[0, 1, 5])]);
        let mut service = connected_service(&transport, Some(queue_path.clone()), entries(0..3));

        service.handle_command(Command::Flush);

        // The entry deferred by the daily limit stays queued for a retry,
        // which the user need not be told about.
        assert_eq!(service.queue, vec![entry(2)]);
        assert_eq!(read_queue_file(&queue_path), vec![entry(2)]);
        assert!(service.retry_due.is_some());
        assert!(service.problems.is_empty(), "{:?}", service.problems);
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
        // Recent Issues is told, and logs it; the service logs nothing of
        // its own for it.
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        assert!(test_log::lines_containing("save-failure").is_empty());

        // The same failure again is not told again, but logged.
        service.handle_command(Command::QueueScrobble(entry(1)));
        assert_eq!(service.problems.len(), 1, "{:?}", service.problems);
        let again = test_log::lines_containing("save-failure");
        assert_eq!(again.len(), 1, "{again:?}");
        assert!(
            again[0]
                .starts_with("[ferrous] [lastfm] warning: Pending scrobbles could not be saved"),
            "{again:?}"
        );
        assert!(again[0].ends_with(" Again."), "{again:?}");
    }

    #[test]
    fn problems_nothing_receives_any_more_are_logged_instead() {
        let dir = scratch_dir("save-failure-unsent");
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
        let (event_tx, event_rx) = unbounded();
        drop(event_rx);

        service.publish(&event_tx);

        let logged = test_log::lines_containing("save-failure-unsent");
        assert_eq!(logged.len(), 1, "{logged:?}");
        assert!(
            logged[0].starts_with("[ferrous] [lastfm] error: Pending scrobbles could not be saved"),
            "{logged:?}"
        );
        assert!(service.problems.is_empty());
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

        test_log::clear();
        let missing = load(MemorySessionKeys::default());
        assert_eq!(missing.auth_state, AuthState::Disconnected);
        assert_eq!(missing.username, USERNAME);

        let failing = load(MemorySessionKeys::failing(&format!(
            "keyring locked for entry ferrous.lastfm/{USERNAME}"
        )));
        assert_eq!(failing.auth_state, AuthState::Error);
        assert!(failing.status_text.contains("keyring locked"));

        // The log tells both, without the account's name.
        let logged = test_log::lines_containing("[ferrous] [lastfm] warning: ");
        assert_eq!(logged.len(), 2, "{logged:?}");
        assert!(logged[0].contains("No session key was found"), "{logged:?}");
        assert!(logged[1].contains("could not be read: "), "{logged:?}");
        assert!(logged[1].contains("keyring locked"), "{logged:?}");
        for line in test_log::all() {
            assert!(!line.contains(USERNAME), "{line}");
        }
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
            .filter_map(|event| match event {
                Event::State(state) => Some(state),
                Event::SavedForQuit { .. } => None,
            })
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
    fn a_save_for_quit_reports_scrobbles_it_could_not_save_and_keeps_running() {
        let dir = scratch_dir("quit-save-failure");
        let config_dir = dir.join("config");
        fs::write(&config_dir, b"").expect("write blocking file");
        let (handle, events) = spawn_with_backends(
            ServiceOptions {
                queue_path: Some(config_dir.join("lastfm_queue.json")),
                initial_enabled: false,
            },
            || backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );
        handle.command(Command::QueueScrobble(entry(0)));
        handle.command(Command::SaveForQuit(7));
        handle.command(Command::SaveForQuit(8));

        let deadline = Instant::now() + Duration::from_secs(5);
        let mut states = Vec::new();
        let mut answers = Vec::new();
        while answers.len() < 2 {
            match events.recv_deadline(deadline).expect("events") {
                Event::State(state) => states.push(state),
                Event::SavedForQuit { request_id, result } => answers.push((request_id, result)),
            }
        }
        for (expected_id, (request_id, result)) in [7, 8].into_iter().zip(&answers) {
            assert_eq!(*request_id, expected_id);
            let message = result.as_ref().expect_err("still unsaved");
            assert!(
                message.contains("1 scrobble was not saved and would be lost on quit"),
                "{message}"
            );
        }
        let latest = states.last().expect("a state");
        assert!(latest.unsaved_scrobbles);
        // The failed save was told once, though every save failed.
        let problems: Vec<&Problem> = states.iter().flat_map(|state| &state.problems).collect();
        assert_eq!(problems.len(), 1, "{problems:?}");
        assert!(!problems[0].warning);
        assert!(problems[0].message.contains("could not be saved"));

        // The service is still running.
        assert!(matches!(
            handle.shutdown(Duration::from_secs(5)),
            ShutdownOutcome::SaveFailed(_)
        ));
    }

    #[test]
    fn a_save_for_quit_succeeds_once_the_queue_can_be_saved() {
        let dir = scratch_dir("quit-save-recovered");
        let config_dir = dir.join("config");
        fs::write(&config_dir, b"").expect("write blocking file");
        let (handle, events) = spawn_with_backends(
            ServiceOptions {
                queue_path: Some(config_dir.join("lastfm_queue.json")),
                initial_enabled: false,
            },
            || backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );
        handle.command(Command::QueueScrobble(entry(0)));
        let deadline = Instant::now() + Duration::from_secs(5);
        while !matches!(
            events.recv_deadline(deadline).expect("state"),
            Event::State(RuntimeState {
                unsaved_scrobbles: true,
                ..
            })
        ) {}
        // The user frees the folder, then retries quitting.
        fs::remove_file(&config_dir).expect("remove blocking file");
        fs::create_dir(&config_dir).expect("config folder");
        handle.command(Command::SaveForQuit(3));

        let answer = loop {
            match events.recv_deadline(deadline).expect("answer") {
                Event::State(state) => assert!(state.problems.is_empty()),
                Event::SavedForQuit { request_id, result } => break (request_id, result),
            }
        };
        assert_eq!(answer, (3, Ok(())));
        assert!(config_dir.join("lastfm_queue.json").is_file());
        assert_eq!(
            handle.shutdown(Duration::from_secs(5)),
            ShutdownOutcome::Stopped
        );
    }

    /// A directory in place of the queue file, which cannot be read,
    /// parsed or replaced, holding a marker file.
    fn unreadable_queue(dir: &Path) -> PathBuf {
        let queue_path = dir.join("lastfm_queue.json");
        fs::create_dir(&queue_path).expect("create directory at queue path");
        fs::write(queue_path.join("keep.txt"), b"keep").expect("write marker");
        queue_path
    }

    fn assert_unreadable_queue_kept(queue_path: &Path) {
        assert!(queue_path.is_dir());
        assert_eq!(
            fs::read(queue_path.join("keep.txt")).expect("marker"),
            b"keep"
        );
    }

    /// Shuts `service` down through its loop and returns what it reported.
    fn shut_down(service: Service) -> Result<(), String> {
        let (cmd_tx, cmd_rx) = unbounded();
        let (event_tx, _event_rx) = unbounded();
        let (done_tx, done_rx) = bounded(1);
        cmd_tx
            .send(Command::Shutdown(done_tx))
            .expect("queue shutdown");
        run_service_loop(&cmd_rx, &event_tx, service);
        done_rx.try_recv().expect("shutdown result")
    }

    fn service_with_unreadable_queue(queue_path: &Path) -> Service {
        let mut service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.to_path_buf()),
                initial_enabled: false,
            },
            backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );
        service.handle_command(Command::QueueScrobble(entry(1)));
        service
    }

    #[test]
    fn shutdown_reports_scrobbles_held_in_memory_after_the_queue_failed_to_load() {
        let dir = scratch_dir("shutdown-unreadable");
        let queue_path = unreadable_queue(&dir);
        let (handle, events) =
            test_support::spawn_connected(test_support::OfflineTransport, queue_path.clone());
        handle.command(Command::QueueScrobble(entry(0)));
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut offline = false;
        while let Ok(Event::State(state)) = events.recv_deadline(deadline) {
            if state.status_text.contains("retrying in") {
                // Saving stays visibly off.
                assert!(
                    state.status_text.contains("could not be read"),
                    "{}",
                    state.status_text
                );
                assert_eq!(state.pending_scrobble_count, 1);
                offline = true;
                break;
            }
        }
        assert!(offline, "the scrobble was not attempted");

        match handle.shutdown(Duration::from_secs(5)) {
            ShutdownOutcome::SaveFailed(message) => {
                assert!(message.contains("1 scrobble was lost"), "{message}");
                assert!(message.contains("could not be read"), "{message}");
            }
            other => panic!("expected a save failure, got {other:?}"),
        }
        assert_unreadable_queue_kept(&queue_path);
    }

    #[test]
    fn shutdown_without_scrobbles_leaves_an_unreadable_queue_alone() {
        let dir = scratch_dir("shutdown-unreadable-empty");
        let queue_path = unreadable_queue(&dir);
        let service = Service::new(
            ServiceOptions {
                queue_path: Some(queue_path.clone()),
                initial_enabled: false,
            },
            backends(&ScriptedTransport::default(), MemorySessionKeys::default()),
        );

        assert_eq!(shut_down(service), Ok(()));
        assert_unreadable_queue_kept(&queue_path);
    }

    #[test]
    fn shutdown_adds_scrobbles_held_in_memory_to_a_queue_file_readable_again() {
        let dir = scratch_dir("shutdown-readable-again");
        let queue_path = unreadable_queue(&dir);
        let service = service_with_unreadable_queue(&queue_path);
        // Fixed while Ferrous ran: the file holds an earlier scrobble.
        fs::remove_dir_all(&queue_path).expect("remove the unreadable queue");
        queue_file::save(&queue_path, &[entry(0)]).expect("write the earlier queue");

        assert_eq!(shut_down(service), Ok(()));
        assert_eq!(read_queue_file(&queue_path), entries(0..2));
    }

    #[test]
    fn shutdown_moves_an_invalid_queue_file_aside_before_saving() {
        let dir = scratch_dir("shutdown-invalid-again");
        let queue_path = unreadable_queue(&dir);
        let service = service_with_unreadable_queue(&queue_path);
        fs::remove_dir_all(&queue_path).expect("remove the unreadable queue");
        fs::write(&queue_path, b"not json").expect("write an invalid queue");

        assert_eq!(shut_down(service), Ok(()));
        assert_eq!(read_queue_file(&queue_path), vec![entry(1)]);
        assert_eq!(
            fs::read(dir.join("lastfm_queue.json.corrupt")).expect("kept copy"),
            b"not json"
        );
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

    /// The info lines this thread logged about Last.fm.
    fn lastfm_info_lines() -> Vec<String> {
        test_log::lines_containing("[ferrous] [lastfm] info: ")
            .into_iter()
            .filter(|line| !line.contains("reachable again"))
            .collect()
    }

    fn info(message: &str) -> String {
        format!("[ferrous] [lastfm] info: {message}")
    }

    #[test]
    fn now_playing_updates_are_logged_with_what_last_fm_did_with_them() {
        let transport = ScriptedTransport::with_replies([
            reply(
                200,
                r##"{"nowplaying":{"ignoredMessage":{"code":"0","#text":""}}}"##,
            ),
            reply(
                200,
                r##"{"nowplaying":{"ignoredMessage":{"code":"1","#text":"Artist ignored"}}}"##,
            ),
        ]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::SendNowPlaying(now_playing_track()));

        assert_eq!(
            lastfm_info_lines(),
            [
                info("Sending now playing: Artist 0 – Track 0."),
                info("Last.fm accepted now playing: Artist 0 – Track 0."),
                info("Sending now playing: Artist 0 – Track 0."),
                info("Last.fm ignored now playing: Artist 0 – Track 0 (Artist ignored)."),
            ]
        );
        assert!(service.problems.is_empty(), "{:?}", service.problems);
    }

    #[test]
    fn a_now_playing_update_that_fails_logs_its_failure_once() {
        let transport = ScriptedTransport::with_replies([reply(502, "")]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::SendNowPlaying(now_playing_track()));

        // The warning that #510 logs is the result.
        assert_eq!(
            lastfm_info_lines(),
            [info("Sending now playing: Artist 0 – Track 0.")]
        );
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] warning: "),
            [
                "[ferrous] [lastfm] warning: Last.fm HTTP error 502; the now-playing update was \
              not sent."
            ]
        );
    }

    #[test]
    fn an_accepted_scrobble_is_logged_with_its_track_and_time() {
        let transport = ScriptedTransport::with_replies([accepted_reply(1)]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));

        assert!(service.queue.is_empty());
        assert_eq!(
            lastfm_info_lines(),
            [
                info("Sending 1 scrobble: Artist 0 – Track 0 at 2023-11-14T22:13:20Z."),
                info("Last.fm accepted 1 scrobble."),
            ]
        );
    }

    #[test]
    fn a_scrobble_batch_is_logged_as_one_line_with_its_count() {
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[0, 1, 0, 0, 1])]);
        let mut service = connected_service(&transport, None, entries(0..5));

        service.handle_command(Command::Flush);

        assert!(service.queue.is_empty());
        assert_eq!(
            lastfm_info_lines(),
            [
                info(
                    "Sending 5 scrobbles: Artist 0 – Track 0 at 2023-11-14T22:13:20Z; \
                     Artist 1 – Track 1 at 2023-11-14T22:13:21Z; \
                     Artist 2 – Track 2 at 2023-11-14T22:13:22Z; +2 more."
                ),
                info(
                    "Last.fm accepted 3 of 5 scrobbles and ignored 2 (Artist 1 – Track 1: \
                     Ignored for testing; Artist 4 – Track 4: Ignored for testing)."
                ),
            ]
        );
    }

    #[test]
    fn an_ignored_scrobble_is_logged_with_the_reason() {
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[1])]);
        let mut service = connected_service(&transport, None, Vec::new());

        service.handle_command(Command::QueueScrobble(entry(0)));

        assert!(service.queue.is_empty());
        assert_eq!(
            lastfm_info_lines(),
            [
                info("Sending 1 scrobble: Artist 0 – Track 0 at 2023-11-14T22:13:20Z."),
                info(
                    "Last.fm accepted 0 of 1 scrobble and ignored 1 (Artist 0 – Track 0: \
                     Ignored for testing)."
                ),
            ]
        );
    }

    #[test]
    fn scrobbles_deferred_for_the_daily_limit_are_logged_with_their_batch() {
        let transport = ScriptedTransport::with_replies([scrobble_reply(&[0, 5, 1])]);
        let mut service = connected_service(&transport, None, entries(0..3));

        service.handle_command(Command::Flush);

        assert_eq!(service.queue, vec![entry(1)]);
        assert_eq!(
            lastfm_info_lines()[1],
            info(
                "Last.fm accepted 1 of 3 scrobbles, ignored 1 (Artist 2 – Track 2: Ignored \
                 for testing) and deferred 1 (daily scrobble limit reached)."
            )
        );
    }

    #[test]
    fn a_failed_scrobble_batch_logs_its_failure_once() {
        // A temporary failure: the retry line #510 logs is the result.
        let transport = ScriptedTransport::with_replies([reply(503, "")]);
        let mut service = connected_service(&transport, None, Vec::new());
        service.handle_command(Command::QueueScrobble(entry(0)));
        assert_eq!(service.queue, vec![entry(0)]);
        assert_eq!(
            lastfm_info_lines(),
            [info(
                "Sending 1 scrobble: Artist 0 – Track 0 at 2023-11-14T22:13:20Z."
            )]
        );
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] warning: "),
            ["[ferrous] [lastfm] warning: Last.fm HTTP error 503; retrying in 30 s."]
        );

        // A refused request goes to Recent Issues, which logs it.
        let transport = ScriptedTransport::with_replies([reply(
            403,
            r#"{"error":10,"message":"Invalid API key"}"#,
        )]);
        let mut service = connected_service(&transport, None, entries(1..2));
        service.handle_command(Command::Flush);
        assert_eq!(service.queue, entries(1..2));
        assert_eq!(
            lastfm_info_lines()[1..],
            [info(
                "Sending 1 scrobble: Artist 1 – Track 1 at 2023-11-14T22:13:21Z."
            )]
        );
        assert_eq!(
            service.problems,
            [Problem {
                message: "Last.fm API error 10: Invalid API key. Scrobbles stay queued."
                    .to_string(),
                warning: false,
            }]
        );
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] warning: ").len(),
            1
        );
    }

    #[test]
    fn no_secret_reaches_the_logged_lines() {
        const API_KEY: &str = "api-key-7c1f";
        const SHARED_SECRET: &str = "shared-secret-9e2d";
        const SESSION_KEY: &str = "session-key-4b8a";
        let transport = ScriptedTransport::with_replies([
            reply(200, r#"{"nowplaying":{}}"#),
            reply(502, ""),
            accepted_reply(1),
            scrobble_reply(&[1]),
            reply(503, ""),
            reply(403, r#"{"error":10,"message":"Invalid API key"}"#),
        ]);
        let mut service = Service::new(
            ServiceOptions {
                queue_path: None,
                initial_enabled: true,
            },
            ServiceBackends {
                transport: Box::new(transport.clone()),
                session_keys: Box::new(MemorySessionKeys::with_key(USERNAME, SESSION_KEY)),
                credentials: Some(AppCredentials {
                    api_key: API_KEY.to_string(),
                    shared_secret: SHARED_SECRET.to_string(),
                }),
                retry: RetryPolicy::DEFAULT,
            },
        );
        service.handle_command(Command::LoadStoredSession {
            username: USERNAME.to_string(),
        });

        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::SendNowPlaying(now_playing_track()));
        service.handle_command(Command::QueueScrobble(entry(0)));
        service.handle_command(Command::QueueScrobble(entry(1)));
        service.handle_command(Command::QueueScrobble(entry(2)));
        service.handle_command(Command::Flush);

        let requests = transport.requests();
        assert_eq!(requests.len(), 6);
        let mut secrets = vec![
            API_KEY.to_string(),
            SHARED_SECRET.to_string(),
            SESSION_KEY.to_string(),
        ];
        secrets.extend(requests.iter().flat_map(|form| {
            form.iter()
                .filter(|(name, _)| name == "api_sig")
                .map(|(_, signature)| signature.clone())
        }));
        let logged = test_log::lines_containing("[ferrous] [lastfm] ");
        assert!(logged.len() >= 10, "{logged:?}");
        let problems = service
            .problems
            .iter()
            .map(|problem| problem.message.clone());
        for line in logged.iter().cloned().chain(problems) {
            for secret in &secrets {
                assert!(!line.contains(secret.as_str()), "{line}");
            }
        }
    }

    #[test]
    fn a_retried_batch_is_listed_only_once() {
        let transport = ScriptedTransport::with_replies([
            reply(503, ""),
            Err("offline".to_string()),
            accepted_reply(5),
        ]);
        let mut service = connected_service(&transport, None, entries(0..5));

        for _ in 0..3 {
            service.handle_command(Command::Flush);
        }

        assert!(service.queue.is_empty());
        assert_eq!(scrobble_requests(&transport).len(), 3);
        assert_eq!(test_log::lines_containing("Artist 0 – Track 0").len(), 1);
        assert_eq!(
            lastfm_info_lines(),
            [
                info(
                    "Sending 5 scrobbles: Artist 0 – Track 0 at 2023-11-14T22:13:20Z; \
                     Artist 1 – Track 1 at 2023-11-14T22:13:21Z; \
                     Artist 2 – Track 2 at 2023-11-14T22:13:22Z; +2 more."
                ),
                info("Sending 5 scrobbles again."),
                info("Sending 5 scrobbles again."),
                info("Last.fm accepted 5 scrobbles."),
            ]
        );
        // Each retry adds one line besides #510's warning for the failure.
        assert_eq!(
            test_log::lines_containing("[ferrous] [lastfm] warning: "),
            [
                "[ferrous] [lastfm] warning: Last.fm HTTP error 503; retrying in 30 s.",
                "[ferrous] [lastfm] warning: Last.fm transport error: offline; retrying in 60 s.",
            ]
        );
    }

    #[test]
    fn a_retry_lists_only_the_scrobbles_that_joined_it() {
        let transport = ScriptedTransport::with_replies([
            reply(503, ""),
            reply(503, ""),
            accepted_reply(3),
            accepted_reply(1),
        ]);
        let mut service = connected_service(&transport, None, entries(0..2));

        service.handle_command(Command::Flush);
        service.handle_command(Command::QueueScrobble(entry(2)));
        service.handle_command(Command::Flush);
        service.handle_command(Command::QueueScrobble(entry(3)));

        assert!(service.queue.is_empty());
        assert_eq!(
            lastfm_info_lines(),
            [
                info(
                    "Sending 2 scrobbles: Artist 0 – Track 0 at 2023-11-14T22:13:20Z; \
                     Artist 1 – Track 1 at 2023-11-14T22:13:21Z."
                ),
                info(
                    "Sending 3 scrobbles, 2 of them again: Artist 2 – Track 2 at \
                     2023-11-14T22:13:22Z."
                ),
                info("Sending 3 scrobbles again."),
                info("Last.fm accepted 3 scrobbles."),
                // Once taken, the queue's entries are new again.
                info("Sending 1 scrobble: Artist 3 – Track 3 at 2023-11-14T22:13:23Z."),
                info("Last.fm accepted 1 scrobble."),
            ]
        );
    }

    #[test]
    fn scrobbles_deferred_for_the_daily_limit_are_not_listed_again() {
        let transport =
            ScriptedTransport::with_replies([scrobble_reply(&[0, 5]), accepted_reply(2)]);
        let mut service = connected_service(&transport, None, entries(0..2));

        service.handle_command(Command::Flush);
        service.handle_command(Command::QueueScrobble(entry(2)));

        assert!(service.queue.is_empty());
        assert_eq!(
            lastfm_info_lines()[2..],
            [
                info(
                    "Sending 2 scrobbles, 1 of them again: Artist 2 – Track 2 at \
                     2023-11-14T22:13:22Z."
                ),
                info("Last.fm accepted 2 scrobbles."),
            ]
        );
    }
}
