// SPDX-License-Identifier: GPL-3.0-or-later

//! Finding the Vorbis comments lofty leaves out when it reads a file.
//!
//! lofty 0.24 reads Vorbis comments leniently: it skips a comment it cannot
//! represent, so writing back what it read deletes that comment. This module
//! reads a file's own comment blocks and counts the comments lofty skips,
//! using the rules lofty applies. FLAC `PICTURE` blocks are read and written
//! with the comments, so the ones lofty skips are counted too.

use std::fs::File;
use std::io::{self, BufReader, Read, Seek, SeekFrom};
use std::path::Path;

use data_encoding::BASE64;
use lofty::config::ParseOptions;
use lofty::file::FileType;
use lofty::picture::Picture;

const FLAC_VORBIS_COMMENT_BLOCK: u8 = 4;
const FLAC_PICTURE_BLOCK: u8 = 6;
const VORBIS_COMMENT_HEADER: &[u8] = b"\x03vorbis";
const OPUS_COMMENT_HEADER: &[u8] = b"OpusTags";

/// The comments of a file that lofty does not read, by the reason, and the
/// FLAC picture blocks it does not read.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub(crate) struct SkippedComments {
    /// Names with a byte outside `0x20..=0x7D`, the range lofty allows.
    invalid_name: usize,
    /// Comments without the `=` that ends the name.
    no_separator: usize,
    /// Values that are not UTF-8.
    non_utf8_value: usize,
    /// `METADATA_BLOCK_PICTURE` and `COVERART` values lofty cannot decode.
    unreadable_picture: usize,
    /// Comments in a FLAC `VORBIS_COMMENT` block other than the last one,
    /// which is the only one lofty reads.
    other_block: usize,
    /// FLAC `PICTURE` blocks lofty cannot decode. Writing the comments
    /// replaces every picture block with the pictures lofty read.
    unreadable_picture_block: usize,
}

impl SkippedComments {
    fn total(&self) -> usize {
        self.invalid_name
            + self.no_separator
            + self.non_utf8_value
            + self.unreadable_picture
            + self.other_block
    }

    /// Why saving would delete comments or picture blocks, when it would.
    pub(crate) fn save_refusal(&self) -> Option<String> {
        let comments = self.total();
        let pictures = self.unreadable_picture_block;
        if comments == 0 && pictures == 0 {
            return None;
        }
        let plural = |count: usize, one: &str, many: &str| {
            format!("{count} {}", if count == 1 { one } else { many })
        };
        let mut lost = Vec::new();
        if comments > 0 {
            lost.push(plural(comments, "comment", "comments"));
        }
        if pictures > 0 {
            lost.push(plural(pictures, "picture block", "picture blocks"));
        }
        let reasons = [
            (self.invalid_name, "with an invalid name"),
            (self.no_separator, "without \"=\""),
            (self.non_utf8_value, "with a value that is not UTF-8"),
            (
                self.unreadable_picture,
                "with a picture that cannot be read",
            ),
            (self.other_block, "in an extra comment block"),
        ]
        .into_iter()
        .filter(|&(count, _)| count > 0)
        .map(|(count, reason)| format!("{count} {reason}"))
        .collect::<Vec<_>>()
        .join(", ");
        let reasons = if reasons.is_empty() {
            reasons
        } else {
            format!(" ({reasons})")
        };
        let them = if comments + pictures == 1 {
            "it"
        } else {
            "them"
        };
        Some(format!(
            "{} in this file can't be read safely{reasons}; saving would delete {them}",
            lost.join(" and ")
        ))
    }

    fn count(&mut self, comment: &[u8]) {
        // lofty splits at the first `=`, so the name never holds one.
        let Some(separator) = comment.iter().position(|&byte| byte == b'=') else {
            self.no_separator += 1;
            return;
        };
        let (name, value) = (&comment[..separator], &comment[separator + 1..]);
        if name.eq_ignore_ascii_case(b"METADATA_BLOCK_PICTURE") {
            let mode = ParseOptions::DEFAULT_PARSING_MODE;
            if Picture::from_flac_bytes(value, true, mode).is_err() {
                self.unreadable_picture += 1;
            }
        } else if name.eq_ignore_ascii_case(b"COVERART") {
            if BASE64.decode(value).is_err() {
                self.unreadable_picture += 1;
            }
        } else if !name.iter().all(|byte| (b' '..=b'}').contains(byte)) {
            self.invalid_name += 1;
        } else if std::str::from_utf8(value).is_err() {
            self.non_utf8_value += 1;
        }
    }
}

