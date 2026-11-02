// SPDX-License-Identifier: GPL-3.0-or-later

//! AC3 (A/52) and DTS frame headers: sync words, frame lengths, and bitrates.
//!
//! A DTS stream comes in 16-bit words of either byte order, each holding 16
//! bits of the stream or, as on DTS-CDs, only 14. A DTS-CD keeps the
//! stream in its PCM audio, so a rip of one is often a WAV file that tells
//! plain 16-bit stereo PCM: its frames are found past the WAV header as
//! past any leading junk, so that `GStreamer` reads only the DTS stream.

/// AC3 bitrate lookup indexed by `frmsizecod / 2` (kbps) — A/52 Table 5.18.
const AC3_BITRATES_KBPS: [u32; 19] = [
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
];

/// DTS sample rate table indexed by SFREQ code.
const DTS_SAMPLE_RATES: [u32; 16] = [
    0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0,
];

const AC3_SYNC: [u8; 2] = [0x0B, 0x77];
/// The highest `bsid` of plain AC3; above it, a frame is E-AC3.
const AC3_MAX_BSID: u8 = 10;
/// The DTS core sync word, as the stream holds it in 16-bit big-endian
/// words.
const DTS_SYNC: [u8; 4] = [0x7F, 0xFE, 0x80, 0x01];
/// Bytes of a DTS core frame header that [`parse_dts_bitrate`] reads: up to
/// `SFREQ`.
const DTS_HEADER_BYTES: usize = 9;

/// The bitstream format of a raw surround file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Codec {
    /// AC3 (A/52), including E-AC3 frames.
    Ac3,
    /// DTS core frames, in any of the [`DtsWords`] layouts.
    Dts,
}

/// How the 16-bit words of a DTS stream hold its bits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum DtsWords {
    BigEndian16,
    LittleEndian16,
    /// The low 14 bits of each word, as on DTS-CDs; the top 2 repeat the
    /// sign of the 14.
    BigEndian14,
    LittleEndian14,
}

impl DtsWords {
    /// The layout whose sync word starts `data`.
    fn of(data: &[u8]) -> Option<Self> {
        match data.get(..4)? {
            [0x7F, 0xFE, 0x80, 0x01] => Some(Self::BigEndian16),
            [0xFE, 0x7F, 0x01, 0x80] => Some(Self::LittleEndian16),
            [0x1F, 0xFF, 0xE8, 0x00] => Some(Self::BigEndian14),
            [0xFF, 0x1F, 0x00, 0xE8] => Some(Self::LittleEndian14),
            _ => None,
        }
    }

    fn word(self, bytes: [u8; 2]) -> u16 {
        match self {
            Self::BigEndian16 | Self::BigEndian14 => u16::from_be_bytes(bytes),
            Self::LittleEndian16 | Self::LittleEndian14 => u16::from_le_bytes(bytes),
        }
    }

    fn bits_per_word(self) -> usize {
        match self {
            Self::BigEndian16 | Self::LittleEndian16 => 16,
            Self::BigEndian14 | Self::LittleEndian14 => 14,
        }
    }

    /// Bytes of the file that hold a frame of `len` bytes of the stream:
    /// whole words, the last padded, which is where `FFmpeg` finds the next
    /// frame. (`GStreamer`'s `dcaparse` takes `len * 16 / 14` bytes, short
    /// of a padded word, and loses most of the frames of such a stream;
    /// the frames of DTS-CDs, of 3584 bytes, fill whole words.)
    fn file_len(self, len: usize) -> usize {
        match self {
            Self::BigEndian16 | Self::LittleEndian16 => len,
            Self::BigEndian14 | Self::LittleEndian14 => (len * 8).div_ceil(14) * 2,
        }
    }
}

/// The first `N` bytes of the DTS stream that starts `data`, unpacked from
/// its words into the 16-bit big-endian layout, and the layout they had.
fn dts_stream_bytes<const N: usize>(data: &[u8]) -> Option<(DtsWords, [u8; N])> {
    let words = DtsWords::of(data)?;
    let bits = words.bits_per_word();
    let mask = (1_u32 << bits) - 1;
    let mut bytes = [0; N];
    let mut filled = 0;
    // Bits not yet copied, the oldest first; fewer than 8 between words.
    let (mut pending, mut pending_bits) = (0_u32, 0);
    for pair in data.chunks_exact(2) {
        if filled == N {
            break;
        }
        pending = (pending << bits) | (u32::from(words.word([pair[0], pair[1]])) & mask);
        pending_bits += bits;
        while pending_bits >= 8 && filled < N {
            pending_bits -= 8;
            bytes[filled] = (pending >> pending_bits).to_le_bytes()[0];
            filled += 1;
        }
        pending &= (1 << pending_bits) - 1;
    }
    (filled == N).then_some((words, bytes))
}

