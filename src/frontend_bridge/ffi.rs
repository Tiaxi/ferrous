// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{hash_map::DefaultHasher, HashMap, VecDeque};
use std::ffi::c_uchar;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[cfg(unix)]
use std::os::fd::RawFd;

use super::imports::ImportGate;
use super::{
    BridgeAnalysisCommand, BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand,
    BridgeQueueCommand, BridgeSearchResultRowType, BridgeSearchResultsFrame, BridgeSettingsCommand,
    BridgeSnapshot, FrontendBridgeHandle, LibrarySortMode, ViewerFullscreenMode,
};
use crate::analysis::{
    AnalysisEvent, PrecomputedSpectrogramChunk, SpectrogramDisplayMode, SpectrogramViewMode,
};
use crate::library::{IndexedTrack, LibraryTrack};
use crate::panic_guard;
#[cfg(test)]
use crate::panic_guard::test_hooks;
use crate::playback::{PathRenames, PlaybackState, RepeatMode};
use crate::tag_editor;

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const PRECOMPUTED_SPECTROGRAM_MAGIC: u8 = 0xA2;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_WAVEFORM_COMPLETE: u8 = 0x08;
const MAX_PENDING_BINARY_EVENTS: usize = 12;
const MAX_PENDING_ANALYSIS_FRAMES: usize = 24;
const MAX_PENDING_PRECOMPUTED_SPECTROGRAM: usize = 64;
const MAX_PENDING_LIBRARY_TREES: usize = 1;
const MAX_PENDING_SEARCH_RESULTS: usize = 2;
const MAX_PENDING_SPECTROGRAM_BYTES: usize = 16 * 1024 * 1024;
const RELAY_RECV_TIMEOUT: Duration = Duration::from_millis(250);
/// What the UI shows when the bridge thread ends without stopping normally.
const BACKEND_STOPPED_MESSAGE: &str =
    "The Ferrous backend stopped unexpectedly. Restart Ferrous to continue.";

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
    // SAFETY: `fds` is a writable array of the two descriptors `pipe2` fills.
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
    waveform_changed: bool,
    waveform_coverage_millis: u32,
    waveform_complete: bool,
    waveform_peaks_u8: Vec<u8>,
}