/// Count the Vorbis comments and FLAC picture blocks of `path` that lofty
/// does not read.
///
/// The comment blocks are read in a stream, one comment at a time, so a
/// large embedded picture is held once at most. Lengths are checked against
/// the block or file they are in before anything is read for them.
///
/// # Errors
///
/// Returns a message when the comment or picture blocks cannot be read,
/// such as when the file is truncated.
pub(crate) fn skipped_comments(
    path: &Path,
    file_type: FileType,
) -> Result<SkippedComments, String> {
    let scan_file = || {
        let file = File::open(path)?;
        let file_len = file.metadata()?.len();
        scan(&mut BufReader::new(file), file_len, file_type)
    };
    scan_file().map_err(|err| format!("its tags cannot be read: {err}"))
}

fn scan(
    reader: &mut (impl Read + Seek),
    file_len: u64,
    file_type: FileType,
) -> io::Result<SkippedComments> {
    match file_type {
        FileType::Flac => scan_flac(reader, file_len),
        FileType::Vorbis => scan_ogg(reader, file_len, VORBIS_COMMENT_HEADER),
        FileType::Opus => scan_ogg(reader, file_len, OPUS_COMMENT_HEADER),
        // Speex comments follow the header packet without a signature.
        FileType::Speex => scan_ogg(reader, file_len, b""),
        _ => Ok(SkippedComments::default()),
    }
}

/// Count the skipped comments and picture blocks of the FLAC stream `reader`
/// holds. lofty reads only the last nonempty `VORBIS_COMMENT` block, and
/// writing replaces all of them, so the comments of the others are lost too.
/// Writing also replaces every `PICTURE` block with the pictures lofty read,
/// so the blocks it skips are lost.
fn scan_flac(reader: &mut (impl Read + Seek), file_len: u64) -> io::Result<SkippedComments> {
    skip_id3v2(reader)?;
    let mut marker = [0; 4];
    read_exact(reader, &mut marker)?;
    if &marker != b"fLaC" {
        return Err(invalid("the FLAC stream marker is missing"));
    }
    let mut position = reader.stream_position()?;
    // The comment count and skipped comments of the last block read.
    let mut last: Option<(u64, SkippedComments)> = None;
    let mut other_block = 0;
    let mut unreadable_picture_block = 0;
    loop {
        let mut header = [0; 4];
        read_exact(reader, &mut header)?;
        position += 4;
        let size = u64::from(u32::from_be_bytes([0, header[1], header[2], header[3]]));
        let block_type = header[0] & 0x7F;
        if block_type == FLAC_VORBIS_COMMENT_BLOCK && size > 0 {
            if position + size > file_len {
                return Err(invalid("a comment block runs past the end of the file"));
            }
            let block = scan_comments(&mut reader.by_ref().take(size), size)?;
            if let Some((count, _)) = last.replace(block) {
                other_block += count;
            }
        } else if block_type == FLAC_PICTURE_BLOCK && size > 0 {
            if position + size > file_len {
                return Err(invalid("a picture block runs past the end of the file"));
            }
            // lofty skips empty blocks, so they hold nothing to lose. Others
            // are read whole, as lofty does, and are under 16 MiB.
            let mut content = Vec::new();
            reader.by_ref().take(size).read_to_end(&mut content)?;
            if u64::try_from(content.len()) != Ok(size) {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "a picture block is truncated",
                ));
            }
            let mode = ParseOptions::DEFAULT_PARSING_MODE;
            if Picture::from_flac_bytes(&content, false, mode).is_err() {
                unreadable_picture_block += 1;
            }
        }
        position += size;
        reader.seek(SeekFrom::Start(position))?;
        if header[0] & 0x80 != 0 {
            break;
        }
    }
    let mut skipped = last.map(|(_, skipped)| skipped).unwrap_or_default();
    skipped.other_block = usize::try_from(other_block).unwrap_or(usize::MAX);
    skipped.unreadable_picture_block = unreadable_picture_block;
    Ok(skipped)
}

/// Skip an `ID3v2` tag at the start of `reader`, as lofty does for FLAC.
fn skip_id3v2(reader: &mut (impl Read + Seek)) -> io::Result<()> {
    let mut header = [0; 10];
    let is_id3v2 = match reader.read_exact(&mut header) {
        Ok(()) => &header[..3] == b"ID3" && (2..=4).contains(&header[3]),
        Err(err) if err.kind() == io::ErrorKind::UnexpectedEof => false,
        Err(err) => return Err(err),
    };
    if !is_id3v2 {
        reader.seek(SeekFrom::Start(0))?;
        return Ok(());
    }
    // The size is a synchsafe integer: seven bits in each byte.
    let size = header[6..]
        .iter()
        .fold(0_i64, |size, &byte| (size << 7) | i64::from(byte & 0x7F));
    let footer = header[3] >= 3 && header[5] & 0x10 != 0;
    reader.seek(SeekFrom::Current(size + if footer { 10 } else { 0 }))?;
    Ok(())
}

