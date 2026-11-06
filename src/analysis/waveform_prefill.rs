// SPDX-License-Identifier: GPL-3.0-or-later

//! The coarse summaries of the waveform detail service, built from the
//! overview decode, so the first whole-track window of a track needs no
//! decode of its own.
//!
//! The overview reads the file from its start to its end, as a whole-track
//! window reads it, with the decoder the service opens for it. Its packets
//! go into tiles here as the service's decoder puts them in tiles reading
//! the file in order (see [`TilePacket`]), and the tiles' summaries are
//! built by the same code, so they are those the service builds itself:
//! gaps stay missing coverage, unlike in the overview's timeline.

use std::path::Path;
use std::sync::Arc;

use super::decoders::AudioFrames;
use super::waveform_pyramid::{PyramidTile, TileRows};
use super::waveform_service::{
    retained_bytes, session_channels, session_sample_rate, PcmTile, SourceIdentity, TilePacket,
    COARSE_SUMMARY_BYTES, TILE_FRAMES,
};

/// The most channels of a file whose last tile the overview keeps as PCM
/// for the service: a tile of them is 4 MiB of 32-bit samples.
const LAST_TILE_PCM_CHANNELS: usize = 64;

/// The coarse summaries of a file for the waveform detail service: those
/// the overview decode built reading it to its end, or those a session kept
/// for the current track when no view read it any more.
#[cfg_attr(test, derive(Clone))]
pub(super) struct CoarsePrefill {
    pub(super) identity: SourceIdentity,
    pub(super) sample_rate: u32,
    pub(super) channels: usize,
    /// Whether `GStreamer` decoded the file, rather than Symphonia.
    pub(super) gstreamer: bool,
    /// The complete tiles, in order, up to the service's budget.
    pub(super) tiles: Vec<(u64, Arc<PyramidTile>)>,
    /// The end of the file, if known: the overview decode always knows it,
    /// a session only once a window has read that far.
    pub(super) eof_frame: Option<u64>,
    /// The PCM of the tile that holds the last frame.
    pub(super) last_tile: Option<(u64, Arc<PcmTile>)>,
}

/// Builds the [`CoarsePrefill`] of a file from the packets of its overview
/// decode, in the order the decoder gives them.
pub(super) struct PrefillBuilder {
    identity: SourceIdentity,
    sample_rate: u32,
    channels: usize,
    gstreamer: bool,
    /// The tile the packets go in, and the frame the service's decoder
    /// would read next.
    index: u64,
    next_frame: u64,
    /// Its rows, until the store is full.
    rows: Option<TileRows>,
    /// Its PCM, in case it is the last tile.
    pcm: TilePcm,
    holds_audio: bool,
    /// The frames the file is expected to have, if it tells.
    estimated_frames: Option<u64>,
    tiles: Vec<(u64, Arc<PyramidTile>)>,
    bytes: usize,
    budget: usize,
    /// Set when the decode did what the service's decoder would not.
    failed: bool,
}

impl PrefillBuilder {
    /// A builder for the file at `path` with `metadata`, as read from the
    /// handle the decode reads, which its decoder reads at `rate` Hz in
    /// `channels` channels, as the service's decoder of the file would.
    pub(super) fn new(
        path: &Path,
        metadata: &std::fs::Metadata,
        rate: u64,
        channels: usize,
        gstreamer: bool,
    ) -> Option<Self> {
        let identity = SourceIdentity::of(path, metadata).ok()?;
        Some(Self::of(identity, rate, channels, gstreamer))
    }

    /// A builder for the file of `identity`; see [`Self::new`].
    pub(super) fn of(
        identity: SourceIdentity,
        rate: u64,
        channels: usize,
        gstreamer: bool,
    ) -> Self {
        let channels = session_channels(channels);
        let mut builder = Self {
            identity,
            sample_rate: session_sample_rate(rate),
            channels,
            gstreamer,
            index: 0,
            next_frame: 0,
            rows: Some(TileRows::new(0, channels)),
            pcm: TilePcm::default(),
            holds_audio: false,
            estimated_frames: None,
            tiles: Vec::new(),
            bytes: 0,
            budget: COARSE_SUMMARY_BYTES,
            failed: false,
        };
        builder.pcm.keeping = builder.may_be_last(0);
        builder
    }

    /// The builder of a file expected to have `frames` frames, as its
    /// header or decoder tells. It keeps the PCM of a tile only near there,
    /// in case that tile is the last.
    pub(super) fn expecting(mut self, frames: Option<u64>) -> Self {
        self.estimated_frames = frames;
        self.pcm.keeping = self.may_be_last(self.index);
        self
    }

