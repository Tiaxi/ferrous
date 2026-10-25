// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::VecDeque;
use std::ffi::c_uchar;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[cfg(unix)]
use std::os::fd::RawFd;

use super::imports::{ImportGate, RenamedDetails};
use super::queue_relay::{QueueBatch, QueueRelay};
use super::{
    BridgeAnalysisCommand, BridgeCommand, BridgeEvent, BridgeLibraryCommand, BridgePlaybackCommand,
    BridgeQueueCommand, BridgeSearchResultRowType, BridgeSearchResultsFrame, BridgeSettingsCommand,
    BridgeSnapshot, BridgeThread, FrontendBridgeHandle, LibrarySortMode, ViewerFullscreenMode,
    BRIDGE_SHUTDOWN_TIMEOUT,
};
use crate::analysis::{
    AnalysisEvent, PrecomputedSpectrogramChunk, SpectrogramDisplayMode, SpectrogramViewMode,
};
use crate::library::{track_file_fingerprint, TrackRefresh};
use crate::panic_guard;
#[cfg(test)]
use crate::panic_guard::test_hooks;
use crate::playback::{PathRenames, PlaybackState, RepeatMode};
use crate::tag_editor;

const ANALYSIS_FRAME_MAGIC: u8 = 0xA1;
const PRECOMPUTED_SPECTROGRAM_MAGIC: u8 = 0xA2;
const ANALYSIS_FLAG_WAVEFORM: u8 = 0x01;
const ANALYSIS_FLAG_WAVEFORM_COMPLETE: u8 = 0x08;
/// Errors waiting for the UI past this many give way, oldest first, to one
/// entry that counts them.
const MAX_PENDING_ERRORS: usize = 32;
const MAX_PENDING_ANALYSIS_FRAMES: usize = 24;
const MAX_PENDING_PRECOMPUTED_SPECTROGRAM: usize = 64;
const MAX_PENDING_LIBRARY_TREES: usize = 1;
const MAX_PENDING_SEARCH_RESULTS: usize = 2;
const MAX_PENDING_SPECTROGRAM_BYTES: usize = 16 * 1024 * 1024;
const RELAY_RECV_TIMEOUT: Duration = Duration::from_millis(250);
/// What the UI shows when the bridge thread ends without stopping normally.
const BACKEND_STOPPED_MESSAGE: &str =
    "The Ferrous backend stopped unexpectedly. Restart Ferrous to continue.";

/// A command is a u16 id, a u32 payload length, and the payload. Strings in
/// it, and in every frame sent to the UI, are a u32 byte length and UTF-8;
/// lists of strings or indices start with a u32 count. The payload bound
/// matches `kMaxCommandPayloadBytes` in `ui/src/BinaryBridgeCodec.h`.
const COMMAND_HEADER_BYTES: usize = 6;
const MAX_COMMAND_PAYLOAD_BYTES: usize = 8 * 1024 * 1024;

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
/// The answer to a save request: a u8, 1 if it succeeded, and a string with
/// what to show the user otherwise.
const SECTION_STATE_SAVED: u16 = 1 << 9;

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
    /// Whether the waveform or its coverage changed since the last frame.
    changed: bool,
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

/// The sections of a snapshot packet, in the order of their bits, which is
/// the order the UI reads them in.
type SnapshotSections = Vec<(u16, Vec<u8>)>;

/// An event waiting for the UI in the binary event queue. Each holds the
/// finished packet, so the UI's pop only hands it over.
enum PendingBinaryEvent {
    /// A packet the UI must receive, such as a save's answer or the
    /// bridge's stop.
    Packet(Vec<u8>),
    /// An error. Past [`MAX_PENDING_ERRORS`], the oldest give way to
    /// [`PendingBinaryEvent::EarlierErrors`].
    Error(Vec<u8>),
    /// How many errors gave way to newer ones, and the packet that says so.
    EarlierErrors { count: usize, packet: Vec<u8> },
    /// The newest snapshot. At most one waits at a time: the relay takes it
    /// back and merges it into the next one.
    Snapshot(Vec<u8>),
}

struct FfiRuntime {
    command_tx: crossbeam_channel::Sender<BridgeCommand>,
    #[cfg(test)]
    encoder: FfiEncoder,
    pending_binary_events: VecDeque<PendingBinaryEvent>,
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
            pending_binary_events: VecDeque::new(),
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

    fn push_pending_binary_event(&mut self, event: PendingBinaryEvent) {
        let was_empty = !self.has_pending_queues();
        self.pending_binary_events.push_back(event);
        if was_empty {
            self.signal_wakeup_if_needed();
        }
    }

    /// Queue a packet the UI must receive. Nothing drops it: these are the
    /// answers to save requests and the bridge's stop, which are few.
    fn push_binary_event(&mut self, payload: Vec<u8>) {
        if payload.is_empty() {
            return;
        }
        self.push_pending_binary_event(PendingBinaryEvent::Packet(payload));
    }

    /// Queue an error. If the UI has not taken the last
    /// [`MAX_PENDING_ERRORS`], the oldest gives way to the count of earlier
    /// errors, so the UI still hears that they happened.
    fn push_error(&mut self, packet: Vec<u8>) {
        let errors = self
            .pending_binary_events
            .iter()
            .filter(|event| matches!(event, PendingBinaryEvent::Error(_)))
            .count();
        if errors >= MAX_PENDING_ERRORS {
            self.collapse_oldest_error();
        }
        self.push_pending_binary_event(PendingBinaryEvent::Error(packet));
    }

    fn collapse_oldest_error(&mut self) {
        let Some(oldest) = self
            .pending_binary_events
            .iter()
            .position(|event| matches!(event, PendingBinaryEvent::Error(_)))
        else {
            return;
        };
        // The count takes the place of the first error it stands for, so it
        // comes before the errors that are still shown.
        let earlier = self.pending_binary_events.iter_mut().find_map(|event| {
            if let PendingBinaryEvent::EarlierErrors { count, packet } = event {
                Some((count, packet))
            } else {
                None
            }
        });
        if let Some((count, packet)) = earlier {
            *count += 1;
            *packet = encode_earlier_errors_event(*count);
            self.pending_binary_events.remove(oldest);
        } else {
            self.pending_binary_events[oldest] = PendingBinaryEvent::EarlierErrors {
                count: 1,
                packet: encode_earlier_errors_event(1),
            };
        }
    }

    /// Take back the snapshot the UI has not taken yet, for the relay to
    /// merge into a newer one.
    fn take_pending_snapshot(&mut self) -> Option<Vec<u8>> {
        let index = self
            .pending_binary_events
            .iter()
            .position(|event| matches!(event, PendingBinaryEvent::Snapshot(_)))?;
        match self.pending_binary_events.remove(index)? {
            PendingBinaryEvent::Snapshot(packet) => Some(packet),
            _ => None,
        }
    }

    /// Queue a snapshot the relay has merged with the one it took back.
    fn push_snapshot(&mut self, packet: Vec<u8>) {
        self.push_pending_binary_event(PendingBinaryEvent::Snapshot(packet));
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
                self.push_error(encode_error_event(&message));
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
        let snapshot = prepared.snapshot.map(|sections| {
            let older = self.take_pending_snapshot();
            self.encoder.merge_with_pending(sections, older.as_deref())
        });
        self.publish(prepared.events, snapshot);
    }