/// Count the skipped comments of the Ogg stream `reader` holds. Its second
/// packet is the comment header, which starts with `signature`.
fn scan_ogg(
    reader: &mut impl Read,
    file_len: u64,
    signature: &[u8],
) -> io::Result<SkippedComments> {
    let mut packets = OggPackets::new(reader);
    // The first packet identifies the codec.
    io::copy(&mut packets, &mut io::sink())?;
    packets.next_packet();
    let mut found = vec![0; signature.len()];
    read_exact(&mut packets, &mut found)?;
    if found != signature {
        return Err(invalid("the comment header is missing"));
    }
    Ok(scan_comments(&mut packets, file_len)?.1)
}

/// Count the comments of a comment block and the skipped ones among them.
/// No length in the block can be more than `limit`, the length of the block
/// or, when that is not known, of the file.
fn scan_comments(reader: &mut impl Read, limit: u64) -> io::Result<(u64, SkippedComments)> {
    let vendor_len = read_length(reader, limit)?;
    let skipped_vendor = io::copy(&mut reader.by_ref().take(vendor_len), &mut io::sink())?;
    if skipped_vendor != vendor_len {
        return Err(truncated());
    }
    let count = u64::from(read_u32(reader)?);
    let mut skipped = SkippedComments::default();
    for _ in 0..count {
        let len = read_length(reader, limit)?;
        let mut comment = Vec::new();
        reader.by_ref().take(len).read_to_end(&mut comment)?;
        if u64::try_from(comment.len()) != Ok(len) {
            return Err(truncated());
        }
        skipped.count(&comment);
    }
    Ok((count, skipped))
}

fn read_length(reader: &mut impl Read, limit: u64) -> io::Result<u64> {
    let len = u64::from(read_u32(reader)?);
    if len > limit {
        return Err(invalid(
            "a length in the comment block is larger than the block",
        ));
    }
    Ok(len)
}

fn read_u32(reader: &mut impl Read) -> io::Result<u32> {
    let mut bytes = [0; 4];
    read_exact(reader, &mut bytes)?;
    Ok(u32::from_le_bytes(bytes))
}

fn read_exact(reader: &mut impl Read, buf: &mut [u8]) -> io::Result<()> {
    reader.read_exact(buf).map_err(|err| {
        if err.kind() == io::ErrorKind::UnexpectedEof {
            truncated()
        } else {
            err
        }
    })
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn truncated() -> io::Error {
    io::Error::new(
        io::ErrorKind::UnexpectedEof,
        "the comment block is truncated",
    )
}

/// The packets of an Ogg stream, read one at a time and page by page. Pages
/// are read in order whatever logical stream they belong to, as lofty does.
struct OggPackets<R> {
    reader: R,
    /// The segment lengths of the current page.
    lacing: Vec<u8>,
    next_segment: usize,
    /// Bytes of the current segment not read yet.
    segment_left: usize,
    /// The current segment is the last one of its packet.
    packet_ends: bool,
}

impl<R: Read> OggPackets<R> {
    fn new(reader: R) -> Self {
        Self {
            reader,
            lacing: Vec::new(),
            next_segment: 0,
            segment_left: 0,
            packet_ends: false,
        }
    }

    /// Start reading the next packet. The current one must have been read
    /// to its end.
    fn next_packet(&mut self) {
        self.packet_ends = false;
    }

    fn read_page_header(&mut self) -> io::Result<()> {
        let mut header = [0; 27];
        read_exact(&mut self.reader, &mut header)?;
        if &header[..4] != b"OggS" {
            return Err(invalid("an Ogg page is missing its capture pattern"));
        }
        self.lacing.resize(usize::from(header[26]), 0);
        read_exact(&mut self.reader, &mut self.lacing)?;
        self.next_segment = 0;
        Ok(())
    }
}

impl<R: Read> Read for OggPackets<R> {
    /// Read from the current packet. Returns 0 at its end.
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        while self.segment_left == 0 {
            if self.packet_ends {
                return Ok(0);
            }
            if let Some(&len) = self.lacing.get(self.next_segment) {
                self.next_segment += 1;
                self.segment_left = usize::from(len);
                // A segment shorter than 255 bytes ends its packet.
                self.packet_ends = len < 255;
            } else {
                self.read_page_header()?;
            }
        }
        let wanted = buf.len().min(self.segment_left);
        let read = self.reader.read(&mut buf[..wanted])?;
        if read == 0 {
            return Err(truncated());
        }
        self.segment_left -= read;
        Ok(read)
    }
}

