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
    cover_candidates(dir).into_iter().next()
}

/// The image in `dir` that holds its cover under a recognised name, such as
/// `Cover.JPG`, if there is one. Other images, such as `back.jpg`, are never
/// returned.
pub(crate) fn find_named_cover(dir: &Path) -> Option<PathBuf> {
    let first = cover_candidates(dir).into_iter().next()?;
    let name = first.file_name()?.to_str()?;
    cover_rank(name)?.recognised().then_some(first)
}

/// Every image in `dir`, best cover first: images with a recognised cover
/// name in the order of [`COVER_STEMS`], then the other images by name.
pub(crate) fn cover_candidates(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut ranked = entries
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name().into_string().ok()?;
            let rank = cover_rank(&name)?;
            let path = entry.path();
            // Follows links, so a linked image counts like the image itself.
            path.is_file().then_some((rank, name, path))
        })
        .collect::<Vec<_>>();
    ranked.sort_by(|(left_rank, left_name, _), (right_rank, right_name, _)| {
        left_rank.cmp(right_rank).then_with(|| {
            compare_names_ignoring_case(left_name, right_name)
                .then_with(|| left_name.cmp(right_name))
        })
    });
    ranked.into_iter().map(|(_, _, path)| path).collect()
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
            names(&cover_candidates(&dir)),
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

        assert!(cover_candidates(&dir).is_empty());
        assert_eq!(find_folder_cover(&dir.join("missing")), None);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
