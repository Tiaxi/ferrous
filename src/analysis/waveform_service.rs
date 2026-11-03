// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
#[cfg(test)]
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, SystemTime};

use super::decoders::{open_audio_file, AudioFrameSource, AudioFrames, AudioRead};
use super::usize_to_u64;
use super::waveform_prefill::CoarsePrefill;
use super::waveform_pyramid::{PyramidTile, COARSE_SUMMARY_FRAMES};
use crate::file_identity::FileId;

pub(super) const TILE_FRAMES: u64 = 16_384;
const PCM_CACHE_BYTES: usize = 32 * 1024 * 1024;
const SUMMARY_CACHE_BYTES: usize = 16 * 1024 * 1024;
/// Coarse summaries, kept for the session outside the LRU: a whole-track or
/// zoomed-out window reads every tile of the track, and the fine levels of a
/// hi-res surround track exceed the LRU's budget. This holds ~26 minutes of
/// 96 kHz 5.1 audio, ~13 of 192 kHz 5.1, or about two hours of 44.1 kHz
/// stereo.
pub(super) const COARSE_SUMMARY_BYTES: usize = 16 * 1024 * 1024;

// Calls originate on the Qt worker pool: the level meter and the waveform
// detail view share one active-track decoder and cache across those threads
// instead of depending on thread-local affinity. Each holds the decoder for
// one tile at a time, so neither waits for the other's whole window.
static SERVICE: Mutex<ServiceSlot> = Mutex::new(ServiceSlot {
    session: None,
    opening: false,
    prefill: None,
});
/// Signalled when an open of a session ends.
static OPENED: Condvar = Condvar::new();
/// How often a request waiting for another's open looks at its cancellation.
const CANCELLATION_CHECK_INTERVAL: Duration = Duration::from_millis(5);

pub(super) struct ServiceSlot {
    session: Option<Arc<WaveformSession>>,
    /// Whether a request is opening a session. One file is opened at a
    /// time, so requests for the same file share one decoder.
    opening: bool,
    /// Coarse summaries the overview decode built for a file the service
    /// had no session for, which the session that opens it takes.
    prefill: Option<CoarsePrefill>,
}

fn lock_service() -> MutexGuard<'static, ServiceSlot> {
    // The slot is consistent between statements, so a panic elsewhere
    // cannot leave it half-changed.
    SERVICE.lock().unwrap_or_else(PoisonError::into_inner)
}

/// The file at a path, as the service tells one from another: by its path,
/// length and modification time, and by the file itself and the time of
/// its last change of any kind, which tell a replacement of the same length
/// and modification time, such as a save of other tags, from the file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct SourceIdentity {
    path: PathBuf,
    length: u64,
    modified: SystemTime,
    file: FileId,
    changed: (i64, i64),
}

impl SourceIdentity {
    fn read(path: &Path) -> anyhow::Result<Self> {
        Ok(Self::of(path, &path.metadata()?)?)
    }

    /// A made-up identity of a file at `path`.
    #[cfg(test)]
    pub(super) fn made_up(path: &str) -> Self {
        Self {
            path: PathBuf::from(path),
            length: 0,
            modified: SystemTime::UNIX_EPOCH,
            file: FileId::from_numbers(0, 0),
            changed: (0, 0),
        }
    }

    /// The identity of the file at `path` whose metadata is `metadata`, as
    /// the handle that reads it tells.
    pub(super) fn of(path: &Path, metadata: &std::fs::Metadata) -> std::io::Result<Self> {
        Ok(Self {
            path: path.to_path_buf(),
            length: metadata.len(),
            modified: metadata.modified()?,
            file: FileId::of(metadata),
            changed: (metadata.ctime(), metadata.ctime_nsec()),
        })
    }
}

pub(super) struct PcmTile {
    // Keep timestamps and gaps; padding a missing packet with zero would
    // change extrema in bins containing only positive or negative samples.
    pub(super) segments: Vec<AudioFrames>,
}

