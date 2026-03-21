use std::collections::{hash_map::DefaultHasher, HashMap, VecDeque};
use std::ffi::c_uchar;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[cfg(unix)]
use std::os::fd::RawFd;

use super::{
    BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand, BridgeQueueCommand,
    BridgeSearchResultRowType, BridgeSearchResultsFrame, BridgeSettingsCommand, BridgeSnapshot,
    FrontendBridgeHandle, LibrarySortMode, ViewerFullscreenMode,
};
use crate::analysis::{PrecomputedSpectrogramChunk, SpectrogramChannelLabel, SpectrogramViewMode};
use crate::library::{IndexedTrack, LibraryTrack};
use crate::playback::{PlaybackState, RepeatMode};
use crate::tag_editor;

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const PRECOMPUTED_SPECTROGRAM_MAGIC: u8 = 0xA2;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_RESET: u8 = 0x02;
const ANALYSIS_FLAG_SPECTROGRAM: u8 = 0x04;
const ANALYSIS_FLAG_WAVEFORM_COMPLETE: u8 = 0x08;
const MAX_PENDING_BINARY_EVENTS: usize = 12;
const MAX_PENDING_ANALYSIS_FRAMES: usize = 24;
const MAX_PENDING_PRECOMPUTED_SPECTROGRAM: usize = 64;
const MAX_PENDING_LIBRARY_TREES: usize = 1;
const MAX_PENDING_SEARCH_RESULTS: usize = 2;
const RELAY_RECV_TIMEOUT: Duration = Duration::from_millis(250);

const SNAPSHOT_MAGIC: u32 = 0xFE55_0001;
const SECTION_PLAYBACK: u16 = 1 << 0;
const SECTION_QUEUE: u16 = 1 << 1;
const SECTION_LIBRARY_META: u16 = 1 << 2;
const _SECTION_LIBRARY_TREE_RESERVED: u16 = 1 << 3;
const SECTION_METADATA: u16 = 1 << 4;
const SECTION_SETTINGS: u16 = 1 << 5;
const SECTION_ERROR: u16 = 1 << 6;
const SECTION_STOPPED: u16 = 1 << 7;
const SECTION_LASTFM: u16 = 1 << 8;

#[cfg(unix)]
fn create_nonblocking_pipe() -> Option<(RawFd, RawFd)> {
    let mut fds = [0; 2];
    let rc = unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_CLOEXEC | libc::O_NONBLOCK) };
    if rc != 0 {
        return None;
    }
    Some((fds[0], fds[1]))
}

#[cfg(not(unix))]
fn create_nonblocking_pipe() -> Option<(i32, i32)> {
    None
}

#[derive(Default)]
struct AnalysisDelta {
    sample_rate_hz: u32,
    frame_seq: u32,
    spectrogram_reset: bool,
    waveform_changed: bool,
    waveform_coverage_millis: u32,
    waveform_complete: bool,
    waveform_peaks_u8: Vec<u8>,
    spectrogram_channels_u8: Vec<EncodedSpectrogramChannel>,
}

#[derive(Default)]
struct EncodedSpectrogramChannel {
    label: SpectrogramChannelLabel,
    rows_u8: Vec<Vec<u8>>,
}

#[derive(Default)]
struct AnalysisEmitState {
    last_waveform_peaks: Vec<f32>,
    last_waveform_coverage_millis: u32,
    last_waveform_complete: bool,
    last_spectrogram_seq: u64,
    analysis_frame_seq: u32,
}

#[derive(Default)]
struct QueueSectionCache {
    library_ptr: usize,
    queue_paths: Vec<PathBuf>,
    queue_details_signature: u64,
    queue_section: QueueSectionData,
}

#[derive(Debug, Clone, Default)]
struct QueueSectionData {
    total_duration_secs: f64,
    unknown_duration_count: usize,
    tracks: Vec<EncodedQueueTrack>,
}

#[derive(Debug, Clone, Default)]
struct EncodedQueueTrack {
    title: String,
    artist: String,
    album: String,
    cover_path: String,
    genre: String,
    year: Option<i32>,
    track_number: Option<u32>,
    length_seconds: Option<f32>,
    path: String,
}

struct LibraryTreeFrame {
    version: u32,
    bytes: Vec<u8>,
}

struct SearchResultsFrame {
    seq: u32,
    bytes: Vec<u8>,
}

struct FfiRuntime {
    command_tx: crossbeam_channel::Sender<BridgeCommand>,
    analysis_state: AnalysisEmitState,
    queue_section_cache: QueueSectionCache,
    pending_binary_events: VecDeque<Vec<u8>>,
    pending_analysis_frames: VecDeque<Vec<u8>>,
    pending_precomputed_spectrogram: VecDeque<Vec<u8>>,
    pending_library_trees: VecDeque<LibraryTreeFrame>,
    pending_search_results: VecDeque<SearchResultsFrame>,
    next_tree_version: u32,
    wake_read_fd: i32,
    wake_write_fd: i32,
    wake_signaled: bool,
    stopped: bool,
}

impl FfiRuntime {
    fn new(
        command_tx: crossbeam_channel::Sender<BridgeCommand>,
        wake_read_fd: i32,
        wake_write_fd: i32,
    ) -> Self {
        Self {
            command_tx,
            analysis_state: AnalysisEmitState::default(),
            queue_section_cache: QueueSectionCache::default(),
            pending_binary_events: VecDeque::with_capacity(MAX_PENDING_BINARY_EVENTS),
            pending_analysis_frames: VecDeque::with_capacity(MAX_PENDING_ANALYSIS_FRAMES),
            pending_precomputed_spectrogram: VecDeque::with_capacity(
                MAX_PENDING_PRECOMPUTED_SPECTROGRAM,
            ),
            pending_library_trees: VecDeque::with_capacity(MAX_PENDING_LIBRARY_TREES),
            pending_search_results: VecDeque::with_capacity(MAX_PENDING_SEARCH_RESULTS),
            next_tree_version: 1,
            wake_read_fd,
            wake_write_fd,
            wake_signaled: false,
            stopped: false,
        }
    }

    fn has_pending_queues(&self) -> bool {
        !self.pending_binary_events.is_empty()
            || !self.pending_analysis_frames.is_empty()
            || !self.pending_precomputed_spectrogram.is_empty()
            || !self.pending_library_trees.is_empty()
            || !self.pending_search_results.is_empty()
    }

    #[cfg(unix)]
    fn signal_wakeup_if_needed(&mut self) {
        if self.wake_signaled || !self.has_pending_queues() || self.wake_write_fd < 0 {
            return;
        }

        let byte = [1u8; 1];
        loop {
            let written =
                unsafe { libc::write(self.wake_write_fd, byte.as_ptr().cast(), byte.len()) };
            if written == 1 {
                self.wake_signaled = true;
                return;
            }
            if written >= 0 {
                self.wake_signaled = true;
                return;
            }
            let err = std::io::Error::last_os_error()
                .raw_os_error()
                .unwrap_or_default();
            if err == libc::EINTR {
                continue;
            }
            if err == libc::EAGAIN || err == libc::EWOULDBLOCK {
                self.wake_signaled = true;
            }
            return;
        }
    }

    #[cfg(not(unix))]
    fn signal_wakeup_if_needed(&mut self) {}

    #[cfg(unix)]
    fn ack_wakeup(&mut self) {
        if self.wake_read_fd < 0 {
            return;
        }

        let mut buffer = [0u8; 64];
        loop {
            let read =
                unsafe { libc::read(self.wake_read_fd, buffer.as_mut_ptr().cast(), buffer.len()) };
            if read > 0 {
                continue;
            }
            if read == 0 {
                break;
            }
            let err = std::io::Error::last_os_error()
                .raw_os_error()
                .unwrap_or_default();
            if err == libc::EINTR {
                continue;
            }
            if err == libc::EAGAIN || err == libc::EWOULDBLOCK {
                break;
            }
            break;
        }

        self.wake_signaled = false;
        self.signal_wakeup_if_needed();
    }

    #[cfg(not(unix))]
    fn ack_wakeup(&mut self) {}

    fn close_wakeup_pipe(&mut self) {
        #[cfg(unix)]
        {
            if self.wake_read_fd >= 0 {
                unsafe { libc::close(self.wake_read_fd) };
                self.wake_read_fd = -1;
            }
            if self.wake_write_fd >= 0 {
                unsafe { libc::close(self.wake_write_fd) };
                self.wake_write_fd = -1;
            }
        }
    }