/// Files with Vorbis comments as given, which lofty cannot write.
#[cfg(test)]
pub(crate) mod fixtures {
    /// A 0.1 s Opus tone without comments.
    pub(crate) const OPUS: &[u8] = include_bytes!("analysis/fixtures/tag-tone.opus");
    /// A 0.1 s Ogg Vorbis tone without comments.
    pub(crate) const VORBIS: &[u8] = include_bytes!("analysis/fixtures/tag-tone.ogg");

    fn le_u32(len: usize) -> [u8; 4] {
        u32::try_from(len).expect("fixture length").to_le_bytes()
    }

    /// A Vorbis comment block holding `comments` byte for byte.
    pub(crate) fn comment_block(comments: &[&[u8]]) -> Vec<u8> {
        let vendor = b"ferrous-test";
        let mut block = le_u32(vendor.len()).to_vec();
        block.extend_from_slice(vendor);
        block.extend_from_slice(&le_u32(comments.len()));
        for comment in comments {
            block.extend_from_slice(&le_u32(comment.len()));
            block.extend_from_slice(comment);
        }
        block
    }

    /// A FLAC stream with these `VORBIS_COMMENT` blocks and no audio frames.
    pub(crate) fn flac(comment_blocks: &[&[u8]]) -> Vec<u8> {
        let blocks = comment_blocks
            .iter()
            .map(|&block| (super::FLAC_VORBIS_COMMENT_BLOCK, block))
            .collect::<Vec<_>>();
        flac_blocks(&blocks)
    }

    /// A FLAC stream with these metadata blocks, given by their type, and no
    /// audio frames.
    pub(crate) fn flac_blocks(blocks: &[(u8, &[u8])]) -> Vec<u8> {
        let mut bytes = b"fLaC".to_vec();
        // STREAMINFO: 4096-sample blocks, no frame sizes, then the packed fields.
        bytes.extend_from_slice(&[0x00, 0, 0, 34]);
        bytes.extend_from_slice(&4096_u16.to_be_bytes());
        bytes.extend_from_slice(&4096_u16.to_be_bytes());
        bytes.extend_from_slice(&[0; 6]);
        let (sample_rate, channels, bits_per_sample, total_samples) =
            (44_100_u64, 2_u64, 16_u64, 44_100_u64);
        let packed = (sample_rate << 44)
            | ((channels - 1) << 41)
            | ((bits_per_sample - 1) << 36)
            | total_samples;
        bytes.extend_from_slice(&packed.to_be_bytes());
        bytes.extend_from_slice(&[0; 16]);
        for (index, &(block_type, block)) in blocks.iter().enumerate() {
            let last = if index + 1 == blocks.len() { 0x80 } else { 0 };
            bytes.push(last | block_type);
            bytes.extend_from_slice(&u32::try_from(block.len()).expect("block").to_be_bytes()[1..]);
            bytes.extend_from_slice(block);
        }
        bytes
    }

    /// The type of a FLAC `VORBIS_COMMENT` block, for [`flac_blocks`].
    pub(crate) const VORBIS_COMMENT: u8 = super::FLAC_VORBIS_COMMENT_BLOCK;
    /// The type of a FLAC `PICTURE` block, for [`flac_blocks`].
    pub(crate) const PICTURE: u8 = super::FLAC_PICTURE_BLOCK;

    /// The content of a FLAC `PICTURE` block holding `data` as a JPEG front
    /// cover, which lofty reads.
    pub(crate) fn picture_block(data: &[u8]) -> Vec<u8> {
        use lofty::picture::{MimeType, Picture, PictureInformation, PictureType};

        Picture::unchecked(data.to_vec())
            .mime_type(MimeType::Jpeg)
            .pic_type(PictureType::CoverFront)
            .build()
            .as_flac_bytes(PictureInformation::default(), false)
    }

    /// The content of a FLAC `PICTURE` block whose MIME type length runs past
    /// the block, which lofty cannot read.
    pub(crate) fn malformed_picture_block() -> Vec<u8> {
        let mut block = picture_block(b"\xff\xd8\xff\xd9");
        let len = u32::try_from(block.len()).expect("length");
        block[4..8].copy_from_slice(&len.to_be_bytes());
        block
    }

    /// The pages of an Ogg stream.
    pub(crate) fn ogg_pages(stream: &[u8]) -> Vec<&[u8]> {
        let mut pages = Vec::new();
        let mut rest = stream;
        while !rest.is_empty() {
            let segments = usize::from(rest[26]);
            let data_len = rest[27..27 + segments]
                .iter()
                .map(|&len| usize::from(len))
                .sum::<usize>();
            let (page, next) = rest.split_at(27 + segments + data_len);
            pages.push(page);
            rest = next;
        }
        pages
    }