#[derive(Default)]
struct AnalysisEmitState {
    last_waveform_peaks: Vec<f32>,
    last_waveform_coverage_millis: u32,
    last_waveform_complete: bool,
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

struct PendingPrecomputedSpectrogramFrame {
    generation: u64,
    bytes: Vec<u8>,
}

struct FfiRuntime {
    command_tx: crossbeam_channel::Sender<BridgeCommand>,
    #[cfg(test)]
    encoder: FfiEncoder,
    pending_binary_events: VecDeque<Vec<u8>>,
    pending_analysis_frames: VecDeque<Vec<u8>>,
    pending_precomputed_spectrogram: VecDeque<PendingPrecomputedSpectrogramFrame>,
    latest_precomputed_generation: u64,
    pending_spectrogram_bytes: usize,
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
            #[cfg(test)]
            encoder: FfiEncoder::default(),
            pending_binary_events: VecDeque::with_capacity(MAX_PENDING_BINARY_EVENTS),
            pending_analysis_frames: VecDeque::with_capacity(MAX_PENDING_ANALYSIS_FRAMES),
            pending_precomputed_spectrogram: VecDeque::with_capacity(
                MAX_PENDING_PRECOMPUTED_SPECTROGRAM,
            ),
            latest_precomputed_generation: 0,
            pending_spectrogram_bytes: 0,
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
            // SAFETY: `byte` is readable for its length, and the descriptor is
            // the pipe this runtime owns until `close_wakeup_pipe`.
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
            // SAFETY: `buffer` is writable for its length, and the descriptor
            // is the pipe this runtime owns until `close_wakeup_pipe`.
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
                // SAFETY: The runtime owns this descriptor and forgets it here,
                // so it is closed once.
                unsafe { libc::close(self.wake_read_fd) };
                self.wake_read_fd = -1;
            }
            if self.wake_write_fd >= 0 {
                // SAFETY: The runtime owns this descriptor and forgets it here,
                // so it is closed once.
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

    fn advance_spectral_generation(&mut self, generation: u64) {
        if generation > self.latest_precomputed_generation {
            self.latest_precomputed_generation = generation;
            self.pending_precomputed_spectrogram
                .retain(|frame| frame.generation == 0 || frame.generation >= generation);
            self.pending_spectrogram_bytes = self
                .pending_precomputed_spectrogram
                .iter()
                .map(|frame| frame.bytes.len())
                .sum();
        }
    }

    fn push_precomputed_frame(&mut self, generation: u64, frame: Vec<u8>) {
        if generation != 0 && generation < self.latest_precomputed_generation {
            return;
        }
        self.advance_spectral_generation(generation);

        if frame.is_empty() {
            return;
        }
        let was_empty = !self.has_pending_queues();
        // Precomputed spectrogram chunks form a lossless column stream. Under
        // rapid restarts, purge stale generations but never drop the prefix of
        // the active generation; missing early columns render as sparse views.
        while self.pending_precomputed_spectrogram.len() >= MAX_PENDING_PRECOMPUTED_SPECTROGRAM {
            let Some(front) = self.pending_precomputed_spectrogram.front() else {
                break;
            };
            if generation != 0 && front.generation != 0 && front.generation >= generation {
                break;
            }
            if let Some(frame) = self.pending_precomputed_spectrogram.pop_front() {
                self.pending_spectrogram_bytes -= frame.bytes.len();
            }
        }
        self.pending_spectrogram_bytes += frame.len();
        self.pending_precomputed_spectrogram
            .push_back(PendingPrecomputedSpectrogramFrame {
                generation,
                bytes: frame,
            });
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    fn pop_precomputed_spectrogram(&mut self) -> Option<Vec<u8>> {
        let frame = self.pending_precomputed_spectrogram.pop_front()?;
        self.pending_spectrogram_bytes -= frame.bytes.len();
        Some(frame.bytes)
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

    /// Send a parsed command to the bridge. Reports a command that did not
    /// parse, or a bridge that can no longer take commands, to the UI.
    fn send_command(&mut self, parsed: Result<Option<BridgeCommand>, String>) -> bool {
        match parsed {
            Ok(None) => true,
            Ok(Some(cmd)) => {
                if self.command_tx.send(cmd).is_ok() {
                    return true;
                }
                // The bridge thread has ended, so the command was lost.
                self.fail(BACKEND_STOPPED_MESSAGE);
                false
            }
            Err(message) => {
                self.push_binary_event(encode_error_event(&message));
                false
            }
        }
    }

    /// Tell the UI that the bridge has stopped and why, unless it stopped
    /// already, normally or not.
    fn fail(&mut self, message: &str) {
        if self.stopped {
            return;
        }
        self.stopped = true;
        self.push_binary_event(encode_failure_event(message));
    }

    /// Restore the runtime's invariants after a panic while it was locked.
    ///
    /// The queues stay valid through a panic, but the byte count of the
    /// spectrogram queue may lag a push or pop, and the wake pipe may not
    /// have been signalled for queued work.
    fn recover_after_panic(&mut self) {
        self.pending_spectrogram_bytes = self
            .pending_precomputed_spectrogram
            .iter()
            .map(|frame| frame.bytes.len())
            .sum();
        self.wake_signaled = false;
        self.signal_wakeup_if_needed();
    }

    #[cfg(test)]
    fn process_bridge_events<I>(&mut self, events: I)
    where
        I: IntoIterator<Item = BridgeEvent>,
    {
        let prepared = self.encoder.prepare(events);
        self.publish(prepared);
    }

    fn publish(&mut self, events: Vec<PreparedEvent>) {
        for event in events {
            match event {
                PreparedEvent::Binary(bytes) => self.push_binary_event(bytes),
                PreparedEvent::Analysis(bytes) => self.push_analysis_frame(bytes),
                PreparedEvent::Tree(bytes) => self.push_library_tree_frame(bytes),
                PreparedEvent::Search(seq, bytes) => self.push_search_results_frame(seq, bytes),
                PreparedEvent::Precomputed(generation, bytes) => {
                    self.push_precomputed_frame(generation, bytes);
                }
                PreparedEvent::Stopped => {
                    self.stopped = true;
                }
            }
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
}

enum PreparedEvent {
    Binary(Vec<u8>),
    Analysis(Vec<u8>),
    Tree(Vec<u8>),
    Search(u32, Vec<u8>),
    Precomputed(u64, Vec<u8>),
    Stopped,
}

#[derive(Default)]
struct FfiEncoder {
    analysis_state: AnalysisEmitState,
    queue_section_cache: QueueSectionCache,
    stopped: bool,
}

impl FfiEncoder {
    fn prepare<I>(&mut self, events: I) -> Vec<PreparedEvent>
    where
        I: IntoIterator<Item = BridgeEvent>,
    {
        if self.stopped {
            return Vec::new();
        }

        let mut prepared = Vec::new();
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
                    prepared.push(PreparedEvent::Precomputed(
                        chunk.generation,
                        encode_precomputed_spectrogram_chunk(&chunk),
                    ));
                }
                BridgeEvent::Error(message) => {
                    prepared.push(PreparedEvent::Binary(encode_error_event(&message)));
                }
                BridgeEvent::SearchResults(frame) => {
                    prepared.push(PreparedEvent::Search(
                        frame.seq,
                        encode_search_results_frame(&frame),
                    ));
                }
                BridgeEvent::Stopped => {
                    self.stopped = true;
                    prepared.push(PreparedEvent::Binary(encode_stopped_event()));
                    prepared.push(PreparedEvent::Stopped);
                }
                BridgeEvent::Failed(message) => {
                    self.stopped = true;
                    prepared.push(PreparedEvent::Binary(encode_failure_event(&message)));
                    prepared.push(PreparedEvent::Stopped);
                }
            }
        }

        if let Some(tree_bytes) = latest_tree_bytes.as_ref() {
            prepared.push(PreparedEvent::Tree(tree_bytes.as_ref().clone()));
        }
        if let Some(mut snapshot) =
            latest_snapshot.map(|snapshot| merge_queue_snapshot(snapshot, latest_queue_snapshot))
        {
            if latest_tree_bytes.is_some() {
                snapshot.pre_built_tree_bytes = latest_tree_bytes;
            }
            let analysis_delta = compute_analysis_delta(&snapshot, &mut self.analysis_state);
            prepared.push(PreparedEvent::Analysis(encode_analysis_frame(
                &analysis_delta,
            )));
            let queue_section = if snapshot.queue_included {
                Some(self.queue_section_for_snapshot(&snapshot))
            } else {
                None
            };
            prepared.push(PreparedEvent::Binary(encode_binary_snapshot(
                &snapshot,
                queue_section.as_ref(),
            )));
        }
        prepared
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
    let mut encoder = FfiEncoder::default();
    loop {
        if shared.stop_requested.load(Ordering::Relaxed) {
            break;
        }

        let first_event = match event_rx.recv_timeout(RELAY_RECV_TIMEOUT) {
            Ok(event) => event,
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => continue,
            Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                // The bridge sends `Stopped` or `Failed` before it drops the
                // channel, so a channel closed without either means the bridge
                // thread ended abnormally.
                shared.lock_runtime().fail(BACKEND_STOPPED_MESSAGE);
                break;
            }
        };

        let mut events = vec![first_event];
        events.extend(event_rx.try_iter().take(63));
        let published = panic_guard::catch("the bridge event relay", || {
            prepare_and_publish(&shared, &mut encoder, events);
        });
        if let Err(message) = published {
            // Without the relay the UI hears nothing more from the bridge, so
            // stop the bridge too and show the UI why.
            let mut runtime = shared.lock_runtime();
            runtime.fail(&format!(
                "The Ferrous backend stopped after an {message}. Restart Ferrous to continue."
            ));
            let _ = runtime.command_tx.send(BridgeCommand::Shutdown);
            drop(runtime);
            shared.request_stop();
            break;
        }
        if encoder.stopped {
            shared.request_stop();
            break;
        }
    }
}

// Dedicated spectral relay may wait for UI consumption; control publication never waits here.
fn run_spectral_relay(shared: &FfiShared, event_rx: &crossbeam_channel::Receiver<AnalysisEvent>) {
    let relayed = panic_guard::catch("the spectrogram relay", || {
        relay_spectral_events(shared, event_rx);
    });
    if let Err(message) = relayed {
        // Playback goes on; only the spectrogram stops updating.
        shared
            .lock_runtime()
            .push_binary_event(encode_error_event(&format!(
                "The spectrogram stopped updating after an {message}."
            )));
    }
}

fn relay_spectral_events(
    shared: &FfiShared,
    event_rx: &crossbeam_channel::Receiver<AnalysisEvent>,
) {
    while !shared.stop_requested.load(Ordering::Relaxed) {
        let event = crossbeam_channel::select! {
            recv(shared.spectral_stop_rx) -> _ => return,
            recv(event_rx) -> event => match event { Ok(event) => event, Err(_) => return },
        };
        let AnalysisEvent::PrecomputedSpectrogramChunk(chunk) = event else {
            continue;
        };
        let bytes = encode_precomputed_spectrogram_chunk(&chunk);
        let mut runtime = shared.lock_runtime();
        loop {
            runtime.advance_spectral_generation(shared.spectral_generation.load(Ordering::Relaxed));
            if shared.stop_requested.load(Ordering::Relaxed) {
                return;
            }
            if chunk.generation != 0 && chunk.generation < runtime.latest_precomputed_generation {
                break;
            }
            if runtime.pending_spectrogram_bytes + bytes.len() <= MAX_PENDING_SPECTROGRAM_BYTES {
                break;
            }
            // A poisoned runtime is repaired by the next `lock_runtime`; the
            // queues this loop reads stay valid meanwhile.
            let (guard, _) = shared
                .spectral_space
                .wait_timeout(runtime, Duration::from_millis(50))
                .unwrap_or_else(PoisonError::into_inner);
            runtime = guard;
        }
        runtime.push_precomputed_frame(chunk.generation, bytes);
    }
}

fn prepare_and_publish(
    shared: &FfiShared,
    encoder: &mut FfiEncoder,
    events: impl IntoIterator<Item = BridgeEvent>,
) {
    // Serialization and metadata indexing belong exclusively to the relay thread.
    // The UI mutex protects only publication and removal of completed buffers.
    let prepared = encoder.prepare(events);
    shared.lock_runtime().publish(prepared);
}

#[repr(C)]
struct FfiShared {
    runtime: Mutex<FfiRuntime>,
    stop_requested: AtomicBool,
    spectral_space: Condvar,
    spectral_generation: Arc<AtomicU64>,
    spectral_stop_tx: crossbeam_channel::Sender<()>,
    spectral_stop_rx: crossbeam_channel::Receiver<()>,
}

impl FfiShared {
    fn new(runtime: FfiRuntime, spectral_generation: Arc<AtomicU64>) -> Self {
        let (spectral_stop_tx, spectral_stop_rx) = crossbeam_channel::bounded(1);
        Self {
            runtime: Mutex::new(runtime),
            stop_requested: AtomicBool::new(false),
            spectral_space: Condvar::new(),
            spectral_generation,
            spectral_stop_tx,
            spectral_stop_rx,
        }
    }

    /// Lock the runtime, repairing it if a panic poisoned the lock, so one
    /// failed call does not disable every later one.
    fn lock_runtime(&self) -> MutexGuard<'_, FfiRuntime> {
        self.runtime.lock().unwrap_or_else(|poisoned| {
            let mut runtime = poisoned.into_inner();
            runtime.recover_after_panic();
            self.runtime.clear_poison();
            runtime
        })
    }

    fn request_stop(&self) {
        self.stop_requested.store(true, Ordering::Relaxed);
        let _ = self.spectral_stop_tx.try_send(());
        self.spectral_space.notify_all();
    }
}

#[repr(C)]
pub struct FerrousFfiBridge {
    shared: Arc<FfiShared>,
    import_gate: Arc<ImportGate>,
    relay_thread: Option<JoinHandle<()>>,
    spectral_relay_thread: Option<JoinHandle<()>>,
}

impl Drop for FerrousFfiBridge {
    fn drop(&mut self) {
        self.shared.request_stop();
        let _ = self
            .shared
            .lock_runtime()
            .command_tx
            .send(BridgeCommand::Shutdown);
        for thread in [self.relay_thread.take(), self.spectral_relay_thread.take()]
            .into_iter()
            .flatten()
        {
            let _ = thread.join();
        }
        self.shared.lock_runtime().close_wakeup_pipe();
    }
}

/// Start the bridge thread and the threads that relay its output to the UI.
fn create_bridge() -> Result<FerrousFfiBridge, String> {
    let handle = FrontendBridgeHandle::spawn()?;
    let import_gate = handle.import_gate();
    let (command_tx, event_rx, spectral_rx, spectral_generation) = handle.into_parts();
    let (wake_read_fd, wake_write_fd) = create_nonblocking_pipe().unwrap_or((-1, -1));
    let shared = Arc::new(FfiShared::new(
        FfiRuntime::new(command_tx, wake_read_fd, wake_write_fd),
        spectral_generation,
    ));
    let _ = shared
        .lock_runtime()
        .command_tx
        .send(BridgeCommand::RequestSnapshot);

    // Dropping `bridge` on an error below stops everything started so far.
    let mut bridge = FerrousFfiBridge {
        shared: Arc::clone(&shared),
        import_gate,
        relay_thread: None,
        spectral_relay_thread: None,
    };
    let relay_shared = Arc::clone(&shared);
    bridge.relay_thread = Some(
        thread::Builder::new()
            .name("ferrous-ffi-relay".to_string())
            .spawn(move || run_ffi_relay_loop(relay_shared, event_rx))
            .map_err(|error| format!("failed to start the bridge event relay: {error}"))?,
    );
    bridge.spectral_relay_thread = Some(
        thread::Builder::new()
            .name("ferrous-spectral-relay".to_string())
            .spawn(move || run_spectral_relay(&shared, &spectral_rx))
            .map_err(|error| format!("failed to start the spectrogram relay: {error}"))?,
    );
    Ok(bridge)
}

/// Run the body of the exported function `name`, returning `on_panic`'s
/// value for the panic's message if it panics. A panic must not unwind out
/// of an `extern "C"` function, which would abort the process.
fn ffi_call<R>(name: &str, on_panic: impl FnOnce(String) -> R, body: impl FnOnce() -> R) -> R {
    panic_guard::catch(name, || {
        #[cfg(test)]
        test_hooks::checkpoint(name);
        body()
    })
    .unwrap_or_else(on_panic)
}

/// Write `value` through `out` unless it is null.
///
/// # Safety
///
/// `out` must be null or valid for writing one `T`.
unsafe fn write_out<T: Copy>(out: *mut T, value: T) {
    if !out.is_null() {
        // SAFETY: The caller guarantees that a non-null `out` is writable.
        unsafe { *out = value };
    }
}

/// The `len` bytes at `ptr`, or `None` for a null or empty input.
///
/// # Safety
///
/// A non-null `ptr` must point to `len` readable bytes that stay unchanged
/// for `'a`.
unsafe fn input_bytes<'a>(ptr: *const c_uchar, len: usize) -> Option<&'a [u8]> {
    if ptr.is_null() || len == 0 {
        return None;
    }
    // SAFETY: The caller guarantees `len` readable bytes at the non-null `ptr`.
    Some(unsafe { std::slice::from_raw_parts(ptr, len) })
}

/// The bridge behind `handle`, or `None` for a null handle.
///
/// # Safety
///
/// `handle` must be null or a pointer returned by
/// [`ferrous_ffi_bridge_create`] that has not been destroyed, and it must
/// stay alive for `'a`.
unsafe fn bridge_ref<'a>(handle: *mut FerrousFfiBridge) -> Option<&'a FerrousFfiBridge> {
    // SAFETY: The caller guarantees that a non-null `handle` is a live bridge.
    unsafe { handle.as_ref() }
}

/// Hand `bytes` to the caller, who frees them with [`free_raw_buffer`]
/// through the matching exported free function.
///
/// # Safety
///
/// `len_out` must be null or writable for one `usize`.
unsafe fn into_raw_buffer(bytes: Vec<u8>, len_out: *mut usize) -> *mut c_uchar {
    let len = bytes.len();
    let ptr = Box::into_raw(bytes.into_boxed_slice()).cast::<c_uchar>();
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { write_out(len_out, len) };
    ptr
}

/// Free a buffer that [`into_raw_buffer`] handed out.
///
/// # Safety
///
/// `ptr` must be null, or `ptr` and `len` must be a pointer and length from
/// [`into_raw_buffer`] whose buffer has not been freed.
unsafe fn free_raw_buffer(ptr: *mut c_uchar, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    // SAFETY: The caller guarantees that `ptr` and `len` describe a boxed
    // slice that `into_raw_buffer` leaked and nothing has freed.
    drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)) });
}

/// Pop one buffer from the bridge's runtime with `pop` and hand it out.
///
/// # Safety
///
/// `handle` must be null or a live bridge, and `len_out` must be null or
/// writable for one `usize`.
unsafe fn pop_buffer(
    name: &str,
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
    pop: impl FnOnce(&FfiShared, &mut FfiRuntime) -> Option<Vec<u8>>,
) -> *mut c_uchar {
    // Zeroed before anything can panic, so a failed call reports no buffer.
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { write_out(len_out, 0) };
    ffi_call(
        name,
        |_| std::ptr::null_mut(),
        || {
            // SAFETY: The caller guarantees that a non-null `handle` is live.
            let Some(bridge) = (unsafe { bridge_ref(handle) }) else {
                return std::ptr::null_mut();
            };
            let bytes = pop(&bridge.shared, &mut bridge.shared.lock_runtime());
            match bytes {
                // SAFETY: The caller guarantees that `len_out` is null or writable.
                Some(bytes) => unsafe { into_raw_buffer(bytes, len_out) },
                None => std::ptr::null_mut(),
            }
        },
    )
}

/// Run a tag editor request that answers with a JSON buffer. A panic in the
/// request answers with an error response, which the UI shows like any
/// failed request; invalid input answers with null.
///
/// # Safety
///
/// `len_out` must be null or writable for one `usize`.
unsafe fn tag_editor_call(
    name: &str,
    len_out: *mut usize,
    request: impl FnOnce() -> Option<Vec<u8>>,
) -> *mut c_uchar {
    // Zeroed before anything can panic, so a failed call reports no buffer.
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { write_out(len_out, 0) };
    ffi_call(
        name,
        |_| std::ptr::null_mut(),
        || {
            let response = panic_guard::catch(name, || {
                #[cfg(test)]
                test_hooks::checkpoint("tag_editor_request");
                request()
            })
            .unwrap_or_else(|message| Some(tag_editor::serialize_error_response(&message)));
            match response {
                // SAFETY: The caller guarantees that `len_out` is null or writable.
                Some(bytes) => unsafe { into_raw_buffer(bytes, len_out) },
                None => std::ptr::null_mut(),
            }
        },
    )
}

