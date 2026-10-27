// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::json;

use crate::analysis::{SpectrogramDisplayMode, SpectrogramViewMode};
use crate::playback::{
    volume_gain_to_level, volume_level_to_gain, FileStamp, PlaybackCommand, PlaybackEngine,
    PlaybackState, RepeatMode,
};

use super::{BridgeSettings, BridgeState, LibrarySortMode, ViewerFullscreenMode};

#[derive(Debug, Clone, PartialEq, Default)]
pub(super) struct SessionSnapshot {
    pub(super) queue: Vec<PathBuf>,
    pub(super) selected_queue_index: Option<usize>,
    pub(super) current_queue_index: Option<usize>,
    pub(super) current_path: Option<PathBuf>,
    pub(super) library_expanded_keys: Vec<String>,
    pub(super) library_view_selection_key: String,
    pub(super) library_view_anchor_key: String,
    pub(super) library_view_anchor_offset: f32,
    pub(super) repeat_mode: RepeatMode,
    pub(super) shuffle_enabled: bool,
    /// Where the current track was paused or playing.
    pub(super) position: Option<SavedPosition>,
    /// Keys this build does not know, saved as they were.
    pub(super) unknown_keys: serde_json::Map<String, serde_json::Value>,
}

/// A position in the current track, with the stamp its file had then.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct SavedPosition {
    pub(super) position: Duration,
    /// The track's duration, if playback knew it then.
    pub(super) duration: Option<Duration>,
    pub(super) file: FileStamp,
}

/// The largest position in milliseconds that playback can seek to: a
/// `GStreamer` seek takes a signed 64-bit count of nanoseconds.
const MAX_POSITION_MS: u64 = i64::MAX.unsigned_abs() / 1_000_000;

/// The keys of the session file that this build reads and writes. Others
/// are kept as they were.
const SESSION_KEYS: &[&str] = &[
    "queue",
    "queue_path_bytes",
    "selected_queue_index",
    "current_queue_index",
    "current_path",
    "current_path_bytes",
    "library_expanded_keys",
    "library_view_selection_key",
    "library_view_anchor_key",
    "library_view_anchor_offset",
    "repeat_mode",
    "shuffle",
    "playback_position",
];

pub(super) fn config_base_path() -> Option<PathBuf> {
    crate::paths::config_dir()
}

pub(super) fn settings_path() -> Option<PathBuf> {
    config_base_path().map(|base| base.join("settings.txt"))
}

pub(super) fn session_path() -> Option<PathBuf> {
    config_base_path().map(|base| base.join("session.json"))
}

pub(super) fn session_snapshot_for_state(state: &BridgeState) -> SessionSnapshot {
    let current_path = state
        .playback
        .current
        .clone()
        .filter(|path| state.queue.iter().any(|queued| queued == path));
    let current_queue_index = resolve_session_current_index(
        &state.queue,
        state.playback.current_queue_index,
        current_path.as_ref(),
    );
    let mut library_expanded_keys = state.expanded_keys.iter().cloned().collect::<Vec<_>>();
    library_expanded_keys.sort_unstable();
    SessionSnapshot {
        queue: state.queue.clone(),
        selected_queue_index: state.selected_queue_index,
        current_queue_index,
        library_expanded_keys,
        library_view_selection_key: state.library_view_selection_key.clone(),
        library_view_anchor_key: state.library_view_anchor_key.clone(),
        library_view_anchor_offset: state.library_view_anchor_offset,
        repeat_mode: state.playback.repeat_mode,
        shuffle_enabled: state.playback.shuffle_enabled,
        position: current_path.as_ref().and_then(|_| saved_position(state)),
        unknown_keys: state.session_unknown_keys.clone(),
        current_path,
    }
}

/// Where the current track is, while it plays or is paused away from its
/// start. A stopped track starts again from its beginning.
fn saved_position(state: &BridgeState) -> Option<SavedPosition> {
    let playback = &state.playback;
    if playback.state == PlaybackState::Stopped || playback.position.is_zero() {
        return None;
    }
    Some(SavedPosition {
        position: playback.position,
        duration: Some(playback.duration).filter(|duration| !duration.is_zero()),
        file: playback.current_file?,
    })
}

/// How often the position of a track that plays is saved. After a pause,
/// a stop, or a track change it is saved with the next periodic save, and
/// quitting saves it too.
pub(super) const PLAYING_POSITION_SAVE_INTERVAL: Duration = Duration::from_secs(30);

/// Whether `next` has to be saved over `saved`, the session saved last.
/// While a track plays, its position changes all the time, so a change of
/// the position alone is saved only `since_position_save` after the last
/// save reached [`PLAYING_POSITION_SAVE_INTERVAL`].
pub(super) fn session_save_due(
    saved: Option<&SessionSnapshot>,
    next: &SessionSnapshot,
    playing: bool,
    since_position_save: Duration,
) -> bool {
    let Some(saved) = saved else {
        return true;
    };
    if saved.position != next.position
        && (!playing || since_position_save >= PLAYING_POSITION_SAVE_INTERVAL)
    {
        return true;
    }
    let SessionSnapshot {
        queue,
        selected_queue_index,
        current_queue_index,
        current_path,
        library_expanded_keys,
        library_view_selection_key,
        library_view_anchor_key,
        library_view_anchor_offset,
        repeat_mode,
        shuffle_enabled,
        position: _,
        unknown_keys,
    } = next;
    saved.queue != *queue
        || saved.selected_queue_index != *selected_queue_index
        || saved.current_queue_index != *current_queue_index
        || saved.current_path != *current_path
        || saved.library_expanded_keys != *library_expanded_keys
        || saved.library_view_selection_key != *library_view_selection_key
        || saved.library_view_anchor_key != *library_view_anchor_key
        || saved.library_view_anchor_offset.to_bits() != library_view_anchor_offset.to_bits()
        || saved.repeat_mode != *repeat_mode
        || saved.shuffle_enabled != *shuffle_enabled
        || saved.unknown_keys != *unknown_keys
}

pub(super) fn resolve_session_current_index(
    queue: &[PathBuf],
    current_queue_index: Option<usize>,
    current_path: Option<&PathBuf>,
) -> Option<usize> {
    if let Some(idx) = current_queue_index.filter(|idx| *idx < queue.len()) {
        return Some(idx);
    }
    current_path.and_then(|path| queue.iter().position(|queued| queued == path))
}

