use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::{fs::File, io::ErrorKind};

use crossbeam_channel::{unbounded, Receiver, Sender};
use lofty::file::{AudioFile, FileType, TaggedFileExt};
use lofty::prelude::Accessor;
use symphonia::core::codecs::{
    CodecType, CODEC_TYPE_AAC, CODEC_TYPE_ALAC, CODEC_TYPE_FLAC, CODEC_TYPE_MP1, CODEC_TYPE_MP2,
    CODEC_TYPE_MP3, CODEC_TYPE_OPUS, CODEC_TYPE_PCM_ALAW, CODEC_TYPE_PCM_F32BE,
    CODEC_TYPE_PCM_F32LE, CODEC_TYPE_PCM_F64BE, CODEC_TYPE_PCM_F64LE, CODEC_TYPE_PCM_MULAW,
    CODEC_TYPE_PCM_S16BE, CODEC_TYPE_PCM_S16BE_PLANAR, CODEC_TYPE_PCM_S16LE,
    CODEC_TYPE_PCM_S16LE_PLANAR, CODEC_TYPE_PCM_S24BE, CODEC_TYPE_PCM_S24BE_PLANAR,
    CODEC_TYPE_PCM_S24LE, CODEC_TYPE_PCM_S24LE_PLANAR, CODEC_TYPE_PCM_S32BE,
    CODEC_TYPE_PCM_S32BE_PLANAR, CODEC_TYPE_PCM_S32LE, CODEC_TYPE_PCM_S32LE_PLANAR,
    CODEC_TYPE_PCM_S8, CODEC_TYPE_PCM_S8_PLANAR, CODEC_TYPE_PCM_U16BE, CODEC_TYPE_PCM_U16BE_PLANAR,
    CODEC_TYPE_PCM_U16LE, CODEC_TYPE_PCM_U16LE_PLANAR, CODEC_TYPE_PCM_U24BE,
    CODEC_TYPE_PCM_U24BE_PLANAR, CODEC_TYPE_PCM_U24LE, CODEC_TYPE_PCM_U24LE_PLANAR,
    CODEC_TYPE_PCM_U32BE, CODEC_TYPE_PCM_U32BE_PLANAR, CODEC_TYPE_PCM_U32LE,
    CODEC_TYPE_PCM_U32LE_PLANAR, CODEC_TYPE_PCM_U8, CODEC_TYPE_PCM_U8_PLANAR, CODEC_TYPE_VORBIS,
    CODEC_TYPE_WAVPACK,
};
use symphonia::core::formats::{FormatOptions, Packet};
use symphonia::core::io::{MediaSourceStream, MediaSourceStreamOptions};
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;
use symphonia::core::units::TimeBase;

use crate::raw_audio::{
    is_raw_surround_file, probe_raw_surround_technical_details, raw_surround_format_label,
    read_appended_apev2_text_metadata,
};

#[derive(Debug, Clone, Default)]
pub struct TrackMetadata {
    pub source_path: Option<String>,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub genre: String,
    pub year: Option<i32>,
    pub sample_rate_hz: Option<u32>,
    pub bitrate_kbps: Option<u32>,
    pub channels: Option<u8>,
    pub bit_depth: Option<u8>,
    pub format_label: String,
    pub current_bitrate_kbps: Option<u32>,
    pub bitrate_timeline_kbps: Vec<u16>,
    pub cover_art_path: Option<String>,
    pub cover_art_rgba: Option<(usize, usize, Vec<u8>)>,
}

#[derive(Debug, Clone)]
pub enum MetadataEvent {
    Loaded(TrackMetadata),
}

pub struct MetadataService {
    tx: Sender<PathBuf>,
}

impl MetadataService {
    pub fn new() -> (Self, Receiver<MetadataEvent>) {
        Self::new_with_delay(std::time::Duration::ZERO)
    }

