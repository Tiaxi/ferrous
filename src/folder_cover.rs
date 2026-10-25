// SPDX-License-Identifier: GPL-3.0-or-later

//! Choosing the cover image in an album folder.
//!
//! The library scan, the library tree, the now-playing view, and artwork
//! apply all pick a folder's cover here, so they agree on the same file.

use std::cmp::Ordering;
use std::fs;
use std::path::{Path, PathBuf};

/// File stems that name a folder's cover, best first, compared without
/// regard to ASCII case. Artwork apply replaces only files named this way.
const COVER_STEMS: [&str; 3] = ["cover", "folder", "front"];

/// Image extensions that can hold a cover, best first for files that share
/// a stem.
const IMAGE_EXTENSIONS: [&str; 3] = ["jpg", "jpeg", "png"];

/// The folder image shown as the cover of tracks in `dir`.
pub(crate) fn find_folder_cover(dir: &Path) -> Option<PathBuf> {
    FolderImages::read(dir).ok()?.into_cover()
}

/// The image in `dir` that holds its cover under a recognised name, such as
/// `Cover.JPG`, if there is one. Other images, such as `back.jpg`, are never
/// returned.
pub(crate) fn find_named_cover(dir: &Path) -> Option<PathBuf> {
    let first = FolderImages::read(dir).ok()?.into_cover()?;
    let name = first.file_name()?.to_str()?;
    cover_rank(name)?.recognised().then_some(first)
}

/// Whether `path` names an image that a folder's cover can be chosen from,
/// by its name alone; it reads nothing.
pub(crate) fn is_cover_image(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .and_then(cover_rank)
        .is_some()
}

/// A stamp of the images a folder's cover was chosen from; see
/// [`FolderImages::stamp`]. Zero stands for a choice whose images are not
/// known, such as one made before stamps were kept, or in a folder that
/// could not be read.
pub(crate) type CoverStamp = u64;

/// The cover images of a folder, read once: the cover choice, and a stamp
/// that changes whenever an image is added, removed, renamed, or changed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct FolderImages {
    /// Every image, best cover first: images with a recognised cover name in
    /// the order of [`COVER_STEMS`], then the other images by name.
    candidates: Vec<PathBuf>,
    stamp: CoverStamp,
}

impl FolderImages {
    /// Reads the images in `dir`. Fails only if the folder cannot be listed.
    pub(crate) fn read(dir: &Path) -> std::io::Result<Self> {
        let mut images = Vec::new();
        for entry in fs::read_dir(dir)?.flatten() {
            let Ok(name) = entry.file_name().into_string() else {
                continue;
            };
            let Some(rank) = cover_rank(&name) else {
                continue;
            };
            let path = entry.path();
            // Follows links, so a linked image counts like the image itself.
            let Ok(metadata) = fs::metadata(&path) else {
                continue;
            };
            if metadata.is_file() {
                images.push(FolderImage {
                    rank,
                    name,
                    path,
                    size: metadata.len(),
                    modified_ns: modified_ns(&metadata),
                });
            }
        }
        images.sort_by(|left, right| {
            left.rank.cmp(&right.rank).then_with(|| {
                compare_names_ignoring_case(&left.name, &right.name)
                    .then_with(|| left.name.cmp(&right.name))
            })
        });
        Ok(Self {
            stamp: stamp_of(&images),
            candidates: images.into_iter().map(|image| image.path).collect(),
        })
    }

    /// The image shown as the folder's cover.
    pub(crate) fn cover(&self) -> Option<&Path> {
        self.candidates.first().map(PathBuf::as_path)
    }

    pub(crate) fn into_cover(self) -> Option<PathBuf> {
        self.candidates.into_iter().next()
    }

    /// Never zero. Equal stamps mean the same image names, sizes, and
    /// modification times, so the same cover choice from the same files.
    pub(crate) fn stamp(&self) -> CoverStamp {
        self.stamp
    }

    #[cfg(test)]
    fn candidates(&self) -> &[PathBuf] {
        &self.candidates
    }
}

struct FolderImage {
    rank: CoverRank,
    name: String,
    path: PathBuf,
    size: u64,
    modified_ns: u128,
}

fn modified_ns(metadata: &fs::Metadata) -> u128 {
    metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
        .map_or(0, |duration| duration.as_nanos())
}

/// FNV-1a over each image's name, size, and modification time, in a fixed
/// order. Unlike the standard library's hashers, it stays the same across
/// builds, so stored stamps stay comparable.
fn stamp_of(images: &[FolderImage]) -> CoverStamp {
    const OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
    const PRIME: u64 = 0x0000_0100_0000_01b3;
    let mut ordered: Vec<&FolderImage> = images.iter().collect();
    ordered.sort_by(|left, right| left.name.cmp(&right.name));
    let mut hash = OFFSET_BASIS;
    let mut feed = |bytes: &[u8]| {
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(PRIME);
        }
    };
    for image in ordered {
        feed(image.name.as_bytes());
        // Names cannot hold a NUL byte, so this ends the name.
        feed(&[0]);
        feed(&image.size.to_le_bytes());
        feed(&image.modified_ns.to_le_bytes());
    }
    hash.max(1)
}

