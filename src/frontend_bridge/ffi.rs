use std::collections::{BTreeMap, HashMap, VecDeque};
use std::ffi::{c_char, c_uchar, CStr, CString};
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use serde::Deserialize;
use serde_json::json;

use super::{
    BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSettingsCommand, BridgeSnapshot, FrontendBridgeHandle,
};
use crate::playback::{PlaybackState, RepeatMode};

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_RESET: u8 = 0x02;
const ANALYSIS_FLAG_SPECTROGRAM: u8 = 0x04;
const MAX_PENDING_JSON_EVENTS: usize = 64;
const MAX_PENDING_ANALYSIS_FRAMES: usize = 64;

#[derive(Default)]
struct AnalysisDelta {
    sample_rate_hz: u32,
    frame_seq: u32,
    spectrogram_seq: u64,
    spectrogram_reset: bool,
    waveform_len: usize,
    waveform_changed: bool,
    spectrogram_lag_estimate_ms: f32,
    spectrogram_fifo_delay_ms: f32,
    spectrogram_stft_pending_ms: f32,
    spectrogram_window_center_ms: f32,
    spectrogram_target_delay_ms: f32,
    waveform_peaks_u8: Vec<u8>,
    spectrogram_rows_u8: Vec<Vec<u8>>,
}