impl PcmTile {
    /// Each segment's first frame, frames, channels and samples, the
    /// samples bit for bit.
    #[cfg(test)]
    pub(super) fn bits(&self) -> Vec<(u64, usize, usize, Vec<u32>)> {
        self.segments
            .iter()
            .map(|segment| {
                let samples = segment
                    .samples
                    .iter()
                    .map(|value| value.to_bits())
                    .collect();
                (
                    segment.first_frame,
                    segment.frames,
                    segment.channels,
                    samples,
                )
            })
            .collect()
    }

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

/// Tiles kept until the session ends, up to a byte budget; once it is full,
/// no more are added. A least-recently-used policy would evict every tile of
/// a whole-track pass that does not fit before the next pass reads it.
struct RetainedTiles<T> {
    tiles: HashMap<u64, Arc<T>>,
    bytes: usize,
    budget: usize,
}

impl<T> RetainedTiles<T> {
    fn new(budget: usize) -> Self {
        Self {
            tiles: HashMap::new(),
            bytes: 0,
            budget,
        }
    }

    fn get(&self, index: u64) -> Option<Arc<T>> {
        self.tiles.get(&index).map(Arc::clone)
    }

    fn insert(&mut self, index: u64, tile: Arc<T>, bytes: usize) {
        let bytes = retained_bytes::<T>(bytes);
        if self.tiles.contains_key(&index) || self.bytes + bytes > self.budget {
            return;
        }
        self.tiles.insert(index, tile);
        self.bytes += bytes;
    }
}

/// What a tile of `bytes` takes in a [`RetainedTiles`]: the map's entry,
/// the `Arc`'s counts and the tile itself.
pub(super) fn retained_bytes<T>(bytes: usize) -> usize {
    bytes + std::mem::size_of::<(u64, Arc<T>)>() + 2 * std::mem::size_of::<usize>()
}

/// What the decoder reads next. Held while one tile decodes.
struct Decoder<S = AudioFrameSource> {
    source: S,
    next_frame: u64,
    pending: Option<AudioFrames>,
}

/// The decoder of a file, as the service reads it.
trait FrameSource {
    fn next_frames(&mut self) -> anyhow::Result<AudioRead>;
    fn seek_to_frame(&mut self, frame: u64, sample_rate: u32) -> anyhow::Result<()>;
}

impl FrameSource for AudioFrameSource {
    fn next_frames(&mut self) -> anyhow::Result<AudioRead> {
        AudioFrameSource::next_frames(self)
    }

    fn seek_to_frame(&mut self, frame: u64, sample_rate: u32) -> anyhow::Result<()> {
        AudioFrameSource::seek_to_frame(self, frame, sample_rate)
    }
}

/// Decoded tiles, shared by every request. Held only to look up or store.
struct Caches {
    pcm: TileCache<PcmTile>,
    /// Summary levels finer than `COARSE_SUMMARY_FRAMES`.
    summaries: TileCache<PyramidTile>,
    coarse_summaries: RetainedTiles<PyramidTile>,
    eof_frame: Option<u64>,
    /// The tile holding the last frame of the file. A whole-track window
    /// whose end falls short of the end of the file reads it as PCM, so it
    /// is kept outside the LRU, which zoomed windows cycle through.
    last_tile: Option<(u64, Arc<PcmTile>)>,
}

impl Caches {
    fn pcm(&mut self, index: u64) -> Option<Arc<PcmTile>> {
        let tile = self.pcm.get(index).or_else(|| {
            self.last_tile
                .as_ref()
                .filter(|(last, _)| *last == index)
                .map(|(_, tile)| Arc::clone(tile))
        })?;
        self.keep_if_last(index, &tile);
        Some(tile)
    }

