// SPDX-License-Identifier: GPL-3.0-or-later

//! The end of the audio of a FLAC file, before the tags appended after it.
//!
//! A FLAC file may end with an `ID3v1` tag or an `APEv2` tag after its last
//! frame. Symphonia's FLAC reader takes those bytes for part of the last
//! frame, whose checksum then fails, and drops the frame: a decode ends a
//! frame short of the length in the header. Reading the file through
//! [`without_trailing_tags`] ends the stream where the last frame ends.

use std::io::{self, Read, Seek, SeekFrom};

use symphonia::core::io::MediaSource;

/// The size of an `ID3v1` tag, which starts with `TAG`.
const ID3V1_LEN: u64 = 128;
/// The size of an `APEv2` header or footer, which starts with `APETAGEX`.
const APE_FOOTER_LEN: u64 = 32;
/// The `APEv2` flag that the tag has a header before its items.
const APE_HAS_HEADER: u32 = 1 << 31;

/// `source`, ending before the `ID3v1` and `APEv2` tags after the audio if it
/// is a FLAC file that has them, and as it is otherwise. A source that
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

/// Where the tags after the audio of the FLAC file of `len` bytes that
/// `source` reads start, or `None` if it is not a FLAC file. An end
/// before which there is no room for the stream header is no tag.
fn flac_audio_end<R: Read + Seek>(source: &mut R, len: u64) -> io::Result<Option<u64>> {
    let Some(audio_start) = flac_stream_start(source)? else {
        return Ok(None);
    };
    // The stream header and STREAMINFO before any frame.
    let floor = audio_start + 42;
    let mut end = len;
    loop {
        if end >= floor + ID3V1_LEN && read_at::<3>(source, end - ID3V1_LEN)? == *b"TAG" {
            end -= ID3V1_LEN;
            continue;
        }
        if end >= floor + APE_FOOTER_LEN {
            let footer = read_at::<32>(source, end - APE_FOOTER_LEN)?;
            if footer.starts_with(b"APETAGEX") {
                // The size counts the items and the footer, not the header.
                let size = u32::from_le_bytes([footer[12], footer[13], footer[14], footer[15]]);
                let flags = u32::from_le_bytes([footer[20], footer[21], footer[22], footer[23]]);
                let header = if flags & APE_HAS_HEADER == 0 {
                    0
                } else {
                    APE_FOOTER_LEN
                };
                let tag = u64::from(size) + header;
                if tag >= APE_FOOTER_LEN && end - floor >= tag {
                    end -= tag;
                    continue;
                }
            }
        }
        return Ok(Some(end));
    }
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
    use super::super::test_flac::{apev2_tag as apev2, id3v1_tag as id3v1};
    use super::*;
    use std::io::Cursor;

    /// A FLAC stream header and STREAMINFO, then `frames` bytes of frames.
    fn flac(frames: usize) -> Vec<u8> {
        let mut bytes = b"fLaC".to_vec();
        bytes.extend([0x80, 0, 0, 34]);
        bytes.extend([0_u8; 34]);
        bytes.extend((0..frames).map(|i| u8::try_from(i % 251).unwrap()));
        bytes
    }

    fn read_all(bytes: Vec<u8>) -> (Option<u64>, Vec<u8>) {
        let mut source = without_trailing_tags(Box::new(Cursor::new(bytes)));
        let len = source.byte_len();
        let mut out = Vec::new();
        source.read_to_end(&mut out).expect("read");
        (len, out)
    }

    #[test]
    fn a_flac_stream_ends_before_the_tags_after_its_frames() {
        let audio = flac(5_000);
        for (name, tags) in [
            ("ID3v1", id3v1()),
            ("APEv2 with a header", apev2(true)),
            ("APEv2 without a header", apev2(false)),
            ("APEv2 and ID3v1", [apev2(true), id3v1()].concat()),
        ] {
            let (len, read) = read_all([audio.clone(), tags].concat());
            assert_eq!(len, Some(u64::try_from(audio.len()).unwrap()), "{name}");
            assert!(read == audio, "{name}: read {} bytes", read.len());
        }
        // After an ID3v2 tag at the start too.
        let mut id3v2 = b"ID3\x04\x00\x00\x00\x00\x00\x05".to_vec();
        id3v2.extend([0_u8; 5]);
        let file = [id3v2.clone(), audio.clone()].concat();
        let (len, read) = read_all([file.clone(), id3v1()].concat());
        assert_eq!(len, Some(u64::try_from(file.len()).unwrap()));
        assert!(read == file);
    }

    #[test]
    fn other_files_and_untagged_flac_files_read_whole() {
        let mut wave = b"RIFF".to_vec();
        wave.extend([0_u8; 400]);
        // `TAG` 128 bytes before the end, but inside STREAMINFO: no tag
        // ends the stream before its header does.
        let mut short = flac(108);
        short[22..25].copy_from_slice(b"TAG");
        for (name, bytes) in [
            ("FLAC", flac(5_000)),
            ("WAV with ID3v1", [wave, id3v1()].concat()),
            ("FLAC with TAG in its header", short),
        ] {
            let (len, read) = read_all(bytes.clone());
            assert_eq!(len, Some(u64::try_from(bytes.len()).unwrap()), "{name}");
            assert!(read == bytes, "{name}");
        }
    }

    #[test]
    fn seeks_are_within_the_audio() {
        let audio = flac(5_000);
        let mut source =
            without_trailing_tags(Box::new(Cursor::new([audio.clone(), id3v1()].concat())));
        let end = u64::try_from(audio.len()).unwrap();
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
