use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

#[cfg(feature = "gst")]
use gstreamer as gst;
#[cfg(feature = "gst")]
use gstreamer_pbutils as gst_pbutils;

const APE_PREAMBLE: &[u8; 8] = b"APETAGEX";
const APE_TAG_HEADER_BYTES: usize = 32;
const APE_TAG_HEADER_BYTES_U64: u64 = 32;
const APE_MIN_ITEM_BYTES: u64 = 11;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub(crate) struct RawAudioTagMetadata {
    pub(crate) title: Option<String>,
    pub(crate) artist: Option<String>,
    pub(crate) album: Option<String>,
    pub(crate) genre: Option<String>,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
}

impl RawAudioTagMetadata {
    pub(crate) fn is_empty(&self) -> bool {
        self.title.is_none()
            && self.artist.is_none()
            && self.album.is_none()
            && self.genre.is_none()
            && self.year.is_none()
            && self.track_no.is_none()
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub(crate) struct RawAudioTechnicalDetails {
    pub(crate) duration_secs: Option<f32>,
    pub(crate) sample_rate_hz: Option<u32>,
    pub(crate) bitrate_kbps: Option<u32>,
    pub(crate) channels: Option<u8>,
    pub(crate) bit_depth: Option<u8>,
    pub(crate) current_bitrate_kbps: Option<u32>,
    pub(crate) format_label: String,
}

#[derive(Debug, Clone, Copy)]
struct Apev2Footer {
    item_count: u32,
    tag_size: u32,
}

pub(crate) fn is_raw_surround_file(path: &Path) -> bool {
    matches!(raw_surround_extension(path).as_deref(), Some("ac3" | "dts"))
}

pub(crate) fn is_dts_file(path: &Path) -> bool {
    matches!(raw_surround_extension(path).as_deref(), Some("dts"))
}

fn raw_surround_extension(path: &Path) -> Option<String> {
    let ext = path.extension().and_then(|value| value.to_str())?;
    Some(ext.to_ascii_lowercase())
}

fn round_f64_to_u32(value: f64) -> Option<u32> {
    if !value.is_finite() || value < 0.0 {
        return None;
    }
    value
        .round()
        .clamp(0.0, f64::from(u32::MAX))
        .to_string()
        .parse::<u32>()
        .ok()
}

pub(crate) fn raw_surround_format_label(path: &Path) -> String {
    match raw_surround_extension(path).as_deref().unwrap_or_default() {
        "ac3" => "AC3".to_string(),
        "dts" => "DTS".to_string(),
        _ => String::new(),
    }
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

#[cfg(feature = "gst")]
pub(crate) fn probe_raw_surround_technical_details(
    path: &Path,
) -> Option<RawAudioTechnicalDetails> {
    if !is_raw_surround_file(path) {
        return None;
    }

    gst::init().ok()?;

    let uri = url::Url::from_file_path(path).ok()?.to_string();
    let timeout = gst::ClockTime::from_seconds(5);
    let discoverer = gst_pbutils::Discoverer::new(timeout).ok()?;
    let info = discoverer.discover_uri(&uri).ok()?;
    let audio_info = info.audio_streams().into_iter().next()?;

    let bitrate_kbps = kbps_from_bits_per_second(audio_info.bitrate())
        .or_else(|| kbps_from_bits_per_second(audio_info.max_bitrate()));

    Some(RawAudioTechnicalDetails {
        duration_secs: info
            .duration()
            .map(gst::ClockTime::nseconds)
            .map(std::time::Duration::from_nanos)
            .map(|duration| duration.as_secs_f32())
            .filter(|value| *value > 0.0),
        sample_rate_hz: Some(audio_info.sample_rate()).filter(|value| *value > 0),
        bitrate_kbps,
        channels: u8::try_from(audio_info.channels())
            .ok()
            .filter(|value| *value > 0),
        bit_depth: u8::try_from(audio_info.depth())
            .ok()
            .filter(|value| *value > 0),
        current_bitrate_kbps: bitrate_kbps,
        format_label: raw_surround_format_label(path),
    })
}

#[cfg(not(feature = "gst"))]
pub(crate) fn probe_raw_surround_technical_details(
    _path: &Path,
) -> Option<RawAudioTechnicalDetails> {
    None
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
    if tag_size < u32::try_from(APE_TAG_HEADER_BYTES).ok()?
        || tag_size > u32::try_from(file_len).ok()?
    {
        return None;
    }

    Some(Apev2Footer {
        item_count,
        tag_size,
    })
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
    } else if key.eq_ignore_ascii_case("genre") {
        metadata.genre = Some(value.to_string());
    } else if key.eq_ignore_ascii_case("year") {
        metadata.year = parse_ape_year(value);
    } else if key.eq_ignore_ascii_case("track") {
        metadata.track_no = parse_ape_track_number(value);
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

fn parse_ape_track_number(value: &str) -> Option<u32> {
    let trimmed = value.trim();
    let number = trimmed.split('/').next()?.trim();
    number.parse().ok()
}

#[cfg(feature = "gst")]
fn kbps_from_bits_per_second(bits_per_second: u32) -> Option<u32> {
    (bits_per_second > 0)
        .then(|| round_f64_to_u32(f64::from(bits_per_second) / 1000.0))
        .flatten()
}

#[cfg(test)]
pub(crate) fn write_test_apev2_file(path: &Path, items: &[(&str, &str)], header: bool) {
    let mut bytes = b"stub".to_vec();
    let tag = build_test_apev2_tag(items, header);
    bytes.extend_from_slice(&tag);
    std::fs::write(path, bytes).expect("write test raw audio file");
}

#[cfg(test)]
fn build_test_apev2_tag(items: &[(&str, &str)], header: bool) -> Vec<u8> {
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
        out.extend_from_slice(&build_test_ape_block(
            size_field,
            u32::try_from(items.len()).expect("test item count fits into u32"),
            0xA0,
        ));
    }
    out.extend_from_slice(&item_bytes);
    out.extend_from_slice(&build_test_ape_block(
        size_field,
        u32::try_from(items.len()).expect("test item count fits into u32"),
        0x80,
    ));
    out
}

#[cfg(test)]
fn build_test_ape_block(size_field: u32, item_count: u32, flag_byte: u8) -> [u8; 32] {
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
        build_test_apev2_tag, parse_ape_track_number, parse_ape_year, raw_surround_format_label,
        read_appended_apev2_text_metadata, write_test_apev2_file,
    };
    use std::path::{Path, PathBuf};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_path(name: &str, ext: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|value| value.as_nanos())
            .unwrap_or(0);
        path.push(format!(
            "ferrous-raw-audio-{name}-{}-{nanos}.{ext}",
            std::process::id()
        ));
        path
    }

    #[test]
    fn parse_ape_track_supports_number_pairs() {
        assert_eq!(parse_ape_track_number("3"), Some(3));
        assert_eq!(parse_ape_track_number("03/8"), Some(3));
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
                ("Genre", "Progressive death metal"),
                ("Year", "2001"),
                ("Track", "01/8"),
            ],
            true,
        );

        let metadata = read_appended_apev2_text_metadata(&path).expect("metadata");
        assert_eq!(metadata.title.as_deref(), Some("The Leper Affinity"));
        assert_eq!(metadata.artist.as_deref(), Some("Opeth"));
        assert_eq!(metadata.album.as_deref(), Some("Blackwater Park"));
        assert_eq!(metadata.genre.as_deref(), Some("Progressive death metal"));
        assert_eq!(metadata.year, Some(2001));
        assert_eq!(metadata.track_no, Some(1));

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

    #[test]
    fn raw_surround_format_labels_match_extensions() {
        assert_eq!(raw_surround_format_label(Path::new("a.ac3")), "AC3");
        assert_eq!(raw_surround_format_label(Path::new("a.dts")), "DTS");
    }
}
