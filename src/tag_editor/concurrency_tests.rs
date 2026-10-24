// SPDX-License-Identifier: GPL-3.0-or-later

//! Tag editor saves and renames that overlap artwork apply or each other on
//! one file. Each test pauses the first edit between its read and its
//! replace, then checks that the second one waits for it.

use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::thread;

use image::{DynamicImage, ImageBuffer, ImageFormat, Rgb};
use lofty::file::TaggedFileExt;
use lofty::picture::PictureType;
use lofty::prelude::Accessor;

use super::tests::{assert_saved, dir_entries, edited, load_one, save_json, test_dir};
use super::{parse_rename_request, rename_rows};
use crate::artwork::{apply_artwork_to_track, normalize_artwork_file};
use crate::file_edit_lock::test_hooks::{pause_first_edit, Event};
use crate::library::index_tracks_for_test;
use crate::tag_write::write_test_mp3_with_id3v23;

/// MP3s of one album in `album/` under `dir`, with `ID3v2` tags, which a save
/// rewrites as a whole.
fn album_tracks(dir: &Path, names: &[&str]) -> Vec<PathBuf> {
    let album = dir.join("album");
    fs::create_dir_all(&album).expect("create album dir");
    names
        .iter()
        .map(|name| {
            let path = album.join(name);
            write_test_mp3_with_id3v23(&path);
            path
        })
        .collect()
}

/// A PNG outside the album folder, and the cover bytes it becomes.
fn artwork(dir: &Path) -> (PathBuf, Vec<u8>) {
    let mut cursor = Cursor::new(Vec::new());
    DynamicImage::ImageRgb8(ImageBuffer::from_pixel(4, 4, Rgb([0, 0, 255])))
        .write_to(&mut cursor, ImageFormat::Png)
        .expect("encode artwork");
    let path = dir.join("artwork.png");
    fs::write(&path, cursor.into_inner()).expect("write artwork");
    let cover = normalize_artwork_file(&path).expect("normalize").bytes;
    (path, cover)
}

fn title_and_cover(path: &Path) -> (String, Option<Vec<u8>>) {
    let tagged = lofty::read_from_path(path).expect("read tags");
    let tag = tagged.primary_tag().expect("primary tag");
    (
        tag.title()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default(),
        tag.get_picture_type(PictureType::CoverFront)
            .map(|picture| picture.data().to_vec()),
    )
}

/// The save request that changes the titles of `paths`, loaded now.
fn title_edits(paths: &[PathBuf], title: &str) -> Vec<serde_json::Value> {
    paths
        .iter()
        .map(|path| {
            let mut row = load_one(path);
            row.title = title.to_string();
            edited(&row, &["title"])
        })
        .collect()
}

fn apply_artwork_in_background(
    track: &Path,
    artwork: &Path,
) -> thread::JoinHandle<crate::artwork::ApplyArtworkOutcome> {
    let (track, artwork) = (track.to_path_buf(), artwork.to_path_buf());
    thread::spawn(move || apply_artwork_to_track(&track, &artwork).expect("apply artwork"))
}

#[test]
fn artwork_apply_waits_for_a_tag_save_and_keeps_its_change() {
    let dir = test_dir("save-then-artwork");
    let track = album_tracks(&dir, &["song.mp3"]).remove(0);
    let (artwork, cover) = artwork(&dir);
    let edits = title_edits(std::slice::from_ref(&track), "New");
    let observer = pause_first_edit(&track);

    let save = thread::spawn(move || save_json(&edits));
    assert_eq!(observer.next_event(), Event::Checkpoint);
    let apply = apply_artwork_in_background(&track, &artwork);
    assert_eq!(
        observer.next_event(),
        Event::Waiting,
        "artwork apply read the file during the save"
    );
    observer.resume();
    assert_eq!(observer.next_event(), Event::Checkpoint);

    assert_saved(&save.join().expect("save"));
    let outcome = apply.join().expect("apply");
    assert!(
        outcome.failed_tracks.is_empty(),
        "{:?}",
        outcome.failed_tracks
    );
    assert_eq!(title_and_cover(&track), ("New".to_string(), Some(cover)));
    assert_eq!(dir_entries(track.parent().expect("album")), ["song.mp3"]);
    fs::remove_dir_all(dir).expect("remove test dir");
}

