// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::path::PathBuf;

use serde_json::json;

use crate::analysis::{SpectrogramDisplayMode, SpectrogramViewMode};
use crate::playback::{PlaybackCommand, PlaybackEngine};

use super::{BridgeSettings, BridgeState, LibrarySortMode, ViewerFullscreenMode};

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub(super) struct SessionSnapshot {
    pub(super) queue: Vec<PathBuf>,
    pub(super) selected_queue_index: Option<usize>,
    pub(super) current_queue_index: Option<usize>,
    pub(super) current_path: Option<PathBuf>,
}

pub(super) fn config_base_path() -> Option<PathBuf> {
    #[cfg(test)]
    {
        static TEST_CONFIG_BASE: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();
        let path = TEST_CONFIG_BASE.get_or_init(|| {
            let mut base = std::env::temp_dir();
            base.push(format!("ferrous-test-config-{}", std::process::id()));
            let _ = fs::create_dir_all(&base);
            base
        });
        return Some(path.clone());
    }

    #[cfg(not(test))]
    std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME")
                .map(PathBuf::from)
                .map(|h| h.join(".config"))
        })
        .map(|base| base.join("ferrous"))
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
    SessionSnapshot {
        queue: state.queue.clone(),
        selected_queue_index: state.selected_queue_index,
        current_queue_index,
        current_path,
    }
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
    let restored_current_index = resolve_session_current_index(
        &state.queue,
        session.current_queue_index,
        session.current_path.as_ref(),
    );
    state.selected_queue_index = session
        .selected_queue_index
        .filter(|idx| *idx < state.queue.len())
        .or(restored_current_index);
    if state.queue.is_empty() {
        return;
    }
    playback.command(PlaybackCommand::LoadQueue(state.queue.clone()));
    if let Some(idx) = restored_current_index {
        state.playback.current = state.queue.get(idx).cloned();
        state.playback.current_queue_index = Some(idx);
        playback.command(PlaybackCommand::PlayAt(idx));
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
    let queue = queue_values
        .iter()
        .filter_map(|v| v.as_str().map(PathBuf::from))
        .collect::<Vec<_>>();
    let selected_queue_index = value
        .get("selected_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_queue_index = value
        .get("current_queue_index")
        .and_then(serde_json::Value::as_u64)
        .and_then(|value| usize::try_from(value).ok());
    let current_path = value
        .get("current_path")
        .and_then(serde_json::Value::as_str)
        .map(PathBuf::from);
    Some(SessionSnapshot {
        queue,
        selected_queue_index,
        current_queue_index,
        current_path,
    })
}

pub(super) fn format_session_text(session: &SessionSnapshot) -> String {
    let payload = json!({
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
    });
    serde_json::to_string_pretty(&payload).unwrap_or_else(|_| "{}".to_string())
}

pub(super) fn save_session_snapshot(session: &SessionSnapshot) {
    let Some(path) = session_path() else {
        return;
    };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let text = format_session_text(session);
    let tmp_path = path.with_extension("json.tmp");
    if fs::write(&tmp_path, text).is_ok() {
        let _ = fs::rename(&tmp_path, &path);
    } else {
        let _ = fs::remove_file(&tmp_path);
    }
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

pub(super) fn parse_settings_text(settings: &mut BridgeSettings, text: &str) {
    for line in text.lines() {
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let key = k.trim();
        let value = v.trim();
        match key {
            "volume" => {
                if let Ok(x) = value.parse::<f32>() {
                    settings.volume = x.clamp(0.0, 1.0);
                }
            }
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
            "log_scale" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.log_scale = x != 0;
                }
            }
            "show_fps" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.show_fps = x != 0;
                }
            }
            "show_spectrogram_crosshair" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.show_spectrogram_crosshair = x != 0;
                }
            }
            "show_spectrogram_scale" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.show_spectrogram_scale = x != 0;
                }
            }
            "channel_buttons_visibility" => {
                if let Ok(x) = value.parse::<u8>() {
                    settings.display.channel_buttons_visibility = x.min(2);
                }
            }
            "spectrogram_zoom_enabled" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.display.spectrogram_zoom_enabled = x != 0;
                }
            }
            "system_media_controls_enabled" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.integrations.system_media_controls_enabled = x != 0;
                }
            }
            "library_sort_mode" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.library_sort_mode = LibrarySortMode::from_i32(x);
                }
            }
            "lastfm_scrobbling_enabled" => {
                if let Ok(x) = value.parse::<i32>() {
                    settings.integrations.lastfm_scrobbling_enabled = x != 0;
                }
            }
            "lastfm_username" => {
                settings.integrations.lastfm_username = value.to_string();
            }
            _ => {}
        }
    }
}