/// The header of the DTS core frame that starts `data`, in the 16-bit
/// big-endian layout, and the layout of the file's words.
fn dts_header(data: &[u8]) -> Option<(DtsWords, [u8; DTS_HEADER_BYTES])> {
    // A 14-bit sync word continues into a third word.
    dts_stream_bytes(data).filter(|(_, header)| header.starts_with(&DTS_SYNC))
}

impl Codec {
    pub(super) fn of_path(path: &std::path::Path) -> Self {
        if super::is_dts_file(path) {
            Self::Dts
        } else {
            Self::Ac3
        }
    }

    /// Bytes of the sync word, which every frame of a stream starts with.
    fn sync_len(self) -> usize {
        match self {
            Self::Ac3 => AC3_SYNC.len(),
            Self::Dts => DTS_SYNC.len(),
        }
    }

    /// Length in bytes of the frame whose header starts `data`.
    fn frame_len(self, data: &[u8]) -> Option<usize> {
        match self {
            Self::Ac3 => ac3_frame_len(data),
            Self::Dts => dts_frame_len(data),
        }
    }
}

/// Offset of the first frame in `data`, the start of a raw surround file.
///
/// Files ripped from discs often start with the end of the previous
/// track's last frame, which can contain the sync word by chance. A sync
/// word counts only with a valid header and, when `data` reaches that far,
/// the same sync word right after the frame.
pub(super) fn first_frame_offset(data: &[u8], codec: Codec) -> Option<usize> {
    let sync_len = codec.sync_len();
    (0..data.len().saturating_sub(sync_len - 1)).find(|&offset| {
        let frame = &data[offset..];
        let Some(len) = codec.frame_len(frame) else {
            return false;
        };
        match frame.get(len..) {
            // A valid header starts with its sync word.
            Some(next) if next.len() >= sync_len => next.starts_with(&frame[..sync_len]),
            // The frame ends at or after the end of `data`: nothing to compare.
            _ => true,
        }
    })
}

/// Length of an AC3 or E-AC3 frame from its header (A/52 Table 5.18 and
/// Annex E).
fn ac3_frame_len(header: &[u8]) -> Option<usize> {
    if header.len() < 6 || !header.starts_with(&AC3_SYNC) {
        return None;
    }
    let bsid = header[5] >> 3;
    if bsid > AC3_MAX_BSID {
        if bsid > 16 {
            return None;
        }
        // E-AC3: `frmsiz` counts 16-bit words minus one.
        let words = (usize::from(header[2] & 0x07) << 8) | usize::from(header[3]);
        return Some((words + 1) * 2);
    }
    let frmsizecod = usize::from(header[4] & 0x3F);
    let kbps = usize::try_from(*AC3_BITRATES_KBPS.get(frmsizecod / 2)?).ok()?;
    // 1536 samples per frame, in 16-bit words.
    let words = match header[4] >> 6 {
        0 => kbps * 2,
        1 => kbps * 1_536_000 / (44_100 * 16) + (frmsizecod & 1),
        2 => kbps * 3,
        _ => return None,
    };
    Some(words * 2)
}

/// Length of the frame that `header` starts if it is plain AC3, not E-AC3.
pub(super) fn plain_ac3_frame_len(header: &[u8]) -> Option<usize> {
    (*header.get(5)? >> 3 <= AC3_MAX_BSID).then(|| ac3_frame_len(header))?
}

/// Length in bytes of the file of the DTS core frame that starts `data`,
/// from its `FSIZE` field.
fn dts_frame_len(data: &[u8]) -> Option<usize> {
    let (words, header) = dts_header(data)?;
    let fsize = (usize::from(header[5] & 0x03) << 12)
        | (usize::from(header[6]) << 4)
        | (usize::from(header[7]) >> 4);
    // Values below 95 are invalid.
    (fsize >= 95).then(|| words.file_len(fsize + 1))
}

/// Parse the bitrate from an AC3 (A/52) sync frame header.
///
/// Layout of the first 5 bytes:
///   `[0..2]` sync word `0x0B77`
///   `[2..4]` CRC1
///   `[4]`    fscod (2 bits) | frmsizecod (6 bits)
pub(super) fn parse_ac3_bitrate(header: &[u8]) -> Option<u32> {
    if header.len() < 5 || header[0] != 0x0B || header[1] != 0x77 {
        return None;
    }
    let frmsizcod = usize::from(header[4] & 0x3F);
    let kbps = *AC3_BITRATES_KBPS.get(frmsizcod / 2)?;
    Some(kbps * 1000)
}

