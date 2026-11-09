// SPDX-License-Identifier: GPL-3.0-or-later

//! The end of the audio of a FLAC file, before the tags appended after it.
//!
//! A FLAC file may end with an `ID3v1` tag or an `APEv2` tag after its last
//! frame. Symphonia's FLAC reader takes those bytes for part of the last
//! frame, whose checksum then fails, and drops the frame: a decode ends a
//! frame short of the length in the header. Reading the file through
//! [`without_trailing_tags`] ends the stream where the last frame ends.
//!
//! Only a tag that checks out ends the stream early, since a wrong end
//! drops a frame that would decode. An `APEv2` tag is checked whole: its
//! footer, its items, which fill it exactly, and the header it claims. An
//! `ID3v1` tag is only a `TAG` marker 128 bytes before the end, which the
//! audio of the last frame may hold too, so it counts only if the bytes
//! before it end with a whole FLAC frame: a frame header that its CRC-8
//! checks, and the frame's CRC-16 in its last two bytes. That is what the
//! reader needs to give that frame.

use std::io::{self, Read, Seek, SeekFrom};

use symphonia::core::checksum::{Crc16Ansi, Crc8Ccitt};
use symphonia::core::io::{MediaSource, Monitor};

/// The size of an `ID3v1` tag, which starts with `TAG`.
const ID3V1_LEN: u64 = 128;
/// The size of the `fLaC` marker, the metadata block header and STREAMINFO.
const STREAM_HEADER_LEN: u64 = 42;
/// The size of an `APEv2` header or footer, which starts with `APETAGEX`.
const APE_BLOCK_LEN: u64 = 32;
/// The `APEv2` flag that the tag has a header before its items.
const APE_HAS_HEADER: u32 = 1 << 31;
/// The `APEv2` flag that the block is the header.
const APE_IS_HEADER: u32 = 1 << 29;
/// The `APEv2` flags of a tag: read only, and the type of its items.
const APE_TAG_FLAGS: u32 = 0b111;
/// The most items of an `APEv2` tag that is read; each takes a read.
const APE_MAX_ITEMS: u32 = 1_024;
/// The longest key of an `APEv2` item, after which comes a NUL.
const APE_MAX_KEY_LEN: usize = 255;
/// The longest start of an `APEv2` item: the size of its value, its flags,
/// and its key with the NUL after it.
const APE_MAX_ITEM_HEAD: u64 = 8 + 255 + 1;
/// How far before the end the last frame is looked for when STREAMINFO
/// does not tell the largest frame.
const UNKNOWN_FRAME_SEARCH: u64 = 1 << 20;
/// How many frame headers before the end are tried for the last frame:
/// audio data may look like a header.
const MAX_FRAME_HEADER_TRIES: usize = 16;

/// `source`, ending before the `ID3v1` and `APEv2` tags after the audio if
/// it is a FLAC file that has them, and as it is otherwise. A source that
/// cannot seek or does not tell its length is left as it is.
pub(crate) fn without_trailing_tags(mut source: Box<dyn MediaSource>) -> Box<dyn MediaSource> {
    let Some(len) = source.byte_len().filter(|_| source.is_seekable()) else {
        return source;
    };
    let end = flac_audio_end(&mut source, len);
    // The probe reads from the start; if the source cannot go back there,
    // it fails as it would have.
    if source.seek(SeekFrom::Start(0)).is_err() {
        return source;
    }
    match end {
        Ok(Some(end)) if end < len => Box::new(AudioEnd {
            inner: source,
            end,
            pos: 0,
        }),
        _ => source,
    }
}

/// An `APEv2` tag that ends at a place in a file.
enum ApeTag {
    /// No tag ends there.
    None,
    /// A tag that checks out, which starts at this place.
    StartsAt(u64),
    /// A footer whose tag does not check out.
    Damaged,
}

