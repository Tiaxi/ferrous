use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::ops::Range;
use std::path::Path;

#[cfg(feature = "gst")]
use gstreamer as gst;
#[cfg(feature = "gst")]
use gstreamer_pbutils as gst_pbutils;

#[cfg(feature = "gst")]
use std::sync::Once;

/// Disable `GStreamer`'s APE tag handling so that raw surround files with
/// appended `APEv2` tags are not misidentified as `application/x-apetag`.
///
/// We handle `APEv2` tags ourselves via [`read_appended_apev2_text_metadata`].
/// The `GStreamer` APE typefinder + `apedemux` combo causes crashes and decode
/// failures for AC3/DTS files because `apedemux` strips the tag but cannot
/// identify the remaining audio content.
///
/// We also register backup typefinders for AC3/DTS that check the audio sync
/// words at byte 0, ensuring correct type detection even if the built-in
/// typefinders are not installed.
#[cfg(feature = "gst")]
pub(crate) fn register_raw_surround_typefinders() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        // Disable the APE tag typefinder so files are never identified as
        // application/x-apetag.
        {
            use gst::prelude::{GstObjectExt, PluginFeatureExtManual};

            // Disable all typefinders whose caps mention APE tags, and the
            // apedemux element.  Match on caps string rather than factory name
            // because the factory name varies across GStreamer versions.
            for factory in gst::TypeFindFactory::factories() {
                let dominated_by_ape = factory.caps().is_some_and(|c| {
                    c.to_string().contains("apetag") || c.to_string().contains("x-ape")
                });
                if dominated_by_ape {
                    eprintln!(
                        "[ferrous] disabling APE typefinder: {} (caps: {:?})",
                        factory.name(),
                        factory.caps().map(|c| c.to_string())
                    );
                    factory.set_rank(gst::Rank::NONE);
                }
            }

            if let Some(factory) = gst::ElementFactory::find("apedemux") {
                eprintln!("[ferrous] disabling apedemux element factory");
                factory.set_rank(gst::Rank::NONE);
            }
        }

        // Register AC3/DTS typefinders as a safety net in case the built-in
        // ones are not installed.
        let ac3_caps = gst::Caps::builder("audio/x-ac3").build();
        let _ = gst::TypeFind::register(
            None,
            "ferrous-ac3-typefind",
            gst::Rank::PRIMARY + 1,
            Some("ac3"),
            Some(&ac3_caps),
            |tf| {
                if let Some(data) = tf.peek(0, 2) {
                    if data == [0x0B, 0x77] {
                        tf.suggest(
                            gst::TypeFindProbability::Maximum,
                            &gst::Caps::builder("audio/x-ac3").build(),
                        );
                    }
                }
            },
        );

        let dts_caps = gst::Caps::builder("audio/x-dts").build();
        let _ = gst::TypeFind::register(
            None,
            "ferrous-dts-typefind",
            gst::Rank::PRIMARY + 1,
            Some("dts"),
            Some(&dts_caps),
            |tf| {
                if let Some(data) = tf.peek(0, 4) {
                    if data == [0x7F, 0xFE, 0x80, 0x01] {
                        tf.suggest(
                            gst::TypeFindProbability::Maximum,
                            &gst::Caps::builder("audio/x-dts").build(),
                        );
                    }
                }
            },
        );
    });
}

const APE_PREAMBLE: &[u8; 8] = b"APETAGEX";
const APE_TAG_HEADER_BYTES: usize = 32;
const APE_TAG_HEADER_BYTES_U64: u64 = 32;
const APE_MIN_ITEM_BYTES: u64 = 11;

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
    register_raw_surround_typefinders();

    let uri = url::Url::from_file_path(path).ok()?.to_string();
    let timeout = gst::ClockTime::from_seconds(10);
    let discoverer = gst_pbutils::Discoverer::new(timeout).ok()?;
    match discoverer.discover_uri(&uri) {
        Ok(info) => {
            let audio_info = info.audio_streams().into_iter().next()?;

            let bitrate_kbps = kbps_from_bits_per_second(audio_info.bitrate())
                .or_else(|| kbps_from_bits_per_second(audio_info.max_bitrate()));

            let mut details = RawAudioTechnicalDetails {
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
            };

            if details.duration_secs.is_none() {
                eprintln!(
                    "[ferrous] GStreamer Discoverer returned no duration for {}, \
                     trying bitstream header fallback",
                    path.display()
                );
                details.duration_secs = estimate_duration_from_bitstream(path);
            }

            Some(details)
        }
        Err(err) => {
            eprintln!(
                "[ferrous] GStreamer Discoverer failed for {}: {} — \
                 this may be caused by an appended APEv2 tag confusing the demuxer; \
                 trying bitstream header fallback",
                path.display(),
                err
            );
            let duration_secs = estimate_duration_from_bitstream(path);
            Some(RawAudioTechnicalDetails {
                duration_secs,
                format_label: raw_surround_format_label(path),
                ..RawAudioTechnicalDetails::default()
            })
        }
    }
}