/// The bitrate of the file, from the DTS frame that starts `data`: the
/// frame's length in the file's words by the samples it codes.
///
/// After the 32-bit sync word, the header holds `NBLKS` (bits 39..46), the
/// blocks of 32 samples less one, `FSIZE` (bits 46..60) and `SFREQ` (bits
/// 66..70).
pub(super) fn parse_dts_bitrate(data: &[u8]) -> Option<u32> {
    let (_, header) = dts_header(data)?;
    let frame_bytes = u64::try_from(dts_frame_len(data)?).ok()?;
    let blocks = (u64::from(header[4] & 0x01) << 6) | u64::from(header[5] >> 2);
    // Values below 5 are invalid.
    if blocks < 5 {
        return None;
    }
    let samples = (blocks + 1) * 32;
    let sample_rate = *DTS_SAMPLE_RATES.get(usize::from((header[8] >> 2) & 0x0F))?;
    if sample_rate == 0 {
        return None;
    }
    u32::try_from(frame_bytes * 8 * u64::from(sample_rate) / samples).ok()
}

/// `stream`, a DTS stream in 16-bit big-endian words, in the words of
/// `layout`: each frame in whole words, the last padded with zeros.
#[cfg(test)]
pub(super) fn repack_dts(stream: &[u8], layout: DtsWords) -> Vec<u8> {
    let mut packed = Vec::new();
    let mut rest = stream;
    while !rest.is_empty() {
        let len = dts_frame_len(rest).expect("a frame").min(rest.len());
        repack_dts_frame(&rest[..len], layout, &mut packed);
        rest = &rest[len..];
    }
    packed
}

#[cfg(test)]
fn repack_dts_frame(frame: &[u8], layout: DtsWords, packed: &mut Vec<u8>) {
    let bits = layout.bits_per_word();
    let mask = (1_u32 << bits) - 1;
    let mut push = |word: u32| {
        let mut word = u16::try_from(word & mask).expect("a word");
        if bits == 14 && word & 0x2000 != 0 {
            word |= 0xC000;
        }
        packed.extend_from_slice(&match layout {
            DtsWords::BigEndian16 | DtsWords::BigEndian14 => word.to_be_bytes(),
            DtsWords::LittleEndian16 | DtsWords::LittleEndian14 => word.to_le_bytes(),
        });
    };
    let (mut pending, mut pending_bits) = (0_u32, 0);
    for byte in frame {
        pending = (pending << 8) | u32::from(*byte);
        pending_bits += 8;
        if pending_bits >= bits {
            pending_bits -= bits;
            push(pending >> pending_bits);
            pending &= (1 << pending_bits) - 1;
        }
    }
    if pending_bits > 0 {
        push(pending << (bits - pending_bits));
    }
}

#[cfg(test)]
mod tests {
    use super::{dts_frame_len, first_frame_offset, parse_dts_bitrate, repack_dts};
    use super::{Codec, DtsWords};
    use crate::raw_audio::{TEST_DTS, TEST_DTS_44K};