/// Where the tags after the audio of the FLAC file of `len` bytes that
/// `source` reads start, or `None` if it is not a FLAC file. The file ends
/// with an `APEv2` tag, an `ID3v1` tag, or both, in either order; a tag
/// that does not check out leaves the end where it is.
fn flac_audio_end<R: Read + Seek>(source: &mut R, len: u64) -> io::Result<Option<u64>> {
    let Some(audio_start) = flac_stream_start(source)? else {
        return Ok(None);
    };
    let floor = audio_start + STREAM_HEADER_LEN;
    if len < floor {
        return Ok(None);
    }
    // STREAMINFO's largest frame, 0 if not known.
    let largest = read_at::<3>(source, audio_start + 15)?;
    let largest = u64::from(u32::from_be_bytes([0, largest[0], largest[1], largest[2]]));
    let search = if largest == 0 {
        UNKNOWN_FRAME_SEARCH
    } else {
        largest
    };

    let end = match ape_tag_ending_at(source, len, floor)? {
        ApeTag::StartsAt(start) => start,
        ApeTag::Damaged => return Ok(Some(len)),
        ApeTag::None => len,
    };
    if end - floor >= ID3V1_LEN && read_at::<3>(source, end - ID3V1_LEN)? == *b"TAG" {
        let before = end - ID3V1_LEN;
        match ape_tag_ending_at(source, before, floor)? {
            // The order the tags usually have.
            ApeTag::StartsAt(start) => return Ok(Some(start)),
            ApeTag::Damaged => return Ok(Some(len)),
            ApeTag::None => {
                if ends_with_frame(source, before, floor, search)? {
                    return Ok(Some(before));
                }
            }
        }
    }
    Ok(Some(end))
}

/// The `APEv2` (or `APEv1`) tag of the file that `source` reads whose
/// footer ends at `end`, after no earlier than `floor`.
fn ape_tag_ending_at<R: Read + Seek>(source: &mut R, end: u64, floor: u64) -> io::Result<ApeTag> {
    if end - floor < APE_BLOCK_LEN {
        return Ok(ApeTag::None);
    }
    let footer = read_at::<32>(source, end - APE_BLOCK_LEN)?;
    if !footer.starts_with(b"APETAGEX") {
        return Ok(ApeTag::None);
    }
    let Some(footer) = ApeBlock::parse(&footer) else {
        return Ok(ApeTag::Damaged);
    };
    let has_header = footer.flags & APE_HAS_HEADER != 0;
    // A footer has no flags but these; version 1 has none, nor a header.
    let flags_valid = footer.flags & !(APE_HAS_HEADER | APE_TAG_FLAGS) == 0
        && (footer.version == 2_000 || footer.flags == 0);
    // The size counts the items and the footer, not the header.
    let size = u64::from(footer.size);
    let header_len = if has_header { APE_BLOCK_LEN } else { 0 };
    let fits = size >= APE_BLOCK_LEN && end - floor >= size + header_len;
    if !flags_valid || !fits || footer.items > APE_MAX_ITEMS {
        return Ok(ApeTag::Damaged);
    }
    let items_start = end - size;
    if has_header {
        let header = read_at::<32>(source, items_start - APE_BLOCK_LEN)?;
        let matches = header.starts_with(b"APETAGEX")
            && ApeBlock::parse(&header).is_some_and(|header| {
                header.version == footer.version
                    && header.size == footer.size
                    && header.items == footer.items
                    && header.flags == footer.flags | APE_IS_HEADER
            });
        if !matches {
            return Ok(ApeTag::Damaged);
        }
    }
    if !items_fill(source, items_start, end - APE_BLOCK_LEN, footer.items)? {
        return Ok(ApeTag::Damaged);
    }
    Ok(ApeTag::StartsAt(items_start - header_len))
}

/// The fields of an `APEv2` header or footer after `APETAGEX`.
struct ApeBlock {
    version: u32,
    size: u32,
    items: u32,
    flags: u32,
}