    /// Whether to keep the PCM of tile `index` in case it is the last: a
    /// tile of up to 64 channels, from the one before the end the file is
    /// expected to have on, or all if it does not tell. A file that turns
    /// out shorter than it tells has no last tile kept; the service decodes
    /// it if a window needs its PCM.
    fn may_be_last(&self, index: u64) -> bool {
        self.channels <= LAST_TILE_PCM_CHANNELS
            && self
                .estimated_frames
                .is_none_or(|frames| index.saturating_add(2) >= frames.div_ceil(TILE_FRAMES))
    }

    /// Add a packet the decoder gave: `samples` interleaved in `channels`
    /// channels, whose first frame is `first_frame`.
    pub(super) fn push(&mut self, first_frame: u64, samples: &[f32], channels: usize) {
        let frames = samples.len() / channels.max(1);
        // The service's decoder never sees an empty packet.
        if self.failed || frames == 0 {
            return;
        }
        loop {
            let mut start = self.index.saturating_mul(TILE_FRAMES);
            if self.next_frame >= start.saturating_add(TILE_FRAMES) {
                // A complete tile, which this packet follows.
                self.finish_tile();
                self.index += 1;
                start = self.index.saturating_mul(TILE_FRAMES);
            }
            let end = start.saturating_add(TILE_FRAMES);
            let Ok(packet) = TilePacket::of(first_frame, frames, channels, start, end) else {
                self.failed = true;
                return;
            };
            if let Some((first, count, within)) = packet.samples(samples) {
                self.holds_audio = true;
                if let Some(rows) = self.rows.as_mut() {
                    rows.push(first, within, channels);
                }
                self.pcm.push(first, count, within, channels);
            }
            self.next_frame = packet.next_frame(self.next_frame);
            if !packet.reaches_past_tile() {
                return;
            }
        }
    }

    /// The decode did something the service's decoder would not, such as
    /// going on past a decoder error: build nothing.
    pub(super) fn fail(&mut self) {
        self.failed = true;
    }

    /// The summaries, once the decode reached the end of the file.
    pub(super) fn finish(mut self) -> Option<CoarsePrefill> {
        if self.failed {
            return None;
        }
        let eof_frame = self.next_frame;
        let last_index = eof_frame.checked_sub(1).map(|frame| frame / TILE_FRAMES);
        let last_tile = (self.holds_audio && last_index == Some(self.index))
            .then(|| self.pcm.tile())
            .flatten()
            .map(|tile| (self.index, Arc::new(tile)));
        self.finish_tile();
        Some(CoarsePrefill {
            identity: self.identity,
            sample_rate: self.sample_rate,
            channels: self.channels,
            gstreamer: self.gstreamer,
            tiles: self.tiles,
            eof_frame: Some(eof_frame),
            last_tile,
        })
    }

    /// Keep the summaries of the current tile, if it holds audio, as the
    /// service does, and start the next.
    fn finish_tile(&mut self) {
        if let Some(rows) = self.rows.take() {
            let mut room = true;
            if self.holds_audio {
                let (_, coarse) = rows.finish();
                let bytes = retained_bytes::<PyramidTile>(coarse.bytes());
                // Past the budget, the service builds the rest itself, as
                // its store keeps no more.
                room = self.bytes + bytes <= self.budget;
                if room {
                    self.tiles.push((self.index, Arc::new(coarse)));
                    self.bytes += bytes;
                }
            }
            if room {
                self.rows = Some(TileRows::new(self.index.saturating_add(1), self.channels));
            }
        }
        self.pcm
            .clear(self.may_be_last(self.index.saturating_add(1)));
        self.holds_audio = false;
    }

    /// The builder with a budget of `budget` bytes for the summaries.
    #[cfg(test)]
    pub(super) fn within(mut self, budget: usize) -> Self {
        self.budget = budget;
        self
    }
}

/// The PCM of one tile in one buffer, reused from tile to tile.
#[derive(Default)]
struct TilePcm {
    /// Whether it keeps the PCM of the current tile.
    keeping: bool,
    samples: Vec<f32>,
    /// The first frame, frames and channels of each segment, in order.
    segments: Vec<(u64, usize, usize)>,
}

impl TilePcm {
    fn push(&mut self, first_frame: u64, frames: usize, samples: &[f32], channels: usize) {
        if self.keeping {
            self.samples.extend_from_slice(samples);
            self.segments.push((first_frame, frames, channels));
        }
    }

