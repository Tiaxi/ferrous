// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::path::{Path, PathBuf};
#[cfg(test)]
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, SystemTime};

use super::decoders::{open_audio_file, AudioFrameSource, AudioFrames, AudioRead};
use super::usize_to_u64;
use super::waveform_pyramid::PyramidTile;

pub(super) const TILE_FRAMES: u64 = 16_384;
const PCM_CACHE_BYTES: usize = 32 * 1024 * 1024;
const SUMMARY_CACHE_BYTES: usize = 16 * 1024 * 1024;

// Calls originate on the Qt worker pool: the level meter and the waveform
// detail view share one active-track decoder and cache across those threads
// instead of depending on thread-local affinity. Each holds the decoder for
// one tile at a time, so neither waits for the other's whole window.
static SERVICE: Mutex<ServiceSlot> = Mutex::new(ServiceSlot {
    session: None,
    opening: false,
});
/// Signalled when an open of a session ends.
static OPENED: Condvar = Condvar::new();
/// How often a request waiting for another's open looks at its cancellation.
const CANCELLATION_CHECK_INTERVAL: Duration = Duration::from_millis(5);

struct ServiceSlot {
    session: Option<Arc<WaveformSession>>,
    /// Whether a request is opening a session. One file is opened at a
    /// time, so requests for the same file share one decoder.
    opening: bool,
}

fn lock_service() -> MutexGuard<'static, ServiceSlot> {
    // The slot is consistent between statements, so a panic elsewhere
    // cannot leave it half-changed.
    SERVICE.lock().unwrap_or_else(PoisonError::into_inner)
}

#[derive(PartialEq, Eq)]
struct SourceIdentity {
    path: PathBuf,
    length: u64,
    modified: SystemTime,
}

impl SourceIdentity {
    fn read(path: &Path) -> anyhow::Result<Self> {
        let metadata = path.metadata()?;
        Ok(Self {
            path: path.to_path_buf(),
            length: metadata.len(),
            modified: metadata.modified()?,
        })
    }
}

pub(super) struct PcmTile {
    // Keep timestamps and gaps; padding a missing packet with zero would
    // change extrema in bins containing only positive or negative samples.
    pub(super) segments: Vec<AudioFrames>,
}

impl PcmTile {
    fn bytes(&self) -> usize {
        self.segments.capacity() * std::mem::size_of::<AudioFrames>()
            + self
                .segments
                .iter()
                .map(|frames| frames.samples.capacity() * std::mem::size_of::<f32>())
                .sum::<usize>()
    }
}

struct TileCache<T> {
    tiles: HashMap<u64, (Arc<T>, usize, u64)>,
    bytes: usize,
    budget: usize,
    access: u64,
}

impl<T> TileCache<T> {
    fn new(budget: usize) -> Self {
        Self {
            tiles: HashMap::new(),
            bytes: 0,
            budget,
            access: 0,
        }
    }

    fn get(&mut self, index: u64) -> Option<Arc<T>> {
        self.access = self.access.saturating_add(1);
        self.tiles.get_mut(&index).map(|(tile, _, accessed)| {
            *accessed = self.access;
            Arc::clone(tile)
        })
    }

    fn insert(&mut self, index: u64, tile: Arc<T>, bytes: usize) {
        if bytes > self.budget {
            return;
        }
        while self.bytes + bytes > self.budget {
            let Some(oldest) = self
                .tiles
                .iter()
                .min_by_key(|(_, (_, _, access))| access)
                .map(|(&index, _)| index)
            else {
                break;
            };
            if let Some((_, removed_bytes, _)) = self.tiles.remove(&oldest) {
                self.bytes -= removed_bytes;
            }
        }
        if let Some((_, previous_bytes, _)) = self.tiles.insert(index, (tile, bytes, self.access)) {
            self.bytes -= previous_bytes;
        }
        self.bytes += bytes;
    }
}

/// What the decoder reads next. Held while one tile decodes.
struct Decoder {
    source: AudioFrameSource,
    next_frame: u64,
    pending: Option<AudioFrames>,
}

/// Decoded tiles, shared by every request. Held only to look up or store.
struct Caches {
    pcm: TileCache<PcmTile>,
    summaries: TileCache<PyramidTile>,
    eof_frame: Option<u64>,
}

