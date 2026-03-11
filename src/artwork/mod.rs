use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, bail, Context, Result};
use image::codecs::jpeg::JpegEncoder;
use image::{DynamicImage, GenericImageView, ImageFormat};
use lofty::config::WriteOptions;
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::picture::{MimeType, Picture, PictureType};
use lofty::prelude::Accessor;
use lofty::tag::Tag;
use url::Url;

use crate::raw_audio::{is_raw_surround_file, read_appended_apev2_text_metadata};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ItunesAlbumArtworkUrls {
    pub original_url: String,
    pub high_res_fallback_url: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NormalizedArtwork {
    pub bytes: Vec<u8>,
    pub extension: &'static str,
    pub mime_type: MimeType,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApplyArtworkOutcome {
    pub affected_track_paths: Vec<PathBuf>,
    pub cover_path_override: Option<PathBuf>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ArtworkSource {
    Sidecar(PathBuf),
    Embedded,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct TrackIdentity {
    artist: String,
    album: String,
}

#[must_use]
pub fn derive_itunes_album_artwork_urls(artwork_url_100: &str) -> Option<ItunesAlbumArtworkUrls> {
    let high_res_seed = artwork_url_100.replace("100x100bb", "100000x100000-999");
    let parsed = Url::parse(&high_res_seed).ok()?;
    let high_res_fallback_url = format!("https://is5-ssl.mzstatic.com{}", parsed.path());

    let thumb_marker = "/image/thumb/";
    let thumb_path = parsed.path();
    let thumb_offset = thumb_path.find(thumb_marker)?;
    let remaining = &thumb_path[thumb_offset + thumb_marker.len()..];
    let slash = remaining.rfind('/')?;
    let original_path = &remaining[..slash];

    Some(ItunesAlbumArtworkUrls {
        original_url: format!("https://a5.mzstatic.com/us/r1000/0/{original_path}"),
        high_res_fallback_url,
    })
}

/// Normalize an image file to the square artwork payload Ferrous applies.
///
/// # Errors
///
/// Returns an error when the file cannot be read, decoded, or re-encoded.
pub fn normalize_artwork_file(path: &Path) -> Result<NormalizedArtwork> {
    let bytes = fs::read(path)
        .with_context(|| format!("failed to read artwork file {}", path.to_string_lossy()))?;
    normalize_artwork_bytes(&bytes)
}

/// Normalize encoded image bytes to the square artwork payload Ferrous applies.
///
/// # Errors
///
/// Returns an error when the bytes cannot be decoded or re-encoded.
pub fn normalize_artwork_bytes(bytes: &[u8]) -> Result<NormalizedArtwork> {
    let format = image::guess_format(bytes).context("failed to determine artwork image format")?;
    let (extension, mime_type) = match format {
        ImageFormat::Png => ("png", MimeType::Png),
        ImageFormat::Jpeg => ("jpg", MimeType::Jpeg),
        _ => bail!("unsupported artwork image format"),
    };

    let decoded = image::load_from_memory_with_format(bytes, format)
        .context("failed to decode artwork image")?;
    let normalized = center_crop_to_square(&decoded);
    let out_bytes = if decoded.dimensions() == normalized.dimensions() {
        bytes.to_vec()
    } else {
        encode_dynamic_image(&normalized, format)?
    };

    Ok(NormalizedArtwork {
        bytes: out_bytes,
        extension,
        mime_type,
    })
}

/// Apply the provided artwork file to the current track context.
///
/// # Errors
///
/// Returns an error when the artwork cannot be normalized or written to the
/// active sidecar or embedded artwork targets.
pub fn apply_artwork_to_track(
    track_path: &Path,
    artwork_path: &Path,
) -> Result<ApplyArtworkOutcome> {
    let normalized = normalize_artwork_file(artwork_path)?;
    match classify_artwork_source(track_path) {
        ArtworkSource::Sidecar(sidecar_path) => {
            let cover_path = write_sidecar_replacement(&sidecar_path, &normalized)?;
            Ok(ApplyArtworkOutcome {
                affected_track_paths: sibling_supported_audio_files(track_path)?,
                cover_path_override: Some(cover_path),
            })
        }
        ArtworkSource::Embedded => {
            let affected = rewrite_embedded_artwork(track_path, &normalized)?;
            Ok(ApplyArtworkOutcome {
                affected_track_paths: affected,
                cover_path_override: None,
            })
        }
    }
}

fn center_crop_to_square(image: &DynamicImage) -> DynamicImage {
    let (width, height) = image.dimensions();
    if width == height {
        return image.clone();
    }

    let side = width.min(height);
    let x = (width - side) / 2;
    let y = (height - side) / 2;
    image.crop_imm(x, y, side, side)
}

fn encode_dynamic_image(image: &DynamicImage, format: ImageFormat) -> Result<Vec<u8>> {
    match format {
        ImageFormat::Png => {
            let mut cursor = Cursor::new(Vec::new());
            image
                .write_to(&mut cursor, ImageFormat::Png)
                .context("failed to encode normalized PNG artwork")?;
            Ok(cursor.into_inner())
        }
        ImageFormat::Jpeg => {
            let mut out = Vec::new();
            let mut encoder = JpegEncoder::new_with_quality(&mut out, 95);
            encoder
                .encode_image(image)
                .context("failed to encode normalized JPEG artwork")?;
            Ok(out)
        }
        _ => Err(anyhow!("unsupported artwork image format")),
    }
}

fn classify_artwork_source(track_path: &Path) -> ArtworkSource {
    match find_sidecar_for_track(track_path) {
        Some(sidecar) => ArtworkSource::Sidecar(sidecar),
        None => ArtworkSource::Embedded,
    }
}

fn write_sidecar_replacement(
    current_sidecar_path: &Path,
    artwork: &NormalizedArtwork,
) -> Result<PathBuf> {
    let target_path = if current_sidecar_path
        .extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| ext.eq_ignore_ascii_case(artwork.extension))
    {
        current_sidecar_path.to_path_buf()
    } else {
        current_sidecar_path.with_extension(artwork.extension)
    };

    write_bytes_atomically(&target_path, &artwork.bytes)?;
    if target_path != current_sidecar_path && current_sidecar_path.is_file() {
        fs::remove_file(current_sidecar_path).with_context(|| {
            format!(
                "failed to remove previous sidecar artwork {}",
                current_sidecar_path.to_string_lossy()
            )
        })?;
    }
    Ok(target_path)
}

fn rewrite_embedded_artwork(
    current_track_path: &Path,
    artwork: &NormalizedArtwork,
) -> Result<Vec<PathBuf>> {
    let current_identity = track_identity(current_track_path);
    let current_album_key = normalize_match_key(&current_identity.album);
    let current_artist_key = normalize_match_key(&current_identity.artist);
    if current_album_key.is_empty() || current_artist_key.is_empty() {
        bail!("current track album/artist metadata is missing");
    }

    let mut affected = Vec::new();
    for candidate in sibling_supported_audio_files(current_track_path)? {
        let identity = track_identity(&candidate);
        if normalize_match_key(&identity.album) != current_album_key
            || normalize_match_key(&identity.artist) != current_artist_key
        {
            continue;
        }
        rewrite_embedded_artwork_for_file(&candidate, artwork)?;
        affected.push(candidate);
    }

    if affected.is_empty() {
        bail!("no matching sibling audio files were updated");
    }
    Ok(affected)
}

fn rewrite_embedded_artwork_for_file(path: &Path, artwork: &NormalizedArtwork) -> Result<()> {
    let mut tagged = lofty::read_from_path(path)
        .with_context(|| format!("failed to read tags from {}", path.to_string_lossy()))?;
    if tagged.primary_tag_mut().is_none() {
        tagged.insert_tag(Tag::new(tagged.primary_tag_type()));
    }
    let tag = if let Some(tag) = tagged.primary_tag_mut() {
        tag
    } else if let Some(tag) = tagged.first_tag_mut() {
        tag
    } else {
        return Err(anyhow!(
            "failed to resolve writable tag for {}",
            path.to_string_lossy()
        ));
    };

    tag.remove_picture_type(PictureType::CoverFront);
    tag.push_picture(
        Picture::unchecked(artwork.bytes.clone())
            .mime_type(artwork.mime_type.clone())
            .pic_type(PictureType::CoverFront)
            .build(),
    );
    tagged
        .save_to_path(path, WriteOptions::default())
        .with_context(|| {
            format!(
                "failed to save updated artwork to {}",
                path.to_string_lossy()
            )
        })
}

fn sibling_supported_audio_files(track_path: &Path) -> Result<Vec<PathBuf>> {
    let dir = track_path
        .parent()
        .ok_or_else(|| anyhow!("track path has no parent directory"))?;
    let mut out = Vec::new();
    for entry in fs::read_dir(dir)
        .with_context(|| format!("failed to read directory {}", dir.to_string_lossy()))?
    {
        let path = entry?.path();
        if path.is_file() && crate::library::is_supported_audio(&path) {
            out.push(path);
        }
    }
    out.sort();
    Ok(out)
}

fn track_identity(path: &Path) -> TrackIdentity {
    let mut artist = String::new();
    let mut album = String::new();

    if let Ok(tagged) = lofty::read_from_path(path) {
        if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
            if let Some(value) = tag.artist() {
                artist = value.into_owned();
            }
            if let Some(value) = tag.album() {
                album = value.into_owned();
            }
        }
    }

    if is_raw_surround_file(path) {
        if let Some(tagged) = read_appended_apev2_text_metadata(path) {
            if let Some(value) = tagged.artist {
                artist = value;
            }
            if let Some(value) = tagged.album {
                album = value;
            }
        }
    }

    TrackIdentity { artist, album }
}

fn normalize_match_key(value: &str) -> String {
    value
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .trim()
        .to_ascii_lowercase()
}

fn find_sidecar_for_track(path: &Path) -> Option<PathBuf> {
    let dir = path.parent()?;
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
    .map(|name| dir.join(name))
    .collect::<Vec<_>>();

    if let Ok(read_dir) = fs::read_dir(dir) {
        for entry in read_dir.flatten() {
            let path = entry.path();
            if !path.is_file() {
                continue;
            }
            let Some(ext) = path.extension().and_then(|ext| ext.to_str()) else {
                continue;
            };
            let ext = ext.to_ascii_lowercase();
            if (ext == "jpg" || ext == "jpeg" || ext == "png")
                && !candidates.iter().any(|candidate| candidate == &path)
            {
                candidates.push(path);
            }
        }
    }

    candidates.into_iter().find(|candidate| candidate.is_file())
}

fn write_bytes_atomically(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| anyhow!("path has no parent directory"))?;
    let temp_path = parent.join(format!(
        ".{}.{}.tmp",
        path.file_name()
            .and_then(|name| name.to_str())
            .unwrap_or("artwork"),
        unique_suffix()
    ));
    fs::write(&temp_path, bytes).with_context(|| {
        format!(
            "failed to write temporary artwork {}",
            temp_path.to_string_lossy()
        )
    })?;
    fs::rename(&temp_path, path).with_context(|| {
        format!(
            "failed to move temporary artwork {} into place at {}",
            temp_path.to_string_lossy(),
            path.to_string_lossy()
        )
    })
}

fn unique_suffix() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgb, Rgba};

    fn test_dir(name: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!("ferrous-artwork-{name}-{}", unique_suffix()));
        fs::create_dir_all(&path).expect("create test dir");
        path
    }

    #[test]
    fn derives_itunes_album_original_and_high_res_urls() {
        let urls = derive_itunes_album_artwork_urls(
            "https://is1-ssl.mzstatic.com/image/thumb/Music126/v4/aa/bb/cc/dddd/source/100x100bb.jpg",
        )
        .expect("itunes urls");
        assert_eq!(
            urls.high_res_fallback_url,
            "https://is5-ssl.mzstatic.com/image/thumb/Music126/v4/aa/bb/cc/dddd/source/100000x100000-999.jpg"
        );
        assert_eq!(
            urls.original_url,
            "https://a5.mzstatic.com/us/r1000/0/Music126/v4/aa/bb/cc/dddd/source"
        );
    }

    #[test]
    fn normalizes_non_square_image_to_center_square() {
        let image = ImageBuffer::from_fn(6, 4, |x, _| Rgba([x as u8, 0, 0, 255]));
        let mut cursor = Cursor::new(Vec::new());
        DynamicImage::ImageRgba8(image)
            .write_to(&mut cursor, ImageFormat::Png)
            .expect("encode png");

        let normalized = normalize_artwork_bytes(&cursor.into_inner()).expect("normalize");
        let decoded = image::load_from_memory(&normalized.bytes).expect("decode normalized image");
        assert_eq!(decoded.dimensions(), (4, 4));
        assert_eq!(decoded.get_pixel(0, 0)[0], 1);
        assert_eq!(decoded.get_pixel(3, 0)[0], 4);
    }

    #[test]
    fn sidecar_replacement_with_extension_change_preserves_stem_and_deletes_old_file() {
        let dir = test_dir("sidecar-extension-change");
        let sidecar_path = dir.join("cover.jpg");
        fs::write(&sidecar_path, b"old").expect("write old sidecar");

        let png = ImageBuffer::from_pixel(4, 4, Rgb([0, 0, 255]));
        let mut cursor = Cursor::new(Vec::new());
        DynamicImage::ImageRgb8(png)
            .write_to(&mut cursor, ImageFormat::Png)
            .expect("encode png");
        let normalized = normalize_artwork_bytes(&cursor.into_inner()).expect("normalize");

        write_sidecar_replacement(&sidecar_path, &normalized).expect("replace sidecar");

        assert!(!sidecar_path.exists());
        assert!(dir.join("cover.png").exists());
    }
}