    pub(crate) fn new_with_delay(delay: std::time::Duration) -> (Self, Receiver<MetadataEvent>) {
        let (req_tx, req_rx) = unbounded::<PathBuf>();
        let (event_tx, event_rx) = unbounded::<MetadataEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-metadata".to_string())
            .spawn(move || {
                while let Ok(mut path) = req_rx.recv() {
                    while let Ok(newer_path) = req_rx.try_recv() {
                        path = newer_path;
                    }
                    let mut metadata = TrackMetadata {
                        source_path: Some(path.to_string_lossy().to_string()),
                        title: path
                            .file_stem()
                            .and_then(|s| s.to_str())
                            .unwrap_or_default()
                            .to_owned(),
                        ..TrackMetadata::default()
                    };

                    if let Ok(tagged) = lofty::read_from_path(&path) {
                        let props = tagged.properties();
                        metadata.sample_rate_hz = props.sample_rate();
                        metadata.channels = props.channels();
                        metadata.bit_depth = props.bit_depth();
                        metadata.bitrate_kbps = props.audio_bitrate();
                        metadata.format_label = format_label_from_lofty_file_type(
                            tagged.file_type(),
                            path.extension().and_then(|value| value.to_str()),
                        );

                        if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
                            metadata.title = tag.title().map_or_else(
                                || "Unknown title".to_string(),
                                std::borrow::Cow::into_owned,
                            );
                            metadata.artist = tag.artist().map_or_else(
                                || "Unknown artist".to_string(),
                                std::borrow::Cow::into_owned,
                            );
                            metadata.album = tag.album().map_or_else(
                                || "Unknown album".to_string(),
                                std::borrow::Cow::into_owned,
                            );
                            metadata.genre = tag
                                .genre()
                                .map_or_else(String::new, std::borrow::Cow::into_owned);
                            metadata.year = tag.date().map(|v| i32::from(v.year));

                            if let Some(pic) = tag.pictures().first() {
                                if let Ok(img) = image::load_from_memory(pic.data()) {
                                    let rgba = img.to_rgba8();
                                    let width = rgba.width() as usize;
                                    let height = rgba.height() as usize;
                                    let raw = rgba.into_raw();
                                    metadata.cover_art_path =
                                        cache_embedded_cover_png(&path, width, height, &raw);
                                    metadata.cover_art_rgba = Some((width, height, raw));
                                }
                            }
                        }
                    }

                    if is_raw_surround_file(&path) {
                        if let Some(tagged) = read_appended_apev2_text_metadata(&path) {
                            if let Some(title) = tagged.title {
                                metadata.title = title;
                            }
                            if let Some(artist) = tagged.artist {
                                metadata.artist = artist;
                            }
                            if let Some(album) = tagged.album {
                                metadata.album = album;
                            }
                            if let Some(genre) = tagged.genre {
                                metadata.genre = genre;
                            }
                            metadata.year = tagged.year.or(metadata.year);
                        }
                    }

                    if metadata.format_label.is_empty() {
                        metadata.format_label = format_label_from_extension(
                            path.extension().and_then(|value| value.to_str()),
                        );
                    }
                    if metadata.current_bitrate_kbps.is_none() {
                        metadata.current_bitrate_kbps = metadata.bitrate_kbps;
                    }

                    let _ = event_tx.send(MetadataEvent::Loaded(metadata.clone()));

                    if is_raw_surround_file(&path) {
                        if let Some(details) = probe_raw_surround_technical_details(&path) {
                            if !details.format_label.is_empty() {
                                metadata.format_label = details.format_label;
                            }
                            metadata.sample_rate_hz =
                                metadata.sample_rate_hz.or(details.sample_rate_hz);
                            metadata.channels = metadata.channels.or(details.channels);
                            metadata.bit_depth = metadata.bit_depth.or(details.bit_depth);
                            metadata.bitrate_kbps = metadata.bitrate_kbps.or(details.bitrate_kbps);
                            metadata.current_bitrate_kbps = details
                                .current_bitrate_kbps
                                .or(metadata.current_bitrate_kbps)
                                .or(metadata.bitrate_kbps);

                            let _ = event_tx.send(MetadataEvent::Loaded(metadata.clone()));
                        }
                    }

                    if !is_raw_surround_file(&path) {
                        if let Some(details) = probe_stream_details(&path) {
                            if !details.format_label.is_empty() {
                                metadata.format_label = details.format_label;
                            }
                            metadata.sample_rate_hz =
                                metadata.sample_rate_hz.or(details.sample_rate_hz);
                            metadata.channels = metadata.channels.or(details.channels);
                            metadata.bit_depth = metadata.bit_depth.or(details.bit_depth);
                            metadata.bitrate_kbps = metadata.bitrate_kbps.or(details.bitrate_kbps);
                            metadata.current_bitrate_kbps = details.current_bitrate_kbps;
                            metadata.bitrate_timeline_kbps = details.bitrate_timeline_kbps;

                            let _ = event_tx.send(MetadataEvent::Loaded(metadata.clone()));
                        }
                    }

                    if metadata.current_bitrate_kbps.is_none() {
                        metadata.current_bitrate_kbps = metadata.bitrate_kbps;
                    }

                    if metadata.cover_art_rgba.is_none() {
                        metadata.cover_art_rgba = load_folder_cover_art(&path);
                    }

                    if !delay.is_zero() {
                        std::thread::sleep(delay);
                    }
                    let _ = event_tx.send(MetadataEvent::Loaded(metadata));
                }
            });