pub(super) fn apply_session_restore(
    state: &mut BridgeState,
    playback: &PlaybackEngine,
    session: Option<&SessionSnapshot>,
) {
    let Some(session) = session else {
        return;
    };
    state.queue.clone_from(&session.queue);
    state.queue_sync.reset();
    state.queue_undo.forget();
    let restored_current_index = resolve_session_current_index(
        &state.queue,
        session.current_queue_index,
        session.current_path.as_ref(),
    );
    state.selected_queue_index = session
        .selected_queue_index
        .filter(|idx| *idx < state.queue.len())
        .or(restored_current_index);
    state.expanded_keys = session.library_expanded_keys.iter().cloned().collect();
    state
        .library_view_selection_key
        .clone_from(&session.library_view_selection_key);
    state
        .library_view_anchor_key
        .clone_from(&session.library_view_anchor_key);
    state.library_view_anchor_offset = session.library_view_anchor_offset;
    state.session_unknown_keys.clone_from(&session.unknown_keys);
    // Before the queue, which the engine orders for shuffle.
    state.playback.repeat_mode = session.repeat_mode;
    state.playback.shuffle_enabled = session.shuffle_enabled;
    playback.command(PlaybackCommand::SetRepeatMode(session.repeat_mode));
    playback.command(PlaybackCommand::SetShuffle(session.shuffle_enabled));
    if state.queue.is_empty() {
        return;
    }
    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    if let Some(idx) = restored_current_index {
        state.playback.current = state.queue.get(idx).cloned();
        state.playback.current_queue_index = Some(idx);
        playback.command(PlaybackCommand::PlayAt(idx));
        // The position was saved for the track at `current_path`. The
        // engine opens it paused there, unless its file has changed since;
        // nothing starts playing.
        if let Some(saved) = session
            .position
            .filter(|_| state.playback.current.is_some())
            .filter(|_| state.playback.current == session.current_path)
        {
            playback.command(PlaybackCommand::Cue {
                position: saved.position,
                expected: saved.file,
            });
        }
    } else {
        state.playback.current_queue_index = None;
    }
}

pub(super) fn load_session_snapshot() -> Option<SessionSnapshot> {
    let path = session_path()?;
    let text = fs::read_to_string(path).ok()?;
    parse_session_text(&text)
}

