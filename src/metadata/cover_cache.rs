// SPDX-License-Identifier: GPL-3.0-or-later

//! Content-addressed cache of embedded cover art.
//!
//! Each entry holds the picture's original encoded bytes under the MD5 of
//! those bytes, so tracks that share a cover share one file and repeated reads
//! reuse it. The library database stores entry paths, so entries are never
//! evicted by size or age; the library removes entries that no track row
//! references instead.

use std::collections::HashSet;
use std::fs;
use std::io::{self, Cursor, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::OnceLock;

use lofty::file::{TaggedFile, TaggedFileExt};
use parking_lot::Mutex;

const DIR_NAME: &str = "embedded_covers";
const KEY_HEX_LEN: usize = 32;
/// Earlier releases decoded each track's picture into its own PNG, named
/// after a 64-bit hash of the track path.
const LEGACY_KEY_HEX_LEN: usize = 16;

pub(crate) struct EmbeddedCoverCache {
    dir: PathBuf,
    /// Entry names this process has handed out. Cleanup keeps them because
    /// some are referenced only in memory (the now-playing cover) and others
    /// are handed out before the database row that references them commits.
    in_use: Mutex<HashSet<String>>,
}

impl EmbeddedCoverCache {
    /// The cache under the user's cache directory, if one can be resolved.
    pub(crate) fn shared() -> Option<&'static Self> {
        static SHARED: OnceLock<Option<EmbeddedCoverCache>> = OnceLock::new();
        SHARED
            .get_or_init(|| crate::paths::cache_dir().map(|dir| Self::new(dir.join(DIR_NAME))))
            .as_ref()
    }

    pub(crate) fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            in_use: Mutex::new(HashSet::new()),
        }
    }

    pub(crate) fn dir(&self) -> &Path {
        &self.dir
    }

    /// Caches the first picture of the track's primary tag. Tells a track
    /// without a picture that can be cached (`Ok(None)`) apart from one whose
    /// tags could not be read or whose entry could not be written.
    pub(crate) fn try_store_track_cover(
        &self,
        track_path: &Path,
    ) -> Result<Option<PathBuf>, String> {
        let tagged = crate::panic_guard::read_tags(track_path).map_err(|error| {
            format!(
                "could not read the tags of '{}': {error}",
                track_path.display()
            )
        })?;
        self.try_store_tagged_cover(&tagged)
    }

    /// Caches the first picture of the primary tag of `tagged`, read already.
    /// Returns `Ok(None)` when it has no picture that can be cached and an
    /// error when the entry could not be written.
    pub(crate) fn try_store_tagged_cover(
        &self,
        tagged: &TaggedFile,
    ) -> Result<Option<PathBuf>, String> {
        let Some(picture) = tagged
            .primary_tag()
            .or_else(|| tagged.first_tag())
            .and_then(|tag| tag.pictures().first())
        else {
            return Ok(None);
        };
        self.try_store(picture.data()).map_err(|error| {
            format!(
                "could not write to the embedded cover cache {}: {error}",
                self.dir.display()
            )
        })
    }

    /// Returns the entry for an encoded PNG or JPEG picture, writing it only
    /// when it is missing or incomplete.
    pub(crate) fn store(&self, encoded: &[u8]) -> Option<PathBuf> {
        self.try_store(encoded).ok().flatten()
    }

    /// Like [`Self::store`], but returns `Ok(None)` for a picture that is not
    /// cached and an error when the entry could not be written.
    fn try_store(&self, encoded: &[u8]) -> io::Result<Option<PathBuf>> {
        let Some(extension) = picture_extension(encoded) else {
            return Ok(None);
        };
        let name = format!("{:x}.{extension}", md5::compute(encoded));
        let path = self.dir.join(&name);
        // Mark the entry before checking for it: cleanup has either removed it
        // already, so it is rewritten below, or it sees the mark and keeps it.
        self.in_use.lock().insert(name.clone());
        let complete = fs::metadata(&path).is_ok_and(|metadata| {
            metadata.is_file() && u64::try_from(encoded.len()) == Ok(metadata.len())
        });
        if !complete {
            write_atomically(&self.dir, &name, encoded)?;
        }
        Ok(Some(path))
    }

    /// The entry name of `path` if it names a current or legacy entry of this
    /// cache.
    pub(crate) fn entry_name<'a>(&self, path: &'a Path) -> Option<&'a str> {
        if path.parent() != Some(self.dir.as_path()) {
            return None;
        }
        path.file_name()
            .and_then(|name| name.to_str())
            .filter(|name| is_entry_name(name))
    }

    /// Whether `path` is a per-track PNG written by an earlier release.
    pub(crate) fn is_legacy_entry(&self, path: &Path) -> bool {
        self.entry_name(path).is_some_and(is_legacy_entry_name)
    }

    /// Removes entries that are not in `referenced` and that this process has
    /// not handed out, and returns how many were removed. Only regular files
    /// named by this cache's current or legacy scheme are considered.
    pub(crate) fn remove_unreferenced(&self, referenced: &HashSet<String>) -> io::Result<usize> {
        let entries = match fs::read_dir(&self.dir) {
            Ok(entries) => entries,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(0),
            Err(error) => return Err(error),
        };
        let mut removed = 0;
        let mut first_error = None;
        for entry in entries {
            let entry = match entry {
                Ok(entry) => entry,
                Err(error) => {
                    first_error.get_or_insert(error);
                    continue;
                }
            };
            let file_name = entry.file_name();
            let Some(name) = file_name.to_str() else {
                continue;
            };
            if !is_entry_name(name)
                || referenced.contains(name)
                || !entry.file_type().is_ok_and(|kind| kind.is_file())
            {
                continue;
            }
            // Hold the lock across the check and the removal so `store` cannot
            // hand the entry out in between.
            let in_use = self.in_use.lock();
            if in_use.contains(name) {
                continue;
            }
            match fs::remove_file(entry.path()) {
                Ok(()) => removed += 1,
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(error) => {
                    first_error.get_or_insert(error);
                }
            }
        }
        first_error.map_or(Ok(removed), Err)
    }
}