        (Self { tx: req_tx }, event_rx)
    }

    pub fn request(&self, path: PathBuf) {
        let _ = self.tx.send(path);
    }
}

impl TrackMetadata {
    pub fn displayed_bitrate_kbps(&self, position_seconds: f64) -> Option<u32> {
        if position_seconds.is_finite() && position_seconds >= 0.0 {
            let index = position_seconds.floor() as usize;
            if let Some(value) = self.bitrate_timeline_kbps.get(index).copied() {
                if value > 0 {
                    return Some(u32::from(value));
                }
            }
        }
        self.current_bitrate_kbps.or(self.bitrate_kbps)
    }
}

#[derive(Debug, Clone, Default)]
struct StreamTechnicalDetails {
    format_label: String,
    sample_rate_hz: Option<u32>,
    bitrate_kbps: Option<u32>,
    channels: Option<u8>,
    bit_depth: Option<u8>,
    current_bitrate_kbps: Option<u32>,
    bitrate_timeline_kbps: Vec<u16>,
}

fn probe_stream_details(track_path: &Path) -> Option<StreamTechnicalDetails> {
    let mut hint = Hint::new();
    if let Some(ext) = track_path.extension().and_then(|value| value.to_str()) {
        hint.with_extension(ext);
    }

    let file = File::open(track_path).ok()?;
    let mss = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
    let mut format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .ok()?
        .format;

    let track = format.default_track()?;
    let track_id = track.id;
    let codec = track.codec_params.codec;
    let sample_rate_hz = track.codec_params.sample_rate;
    let time_base = track.codec_params.time_base;
    let n_frames = track.codec_params.n_frames;
    let channels = track
        .codec_params
        .channels
        .as_ref()
        .and_then(|value| u8::try_from(value.count()).ok());
    let bit_depth = track
        .codec_params
        .bits_per_sample
        .or(track.codec_params.bits_per_coded_sample)
        .and_then(|value| u8::try_from(value).ok());
    let mut bucket_bytes = Vec::<f64>::new();
    let mut total_bytes = 0u64;

    loop {
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(symphonia::core::errors::Error::IoError(err))
                if err.kind() == ErrorKind::UnexpectedEof =>
            {
                break;
            }
            Err(symphonia::core::errors::Error::ResetRequired) => break,
            Err(_) => break,
        };

        if packet.track_id() != track_id {
            continue;
        }

        total_bytes = total_bytes.saturating_add(packet.buf().len() as u64);
        if let Some(tb) = time_base {
            accumulate_packet_bitrate_bytes(&mut bucket_bytes, tb, &packet);
        }
    }

    let mut details = StreamTechnicalDetails {
        format_label: codec_label(codec).map_or_else(
            || format_label_from_extension(track_path.extension().and_then(|value| value.to_str())),
            str::to_string,
        ),
        sample_rate_hz,
        bitrate_kbps: None,
        channels,
        bit_depth,
        current_bitrate_kbps: None,
        bitrate_timeline_kbps: Vec::new(),
    };

    if !bucket_bytes.is_empty() {
        details.bitrate_timeline_kbps = bucket_bytes
            .into_iter()
            .map(bytes_to_kbps)
            .collect::<Vec<_>>();
        details.current_bitrate_kbps = details
            .bitrate_timeline_kbps
            .first()
            .copied()
            .filter(|value| *value > 0)
            .map(u32::from);
    }

    if total_bytes > 0 {
        if let (Some(tb), Some(frame_count)) = (time_base, n_frames) {
            let duration = tb.calc_time(frame_count);
            let seconds = duration.seconds as f64 + duration.frac;
            if seconds > 0.0 {
                details.bitrate_kbps =
                    Some(((total_bytes as f64 * 8.0) / seconds / 1000.0).round() as u32);
            }
        }
    }

    Some(details)
}