    #[test]
    fn ac3_bitrate_parses_standard_header() {
        // AC3 sync word 0x0B77, CRC1 placeholder, fscod=00 (48kHz), frmsizecod=12 → index 6 → 96 kbps
        let header = [0x0B, 0x77, 0x00, 0x00, 0b00_001100, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(super::parse_ac3_bitrate(&header), Some(96_000));
    }

    #[test]
    fn ac3_bitrate_parses_640kbps() {
        // frmsizecod=36 → index 18 → 640 kbps
        let header = [0x0B, 0x77, 0x00, 0x00, 0b00_100100, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(super::parse_ac3_bitrate(&header), Some(640_000));
    }

    #[test]
    fn ac3_bitrate_rejects_bad_sync() {
        let header = [0x00, 0x00, 0x00, 0x00, 0x00];
        assert_eq!(super::parse_ac3_bitrate(&header), None);
    }

    #[test]
    fn dts_bitrate_parses_standard_header() {
        // DTS sync: 7F FE 80 01
        // Build a header with known FSIZE and SFREQ.
        // FSIZE = 2047 (frame_bytes = 2048) at bits 47..61
        // SFREQ = 13 (48000 Hz) at bits 67..71
        //
        // Byte layout:
        //   [0..4] = 7F FE 80 01  (sync)
        //   [4]    = don't care (FTYPE, SHORT, CPF, NBLKS upper)
        //   [5]    = NBLKS lower 2 bits | FSIZE upper 2 bits
        //           FSIZE=2047 → 0b_0111_1111_1111 (11 bits in 14-bit field → upper 2 = 0b01)
        //           → byte5 lower 2 bits = 0b11 (FSIZE bits 12..13)
        //           Wait, FSIZE is 14 bits. 2047 = 0x7FF = 0b0000_0111_1111_1111
        //           byte5[1:0] = bits 13..12 = 0b00
        //   Actually let me just compute the exact bytes.
        //
        // FSIZE = 2047 → 14 bits → 0b00_0111_1111_1111
        // Packed into bytes 5..7:
        //   byte5 & 0x03 = upper 2 bits of 14 = 0b00
        //   byte6 = next 8 bits = 0b0111_1111 = 0x7F
        //   byte7 upper 4 bits = lower 4 bits = 0b1111 → 0xF0
        //
        // SFREQ = 13 → 4 bits → 0b1101
        // Packed into byte 8: bits [5..2] = (byte8 >> 2) & 0x0F
        //   byte8 = 0b00_1101_00 = 0x34
        let mut header = [0u8; 12];
        header[0] = 0x7F;
        header[1] = 0xFE;
        header[2] = 0x80;
        header[3] = 0x01;
        header[5] = 0x3C; // NBLKS lower 6 = 15 (512 samples), FSIZE upper 2 = 0
        header[6] = 0x7F; // FSIZE mid 8
        header[7] = 0xF0; // FSIZE lower 4 in upper nibble
        header[8] = 0x34; // SFREQ=13 at bits 5..2

        // frame_bytes = 2047 + 1 = 2048
        // sample_rate = 48000
        // bitrate = 2048 * 8 * 48000 / 512 = 1_536_000
        assert_eq!(super::parse_dts_bitrate(&header), Some(1_536_000));
    }

    #[test]
    fn dts_bitrate_rejects_bad_sync() {
        let header = [0u8; 12];
        assert_eq!(super::parse_dts_bitrate(&header), None);
    }

    /// A DTS stream in one layout.
    struct Case {
        stream: Vec<u8>,
        layout: DtsWords,
        frames: usize,
        frame_len: usize,
        sample_rate: usize,
    }

    /// Both fixtures in every layout. The 224-byte frames of
    /// `TEST_DTS_44K` fill whole 14-bit words; the 256-byte frames of
    /// `TEST_DTS` end in a padded one.
    fn cases() -> Vec<Case> {
        let fixtures = [
            (TEST_DTS_44K, 44, 44_100, [224, 256]),
            (TEST_DTS, 47, 48_000, [256, 294]),
        ];
        let layouts = [
            (DtsWords::BigEndian16, 0),
            (DtsWords::LittleEndian16, 0),
            (DtsWords::BigEndian14, 1),
            (DtsWords::LittleEndian14, 1),
        ];
        fixtures
            .into_iter()
            .flat_map(|(fixture, frames, sample_rate, lengths)| {
                layouts.map(|(layout, length)| Case {
                    stream: repack_dts(fixture, layout),
                    layout,
                    frames,
                    frame_len: lengths[length],
                    sample_rate,
                })
            })
            .collect()
    }

    #[test]
    fn dts_frames_are_found_in_every_word_layout() {
        for case in cases() {
            let (stream, name) = (&case.stream, (case.layout, case.frame_len));
            assert_eq!(stream.len(), case.frames * case.frame_len, "{name:?}");
            assert_eq!(dts_frame_len(stream), Some(case.frame_len), "{name:?}");
            // After a WAV header and digital silence, as in a DTS-CD rip.
            let mut file = vec![0; 1000];
            file[..4].copy_from_slice(b"RIFF");
            file.extend_from_slice(stream);
            assert_eq!(
                first_frame_offset(&file, Codec::Dts),
                Some(1000),
                "{name:?}"
            );
        }
    }

    #[test]
    fn a_dts_sync_word_needs_its_frame_header() {
        for case in cases() {
            let (stream, name) = (&case.stream, (case.layout, case.frame_len));
            let mut sync_only = stream[..4].to_vec();
            sync_only.resize(64, 0);
            assert_eq!(dts_frame_len(&sync_only), None, "{name:?}");
            // Its next frame, cut short, is not taken for a frame of
            // another layout.
            assert_eq!(
                first_frame_offset(&stream[1..], Codec::Dts),
                Some(case.frame_len - 1),
                "{name:?}"
            );
        }
    }

    #[test]
    fn dts_bitrates_count_the_words_of_the_file() {
        // 512 samples a frame.
        for case in cases() {
            let expected = case.frame_len * 8 * case.sample_rate / 512;
            assert_eq!(
                parse_dts_bitrate(&case.stream),
                Some(u32::try_from(expected).expect("bitrate")),
                "{:?}",
                (case.layout, case.frame_len)
            );
        }
    }
}