    /// `stream`, an Ogg stream that starts with `header_packets` header
    /// packets, with `comment_header` as its second packet. The header pages
    /// are rebuilt and the others renumbered after them.
    pub(crate) fn ogg_with_comment_header(
        stream: &[u8],
        header_packets: usize,
        comment_header: &[u8],
    ) -> Vec<u8> {
        let pages = ogg_pages(stream);
        let mut packets = Vec::new();
        let mut packet = Vec::new();
        let mut header_pages = 0;
        while packets.len() < header_packets {
            let page = pages[header_pages];
            header_pages += 1;
            let segments = usize::from(page[26]);
            let mut data = &page[27 + segments..];
            for &len in &page[27..27 + segments] {
                let (segment, rest) = data.split_at(usize::from(len));
                packet.extend_from_slice(segment);
                data = rest;
                if len < 255 {
                    packets.push(std::mem::take(&mut packet));
                }
            }
        }
        assert_eq!(packets.len(), header_packets, "headers end with a page");
        packets[1] = comment_header.to_vec();

        let serial: [u8; 4] = pages[0][14..18].try_into().expect("serial");
        let mut sequence = 0;
        let mut out = Vec::new();
        // The identification header has the first page to itself.
        push_packets(&mut out, serial, &mut sequence, &packets[..1], 0x02);
        push_packets(&mut out, serial, &mut sequence, &packets[1..], 0);
        for page in &pages[header_pages..] {
            let mut page = page.to_vec();
            page[18..22].copy_from_slice(&sequence.to_le_bytes());
            sequence += 1;
            set_crc(&mut page);
            out.extend_from_slice(&page);
        }
        out
    }

    /// Append pages holding `packets`, the first one with `flags`.
    fn push_packets(
        out: &mut Vec<u8>,
        serial: [u8; 4],
        sequence: &mut u32,
        packets: &[Vec<u8>],
        mut flags: u8,
    ) {
        let mut lacing = Vec::new();
        for packet in packets {
            lacing.extend(std::iter::repeat_n(255, packet.len() / 255));
            lacing.push(u8::try_from(packet.len() % 255).expect("segment"));
        }
        let mut data = packets.concat();
        for page_lacing in lacing.chunks(255) {
            let len = page_lacing
                .iter()
                .map(|&len| usize::from(len))
                .sum::<usize>();
            let rest = data.split_off(len);
            let mut page = b"OggS\0".to_vec();
            page.push(flags);
            page.extend_from_slice(&[0; 8]);
            page.extend_from_slice(&serial);
            page.extend_from_slice(&sequence.to_le_bytes());
            page.extend_from_slice(&[0; 4]);
            page.push(u8::try_from(page_lacing.len()).expect("segments"));
            page.extend_from_slice(page_lacing);
            page.extend_from_slice(&data);
            set_crc(&mut page);
            out.extend_from_slice(&page);
            data = rest;
            *sequence += 1;
            // Later pages continue a packet when this one ends in the middle.
            flags = u8::from(page_lacing.last() == Some(&255));
        }
    }

    fn set_crc(page: &mut [u8]) {
        page[22..26].fill(0);
        let crc = ogg_crc(page);
        page[22..26].copy_from_slice(&crc.to_le_bytes());
    }

    /// The CRC of an Ogg page, computed with its own CRC field zeroed.
    pub(crate) fn ogg_crc(page: &[u8]) -> u32 {
        page.iter().fold(0, |crc, &byte| {
            (0..8).fold(crc ^ (u32::from(byte) << 24), |crc, _| {
                if crc & 0x8000_0000 == 0 {
                    crc << 1
                } else {
                    (crc << 1) ^ 0x04c1_1db7
                }
            })
        })
    }

    /// The Opus fixture with `comments` as given.
    pub(crate) fn opus(comments: &[&[u8]]) -> Vec<u8> {
        let header = [super::OPUS_COMMENT_HEADER, &comment_block(comments)].concat();
        ogg_with_comment_header(OPUS, 2, &header)
    }

    /// The Ogg Vorbis fixture with `comments` as given.
    pub(crate) fn vorbis(comments: &[&[u8]]) -> Vec<u8> {
        // The comment header ends with a framing bit.
        let header = [super::VORBIS_COMMENT_HEADER, &comment_block(comments), &[1]].concat();
        ogg_with_comment_header(VORBIS, 3, &header)
    }
}

