// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crossbeam_channel::{unbounded, Receiver, Sender};
use keyring::Entry;
use reqwest::blocking::Client;
use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const KEYRING_SERVICE: &str = "ferrous.lastfm";
const API_ENDPOINT: &str = "https://ws.audioscrobbler.com/2.0/";
const AUTH_ENDPOINT: &str = "https://www.last.fm/api/auth/";
const REQUEST_TIMEOUT: Duration = Duration::from_secs(15);
const MAX_SCROBBLES_PER_BATCH: usize = 50;
const LASTFM_ERROR_INVALID_SESSION: i64 = 9;
const LASTFM_ERROR_TEMPORARY: i64 = 11;
const LASTFM_ERROR_SERVICE_OFFLINE: i64 = 16;

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
    LoadStoredSession { username: String },
    BeginDesktopAuth,
    CompleteDesktopAuth,
    Disconnect { clear_queue: bool },
    SendNowPlaying(NowPlayingTrack),
    QueueScrobble(ScrobbleEntry),
    Flush,
    Shutdown,
}

#[derive(Debug, Clone)]
pub enum Event {
    State(RuntimeState),
}

#[derive(Debug, Clone)]
pub struct Handle {
    tx: Sender<Command>,
}

impl Handle {
    pub fn command(&self, command: Command) {
        let _ = self.tx.send(command);
    }
}

#[derive(Debug, Clone, Default)]
pub struct ServiceOptions {
    pub queue_path: Option<PathBuf>,
    pub initial_enabled: bool,
}

#[derive(Debug)]
struct Service {
    client: Client,
    credentials: Option<AppCredentials>,
    queue_path: Option<PathBuf>,
    queue: Vec<ScrobbleEntry>,
    session: Option<Session>,
    pending_auth: Option<PendingAuth>,
    state: RuntimeState,
}

#[derive(Debug)]
enum RequestError {
    Transport(String),
    Api { code: i64, message: String },
    InvalidResponse(String),
    Keyring(String),
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
pub fn spawn(options: ServiceOptions) -> (Handle, Receiver<Event>) {
    let (cmd_tx, cmd_rx) = unbounded();
    let (event_tx, event_rx) = unbounded();
    let _ = std::thread::Builder::new()
        .name("ferrous-lastfm".to_string())
        .spawn(move || run_service_loop(&cmd_rx, &event_tx, options));
    (Handle { tx: cmd_tx }, event_rx)
}

fn run_service_loop(cmd_rx: &Receiver<Command>, event_tx: &Sender<Event>, options: ServiceOptions) {
    let queue = options
        .queue_path
        .as_ref()
        .and_then(|path| load_queue(path).ok())
        .unwrap_or_default();
    let credentials = app_credentials();
    let mut service = Service {
        client: Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()
            .unwrap_or_else(|_| Client::new()),
        credentials: credentials.clone(),
        queue_path: options.queue_path,
        queue,
        session: None,
        pending_auth: None,
        state: RuntimeState {
            enabled: options.initial_enabled,
            build_configured: credentials.is_some(),
            pending_scrobble_count: 0,
            status_text: if credentials.is_some() {
                String::new()
            } else {
                "Last.fm not configured in this build.".to_string()
            },
            ..RuntimeState::default()
        },
    };
    service.state.pending_scrobble_count = service.queue.len();
    let _ = emit_state(event_tx, &service.state);

    while let Ok(command) = cmd_rx.recv() {
        let keep_running = service.handle_command(command);
        let _ = emit_state(event_tx, &service.state);
        if !keep_running {
            break;
        }
    }
}

fn emit_state(event_tx: &Sender<Event>, state: &RuntimeState) -> Result<(), ()> {
    event_tx.send(Event::State(state.clone())).map_err(|_| ())
}

impl Service {
    fn handle_command(&mut self, command: Command) -> bool {
        match command {
            Command::SetEnabled(enabled) => {
                self.state.enabled = enabled;
                if enabled {
                    self.flush_queue();
                }
            }
            Command::LoadStoredSession { username } => self.load_session(username),
            Command::BeginDesktopAuth => self.begin_desktop_auth(),
            Command::CompleteDesktopAuth => self.complete_desktop_auth(),
            Command::Disconnect { clear_queue } => self.disconnect(clear_queue),
            Command::SendNowPlaying(track) => self.send_now_playing(&track),
            Command::QueueScrobble(entry) => {
                self.queue.push(entry);
                self.save_queue();
                self.state.pending_scrobble_count = self.queue.len();
                self.flush_queue();
            }
            Command::Flush => self.flush_queue(),
            Command::Shutdown => return false,
        }
        true
    }