#[derive(Default)]
struct JsonEmitState {
    last_waveform_peaks: Vec<f32>,
    last_library_digest: Option<LibraryDigest>,
    last_queue_digest: Option<QueueDigest>,
    last_queue_total_duration_secs: f64,
    last_queue_unknown_duration_count: usize,
    last_spectrogram_seq: u64,
    analysis_frame_seq: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LibraryDigest {
    roots_len: usize,
    tracks_len: usize,
    scan_in_progress: bool,
    first_root: Option<String>,
    last_root: Option<String>,
    first_track: Option<String>,
    last_track: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct QueueDigest {
    len: usize,
    selected: Option<usize>,
    first: Option<String>,
    last: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct JsonSnapshotSummary {
    playback_state: PlaybackState,
    playback_current: Option<PathBuf>,
    playback_repeat_mode: RepeatMode,
    playback_shuffle_enabled: bool,
    queue_len: usize,
    queue_selected: Option<usize>,
    queue_first: Option<PathBuf>,
    queue_last: Option<PathBuf>,
    library_roots: usize,
    library_tracks: usize,
    library_scan_in_progress: bool,
}

struct FfiRuntime {
    bridge: FrontendBridgeHandle,
    emit_state: JsonEmitState,
    json_snapshot_interval: Duration,
    last_json_snapshot_emit: Instant,
    last_json_summary: Option<JsonSnapshotSummary>,
    force_json_snapshot_emit: bool,
    pending_json_events: VecDeque<Vec<u8>>,
    pending_analysis_frames: VecDeque<Vec<u8>>,
    stopped: bool,
}

impl FfiRuntime {
    fn new() -> Self {
        let bridge = FrontendBridgeHandle::spawn();
        let json_snapshot_interval_ms = std::env::var("FERROUS_FFI_JSON_SNAPSHOT_MS")
            .ok()
            .and_then(|raw| raw.parse::<u64>().ok())
            .map_or(100, |v| v.clamp(16, 1000));
        let json_snapshot_interval = Duration::from_millis(json_snapshot_interval_ms);
        let now = Instant::now();
        let last_json_snapshot_emit = now.checked_sub(json_snapshot_interval).unwrap_or(now);
        let mut runtime = Self {
            bridge,
            emit_state: JsonEmitState::default(),
            json_snapshot_interval,
            last_json_snapshot_emit,
            last_json_summary: None,
            force_json_snapshot_emit: true,
            pending_json_events: VecDeque::with_capacity(MAX_PENDING_JSON_EVENTS),
            pending_analysis_frames: VecDeque::with_capacity(MAX_PENDING_ANALYSIS_FRAMES),
            stopped: false,
        };
        runtime.bridge.command(BridgeCommand::RequestSnapshot);
        runtime.poll(16);
        runtime
    }

    fn push_json_event(&mut self, payload: serde_json::Value) {
        let Ok(bytes) = serde_json::to_vec(&payload) else {
            return;
        };
        while self.pending_json_events.len() >= MAX_PENDING_JSON_EVENTS {
            self.pending_json_events.pop_front();
        }
        self.pending_json_events.push_back(bytes);
    }

    fn push_analysis_frame(&mut self, frame: Vec<u8>) {
        if frame.is_empty() {
            return;
        }
        while self.pending_analysis_frames.len() >= MAX_PENDING_ANALYSIS_FRAMES {
            self.pending_analysis_frames.pop_front();
        }
        self.pending_analysis_frames.push_back(frame);
    }

    fn send_json_command_line(&mut self, line: &str) -> Result<(), String> {
        let cmd = parse_json_command(line)?;
        if let Some(cmd) = cmd {
            self.bridge.command(cmd);
            self.force_json_snapshot_emit = true;
        }
        Ok(())
    }

    fn poll(&mut self, max_events: usize) {
        if self.stopped {
            return;
        }

        let mut snapshots: Vec<BridgeSnapshot> = Vec::new();
        for _ in 0..max_events.max(1) {
            let event = self.bridge.try_recv();
            let Some(event) = event else {
                break;
            };
            match event {
                BridgeEvent::Snapshot(snapshot) => snapshots.push(*snapshot),
                BridgeEvent::Error(message) => {
                    self.push_json_event(json!({ "event": "error", "message": message }));
                }
                BridgeEvent::Stopped => {
                    self.stopped = true;
                    self.push_json_event(json!({ "event": "stopped" }));
                }
            }
        }

        for snapshot in snapshots {
            let analysis_delta = compute_analysis_delta(&snapshot, &mut self.emit_state);
            let analysis_frame = encode_analysis_frame(&analysis_delta);
            self.push_analysis_frame(analysis_frame);
            let summary = snapshot_summary(&snapshot);
            let summary_changed = self.last_json_summary.as_ref() != Some(&summary);
            let emit_due = self.last_json_snapshot_emit.elapsed() >= self.json_snapshot_interval;
            if self.force_json_snapshot_emit || summary_changed || emit_due {
                let payload = encode_snapshot_payload(
                    &snapshot,
                    &analysis_delta,
                    &mut self.emit_state,
                    false,
                );
                self.push_json_event(payload);
                self.last_json_snapshot_emit = Instant::now();
                self.last_json_summary = Some(summary);
                self.force_json_snapshot_emit = false;
            }
        }
    }

    fn pop_json_event(&mut self) -> Option<Vec<u8>> {
        self.pending_json_events.pop_front()
    }

    fn pop_analysis_frame(&mut self) -> Option<Vec<u8>> {
        self.pending_analysis_frames.pop_front()
    }
}

#[repr(C)]
pub struct FerrousFfiBridge {
    runtime: Mutex<FfiRuntime>,
}

#[no_mangle]
pub extern "C" fn ferrous_ffi_bridge_create() -> *mut FerrousFfiBridge {
    Box::into_raw(Box::new(FerrousFfiBridge {
        runtime: Mutex::new(FfiRuntime::new()),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_destroy(handle: *mut FerrousFfiBridge) {
    if handle.is_null() {
        return;
    }
    drop(Box::from_raw(handle));
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_send_json(
    handle: *mut FerrousFfiBridge,
    cmd_json: *const c_char,
) -> bool {
    if handle.is_null() || cmd_json.is_null() {
        return false;
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.runtime.lock() else {
        return false;
    };
    let line = CStr::from_ptr(cmd_json).to_string_lossy();
    let line = line.trim();
    if line.is_empty() {
        return true;
    }
    match runtime.send_json_command_line(line) {
        Ok(()) => true,
        Err(message) => {
            runtime.push_json_event(json!({ "event": "error", "message": message }));
            false
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_poll(
    handle: *mut FerrousFfiBridge,
    max_events: u32,
) -> bool {
    if handle.is_null() {
        return false;
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.runtime.lock() else {
        return false;
    };
    runtime.poll(max_events as usize);
    !runtime.pending_json_events.is_empty() || !runtime.pending_analysis_frames.is_empty()
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_json_event(
    handle: *mut FerrousFfiBridge,
) -> *mut c_char {
    if handle.is_null() {
        return std::ptr::null_mut();
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.runtime.lock() else {
        return std::ptr::null_mut();
    };
    let Some(bytes) = runtime.pop_json_event() else {
        return std::ptr::null_mut();
    };
    let sanitized: Vec<u8> = bytes.into_iter().filter(|b| *b != 0).collect();
    let Ok(cstring) = CString::new(sanitized) else {
        return std::ptr::null_mut();
    };
    cstring.into_raw()
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_free_json_event(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    drop(CString::from_raw(ptr));
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_analysis_frame(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if handle.is_null() {
        return std::ptr::null_mut();
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.runtime.lock() else {
        return std::ptr::null_mut();
    };
    let Some(frame) = runtime.pop_analysis_frame() else {
        return std::ptr::null_mut();
    };
    let mut boxed = frame.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    if !len_out.is_null() {
        *len_out = len;
    }
    ptr
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_free_analysis_frame(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[derive(Debug, Deserialize)]
struct JsonCommand {
    cmd: String,
    value: Option<f64>,
    from: Option<f64>,
    to: Option<f64>,
    paths: Option<Vec<String>>,
    path: Option<String>,
    artist: Option<String>,
    album: Option<String>,
}

fn parse_json_command(line: &str) -> Result<Option<BridgeCommand>, String> {
    let parsed: JsonCommand =
        serde_json::from_str(line).map_err(|err| format!("invalid json command: {err}"))?;

    let out = match parsed.cmd.as_str() {
        "play" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Play)),
        "pause" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Pause)),
        "stop" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Stop)),
        "next" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Next)),
        "prev" => Some(BridgeCommand::Playback(BridgePlaybackCommand::Previous)),
        "set_volume" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_volume requires numeric field 'value'".to_string())?;
            Some(BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(
                value as f32,
            )))
        }
        "set_repeat_mode" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_repeat_mode requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_repeat_mode value must be a finite number".to_string());
            }
            let mode = match value as i32 {
                1 => RepeatMode::One,
                2 => RepeatMode::All,
                _ => RepeatMode::Off,
            };
            Some(BridgeCommand::Playback(
                BridgePlaybackCommand::SetRepeatMode(mode),
            ))
        }
        "set_shuffle" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_shuffle requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_shuffle value must be a finite number".to_string());
            }
            Some(BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(
                value != 0.0,
            )))
        }
        "set_db_range" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_db_range requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_db_range value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(
                value as f32,
            )))
        }
        "set_log_scale" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_log_scale requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_log_scale value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(
                value != 0.0,
            )))
        }
        "set_show_fps" => {
            let value = parsed
                .value
                .ok_or_else(|| "set_show_fps requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("set_show_fps value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(
                value != 0.0,
            )))
        }
        "set_spectrogram_offset_ms" => {
            let value = parsed.value.ok_or_else(|| {
                "set_spectrogram_offset_ms requires numeric field 'value'".to_string()
            })?;
            if !value.is_finite() {
                return Err("set_spectrogram_offset_ms value must be a finite number".to_string());
            }
            Some(BridgeCommand::Settings(
                BridgeSettingsCommand::SetSpectrogramOffsetMs(value.round() as i32),
            ))
        }
        "set_spectrogram_lookahead_ms" => {
            let value = parsed.value.ok_or_else(|| {
                "set_spectrogram_lookahead_ms requires numeric field 'value'".to_string()
            })?;
            if !value.is_finite() {
                return Err(
                    "set_spectrogram_lookahead_ms value must be a finite number".to_string()
                );
            }
            Some(BridgeCommand::Settings(
                BridgeSettingsCommand::SetSpectrogramLookaheadMs(value.round() as i32),
            ))
        }
        "seek" => {
            let value = parsed
                .value
                .ok_or_else(|| "seek requires numeric field 'value'".to_string())?;
            if value < 0.0 {
                return Err("seek value must be >= 0".to_string());
            }
            Some(BridgeCommand::Playback(BridgePlaybackCommand::Seek(
                Duration::from_secs_f64(value),
            )))
        }
        "play_at" => {
            let value = parsed
                .value
                .ok_or_else(|| "play_at requires numeric field 'value'".to_string())?;
            if value < 0.0 || !value.is_finite() {
                return Err("play_at value must be a non-negative number".to_string());
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::PlayAt(
                value as usize,
            )))
        }
        "select_queue" => {
            let value = parsed
                .value
                .ok_or_else(|| "select_queue requires numeric field 'value'".to_string())?;
            if !value.is_finite() {
                return Err("select_queue value must be a number".to_string());
            }
            let selected = if value < 0.0 {
                None
            } else {
                Some(value as usize)
            };
            Some(BridgeCommand::Queue(BridgeQueueCommand::Select(selected)))
        }
        "remove_at" => {
            let value = parsed
                .value
                .ok_or_else(|| "remove_at requires numeric field 'value'".to_string())?;
            if value < 0.0 || !value.is_finite() {
                return Err("remove_at value must be a non-negative number".to_string());
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::Remove(
                value as usize,
            )))
        }
        "move_queue" => {
            let from = parsed
                .from
                .ok_or_else(|| "move_queue requires numeric field 'from'".to_string())?;
            let to = parsed
                .to
                .ok_or_else(|| "move_queue requires numeric field 'to'".to_string())?;
            if !from.is_finite() || !to.is_finite() || from < 0.0 || to < 0.0 {
                return Err(
                    "move_queue fields 'from' and 'to' must be non-negative numbers".to_string(),
                );
            }
            Some(BridgeCommand::Queue(BridgeQueueCommand::Move {
                from: from as usize,
                to: to as usize,
            }))
        }
        "replace_album" => {
            let paths = parsed
                .paths
                .ok_or_else(|| "replace_album requires array field 'paths'".to_string())?;
            let items: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceWithAlbum(items),
            ))
        }
        "append_album" => {
            let paths = parsed
                .paths
                .ok_or_else(|| "append_album requires array field 'paths'".to_string())?;
            let items: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
            Some(BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(
                items,
            )))
        }
        "replace_album_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "replace_album_by_key requires string field 'artist'".to_string())?;
            let album = parsed
                .album
                .ok_or_else(|| "replace_album_by_key requires string field 'album'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceAlbumByKey { artist, album },
            ))
        }
        "append_album_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "append_album_by_key requires string field 'artist'".to_string())?;
            let album = parsed
                .album
                .ok_or_else(|| "append_album_by_key requires string field 'album'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::AppendAlbumByKey { artist, album },
            ))
        }
        "replace_artist_by_key" => {
            let artist = parsed.artist.ok_or_else(|| {
                "replace_artist_by_key requires string field 'artist'".to_string()
            })?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::ReplaceArtistByKey { artist },
            ))
        }
        "append_artist_by_key" => {
            let artist = parsed
                .artist
                .ok_or_else(|| "append_artist_by_key requires string field 'artist'".to_string())?;
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::AppendArtistByKey { artist },
            ))
        }
        "add_track" => {
            let path = parsed
                .path
                .ok_or_else(|| "add_track requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::AddTrack(
                PathBuf::from(path),
            )))
        }
        "play_track" => {
            let path = parsed
                .path
                .ok_or_else(|| "play_track requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(
                PathBuf::from(path),
            )))
        }
        "scan_root" => {
            let path = parsed
                .path
                .ok_or_else(|| "scan_root requires string field 'path'".to_string())?;
            Some(BridgeCommand::Library(BridgeLibraryCommand::ScanRoot(
                PathBuf::from(path),
            )))
        }
        "clear_queue" => Some(BridgeCommand::Queue(BridgeQueueCommand::Clear)),
        "request_snapshot" => Some(BridgeCommand::RequestSnapshot),
        "shutdown" => Some(BridgeCommand::Shutdown),
        _ => return Err(format!("unknown command '{}'", parsed.cmd)),
    };
    Ok(out)
}