#[cfg(test)]
mod tests {
    use super::fixtures::{self, comment_block};
    use super::*;
    use lofty::file::AudioFile;
    use lofty::flac::FlacFile;
    use lofty::ogg::{OggPictureStorage, OpusFile, VorbisComments, VorbisFile};
    use lofty::picture::{MimeType, PictureInformation, PictureType};
    use std::io::Cursor;

    /// One comment lofty skips for each of the reasons in the issue, between
    /// comments it reads, including one with an empty name.
    const COMMENTS: [&[u8]; 6] = [
        b"TITLE=Title",
        b"N\xc3\x84ME=non-ASCII name",
        b"no separator",
        b"=empty name",
        b"ARTIST=\xff not UTF-8",
        b"ALBUM=Album",
    ];

    fn skipped_reasons() -> SkippedComments {
        SkippedComments {
            invalid_name: 1,
            no_separator: 1,
            non_utf8_value: 1,
            ..SkippedComments::default()
        }
    }

    fn scan_bytes(file_type: FileType, bytes: &[u8]) -> io::Result<SkippedComments> {
        let len = u64::try_from(bytes.len()).expect("length");
        scan(&mut Cursor::new(bytes), len, file_type)
    }

    /// The comments lofty reads from `bytes`, as it reads them for a save.
    fn lofty_comments(file_type: FileType, bytes: &[u8]) -> VorbisComments {
        let options = ParseOptions::new().read_properties(false);
        let mut reader = Cursor::new(bytes);
        match file_type {
            FileType::Flac => FlacFile::read_from(&mut reader, options)
                .expect("read FLAC")
                .vorbis_comments()
                .cloned()
                .expect("comments"),
            FileType::Opus => OpusFile::read_from(&mut reader, options)
                .expect("read Opus")
                .vorbis_comments()
                .clone(),
            FileType::Vorbis => VorbisFile::read_from(&mut reader, options)
                .expect("read Vorbis")
                .vorbis_comments()
                .clone(),
            _ => unreachable!("not a Vorbis comment format"),
        }
    }

    fn each_format(comments: &[&[u8]]) -> [(FileType, Vec<u8>); 3] {
        [
            (FileType::Flac, fixtures::flac(&[&comment_block(comments)])),
            (FileType::Opus, fixtures::opus(comments)),
            (FileType::Vorbis, fixtures::vorbis(comments)),
        ]
    }

    #[test]
    fn fixture_pages_have_valid_checksums() {
        let rebuilt = fixtures::opus(&COMMENTS);
        for stream in [fixtures::OPUS, fixtures::VORBIS, &rebuilt] {
            for page in fixtures::ogg_pages(stream) {
                let mut zeroed = page.to_vec();
                zeroed[22..26].fill(0);
                assert_eq!(fixtures::ogg_crc(&zeroed).to_le_bytes(), page[22..26]);
            }
        }
    }

    #[test]
    fn counts_the_comments_lofty_skips() {
        for (file_type, bytes) in each_format(&COMMENTS) {
            assert_eq!(
                scan_bytes(file_type, &bytes).expect("scan"),
                skipped_reasons(),
                "{file_type:?}"
            );
            let read = lofty_comments(file_type, &bytes);
            assert_eq!(
                read.items().collect::<Vec<_>>(),
                [("TITLE", "Title"), ("", "empty name"), ("ALBUM", "Album")],
                "{file_type:?}"
            );
        }
    }

    #[test]
    fn readable_comments_are_not_counted() {
        for (file_type, bytes) in each_format(&[b"TITLE=Title", b"=empty name", b"~=tilde"]) {
            let skipped = scan_bytes(file_type, &bytes).expect("scan");
            // lofty does not allow `~` in a name.
            assert_eq!(
                skipped,
                SkippedComments {
                    invalid_name: 1,
                    ..SkippedComments::default()
                }
            );
            assert_eq!(lofty_comments(file_type, &bytes).items().len(), 2);
        }
        let empty = fixtures::flac(&[&comment_block(&[])]);
        let skipped = scan_bytes(FileType::Flac, &empty).expect("scan");
        assert_eq!(skipped.save_refusal(), None);
        let no_block = fixtures::flac(&[]);
        let mut no_block = no_block;
        // STREAMINFO is the last block then.
        no_block[4] |= 0x80;
        let skipped = scan_bytes(FileType::Flac, &no_block).expect("scan");
        assert_eq!(skipped, SkippedComments::default());
    }