/// Shared retention policy used by the decoder and each Qt channel view.
#[no_mangle]
pub extern "C" fn ferrous_ffi_spectrogram_ring_capacity(
    width: u32,
    sample_rate: u32,
    hop: u32,
    bins: u32,
    channels: u32,
    zoom: f64,
    centered: bool,
) -> u64 {
    ffi_call(
        "ferrous_ffi_spectrogram_ring_capacity",
        |_| 0,
        || {
            crate::analysis::retention::SpectrogramBufferConfig {
                width,
                sample_rate,
                hop,
                bins,
                channels,
                zoom,
                centered,
            }
            .limits(crate::analysis::retention::lookahead_seconds())
            .capacity
        },
    )
}

/// Start the bridge. Returns null if it cannot start.
#[no_mangle]
pub extern "C" fn ferrous_ffi_bridge_create() -> *mut FerrousFfiBridge {
    ffi_call(
        "ferrous_ffi_bridge_create",
        |_| std::ptr::null_mut(),
        || match create_bridge() {
            Ok(bridge) => Box::into_raw(Box::new(bridge)),
            Err(message) => {
                eprintln!("[ferrous] {message}");
                std::ptr::null_mut()
            }
        },
    )
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a pointer previously returned by
/// [`ferrous_ffi_bridge_create`], and must not be used again after this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_destroy(handle: *mut FerrousFfiBridge) {
    ffi_call(
        "ferrous_ffi_bridge_destroy",
        |_| (),
        || {
            if !handle.is_null() {
                // SAFETY: The caller hands over the pointer `Box::into_raw`
                // made in `ferrous_ffi_bridge_create` and never uses it again.
                drop(unsafe { Box::from_raw(handle) });
            }
        },
    );
}