pub(super) fn parse_session_text(text: &str) -> Option<SessionSnapshot> {
    let value = serde_json::from_str::<serde_json::Value>(text).ok()?;
    let queue_values = value.get("queue")?.as_array()?;
    let mut queue = queue_values
        .iter()
        .filter_map(|v| v.as_str().map(PathBuf::from))
        .collect::<Vec<_>>();
    if let Some(raw_paths) = value
        .get("queue_path_bytes")
        .and_then(serde_json::Value::as_object)
    {
        for (index, raw) in raw_paths {
            let entry = index.parse::<usize>().ok().and_then(|i| queue.get_mut(i));
            if let (Some(entry), Some(path)) = (entry, raw.as_str().and_then(path_from_hex)) {
                *entry = path;
            }
        }
    }
    let selected_queue_index = value
        .get("selected_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_queue_index = value
        .get("current_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_path = value
        .get("current_path_bytes")
        .and_then(serde_json::Value::as_str)
        .and_then(path_from_hex)
        .or_else(|| {
            value
                .get("current_path")
                .and_then(serde_json::Value::as_str)
                .map(PathBuf::from)
        });
    let library_expanded_keys = value
        .get("library_expanded_keys")
        .and_then(serde_json::Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(serde_json::Value::as_str)
                .map(ToString::to_string)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let library_view_selection_key = value
        .get("library_view_selection_key")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_string();
    let library_view_anchor_key = value
        .get("library_view_anchor_key")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_string();
    let library_view_anchor_offset = value
        .get("library_view_anchor_offset")
        .and_then(|offset| offset.to_string().parse::<f32>().ok())
        .filter(|offset| offset.is_finite())
        .map_or(0.0, |offset| {
            offset.clamp(
                -super::MAX_LIBRARY_VIEW_ANCHOR_OFFSET,
                super::MAX_LIBRARY_VIEW_ANCHOR_OFFSET,
            )
        });
    Some(SessionSnapshot {
        queue,
        selected_queue_index,
        current_queue_index,
        current_path,
        library_expanded_keys,
        library_view_selection_key,
        library_view_anchor_key,
        library_view_anchor_offset,
        repeat_mode: value
            .get("repeat_mode")
            .and_then(serde_json::Value::as_str)
            .and_then(parse_repeat_mode)
            .unwrap_or_default(),
        shuffle_enabled: value
            .get("shuffle")
            .and_then(serde_json::Value::as_bool)
            .unwrap_or_default(),
        position: value
            .get("playback_position")
            .and_then(parse_saved_position),
        unknown_keys: unknown_session_keys(&value),
    })
}

/// The keys of a session file that this build does not know.
fn unknown_session_keys(value: &serde_json::Value) -> serde_json::Map<String, serde_json::Value> {
    value
        .as_object()
        .map(|object| {
            object
                .iter()
                .filter(|(key, _)| !SESSION_KEYS.contains(&key.as_str()))
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect()
        })
        .unwrap_or_default()
}

fn repeat_mode_value(mode: RepeatMode) -> &'static str {
    match mode {
        RepeatMode::Off => "off",
        RepeatMode::One => "one",
        RepeatMode::All => "all",
    }
}

fn parse_repeat_mode(value: &str) -> Option<RepeatMode> {
    match value {
        "off" => Some(RepeatMode::Off),
        "one" => Some(RepeatMode::One),
        "all" => Some(RepeatMode::All),
        _ => None,
    }
}

/// A position saved by [`format_session_text`]; `None` unless every part is
/// there, and the position is past the start, before the track's end, and
/// one that playback can seek to.
fn parse_saved_position(value: &serde_json::Value) -> Option<SavedPosition> {
    let number = |key: &str| value.get(key).and_then(serde_json::Value::as_u64);
    let millis = number("ms").filter(|ms| (1..=MAX_POSITION_MS).contains(ms))?;
    let position = Duration::from_millis(millis);
    let duration = match value.get("duration_ms") {
        None => None,
        Some(duration) => Some(Duration::from_millis(duration.as_u64()?)),
    };
    if duration.is_some_and(|duration| position >= duration) {
        return None;
    }
    Some(SavedPosition {
        position,
        duration,
        file: FileStamp {
            size: number("file_size")?,
            modified_ns: number("file_modified_ns")?,
        },
    })
}

pub(super) fn format_session_text(session: &SessionSnapshot) -> String {
    let mut payload = json!({
        "queue": session
            .queue
            .iter()
            .map(|p| p.to_string_lossy().to_string())
            .collect::<Vec<_>>(),
        "selected_queue_index": session.selected_queue_index,
        "current_queue_index": session.current_queue_index,
        "current_path": session
            .current_path
            .as_ref()
            .map(|p| p.to_string_lossy().to_string()),
        "library_expanded_keys": &session.library_expanded_keys,
        "library_view_selection_key": &session.library_view_selection_key,
        "library_view_anchor_key": &session.library_view_anchor_key,
        "library_view_anchor_offset": session.library_view_anchor_offset,
        "repeat_mode": repeat_mode_value(session.repeat_mode),
        "shuffle": session.shuffle_enabled,
    });
    if let Some(saved) = session.position {
        payload["playback_position"] = json!({
            "ms": u64::try_from(saved.position.as_millis()).unwrap_or(u64::MAX),
            "file_size": saved.file.size,
            "file_modified_ns": saved.file.modified_ns,
        });
        if let Some(duration) = saved.duration {
            payload["playback_position"]["duration_ms"] = u64::try_from(duration.as_millis())
                .unwrap_or(u64::MAX)
                .into();
        }
    }
    // JSON strings hold only Unicode, so a path that is not UTF-8 is also
    // saved as its bytes. Builds that don't read them still find each entry
    // at its index.
    let raw_queue_paths: serde_json::Map<String, serde_json::Value> = session
        .queue
        .iter()
        .enumerate()
        .filter_map(|(index, path)| Some((index.to_string(), path_to_hex(path)?.into())))
        .collect();
    if !raw_queue_paths.is_empty() {
        payload["queue_path_bytes"] = raw_queue_paths.into();
    }
    if let Some(raw) = session.current_path.as_deref().and_then(path_to_hex) {
        payload["current_path_bytes"] = raw.into();
    }
    if let Some(object) = payload.as_object_mut() {
        for (key, value) in &session.unknown_keys {
            object.entry(key.clone()).or_insert_with(|| value.clone());
        }
    }
    serde_json::to_string_pretty(&payload).unwrap_or_else(|_| "{}".to_string())
}

/// The bytes of `path` in hex, if it is not UTF-8 and so cannot be saved as
/// a JSON string.
#[cfg(unix)]
fn path_to_hex(path: &Path) -> Option<String> {
    use std::os::unix::ffi::OsStrExt;
    path.to_str()
        .is_none()
        .then(|| data_encoding::HEXLOWER.encode(path.as_os_str().as_bytes()))
}

#[cfg(not(unix))]
fn path_to_hex(_path: &Path) -> Option<String> {
    None
}

/// The path saved by [`path_to_hex`].
#[cfg(unix)]
fn path_from_hex(hex: &str) -> Option<PathBuf> {
    use std::os::unix::ffi::OsStringExt;
    let bytes = data_encoding::HEXLOWER_PERMISSIVE
        .decode(hex.as_bytes())
        .ok()?;
    Some(PathBuf::from(std::ffi::OsString::from_vec(bytes)))
}

#[cfg(not(unix))]
fn path_from_hex(_hex: &str) -> Option<PathBuf> {
    None
}

/// Save the queue and library view for the next start.
///
/// # Errors
///
/// Returns why the session cannot be saved.
pub(super) fn save_session_snapshot(session: &SessionSnapshot) -> Result<(), SaveFailure> {
    let Some(path) = session_path() else {
        return Err(SaveFailure::no_config_dir());
    };
    write_config_file(&path, format_session_text(session).as_bytes())
        .map_err(|failure| failure.explained("The queue could not be saved for the next start"))
}

pub(super) fn load_settings_into(settings: &mut BridgeSettings) {
    let Some(path) = settings_path() else {
        return;
    };
    let Ok(text) = fs::read_to_string(path) else {
        return;
    };
    parse_settings_text(settings, &text);
}

/// Parse a flag saved as `0` or `1` into `flag`, leaving it as it is if the
/// value is not a number.
fn parse_flag(value: &str, flag: &mut bool) {
    if let Ok(x) = value.parse::<i32>() {
        *flag = x != 0;
    }
}

/// Parse a settings value in the unit interval, rejecting NaN and infinities.
fn parse_unit_f32(value: &str) -> Option<f32> {
    value
        .parse::<f32>()
        .ok()
        .filter(|x| x.is_finite())
        .map(|x| x.clamp(0.0, 1.0))
}

pub(super) fn parse_settings_text(settings: &mut BridgeSettings, text: &str) {
    // Builds before the cubic volume scale saved only `volume`, the linear
    // gain.  Convert it to the level with the same loudness, unless the file
    // also has `volume_cubic`, the level itself.
    let mut legacy_volume_gain = None;
    let mut volume_level = None;
    settings.unknown_lines.clear();
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let key = k.trim();
        let value = v.trim();
        match key {
            "volume" => legacy_volume_gain = parse_unit_f32(value).or(legacy_volume_gain),
            "volume_cubic" => volume_level = parse_unit_f32(value).or(volume_level),
            "fft_size" => {
                if let Ok(x) = value.parse::<usize>() {
                    settings.fft_size = x.clamp(512, 8192).next_power_of_two();
                }
            }
            "spectrogram_view_mode" => {
                if let Some(mode) = SpectrogramViewMode::parse_settings_value(value) {
                    settings.spectrogram_view_mode = mode;
                }
            }
            "spectrogram_display_mode" => {
                if let Some(mode) = SpectrogramDisplayMode::parse_settings_value(value) {
                    settings.spectrogram_display_mode = mode;
                }
            }
            "viewer_fullscreen_mode" => {
                if let Some(mode) = ViewerFullscreenMode::parse_settings_value(value) {
                    settings.viewer_fullscreen_mode = mode;
                }
            }
            "db_range" => {
                if let Ok(x) = value.parse::<f32>() {
                    settings.db_range = x.clamp(50.0, 150.0);
                }
            }
            "log_scale" => parse_flag(value, &mut settings.display.log_scale),
            "show_level_meter" => parse_flag(value, &mut settings.display.show_level_meter),
            "show_fps" => parse_flag(value, &mut settings.display.show_fps),
            "prevent_display_sleep_in_fullscreen" => parse_flag(
                value,
                &mut settings.display.prevent_display_sleep_in_fullscreen,
            ),
            "show_spectrogram_crosshair" => {
                parse_flag(value, &mut settings.display.show_spectrogram_crosshair);
            }
            "show_spectrogram_scale" => {
                parse_flag(value, &mut settings.display.show_spectrogram_scale);
            }
            "channel_buttons_visibility" => {
                if let Ok(x) = value.parse::<u8>() {
                    settings.display.channel_buttons_visibility = x.min(2);
                }
            }
            "spectrogram_zoom_enabled" => {
                parse_flag(value, &mut settings.display.spectrogram_zoom_enabled);
            }
            "system_media_controls_enabled" => parse_flag(
                value,
                &mut settings.integrations.system_media_controls_enabled,
            ),
            "library_sort_mode" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.library_sort_mode = LibrarySortMode::from_i32(x);
                }
            }
            "lastfm_scrobbling_enabled" => {
                parse_flag(value, &mut settings.integrations.lastfm_scrobbling_enabled);
            }
            "lastfm_username" => {
                settings.integrations.lastfm_username = value.to_string();
            }
            "library_watch_enabled" => parse_flag(value, &mut settings.library_watch_enabled),
            _ if super::window_layout::parse_setting(&mut settings.window, key, value) => {}
            // Kept for the build that wrote it.
            _ => settings.unknown_lines.push(line.to_string()),
        }
    }
    if let Some(level) = volume_level.or_else(|| legacy_volume_gain.map(volume_gain_to_level)) {
        settings.volume = level;
    }
}

