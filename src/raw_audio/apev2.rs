// SPDX-License-Identifier: GPL-3.0-or-later

//! `APEv2` tags appended to raw AC3/DTS files: reading the text items the
//! library shows and rewriting the fields the tag editor changes.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use super::is_raw_surround_file;

const APE_PREAMBLE: &[u8; 8] = b"APETAGEX";
const APE_TAG_HEADER_BYTES: usize = 32;
const APE_TAG_HEADER_BYTES_U64: u64 = 32;
const APE_MIN_ITEM_BYTES: u64 = 11;
const APE_ITEM_READ_ONLY: u32 = 1;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct RawAudioTagMetadata {
    pub(crate) title: Option<String>,
    pub(crate) artist: Option<String>,
    pub(crate) album: Option<String>,
    pub(crate) album_artist: Option<String>,
    pub(crate) genre: Option<String>,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
    pub(crate) track_total: Option<u32>,
    pub(crate) disc_no: Option<u32>,
    pub(crate) disc_total: Option<u32>,
    pub(crate) comment: Option<String>,
}

impl RawAudioTagMetadata {
    pub(crate) fn is_empty(&self) -> bool {
        self.title.is_none()
            && self.artist.is_none()
            && self.album.is_none()
            && self.album_artist.is_none()
            && self.genre.is_none()
            && self.year.is_none()
            && self.track_no.is_none()
            && self.track_total.is_none()
            && self.disc_no.is_none()
            && self.disc_total.is_none()
            && self.comment.is_none()
    }
}

#[derive(Debug, Clone, Copy)]
pub(super) struct Apev2Footer {
    pub(super) item_count: u32,
    pub(super) tag_size: u32,
}

pub(crate) fn read_appended_apev2_text_metadata(path: &Path) -> Option<RawAudioTagMetadata> {
    if !is_raw_surround_file(path) {
        return None;
    }

    let mut file = File::open(path).ok()?;
    let file_len = file.seek(SeekFrom::End(0)).ok()?;
    let footer = read_apev2_footer(&mut file, file_len)?;
    let items_start = locate_apev2_items_start(&mut file, file_len, footer.tag_size)?;
    let items_end = file_len.checked_sub(APE_TAG_HEADER_BYTES_U64)?;
    if items_start >= items_end {
        return None;
    }

    parse_apev2_text_items(&mut file, items_start, items_end, footer.item_count)
}

fn read_apev2_footer(file: &mut File, file_len: u64) -> Option<Apev2Footer> {
    if file_len < APE_TAG_HEADER_BYTES_U64 {
        return None;
    }

    file.seek(SeekFrom::End(-i64::try_from(APE_TAG_HEADER_BYTES).ok()?))
        .ok()?;
    let mut footer = [0u8; APE_TAG_HEADER_BYTES];
    file.read_exact(&mut footer).ok()?;
    if &footer[..8] != APE_PREAMBLE {
        return None;
    }

    let version = u32::from_le_bytes(footer[8..12].try_into().ok()?);
    if version != 1000 && version != 2000 {
        return None;
    }

    let tag_size = u32::from_le_bytes(footer[12..16].try_into().ok()?);
    let item_count = u32::from_le_bytes(footer[16..20].try_into().ok()?);
    // Compare in 64 bits: files over 4 GiB still end in a tag.
    if u64::from(tag_size) < APE_TAG_HEADER_BYTES_U64 || u64::from(tag_size) > file_len {
        return None;
    }

    Some(Apev2Footer {
        item_count,
        tag_size,
    })
}

/// Length of the `APEv2` tag at the end of a file, including its optional
/// header; 0 when the file has no tag.
#[cfg(any(feature = "gst", test))]
pub(super) fn appended_tag_len(file: &mut File, file_len: u64) -> u64 {
    let Some(footer) = read_apev2_footer(file, file_len) else {
        return 0;
    };
    // The tag size covers the items and the footer, not the header.
    let tag_size = u64::from(footer.tag_size);
    let with_header = tag_size.saturating_add(APE_TAG_HEADER_BYTES_U64);
    if file_len
        .checked_sub(with_header)
        .is_some_and(|header_start| has_ape_preamble(file, header_start))
    {
        with_header
    } else {
        tag_size
    }
}

