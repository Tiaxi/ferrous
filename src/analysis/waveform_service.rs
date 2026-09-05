// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, TryLockError};
use std::time::{Duration, SystemTime};

use super::decoders::{open_audio_file, AudioFrameSource, AudioFrames, AudioRead};
use super::usize_to_u64;

pub(super) const TILE_FRAMES: u64 = 16_384;
const PCM_CACHE_BYTES: usize = 32 * 1024 * 1024;

// Calls originate on the Qt worker pool. Share one active-track decoder and
// cache across those threads instead of depending on thread-local affinity.
static SERVICE: Mutex<Option<WaveformSession>> = Mutex::new(None);

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

pub(super) struct WaveformSession {
    identity: SourceIdentity,
    source: AudioFrameSource,
    pub(super) sample_rate: u32,
    pub(super) channels: usize,
    next_frame: u64,
    pending: Option<AudioFrames>,
    eof_frame: Option<u64>,
    tiles: HashMap<u64, (Arc<PcmTile>, u64)>,
    cache_bytes: usize,
    cache_budget: usize,
    access: u64,
    #[cfg(test)]
    pub(super) decoded_tiles: usize,
}

impl WaveformSession {
    pub(super) fn open(path: &Path) -> anyhow::Result<Self> {
        let identity = SourceIdentity::read(path)?;
        let (source, rate, channels, _) =
            open_audio_file(path).ok_or_else(|| anyhow::anyhow!("unsupported audio file"))?;
        Ok(Self {
            identity,
            source,
            sample_rate: u32::try_from(rate).unwrap_or(u32::MAX).max(1),
            channels: channels.clamp(1, usize::from(u16::MAX)),
            next_frame: 0,
            pending: None,
            eof_frame: None,
            tiles: HashMap::new(),
            cache_bytes: 0,
            cache_budget: PCM_CACHE_BYTES,
            access: 0,
            #[cfg(test)]
            decoded_tiles: 0,
        })
    }