    fn keep_if_last(&mut self, index: u64, tile: &Arc<PcmTile>) {
        let last = self
            .eof_frame
            .and_then(|eof| eof.checked_sub(1))
            .map(|frame| frame / TILE_FRAMES);
        if last == Some(index) && !tile.segments.is_empty() {
            self.last_tile = Some((index, Arc::clone(tile)));
        }
    }
}

pub(super) struct WaveformSession {
    identity: SourceIdentity,
    pub(super) sample_rate: u32,
    pub(super) channels: usize,
    /// Whether `GStreamer` decodes the file, rather than Symphonia.
    gstreamer: bool,
    /// Whether the file the decoder reads is known to be that of
    /// `identity`; only then does the session take the overview's
    /// summaries.
    confirmed: bool,
    /// Locked after `decoder` when both are held.
    caches: Mutex<Caches>,
    decoder: Mutex<Decoder>,
    /// Set when a decode panicked, leaving the decoder's position unknown;
    /// the next request opens the file again.
    broken: AtomicBool,
    #[cfg(test)]
    decoded_tiles: AtomicUsize,
    /// Whether the session took summaries from the overview decode.
    #[cfg(test)]
    adopted: AtomicBool,
}

impl WaveformSession {
    /// Open the file at `path`. Its identity is that of the file a handle
    /// opened here reads. The decoders open the path themselves, so the
    /// identity is confirmed only if the path still holds that file once
    /// they have opened it.
    pub(super) fn open(path: &Path) -> anyhow::Result<Self> {
        let identity = SourceIdentity::of(path, &std::fs::File::open(path)?.metadata()?)?;
        #[cfg(test)]
        test_hooks::identified();
        let (mut source, rate, channels, _) =
            open_audio_file(path).ok_or_else(|| anyhow::anyhow!("unsupported audio file"))?;
        let pending = first_frames(&mut source)?;
        let confirmed = SourceIdentity::read(path).is_ok_and(|now| now == identity);
        Ok(Self {
            identity,
            sample_rate: session_sample_rate(rate),
            channels: session_channels(channels),
            gstreamer: source.decodes_on_its_own_thread(),
            confirmed,
            caches: Mutex::new(Caches {
                pcm: TileCache::new(PCM_CACHE_BYTES),
                summaries: TileCache::new(SUMMARY_CACHE_BYTES),
                coarse_summaries: RetainedTiles::new(COARSE_SUMMARY_BYTES),
                eof_frame: None,
                last_tile: None,
            }),
            decoder: Mutex::new(Decoder {
                source,
                next_frame: 0,
                pending,
            }),
            broken: AtomicBool::new(false),
            #[cfg(test)]
            decoded_tiles: AtomicUsize::new(0),
            #[cfg(test)]
            adopted: AtomicBool::new(false),
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
        if let Some(tile) = self.caches().pcm(index) {
            return Ok(tile);
        }
        let tile = {
            let Ok(mut decoder) = self.decoder.lock() else {
                self.broken.store(true, Ordering::Relaxed);
                anyhow::bail!("waveform decoder failed");
            };
            // Another request may have decoded it while this one waited,
            // or this one may have been cancelled meanwhile.
            if let Some(tile) = self.caches().pcm(index) {
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
            caches.keep_if_last(index, &tile);
            tile
        };
        #[cfg(test)]
        test_hooks::tile_decoded();
        Ok(tile)
    }

    /// The summary levels of tile `index` for `frames_per_point`: the
    /// coarse ones from `COARSE_SUMMARY_FRAMES` on, otherwise the fine ones.
    pub(super) fn summary(
        &self,
        index: u64,
        frames_per_point: u64,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<Arc<PyramidTile>> {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        let coarse = frames_per_point >= COARSE_SUMMARY_FRAMES;
        let hit = {
            let mut caches = self.caches();
            if coarse {
                caches.coarse_summaries.get(index).or_else(|| {
                    // The store may be full. Fine rows aggregate exactly
                    // into coarse ones, so the LRU spares a decode.
                    let fine = caches.summaries.get(index)?;
                    let coarse = Arc::new(PyramidTile::coarse_from(&fine));
                    caches
                        .coarse_summaries
                        .insert(index, Arc::clone(&coarse), coarse.bytes());
                    Some(coarse)
                })
            } else {
                caches.summaries.get(index)
            }
        };
        if let Some(tile) = hit {
            return Ok(tile);
        }
        let pcm = self.tile(index, cancelled)?;
        let (fine, coarse_levels) = PyramidTile::build(&pcm, index, self.channels);
        let (fine, coarse_levels) = (Arc::new(fine), Arc::new(coarse_levels));
        if !pcm.segments.is_empty() {
            let mut caches = self.caches();
            caches
                .summaries
                .insert(index, Arc::clone(&fine), fine.bytes());
            caches.coarse_summaries.insert(
                index,
                Arc::clone(&coarse_levels),
                coarse_levels.bytes(),
            );
        }
        Ok(if coarse { coarse_levels } else { fine })
    }

    /// Whether every frame of tile `index` lies before `end_frame`: the
    /// tile ends by then, or the file does.
    pub(super) fn tile_ends_by(&self, index: u64, end_frame: u64) -> bool {
        index.saturating_add(1).saturating_mul(TILE_FRAMES) <= end_frame
            || self.caches().eof_frame.is_some_and(|eof| eof <= end_frame)
    }

    #[cfg(test)]
    pub(super) fn discard_pcm(&self) {
        let mut caches = self.caches();
        caches.pcm = TileCache::new(0);
        caches.last_tile = None;
    }

    #[cfg(test)]
    pub(super) fn limit_fine_caches(&self, pcm_bytes: usize, summary_bytes: usize) {
        let mut caches = self.caches();
        caches.pcm.budget = pcm_bytes;
        caches.summaries.budget = summary_bytes;
    }

    #[cfg(test)]
    pub(super) fn limit_coarse_summaries(&self, bytes: usize) {
        self.caches().coarse_summaries.budget = bytes;
    }

    /// The bytes the coarse summaries hold, and their budget.
    #[cfg(test)]
    pub(super) fn coarse_summary_bytes(&self) -> (usize, usize) {
        let caches = self.caches();
        (
            caches.coarse_summaries.bytes,
            caches.coarse_summaries.budget,
        )
    }

    #[cfg(test)]
    pub(super) fn decoded_tiles(&self) -> usize {
        self.decoded_tiles.load(Ordering::Relaxed)
    }

    /// Whether the session took summaries from the overview decode.
    #[cfg(test)]
    pub(super) fn adopted(&self) -> bool {
        self.adopted.load(Ordering::Relaxed)
    }

    #[cfg(test)]
    pub(super) fn eof_frame(&self) -> Option<u64> {
        self.caches().eof_frame
    }

    #[cfg(test)]
    pub(super) fn identity(&self) -> &SourceIdentity {
        &self.identity
    }

    /// How many tiles the coarse summaries hold.
    #[cfg(test)]
    pub(super) fn coarse_tiles(&self) -> usize {
        self.caches().coarse_summaries.tiles.len()
    }

    pub(super) fn reached_eof(&self, frame: u64) -> bool {
        self.caches().eof_frame.is_some_and(|eof| frame >= eof)
    }
}

impl<S: FrameSource> Decoder<S> {
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
            let packet = TilePacket::of(
                frames.first_frame,
                frames.frames,
                frames.channels,
                start,
                end,
            )?;
            if let Some((first, count, samples)) = packet.samples(&frames.samples) {
                tile.segments.push(AudioFrames {
                    samples: samples.to_vec(),
                    first_frame: first,
                    frames: count,
                    channels: frames.channels,
                });
            }
            self.next_frame = packet.next_frame(self.next_frame);
            if packet.reaches_past_tile() {
                self.pending = Some(frames);
            }
        }
        // A tile that ends where a packet does has not seen whether the
        // file ends there too, and a whole-track window needs to know which
        // tile is the last. Read the packet the next tile starts with.
        while reached_eof.is_none() && self.pending.is_none() && !cancelled() {
            match self.source.next_frames() {
                Ok(AudioRead::Frames(frames)) => self.pending = Some(frames),
                Ok(AudioRead::Pending) => {}
                Ok(AudioRead::Eof) => reached_eof = Some(self.next_frame),
                // The tile is complete; the next one seeks and reports it.
                Err(_) => {
                    self.next_frame = u64::MAX;
                    break;
                }
            }
        }
        Ok((tile, reached_eof))
    }
}

/// How many reads [`first_frames`] makes at most.
const FIRST_READS: usize = 256;

/// The first audio of `source`, read when it is opened if it decodes on
/// the reading thread, for its decoder to take first. A Symphonia source
/// that decodes no audio reads the file with `GStreamer` instead, so this
/// tells which decoder reads the file, as the overview's summaries must
/// come from the same one.
fn first_frames(source: &mut AudioFrameSource) -> anyhow::Result<Option<AudioFrames>> {
    for _ in 0..FIRST_READS {
        if source.decodes_on_its_own_thread() {
            break;
        }
        match source.next_frames()? {
            AudioRead::Frames(frames) => return Ok(Some(frames)),
            AudioRead::Eof => break,
            AudioRead::Pending => {}
        }
    }
    Ok(None)
}

/// Where a packet the decoder gives falls in tile `[start, end)`, as the
/// service's decoder places it while it reads the file in order: the frames
/// of the packet within the tile go in it at their own position, and a
/// packet that reaches past the tile is read again for the next one. Frames
/// before the tile, which an overlap gives, are left out, and a gap stays
/// empty.
pub(super) struct TilePacket {
    first: u64,
    last: u64,
    offset: usize,
    channels: usize,
    reaches_past: bool,
}

impl TilePacket {
    /// The packet of `frames` frames in `channels` channels whose first
    /// frame is `first_frame`, for tile `[start, end)`.
    pub(super) fn of(
        first_frame: u64,
        frames: usize,
        channels: usize,
        start: u64,
        end: u64,
    ) -> anyhow::Result<Self> {
        let packet_end = first_frame.saturating_add(usize_to_u64(frames));
        let first = first_frame.max(start);
        let last = packet_end.min(end);
        let offset = if first < last {
            usize::try_from(first - first_frame)? * channels
        } else {
            0
        };
        Ok(Self {
            first,
            last,
            offset,
            channels,
            reaches_past: packet_end > end,
        })
    }

    /// The first frame, the frame count and the samples of what the tile
    /// holds of the packet whose samples are `samples`, if anything.
    pub(super) fn samples<'a>(&self, samples: &'a [f32]) -> Option<(u64, usize, &'a [f32])> {
        if self.first >= self.last {
            return None;
        }
        let count = usize::try_from(self.last - self.first).ok()?;
        let samples = samples.get(self.offset..self.offset + count * self.channels)?;
        Some((self.first, count, samples))
    }

    /// The frame the decoder reads next once it read the packet, having
    /// read up to `next_frame` before.
    pub(super) fn next_frame(&self, next_frame: u64) -> u64 {
        self.last.max(next_frame)
    }

    /// Whether the packet has frames past the tile, so it is kept for the
    /// next tile, which then completes the tile without reading further.
    pub(super) fn reaches_past_tile(&self) -> bool {
        self.reaches_past
    }
}

/// The sample rate of a session for a decoder of `rate` Hz.
pub(super) fn session_sample_rate(rate: u64) -> u32 {
    u32::try_from(rate).unwrap_or(u32::MAX).max(1)
}

/// The channel count of a session for a decoder of `channels` channels.
pub(super) fn session_channels(channels: usize) -> usize {
    channels.clamp(1, usize::from(u16::MAX))
}

impl WaveformSession {
    /// Take the coarse summaries the overview decode built for this file,
    /// if it decoded it as this session does. Tiles the session has keep
    /// theirs.
    fn adopt(&self, prefill: CoarsePrefill) {
        let CoarsePrefill {
            identity,
            sample_rate,
            channels,
            gstreamer,
            tiles,
            eof_frame,
            last_tile,
            ..
        } = prefill;
        if !self.confirmed
            || identity != self.identity
            || (sample_rate, channels, gstreamer)
                != (self.sample_rate, self.channels, self.gstreamer)
        {
            return;
        }
        let mut caches = self.caches();
        if caches.eof_frame.is_some_and(|eof| eof != eof_frame) {
            return;
        }
        caches.eof_frame = Some(eof_frame);
        for (index, tile) in tiles {
            let bytes = tile.bytes();
            caches.coarse_summaries.insert(index, tile, bytes);
        }
        if caches.last_tile.is_none() {
            if let Some((index, tile)) = last_tile {
                caches.keep_if_last(index, &tile);
            }
        }
        #[cfg(test)]
        self.adopted.store(true, Ordering::Relaxed);
    }
}

impl ServiceSlot {
    /// A slot with `session` open, for tests of the handover.
    #[cfg(test)]
    pub(super) fn holding(session: Option<Arc<WaveformSession>>) -> Self {
        Self {
            session,
            opening: false,
            prefill: None,
        }
    }