fn accumulate_packet_bitrate_bytes(
    bucket_bytes: &mut Vec<f64>,
    time_base: TimeBase,
    packet: &Packet,
) {
    let start = time_to_seconds(time_base.calc_time(packet.ts()));
    let duration_ticks = packet.dur().max(1);
    let end = time_to_seconds(time_base.calc_time(packet.ts().saturating_add(duration_ticks)));
    let byte_len = packet.buf().len() as f64;

    if !start.is_finite() || !end.is_finite() || byte_len <= 0.0 {
        return;
    }

    let packet_end = if end > start { end } else { start + 0.001 };
    let total_span = packet_end - start;
    let mut cursor = start;

    while cursor < packet_end {
        let bucket_index = cursor.floor().max(0.0) as usize;
        let bucket_end = ((bucket_index + 1) as f64).min(packet_end);
        let share = ((bucket_end - cursor) / total_span).clamp(0.0, 1.0);
        if bucket_bytes.len() <= bucket_index {
            bucket_bytes.resize(bucket_index + 1, 0.0);
        }
        bucket_bytes[bucket_index] += byte_len * share;
        cursor = bucket_end;
    }
}

fn time_to_seconds(time: symphonia::core::units::Time) -> f64 {
    time.seconds as f64 + time.frac
}

fn bytes_to_kbps(bytes: f64) -> u16 {
    ((bytes * 8.0) / 1000.0).round().clamp(0.0, u16::MAX as f64) as u16
}

fn format_label_from_lofty_file_type(file_type: FileType, extension: Option<&str>) -> String {
    match file_type {
        FileType::Aac => "AAC".to_string(),
        FileType::Aiff => "AIFF".to_string(),
        FileType::Flac => "FLAC".to_string(),
        FileType::Mpeg => format_label_from_extension(extension),
        FileType::Mp4 => format_label_from_extension(extension),
        FileType::Opus => "Opus".to_string(),
        FileType::Vorbis => "Vorbis".to_string(),
        FileType::Wav => "WAV".to_string(),
        FileType::WavPack => "WavPack".to_string(),
        _ => format_label_from_extension(extension),
    }
}

fn format_label_from_extension(extension: Option<&str>) -> String {
    match extension
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase()
        .as_str()
    {
        "aac" => "AAC".to_string(),
        "ac3" => raw_surround_format_label(Path::new("a.ac3")),
        "aif" | "aiff" | "aifc" | "afc" => "AIFF".to_string(),
        "alac" => "ALAC".to_string(),
        "dts" => raw_surround_format_label(Path::new("a.dts")),
        "flac" => "FLAC".to_string(),
        "m4a" | "m4b" | "m4p" | "m4r" | "mp4" => "AAC".to_string(),
        "mp1" => "MP1".to_string(),
        "mp2" => "MP2".to_string(),
        "mp3" => "MP3".to_string(),
        "ogg" => "Vorbis".to_string(),
        "opus" => "Opus".to_string(),
        "wav" | "wave" => "WAV".to_string(),
        "wv" => "WavPack".to_string(),
        other if !other.is_empty() => other.to_ascii_uppercase(),
        _ => String::new(),
    }
}

