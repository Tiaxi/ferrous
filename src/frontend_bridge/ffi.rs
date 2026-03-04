use std::collections::{HashMap, VecDeque};
use std::ffi::c_uchar;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use super::{
    BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSettingsCommand, BridgeSnapshot, FrontendBridgeHandle, LibrarySortMode,
};
use crate::playback::{PlaybackState, RepeatMode};

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_RESET: u8 = 0x02;
const ANALYSIS_FLAG_SPECTROGRAM: u8 = 0x04;
const MAX_PENDING_BINARY_EVENTS: usize = 12;
const MAX_PENDING_ANALYSIS_FRAMES: usize = 24;

const SNAPSHOT_MAGIC: u32 = 0xFE55_0001;
const SECTION_PLAYBACK: u16 = 1 << 0;
const SECTION_QUEUE: u16 = 1 << 1;
const SECTION_LIBRARY_META: u16 = 1 << 2;
const SECTION_LIBRARY_TREE: u16 = 1 << 3;
const SECTION_METADATA: u16 = 1 << 4;
const SECTION_SETTINGS: u16 = 1 << 5;
const SECTION_ERROR: u16 = 1 << 6;
const SECTION_STOPPED: u16 = 1 << 7;

fn read_env_millis(key: &str, fallback: u64) -> Duration {
    let millis = std::env::var(key)
        .ok()
        .and_then(|raw| raw.parse::<u64>().ok())
        .map_or(fallback, |v| v.clamp(8, 1000));
    Duration::from_millis(millis)
}

#[derive(Default)]
struct AnalysisDelta {
    sample_rate_hz: u32,
    frame_seq: u32,
    spectrogram_reset: bool,
    waveform_changed: bool,
    waveform_peaks_u8: Vec<u8>,
    spectrogram_rows_u8: Vec<Vec<u8>>,
}

#[derive(Default)]
struct AnalysisEmitState {
    last_waveform_peaks: Vec<f32>,
    last_spectrogram_seq: u64,
    analysis_frame_seq: u32,
}

#[derive(Default)]
struct QueueDurationCache {
    library_ptr: usize,
    queue_paths: Vec<PathBuf>,
    total_duration_secs: f64,
    unknown_duration_count: usize,
}

struct FfiRuntime {
    bridge: FrontendBridgeHandle,
    analysis_state: AnalysisEmitState,
    queue_duration_cache: QueueDurationCache,
    pending_binary_events: VecDeque<Vec<u8>>,
    pending_analysis_frames: VecDeque<Vec<u8>>,
    binary_emit_interval: Duration,
    scan_binary_emit_interval: Duration,
    last_binary_emit_at: Option<Instant>,
    stopped: bool,
}

impl FfiRuntime {
    fn new() -> Self {
        let bridge = FrontendBridgeHandle::spawn();
        let runtime = Self {
            bridge,
            analysis_state: AnalysisEmitState::default(),
            queue_duration_cache: QueueDurationCache::default(),
            pending_binary_events: VecDeque::with_capacity(MAX_PENDING_BINARY_EVENTS),
            pending_analysis_frames: VecDeque::with_capacity(MAX_PENDING_ANALYSIS_FRAMES),
            binary_emit_interval: read_env_millis("FERROUS_FFI_SNAPSHOT_MS", 24),
            scan_binary_emit_interval: read_env_millis("FERROUS_FFI_SCAN_SNAPSHOT_MS", 80),
            last_binary_emit_at: None,
            stopped: false,
        };
        runtime.bridge.command(BridgeCommand::RequestSnapshot);
        runtime
    }