    /// The identity of the file whose summaries the slot keeps.
    #[cfg(test)]
    pub(super) fn prefill_identity(&self) -> Option<&SourceIdentity> {
        self.prefill.as_ref().map(|prefill| &prefill.identity)
    }

    /// Give the open session the coarse summaries the overview decode
    /// built, if they are of its file, or else keep them for the session
    /// that opens it next. Returns those they replace, of another file.
    pub(super) fn offer(&mut self, prefill: CoarsePrefill) -> Option<CoarsePrefill> {
        let open = self
            .session
            .as_ref()
            .filter(|session| session.identity == prefill.identity)
            .filter(|session| !session.broken.load(Ordering::Relaxed));
        if let Some(session) = open {
            session.adopt(prefill);
            self.prefill.take()
        } else {
            self.prefill.replace(prefill)
        }
    }

    /// Give `session`, which has just opened its file, the summaries kept
    /// for it. Those of its path for another identity are of a file that
    /// has changed since and are dropped; those of another path are kept
    /// for when that opens.
    pub(super) fn hand_over(&mut self, session: &WaveformSession) {
        if let Some(prefill) = self
            .prefill
            .take_if(|prefill| prefill.identity.path == session.identity.path)
        {
            session.adopt(prefill);
        }
    }
}

/// Give the service the coarse summaries the overview decode built: to the
/// session of their file, if it is open, or else to the session that opens
/// it next. They replace any the service holds for another.
pub(super) fn offer_prefill(prefill: CoarsePrefill) {
    let replaced = lock_service().offer(prefill);
    drop(replaced);
}

/// Drop the coarse summaries the overview decode built that no session
/// took, as when another track starts.
pub(super) fn discard_prefill() {
    let prefill = lock_service().prefill.take();
    drop(prefill);
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
    let session = Arc::new(WaveformSession::open(path)?);
    let mut service = lock_service();
    service.hand_over(&session);
    service.session = Some(Arc::clone(&session));
    drop(service);
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
        static IDENTIFIED: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
    }