/// Where a file name sorts among cover candidates; `None` for non-images.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct CoverRank {
    /// Index into [`COVER_STEMS`], or its length for other names.
    stem: usize,
    /// Index into [`IMAGE_EXTENSIONS`] for recognised names; zero otherwise
    /// so other images sort by name alone.
    extension: usize,
}

impl CoverRank {
    fn recognised(self) -> bool {
        self.stem < COVER_STEMS.len()
    }
}

fn cover_rank(name: &str) -> Option<CoverRank> {
    let (stem, extension) = name.rsplit_once('.')?;
    let extension = IMAGE_EXTENSIONS
        .iter()
        .position(|known| extension.eq_ignore_ascii_case(known))?;
    Some(
        match COVER_STEMS
            .iter()
            .position(|known| stem.eq_ignore_ascii_case(known))
        {
            Some(stem) => CoverRank { stem, extension },
            None => CoverRank {
                stem: COVER_STEMS.len(),
                extension: 0,
            },
        },
    )
}

fn compare_names_ignoring_case(left: &str, right: &str) -> Ordering {
    left.bytes()
        .map(|byte| byte.to_ascii_lowercase())
        .cmp(right.bytes().map(|byte| byte.to_ascii_lowercase()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering as AtomicOrdering};

    fn test_dir(name: &str, files: &[&str]) -> PathBuf {
        static NEXT_ID: AtomicU64 = AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "ferrous-folder-cover-{name}-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, AtomicOrdering::Relaxed)
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        for file in files {
            fs::write(dir.join(file), b"image").expect("write fixture");
        }
        dir
    }

    fn names(paths: &[PathBuf]) -> Vec<String> {
        paths
            .iter()
            .map(|path| path.file_name().unwrap().to_string_lossy().into_owned())
            .collect()
    }

    #[test]
    fn recognised_names_match_without_regard_to_case_and_win_over_other_images() {
        let dir = test_dir("case", &["a.jpg", "back.jpg", "Cover.JPG", "track.flac"]);

        assert_eq!(find_folder_cover(&dir), Some(dir.join("Cover.JPG")));
        assert_eq!(find_named_cover(&dir), Some(dir.join("Cover.JPG")));
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn recognised_names_follow_a_fixed_priority() {
        let dir = test_dir(
            "priority",
            &[
                "front.png",
                "FOLDER.png",
                "folder.jpeg",
                "cover.png",
                "Cover.jpeg",
                "zzz.jpg",
                "back.jpg",
            ],
        );

        assert_eq!(
            names(FolderImages::read(&dir).unwrap().candidates()),
            [
                "Cover.jpeg",
                "cover.png",
                "folder.jpeg",
                "FOLDER.png",
                "front.png",
                "back.jpg",
                "zzz.jpg"
            ]
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn other_images_are_shown_by_name_but_never_named_as_the_cover() {
        let dir = test_dir("others", &["booklet01.jpg", "Back.png", "notes.txt"]);

        assert_eq!(find_folder_cover(&dir), Some(dir.join("Back.png")));
        assert_eq!(find_named_cover(&dir), None);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn front_wins_over_an_alphabetically_earlier_back_scan() {
        let dir = test_dir("front", &["back.jpg", "front.jpg"]);

        assert_eq!(find_folder_cover(&dir), Some(dir.join("front.jpg")));
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn only_image_files_are_candidates() {
        let dir = test_dir("files-only", &["cover.gif", "cover.jpg.part", "front"]);
        fs::create_dir(dir.join("cover.jpg")).expect("create directory");

        assert!(FolderImages::read(&dir).unwrap().candidates().is_empty());
        assert_eq!(find_folder_cover(&dir.join("missing")), None);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn the_stamp_changes_with_the_images_and_only_with_them() {
        let dir = test_dir("stamp", &["01.flac", "back.jpg"]);
        let stamp = || FolderImages::read(&dir).unwrap().stamp();
        let first = stamp();
        assert_ne!(first, 0);
        assert_eq!(stamp(), first, "reading again gives the same stamp");

        fs::write(dir.join("notes.txt"), b"text").unwrap();
        fs::write(dir.join("01.flac"), b"changed audio").unwrap();
        assert_eq!(stamp(), first, "other files do not count");

        fs::write(dir.join("cover.jpg"), b"image").unwrap();
        let added = stamp();
        assert_ne!(added, first, "an added image counts");

        fs::rename(dir.join("cover.jpg"), dir.join("folder.jpg")).unwrap();
        let renamed = stamp();
        assert_ne!(renamed, added, "a renamed image counts");

        fs::write(dir.join("folder.jpg"), b"a longer replacement image").unwrap();
        let replaced = stamp();
        assert_ne!(
            replaced, renamed,
            "an image replaced at the same name counts"
        );

        fs::remove_file(dir.join("folder.jpg")).unwrap();
        assert_eq!(stamp(), first, "removing it gives back the first stamp");
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn an_unreadable_folder_is_an_error_not_a_folder_without_images() {
        let dir = test_dir("missing", &[]);
        assert!(FolderImages::read(&dir.join("missing")).is_err());
        assert_ne!(FolderImages::read(&dir).unwrap().stamp(), 0);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