    fn push_binary_event(&mut self, payload: Vec<u8>) {
        if payload.is_empty() {
            return;
        }
        let was_empty = !self.has_pending_queues();
        while self.pending_binary_events.len() >= MAX_PENDING_BINARY_EVENTS {
            self.pending_binary_events.pop_front();
        }
        self.pending_binary_events.push_back(payload);
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn push_analysis_frame(&mut self, frame: Vec<u8>) {
        if frame.is_empty() {
            return;
        }
        let was_empty = !self.has_pending_queues();
        while self.pending_analysis_frames.len() >= MAX_PENDING_ANALYSIS_FRAMES {
            self.pending_analysis_frames.pop_front();
        }
        self.pending_analysis_frames.push_back(frame);
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn push_precomputed_spectrogram(&mut self, frame: Vec<u8>) {
        if frame.is_empty() {
            return;
        }
        let was_empty = !self.has_pending_queues();
        while self.pending_precomputed_spectrogram.len() >= MAX_PENDING_PRECOMPUTED_SPECTROGRAM {
            self.pending_precomputed_spectrogram.pop_front();
        }
        self.pending_precomputed_spectrogram.push_back(frame);
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn pop_precomputed_spectrogram(&mut self) -> Option<Vec<u8>> {
        self.pending_precomputed_spectrogram.pop_front()
    }

    fn push_library_tree_frame(&mut self, bytes: Vec<u8>) {
        let was_empty = !self.has_pending_queues();
        let mut payload = bytes;
        if payload.is_empty() {
            payload.extend_from_slice(&0u32.to_le_bytes());
        }
        let version = self.next_tree_version;
        self.next_tree_version = self.next_tree_version.wrapping_add(1).max(1);
        let frame = LibraryTreeFrame {
            version,
            bytes: payload,
        };
        while self.pending_library_trees.len() >= MAX_PENDING_LIBRARY_TREES {
            self.pending_library_trees.pop_front();
        }
        self.pending_library_trees.push_back(frame);
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn push_search_results_frame(&mut self, seq: u32, bytes: Vec<u8>) {
        if bytes.is_empty() {
            return;
        }
        let was_empty = !self.has_pending_queues();
        // Search result frames are superseded by newer seq values.
        // Keep latest-only to avoid backlogging stale UI apply work.
        self.pending_search_results.clear();
        self.pending_search_results
            .push_back(SearchResultsFrame { seq, bytes });
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn send_binary_command(&mut self, payload: &[u8]) -> Result<(), String> {
        let cmd = parse_binary_command(payload)?;
        if let Some(cmd) = cmd {
            let _ = self.command_tx.send(cmd);
        }
        Ok(())
    }

    fn process_bridge_events<I>(&mut self, events: I)
    where
        I: IntoIterator<Item = BridgeEvent>,
    {
        if self.stopped {
            return;
        }

        let mut latest_snapshot: Option<BridgeSnapshot> = None;
        let mut latest_queue_snapshot: Option<BridgeSnapshot> = None;
        let mut latest_tree_bytes: Option<std::sync::Arc<Vec<u8>>> = None;
        for event in events {
            match event {
                BridgeEvent::Snapshot(snapshot) => {
                    if let Some(tree_bytes) = snapshot.pre_built_tree_bytes.as_ref() {
                        latest_tree_bytes = Some(tree_bytes.clone());
                    }
                    if snapshot.queue_included {
                        latest_queue_snapshot = Some((*snapshot).clone());
                    }
                    latest_snapshot = Some(*snapshot);
                }
                BridgeEvent::PrecomputedSpectrogramChunk(chunk) => {
                    self.push_precomputed_spectrogram(encode_precomputed_spectrogram_chunk(&chunk));
                }
                BridgeEvent::Error(message) => {
                    self.push_binary_event(encode_error_event(&message));
                }
                BridgeEvent::SearchResults(frame) => {
                    self.push_search_results_frame(frame.seq, encode_search_results_frame(&frame));
                }
                BridgeEvent::Stopped => {
                    self.stopped = true;
                    self.push_binary_event(encode_stopped_event());
                }
            }
        }

        if let Some(tree_bytes) = latest_tree_bytes {
            self.push_library_tree_frame(tree_bytes.as_ref().clone());
        }
        if let Some(snapshot) =
            latest_snapshot.map(|snapshot| merge_queue_snapshot(snapshot, latest_queue_snapshot))
        {
            let analysis_delta = compute_analysis_delta(&snapshot, &mut self.analysis_state);
            self.push_analysis_frame(encode_analysis_frame(&analysis_delta));
            let queue_section = if snapshot.queue_included {
                Some(self.queue_section_for_snapshot(&snapshot))
            } else {
                None
            };
            self.push_binary_event(encode_binary_snapshot(&snapshot, queue_section.as_ref()));
        }
    }

    fn pop_binary_event(&mut self) -> Option<Vec<u8>> {
        self.pending_binary_events.pop_front()
    }

    fn pop_analysis_frame(&mut self) -> Option<Vec<u8>> {
        self.pending_analysis_frames.pop_front()
    }

    fn pop_library_tree_frame(&mut self) -> Option<LibraryTreeFrame> {
        self.pending_library_trees.pop_front()
    }

    fn pop_search_results_frame(&mut self) -> Option<SearchResultsFrame> {
        self.pending_search_results.pop_front()
    }

    fn queue_section_for_snapshot(&mut self, snapshot: &BridgeSnapshot) -> QueueSectionData {
        if snapshot.queue.is_empty() {
            return QueueSectionData::default();
        }

        if queue_section_cache_matches(&self.queue_section_cache, snapshot) {
            return self.queue_section_cache.queue_section.clone();
        }

        let queue_section = compute_queue_section_data(snapshot);
        self.queue_section_cache.library_ptr = std::sync::Arc::as_ptr(&snapshot.library).addr();
        self.queue_section_cache
            .queue_paths
            .clone_from(&snapshot.queue);
        self.queue_section_cache.queue_details_signature = queue_details_signature(snapshot);
        self.queue_section_cache.queue_section = queue_section.clone();
        queue_section
    }
}

fn queue_section_cache_matches(cache: &QueueSectionCache, snapshot: &BridgeSnapshot) -> bool {
    cache.library_ptr == std::sync::Arc::as_ptr(&snapshot.library).addr()
        && cache.queue_paths == snapshot.queue
        && cache.queue_details_signature == queue_details_signature(snapshot)
}

fn queue_details_signature(snapshot: &BridgeSnapshot) -> u64 {
    let mut hasher = DefaultHasher::new();
    for path in &snapshot.queue {
        path.hash(&mut hasher);
        if let Some(track) = snapshot.queue_details.get(path) {
            track.title.hash(&mut hasher);
            track.artist.hash(&mut hasher);
            track.album.hash(&mut hasher);
            track.cover_path.hash(&mut hasher);
            track.genre.hash(&mut hasher);
            track.year.hash(&mut hasher);
            track.track_no.hash(&mut hasher);
            track.duration_secs.map(f32::to_bits).hash(&mut hasher);
        }
    }
    hasher.finish()
}

fn merge_queue_snapshot(
    mut latest_snapshot: BridgeSnapshot,
    latest_queue_snapshot: Option<BridgeSnapshot>,
) -> BridgeSnapshot {
    if latest_snapshot.queue_included {
        return latest_snapshot;
    }
    let Some(queue_snapshot) = latest_queue_snapshot else {
        return latest_snapshot;
    };
    latest_snapshot.queue_included = true;
    latest_snapshot.queue = queue_snapshot.queue;
    latest_snapshot.selected_queue_index = queue_snapshot.selected_queue_index;
    latest_snapshot
}

// Receiver is consumed by this thread's main loop — ownership transfer is intentional.
#[allow(clippy::needless_pass_by_value)]
fn run_ffi_relay_loop(shared: Arc<FfiShared>, event_rx: crossbeam_channel::Receiver<BridgeEvent>) {
    loop {
        if shared.stop_requested.load(Ordering::Relaxed) {
            break;
        }

        let first_event = match event_rx.recv_timeout(RELAY_RECV_TIMEOUT) {
            Ok(event) => event,
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => continue,
            Err(crossbeam_channel::RecvTimeoutError::Disconnected) => break,
        };

        let mut events = vec![first_event];
        while let Ok(event) = event_rx.try_recv() {
            events.push(event);
        }

        let Ok(mut runtime) = shared.runtime.lock() else {
            break;
        };
        runtime.process_bridge_events(events);
        if runtime.stopped {
            shared.stop_requested.store(true, Ordering::Relaxed);
            break;
        }
    }
}

#[repr(C)]
struct FfiShared {
    runtime: Mutex<FfiRuntime>,
    stop_requested: AtomicBool,
}

#[repr(C)]
pub struct FerrousFfiBridge {
    shared: Arc<FfiShared>,
    relay_thread: Mutex<Option<JoinHandle<()>>>,
}

#[no_mangle]
pub extern "C" fn ferrous_ffi_bridge_create() -> *mut FerrousFfiBridge {
    let bridge = FrontendBridgeHandle::spawn();
    let (command_tx, event_rx) = bridge.into_parts();
    let (wake_read_fd, wake_write_fd) = create_nonblocking_pipe().unwrap_or((-1, -1));
    let shared = Arc::new(FfiShared {
        runtime: Mutex::new(FfiRuntime::new(command_tx, wake_read_fd, wake_write_fd)),
        stop_requested: AtomicBool::new(false),
    });
    if let Ok(runtime) = shared.runtime.lock() {
        let _ = runtime.command_tx.send(BridgeCommand::RequestSnapshot);
    }

    let relay_shared = Arc::clone(&shared);
    let relay_thread = thread::Builder::new()
        .name("ferrous-ffi-relay".to_string())
        .spawn(move || run_ffi_relay_loop(relay_shared, event_rx))
        .ok();

    Box::into_raw(Box::new(FerrousFfiBridge {
        shared,
        relay_thread: Mutex::new(relay_thread),
    }))
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a pointer previously returned by
/// [`ferrous_ffi_bridge_create`] and must not be used again after this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_destroy(handle: *mut FerrousFfiBridge) {
    if handle.is_null() {
        return;
    }
    let bridge = Box::from_raw(handle);
    let shared = Arc::clone(&bridge.shared);
    shared.stop_requested.store(true, Ordering::Relaxed);
    if let Ok(runtime) = shared.runtime.lock() {
        let _ = runtime.command_tx.send(BridgeCommand::Shutdown);
    }
    if let Ok(mut relay_thread) = bridge.relay_thread.lock() {
        if let Some(join_handle) = relay_thread.take() {
            let _ = join_handle.join();
        }
    }
    {
        let runtime_lock = shared.runtime.lock();
        if let Ok(mut runtime) = runtime_lock {
            runtime.close_wakeup_pipe();
        }
    }
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// `cmd_ptr` must point to `cmd_len` readable bytes for the duration of this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_send_binary(
    handle: *mut FerrousFfiBridge,
    cmd_ptr: *const c_uchar,
    cmd_len: usize,
) -> bool {
    if handle.is_null() || cmd_ptr.is_null() || cmd_len == 0 {
        return false;
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
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
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_poll(
    handle: *mut FerrousFfiBridge,
    max_events: u32,
) -> bool {
    let _ = max_events;
    if handle.is_null() {
        return false;
    }
    let bridge = &*handle;
    let Ok(runtime) = bridge.shared.runtime.lock() else {
        return false;
    };
    runtime.has_pending_queues()
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_wakeup_fd(handle: *mut FerrousFfiBridge) -> i32 {
    if handle.is_null() {
        return -1;
    }
    let bridge = &*handle;
    let Ok(runtime) = bridge.shared.runtime.lock() else {
        return -1;
    };
    runtime.wake_read_fd
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_ack_wakeup(handle: *mut FerrousFfiBridge) {
    if handle.is_null() {
        return;
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
        return;
    };
    runtime.ack_wakeup();
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
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
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
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
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_binary_event`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_binary_event(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
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
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
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
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_analysis_frame`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_analysis_frame(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_precomputed_spectrogram(
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
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
        return std::ptr::null_mut();
    };
    let Some(frame) = runtime.pop_precomputed_spectrogram() else {
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
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_precomputed_spectrogram`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_precomputed_spectrogram(
    ptr: *mut c_uchar,
    len: usize,
) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` or `version_out` are non-null, each must be writable for one value.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_library_tree(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
    version_out: *mut u32,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if !version_out.is_null() {
        *version_out = 0;
    }
    if handle.is_null() {
        return std::ptr::null_mut();
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
        return std::ptr::null_mut();
    };
    let Some(frame) = runtime.pop_library_tree_frame() else {
        return std::ptr::null_mut();
    };
    let mut boxed = frame.bytes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    if !len_out.is_null() {
        *len_out = len;
    }
    if !version_out.is_null() {
        *version_out = frame.version;
    }
    ptr
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_library_tree`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_library_tree(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` or `seq_out` are non-null, each must be writable for one value.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_search_results(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
    seq_out: *mut u32,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if !seq_out.is_null() {
        *seq_out = 0;
    }
    if handle.is_null() {
        return std::ptr::null_mut();
    }
    let bridge = &*handle;
    let Ok(mut runtime) = bridge.shared.runtime.lock() else {
        return std::ptr::null_mut();
    };
    let Some(frame) = runtime.pop_search_results_frame() else {
        return std::ptr::null_mut();
    };
    let mut boxed = frame.bytes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    if !len_out.is_null() {
        *len_out = len;
    }
    if !seq_out.is_null() {
        *seq_out = frame.seq;
    }
    ptr
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_search_results`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_search_results(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// `paths_ptr` must point to `paths_len` readable bytes for the duration of this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_refresh_edited_paths(
    handle: *mut FerrousFfiBridge,
    paths_ptr: *const c_uchar,
    paths_len: usize,
) -> bool {
    if handle.is_null() || paths_ptr.is_null() || paths_len == 0 {
        return false;
    }
    let Ok(paths) = tag_editor::parse_paths_blob(std::slice::from_raw_parts(paths_ptr, paths_len))
    else {
        return false;
    };
    let bridge = &*handle;
    let Ok(runtime) = bridge.shared.runtime.lock() else {
        return false;
    };
    let _ = runtime.command_tx.send(BridgeCommand::Library(
        BridgeLibraryCommand::RefreshEditedPaths(paths),
    ));
    true
}

#[no_mangle]
/// # Safety
///
/// `paths_ptr` must point to `paths_len` readable bytes for the duration of this call.
/// If `len_out` is non-null it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_load(
    paths_ptr: *const c_uchar,
    paths_len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if paths_ptr.is_null() || paths_len == 0 {
        return std::ptr::null_mut();
    }
    let Ok(paths) = tag_editor::parse_paths_blob(std::slice::from_raw_parts(paths_ptr, paths_len))
    else {
        return std::ptr::null_mut();
    };
    let response = tag_editor::load_rows_for_paths(&paths);
    let Ok(bytes) = tag_editor::serialize_load_response(&response) else {
        return std::ptr::null_mut();
    };
    into_raw_buffer(bytes, len_out)
}

#[no_mangle]
/// # Safety
///
/// `save_ptr` must point to `save_len` readable bytes for the duration of this call.
/// If `len_out` is non-null it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_save(
    save_ptr: *const c_uchar,
    save_len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if save_ptr.is_null() || save_len == 0 {
        return std::ptr::null_mut();
    }
    let Ok(request) =
        tag_editor::parse_save_request(std::slice::from_raw_parts(save_ptr, save_len))
    else {
        return std::ptr::null_mut();
    };
    let response = tag_editor::save_rows(request);
    let Ok(bytes) = tag_editor::serialize_save_response(&response) else {
        return std::ptr::null_mut();
    };
    into_raw_buffer(bytes, len_out)
}

#[no_mangle]
/// # Safety
///
/// `handle` must be a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// `rename_ptr` must point to `rename_len` readable bytes for the duration of this call.
/// If `len_out` is non-null it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_bridge_rename_edited_files(
    handle: *mut FerrousFfiBridge,
    rename_ptr: *const c_uchar,
    rename_len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    if !len_out.is_null() {
        *len_out = 0;
    }
    if handle.is_null() || rename_ptr.is_null() || rename_len == 0 {
        return std::ptr::null_mut();
    }
    let Ok(request) =
        tag_editor::parse_rename_request(std::slice::from_raw_parts(rename_ptr, rename_len))
    else {
        return std::ptr::null_mut();
    };
    let response = tag_editor::rename_rows(request);
    let rename_pairs = response
        .results
        .iter()
        .filter_map(|result| {
            if !result.ok {
                return None;
            }
            let new_path = result.new_path.as_deref()?;
            Some((PathBuf::from(&result.path), PathBuf::from(new_path)))
        })
        .filter(|(old_path, new_path)| old_path != new_path)
        .collect::<Vec<_>>();
    if !rename_pairs.is_empty() {
        let bridge = &*handle;
        if let Ok(runtime) = bridge.shared.runtime.lock() {
            let _ = runtime.command_tx.send(BridgeCommand::Library(
                BridgeLibraryCommand::RefreshRenamedPaths(rename_pairs),
            ));
        }
    }
    let Ok(bytes) = tag_editor::serialize_rename_response(&response) else {
        return std::ptr::null_mut();
    };
    into_raw_buffer(bytes, len_out)
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by one of the tag editor helpers.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_free_buffer(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    drop(Vec::from_raw_parts(ptr, len, len));
}

#[no_mangle]
/// # Safety
///
/// Each `(ptr, len)` pair must point to `len` readable bytes for the
/// duration of this call (or `ptr` may be null when `len` is 0).
pub unsafe extern "C" fn ferrous_ffi_fuzzy_match_score(
    candidate_album_ptr: *const c_uchar,
    candidate_album_len: usize,
    candidate_artist_ptr: *const c_uchar,
    candidate_artist_len: usize,
    wanted_album_ptr: *const c_uchar,
    wanted_album_len: usize,
    wanted_artist_ptr: *const c_uchar,
    wanted_artist_len: usize,
) -> f64 {
    let to_str = |ptr: *const c_uchar, len: usize| -> &str {
        if ptr.is_null() || len == 0 {
            return "";
        }
        std::str::from_utf8(std::slice::from_raw_parts(ptr, len)).unwrap_or("")
    };
    let candidate_album = to_str(candidate_album_ptr, candidate_album_len);
    let candidate_artist = to_str(candidate_artist_ptr, candidate_artist_len);
    let wanted_album = to_str(wanted_album_ptr, wanted_album_len);
    let wanted_artist = to_str(wanted_artist_ptr, wanted_artist_len);
    crate::fuzzy_match::itunes_relevance_score(
        candidate_album,
        candidate_artist,
        wanted_album,
        wanted_artist,
    )
}

fn into_raw_buffer(bytes: Vec<u8>, len_out: *mut usize) -> *mut c_uchar {
    let mut boxed = bytes.into_boxed_slice();
    let ptr = boxed.as_mut_ptr();
    let len = boxed.len();
    std::mem::forget(boxed);
    if !len_out.is_null() {
        unsafe {
            *len_out = len;
        }
    }
    ptr
}

fn parse_binary_command(payload: &[u8]) -> Result<Option<BridgeCommand>, String> {
    if payload.len() < 4 {
        return Err("binary command payload too short".to_string());
    }

    let cmd_id = u16::from_le_bytes([payload[0], payload[1]]);
    let declared_len = usize::from(u16::from_le_bytes([payload[2], payload[3]]));
    let actual_payload = &payload[4..];
    if actual_payload.len() != declared_len {
        return Err(format!(
            "binary command payload length mismatch: header={declared_len}, actual={}",
            actual_payload.len()
        ));
    }

    let mut reader = BinaryReader::new(actual_payload);
    let command = if let Some(command) = parse_playback_command(cmd_id, &mut reader)? {
        command
    } else if let Some(command) = parse_queue_command(cmd_id, &mut reader)? {
        command
    } else if let Some(command) = parse_library_collection_command(cmd_id, &mut reader)? {
        command
    } else if let Some(command) = parse_library_ui_command(cmd_id, &mut reader)? {
        command
    } else if let Some(command) = parse_settings_command(cmd_id, &mut reader)? {
        command
    } else if let Some(command) = parse_system_command(cmd_id, &mut reader)? {
        command
    } else {
        return Err(format!("unknown binary command id {cmd_id}"));
    };

    Ok(Some(command))
}

fn parse_playback_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        1 => BridgeCommand::Playback(BridgePlaybackCommand::Play),
        2 => BridgeCommand::Playback(BridgePlaybackCommand::Pause),
        3 => BridgeCommand::Playback(BridgePlaybackCommand::Stop),
        4 => BridgeCommand::Playback(BridgePlaybackCommand::Next),
        5 => BridgeCommand::Playback(BridgePlaybackCommand::Previous),
        6 => {
            let value = reader.read_f64()?;
            if !value.is_finite() {
                return Err("set_volume value must be finite".to_string());
            }
            BridgeCommand::Playback(BridgePlaybackCommand::SetVolume(parse_f32(value)?))
        }
        7 => {
            let value = reader.read_f64()?;
            if !value.is_finite() || value < 0.0 {
                return Err("seek value must be finite and >= 0".to_string());
            }
            BridgeCommand::Playback(BridgePlaybackCommand::Seek(Duration::from_secs_f64(value)))
        }
        25 => {
            let mode = match reader.read_u8()? {
                1 => RepeatMode::One,
                2 => RepeatMode::All,
                _ => RepeatMode::Off,
            };
            BridgeCommand::Playback(BridgePlaybackCommand::SetRepeatMode(mode))
        }
        26 => BridgeCommand::Playback(BridgePlaybackCommand::SetShuffle(reader.read_u8()? != 0)),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(command))
}

fn parse_queue_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        8 => BridgeQueueCommand::PlayAt(usize_from_u32(reader.read_u32()?)),
        9 => {
            let selected = match reader.read_i32()? {
                index if index < 0 => None,
                index => Some(usize_from_i32(index)?),
            };
            BridgeQueueCommand::Select(selected)
        }
        10 => BridgeQueueCommand::Remove(usize_from_u32(reader.read_u32()?)),
        11 => {
            let from = usize_from_u32(reader.read_u32()?);
            let to = usize_from_u32(reader.read_u32()?);
            BridgeQueueCommand::Move { from, to }
        }
        12 => BridgeQueueCommand::Clear,
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Queue(command)))
}

fn parse_library_collection_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        13 => BridgeLibraryCommand::AddTrack(PathBuf::from(reader.read_u16_string()?)),
        14 => BridgeLibraryCommand::PlayTrack(PathBuf::from(reader.read_u16_string()?)),
        15 => BridgeLibraryCommand::ReplaceWithAlbum(read_path_vec(reader)?),
        16 => BridgeLibraryCommand::AppendAlbum(read_path_vec(reader)?),
        17 => BridgeLibraryCommand::ReplaceAlbumByKey {
            artist: reader.read_u16_string()?,
            album: reader.read_u16_string()?,
        },
        18 => BridgeLibraryCommand::AppendAlbumByKey {
            artist: reader.read_u16_string()?,
            album: reader.read_u16_string()?,
        },
        19 => BridgeLibraryCommand::ReplaceArtistByKey {
            artist: reader.read_u16_string()?,
        },
        20 => BridgeLibraryCommand::AppendArtistByKey {
            artist: reader.read_u16_string()?,
        },
        37 => BridgeLibraryCommand::ReplaceAllTracks,
        38 => BridgeLibraryCommand::AppendAllTracks,
        48 => BridgeLibraryCommand::ReplaceRootByPath {
            root: reader.read_u16_string()?,
        },
        49 => BridgeLibraryCommand::AppendRootByPath {
            root: reader.read_u16_string()?,
        },
        46 => BridgeLibraryCommand::ApplyAlbumArt {
            track_path: PathBuf::from(reader.read_u16_string()?),
            artwork_path: PathBuf::from(reader.read_u16_string()?),
        },
        47 => BridgeLibraryCommand::RefreshEditedPaths(read_path_vec(reader)?),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Library(command)))
}

fn parse_library_ui_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        21 => BridgeLibraryCommand::AddRoot {
            path: PathBuf::from(reader.read_u16_string()?),
            name: reader.read_u16_string()?,
        },
        22 => BridgeLibraryCommand::RemoveRoot(PathBuf::from(reader.read_u16_string()?)),
        23 => BridgeLibraryCommand::RescanRoot(PathBuf::from(reader.read_u16_string()?)),
        24 => BridgeLibraryCommand::RescanAll,
        35 => BridgeLibraryCommand::SetNodeExpanded {
            key: reader.read_u16_string()?,
            expanded: reader.read_u8()? != 0,
        },
        36 => BridgeLibraryCommand::SetSearchQuery {
            seq: reader.read_u32()?,
            query: reader.read_u16_string()?,
        },
        45 => BridgeLibraryCommand::RenameRoot {
            path: PathBuf::from(reader.read_u16_string()?),
            name: reader.read_u16_string()?,
        },
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Library(command)))
}

fn parse_settings_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        27 => match reader.read_f32()? {
            value if value.is_finite() => BridgeSettingsCommand::SetDbRange(value),
            _ => return Err("set_db_range value must be finite".to_string()),
        },
        28 => BridgeSettingsCommand::SetLogScale(reader.read_u8()? != 0),
        29 => BridgeSettingsCommand::SetShowFps(reader.read_u8()? != 0),
        30 => {
            BridgeSettingsCommand::SetLibrarySortMode(LibrarySortMode::from_i32(reader.read_i32()?))
        }
        31 => BridgeSettingsCommand::SetFftSize(usize_from_u32(reader.read_u32()?)),
        32 => BridgeSettingsCommand::SetSpectrogramViewMode(SpectrogramViewMode::from_i32(
            i32::from(reader.read_u8()?),
        )),
        39 => BridgeSettingsCommand::SetSystemMediaControlsEnabled(reader.read_u8()? != 0),
        40 => BridgeSettingsCommand::SetLastFmScrobblingEnabled(reader.read_u8()? != 0),
        41 => BridgeSettingsCommand::BeginLastFmAuth,
        42 => BridgeSettingsCommand::CompleteLastFmAuth,
        43 => BridgeSettingsCommand::DisconnectLastFm,
        44 => BridgeSettingsCommand::SetViewerFullscreenMode(ViewerFullscreenMode::from_i32(
            i32::from(reader.read_u8()?),
        )),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Settings(command)))
}

fn parse_system_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        33 => BridgeCommand::RequestSnapshot,
        34 => BridgeCommand::Shutdown,
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(command))
}

fn read_path_vec(reader: &mut BinaryReader<'_>) -> Result<Vec<PathBuf>, String> {
    Ok(reader
        .read_u16_string_vec()?
        .into_iter()
        .map(PathBuf::from)
        .collect())
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
        let len = usize::from(self.read_u16()?);
        if self.offset + len > self.bytes.len() {
            return Err("binary command string truncated".to_string());
        }
        let bytes = &self.bytes[self.offset..self.offset + len];
        self.offset += len;
        Ok(String::from_utf8_lossy(bytes).to_string())
    }

    fn read_u16_string_vec(&mut self) -> Result<Vec<String>, String> {
        let count = usize::from(self.read_u16()?);
        let mut out = Vec::with_capacity(count);
        for _ in 0..count {
            out.push(self.read_u16_string()?);
        }
        Ok(out)
    }
}

fn encode_binary_snapshot(
    snapshot: &BridgeSnapshot,
    queue_section: Option<&QueueSectionData>,
) -> Vec<u8> {
    let mut sections: Vec<(u16, Vec<u8>)> = vec![
        (SECTION_PLAYBACK, encode_playback_section(snapshot)),
        (SECTION_LIBRARY_META, encode_library_meta_section(snapshot)),
        (SECTION_METADATA, encode_metadata_section(snapshot)),
        (SECTION_SETTINGS, encode_settings_section(snapshot)),
        (SECTION_LASTFM, encode_lastfm_section(snapshot)),
    ];
    if let Some(section) = queue_section {
        sections.insert(1, (SECTION_QUEUE, encode_queue_section(snapshot, section)));
    }
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

fn encode_search_results_frame(frame: &BridgeSearchResultsFrame) -> Vec<u8> {
    let mut out = Vec::with_capacity(64 + frame.rows.len() * 128);
    push_u8(&mut out, b'S');
    push_u8(&mut out, 2);
    push_u16(&mut out, clamp_u16(frame.rows.len()));
    push_u32(&mut out, frame.seq);
    for row in &frame.rows {
        let row_type = match row.row_type {
            BridgeSearchResultRowType::Artist => 1u8,
            BridgeSearchResultRowType::Album => 2u8,
            BridgeSearchResultRowType::Track => 3u8,
        };
        push_u8(&mut out, row_type);
        push_f32(&mut out, row.score);
        push_i32(&mut out, row.year.unwrap_or(i32::MIN));
        push_u16(&mut out, row.track_number.map_or(0, clamp_u32_to_u16));
        push_u32(&mut out, row.count);
        push_f32(&mut out, row.length_seconds.unwrap_or(-1.0));
        push_u16_string(&mut out, &row.label);
        push_u16_string(&mut out, &row.artist);
        push_u16_string(&mut out, &row.album);
        push_u16_string(&mut out, &row.root_label);
        push_u16_string(&mut out, &row.genre);
        push_u16_string(&mut out, &row.cover_path);
        push_u16_string(&mut out, &row.artist_key);
        push_u16_string(&mut out, &row.album_key);
        push_u16_string(&mut out, &row.section_key);
        push_u16_string(&mut out, &row.track_key);
        push_u16_string(&mut out, &row.track_path);
    }
    out
}

fn encode_packet(sections: Vec<(u16, Vec<u8>)>) -> Vec<u8> {
    let mut section_mask = 0u16;
    let mut total_length = 12u32;
    for (bit, payload) in &sections {
        section_mask |= *bit;
        total_length = total_length
            .saturating_add(4)
            .saturating_add(clamp_u32(payload.len()));
    }

    let mut out = Vec::with_capacity(usize_from_u32(total_length));
    push_u32(&mut out, SNAPSHOT_MAGIC);
    push_u32(&mut out, total_length);
    push_u16(&mut out, section_mask);
    push_u16(&mut out, 0);
    for (_, payload) in sections {
        push_u32(&mut out, clamp_u32(payload.len()));
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
    let current_queue_index = snapshot.playback.current_queue_index.map_or(-1, clamp_i32);
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

fn path_title_fallback(path: &std::path::Path, path_str: &str) -> String {
    path.file_name().map_or_else(
        || path_str.to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

fn compute_queue_section_data(snapshot: &BridgeSnapshot) -> QueueSectionData {
    let mut by_path: HashMap<&std::path::Path, &crate::library::LibraryTrack> =
        HashMap::with_capacity(snapshot.library.tracks.len());
    for track in &snapshot.library.tracks {
        by_path.insert(track.path.as_path(), track);
    }

    let mut tracks = Vec::with_capacity(snapshot.queue.len());
    let mut total_duration_secs = 0.0;
    let mut unknown_duration_count = 0usize;

    for path in &snapshot.queue {
        let path_str = path.to_string_lossy().to_string();
        let fallback_title = path_title_fallback(path, &path_str);

        if let Some(track) = by_path.get(path.as_path()) {
            update_queue_duration_stats(
                track.duration_secs,
                &mut total_duration_secs,
                &mut unknown_duration_count,
            );
            tracks.push(encoded_queue_track_from_library(
                track,
                fallback_title,
                path_str,
            ));
            continue;
        }

        if let Some(track) = snapshot.queue_details.get(path) {
            update_queue_duration_stats(
                track.duration_secs,
                &mut total_duration_secs,
                &mut unknown_duration_count,
            );
            tracks.push(encoded_queue_track_from_indexed(
                track,
                fallback_title,
                path_str,
            ));
            continue;
        }

        unknown_duration_count = unknown_duration_count.saturating_add(1);
        tracks.push(EncodedQueueTrack {
            title: fallback_title,
            artist: String::new(),
            album: String::new(),
            cover_path: String::new(),
            genre: String::new(),
            year: None,
            track_number: None,
            length_seconds: None,
            path: path_str,
        });
    }

    QueueSectionData {
        total_duration_secs,
        unknown_duration_count,
        tracks,
    }
}

fn update_queue_duration_stats(
    duration_secs: Option<f32>,
    total_duration_secs: &mut f64,
    unknown_duration_count: &mut usize,
) {
    if let Some(value) = duration_secs.filter(|value| value.is_finite() && *value > 0.0) {
        *total_duration_secs += f64::from(value);
    } else {
        *unknown_duration_count = unknown_duration_count.saturating_add(1);
    }
}

fn encoded_queue_track_from_library(
    track: &LibraryTrack,
    fallback_title: String,
    path: String,
) -> EncodedQueueTrack {
    encoded_queue_track(
        &QueueTrackSource {
            title: &track.title,
            artist: &track.artist,
            album: &track.album,
            cover_path: &track.cover_path,
            genre: &track.genre,
            year: track.year,
            track_number: track.track_no,
            duration_secs: track.duration_secs,
        },
        fallback_title,
        path,
    )
}

fn encoded_queue_track_from_indexed(
    track: &IndexedTrack,
    fallback_title: String,
    path: String,
) -> EncodedQueueTrack {
    encoded_queue_track(
        &QueueTrackSource {
            title: &track.title,
            artist: &track.artist,
            album: &track.album,
            cover_path: &track.cover_path,
            genre: &track.genre,
            year: track.year,
            track_number: track.track_no,
            duration_secs: track.duration_secs,
        },
        fallback_title,
        path,
    )
}

struct QueueTrackSource<'a> {
    title: &'a str,
    artist: &'a str,
    album: &'a str,
    cover_path: &'a str,
    genre: &'a str,
    year: Option<i32>,
    track_number: Option<u32>,
    duration_secs: Option<f32>,
}

fn encoded_queue_track(
    track: &QueueTrackSource<'_>,
    fallback_title: String,
    path: String,
) -> EncodedQueueTrack {
    EncodedQueueTrack {
        title: normalized_queue_title(track.title, fallback_title),
        artist: track.artist.to_string(),
        album: track.album.to_string(),
        cover_path: track.cover_path.to_string(),
        genre: track.genre.to_string(),
        year: track.year,
        track_number: track.track_number,
        length_seconds: normalized_queue_duration(track.duration_secs),
        path,
    }
}

fn normalized_queue_title(title: &str, fallback_title: String) -> String {
    if title.trim().is_empty() {
        fallback_title
    } else {
        title.to_string()
    }
}

fn normalized_queue_duration(duration_secs: Option<f32>) -> Option<f32> {
    duration_secs.filter(|value| value.is_finite() && *value >= 0.0)
}

fn encode_queue_section(snapshot: &BridgeSnapshot, queue_section: &QueueSectionData) -> Vec<u8> {
    let mut out = Vec::new();
    let selected_index = snapshot.selected_queue_index.map_or(-1, clamp_i32);

    push_u32(&mut out, clamp_u32(snapshot.queue.len()));
    push_i32(&mut out, selected_index);
    push_f64(&mut out, queue_section.total_duration_secs);
    push_u32(&mut out, clamp_u32(queue_section.unknown_duration_count));
    push_u32(&mut out, clamp_u32(queue_section.tracks.len()));

    for track in &queue_section.tracks {
        push_u16_string(&mut out, &track.title);
        push_u16_string(&mut out, &track.artist);
        push_u16_string(&mut out, &track.album);
        push_u16_string(&mut out, &track.cover_path);
        push_u16_string(&mut out, &track.genre);
        push_i32(&mut out, track.year.unwrap_or(i32::MIN));
        push_u16(&mut out, track.track_number.map_or(0, clamp_u32_to_u16));
        push_f32(&mut out, track.length_seconds.unwrap_or(-1.0));
        push_u16_string(&mut out, &track.path);
    }

    out
}

fn encode_library_meta_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    let progress = snapshot.library.scan_progress.as_ref();
    let roots_completed = progress.map_or(0, |p| clamp_u32(p.roots_completed));
    let roots_total = progress.map_or(0, |p| clamp_u32(p.roots_total));
    let files_discovered = progress.map_or(0, |p| clamp_u32(p.supported_files_discovered));
    let files_processed = progress.map_or(0, |p| clamp_u32(p.supported_files_processed));
    let files_per_second = progress.and_then(|p| p.files_per_second).unwrap_or(0.0);
    let eta_seconds = progress.and_then(|p| p.eta_seconds).unwrap_or(-1.0);

    push_u32(&mut out, clamp_u32(snapshot.library.roots.len()));
    push_u32(&mut out, clamp_u32(snapshot.library.tracks.len()));
    push_u32(&mut out, clamp_u32(snapshot.library_artist_count));
    push_u32(&mut out, clamp_u32(snapshot.library_album_count));
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
        let root_str = root.path.to_string_lossy().to_string();
        push_u16_string(&mut out, &root_str);
        push_u16_string(&mut out, &root.name);
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
    push_u16_string(&mut out, &snapshot.metadata.genre);
    push_i32(&mut out, snapshot.metadata.year.unwrap_or(i32::MIN));
    push_u32(&mut out, snapshot.metadata.sample_rate_hz.unwrap_or(0));
    push_u32(&mut out, snapshot.metadata.bitrate_kbps.unwrap_or(0));
    push_u16(&mut out, snapshot.metadata.channels.map_or(0, u16::from));
    push_u16(&mut out, snapshot.metadata.bit_depth.map_or(0, u16::from));
    push_u16_string(&mut out, &snapshot.metadata.format_label);
    push_u32(
        &mut out,
        snapshot.metadata.current_bitrate_kbps.unwrap_or(0),
    );
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
    push_u32(&mut out, clamp_u32(snapshot.settings.fft_size));
    push_u8(
        &mut out,
        clamp_u8_from_i32(snapshot.settings.spectrogram_view_mode.to_i32()),
    );
    push_f32(&mut out, snapshot.settings.db_range);
    push_u8(&mut out, u8::from(snapshot.settings.display.log_scale));
    push_u8(&mut out, u8::from(snapshot.settings.display.show_fps));
    push_i32(&mut out, snapshot.settings.library_sort_mode.to_i32());
    push_u8(
        &mut out,
        u8::from(snapshot.settings.integrations.system_media_controls_enabled),
    );
    push_u8(
        &mut out,
        clamp_u8_from_i32(snapshot.settings.viewer_fullscreen_mode.to_i32()),
    );
    out
}

fn encode_lastfm_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    let auth_state = match snapshot.lastfm.auth_state {
        crate::lastfm::AuthState::Disconnected => 0u8,
        crate::lastfm::AuthState::AwaitingBrowserApproval => 1u8,
        crate::lastfm::AuthState::Connected => 2u8,
        crate::lastfm::AuthState::ReauthRequired => 3u8,
        crate::lastfm::AuthState::Error => 4u8,
    };
    push_u8(&mut out, u8::from(snapshot.lastfm.enabled));
    push_u8(&mut out, u8::from(snapshot.lastfm.build_configured));
    push_u8(&mut out, auth_state);
    push_u32(&mut out, clamp_u32(snapshot.lastfm.pending_scrobble_count));
    push_u16_string(&mut out, &snapshot.lastfm.username);
    push_u16_string(&mut out, &snapshot.lastfm.status_text);
    push_u16_string(&mut out, &snapshot.lastfm.auth_url);
    out
}

fn clamp_u16(value: usize) -> u16 {
    u16::try_from(value).unwrap_or(u16::MAX)
}

fn clamp_u32_to_u16(value: u32) -> u16 {
    u16::try_from(value).unwrap_or(u16::MAX)
}

fn clamp_u32(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

fn clamp_u8(value: usize) -> u8 {
    u8::try_from(value).unwrap_or(u8::MAX)
}

fn clamp_i32(value: usize) -> i32 {
    i32::try_from(value).unwrap_or(i32::MAX)
}

fn clamp_u8_from_i32(value: i32) -> u8 {
    u8::try_from(value).unwrap_or_default()
}

fn usize_from_u32(value: u32) -> usize {
    match usize::try_from(value) {
        Ok(value) => value,
        Err(_) => usize::MAX,
    }
}

fn usize_from_u64(value: u64) -> usize {
    match usize::try_from(value) {
        Ok(value) => value,
        Err(_) => usize::MAX,
    }
}

fn usize_from_i32(value: i32) -> Result<usize, String> {
    usize::try_from(value).map_err(|_| format!("binary command index out of range: {value}"))
}

fn parse_f32(value: f64) -> Result<f32, String> {
    if value < f64::from(f32::MIN) || value > f64::from(f32::MAX) {
        return Err("set_volume value is out of range".to_string());
    }
    value
        .to_string()
        .parse::<f32>()
        .map_err(|_| "set_volume value could not be represented".to_string())
}

fn waveform_coverage_millis(coverage_seconds: f32) -> u32 {
    let seconds = if coverage_seconds.is_finite() {
        coverage_seconds.max(0.0)
    } else {
        0.0
    };
    let millis = Duration::try_from_secs_f32(seconds)
        .map_or(u128::from(u32::MAX), |duration| duration.as_millis());
    u32::try_from(millis).unwrap_or(u32::MAX)
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
    let len = clamp_u16(bytes.len());
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(&bytes[..usize::from(len)]);
}

fn compute_analysis_delta(s: &BridgeSnapshot, emit_state: &mut AnalysisEmitState) -> AnalysisDelta {
    let waveform_changed = s.analysis.waveform_peaks != emit_state.last_waveform_peaks;
    let waveform_coverage_millis = waveform_coverage_millis(s.analysis.waveform_coverage_seconds);
    let waveform_meta_changed = waveform_coverage_millis
        != emit_state.last_waveform_coverage_millis
        || s.analysis.waveform_complete != emit_state.last_waveform_complete;
    let waveform_peaks_u8 = if waveform_changed {
        emit_state
            .last_waveform_peaks
            .clone_from(&s.analysis.waveform_peaks);
        downsample_waveform_peaks(&s.analysis.waveform_peaks, 1024)
            .into_iter()
            .map(to_u8_norm)
            .collect()
    } else {
        Vec::new()
    };
    emit_state.last_waveform_coverage_millis = waveform_coverage_millis;
    emit_state.last_waveform_complete = s.analysis.waveform_complete;

    let spectrogram_reset = s.analysis.spectrogram_seq < emit_state.last_spectrogram_seq
        || (s.analysis.spectrogram_seq == 0
            && s.analysis.spectrogram_channels.is_empty()
            && emit_state.last_spectrogram_seq > 0);
    let spectrogram_seq = s.analysis.spectrogram_seq;
    let spectrogram_delta =
        usize_from_u64(spectrogram_seq.saturating_sub(emit_state.last_spectrogram_seq));
    let spectrogram_channels_u8 =
        if spectrogram_reset && !s.analysis.spectrogram_channels.is_empty() {
            s.analysis
                .spectrogram_channels
                .iter()
                .map(|channel| EncodedSpectrogramChannel {
                    label: channel.label,
                    rows_u8: channel
                        .rows
                        .iter()
                        .map(|row| {
                            row.iter()
                                .map(|v| to_u8_spectrum(*v, s.settings.db_range, s.settings.fft_size as usize))
                                .collect::<Vec<u8>>()
                        })
                        .collect(),
                })
                .collect()
        } else if spectrogram_delta > 0 && !s.analysis.spectrogram_channels.is_empty() {
            let frame_count = s
                .analysis
                .spectrogram_channels
                .first()
                .map_or(0, |channel| channel.rows.len());
            let tail = spectrogram_delta.min(frame_count);
            let start = frame_count.saturating_sub(tail);
            s.analysis
                .spectrogram_channels
                .iter()
                .map(|channel| EncodedSpectrogramChannel {
                    label: channel.label,
                    rows_u8: channel.rows[start..]
                        .iter()
                        .map(|row| {
                            row.iter()
                                .map(|v| to_u8_spectrum(*v, s.settings.db_range, s.settings.fft_size as usize))
                                .collect::<Vec<u8>>()
                        })
                        .collect(),
                })
                .collect()
        } else {
            Vec::new()
        };
    if !spectrogram_reset || !s.analysis.spectrogram_channels.is_empty() {
        emit_state.last_spectrogram_seq = spectrogram_seq;
    }
    let has_payload = waveform_changed
        || waveform_meta_changed
        || (spectrogram_reset && !spectrogram_channels_u8.is_empty())
        || !spectrogram_channels_u8.is_empty();
    if has_payload {
        emit_state.analysis_frame_seq = emit_state.analysis_frame_seq.wrapping_add(1);
    }

    AnalysisDelta {
        sample_rate_hz: s.analysis.sample_rate_hz,
        frame_seq: emit_state.analysis_frame_seq,
        spectrogram_reset: spectrogram_reset && !spectrogram_channels_u8.is_empty(),
        waveform_changed,
        waveform_coverage_millis,
        waveform_complete: s.analysis.waveform_complete,
        waveform_peaks_u8,
        spectrogram_channels_u8,
    }
}

fn to_u8_norm(v: f32) -> u8 {
    let clamped = v.clamp(0.0, 1.0);
    round_clamped_to_u8(f64::from(clamped) * 255.0)
}

fn to_u8_spectrum(v: f32, db_range: f32, fft_size: usize) -> u8 {
    let range = f64::from(db_range.clamp(50.0, 150.0));
    let db = if v > 0.0 {
        (10.0 / std::f64::consts::LN_10) * f64::from(v).ln()
    } else {
        -200.0
    };
    // Normalise for FFT size + BH4 window energy: 20·log₁₀(N·a₀/2).
    let peak_db = 20.0 * (fft_size as f64 * 0.35875 / 2.0).log10();
    let xdb = (db + range - peak_db).clamp(0.0, range);
    round_clamped_to_u8((xdb / range) * 255.0)
}

fn round_clamped_to_u8(value: f64) -> u8 {
    let clamped = value.round().clamp(0.0, 255.0);
    clamped
        .to_string()
        .parse::<u16>()
        .ok()
        .and_then(|value| u8::try_from(value).ok())
        .unwrap_or(u8::MAX)
}

fn encode_channel_label(label: SpectrogramChannelLabel) -> u8 {
    match label {
        SpectrogramChannelLabel::Mono => 0,
        SpectrogramChannelLabel::FrontLeft => 1,
        SpectrogramChannelLabel::FrontRight => 2,
        SpectrogramChannelLabel::FrontCenter => 3,
        SpectrogramChannelLabel::Lfe => 4,
        SpectrogramChannelLabel::SideLeft => 5,
        SpectrogramChannelLabel::SideRight => 6,
        SpectrogramChannelLabel::RearLeft => 7,
        SpectrogramChannelLabel::RearRight => 8,
        SpectrogramChannelLabel::RearCenter => 9,
        SpectrogramChannelLabel::Unknown => 255,
    }
}

fn encode_analysis_frame(delta: &AnalysisDelta) -> Vec<u8> {
    let waveform_len = delta.waveform_peaks_u8.len();
    let channel_count = delta.spectrogram_channels_u8.len();
    let row_count = delta
        .spectrogram_channels_u8
        .first()
        .map_or(0, |channel| channel.rows_u8.len());
    let bin_count = delta
        .spectrogram_channels_u8
        .first()
        .and_then(|channel| channel.rows_u8.first())
        .map_or(0, std::vec::Vec::len);
    let has_spectrogram = row_count > 0 && bin_count > 0 && channel_count > 0;

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
    if delta.waveform_complete {
        flags |= ANALYSIS_FLAG_WAVEFORM_COMPLETE;
    }

    if flags == 0 {
        return Vec::new();
    }

    let waveform_len_u16 = clamp_u16(waveform_len);
    let row_count_u16 = clamp_u16(row_count);
    let bin_count_u16 = clamp_u16(bin_count);
    let channel_count_u8 = clamp_u8(channel_count);
    let label_bytes = if has_spectrogram || delta.spectrogram_reset {
        usize::from(channel_count_u8)
    } else {
        0
    };
    let spectrogram_bytes =
        usize::from(row_count_u16) * usize::from(channel_count_u8) * usize::from(bin_count_u16);
    let payload_len = 21usize + usize::from(waveform_len_u16) + label_bytes + spectrogram_bytes;

    let mut out = Vec::with_capacity(4 + payload_len);
    out.extend_from_slice(&clamp_u32(payload_len).to_le_bytes());
    out.push(ANALYSIS_FRAME_MAGIC);
    out.extend_from_slice(&delta.sample_rate_hz.to_le_bytes());
    out.push(flags);
    out.extend_from_slice(&waveform_len_u16.to_le_bytes());
    out.extend_from_slice(&delta.waveform_coverage_millis.to_le_bytes());
    out.extend_from_slice(&row_count_u16.to_le_bytes());
    out.extend_from_slice(&bin_count_u16.to_le_bytes());
    out.extend_from_slice(&delta.frame_seq.to_le_bytes());
    out.push(channel_count_u8);

    if (flags & ANALYSIS_FLAG_WAVEFORM) != 0 {
        out.extend_from_slice(&delta.waveform_peaks_u8[..usize::from(waveform_len_u16)]);
    }
    if label_bytes > 0 {
        for channel in delta
            .spectrogram_channels_u8
            .iter()
            .take(usize::from(channel_count_u8))
        {
            out.push(encode_channel_label(channel.label));
        }
    }
    if (flags & ANALYSIS_FLAG_SPECTROGRAM) != 0 {
        for row_index in 0..usize::from(row_count_u16) {
            for channel in delta
                .spectrogram_channels_u8
                .iter()
                .take(usize::from(channel_count_u8))
            {
                out.extend_from_slice(&channel.rows_u8[row_index][..usize::from(bin_count_u16)]);
            }
        }
    }

    out
}

/// Binary frame layout for precomputed spectrogram chunks:
/// ```text
/// [0]      u8   magic = 0xA2
/// [1..9]   u64  track_token
/// [9..11]  u16  bins_per_column
/// [11..13] u16  column_count
/// [13]     u8   channel_count
/// [14..18] u32  start_column_index
/// [18..22] u32  total_columns_estimate
/// [22..26] u32  sample_rate_hz
/// [26..28] u16  hop_size
/// [28..32] f32  coverage_seconds
/// [32]     u8   complete (0 or 1)
/// [33]     u8   buffer_reset (0 or 1)
/// [34..]   column_data (column_count × channel_count × bins_per_column bytes)
/// ```
fn encode_precomputed_spectrogram_chunk(chunk: &PrecomputedSpectrogramChunk) -> Vec<u8> {
    let header_len = 34;
    let data_len = chunk.columns_u8.len();
    let total_len = header_len + data_len;

    let mut out = Vec::with_capacity(4 + total_len);
    // Frame length prefix (excluding the 4-byte length itself).
    out.extend_from_slice(&clamp_u32(total_len).to_le_bytes());
    out.push(PRECOMPUTED_SPECTROGRAM_MAGIC);
    out.extend_from_slice(&chunk.track_token.to_le_bytes());
    out.extend_from_slice(&chunk.bins_per_column.to_le_bytes());
    out.extend_from_slice(&chunk.column_count.to_le_bytes());
    out.push(chunk.channel_count);
    out.extend_from_slice(&chunk.start_column_index.to_le_bytes());
    out.extend_from_slice(&chunk.total_columns_estimate.to_le_bytes());
    out.extend_from_slice(&chunk.sample_rate_hz.to_le_bytes());
    out.extend_from_slice(&chunk.hop_size.to_le_bytes());
    out.extend_from_slice(&chunk.coverage_seconds.to_le_bytes());
    out.push(u8::from(chunk.complete));
    out.push(u8::from(chunk.buffer_reset));
    out.extend_from_slice(&chunk.columns_u8);
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
    use crate::analysis::{AnalysisSnapshot, AnalysisSpectrogramChannel, SpectrogramChannelLabel};
    use crate::library::{LibraryRoot, LibrarySnapshot, LibraryTrack};
    use crate::playback::{PlaybackSnapshot, PlaybackState};
    use std::sync::Arc;
    use std::thread;
    use std::time::Instant;

    fn root(path: &str) -> LibraryRoot {
        LibraryRoot {
            path: PathBuf::from(path),
            name: String::new(),
        }
    }

    fn sample_snapshot() -> BridgeSnapshot {
        BridgeSnapshot {
            playback: PlaybackSnapshot {
                state: PlaybackState::Playing,
                position: Duration::from_secs(12),
                duration: Duration::from_secs(180),
                current: Some(PathBuf::from("/music/a.flac")),
                current_queue_index: Some(0),
                current_bitrate_kbps: Some(905),
                volume: 0.75,
                repeat_mode: RepeatMode::Off,
                shuffle_enabled: false,
            },
            analysis: AnalysisSnapshot {
                waveform_peaks: vec![0.1, 0.5, 0.9],
                waveform_coverage_seconds: 0.0,
                waveform_complete: true,
                spectrogram_channels: vec![AnalysisSpectrogramChannel {
                    label: SpectrogramChannelLabel::Mono,
                    rows: vec![vec![0.0, 1.0], vec![2.0, 3.0]],
                }],
                spectrogram_seq: 2,
                sample_rate_hz: 48_000,
                spectrogram_view_mode: SpectrogramViewMode::Downmix,
            },
            metadata: crate::metadata::TrackMetadata {
                source_path: Some("/music/a.flac".to_string()),
                title: "Sample Track".to_string(),
                artist: "Sample Artist".to_string(),
                album: "Sample Album".to_string(),
                genre: "Rock".to_string(),
                year: Some(2020),
                sample_rate_hz: Some(48_000),
                bitrate_kbps: Some(320),
                channels: Some(2),
                bit_depth: Some(24),
                format_label: "FLAC".to_string(),
                current_bitrate_kbps: Some(905),
                bitrate_timeline_kbps: vec![905, 877, 901],
                cover_art_path: Some("/music/a.cover.png".to_string()),
                cover_art_rgba: None,
            },
            library: Arc::new(LibrarySnapshot {
                roots: vec![root("/music")],
                tracks: vec![LibraryTrack {
                    path: PathBuf::from("/music/a.flac"),
                    root_path: PathBuf::from("/music"),
                    title: "Sample Track".to_string(),
                    artist: "Sample Artist".to_string(),
                    album: "Sample Album".to_string(),
                    cover_path: "/music/a.cover.png".to_string(),
                    genre: "Rock".to_string(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: Some(180.0),
                }],
                ..LibrarySnapshot::default()
            }),
            queue_details: HashMap::new(),
            library_artist_count: 1,
            library_album_count: 1,
            pre_built_tree_bytes: Some(Arc::new(vec![0, 0, 0, 0])),
            queue_included: true,
            queue: vec![PathBuf::from("/music/a.flac")],
            selected_queue_index: Some(0),
            settings: super::super::BridgeSettings {
                volume: 0.75,
                fft_size: 2048,
                spectrogram_view_mode: SpectrogramViewMode::Downmix,
                viewer_fullscreen_mode: ViewerFullscreenMode::WholeScreen,
                db_range: 132.0,
                display: super::super::BridgeDisplaySettings {
                    log_scale: false,
                    show_fps: false,
                },
                library_sort_mode: LibrarySortMode::Year,
                integrations: super::super::BridgeIntegrationSettings {
                    system_media_controls_enabled: true,
                    lastfm_scrobbling_enabled: true,
                    lastfm_username: "tester".to_string(),
                },
            },
            lastfm: crate::lastfm::RuntimeState {
                enabled: true,
                build_configured: true,
                username: "tester".to_string(),
                auth_state: crate::lastfm::AuthState::Connected,
                pending_scrobble_count: 2,
                status_text: "Connected".to_string(),
                auth_url: String::new(),
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
        out.extend_from_slice(&clamp_u16(payload.len()).to_le_bytes());
        out.extend_from_slice(payload);
        out
    }

    fn read_u32(bytes: &[u8], offset: &mut usize) -> u32 {
        let start = *offset;
        let end = start + 4;
        *offset = end;
        u32::from_le_bytes(bytes[start..end].try_into().expect("u32 bytes"))
    }

    fn read_i32(bytes: &[u8], offset: &mut usize) -> i32 {
        let start = *offset;
        let end = start + 4;
        *offset = end;
        i32::from_le_bytes(bytes[start..end].try_into().expect("i32 bytes"))
    }

    fn read_f32(bytes: &[u8], offset: &mut usize) -> f32 {
        let start = *offset;
        let end = start + 4;
        *offset = end;
        f32::from_le_bytes(bytes[start..end].try_into().expect("f32 bytes"))
    }

    fn read_u16(bytes: &[u8], offset: &mut usize) -> u16 {
        let start = *offset;
        let end = start + 2;
        *offset = end;
        u16::from_le_bytes(bytes[start..end].try_into().expect("u16 bytes"))
    }

    fn read_f64(bytes: &[u8], offset: &mut usize) -> f64 {
        let start = *offset;
        let end = start + 8;
        *offset = end;
        f64::from_le_bytes(bytes[start..end].try_into().expect("f64 bytes"))
    }

    fn read_u16_string(bytes: &[u8], offset: &mut usize) -> String {
        let len = {
            let start = *offset;
            let end = start + 2;
            *offset = end;
            usize::from(u16::from_le_bytes(
                bytes[start..end].try_into().expect("u16 bytes"),
            ))
        };
        let start = *offset;
        let end = start + len;
        *offset = end;
        String::from_utf8_lossy(&bytes[start..end]).to_string()
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

        let cmd = parse_binary_command(&encode_command(32, &[1]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetSpectrogramViewMode(mode)) => {
                assert_eq!(mode, SpectrogramViewMode::PerChannel);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(39, &[0]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetSystemMediaControlsEnabled(v)) => {
                assert!(!v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(40, &[1]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetLastFmScrobblingEnabled(v)) => {
                assert!(v);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(44, &[1]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Settings(BridgeSettingsCommand::SetViewerFullscreenMode(mode)) => {
                assert_eq!(mode, ViewerFullscreenMode::WholeScreen);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(41, &[]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::BeginLastFmAuth)
        ));

        let cmd = parse_binary_command(&encode_command(42, &[]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::CompleteLastFmAuth)
        ));

        let cmd = parse_binary_command(&encode_command(43, &[]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::DisconnectLastFm)
        ));
    }

    #[test]
    fn parse_binary_command_supports_library_batch_commands() {
        let mut payload = Vec::new();
        let first = b"/music/a.flac";
        let second = b"/music/b.flac";
        payload.extend_from_slice(&2u16.to_le_bytes());
        payload.extend_from_slice(&clamp_u16(first.len()).to_le_bytes());
        payload.extend_from_slice(first);
        payload.extend_from_slice(&clamp_u16(second.len()).to_le_bytes());
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
    fn parse_binary_command_supports_set_node_expanded() {
        let key = "artist|/music|Artist A";
        let mut payload = Vec::new();
        payload.extend_from_slice(&clamp_u16(key.len()).to_le_bytes());
        payload.extend_from_slice(key.as_bytes());
        payload.push(1);

        let cmd = parse_binary_command(&encode_command(35, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::SetNodeExpanded {
                key: parsed,
                expanded,
            }) => {
                assert_eq!(parsed, key);
                assert!(expanded);
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_binary_command_supports_set_search_query() {
        let query = "pink floyd";
        let mut payload = Vec::new();
        payload.extend_from_slice(&42u32.to_le_bytes());
        payload.extend_from_slice(&clamp_u16(query.len()).to_le_bytes());
        payload.extend_from_slice(query.as_bytes());

        let cmd = parse_binary_command(&encode_command(36, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::SetSearchQuery { seq, query: q }) => {
                assert_eq!(seq, 42);
                assert_eq!(q, query);
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn parse_binary_command_supports_all_library_track_commands() {
        let cmd = parse_binary_command(&encode_command(37, &[]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::ReplaceAllTracks) => {}
            other => panic!("unexpected command: {other:?}"),
        }

        let cmd = parse_binary_command(&encode_command(38, &[]))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::AppendAllTracks) => {}
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
    fn queue_section_encodes_enriched_track_rows() {
        let snapshot = sample_snapshot();
        let queue_section = compute_queue_section_data(&snapshot);
        let encoded = encode_queue_section(&snapshot, &queue_section);

        let mut offset = 0usize;
        assert_eq!(read_u32(&encoded, &mut offset), 1);
        assert_eq!(read_i32(&encoded, &mut offset), 0);
        assert!((read_f64(&encoded, &mut offset) - 180.0).abs() < 0.001);
        assert_eq!(read_u32(&encoded, &mut offset), 0);
        assert_eq!(read_u32(&encoded, &mut offset), 1);
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Track");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Artist");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Album");
        assert_eq!(read_u16_string(&encoded, &mut offset), "/music/a.cover.png");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Rock");
        assert_eq!(read_i32(&encoded, &mut offset), 2020);
        assert_eq!(read_u16(&encoded, &mut offset), 1);
        assert!((read_f32(&encoded, &mut offset) - 180.0).abs() < 0.001);
        assert_eq!(read_u16_string(&encoded, &mut offset), "/music/a.flac");
        assert_eq!(offset, encoded.len());
    }

    #[test]
    fn queue_section_uses_external_queue_details_when_library_misses() {
        let mut snapshot = sample_snapshot();
        snapshot.library = Arc::new(LibrarySnapshot::default());
        snapshot.queue = vec![PathBuf::from("/outside/song.flac")];
        snapshot.queue_details.insert(
            PathBuf::from("/outside/song.flac"),
            crate::library::IndexedTrack {
                title: "Outside Song".to_string(),
                artist: "Outside Artist".to_string(),
                album: "Outside Album".to_string(),
                cover_path: "/outside/cover.jpg".to_string(),
                genre: "Ambient".to_string(),
                year: Some(2024),
                track_no: Some(7),
                duration_secs: Some(245.0),
            },
        );

        let queue_section = compute_queue_section_data(&snapshot);
        assert_eq!(queue_section.unknown_duration_count, 0);
        assert!((queue_section.total_duration_secs - 245.0).abs() < 0.001);
        assert_eq!(queue_section.tracks.len(), 1);
        assert_eq!(queue_section.tracks[0].title, "Outside Song");
        assert_eq!(queue_section.tracks[0].artist, "Outside Artist");
        assert_eq!(queue_section.tracks[0].album, "Outside Album");
        assert_eq!(queue_section.tracks[0].cover_path, "/outside/cover.jpg");
        assert_eq!(queue_section.tracks[0].genre, "Ambient");
        assert_eq!(queue_section.tracks[0].year, Some(2024));
        assert_eq!(queue_section.tracks[0].track_number, Some(7));
        assert_eq!(queue_section.tracks[0].length_seconds, Some(245.0));
    }

    #[test]
    fn queue_section_cache_invalidates_when_external_queue_details_change() {
        let mut snapshot = sample_snapshot();
        snapshot.library = Arc::new(LibrarySnapshot::default());
        snapshot.queue = vec![PathBuf::from("/outside/song.flac")];
        let path = snapshot.queue[0].clone();

        let mut cache = QueueSectionCache::default();
        cache.library_ptr = Arc::as_ptr(&snapshot.library).addr();
        cache.queue_paths = snapshot.queue.clone();
        cache.queue_details_signature = queue_details_signature(&snapshot);
        cache.queue_section = compute_queue_section_data(&snapshot);

        assert!(queue_section_cache_matches(&cache, &snapshot));

        snapshot.queue_details.insert(
            path,
            crate::library::IndexedTrack {
                title: "Outside Song".to_string(),
                artist: "Outside Artist".to_string(),
                album: "Outside Album".to_string(),
                cover_path: "/outside/cover.jpg".to_string(),
                genre: "Ambient".to_string(),
                year: Some(2024),
                track_no: Some(7),
                duration_secs: Some(245.0),
            },
        );

        assert!(!queue_section_cache_matches(&cache, &snapshot));
    }

    #[test]
    fn metadata_section_encodes_genre_and_year() {
        let snapshot = sample_snapshot();
        let encoded = encode_metadata_section(&snapshot);

        let mut offset = 0usize;
        assert_eq!(read_u16_string(&encoded, &mut offset), "/music/a.flac");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Track");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Artist");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Sample Album");
        assert_eq!(read_u16_string(&encoded, &mut offset), "Rock");
        assert_eq!(read_i32(&encoded, &mut offset), 2020);
        assert_eq!(read_u32(&encoded, &mut offset), 48_000);
        assert_eq!(read_u32(&encoded, &mut offset), 320);
        let channels = {
            let start = offset;
            let end = start + 2;
            offset = end;
            u16::from_le_bytes(encoded[start..end].try_into().expect("channels"))
        };
        assert_eq!(channels, 2);
        let bit_depth = {
            let start = offset;
            let end = start + 2;
            offset = end;
            u16::from_le_bytes(encoded[start..end].try_into().expect("bit depth"))
        };
        assert_eq!(bit_depth, 24);
        assert_eq!(read_u16_string(&encoded, &mut offset), "FLAC");
        assert_eq!(read_u32(&encoded, &mut offset), 905);
        assert_eq!(read_u16_string(&encoded, &mut offset), "/music/a.cover.png");
        assert_eq!(offset, encoded.len());
    }

    #[test]
    fn snapshot_packet_contract_has_expected_shape() {
        let snapshot = sample_snapshot();
        let queue_section = compute_queue_section_data(&snapshot);
        let packet = encode_binary_snapshot(&snapshot, Some(&queue_section));
        let (magic, total_len, mask) = parse_packet_header(&packet);
        assert_eq!(magic, SNAPSHOT_MAGIC);
        assert_eq!(usize_from_u32(total_len), packet.len());
        assert_ne!(mask & SECTION_PLAYBACK, 0);
        assert_ne!(mask & SECTION_QUEUE, 0);
        assert_ne!(mask & SECTION_LIBRARY_META, 0);
        assert_eq!(mask & _SECTION_LIBRARY_TREE_RESERVED, 0);
        assert_ne!(mask & SECTION_METADATA, 0);
        assert_ne!(mask & SECTION_SETTINGS, 0);
        assert_eq!(mask & SECTION_ERROR, 0);
        assert_eq!(mask & SECTION_STOPPED, 0);
    }

    #[test]
    fn settings_section_encodes_system_media_controls_flag() {
        let mut snapshot = sample_snapshot();
        snapshot.settings.integrations.system_media_controls_enabled = false;
        snapshot.settings.viewer_fullscreen_mode = ViewerFullscreenMode::WholeScreen;
        let queue_section = compute_queue_section_data(&snapshot);
        let packet = encode_binary_snapshot(&snapshot, Some(&queue_section));
        let mut offset = 12usize;
        let playback_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += playback_len;
        let queue_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += queue_len;
        let library_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += library_len;
        let metadata_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += metadata_len;
        let settings_len = usize_from_u32(read_u32(&packet, &mut offset));
        let settings = &packet[offset..offset + settings_len];
        assert_eq!(settings.iter().rev().nth(1).copied(), Some(0));
        assert_eq!(settings.last().copied(), Some(1));
    }

    #[test]
    fn lastfm_section_encodes_runtime_state() {
        let snapshot = sample_snapshot();
        let queue_section = compute_queue_section_data(&snapshot);
        let packet = encode_binary_snapshot(&snapshot, Some(&queue_section));
        let mut offset = 12usize;
        let playback_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += playback_len;
        let queue_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += queue_len;
        let library_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += library_len;
        let metadata_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += metadata_len;
        let settings_len = usize_from_u32(read_u32(&packet, &mut offset));
        offset += settings_len;
        let lastfm_len = usize_from_u32(read_u32(&packet, &mut offset));
        let lastfm = &packet[offset..offset + lastfm_len];
        let mut lastfm_offset = 0usize;
        assert_eq!(lastfm[lastfm_offset], 1);
        lastfm_offset += 1;
        assert_eq!(lastfm[lastfm_offset], 1);
        lastfm_offset += 1;
        assert_eq!(lastfm[lastfm_offset], 2);
        lastfm_offset += 1;
        assert_eq!(read_u32(lastfm, &mut lastfm_offset), 2);
        assert_eq!(read_u16_string(lastfm, &mut lastfm_offset), "tester");
        assert_eq!(read_u16_string(lastfm, &mut lastfm_offset), "Connected");
        assert_eq!(read_u16_string(lastfm, &mut lastfm_offset), "");
        assert_eq!(lastfm_offset, lastfm.len());
    }

    #[test]
    fn snapshot_packet_can_omit_queue_section() {
        let mut snapshot = sample_snapshot();
        snapshot.queue_included = false;
        snapshot.queue.clear();

        let packet = encode_binary_snapshot(&snapshot, None);
        let (_, total_len, mask) = parse_packet_header(&packet);
        assert_eq!(usize_from_u32(total_len), packet.len());
        assert_ne!(mask & SECTION_PLAYBACK, 0);
        assert_eq!(mask & SECTION_QUEUE, 0);
        assert_ne!(mask & SECTION_METADATA, 0);
    }

    #[test]
    fn merge_queue_snapshot_preserves_latest_included_queue() {
        let queue_snapshot = sample_snapshot();
        let mut latest_snapshot = sample_snapshot();
        latest_snapshot.queue_included = false;
        latest_snapshot.queue.clear();
        latest_snapshot.selected_queue_index = None;
        latest_snapshot.playback.position = std::time::Duration::from_secs(42);

        let merged = merge_queue_snapshot(latest_snapshot, Some(queue_snapshot.clone()));

        assert!(merged.queue_included);
        assert_eq!(merged.queue, queue_snapshot.queue);
        assert_eq!(
            merged.selected_queue_index,
            queue_snapshot.selected_queue_index
        );
        assert_eq!(merged.playback.position, std::time::Duration::from_secs(42));
    }

    #[test]
    fn analysis_delta_and_frame_include_changes() {
        let snapshot = sample_snapshot();
        let mut emit_state = AnalysisEmitState::default();
        let delta = compute_analysis_delta(&snapshot, &mut emit_state);
        assert!(delta.waveform_changed);
        assert!(!delta.spectrogram_channels_u8.is_empty());
        let frame = encode_analysis_frame(&delta);
        assert!(!frame.is_empty());
        assert_eq!(frame[4], ANALYSIS_FRAME_MAGIC);
    }

    #[test]
    fn analysis_delta_sends_full_rows_on_spectrogram_reset() {
        let mut snapshot = sample_snapshot();
        snapshot.analysis.spectrogram_seq = 3;
        let mut emit_state = AnalysisEmitState {
            last_waveform_peaks: snapshot.analysis.waveform_peaks.clone(),
            last_waveform_coverage_millis: waveform_coverage_millis(
                snapshot.analysis.waveform_coverage_seconds,
            ),
            last_waveform_complete: snapshot.analysis.waveform_complete,
            last_spectrogram_seq: 9,
            ..AnalysisEmitState::default()
        };

        let delta = compute_analysis_delta(&snapshot, &mut emit_state);

        assert!(delta.spectrogram_reset);
        assert_eq!(delta.spectrogram_channels_u8.len(), 1);
        assert_eq!(delta.spectrogram_channels_u8[0].rows_u8.len(), 2);
        assert_eq!(emit_state.last_spectrogram_seq, 3);
    }

    #[test]
    fn analysis_delta_suppresses_empty_spectrogram_reset_frame() {
        let mut snapshot = sample_snapshot();
        snapshot.analysis.spectrogram_seq = 0;
        snapshot.analysis.spectrogram_channels.clear();
        snapshot.analysis.waveform_complete = false;
        let mut emit_state = AnalysisEmitState {
            last_waveform_peaks: snapshot.analysis.waveform_peaks.clone(),
            last_waveform_coverage_millis: waveform_coverage_millis(
                snapshot.analysis.waveform_coverage_seconds,
            ),
            last_waveform_complete: false,
            last_spectrogram_seq: 9,
            ..AnalysisEmitState::default()
        };

        let delta = compute_analysis_delta(&snapshot, &mut emit_state);

        assert!(!delta.spectrogram_reset);
        assert!(delta.spectrogram_channels_u8.is_empty());
        assert!(encode_analysis_frame(&delta).is_empty());
        assert_eq!(emit_state.last_spectrogram_seq, 9);
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

    fn ffi_next_tree(handle: *mut FerrousFfiBridge, timeout: Duration) -> Option<(u32, Vec<u8>)> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            unsafe {
                ferrous_ffi_bridge_poll(handle, 64);
                let mut len = 0usize;
                let mut version = 0u32;
                let ptr = ferrous_ffi_bridge_pop_library_tree(
                    handle,
                    &mut len as *mut usize,
                    &mut version as *mut u32,
                );
                if !ptr.is_null() && len > 0 {
                    let bytes = std::slice::from_raw_parts(ptr, len).to_vec();
                    ferrous_ffi_bridge_free_library_tree(ptr, len);
                    return Some((version, bytes));
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

    #[cfg(unix)]
    fn test_runtime_with_wake_pipe() -> FfiRuntime {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let (wake_read_fd, wake_write_fd) = create_nonblocking_pipe().expect("wake pipe");
        FfiRuntime::new(command_tx, wake_read_fd, wake_write_fd)
    }

    #[cfg(unix)]
    fn wake_fd_is_readable(fd: i32, timeout: Duration) -> bool {
        let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;
        let mut poll_fd = libc::pollfd {
            fd,
            events: libc::POLLIN,
            revents: 0,
        };
        loop {
            let rc = unsafe { libc::poll(&mut poll_fd, 1, timeout_ms) };
            if rc > 0 {
                return (poll_fd.revents & libc::POLLIN) != 0;
            }
            if rc == 0 {
                return false;
            }
            let err = std::io::Error::last_os_error()
                .raw_os_error()
                .unwrap_or_default();
            if err == libc::EINTR {
                continue;
            }
            return false;
        }
    }

    #[cfg(unix)]
    fn drain_wakeup_fd(fd: i32) -> usize {
        let mut total = 0usize;
        let mut buffer = [0u8; 64];
        loop {
            let read = unsafe { libc::read(fd, buffer.as_mut_ptr().cast(), buffer.len()) };
            if read > 0 {
                total += read as usize;
                continue;
            }
            if read == 0 {
                return total;
            }
            let err = std::io::Error::last_os_error()
                .raw_os_error()
                .unwrap_or_default();
            if err == libc::EINTR {
                continue;
            }
            if err == libc::EAGAIN || err == libc::EWOULDBLOCK {
                return total;
            }
            return total;
        }
    }

    #[cfg(unix)]
    fn drain_runtime_queues(runtime: &mut FfiRuntime) {
        while runtime.pop_analysis_frame().is_some() {}
        while runtime.pop_precomputed_spectrogram().is_some() {}
        while runtime.pop_library_tree_frame().is_some() {}
        while runtime.pop_search_results_frame().is_some() {}
        while runtime.pop_binary_event().is_some() {}
    }

    #[cfg(unix)]
    #[test]
    fn wake_pipe_becomes_readable_when_work_is_queued() {
        let mut runtime = test_runtime_with_wake_pipe();

        runtime.process_bridge_events(vec![BridgeEvent::Snapshot(Box::new(sample_snapshot()))]);
        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(50)
        ));
        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));

        runtime.push_search_results_frame(7, vec![1, 2, 3]);
        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(50)
        ));
        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));

        runtime.process_bridge_events(vec![BridgeEvent::Error("bad command".to_string())]);
        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(50)
        ));
        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));

        let mut stopped_runtime = test_runtime_with_wake_pipe();
        stopped_runtime.process_bridge_events(vec![BridgeEvent::Stopped]);
        assert!(wake_fd_is_readable(
            stopped_runtime.wake_read_fd,
            Duration::from_millis(50)
        ));
        drain_runtime_queues(&mut stopped_runtime);
        stopped_runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            stopped_runtime.wake_read_fd,
            Duration::from_millis(0)
        ));

        runtime.close_wakeup_pipe();
        stopped_runtime.close_wakeup_pipe();
    }

    #[cfg(unix)]
    #[test]
    fn wake_pipe_coalesces_repeated_queue_signals() {
        let mut runtime = test_runtime_with_wake_pipe();

        runtime.push_binary_event(vec![1]);
        runtime.push_binary_event(vec![2]);
        runtime.push_search_results_frame(9, vec![3, 4, 5]);

        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(50)
        ));
        assert_eq!(drain_wakeup_fd(runtime.wake_read_fd), 1);

        runtime.close_wakeup_pipe();
    }

    #[cfg(unix)]
    #[test]
    fn ack_wakeup_clears_readiness_after_drain() {
        let mut runtime = test_runtime_with_wake_pipe();

        runtime.process_bridge_events(vec![BridgeEvent::Error("oops".to_string())]);
        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(50)
        ));

        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();

        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));
        runtime.close_wakeup_pipe();
    }

    #[test]
    fn ffi_bridge_emits_snapshot_event_end_to_end() {
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());

        let tree_evt = ffi_next_tree(handle, Duration::from_secs(4)).expect("tree frame");
        assert!(tree_evt.0 > 0);
        assert!(!tree_evt.1.is_empty());

        let snapshot_evt = ffi_wait_for_mask(handle, SECTION_PLAYBACK, Duration::from_secs(4))
            .expect("snapshot event");
        let (_, _, mask) = parse_packet_header(&snapshot_evt);
        assert_ne!(mask & SECTION_QUEUE, 0);
        assert_ne!(mask & SECTION_SETTINGS, 0);
        assert_eq!(mask & _SECTION_LIBRARY_TREE_RESERVED, 0);

        assert!(ffi_send_binary(handle, &encode_command(34, &[])));
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