    #[test]
    fn counts_pictures_lofty_cannot_decode() {
        let picture = Picture::unchecked(b"\xff\xd8\xff\xd9".to_vec())
            .mime_type(MimeType::Jpeg)
            .pic_type(PictureType::CoverFront)
            .build();
        let encoded = picture.as_flac_bytes(PictureInformation::default(), true);
        let block_picture = [b"METADATA_BLOCK_PICTURE=".as_slice(), &encoded].concat();
        // lofty 0.24 panics on `COVERART` images shorter than 8 bytes.
        let cover_art = format!(
            "COVERART={}",
            BASE64.encode(b"\xff\xd8\xff\xe0\0\x10JFIF\xff\xd9")
        );
        let comments: [&[u8]; 4] = [
            &block_picture,
            b"metadata_block_picture=not base64!",
            cover_art.as_bytes(),
            b"COVERART=@@@",
        ];
        for (file_type, bytes) in each_format(&comments) {
            let skipped = scan_bytes(file_type, &bytes).expect("scan");
            assert_eq!(
                skipped,
                SkippedComments {
                    unreadable_picture: 2,
                    ..SkippedComments::default()
                }
            );
            assert_eq!(lofty_comments(file_type, &bytes).pictures().len(), 2);
        }
    }

    #[test]
    fn counts_comments_in_flac_blocks_lofty_does_not_read() {
        let bytes = fixtures::flac(&[
            &comment_block(&[b"TITLE=First", b"ARTIST=First"]),
            &comment_block(&[]),
            &comment_block(&[b"TITLE=Last", b"no separator"]),
        ]);
        let skipped = scan_bytes(FileType::Flac, &bytes).expect("scan");
        assert_eq!(
            skipped,
            SkippedComments {
                no_separator: 1,
                other_block: 2,
                ..SkippedComments::default()
            }
        );
        let read = lofty_comments(FileType::Flac, &bytes);
        assert_eq!(read.items().collect::<Vec<_>>(), [("TITLE", "Last")]);
    }

    #[test]
    fn counts_picture_blocks_lofty_cannot_read() {
        let comments = comment_block(&[b"TITLE=Title"]);
        let readable = fixtures::picture_block(b"\xff\xd8\xff\xd9");
        let malformed = fixtures::malformed_picture_block();
        // lofty needs at least 32 bytes for a picture.
        let short = readable[..31].to_vec();
        let bytes = fixtures::flac_blocks(&[
            (fixtures::PICTURE, &readable),
            (FLAC_VORBIS_COMMENT_BLOCK, &comments),
            (fixtures::PICTURE, &malformed),
            (fixtures::PICTURE, &[]),
            (fixtures::PICTURE, &short),
        ]);
        let skipped = scan_bytes(FileType::Flac, &bytes).expect("scan");
        assert_eq!(
            skipped,
            SkippedComments {
                unreadable_picture_block: 2,
                ..SkippedComments::default()
            }
        );
        let options = ParseOptions::new().read_properties(false);
        let file = FlacFile::read_from(&mut Cursor::new(&bytes), options).expect("read FLAC");
        assert_eq!(file.pictures().len(), 1);
    }

    #[test]
    fn readable_picture_blocks_are_not_counted() {
        let readable = fixtures::picture_block(b"\xff\xd8\xff\xd9");
        let bytes = fixtures::flac_blocks(&[
            (fixtures::PICTURE, &readable),
            (fixtures::PICTURE, &readable),
        ]);
        let skipped = scan_bytes(FileType::Flac, &bytes).expect("scan");
        assert_eq!(skipped, SkippedComments::default());
        assert_eq!(skipped.save_refusal(), None);
    }

    #[test]
    fn skips_an_id3v2_tag_before_a_flac_stream() {
        // An empty ID3v2.4 tag with a footer, then the stream.
        let mut bytes = b"ID3\x04\x00\x10\x00\x00\x00\x00".to_vec();
        bytes.extend_from_slice(b"3DI\x04\x00\x10\x00\x00\x00\x00");
        bytes.extend_from_slice(&fixtures::flac(&[&comment_block(&COMMENTS)]));
        assert_eq!(
            scan_bytes(FileType::Flac, &bytes).expect("scan"),
            skipped_reasons()
        );
        assert_eq!(lofty_comments(FileType::Flac, &bytes).items().len(), 3);
    }

    #[test]
    fn reads_comment_headers_that_span_pages() {
        let mut long = b"LYRICS=".to_vec();
        long.resize(200_000, b'x');
        let mut comments = vec![long.as_slice()];
        comments.extend(COMMENTS);
        for (file_type, original, bytes) in [
            (FileType::Opus, fixtures::OPUS, fixtures::opus(&comments)),
            (
                FileType::Vorbis,
                fixtures::VORBIS,
                fixtures::vorbis(&comments),
            ),
        ] {
            let added_pages =
                fixtures::ogg_pages(&bytes).len() - fixtures::ogg_pages(original).len();
            assert!(added_pages >= 3, "{file_type:?}");
            assert_eq!(
                scan_bytes(file_type, &bytes).expect("scan"),
                skipped_reasons(),
                "{file_type:?}"
            );
            assert_eq!(lofty_comments(file_type, &bytes).items().len(), 4);
        }
    }