pub(super) struct WaveformSession {
    identity: SourceIdentity,
    pub(super) sample_rate: u32,
    pub(super) channels: usize,
    /// Locked after `decoder` when both are held.
    caches: Mutex<Caches>,
    decoder: Mutex<Decoder>,
    /// Set when a decode panicked, leaving the decoder's position unknown;
    /// the next request opens the file again.
    broken: AtomicBool,
    #[cfg(test)]
    decoded_tiles: AtomicUsize,
}

impl WaveformSession {
    #[cfg(test)]
    pub(super) fn open(path: &Path) -> anyhow::Result<Self> {
        Self::open_identified(SourceIdentity::read(path)?)
    }

    fn open_identified(identity: SourceIdentity) -> anyhow::Result<Self> {
        let (source, rate, channels, _) = open_audio_file(&identity.path)
            .ok_or_else(|| anyhow::anyhow!("unsupported audio file"))?;
        Ok(Self {
            identity,
            sample_rate: u32::try_from(rate).unwrap_or(u32::MAX).max(1),
            channels: channels.clamp(1, usize::from(u16::MAX)),
            caches: Mutex::new(Caches {
                pcm: TileCache::new(PCM_CACHE_BYTES),
                summaries: TileCache::new(SUMMARY_CACHE_BYTES),
                eof_frame: None,
            }),
            decoder: Mutex::new(Decoder {
                source,
                next_frame: 0,
                pending: None,
            }),
            broken: AtomicBool::new(false),
            #[cfg(test)]
            decoded_tiles: AtomicUsize::new(0),
        })
    }

