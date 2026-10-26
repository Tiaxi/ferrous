// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};
use image::codecs::jpeg::JpegEncoder;
use image::{DynamicImage, GenericImageView, ImageFormat};
use lofty::file::{FileType, TaggedFile, TaggedFileExt};
use lofty::picture::{MimeType, Picture, PictureType};
use lofty::prelude::Accessor;
use lofty::tag::Tag;
use url::Url;

use crate::file_edit_lock;
use crate::folder_cover::{find_folder_cover, find_named_cover};
use crate::raw_audio::{is_raw_surround_file, read_appended_apev2_text_metadata};
use crate::safe_save::{check_rewritable, create_file, replace_file};
use crate::tag_write::{save_tag, seeded_tag};
use crate::vorbis_comment_scan::skipped_comments;

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

/// A track file's size and modification time, to tell whether it is still
/// the file that was seen earlier.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TrackFileIdentity {
    pub len: u64,
    /// Milliseconds since the Unix epoch.
    pub modified_ms: i64,
}

impl TrackFileIdentity {
    /// The identity of the file at `path` now.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be read.
    pub fn of(path: &Path) -> std::io::Result<Self> {
        let metadata = fs::metadata(path)?;
        let modified = metadata.modified()?;
        let modified_ms = match modified.duration_since(std::time::UNIX_EPOCH) {
            Ok(after) => i64::try_from(after.as_millis()).unwrap_or(i64::MAX),
            Err(before) => i64::try_from(before.duration().as_millis()).map_or(i64::MIN, |ms| -ms),
        };
        Ok(Self {
            len: metadata.len(),
            modified_ms,
        })
    }
}