fn leading_track_number(input: &str) -> Option<u32> {
    let mut n: u32 = 0;
    let mut saw_digit = false;
    for ch in input.chars() {
        if let Some(d) = ch.to_digit(10) {
            saw_digit = true;
            n = n.saturating_mul(10).saturating_add(d);
        } else {
            break;
        }
    }
    if saw_digit {
        Some(n)
    } else {
        None
    }
}

fn compute_queue_total_duration(snapshot: &BridgeSnapshot) -> (f64, usize) {
    let mut duration_by_path: HashMap<&std::path::Path, f64> =
        HashMap::with_capacity(snapshot.library.tracks.len());
    for track in &snapshot.library.tracks {
        let Some(duration_secs) = track.duration_secs else {
            continue;
        };
        let duration = f64::from(duration_secs);
        if duration.is_finite() && duration > 0.0 {
            duration_by_path.insert(track.path.as_path(), duration);
        }
    }

    let mut total_duration_secs = 0.0;
    let mut unknown_duration_count = 0usize;
    for path in &snapshot.queue {
        if let Some(duration) = duration_by_path.get(path.as_path()) {
            total_duration_secs += *duration;
        } else {
            unknown_duration_count = unknown_duration_count.saturating_add(1);
        }
    }

    (total_duration_secs, unknown_duration_count)
}