#[no_mangle]
/// Send a binary command to the bridge. Returns false if the command is
/// invalid or the bridge has stopped; the UI receives the reason as an event.
///
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// `cmd_ptr` must point to `cmd_len` readable bytes for the duration of this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_send_binary(
    handle: *mut FerrousFfiBridge,
    cmd_ptr: *const c_uchar,
    cmd_len: usize,
) -> bool {
    ffi_call(
        "ferrous_ffi_bridge_send_binary",
        |_| false,
        || {
            // SAFETY: The caller guarantees that a non-null `handle` is live.
            let Some(bridge) = (unsafe { bridge_ref(handle) }) else {
                return false;
            };
            // SAFETY: The caller guarantees `cmd_len` readable bytes at `cmd_ptr`.
            let Some(payload) = (unsafe { input_bytes(cmd_ptr, cmd_len) }) else {
                return false;
            };
            // Parse before locking, so the UI's queues stay free meanwhile.
            let parsed = parse_binary_command(payload);
            let mut runtime = bridge.shared.lock_runtime();
            #[cfg(test)]
            test_hooks::checkpoint("send_binary:locked");
            runtime.send_command(parsed)
        },
    )
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_wakeup_fd(handle: *mut FerrousFfiBridge) -> i32 {
    ffi_call(
        "ferrous_ffi_bridge_wakeup_fd",
        |_| -1,
        || {
            // SAFETY: The caller guarantees that a non-null `handle` is live.
            unsafe { bridge_ref(handle) }
                .map_or(-1, |bridge| bridge.shared.lock_runtime().wake_read_fd)
        },
    )
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_ack_wakeup(handle: *mut FerrousFfiBridge) {
    ffi_call(
        "ferrous_ffi_bridge_ack_wakeup",
        |_| (),
        || {
            // SAFETY: The caller guarantees that a non-null `handle` is live.
            if let Some(bridge) = unsafe { bridge_ref(handle) } {
                bridge.shared.lock_runtime().ack_wakeup();
            }
        },
    );
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_binary_event(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
) -> *mut c_uchar {
    // SAFETY: The caller upholds `pop_buffer`'s contract, which is this one's.
    unsafe {
        pop_buffer(
            "ferrous_ffi_bridge_pop_binary_event",
            handle,
            len_out,
            |_, runtime| runtime.pop_binary_event(),
        )
    }
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_binary_event`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_binary_event(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_bridge_free_binary_event",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the pop function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_analysis_frame(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
) -> *mut c_uchar {
    // SAFETY: The caller upholds `pop_buffer`'s contract, which is this one's.
    unsafe {
        pop_buffer(
            "ferrous_ffi_bridge_pop_analysis_frame",
            handle,
            len_out,
            |_, runtime| runtime.pop_analysis_frame(),
        )
    }
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_analysis_frame`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_analysis_frame(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_bridge_free_analysis_frame",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the pop function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` is non-null, it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_precomputed_spectrogram(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
) -> *mut c_uchar {
    // SAFETY: The caller upholds `pop_buffer`'s contract, which is this one's.
    unsafe {
        pop_buffer(
            "ferrous_ffi_bridge_pop_precomputed_spectrogram",
            handle,
            len_out,
            |shared, runtime| {
                let frame = runtime.pop_precomputed_spectrogram()?;
                shared.spectral_space.notify_one();
                Some(frame)
            },
        )
    }
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
    ffi_call(
        "ferrous_ffi_bridge_free_precomputed_spectrogram",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the pop function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` or `version_out` are non-null, each must be writable for one value.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_library_tree(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
    version_out: *mut u32,
) -> *mut c_uchar {
    // SAFETY: The caller guarantees that `version_out` is null or writable.
    unsafe { write_out(version_out, 0) };
    let pop = |_: &FfiShared, runtime: &mut FfiRuntime| {
        let frame = runtime.pop_library_tree_frame()?;
        // SAFETY: The caller guarantees that `version_out` is null or writable.
        unsafe { write_out(version_out, frame.version) };
        Some(frame.bytes)
    };
    // SAFETY: The caller upholds `pop_buffer`'s contract, which is part of this one.
    unsafe { pop_buffer("ferrous_ffi_bridge_pop_library_tree", handle, len_out, pop) }
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_library_tree`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_library_tree(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_bridge_free_library_tree",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the pop function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
/// If `len_out` or `seq_out` are non-null, each must be writable for one value.
pub unsafe extern "C" fn ferrous_ffi_bridge_pop_search_results(
    handle: *mut FerrousFfiBridge,
    len_out: *mut usize,
    seq_out: *mut u32,
) -> *mut c_uchar {
    // SAFETY: The caller guarantees that `seq_out` is null or writable.
    unsafe { write_out(seq_out, 0) };
    let pop = |_: &FfiShared, runtime: &mut FfiRuntime| {
        let frame = runtime.pop_search_results_frame()?;
        // SAFETY: The caller guarantees that `seq_out` is null or writable.
        unsafe { write_out(seq_out, frame.seq) };
        Some(frame.bytes)
    };
    // SAFETY: The caller upholds `pop_buffer`'s contract, which is part of this one.
    unsafe {
        pop_buffer(
            "ferrous_ffi_bridge_pop_search_results",
            handle,
            len_out,
            pop,
        )
    }
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by
/// [`ferrous_ffi_bridge_pop_search_results`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_bridge_free_search_results(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_bridge_free_search_results",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the pop function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// Read the tags of the files a JSON request names. A file that cannot be
/// read is reported in the response's failures.
///
/// # Safety
///
/// `paths_ptr` must point to `paths_len` readable bytes for the duration of this call.
/// If `len_out` is non-null it must be writable for one `usize`.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_load(
    paths_ptr: *const c_uchar,
    paths_len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    let request = || {
        // SAFETY: The caller guarantees `paths_len` readable bytes at `paths_ptr`.
        let blob = unsafe { input_bytes(paths_ptr, paths_len) }?;
        let paths = tag_editor::parse_paths_blob(blob).ok()?;
        let response = tag_editor::load_rows_for_paths(&paths);
        tag_editor::serialize_load_response(&response).ok()
    };
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { tag_editor_call("ferrous_ffi_tag_editor_load", len_out, request) }
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
    let request = || {
        // SAFETY: The caller guarantees `save_len` readable bytes at `save_ptr`.
        let blob = unsafe { input_bytes(save_ptr, save_len) }?;
        let request = tag_editor::parse_save_request(blob).ok()?;
        let response = tag_editor::save_rows(request);
        tag_editor::serialize_save_response(&response).ok()
    };
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { tag_editor_call("ferrous_ffi_tag_editor_save", len_out, request) }
}

#[no_mangle]
/// Rename files without retaining a bridge handle. The caller publishes successful pairs later.
///
/// # Safety
/// `data` must contain `len` readable bytes; `len_out` must be null or writable.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_rename(
    data: *const c_uchar,
    len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    let request = || {
        // SAFETY: The caller guarantees `len` readable bytes at `data`.
        let blob = unsafe { input_bytes(data, len) }?;
        let request = tag_editor::parse_rename_request(blob).ok()?;
        let response = tag_editor::rename_rows(request);
        tag_editor::serialize_rename_response(&response).ok()
    };
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { tag_editor_call("ferrous_ffi_tag_editor_rename", len_out, request) }
}

/// A reference to the bridge's import gate that a tag editor worker can own.
///
/// File renames go through it so the bridge learns about them in order with
/// the import results that read the same files; see [`ImportGate`]. It stays
/// valid after the bridge is destroyed, when renames still happen on disk but
/// nothing is told about them.
pub struct FerrousFfiRenameGate {
    gate: Arc<ImportGate>,
}

#[no_mangle]
/// Take a reference to the bridge's import gate. It does not lock the gate,
/// so the UI thread may call it. Release it with
/// [`ferrous_ffi_rename_gate_free`].
///
/// # Safety
///
/// `handle` must be null or a valid pointer returned by [`ferrous_ffi_bridge_create`].
pub unsafe extern "C" fn ferrous_ffi_bridge_rename_gate(
    handle: *mut FerrousFfiBridge,
) -> *mut FerrousFfiRenameGate {
    ffi_call(
        "ferrous_ffi_bridge_rename_gate",
        |_| std::ptr::null_mut(),
        || {
            // SAFETY: The caller guarantees that a non-null `handle` is live.
            let Some(bridge) = (unsafe { bridge_ref(handle) }) else {
                return std::ptr::null_mut();
            };
            Box::into_raw(Box::new(FerrousFfiRenameGate {
                gate: Arc::clone(&bridge.import_gate),
            }))
        },
    )
}

#[no_mangle]
/// # Safety
///
/// `gate` must be null or a pointer returned by [`ferrous_ffi_bridge_rename_gate`]
/// that is not used again after this call.
pub unsafe extern "C" fn ferrous_ffi_rename_gate_free(gate: *mut FerrousFfiRenameGate) {
    ffi_call(
        "ferrous_ffi_rename_gate_free",
        |_| (),
        || {
            if !gate.is_null() {
                // SAFETY: The caller transfers ownership of a pointer from `Box::into_raw`.
                drop(unsafe { Box::from_raw(gate) });
            }
        },
    );
}

#[no_mangle]
/// Rename files like [`ferrous_ffi_tag_editor_rename`] and tell the bridge
/// about the successful renames as one change. It renames files on disk and
/// waits while another rename is in progress, so call it from a worker
/// thread, never from the UI thread.
///
/// # Safety
///
/// `gate` must be null or a valid pointer returned by [`ferrous_ffi_bridge_rename_gate`].
/// `data` must contain `len` readable bytes; `len_out` must be null or writable.
pub unsafe extern "C" fn ferrous_ffi_rename_gate_rename(
    gate: *const FerrousFfiRenameGate,
    data: *const c_uchar,
    len: usize,
    len_out: *mut usize,
) -> *mut c_uchar {
    let request = || {
        // SAFETY: The caller guarantees that a non-null `gate` is live.
        let gate = unsafe { gate.as_ref() }?;
        // SAFETY: The caller guarantees `len` readable bytes at `data`.
        let blob = unsafe { input_bytes(data, len) }?;
        let request = tag_editor::parse_rename_request(blob).ok()?;
        // A panic in the rename still finishes it in the gate, so imports
        // waiting for it go on.
        let response = gate.gate.rename(|| {
            let response = tag_editor::rename_rows(request);
            let renames = renamed_paths(&response);
            (response, renames)
        });
        tag_editor::serialize_rename_response(&response).ok()
    };
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { tag_editor_call("ferrous_ffi_rename_gate_rename", len_out, request) }
}

/// The files `response` reports as moved to a new path.
fn renamed_paths(response: &tag_editor::RenameResponse) -> PathRenames {
    response
        .results
        .iter()
        .filter(|result| result.ok)
        .filter_map(|result| {
            let new_path = result.new_path.as_deref()?;
            (result.path != new_path)
                .then(|| (PathBuf::from(&result.path), PathBuf::from(new_path)))
        })
        .collect()
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer previously returned by one of the tag editor helpers.
pub unsafe extern "C" fn ferrous_ffi_tag_editor_free_buffer(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_tag_editor_free_buffer",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer a tag editor function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
}

#[no_mangle]
/// Decode one waveform viewport at display resolution, with cooperative
/// cancellation between decoder packets. Returns null if the file cannot be
/// decoded or the decode was cancelled.
///
/// # Safety
///
/// `path_ptr` must point to `path_len` readable UTF-8 bytes for this call.
/// If `len_out` is non-null it must be writable for one `usize`.
/// `cancelled` must be safe to call with `context` from this thread until
/// this call returns.
pub unsafe extern "C" fn ferrous_ffi_waveform_window_cancellable(
    path_ptr: *const c_uchar,
    path_len: usize,
    start_seconds: f64,
    end_seconds: f64,
    max_points: u32,
    len_out: *mut usize,
    cancelled: unsafe extern "C" fn(*const std::ffi::c_void) -> bool,
    context: *const std::ffi::c_void,
) -> *mut c_uchar {
    // Zeroed before anything can panic, so a failed call reports no buffer.
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { write_out(len_out, 0) };
    ffi_call(
        "ferrous_ffi_waveform_window_cancellable",
        |_| std::ptr::null_mut(),
        || {
            if max_points == 0 {
                return std::ptr::null_mut();
            }
            // SAFETY: The caller supplies a readable path for the duration of the call.
            let Some(bytes) = (unsafe { input_bytes(path_ptr, path_len) }) else {
                return std::ptr::null_mut();
            };
            let Ok(path) = std::str::from_utf8(bytes) else {
                return std::ptr::null_mut();
            };
            let is_cancelled = || {
                // SAFETY: The caller guarantees the callback may be called with
                // `context` until this function returns.
                unsafe { cancelled(context) }
            };
            let Ok(window) = crate::analysis::decode_waveform_window_cancellable(
                &PathBuf::from(path),
                start_seconds,
                end_seconds,
                usize::try_from(max_points).unwrap_or(usize::MAX),
                &is_cancelled,
            ) else {
                return std::ptr::null_mut();
            };
            // SAFETY: The caller guarantees that `len_out` is null or writable.
            unsafe { into_raw_buffer(encode_waveform_window(&window), len_out) }
        },
    )
}

#[no_mangle]
/// # Safety
///
/// `ptr` and `len` must describe a buffer returned by
/// [`ferrous_ffi_waveform_window_cancellable`] and not yet freed.
pub unsafe extern "C" fn ferrous_ffi_waveform_window_free(ptr: *mut c_uchar, len: usize) {
    ffi_call(
        "ferrous_ffi_waveform_window_free",
        |_| (),
        || {
            // SAFETY: The caller passes a buffer the waveform function handed out.
            unsafe { free_raw_buffer(ptr, len) };
        },
    );
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
    ffi_call(
        "ferrous_ffi_fuzzy_match_score",
        |_| 0.0,
        || {
            let to_str = |ptr: *const c_uchar, len: usize| -> &str {
                // SAFETY: The caller guarantees `len` readable bytes at each
                // non-null `ptr` for the duration of this call.
                unsafe { input_bytes(ptr, len) }
                    .and_then(|bytes| std::str::from_utf8(bytes).ok())
                    .unwrap_or("")
            };
            crate::fuzzy_match::itunes_relevance_score(
                to_str(candidate_album_ptr, candidate_album_len),
                to_str(candidate_artist_ptr, candidate_artist_len),
                to_str(wanted_album_ptr, wanted_album_len),
                to_str(wanted_artist_ptr, wanted_artist_len),
            )
        },
    )
}

fn encode_waveform_window(window: &crate::analysis::WaveformWindow) -> Vec<u8> {
    const HEADER_BYTES: usize = 36;
    let values = window
        .extrema
        .len()
        .saturating_add(window.downmix_extrema.len());
    let mut bytes = Vec::with_capacity(HEADER_BYTES.saturating_add(values.saturating_mul(4)));
    // WVF2 appends point_count mono min/max pairs after native channel extrema.
    bytes.extend_from_slice(b"WVF2");
    bytes.extend_from_slice(&window.sample_rate_hz.to_le_bytes());
    bytes.extend_from_slice(&window.channel_count.to_le_bytes());
    bytes.extend_from_slice(&0_u16.to_le_bytes());
    bytes.extend_from_slice(&window.start_seconds.to_le_bytes());
    bytes.extend_from_slice(&window.end_seconds.to_le_bytes());
    bytes.extend_from_slice(&window.frames_per_point.to_le_bytes());
    let values_per_channel = if window.channel_count == 0 {
        0
    } else {
        window.extrema.len() / (usize::from(window.channel_count) * 2)
    };
    bytes.extend_from_slice(
        &u32::try_from(values_per_channel)
            .unwrap_or(u32::MAX)
            .to_le_bytes(),
    );
    for value in window.extrema.iter().chain(&window.downmix_extrema) {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
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
    } else if let Some(command) = parse_analysis_command(cmd_id, &mut reader)? {
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
        53 => {
            let ch = reader.read_u8()?;
            BridgeCommand::Playback(BridgePlaybackCommand::ToggleChannelMute(ch))
        }
        54 => {
            let ch = reader.read_u8()?;
            BridgeCommand::Playback(BridgePlaybackCommand::SoloChannel(ch))
        }
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
        63 => {
            let count = reader.read_u16()?;
            let mut indices = Vec::with_capacity(usize::from(count));
            for _ in 0..count {
                indices.push(usize_from_u32(reader.read_u32()?));
            }
            BridgeQueueCommand::RemoveMany(indices)
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
        60 => BridgeLibraryCommand::SetViewState {
            selection_key: reader.read_u16_string()?,
            anchor_key: reader.read_u16_string()?,
            anchor_offset: match reader.read_f32()? {
                value if value.is_finite() => value,
                _ => return Err("library view anchor offset must be finite".to_string()),
            },
        },
        61 => BridgeLibraryCommand::ShowTrackInLibrary(PathBuf::from(reader.read_u16_string()?)),
        36 => BridgeLibraryCommand::SetSearchQuery {
            seq: reader.read_u32()?,
            query: reader.read_u16_string()?,
            limit: if reader.offset == reader.bytes.len() {
                0
            } else {
                reader.read_u32()?
            },
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
        65 => BridgeSettingsCommand::SetShowLevelMeter(reader.read_u8()? != 0),
        30 => {
            BridgeSettingsCommand::SetLibrarySortMode(LibrarySortMode::from_i32(reader.read_i32()?))
        }
        31 => BridgeSettingsCommand::SetFftSize(usize_from_u32(reader.read_u32()?)),
        32 => BridgeSettingsCommand::SetSpectrogramViewMode(SpectrogramViewMode::from_i32(
            i32::from(reader.read_u8()?),
        )),
        39 => BridgeSettingsCommand::SetSystemMediaControlsEnabled(reader.read_u8()? != 0),
        50 => BridgeSettingsCommand::SetSpectrogramDisplayMode(match reader.read_u8()? {
            1 => SpectrogramDisplayMode::Centered,
            _ => SpectrogramDisplayMode::Rolling,
        }),
        51 => BridgeSettingsCommand::SetShowSpectrogramCrosshair(reader.read_u8()? != 0),
        52 => BridgeSettingsCommand::SetShowSpectrogramScale(reader.read_u8()? != 0),
        55 => {
            let v = reader.read_u8()?.min(2);
            BridgeSettingsCommand::SetChannelButtonsVisibility(v)
        }
        56 => BridgeSettingsCommand::SetSpectrogramZoomEnabled(reader.read_u8()? != 0),
        40 => BridgeSettingsCommand::SetLastFmScrobblingEnabled(reader.read_u8()? != 0),
        41 => BridgeSettingsCommand::BeginLastFmAuth,
        42 => BridgeSettingsCommand::CompleteLastFmAuth,
        43 => BridgeSettingsCommand::DisconnectLastFm,
        44 => BridgeSettingsCommand::SetViewerFullscreenMode(ViewerFullscreenMode::from_i32(
            i32::from(reader.read_u8()?),
        )),
        59 => BridgeSettingsCommand::SetPreventDisplaySleepInFullscreen(reader.read_u8()? != 0),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Settings(command)))
}

fn parse_analysis_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        57 => {
            let level = reader.read_f32()?;
            if !level.is_finite() || level <= 0.0 {
                return Err("zoom level must be finite and positive".to_string());
            }
            BridgeAnalysisCommand::SetSpectrogramZoomLevel(level)
        }
        58 => BridgeAnalysisCommand::SetSpectrogramWidgetWidth(reader.read_u32()?),
        64 => BridgeAnalysisCommand::SetSpectrogramActive(reader.read_u8()? != 0),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::Analysis(command)))
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

/// A stop the UI did not ask for: the stopped section with the reason.
fn encode_failure_event(message: &str) -> Vec<u8> {
    let mut payload = Vec::new();
    push_u16_string(&mut payload, message);
    encode_packet(vec![
        (SECTION_ERROR, payload),
        (SECTION_STOPPED, Vec::new()),
    ])
}

fn encode_search_results_frame(frame: &BridgeSearchResultsFrame) -> Vec<u8> {
    let mut out = Vec::with_capacity(64 + frame.rows.len() * 128);
    push_u8(&mut out, b'S');
    push_u8(&mut out, 4);
    push_u16(&mut out, clamp_u16(frame.rows.len()));
    push_u32(&mut out, frame.seq);
    for total in frame.totals {
        push_u32(&mut out, total);
    }
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
        push_u16_string(&mut out, &row.match_detail);
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
    push_u64(&mut out, snapshot.playback.muted_channels_mask);
    push_u8(
        &mut out,
        snapshot.playback.soloed_channel.map_or(0xFF, |ch| ch),
    );
    push_u64(&mut out, snapshot.playback.track_token);
    push_u64(&mut out, snapshot.playback.seek_serial);
    push_u64(&mut out, snapshot.playback.seek_landed_serial);
    // The position above was read at this CLOCK_MONOTONIC time, and
    // advances from it in real time while the flag is set.
    push_u64(&mut out, snapshot.playback.position_sampled_at.as_nanos());
    push_u8(&mut out, u8::from(snapshot.playback.position_advancing));
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
    if snapshot.pre_built_tree_bytes.is_some() {
        push_u16(&mut out, clamp_u16(snapshot.library_expanded_keys.len()));
        for key in snapshot
            .library_expanded_keys
            .iter()
            .take(usize::from(u16::MAX))
        {
            push_u16_string(&mut out, key);
        }
        push_u16_string(&mut out, &snapshot.library_view_selection_key);
        push_u16_string(&mut out, &snapshot.library_view_anchor_key);
        push_f32(&mut out, snapshot.library_view_anchor_offset);
        push_u32(&mut out, snapshot.library_reveal_generation);
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
    push_u16_string(&mut out, &snapshot.metadata.album_artist);
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
    push_u32(&mut out, snapshot.metadata.track_number.unwrap_or(0));
    // Optional metadata tail: native decoder order, not inferred from channel count.
    push_u16(&mut out, clamp_u16(snapshot.metadata.channel_labels.len()));
    for label in snapshot
        .metadata
        .channel_labels
        .iter()
        .take(usize::from(u16::MAX))
    {
        push_u16_string(&mut out, label);
    }
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
    push_u8(
        &mut out,
        match snapshot.settings.spectrogram_display_mode {
            SpectrogramDisplayMode::Rolling => 0,
            SpectrogramDisplayMode::Centered => 1,
        },
    );
    push_u8(
        &mut out,
        u8::from(snapshot.settings.display.show_spectrogram_crosshair),
    );
    push_u8(
        &mut out,
        u8::from(snapshot.settings.display.show_spectrogram_scale),
    );
    push_u8(
        &mut out,
        snapshot.settings.display.channel_buttons_visibility,
    );
    push_u8(
        &mut out,
        u8::from(snapshot.settings.display.spectrogram_zoom_enabled),
    );
    push_u8(
        &mut out,
        u8::from(
            snapshot
                .settings
                .display
                .prevent_display_sleep_in_fullscreen,
        ),
    );
    push_u8(
        &mut out,
        u8::from(snapshot.settings.display.show_level_meter),
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

fn push_u64(out: &mut Vec<u8>, value: u64) {
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

#[allow(clippy::too_many_lines)]
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

    let has_payload = waveform_changed || waveform_meta_changed;
    if has_payload {
        emit_state.analysis_frame_seq = emit_state.analysis_frame_seq.wrapping_add(1);
    }

    AnalysisDelta {
        sample_rate_hz: s.analysis.sample_rate_hz,
        frame_seq: emit_state.analysis_frame_seq,
        waveform_changed,
        waveform_coverage_millis,
        waveform_complete: s.analysis.waveform_complete,
        waveform_peaks_u8,
    }
}

fn to_u8_norm(v: f32) -> u8 {
    let clamped = v.clamp(0.0, 1.0);
    round_clamped_to_u8(f64::from(clamped) * 255.0)
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

/// Encode an analysis frame for the binary bridge protocol.
/// Streaming spectrogram fields (rows, channels, reset) are always zero.
fn encode_analysis_frame(delta: &AnalysisDelta) -> Vec<u8> {
    let waveform_len = delta.waveform_peaks_u8.len();

    let mut flags = 0u8;
    if delta.waveform_changed && waveform_len > 0 {
        flags |= ANALYSIS_FLAG_WAVEFORM;
    }
    if delta.waveform_complete {
        flags |= ANALYSIS_FLAG_WAVEFORM_COMPLETE;
    }

    if flags == 0 {
        return Vec::new();
    }

    let waveform_len_u16 = clamp_u16(waveform_len);
    // Streaming spectrogram fields are zero (rows=0, bins=0, channels=0).
    let payload_len = 21usize + usize::from(waveform_len_u16);

    let mut out = Vec::with_capacity(4 + payload_len);
    out.extend_from_slice(&clamp_u32(payload_len).to_le_bytes());
    out.push(ANALYSIS_FRAME_MAGIC);
    out.extend_from_slice(&delta.sample_rate_hz.to_le_bytes());
    out.push(flags);
    out.extend_from_slice(&waveform_len_u16.to_le_bytes());
    out.extend_from_slice(&delta.waveform_coverage_millis.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes()); // row_count = 0
    out.extend_from_slice(&0u16.to_le_bytes()); // bin_count = 0
    out.extend_from_slice(&delta.frame_seq.to_le_bytes());
    out.push(0); // channel_count = 0

    if (flags & ANALYSIS_FLAG_WAVEFORM) != 0 {
        out.extend_from_slice(&delta.waveform_peaks_u8[..usize::from(waveform_len_u16)]);
    }

    out
}

/// Binary frame layout for precomputed spectrogram chunks:
/// ```text
/// [0]      u8   magic = 0xA2
/// [1..9]   u64  track_token
/// [9..17]  u64  generation
/// [17..19] u16  bins_per_column
/// [19..21] u16  column_count
/// [21]     u8   channel_count
/// [22..26] u32  start_column_index
/// [26..30] u32  total_columns_estimate
/// [30..34] u32  sample_rate_hz
/// [34..36] u16  hop_size
/// [36..40] f32  coverage_seconds
/// [40]     u8   complete (0 or 1)
/// [41]     u8   buffer_reset (0 or 1)
/// [42]     u8   clear_history (0 or 1)
/// [43..]   column_data (column_count × channel_count × bins_per_column bytes)
/// ```
fn encode_precomputed_spectrogram_chunk(chunk: &PrecomputedSpectrogramChunk) -> Vec<u8> {
    let header_len = 43;
    let data_len = chunk.columns_u8.len();
    let total_len = header_len + data_len;

    let mut out = Vec::with_capacity(4 + total_len);
    // Frame length prefix (excluding the 4-byte length itself).
    out.extend_from_slice(&clamp_u32(total_len).to_le_bytes());
    out.push(PRECOMPUTED_SPECTROGRAM_MAGIC);
    out.extend_from_slice(&chunk.track_token.to_le_bytes());
    out.extend_from_slice(&chunk.generation.to_le_bytes());
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
    out.push(u8::from(chunk.clear_history));
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
mod error_packet_tests {
    use super::{encode_error_event, encode_failure_event};

    #[test]
    fn failure_packet_layout_matches_the_qt_decoder() {
        // Mirrored by BridgeClientTest::backendFailurePacketShowsStoppedState.
        let message = "The Ferrous backend stopped.";
        let text = message.as_bytes();
        let mut expected = Vec::new();
        expected.extend_from_slice(&0xFE55_0001_u32.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(22 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&((1_u16 << 6) | (1_u16 << 7)).to_le_bytes());
        expected.extend_from_slice(&0_u16.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(2 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&u16::try_from(text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(text);
        expected.extend_from_slice(&0_u32.to_le_bytes());
        assert_eq!(encode_failure_event(message), expected);
    }

    #[test]
    fn playback_error_packet_layout_matches_the_qt_decoder() {
        // Mirrored by BridgeClientTest::playbackErrorPacketReachesBridgeError.
        let message = "Couldn't play \u{201c}bad.flac\u{201d} (Resource not found); skipped it.";
        let text = message.as_bytes();
        let mut expected = Vec::new();
        expected.extend_from_slice(&0xFE55_0001_u32.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(18 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&(1_u16 << 6).to_le_bytes());
        expected.extend_from_slice(&0_u16.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(2 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&u16::try_from(text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(text);
        assert_eq!(encode_error_event(message), expected);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::analysis::AnalysisSnapshot;
    use crate::library::{LibraryRoot, LibrarySnapshot, LibraryTrack};
    use crate::playback::{MonotonicTime, PlaybackSnapshot, PlaybackState};
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
                muted_channels_mask: 0,
                soloed_channel: None,
                track_token: 7,
                seek_serial: 3,
                seek_landed_serial: 2,
                position_sampled_at: MonotonicTime::from_nanos(1_000_000),
                position_advancing: true,
            },
            analysis: AnalysisSnapshot {
                waveform_peaks: vec![0.1, 0.5, 0.9],
                waveform_coverage_seconds: 0.0,
                waveform_complete: true,
                sample_rate_hz: 48_000,
            },
            metadata: crate::metadata::TrackMetadata {
                source_path: Some("/music/a.flac".to_string()),
                title: "Sample Track".to_string(),
                artist: "Sample Artist".to_string(),
                album: "Sample Album".to_string(),
                album_artist: "Sample Album Artist".to_string(),
                genre: "Rock".to_string(),
                year: Some(2020),
                track_number: Some(3),
                sample_rate_hz: Some(48_000),
                bitrate_kbps: Some(320),
                channels: Some(2),
                channel_labels: vec!["L".to_owned(), "R".to_owned()],
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
                    search_tags: crate::library::SearchTags::default(),
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
            queue_details: Arc::default(),
            library_artist_count: 1,
            library_album_count: 1,
            library_expanded_keys: vec!["artist|/music|Sample Artist".to_string()],
            library_view_selection_key: "track|/music/a.flac".to_string(),
            library_view_anchor_key: "artist|/music|Sample Artist".to_string(),
            library_view_anchor_offset: 4.0,
            library_reveal_generation: 0,
            pre_built_tree_bytes: Some(Arc::new(vec![0, 0, 0, 0])),
            queue_included: true,
            queue: vec![PathBuf::from("/music/a.flac")],
            selected_queue_index: Some(0),
            settings: super::super::BridgeSettings {
                volume: 0.75,
                fft_size: 2048,
                spectrogram_view_mode: SpectrogramViewMode::Downmix,
                spectrogram_display_mode: SpectrogramDisplayMode::Rolling,
                viewer_fullscreen_mode: ViewerFullscreenMode::WholeScreen,
                db_range: 132.0,
                display: super::super::BridgeDisplaySettings {
                    log_scale: false,
                    show_fps: false,
                    show_level_meter: true,
                    prevent_display_sleep_in_fullscreen: true,
                    show_spectrogram_crosshair: false,
                    show_spectrogram_scale: false,
                    channel_buttons_visibility: 1,
                    spectrogram_zoom_enabled: true,
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

    fn read_u64(bytes: &[u8], offset: &mut usize) -> u64 {
        let start = *offset;
        let end = start + 8;
        *offset = end;
        u64::from_le_bytes(bytes[start..end].try_into().expect("u64 bytes"))
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
    fn waveform_window_protocol_appends_true_downmix_extrema() {
        let window = crate::analysis::WaveformWindow {
            sample_rate_hz: 48_000,
            channel_count: 2,
            start_seconds: 0.0,
            end_seconds: 0.1,
            frames_per_point: 2400,
            extrema: vec![-0.5, 0.5, -0.5, 0.5, -0.25, 0.75, -0.25, 0.75],
            downmix_extrema: vec![0.0, 0.0, -0.25, 0.75],
        };
        let bytes = encode_waveform_window(&window);
        assert_eq!(&bytes[..4], b"WVF2");
        assert_eq!(bytes.len(), 36 + 12 * 4);
        assert_eq!(
            u32::from_le_bytes(bytes[32..36].try_into().expect("point count")),
            2
        );
        assert_eq!(&bytes[68..72], &0.0_f32.to_le_bytes());
        assert_eq!(&bytes[80..84], &0.75_f32.to_le_bytes());
    }

    #[test]
    fn batch_removal_command_decodes_all_indices_and_rejects_truncation() {
        let mut payload = Vec::new();
        push_u16(&mut payload, 3);
        for index in [9, 5, 1] {
            push_u32(&mut payload, index);
        }
        assert!(
            matches!(parse_binary_command(&encode_command(63, &payload)).expect("batch"),
            Some(BridgeCommand::Queue(BridgeQueueCommand::RemoveMany(indices))) if indices == vec![9, 5, 1])
        );
        payload.pop();
        assert!(parse_binary_command(&encode_command(63, &payload)).is_err());
    }

    #[test]
    fn relay_preparation_does_not_lock_ui_command_or_output_queues() {
        let (command_tx, command_rx) = crossbeam_channel::unbounded();
        let shared = FfiShared::new(
            FfiRuntime::new(command_tx, -1, -1),
            Arc::new(AtomicU64::new(0)),
        );
        let mut encoder = FfiEncoder::default();
        let events = std::iter::once_with(|| {
            let mut runtime = shared
                .runtime
                .try_lock()
                .expect("UI can lock during encoding");
            assert!(runtime.send_command(parse_binary_command(&encode_command(1, &[]))));
            runtime.push_binary_event(encode_error_event("existing"));
            assert!(runtime.pop_binary_event().is_some());
            BridgeEvent::Snapshot(Box::new(sample_snapshot()))
        });
        prepare_and_publish(&shared, &mut encoder, events);
        assert!(command_rx.try_recv().is_ok());
        let mut runtime = shared.runtime.lock().expect("runtime");
        assert!(runtime.pop_binary_event().is_some());
        assert!(runtime.pop_analysis_frame().is_some());
    }

    #[test]
    fn renames_are_not_a_ui_command() {
        // Renames reach the bridge through the import gate, in order with
        // import results, never as a command the UI sends afterwards.
        let mut payload = Vec::new();
        push_u16(&mut payload, 2);
        push_u16_string(&mut payload, "/fixture/old.ac3");
        push_u16_string(&mut payload, "/fixture/new.ac3");
        assert_eq!(
            parse_binary_command(&encode_command(62, &payload)).err(),
            Some("unknown binary command id 62".to_string())
        );
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

        let cmd = parse_binary_command(&encode_command(59, &[0]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::SetPreventDisplaySleepInFullscreen(
                false
            ))
        ));

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
    fn parse_spectrogram_activity_command() {
        for active in [false, true] {
            let command = parse_binary_command(&encode_command(64, &[u8::from(active)]))
                .unwrap()
                .unwrap();
            assert!(matches!(command, BridgeCommand::Analysis(
                BridgeAnalysisCommand::SetSpectrogramActive(value)) if value == active));
        }
        assert!(parse_binary_command(&encode_command(64, &[])).is_err());
    }

    #[test]
    fn parse_set_spectrogram_crosshair_command() {
        let cmd = parse_binary_command(&encode_command(51, &[1]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::SetShowSpectrogramCrosshair(true))
        ));
    }

    #[test]
    fn parse_set_spectrogram_scale_command() {
        let cmd = parse_binary_command(&encode_command(52, &[0]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::SetShowSpectrogramScale(false))
        ));
    }

    #[test]
    fn parse_set_channel_buttons_visibility_command() {
        let cmd = parse_binary_command(&encode_command(55, &[2]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::SetChannelButtonsVisibility(2))
        ));
    }

    #[test]
    fn parse_set_channel_buttons_visibility_clamps_to_max() {
        let cmd = parse_binary_command(&encode_command(55, &[99]))
            .expect("parse")
            .expect("command");
        assert!(matches!(
            cmd,
            BridgeCommand::Settings(BridgeSettingsCommand::SetChannelButtonsVisibility(2))
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
    fn parse_binary_command_supports_library_view_state_and_reveal() {
        let selection_key = "track|/music/Artist/Album/01.flac";
        let anchor_key = "album|/music|Artist|Album";
        let mut payload = Vec::new();
        push_u16_string(&mut payload, selection_key);
        push_u16_string(&mut payload, anchor_key);
        push_f32(&mut payload, 5.5);

        let cmd = parse_binary_command(&encode_command(60, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::SetViewState {
                selection_key: parsed_selection,
                anchor_key: parsed_anchor,
                anchor_offset,
            }) => {
                assert_eq!(parsed_selection, selection_key);
                assert_eq!(parsed_anchor, anchor_key);
                assert!((anchor_offset - 5.5).abs() < f32::EPSILON);
            }
            other => panic!("unexpected command: {other:?}"),
        }

        let path = "/music/Artist/Album/01.flac";
        let mut payload = Vec::new();
        push_u16_string(&mut payload, path);
        let cmd = parse_binary_command(&encode_command(61, &payload))
            .expect("parse")
            .expect("command");
        match cmd {
            BridgeCommand::Library(BridgeLibraryCommand::ShowTrackInLibrary(parsed)) => {
                assert_eq!(parsed, PathBuf::from(path));
            }
            other => panic!("unexpected command: {other:?}"),
        }
    }

    #[test]
    fn search_protocol_carries_expansion_and_totals() {
        let mut payload = Vec::new();
        push_u32(&mut payload, 9);
        push_u16_string(&mut payload, "blue");
        push_u32(&mut payload, 80);
        assert!(matches!(
            parse_binary_command(&encode_command(36, &payload)).expect("parse"),
            Some(BridgeCommand::Library(
                BridgeLibraryCommand::SetSearchQuery {
                    seq: 9,
                    limit: 80,
                    ..
                }
            ))
        ));
        payload.pop();
        assert!(parse_binary_command(&encode_command(36, &payload)).is_err());
        let bytes = encode_search_results_frame(&BridgeSearchResultsFrame {
            seq: 9,
            totals: [7, 12, 500],
            rows: Vec::new(),
        });
        assert_eq!(&bytes[..8], &[b'S', 4, 0, 0, 9, 0, 0, 0]);
        assert_eq!(&bytes[8..12], &7u32.to_le_bytes());
        assert_eq!(&bytes[12..16], &12u32.to_le_bytes());
        assert_eq!(&bytes[16..20], &500u32.to_le_bytes());
        let row = crate::frontend_bridge::BridgeSearchResultRow {
            match_detail: "Comment: remaster".into(),
            row_type: BridgeSearchResultRowType::Track,
            score: 6.0,
            year: Some(1997),
            track_number: Some(1),
            count: 0,
            length_seconds: None,
            label: "Song".into(),
            artist: String::new(),
            album: String::new(),
            root_label: String::new(),
            genre: String::new(),
            cover_path: String::new(),
            artist_key: String::new(),
            album_key: String::new(),
            section_key: String::new(),
            track_key: String::new(),
            track_path: "/fixture/song".into(),
        };
        let bytes = encode_search_results_frame(&BridgeSearchResultsFrame {
            seq: 9,
            totals: [0, 0, 1],
            rows: vec![row],
        });
        let mut suffix = Vec::new();
        push_u16_string(&mut suffix, "Comment: remaster");
        assert!(bytes.ends_with(&suffix));
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
            BridgeCommand::Library(BridgeLibraryCommand::SetSearchQuery {
                seq,
                query: q,
                limit,
            }) => {
                assert_eq!(limit, 0);
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
        Arc::make_mut(&mut snapshot.queue_details).insert(
            PathBuf::from("/outside/song.flac"),
            crate::library::IndexedTrack {
                search_tags: crate::library::SearchTags::default(),
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

        Arc::make_mut(&mut snapshot.queue_details).insert(
            path,
            crate::library::IndexedTrack {
                search_tags: crate::library::SearchTags::default(),
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
        assert_eq!(
            read_u16_string(&encoded, &mut offset),
            "Sample Album Artist"
        );
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
        assert_eq!(read_u32(&encoded, &mut offset), 3);
        assert_eq!(&encoded[offset..offset + 2], &2u16.to_le_bytes());
        offset += 2;
        assert_eq!(read_u16_string(&encoded, &mut offset), "L");
        assert_eq!(read_u16_string(&encoded, &mut offset), "R");
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
    fn level_meter_setting_command_and_snapshot_share_protocol() {
        for enabled in [false, true] {
            let cmd = parse_binary_command(&encode_command(65, &[u8::from(enabled)]))
                .expect("parse level meter setting")
                .expect("level meter setting command");
            assert!(matches!(cmd,
                BridgeCommand::Settings(BridgeSettingsCommand::SetShowLevelMeter(value))
                if value == enabled));
            let mut snapshot = sample_snapshot();
            snapshot.settings.display.show_level_meter = enabled;
            assert_eq!(
                encode_settings_section(&snapshot).last(),
                Some(&u8::from(enabled))
            );
        }
        assert!(parse_binary_command(&encode_command(65, &[])).is_err());
        assert!(parse_binary_command(&encode_command(65, &[1, 0])).is_err());
    }

    #[test]
    fn settings_section_encodes_system_media_controls_flag() {
        let mut snapshot = sample_snapshot();
        snapshot.settings.integrations.system_media_controls_enabled = false;
        snapshot.settings.viewer_fullscreen_mode = ViewerFullscreenMode::WholeScreen;
        snapshot
            .settings
            .display
            .prevent_display_sleep_in_fullscreen = false;
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
        // Settings tail: system media controls, fullscreen mode, display mode,
        // crosshair, scale, channel buttons, zoom, display sleep, level meter.
        assert_eq!(
            &settings[settings.len() - 9..],
            &[0, 1, 0, 0, 0, 1, 1, 0, 1]
        );
    }

    #[test]
    fn settings_section_encodes_overlay_flags() {
        let mut snapshot = sample_snapshot();
        snapshot.settings.display.show_spectrogram_crosshair = true;
        snapshot.settings.display.show_spectrogram_scale = true;
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
        // Crosshair, scale, channel buttons, zoom, display sleep, level meter.
        assert_eq!(&settings[settings.len() - 6..], &[1, 1, 1, 1, 1, 1]);
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
    fn precomputed_spectrogram_frame_includes_generation() {
        let chunk = PrecomputedSpectrogramChunk {
            track_token: 7,
            generation: 42,
            columns_u8: vec![1, 2, 3, 4],
            bins_per_column: 2,
            column_count: 2,
            channel_count: 1,
            start_column_index: 9,
            total_columns_estimate: 12,
            sample_rate_hz: 48_000,
            hop_size: 5_120,
            coverage_seconds: 0.25,
            complete: false,
            buffer_reset: true,
            clear_history: true,
        };

        let frame = encode_precomputed_spectrogram_chunk(&chunk);
        let payload_len = u32::from_le_bytes(frame[0..4].try_into().expect("length prefix"));
        assert_eq!(payload_len, 43 + 4);

        let mut offset = 4usize;
        assert_eq!(frame[offset], PRECOMPUTED_SPECTROGRAM_MAGIC);
        offset += 1;
        assert_eq!(read_u64(&frame, &mut offset), 7);
        assert_eq!(read_u64(&frame, &mut offset), 42);
        assert_eq!(read_u16(&frame, &mut offset), 2);
        assert_eq!(read_u16(&frame, &mut offset), 2);
        assert_eq!(frame[offset], 1);
        offset += 1;
        assert_eq!(read_u32(&frame, &mut offset), 9);
        assert_eq!(read_u32(&frame, &mut offset), 12);
        assert_eq!(read_u32(&frame, &mut offset), 48_000);
        assert_eq!(read_u16(&frame, &mut offset), 5_120);
        assert!((read_f32(&frame, &mut offset) - 0.25).abs() < f32::EPSILON);
        assert_eq!(frame[offset], 0);
        assert_eq!(frame[offset + 1], 1);
        assert_eq!(frame[offset + 2], 1);
        offset += 3;
        assert_eq!(&frame[offset..], chunk.columns_u8.as_slice());
    }

    fn test_precomputed_chunk(
        generation: u64,
        start_column_index: u32,
    ) -> PrecomputedSpectrogramChunk {
        PrecomputedSpectrogramChunk {
            track_token: 7,
            generation,
            columns_u8: vec![u8::try_from(start_column_index % 251).expect("sample byte")],
            bins_per_column: 1,
            column_count: 1,
            channel_count: 1,
            start_column_index,
            total_columns_estimate: 512,
            sample_rate_hz: 48_000,
            hop_size: 1_024,
            coverage_seconds: f32::from(u16::try_from(start_column_index).unwrap_or(u16::MAX)),
            complete: false,
            buffer_reset: start_column_index == 0,
            clear_history: start_column_index == 0,
        }
    }

    fn decode_precomputed_generation_and_start(frame: &[u8]) -> (u64, u32) {
        let mut offset = 4usize;
        assert_eq!(frame[offset], PRECOMPUTED_SPECTROGRAM_MAGIC);
        offset += 1;
        let _track_token = read_u64(frame, &mut offset);
        let generation = read_u64(frame, &mut offset);
        let _bins_per_column = read_u16(frame, &mut offset);
        let _column_count = read_u16(frame, &mut offset);
        offset += 1; // channel_count
        let start_column_index = read_u32(frame, &mut offset);
        (generation, start_column_index)
    }

    #[cfg(unix)]
    fn spectral_shared() -> Arc<FfiShared> {
        let (commands, _) = crossbeam_channel::unbounded();
        Arc::new(FfiShared::new(
            FfiRuntime::new(commands, -1, -1),
            Arc::new(AtomicU64::new(1)),
        ))
    }

    fn spectral_fixture(generation: u64, start: u32) -> AnalysisEvent {
        AnalysisEvent::PrecomputedSpectrogramChunk(PrecomputedSpectrogramChunk {
            track_token: 1,
            generation,
            columns_u8: vec![127; 64 * 1_025 * 2],
            bins_per_column: 1_025,
            column_count: 64,
            channel_count: 2,
            start_column_index: start,
            total_columns_estimate: 20_000,
            sample_rate_hz: 48_000,
            hop_size: 1_024,
            coverage_seconds: 1.0,
            complete: false,
            buffer_reset: start == 0,
            clear_history: start == 0,
        })
    }

    #[test]
    fn spectral_relay_preserves_all_active_columns_under_byte_backpressure() {
        let shared = spectral_shared();
        let (tx, rx) = crossbeam_channel::bounded(4);
        let relay_shared = Arc::clone(&shared);
        let relay = thread::spawn(move || run_spectral_relay(&relay_shared, &rx));
        let (done_tx, done_rx) = crossbeam_channel::bounded(1);
        let producer = thread::spawn(move || {
            for index in 0..200 {
                tx.send(spectral_fixture(1, index * 64)).unwrap();
            }
            done_tx.send(()).unwrap();
        });
        let frame_size = 47 + 64 * 1_025 * 2;
        let deadline = Instant::now() + Duration::from_secs(2);
        while Instant::now() < deadline {
            let bytes = shared.runtime.lock().unwrap().pending_spectrogram_bytes;
            assert!(bytes <= MAX_PENDING_SPECTROGRAM_BYTES);
            if bytes + frame_size > MAX_PENDING_SPECTROGRAM_BYTES {
                break;
            }
            thread::sleep(Duration::from_millis(2));
        }
        assert!(
            done_rx.try_recv().is_err(),
            "producer must wait for the consumer"
        );
        for index in 0..200 {
            let deadline = Instant::now() + Duration::from_secs(2);
            let frame = loop {
                let mut runtime = shared.runtime.lock().unwrap();
                assert!(runtime.pending_spectrogram_bytes <= MAX_PENDING_SPECTROGRAM_BYTES);
                if let Some(frame) = runtime.pop_precomputed_spectrogram() {
                    break frame;
                }
                drop(runtime);
                assert!(Instant::now() < deadline);
                thread::sleep(Duration::from_millis(1));
            };
            shared.spectral_space.notify_one();
            assert_eq!(
                decode_precomputed_generation_and_start(&frame),
                (1, index * 64)
            );
        }
        producer.join().unwrap();
        relay.join().unwrap();
        assert_eq!(shared.runtime.lock().unwrap().pending_spectrogram_bytes, 0);
    }

    #[test]
    fn stalled_spectral_relay_allows_control_and_new_generation_without_a_consumer() {
        let shared = spectral_shared();
        shared
            .runtime
            .lock()
            .unwrap()
            .push_precomputed_frame(1, vec![0; MAX_PENDING_SPECTROGRAM_BYTES]);
        let (tx, rx) = crossbeam_channel::bounded(4);
        tx.send(spectral_fixture(1, 0)).unwrap();
        let relay_shared = Arc::clone(&shared);
        let relay = thread::spawn(move || run_spectral_relay(&relay_shared, &rx));
        let mut encoder = FfiEncoder::default();
        prepare_and_publish(
            &shared,
            &mut encoder,
            [BridgeEvent::Error("control remains responsive".into())],
        );
        assert!(shared.runtime.lock().unwrap().pop_binary_event().is_some());
        shared.spectral_generation.store(2, Ordering::Relaxed);
        shared.spectral_space.notify_all();
        tx.send(spectral_fixture(2, 64)).unwrap();
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            let mut runtime = shared.runtime.lock().unwrap();
            if runtime.latest_precomputed_generation == 2
                && !runtime.pending_precomputed_spectrogram.is_empty()
            {
                let frame = runtime.pop_precomputed_spectrogram().unwrap();
                assert_eq!(decode_precomputed_generation_and_start(&frame), (2, 64));
                assert_eq!(runtime.pending_spectrogram_bytes, 0);
                break;
            }
            drop(runtime);
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        shared.request_stop();
        relay.join().unwrap();
    }

    #[test]
    fn shutdown_releases_a_full_spectral_relay() {
        let shared = spectral_shared();
        shared
            .runtime
            .lock()
            .unwrap()
            .push_precomputed_frame(1, vec![0; MAX_PENDING_SPECTROGRAM_BYTES]);
        let (tx, rx) = crossbeam_channel::bounded(1);
        tx.send(spectral_fixture(1, 0)).unwrap();
        let relay_shared = Arc::clone(&shared);
        let relay = thread::spawn(move || run_spectral_relay(&relay_shared, &rx));
        shared.request_stop();
        relay.join().unwrap();
    }

    #[test]
    fn precomputed_queue_preserves_active_generation_prefix_when_full() {
        let mut runtime = test_runtime_with_wake_pipe();
        let stale_frames = MAX_PENDING_PRECOMPUTED_SPECTROGRAM;
        for start in 0..stale_frames {
            runtime.process_bridge_events(vec![BridgeEvent::PrecomputedSpectrogramChunk(
                test_precomputed_chunk(1, u32::try_from(start).expect("stale start")),
            )]);
        }

        const ACTIVE_EXTRA_FRAMES: usize = 6;
        let active_frames = MAX_PENDING_PRECOMPUTED_SPECTROGRAM + ACTIVE_EXTRA_FRAMES;
        for start in 0..active_frames {
            runtime.process_bridge_events(vec![BridgeEvent::PrecomputedSpectrogramChunk(
                test_precomputed_chunk(2, u32::try_from(start).expect("active start")),
            )]);
        }

        let mut active_starts = Vec::new();
        while let Some(frame) = runtime.pop_precomputed_spectrogram() {
            let (generation, start) = decode_precomputed_generation_and_start(&frame);
            if generation == 2 {
                active_starts.push(start);
            }
        }

        assert_eq!(
            active_starts.len(),
            active_frames,
            "the active spectrogram generation must be lossless"
        );
        assert_eq!(active_starts.first().copied(), Some(0));
        for (expected, actual) in active_starts.iter().copied().enumerate() {
            assert_eq!(actual, u32::try_from(expected).expect("expected start"));
        }

        runtime.close_wakeup_pipe();
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
        let frame = encode_analysis_frame(&delta);
        assert!(!frame.is_empty());
        assert_eq!(frame[4], ANALYSIS_FRAME_MAGIC);
    }

    fn ffi_send_binary(handle: *mut FerrousFfiBridge, cmd: &[u8]) -> bool {
        // SAFETY: The tests pass a live bridge, and `cmd` is readable during the call.
        unsafe { ferrous_ffi_bridge_send_binary(handle, cmd.as_ptr(), cmd.len()) }
    }

    fn ffi_next_event(handle: *mut FerrousFfiBridge, timeout: Duration) -> Option<Vec<u8>> {
        let deadline = Instant::now() + timeout;
        while Instant::now() < deadline {
            // SAFETY: The tests pass a live bridge; `len` is writable, and a
            // returned buffer of `len` bytes is copied, then freed once.
            unsafe {
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
            // SAFETY: The tests pass a live bridge; `len` and `version` are
            // writable, and a returned buffer of `len` bytes is copied, then
            // freed once.
            unsafe {
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
            // SAFETY: `poll_fd` is one valid, writable `pollfd`.
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
            // SAFETY: `buffer` is writable for its length.
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
        // SAFETY: `handle` came from `ferrous_ffi_bridge_create` and is not used again.
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
        // SAFETY: `handle` came from `ferrous_ffi_bridge_create` and is not used again.
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }

    /// The reason in a packet that reports a stop the UI did not ask for.
    fn failure_message(packet: &[u8]) -> Option<String> {
        let (_, _, mask) = parse_packet_header(packet);
        if mask != SECTION_ERROR | SECTION_STOPPED {
            return None;
        }
        let mut offset = 12;
        let _section_len = read_u32(packet, &mut offset);
        Some(read_u16_string(packet, &mut offset))
    }

    fn relay_shared() -> Arc<FfiShared> {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        Arc::new(FfiShared::new(
            FfiRuntime::new(command_tx, -1, -1),
            Arc::new(AtomicU64::new(0)),
        ))
    }

    #[test]
    fn a_command_the_ended_bridge_cannot_take_fails_and_tells_the_ui_once() {
        let (command_tx, command_rx) = crossbeam_channel::unbounded();
        drop(command_rx);
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        let snapshot = encode_command(33, &[]);

        assert!(!runtime.send_command(parse_binary_command(&snapshot)));
        let packet = runtime.pop_binary_event().expect("failure event");
        assert_eq!(
            failure_message(&packet).as_deref(),
            Some(BACKEND_STOPPED_MESSAGE)
        );
        assert!(!runtime.send_command(parse_binary_command(&snapshot)));
        assert!(runtime.pop_binary_event().is_none());
    }

    #[test]
    fn the_relay_reports_a_bridge_that_ends_without_stopping() {
        let shared = relay_shared();
        let (event_tx, event_rx) = crossbeam_channel::bounded(4);
        event_tx
            .send(BridgeEvent::Error("sent before".into()))
            .unwrap();
        drop(event_tx);

        run_ffi_relay_loop(Arc::clone(&shared), event_rx);

        let mut runtime = shared.lock_runtime();
        let first = runtime.pop_binary_event().expect("the earlier error");
        assert_eq!(parse_packet_header(&first).2, SECTION_ERROR);
        let failure = runtime.pop_binary_event().expect("the failure");
        assert_eq!(
            failure_message(&failure).as_deref(),
            Some(BACKEND_STOPPED_MESSAGE)
        );
        assert!(runtime.pop_binary_event().is_none());
    }

    #[test]
    fn the_relay_does_not_report_a_bridge_that_stopped_normally() {
        let shared = relay_shared();
        let (event_tx, event_rx) = crossbeam_channel::bounded(4);
        event_tx.send(BridgeEvent::Stopped).unwrap();
        drop(event_tx);

        run_ffi_relay_loop(Arc::clone(&shared), event_rx);

        let mut runtime = shared.lock_runtime();
        let stopped = runtime.pop_binary_event().expect("the stop");
        assert_eq!(parse_packet_header(&stopped).2, SECTION_STOPPED);
        assert!(runtime.pop_binary_event().is_none());
    }

    #[test]
    fn a_bridge_failure_reaches_the_ui_as_a_stop_with_its_reason() {
        let shared = relay_shared();
        let (event_tx, event_rx) = crossbeam_channel::bounded(4);
        event_tx.send(BridgeEvent::Failed("why".into())).unwrap();

        run_ffi_relay_loop(Arc::clone(&shared), event_rx);

        assert!(shared.stop_requested.load(Ordering::Relaxed));
        let mut runtime = shared.lock_runtime();
        let failure = runtime.pop_binary_event().expect("the failure");
        assert_eq!(failure_message(&failure).as_deref(), Some("why"));
        assert!(runtime.pop_binary_event().is_none());
        drop(event_tx);
    }

    #[test]
    fn a_panicking_ffi_call_returns_its_error_value_and_the_next_call_works() {
        let handle = ferrous_ffi_bridge_create();
        assert!(!handle.is_null());
        let snapshot = encode_command(33, &[]);

        test_hooks::panic_at("ferrous_ffi_bridge_send_binary");
        assert!(!ffi_send_binary(handle, &snapshot));
        assert!(ffi_send_binary(handle, &snapshot));

        // A panic while the runtime is locked poisons its mutex, and the
        // next call repairs it.
        test_hooks::panic_at("send_binary:locked");
        assert!(!ffi_send_binary(handle, &snapshot));
        assert!(ffi_send_binary(handle, &snapshot));
        // SAFETY: `handle` is live until it is destroyed below.
        let bridge = unsafe { &*handle };
        assert!(!bridge.shared.runtime.is_poisoned());
        assert!(ffi_wait_for_mask(handle, SECTION_PLAYBACK, Duration::from_secs(4)).is_some());

        test_hooks::panic_at("ferrous_ffi_bridge_pop_binary_event");
        let mut len = 7_usize;
        // SAFETY: `handle` is live and `len` is writable.
        let ptr = unsafe { ferrous_ffi_bridge_pop_binary_event(handle, &raw mut len) };
        assert!(ptr.is_null());
        assert_eq!(len, 0);

        assert!(ffi_send_binary(handle, &encode_command(34, &[])));
        let _ = ffi_wait_for_mask(handle, SECTION_STOPPED, Duration::from_secs(3));
        // SAFETY: `handle` came from `ferrous_ffi_bridge_create` and is not used again.
        unsafe { ferrous_ffi_bridge_destroy(handle) };
    }

    fn ffi_tag_editor_load(request: &[u8]) -> Option<serde_json::Value> {
        let mut len = 0_usize;
        // SAFETY: `request` is readable and `len` writable during the call.
        let ptr =
            unsafe { ferrous_ffi_tag_editor_load(request.as_ptr(), request.len(), &raw mut len) };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: The call handed out `len` bytes at `ptr`, which are freed
        // once below.
        let bytes = unsafe { std::slice::from_raw_parts(ptr, len) }.to_vec();
        // SAFETY: `ptr` and `len` came from the load and are not used again.
        unsafe { ferrous_ffi_tag_editor_free_buffer(ptr, len) };
        Some(serde_json::from_slice(&bytes).expect("JSON response"))
    }

    #[test]
    fn a_file_that_panics_the_tag_reader_fails_alone_in_the_tag_editor() {
        use crate::vorbis_comment_scan::fixtures::{comment_block, flac};

        let dir = std::env::temp_dir().join(format!("ferrous-ffi-coverart-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("create test dir");
        let broken = dir.join("broken.flac");
        let good = dir.join("good.flac");
        std::fs::write(&broken, panic_guard::fixtures::short_coverart_flac()).expect("write");
        std::fs::write(&good, flac(&[&comment_block(&[b"TITLE=Fine"])])).expect("write");
        let request = serde_json::to_vec(&[&broken, &good]).expect("request");

        let response = ffi_tag_editor_load(&request).expect("a response");

        let rows = response["rows"].as_array().expect("rows");
        assert_eq!(rows.len(), 1, "{response}");
        assert_eq!(rows[0]["title"], "Fine");
        let failures = response["failures"].as_array().expect("failures");
        assert_eq!(failures.len(), 1, "{response}");
        assert_eq!(failures[0]["path"], broken.to_str().expect("UTF-8 path"));
        let error = failures[0]["error"].as_str().expect("error");
        assert!(error.contains("malformed tag data"), "{error}");
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn a_panic_in_a_tag_editor_request_answers_with_an_error() {
        test_hooks::panic_at("tag_editor_request");
        let response = ffi_tag_editor_load(b"[]").expect("an error response");
        let error = response["error"].as_str().expect("error");
        assert!(
            error.contains("test panic at tag_editor_request"),
            "{error}"
        );

        test_hooks::panic_at("ferrous_ffi_tag_editor_load");
        assert!(ffi_tag_editor_load(b"[]").is_none());

        let response = ffi_tag_editor_load(b"[]").expect("a response");
        assert!(response["error"].is_null(), "{response}");
    }

    struct DecodedSnapshotForTest {
        playback_muted_channels_mask: u64,
        playback_soloed_channel: Option<u8>,
        playback_track_token: u64,
        playback_seek_serial: u64,
        playback_seek_landed_serial: u64,
        playback_position: f64,
        playback_position_sampled_at_ns: u64,
        playback_position_advancing: bool,
        settings_channel_buttons_visibility: u8,
    }

    fn decode_binary_snapshot_for_test(data: &[u8]) -> DecodedSnapshotForTest {
        let mut result = DecodedSnapshotForTest {
            playback_muted_channels_mask: 0,
            playback_soloed_channel: None,
            playback_track_token: 0,
            playback_seek_serial: 0,
            playback_seek_landed_serial: 0,
            playback_position: 0.0,
            playback_position_sampled_at_ns: 0,
            playback_position_advancing: false,
            settings_channel_buttons_visibility: 1,
        };
        // Skip 12-byte header (magic 4 + total_len 4 + section_mask 2 + reserved 2).
        let section_mask = u16::from_le_bytes([data[8], data[9]]);
        let mut offset = 12;
        if section_mask & SECTION_PLAYBACK != 0 {
            // Playback section present — parse field-by-field in encoding order.
            let section_len =
                u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap()) as usize;
            offset += 4;
            let s = &data[offset..offset + section_len];
            let mut p = 0;
            p += 1; // u8   state
            result.playback_position = f64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8; // f64  position
            p += 8; // f64  duration
            p += 4; // f32  volume
            p += 1; // u8   repeat_mode
            p += 1; // u8   shuffle
            p += 4; // i32  queue_index
            let path_len = u16::from_le_bytes(s[p..p + 2].try_into().unwrap()) as usize;
            p += 2 + path_len; // skip path bytes
            result.playback_muted_channels_mask =
                u64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8;
            let soloed_raw = s[p];
            result.playback_soloed_channel = if soloed_raw == 0xFF {
                None
            } else {
                Some(soloed_raw)
            };
            p += 1;
            result.playback_track_token = u64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8;
            result.playback_seek_serial = u64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8;
            result.playback_seek_landed_serial =
                u64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8;
            result.playback_position_sampled_at_ns =
                u64::from_le_bytes(s[p..p + 8].try_into().unwrap());
            p += 8;
            result.playback_position_advancing = match s[p] {
                0 => false,
                1 => true,
                other => panic!("advancing flag {other}"),
            };
            assert_eq!(
                p + 1,
                section_len,
                "the position sample ends the playback section"
            );
            offset += section_len;
        }
        // Skip remaining sections before settings.  Sections appear in
        // encoding order: playback (already consumed), [queue if present],
        // library_meta, metadata — then settings.
        let sections_before_settings: &[u16] =
            &[SECTION_QUEUE, SECTION_LIBRARY_META, SECTION_METADATA];
        for &bit in sections_before_settings {
            if section_mask & bit != 0 {
                let section_len =
                    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap()) as usize;
                offset += 4 + section_len;
            }
        }
        // Settings section (SECTION_SETTINGS).
        if section_mask & SECTION_SETTINGS != 0 {
            let section_len =
                u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap()) as usize;
            offset += 4;
            let s = &data[offset..offset + section_len];
            // Settings encoding order: volume(4) + fft_size(4) + view_mode(1)
            // + db_range(4) + log_scale(1) + show_fps(1) + library_sort_mode(4)
            // + system_media_controls(1) + viewer_fullscreen(1)
            // + display_mode(1) + crosshair(1) + scale(1)
            // + channel_buttons_visibility(1) + spectrogram_zoom_enabled(1)
            // + prevent_display_sleep_in_fullscreen(1)
            let p = 4 + 4 + 1 + 4 + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1;
            result.settings_channel_buttons_visibility = s[p];
        }
        result
    }

    #[test]
    fn snapshot_encodes_muted_channels_mask() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.muted_channels_mask = 0b101; // channels 0 and 2 muted
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.playback_muted_channels_mask, 0b101);
    }

    #[test]
    fn snapshot_encodes_soloed_channel() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.soloed_channel = Some(3);
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.playback_soloed_channel, Some(3));
    }

    #[test]
    fn snapshot_encodes_soloed_channel_none() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.soloed_channel = None;
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.playback_soloed_channel, None);
    }

    #[test]
    fn snapshot_encodes_track_token() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.track_token = u64::MAX - 1;
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.playback_track_token, u64::MAX - 1);
        assert_eq!(decoded.playback_soloed_channel, None);
    }

    #[test]
    fn snapshot_encodes_seek_serials() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.seek_serial = u64::MAX - 2;
        snapshot.playback.seek_landed_serial = u64::MAX - 3;
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.playback_track_token, 7);
        assert_eq!(decoded.playback_seek_serial, u64::MAX - 2);
        assert_eq!(decoded.playback_seek_landed_serial, u64::MAX - 3);
    }

    #[test]
    fn snapshot_encodes_the_raw_position_with_its_sample_time() {
        let mut snapshot = sample_snapshot();
        snapshot.playback.position = Duration::from_micros(12_345_678);
        snapshot.playback.position_sampled_at = MonotonicTime::from_nanos(u64::MAX - 4);
        for advancing in [false, true] {
            snapshot.playback.position_advancing = advancing;
            let encoded = encode_binary_snapshot(&snapshot, None);
            let decoded = decode_binary_snapshot_for_test(&encoded);
            assert!((decoded.playback_position - 12.345_678).abs() < 1e-9);
            assert_eq!(decoded.playback_position_sampled_at_ns, u64::MAX - 4);
            assert_eq!(decoded.playback_position_advancing, advancing);
            assert_eq!(decoded.playback_seek_landed_serial, 2);
        }
    }

    #[test]
    fn snapshot_encodes_channel_buttons_visibility() {
        let mut snapshot = sample_snapshot();
        snapshot.settings.display.channel_buttons_visibility = 2;
        let encoded = encode_binary_snapshot(&snapshot, None);
        let decoded = decode_binary_snapshot_for_test(&encoded);
        assert_eq!(decoded.settings_channel_buttons_visibility, 2);
    }
}