impl ApeBlock {
    /// The fields of `block`, or `None` if its version is unknown or its
    /// reserved bytes are not zero.
    fn parse(block: &[u8; 32]) -> Option<Self> {
        let field = |at: usize| {
            u32::from_le_bytes([block[at], block[at + 1], block[at + 2], block[at + 3]])
        };
        let parsed = Self {
            version: field(8),
            size: field(12),
            items: field(16),
            flags: field(20),
        };
        let valid =
            matches!(parsed.version, 1_000 | 2_000) && block[24..].iter().all(|&byte| byte == 0);
        valid.then_some(parsed)
    }
}

/// Whether `count` `APEv2` items fill the bytes from `start` to `end` of
/// the file that `source` reads exactly. An item is the size of its value,
/// its flags, a key of 2 to 255 printable ASCII characters and a NUL, and
/// the value.
fn items_fill<R: Read + Seek>(
    source: &mut R,
    start: u64,
    end: u64,
    count: u32,
) -> io::Result<bool> {
    let mut at = start;
    for _ in 0..count {
        let left = end - at;
        // The sizes, flags and the shortest key, `XY\0`.
        if left < 11 {
            return Ok(false);
        }
        let head_len = usize::try_from(left.min(APE_MAX_ITEM_HEAD)).unwrap_or(0);
        let head = read_vec_at(source, at, head_len)?;
        let value_len = u64::from(u32::from_le_bytes([head[0], head[1], head[2], head[3]]));
        let Some(key_len) = head[8..].iter().position(|&byte| byte == 0) else {
            return Ok(false);
        };
        let key = &head[8..8 + key_len];
        if !(2..=APE_MAX_KEY_LEN).contains(&key_len)
            || !key.iter().all(|&byte| (0x20..0x7f).contains(&byte))
        {
            return Ok(false);
        }
        let item_len = 8 + u64::try_from(key_len).unwrap_or(u64::MAX) + 1 + value_len;
        if item_len > left {
            return Ok(false);
        }
        at += item_len;
    }
    Ok(at == end)
}

/// Whether the bytes of the file that `source` reads before `end` end with
/// a whole FLAC frame that starts no earlier than `floor` and is at most
/// `search` bytes long.
fn ends_with_frame<R: Read + Seek>(
    source: &mut R,
    end: u64,
    floor: u64,
    search: u64,
) -> io::Result<bool> {
    let window = (end - floor).min(search);
    let bytes = read_vec_at(source, end - window, usize::try_from(window).unwrap_or(0))?;
    let Some(body_len) = bytes.len().checked_sub(2) else {
        return Ok(false);
    };
    let (body, crc) = bytes.split_at(body_len);
    let crc = u16::from_be_bytes([crc[0], crc[1]]);
    let mut tries = 0;
    for start in (0..body_len).rev() {
        if body[start] != 0xff || frame_header_len(&body[start..]).is_none() {
            continue;
        }
        let mut frame_crc = Crc16Ansi::new(0);
        frame_crc.process_buf_bytes(&body[start..]);
        if frame_crc.crc() == crc {
            return Ok(true);
        }
        tries += 1;
        if tries == MAX_FRAME_HEADER_TRIES {
            break;
        }
    }
    Ok(false)
}