/// Estimate duration from the bitstream frame header bitrate and file size,
/// subtracting the `APEv2` tag if present.  Works for AC3 (A/52) and DTS files.
#[allow(clippy::cast_precision_loss)]
fn estimate_duration_from_bitstream(path: &Path) -> Option<f32> {
    let mut file = File::open(path).ok()?;
    let file_len = file.seek(SeekFrom::End(0)).ok()?;

    // Determine audio-only size by subtracting the APEv2 tag.
    let apev2_size = read_apev2_footer(&mut file, file_len).map_or(0, |footer| {
        let tag_size = u64::from(footer.tag_size);
        // tag_size covers items + footer; check for a separate header.
        let with_header = tag_size.saturating_add(APE_TAG_HEADER_BYTES_U64);
        if let Some(header_start) = file_len.checked_sub(with_header) {
            if has_ape_preamble(&mut file, header_start) {
                return with_header;
            }
        }
        tag_size
    });
    let audio_bytes = file_len.saturating_sub(apev2_size);
    if audio_bytes == 0 {
        return None;
    }

    // Read the first few bytes for header parsing.
    file.seek(SeekFrom::Start(0)).ok()?;
    let mut header = [0u8; 12];
    file.read_exact(&mut header).ok()?;

    let bitrate_bps = if is_dts_file(path) {
        parse_dts_bitrate(&header)?
    } else {
        parse_ac3_bitrate(&header)?
    };

    if bitrate_bps == 0 {
        return None;
    }

    let duration = (audio_bytes as f64 * 8.0) / f64::from(bitrate_bps);
    #[allow(clippy::cast_possible_truncation)]
    let duration = duration as f32;
    (duration > 0.0).then_some(duration)
}

/// AC3 bitrate lookup indexed by `frmsizecod / 2` (kbps) — A/52 Table 5.18.
const AC3_BITRATES_KBPS: [u32; 19] = [
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
];

/// DTS sample rate table indexed by SFREQ code.
const DTS_SAMPLE_RATES: [u32; 16] = [
    0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0,
];

/// Parse the bitrate from an AC3 (A/52) sync frame header.
///
/// Layout of the first 5 bytes:
///   `[0..2]` sync word `0x0B77`
///   `[2..4]` CRC1
///   `[4]`    fscod (2 bits) | frmsizecod (6 bits)
fn parse_ac3_bitrate(header: &[u8]) -> Option<u32> {
    if header.len() < 5 || header[0] != 0x0B || header[1] != 0x77 {
        return None;
    }
    let frmsizcod = usize::from(header[4] & 0x3F);
    let kbps = *AC3_BITRATES_KBPS.get(frmsizcod / 2)?;
    Some(kbps * 1000)
}