    /// Queue the relay's finished buffers, the snapshot last.
    fn publish(&mut self, events: Vec<PreparedEvent>, snapshot: Option<Vec<u8>>) {
        for event in events {
            match event {
                PreparedEvent::Binary(bytes) => self.push_binary_event(bytes),
                PreparedEvent::Error(bytes) => self.push_error(bytes),
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
        if let Some(packet) = snapshot {
            self.push_snapshot(packet);
        }
    }

    fn pop_binary_event(&mut self) -> Option<Vec<u8>> {
        Some(match self.pending_binary_events.pop_front()? {
            PendingBinaryEvent::Packet(packet)
            | PendingBinaryEvent::Error(packet)
            | PendingBinaryEvent::EarlierErrors { packet, .. }
            | PendingBinaryEvent::Snapshot(packet) => packet,
        })
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

/// What the relay prepared from a batch of bridge events.
struct Prepared {
    events: Vec<PreparedEvent>,
    /// The batch's snapshot, which still has to be merged with the one
    /// waiting for the UI.
    snapshot: Option<SnapshotSections>,
}

enum PreparedEvent {
    Binary(Vec<u8>),
    Error(Vec<u8>),
    Analysis(Vec<u8>),
    Tree(Vec<u8>),
    Search(u32, Vec<u8>),
    Precomputed(u64, Vec<u8>),
    Stopped,
}

#[derive(Default)]
struct FfiEncoder {
    analysis_state: AnalysisEmitState,
    queue: QueueRelay,
    stopped: bool,
}

impl FfiEncoder {
    fn prepare<I>(&mut self, events: I) -> Prepared
    where
        I: IntoIterator<Item = BridgeEvent>,
    {
        let mut prepared = Vec::new();
        if self.stopped {
            return Prepared {
                events: prepared,
                snapshot: None,
            };
        }

        let mut latest_snapshot: Option<BridgeSnapshot> = None;
        // Every snapshot's queue update, in order: none may be lost.
        let mut queue_batch: Option<QueueBatch> = None;
        // The latest tree, with the expanded keys sent with it.
        let mut latest_tree: Option<(std::sync::Arc<Vec<u8>>, Vec<String>)> = None;
        for event in events {
            match event {
                BridgeEvent::Snapshot(snapshot) => {
                    if let Some(tree_bytes) = snapshot.pre_built_tree_bytes.as_ref() {
                        latest_tree =
                            Some((tree_bytes.clone(), snapshot.library_expanded_keys.clone()));
                    }
                    self.queue.apply(&snapshot, &mut queue_batch);
                    latest_snapshot = Some(*snapshot);
                }
                BridgeEvent::PrecomputedSpectrogramChunk(chunk) => {
                    prepared.push(PreparedEvent::Precomputed(
                        chunk.generation,
                        encode_precomputed_spectrogram_chunk(&chunk),
                    ));
                }
                BridgeEvent::Error(message) => {
                    prepared.push(PreparedEvent::Error(encode_error_event(&message)));
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
                BridgeEvent::StateSaved { request_id, result } => {
                    prepared.push(PreparedEvent::Binary(encode_state_saved_event(
                        request_id, &result,
                    )));
                }
            }
        }

        if let Some((tree_bytes, _)) = latest_tree.as_ref() {
            prepared.push(PreparedEvent::Tree(tree_bytes.as_ref().clone()));
        }
        let mut sections = None;
        if let Some(mut snapshot) = latest_snapshot {
            if let Some((tree_bytes, expanded_keys)) = latest_tree {
                snapshot.pre_built_tree_bytes = Some(tree_bytes);
                snapshot.library_expanded_keys = expanded_keys;
            }
            let analysis_delta = compute_analysis_delta(&snapshot, &mut self.analysis_state);
            prepared.push(PreparedEvent::Analysis(encode_analysis_frame(
                &analysis_delta,
            )));
            let queue_section = queue_batch.map(|batch| self.queue.section(batch, &snapshot));
            sections = Some(snapshot_sections(&snapshot, queue_section));
        }
        Prepared {
            events: prepared,
            snapshot: sections,
        }
    }

    /// The packet for a snapshot that replaces `older`, which the UI has
    /// not taken; see [`merge_snapshot`]. A queue delta in `newer` builds on
    /// the queue `older` leaves, so the two become one.
    fn merge_with_pending(&self, mut newer: SnapshotSections, older: Option<&[u8]>) -> Vec<u8> {
        if let Some(older) = older {
            let older_queue = split_packet(older)
                .into_iter()
                .find(|(bit, _)| *bit == SECTION_QUEUE);
            let newer_queue = newer.iter_mut().find(|(bit, _)| *bit == SECTION_QUEUE);
            if let (Some((_, older_queue)), Some((_, newer_queue))) = (older_queue, newer_queue) {
                *newer_queue = self.queue.merge(newer_queue, older_queue);
            }
        }
        merge_snapshot(&newer, older)
    }
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
            .push_error(encode_error_event(&format!(
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
    // Merging copies the sections of the snapshot the UI has not taken,
    // which may hold a large queue, so it happens without the lock. Taken
    // back, that snapshot cannot also reach the UI.
    let snapshot = prepared.snapshot.map(|sections| {
        let older = shared.lock_runtime().take_pending_snapshot();
        encoder.merge_with_pending(sections, older.as_deref())
    });
    shared.lock_runtime().publish(prepared.events, snapshot);
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
    bridge_thread: Option<BridgeThread>,
    /// How long [`Self::shutdown`] waits for the bridge thread.
    shutdown_timeout: Duration,
    relay_thread: Option<JoinHandle<()>>,
    spectral_relay_thread: Option<JoinHandle<()>>,
}

impl FerrousFfiBridge {
    /// Stop the bridge, waiting up to `shutdown_timeout` for it to save
    /// the settings, the session, and Last.fm's queue, and then stop the
    /// relays. Returns false if one of them was not saved, or if the bridge
    /// thread was still running at the bound; it is left running then, so
    /// the app's exit is not held up.
    fn shutdown(&mut self) -> bool {
        // The relays keep draining the bridge's output meanwhile, so it
        // never waits on a full channel.
        let _ = self
            .shared
            .lock_runtime()
            .command_tx
            .send(BridgeCommand::Shutdown);
        let finished = self
            .bridge_thread
            .take()
            .is_none_or(|thread| thread.join_by(Instant::now() + self.shutdown_timeout));
        self.shared.request_stop();
        for thread in [self.relay_thread.take(), self.spectral_relay_thread.take()]
            .into_iter()
            .flatten()
        {
            let _ = thread.join();
        }
        self.shared.lock_runtime().close_wakeup_pipe();
        finished
    }
}

impl Drop for FerrousFfiBridge {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Start the bridge thread and the threads that relay its output to the UI.
fn create_bridge() -> Result<FerrousFfiBridge, String> {
    connect_bridge(FrontendBridgeHandle::spawn()?)
}

/// Start the threads that relay the output of the bridge behind `handle`
/// to the UI.
fn connect_bridge(handle: FrontendBridgeHandle) -> Result<FerrousFfiBridge, String> {
    let import_gate = handle.import_gate();
    let parts = handle.into_parts();
    let (event_rx, spectral_rx) = (parts.events, parts.spectra);
    let (wake_read_fd, wake_write_fd) = create_nonblocking_pipe().unwrap_or((-1, -1));
    let shared = Arc::new(FfiShared::new(
        FfiRuntime::new(parts.commands, wake_read_fd, wake_write_fd),
        parts.spectral_generation,
    ));
    let _ = shared
        .lock_runtime()
        .command_tx
        .send(BridgeCommand::RequestSnapshot);

    // Dropping `bridge` on an error below stops everything started so far.
    let mut bridge = FerrousFfiBridge {
        shared: Arc::clone(&shared),
        import_gate,
        bridge_thread: Some(parts.thread),
        shutdown_timeout: BRIDGE_SHUTDOWN_TIMEOUT,
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
/// Stop the bridge and free it. Blocks until the bridge has saved the
/// settings, the session, and Last.fm's queue, or for at most
/// [`BRIDGE_SHUTDOWN_TIMEOUT`]. Returns false if one of them was not saved,
/// or if the bridge was still shutting down then, so they may be missing.
///
/// # Safety
///
/// `handle` must be null or a pointer previously returned by
/// [`ferrous_ffi_bridge_create`], and must not be used again after this call.
pub unsafe extern "C" fn ferrous_ffi_bridge_destroy(handle: *mut FerrousFfiBridge) -> bool {
    ffi_call(
        "ferrous_ffi_bridge_destroy",
        |_| false,
        || {
            if handle.is_null() {
                return true;
            }
            // SAFETY: The caller hands over the pointer `Box::into_raw`
            // made in `ferrous_ffi_bridge_create` and never uses it again.
            let mut bridge = unsafe { Box::from_raw(handle) };
            bridge.shutdown()
        },
    )
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
/// Clear the wake descriptor's readiness once the UI has drained the
/// queues. It becomes ready again at once if anything is still queued, so
/// acknowledging before draining wakes the UI a second time for nothing.
///
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
        let response = rename_through_gate(&gate.gate, request);
        tag_editor::serialize_rename_response(&response).ok()
    };
    // SAFETY: The caller guarantees that `len_out` is null or writable.
    unsafe { tag_editor_call("ferrous_ffi_rename_gate_rename", len_out, request) }
}

/// Rename files for the tag editor, and tell the bridge through `gate` which
/// files moved and what the rename read from them.
pub(super) fn rename_through_gate(
    gate: &ImportGate,
    request: tag_editor::RenameRequest,
) -> tag_editor::RenameResponse {
    // A panic in the rename still finishes it in the gate, so imports
    // waiting for it go on.
    gate.rename_with_details(|| {
        let (response, refresh) = tag_editor::rename_rows_with_refresh(request);
        let renames = renamed_paths(&response);
        let details = renamed_details(&renames, refresh);
        (response, renames, details)
    })
}

/// What the bridge needs to know about the files in `renames` at their new
/// paths, read here so that the bridge thread does no file or database I/O.
fn renamed_details(renames: &PathRenames, refresh: TrackRefresh) -> RenamedDetails {
    let TrackRefresh { indexed, problem } = refresh;
    let fingerprints = renames
        .values()
        .filter_map(|path| Some((path.clone(), track_file_fingerprint(path)?)))
        .collect();
    RenamedDetails {
        indexed,
        fingerprints,
        problem,
        ..RenamedDetails::default()
    }
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

pub(super) fn parse_binary_command(payload: &[u8]) -> Result<Option<BridgeCommand>, String> {
    let Some((header, actual_payload)) = payload.split_at_checked(COMMAND_HEADER_BYTES) else {
        return Err("binary command payload too short".to_string());
    };
    let cmd_id = u16::from_le_bytes([header[0], header[1]]);
    let declared_len = usize_from_u32(u32::from_le_bytes([
        header[2], header[3], header[4], header[5],
    ]));
    if declared_len > MAX_COMMAND_PAYLOAD_BYTES || actual_payload.len() > MAX_COMMAND_PAYLOAD_BYTES
    {
        return Err(format!(
            "Command too large for the backend ({} bytes; the limit is {} MiB). Nothing was changed.",
            declared_len.max(actual_payload.len()),
            MAX_COMMAND_PAYLOAD_BYTES / (1024 * 1024)
        ));
    }
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

/// Queue commands. Those that name rows start with the u64 queue revision
/// the UI's rows showed, so the bridge can carry them across later edits.
fn parse_queue_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    if cmd_id == 12 {
        reader.expect_done()?;
        return Ok(Some(BridgeCommand::Queue(BridgeQueueCommand::Clear)));
    }
    if !matches!(cmd_id, 8 | 9 | 10 | 11 | 63) {
        return Ok(None);
    }
    let revision = reader.read_u64()?;
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
            let count = reader.read_count(4)?;
            let mut indices = Vec::with_capacity(count);
            for _ in 0..count {
                indices.push(usize_from_u32(reader.read_u32()?));
            }
            BridgeQueueCommand::RemoveMany(indices)
        }
        10 => BridgeQueueCommand::Remove(usize_from_u32(reader.read_u32()?)),
        _ => {
            let from = usize_from_u32(reader.read_u32()?);
            let to = usize_from_u32(reader.read_u32()?);
            BridgeQueueCommand::Move { from, to }
        }
    };
    reader.expect_done()?;
    Ok(Some(BridgeCommand::QueueAt { revision, command }))
}

fn parse_library_collection_command(
    cmd_id: u16,
    reader: &mut BinaryReader<'_>,
) -> Result<Option<BridgeCommand>, String> {
    let command = match cmd_id {
        13 => BridgeLibraryCommand::AddTrack(PathBuf::from(reader.read_string()?)),
        14 => BridgeLibraryCommand::PlayTrack(PathBuf::from(reader.read_string()?)),
        15 => BridgeLibraryCommand::ReplaceWithAlbum(read_path_vec(reader)?),
        16 => BridgeLibraryCommand::AppendAlbum(read_path_vec(reader)?),
        17 => BridgeLibraryCommand::ReplaceAlbumByKey {
            artist: reader.read_string()?,
            album: reader.read_string()?,
        },
        18 => BridgeLibraryCommand::AppendAlbumByKey {
            artist: reader.read_string()?,
            album: reader.read_string()?,
        },
        19 => BridgeLibraryCommand::ReplaceArtistByKey {
            artist: reader.read_string()?,
        },
        20 => BridgeLibraryCommand::AppendArtistByKey {
            artist: reader.read_string()?,
        },
        37 => BridgeLibraryCommand::ReplaceAllTracks,
        38 => BridgeLibraryCommand::AppendAllTracks,
        48 => BridgeLibraryCommand::ReplaceRootByPath {
            root: reader.read_string()?,
        },
        49 => BridgeLibraryCommand::AppendRootByPath {
            root: reader.read_string()?,
        },
        46 => BridgeLibraryCommand::ApplyAlbumArt {
            track_path: PathBuf::from(reader.read_string()?),
            artwork_path: PathBuf::from(reader.read_string()?),
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
            path: PathBuf::from(reader.read_string()?),
            name: reader.read_string()?,
        },
        22 => BridgeLibraryCommand::RemoveRoot(PathBuf::from(reader.read_string()?)),
        23 => BridgeLibraryCommand::RescanRoot(PathBuf::from(reader.read_string()?)),
        24 => BridgeLibraryCommand::RescanAll,
        35 => BridgeLibraryCommand::SetNodeExpanded {
            key: reader.read_string()?,
            expanded: reader.read_u8()? != 0,
        },
        60 => BridgeLibraryCommand::SetViewState {
            selection_key: reader.read_string()?,
            anchor_key: reader.read_string()?,
            anchor_offset: match reader.read_f32()? {
                value if value.is_finite() => value,
                _ => return Err("library view anchor offset must be finite".to_string()),
            },
        },
        61 => BridgeLibraryCommand::ShowTrackInLibrary(PathBuf::from(reader.read_string()?)),
        36 => BridgeLibraryCommand::SetSearchQuery {
            seq: reader.read_u32()?,
            query: reader.read_string()?,
            limit: if reader.offset == reader.bytes.len() {
                0
            } else {
                reader.read_u32()?
            },
        },
        45 => BridgeLibraryCommand::RenameRoot {
            path: PathBuf::from(reader.read_string()?),
            name: reader.read_string()?,
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
        66 => BridgeCommand::SaveState(reader.read_u32()?),
        _ => return Ok(None),
    };
    reader.expect_done()?;
    Ok(Some(command))
}

fn read_path_vec(reader: &mut BinaryReader<'_>) -> Result<Vec<PathBuf>, String> {
    Ok(reader
        .read_string_vec()?
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
        if N > self.remaining() {
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

    fn read_u32(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_i32(&mut self) -> Result<i32, String> {
        Ok(i32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_u64(&mut self) -> Result<u64, String> {
        Ok(u64::from_le_bytes(self.read_exact::<8>()?))
    }

    fn read_f32(&mut self) -> Result<f32, String> {
        Ok(f32::from_le_bytes(self.read_exact::<4>()?))
    }

    fn read_f64(&mut self) -> Result<f64, String> {
        Ok(f64::from_le_bytes(self.read_exact::<8>()?))
    }

    fn remaining(&self) -> usize {
        self.bytes.len() - self.offset
    }

    /// Read a u32 count of items that each take at least `min_item_bytes`,
    /// refusing one the rest of the payload cannot hold before anything is
    /// allocated for it.
    fn read_count(&mut self, min_item_bytes: usize) -> Result<usize, String> {
        let count = usize_from_u32(self.read_u32()?);
        if count > self.remaining() / min_item_bytes {
            return Err("binary command list count exceeds its payload".to_string());
        }
        Ok(count)
    }

    fn read_string(&mut self) -> Result<String, String> {
        let len = usize_from_u32(self.read_u32()?);
        if len > self.remaining() {
            return Err("binary command string truncated".to_string());
        }
        let bytes = &self.bytes[self.offset..self.offset + len];
        self.offset += len;
        Ok(String::from_utf8_lossy(bytes).into_owned())
    }

    fn read_string_vec(&mut self) -> Result<Vec<String>, String> {
        let count = self.read_count(4)?;
        let mut out = Vec::with_capacity(count);
        for _ in 0..count {
            out.push(self.read_string()?);
        }
        Ok(out)
    }
}

#[cfg(test)]
#[cfg(test)]
fn encode_binary_snapshot(snapshot: &BridgeSnapshot, queue_section: Option<Vec<u8>>) -> Vec<u8> {
    encode_packet(&snapshot_sections(snapshot, queue_section))
}

fn snapshot_sections(
    snapshot: &BridgeSnapshot,
    queue_section: Option<Vec<u8>>,
) -> SnapshotSections {
    let mut sections: SnapshotSections = vec![
        (SECTION_PLAYBACK, encode_playback_section(snapshot)),
        (SECTION_LIBRARY_META, encode_library_meta_section(snapshot)),
        (SECTION_METADATA, encode_metadata_section(snapshot)),
        (SECTION_SETTINGS, encode_settings_section(snapshot)),
        (SECTION_LASTFM, encode_lastfm_section(snapshot)),
    ];
    if let Some(section) = queue_section {
        sections.insert(1, (SECTION_QUEUE, section));
    }
    sections
}

fn encode_error_event(message: &str) -> Vec<u8> {
    let mut payload = Vec::new();
    push_string(&mut payload, message);
    encode_packet(&[(SECTION_ERROR, payload)])
}

/// The error that stands for `count` errors the UI did not take in time.
fn encode_earlier_errors_event(count: usize) -> Vec<u8> {
    encode_error_event(&if count == 1 {
        "1 earlier error was not shown.".to_string()
    } else {
        format!("{count} earlier errors were not shown.")
    })
}

fn encode_state_saved_event(request_id: u32, result: &Result<(), String>) -> Vec<u8> {
    let mut payload = Vec::new();
    push_u32(&mut payload, request_id);
    push_u8(&mut payload, u8::from(result.is_ok()));
    push_string(
        &mut payload,
        result.as_ref().err().map_or("", String::as_str),
    );
    encode_packet(&[(SECTION_STATE_SAVED, payload)])
}

fn encode_stopped_event() -> Vec<u8> {
    encode_packet(&[(SECTION_STOPPED, Vec::new())])
}

/// A stop the UI did not ask for: the stopped section with the reason.
fn encode_failure_event(message: &str) -> Vec<u8> {
    let mut payload = Vec::new();
    push_string(&mut payload, message);
    encode_packet(&[(SECTION_ERROR, payload), (SECTION_STOPPED, Vec::new())])
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
        push_string(&mut out, &row.label);
        push_string(&mut out, &row.artist);
        push_string(&mut out, &row.album);
        push_string(&mut out, &row.root_label);
        push_string(&mut out, &row.genre);
        push_string(&mut out, &row.cover_path);
        push_string(&mut out, &row.artist_key);
        push_string(&mut out, &row.album_key);
        push_string(&mut out, &row.section_key);
        push_string(&mut out, &row.track_key);
        push_string(&mut out, &row.track_path);
        push_string(&mut out, &row.match_detail);
    }
    out
}

fn encode_packet(sections: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let sections: Vec<(u16, &[u8])> = sections
        .iter()
        .map(|(bit, payload)| (*bit, payload.as_slice()))
        .collect();
    encode_packet_sections(&sections)
}

#[cfg(test)]
thread_local! {
    /// How many packets this thread has built.
    static PACKETS_ENCODED: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
}

/// Build a packet from sections in the order of their bits.
fn encode_packet_sections(sections: &[(u16, &[u8])]) -> Vec<u8> {
    #[cfg(test)]
    PACKETS_ENCODED.with(|count| count.set(count.get() + 1));
    let mut section_mask = 0u16;
    let mut total_length = 12u32;
    for (bit, payload) in sections {
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
        out.extend_from_slice(payload);
    }
    out
}

/// The sections of a packet built by [`encode_packet_sections`].
fn split_packet(packet: &[u8]) -> Vec<(u16, &[u8])> {
    let mut sections = Vec::new();
    let Some(mask) = packet
        .get(8..10)
        .map(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]))
    else {
        return sections;
    };
    let mut offset = 12;
    for bit in (0..16).map(|shift| 1u16 << shift) {
        if mask & bit == 0 {
            continue;
        }
        let Some(len) = packet
            .get(offset..offset + 4)
            .and_then(|bytes| bytes.try_into().ok())
            .map(|bytes| usize_from_u32(u32::from_le_bytes(bytes)))
        else {
            break;
        };
        offset += 4;
        let Some(payload) = packet.get(offset..offset + len) else {
            break;
        };
        sections.push((bit, payload));
        offset += len;
    }
    sections
}

/// The packet for a snapshot that replaces `older`, which the UI has not
/// taken. The bridge sends a section such as the queue only when it
/// changed, so one the older snapshot had and this one lacks is still
/// current and is kept.
fn merge_snapshot(newer: &[(u16, Vec<u8>)], older: Option<&[u8]>) -> Vec<u8> {
    let Some(older) = older else {
        return encode_packet(newer);
    };
    let mut sections: Vec<(u16, &[u8])> = newer
        .iter()
        .map(|(bit, payload)| (*bit, payload.as_slice()))
        .collect();
    for (bit, payload) in split_packet(older) {
        if !sections.iter().any(|(newer_bit, _)| *newer_bit == bit) {
            sections.push((bit, payload));
        }
    }
    sections.sort_by_key(|(bit, _)| *bit);
    encode_packet_sections(&sections)
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
    push_string(&mut out, &current_path);
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

fn encode_library_meta_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    let progress = snapshot.library_status.scan_progress.as_ref();
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
    push_u8(&mut out, u8::from(snapshot.library_status.scan_in_progress));
    push_i32(&mut out, snapshot.settings.library_sort_mode.to_i32());
    push_string(
        &mut out,
        snapshot
            .library_status
            .last_error
            .as_deref()
            .unwrap_or_default(),
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
        push_string(&mut out, &root_str);
        push_string(&mut out, &root.name);
    }
    if snapshot.pre_built_tree_bytes.is_some() {
        push_u16(&mut out, clamp_u16(snapshot.library_expanded_keys.len()));
        for key in snapshot
            .library_expanded_keys
            .iter()
            .take(usize::from(u16::MAX))
        {
            push_string(&mut out, key);
        }
        push_string(&mut out, &snapshot.library_view_selection_key);
        push_string(&mut out, &snapshot.library_view_anchor_key);
        push_f32(&mut out, snapshot.library_view_anchor_offset);
        push_u32(&mut out, snapshot.library_reveal_generation);
    }

    out
}

fn encode_metadata_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
    let mut out = Vec::new();
    push_string(
        &mut out,
        snapshot.metadata.source_path.as_deref().unwrap_or_default(),
    );
    push_string(&mut out, &snapshot.metadata.title);
    push_string(&mut out, &snapshot.metadata.artist);
    push_string(&mut out, &snapshot.metadata.album);
    push_string(&mut out, &snapshot.metadata.album_artist);
    push_string(&mut out, &snapshot.metadata.genre);
    push_i32(&mut out, snapshot.metadata.year.unwrap_or(i32::MIN));
    push_u32(&mut out, snapshot.metadata.sample_rate_hz.unwrap_or(0));
    push_u32(&mut out, snapshot.metadata.bitrate_kbps.unwrap_or(0));
    push_u16(&mut out, snapshot.metadata.channels.map_or(0, u16::from));
    push_u16(&mut out, snapshot.metadata.bit_depth.map_or(0, u16::from));
    push_string(&mut out, &snapshot.metadata.format_label);
    push_u32(
        &mut out,
        snapshot.metadata.current_bitrate_kbps.unwrap_or(0),
    );
    push_string(
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
        push_string(&mut out, label);
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
    push_string(&mut out, &snapshot.lastfm.username);
    push_string(&mut out, &snapshot.lastfm.status_text);
    push_string(&mut out, &snapshot.lastfm.auth_url);
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

pub(super) fn push_u8(out: &mut Vec<u8>, value: u8) {
    out.push(value);
}

pub(super) fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(super) fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(super) fn push_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(super) fn push_f32(out: &mut Vec<u8>, value: f32) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(super) fn push_u64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}

pub(super) fn push_f64(out: &mut Vec<u8>, value: f64) {
    out.extend_from_slice(&value.to_le_bytes());
}

/// Append a u32 byte length and the UTF-8 bytes. No string the bridge sends
/// comes near 4 GiB, and no frame could hold one, so one that did would be
/// sent empty rather than cut inside a character.
pub(super) fn push_string(out: &mut Vec<u8>, value: &str) {
    let Ok(len) = u32::try_from(value.len()) else {
        push_u32(out, 0);
        return;
    };
    push_u32(out, len);
    out.extend_from_slice(value.as_bytes());
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
        changed: has_payload,
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
    // A frame without a change would repeat the last sequence number, and
    // the UI would discard it.
    if !delta.changed {
        return Vec::new();
    }
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
        expected.extend_from_slice(&u32::try_from(24 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&((1_u16 << 6) | (1_u16 << 7)).to_le_bytes());
        expected.extend_from_slice(&0_u16.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(4 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&u32::try_from(text.len()).unwrap().to_le_bytes());
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
        expected.extend_from_slice(&u32::try_from(20 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&(1_u16 << 6).to_le_bytes());
        expected.extend_from_slice(&0_u16.to_le_bytes());
        expected.extend_from_slice(&u32::try_from(4 + text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(&u32::try_from(text.len()).unwrap().to_le_bytes());
        expected.extend_from_slice(text);
        assert_eq!(encode_error_event(message), expected);
    }
}

#[cfg(test)]
mod tests {
    use super::super::queue_relay::{QueueRelay, QUEUE_DELTA, QUEUE_FULL, QUEUE_RESYNC};
    use super::super::{config, BridgeRuntimeOptions};
    use super::super::{QueueEdit, QueueUpdate};
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
            },
            library: Arc::new(LibrarySnapshot {
                roots: vec![root("/music")],
                tracks: crate::library::shared_tracks(vec![LibraryTrack {
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
                }]),
                ..LibrarySnapshot::default()
            }),
            library_status: crate::library::LibraryStatus::default(),
            queue_details: Arc::default(),
            library_artist_count: 1,
            library_album_count: 1,
            library_expanded_keys: vec!["artist|/music|Sample Artist".to_string()],
            library_view_selection_key: "track|/music/a.flac".to_string(),
            library_view_anchor_key: "artist|/music|Sample Artist".to_string(),
            library_view_anchor_offset: 4.0,
            library_reveal_generation: 0,
            pre_built_tree_bytes: Some(Arc::new(vec![0, 0, 0, 0])),
            queue_len: 1,
            queue_revision: 1,
            queue_update: Some(QueueUpdate::Full(vec![PathBuf::from("/music/a.flac")])),
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
        let mut out = Vec::with_capacity(COMMAND_HEADER_BYTES + payload.len());
        out.extend_from_slice(&cmd_id.to_le_bytes());
        out.extend_from_slice(&clamp_u32(payload.len()).to_le_bytes());
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

    fn read_string(bytes: &[u8], offset: &mut usize) -> String {
        let len = usize::try_from(read_u32(bytes, offset)).expect("string length");
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
        push_u64(&mut payload, 41);
        push_u32(&mut payload, 3);
        for index in [9, 5, 1] {
            push_u32(&mut payload, index);
        }
        assert!(matches!(
            parse_binary_command(&encode_command(63, &payload)).expect("batch"),
            Some(BridgeCommand::QueueAt {
                revision: 41,
                command: BridgeQueueCommand::RemoveMany(indices),
            }) if indices == vec![9, 5, 1]
        ));
        payload.pop();
        assert!(parse_binary_command(&encode_command(63, &payload)).is_err());
    }

    #[test]
    fn row_commands_carry_the_revision_the_ui_showed() {
        let with_revision = |fields: &[u32]| {
            let mut payload = Vec::new();
            push_u64(&mut payload, 7);
            for field in fields {
                push_u32(&mut payload, *field);
            }
            payload
        };
        let parse = |cmd_id, payload: &[u8]| {
            parse_binary_command(&encode_command(cmd_id, payload))
                .expect("parse")
                .expect("command")
        };
        assert!(matches!(
            parse(8, &with_revision(&[3])),
            BridgeCommand::QueueAt {
                revision: 7,
                command: BridgeQueueCommand::PlayAt(3)
            }
        ));
        assert!(matches!(
            parse(9, &with_revision(&[u32::MAX])),
            BridgeCommand::QueueAt {
                revision: 7,
                command: BridgeQueueCommand::Select(None)
            }
        ));
        assert!(matches!(
            parse(10, &with_revision(&[2])),
            BridgeCommand::QueueAt {
                revision: 7,
                command: BridgeQueueCommand::Remove(2)
            }
        ));
        assert!(matches!(
            parse(11, &with_revision(&[1, 4])),
            BridgeCommand::QueueAt {
                revision: 7,
                command: BridgeQueueCommand::Move { from: 1, to: 4 },
            }
        ));
        assert!(matches!(
            parse(12, &[]),
            BridgeCommand::Queue(BridgeQueueCommand::Clear)
        ));
        // A row command without its revision is refused, not misread.
        assert!(parse_binary_command(&encode_command(10, &2u32.to_le_bytes())).is_err());
    }

    /// A string list payload as the Qt encoder writes it.
    fn string_list_payload(values: &[String]) -> Vec<u8> {
        let mut payload = Vec::new();
        push_u32(&mut payload, u32::try_from(values.len()).expect("count"));
        for value in values {
            push_string(&mut payload, value);
        }
        payload
    }

    /// 65,534 ASCII bytes, then characters of 2, 3, and 4 bytes that
    /// straddle the 65,535 bytes a u16 length could hold.
    fn string_past_the_old_length_limit() -> String {
        let text = format!("{}é€😀tail", "a".repeat(65_534));
        assert!(text.len() > usize::from(u16::MAX));
        assert!(!text.is_char_boundary(usize::from(u16::MAX)));
        text
    }

    #[test]
    fn a_path_list_far_beyond_64_kib_decodes_whole_and_in_order() {
        let paths: Vec<String> = (0..5_000)
            .rev()
            .map(|i| format!("/fixture/Ärtist {i:04}/トラック – {i:04}.flac"))
            .collect();
        let payload = string_list_payload(&paths);
        assert!(payload.len() > 3 * usize::from(u16::MAX));
        let expected: Vec<PathBuf> = paths.iter().map(PathBuf::from).collect();
        for cmd_id in [15, 16] {
            match parse_binary_command(&encode_command(cmd_id, &payload)) {
                Ok(Some(BridgeCommand::Library(
                    BridgeLibraryCommand::ReplaceWithAlbum(parsed)
                    | BridgeLibraryCommand::AppendAlbum(parsed),
                ))) => assert_eq!(parsed, expected, "command {cmd_id}"),
                other => panic!("unexpected command: {other:?}"),
            }
        }
        assert!(matches!(
            parse_binary_command(&encode_command(47, &payload)),
            Ok(Some(BridgeCommand::Library(BridgeLibraryCommand::RefreshEditedPaths(parsed))))
                if parsed == expected
        ));
    }

    #[test]
    fn strings_past_the_old_length_limit_cross_the_bridge_whole_both_ways() {
        // Mirrored by BridgeClientTest::stringsPastTheOldLengthLimitCrossWhole.
        let text = string_past_the_old_length_limit();
        let mut payload = Vec::new();
        push_string(&mut payload, &text);
        assert_eq!(
            &payload[..4],
            &u32::try_from(text.len()).unwrap().to_le_bytes()
        );
        assert!(matches!(
            parse_binary_command(&encode_command(14, &payload)),
            Ok(Some(BridgeCommand::Library(BridgeLibraryCommand::PlayTrack(path))))
                if path.as_os_str() == text.as_str()
        ));

        // The UI gets the same string whole in an error packet.
        let packet = encode_failure_event(&text);
        assert_eq!(failure_message(&packet).as_deref(), Some(text.as_str()));
    }

    #[test]
    fn commands_over_the_bound_are_refused_with_a_message_the_ui_shows() {
        let (command_tx, command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        let oversized = vec![0_u8; MAX_COMMAND_PAYLOAD_BYTES + 1];
        let parsed = parse_binary_command(&encode_command(15, &oversized));
        let message = parsed.clone().expect_err("over the bound");
        assert!(message.contains("too large"), "{message}");
        assert!(message.contains("8 MiB"), "{message}");
        assert!(!runtime.send_command(parsed));
        assert!(command_rx.try_recv().is_err(), "nothing reaches the bridge");
        let packet = runtime.pop_binary_event().expect("error event");
        let (_, _, mask) = parse_packet_header(&packet);
        assert_eq!(mask, SECTION_ERROR);
        let mut offset = 16;
        assert_eq!(read_string(&packet, &mut offset), message);

        // A header that claims more than the bound is refused before the
        // payload is looked at.
        let mut header = encode_command(15, &[]);
        header[2..6].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(parse_binary_command(&header)
            .expect_err("claimed length")
            .contains("too large"));

        // A list at the bound is still taken.
        let path = "p".repeat(1020);
        let fill = (MAX_COMMAND_PAYLOAD_BYTES - 4) / (4 + path.len());
        let payload = string_list_payload(&vec![path; fill]);
        assert!(payload.len() <= MAX_COMMAND_PAYLOAD_BYTES);
        assert!(parse_binary_command(&encode_command(16, &payload)).is_ok());
    }

    #[test]
    fn truncated_or_corrupt_lengths_are_refused_without_a_panic() {
        let payload = string_list_payload(&["/fixture/a.flac".to_string(), "é€😀".to_string()]);
        let command = encode_command(15, &payload);
        // Every cut of the command is refused: in the header, or because the
        // payload no longer matches its length.
        for len in 0..command.len() {
            assert!(
                parse_binary_command(&command[..len]).is_err(),
                "cut at {len}"
            );
        }
        // Every cut of the payload, framed with its own length, is refused
        // by the reader, also within a character.
        for len in 0..payload.len() {
            assert!(
                parse_binary_command(&encode_command(15, &payload[..len])).is_err(),
                "payload cut at {len}"
            );
        }
        let long = string_list_payload(&[string_past_the_old_length_limit()]);
        for len in [8, 4 + usize::from(u16::MAX), long.len() - 1] {
            assert!(parse_binary_command(&encode_command(15, &long[..len])).is_err());
        }

        // Counts and lengths the payload cannot hold allocate nothing.
        let mut huge_count = Vec::new();
        push_u32(&mut huge_count, u32::MAX);
        for cmd_id in [15, 16, 47, 63] {
            assert!(parse_binary_command(&encode_command(cmd_id, &huge_count)).is_err());
        }
        let mut huge_string = Vec::new();
        push_u32(&mut huge_string, 1);
        push_u32(&mut huge_string, u32::MAX);
        huge_string.extend_from_slice(b"short");
        assert!(parse_binary_command(&encode_command(15, &huge_string)).is_err());
        assert!(parse_binary_command(&encode_command(14, &huge_string[4..])).is_err());
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
        push_u32(&mut payload, 2);
        push_string(&mut payload, "/fixture/old.ac3");
        push_string(&mut payload, "/fixture/new.ac3");
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
        push_u32(&mut payload, 2);
        push_string(&mut payload, "/music/a.flac");
        push_string(&mut payload, "/music/b.flac");

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
        push_string(&mut payload, key);
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
        push_string(&mut payload, selection_key);
        push_string(&mut payload, anchor_key);
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
        push_string(&mut payload, path);
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
        push_string(&mut payload, "blue");
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
        push_string(&mut suffix, "Comment: remaster");
        assert!(bytes.ends_with(&suffix));
    }

    #[test]
    fn parse_binary_command_supports_set_search_query() {
        let query = "pink floyd";
        let mut payload = Vec::new();
        payload.extend_from_slice(&42u32.to_le_bytes());
        push_string(&mut payload, query);

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

    /// The section a relay that has no queue yet sends for `snapshot`.
    fn full_queue_section(snapshot: &BridgeSnapshot) -> Vec<u8> {
        queue_sections(&mut QueueRelay::default(), [snapshot]).expect("a queue section")
    }

    /// The section the relay sends for a batch of `snapshots`.
    fn queue_sections<'a>(
        relay: &mut QueueRelay,
        snapshots: impl IntoIterator<Item = &'a BridgeSnapshot>,
    ) -> Option<Vec<u8>> {
        let mut batch = None;
        let mut latest = None;
        for snapshot in snapshots {
            relay.apply(snapshot, &mut batch);
            latest = Some(snapshot);
        }
        Some(relay.section(batch?, latest?))
    }

    fn outside_track(title: &str) -> crate::library::IndexedTrack {
        crate::library::IndexedTrack {
            search_tags: crate::library::SearchTags::default(),
            title: title.to_string(),
            artist: "Outside Artist".to_string(),
            album: "Outside Album".to_string(),
            cover_path: "/outside/cover.jpg".to_string(),
            genre: "Ambient".to_string(),
            year: Some(2024),
            track_no: Some(7),
            duration_secs: Some(245.0),
        }
    }

    /// A snapshot that carries `update`, which leaves the queue at
    /// `revision`.
    fn snapshot_with_update(update: QueueUpdate, revision: u64) -> BridgeSnapshot {
        let mut snapshot = sample_snapshot();
        snapshot.queue_revision = revision;
        snapshot.queue_update = Some(update);
        snapshot
    }

    fn edits(base_revision: u64, edits: Vec<QueueEdit>) -> QueueUpdate {
        QueueUpdate::Edits {
            base_revision,
            edits,
        }
    }

    fn paths(names: &[&str]) -> Vec<PathBuf> {
        names.iter().map(PathBuf::from).collect()
    }

    #[derive(Debug, PartialEq)]
    enum TestQueueOp {
        Insert(u32, Vec<String>),
        Remove(u32, u32),
        Move(u32, u32),
        Update(u32, String, String),
    }

    /// A queue section as the UI decodes it. Rows are (title, path).
    #[derive(Debug, Default)]
    struct TestQueueSection {
        kind: u8,
        revision: u64,
        len: u32,
        selected: i32,
        total_seconds: f64,
        unknown: u32,
        base_revision: u64,
        rows: Vec<(String, String)>,
        ops: Vec<TestQueueOp>,
    }

    fn read_queue_row(bytes: &[u8], offset: &mut usize) -> (String, String) {
        let title = read_string(bytes, offset);
        for _ in 0..4 {
            read_string(bytes, offset);
        }
        let _year = read_i32(bytes, offset);
        let _track = read_u16(bytes, offset);
        let _length = read_f32(bytes, offset);
        (title, read_string(bytes, offset))
    }

    fn decode_queue_section(section: &[u8]) -> TestQueueSection {
        let mut offset = 1;
        let mut decoded = TestQueueSection {
            kind: section[0],
            revision: read_u64(section, &mut offset),
            len: read_u32(section, &mut offset),
            selected: read_i32(section, &mut offset),
            total_seconds: read_f64(section, &mut offset),
            unknown: read_u32(section, &mut offset),
            ..TestQueueSection::default()
        };
        match decoded.kind {
            QUEUE_FULL => {
                for _ in 0..decoded.len {
                    decoded.rows.push(read_queue_row(section, &mut offset));
                }
            }
            QUEUE_DELTA => {
                decoded.base_revision = read_u64(section, &mut offset);
                let count = read_u32(section, &mut offset);
                for _ in 0..count {
                    let op = section[offset];
                    offset += 1;
                    let first = read_u32(section, &mut offset);
                    decoded.ops.push(match op {
                        1 => {
                            let count = read_u32(section, &mut offset);
                            TestQueueOp::Insert(
                                first,
                                (0..count)
                                    .map(|_| read_queue_row(section, &mut offset).1)
                                    .collect(),
                            )
                        }
                        2 => TestQueueOp::Remove(first, read_u32(section, &mut offset)),
                        3 => TestQueueOp::Move(first, read_u32(section, &mut offset)),
                        4 => {
                            let (title, path) = read_queue_row(section, &mut offset);
                            TestQueueOp::Update(first, title, path)
                        }
                        other => panic!("unknown queue operation {other}"),
                    });
                }
            }
            kind => assert_eq!(kind, QUEUE_RESYNC),
        }
        assert_eq!(offset, section.len(), "the whole section is read");
        decoded
    }

    /// The UI's queue, which applies queue sections as the Qt client does.
    #[derive(Default)]
    struct TestUiQueue {
        revision: u64,
        paths: Vec<String>,
        resyncs: usize,
    }

    impl TestUiQueue {
        fn apply(&mut self, section: &[u8]) {
            let decoded = decode_queue_section(section);
            match decoded.kind {
                QUEUE_FULL => {
                    self.paths = decoded.rows.into_iter().map(|(_, path)| path).collect();
                }
                QUEUE_DELTA if decoded.base_revision == self.revision => {
                    for op in decoded.ops {
                        match op {
                            TestQueueOp::Insert(at, rows) => {
                                let at = usize_from_u32(at);
                                self.paths.splice(at..at, rows);
                            }
                            TestQueueOp::Remove(start, count) => {
                                let start = usize_from_u32(start);
                                self.paths.drain(start..start + usize_from_u32(count));
                            }
                            TestQueueOp::Move(from, to) => {
                                let path = self.paths.remove(usize_from_u32(from));
                                self.paths.insert(usize_from_u32(to), path);
                            }
                            TestQueueOp::Update(index, _, path) => {
                                self.paths[usize_from_u32(index)] = path;
                            }
                        }
                    }
                }
                _ => {
                    self.resyncs += 1;
                    return;
                }
            }
            assert_eq!(self.paths.len(), usize_from_u32(decoded.len));
            self.revision = decoded.revision;
        }
    }

    fn path_strings(paths: &[PathBuf]) -> Vec<String> {
        paths
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect()
    }

    #[test]
    fn queue_section_encodes_enriched_track_rows() {
        let encoded = full_queue_section(&sample_snapshot());

        assert_eq!(encoded[0], QUEUE_FULL);
        let mut offset = 1usize;
        assert_eq!(read_u64(&encoded, &mut offset), 1);
        assert_eq!(read_u32(&encoded, &mut offset), 1);
        assert_eq!(read_i32(&encoded, &mut offset), 0);
        assert!((read_f64(&encoded, &mut offset) - 180.0).abs() < 0.001);
        assert_eq!(read_u32(&encoded, &mut offset), 0);
        assert_eq!(read_string(&encoded, &mut offset), "Sample Track");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Artist");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Album");
        assert_eq!(read_string(&encoded, &mut offset), "/music/a.cover.png");
        assert_eq!(read_string(&encoded, &mut offset), "Rock");
        assert_eq!(read_i32(&encoded, &mut offset), 2020);
        assert_eq!(read_u16(&encoded, &mut offset), 1);
        assert!((read_f32(&encoded, &mut offset) - 180.0).abs() < 0.001);
        assert_eq!(read_string(&encoded, &mut offset), "/music/a.flac");
        assert_eq!(offset, encoded.len());
    }

    #[test]
    fn queue_section_uses_external_queue_details_when_library_misses() {
        let mut snapshot = snapshot_with_update(
            QueueUpdate::Full(paths(&["/outside/song.flac", "/outside/unknown.flac"])),
            1,
        );
        snapshot.library = Arc::new(LibrarySnapshot::default());
        Arc::make_mut(&mut snapshot.queue_details).insert(
            PathBuf::from("/outside/song.flac"),
            outside_track("Outside Song"),
        );

        let mut relay = QueueRelay::default();
        let section = decode_queue_section(&queue_sections(&mut relay, [&snapshot]).unwrap());
        assert_eq!(section.unknown, 1);
        assert!((section.total_seconds - 245.0).abs() < 0.001);
        let row = &relay.rows()[0];
        assert_eq!(row.title, "Outside Song");
        assert_eq!(row.artist, "Outside Artist");
        assert_eq!(row.album, "Outside Album");
        assert_eq!(row.cover_path, "/outside/cover.jpg");
        assert_eq!(row.genre, "Ambient");
        assert_eq!(row.year, Some(2024));
        assert_eq!(row.track_number, Some(7));
        assert_eq!(row.length_seconds, Some(245.0));
        // A file the bridge knows nothing about shows its name.
        assert_eq!(relay.rows()[1].title, "unknown.flac");
        assert_eq!(relay.rows()[1].length_seconds, None);
    }

    #[test]
    fn a_refresh_sends_only_the_rows_whose_details_changed() {
        let queue = paths(&["/outside/song.flac", "/music/a.flac", "/outside/other.flac"]);
        let mut snapshot = snapshot_with_update(QueueUpdate::Full(queue), 1);
        Arc::make_mut(&mut snapshot.queue_details)
            .insert(PathBuf::from("/outside/song.flac"), outside_track("Before"));
        let mut relay = QueueRelay::default();
        let _ = queue_sections(&mut relay, [&snapshot]);

        Arc::make_mut(&mut snapshot.queue_details)
            .insert(PathBuf::from("/outside/song.flac"), outside_track("After"));
        snapshot.queue_revision = 2;
        snapshot.queue_update = Some(edits(1, vec![QueueEdit::Refresh]));
        let section = decode_queue_section(&queue_sections(&mut relay, [&snapshot]).unwrap());
        assert_eq!(section.kind, QUEUE_DELTA);
        assert_eq!((section.base_revision, section.revision), (1, 2));
        assert_eq!(
            section.ops,
            vec![TestQueueOp::Update(
                0,
                "After".to_string(),
                "/outside/song.flac".to_string()
            )]
        );

        // A refresh that changes nothing sends no rows.
        snapshot.queue_revision = 3;
        snapshot.queue_update = Some(edits(2, vec![QueueEdit::Refresh]));
        let section = decode_queue_section(&queue_sections(&mut relay, [&snapshot]).unwrap());
        assert_eq!(section.kind, QUEUE_DELTA);
        assert!(section.ops.is_empty());
        assert_eq!(section.revision, 3);
    }

    #[test]
    fn each_queue_edit_becomes_its_row_operations() {
        let start = paths(&["/0", "/1", "/2", "/3", "/4", "/5"]);
        let mut relay = QueueRelay::default();
        let mut ui = TestUiQueue::default();
        ui.apply(
            &queue_sections(
                &mut relay,
                [&snapshot_with_update(QueueUpdate::Full(start.clone()), 1)],
            )
            .unwrap(),
        );

        let edit_list = vec![
            QueueEdit::Insert {
                at: 2,
                paths: paths(&["/x", "/y"]),
            },
            // [0, 1, x, y, 2, 3, 4, 5]
            QueueEdit::Remove(vec![0, 3, 4, 7]),
            // [1, x, 3, 4]
            QueueEdit::Move { from: 0, to: 3 },
            // [x, 3, 4, 1]
            QueueEdit::SetPaths(vec![(1, PathBuf::from("/renamed"))]),
        ];
        let snapshot = snapshot_with_update(edits(1, edit_list.clone()), 5);
        let section = queue_sections(&mut relay, [&snapshot]).unwrap();
        let decoded = decode_queue_section(&section);
        assert_eq!(
            decoded.ops,
            vec![
                TestQueueOp::Insert(2, vec!["/x".to_string(), "/y".to_string()]),
                // Last range first.
                TestQueueOp::Remove(7, 1),
                TestQueueOp::Remove(3, 2),
                TestQueueOp::Remove(0, 1),
                TestQueueOp::Move(0, 3),
                TestQueueOp::Update(1, "renamed".to_string(), "/renamed".to_string()),
            ]
        );
        ui.apply(&section);
        let mut expected = start;
        for edit in &edit_list {
            assert!(edit.apply_to(&mut expected));
        }
        assert_eq!(ui.paths, path_strings(&expected));
        assert_eq!(ui.revision, 5);
        assert_eq!(decoded.selected, 0);
    }

    #[test]
    fn an_edit_in_a_large_queue_sends_only_the_rows_it_changes() {
        let queue: Vec<PathBuf> = (0..10_000)
            .map(|index| PathBuf::from(format!("/outside/album/{index:05} track.flac")))
            .collect();
        let mut snapshot = snapshot_with_update(QueueUpdate::Full(queue.clone()), 1);
        snapshot.library = Arc::new(LibrarySnapshot::default());
        let mut relay = QueueRelay::default();
        let mut ui = TestUiQueue::default();
        let full = queue_sections(&mut relay, [&snapshot]).unwrap();
        ui.apply(&full);
        assert!(
            full.len() > 10_000 * 30,
            "the whole queue is {} bytes",
            full.len()
        );

        let mut expected = queue;
        let mut send = |snapshot: &mut BridgeSnapshot, edit: QueueEdit| {
            let base = snapshot.queue_revision;
            assert!(edit.apply_to(&mut expected));
            snapshot.queue_revision += 1;
            snapshot.queue_update = Some(edits(base, vec![edit]));
            let section = queue_sections(&mut relay, [&*snapshot]).unwrap();
            ui.apply(&section);
            assert_eq!(ui.paths, path_strings(&expected));
            let decoded = decode_queue_section(&section);
            assert_eq!(decoded.kind, QUEUE_DELTA);
            (section.len(), decoded.ops.len())
        };

        // The header, the operation count, and one operation of no rows.
        assert_eq!(send(&mut snapshot, QueueEdit::Remove(vec![5_000])), (50, 1));
        assert_eq!(
            send(&mut snapshot, QueueEdit::Move { from: 9_000, to: 2 }),
            (50, 1)
        );
        // One new row, and the details of one row.
        let (bytes, ops) = send(
            &mut snapshot,
            QueueEdit::Insert {
                at: 4_000,
                paths: paths(&["/outside/new.flac"]),
            },
        );
        assert_eq!(ops, 1);
        assert!(bytes < 150, "{bytes} bytes");
        Arc::make_mut(&mut snapshot.queue_details).insert(
            PathBuf::from("/outside/album/07777 track.flac"),
            outside_track("Tagged"),
        );
        let (bytes, ops) = send(&mut snapshot, QueueEdit::Refresh);
        assert_eq!(ops, 1);
        assert!(bytes < 200, "{bytes} bytes");
        let tagged = relay
            .rows()
            .iter()
            .find(|row| row.path.ends_with("07777 track.flac"))
            .expect("the tagged row");
        assert_eq!(tagged.title, "Tagged");
        assert_eq!(ui.resyncs, 0);
    }

    #[test]
    fn larger_changes_fall_back_to_the_whole_queue_and_lost_ones_to_a_resync() {
        let queue: Vec<PathBuf> = (0..1_000)
            .map(|index| PathBuf::from(format!("/outside/{index:04}.flac")))
            .collect();
        let mut snapshot = snapshot_with_update(QueueUpdate::Full(queue.clone()), 1);
        snapshot.library = Arc::new(LibrarySnapshot::default());
        let mut relay = QueueRelay::default();
        let mut ui = TestUiQueue::default();
        ui.apply(&queue_sections(&mut relay, [&snapshot]).unwrap());

        // In a small queue, updates to every row still travel as a delta.
        let small = paths(&["/outside/0000.flac", "/outside/0001.flac"]);
        let mut small_snapshot = snapshot_with_update(QueueUpdate::Full(small.clone()), 1);
        let mut small_relay = QueueRelay::default();
        let _ = queue_sections(&mut small_relay, [&small_snapshot]);
        for path in &small {
            Arc::make_mut(&mut small_snapshot.queue_details)
                .insert(path.clone(), outside_track("New"));
        }
        small_snapshot.queue_revision = 2;
        small_snapshot.queue_update = Some(edits(1, vec![QueueEdit::Refresh]));
        let section = queue_sections(&mut small_relay, [&small_snapshot]).unwrap();
        assert_eq!(decode_queue_section(&section).ops.len(), 2);

        // In a large one, the rows alone are smaller than updates to them.
        for path in &queue {
            Arc::make_mut(&mut snapshot.queue_details).insert(path.clone(), outside_track("New"));
        }
        snapshot.queue_revision = 2;
        snapshot.queue_update = Some(edits(1, vec![QueueEdit::Refresh]));
        let section = queue_sections(&mut relay, [&snapshot]).unwrap();
        assert_eq!(section[0], QUEUE_FULL);
        ui.apply(&section);
        assert_eq!(ui.revision, 2);

        // An edit that does not fit the rows, or does not follow them, loses
        // track of the queue until it comes whole again.
        for update in [
            edits(2, vec![QueueEdit::Remove(vec![1_000])]),
            edits(1, vec![QueueEdit::Remove(vec![0])]),
        ] {
            let lost = snapshot_with_update(update, 3);
            let section = queue_sections(&mut relay, [&lost]).unwrap();
            assert_eq!(section[0], QUEUE_RESYNC);
            ui.apply(&section);
            let after = snapshot_with_update(edits(3, vec![QueueEdit::Refresh]), 4);
            assert_eq!(
                queue_sections(&mut relay, [&after]).unwrap()[0],
                QUEUE_RESYNC
            );
            snapshot.queue_revision = 5;
            snapshot.queue_update = Some(QueueUpdate::Full(queue.clone()));
            let section = queue_sections(&mut relay, [&snapshot]).unwrap();
            assert_eq!(section[0], QUEUE_FULL);
            ui.apply(&section);
            assert_eq!(ui.revision, 5);
        }
        assert_eq!(ui.resyncs, 2);
    }

    #[test]
    fn snapshots_the_ui_has_not_taken_carry_every_queue_delta() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        let mut ui = TestUiQueue::default();
        let take_queue = |runtime: &mut FfiRuntime, ui: &mut TestUiQueue| {
            let packets = pending_packets(runtime);
            assert_eq!(packets.len(), 1, "one snapshot waits");
            let queue = section(&packets[0], SECTION_QUEUE).expect("a queue section");
            ui.apply(&queue);
            decode_queue_section(&queue)
        };
        let publish = |runtime: &mut FfiRuntime, update, revision| {
            runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_update(
                update, revision,
            )))]);
        };
        publish(&mut runtime, QueueUpdate::Full(paths(&["/a", "/b"])), 1);
        assert_eq!(take_queue(&mut runtime, &mut ui).kind, QUEUE_FULL);

        // The UI stalls through three edits and a snapshot without one.
        publish(
            &mut runtime,
            edits(
                1,
                vec![QueueEdit::Insert {
                    at: 2,
                    paths: paths(&["/c"]),
                }],
            ),
            2,
        );
        publish(&mut runtime, edits(2, vec![QueueEdit::Remove(vec![0])]), 3);
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_without_queue(9)))]);
        publish(
            &mut runtime,
            edits(3, vec![QueueEdit::Move { from: 1, to: 0 }]),
            4,
        );
        let merged = take_queue(&mut runtime, &mut ui);
        assert_eq!(merged.kind, QUEUE_DELTA);
        assert_eq!((merged.base_revision, merged.revision), (1, 4));
        assert_eq!(merged.ops.len(), 3);
        assert_eq!(ui.paths, ["/c", "/b"]);

        // A whole queue waiting for the UI stays whole with the edits after it.
        publish(&mut runtime, QueueUpdate::Full(paths(&["/x", "/y"])), 5);
        publish(&mut runtime, edits(5, vec![QueueEdit::Remove(vec![0])]), 6);
        let merged = take_queue(&mut runtime, &mut ui);
        assert_eq!(merged.kind, QUEUE_FULL);
        assert_eq!(ui.paths, ["/y"]);
        assert_eq!((ui.revision, ui.resyncs), (6, 0));
    }

    #[test]
    fn metadata_section_encodes_genre_and_year() {
        let snapshot = sample_snapshot();
        let encoded = encode_metadata_section(&snapshot);

        let mut offset = 0usize;
        assert_eq!(read_string(&encoded, &mut offset), "/music/a.flac");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Track");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Artist");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Album");
        assert_eq!(read_string(&encoded, &mut offset), "Sample Album Artist");
        assert_eq!(read_string(&encoded, &mut offset), "Rock");
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
        assert_eq!(read_string(&encoded, &mut offset), "FLAC");
        assert_eq!(read_u32(&encoded, &mut offset), 905);
        assert_eq!(read_string(&encoded, &mut offset), "/music/a.cover.png");
        assert_eq!(read_u32(&encoded, &mut offset), 3);
        assert_eq!(&encoded[offset..offset + 2], &2u16.to_le_bytes());
        offset += 2;
        assert_eq!(read_string(&encoded, &mut offset), "L");
        assert_eq!(read_string(&encoded, &mut offset), "R");
        assert_eq!(offset, encoded.len());
    }

    #[test]
    fn snapshot_packet_contract_has_expected_shape() {
        let snapshot = sample_snapshot();
        let packet = encode_binary_snapshot(&snapshot, Some(full_queue_section(&snapshot)));
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
        let packet = encode_binary_snapshot(&snapshot, Some(full_queue_section(&snapshot)));
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
        let packet = encode_binary_snapshot(&snapshot, Some(full_queue_section(&snapshot)));
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
        let packet = encode_binary_snapshot(&snapshot, Some(full_queue_section(&snapshot)));
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
        assert_eq!(read_string(lastfm, &mut lastfm_offset), "tester");
        assert_eq!(read_string(lastfm, &mut lastfm_offset), "Connected");
        assert_eq!(read_string(lastfm, &mut lastfm_offset), "");
        assert_eq!(lastfm_offset, lastfm.len());
    }

    #[test]
    fn snapshot_packet_can_omit_queue_section() {
        let mut snapshot = sample_snapshot();
        snapshot.queue_update = None;

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
    fn analysis_delta_and_frame_include_changes() {
        let snapshot = sample_snapshot();
        let mut emit_state = AnalysisEmitState::default();
        let delta = compute_analysis_delta(&snapshot, &mut emit_state);
        assert!(delta.waveform_changed);
        let frame = encode_analysis_frame(&delta);
        assert!(!frame.is_empty());
        assert_eq!(frame[4], ANALYSIS_FRAME_MAGIC);
    }

    /// The sections of a packet, by bit, as the UI reads them.
    fn packet_sections(packet: &[u8]) -> Vec<(u16, Vec<u8>)> {
        let (magic, total_len, mask) = parse_packet_header(packet);
        assert_eq!(magic, SNAPSHOT_MAGIC);
        assert_eq!(usize_from_u32(total_len), packet.len());
        let mut offset = 12;
        let mut sections = Vec::new();
        for bit in (0..16).map(|shift| 1u16 << shift) {
            if mask & bit == 0 {
                continue;
            }
            let len = usize_from_u32(read_u32(packet, &mut offset));
            sections.push((bit, packet[offset..offset + len].to_vec()));
            offset += len;
        }
        assert_eq!(offset, packet.len());
        sections
    }

    fn section(packet: &[u8], bit: u16) -> Option<Vec<u8>> {
        packet_sections(packet)
            .into_iter()
            .find_map(|(section_bit, payload)| (section_bit == bit).then_some(payload))
    }

    /// The paths of the rows in a whole-queue section.
    fn queue_section_paths(section: &[u8]) -> Vec<String> {
        let decoded = decode_queue_section(section);
        assert_eq!(decoded.kind, QUEUE_FULL);
        decoded.rows.into_iter().map(|(_, path)| path).collect()
    }

    fn snapshot_with_queue(paths: &[&str], position_secs: u64) -> BridgeSnapshot {
        let mut snapshot = sample_snapshot();
        snapshot.queue_update = Some(QueueUpdate::Full(paths.iter().map(PathBuf::from).collect()));
        snapshot.playback.position = Duration::from_secs(position_secs);
        snapshot
    }

    fn snapshot_without_queue(position_secs: u64) -> BridgeSnapshot {
        let mut snapshot = snapshot_with_queue(&[], position_secs);
        snapshot.queue_update = None;
        snapshot
    }

    fn pending_packets(runtime: &mut FfiRuntime) -> Vec<Vec<u8>> {
        std::iter::from_fn(|| runtime.pop_binary_event()).collect()
    }

    #[test]
    fn a_stalled_consumer_receives_the_latest_queue_and_every_error() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        // The UI stalls while the relay publishes far more batches than the
        // old queue of 12 held: the queue changes once early, then only
        // playback moves, with errors in between.
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_queue(
            &["/music/old.flac"],
            0,
        )))]);
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_queue(
            &["/music/a.flac", "/music/b.flac"],
            1,
        )))]);
        for second in 2..60 {
            let mut batch = vec![BridgeEvent::Snapshot(Box::new(snapshot_without_queue(
                second,
            )))];
            if second % 3 == 0 {
                batch.push(BridgeEvent::Error(format!("error {second}")));
            }
            runtime.process_bridge_events(batch);
        }
        runtime.process_bridge_events([BridgeEvent::StateSaved {
            request_id: 9,
            result: Ok(()),
        }]);

        let packets = pending_packets(&mut runtime);
        let errors: Vec<String> = packets
            .iter()
            .filter_map(|packet| section(packet, SECTION_ERROR))
            .map(|payload| read_string(&payload, &mut 0))
            .collect();
        let expected: Vec<String> = (2..60)
            .filter(|second| second % 3 == 0)
            .map(|second| format!("error {second}"))
            .collect();
        assert_eq!(errors, expected);
        assert_eq!(
            packets
                .iter()
                .filter(|packet| section(packet, SECTION_STATE_SAVED).is_some())
                .count(),
            1
        );
        // One snapshot: the newest playback with the newest queue.
        let snapshots: Vec<&Vec<u8>> = packets
            .iter()
            .filter(|packet| section(packet, SECTION_PLAYBACK).is_some())
            .collect();
        assert_eq!(snapshots.len(), 1);
        let decoded = decode_binary_snapshot_for_test(snapshots[0]);
        assert!((decoded.playback_position - 59.0).abs() < f64::EPSILON);
        let queue = section(snapshots[0], SECTION_QUEUE).expect("the queue section");
        assert_eq!(
            queue_section_paths(&queue),
            ["/music/a.flac", "/music/b.flac"]
        );
        // The merged packet keeps every section of the newest snapshot.
        let bits: Vec<u16> = packet_sections(snapshots[0])
            .iter()
            .map(|(bit, _)| *bit)
            .collect();
        assert_eq!(
            bits,
            [
                SECTION_PLAYBACK,
                SECTION_QUEUE,
                SECTION_LIBRARY_META,
                SECTION_METADATA,
                SECTION_SETTINGS,
                SECTION_LASTFM
            ]
        );
    }

    #[test]
    fn a_newer_queue_replaces_the_one_waiting_for_the_ui() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_queue(
            &["/music/old.flac"],
            0,
        )))]);
        runtime
            .process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_queue(&[], 1)))]);

        let packets = pending_packets(&mut runtime);
        assert_eq!(packets.len(), 1);
        let queue = section(&packets[0], SECTION_QUEUE).expect("the queue section");
        assert!(queue_section_paths(&queue).is_empty());
        assert!(runtime.pop_binary_event().is_none());

        // Once the UI has taken a queue, a snapshot without one carries none.
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_without_queue(2)))]);
        let packets = pending_packets(&mut runtime);
        assert_eq!(packets.len(), 1);
        assert!(section(&packets[0], SECTION_QUEUE).is_none());
    }

    #[test]
    fn queue_rows_follow_a_new_library_at_the_old_ones_address() {
        let mut relay = QueueRelay::default();
        let mut snapshot = sample_snapshot();
        let _ = queue_sections(&mut relay, [&snapshot]);
        assert_eq!(relay.rows()[0].title, "Sample Track");

        // The bridge replaces its library once the relay has let go of the
        // old one, and the allocator may build the new one at the old
        // address. Editing the library in place stands in for that: it
        // keeps the address whenever nothing else holds the library.
        let address = Arc::as_ptr(&snapshot.library).addr();
        Arc::make_mut(&mut Arc::make_mut(&mut snapshot.library).tracks[0]).title =
            "Renamed Track".to_string();
        snapshot.queue_revision = 2;
        snapshot.queue_update = Some(edits(1, vec![QueueEdit::Refresh]));
        let section = decode_queue_section(&queue_sections(&mut relay, [&snapshot]).unwrap());
        assert_eq!(
            section.ops,
            vec![TestQueueOp::Update(
                0,
                "Renamed Track".to_string(),
                "/music/a.flac".to_string()
            )]
        );
        // The relay holds the old library, so the new one cannot reuse it.
        assert_ne!(Arc::as_ptr(&snapshot.library).addr(), address);
    }

    #[test]
    fn snapshots_with_an_unchanged_waveform_send_no_analysis_frame() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        let mut analysing = snapshot_without_queue(0);
        analysing.analysis.waveform_complete = false;
        for _ in 0..3 {
            runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(analysing.clone()))]);
        }
        let frame = runtime.pop_analysis_frame().expect("a frame for the peaks");
        assert_ne!(frame[9] & ANALYSIS_FLAG_WAVEFORM, 0);
        assert!(runtime.pop_analysis_frame().is_none());

        // A complete waveform used to be sent with every snapshot, for the
        // UI to discard by its repeated sequence number.
        for second in 1..5 {
            runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(
                snapshot_without_queue(second),
            ))]);
        }
        let frame = runtime
            .pop_analysis_frame()
            .expect("a frame for the change");
        // Only the flag changed: the frame says so without the peaks.
        assert_eq!(
            frame[9] & (ANALYSIS_FLAG_WAVEFORM | ANALYSIS_FLAG_WAVEFORM_COMPLETE),
            ANALYSIS_FLAG_WAVEFORM_COMPLETE
        );
        assert!(runtime.pop_analysis_frame().is_none());
    }

    fn packets_encoded() -> usize {
        PACKETS_ENCODED.with(std::cell::Cell::get)
    }

    #[test]
    fn the_ui_pop_hands_over_finished_packets() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        // A queue waits for the UI when newer snapshots arrive, so each is
        // merged with it, along with errors and a save's answer.
        runtime.process_bridge_events([BridgeEvent::Snapshot(Box::new(snapshot_with_queue(
            &["/music/a.flac"],
            0,
        )))]);
        for second in 1..4 {
            runtime.process_bridge_events([
                BridgeEvent::Snapshot(Box::new(snapshot_without_queue(second))),
                BridgeEvent::Error(format!("error {second}")),
            ]);
        }
        runtime.process_bridge_events([BridgeEvent::StateSaved {
            request_id: 1,
            result: Ok(()),
        }]);

        let encoded = packets_encoded();
        let packets = pending_packets(&mut runtime);
        assert_eq!(packets_encoded(), encoded, "the pop built a packet");
        assert_eq!(packets.len(), 5);
        let snapshot = packets
            .iter()
            .find(|packet| section(packet, SECTION_PLAYBACK).is_some())
            .expect("the snapshot");
        let queue = section(snapshot, SECTION_QUEUE).expect("the queue section");
        assert_eq!(queue_section_paths(&queue), ["/music/a.flac"]);
    }

    #[test]
    fn the_relay_merges_snapshots_without_holding_the_ui_lock() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let shared = FfiShared::new(
            FfiRuntime::new(command_tx, -1, -1),
            Arc::new(AtomicU64::new(0)),
        );
        let mut encoder = FfiEncoder::default();
        prepare_and_publish(
            &shared,
            &mut encoder,
            [BridgeEvent::Snapshot(Box::new(snapshot_with_queue(
                &["/music/a.flac"],
                0,
            )))],
        );
        let before_merge = packets_encoded();
        prepare_and_publish(
            &shared,
            &mut encoder,
            [BridgeEvent::Snapshot(Box::new(snapshot_without_queue(1)))],
        );
        // The relay built the merged packet, on its own thread here.
        assert_eq!(packets_encoded(), before_merge + 1);

        let before_pop = packets_encoded();
        let packets = pending_packets(&mut shared.lock_runtime());
        assert_eq!(packets_encoded(), before_pop);
        assert_eq!(packets.len(), 1);
        let decoded = decode_binary_snapshot_for_test(&packets[0]);
        assert!((decoded.playback_position - 1.0).abs() < f64::EPSILON);
        let queue = section(&packets[0], SECTION_QUEUE).expect("the queue section");
        assert_eq!(queue_section_paths(&queue), ["/music/a.flac"]);
    }

    fn error_message(packet: &[u8]) -> Option<String> {
        section(packet, SECTION_ERROR).map(|payload| read_string(&payload, &mut 0))
    }

    #[test]
    fn errors_past_the_bound_give_way_to_a_count_of_earlier_ones() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        let total = MAX_PENDING_ERRORS + 5;
        for index in 0..total {
            runtime.process_bridge_events([BridgeEvent::Error(format!("error {index}"))]);
            // Save answers are never collapsed.
            runtime.process_bridge_events([BridgeEvent::StateSaved {
                request_id: u32::try_from(index).expect("small index"),
                result: Ok(()),
            }]);
        }
        // An error the UI's own command caused counts too.
        assert!(!runtime.send_command(Err("bad command".to_string())));

        let packets = pending_packets(&mut runtime);
        let errors: Vec<String> = packets.iter().filter_map(|p| error_message(p)).collect();
        let mut expected = vec!["6 earlier errors were not shown.".to_string()];
        expected.extend((6..total).map(|index| format!("error {index}")));
        expected.push("bad command".to_string());
        assert_eq!(errors, expected);
        let answers: Vec<u32> = packets
            .iter()
            .filter_map(|packet| section(packet, SECTION_STATE_SAVED))
            .map(|payload| read_u32(&payload, &mut 0))
            .collect();
        let expected_answers: Vec<u32> = (0..u32::try_from(total).expect("small total")).collect();
        assert_eq!(answers, expected_answers);

        // Once the UI has caught up, errors are shown one by one again.
        runtime.process_bridge_events([BridgeEvent::Error("later".to_string())]);
        let packets = pending_packets(&mut runtime);
        assert_eq!(packets.len(), 1);
        assert_eq!(error_message(&packets[0]).as_deref(), Some("later"));
    }

    #[test]
    fn a_single_earlier_error_is_counted_in_the_singular() {
        let (command_tx, _command_rx) = crossbeam_channel::unbounded();
        let mut runtime = FfiRuntime::new(command_tx, -1, -1);
        for index in 0..=MAX_PENDING_ERRORS {
            runtime.process_bridge_events([BridgeEvent::Error(format!("error {index}"))]);
        }
        let packets = pending_packets(&mut runtime);
        assert_eq!(packets.len(), MAX_PENDING_ERRORS + 1);
        assert_eq!(
            error_message(&packets[0]).as_deref(),
            Some("1 earlier error was not shown.")
        );
        assert_eq!(error_message(&packets[1]).as_deref(), Some("error 1"));
    }

    #[cfg(unix)]
    #[test]
    fn one_wakeup_per_drain() {
        let mut runtime = test_runtime_with_wake_pipe();
        for second in 0..20 {
            runtime.process_bridge_events([
                BridgeEvent::Snapshot(Box::new(snapshot_without_queue(second))),
                BridgeEvent::Error(format!("error {second}")),
            ]);
        }
        assert_eq!(drain_wakeup_fd(runtime.wake_read_fd), 1);

        // The UI drains, then acknowledges: nothing is left to wake it for.
        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));

        // A drain cut short by its budget is woken again for the rest.
        runtime.process_bridge_events([
            BridgeEvent::Error("first".to_string()),
            BridgeEvent::Error("second".to_string()),
        ]);
        assert!(runtime.pop_binary_event().is_some());
        runtime.ack_wakeup();
        assert!(wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));
        drain_runtime_queues(&mut runtime);
        runtime.ack_wakeup();
        assert!(!wake_fd_is_readable(
            runtime.wake_read_fd,
            Duration::from_millis(0)
        ));
        runtime.close_wakeup_pipe();
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

    /// Start a bridge with `options` behind the same handle the UI gets.
    fn ffi_bridge_with(options: BridgeRuntimeOptions) -> *mut FerrousFfiBridge {
        let handle = FrontendBridgeHandle::spawn_with_options(options).expect("spawn bridge");
        Box::into_raw(Box::new(connect_bridge(handle).expect("connect bridge")))
    }

    /// A bridge that saves its state, as the app's does, starting from no
    /// saved state. It saves in the test's config directory, which the test
    /// makes its own with [`crate::paths::TestConfigDir`] so that no other
    /// test's save reaches it.
    fn ffi_bridge_saving_state() -> *mut FerrousFfiBridge {
        for path in [config::settings_path(), config::session_path()]
            .into_iter()
            .flatten()
        {
            let _ = std::fs::remove_file(path);
        }
        ffi_bridge_with(BridgeRuntimeOptions {
            persisted_state_enabled: true,
            ..BridgeRuntimeOptions::default()
        })
    }

    fn ffi_set_db_range(handle: *mut FerrousFfiBridge, value: f32) {
        assert!(ffi_send_binary(
            handle,
            &encode_command(27, &value.to_le_bytes())
        ));
    }

    fn saved_settings() -> String {
        std::fs::read_to_string(config::settings_path().expect("settings path"))
            .expect("read the saved settings")
    }

    /// Files in the config directory that a save left behind.
    fn leftover_temporary_files() -> Vec<String> {
        let dir = config::config_base_path().expect("config dir");
        std::fs::read_dir(dir)
            .expect("read the config dir")
            .map(|entry| {
                entry
                    .expect("dir entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .filter(|name| std::path::Path::new(name).extension() == Some("tmp".as_ref()))
            .collect()
    }

    #[test]
    fn destroy_returns_after_saving_settings_changed_just_before() {
        let _config = crate::paths::TestConfigDir::new();
        let handle = ffi_bridge_saving_state();
        ffi_set_db_range(handle, 123.0);

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });

        assert!(
            saved_settings().contains("db_range=123.00"),
            "{}",
            saved_settings()
        );
        assert!(config::session_path().expect("session path").exists());
    }

    #[test]
    fn bridges_saving_settings_back_to_back_keep_the_last_ones() {
        let _config = crate::paths::TestConfigDir::new();
        for value in [111.0_f32, 122.0, 133.0] {
            let handle = ffi_bridge_saving_state();
            ffi_set_db_range(handle, value);
            // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
            assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });
        }

        assert!(
            saved_settings().contains("db_range=133.00"),
            "{}",
            saved_settings()
        );
        assert_eq!(leftover_temporary_files(), Vec::<String>::new());
    }

    /// A Last.fm service that waits for `release` before it confirms
    /// shutdown, as if it were saving its queue, and then sets `saved`.
    fn slow_lastfm(
        release: crossbeam_channel::Receiver<()>,
        saved: Arc<AtomicBool>,
    ) -> (
        crate::lastfm::Handle,
        crossbeam_channel::Receiver<crate::lastfm::Event>,
    ) {
        let (handle, commands) = crate::lastfm::test_support::command_handle();
        thread::spawn(move || {
            while let Ok(command) = commands.recv() {
                if let crate::lastfm::Command::Shutdown(done) = command {
                    let _ = release.recv();
                    saved.store(true, Ordering::SeqCst);
                    let _ = done.send(Ok(()));
                    return;
                }
            }
        });
        let (_event_tx, events) = crossbeam_channel::unbounded();
        (handle, events)
    }

    #[test]
    fn destroy_returns_after_lastfm_has_saved_its_queue() {
        let (release_tx, release_rx) = crossbeam_channel::bounded(1);
        let saved = Arc::new(AtomicBool::new(false));
        let handle = ffi_bridge_with(BridgeRuntimeOptions {
            lastfm: Some(slow_lastfm(release_rx, Arc::clone(&saved))),
            ..BridgeRuntimeOptions::default()
        });
        // The save finishes while destroy waits.
        release_tx.send(()).expect("release the save");

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });

        assert!(saved.load(Ordering::SeqCst));
    }

    #[test]
    fn destroy_gives_up_on_a_bridge_that_does_not_stop_in_time() {
        let (release_tx, release_rx) = crossbeam_channel::bounded(1);
        let saved = Arc::new(AtomicBool::new(false));
        let handle = ffi_bridge_with(BridgeRuntimeOptions {
            lastfm: Some(slow_lastfm(release_rx, Arc::clone(&saved))),
            ..BridgeRuntimeOptions::default()
        });
        // SAFETY: `handle` is live until it is destroyed below.
        unsafe { (*handle).shutdown_timeout = Duration::from_millis(50) };

        // The Last.fm save is held until destroy has returned.
        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(!unsafe { ferrous_ffi_bridge_destroy(handle) });

        assert!(!saved.load(Ordering::SeqCst));
        release_tx.send(()).expect("release the save");
    }

    #[test]
    fn destroy_saves_a_scrobble_queued_behind_a_lastfm_request_that_does_not_answer() {
        let dir = crate::paths::test_root().join("ffi-lastfm-blocked-request");
        let _ = std::fs::remove_dir_all(&dir);
        let queue_path = dir.join("lastfm_queue.json");
        let (transport, blocked) = crate::lastfm::test_support::BlockingTransport::new();
        let (lastfm, events) =
            crate::lastfm::test_support::spawn_connected(transport, queue_path.clone());
        lastfm.command(crate::lastfm::Command::QueueScrobble(scrobble(0)));
        blocked
            .started
            .recv_timeout(Duration::from_secs(5))
            .expect("the scrobble is sent");
        // Accepted while Last.fm waits for the request, so only in memory.
        lastfm.command(crate::lastfm::Command::QueueScrobble(scrobble(1)));
        let handle = ffi_bridge_with(BridgeRuntimeOptions {
            lastfm: Some((lastfm, events)),
            ..BridgeRuntimeOptions::default()
        });

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });

        let saved: Vec<crate::lastfm::ScrobbleEntry> =
            serde_json::from_slice(&std::fs::read(&queue_path).expect("read the queue"))
                .expect("parse the queue");
        assert_eq!(saved, vec![scrobble(0), scrobble(1)]);
        assert_eq!(blocked.answered(), 0, "the request is still running");
        blocked.release.send(()).expect("release the request");
    }

    #[test]
    fn destroy_reports_scrobbles_lastfm_could_only_keep_in_memory() {
        let dir = crate::paths::test_root().join("ffi-lastfm-unreadable-queue");
        let _ = std::fs::remove_dir_all(&dir);
        // A directory in place of the queue file cannot be read or replaced.
        let queue_path = dir.join("lastfm_queue.json");
        std::fs::create_dir_all(&queue_path).expect("create directory at queue path");
        let (lastfm, events) = crate::lastfm::test_support::spawn_connected(
            crate::lastfm::test_support::OfflineTransport,
            queue_path.clone(),
        );
        lastfm.command(crate::lastfm::Command::QueueScrobble(scrobble(0)));
        let handle = ffi_bridge_with(BridgeRuntimeOptions {
            lastfm: Some((lastfm, events)),
            ..BridgeRuntimeOptions::default()
        });

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(!unsafe { ferrous_ffi_bridge_destroy(handle) });

        assert!(queue_path.is_dir());
    }

    fn scrobble(index: i64) -> crate::lastfm::ScrobbleEntry {
        crate::lastfm::ScrobbleEntry {
            artist: "Artist".to_string(),
            track: format!("Track {index}"),
            album: String::new(),
            track_number: None,
            duration_seconds: Some(200),
            timestamp_utc: 1_700_000_000 + index,
        }
    }

    #[test]
    fn destroy_reports_a_lastfm_queue_that_was_not_confirmed_saved() {
        let (lastfm, commands) = crate::lastfm::test_support::command_handle();
        // Stops without confirming that the queue was saved.
        thread::spawn(move || {
            while let Ok(command) = commands.recv() {
                if let crate::lastfm::Command::Shutdown(done) = command {
                    drop(done);
                    return;
                }
            }
        });
        let (_event_tx, events) = crossbeam_channel::unbounded();
        let handle = ffi_bridge_with(BridgeRuntimeOptions {
            lastfm: Some((lastfm, events)),
            ..BridgeRuntimeOptions::default()
        });

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(!unsafe { ferrous_ffi_bridge_destroy(handle) });
    }

    fn ffi_send_save_state(handle: *mut FerrousFfiBridge, request_id: u32) {
        assert!(ffi_send_binary(
            handle,
            &encode_command(66, &request_id.to_le_bytes())
        ));
    }

    /// The next answer to a save request: its request ID, and `None` for
    /// success or what the user is shown.
    fn ffi_next_state_saved(handle: *mut FerrousFfiBridge) -> (u32, Option<String>) {
        let packet = ffi_wait_for_mask(handle, SECTION_STATE_SAVED, Duration::from_secs(5))
            .expect("the bridge answers");
        let (_, _, mask) = parse_packet_header(&packet);
        assert_eq!(mask, SECTION_STATE_SAVED);
        // The header and the section length, then the request ID, the
        // success flag, and the message.
        let request_id = u32::from_le_bytes(packet[16..20].try_into().expect("request ID"));
        let saved = packet[20] != 0;
        let len = u32::from_le_bytes(packet[21..25].try_into().expect("length")) as usize;
        let message = String::from_utf8(packet[25..25 + len].to_vec()).expect("UTF-8");
        assert_eq!(packet.len(), 25 + len);
        assert_eq!(saved, message.is_empty());
        (request_id, (!saved).then_some(message))
    }

    /// Ask the bridge to save its state, and return its answer: `None` for
    /// success, or what the user is shown.
    fn ffi_save_state(handle: *mut FerrousFfiBridge) -> Option<String> {
        ffi_send_save_state(handle, 7);
        let (request_id, message) = ffi_next_state_saved(handle);
        assert_eq!(request_id, 7);
        message
    }

    #[test]
    fn save_state_answers_whether_settings_and_queue_reached_the_disk() {
        let _config = crate::paths::TestConfigDir::new();
        let handle = ffi_bridge_saving_state();
        ffi_set_db_range(handle, 101.0);
        assert_eq!(ffi_save_state(handle), None);
        assert!(saved_settings().contains("db_range=101.00"));
        assert!(config::session_path().expect("session path").exists());

        // Nothing can be renamed over a directory.
        let settings_path = config::settings_path().expect("settings path");
        std::fs::remove_file(&settings_path).expect("remove the settings");
        std::fs::create_dir(&settings_path).expect("block the settings");
        ffi_set_db_range(handle, 102.0);
        let message = ffi_save_state(handle).expect("the save fails");
        assert!(
            message.starts_with("Settings could not be saved"),
            "{message}"
        );
        assert!(
            message.contains(&settings_path.display().to_string()),
            "{message}"
        );

        // Retrying once the cause is gone saves the latest settings.
        std::fs::remove_dir(&settings_path).expect("unblock the settings");
        assert_eq!(ffi_save_state(handle), None);
        assert!(saved_settings().contains("db_range=102.00"));

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });
    }

    #[test]
    fn save_state_answers_each_request_with_its_own_id() {
        let _config = crate::paths::TestConfigDir::new();
        let handle = ffi_bridge_saving_state();
        // A retry sent before the first answer arrived.
        ffi_send_save_state(handle, 1);
        ffi_send_save_state(handle, u32::MAX);
        assert_eq!(ffi_next_state_saved(handle), (1, None));
        assert_eq!(ffi_next_state_saved(handle), (u32::MAX, None));
        // A save request must say which attempt it is.
        assert!(parse_binary_command(&encode_command(66, &[])).is_err());

        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });
    }

    #[test]
    fn save_state_succeeds_when_state_is_not_persisted() {
        let handle = ffi_bridge_with(BridgeRuntimeOptions::default());
        assert_eq!(ffi_save_state(handle), None);
        // SAFETY: `handle` came from `ffi_bridge_with` and is not used again.
        assert!(unsafe { ferrous_ffi_bridge_destroy(handle) });
    }

    /// The reason in a packet that reports a stop the UI did not ask for.
    fn failure_message(packet: &[u8]) -> Option<String> {
        let (_, _, mask) = parse_packet_header(packet);
        if mask != SECTION_ERROR | SECTION_STOPPED {
            return None;
        }
        let mut offset = 12;
        let _section_len = read_u32(packet, &mut offset);
        Some(read_string(packet, &mut offset))
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
            let path_len = u32::from_le_bytes(s[p..p + 4].try_into().unwrap()) as usize;
            p += 4 + path_len; // skip path bytes
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