/// Save the settings.
///
/// # Errors
///
/// Returns why the settings cannot be saved.
pub(super) fn save_settings(settings: &BridgeSettings) -> Result<(), SaveFailure> {
    let Some(path) = settings_path() else {
        return Err(SaveFailure::no_config_dir());
    };
    write_config_file(&path, format_settings_text(settings).as_bytes())
        .map_err(|failure| failure.explained("Settings could not be saved"))
}

/// Why a file in the config directory could not be saved.
#[derive(Debug)]
pub(super) struct SaveFailure {
    /// What the user is shown.
    pub(super) message: String,
    /// The file and the kind of error, which stay the same while a save
    /// keeps failing the same way.
    pub(super) identity: (PathBuf, io::ErrorKind),
}

impl SaveFailure {
    fn no_config_dir() -> Self {
        Self {
            message: "Settings and the queue cannot be saved: set HOME or XDG_CONFIG_HOME."
                .to_string(),
            identity: (PathBuf::new(), io::ErrorKind::NotFound),
        }
    }

    fn explained(self, what: &str) -> Self {
        Self {
            message: format!("{what}: {}", self.message),
            ..self
        }
    }
}

/// Write a file in the config directory without a crash or a concurrent
/// save leaving it truncated; see [`crate::safe_save::write_file`].
fn write_config_file(path: &Path, bytes: &[u8]) -> Result<(), SaveFailure> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| SaveFailure {
            message: format!("failed to create {}: {error}", parent.display()),
            identity: (parent.to_path_buf(), error.kind()),
        })?;
    }
    crate::safe_save::write_file(path, bytes).map_err(|failure| SaveFailure {
        message: failure.to_string(),
        identity: (failure.path, failure.error.kind()),
    })
}

pub(super) fn format_settings_text(settings: &BridgeSettings) -> String {
    // `volume` keeps the linear gain so older builds play at the same loudness.
    let mut text = format!(
        "volume_cubic={:.4}\nvolume={:.6}\nfft_size={}\nspectrogram_view_mode={}\nspectrogram_display_mode={}\nviewer_fullscreen_mode={}\ndb_range={:.2}\nlog_scale={}\nshow_fps={}\nshow_level_meter={}\nprevent_display_sleep_in_fullscreen={}\nshow_spectrogram_crosshair={}\nshow_spectrogram_scale={}\nchannel_buttons_visibility={}\nspectrogram_zoom_enabled={}\nsystem_media_controls_enabled={}\nlibrary_sort_mode={}\nlastfm_scrobbling_enabled={}\nlastfm_username={}\nlibrary_watch_enabled={}\n",
        settings.volume,
        volume_level_to_gain(settings.volume),
        settings.fft_size,
        settings.spectrogram_view_mode.settings_value(),
        settings.spectrogram_display_mode.settings_value(),
        settings.viewer_fullscreen_mode.settings_value(),
        settings.db_range,
        i32::from(settings.display.log_scale),
        i32::from(settings.display.show_fps),
        i32::from(settings.display.show_level_meter),
        i32::from(settings.display.prevent_display_sleep_in_fullscreen),
        i32::from(settings.display.show_spectrogram_crosshair),
        i32::from(settings.display.show_spectrogram_scale),
        settings.display.channel_buttons_visibility,
        i32::from(settings.display.spectrogram_zoom_enabled),
        i32::from(settings.integrations.system_media_controls_enabled),
        settings.library_sort_mode.to_i32(),
        i32::from(settings.integrations.lastfm_scrobbling_enabled),
        settings.integrations.lastfm_username,
        i32::from(settings.library_watch_enabled),
    );
    super::window_layout::format_settings(&settings.window, &mut text);
    for line in &settings.unknown_lines {
        text.push_str(line);
        text.push('\n');
    }
    text
}