    fn push_binary_event(&mut self, payload: Vec<u8>) {
        if payload.is_empty() {
            return;
        }
        while self.pending_binary_events.len() >= MAX_PENDING_BINARY_EVENTS {
            self.pending_binary_events.pop_front();
        }
        self.pending_binary_events.push_back(payload);
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

    fn send_binary_command(&mut self, payload: &[u8]) -> Result<(), String> {
        let cmd = parse_binary_command(payload)?;
        if let Some(cmd) = cmd {
            self.bridge.command(cmd);
        }
        Ok(())
    }

    fn poll(&mut self, max_events: usize) {
        if self.stopped {
            return;
        }

        let mut latest_snapshot: Option<BridgeSnapshot> = None;
        for _ in 0..max_events.max(1) {
            let event = self.bridge.try_recv();
            let Some(event) = event else {
                break;
            };
            match event {
                BridgeEvent::Snapshot(snapshot) => latest_snapshot = Some(*snapshot),
                BridgeEvent::Error(message) => {
                    self.push_binary_event(encode_error_event(&message));
                }
                BridgeEvent::Stopped => {
                    self.stopped = true;
                    self.push_binary_event(encode_stopped_event());
                }
            }
        }

        if let Some(snapshot) = latest_snapshot {
            let analysis_delta = compute_analysis_delta(&snapshot, &mut self.analysis_state);
            self.push_analysis_frame(encode_analysis_frame(&analysis_delta));
            if self.binary_emit_due(&snapshot) {
                let queue_duration = self.queue_duration_for_snapshot(&snapshot);
                self.push_binary_event(encode_binary_snapshot(&snapshot, queue_duration));
                self.last_binary_emit_at = Some(Instant::now());
            }
        }
    }

    fn pop_binary_event(&mut self) -> Option<Vec<u8>> {
        self.pending_binary_events.pop_front()
    }

    fn pop_analysis_frame(&mut self) -> Option<Vec<u8>> {
        self.pending_analysis_frames.pop_front()
    }

    fn binary_emit_due(&self, snapshot: &BridgeSnapshot) -> bool {
        if snapshot.pre_built_tree_bytes.is_some() {
            return true;
        }
        let interval = if snapshot.library.scan_in_progress {
            self.scan_binary_emit_interval
        } else {
            self.binary_emit_interval
        };
        self.last_binary_emit_at
            .map_or(true, |last| last.elapsed() >= interval)
    }

    fn queue_duration_for_snapshot(&mut self, snapshot: &BridgeSnapshot) -> (f64, usize) {
        if snapshot.queue.is_empty() {
            return (0.0, 0);
        }
        // Queue duration lookups against a rapidly mutating scan snapshot are expensive and
        // not essential for transport/visual responsiveness; skip until the scan settles.
        if snapshot.library.scan_in_progress {
            return (0.0, snapshot.queue.len());
        }

        let library_ptr = std::sync::Arc::as_ptr(&snapshot.library) as usize;
        if self.queue_duration_cache.library_ptr == library_ptr
            && self.queue_duration_cache.queue_paths == snapshot.queue
        {
            return (
                self.queue_duration_cache.total_duration_secs,
                self.queue_duration_cache.unknown_duration_count,
            );
        }

        let (total_duration_secs, unknown_duration_count) = compute_queue_total_duration(snapshot);
        self.queue_duration_cache.library_ptr = library_ptr;
        self.queue_duration_cache.queue_paths = snapshot.queue.clone();
        self.queue_duration_cache.total_duration_secs = total_duration_secs;
        self.queue_duration_cache.unknown_duration_count = unknown_duration_count;
        (total_duration_secs, unknown_duration_count)
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
pub unsafe extern "C" fn ferrous_ffi_bridge_send_binary(
    handle: *mut FerrousFfiBridge,
    cmd_ptr: *const c_uchar,
    cmd_len: usize,
) -> bool {
    if handle.is_null() || cmd_ptr.is_null() || cmd_len == 0 {
        return false;
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.runtime.lock() else {
        return false;
    };
    let payload = std::slice::from_raw_parts(cmd_ptr, cmd_len);
    match runtime.send_binary_command(payload) {
        Ok(()) => true,
        Err(message) => {
            runtime.push_binary_event(encode_error_event(&message));
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
    !runtime.pending_binary_events.is_empty() || !runtime.pending_analysis_frames.is_empty()
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_binary_event(
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
    let Some(bytes) = runtime.pop_binary_event() else {
        return std::ptr::null_mut();
    };
    let mut boxed = bytes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    if !len_out.is_null() {
        *len_out = len;
    }
    ptr
}

#[no_mangle]
pub unsafe extern "C" fn ferrous_ffi_bridge_free_binary_event(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
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

fn parse_binary_command(payload: &[u8]) -> Result<Option<BridgeCommand>, String> {
    if payload.len() < 4 {
        return Err("binary command payload too short".to_string());
    }

    let cmd_id = u16::from_le_bytes([payload[0], payload[1]]);
    let declared_len = u16::from_le_bytes([payload[2], payload[3]]) as usize;
    let actual_payload = &payload[4..];
    if actual_payload.len() != declared_len {
        return Err(format!(
            "binary command payload length mismatch: header={declared_len}, actual={}",
            actual_payload.len()
        ));
    }

    let mut reader = BinaryReader::new(actual_payload);
    let command = match cmd_id {
        1 => {
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::Play)
        }
        2 => {
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::Pause)
        }
        3 => {
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::Stop)
        }
        4 => {
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::Next)
        }
        5 => {
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::Previous)
        }
        6 => {
            let value = reader.read_f64()?;
            reader.expect_done()?;
            if !value.is_finite() {
                return Err("set_volume value must be finite".to_string());
            }
            BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(value as f32))
        }
        7 => {
            let value = reader.read_f64()?;
            reader.expect_done()?;
            if !value.is_finite() || value < 0.0 {
                return Err("seek value must be finite and >= 0".to_string());
            }
            BridgeCommand::Playback(BridgePlaybackCommand::Seek(Duration::from_secs_f64(value)))
        }
        8 => {
            let index = reader.read_u32()? as usize;
            reader.expect_done()?;
            BridgeCommand::Queue(BridgeQueueCommand::PlayAt(index))
        }
        9 => {
            let index = reader.read_i32()?;
            reader.expect_done()?;
            let selected = if index < 0 {
                None
            } else {
                Some(index as usize)
            };
            BridgeCommand::Queue(BridgeQueueCommand::Select(selected))
        }
        10 => {
            let index = reader.read_u32()? as usize;
            reader.expect_done()?;
            BridgeCommand::Queue(BridgeQueueCommand::Remove(index))
        }
        11 => {
            let from = reader.read_u32()? as usize;
            let to = reader.read_u32()? as usize;
            reader.expect_done()?;
            BridgeCommand::Queue(BridgeQueueCommand::Move { from, to })
        }
        12 => {
            reader.expect_done()?;
            BridgeCommand::Queue(BridgeQueueCommand::Clear)
        }
        13 => {
            let path = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::AddTrack(PathBuf::from(path)))
        }
        14 => {
            let path = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(PathBuf::from(path)))
        }
        15 => {
            let paths = reader.read_u16_string_vec()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceWithAlbum(
                paths.into_iter().map(PathBuf::from).collect(),
            ))
        }
        16 => {
            let paths = reader.read_u16_string_vec()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbum(
                paths.into_iter().map(PathBuf::from).collect(),
            ))
        }
        17 => {
            let artist = reader.read_u16_string()?;
            let album = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceAlbumByKey { artist, album })
        }
        18 => {
            let artist = reader.read_u16_string()?;
            let album = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::AppendAlbumByKey { artist, album })
        }
        19 => {
            let artist = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceArtistByKey { artist })
        }
        20 => {
            let artist = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::AppendArtistByKey { artist })
        }
        21 => {
            let path = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::AddRoot(PathBuf::from(path)))
        }
        22 => {
            let path = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::RemoveRoot(PathBuf::from(path)))
        }
        23 => {
            let path = reader.read_u16_string()?;
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::RescanRoot(PathBuf::from(path)))
        }
        24 => {
            reader.expect_done()?;
            BridgeCommand::Library(BridgeLibraryCommand::RescanAll)
        }
        25 => {
            let value = reader.read_u8()?;
            reader.expect_done()?;
            let mode = match value {
                1 => RepeatMode::One,
                2 => RepeatMode::All,
                _ => RepeatMode::Off,
            };
            BridgeCommand::Playback(BridgePlaybackCommand::SetRepeatMode(mode))
        }
        26 => {
            let enabled = reader.read_u8()? != 0;
            reader.expect_done()?;
            BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(enabled))
        }
        27 => {
            let value = reader.read_f32()?;
            reader.expect_done()?;
            if !value.is_finite() {
                return Err("set_db_range value must be finite".to_string());
            }
            BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(value))
        }
        28 => {
            let enabled = reader.read_u8()? != 0;
            reader.expect_done()?;
            BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(enabled))
        }
        29 => {
            let enabled = reader.read_u8()? != 0;
            reader.expect_done()?;
            BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(enabled))
        }
        30 => {
            let mode = LibrarySortMode::from_i32(reader.read_i32()?);
            reader.expect_done()?;
            BridgeCommand::Settings(BridgeSettingsCommand::SetLibrarySortMode(mode))
        }
        31 => {
            let fft_size = reader.read_u32()? as usize;
            reader.expect_done()?;
            BridgeCommand::Settings(BridgeSettingsCommand::SetFftSize(fft_size))
        }
        32 => {
            reader.expect_done()?;
            BridgeCommand::RequestSnapshot
        }
        33 => {
            reader.expect_done()?;
            BridgeCommand::Shutdown
        }
        _ => return Err(format!("unknown binary command id {cmd_id}")),
    };

    Ok(Some(command))
}