    /// Run `hook` when this thread has read the identity of a file it
    /// opens a session for, before its decoder opens it.
    pub(in super::super) fn after_each_identity(hook: impl FnMut() + 'static) {
        IDENTIFIED.with(|cell| *cell.borrow_mut() = Some(Box::new(hook)));
    }

    pub(super) fn identified() {
        IDENTIFIED.with(|cell| {
            if let Some(hook) = cell.borrow_mut().as_mut() {
                hook();
            }
        });
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
    use super::super::waveform_pyramid::COARSE_SUMMARY_FRAMES;
    use super::super::waveform_window::decode_waveform_window;
    use super::super::waveform_window::tests::write_test_wave;
    use super::*;

    /// Packets a test gives the service's decoder, in order; `None` is a
    /// read that finds nothing yet.
    struct Packets(std::collections::VecDeque<Option<AudioFrames>>);

    impl FrameSource for Packets {
        fn next_frames(&mut self) -> anyhow::Result<AudioRead> {
            Ok(match self.0.pop_front() {
                None => AudioRead::Eof,
                // As `AudioFrameSource` gives an empty packet.
                Some(Some(frames)) if frames.frames > 0 => AudioRead::Frames(frames),
                Some(_) => AudioRead::Pending,
            })
        }

        fn seek_to_frame(&mut self, _: u64, _: u32) -> anyhow::Result<()> {
            anyhow::bail!("a whole-track window reads the file in order")
        }
    }

    /// A packet of `frames` frames in `channels` channels from
    /// `first_frame` on, with values past full scale both ways.
    fn packet(first_frame: u64, frames: usize, channels: usize) -> AudioFrames {
        let mut state = first_frame.wrapping_mul(0x9e37_79b9) ^ usize_to_u64(channels);
        let samples = (0..frames * channels)
            .map(|_| {
                state = state
                    .wrapping_mul(6_364_136_223_846_793_005)
                    .wrapping_add(1_442_695_040_888_963_407);
                f32::from(u16::try_from(state >> 48).unwrap()) / 25_000.0 - 1.3
            })
            .collect();
        AudioFrames {
            samples,
            first_frame,
            frames,
            channels,
        }
    }

    type TileBits = (u64, (u32, u32, usize, Vec<[u32; 2]>));
    type PcmBits = Vec<(u64, usize, usize, Vec<u32>)>;

    /// The coarse tiles, the end of the file and the last tile's PCM that
    /// the service builds from `packets`, reading them as a whole-track
    /// window does.
    fn service_reading(
        packets: &[Option<AudioFrames>],
        channels: usize,
    ) -> (Vec<TileBits>, Option<u64>, Option<(u64, PcmBits)>) {
        let copy = |frames: &AudioFrames| AudioFrames {
            samples: frames.samples.clone(),
            ..*frames
        };
        let mut decoder = Decoder {
            source: Packets(packets.iter().map(|p| p.as_ref().map(copy)).collect()),
            next_frame: 0,
            pending: None,
        };
        let (mut tiles, mut eof, mut last) = (Vec::new(), None, None);
        for index in 0..100 {
            if eof.is_some_and(|eof| index * TILE_FRAMES >= eof) {
                break;
            }
            let (tile, reached) = decoder
                .decode_tile(index, eof, 48_000, &|| false)
                .expect("decode in order");
            eof = eof.or(reached);
            if !tile.segments.is_empty() {
                let (_, coarse) = PyramidTile::build(&tile, index, channels);
                tiles.push((index, coarse.bits()));
                if eof
                    .and_then(|eof| eof.checked_sub(1))
                    .map(|f| f / TILE_FRAMES)
                    == Some(index)
                {
                    last = Some((index, tile.bits()));
                }
            }
        }
        (tiles, eof, last)
    }

    #[test]
    fn prefilled_tiles_equal_the_tiles_the_service_builds_packet_for_packet() {
        use super::super::waveform_prefill::PrefillBuilder;

        // (first frame, frames, channels) of the packets, for a stereo file.
        let packets = [
            (300, 700, 2),        // a stream that starts late
            (1_000, 4_000, 2),    //
            (5_100, 3_900, 2),    // after a gap
            (8_990, 7_394, 2),    // overlapping, ending on the edge of tile 0
            (16_384, 3_616, 2),   //
            (20_000, 40_000, 2),  // over tiles 1 to 3
            (70_000, 5_000, 2),   // after a gap over the edge of tile 4
            (100_000, 10_000, 2), // after a gap over all of tile 5
            (110_000, 5_000, 2),  // over the edge of tile 7
            (114_000, 6_000, 1),  // mono, overlapping that edge
            (120_000, 0, 2),      // empty
            (120_000, 1_000, 8),  // eight channels
            (121_000, 10_072, 2), // to the edge of tile 8
        ];
        for (name, channels, end) in [
            ("ending on a tile edge", 2, None),
            ("ending within a tile", 2, Some(4_000)),
            ("ending a frame past a tile edge", 2, Some(10_073)),
            ("of six channels", 6, None),
        ] {
            let mut list: Vec<_> = packets
                .iter()
                .map(|&(first, frames, decoded)| {
                    let decoded = if channels == 2 { decoded } else { channels };
                    Some(packet(first, frames, decoded))
                })
                .collect();
            if let Some(frames) = end {
                *list.last_mut().unwrap() = Some(packet(121_000, frames, channels));
            }
            // Reads that find nothing yet change nothing.
            list.insert(3, None);
            list.insert(9, None);
            let (tiles, eof, last) = service_reading(&list, channels);

            let identity = SourceIdentity::made_up("/packets");
            let mut builder = PrefillBuilder::of(identity, 48_000, channels, false);
            for frames in list.iter().flatten() {
                builder.push(frames.first_frame, &frames.samples, frames.channels);
            }
            let prefill = builder.finish().expect("a complete decode");
            let built: Vec<_> = prefill
                .tiles
                .iter()
                .map(|(index, tile)| (*index, tile.bits()))
                .collect();
            assert_eq!(
                built.iter().map(|(index, _)| *index).collect::<Vec<_>>(),
                tiles.iter().map(|(index, _)| *index).collect::<Vec<_>>(),
                "{name}: the tiles that hold audio"
            );
            assert!(built == tiles, "{name}: the summaries differ");
            assert_eq!(Some(prefill.eof_frame), eof, "{name}: the end of the file");
            let built_last = prefill
                .last_tile
                .as_ref()
                .map(|(index, tile)| (*index, tile.bits()));
            assert!(built_last == last, "{name}: the last tile differs");
            assert!(last.is_some(), "{name}: the last tile holds audio");
        }
    }

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
        let first = session.summary(0, 64, &|| false).expect("summary");
        session.caches().summaries.budget = first.bytes();
        for index in [1, 2, 3, 0] {
            let summary = session
                .summary(index, 64, &|| false)
                .expect("summary after eviction");
            assert!(summary.rows(64).0.iter().all(|row| *row == [0.25, 0.25]));
            let caches = session.caches();
            assert!(caches.summaries.bytes <= caches.summaries.budget);
            assert_eq!(caches.summaries.tiles.len(), 1);
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn coarse_summaries_stop_growing_at_their_budget_and_keep_what_they_hold() {
        let path = write_test_wave(&vec![-4_096; 10 * 16_384 + 11], 48_000, 1);
        let session = WaveformSession::open(&path).expect("open fixture");
        session
            .summary(0, COARSE_SUMMARY_FRAMES, &|| false)
            .expect("summary");
        {
            let mut caches = session.caches();
            caches.coarse_summaries.budget = caches.coarse_summaries.bytes * 3;
            caches.pcm = TileCache::new(0);
            caches.summaries = TileCache::new(0);
        }
        // A whole-track pass in order, as a window reads it, twice.
        for _ in 0..2 {
            for index in 0..10 {
                let summary = session
                    .summary(index, COARSE_SUMMARY_FRAMES, &|| false)
                    .expect("coarse summary");
                let (rows, span) = summary.rows(COARSE_SUMMARY_FRAMES);
                assert_eq!(span, COARSE_SUMMARY_FRAMES);
                assert!(rows.iter().all(|row| *row == [-0.125, -0.125]));
            }
            let caches = session.caches();
            assert!(caches.coarse_summaries.bytes <= caches.coarse_summaries.budget);
            let mut kept: Vec<_> = caches.coarse_summaries.tiles.keys().copied().collect();
            kept.sort_unstable();
            assert_eq!(kept, [0, 1, 2], "the first tiles stay");
        }
        assert_eq!(
            session.decoded_tiles(),
            10 + 7,
            "only the tiles beyond the budget decode again"
        );
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