/// The length of the FLAC frame header at the start of `bytes`, with its
/// CRC-8, if a header whose CRC-8 checks is there.
fn frame_header_len(bytes: &[u8]) -> Option<usize> {
    let [0xff, sync, rate_and_size, channels_and_bits, number, ..] = *bytes else {
        return None;
    };
    let block_size_code = rate_and_size >> 4;
    let rate_code = rate_and_size & 0x0f;
    let reserved = sync & 0xfe != 0xf8
        || block_size_code == 0
        || rate_code == 0x0f
        || channels_and_bits >> 4 > 0b1010
        || (channels_and_bits >> 1) & 0b111 == 0b011
        || channels_and_bits & 1 != 0;
    if reserved {
        return None;
    }
    // The frame or sample number, coded as UTF-8 is.
    let number_len = match number.leading_ones() {
        0 => 1,
        ones @ 2..=7 => usize::try_from(ones).ok()?,
        _ => return None,
    };
    let continued = bytes.get(5..4 + number_len)?;
    if continued.iter().any(|&byte| byte & 0xc0 != 0x80) {
        return None;
    }
    let len = 4
        + number_len
        + match block_size_code {
            0b0110 => 1,
            0b0111 => 2,
            _ => 0,
        }
        + match rate_code {
            0b1100 => 1,
            0b1101 | 0b1110 => 2,
            _ => 0,
        };
    let header_crc = *bytes.get(len)?;
    let mut crc = Crc8Ccitt::new(0);
    crc.process_buf_bytes(&bytes[..len]);
    (crc.crc() == header_crc).then_some(len + 1)
}

/// Where the `fLaC` marker of the file that `source` reads is: at its
/// start, or after an `ID3v2` tag there. `None` if it is not a FLAC file.
fn flac_stream_start<R: Read + Seek>(source: &mut R) -> io::Result<Option<u64>> {
    let mut head = [0_u8; 10];
    source.seek(SeekFrom::Start(0))?;
    if source.read_exact(&mut head).is_err() {
        return Ok(None);
    }
    let start = if head.starts_with(b"ID3") {
        // A synchsafe size of the tag after its 10-byte header, and a
        // 10-byte footer if the flags say so.
        let size = head[6..10]
            .iter()
            .fold(0_u64, |size, &byte| (size << 7) | u64::from(byte & 0x7f));
        let footer = if head[5] & 0x10 == 0 { 0 } else { 10 };
        10 + size + footer
    } else {
        0
    };
    let mut marker = [0_u8; 4];
    source.seek(SeekFrom::Start(start))?;
    if source.read_exact(&mut marker).is_err() || marker != *b"fLaC" {
        return Ok(None);
    }
    Ok(Some(start))
}

fn read_at<const N: usize>(source: &mut (impl Read + Seek), at: u64) -> io::Result<[u8; N]> {
    let mut bytes = [0_u8; N];
    source.seek(SeekFrom::Start(at))?;
    source.read_exact(&mut bytes)?;
    Ok(bytes)
}

fn read_vec_at(source: &mut (impl Read + Seek), at: u64, len: usize) -> io::Result<Vec<u8>> {
    let mut bytes = vec![0_u8; len];
    source.seek(SeekFrom::Start(at))?;
    source.read_exact(&mut bytes)?;
    Ok(bytes)
}

/// `inner` ending at `end`, for readers and seeks alike.
struct AudioEnd {
    inner: Box<dyn MediaSource>,
    end: u64,
    /// Where `inner` is.
    pos: u64,
}

impl Read for AudioEnd {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let left = usize::try_from(self.end.saturating_sub(self.pos)).unwrap_or(usize::MAX);
        let take = buf.len().min(left);
        if take == 0 {
            return Ok(0);
        }
        let read = self.inner.read(&mut buf[..take])?;
        self.pos += u64::try_from(read).unwrap_or(u64::MAX);
        Ok(read)
    }
}

impl Seek for AudioEnd {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let target = match to {
            SeekFrom::Start(offset) => Some(offset),
            SeekFrom::Current(delta) => self.pos.checked_add_signed(delta),
            SeekFrom::End(delta) => self.end.checked_add_signed(delta),
        }
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "seek before the start"))?;
        self.pos = self.inner.seek(SeekFrom::Start(target))?;
        Ok(self.pos)
    }
}

impl MediaSource for AudioEnd {
    fn is_seekable(&self) -> bool {
        true
    }