    fn caches(&self) -> MutexGuard<'_, Caches> {
        // Cache updates are single statements; a panic between them leaves
        // at worst a byte count that evicts early.
        self.caches.lock().unwrap_or_else(PoisonError::into_inner)
    }

    pub(super) fn tile(
        &self,
        index: u64,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<Arc<PcmTile>> {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        if let Some(tile) = self.caches().pcm.get(index) {
            return Ok(tile);
        }
        let tile = {
            let Ok(mut decoder) = self.decoder.lock() else {
                self.broken.store(true, Ordering::Relaxed);
                anyhow::bail!("waveform decoder failed");
            };
            // Another request may have decoded it while this one waited,
            // or this one may have been cancelled meanwhile.
            if let Some(tile) = self.caches().pcm.get(index) {
                return Ok(tile);
            }
            anyhow::ensure!(!cancelled(), "waveform decode cancelled");
            let eof_frame = self.caches().eof_frame;
            #[cfg(test)]
            if eof_frame.is_none_or(|eof| index.saturating_mul(TILE_FRAMES) < eof) {
                self.decoded_tiles.fetch_add(1, Ordering::Relaxed);
            }
            let (tile, eof_frame) =
                decoder.decode_tile(index, eof_frame, self.sample_rate, cancelled)?;
            let tile = Arc::new(tile);
            let mut caches = self.caches();
            caches.eof_frame = caches.eof_frame.or(eof_frame);
            if !tile.segments.is_empty() {
                caches.pcm.insert(index, Arc::clone(&tile), tile.bytes());
            }
            tile
        };
        #[cfg(test)]
        test_hooks::tile_decoded();
        Ok(tile)
    }

    pub(super) fn summary(
        &self,
        index: u64,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<Arc<PyramidTile>> {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        if let Some(tile) = self.caches().summaries.get(index) {
            return Ok(tile);
        }
        let pcm = self.tile(index, cancelled)?;
        let summary = Arc::new(PyramidTile::new(&pcm, index, self.channels));
        if !pcm.segments.is_empty() {
            self.caches()
                .summaries
                .insert(index, Arc::clone(&summary), summary.bytes());
        }
        Ok(summary)
    }

    #[cfg(test)]
    pub(super) fn discard_pcm(&self) {
        self.caches().pcm = TileCache::new(0);
    }

    #[cfg(test)]
    pub(super) fn decoded_tiles(&self) -> usize {
        self.decoded_tiles.load(Ordering::Relaxed)
    }

    pub(super) fn reached_eof(&self, frame: u64) -> bool {
        self.caches().eof_frame.is_some_and(|eof| frame >= eof)
    }
}

impl Decoder {
    /// Decode tile `index`, and return it with the end of the file if the
    /// decode reached it.
    fn decode_tile(
        &mut self,
        index: u64,
        eof_frame: Option<u64>,
        sample_rate: u32,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<(PcmTile, Option<u64>)> {
        let start = index.saturating_mul(TILE_FRAMES);
        let end = start.saturating_add(TILE_FRAMES);
        let mut tile = PcmTile {
            segments: Vec::new(),
        };
        if eof_frame.is_some_and(|eof| start >= eof) {
            return Ok((tile, None));
        }
        if self.next_frame != start {
            // A failed seek leaves the position unknown; seek again next time.
            self.next_frame = u64::MAX;
            self.pending = None;
            self.source.seek_to_frame(start, sample_rate)?;
            self.next_frame = start;
        }
        let mut reached_eof = None;
        while self.next_frame < end {
            anyhow::ensure!(!cancelled(), "waveform decode cancelled");
            let frames = if let Some(frames) = self.pending.take() {
                frames
            } else {
                match self.source.next_frames()? {
                    AudioRead::Frames(frames) => frames,
                    AudioRead::Pending => continue,
                    AudioRead::Eof => {
                        reached_eof = Some(self.next_frame);
                        break;
                    }
                }
            };
            let packet_end = frames
                .first_frame
                .saturating_add(usize_to_u64(frames.frames));
            let first = frames.first_frame.max(start);
            let last = packet_end.min(end);
            if first < last {
                let offset = usize::try_from(first - frames.first_frame)? * frames.channels;
                let count = usize::try_from(last - first)?;
                tile.segments.push(AudioFrames {
                    samples: frames.samples[offset..offset + count * frames.channels].to_vec(),
                    first_frame: first,
                    frames: count,
                    channels: frames.channels,
                });
            }
            self.next_frame = packet_end.min(end).max(self.next_frame);
            if packet_end > end {
                self.pending = Some(frames);
            }
        }
        Ok((tile, reached_eof))
    }
}

/// Clears the opening flag, and wakes the requests waiting for it, when an
/// open ends, by a panic included.
struct OpeningGuard;

impl Drop for OpeningGuard {
    fn drop(&mut self) {
        lock_service().opening = false;
        OPENED.notify_all();
    }
}

/// The shared session for the file at `path`, opened if it is not open.
fn shared_session(
    path: &Path,
    cancelled: &impl Fn() -> bool,
) -> anyhow::Result<Arc<WaveformSession>> {
    anyhow::ensure!(!cancelled(), "waveform decode cancelled");
    let identity = SourceIdentity::read(path)?;
    let mut service = lock_service();
    loop {
        // Before every wait and before opening: a request cancelled while
        // it waited, by a skip to another track, must not open its file.
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        if let Some(session) = service
            .session
            .as_ref()
            .filter(|session| session.identity == identity)
            .filter(|session| !session.broken.load(Ordering::Relaxed))
        {
            return Ok(Arc::clone(session));
        }
        if !service.opening {
            break;
        }
        // The decoder is needed before anything can be read, and the
        // request opening one may be opening this file. Qt cancels a
        // request by setting a flag, which wakes nothing, so the wait ends
        // now and then to look at it; the end of the open wakes it at once.
        service = OPENED
            .wait_timeout(service, CANCELLATION_CHECK_INTERVAL)
            .map_or_else(|poisoned| poisoned.into_inner().0, |(guard, _)| guard);
    }
    // Release the previous decoder before opening another; requests still
    // reading it keep it until they finish their tile.
    service.session = None;
    service.opening = true;
    drop(service);
    let opening = OpeningGuard;
    #[cfg(test)]
    test_hooks::opening();
    let session = Arc::new(WaveformSession::open_identified(identity)?);
    lock_service().session = Some(Arc::clone(&session));
    drop(opening);
    Ok(session)
}

pub(super) fn with_session<T>(
    path: &Path,
    cancelled: &impl Fn() -> bool,
    operation: impl FnOnce(&WaveformSession) -> anyhow::Result<T>,
) -> anyhow::Result<T> {
    let session = shared_session(path, cancelled)?;
    operation(&session)
}

/// Pause a decode between tiles, or a request as it opens its file.
#[cfg(test)]
pub(super) mod test_hooks {
    use std::cell::RefCell;

    thread_local! {
        static AFTER_TILE: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
        static BEFORE_OPEN: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
    }

    /// Run `hook` when this thread starts opening a file for the service.
    pub(in super::super) fn before_each_open(hook: impl FnMut() + 'static) {
        BEFORE_OPEN.with(|cell| *cell.borrow_mut() = Some(Box::new(hook)));
    }

    pub(super) fn opening() {
        BEFORE_OPEN.with(|cell| {
            if let Some(hook) = cell.borrow_mut().as_mut() {
                hook();
            }
        });
    }

    /// Run `hook` after each tile this thread decodes.
    pub(in super::super) fn after_each_tile(hook: impl FnMut() + 'static) {
        AFTER_TILE.with(|cell| *cell.borrow_mut() = Some(Box::new(hook)));
    }

    pub(super) fn tile_decoded() {
        AFTER_TILE.with(|cell| {
            if let Some(hook) = cell.borrow_mut().as_mut() {
                hook();
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::super::waveform_window::decode_waveform_window;
    use super::super::waveform_window::tests::write_test_wave;
    use super::*;

    #[test]
    fn the_level_meter_is_not_blocked_by_a_detail_decode_in_progress() {
        // Twenty seconds: the detail view decodes all of it, tile by tile.
        let path = write_test_wave(&vec![4_096; 960_011], 48_000, 1);
        let (paused_tx, paused_rx) = crossbeam_channel::bounded(1);
        let (resume_tx, resume_rx) = crossbeam_channel::bounded::<()>(1);
        let detail_path = path.clone();
        let detail = std::thread::spawn(move || {
            let mut first = true;
            test_hooks::after_each_tile(move || {
                if std::mem::take(&mut first) {
                    let _ = paused_tx.send(());
                    let _ = resume_rx.recv();
                }
            });
            decode_waveform_window(&detail_path, 0.0, 20.0, 64)
        });
        paused_rx
            .recv_timeout(Duration::from_secs(10))
            .expect("the detail decode started");

        // The meter reads two seconds near the playhead meanwhile.
        let (meter_tx, meter_rx) = crossbeam_channel::bounded(1);
        let meter_path = path.clone();
        let meter = std::thread::spawn(move || {
            let _ = meter_tx.send(decode_waveform_window(&meter_path, 15.0, 17.0, 2_048));
        });
        let window = meter_rx.recv_timeout(Duration::from_secs(10));
        drop(resume_tx);
        meter.join().expect("meter");
        let detail = detail.join().expect("detail");
        let window = window
            .expect("the meter waited for the detail decode")
            .expect("meter window");
        assert!(window.extrema.iter().any(|&value| value > 0.1));
        assert!(detail
            .expect("detail window")
            .extrema
            .iter()
            .any(|&value| value > 0.1));
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn pcm_cache_reuses_tiles_and_evicts_without_changing_samples() {
        let samples: Vec<i16> = (0..70_007)
            .map(|i| i16::try_from(i % 20_000).unwrap())
            .collect();
        let path = write_test_wave(&samples, 48_000, 1);
        let session = WaveformSession::open(&path).expect("open fixture");
        let first = session.tile(0, &|| false).expect("first tile");
        let again = session.tile(0, &|| false).expect("cached tile");
        assert!(Arc::ptr_eq(&first, &again));
        assert_eq!(session.decoded_tiles(), 1);
        session.caches().pcm.budget = first.bytes() * 2;
        for index in [1, 2, 3, 0] {
            let tile = session.tile(index, &|| false).expect("read tile");
            for segment in &tile.segments {
                let offset = usize::try_from(segment.first_frame).unwrap();
                for (actual, &expected) in segment.samples.iter().zip(&samples[offset..]) {
                    assert!((actual - f32::from(expected) / 32768.0).abs() < 0.000_001);
                }
            }
            let caches = session.caches();
            assert!(caches.pcm.bytes <= caches.pcm.budget);
        }
        assert_eq!(
            session.decoded_tiles(),
            5,
            "evicted prefix must decode again"
        );
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn summary_cache_obeys_its_own_byte_budget() {
        let path = write_test_wave(&vec![8_192; 70_009], 48_000, 1);
        let session = WaveformSession::open(&path).expect("open fixture");
        let first = session.summary(0, &|| false).expect("summary");
        session.caches().summaries.budget = first.bytes();
        for index in [1, 2, 3, 0] {
            let summary = session
                .summary(index, &|| false)
                .expect("summary after eviction");
            assert_eq!(summary.rows(TILE_FRAMES).0, &[[0.25, 0.25], [0.25, 0.25]]);
            let caches = session.caches();
            assert!(caches.summaries.bytes <= caches.summaries.budget);
            assert_eq!(caches.summaries.tiles.len(), 1);
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn cancelled_partial_tile_is_not_cached_and_retry_is_complete() {
        let path = write_test_wave(&vec![1234; 33_001], 48_000, 1);
        let session = WaveformSession::open(&path).expect("open fixture");
        let calls = std::cell::Cell::new(0);
        let result = session.tile(0, &|| {
            calls.set(calls.get() + 1);
            calls.get() >= 3
        });
        assert!(result.is_err());
        assert!(session.caches().pcm.tiles.is_empty());
        let tile = session.tile(0, &|| false).expect("retry cancelled tile");
        assert_eq!(
            tile.segments
                .iter()
                .map(|segment| segment.frames)
                .sum::<usize>(),
            16_384
        );
        assert_eq!(tile.segments.first().unwrap().first_frame, 0);
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_request_cancelled_while_another_opens_a_file_does_not_open_its_own() {
        let a = write_test_wave(&vec![2_048; 48_013], 48_000, 1);
        let b = write_test_wave(&vec![2_048; 48_017], 48_000, 1);
        // Holds the first request where it opens A, as slow storage would.
        let (opening_tx, opening_rx) = crossbeam_channel::bounded(1);
        let (release_tx, release_rx) = crossbeam_channel::bounded::<()>(1);
        let opener_path = a.clone();
        let opener = std::thread::spawn(move || {
            test_hooks::before_each_open(move || {
                let _ = opening_tx.send(());
                let _ = release_rx.recv();
            });
            shared_session(&opener_path, &|| false).map(|_| ())
        });
        opening_rx
            .recv_timeout(Duration::from_secs(10))
            .expect("the first request opens A");

        // A meter request for B waits for that open; it is cancelled while
        // it waits.
        let cancel = Arc::new(AtomicBool::new(false));
        let (checked_tx, checked_rx) = crossbeam_channel::unbounded();
        let (done_tx, done_rx) = crossbeam_channel::bounded(1);
        let waiter_cancel = Arc::clone(&cancel);
        let waiter_path = b.clone();
        let waiter = std::thread::spawn(move || {
            test_hooks::before_each_open(|| panic!("the cancelled request opened B"));
            let cancelled = || {
                let _ = checked_tx.send(());
                waiter_cancel.load(Ordering::Relaxed)
            };
            let result = shared_session(&waiter_path, &cancelled).map(|_| ());
            let _ = done_tx.send(());
            result
        });
        // Checked on entry, then again after waiting once.
        for _ in 0..2 {
            checked_rx
                .recv_timeout(Duration::from_secs(10))
                .expect("the waiter checks its cancellation");
        }
        cancel.store(true, Ordering::Relaxed);
        let returned = done_rx.recv_timeout(Duration::from_secs(10));
        drop(release_tx);
        let waited = waiter.join().expect("waiter");
        opener.join().expect("opener").expect("open A");
        assert!(returned.is_ok(), "the cancelled request kept waiting");
        let error = waited.expect_err("cancelled request");
        assert!(error.to_string().contains("cancelled"), "{error}");
        for path in [a, b] {
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    #[test]
    fn a_cancelled_request_does_not_wait_for_the_service() {
        // As while another request opens a file.
        let guard = lock_service();
        let error = std::thread::spawn(|| {
            with_session(Path::new("/missing/cancelled-busy.wav"), &|| true, |_| {
                Ok(())
            })
        })
        .join()
        .expect("worker")
        .expect_err("cancelled request");
        assert!(error.to_string().contains("cancelled"));
        drop(guard);
    }
}