/// Only PNG and JPEG pictures are cached. Their headers are parsed so that a
/// mislabelled or truncated picture is not stored.
fn picture_extension(encoded: &[u8]) -> Option<&'static str> {
    let reader = image::ImageReader::new(Cursor::new(encoded))
        .with_guessed_format()
        .ok()?;
    let extension = match reader.format()? {
        image::ImageFormat::Jpeg => "jpg",
        image::ImageFormat::Png => "png",
        _ => return None,
    };
    reader.into_dimensions().ok()?;
    Some(extension)
}

fn write_atomically(dir: &Path, name: &str, bytes: &[u8]) -> io::Result<()> {
    static NEXT_TEMP_ID: AtomicU64 = AtomicU64::new(0);
    fs::create_dir_all(dir)?;
    // A dot prefix keeps temporary files outside the entry naming scheme.
    let temp_path = dir.join(format!(
        ".{name}.{}-{}.tmp",
        std::process::id(),
        NEXT_TEMP_ID.fetch_add(1, Ordering::Relaxed)
    ));
    let result = fs::File::create_new(&temp_path)
        .and_then(|mut file| file.write_all(bytes))
        .and_then(|()| fs::rename(&temp_path, dir.join(name)));
    if result.is_err() {
        let _ = fs::remove_file(&temp_path);
    }
    result
}

fn is_entry_name(name: &str) -> bool {
    is_current_entry_name(name) || is_legacy_entry_name(name)
}

fn is_current_entry_name(name: &str) -> bool {
    name.split_once('.').is_some_and(|(key, extension)| {
        is_hex_key(key, KEY_HEX_LEN) && matches!(extension, "jpg" | "png")
    })
}

fn is_legacy_entry_name(name: &str) -> bool {
    name.split_once('.')
        .is_some_and(|(key, extension)| is_hex_key(key, LEGACY_KEY_HEX_LEN) && extension == "png")
}