/// Parse the bitrate from a DTS frame header.
///
/// Sync word: `0x7FFE8001` (bytes `[0..4]`).
/// Frame size and sample rate are extracted from the header to compute
/// the bitrate as `frame_bytes * 8 * sample_rate / 512` (512 PCM samples
/// per DTS frame).
fn parse_dts_bitrate(header: &[u8]) -> Option<u32> {
    if header.len() < 12 {
        return None;
    }
    if header[0] != 0x7F || header[1] != 0xFE || header[2] != 0x80 || header[3] != 0x01 {
        return None;
    }

    // FSIZE is in bits 47..61 (14 bits), big-endian packed.
    let fsize_raw = (u16::from(header[5] & 0x03) << 12)
        | (u16::from(header[6]) << 4)
        | (u16::from(header[7]) >> 4);
    let frame_bytes = u32::from(fsize_raw) + 1;

    // SFREQ is at bits 67..71.
    let sfreq_code = usize::from((header[8] >> 2) & 0x0F);
    let sample_rate = *DTS_SAMPLE_RATES.get(sfreq_code)?;
    if sample_rate == 0 || frame_bytes == 0 {
        return None;
    }

    let bitrate = u64::from(frame_bytes) * 8 * u64::from(sample_rate) / 512;
    u32::try_from(bitrate).ok()
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

fn format_ape_number_pair(number: Option<u32>, total: Option<u32>) -> Option<String> {
    match (number, total) {
        (None, None) => None,
        (Some(number), None) => Some(number.to_string()),
        (None, Some(total)) => Some(format!("/{total}")),
        (Some(number), Some(total)) => Some(format!("{number}/{total}")),
    }
}

pub(crate) fn write_appended_apev2_text_metadata(
    path: &Path,
    metadata: &RawAudioTagMetadata,
) -> Result<(), String> {
    if !is_raw_surround_file(path) {
        return Err(format!(
            "APEv2 write is only supported for AC3/DTS files: {}",
            path.to_string_lossy()
        ));
    }

    let mut bytes = fs::read(path)
        .map_err(|err| format!("failed to read raw surround file for tag write: {err}"))?;
    if let Some(range) = locate_appended_apev2_range(&bytes) {
        bytes.truncate(range.start);
    }
    if !metadata.is_empty() {
        bytes.extend_from_slice(&build_apev2_tag(metadata));
    }
    fs::write(path, bytes)
        .map_err(|err| format!("failed to write raw surround file with updated tags: {err}"))
}

fn locate_appended_apev2_range(bytes: &[u8]) -> Option<Range<usize>> {
    let footer = read_apev2_footer_from_bytes(bytes)?;
    let footer_start = bytes.len().checked_sub(APE_TAG_HEADER_BYTES)?;
    let footer_only_start = bytes
        .len()
        .checked_sub(usize::try_from(footer.tag_size).ok()?)?;
    let header_start = footer_only_start.checked_sub(APE_TAG_HEADER_BYTES)?;
    let start = if bytes
        .get(header_start..header_start + APE_PREAMBLE.len())
        .is_some_and(|value| value == APE_PREAMBLE)
    {
        header_start
    } else {
        footer_only_start
    };
    (start <= footer_start).then_some(start..bytes.len())
}

fn read_apev2_footer_from_bytes(bytes: &[u8]) -> Option<Apev2Footer> {
    let footer = bytes.get(bytes.len().checked_sub(APE_TAG_HEADER_BYTES)?..)?;
    if footer.get(..APE_PREAMBLE.len())? != APE_PREAMBLE {
        return None;
    }

    let version = u32::from_le_bytes(footer.get(8..12)?.try_into().ok()?);
    if version != 1000 && version != 2000 {
        return None;
    }

    let tag_size = u32::from_le_bytes(footer.get(12..16)?.try_into().ok()?);
    let item_count = u32::from_le_bytes(footer.get(16..20)?.try_into().ok()?);
    if tag_size < u32::try_from(APE_TAG_HEADER_BYTES).ok()?
        || usize::try_from(tag_size).ok()? > bytes.len()
    {
        return None;
    }

    Some(Apev2Footer {
        item_count,
        tag_size,
    })
}

fn build_apev2_tag(metadata: &RawAudioTagMetadata) -> Vec<u8> {
    let mut items = Vec::<(&str, String)>::new();
    if let Some(title) = metadata.title.as_ref() {
        items.push(("Title", title.clone()));
    }
    if let Some(artist) = metadata.artist.as_ref() {
        items.push(("Artist", artist.clone()));
    }
    if let Some(album) = metadata.album.as_ref() {
        items.push(("Album", album.clone()));
    }
    if let Some(album_artist) = metadata.album_artist.as_ref() {
        items.push(("Album Artist", album_artist.clone()));
    }
    if let Some(genre) = metadata.genre.as_ref() {
        items.push(("Genre", genre.clone()));
    }
    if let Some(year) = metadata.year {
        items.push(("Year", year.to_string()));
    }
    if let Some(track) = format_ape_number_pair(metadata.track_no, metadata.track_total) {
        items.push(("Track", track));
    }
    if let Some(disc) = format_ape_number_pair(metadata.disc_no, metadata.disc_total) {
        items.push(("Disc", disc));
    }
    if let Some(comment) = metadata.comment.as_ref() {
        items.push(("Comment", comment.clone()));
    }

    let mut item_bytes = Vec::new();
    let item_count = items.len();
    for (key, value) in items {
        item_bytes.extend_from_slice(&u32::try_from(value.len()).unwrap_or(u32::MAX).to_le_bytes());
        item_bytes.extend_from_slice(&0u32.to_le_bytes());
        item_bytes.extend_from_slice(key.as_bytes());
        item_bytes.push(0);
        item_bytes.extend_from_slice(value.as_bytes());
    }

    let size_field = u32::try_from(item_bytes.len())
        .unwrap_or(u32::MAX)
        .saturating_add(u32::try_from(APE_TAG_HEADER_BYTES).unwrap_or(u32::MAX));
    let mut out = item_bytes;
    out.extend_from_slice(&build_ape_block(
        size_field,
        u32::try_from(item_count).unwrap_or(u32::MAX),
        0x80,
    ));
    out
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
        build_test_apev2_tag, parse_ape_number_pair, parse_ape_track_number, parse_ape_year,
        raw_surround_format_label, read_appended_apev2_text_metadata,
        write_appended_apev2_text_metadata, write_test_apev2_file, RawAudioTagMetadata,
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

    #[test]
    fn raw_surround_format_labels_match_extensions() {
        assert_eq!(raw_surround_format_label(Path::new("a.ac3")), "AC3");
        assert_eq!(raw_surround_format_label(Path::new("a.dts")), "DTS");
    }

    #[test]
    fn write_apev2_text_metadata_replaces_existing_appended_block() {
        let path = test_path("rewrite", "ac3");
        write_test_apev2_file(&path, &[("Title", "Old"), ("Track", "01/9")], true);

        write_appended_apev2_text_metadata(
            &path,
            &RawAudioTagMetadata {
                title: Some("New".to_string()),
                artist: Some("Artist".to_string()),
                album_artist: Some("Album Artist".to_string()),
                track_no: Some(3),
                track_total: Some(8),
                disc_no: Some(2),
                disc_total: Some(4),
                comment: Some("Updated".to_string()),
                ..RawAudioTagMetadata::default()
            },
        )
        .expect("rewrite tags");

        let metadata = read_appended_apev2_text_metadata(&path).expect("metadata");
        assert_eq!(metadata.title.as_deref(), Some("New"));
        assert_eq!(metadata.artist.as_deref(), Some("Artist"));
        assert_eq!(metadata.album_artist.as_deref(), Some("Album Artist"));
        assert_eq!(metadata.track_no, Some(3));
        assert_eq!(metadata.track_total, Some(8));
        assert_eq!(metadata.disc_no, Some(2));
        assert_eq!(metadata.disc_total, Some(4));
        assert_eq!(metadata.comment.as_deref(), Some("Updated"));

        let bytes = std::fs::read(&path).expect("read rewritten file");
        let ape_count = bytes
            .windows(super::APE_PREAMBLE.len())
            .filter(|window| *window == super::APE_PREAMBLE)
            .count();
        assert_eq!(ape_count, 1);

        let _ = std::fs::remove_file(path);
    }

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
        header[5] = 0x00; // FSIZE upper 2 = 0
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

    #[test]
    fn estimate_duration_from_ac3_with_apev2() {
        // Build a fake AC3 file: valid header + padding + APEv2 tag
        let path = test_path("ac3-duration", "ac3");

        // AC3 header: 0x0B77, CRC, fscod=00 frmsizecod=26 → index 13 → 320 kbps
        let ac3_header = [0x0B, 0x77, 0x00, 0x00, 0b00_011010];
        let audio_size: usize = 320_000; // 320 kB of audio → 8 seconds at 320 kbps
        let mut data = Vec::with_capacity(audio_size + 200);
        data.extend_from_slice(&ac3_header);
        data.resize(audio_size, 0x00);
        let tag = build_test_apev2_tag(&[("Title", "Test")], true);
        data.extend_from_slice(&tag);

        std::fs::write(&path, &data).expect("write test file");

        let duration = super::estimate_duration_from_bitstream(&path);
        assert!(duration.is_some(), "should estimate duration");
        let d = duration.unwrap();
        // audio_size = 320000 bytes, bitrate = 320000 bps → duration = 320000*8/320000 = 8.0s
        assert!((d - 8.0).abs() < 0.1, "expected ~8.0s, got {d}");

        let _ = std::fs::remove_file(path);
    }
}