fn locate_apev2_items_start(file: &mut File, file_len: u64, tag_size: u32) -> Option<u64> {
    let tag_size = u64::from(tag_size);

    if let Some(header_start) =
        file_len.checked_sub(tag_size.checked_add(APE_TAG_HEADER_BYTES_U64)?)
    {
        if header_start < file_len.saturating_sub(APE_TAG_HEADER_BYTES_U64)
            && has_ape_preamble(file, header_start)
        {
            return header_start.checked_add(APE_TAG_HEADER_BYTES_U64);
        }
    }

    file_len.checked_sub(tag_size)
}

fn has_ape_preamble(file: &mut File, offset: u64) -> bool {
    let Ok(current_pos) = file.stream_position() else {
        return false;
    };

    let mut preamble = [0u8; 8];
    let result = file
        .seek(SeekFrom::Start(offset))
        .and_then(|_| file.read_exact(&mut preamble))
        .is_ok()
        && preamble == *APE_PREAMBLE;

    let _ = file.seek(SeekFrom::Start(current_pos));
    result
}

fn parse_apev2_text_items(
    file: &mut File,
    items_start: u64,
    items_end: u64,
    item_count: u32,
) -> Option<RawAudioTagMetadata> {
    file.seek(SeekFrom::Start(items_start)).ok()?;
    let mut metadata = RawAudioTagMetadata::default();

    for _ in 0..item_count {
        let current_pos = file.stream_position().ok()?;
        if current_pos >= items_end {
            break;
        }
        if items_end.saturating_sub(current_pos) < APE_MIN_ITEM_BYTES {
            break;
        }

        let value_size = read_u32(file)?;
        let flags = read_u32(file)?;
        let key = read_ape_text_key(file, items_end)?;
        if key.is_empty() || key.len() > 255 {
            break;
        }

        let value_end = file
            .stream_position()
            .ok()?
            .checked_add(u64::from(value_size))?;
        if value_end > items_end {
            break;
        }

        let mut value = vec![0u8; usize::try_from(value_size).ok()?];
        file.read_exact(&mut value).ok()?;

        let item_type = (flags >> 1) & 0b11;
        if item_type != 0 {
            continue;
        }

        let Ok(text_value) = String::from_utf8(value) else {
            continue;
        };
        apply_ape_text_item(
            &mut metadata,
            &key,
            text_value.trim_end_matches('\0').trim(),
        );
    }

    (!metadata.is_empty()).then_some(metadata)
}

fn read_u32(file: &mut File) -> Option<u32> {
    let mut bytes = [0u8; 4];
    file.read_exact(&mut bytes).ok()?;
    Some(u32::from_le_bytes(bytes))
}

fn read_ape_text_key(file: &mut File, items_end: u64) -> Option<String> {
    let mut key_bytes = Vec::new();

    while file.stream_position().ok()? < items_end {
        let mut byte = [0u8; 1];
        file.read_exact(&mut byte).ok()?;
        if byte[0] == 0 {
            return String::from_utf8(key_bytes).ok();
        }
        key_bytes.push(byte[0]);
        if key_bytes.len() > 255 {
            return None;
        }
    }

    None
}