fn encode_snapshot_payload(
    s: &BridgeSnapshot,
    analysis_delta: &AnalysisDelta,
    emit_state: &mut JsonEmitState,
    include_analysis_payload: bool,
) -> serde_json::Value {
    let library_digest = LibraryDigest {
        roots_len: s.library.roots.len(),
        tracks_len: s.library.tracks.len(),
        scan_in_progress: s.library.scan_in_progress,
        first_root: s
            .library
            .roots
            .first()
            .map(|p| p.to_string_lossy().to_string()),
        last_root: s
            .library
            .roots
            .last()
            .map(|p| p.to_string_lossy().to_string()),
        first_track: s
            .library
            .tracks
            .first()
            .map(|t| t.path.to_string_lossy().to_string()),
        last_track: s
            .library
            .tracks
            .last()
            .map(|t| t.path.to_string_lossy().to_string()),
    };
    let albums_changed = emit_state.last_library_digest.as_ref() != Some(&library_digest);
    let should_emit_albums =
        albums_changed && (!s.library.scan_in_progress || emit_state.last_library_digest.is_none());
    emit_state.last_library_digest = Some(library_digest);
    let library_albums = if should_emit_albums {
        let mut grouped: BTreeMap<(String, String), Vec<(u8, u32, String, String)>> =
            BTreeMap::new();
        for track in &s.library.tracks {
            let album = if track.album.trim().is_empty() {
                String::from("Unknown Album")
            } else {
                track.album.clone()
            };
            let artist = if track.artist.trim().is_empty() {
                String::from("Unknown Artist")
            } else {
                track.artist.clone()
            };
            let title = if track.title.trim().is_empty() {
                track.path.file_stem().map_or_else(
                    || track.path.to_string_lossy().to_string(),
                    |s| s.to_string_lossy().into_owned(),
                )
            } else {
                track.title.clone()
            };
            let fallback_number = leading_track_number(title.trim_start()).or_else(|| {
                track
                    .path
                    .file_stem()
                    .and_then(|s| leading_track_number(&s.to_string_lossy()))
            });
            let rank = if track.track_no.is_some() {
                0
            } else if fallback_number.is_some() {
                1
            } else {
                2
            };
            let sort_number = track.track_no.or(fallback_number).unwrap_or(u32::MAX);
            let path_string = track.path.to_string_lossy().to_string();
            grouped.entry((artist, album)).or_default().push((
                rank,
                sort_number,
                title,
                path_string,
            ));
        }
        serde_json::Value::Array(
            grouped
                .into_iter()
                .map(|((artist, album), mut tracks)| {
                    tracks.sort_by(
                        |(a_rank, a_no, a_title, a_path), (b_rank, b_no, b_title, b_path)| {
                            a_rank
                                .cmp(b_rank)
                                .then_with(|| a_no.cmp(b_no))
                                .then_with(|| a_path.cmp(b_path))
                                .then_with(|| a_title.cmp(b_title))
                        },
                    );
                    let count = tracks.len();
                    let track_items: Vec<serde_json::Value> = tracks
                        .into_iter()
                        .map(|(_, _, title, path)| {
                            json!({
                                "title": title,
                                "path": path,
                            })
                        })
                        .collect();
                    json!({
                        "artist": artist,
                        "name": album,
                        "count": count,
                        "tracks": track_items,
                    })
                })
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };

    let queue_digest = QueueDigest {
        len: s.queue.len(),
        selected: s.selected_queue_index,
        first: s.queue.first().map(|p| p.to_string_lossy().to_string()),
        last: s.queue.last().map(|p| p.to_string_lossy().to_string()),
    };
    let queue_changed = emit_state.last_queue_digest.as_ref() != Some(&queue_digest);
    let queue_tracks = if queue_changed {
        emit_state.last_queue_digest = Some(queue_digest);
        serde_json::Value::Array(
            s.queue
                .iter()
                .map(|path| {
                    let title = path.file_name().map_or_else(
                        || path.to_string_lossy().into_owned(),
                        |n| n.to_string_lossy().into_owned(),
                    );
                    json!({
                        "path": path.to_string_lossy().to_string(),
                        "title": title,
                    })
                })
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };
    let (queue_total_duration_secs, queue_unknown_duration_count) =
        if queue_changed || albums_changed {
            let (total_duration_secs, unknown_duration_count) = compute_queue_total_duration(s);
            emit_state.last_queue_total_duration_secs = total_duration_secs;
            emit_state.last_queue_unknown_duration_count = unknown_duration_count;
            (total_duration_secs, unknown_duration_count)
        } else {
            (
                emit_state.last_queue_total_duration_secs,
                emit_state.last_queue_unknown_duration_count,
            )
        };

    let waveform_peaks = if include_analysis_payload && analysis_delta.waveform_changed {
        serde_json::Value::Array(
            analysis_delta
                .waveform_peaks_u8
                .iter()
                .map(|v| json!((*v as f64) / 255.0))
                .collect(),
        )
    } else {
        serde_json::Value::Null
    };
    let spectrogram_rows =
        if include_analysis_payload && !analysis_delta.spectrogram_rows_u8.is_empty() {
            serde_json::Value::Array(
                analysis_delta
                    .spectrogram_rows_u8
                    .iter()
                    .map(|row| serde_json::Value::Array(row.iter().map(|v| json!(v)).collect()))
                    .collect(),
            )
        } else {
            serde_json::Value::Null
        };
    let current_queue_index = s
        .playback
        .current_queue_index
        .filter(|idx| *idx < s.queue.len());

    json!({
        "event": "snapshot",
        "playback": {
            "state": format!("{:?}", s.playback.state),
            "position_secs": s.playback.position.as_secs_f64(),
            "duration_secs": s.playback.duration.as_secs_f64(),
            "volume": s.playback.volume,
            "repeat_mode": match s.playback.repeat_mode {
                RepeatMode::Off => 0,
                RepeatMode::One => 1,
                RepeatMode::All => 2,
            },
            "shuffle_enabled": s.playback.shuffle_enabled,
            "has_current": s.playback.current.is_some(),
            "current_path": s.playback.current.as_ref().map(|path| path.to_string_lossy().to_string()),
            "current_queue_index": current_queue_index,
        },
        "queue": {
            "len": s.queue.len(),
            "selected_index": s.selected_queue_index,
            "total_duration_secs": queue_total_duration_secs,
            "unknown_duration_count": queue_unknown_duration_count,
            "tracks": queue_tracks,
        },
        "library": {
            "roots": s.library.roots.len(),
            "tracks": s.library.tracks.len(),
            "scan_in_progress": s.library.scan_in_progress,
            "albums_changed": should_emit_albums,
            "albums": library_albums,
        },
        "metadata": {
            "title": s.metadata.title.clone(),
            "artist": s.metadata.artist.clone(),
            "album": s.metadata.album.clone(),
            "sample_rate_hz": s.metadata.sample_rate_hz,
            "bitrate_kbps": s.metadata.bitrate_kbps,
            "channels": s.metadata.channels,
            "bit_depth": s.metadata.bit_depth,
        },
        "analysis": {
            "spectrogram_seq": analysis_delta.spectrogram_seq,
            "spectrogram_reset": include_analysis_payload && analysis_delta.spectrogram_reset,
            "spectrogram_rows": spectrogram_rows,
            "sample_rate_hz": if include_analysis_payload { analysis_delta.sample_rate_hz } else { 0 },
            "waveform_len": if include_analysis_payload { analysis_delta.waveform_len } else { 0 },
            "waveform_changed": include_analysis_payload && analysis_delta.waveform_changed,
            "spectrogram_lag_estimate_ms": analysis_delta.spectrogram_lag_estimate_ms,
            "spectrogram_fifo_delay_ms": analysis_delta.spectrogram_fifo_delay_ms,
            "spectrogram_stft_pending_ms": analysis_delta.spectrogram_stft_pending_ms,
            "spectrogram_window_center_ms": analysis_delta.spectrogram_window_center_ms,
            "spectrogram_target_delay_ms": analysis_delta.spectrogram_target_delay_ms,
            "waveform_peaks": waveform_peaks,
        },
        "settings": {
            "volume": s.settings.volume,
            "fft_size": s.settings.fft_size,
            "spectrogram_offset_ms": s.settings.spectrogram_offset_ms,
            "spectrogram_lookahead_ms": s.settings.spectrogram_lookahead_ms,
            "db_range": s.settings.db_range,
            "log_scale": s.settings.log_scale,
            "show_fps": s.settings.show_fps,
        }
    })
}

fn compute_analysis_delta(s: &BridgeSnapshot, emit_state: &mut JsonEmitState) -> AnalysisDelta {
    let waveform_changed = s.analysis.waveform_peaks != emit_state.last_waveform_peaks;
    let waveform_peaks_u8 = if waveform_changed {
        emit_state.last_waveform_peaks = s.analysis.waveform_peaks.clone();
        downsample_waveform_peaks(&s.analysis.waveform_peaks, 1024)
            .into_iter()
            .map(to_u8_norm)
            .collect()
    } else {
        Vec::new()
    };

    let spectrogram_reset = s.analysis.spectrogram_seq < emit_state.last_spectrogram_seq
        || (s.analysis.spectrogram_seq == 0
            && s.analysis.spectrogram_rows.is_empty()
            && emit_state.last_spectrogram_seq > 0);
    let spectrogram_seq = s.analysis.spectrogram_seq;
    let spectrogram_delta =
        spectrogram_seq.saturating_sub(emit_state.last_spectrogram_seq) as usize;
    let spectrogram_rows_u8 = if spectrogram_delta > 0 && !s.analysis.spectrogram_rows.is_empty() {
        let tail = spectrogram_delta.min(s.analysis.spectrogram_rows.len());
        let start = s.analysis.spectrogram_rows.len().saturating_sub(tail);
        s.analysis.spectrogram_rows[start..]
            .iter()
            .map(|row| {
                row.iter()
                    .map(|v| to_u8_spectrum(*v, s.settings.db_range))
                    .collect::<Vec<u8>>()
            })
            .collect()
    } else {
        Vec::new()
    };
    emit_state.last_spectrogram_seq = spectrogram_seq;
    let has_payload = waveform_changed || spectrogram_reset || !spectrogram_rows_u8.is_empty();
    if has_payload {
        emit_state.analysis_frame_seq = emit_state.analysis_frame_seq.wrapping_add(1);
    }

    AnalysisDelta {
        sample_rate_hz: s.analysis.sample_rate_hz,
        frame_seq: emit_state.analysis_frame_seq,
        spectrogram_seq,
        spectrogram_reset,
        waveform_len: s.analysis.waveform_peaks.len(),
        waveform_changed,
        spectrogram_lag_estimate_ms: s.analysis.spectrogram_lag_estimate_ms,
        spectrogram_fifo_delay_ms: s.analysis.spectrogram_fifo_delay_ms,
        spectrogram_stft_pending_ms: s.analysis.spectrogram_stft_pending_ms,
        spectrogram_window_center_ms: s.analysis.spectrogram_window_center_ms,
        spectrogram_target_delay_ms: s.analysis.spectrogram_target_delay_ms,
        waveform_peaks_u8,
        spectrogram_rows_u8,
    }
}

fn snapshot_summary(s: &BridgeSnapshot) -> JsonSnapshotSummary {
    JsonSnapshotSummary {
        playback_state: s.playback.state,
        playback_current: s.playback.current.clone(),
        playback_repeat_mode: s.playback.repeat_mode,
        playback_shuffle_enabled: s.playback.shuffle_enabled,
        queue_len: s.queue.len(),
        queue_selected: s.selected_queue_index,
        queue_first: s.queue.first().cloned(),
        queue_last: s.queue.last().cloned(),
        library_roots: s.library.roots.len(),
        library_tracks: s.library.tracks.len(),
        library_scan_in_progress: s.library.scan_in_progress,
    }
}

fn to_u8_norm(v: f32) -> u8 {
    let clamped = v.clamp(0.0, 1.0);
    (clamped * 255.0).round() as u8
}

fn to_u8_spectrum(v: f32, db_range: f32) -> u8 {
    let range = db_range.clamp(50.0, 120.0) as f64;
    let db = if v > 0.0 {
        (10.0 / std::f64::consts::LN_10) * (v as f64).ln()
    } else {
        -200.0
    };
    let xdb = (db + range - 63.0).clamp(0.0, range);
    ((xdb / range) * 255.0).round().clamp(0.0, 255.0) as u8
}

fn encode_analysis_frame(delta: &AnalysisDelta) -> Vec<u8> {
    let waveform_len = delta.waveform_peaks_u8.len();
    let row_count = delta.spectrogram_rows_u8.len();
    let bin_count = delta
        .spectrogram_rows_u8
        .first()
        .map_or(0, std::vec::Vec::len);
    let has_spectrogram = row_count > 0 && bin_count > 0;

    let mut flags = 0u8;
    if delta.waveform_changed && waveform_len > 0 {
        flags |= ANALYSIS_FLAG_WAVEFORM;
    }
    if delta.spectrogram_reset {
        flags |= ANALYSIS_FLAG_RESET;
    }
    if has_spectrogram {
        flags |= ANALYSIS_FLAG_SPECTROGRAM;
    }

    if flags == 0 {
        return Vec::new();
    }

    let waveform_len_u16 = waveform_len.min(u16::MAX as usize) as u16;
    let row_count_u16 = row_count.min(u16::MAX as usize) as u16;
    let bin_count_u16 = bin_count.min(u16::MAX as usize) as u16;
    let spectrogram_bytes = row_count_u16 as usize * bin_count_u16 as usize;
    let payload_len = 16usize + waveform_len_u16 as usize + spectrogram_bytes;

    let mut out = Vec::with_capacity(4 + payload_len);
    out.extend_from_slice(&(payload_len as u32).to_le_bytes());
    out.push(ANALYSIS_FRAME_MAGIC);
    out.extend_from_slice(&delta.sample_rate_hz.to_le_bytes());
    out.push(flags);
    out.extend_from_slice(&waveform_len_u16.to_le_bytes());
    out.extend_from_slice(&row_count_u16.to_le_bytes());
    out.extend_from_slice(&bin_count_u16.to_le_bytes());
    out.extend_from_slice(&delta.frame_seq.to_le_bytes());

    if (flags & ANALYSIS_FLAG_WAVEFORM) != 0 {
        out.extend_from_slice(&delta.waveform_peaks_u8[..waveform_len_u16 as usize]);
    }
    if (flags & ANALYSIS_FLAG_SPECTROGRAM) != 0 {
        for row in delta
            .spectrogram_rows_u8
            .iter()
            .take(row_count_u16 as usize)
        {
            out.extend_from_slice(&row[..bin_count_u16 as usize]);
        }
    }

    out
}

fn downsample_waveform_peaks(peaks: &[f32], max_points: usize) -> Vec<f32> {
    if peaks.len() <= max_points || max_points == 0 {
        return peaks.to_vec();
    }
    let mut out = Vec::with_capacity(max_points);
    for i in 0..max_points {
        let start = i * peaks.len() / max_points;
        let mut end = (i + 1) * peaks.len() / max_points;
        if end <= start {
            end = (start + 1).min(peaks.len());
        }
        let mut peak = 0.0f32;
        for &v in &peaks[start..end] {
            if v > peak {
                peak = v;
            }
        }
        out.push(peak);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::analysis::AnalysisSnapshot;
    use crate::library::{LibrarySnapshot, LibraryTrack};
    use crate::playback::{PlaybackSnapshot, PlaybackState};
    use std::ffi::{CStr, CString};
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;
    use std::time::Instant;

    #[test]
    fn parse_json_command_supports_settings_updates() {
        let cmd = parse_json_command(r#"{"cmd":"set_db_range","value":88}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(v)) => {
                assert!((v - 88.0).abs() < 0.001);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_log_scale","value":1}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(v)) => {
                assert!(v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_show_fps","value":1}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(v)) => {
                assert!(v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_spectrogram_offset_ms","value":-28}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetSpectrogramOffsetMs(v)) => {
                assert_eq!(v, -28);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_spectrogram_lookahead_ms","value":52}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetSpectrogramLookaheadMs(v)) => {
                assert_eq!(v, 52);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_repeat_mode","value":2}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Playback(BridgePlaybackCommand::SetRepeatMode(mode)) => {
                assert!(matches!(mode, RepeatMode::All));
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"set_shuffle","value":1}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(enabled)) => {
                assert!(enabled);
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_json_command_supports_library_batch_commands() {
        let cmd = parse_json_command(
            r#"{"cmd":"replace_album","paths":["/music/a.flac","/music/b.flac"]}"#,
        )
        .expect("parse")
        .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceWithAlbum(paths)) => {
                assert_eq!(
                    paths,
                    vec![
                        PathBuf::from("/music/a.flac"),
                        PathBuf::from("/music/b.flac")
                    ]
                );
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"append_album","paths":["/music/c.flac"]}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(paths)) => {
                assert_eq!(paths, vec![PathBuf::from("/music/c.flac")]);
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_json_command_supports_library_track_and_scan_commands() {
        let cmd = parse_json_command(r#"{"cmd":"play_track","path":"/music/track.flac"}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(path)) => {
                assert_eq!(path, PathBuf::from("/music/track.flac"));
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_json_command(r#"{"cmd":"scan_root","path":"/home/user/Music"}"#)
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::ScanRoot(path)) => {
                assert_eq!(path, PathBuf::from("/home/user/Music"));
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_json_command_rejects_invalid_seek() {
        let err = parse_json_command(r#"{"cmd":"seek","value":-1}"#).unwrap_err();
        assert!(err.contains("seek value must be >= 0"));
    }

    fn sample_snapshot() -> BridgeSnapshot {
        BridgeSnapshot {
            playback: PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_secs(12),
                duration: Duration::from_secs(180),
                current: Some(PathBuf::from("/music/a.flac")),
                current_queue_index: Some(0),
                volume: 0.75,
                repeat_mode: RepeatMode::Off,
                shuffle_enabled: false,
            },
            analysis: AnalysisSnapshot {
                waveform_peaks: vec![0.1, 0.5, 0.9],
                spectrogram_rows: vec![vec![0.0, 1.0], vec![2.0, 3.0]],
                spectrogram_seq: 2,
                sample_rate_hz: 48_000,
                spectrogram_lag_estimate_ms: 96.0,
                spectrogram_fifo_delay_ms: 44.0,
                spectrogram_stft_pending_ms: 31.0,
                spectrogram_window_center_ms: 21.0,
                spectrogram_target_delay_ms: 40.0,
            },
            metadata: crate::metadata::TrackMetadata {
                title: "Sample Track".to_string(),
                artist: "Sample Artist".to_string(),
                album: "Sample Album".to_string(),
                sample_rate_hz: Some(48_000),
                bitrate_kbps: Some(320),
                channels: Some(2),
                bit_depth: Some(24),
                cover_art_rgba: None,
            },
            library: Arc::new(LibrarySnapshot {
                tracks: vec![LibraryTrack {
                    path: PathBuf::from("/music/a.flac"),
                    title: "Sample Track".to_string(),
                    artist: "Sample Artist".to_string(),
                    album: "Sample Album".to_string(),
                    track_no: Some(1),
                    duration_secs: Some(180.0),
                }],
                ..LibrarySnapshot::default()
            }),
            queue: vec![PathBuf::from("/music/a.flac")],
            selected_queue_index: Some(0),
            settings: super::super::BridgeSettings {
                volume: 0.75,
                fft_size: 2048,
                spectrogram_offset_ms: 0,
                spectrogram_lookahead_ms: 0,
                db_range: 90.0,
                log_scale: false,
                show_fps: false,
            },
        }
    }

    #[test]
    fn snapshot_payload_contract_has_expected_shape() {
        let snapshot = sample_snapshot();
        let mut emit_state = JsonEmitState::default();
        let analysis_delta = compute_analysis_delta(&snapshot, &mut emit_state);
        let payload = encode_snapshot_payload(&snapshot, &analysis_delta, &mut emit_state, false);
        assert_eq!(
            payload.get("event").and_then(|v| v.as_str()),
            Some("snapshot")
        );
        assert!(payload
            .get("playback")
            .and_then(|v| v.as_object())
            .is_some());
        assert!(payload.get("queue").and_then(|v| v.as_object()).is_some());
        assert!(payload.get("library").and_then(|v| v.as_object()).is_some());
        assert!(payload
            .get("metadata")
            .and_then(|v| v.as_object())
            .is_some());
        assert!(payload
            .get("settings")
            .and_then(|v| v.as_object())
            .is_some());
        assert_eq!(
            payload
                .get("queue")
                .and_then(|v| v.get("total_duration_secs"))
                .and_then(|v| v.as_f64()),
            Some(180.0)
        );
        assert_eq!(
            payload
                .get("queue")
                .and_then(|v| v.get("unknown_duration_count"))
                .and_then(|v| v.as_u64()),
            Some(0)
        );
    }

    #[test]
    fn analysis_delta_and_frame_include_changes() {
        let snapshot = sample_snapshot();
        let mut emit_state = JsonEmitState::default();
        let delta = compute_analysis_delta(&snapshot, &mut emit_state);
        assert!(delta.waveform_changed);
        assert!(!delta.spectrogram_rows_u8.is_empty());
        let frame = encode_analysis_frame(&delta);
        assert!(!frame.is_empty());
        assert_eq!(frame[4], ANALYSIS_FRAME_MAGIC);
    }

    fn ffi_send_json(handle: *mut FerrousFfiBridge, cmd: &str) -> bool {
        let c = CString::new(cmd).expect("CString");
        // SAFETY: `handle` comes from `ferrous_ffi_bridge_create`, and `c` lives across the call.
        unsafe { ferrous_ffi_bridge_send_json(handle, c.as_ptr()) }
    }

    fn ffi_next_event(
        handle: *mut FerrousFfiBridge,
        timeout: Duration,
    ) -> Option<serde_json::Value> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            // SAFETY: `handle` comes from `ferrous_ffi_bridge_create` for test lifetime.
            unsafe {
                ferrous_ffi_bridge_poll(handle, 64);
                let ptr = ferrous_ffi_bridge_pop_json_event(handle);
                if !ptr.is_null() {
                    let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
                    ferrous_ffi_bridge_free_json_event(ptr);
                    if let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) {
                        return Some(value);
                    }
                }
            }
            thread::sleep(Duration::from_millis(10));
        }
        None
    }

    fn ffi_wait_event_kind(
        handle: *mut FerrousFfiBridge,
        kind: &str,
        timeout: Duration,
    ) -> Option<serde_json::Value> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            let remaining = deadline.saturating_duration_since(Instant::now());
            let Some(evt) = ffi_next_event(handle, remaining.min(Duration::from_millis(100)))
            else {
                continue;
            };
            if evt.get("event").and_then(|v| v.as_str()) == Some(kind) {
                return Some(evt);
            }
        }
        None
    }

    #[test]
    fn ffi_bridge_emits_snapshot_event_end_to_end() {
        // SAFETY: creating and destroying handle in this test scope.
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());

        let snapshot_evt = ffi_wait_event_kind(handle, "snapshot", Duration::from_secs(4))
            .expect("snapshot event");
        assert!(snapshot_evt.get("playback").is_some());
        assert!(snapshot_evt.get("queue").is_some());
        assert!(snapshot_evt.get("settings").is_some());

        assert!(ffi_send_json(handle, r#"{"cmd":"shutdown"}"#));
        let stopped = ffi_wait_event_kind(handle, "stopped", Duration::from_secs(3));
        assert!(stopped.is_some());
        // SAFETY: paired with create and not used after destroy.
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }

    #[test]
    fn ffi_bridge_reports_error_for_bad_command_end_to_end() {
        // SAFETY: creating and destroying handle in this test scope.
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());

        assert!(!ffi_send_json(handle, r#"{"cmd":"seek","value":-1}"#));
        let error_evt =
            ffi_wait_event_kind(handle, "error", Duration::from_secs(3)).expect("error event");
        let message = error_evt
            .get("message")
            .and_then(|v| v.as_str())
            .unwrap_or_default();
        assert!(message.contains("seek value must be >= 0"));

        assert!(ffi_send_json(handle, r#"{"cmd":"shutdown"}"#));
        let _ = ffi_wait_event_kind(handle, "stopped", Duration::from_secs(3));
        // SAFETY: paired with create and not used after destroy.
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }
}