struct BinaryReader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> BinaryReader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn expect_done(&self) -> Result<(), String> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err("binary command payload has trailing bytes".to_string())
        }
    }

    fn read_exact<const N: usize>(&mut self) -> Result<[u8; N], String> {
        if self.offset + N > self.bytes.len() {
            return Err("binary command payload truncated".to_string());
        }
        let mut out = [0u8; N];
        out.copy_from_slice(&self.bytes[self.offset..self.offset + N]);
        self.offset += N;
        Ok(out)
    }

    fn read_u8(&mut self) -> Result<u8, String> {
        Ok(self.read_exact::<1>()?[0])
    }

    fn read_u16(&mut self) -> Result<u16, String> {
        Ok(u16::from_le_bytes(self.read_exact::<2>()?))
    }

    fn read_u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_i32(&mut self) -> Result<i32, String> {
        Ok(i32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_f32(&mut self) -> Result<f32, String> {
        Ok(f32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_f64(&mut self) -> Result<f64, String> {
        Ok(f64::from_le_bytes(self.read_exact::<8>()?))
    }

    fn read_u16_string(&mut self) -> Result<String, String> {
        let len = self.read_u16()? as usize;
        if self.offset + len > self.bytes.len() {
            return Err("binary command string truncated".to_string());
        }
        let bytes = &self.bytes[self.offset..self.offset + len];
        self.offset += len;
        Ok(String::from_utf8_lossy(bytes).to_string())
    }

    fn read_u16_string_vec(&mut self) -> Result<Vec<String>, String> {
        let count = self.read_u16()? as usize;
        let mut out = Vec::with_capacity(count);
        for _ in 0..count {
            out.push(self.read_u16_string()?);
        }
        Ok(out)
    }
}

fn encode_binary_snapshot(snapshot: &BridgeSnapshot, queue_duration: (f64, usize)) -> Vec<u8> {
    let mut sections: Vec<(u16, Vec<u8>)> = Vec::new();

    sections.push((SECTION_PLAYBACK, encode_playback_section(snapshot)));
    sections.push((
        SECTION_QUEUE,
        encode_queue_section(snapshot, queue_duration),
    ));
    sections.push((SECTION_LIBRARY_META, encode_library_meta_section(snapshot)));
    if let Some(tree_bytes) = snapshot.pre_built_tree_bytes.as_ref() {
        sections.push((SECTION_LIBRARY_TREE, tree_bytes.as_ref().clone()));
    }
    sections.push((SECTION_METADATA, encode_metadata_section(snapshot)));
    sections.push((SECTION_SETTINGS, encode_settings_section(snapshot)));

    encode_packet(sections)
}

fn encode_error_event(message: &str) -> Vec<u8> {
    let mut payload = Vec::new();
    push_u16_string(&mut payload, message);
    encode_packet(vec![(SECTION_ERROR, payload)])
}

fn encode_stopped_event() -> Vec<u8> {
    encode_packet(vec![(SECTION_STOPPED, Vec::new())])
}

fn encode_packet(sections: Vec<(u16, Vec<u8>)>) -> Vec<u8> {
    let mut section_mask = 0u16;
    let mut total_length = 12u32;
    for (bit, payload) in &sections {
        section_mask |= *bit;
        total_length = total_length
            .saturating_add(4)
            .saturating_add(payload.len() as u32);
    }

    let mut out = Vec::with_capacity(total_length as usize);
    push_u32(&mut out, SNAPSHOT_MAGIC);
    push_u32(&mut out, total_length);
    push_u16(&mut out, section_mask);
    push_u16(&mut out, 0);
    for (_, payload) in sections {
        push_u32(&mut out, payload.len() as u32);
        out.extend_from_slice(&payload);
    }
    out
}

fn encode_playback_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    let state = match snapshot.playback.state {
        PlaybackState::Stopped => 0u8,
        PlaybackState::Playing => 1u8,
        PlaybackState::Paused => 2u8,
    };
    let repeat_mode = match snapshot.playback.repeat_mode {
        RepeatMode::Off => 0u8,
        RepeatMode::One => 1u8,
        RepeatMode::All => 2u8,
    };
    let current_queue_index = snapshot
        .playback
        .current_queue_index
        .filter(|idx| *idx < snapshot.queue.len())
        .map_or(-1, |idx| idx as i32);
    let current_path = snapshot
        .playback
        .current
        .as_ref()
        .map(|path| path.to_string_lossy().to_string())
        .unwrap_or_default();

    push_u8(&mut out, state);
    push_f64(&mut out, snapshot.playback.position.as_secs_f64());
    push_f64(&mut out, snapshot.playback.duration.as_secs_f64());
    push_f32(&mut out, snapshot.playback.volume);
    push_u8(&mut out, repeat_mode);
    push_u8(&mut out, u8::from(snapshot.playback.shuffle_enabled));
    push_i32(&mut out, current_queue_index);
    push_u16_string(&mut out, &current_path);
    out
}

fn compute_queue_total_duration(snapshot: &BridgeSnapshot) -> (f64, usize) {
    let mut queue_path_counts: HashMap<&std::path::Path, usize> =
        HashMap::with_capacity(snapshot.queue.len());
    for path in &snapshot.queue {
        let entry = queue_path_counts.entry(path.as_path()).or_insert(0);
        *entry = entry.saturating_add(1);
    }
    if queue_path_counts.is_empty() {
        return (0.0, 0);
    }

    let mut total_duration_secs = 0.0;
    let mut known_duration_count = 0usize;
    for track in &snapshot.library.tracks {
        let Some(count) = queue_path_counts.remove(track.path.as_path()) else {
            continue;
        };
        if let Some(duration_secs) = track.duration_secs {
            let duration = f64::from(duration_secs);
            if duration.is_finite() && duration > 0.0 {
                total_duration_secs += duration * (count as f64);
                known_duration_count = known_duration_count.saturating_add(count);
            }
        }
        if queue_path_counts.is_empty() {
            break;
        }
    }

    let unknown_duration_count = snapshot.queue.len().saturating_sub(known_duration_count);
    (total_duration_secs, unknown_duration_count)
}

fn encode_queue_section(snapshot: &BridgeSnapshot, queue_duration: (f64, usize)) -> Vec<u8> {
    let mut out = Vec::new();
    let selected_index = snapshot.selected_queue_index.map_or(-1, |idx| idx as i32);
    let (total_duration_secs, unknown_duration_count) = queue_duration;

    push_u32(&mut out, snapshot.queue.len() as u32);
    push_i32(&mut out, selected_index);
    push_f64(&mut out, total_duration_secs);
    push_u32(&mut out, unknown_duration_count as u32);
    push_u32(&mut out, snapshot.queue.len() as u32);

    for path in &snapshot.queue {
        let path_str = path.to_string_lossy().to_string();
        let title = path.file_name().map_or_else(
            || path_str.clone(),
            |name| name.to_string_lossy().into_owned(),
        );
        push_u16_string(&mut out, &title);
        push_u16_string(&mut out, &path_str);
    }

    out
}

fn encode_library_meta_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    let progress = snapshot.library.scan_progress.as_ref();
    let roots_completed = progress.map_or(0, |p| p.roots_completed as u32);
    let roots_total = progress.map_or(0, |p| p.roots_total as u32);
    let files_discovered = progress.map_or(0, |p| p.supported_files_discovered as u32);
    let files_processed = progress.map_or(0, |p| p.supported_files_processed as u32);
    let files_per_second = progress.and_then(|p| p.files_per_second).unwrap_or(0.0);
    let eta_seconds = progress.and_then(|p| p.eta_seconds).unwrap_or(-1.0);

    push_u32(&mut out, snapshot.library.roots.len() as u32);
    push_u32(&mut out, snapshot.library.tracks.len() as u32);
    push_u8(&mut out, u8::from(snapshot.library.scan_in_progress));
    push_i32(&mut out, snapshot.settings.library_sort_mode.to_i32());
    push_u16_string(
        &mut out,
        snapshot.library.last_error.as_deref().unwrap_or_default(),
    );
    push_u32(&mut out, roots_completed);
    push_u32(&mut out, roots_total);
    push_u32(&mut out, files_discovered);
    push_u32(&mut out, files_processed);
    push_f32(&mut out, files_per_second);
    push_f32(&mut out, eta_seconds);
    push_u16(&mut out, clamp_u16(snapshot.library.roots.len()));
    for root in &snapshot.library.roots {
        let root_str = root.to_string_lossy().to_string();
        push_u16_string(&mut out, &root_str);
    }

    out
}

fn encode_metadata_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    push_u16_string(
        &mut out,
        snapshot.metadata.source_path.as_deref().unwrap_or_default(),
    );
    push_u16_string(&mut out, &snapshot.metadata.title);
    push_u16_string(&mut out, &snapshot.metadata.artist);
    push_u16_string(&mut out, &snapshot.metadata.album);
    push_u32(&mut out, snapshot.metadata.sample_rate_hz.unwrap_or(0));
    push_u32(&mut out, snapshot.metadata.bitrate_kbps.unwrap_or(0));
    push_u16(&mut out, snapshot.metadata.channels.map_or(0, u16::from));
    push_u16(&mut out, snapshot.metadata.bit_depth.map_or(0, u16::from));
    push_u16_string(
        &mut out,
        snapshot
            .metadata
            .cover_art_path
            .as_deref()
            .unwrap_or_default(),
    );
    out
}