    /// The tile, if the PCM was kept.
    fn tile(&self) -> Option<PcmTile> {
        if !self.keeping {
            return None;
        }
        let mut samples = self.samples.as_slice();
        let segments = self
            .segments
            .iter()
            .map(|&(first_frame, frames, channels)| {
                let (segment, rest) = samples.split_at(frames * channels);
                samples = rest;
                AudioFrames {
                    samples: segment.to_vec(),
                    first_frame,
                    frames,
                    channels,
                }
            })
            .collect();
        Some(PcmTile { segments })
    }

    /// Start the next tile, keeping its PCM if `keeping`.
    fn clear(&mut self, keeping: bool) {
        self.samples.clear();
        self.segments.clear();
        self.keeping = keeping;
    }
}

/// Where an overview decode builds the coarse summaries of its file for the
/// waveform detail service, if it is asked to.
#[derive(Default)]
pub(super) struct PrefillSlot<'a> {
    /// Takes the summaries as soon as they are built, before the decode
    /// sends its last update. Without it, none are built.
    sink: Option<&'a mut dyn FnMut(CoarsePrefill)>,
    /// The file the decode reads, as the handle it opened tells.
    opened: Option<(std::path::PathBuf, std::fs::Metadata)>,
    /// A smaller budget for the summaries, for tests.
    #[cfg(test)]
    budget: Option<usize>,
}

impl<'a> PrefillSlot<'a> {
    /// A slot that asks the decode to build the summaries and gives them
    /// to `sink` once built.
    pub(super) fn offering(sink: &'a mut dyn FnMut(CoarsePrefill)) -> Self {
        Self {
            sink: Some(sink),
            ..Self::default()
        }
    }

    /// The decode opened the file at `path`, whose handle has `metadata`.
    pub(super) fn opened(&mut self, path: &Path, metadata: Option<std::fs::Metadata>) {
        self.opened = metadata.map(|metadata| (path.to_path_buf(), metadata));
    }

    /// A builder for the decode of the opened file, whose decoder reads it
    /// at `rate` Hz in `channels` channels, if the slot wants one.
    pub(super) fn builder(
        &self,
        rate: u64,
        channels: usize,
        gstreamer: bool,
    ) -> Option<PrefillBuilder> {
        let (path, metadata) = self.opened.as_ref().filter(|_| self.sink.is_some())?;
        let builder = PrefillBuilder::new(path, metadata, rate, channels, gstreamer)?;
        #[cfg(test)]
        let builder = match self.budget {
            Some(budget) => builder.within(budget),
            None => builder,
        };
        Some(builder)
    }

    /// The decode reached the end of the file with `builder`.
    pub(super) fn finished(&mut self, builder: Option<PrefillBuilder>) {
        if let (Some(sink), Some(built)) =
            (self.sink.as_mut(), builder.and_then(PrefillBuilder::finish))
        {
            sink(built);
        }
    }