    fn begin_desktop_auth(&mut self) {
        let Some(credentials) = self.credentials.as_ref() else {
            self.state.auth_state = AuthState::Error;
            self.state.status_text = "Last.fm not configured in this build.".to_string();
            return;
        };
        match request_token(&self.client, credentials) {
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
                self.state.status_text = request_error_message(err);
            }
        }
    }

    fn complete_desktop_auth(&mut self) {
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
        match request_session(&self.client, credentials, &pending_auth.token) {
            Ok(session) => {
                if let Err(err) = store_session_key(&session.username, &session.session_key) {
                    self.state.auth_state = AuthState::Error;
                    self.state.status_text = request_error_message(err);
                    return;
                }
                self.session = Some(session.clone());
                self.pending_auth = None;
                self.state.username = session.username;
                self.state.auth_state = AuthState::Connected;
                self.state.auth_url.clear();
                self.state.status_text = "Last.fm account connected.".to_string();
                self.flush_queue();
            }
            Err(err) => {
                self.state.auth_state = AuthState::Error;
                self.state.status_text = request_error_message(err);
            }
        }
    }

    fn load_session(&mut self, username: String) {
        if username.trim().is_empty() {
            return;
        }
        match load_session_key(&username) {
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
                let message = request_error_message(err);
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
        if !self.state.username.trim().is_empty() {
            let _ = delete_session_key(&self.state.username);
        }
        self.session = None;
        self.pending_auth = None;
        self.state.username.clear();
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::Disconnected;
        self.state.status_text = "Last.fm disconnected.".to_string();
        if clear_queue {
            self.queue.clear();
            self.save_queue();
        }
        self.state.pending_scrobble_count = self.queue.len();
    }

    fn send_now_playing(&mut self, track: &NowPlayingTrack) {
        if !self.state.enabled {
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
        match send_now_playing_request(&self.client, credentials, session, track) {
            Ok(()) => {
                self.state.status_text = format!("Now playing: {} - {}", track.artist, track.track);
            }
            Err(err) if is_invalid_session(&err) => {
                self.transition_to_reauth(
                    "Last.fm session expired. Reconnect to resume scrobbling.",
                );
            }
            Err(err) => {
                self.state.status_text = request_error_message(err);
            }
        }
    }

    fn flush_queue(&mut self) {
        if !self.state.enabled || self.queue.is_empty() {
            self.state.pending_scrobble_count = self.queue.len();
            return;
        }
        let Some(credentials) = self.credentials.as_ref() else {
            self.state.pending_scrobble_count = self.queue.len();
            return;
        };
        let Some(session) = self.session.as_ref() else {
            self.state.pending_scrobble_count = self.queue.len();
            return;
        };

        while !self.queue.is_empty() {
            let batch_len = self.queue.len().min(MAX_SCROBBLES_PER_BATCH);
            let batch = self
                .queue
                .iter()
                .take(batch_len)
                .cloned()
                .collect::<Vec<_>>();
            match send_scrobble_batch(&self.client, credentials, session, &batch) {
                Ok(()) => {
                    self.queue.drain(..batch_len);
                    self.save_queue();
                    self.state.status_text = "Last.fm scrobble queue flushed.".to_string();
                }
                Err(err) if is_invalid_session(&err) => {
                    self.transition_to_reauth(
                        "Last.fm session expired. Reconnect to flush queued scrobbles.",
                    );
                    break;
                }
                Err(err) if is_transient(&err) => {
                    self.state.status_text = request_error_message(err);
                    break;
                }
                Err(err) => {
                    self.state.status_text = request_error_message(err);
                    self.queue.drain(..batch_len);
                    self.save_queue();
                }
            }
        }

        self.state.pending_scrobble_count = self.queue.len();
    }

    fn transition_to_reauth(&mut self, message: &str) {
        self.session = None;
        self.pending_auth = None;
        self.state.auth_url.clear();
        self.state.auth_state = AuthState::ReauthRequired;
        self.state.status_text = message.to_string();
    }

    fn save_queue(&self) {
        let Some(path) = self.queue_path.as_ref() else {
            return;
        };
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        if self.queue.is_empty() {
            let _ = fs::remove_file(path);
            return;
        }
        if let Ok(text) = serde_json::to_string_pretty(&self.queue) {
            let _ = fs::write(path, text);
        }
    }
}

fn request_token(client: &Client, credentials: &AppCredentials) -> Result<String, RequestError> {
    let payload = send_signed_request(client, credentials, "auth.getToken", Vec::new())?;
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
    client: &Client,
    credentials: &AppCredentials,
    token: &str,
) -> Result<Session, RequestError> {
    let payload = send_signed_request(
        client,
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
    client: &Client,
    credentials: &AppCredentials,
    session: &Session,
    track: &NowPlayingTrack,
) -> Result<(), RequestError> {
    let mut params = vec![
        ("artist".to_string(), track.artist.clone()),
        ("track".to_string(), track.track.clone()),
        ("sk".to_string(), session.session_key.clone()),
    ];
    if !track.album.trim().is_empty() {
        params.push(("album".to_string(), track.album.clone()));
    }
    if let Some(track_number) = track.track_number {
        params.push(("trackNumber".to_string(), track_number.to_string()));
    }
    if let Some(duration_seconds) = track.duration_seconds {
        params.push(("duration".to_string(), duration_seconds.to_string()));
    }
    let _ = send_signed_request(client, credentials, "track.updateNowPlaying", params)?;
    Ok(())
}

fn send_scrobble_batch(
    client: &Client,
    credentials: &AppCredentials,
    session: &Session,
    batch: &[ScrobbleEntry],
) -> Result<(), RequestError> {
    let mut params = vec![("sk".to_string(), session.session_key.clone())];
    for (index, track) in batch.iter().enumerate() {
        let suffix = format!("[{index}]");
        params.push((format!("artist{suffix}"), track.artist.clone()));
        params.push((format!("track{suffix}"), track.track.clone()));
        params.push((
            format!("timestamp{suffix}"),
            track.timestamp_utc.to_string(),
        ));
        if !track.album.trim().is_empty() {
            params.push((format!("album{suffix}"), track.album.clone()));
        }
        if let Some(track_number) = track.track_number {
            params.push((format!("trackNumber{suffix}"), track_number.to_string()));
        }
        if let Some(duration_seconds) = track.duration_seconds {
            params.push((format!("duration{suffix}"), duration_seconds.to_string()));
        }
    }
    let _ = send_signed_request(client, credentials, "track.scrobble", params)?;
    Ok(())
}

fn send_signed_request(
    client: &Client,
    credentials: &AppCredentials,
    method: &str,
    mut params: Vec<(String, String)>,
) -> Result<Value, RequestError> {
    params.push(("method".to_string(), method.to_string()));
    params.push(("api_key".to_string(), credentials.api_key.clone()));
    let api_sig = api_signature(&params, &credentials.shared_secret);
    params.push(("api_sig".to_string(), api_sig));
    params.push(("format".to_string(), "json".to_string()));

    let response = client
        .post(API_ENDPOINT)
        .form(&params)
        .send()
        .map_err(|err| RequestError::Transport(err.to_string()))?;
    let payload = response
        .json::<Value>()
        .map_err(|err| RequestError::InvalidResponse(err.to_string()))?;
    if let Some(code) = payload.get("error").and_then(Value::as_i64) {
        let message = payload
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("Last.fm request failed")
            .to_string();
        return Err(RequestError::Api { code, message });
    }
    Ok(payload)
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

fn load_queue(path: &Path) -> Result<Vec<ScrobbleEntry>, std::io::Error> {
    let text = fs::read_to_string(path)?;
    Ok(serde_json::from_str(&text).unwrap_or_default())
}

fn keyring_entry(username: &str) -> Result<Entry, RequestError> {
    Entry::new(KEYRING_SERVICE, username).map_err(|err| RequestError::Keyring(err.to_string()))
}

fn store_session_key(username: &str, session_key: &str) -> Result<(), RequestError> {
    keyring_entry(username)?
        .set_password(session_key)
        .map_err(|err| RequestError::Keyring(err.to_string()))
}

fn load_session_key(username: &str) -> Result<Option<String>, RequestError> {
    let entry = keyring_entry(username)?;
    match entry.get_password() {
        Ok(value) if !value.trim().is_empty() => Ok(Some(value)),
        Ok(_) => Ok(None),
        Err(err) => {
            let message = err.to_string();
            if message.contains("No entry") || message.contains("NoPasswordFound") {
                Ok(None)
            } else {
                Err(RequestError::Keyring(message))
            }
        }
    }
}

fn delete_session_key(username: &str) -> Result<(), RequestError> {
    keyring_entry(username)?
        .delete_credential()
        .map_err(|err| RequestError::Keyring(err.to_string()))
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

fn is_transient(error: &RequestError) -> bool {
    match error {
        RequestError::Transport(_) => true,
        RequestError::Api { code, .. } => {
            *code == LASTFM_ERROR_TEMPORARY || *code == LASTFM_ERROR_SERVICE_OFFLINE
        }
        _ => false,
    }
}

fn request_error_message(error: RequestError) -> String {
    match error {
        RequestError::Transport(message) => format!("Last.fm transport error: {message}"),
        RequestError::Api { code, message } => format!("Last.fm API error {code}: {message}"),
        RequestError::InvalidResponse(message) => format!("Last.fm response error: {message}"),
        RequestError::Keyring(message) => format!("Last.fm keyring error: {message}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