fn apply_ape_text_item(metadata: &mut RawAudioTagMetadata, key: &str, value: &str) {
    if value.is_empty() {
        return;
    }

    if key.eq_ignore_ascii_case("title") {
        metadata.title = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("artist") {
        metadata.artist = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("album") {
        metadata.album = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("album artist") || key.eq_ignore_ascii_case("albumartist") {
        metadata.album_artist = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("genre") {
        metadata.genre = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("year") {
        metadata.year = parse_ape_year(value);
    } else if key.eq_ignore_ascii_case("track") {
        let (track_no, track_total) = parse_ape_number_pair(value);
        metadata.track_no = track_no;
        metadata.track_total = track_total;
    } else if key.eq_ignore_ascii_case("disc") {
        let (disc_no, disc_total) = parse_ape_number_pair(value);
        metadata.disc_no = disc_no;
        metadata.disc_total = disc_total;
    } else if key.eq_ignore_ascii_case("comment") {
        metadata.comment = Some(value.to_string());
    }
}

fn parse_ape_year(value: &str) -> Option<i32> {
    let trimmed = value.trim();
    let digits = trimmed
        .chars()
        .take_while(char::is_ascii_digit)
        .take(4)
        .collect::<String>();

    (digits.len() == 4).then(|| digits.parse().ok()).flatten()
}

#[cfg(test)]
fn parse_ape_track_number(value: &str) -> Option<u32> {
    parse_ape_number_pair(value).0
}

fn parse_ape_number_pair(value: &str) -> (Option<u32>, Option<u32>) {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return (None, None);
    }

    let mut parts = trimmed.splitn(2, '/');
    let number = parts.next().and_then(parse_optional_u32);
    let total = parts.next().and_then(parse_optional_u32);
    (number, total)
}

fn parse_optional_u32(value: &str) -> Option<u32> {
    let trimmed = value.trim();
    (!trimmed.is_empty())
        .then(|| trimmed.parse().ok())
        .flatten()
}

/// A text field of an appended `APEv2` tag that the tag editor can change.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ApeTextField {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Year,
    Track,
    Disc,
    Comment,
}

impl ApeTextField {
    /// Keys that hold this field, matched case-insensitively. New items use the first.
    fn keys(self) -> &'static [&'static str] {
        match self {
            Self::Title => &["Title"],
            Self::Artist => &["Artist"],
            Self::Album => &["Album"],
            Self::AlbumArtist => &["Album Artist", "AlbumArtist"],
            Self::Genre => &["Genre"],
            Self::Year => &["Year"],
            Self::Track => &["Track"],
            Self::Disc => &["Disc"],
            Self::Comment => &["Comment"],
        }
    }

    fn matches(self, key: &str) -> bool {
        self.keys()
            .iter()
            .any(|candidate| candidate.eq_ignore_ascii_case(key))
    }
}

/// One item of an `APEv2` tag, kept byte for byte when it is not edited.
#[derive(Debug, Clone, PartialEq, Eq)]
struct ApeItem {
    key: String,
    flags: u32,
    value: Vec<u8>,
}

/// Format a number and total as an `APEv2` `Track` or `Disc` value.
pub(crate) fn format_ape_number_pair(number: Option<u32>, total: Option<u32>) -> Option<String> {
    match (number, total) {
        (None, None) => None,
        (Some(number), None) => Some(number.to_string()),
        (None, Some(total)) => Some(format!("/{total}")),
        (Some(number), Some(total)) => Some(format!("{number}/{total}")),
    }
}

/// Change the given fields of the `APEv2` tag appended to a raw surround file.
///
/// A `None` or empty value removes the field. All other items, including
/// binary items and unknown keys, are kept byte for byte. An edited item
/// keeps the key spelling and read-only flag of the item it replaces. A tag
/// whose items do not exactly fill its declared size is not rewritten. The
/// file is replaced through [`crate::safe_save::rewrite_via_temp_copy`].
pub(crate) fn edit_appended_apev2_text_items(
    path: &Path,
    edits: &[(ApeTextField, Option<String>)],
) -> Result<(), String> {
    if !is_raw_surround_file(path) {
        return Err(format!(
            "APEv2 write is only supported for AC3/DTS files: {}",
            path.to_string_lossy()
        ));
    }
    crate::safe_save::rewrite_via_temp_copy(path, |file| {
        apply_apev2_text_edits(file, edits).map_err(|err| {
            format!(
                "failed to update APEv2 tag in {}: {err}",
                path.to_string_lossy()
            )
        })
    })
}

fn apply_apev2_text_edits(
    file: &mut File,
    edits: &[(ApeTextField, Option<String>)],
) -> Result<(), String> {
    let file_len = file.seek(SeekFrom::End(0)).map_err(|err| err.to_string())?;
    let (tag_start, mut items) = match read_apev2_footer(file, file_len) {
        Some(footer) => read_apev2_items(file, file_len, footer)?,
        None => (file_len, Vec::new()),
    };

    for (field, value) in edits {
        let first = items.iter().position(|item| field.matches(&item.key));
        // An edited item keeps the key spelling and read-only flag of the item
        // it replaces; its value becomes UTF-8 text.
        let (key, flags) = first.map_or_else(
            || (field.keys()[0].to_string(), 0),
            |index| {
                let item = &items[index];
                (item.key.clone(), item.flags & APE_ITEM_READ_ONLY)
            },
        );
        items.retain(|item| !field.matches(&item.key));
        if let Some(value) = value
            .as_deref()
            .map(str::trim)
            .filter(|value| !value.is_empty())
        {
            let index = first.unwrap_or(items.len()).min(items.len());
            items.insert(
                index,
                ApeItem {
                    key,
                    flags,
                    value: value.as_bytes().to_vec(),
                },
            );
        }
    }

    file.set_len(tag_start).map_err(|err| err.to_string())?;
    if !items.is_empty() {
        file.seek(SeekFrom::Start(tag_start))
            .map_err(|err| err.to_string())?;
        file.write_all(&build_apev2_tag(&items)?)
            .map_err(|err| err.to_string())?;
    }
    Ok(())
}

/// Read every item of an appended tag and the offset where the tag starts.
fn read_apev2_items(
    file: &mut File,
    file_len: u64,
    footer: Apev2Footer,
) -> Result<(u64, Vec<ApeItem>), String> {
    let tag_size = u64::from(footer.tag_size);
    let items_start = file_len - tag_size;
    let items_end = file_len - APE_TAG_HEADER_BYTES_U64;
    let tag_start = match items_start.checked_sub(APE_TAG_HEADER_BYTES_U64) {
        Some(header_start) if has_ape_preamble(file, header_start) => header_start,
        _ => items_start,
    };
    let mut bytes =
        vec![0u8; usize::try_from(items_end - items_start).map_err(|err| err.to_string())?];
    file.seek(SeekFrom::Start(items_start))
        .and_then(|_| file.read_exact(&mut bytes))
        .map_err(|err| err.to_string())?;
    // Rewriting a tag that cannot be parsed completely would drop the rest of it.
    let items = parse_apev2_items(&bytes, footer.item_count).ok_or_else(|| {
        "the existing tag is malformed: its items do not match its item count and size".to_string()
    })?;
    Ok((tag_start, items))
}

fn parse_apev2_items(mut bytes: &[u8], item_count: u32) -> Option<Vec<ApeItem>> {
    let mut items = Vec::new();
    for _ in 0..item_count {
        let value_len =
            usize::try_from(u32::from_le_bytes(bytes.get(..4)?.try_into().ok()?)).ok()?;
        let flags = u32::from_le_bytes(bytes.get(4..8)?.try_into().ok()?);
        let rest = bytes.get(8..)?;
        let key_len = rest.iter().position(|&byte| byte == 0)?;
        let key = std::str::from_utf8(&rest[..key_len]).ok()?;
        if key.is_empty() {
            return None;
        }
        let value_start = key_len + 1;
        let value_end = value_start.checked_add(value_len)?;
        items.push(ApeItem {
            key: key.to_string(),
            flags,
            value: rest.get(value_start..value_end)?.to_vec(),
        });
        bytes = &rest[value_end..];
    }
    // Bytes left over are items the count leaves out; dropping them would
    // lose them.
    bytes.is_empty().then_some(items)
}

/// Build a complete `APEv2` tag with a header and a footer.
fn build_apev2_tag(items: &[ApeItem]) -> Result<Vec<u8>, String> {
    let too_large = |_| "the tag is too large".to_string();
    let mut body = Vec::new();
    for item in items {
        body.extend_from_slice(
            &u32::try_from(item.value.len())
                .map_err(too_large)?
                .to_le_bytes(),
        );
        body.extend_from_slice(&item.flags.to_le_bytes());
        body.extend_from_slice(item.key.as_bytes());
        body.push(0);
        body.extend_from_slice(&item.value);
    }
    let size_field = u32::try_from(body.len() + APE_TAG_HEADER_BYTES).map_err(too_large)?;
    let item_count = u32::try_from(items.len()).map_err(too_large)?;
    let mut out = Vec::with_capacity(body.len() + 2 * APE_TAG_HEADER_BYTES);
    out.extend_from_slice(&build_ape_block(size_field, item_count, 0xA0));
    out.extend_from_slice(&body);
    out.extend_from_slice(&build_ape_block(size_field, item_count, 0x80));
    Ok(out)
}

#[cfg(test)]
pub(crate) fn write_test_apev2_file(path: &Path, items: &[(&str, &str)], header: bool) {
    let mut bytes = b"stub".to_vec();
    let tag = build_test_apev2_tag(items, header);
    bytes.extend_from_slice(&tag);
    std::fs::write(path, bytes).expect("write test raw audio file");
}

#[cfg(test)]
pub(crate) fn build_test_apev2_tag(items: &[(&str, &str)], header: bool) -> Vec<u8> {
    let mut item_bytes = Vec::new();
    for (key, value) in items {
        item_bytes.extend_from_slice(
            &u32::try_from(value.len())
                .expect("test APE value length fits into u32")
                .to_le_bytes(),
        );
        item_bytes.extend_from_slice(&0u32.to_le_bytes());
        item_bytes.extend_from_slice(key.as_bytes());
        item_bytes.push(0);
        item_bytes.extend_from_slice(value.as_bytes());
    }

    let size_field = u32::try_from(item_bytes.len())
        .expect("test APE item payload fits into u32")
        .saturating_add(
            u32::try_from(APE_TAG_HEADER_BYTES).expect("APE header size fits into u32"),
        );
    let mut out = Vec::new();
    if header {
        out.extend_from_slice(&build_ape_block(
            size_field,
            u32::try_from(items.len()).expect("test item count fits into u32"),
            0xA0,
        ));
    }
    out.extend_from_slice(&item_bytes);
    out.extend_from_slice(&build_ape_block(
        size_field,
        u32::try_from(items.len()).expect("test item count fits into u32"),
        0x80,
    ));
    out
}

fn build_ape_block(size_field: u32, item_count: u32, flag_byte: u8) -> [u8; 32] {
    let mut block = [0u8; 32];
    block[..8].copy_from_slice(APE_PREAMBLE);
    block[8..12].copy_from_slice(&2000u32.to_le_bytes());
    block[12..16].copy_from_slice(&size_field.to_le_bytes());
    block[16..20].copy_from_slice(&item_count.to_le_bytes());
    block[23] = flag_byte;
    block
}

#[cfg(test)]
mod tests {
    use super::{
        build_test_apev2_tag, edit_appended_apev2_text_items, parse_ape_number_pair,
        parse_ape_track_number, parse_ape_year, read_appended_apev2_text_metadata,
        write_test_apev2_file, ApeTextField,
    };
    use crate::raw_audio::test_path;
    use std::path::Path;

    #[test]
    fn parse_ape_track_supports_number_pairs() {
        assert_eq!(parse_ape_track_number("3"), Some(3));
        assert_eq!(parse_ape_track_number("03/8"), Some(3));
    }

    #[test]
    fn parse_ape_number_pair_supports_missing_number() {
        assert_eq!(parse_ape_number_pair("/8"), (None, Some(8)));
        assert_eq!(parse_ape_number_pair("03/08"), (Some(3), Some(8)));
    }

    #[test]
    fn parse_ape_year_uses_leading_digits() {
        assert_eq!(parse_ape_year("2001"), Some(2001));
        assert_eq!(parse_ape_year("2010-06-03"), Some(2010));
        assert_eq!(parse_ape_year("June 2010"), None);
    }

    #[test]
    fn read_apev2_text_metadata_from_header_and_footer_tag() {
        let path = test_path("header-footer", "dts");
        write_test_apev2_file(
            &path,
            &[
                ("Title", "The Leper Affinity"),
                ("Artist", "Opeth"),
                ("Album", "Blackwater Park"),
                ("Album Artist", "Opeth"),
                ("Genre", "Progressive death metal"),
                ("Year", "2001"),
                ("Track", "01/8"),
                ("Disc", "02/3"),
                ("Comment", "Classic"),
            ],
            true,
        );

        let metadata = read_appended_apev2_text_metadata(&path).expect("metadata");
        assert_eq!(metadata.title.as_deref(), Some("The Leper Affinity"));
        assert_eq!(metadata.artist.as_deref(), Some("Opeth"));
        assert_eq!(metadata.album.as_deref(), Some("Blackwater Park"));
        assert_eq!(metadata.album_artist.as_deref(), Some("Opeth"));
        assert_eq!(metadata.genre.as_deref(), Some("Progressive death metal"));
        assert_eq!(metadata.year, Some(2001));
        assert_eq!(metadata.track_no, Some(1));
        assert_eq!(metadata.track_total, Some(8));
        assert_eq!(metadata.disc_no, Some(2));
        assert_eq!(metadata.disc_total, Some(3));
        assert_eq!(metadata.comment.as_deref(), Some("Classic"));

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn read_apev2_text_metadata_from_footer_only_tag() {
        let path = test_path("footer-only", "ac3");
        let mut bytes = b"stub".to_vec();
        bytes.extend_from_slice(&build_test_apev2_tag(
            &[("Title", "Harvest"), ("Track", "03/8")],
            false,
        ));
        std::fs::write(&path, bytes).expect("write file");

        let metadata = read_appended_apev2_text_metadata(&path).expect("metadata");
        assert_eq!(metadata.title.as_deref(), Some("Harvest"));
        assert_eq!(metadata.track_no, Some(3));

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn malformed_tag_is_ignored() {
        let path = test_path("malformed", "dts");
        std::fs::write(&path, b"not a tag").expect("write file");
        assert!(read_appended_apev2_text_metadata(&path).is_none());
        let _ = std::fs::remove_file(path);
    }

    fn ape_items(path: &Path) -> Vec<(String, Vec<u8>)> {
        let mut file = std::fs::File::open(path).expect("open tagged file");
        let file_len = std::io::Seek::seek(&mut file, std::io::SeekFrom::End(0)).expect("seek");
        let footer = super::read_apev2_footer(&mut file, file_len).expect("APEv2 footer");
        let (_, items) = super::read_apev2_items(&mut file, file_len, footer).expect("items");
        items
            .into_iter()
            .map(|item| (item.key, item.value))
            .collect()
    }

    fn text_item(key: &str, value: &str) -> (String, Vec<u8>) {
        (key.to_string(), value.as_bytes().to_vec())
    }

    #[test]
    fn apev2_edits_change_only_the_given_fields() {
        let path = test_path("rewrite", "ac3");
        let mut bytes = b"stub".to_vec();
        let mut tag = build_test_apev2_tag(
            &[
                ("TITLE", "Old"),
                ("Artist", "Artist"),
                ("Year", "1997-05-12"),
                ("Composer", "Kept"),
                ("Track", "01/9"),
            ],
            false,
        );
        bytes.append(&mut tag);
        std::fs::write(&path, &bytes).expect("write file");

        edit_appended_apev2_text_items(
            &path,
            &[
                (ApeTextField::Title, Some("New".to_string())),
                (ApeTextField::Artist, None),
                (ApeTextField::AlbumArtist, Some("Album Artist".to_string())),
            ],
        )
        .expect("rewrite tags");

        assert_eq!(
            ape_items(&path),
            vec![
                text_item("TITLE", "New"),
                text_item("Year", "1997-05-12"),
                text_item("Composer", "Kept"),
                text_item("Track", "01/9"),
                text_item("Album Artist", "Album Artist"),
            ]
        );
        let metadata = read_appended_apev2_text_metadata(&path).expect("metadata");
        assert_eq!(metadata.title.as_deref(), Some("New"));
        assert_eq!(metadata.artist, None);
        assert_eq!(metadata.year, Some(1997));
        assert_eq!(metadata.track_no, Some(1));
        assert_eq!(metadata.track_total, Some(9));

        let rewritten = std::fs::read(&path).expect("read rewritten file");
        assert!(
            rewritten.starts_with(b"stubAPETAGEX"),
            "tag starts with a header"
        );
        let ape_count = rewritten
            .windows(super::APE_PREAMBLE.len())
            .filter(|window| *window == super::APE_PREAMBLE)
            .count();
        assert_eq!(ape_count, 2, "one header and one footer");
        assert_eq!(
            crate::raw_audio::byte_filter::audio_byte_range(&path),
            Some((0, 4))
        );

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn apev2_edits_keep_binary_items_and_remove_empty_tags() {
        let path = test_path("binary", "dts");
        let mut bytes = b"stub".to_vec();
        bytes.extend_from_slice(
            &super::build_apev2_tag(&[
                super::ApeItem {
                    key: "Title".to_string(),
                    flags: 0,
                    value: b"Old".to_vec(),
                },
                super::ApeItem {
                    key: "Cover Art (Front)".to_string(),
                    flags: 0b10,
                    value: b"cover.jpg\0\xff\xd8".to_vec(),
                },
            ])
            .expect("build tag"),
        );
        std::fs::write(&path, &bytes).expect("write file");

        edit_appended_apev2_text_items(&path, &[(ApeTextField::Title, Some(" New ".to_string()))])
            .expect("edit title");
        let items = ape_items(&path);
        assert_eq!(items[0], text_item("Title", "New"));
        assert_eq!(items[1].0, "Cover Art (Front)");
        assert_eq!(items[1].1, b"cover.jpg\0\xff\xd8".to_vec());

        let tagless = test_path("tagless", "ac3");
        std::fs::write(&tagless, b"stub").expect("write tagless file");
        edit_appended_apev2_text_items(
            &tagless,
            &[(ApeTextField::Genre, Some("Rock".to_string()))],
        )
        .expect("add tag");
        assert_eq!(ape_items(&tagless), vec![text_item("Genre", "Rock")]);
        edit_appended_apev2_text_items(&tagless, &[(ApeTextField::Genre, None)])
            .expect("remove last item");
        assert_eq!(std::fs::read(&tagless).expect("read file"), b"stub");

        let _ = std::fs::remove_file(path);
        let _ = std::fs::remove_file(tagless);
    }

    fn raw_items(path: &Path) -> Vec<super::ApeItem> {
        let mut file = std::fs::File::open(path).expect("open tagged file");
        let file_len = std::io::Seek::seek(&mut file, std::io::SeekFrom::End(0)).expect("seek");
        let footer = super::read_apev2_footer(&mut file, file_len).expect("APEv2 footer");
        super::read_apev2_items(&mut file, file_len, footer)
            .expect("items")
            .1
    }

    fn ape_item(key: &str, flags: u32, value: &[u8]) -> super::ApeItem {
        super::ApeItem {
            key: key.to_string(),
            flags,
            value: value.to_vec(),
        }
    }

    #[test]
    fn apev2_edits_keep_read_only_flags() {
        let path = test_path("read-only", "ac3");
        let original = [
            ape_item("TITLE", 1, b"Old"),
            ape_item("Artist", 1, b"A\0B"),
            ape_item("Locator", 0b101, b"http://example.com"),
        ];
        let mut bytes = b"stub".to_vec();
        bytes.extend_from_slice(&super::build_apev2_tag(&original).expect("build tag"));
        std::fs::write(&path, &bytes).expect("write file");

        edit_appended_apev2_text_items(&path, &[(ApeTextField::Title, Some("New".into()))])
            .expect("edit title");

        assert_eq!(
            raw_items(&path),
            vec![
                ape_item("TITLE", 1, b"New"),
                original[1].clone(),
                original[2].clone()
            ]
        );
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn apev2_edits_refuse_tags_with_uncounted_items() {
        let path = test_path("uncounted", "dts");
        let mut bytes = b"stub".to_vec();
        let mut tag = build_test_apev2_tag(
            &[("Title", "Old"), ("Artist", "Kept"), ("Composer", "Hidden")],
            true,
        );
        // Claim one item fewer than the tag holds, in the header and the footer.
        tag[16] = 2;
        let footer = tag.len() - 32;
        tag[footer + 16] = 2;
        bytes.append(&mut tag);
        std::fs::write(&path, &bytes).expect("write file");

        let error =
            edit_appended_apev2_text_items(&path, &[(ApeTextField::Title, Some("New".into()))])
                .expect_err("tag with uncounted items is not rewritten");

        assert!(error.contains("malformed"), "{error}");
        assert_eq!(std::fs::read(&path).expect("read file"), bytes);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn apev2_tags_of_files_over_4_gib_are_read_and_replaced() {
        use std::io::{Seek, SeekFrom, Write};

        // A sparse file: only the tag at the end takes space on disk.
        let path = test_path("over-4-gib", "ac3");
        let audio_len = (1_u64 << 32) + 4;
        let mut file = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .create_new(true)
            .open(&path)
            .expect("create sparse file");
        file.set_len(audio_len).expect("extend sparse file");
        file.seek(SeekFrom::End(0)).expect("seek to end");
        file.write_all(&build_test_apev2_tag(
            &[("Title", "Old"), ("Composer", "Kept")],
            true,
        ))
        .expect("append tag");

        let metadata = read_appended_apev2_text_metadata(&path).expect("tag is found");
        assert_eq!(metadata.title.as_deref(), Some("Old"));

        // The edit itself; the safe save would copy all 4 GiB.
        super::apply_apev2_text_edits(&mut file, &[(ApeTextField::Title, Some("New".into()))])
            .expect("edit title");

        let new_tag = super::build_apev2_tag(&[
            ape_item("Title", 0, b"New"),
            ape_item("Composer", 0, b"Kept"),
        ])
        .expect("build tag");
        let file_len = file.seek(SeekFrom::End(0)).expect("seek to end");
        let new_tag_len = u64::try_from(new_tag.len()).expect("tag length");
        assert_eq!(file_len, audio_len + new_tag_len, "one tag, replaced");
        assert_eq!(
            raw_items(&path),
            vec![
                ape_item("Title", 0, b"New"),
                ape_item("Composer", 0, b"Kept")
            ]
        );
        drop(file);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn apev2_edits_refuse_to_rewrite_malformed_tags() {
        let path = test_path("malformed", "ac3");
        let mut bytes = b"stub".to_vec();
        let mut tag = build_test_apev2_tag(&[("Title", "Old"), ("Artist", "Kept")], true);
        // Claim one more item than the tag holds.
        let footer = tag.len() - 32;
        tag[footer + 16] = 3;
        bytes.append(&mut tag);
        std::fs::write(&path, &bytes).expect("write file");

        let error =
            edit_appended_apev2_text_items(&path, &[(ApeTextField::Title, Some("New".into()))])
                .expect_err("malformed tag is not rewritten");

        assert!(error.contains("malformed"), "{error}");
        assert_eq!(std::fs::read(&path).expect("read file"), bytes);
        let _ = std::fs::remove_file(path);
    }
}