/// Check that `track_path` is still the file `expected` describes.
///
/// # Errors
///
/// Returns an error for the user when the file is gone, or is another file
/// or a changed one.
pub fn check_track_unchanged(track_path: &Path, expected: TrackFileIdentity) -> Result<()> {
    match TrackFileIdentity::of(track_path) {
        Ok(identity) if identity == expected => Ok(()),
        Ok(_) => bail!(
            "{} changed after the artwork search, so nothing was changed; \
             search again to apply artwork to it",
            file_label(track_path)
        ),
        Err(_) => bail!(
            "{} was moved, renamed or deleted after the artwork search, so nothing \
             was changed; play it and search again to apply artwork to it",
            file_label(track_path)
        ),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApplyArtworkOutcome {
    /// Tracks whose cover changed.
    pub affected_track_paths: Vec<PathBuf>,
    /// The folder image that now holds the cover of the affected tracks,
    /// when the artwork went to a folder image rather than into the tracks.
    pub cover_path_override: Option<PathBuf>,
    /// Album tracks that could not be updated, with the reason for each.
    pub failed_tracks: Vec<(PathBuf, String)>,
}

impl ApplyArtworkOutcome {
    /// A message for the user that names the album tracks that were not
    /// updated, if there are any.
    #[must_use]
    pub fn failure_summary(&self) -> Option<String> {
        if self.failed_tracks.is_empty() {
            return None;
        }
        Some(format!(
            "album art was applied to {} of {} album tracks; not updated: {}",
            self.affected_track_paths.len(),
            self.affected_track_paths.len() + self.failed_tracks.len(),
            describe_track_errors(&self.failed_tracks)
        ))
    }
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
/// When the track's folder shows an image as its cover, the artwork goes
/// to a folder image: the one with a recognised cover name, or a new
/// `cover.<ext>` next to the folder's other images. Otherwise it is
/// embedded into every track in the folder that has the track's album tag.
///
/// # Errors
///
/// Returns an error when the artwork cannot be normalized, when the folder
/// image cannot be written, or when any album track cannot take embedded
/// artwork or any track in the folder cannot be read; in those cases no
/// track is changed. Tracks that fail while being written are listed in the
/// outcome instead.
pub fn apply_artwork_to_track(
    track_path: &Path,
    artwork_path: &Path,
) -> Result<ApplyArtworkOutcome> {
    let normalized = normalize_artwork_file(artwork_path)?;
    let dir = track_path
        .parent()
        .ok_or_else(|| anyhow!("track path has no parent directory"))?;
    if find_folder_cover(dir).is_none() {
        return rewrite_embedded_artwork(track_path, &normalized);
    }
    let affected_track_paths = sibling_supported_audio_files(track_path)?;
    let cover_path = write_folder_cover(dir, &normalized)?;
    Ok(ApplyArtworkOutcome {
        affected_track_paths,
        cover_path_override: Some(cover_path),
        failed_tracks: Vec::new(),
    })
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

/// Write `artwork` as the cover image of `dir` and return its path.
///
/// Only an image with a recognised cover name is replaced, in its own
/// format, so no file is renamed or removed. Without one, a new
/// `cover.<ext>` is created; other images, such as `back.jpg`, stay as they
/// are and rank below it.
fn write_folder_cover(dir: &Path, artwork: &NormalizedArtwork) -> Result<PathBuf> {
    if let Some(existing) = find_named_cover(dir) {
        let bytes = encode_for_path(artwork, &existing)?;
        replace_file(&existing, &bytes).map_err(anyhow::Error::msg)?;
        return Ok(existing);
    }
    let path = dir.join(format!("cover.{}", artwork.extension));
    create_file(&path, &artwork.bytes).map_err(anyhow::Error::msg)?;
    Ok(path)
}

/// `artwork` in the image format that `path`'s extension names.
fn encode_for_path(artwork: &NormalizedArtwork, path: &Path) -> Result<Vec<u8>> {
    let format = path
        .extension()
        .and_then(ImageFormat::from_extension)
        .ok_or_else(|| anyhow!("unsupported cover image {}", path.display()))?;
    if ImageFormat::from_extension(artwork.extension) == Some(format) {
        return Ok(artwork.bytes.clone());
    }
    let decoded =
        image::load_from_memory(&artwork.bytes).context("failed to decode artwork image")?;
    encode_dynamic_image(&decoded, format)
}

/// Embed `artwork` into every track in the current track's folder that has
/// its album tag, whatever each track's artist, so compilations are updated
/// as a whole.
///
/// Every track is checked first and nothing is written unless all of them
/// can take the artwork, and unless the album of every track in the folder
/// is known. A track that still fails while being written is reported in the
/// outcome, and the remaining tracks are written.
fn rewrite_embedded_artwork(
    current_track_path: &Path,
    artwork: &NormalizedArtwork,
) -> Result<ApplyArtworkOutcome> {
    let album_key = normalize_match_key(&track_album(current_track_path).map_err(|error| {
        anyhow!(
            "the tracks of the album of {} cannot be found: {error}",
            file_label(current_track_path)
        )
    })?);
    if album_key.is_empty() {
        bail!(
            "{} has no album tag, so the tracks of its album cannot be found",
            file_label(current_track_path)
        );
    }
    let siblings = sibling_supported_audio_files(current_track_path)?;
    let sibling_count = siblings.len();
    let mut members = Vec::new();
    let mut unreadable = Vec::new();
    for candidate in siblings {
        match track_album(&candidate) {
            Ok(album) if normalize_match_key(&album) == album_key => members.push(candidate),
            Ok(_) => {}
            Err(error) => unreadable.push((candidate, error)),
        }
    }
    if !unreadable.is_empty() {
        bail!(
            "{} of {} tracks in this folder cannot be read, so the album's tracks are \
             unknown and none were changed: {}",
            unreadable.len(),
            sibling_count,
            describe_track_errors(&unreadable)
        );
    }

    let unwritable = members
        .iter()
        .filter_map(|path| {
            check_embedded_artwork_writable(path, artwork)
                .err()
                .map(|error| (path.clone(), error))
        })
        .collect::<Vec<_>>();
    if !unwritable.is_empty() {
        bail!(
            "{} of {} album tracks cannot be updated, so none were changed: {}",
            unwritable.len(),
            members.len(),
            describe_track_errors(&unwritable)
        );
    }

    Ok(write_each_track(members, |path| {
        rewrite_embedded_artwork_for_file(path, artwork)
    }))
}

/// Write every track with `write`, continuing past failures.
fn write_each_track(
    tracks: Vec<PathBuf>,
    write: impl Fn(&Path) -> Result<(), String>,
) -> ApplyArtworkOutcome {
    let mut outcome = ApplyArtworkOutcome {
        affected_track_paths: Vec::new(),
        cover_path_override: None,
        failed_tracks: Vec::new(),
    };
    for path in tracks {
        match write(&path) {
            Ok(()) => outcome.affected_track_paths.push(path),
            Err(error) => outcome.failed_tracks.push((path, error)),
        }
    }
    outcome
}

/// Check, without writing, that `path` can take embedded artwork now:
/// saving would not delete comments lofty cannot read, as [`save_tag`]
/// refuses then, and the file can be replaced.
fn check_embedded_artwork_writable(path: &Path, artwork: &NormalizedArtwork) -> Result<(), String> {
    let (file_type, _) = edited_artwork_tag(path, artwork)?;
    if let Some(refusal) = skipped_comments(path, file_type)?.save_refusal() {
        return Err(refusal);
    }
    check_rewritable(path)
}

fn rewrite_embedded_artwork_for_file(
    path: &Path,
    artwork: &NormalizedArtwork,
) -> Result<(), String> {
    let edit = file_edit_lock::lock(path);
    let (file_type, tag) = edited_artwork_tag(path, artwork)?;
    #[cfg(test)]
    edit.checkpoint();
    let saved =
        save_tag(path, file_type, &tag).map_err(|err| format!("failed to update artwork: {err}"));
    drop(edit);
    saved
}

/// The tag of `path` with `artwork` as its front cover.
///
/// The front cover goes first, so readers that show a track's first
/// picture, as Ferrous does, show the new artwork. Earlier front covers are
/// replaced; other pictures are kept after it in their order.
///
/// A file without its primary tag, such as an MP3 with only `ID3v1`, gets
/// one that starts from the tag readers show, as the tag editor does, so the
/// new tag does not hide the title, artist, and album. The other tags stay.
fn edited_artwork_tag(path: &Path, artwork: &NormalizedArtwork) -> Result<(FileType, Tag), String> {
    let tagged = crate::panic_guard::read_tags(path)
        .map_err(|err| format!("failed to read tags from {}: {err}", path.display()))?;
    let file_type = tagged.file_type();
    let primary_type = tagged.primary_tag_type();
    let mut tag = tagged
        .primary_tag()
        .cloned()
        .unwrap_or_else(|| seeded_tag(tagged.first_tag(), primary_type));

    let mut kept = Vec::new();
    while !tag.pictures().is_empty() {
        let picture = tag.remove_picture(0);
        if picture.pic_type() != PictureType::CoverFront {
            kept.push(picture);
        }
    }
    tag.push_picture(
        Picture::unchecked(artwork.bytes.clone())
            .mime_type(artwork.mime_type.clone())
            .pic_type(PictureType::CoverFront)
            .build(),
    );
    for picture in kept {
        tag.push_picture(picture);
    }
    Ok((file_type, tag))
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
        if path.is_file() && crate::library::is_audio_track_file(&path) {
            out.push(path);
        }
    }
    out.sort();
    Ok(out)
}

/// The album tag of `path`, empty when it has none.
///
/// # Errors
///
/// Returns a message when the tags of `path` cannot be read.
fn track_album(path: &Path) -> Result<String, String> {
    let read_error =
        |err: &dyn std::fmt::Display| format!("failed to read tags from {}: {err}", path.display());
    let tagged = crate::panic_guard::read_tags(path);
    let album = |tagged: &TaggedFile| {
        tagged
            .primary_tag()
            .or_else(|| tagged.first_tag())
            .and_then(|tag| tag.album().map(std::borrow::Cow::into_owned))
    };
    if !is_raw_surround_file(path) {
        return tagged
            .map(|tagged| album(&tagged).unwrap_or_default())
            .map_err(|err| read_error(&err));
    }
    // lofty reads no raw surround streams, so its error only means the
    // file has no tag lofty knows; the album is in an appended APE tag.
    if let Some(value) = read_appended_apev2_text_metadata(path).and_then(|tags| tags.album) {
        return Ok(value);
    }
    fs::File::open(path).map_err(|err| read_error(&err))?;
    Ok(tagged
        .ok()
        .and_then(|tagged| album(&tagged))
        .unwrap_or_default())
}

fn normalize_match_key(value: &str) -> String {
    value
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .trim()
        .to_ascii_lowercase()
}

/// `name (reason)` for each track, for messages to the user.
fn describe_track_errors(errors: &[(PathBuf, String)]) -> String {
    errors
        .iter()
        .map(|(path, error)| format!("{} ({error})", file_label(path)))
        .collect::<Vec<_>>()
        .join("; ")
}

fn file_label(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tag_write::write_test_flac;
    use image::{ImageBuffer, Rgb, Rgba};
    use std::sync::atomic::{AtomicU64, Ordering};

    fn test_dir(name: &str) -> PathBuf {
        static NEXT_ID: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "ferrous-artwork-{name}-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).expect("create test dir");
        path
    }

    fn encoded_image(color: [u8; 3], format: ImageFormat) -> Vec<u8> {
        let mut cursor = Cursor::new(Vec::new());
        DynamicImage::ImageRgb8(ImageBuffer::from_pixel(4, 4, Rgb(color)))
            .write_to(&mut cursor, format)
            .expect("encode image");
        cursor.into_inner()
    }

    /// An artwork file outside the album folder, as the iTunes download is.
    fn artwork_file(root: &Path, format: ImageFormat) -> (PathBuf, Vec<u8>) {
        let bytes = encoded_image([0, 0, 255], format);
        let path = root.join(match format {
            ImageFormat::Png => "artwork.png",
            _ => "artwork.jpg",
        });
        fs::write(&path, &bytes).expect("write artwork");
        (path, bytes)
    }

    fn dir_entries(dir: &Path) -> Vec<String> {
        let mut entries = fs::read_dir(dir)
            .expect("read dir")
            .map(|entry| {
                entry
                    .expect("entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        entries.sort();
        entries
    }

    fn front_cover(path: &Path) -> Option<Vec<u8>> {
        let tagged = lofty::read_from_path(path).expect("read tags");
        let tag = tagged.primary_tag().expect("primary tag");
        tag.get_picture_type(PictureType::CoverFront)
            .map(|picture| picture.data().to_vec())
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
    fn folder_with_only_other_images_keeps_them_and_gets_a_new_cover() {
        let root = test_dir("other-images");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        let track_bytes = fs::read(&track).expect("read track");
        fs::write(album.join("back.jpg"), b"back scan").expect("write back scan");
        fs::write(album.join("booklet01.jpg"), b"booklet").expect("write booklet");
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Png);

        let outcome = apply_artwork_to_track(&track, &artwork).expect("apply artwork");

        let cover = album.join("cover.png");
        assert_eq!(outcome.cover_path_override.as_ref(), Some(&cover));
        assert_eq!(outcome.affected_track_paths, std::slice::from_ref(&track));
        assert_eq!(outcome.failure_summary(), None);
        assert_eq!(fs::read(&cover).expect("read cover"), artwork_bytes);
        assert_eq!(
            fs::read(album.join("back.jpg")).expect("read back"),
            b"back scan"
        );
        assert_eq!(
            fs::read(album.join("booklet01.jpg")).expect("read booklet"),
            b"booklet"
        );
        assert_eq!(fs::read(&track).expect("read track"), track_bytes);
        assert_eq!(
            dir_entries(&album),
            ["01.flac", "back.jpg", "booklet01.jpg", "cover.png"]
        );
        // The library shows the new cover rather than the scans.
        assert_eq!(find_folder_cover(&album), Some(cover));
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn a_track_moved_or_changed_since_the_search_is_refused() {
        let root = test_dir("moved-track");
        let track = root.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        let identity = TrackFileIdentity::of(&track).expect("identity");
        check_track_unchanged(&track, identity).expect("the same file");

        let changed = TrackFileIdentity {
            len: identity.len + 1,
            ..identity
        };
        let error = check_track_unchanged(&track, changed).expect_err("another file");
        assert!(
            error
                .to_string()
                .contains("changed after the artwork search"),
            "{error}"
        );

        fs::rename(&track, root.join("02.flac")).expect("rename track");
        let error = check_track_unchanged(&track, identity).expect_err("moved file");
        assert!(
            error.to_string().contains("moved, renamed or deleted"),
            "{error}"
        );
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn recognised_cover_is_replaced_in_its_own_format_and_nothing_is_removed() {
        let root = test_dir("recognised-cover");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        let cover = album.join("Cover.JPG");
        fs::write(&cover, encoded_image([255, 0, 0], ImageFormat::Jpeg)).expect("write cover");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&cover, fs::Permissions::from_mode(0o640)).expect("chmod");
        }
        fs::write(album.join("back.jpg"), b"back scan").expect("write back scan");
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let outcome = apply_artwork_to_track(&track, &artwork).expect("apply artwork");

        assert_eq!(outcome.cover_path_override.as_ref(), Some(&cover));
        let written = fs::read(&cover).expect("read cover");
        assert_eq!(image::guess_format(&written).ok(), Some(ImageFormat::Jpeg));
        let decoded = image::load_from_memory(&written).expect("decode cover");
        assert_eq!(decoded.dimensions(), (4, 4));
        let pixel = decoded.get_pixel(1, 1);
        assert!(pixel[0] < 16 && pixel[2] > 239, "{pixel:?}");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = fs::metadata(&cover).expect("metadata").permissions().mode();
            assert_eq!(mode & 0o777, 0o640);
        }
        assert_eq!(
            fs::read(album.join("back.jpg")).expect("read back"),
            b"back scan"
        );
        assert_eq!(dir_entries(&album), ["01.flac", "Cover.JPG", "back.jpg"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn jpeg_artwork_keeps_a_cover_jpeg_name() {
        let root = test_dir("cover-jpeg");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        let cover = album.join("cover.jpeg");
        fs::write(&cover, b"old cover").expect("write cover");
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Jpeg);

        let outcome = apply_artwork_to_track(&track, &artwork).expect("apply artwork");

        assert_eq!(outcome.cover_path_override.as_ref(), Some(&cover));
        assert_eq!(fs::read(&cover).expect("read cover"), artwork_bytes);
        assert_eq!(dir_entries(&album), ["01.flac", "cover.jpeg"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn a_new_cover_does_not_replace_what_already_has_its_name() {
        let root = test_dir("name-taken");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        fs::write(album.join("back.jpg"), b"back scan").expect("write back scan");
        fs::create_dir(album.join("cover.png")).expect("create directory");
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&track, &artwork).expect_err("name is taken");

        assert!(error.to_string().contains("already exists"), "{error}");
        assert!(album.join("cover.png").is_dir());
        assert_eq!(dir_entries(&album), ["01.flac", "back.jpg", "cover.png"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn embedded_artwork_updates_every_track_of_a_compilation() {
        let root = test_dir("compilation");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let tracks = [
            ("01.flac", "First Artist", "Hits Collection"),
            ("02.flac", "Second Artist", "Hits Collection"),
            ("03.flac", "Third Artist", "  hits   COLLECTION "),
        ]
        .map(|(name, artist, album_title)| {
            let path = album.join(name);
            write_test_flac(&path, &[("ARTIST", artist), ("ALBUM", album_title)]);
            path
        });
        let other = album.join("04.flac");
        write_test_flac(
            &other,
            &[("ARTIST", "First Artist"), ("ALBUM", "Other Album")],
        );
        let other_bytes = fs::read(&other).expect("read other album");
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Png);

        let outcome = apply_artwork_to_track(&tracks[1], &artwork).expect("apply artwork");

        assert_eq!(outcome.affected_track_paths, tracks);
        assert_eq!(outcome.cover_path_override, None);
        assert_eq!(outcome.failure_summary(), None);
        for track in &tracks {
            assert_eq!(
                front_cover(track).as_ref(),
                Some(&artwork_bytes),
                "{track:?}"
            );
        }
        assert_eq!(fs::read(&other).expect("read other album"), other_bytes);
        assert_eq!(
            dir_entries(&album),
            ["01.flac", "02.flac", "03.flac", "04.flac"]
        );
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn embedded_artwork_changes_nothing_when_an_album_track_cannot_take_it() {
        let root = test_dir("unwritable-member");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let flac = album.join("01.flac");
        write_test_flac(&flac, &[("ARTIST", "Artist"), ("ALBUM", "Album")]);
        // Raw DTS has an appended APE tag for its text but cannot hold pictures.
        let dts = album.join("02.dts");
        crate::raw_audio::write_test_apev2_file(&dts, &[("Album", "Album")], true);
        let before = [&flac, &dts].map(|path| fs::read(path).expect("read track"));
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&flac, &artwork).expect_err("DTS cannot be updated");

        let message = error.to_string();
        assert!(message.contains("1 of 2 album tracks"), "{message}");
        assert!(message.contains("none were changed"), "{message}");
        assert!(message.contains("02.dts ("), "{message}");
        assert!(!message.contains("01.flac"), "{message}");
        let after = [&flac, &dts].map(|path| fs::read(path).expect("read track"));
        assert_eq!(after, before);
        assert_eq!(dir_entries(&album), ["01.flac", "02.dts"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn embedded_artwork_changes_nothing_when_an_album_track_is_read_only() {
        use std::os::unix::fs::PermissionsExt;

        let root = test_dir("read-only-member");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let tracks = ["01.flac", "02.flac", "03.flac"].map(|name| {
            let path = album.join(name);
            write_test_flac(&path, &[("ALBUM", "Album")]);
            path
        });
        fs::set_permissions(&tracks[2], fs::Permissions::from_mode(0o444)).expect("chmod");
        // Root ignores file permissions, so the check only holds for normal users.
        if fs::OpenOptions::new().write(true).open(&tracks[2]).is_ok() {
            fs::remove_dir_all(root).expect("remove test dir");
            return;
        }
        let before = tracks
            .iter()
            .map(|path| fs::read(path).expect("read track"))
            .collect::<Vec<_>>();
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&tracks[0], &artwork).expect_err("read-only track");

        let message = error.to_string();
        assert!(message.contains("03.flac ("), "{message}");
        assert!(message.contains("for writing"), "{message}");
        let after = tracks
            .iter()
            .map(|path| fs::read(path).expect("read track"))
            .collect::<Vec<_>>();
        assert_eq!(after, before);
        assert_eq!(dir_entries(&album), ["01.flac", "02.flac", "03.flac"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn embedded_artwork_changes_nothing_when_saving_would_delete_comments() {
        use crate::vorbis_comment_scan::fixtures;

        let root = test_dir("unreadable-comments");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let readable = album.join("01.flac");
        write_test_flac(&readable, &[("ALBUM", "Album")]);
        let refused = album.join("02.flac");
        let block = fixtures::comment_block(&[b"ALBUM=Album", b"no separator"]);
        fs::write(&refused, fixtures::flac(&[&block])).expect("write track");
        let before = [&readable, &refused].map(|path| fs::read(path).expect("read track"));
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&readable, &artwork).expect_err("comments are kept");

        let message = error.to_string();
        assert!(
            message.starts_with("1 of 2 album tracks cannot be updated, so none were changed"),
            "{message}"
        );
        assert!(
            message.contains(
                "02.flac (1 comment in this file can't be read safely (1 without \"=\"); \
                 saving would delete it)"
            ),
            "{message}"
        );
        assert!(!message.contains("01.flac"), "{message}");
        let after = [&readable, &refused].map(|path| fs::read(path).expect("read track"));
        assert_eq!(after, before);
        assert_eq!(dir_entries(&album), ["01.flac", "02.flac"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    /// The front cover replaces every picture block, so a picture block
    /// lofty cannot read, which may hold another picture type, would be lost.
    #[test]
    fn embedded_artwork_changes_nothing_when_saving_would_delete_a_picture_block() {
        use crate::vorbis_comment_scan::fixtures;

        let root = test_dir("unreadable-picture");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let readable = album.join("01.flac");
        write_test_flac(&readable, &[("ALBUM", "Album")]);
        let refused = album.join("02.flac");
        let comments = fixtures::comment_block(&[b"ALBUM=Album"]);
        let malformed = fixtures::malformed_picture_block();
        fs::write(
            &refused,
            fixtures::flac_blocks(&[
                (fixtures::VORBIS_COMMENT, &comments),
                (fixtures::PICTURE, &malformed),
            ]),
        )
        .expect("write track");
        let before = [&readable, &refused].map(|path| fs::read(path).expect("read track"));
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&refused, &artwork).expect_err("picture is kept");

        let message = error.to_string();
        assert!(
            message.starts_with("1 of 2 album tracks cannot be updated, so none were changed"),
            "{message}"
        );
        assert!(
            message.contains(
                "02.flac (1 picture block in this file can't be read safely; \
                 saving would delete it)"
            ),
            "{message}"
        );
        let after = [&readable, &refused].map(|path| fs::read(path).expect("read track"));
        assert_eq!(after, before);
        assert_eq!(dir_entries(&album), ["01.flac", "02.flac"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[cfg(unix)]
    #[test]
    fn embedded_artwork_changes_nothing_when_a_track_in_the_folder_cannot_be_read() {
        use std::os::unix::fs::PermissionsExt;

        let root = test_dir("unreadable-member");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let tracks = ["01.flac", "02.flac", "03.flac"].map(|name| {
            let path = album.join(name);
            write_test_flac(&path, &[("ALBUM", "Album")]);
            path
        });
        let before = tracks
            .iter()
            .map(|path| fs::read(path).expect("read track"))
            .collect::<Vec<_>>();
        fs::set_permissions(&tracks[1], fs::Permissions::from_mode(0o000)).expect("chmod");
        // Root ignores file permissions, so the check only holds for normal users.
        if fs::File::open(&tracks[1]).is_ok() {
            fs::remove_dir_all(root).expect("remove test dir");
            return;
        }
        let (artwork, _) = artwork_file(&root, ImageFormat::Png);

        let error = apply_artwork_to_track(&tracks[0], &artwork).expect_err("unreadable track");

        let message = error.to_string();
        assert!(
            message.starts_with(
                "1 of 3 tracks in this folder cannot be read, so the album's tracks are \
                 unknown and none were changed: 02.flac (failed to read tags from"
            ),
            "{message}"
        );
        assert!(!message.contains("01.flac"), "{message}");
        assert!(!message.contains("03.flac"), "{message}");
        fs::set_permissions(&tracks[1], fs::Permissions::from_mode(0o644)).expect("chmod");
        let after = tracks
            .iter()
            .map(|path| fs::read(path).expect("read track"))
            .collect::<Vec<_>>();
        assert_eq!(after, before);
        assert_eq!(dir_entries(&album), ["01.flac", "02.flac", "03.flac"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn artwork_on_an_id3v1_only_mp3_keeps_its_tags_visible() {
        use crate::tag_write::write_test_mp3_with_only_id3v1;
        use lofty::tag::{ItemKey, TagType};

        let root = test_dir("id3v1-only");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.mp3");
        write_test_mp3_with_only_id3v1(
            &track,
            &[
                (ItemKey::TrackTitle, "Title"),
                (ItemKey::TrackArtist, "Artist"),
                (ItemKey::AlbumTitle, "Album"),
                (ItemKey::Year, "1997"),
            ],
        );
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Png);

        let outcome = apply_artwork_to_track(&track, &artwork).expect("apply artwork");

        assert_eq!(outcome.affected_track_paths, std::slice::from_ref(&track));
        let tagged = lofty::read_from_path(&track).expect("read tags");
        let shown = tagged.primary_tag().expect("ID3v2 tag");
        assert_eq!(shown.tag_type(), TagType::Id3v2);
        assert_eq!(shown.title().as_deref(), Some("Title"));
        assert_eq!(shown.artist().as_deref(), Some("Artist"));
        assert_eq!(shown.album().as_deref(), Some("Album"));
        assert_eq!(shown.get_string(ItemKey::RecordingDate), Some("1997"));
        assert_eq!(front_cover(&track).as_ref(), Some(&artwork_bytes));
        let kept = tagged.tag(TagType::Id3v1).expect("ID3v1 tag");
        assert_eq!(kept.title().as_deref(), Some("Title"));
        assert_eq!(kept.get_string(ItemKey::Year), Some("1997"));
        assert!(kept.pictures().is_empty());
        let indexed = crate::library::read_track_info(&track);
        assert_eq!(indexed.title, "Title");
        assert_eq!(indexed.album, "Album");
        assert_eq!(indexed.year, Some(1997));
        assert_eq!(dir_entries(&album), ["01.mp3"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn embedded_artwork_skips_apple_double_files_next_to_the_tracks() {
        use crate::tag_write::write_test_mp3_with_id3v23;

        let root = test_dir("apple-double");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let tracks = ["01.mp3", "02.mp3"].map(|name| {
            let path = album.join(name);
            write_test_mp3_with_id3v23(&path);
            path
        });
        let sidecars = ["._01.mp3", "._02.mp3"].map(|name| {
            let path = album.join(name);
            crate::library::write_test_apple_double(&path);
            (
                path.clone(),
                fs::read(&path).expect("read AppleDouble file"),
            )
        });
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Png);

        let outcome = apply_artwork_to_track(&tracks[0], &artwork).expect("apply artwork");

        assert_eq!(outcome.affected_track_paths, tracks);
        assert_eq!(outcome.failure_summary(), None);
        for track in &tracks {
            assert_eq!(
                front_cover(track).as_ref(),
                Some(&artwork_bytes),
                "{track:?}"
            );
        }
        for (path, bytes) in &sidecars {
            assert_eq!(&fs::read(path).expect("read AppleDouble file"), bytes);
        }
        assert_eq!(
            dir_entries(&album),
            ["._01.mp3", "._02.mp3", "01.mp3", "02.mp3"]
        );
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn tracks_that_fail_while_written_are_reported_and_the_rest_are_written() {
        let tracks = ["01.flac", "02.flac", "03.flac"].map(|name| Path::new("/music").join(name));

        let outcome = write_each_track(tracks.to_vec(), |path| {
            if path.ends_with("02.flac") {
                Err("disk full".to_string())
            } else {
                Ok(())
            }
        });

        assert_eq!(
            outcome.affected_track_paths,
            [tracks[0].clone(), tracks[2].clone()]
        );
        assert_eq!(
            outcome.failed_tracks,
            [(tracks[1].clone(), "disk full".to_string())]
        );
        assert_eq!(
            outcome.failure_summary().as_deref(),
            Some("album art was applied to 2 of 3 album tracks; not updated: 02.flac (disk full)")
        );
    }

    #[test]
    fn applied_front_cover_comes_first_and_becomes_the_library_cover() {
        use lofty::config::WriteOptions;
        use lofty::file::AudioFile;

        let root = test_dir("front-first");
        let album = root.join("Album");
        fs::create_dir(&album).expect("create album dir");
        let track = album.join("01.flac");
        write_test_flac(&track, &[("ALBUM", "Album")]);
        // Covers tagged as "other" are common; readers show the first picture.
        let old_cover = encoded_image([255, 0, 0], ImageFormat::Jpeg);
        let old_front = encoded_image([0, 255, 0], ImageFormat::Jpeg);
        let mut tagged = lofty::read_from_path(&track).expect("read fixture");
        let tag = tagged.primary_tag_mut().expect("Vorbis comments");
        for (bytes, kind) in [
            (&old_cover, PictureType::Other),
            (&old_front, PictureType::CoverFront),
        ] {
            tag.push_picture(
                Picture::unchecked(bytes.clone())
                    .mime_type(MimeType::Jpeg)
                    .pic_type(kind)
                    .build(),
            );
        }
        tagged
            .save_to_path(&track, WriteOptions::default())
            .expect("save fixture pictures");
        let previous_cover = crate::library::read_track_info(&track).cover_path;
        assert!(previous_cover.ends_with(&format!("{:x}.jpg", md5::compute(&old_cover))));
        let (artwork, artwork_bytes) = artwork_file(&root, ImageFormat::Png);

        apply_artwork_to_track(&track, &artwork).expect("apply artwork");

        let tagged = lofty::read_from_path(&track).expect("read tags");
        let pictures = tagged.primary_tag().expect("primary tag").pictures();
        assert_eq!(pictures.len(), 2);
        assert_eq!(pictures[0].pic_type(), PictureType::CoverFront);
        assert_eq!(pictures[0].data(), artwork_bytes);
        assert_eq!(pictures[1].pic_type(), PictureType::Other);
        assert_eq!(pictures[1].data(), old_cover);
        // The library row moves to the new content-addressed entry; the old
        // entry stays for the cleanup after the next scan.
        let cover = crate::library::read_track_info(&track).cover_path;
        assert!(
            cover.ends_with(&format!("{:x}.png", md5::compute(&artwork_bytes))),
            "{cover}"
        );
        assert_eq!(fs::read(&cover).expect("read new entry"), artwork_bytes);
        assert!(Path::new(&previous_cover).is_file());
        assert_eq!(dir_entries(&album), ["01.flac"]);
        fs::remove_dir_all(root).expect("remove test dir");
    }

    #[test]
    fn embedded_artwork_write_keeps_other_tag_data() {
        use crate::tag_write::write_test_mp3_with_id3v23;
        use lofty::config::ParseOptions;
        use lofty::file::AudioFile;
        use lofty::ogg::OggPictureStorage;
        use lofty::tag::{ItemKey, TagType};

        let dir = test_dir("embedded-keeps-tags");
        let flac = dir.join("01.flac");
        write_test_flac(
            &flac,
            &[
                ("ARTIST", "A"),
                ("ARTIST", "B"),
                ("ALBUM", "Album"),
                ("CUSTOM", "kept"),
            ],
        );
        let mp3 = dir.join("02.mp3");
        write_test_mp3_with_id3v23(&mp3);
        let png = ImageBuffer::from_pixel(4, 4, Rgb([0, 0, 255]));
        let mut cursor = Cursor::new(Vec::new());
        DynamicImage::ImageRgb8(png)
            .write_to(&mut cursor, ImageFormat::Png)
            .expect("encode png");
        let artwork = normalize_artwork_bytes(&cursor.into_inner()).expect("normalize");

        rewrite_embedded_artwork_for_file(&flac, &artwork).expect("write FLAC artwork");
        rewrite_embedded_artwork_for_file(&mp3, &artwork).expect("write MP3 artwork");

        let mut file = fs::File::open(&flac).expect("open FLAC");
        let parsed =
            lofty::flac::FlacFile::read_from(&mut file, ParseOptions::new()).expect("read FLAC");
        assert_eq!(parsed.pictures().len(), 1);
        let comments = parsed.vorbis_comments().expect("Vorbis comments");
        assert_eq!(comments.get_all("ARTIST").collect::<Vec<_>>(), ["A", "B"]);
        assert_eq!(comments.get("CUSTOM"), Some("kept"));

        assert_eq!(&fs::read(&mp3).expect("read MP3")[..4], b"ID3\x03");
        let tagged = lofty::read_from_path(&mp3).expect("read MP3 tags");
        let tag = tagged.tag(TagType::Id3v2).expect("ID3v2 tag");
        assert_eq!(tag.pictures().len(), 1);
        assert_eq!(tag.get_string(ItemKey::RecordingDate), Some("1997-05-12"));

        assert_eq!(dir_entries(&dir), ["01.flac", "02.mp3"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