    #[test]
    fn malformed_comment_blocks_are_errors() {
        let block = comment_block(&COMMENTS);
        let mut overlong = block.clone();
        // The first comment's length, after the vendor and the count.
        overlong[20..24].copy_from_slice(&u32::MAX.to_le_bytes());
        let mut too_many = block.clone();
        too_many[16..20].copy_from_slice(&1000_u32.to_le_bytes());
        let mut long_vendor = block.clone();
        long_vendor[..4].copy_from_slice(&u32::MAX.to_le_bytes());
        let flac = fixtures::flac(&[&block]);
        let opus = fixtures::opus(&COMMENTS);
        let mut wrong_signature = opus.clone();
        let second_page = fixtures::ogg_pages(&opus)[0].len();
        // The second page's data starts after its header and one segment.
        wrong_signature[second_page + 28] = b'X';
        let mut missing_page = opus.clone();
        missing_page[second_page] = b'X';
        let picture =
            fixtures::flac_blocks(&[(fixtures::PICTURE, &fixtures::malformed_picture_block())]);
        let cases = [
            (
                FileType::Flac,
                fixtures::flac(&[&overlong]),
                "larger than the block",
            ),
            (FileType::Flac, fixtures::flac(&[&too_many]), "truncated"),
            (
                FileType::Flac,
                fixtures::flac(&[&long_vendor]),
                "larger than the block",
            ),
            (
                FileType::Flac,
                flac[..flac.len() - 10].to_vec(),
                "past the end",
            ),
            (FileType::Flac, flac[..10].to_vec(), "truncated"),
            (
                FileType::Flac,
                picture[..picture.len() - 1].to_vec(),
                "a picture block runs past the end",
            ),
            (FileType::Flac, b"OggS".to_vec(), "marker is missing"),
            (
                FileType::Opus,
                opus[..second_page + 60].to_vec(),
                "truncated",
            ),
            (
                FileType::Opus,
                fixtures::opus(&[]).split_at(second_page + 40).0.to_vec(),
                "truncated",
            ),
            (FileType::Opus, wrong_signature, "comment header is missing"),
            (FileType::Opus, missing_page, "capture pattern"),
            (FileType::Opus, Vec::new(), "truncated"),
            (
                FileType::Vorbis,
                fixtures::opus(&COMMENTS),
                "comment header is missing",
            ),
        ];
        for (file_type, bytes, expected) in cases {
            let error = scan_bytes(file_type, &bytes).expect_err(expected);
            assert!(error.to_string().contains(expected), "{error}");
        }
        let opus_overlong = fixtures::ogg_with_comment_header(
            fixtures::OPUS,
            2,
            &[OPUS_COMMENT_HEADER, &overlong].concat(),
        );
        let error = scan_bytes(FileType::Opus, &opus_overlong).expect_err("overlong");
        assert!(
            error.to_string().contains("larger than the block"),
            "{error}"
        );
    }

    #[test]
    fn unreadable_files_are_errors() {
        let error = skipped_comments(Path::new("/nonexistent/song.flac"), FileType::Flac)
            .expect_err("missing file");
        assert!(error.starts_with("its tags cannot be read: "), "{error}");
    }

    #[test]
    fn refusals_name_the_count_and_the_reasons() {
        assert_eq!(
            skipped_reasons().save_refusal().as_deref(),
            Some(
                "3 comments in this file can't be read safely (1 with an invalid name, \
                 1 without \"=\", 1 with a value that is not UTF-8); saving would delete them"
            )
        );
        let one = SkippedComments {
            other_block: 1,
            ..SkippedComments::default()
        };
        assert_eq!(
            one.save_refusal().as_deref(),
            Some(
                "1 comment in this file can't be read safely (1 in an extra comment block); \
                 saving would delete it"
            )
        );
        let picture = SkippedComments {
            unreadable_picture_block: 1,
            ..SkippedComments::default()
        };
        assert_eq!(
            picture.save_refusal().as_deref(),
            Some("1 picture block in this file can't be read safely; saving would delete it")
        );
        let both = SkippedComments {
            no_separator: 1,
            unreadable_picture_block: 2,
            ..SkippedComments::default()
        };
        assert_eq!(
            both.save_refusal().as_deref(),
            Some(
                "1 comment and 2 picture blocks in this file can't be read safely \
                 (1 without \"=\"); saving would delete them"
            )
        );
        assert_eq!(SkippedComments::default().save_refusal(), None);
    }
}