fn codec_label(codec: CodecType) -> Option<&'static str> {
    match codec {
        CODEC_TYPE_AAC => Some("AAC"),
        CODEC_TYPE_ALAC => Some("ALAC"),
        CODEC_TYPE_FLAC => Some("FLAC"),
        CODEC_TYPE_MP1 => Some("MP1"),
        CODEC_TYPE_MP2 => Some("MP2"),
        CODEC_TYPE_MP3 => Some("MP3"),
        CODEC_TYPE_OPUS => Some("Opus"),
        CODEC_TYPE_VORBIS => Some("Vorbis"),
        CODEC_TYPE_WAVPACK => Some("WavPack"),
        CODEC_TYPE_PCM_ALAW
        | CODEC_TYPE_PCM_F32BE
        | CODEC_TYPE_PCM_F32LE
        | CODEC_TYPE_PCM_F64BE
        | CODEC_TYPE_PCM_F64LE
        | CODEC_TYPE_PCM_MULAW
        | CODEC_TYPE_PCM_S16BE
        | CODEC_TYPE_PCM_S16BE_PLANAR
        | CODEC_TYPE_PCM_S16LE
        | CODEC_TYPE_PCM_S16LE_PLANAR
        | CODEC_TYPE_PCM_S24BE
        | CODEC_TYPE_PCM_S24BE_PLANAR
        | CODEC_TYPE_PCM_S24LE
        | CODEC_TYPE_PCM_S24LE_PLANAR
        | CODEC_TYPE_PCM_S32BE
        | CODEC_TYPE_PCM_S32BE_PLANAR
        | CODEC_TYPE_PCM_S32LE
        | CODEC_TYPE_PCM_S32LE_PLANAR
        | CODEC_TYPE_PCM_S8
        | CODEC_TYPE_PCM_S8_PLANAR
        | CODEC_TYPE_PCM_U16BE
        | CODEC_TYPE_PCM_U16BE_PLANAR
        | CODEC_TYPE_PCM_U16LE
        | CODEC_TYPE_PCM_U16LE_PLANAR
        | CODEC_TYPE_PCM_U24BE
        | CODEC_TYPE_PCM_U24BE_PLANAR
        | CODEC_TYPE_PCM_U24LE
        | CODEC_TYPE_PCM_U24LE_PLANAR
        | CODEC_TYPE_PCM_U32BE
        | CODEC_TYPE_PCM_U32BE_PLANAR
        | CODEC_TYPE_PCM_U32LE
        | CODEC_TYPE_PCM_U32LE_PLANAR
        | CODEC_TYPE_PCM_U8
        | CODEC_TYPE_PCM_U8_PLANAR => Some("PCM"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::{
        bytes_to_kbps, format_label_from_extension, MetadataEvent, MetadataService, TrackMetadata,
    };
    use crate::raw_audio::write_test_apev2_file;
    use std::path::PathBuf;
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    fn metadata_test_path(name: &str, ext: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|value| value.as_nanos())
            .unwrap_or(0);
        path.push(format!(
            "ferrous-metadata-{name}-{}-{nanos}.{ext}",
            std::process::id()
        ));
        path
    }

    #[test]
    fn format_label_prefers_user_facing_names() {
        assert_eq!(format_label_from_extension(Some("mp3")), "MP3");
        assert_eq!(format_label_from_extension(Some("flac")), "FLAC");
        assert_eq!(format_label_from_extension(Some("m4a")), "AAC");
        assert_eq!(format_label_from_extension(Some("ac3")), "AC3");
        assert_eq!(format_label_from_extension(Some("dts")), "DTS");
    }

    #[test]
    fn displayed_bitrate_uses_timeline_before_fallback() {
        let metadata = TrackMetadata {
            bitrate_kbps: Some(320),
            current_bitrate_kbps: Some(280),
            bitrate_timeline_kbps: vec![905, 777],
            ..TrackMetadata::default()
        };
        assert_eq!(metadata.displayed_bitrate_kbps(0.2), Some(905));
        assert_eq!(metadata.displayed_bitrate_kbps(1.4), Some(777));
        assert_eq!(metadata.displayed_bitrate_kbps(9.0), Some(280));
    }

    #[test]
    fn kbps_rounding_matches_expected_values() {
        assert_eq!(bytes_to_kbps(113_125.0), 905);
    }

    #[test]
    fn metadata_service_reads_appended_apev2_for_raw_surround_files() {
        let path = metadata_test_path("apev2", "ac3");
        write_test_apev2_file(
            &path,
            &[
                ("Title", "Harvest"),
                ("Artist", "Opeth"),
                ("Album", "In Live Concert at the Royal Albert Hall"),
                ("Genre", "Progressive death metal"),
                ("Year", "2010"),
                ("Track", "03/8"),
            ],
            true,
        );

        let (service, rx) = MetadataService::new_with_delay(Duration::ZERO);
        service.request(path.clone());

        let mut seen = None;
        for _ in 0..3 {
            let event = rx
                .recv_timeout(Duration::from_secs(2))
                .expect("metadata event");
            let MetadataEvent::Loaded(metadata) = event;
            if metadata.title == "Harvest" {
                seen = Some(metadata);
                break;
            }
        }

        let metadata = seen.expect("raw metadata");
        assert_eq!(metadata.title, "Harvest");
        assert_eq!(metadata.artist, "Opeth");
        assert_eq!(metadata.album, "In Live Concert at the Royal Albert Hall");
        assert_eq!(metadata.genre, "Progressive death metal");
        assert_eq!(metadata.year, Some(2010));
        assert_eq!(metadata.format_label, "AC3");

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn metadata_service_keeps_raw_dts_format_label() {
        let path = metadata_test_path("dts-format", "dts");
        write_test_apev2_file(&path, &[("Title", "The Leper Affinity")], true);

        let (service, rx) = MetadataService::new_with_delay(Duration::ZERO);
        service.request(path.clone());

        let mut final_metadata = None;
        while let Ok(event) = rx.recv_timeout(Duration::from_millis(250)) {
            let MetadataEvent::Loaded(metadata) = event;
            final_metadata = Some(metadata);
        }

        let metadata = final_metadata.expect("final metadata");
        assert_eq!(metadata.format_label, "DTS");

        let _ = std::fs::remove_file(path);
    }
}

fn load_folder_cover_art(track_path: &PathBuf) -> Option<(usize, usize, Vec<u8>)> {
    let dir = track_path.parent()?;
    let mut candidates = vec![
        "cover.jpg",
        "cover.jpeg",
        "cover.png",
        "folder.jpg",
        "folder.jpeg",
        "folder.png",
        "front.jpg",
        "front.png",
    ]
    .into_iter()
    .map(|n| dir.join(n))
    .collect::<Vec<_>>();

    if let Ok(read_dir) = std::fs::read_dir(dir) {
        for ent in read_dir.flatten() {
            let p = ent.path();
            if !p.is_file() {
                continue;
            }
            let Some(ext) = p.extension().and_then(|e| e.to_str()) else {
                continue;
            };
            let ext = ext.to_ascii_lowercase();
            if (ext == "jpg" || ext == "jpeg" || ext == "png")
                && !candidates.iter().any(|c| c == &p)
            {
                candidates.push(p);
            }
        }
    }

    for p in candidates {
        if !p.is_file() {
            continue;
        }
        if let Ok(bytes) = std::fs::read(&p) {
            if let Ok(img) = image::load_from_memory(&bytes) {
                let rgba = img.to_rgba8();
                return Some((
                    rgba.width() as usize,
                    rgba.height() as usize,
                    rgba.into_raw(),
                ));
            }
        }
    }
    None
}

fn cover_cache_dir() -> Option<PathBuf> {
    let cache_base = std::env::var_os("XDG_CACHE_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| Path::new(&home).join(".cache")))?;
    Some(cache_base.join("ferrous").join("embedded_covers"))
}

fn cache_embedded_cover_png(
    track_path: &Path,
    width: usize,
    height: usize,
    rgba: &[u8],
) -> Option<String> {
    let cache_dir = cover_cache_dir()?;
    if std::fs::create_dir_all(&cache_dir).is_err() {
        return None;
    }

    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    track_path.hash(&mut hasher);
    let key = hasher.finish();
    let out_path = cache_dir.join(format!("{key:016x}.png"));

    if !out_path.is_file() {
        let dims_match = image::RgbaImage::from_raw(width as u32, height as u32, rgba.to_vec())?;
        if dims_match
            .save_with_format(&out_path, image::ImageFormat::Png)
            .is_err()
        {
            return None;
        }
    }

    Some(out_path.to_string_lossy().to_string())
}