    fn byte_len(&self) -> Option<u64> {
        Some(self.end)
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_flac::{
        apev2_tag as apev2, apev2_tag_with, flac_bytes, id3v1_tag as id3v1,
        with_tag_marker_at_the_end,
    };
    use super::*;
    use std::io::Cursor;

    /// Interleaved 16-bit stereo samples of `frames` frames.
    fn samples(frames: usize) -> Vec<i32> {
        (0..frames * 2)
            .map(|i| i32::try_from((i * 7_919) % 20_001).unwrap() - 10_000)
            .collect()
    }

    /// A 44.1 kHz stereo FLAC of two whole blocks and a short one.
    fn audio() -> Vec<u8> {
        flac_bytes(&samples(2 * 4_096 + 1_000), 44_100, 2, 16, None)
    }

    fn read_all(bytes: Vec<u8>) -> (Option<u64>, Vec<u8>) {
        let mut source = without_trailing_tags(Box::new(Cursor::new(bytes)));
        let len = source.byte_len();
        let mut out = Vec::new();
        source.read_to_end(&mut out).expect("read");
        (len, out)
    }

    /// Asserts that `bytes` read as `expected`, `name` telling which.
    fn assert_reads(name: &str, bytes: Vec<u8>, expected: &[u8]) {
        let (len, read) = read_all(bytes);
        assert_eq!(len, Some(u64::try_from(expected.len()).unwrap()), "{name}");
        assert!(read == expected, "{name}: read {} bytes", read.len());
    }

    /// An `APEv1` tag of one item, which has no header.
    fn apev1() -> Vec<u8> {
        let mut tag = apev2(false);
        let footer = tag.len() - 32;
        tag[footer + 8..footer + 12].copy_from_slice(&1_000_u32.to_le_bytes());
        tag
    }

    #[test]
    fn a_flac_stream_ends_before_the_tags_after_its_frames() {
        let audio = audio();
        for (name, tags) in [
            ("ID3v1", id3v1()),
            ("APEv2 with a header", apev2(true)),
            ("APEv2 without a header", apev2(false)),
            ("APEv1", apev1()),
            ("APEv2 and ID3v1", [apev2(true), id3v1()].concat()),
            ("ID3v1 and APEv2", [id3v1(), apev2(false)].concat()),
        ] {
            assert_reads(name, [audio.clone(), tags].concat(), &audio);
        }
        // After an ID3v2 tag at the start too.
        let mut id3v2 = b"ID3\x04\x00\x00\x00\x00\x00\x05".to_vec();
        id3v2.extend([0_u8; 5]);
        let file = [id3v2, audio].concat();
        assert_reads("ID3v2 and ID3v1", [file.clone(), id3v1()].concat(), &file);
    }

    #[test]
    fn an_ape_tag_holding_tag_where_an_id3v1_tag_would_start_ends_the_audio_whole() {
        // A footer-only APEv2 tag whose value has `TAG` 128 bytes before
        // the end of the file: 96 bytes before the end of the value.
        let mut value = vec![b'x'; 120];
        value[120 - 96..120 - 93].copy_from_slice(b"TAG");
        let tag = apev2_tag_with(false, &value);
        assert_eq!(&tag[tag.len() - 128..tag.len() - 125], b"TAG");
        let audio = audio();
        assert_reads("APEv2", [audio.clone(), tag.clone()].concat(), &audio);
        assert_reads(
            "APEv2 and ID3v1",
            [audio.clone(), tag, id3v1()].concat(),
            &audio,
        );
    }

    #[test]
    fn tag_in_the_audio_of_the_last_frame_is_no_id3v1_tag() {
        let mut samples = samples(2 * 4_096 + 1_000);
        with_tag_marker_at_the_end(&mut samples);
        let audio = flac_bytes(&samples, 44_100, 2, 16, None);
        assert_eq!(&audio[audio.len() - 128..audio.len() - 125], b"TAG");
        assert_reads("TAG in the last frame", audio.clone(), &audio);
    }

    #[test]
    fn the_last_frame_is_looked_for_within_the_largest_frame_size() {
        let audio = audio();
        // The blocks are verbatim: a whole one is the largest frame.
        let largest =
            u32::try_from(flac_bytes(&samples(4_096), 44_100, 2, 16, None).len() - 42).unwrap();
        let with_largest = |largest: u32| {
            let mut bytes = audio.clone();
            bytes[15..18].copy_from_slice(&largest.to_be_bytes()[1..]);
            bytes
        };
        let told = with_largest(largest);
        assert_reads("told", [told.clone(), id3v1()].concat(), &told);
        // A largest frame shorter than the last frame: the frame is not
        // found, and the stream is left whole.
        let wrong = with_largest(64);
        let file = [wrong, id3v1()].concat();
        assert_reads("too short", file.clone(), &file);
    }

    #[test]
    fn a_damaged_ape_tag_leaves_the_stream_whole() {
        let audio = audio();
        let footer_field = |tag: &mut Vec<u8>, at: usize, value: u32| {
            let footer = tag.len() - 32;
            tag[footer + at..footer + at + 4].copy_from_slice(&value.to_le_bytes());
        };
        let size = |tag: &[u8]| {
            let footer = tag.len() - 32;
            u32::from_le_bytes(tag[footer + 12..footer + 16].try_into().unwrap())
        };
        let mut oversized = apev2(false);
        let larger = size(&oversized) + 64;
        footer_field(&mut oversized, 12, larger);
        let mut headless = apev2(false);
        footer_field(&mut headless, 20, APE_HAS_HEADER);
        let mut unknown_version = apev2(false);
        footer_field(&mut unknown_version, 8, 3_000);
        let mut miscounted = apev2(false);
        footer_field(&mut miscounted, 16, 2);
        let mut header_differs = apev2(true);
        header_differs[16..20].copy_from_slice(&2_u32.to_le_bytes());
        let mut reserved = apev2(false);
        let at = reserved.len() - 4;
        reserved[at] = 1;
        for (name, tag) in [
            ("a size that still fits", oversized),
            ("a header that is not there", headless),
            ("an unknown version", unknown_version),
            ("an item count of more items", miscounted),
            ("a header of another item count", header_differs),
            ("reserved bytes set", reserved),
        ] {
            let file = [audio.clone(), tag.clone()].concat();
            assert_reads(name, file.clone(), &file);
            let file = [audio.clone(), tag, id3v1()].concat();
            assert_reads(&format!("{name}, then ID3v1"), file.clone(), &file);
        }
    }

    #[test]
    fn other_files_and_untagged_flac_files_read_whole() {
        let mut wave = b"RIFF".to_vec();
        wave.extend([0_u8; 400]);
        // `TAG` 128 bytes before the end, but inside STREAMINFO: no tag
        // ends the stream before its header does.
        let mut short = audio()[..150].to_vec();
        short[22..25].copy_from_slice(b"TAG");
        for (name, bytes) in [
            ("FLAC", audio()),
            ("WAV with ID3v1", [wave, id3v1()].concat()),
            ("FLAC with TAG in its header", short),
        ] {
            assert_reads(name, bytes.clone(), &bytes);
        }
    }

    #[test]
    fn seeks_are_within_the_audio() {
        let audio = audio();
        let mut source =
            without_trailing_tags(Box::new(Cursor::new([audio.clone(), id3v1()].concat())));
        let end = u64::try_from(audio.len()).unwrap();
        assert_eq!(source.byte_len(), Some(end));
        assert_eq!(source.seek(SeekFrom::End(-4)).unwrap(), end - 4);
        let mut tail = Vec::new();
        source.read_to_end(&mut tail).unwrap();
        assert_eq!(tail, audio[audio.len() - 4..]);
        assert_eq!(source.seek(SeekFrom::Current(-10)).unwrap(), end - 10);
        assert_eq!(source.seek(SeekFrom::Start(4)).unwrap(), 4);
        let mut marker = [0_u8; 4];
        source.read_exact(&mut marker).unwrap();
        assert_eq!(marker, [0x80, 0, 0, 34]);
        assert!(source.seek(SeekFrom::Current(-100)).is_err());
    }
}