#[cfg(test)]
mod tests {
    use super::super::{BridgeDisplaySettings, BridgeIntegrationSettings, WindowLayout};
    use super::*;
    use std::path::PathBuf;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    #[test]
    fn level_meter_setting_defaults_on_and_roundtrips_both_values() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(&mut settings, "volume=0.5\n");
        assert!(settings.display.show_level_meter);
        for enabled in [false, true] {
            settings.display.show_level_meter = enabled;
            let mut parsed = BridgeSettings::default();
            parse_settings_text(&mut parsed, &format_settings_text(&settings));
            assert_eq!(parsed.display.show_level_meter, enabled);
        }
    }

    #[test]
    fn settings_roundtrip_text_format() {
        let settings = BridgeSettings {
            volume: 0.42,
            fft_size: 2048,
            spectrogram_view_mode: SpectrogramViewMode::PerChannel,
            spectrogram_display_mode: SpectrogramDisplayMode::Rolling,
            viewer_fullscreen_mode: ViewerFullscreenMode::WholeScreen,
            db_range: 77.5,
            display: BridgeDisplaySettings {
                log_scale: true,
                show_fps: true,
                show_level_meter: true,
                prevent_display_sleep_in_fullscreen: false,
                show_spectrogram_crosshair: true,
                show_spectrogram_scale: true,
                channel_buttons_visibility: 1,
                spectrogram_zoom_enabled: true,
            },
            library_sort_mode: LibrarySortMode::Title,
            library_watch_enabled: false,
            integrations: BridgeIntegrationSettings {
                system_media_controls_enabled: false,
                lastfm_scrobbling_enabled: true,
                lastfm_username: "tester".to_string(),
            },
            window: WindowLayout::default(),
            unknown_lines: Vec::new(),
        };
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!((parsed.volume - 0.42).abs() < 0.0001);
        assert_eq!(parsed.fft_size, 2048);
        assert_eq!(
            parsed.spectrogram_view_mode,
            SpectrogramViewMode::PerChannel
        );
        assert_eq!(
            parsed.viewer_fullscreen_mode,
            ViewerFullscreenMode::WholeScreen
        );
        assert!((parsed.db_range - 77.5).abs() < 0.0001);
        assert!(parsed.display.log_scale);
        assert!(parsed.display.show_fps);
        assert!(!parsed.display.prevent_display_sleep_in_fullscreen);
        assert!(parsed.display.show_spectrogram_crosshair);
        assert!(parsed.display.show_spectrogram_scale);
        assert!(parsed.display.spectrogram_zoom_enabled);
        assert!(!parsed.integrations.system_media_controls_enabled);
        assert_eq!(parsed.library_sort_mode, LibrarySortMode::Title);
        assert!(parsed.integrations.lastfm_scrobbling_enabled);
        assert_eq!(parsed.integrations.lastfm_username, "tester");
        assert!(!parsed.library_watch_enabled);
    }

    #[test]
    fn library_watching_defaults_on_and_roundtrips_both_values() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(&mut settings, "volume=0.5\nlibrary_sort_mode=1\n");
        assert!(settings.library_watch_enabled, "on in older settings files");
        for enabled in [false, true] {
            settings.library_watch_enabled = enabled;
            let mut parsed = BridgeSettings::default();
            parse_settings_text(&mut parsed, &format_settings_text(&settings));
            assert_eq!(parsed.library_watch_enabled, enabled);
        }
    }

    #[test]
    fn settings_this_build_does_not_know_are_saved_as_they_were() {
        let mut settings = BridgeSettings::default();
        let text = "volume_cubic=0.5\nfuture_key=some value\nlibrary_watch_enabled=0\n\
                    another.future = 1 \n";
        parse_settings_text(&mut settings, text);
        settings.volume = 0.25;

        let saved = format_settings_text(&settings);
        assert!(saved.contains("\nfuture_key=some value\n"), "{saved}");
        assert!(saved.contains("\nanother.future = 1 \n"), "{saved}");
        assert!(saved.contains("volume_cubic=0.2500\n"), "{saved}");
        assert!(saved.contains("library_watch_enabled=0\n"), "{saved}");
        assert_eq!(saved.matches("volume_cubic=").count(), 1);
        // Loading again keeps them once.
        let mut reloaded = BridgeSettings::default();
        parse_settings_text(&mut reloaded, &saved);
        parse_settings_text(&mut reloaded, &saved);
        assert_eq!(format_settings_text(&reloaded), saved);
    }

    #[test]
    fn the_window_layout_is_saved_with_the_settings_and_keeps_unknown_lines() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(&mut settings, "volume_cubic=0.5\nfuture_key=1\n");
        assert_eq!(settings.window, WindowLayout::default(), "older files");
        settings.window = WindowLayout {
            geometry: Some(super::super::WindowGeometry {
                x: 10,
                y: 20,
                width: 1100,
                height: 700,
            }),
            maximized: true,
            sidebar_width_ratio: Some(0.2),
            queue_height_ratio: Some(0.7),
            visualization_mode: 1,
            follow_current_track: false,
        };
        let text = format_settings_text(&settings);
        assert!(
            text.contains("\nwindow_geometry=10,20,1100,700\n"),
            "{text}"
        );
        assert!(text.contains("\nfuture_key=1\n"), "{text}");
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert_eq!(parsed.window, settings.window);
        assert_eq!(parsed.unknown_lines, vec!["future_key=1".to_string()]);
        assert_eq!(format_settings_text(&parsed), text);
    }

    #[test]
    fn legacy_linear_volume_keeps_its_loudness_on_the_cubic_scale() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(&mut settings, "volume=0.5\n");
        assert!(
            (settings.volume - 0.793_700_5).abs() < 1e-5,
            "{}",
            settings.volume
        );
        assert!((volume_level_to_gain(settings.volume) - 0.5).abs() < 1e-5);
    }

    #[test]
    fn cubic_volume_level_wins_over_legacy_gain_in_any_order() {
        for text in [
            "volume=0.125\nvolume_cubic=0.3\n",
            "volume_cubic=0.3\nvolume=0.125\n",
        ] {
            let mut settings = BridgeSettings::default();
            parse_settings_text(&mut settings, text);
            assert!((settings.volume - 0.3).abs() < 1e-6, "{text:?}");
        }
    }

    #[test]
    fn settings_save_volume_level_and_linear_gain_for_older_builds() {
        let settings = BridgeSettings {
            volume: 0.5,
            ..BridgeSettings::default()
        };
        let text = format_settings_text(&settings);
        assert!(text.contains("volume_cubic=0.5000\n"), "{text}");
        assert!(text.contains("\nvolume=0.125000\n"), "{text}");
    }

    #[test]
    fn non_finite_volume_values_are_ignored() {
        let mut settings = BridgeSettings {
            volume: 0.4,
            ..BridgeSettings::default()
        };
        parse_settings_text(&mut settings, "volume_cubic=NaN\nvolume=inf\n");
        assert!((settings.volume - 0.4).abs() < f32::EPSILON);
    }

    #[test]
    fn settings_parse_clamps_invalid_ranges() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=2.5\nfft_size=111\nspectrogram_view_mode=bad\nviewer_fullscreen_mode=bad\ndb_range=500\nlog_scale=0\nshow_fps=1\nsystem_media_controls_enabled=0\nlibrary_sort_mode=0\n",
        );
        assert_eq!(settings.volume, 1.0);
        assert_eq!(settings.fft_size, 512);
        assert_eq!(settings.spectrogram_view_mode, SpectrogramViewMode::Downmix);
        assert_eq!(
            settings.viewer_fullscreen_mode,
            ViewerFullscreenMode::WithinWindow
        );
        assert_eq!(settings.db_range, 150.0);
        assert!(!settings.display.log_scale);
        assert!(settings.display.show_fps);
        assert!(!settings.integrations.system_media_controls_enabled);
        assert_eq!(settings.library_sort_mode, LibrarySortMode::Year);
        assert!(!settings.integrations.lastfm_scrobbling_enabled);
        assert!(settings.integrations.lastfm_username.is_empty());
    }

    #[test]
    fn settings_default_system_media_controls_enabled_when_omitted() {
        let mut settings = BridgeSettings::default();
        parse_settings_text(
            &mut settings,
            "volume=0.5\nfft_size=2048\nspectrogram_view_mode=per_channel\nviewer_fullscreen_mode=whole_screen\ndb_range=80\nlog_scale=1\nshow_fps=0\nlibrary_sort_mode=1\n",
        );
        assert!(settings.integrations.system_media_controls_enabled);
        assert!(settings.display.prevent_display_sleep_in_fullscreen);
        assert_eq!(
            settings.spectrogram_view_mode,
            SpectrogramViewMode::PerChannel
        );
        assert_eq!(
            settings.viewer_fullscreen_mode,
            ViewerFullscreenMode::WholeScreen
        );
    }

    #[test]
    fn settings_roundtrip_crosshair_and_scale() {
        let settings = BridgeSettings {
            display: BridgeDisplaySettings {
                log_scale: false,
                show_fps: false,
                show_level_meter: true,
                prevent_display_sleep_in_fullscreen: true,
                show_spectrogram_crosshair: true,
                show_spectrogram_scale: true,
                channel_buttons_visibility: 1,
                spectrogram_zoom_enabled: true,
            },
            ..BridgeSettings::default()
        };
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!(parsed.display.show_spectrogram_crosshair);
        assert!(parsed.display.show_spectrogram_scale);

        // Verify defaults when keys are absent.
        let mut empty_parsed = BridgeSettings::default();
        parse_settings_text(&mut empty_parsed, "volume=1.0\n");
        assert!(!empty_parsed.display.show_spectrogram_crosshair);
        assert!(!empty_parsed.display.show_spectrogram_scale);
    }

    #[test]
    fn channel_buttons_visibility_persists() {
        let mut settings = BridgeSettings::default();
        settings.display.channel_buttons_visibility = 2;
        let text = format_settings_text(&settings);
        let mut restored = BridgeSettings::default();
        parse_settings_text(&mut restored, &text);
        assert_eq!(restored.display.channel_buttons_visibility, 2);
    }

    #[test]
    fn channel_buttons_visibility_clamps_out_of_range() {
        let mut settings = BridgeSettings::default();
        // Parse an out-of-range value — should clamp to 2.
        parse_settings_text(&mut settings, "channel_buttons_visibility=99\n");
        assert_eq!(settings.display.channel_buttons_visibility, 2);
        // Parse a negative string — u8 parse fails, default stays.
        let mut settings2 = BridgeSettings::default();
        parse_settings_text(&mut settings2, "channel_buttons_visibility=-1\n");
        assert_eq!(settings2.display.channel_buttons_visibility, 1); // default
    }

    #[test]
    fn settings_roundtrip_zoom_enabled() {
        let mut settings = BridgeSettings::default();
        settings.display.spectrogram_zoom_enabled = false;
        let text = format_settings_text(&settings);
        let mut parsed = BridgeSettings::default();
        parse_settings_text(&mut parsed, &text);
        assert!(!parsed.display.spectrogram_zoom_enabled);

        // Default (key absent) should be true.
        let mut default_parsed = BridgeSettings::default();
        parse_settings_text(&mut default_parsed, "volume=1.0\n");
        assert!(default_parsed.display.spectrogram_zoom_enabled);
    }

    #[test]
    fn session_roundtrip_text_format() {
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), p("/b.flac")],
            selected_queue_index: Some(1),
            current_queue_index: Some(0),
            current_path: Some(p("/a.flac")),
            library_expanded_keys: vec![
                "album|/music|Artist|Album".to_string(),
                "artist|/music|Artist".to_string(),
            ],
            library_view_selection_key: "track|/music/Artist/Album/a.flac".to_string(),
            library_view_anchor_key: "album|/music|Artist|Album".to_string(),
            library_view_anchor_offset: 7.5,
            repeat_mode: RepeatMode::One,
            shuffle_enabled: true,
            position: Some(SavedPosition {
                position: Duration::from_millis(83_250),
                duration: Some(Duration::from_millis(241_000)),
                file: FileStamp {
                    size: 31_337_000,
                    modified_ns: 1_761_000_000_123_456_789,
                },
            }),
            unknown_keys: serde_json::Map::new(),
        };
        let text = format_session_text(&session);
        let parsed = parse_session_text(&text).expect("parse session text");
        assert_eq!(parsed, session);
    }

    #[test]
    fn library_view_anchor_offset_survives_rows_taller_than_the_default() {
        // Rows follow the font size, so the view's top can be well over
        // 24 pixels into the row at it.
        for offset in [40.0_f32, -40.0, 300.0] {
            let session = SessionSnapshot {
                library_view_anchor_key: "album|/music|Artist|Album".to_string(),
                library_view_anchor_offset: offset,
                ..SessionSnapshot::default()
            };
            let parsed =
                parse_session_text(&format_session_text(&session)).expect("parse session text");
            assert!(
                (parsed.library_view_anchor_offset - offset).abs() < f32::EPSILON,
                "{offset} came back as {}",
                parsed.library_view_anchor_offset
            );
        }
        let parsed = parse_session_text(r#"{"queue": [], "library_view_anchor_offset": 1e9}"#)
            .expect("parse session text");
        assert!(
            (parsed.library_view_anchor_offset - super::super::MAX_LIBRARY_VIEW_ANCHOR_OFFSET)
                .abs()
                < f32::EPSILON
        );
    }

    #[cfg(unix)]
    #[test]
    fn paths_that_are_not_utf8_survive_a_saved_session() {
        use std::os::unix::ffi::OsStringExt;
        let raw = PathBuf::from(std::ffi::OsString::from_vec(
            b"/music/Bj\xf6rk/01 \xe9t\xe9.flac".to_vec(),
        ));
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), raw.clone(), p("/b.flac")],
            selected_queue_index: Some(1),
            current_queue_index: Some(1),
            current_path: Some(raw.clone()),
            ..SessionSnapshot::default()
        };
        let text = format_session_text(&session);
        assert_eq!(
            parse_session_text(&text).expect("parse session text"),
            session
        );

        // The format stays readable by builds that only know the strings:
        // every entry is at its index, and the others are as before.
        let value: serde_json::Value = serde_json::from_str(&text).expect("json");
        let queue = value["queue"].as_array().expect("queue");
        assert_eq!(queue.len(), 3);
        assert_eq!(queue[0], "/a.flac");
        assert_eq!(queue[2], "/b.flac");
        assert!(queue[1].as_str().is_some());
        assert!(value.get("queue_path_bytes").is_some());

        // A session without the bytes reads as before.
        let utf8 = SessionSnapshot {
            queue: vec![p("/a.flac")],
            current_path: Some(p("/a.flac")),
            ..SessionSnapshot::default()
        };
        let text = format_session_text(&utf8);
        assert!(!text.contains("_bytes"), "{text}");
        assert_eq!(parse_session_text(&text).expect("parse session text"), utf8);
    }

    fn stamp() -> FileStamp {
        FileStamp {
            size: 4_096,
            modified_ns: 1_700_000_000_000_000_001,
        }
    }

    fn at(millis: u64) -> Option<SavedPosition> {
        Some(SavedPosition {
            position: Duration::from_millis(millis),
            duration: None,
            file: stamp(),
        })
    }

    #[test]
    fn a_session_from_an_older_build_restores_repeat_and_shuffle_off_and_no_position() {
        let text = r#"{"queue": ["/a.flac"], "current_queue_index": 0, "current_path": "/a.flac"}"#;
        let parsed = parse_session_text(text).expect("parse session text");
        assert_eq!(parsed.repeat_mode, RepeatMode::Off);
        assert!(!parsed.shuffle_enabled);
        assert_eq!(parsed.position, None);
        assert!(parsed.unknown_keys.is_empty());
    }

    #[test]
    fn corrupt_repeat_shuffle_and_position_values_are_ignored() {
        for (repeat, shuffle, position) in [
            (
                r#""sometimes""#,
                r#""yes""#,
                r#"{"ms": -5, "file_size": 1, "file_modified_ns": 2}"#,
            ),
            ("3", "1", r#"{"ms": 1000, "file_size": 1}"#),
            (
                "null",
                "null",
                r#"{"ms": "1000", "file_size": 1, "file_modified_ns": 2}"#,
            ),
            (r#""ONE""#, "[]", r#"[1000, 1, 2]"#),
            (
                r#""all ""#,
                "{}",
                r#"{"ms": 0, "file_size": 1, "file_modified_ns": 2}"#,
            ),
            (
                r#""one""#,
                "true",
                r#"{"ms": 1.5e3, "file_size": 1, "file_modified_ns": 2}"#,
            ),
        ] {
            let text = format!(
                r#"{{"queue": ["/a.flac"], "repeat_mode": {repeat}, "shuffle": {shuffle}, "playback_position": {position}}}"#
            );
            let parsed = parse_session_text(&text).expect("parse session text");
            let expected_repeat = if repeat == r#""one""# {
                RepeatMode::One
            } else {
                RepeatMode::Off
            };
            assert_eq!(parsed.repeat_mode, expected_repeat, "{text}");
            assert_eq!(parsed.shuffle_enabled, shuffle == "true", "{text}");
            assert_eq!(parsed.position, None, "{text}");
            // A key this build knows is not kept as an unknown one.
            assert!(parsed.unknown_keys.is_empty(), "{text}");
        }
    }

    #[test]
    fn positions_playback_cannot_seek_to_are_not_restored() {
        let parse = |position: &str| {
            let text = format!(
                r#"{{"queue": ["/a.flac"], "playback_position": {{{position}, "file_size": 1, "file_modified_ns": 2}}}}"#
            );
            parse_session_text(&text)
                .expect("parse session text")
                .position
                .map(|saved| saved.position)
        };
        // Saturated, as a corrupt or hand-edited file may have it.
        assert_eq!(parse(&format!(r#""ms": {}"#, u64::MAX)), None);
        assert_eq!(parse(&format!(r#""ms": {}"#, MAX_POSITION_MS + 1)), None);
        assert_eq!(
            parse(&format!(r#""ms": {MAX_POSITION_MS}"#)),
            Some(Duration::from_millis(MAX_POSITION_MS))
        );
        // At or past the end of the track.
        assert_eq!(parse(r#""ms": 60000, "duration_ms": 60000"#), None);
        assert_eq!(parse(r#""ms": 90000, "duration_ms": 60000"#), None);
        assert_eq!(parse(r#""ms": 59000, "duration_ms": "long""#), None);
        assert_eq!(
            parse(r#""ms": 59000, "duration_ms": 60000"#),
            Some(Duration::from_millis(59_000))
        );
    }

    #[test]
    fn session_keys_this_build_does_not_know_are_saved_as_they_were() {
        let text = r#"{
            "queue": ["/a.flac"],
            "queue_path_bytes": {"0": "2f61"},
            "current_path_bytes": "2f61",
            "future_flag": true,
            "future_settings": {"zoom": [1, 2, 3], "name": "x"},
            "repeat_mode": "all"
        }"#;
        let mut session = parse_session_text(text).expect("parse session text");
        assert_eq!(session.unknown_keys.len(), 2);
        session.shuffle_enabled = true;

        let saved = format_session_text(&session);
        let value: serde_json::Value = serde_json::from_str(&saved).expect("json");
        assert_eq!(value["future_flag"], serde_json::json!(true));
        assert_eq!(
            value["future_settings"],
            serde_json::json!({"zoom": [1, 2, 3], "name": "x"})
        );
        assert_eq!(value["repeat_mode"], "all");
        assert_eq!(value["shuffle"], true);
        // Keys this build writes only when it needs them do not come back
        // from the file read.
        assert!(value.get("queue_path_bytes").is_none(), "{saved}");
        assert!(value.get("current_path_bytes").is_none(), "{saved}");
        // Loading and saving again keeps them once, unchanged.
        let again = parse_session_text(&saved).expect("parse session text");
        assert_eq!(again, session);
        assert_eq!(format_session_text(&again), saved);
    }

    #[test]
    fn the_position_alone_of_a_playing_track_is_saved_every_interval() {
        let saved = SessionSnapshot {
            queue: vec![p("/a.flac")],
            current_queue_index: Some(0),
            current_path: Some(p("/a.flac")),
            position: at(10_000),
            ..SessionSnapshot::default()
        };
        let later = SessionSnapshot {
            position: at(12_000),
            ..saved.clone()
        };
        let soon = Duration::from_secs(2);
        assert!(session_save_due(None, &saved, true, soon));
        assert!(!session_save_due(Some(&saved), &saved, false, soon));
        // While it plays, only once the interval has passed.
        assert!(!session_save_due(Some(&saved), &later, true, soon));
        assert!(session_save_due(
            Some(&saved),
            &later,
            true,
            PLAYING_POSITION_SAVE_INTERVAL
        ));
        // Paused, seeked while paused, or stopped: at once.
        assert!(session_save_due(Some(&saved), &later, false, soon));
        let stopped = SessionSnapshot {
            position: None,
            ..saved.clone()
        };
        assert!(session_save_due(Some(&saved), &stopped, false, soon));
        // Anything else is saved at once, also while playing.
        for changed in [
            SessionSnapshot {
                repeat_mode: RepeatMode::All,
                ..later.clone()
            },
            SessionSnapshot {
                shuffle_enabled: true,
                ..later.clone()
            },
            SessionSnapshot {
                current_queue_index: None,
                ..later.clone()
            },
            SessionSnapshot {
                queue: vec![p("/a.flac"), p("/b.flac")],
                ..later.clone()
            },
        ] {
            assert!(session_save_due(Some(&saved), &changed, true, soon));
        }
    }

    fn state_at(position: Duration, playback_state: PlaybackState) -> BridgeState {
        BridgeState {
            queue: vec![p("/a.flac"), p("/b.flac")],
            playback: crate::playback::PlaybackSnapshot {
                current: Some(p("/b.flac")),
                current_queue_index: Some(1),
                current_file: Some(stamp()),
                position,
                state: playback_state,
                repeat_mode: RepeatMode::One,
                shuffle_enabled: true,
                ..crate::playback::PlaybackSnapshot::default()
            },
            ..BridgeState::default()
        }
    }

    #[test]
    fn the_session_saves_repeat_shuffle_and_the_position_of_a_paused_or_playing_track() {
        let secs = Duration::from_secs;
        for playback_state in [PlaybackState::Playing, PlaybackState::Paused] {
            let session = session_snapshot_for_state(&state_at(secs(42), playback_state));
            assert_eq!(session.repeat_mode, RepeatMode::One);
            assert!(session.shuffle_enabled);
            assert_eq!(session.position, at(42_000));
        }
        // A stopped track starts from its beginning, and so does one at it.
        let stopped = session_snapshot_for_state(&state_at(secs(42), PlaybackState::Stopped));
        assert_eq!(stopped.position, None);
        let at_start = session_snapshot_for_state(&state_at(secs(0), PlaybackState::Paused));
        assert_eq!(at_start.position, None);
        // Without the stamp of its file, the position could not be checked.
        let mut unstamped = state_at(secs(42), PlaybackState::Paused);
        unstamped.playback.current_file = None;
        assert_eq!(session_snapshot_for_state(&unstamped).position, None);
        // Nor without its track in the queue.
        let mut gone = state_at(secs(42), PlaybackState::Paused);
        gone.queue.pop();
        assert_eq!(session_snapshot_for_state(&gone).position, None);
    }

    /// The commands that restoring `session` sends to the engine.
    fn restore_commands(session: &SessionSnapshot) -> (BridgeState, Vec<PlaybackCommand>) {
        let (tx, rx) = crossbeam_channel::unbounded();
        let playback = PlaybackEngine::from_command_sender(tx);
        let mut state = BridgeState::default();
        apply_session_restore(&mut state, &playback, Some(session));
        (state, rx.try_iter().collect())
    }

    #[test]
    fn a_restored_session_sets_repeat_and_shuffle_and_cues_the_track_without_playing() {
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), p("/b.flac")],
            current_queue_index: Some(1),
            current_path: Some(p("/b.flac")),
            repeat_mode: RepeatMode::All,
            shuffle_enabled: true,
            position: at(61_500),
            ..SessionSnapshot::default()
        };
        let (state, commands) = restore_commands(&session);
        assert_eq!(state.playback.repeat_mode, RepeatMode::All);
        assert!(state.playback.shuffle_enabled);
        let summary: Vec<String> = commands.iter().map(|c| format!("{c:?}")).collect();
        assert!(
            matches!(
                commands.as_slice(),
                [
                    PlaybackCommand::SetRepeatMode(RepeatMode::All),
                    PlaybackCommand::SetShuffle(true),
                    PlaybackCommand::LoadQueue(_),
                    PlaybackCommand::PlayAt(1),
                    PlaybackCommand::Cue { position, expected },
                ] if *position == Duration::from_millis(61_500) && *expected == stamp()
            ),
            "{summary:?}"
        );
        assert!(!commands
            .iter()
            .any(|command| matches!(command, PlaybackCommand::Play)));

        // Repeat and shuffle come back with an empty queue too.
        let (state, commands) = restore_commands(&SessionSnapshot {
            repeat_mode: RepeatMode::One,
            ..SessionSnapshot::default()
        });
        assert_eq!(state.playback.repeat_mode, RepeatMode::One);
        assert!(matches!(
            commands.as_slice(),
            [
                PlaybackCommand::SetRepeatMode(RepeatMode::One),
                PlaybackCommand::SetShuffle(false)
            ]
        ));
    }

    #[test]
    fn a_position_saved_for_another_track_is_not_restored() {
        // The saved row holds another file than the position was saved for.
        let session = SessionSnapshot {
            queue: vec![p("/a.flac"), p("/b.flac")],
            current_queue_index: Some(0),
            current_path: Some(p("/b.flac")),
            position: at(61_500),
            ..SessionSnapshot::default()
        };
        let (_, commands) = restore_commands(&session);
        assert!(matches!(commands.last(), Some(PlaybackCommand::PlayAt(0))));
        assert!(!commands
            .iter()
            .any(|command| matches!(command, PlaybackCommand::Cue { .. })));
    }

    #[test]
    fn session_parse_rejects_missing_queue_array() {
        let parsed = parse_session_text(r#"{"selected_queue_index":1}"#);
        assert!(parsed.is_none());
    }

    #[test]
    fn resolve_session_current_index_prefers_valid_index() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, Some(2), Some(&p("/a.flac")));
        assert_eq!(idx, Some(2));
    }

    #[test]
    fn resolve_session_current_index_falls_back_to_path_when_index_missing() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, None, Some(&p("/b.flac")));
        assert_eq!(idx, Some(1));
    }

    #[test]
    fn resolve_session_current_index_falls_back_to_path_when_index_invalid() {
        let queue = vec![p("/a.flac"), p("/b.flac"), p("/c.flac")];
        let idx = resolve_session_current_index(&queue, Some(9), Some(&p("/c.flac")));
        assert_eq!(idx, Some(2));
    }
}