pub(super) fn save_settings(settings: &BridgeSettings) {
    let Some(path) = settings_path() else {
        return;
    };
    let Some(parent) = path.parent() else {
        return;
    };
    let _ = fs::create_dir_all(parent);
    let text = format_settings_text(settings);

    // Atomic write: write to a temp file in the same directory, then rename.
    // This prevents a crash during write from truncating/corrupting the settings file.
    let tmp_path = path.with_extension("tmp");
    if let Err(err) = fs::write(&tmp_path, &text) {
        eprintln!("Failed to write settings temp file: {err}");
        return;
    }
    if let Err(err) = fs::rename(&tmp_path, &path) {
        eprintln!("Failed to rename settings temp file: {err}");
    }
}

pub(super) fn format_settings_text(settings: &BridgeSettings) -> String {
    format!(
        "volume={:.4}\nfft_size={}\nspectrogram_view_mode={}\nspectrogram_display_mode={}\nviewer_fullscreen_mode={}\ndb_range={:.2}\nlog_scale={}\nshow_fps={}\nshow_spectrogram_crosshair={}\nshow_spectrogram_scale={}\nchannel_buttons_visibility={}\nspectrogram_zoom_enabled={}\nsystem_media_controls_enabled={}\nlibrary_sort_mode={}\nlastfm_scrobbling_enabled={}\nlastfm_username={}\n",
        settings.volume,
        settings.fft_size,
        settings.spectrogram_view_mode.settings_value(),
        settings.spectrogram_display_mode.settings_value(),
        settings.viewer_fullscreen_mode.settings_value(),
        settings.db_range,
        i32::from(settings.display.log_scale),
        i32::from(settings.display.show_fps),
        i32::from(settings.display.show_spectrogram_crosshair),
        i32::from(settings.display.show_spectrogram_scale),
        settings.display.channel_buttons_visibility,
        i32::from(settings.display.spectrogram_zoom_enabled),
        i32::from(settings.integrations.system_media_controls_enabled),
        settings.library_sort_mode.to_i32(),
        i32::from(settings.integrations.lastfm_scrobbling_enabled),
        settings.integrations.lastfm_username,
    )
}

#[cfg(test)]
mod tests {
    use super::super::{BridgeDisplaySettings, BridgeIntegrationSettings};
    use super::*;
    use std::path::PathBuf;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
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
                show_spectrogram_crosshair: true,
                show_spectrogram_scale: true,
                channel_buttons_visibility: 1,
                spectrogram_zoom_enabled: true,
            },
            library_sort_mode: LibrarySortMode::Title,
            integrations: BridgeIntegrationSettings {
                system_media_controls_enabled: false,
                lastfm_scrobbling_enabled: true,
                lastfm_username: "tester".to_string(),
            },
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
        assert!(parsed.display.show_spectrogram_crosshair);
        assert!(parsed.display.show_spectrogram_scale);
        assert!(parsed.display.spectrogram_zoom_enabled);
        assert!(!parsed.integrations.system_media_controls_enabled);
        assert_eq!(parsed.library_sort_mode, LibrarySortMode::Title);
        assert!(parsed.integrations.lastfm_scrobbling_enabled);
        assert_eq!(parsed.integrations.lastfm_username, "tester");
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
        };
        let text = format_session_text(&session);
        let parsed = parse_session_text(&text).expect("parse session text");
        assert_eq!(parsed, session);
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