fn is_hex_key(key: &str, len: usize) -> bool {
    key.len() == len
        && key
            .bytes()
            .all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'))
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use image::{DynamicImage, ImageFormat, Rgb, RgbImage};
    use lofty::config::WriteOptions;
    use lofty::file::AudioFile;
    use lofty::picture::{MimeType, Picture, PictureType};
    use lofty::tag::Tag;
    use std::time::{Duration, SystemTime};

    pub(crate) fn encoded_picture(seed: u8, format: ImageFormat) -> Vec<u8> {
        let image = RgbImage::from_fn(8, 6, |x, y| {
            Rgb([seed, u8::try_from(x).unwrap(), u8::try_from(y).unwrap()])
        });
        let mut cursor = Cursor::new(Vec::new());
        DynamicImage::ImageRgb8(image)
            .write_to(&mut cursor, format)
            .expect("encode picture");
        cursor.into_inner()
    }

    /// A short mono WAV whose `ID3v2` tag embeds `picture` as the front cover.
    pub(crate) fn write_wav_with_picture(path: &Path, picture: &[u8]) {
        let samples = [0_u8; 200];
        let data_len = u32::try_from(samples.len()).unwrap();
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"RIFF");
        bytes.extend_from_slice(&(36 + data_len).to_le_bytes());
        bytes.extend_from_slice(b"WAVEfmt ");
        bytes.extend_from_slice(&16_u32.to_le_bytes());
        bytes.extend_from_slice(&1_u16.to_le_bytes());
        bytes.extend_from_slice(&1_u16.to_le_bytes());
        bytes.extend_from_slice(&8_000_u32.to_le_bytes());
        bytes.extend_from_slice(&16_000_u32.to_le_bytes());
        bytes.extend_from_slice(&2_u16.to_le_bytes());
        bytes.extend_from_slice(&16_u16.to_le_bytes());
        bytes.extend_from_slice(b"data");
        bytes.extend_from_slice(&data_len.to_le_bytes());
        bytes.extend_from_slice(&samples);
        fs::write(path, bytes).expect("write wav fixture");

        let mut tagged = lofty::read_from_path(path).expect("read wav fixture");
        let mut tag = Tag::new(tagged.primary_tag_type());
        tag.push_picture(
            Picture::unchecked(picture.to_vec())
                .mime_type(match image::guess_format(picture) {
                    Ok(ImageFormat::Png) => MimeType::Png,
                    _ => MimeType::Jpeg,
                })
                .pic_type(PictureType::CoverFront)
                .build(),
        );
        tagged.insert_tag(tag);
        tagged
            .save_to_path(path, WriteOptions::default())
            .expect("save wav fixture tags");
    }

    pub(crate) fn test_cache(name: &str) -> EmbeddedCoverCache {
        static NEXT_ID: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "ferrous-cover-cache-{name}-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        ));
        EmbeddedCoverCache::new(dir.join(DIR_NAME))
    }

    fn file_names(dir: &Path) -> Vec<String> {
        let mut names = fs::read_dir(dir)
            .expect("read cache dir")
            .map(|entry| entry.expect("cache entry").file_name())
            .map(|name| name.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        names.sort();
        names
    }

    #[test]
    fn identical_pictures_share_one_entry_with_the_original_bytes() {
        let cache = test_cache("shared-entry");
        let jpeg = encoded_picture(1, ImageFormat::Jpeg);

        let first = cache.store(&jpeg).expect("store first track's cover");
        let second = cache
            .store(&jpeg.clone())
            .expect("store second track's cover");

        assert_eq!(first, second);
        assert_eq!(first.extension().and_then(|ext| ext.to_str()), Some("jpg"));
        assert_eq!(fs::read(&first).expect("read entry"), jpeg);
        assert_eq!(
            file_names(cache.dir()),
            [first.file_name().unwrap().to_string_lossy().into_owned()]
        );
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn entries_are_keyed_by_picture_content_and_format() {
        let cache = test_cache("content-key");
        let jpeg = encoded_picture(2, ImageFormat::Jpeg);
        let png = encoded_picture(2, ImageFormat::Png);
        let other_png = encoded_picture(3, ImageFormat::Png);

        let jpeg_path = cache.store(&jpeg).expect("store jpeg");
        let png_path = cache.store(&png).expect("store png");
        let other_path = cache.store(&other_png).expect("store other png");

        assert_eq!(
            jpeg_path.file_name().unwrap().to_string_lossy(),
            format!("{:x}.jpg", md5::compute(&jpeg))
        );
        assert_eq!(
            png_path.file_name().unwrap().to_string_lossy(),
            format!("{:x}.png", md5::compute(&png))
        );
        assert_ne!(png_path, other_path);
        assert_eq!(fs::read(&png_path).expect("read png entry"), png);
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn repeated_stores_do_not_rewrite_an_existing_entry() {
        let cache = test_cache("no-rewrite");
        let png = encoded_picture(4, ImageFormat::Png);
        let path = cache.store(&png).expect("store png");
        let modified = || {
            fs::metadata(&path)
                .and_then(|metadata| metadata.modified())
                .expect("entry mtime")
        };
        fs::File::options()
            .write(true)
            .open(&path)
            .and_then(|file| file.set_modified(SystemTime::now() - Duration::from_secs(3600)))
            .expect("age entry");
        let aged = modified();

        assert_eq!(cache.store(&png), Some(path.clone()));

        assert_eq!(modified(), aged);
        assert_eq!(file_names(cache.dir()).len(), 1, "no temporary files left");
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn truncated_entries_are_rewritten() {
        let cache = test_cache("truncated");
        let png = encoded_picture(5, ImageFormat::Png);
        let path = cache.store(&png).expect("store png");
        fs::write(&path, &png[..png.len() / 2]).expect("truncate entry");

        assert_eq!(cache.store(&png), Some(path.clone()));

        assert_eq!(fs::read(&path).expect("read entry"), png);
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn unsupported_or_damaged_pictures_are_not_cached() {
        let cache = test_cache("unsupported");
        let png = encoded_picture(6, ImageFormat::Png);
        let gif = b"GIF89a\x01\x00\x01\x00\x00\x00\x00;".to_vec();

        assert_eq!(cache.store(b"not a picture"), None);
        assert_eq!(cache.store(&gif), None);
        assert_eq!(cache.store(&png[..12]), None);
        assert!(!cache.dir().exists());
    }

    #[test]
    fn cleanup_removes_only_unreferenced_entries_that_are_not_in_use() {
        let cache = test_cache("cleanup");
        let dir = cache.dir().to_path_buf();
        fs::create_dir_all(&dir).expect("create cache dir");
        let in_use = cache
            .store(&encoded_picture(7, ImageFormat::Png))
            .expect("store entry handed out by this process");
        let referenced = format!("{}.jpg", "a".repeat(32));
        let referenced_legacy = "fedcba9876543210.png";
        let unreferenced = format!("{}.png", "b".repeat(32));
        let unreferenced_legacy = "0123456789abcdef.png";
        let foreign = [
            "notes.txt",
            "cover.jpg",
            "0123456789ABCDEF.png",
            "0123456789abcdef.jpg",
            "0123456789abcde.png",
            ".0123456789abcdef.png.1-0.tmp",
        ];
        for name in [
            referenced.as_str(),
            referenced_legacy,
            unreferenced.as_str(),
            unreferenced_legacy,
        ]
        .into_iter()
        .chain(foreign)
        {
            fs::write(dir.join(name), b"entry").expect("write cache file");
        }
        fs::create_dir(dir.join("0000000000000000.png")).expect("create directory");

        let removed = cache
            .remove_unreferenced(&HashSet::from([
                referenced.clone(),
                referenced_legacy.to_owned(),
            ]))
            .expect("cleanup");

        assert_eq!(removed, 2);
        assert!(!dir.join(&unreferenced).exists());
        assert!(!dir.join(unreferenced_legacy).exists());
        assert!(in_use.is_file());
        for name in [referenced.as_str(), referenced_legacy]
            .into_iter()
            .chain(foreign)
        {
            assert!(dir.join(name).is_file(), "{name} must be kept");
        }
        assert!(dir.join("0000000000000000.png").is_dir());
        let _ = fs::remove_dir_all(dir.parent().unwrap());
    }

    #[test]
    fn cleanup_without_a_cache_directory_removes_nothing() {
        let cache = test_cache("missing");
        assert_eq!(cache.remove_unreferenced(&HashSet::new()).ok(), Some(0));
    }

    #[test]
    fn an_entry_removed_by_cleanup_is_rewritten_when_stored_again() {
        let cache = test_cache("rewrite-after-cleanup");
        let png = encoded_picture(9, ImageFormat::Png);
        let path = cache.dir().join(format!("{:x}.png", md5::compute(&png)));
        fs::create_dir_all(cache.dir()).expect("create cache dir");
        fs::write(&path, &png).expect("write entry from an earlier session");
        assert_eq!(cache.remove_unreferenced(&HashSet::new()).ok(), Some(1));

        assert_eq!(cache.store(&png), Some(path.clone()));

        assert_eq!(fs::read(&path).expect("read entry"), png);
        assert_eq!(cache.remove_unreferenced(&HashSet::new()).ok(), Some(0));
        assert!(path.is_file());
        let _ = fs::remove_dir_all(cache.dir().parent().unwrap());
    }

    #[test]
    fn entry_names_are_recognized_only_inside_the_cache_directory() {
        let cache = test_cache("entry-names");
        let current = format!("{}.png", "b".repeat(32));
        let legacy = "00000000000000ff.png";

        assert_eq!(
            cache.entry_name(&cache.dir().join(&current)),
            Some(current.as_str())
        );
        assert!(!cache.is_legacy_entry(&cache.dir().join(&current)));
        assert!(cache.is_legacy_entry(&cache.dir().join(legacy)));
        assert_eq!(cache.entry_name(&cache.dir().join("cover.png")), None);
        assert_eq!(cache.entry_name(&Path::new("/music").join(legacy)), None);
        assert!(!cache.is_legacy_entry(&Path::new("/music").join(legacy)));
    }
}