    pub(super) fn tile(
        &mut self,
        index: u64,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<Arc<PcmTile>> {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        self.access = self.access.saturating_add(1);
        if let Some((tile, accessed)) = self.tiles.get_mut(&index) {
            *accessed = self.access;
            return Ok(Arc::clone(tile));
        }
        let tile = Arc::new(self.decode_tile(index, cancelled)?);
        let bytes = tile.bytes();
        if !tile.segments.is_empty() && bytes <= self.cache_budget {
            while self.cache_bytes + bytes > self.cache_budget {
                let Some(oldest) = self
                    .tiles
                    .iter()
                    .min_by_key(|(_, (_, access))| access)
                    .map(|(&index, _)| index)
                else {
                    break;
                };
                if let Some((removed, _)) = self.tiles.remove(&oldest) {
                    self.cache_bytes -= removed.bytes();
                }
            }
            self.cache_bytes += bytes;
            self.tiles.insert(index, (Arc::clone(&tile), self.access));
        }
        Ok(tile)
    }

    pub(super) fn reached_eof(&self, frame: u64) -> bool {
        self.eof_frame.is_some_and(|eof| frame >= eof)
    }

    fn decode_tile(
        &mut self,
        index: u64,
        cancelled: &impl Fn() -> bool,
    ) -> anyhow::Result<PcmTile> {
        let start = index.saturating_mul(TILE_FRAMES);
        let end = start.saturating_add(TILE_FRAMES);
        let mut tile = PcmTile {
            segments: Vec::new(),
        };
        if self.eof_frame.is_some_and(|eof| start >= eof) {
            return Ok(tile);
        }
        if self.next_frame != start {
            self.source.seek_to_frame(start, self.sample_rate)?;
            self.pending = None;
            self.next_frame = start;
        }
        #[cfg(test)]
        {
            self.decoded_tiles += 1;
        }
        while self.next_frame < end {
            anyhow::ensure!(!cancelled(), "waveform decode cancelled");
            let frames = if let Some(frames) = self.pending.take() {
                frames
            } else {
                match self.source.next_frames()? {
                    AudioRead::Frames(frames) => frames,
                    AudioRead::Pending => continue,
                    AudioRead::Eof => {
                        self.eof_frame = Some(self.next_frame);
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
        Ok(tile)
    }
}

pub(super) fn with_session<T>(
    path: &Path,
    cancelled: &impl Fn() -> bool,
    operation: impl FnOnce(&mut WaveformSession) -> anyhow::Result<T>,
) -> anyhow::Result<T> {
    let mut service = loop {
        anyhow::ensure!(!cancelled(), "waveform decode cancelled");
        match SERVICE.try_lock() {
            Ok(service) => break service,
            Err(TryLockError::WouldBlock) => std::thread::sleep(Duration::from_millis(5)),
            Err(TryLockError::Poisoned(error)) => {
                let mut service = error.into_inner();
                *service = None;
                SERVICE.clear_poison();
                break service;
            }
        }
    };
    let identity = SourceIdentity::read(path)?;
    if service
        .as_ref()
        .is_none_or(|session| session.identity != identity)
    {
        *service = None; // Release the previous decoder before opening another.
        *service = Some(WaveformSession::open(path)?);
    }
    let session = service.as_mut().expect("session opened above");
    operation(session)
}

#[cfg(test)]
mod tests {
    use super::super::waveform_window::tests::write_test_wave;
    use super::*;

    #[test]
    fn pcm_cache_reuses_tiles_and_evicts_without_changing_samples() {
        let samples: Vec<i16> = (0..70_007)
            .map(|i| i16::try_from(i % 20_000).unwrap())
            .collect();
        let path = write_test_wave(&samples, 48_000, 1);
        let mut session = WaveformSession::open(&path).expect("open fixture");
        let first = session.tile(0, &|| false).expect("first tile");
        let again = session.tile(0, &|| false).expect("cached tile");
        assert!(Arc::ptr_eq(&first, &again));
        assert_eq!(session.decoded_tiles, 1);
        session.cache_budget = first.bytes() * 2;
        for index in [1, 2, 3, 0] {
            let tile = session.tile(index, &|| false).expect("read tile");
            for segment in &tile.segments {
                let offset = usize::try_from(segment.first_frame).unwrap();
                for (actual, &expected) in segment.samples.iter().zip(&samples[offset..]) {
                    assert!((actual - f32::from(expected) / 32768.0).abs() < 0.000_001);
                }
            }
            assert!(session.cache_bytes <= session.cache_budget);
        }
        assert_eq!(session.decoded_tiles, 5, "evicted prefix must decode again");
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn cancelled_partial_tile_is_not_cached_and_retry_is_complete() {
        let path = write_test_wave(&vec![1234; 33_001], 48_000, 1);
        let mut session = WaveformSession::open(&path).expect("open fixture");
        let calls = std::cell::Cell::new(0);
        let result = session.tile(0, &|| {
            calls.set(calls.get() + 1);
            calls.get() >= 3
        });
        assert!(result.is_err());
        assert!(session.tiles.is_empty());
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
    fn cancelled_request_does_not_wait_for_busy_service() {
        let guard = SERVICE.lock().expect("lock service");
        let started = std::time::Instant::now();
        let worker = std::thread::spawn(|| {
            let calls = std::cell::Cell::new(0);
            with_session(
                Path::new("/missing/cancelled-busy.wav"),
                &|| {
                    calls.set(calls.get() + 1);
                    calls.get() > 2
                },
                |_| Ok(()),
            )
        });
        let error = worker
            .join()
            .expect("worker")
            .expect_err("cancelled lock wait");
        assert!(error.to_string().contains("cancelled"));
        assert!(started.elapsed() < Duration::from_secs(1));
        drop(guard);
    }
}