fn encode_settings_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    push_f32(&mut out, snapshot.settings.volume);
    push_u32(&mut out, snapshot.settings.fft_size as u32);
    push_f32(&mut out, snapshot.settings.db_range);
    push_u8(&mut out, u8::from(snapshot.settings.log_scale));
    push_u8(&mut out, u8::from(snapshot.settings.show_fps));
    push_i32(&mut out, snapshot.settings.library_sort_mode.to_i32());
    out
}

fn clamp_u16(value: usize) -> u16 {
    value.min(u16::MAX as usize) as u16
}

fn push_u8(out: &mut Vec<u8>, value: u8) {
    out.push(value);
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_f32(out: &mut Vec<u8>, value: f32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_f64(out: &mut Vec<u8>, value: f64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u16_string(out: &mut Vec<u8>, value: &str) {
    let bytes = value.as_bytes();
    let len = bytes.len().min(u16::MAX as usize);
    push_u16(out, len as u16);
    out.extend_from_slice(&bytes[..len]);
}

fn compute_analysis_delta(s: &BridgeSnapshot, emit_state: &mut AnalysisEmitState) -> AnalysisDelta {
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
        spectrogram_reset,
        waveform_changed,
        waveform_peaks_u8,
        spectrogram_rows_u8,
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
    use std::sync::Arc;
    use std::thread;
    use std::time::Instant;

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
            },
            metadata: crate::metadata::TrackMetadata {
                source_path: Some("/music/a.flac".to_string()),
                title: "Sample Track".to_string(),
                artist: "Sample Artist".to_string(),
                album: "Sample Album".to_string(),
                sample_rate_hz: Some(48_000),
                bitrate_kbps: Some(320),
                channels: Some(2),
                bit_depth: Some(24),
                cover_art_path: Some("/music/a.cover.png".to_string()),
                cover_art_rgba: None,
            },
            library: Arc::new(LibrarySnapshot {
                roots: vec![PathBuf::from("/music")],
                tracks: vec![LibraryTrack {
                    path: PathBuf::from("/music/a.flac"),
                    root_path: PathBuf::from("/music"),
                    title: "Sample Track".to_string(),
                    artist: "Sample Artist".to_string(),
                    album: "Sample Album".to_string(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: Some(180.0),
                }],
                ..LibrarySnapshot::default()
            }),
            pre_built_tree_bytes: Some(Arc::new(vec![0, 0, 0, 0])),
            queue: vec![PathBuf::from("/music/a.flac")],
            selected_queue_index: Some(0),
            settings: super::super::BridgeSettings {
                volume: 0.75,
                fft_size: 2048,
                db_range: 90.0,
                log_scale: false,
                show_fps: false,
                library_sort_mode: LibrarySortMode::Year,
            },
        }
    }

    fn parse_packet_header(packet: &[u8]) -> (u32, u32, u16) {
        let magic = u32::from_le_bytes([packet[0], packet[1], packet[2], packet[3]]);
        let len = u32::from_le_bytes([packet[4], packet[5], packet[6], packet[7]]);
        let mask = u16::from_le_bytes([packet[8], packet[9]]);
        (magic, len, mask)
    }

    fn encode_command(cmd_id: u16, payload: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(4 + payload.len());
        out.extend_from_slice(&cmd_id.to_le_bytes());
        out.extend_from_slice(&(payload.len() as u16).to_le_bytes());
        out.extend_from_slice(payload);
        out
    }

    #[test]
    fn parse_binary_command_supports_settings_updates() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&88.0f32.to_le_bytes());
        let cmd = parse_binary_command(&encode_command(27, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetDbRange(v)) => {
                assert!((v - 88.0).abs() < 0.001);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(28, &[1]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetLogScale(v)) => {
                assert!(v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(29, &[1]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetShowFps(v)) => {
                assert!(v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let mut payload = Vec::new();
        payload.extend_from_slice(&1i32.to_le_bytes());
        let cmd = parse_binary_command(&encode_command(30, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetLibrarySortMode(mode)) => {
                assert_eq!(mode, LibrarySortMode::Title);
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_binary_command_supports_library_batch_commands() {
        let mut payload = Vec::new();
        let first = b"/music/a.flac";
        let second = b"/music/b.flac";
        payload.extend_from_slice(&2u16.to_le_bytes());
        payload.extend_from_slice(&(first.len() as u16).to_le_bytes());
        payload.extend_from_slice(first);
        payload.extend_from_slice(&(second.len() as u16).to_le_bytes());
        payload.extend_from_slice(second);

        let cmd = parse_binary_command(&encode_command(15, &payload))
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
    }

    #[test]
    fn parse_binary_command_rejects_invalid_seek() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&(-1.0f64).to_le_bytes());
        let err = parse_binary_command(&encode_command(7, &payload)).unwrap_err();
        assert!(err.contains("seek value must be finite and >= 0"));
    }

    #[test]
    fn snapshot_packet_contract_has_expected_shape() {
        let snapshot = sample_snapshot();
        let packet = encode_binary_snapshot(&snapshot, compute_queue_total_duration(&snapshot));
        let (magic, total_len, mask) = parse_packet_header(&packet);
        assert_eq!(magic, SNAPSHOT_MAGIC);
        assert_eq!(total_len as usize, packet.len());
        assert_ne!(mask & SECTION_PLAYBACK, 0);
        assert_ne!(mask & SECTION_QUEUE, 0);
        assert_ne!(mask & SECTION_LIBRARY_META, 0);
        assert_ne!(mask & SECTION_LIBRARY_TREE, 0);
        assert_ne!(mask & SECTION_METADATA, 0);
        assert_ne!(mask & SECTION_SETTINGS, 0);
        assert_eq!(mask & SECTION_ERROR, 0);
        assert_eq!(mask & SECTION_STOPPED, 0);
    }

    #[test]
    fn analysis_delta_and_frame_include_changes() {
        let snapshot = sample_snapshot();
        let mut emit_state = AnalysisEmitState::default();
        let delta = compute_analysis_delta(&snapshot, &mut emit_state);
        assert!(delta.waveform_changed);
        assert!(!delta.spectrogram_rows_u8.is_empty());
        let frame = encode_analysis_frame(&delta);
        assert!(!frame.is_empty());
        assert_eq!(frame[4], ANALYSIS_FRAME_MAGIC);
    }

    fn ffi_send_binary(handle: *mut FerrousFfiBridge, cmd: &[u8]) -> bool {
        unsafe { ferrous_ffi_bridge_send_binary(handle, cmd.as_ptr(), cmd.len()) }
    }

    fn ffi_next_event(handle: *mut FerrousFfiBridge, timeout: Duration) -> Option<Vec<u8>> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            unsafe {
                ferrous_ffi_bridge_poll(handle, 64);
                let mut len = 0usize;
                let ptr = ferrous_ffi_bridge_pop_binary_event(handle, &mut len as *mut usize);
                if !ptr.is_null() && len > 0 {
                    let bytes = std::slice::from_raw_parts(ptr, len).to_vec();
                    ferrous_ffi_bridge_free_binary_event(ptr, len);
                    return Some(bytes);
                }
            }
            thread::sleep(Duration::from_millis(10));
        }
        None
    }

    fn ffi_wait_for_mask(
        handle: *mut FerrousFfiBridge,
        section_mask_bit: u16,
        timeout: Duration,
    ) -> Option<Vec<u8>> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            let remaining = deadline.saturating_duration_since(Instant::now());
            let Some(packet) = ffi_next_event(handle, remaining.min(Duration::from_millis(100)))
            else {
                continue;
            };
            if packet.len() >= 12 {
                let (_, _, mask) = parse_packet_header(&packet);
                if (mask & section_mask_bit) != 0 {
                    return Some(packet);
                }
            }
        }
        None
    }

    #[test]
    fn ffi_bridge_emits_snapshot_event_end_to_end() {
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());

        let snapshot_evt = ffi_wait_for_mask(handle, SECTION_PLAYBACK, Duration::from_secs(4))
            .expect("snapshot event");
        let (_, _, mask) = parse_packet_header(&snapshot_evt);
        assert_ne!(mask & SECTION_QUEUE, 0);
        assert_ne!(mask & SECTION_SETTINGS, 0);

        assert!(ffi_send_binary(handle, &encode_command(33, &[])));
        let stopped = ffi_wait_for_mask(handle, SECTION_STOPPED, Duration::from_secs(3));
        assert!(stopped.is_some());
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }

    #[test]
    fn ffi_bridge_reports_error_for_bad_command_end_to_end() {
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());

        let mut bad_seek = Vec::new();
        bad_seek.extend_from_slice(&(-1.0f64).to_le_bytes());
        assert!(!ffi_send_binary(handle, &encode_command(7, &bad_seek)));
        let error_evt =
            ffi_wait_for_mask(handle, SECTION_ERROR, Duration::from_secs(3)).expect("error event");
        let (_, _, mask) = parse_packet_header(&error_evt);
        assert_ne!(mask & SECTION_ERROR, 0);

        assert!(ffi_send_binary(handle, &encode_command(33, &[])));
        let _ = ffi_wait_for_mask(handle, SECTION_STOPPED, Duration::from_secs(3));
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }
}