    /// The slot, with a budget of `budget` bytes for the summaries.
    #[cfg(test)]
    pub(super) fn within(self, budget: usize) -> Self {
        Self {
            budget: Some(budget),
            ..self
        }
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::path::PathBuf;

    use super::super::waveform_pyramid::COARSE_SUMMARY_FRAMES;
    use super::super::waveform_service::{ServiceSlot, WaveformSession};
    use super::super::waveform_window::{decode_window, tests::write_test_wave_to};
    use super::super::{decode_overview_job, decode_waveform_peaks_stream, usize_to_u64};
    use super::*;

    type TileBits = (u64, (u32, u32, usize, Vec<[u32; 2]>));
    type PcmBits = Vec<(u64, usize, usize, Vec<u32>)>;

    fn fixture_path(name: &str) -> PathBuf {
        let path = crate::paths::test_root().join(format!("waveform-prefill-{name}"));
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        path
    }

    /// The summaries the overview decode of `path` builds, if it does,
    /// within `budget` bytes if given.
    fn overview_prefill(path: &Path, budget: Option<usize>) -> Option<CoarsePrefill> {
        let mut built = None;
        let mut sink = |prefill| built = Some(prefill);
        let slot = PrefillSlot::offering(&mut sink);
        let mut slot = match budget {
            Some(budget) => slot.within(budget),
            None => slot,
        };
        decode_waveform_peaks_stream(
            path,
            1_024,
            &Cell::new(None),
            &mut slot,
            |_, _, _| true,
            || false,
        )
        .expect("overview");
        drop(slot);
        built
    }

    /// What the summaries take in the service's store.
    fn store_bytes(prefill: &CoarsePrefill) -> usize {
        prefill
            .tiles
            .iter()
            .map(|(_, tile)| retained_bytes::<PyramidTile>(tile.bytes()))
            .sum()
    }

    /// The coarse tiles a session of its own builds for `path`, reading it
    /// in order as a whole-track window does, the end of the file, and the
    /// PCM of its last tile.
    fn service_tiles(path: &Path) -> (Vec<TileBits>, u64, Option<(u64, PcmBits)>) {
        let session = WaveformSession::open(path).expect("open the file");
        let mut tiles = Vec::new();
        let mut index = 0;
        while !session.reached_eof(index * TILE_FRAMES) {
            assert!(index < 1_000, "no end of the file");
            let pcm = session.tile(index, &|| false).expect("tile");
            if !pcm.segments.is_empty() {
                let summary = session
                    .summary(index, COARSE_SUMMARY_FRAMES, &|| false)
                    .expect("summary");
                tiles.push((index, summary.bits()));
            }
            index += 1;
        }
        let eof = session.eof_frame().expect("the end of the file");
        let last_index = (eof - 1) / TILE_FRAMES;
        let last = session.tile(last_index, &|| false).expect("last tile");
        let last = (!last.segments.is_empty()).then(|| (last_index, last.bits()));
        (tiles, eof, last)
    }

    /// Assert that the overview decode of `path` builds the summaries that
    /// the service builds itself, and that whole-track windows from them
    /// decode nothing and equal those the service decodes, whether the
    /// service opened the file before the overview ended or after.
    fn assert_prefill_matches_service(name: &str, path: &Path) {
        let prefill =
            overview_prefill(path, None).unwrap_or_else(|| panic!("{name}: no summaries"));
        let (tiles, eof, last) = service_tiles(path);
        let built: Vec<_> = prefill
            .tiles
            .iter()
            .map(|(index, tile)| (*index, tile.bits()))
            .collect();
        assert_eq!(
            built.iter().map(|(index, _)| *index).collect::<Vec<_>>(),
            tiles.iter().map(|(index, _)| *index).collect::<Vec<_>>(),
            "{name}: the tiles"
        );
        assert!(built == tiles, "{name}: the summaries differ");
        assert_eq!(prefill.eof_frame, Some(eof), "{name}: the end of the file");
        let built_last = prefill
            .last_tile
            .as_ref()
            .map(|(index, tile)| (*index, tile.bits()));
        assert!(built_last == last, "{name}: the last tile differs");
        assert!(store_bytes(&prefill) <= COARSE_SUMMARY_BYTES);

        let rate = f64::from(prefill.sample_rate);
        // Coarse enough for the coarse levels.
        let points = usize::try_from(eof / (2 * COARSE_SUMMARY_FRAMES))
            .unwrap()
            .max(1);
        #[allow(clippy::cast_precision_loss)] // Frame counts of a few seconds.
        let ends = [
            eof as f64 / rate,
            (eof - 1) as f64 / rate,
            eof as f64 / rate + 1.0,
        ];
        for open_first in [true, false] {
            for &end in &ends {
                let fresh = WaveformSession::open(path).expect("open");
                let expected = decode_window(&fresh, 0.0, end, points, &|| false).expect("window");
                let session = Arc::new(WaveformSession::open(path).expect("open"));
                if open_first {
                    let mut slot = ServiceSlot::holding(Some(Arc::clone(&session)));
                    assert!(slot.offer(prefill.clone()).is_none());
                    assert!(slot.prefill_identity().is_none(), "{name}: kept");
                } else {
                    let mut slot = ServiceSlot::holding(None);
                    assert!(slot.offer(prefill.clone()).is_none());
                    slot.hand_over(&session);
                    assert!(slot.prefill_identity().is_none(), "{name}: not handed over");
                }
                assert!(session.adopted(), "{name}: not taken");
                let window = decode_window(&session, 0.0, end, points, &|| false).expect("window");
                assert_eq!(session.decoded_tiles(), 0, "{name}: decoded to {end} s");
                let bits = |values: &[f32]| values.iter().map(|v| v.to_bits()).collect::<Vec<_>>();
                assert_eq!(window.frames_per_point, expected.frames_per_point);
                assert!(
                    bits(&window.extrema) == bits(&expected.extrema)
                        && bits(&window.downmix_extrema) == bits(&expected.downmix_extrema),
                    "{name}: the window to {end} s differs"
                );
            }
        }
    }

    /// Interleaved 16-bit samples of `frames` frames in `channels`
    /// channels, with single-sample peaks of each sign on both sides of
    /// every tile edge and stretches of one sign only.
    fn pcm16(frames: usize, channels: usize) -> Vec<i16> {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let mut samples: Vec<i16> = (0..frames * channels)
            .map(|i| {
                let frame = i / channels;
                let value = i16::try_from((i * 7_919) % 20_001).unwrap() - 10_000;
                if (frame / 3_000) % 4 == 1 {
                    value.abs()
                } else {
                    value
                }
            })
            .collect();
        for edge in (tile..frames).step_by(tile) {
            for (offset, peak) in [(edge - 1, i16::MAX), (edge, i16::MIN)] {
                samples[offset * channels + (edge / tile) % channels] = peak;
            }
        }
        samples
    }

    #[test]
    fn prefilled_tiles_equal_the_service_tiles_for_wave_files() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        for (name, rate, channels, frames) in [
            ("5.1 96 kHz WAV of 20 whole tiles", 96_000, 6, 20 * tile),
            ("stereo 44.1 kHz WAV", 44_100, 2, 20 * tile + 1_234),
            ("mono 48 kHz WAV shorter than a tile", 48_000, 1, 9_000),
        ] {
            let path = fixture_path(&format!("{channels}-{rate}.wav"));
            let channels = u16::try_from(channels).unwrap();
            write_test_wave_to(&path, &pcm16(frames, usize::from(channels)), rate, channels);
            assert_prefill_matches_service(name, &path);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    /// A FLAC file of `samples`, interleaved in `channels` channels of
    /// `bits` bits at `rate` Hz, in verbatim blocks of 4,096 frames.
    fn write_flac(path: &Path, samples: &[i32], rate: u32, channels: usize, bits: u32) {
        let bytes = super::super::test_flac::flac_bytes(samples, rate, channels, bits, None);
        std::fs::write(path, bytes).expect("write FLAC fixture");
    }

    #[test]
    fn prefilled_tiles_equal_the_service_tiles_for_flac_files() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        for (name, rate, channels, bits, frames) in [
            ("24-bit 5.1 96 kHz FLAC", 96_000, 6, 24, 5 * tile + 100),
            (
                "16-bit stereo 44.1 kHz FLAC of 4 whole tiles",
                44_100,
                2,
                16,
                4 * tile,
            ),
        ] {
            let scale = 1 << (bits - 16);
            let samples: Vec<i32> = pcm16(frames, channels)
                .into_iter()
                .enumerate()
                .map(|(i, value)| {
                    i32::from(value) * scale + i32::try_from(i % 7).unwrap() * (scale - 1) / 6
                })
                .collect();
            let path = fixture_path(&format!("{channels}-{bits}.flac"));
            write_flac(
                &path,
                &samples,
                rate,
                channels,
                u32::try_from(bits).unwrap(),
            );
            assert_prefill_matches_service(name, &path);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    #[test]
    fn prefilled_tiles_equal_the_service_tiles_for_compressed_fixtures() {
        for (name, bytes, extension) in [
            (
                "MP3 with encoder delay",
                &include_bytes!("fixtures/gapless-click.mp3")[..],
                "mp3",
            ),
            (
                "MP3 changing from stereo to mono",
                &include_bytes!("fixtures/channel-change.mp3")[..],
                "mp3",
            ),
            (
                "Ogg Vorbis",
                &include_bytes!("fixtures/tag-tone.ogg")[..],
                "ogg",
            ),
            ("ALAC", &include_bytes!("fixtures/tag-tone.m4a")[..], "m4a"),
        ] {
            let path = fixture_path(&format!("{}.{extension}", name.replace(' ', "-")));
            std::fs::write(&path, bytes).expect("write fixture");
            assert_prefill_matches_service(name, &path);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    #[cfg(feature = "gst")]
    #[test]
    fn prefilled_tiles_equal_the_service_tiles_for_gstreamer_files() {
        use crate::raw_audio::{
            ripped_dts_cd_bytes, ripped_surround_bytes, TEST_AC3, TEST_DTS, TEST_DTS_44K,
        };
        for (name, bytes, extension) in [
            // 1.024 s at 48 kHz: three whole tiles.
            ("AC-3", TEST_AC3.to_vec(), "ac3"),
            ("AC-3, three times", TEST_AC3.repeat(3), "ac3"),
            ("DTS, four times", TEST_DTS.repeat(4), "dts"),
            ("ripped AC-3", ripped_surround_bytes(TEST_AC3).0, "AC3"),
            ("ripped DTS", ripped_surround_bytes(TEST_DTS).0, "dts"),
            ("DTS-CD", ripped_dts_cd_bytes(TEST_DTS_44K).0, "dts"),
            // As the rate of a DTS 96/24 stream changes where its decoder
            // falls back to the core.
            (
                "DTS from 48 to 44.1 kHz",
                [TEST_DTS, TEST_DTS_44K].concat(),
                "dts",
            ),
            (
                "DTS from 44.1 to 48 kHz",
                [TEST_DTS_44K, TEST_DTS].concat(),
                "dts",
            ),
            (
                "Opus",
                include_bytes!("fixtures/tag-tone.opus").to_vec(),
                "opus",
            ),
            (
                "WavPack",
                include_bytes!("fixtures/tag-tone.wv").to_vec(),
                "wv",
            ),
            // Symphonia opens it but reads no audio from it, so the
            // service's decoder turns to GStreamer only as it reads.
            ("WAV of unknown size", unsized_wave(), "wav"),
        ] {
            let path = fixture_path(&format!("{}.{extension}", name.replace([' ', ','], "-")));
            std::fs::write(&path, bytes).expect("write fixture");
            assert_prefill_matches_service(name, &path);
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    /// A WAV file whose data chunk tells no size, which Symphonia reads
    /// no audio from.
    #[cfg(feature = "gst")]
    fn unsized_wave() -> Vec<u8> {
        let path = fixture_path("unsized-source.wav");
        write_test_wave_to(&path, &pcm16(40_000, 2), 48_000, 2);
        let mut bytes = std::fs::read(&path).expect("read fixture");
        std::fs::remove_file(path).expect("remove fixture");
        bytes[40..44].copy_from_slice(&0_u32.to_le_bytes());
        bytes
    }

    /// A 48 kHz WAV file of `samples` named `name`.
    fn wave(name: &str, samples: &[i16], channels: u16) -> PathBuf {
        let path = fixture_path(name);
        write_test_wave_to(&path, samples, 48_000, channels);
        path
    }

    #[test]
    fn summaries_of_a_changed_file_are_dropped_and_others_wait_for_their_file() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let a = wave("identity-a.wav", &vec![8_192; 3 * tile], 1);
        let b = wave("identity-b.wav", &vec![-8_192; 2 * tile], 1);
        let prefill = overview_prefill(&a, None).expect("summaries");

        // Kept while another file is open or opens, until A opens.
        let session_b = Arc::new(WaveformSession::open(&b).expect("open B"));
        let mut slot = ServiceSlot::holding(Some(Arc::clone(&session_b)));
        assert!(slot.offer(prefill.clone()).is_none());
        let other = WaveformSession::open(&b).expect("open B");
        slot.hand_over(&other);
        assert!(!session_b.adopted() && !other.adopted());
        assert_eq!(slot.prefill_identity(), Some(&prefill.identity));
        let session_a = WaveformSession::open(&a).expect("open A");
        slot.hand_over(&session_a);
        assert!(session_a.adopted());
        assert!(slot.prefill_identity().is_none());

        // Not those of another decode of the file.
        let session_a = Arc::new(WaveformSession::open(&a).expect("open A"));
        let mut slot = ServiceSlot::holding(Some(Arc::clone(&session_a)));
        for (rate, channels) in [(48_000, 2), (44_100, 1)] {
            let mut other = prefill.clone();
            (other.sample_rate, other.channels) = (rate, channels);
            assert!(slot.offer(other).is_none());
        }
        let mut other = prefill.clone();
        other.gstreamer = true;
        assert!(slot.offer(other).is_none());
        assert!(!session_a.adopted());

        // A changes before the service opens it: the summaries go, and the
        // service reads the file as it is.
        let mut slot = ServiceSlot::holding(None);
        assert!(slot.offer(prefill.clone()).is_none());
        write_test_wave_to(&a, &vec![-4_096; 3 * tile + 5], 48_000, 1);
        let changed = Arc::new(WaveformSession::open(&a).expect("open A again"));
        slot.hand_over(&changed);
        assert!(!changed.adopted(), "the summaries of the old file");
        assert!(slot.prefill_identity().is_none(), "kept for a file gone");
        // Nor does the open session of the changed file take them.
        let mut slot = ServiceSlot::holding(Some(Arc::clone(&changed)));
        assert!(slot.offer(prefill.clone()).is_none());
        assert_eq!(slot.prefill_identity(), Some(&prefill.identity));
        assert!(!changed.adopted());
        let window = decode_window(&changed, 0.0, 1.1, 4, &|| false).expect("window");
        assert!(window.extrema.iter().all(|&value| value == -0.125));
        assert!(changed.decoded_tiles() > 0);
        for path in [a, b] {
            std::fs::remove_file(path).expect("remove fixture");
        }
    }

    #[test]
    fn the_last_tile_is_kept_near_the_end_the_file_tells() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let samples: Vec<f32> = (0..5 * tile + 7)
            .map(|i| (i % 100) as f32 / 100.0)
            .collect();
        let eof = usize_to_u64(samples.len());
        for (expected, kept) in [
            (None, true),
            (Some(eof), true),
            (Some(eof - 2 * TILE_FRAMES), true),
            // A tile past the one before the last.
            (Some(eof + TILE_FRAMES), true),
            (Some(eof + 2 * TILE_FRAMES), false),
        ] {
            let identity = SourceIdentity::made_up("/estimate");
            let mut builder = PrefillBuilder::of(identity, 48_000, 1, false).expecting(expected);
            for (packet, chunk) in samples.chunks(4_000).enumerate() {
                builder.push(usize_to_u64(packet * 4_000), chunk, 1);
            }
            let prefill = builder.finish().expect("summaries");
            assert_eq!(prefill.eof_frame, Some(eof));
            let last = prefill.last_tile.map(|(index, tile)| (index, tile.bits()));
            if kept {
                let (index, segments) = last.expect("the last tile");
                assert_eq!(index, 5);
                let frames: usize = segments.iter().map(|segment| segment.1).sum();
                assert_eq!((segments[0].0, frames), (5 * TILE_FRAMES, 7));
                assert_eq!(segments[0].3[0], samples[5 * tile].to_bits());
            } else {
                assert!(last.is_none(), "expecting {expected:?}");
            }
        }
    }

    #[test]
    fn a_replacement_of_the_same_length_and_time_drops_the_summaries() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let path = wave("replaced.wav", &vec![8_192; 3 * tile], 1);
        let prefill = overview_prefill(&path, None).expect("summaries");
        // Other samples, of the same length and modification time, saved
        // over it as an atomic save does.
        let modified = std::fs::metadata(&path).unwrap().modified().unwrap();
        let other = wave("replacement.wav", &vec![-8_192; 3 * tile], 1);
        std::fs::File::options()
            .write(true)
            .open(&other)
            .unwrap()
            .set_modified(modified)
            .unwrap();
        std::fs::rename(&other, &path).expect("replace");
        let metadata = std::fs::metadata(&path).unwrap();
        assert_eq!(metadata.modified().unwrap(), modified);

        // Whether the service opens it after or has it open already.
        let session = Arc::new(WaveformSession::open(&path).expect("open"));
        let mut slot = ServiceSlot::holding(None);
        assert!(slot.offer(prefill.clone()).is_none());
        slot.hand_over(&session);
        assert!(!session.adopted(), "the summaries of the old file");
        assert!(slot.prefill_identity().is_none());
        let mut slot = ServiceSlot::holding(Some(Arc::clone(&session)));
        assert!(slot.offer(prefill).is_none());
        assert!(!session.adopted());
        let end = 3.0 * 16_384.0 / 48_000.0;
        let window = decode_window(&session, 0.0, end, 4, &|| false).expect("window");
        assert!(window.extrema.iter().all(|&value| value == -0.25));
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_file_replaced_while_the_service_opens_it_takes_no_summaries() {
        use super::super::waveform_service::test_hooks::after_each_identity;

        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let path = wave("replaced-on-open.wav", &vec![8_192; 3 * tile], 1);
        let prefill = overview_prefill(&path, None).expect("summaries");
        // Replaced, with a longer file, once the service has read the
        // identity of the file at the path but before its decoder opens it.
        let replacement = fixture_path("replacement-on-open.wav");
        write_test_wave_to(&replacement, &vec![-8_192; 5 * tile], 48_000, 1);
        let (from, to) = (replacement.clone(), path.clone());
        after_each_identity(move || {
            if from.exists() {
                std::fs::rename(&from, &to).expect("replace");
            }
        });
        let session = WaveformSession::open(&path).expect("open");
        after_each_identity(|| {});
        assert!(!replacement.exists(), "replaced");
        let mut slot = ServiceSlot::holding(None);
        assert!(slot.offer(prefill).is_none());
        slot.hand_over(&session);
        assert!(!session.adopted(), "the summaries of the old file");
        // The whole new file, not cut at the old file's end.
        let end = 5.0 * 16_384.0 / 48_000.0;
        let window = decode_window(&session, 0.0, end, 8, &|| false).expect("window");
        assert!(window.extrema.iter().all(|&value| value == -0.25));
        assert_eq!(session.eof_frame(), Some(5 * TILE_FRAMES));
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn the_summaries_stay_within_their_budget_and_the_rest_is_decoded() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let path = wave("budget.wav", &pcm16(10 * tile + 11, 2), 2);
        let whole = overview_prefill(&path, None).expect("summaries");
        assert_eq!(whole.tiles.len(), 11);
        let tile_bytes = store_bytes(&whole) / 11;
        let budget = 3 * tile_bytes + tile_bytes / 2;
        let prefill = overview_prefill(&path, Some(budget)).expect("summaries");
        let indexes: Vec<_> = prefill.tiles.iter().map(|(index, _)| *index).collect();
        assert_eq!(indexes, [0, 1, 2], "the first tiles, in order");
        assert!(store_bytes(&prefill) <= budget);
        assert_eq!(prefill.eof_frame, whole.eof_frame);

        // The service decodes the tiles past them, but for the last, whose
        // PCM it has.
        let session = WaveformSession::open(&path).expect("open");
        let mut slot = ServiceSlot::holding(None);
        assert!(slot.offer(prefill).is_none());
        slot.hand_over(&session);
        decode_window(&session, 0.0, 4.0, 8, &|| false).expect("window");
        assert_eq!(session.decoded_tiles(), 7);

        // Its own budget holds too.
        let session = WaveformSession::open(&path).expect("open");
        session.limit_coarse_summaries(budget);
        let mut slot = ServiceSlot::holding(None);
        assert!(slot.offer(whole).is_none());
        slot.hand_over(&session);
        let (bytes, limit) = session.coarse_summary_bytes();
        assert!(bytes <= limit, "{bytes} of {limit}");
        assert_eq!(session.coarse_tiles(), 3, "no more than the budget holds");
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn a_cancelled_or_superseded_overview_offers_no_summaries() {
        let tile = usize::try_from(TILE_FRAMES).unwrap();
        let path = wave("superseded.wav", &pcm16(40 * tile, 2), 2);

        // Cancelled once it sent its first update.
        let mut built = None;
        let mut sink = |prefill| built = Some(prefill);
        let mut slot = PrefillSlot::offering(&mut sink);
        let updates = Cell::new(0);
        decode_waveform_peaks_stream(
            &path,
            1_024,
            &Cell::new(None),
            &mut slot,
            |_, _, _| {
                updates.set(updates.get() + 1);
                true
            },
            || updates.get() > 0,
        )
        .expect("overview");
        assert_eq!(updates.get(), 1);
        drop(slot);
        assert!(built.is_none());

        // The worker's job: a track not superseded offers the summaries of
        // its file before the last update, and one superseded during the
        // decode offers none.
        for superseded in [false, true] {
            let sent = Cell::new(0_usize);
            let complete = Cell::new(false);
            let offered = Cell::new(None);
            decode_overview_job(
                &path,
                &|| superseded && sent.get() > 0,
                |update| {
                    sent.set(sent.get() + 1);
                    if update.complete {
                        complete.set(true);
                        assert!(!superseded);
                        let prefill = offered.take();
                        assert!(prefill.is_some(), "offered after the last update");
                        offered.set(prefill);
                    }
                },
                |prefill| offered.set(Some(prefill)),
            );
            let offered = offered.take();
            if superseded {
                assert!(offered.is_none() && !complete.get());
            } else {
                assert!(complete.get());
                let prefill = offered.expect("summaries offered");
                assert_eq!(prefill.tiles.len(), 40);
                let session = WaveformSession::open(&path).expect("open");
                assert_eq!(&prefill.identity, session.identity());
            }
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[cfg(feature = "gst")]
    #[test]
    fn a_cancelled_gstreamer_overview_builds_no_summaries() {
        let path = fixture_path("cancelled.ac3");
        std::fs::write(&path, crate::raw_audio::TEST_AC3.repeat(8)).expect("write fixture");
        let mut built = None;
        let mut sink = |prefill| built = Some(prefill);
        let mut slot = PrefillSlot::offering(&mut sink);
        let checks = Cell::new(0);
        super::super::gst_waveform::decode_waveform_peaks_stream_gst(
            &path,
            1_024,
            &Cell::new(None),
            &mut slot,
            |_, _, _| true,
            || {
                checks.set(checks.get() + 1);
                checks.get() > 20
            },
        )
        .expect("overview");
        drop(slot);
        assert!(built.is_none());
        std::fs::remove_file(path).expect("remove fixture");
    }
}