#[test]
fn a_tag_save_waits_for_artwork_apply_and_keeps_its_cover() {
    let dir = test_dir("artwork-then-save");
    let track = album_tracks(&dir, &["song.mp3"]).remove(0);
    let (artwork, cover) = artwork(&dir);
    let edits = title_edits(std::slice::from_ref(&track), "New");
    let observer = pause_first_edit(&track);

    let apply = apply_artwork_in_background(&track, &artwork);
    assert_eq!(observer.next_event(), Event::Checkpoint);
    let save = thread::spawn(move || save_json(&edits));
    assert_eq!(
        observer.next_event(),
        Event::Waiting,
        "the tag save read the file during artwork apply"
    );
    observer.resume();
    assert_eq!(observer.next_event(), Event::Checkpoint);

    let outcome = apply.join().expect("apply");
    assert!(
        outcome.failed_tracks.is_empty(),
        "{:?}",
        outcome.failed_tracks
    );
    assert_saved(&save.join().expect("save"));
    assert_eq!(title_and_cover(&track), ("New".to_string(), Some(cover)));
    fs::remove_dir_all(dir).expect("remove test dir");
}

#[test]
fn album_artwork_and_a_batch_save_hold_one_file_at_a_time() {
    let dir = test_dir("album-and-batch");
    let tracks = album_tracks(&dir, &["a.mp3", "b.mp3"]);
    let (artwork, cover) = artwork(&dir);
    let edits = title_edits(&tracks, "New");
    let observer = pause_first_edit(&tracks[1]);

    // Artwork apply writes a.mp3, then pauses in b.mp3. The batch saves
    // a.mp3, which artwork apply no longer holds, and waits for b.mp3.
    let apply = apply_artwork_in_background(&tracks[0], &artwork);
    assert_eq!(observer.next_event(), Event::Checkpoint);
    let save = thread::spawn(move || save_json(&edits));
    assert_eq!(observer.next_event(), Event::Waiting);
    observer.resume();

    let outcome = apply.join().expect("apply");
    assert!(
        outcome.failed_tracks.is_empty(),
        "{:?}",
        outcome.failed_tracks
    );
    assert_saved(&save.join().expect("save"));
    for track in &tracks {
        assert_eq!(
            title_and_cover(track),
            ("New".to_string(), Some(cover.clone()))
        );
    }
    fs::remove_dir_all(dir).expect("remove test dir");
}

#[test]
fn a_rename_waits_for_a_tag_save_of_the_file() {
    let dir = test_dir("save-then-rename")
        .canonicalize()
        .expect("canonical");
    let track = album_tracks(&dir, &["song.mp3"]).remove(0);
    let album = track.parent().expect("album").to_path_buf();
    index_tracks_for_test(&album, std::slice::from_ref(&track));
    let edits = title_edits(std::slice::from_ref(&track), "Saved");
    let mut row = load_one(&track);
    row.title = "Renamed".to_string();
    row.track_no = "1".to_string();
    let rename_request = parse_rename_request(
        &serde_json::to_vec(&serde_json::json!({ "rows": [row] })).expect("encode"),
    )
    .expect("rename request");
    let observer = pause_first_edit(&track);

    let save = thread::spawn(move || save_json(&edits));
    assert_eq!(observer.next_event(), Event::Checkpoint);
    let rename = thread::spawn(move || rename_rows(rename_request));
    assert_eq!(
        observer.next_event(),
        Event::Waiting,
        "the rename moved the file during the save"
    );
    observer.resume();

    assert_saved(&save.join().expect("save"));
    let renamed = rename.join().expect("rename");
    let new_path = album.join("01 - Renamed.mp3");
    assert!(
        renamed.results.iter().all(|result| result.ok),
        "{:?}",
        renamed.results
    );
    assert_eq!(
        renamed.successful_paths,
        [new_path.to_string_lossy().into_owned()]
    );
    assert_eq!(dir_entries(&album), ["01 - Renamed.mp3"]);
    assert_eq!(title_and_cover(&new_path).0, "Saved");
    fs::remove_dir_all(dir).expect("remove test dir");
}
