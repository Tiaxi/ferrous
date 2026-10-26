// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use lofty::file::TaggedFileExt;
use lofty::prelude::Accessor;
use lofty::tag::{ItemKey, Tag};
use serde::{Deserialize, Serialize};

use crate::file_edit_lock;
use crate::frontend_bridge::library_tree::{album_row_key, section_row_key};
use crate::library::{
    read_library_snapshot_from_db, rename_indexed_metadata_paths, LibraryRoot, LibrarySnapshot,
    TrackRefresh,
};
use crate::panic_guard;
use crate::raw_audio::{
    edit_appended_apev2_text_items, format_ape_number_pair, is_raw_surround_file,
    read_appended_apev2_text_metadata, ApeTextField,
};
use crate::tag_write::{save_tag, seeded_tag, writes_id3v23};
use validation::TagTarget;

#[cfg(test)]
mod concurrency_tests;
#[cfg(test)]
mod rename_tests;
#[cfg(test)]
mod save_validation_tests;
mod validation;

fn lofty_file_type_label(file_type: lofty::file::FileType) -> &'static str {
    match file_type {
        lofty::file::FileType::Aac => "AAC",
        lofty::file::FileType::Aiff => "AIFF",
        lofty::file::FileType::Ape => "APE",
        lofty::file::FileType::Flac => "FLAC",
        lofty::file::FileType::Mpeg => "MP3",
        lofty::file::FileType::Mpc => "MPC",
        lofty::file::FileType::Mp4 => "MP4",
        lofty::file::FileType::Opus => "Opus",
        lofty::file::FileType::Vorbis => "Vorbis",
        lofty::file::FileType::Speex => "Speex",
        lofty::file::FileType::Wav => "WAV",
        lofty::file::FileType::WavPack => "WavPack",
        _ => "Audio",
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct TagEditorRow {
    pub path: String,
    pub file_name: String,
    pub directory: String,
    pub format_kind: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub genre: String,
    pub year: String,
    pub track_no: String,
    pub disc_no: String,
    pub total_tracks: String,
    pub total_discs: String,
    pub comment: String,
    /// The values after the first of a text field that holds several, such as
    /// a second artist, by field name. Saving a changed field replaces them all
    /// with its one value, so the editor shows them. Not read from requests.
    #[serde(
        default,
        skip_serializing_if = "BTreeMap::is_empty",
        skip_deserializing
    )]
    pub extra_values: BTreeMap<String, Vec<String>>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct SelectionEntry {
    path: Option<String>,
    row_type: Option<String>,
    key: Option<String>,
    artist: Option<String>,
    name: Option<String>,
    track_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct LoadRequest {
    selections: Vec<SelectionEntry>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
enum PathsEnvelope {
    Paths(Vec<String>),
    Request(LoadRequest),
    Entries(Vec<SelectionEntry>),
}

/// An editable column. The JSON names match the [`TagEditorRow`] fields.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
enum TagField {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Genre,
    Year,
    TrackNo,
    DiscNo,
    TotalTracks,
    TotalDiscs,
    Comment,
}

impl TagField {
    const ALL: [Self; 11] = [
        Self::Title,
        Self::Artist,
        Self::Album,
        Self::AlbumArtist,
        Self::Genre,
        Self::Year,
        Self::TrackNo,
        Self::DiscNo,
        Self::TotalTracks,
        Self::TotalDiscs,
        Self::Comment,
    ];
}

/// A row to save and the fields the user changed in it.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SaveRow {
    #[serde(flatten)]
    row: TagEditorRow,
    /// Only these fields are written. Requests without the list write every
    /// field, as earlier versions of the request did.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    changed_fields: Option<Vec<TagField>>,
}

impl SaveRow {
    fn changed_fields(&self) -> HashSet<TagField> {
        self.changed_fields.as_deref().map_or_else(
            || TagField::ALL.into_iter().collect(),
            |fields| fields.iter().copied().collect(),
        )
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SaveRequest {
    rows: Vec<SaveRow>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
enum SaveRequestEnvelope {
    Request(SaveRequest),
    Rows(Vec<SaveRow>),
}

impl SaveRequestEnvelope {
    fn into_rows(self) -> Vec<SaveRow> {
        match self {
            Self::Request(request) => request.rows,
            Self::Rows(rows) => rows,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct RenameRequest {
    rows: Vec<TagEditorRow>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
enum RenameRequestEnvelope {
    Request(RenameRequest),
    Rows(Vec<TagEditorRow>),
}

impl RenameRequestEnvelope {
    fn into_rows(self) -> Vec<TagEditorRow> {
        match self {
            Self::Request(request) => request.rows,
            Self::Rows(rows) => rows,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LoadFailure {
    path: String,
    error: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LoadResponse {
    rows: Vec<TagEditorRow>,
    resolved_paths: Vec<String>,
    /// Files that could not be read; the readable ones are still in `rows`.
    failures: Vec<LoadFailure>,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SaveResultRow {
    path: String,
    ok: bool,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SaveResponse {
    results: Vec<SaveResultRow>,
    successful_paths: Vec<String>,
    error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct RenameResultRow {
    pub(crate) path: String,
    pub(crate) new_path: Option<String>,
    pub(crate) ok: bool,
    pub(crate) error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct RenameResponse {
    pub(crate) results: Vec<RenameResultRow>,
    pub(crate) successful_paths: Vec<String>,
    pub(crate) error: Option<String>,
}

#[derive(Debug, Clone)]
struct LibraryPathContext {
    album_key: Option<String>,
    section_key: Option<String>,
    is_main_level_album_track: bool,
}

fn file_name_for_path(path: &Path) -> String {
    path.file_name()
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn directory_for_path(path: &Path) -> String {
    path.parent()
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn number_text_from_tag(tag: &Tag, key: ItemKey, fallback: Option<u32>) -> String {
    tag.get_string(key)
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .or_else(|| fallback.map(|value| value.to_string()))
        .unwrap_or_default()
}

fn padded_number_text(value: &str, total: &str, min_width: usize) -> String {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let width = total
        .trim()
        .parse::<u32>()
        .ok()
        .map_or(trimmed.len(), |parsed| parsed.to_string().len())
        .max(min_width);
    trimmed.parse::<u32>().map_or_else(
        |_| trimmed.to_string(),
        |parsed| format!("{parsed:0width$}"),
    )
}

fn year_text_from_tag(tag: &Tag) -> String {
    tag.get_string(ItemKey::RecordingDate)
        .or_else(|| tag.get_string(ItemKey::OriginalReleaseDate))
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string)
        .or_else(|| tag.date().map(|value| value.year.to_string()))
        .unwrap_or_default()
}

fn text_value(tag: &Tag, key: ItemKey) -> String {
    tag.get_string(key).map(str::to_string).unwrap_or_default()
}

fn load_standard_row(path: &Path) -> Result<TagEditorRow, String> {
    let tagged = crate::panic_guard::read_tags(path).map_err(|err| {
        format!(
            "failed to read tag data for {}: {err}",
            path.to_string_lossy()
        )
    })?;
    let format_kind = lofty_file_type_label(tagged.file_type()).to_string();
    let tag = tagged.primary_tag().or_else(|| tagged.first_tag());
    let mut row = TagEditorRow {
        path: path.to_string_lossy().into_owned(),
        file_name: file_name_for_path(path),
        directory: directory_for_path(path),
        format_kind,
        ..TagEditorRow::default()
    };

    if let Some(tag) = tag {
        row.title = tag
            .title()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default();
        row.artist = tag
            .artist()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default();
        row.album = tag
            .album()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default();
        row.album_artist = text_value(tag, ItemKey::AlbumArtist);
        row.genre = tag
            .genre()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default();
        row.comment = tag
            .comment()
            .map(std::borrow::Cow::into_owned)
            .unwrap_or_default();
        row.year = year_text_from_tag(tag);
        row.track_no = number_text_from_tag(tag, ItemKey::TrackNumber, tag.track());
        row.total_tracks = number_text_from_tag(tag, ItemKey::TrackTotal, tag.track_total());
        row.disc_no = number_text_from_tag(tag, ItemKey::DiscNumber, tag.disk());
        row.total_discs = number_text_from_tag(tag, ItemKey::DiscTotal, tag.disk_total());
        row.track_no = padded_number_text(&row.track_no, &row.total_tracks, 2);
        row.disc_no = padded_number_text(&row.disc_no, &row.total_discs, 1);
        for (field, key) in MULTI_VALUE_TEXT_FIELDS {
            let extra: Vec<String> = tag.get_strings(key).skip(1).map(str::to_string).collect();
            if !extra.is_empty() {
                row.extra_values.insert(field.to_string(), extra);
            }
        }
    }

    Ok(row)
}

/// The text fields that can hold several values, by their request names.
const MULTI_VALUE_TEXT_FIELDS: [(&str, ItemKey); 6] = [
    ("title", ItemKey::TrackTitle),
    ("artist", ItemKey::TrackArtist),
    ("album", ItemKey::AlbumTitle),
    ("albumArtist", ItemKey::AlbumArtist),
    ("genre", ItemKey::Genre),
    ("comment", ItemKey::Comment),
];

/// Splits an `APEv2` text value, whose values are separated by NUL, into the
/// field's first value and the rest, which go to the row's extra values.
fn split_ape_text(row: &mut TagEditorRow, field: &str, value: Option<String>) -> String {
    let value = value.unwrap_or_default();
    let mut values = value
        .split('\0')
        .map(str::trim)
        .filter(|part| !part.is_empty());
    let first = values.next().unwrap_or_default().to_string();
    let extra: Vec<String> = values.map(str::to_string).collect();
    if !extra.is_empty() {
        row.extra_values.insert(field.to_string(), extra);
    }
    first
}

fn load_raw_row(path: &Path) -> Result<TagEditorRow, String> {
    // The tag reader treats I/O errors like a missing tag, so check access first.
    fs::File::open(path).map_err(|err| {
        format!(
            "failed to read tag data for {}: {err}",
            path.to_string_lossy()
        )
    })?;
    let tagged = read_appended_apev2_text_metadata(path).unwrap_or_default();
    let mut row = TagEditorRow {
        path: path.to_string_lossy().into_owned(),
        file_name: file_name_for_path(path),
        directory: directory_for_path(path),
        format_kind: "APEv2".to_string(),
        year: tagged.year_text.unwrap_or_default(),
        track_no: tagged
            .track_no
            .map(|value| value.to_string())
            .unwrap_or_default(),
        disc_no: tagged
            .disc_no
            .map(|value| value.to_string())
            .unwrap_or_default(),
        total_tracks: tagged
            .track_total
            .map(|value| value.to_string())
            .unwrap_or_default(),
        total_discs: tagged
            .disc_total
            .map(|value| value.to_string())
            .unwrap_or_default(),
        ..TagEditorRow::default()
    };
    row.title = split_ape_text(&mut row, "title", tagged.title);
    row.artist = split_ape_text(&mut row, "artist", tagged.artist);
    row.album = split_ape_text(&mut row, "album", tagged.album);
    row.album_artist = split_ape_text(&mut row, "albumArtist", tagged.album_artist);
    row.genre = split_ape_text(&mut row, "genre", tagged.genre);
    row.comment = split_ape_text(&mut row, "comment", tagged.comment);
    row.track_no = padded_number_text(&row.track_no, &row.total_tracks, 2);
    row.disc_no = padded_number_text(&row.disc_no, &row.total_discs, 1);
    Ok(row)
}

fn pick_root_for_path<'a>(roots: &'a [LibraryRoot], path: &Path) -> Option<&'a LibraryRoot> {
    roots
        .iter()
        .filter(|root| path.starts_with(&root.path))
        .max_by_key(|root| root.path.components().count())
}

fn derive_library_path_context(
    path: &Path,
    roots: &[LibraryRoot],
    fallback_artist: &str,
) -> Option<LibraryPathContext> {
    let root = pick_root_for_path(roots, path)?;
    let rel = path.strip_prefix(&root.path).ok()?;
    let components = rel
        .components()
        .filter_map(|component| {
            let std::path::Component::Normal(name) = component else {
                return None;
            };
            Some(name.to_string_lossy().into_owned())
        })
        .collect::<Vec<_>>();

    if components.is_empty() {
        return None;
    }

    let artist_name = if components.len() >= 2 {
        components[0].clone()
    } else if fallback_artist.trim().is_empty() {
        "Unknown Artist".to_string()
    } else {
        fallback_artist.trim().to_string()
    };
    if components.len() <= 2 {
        return Some(LibraryPathContext {
            album_key: None,
            section_key: None,
            is_main_level_album_track: false,
        });
    }

    let root_key = root.path.to_string_lossy();
    let album_folder = components[1].clone();
    let section_key = if components.len() >= 4 {
        Some(section_row_key(
            &root_key,
            &artist_name,
            &album_folder,
            &components[2],
        ))
    } else {
        None
    };

    Some(LibraryPathContext {
        album_key: Some(album_row_key(&root_key, &artist_name, &album_folder)),
        section_key,
        is_main_level_album_track: components.len() == 3,
    })
}

fn resolve_library_selection_paths(
    library: &LibrarySnapshot,
    selection: &SelectionEntry,
) -> Vec<PathBuf> {
    let row_type = selection.row_type.as_deref().unwrap_or_default();
    match row_type {
        "track" => selection
            .track_path
            .as_deref()
            .or(selection.path.as_deref())
            .map(PathBuf::from)
            .into_iter()
            .collect(),
        "section" => {
            let key = selection.key.as_deref().unwrap_or_default();
            if key.is_empty() {
                return Vec::new();
            }
            library
                .tracks
                .iter()
                .filter(|track| {
                    derive_library_path_context(&track.path, &library.roots, &track.artist)
                        .as_ref()
                        .and_then(|ctx| ctx.section_key.as_deref())
                        .is_some_and(|section_key| section_key == key)
                })
                .map(|track| track.path.clone())
                .collect()
        }
        "album" => {
            let key = selection.key.as_deref().unwrap_or_default();
            if key.is_empty() {
                return Vec::new();
            }
            library
                .tracks
                .iter()
                .filter(|track| {
                    derive_library_path_context(&track.path, &library.roots, &track.artist)
                        .is_some_and(|ctx| {
                            ctx.album_key.as_deref() == Some(key) && ctx.is_main_level_album_track
                        })
                })
                .map(|track| track.path.clone())
                .collect()
        }
        _ => selection
            .path
            .as_deref()
            .map(PathBuf::from)
            .into_iter()
            .collect(),
    }
}

fn resolve_selection_paths(selections: &[SelectionEntry]) -> Result<Vec<PathBuf>, String> {
    let needs_library = selections.iter().any(|selection| {
        matches!(
            selection.row_type.as_deref().unwrap_or_default(),
            "album" | "section"
        )
    });
    let library = if needs_library {
        Some(read_library_snapshot_from_db()?)
    } else {
        None
    };

    let mut out = Vec::new();
    let mut seen = HashSet::<PathBuf>::new();
    for selection in selections {
        let resolved = if matches!(
            selection.row_type.as_deref().unwrap_or_default(),
            "album" | "section" | "track"
        ) {
            if let Some(library) = library.as_ref() {
                resolve_library_selection_paths(library, selection)
            } else {
                resolve_library_selection_paths(&LibrarySnapshot::default(), selection)
            }
        } else if let Some(path) = selection.path.as_deref() {
            vec![PathBuf::from(path)]
        } else if let Some(path) = selection.track_path.as_deref() {
            vec![PathBuf::from(path)]
        } else {
            Vec::new()
        };

        for path in resolved {
            if seen.insert(path.clone()) {
                out.push(path);
            }
        }
    }
    Ok(out)
}

fn sanitized_file_stem(title: &str) -> String {
    let mut out = String::new();
    let mut previous_was_space = false;
    for ch in title.trim().chars() {
        let mapped = match ch {
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|' => ' ',
            _ => ch,
        };
        if mapped.is_whitespace() {
            if !out.is_empty() && !previous_was_space {
                out.push(' ');
            }
            previous_was_space = true;
            continue;
        }
        out.push(mapped);
        previous_was_space = false;
    }
    out.trim().to_string()
}

fn rename_target_path_for_row(row: &TagEditorRow) -> Result<PathBuf, String> {
    let current_path = PathBuf::from(&row.path);
    let parent = current_path.parent().ok_or_else(|| {
        format!(
            "missing parent directory for {}",
            current_path.to_string_lossy()
        )
    })?;
    let suffix = current_path
        .extension()
        .and_then(|value| value.to_str())
        .map(str::to_ascii_lowercase)
        .ok_or_else(|| {
            format!(
                "missing file extension for {}",
                current_path.to_string_lossy()
            )
        })?;
    let track = padded_number_text(&row.track_no, &row.total_tracks, 2);
    if track.is_empty() {
        return Err(format!(
            "missing track number for {}",
            current_path.to_string_lossy()
        ));
    }
    let title = sanitized_file_stem(&row.title);
    if title.is_empty() {
        return Err(format!(
            "missing title for {}",
            current_path.to_string_lossy()
        ));
    }
    Ok(parent.join(format!("{track} - {title}.{suffix}")))
}

fn paths_refer_to_same_file(left: &Path, right: &Path) -> bool {
    if left == right {
        return true;
    }
    if let (Ok(left_metadata), Ok(right_metadata)) = (fs::metadata(left), fs::metadata(right)) {
        #[cfg(unix)]
        if left_metadata.dev() == right_metadata.dev()
            && left_metadata.ino() == right_metadata.ino()
        {
            return true;
        }
    }
    match (fs::canonicalize(left), fs::canonicalize(right)) {
        (Ok(left_canonical), Ok(right_canonical)) => left_canonical == right_canonical,
        _ => false,
    }
}

fn path_matches_any_source(sources: &HashSet<PathBuf>, target: &Path) -> bool {
    sources
        .iter()
        .any(|source| paths_refer_to_same_file(source, target))
}

/// Whether `path` names a directory entry, also a dangling symbolic link,
/// which `Path::exists` reports as missing.
fn path_entry_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn set_text_key(tag: &mut Tag, key: ItemKey, value: &str) {
    tag.remove_key(key);
    let trimmed = value.trim();
    if !trimmed.is_empty() {
        tag.insert_text(key, trimmed.to_string());
    }
}

/// The value of a track or disc number field, or total.
fn number_value(row: &TagEditorRow, field: TagField) -> &str {
    match field {
        TagField::TrackNo => &row.track_no,
        TagField::DiscNo => &row.disc_no,
        TagField::TotalTracks => &row.total_tracks,
        TagField::TotalDiscs => &row.total_discs,
        _ => "",
    }
}

const NUMBER_FIELDS: [TagField; 4] = [
    TagField::TrackNo,
    TagField::DiscNo,
    TagField::TotalTracks,
    TagField::TotalDiscs,
];

/// Check the changed number fields of `row` for `target`, before anything is
/// written. Returns the checked year when it changed.
fn validated_changes(
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
    target: TagTarget,
) -> Result<Option<String>, String> {
    for field in NUMBER_FIELDS {
        if changed.contains(&field) {
            target.validated_number(field, number_value(row, field))?;
        }
    }
    changed
        .contains(&TagField::Year)
        .then(|| target.validated_year(&row.year))
        .transpose()
}

/// Write the changed fields of `row` into `tag`, leaving every other item
/// alone. Values `target` cannot store are refused and `tag` is unchanged.
fn apply_changed_fields(
    tag: &mut Tag,
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
    target: TagTarget,
) -> Result<(), String> {
    let year = validated_changes(row, changed, target)?;
    for field in TagField::ALL {
        if !changed.contains(&field) {
            continue;
        }
        let (key, value) = match field {
            TagField::Title => (ItemKey::TrackTitle, row.title.as_str()),
            TagField::Artist => (ItemKey::TrackArtist, row.artist.as_str()),
            TagField::Album => (ItemKey::AlbumTitle, row.album.as_str()),
            TagField::AlbumArtist => (ItemKey::AlbumArtist, row.album_artist.as_str()),
            TagField::Genre => (ItemKey::Genre, row.genre.as_str()),
            TagField::Comment => (ItemKey::Comment, row.comment.as_str()),
            TagField::TrackNo => (ItemKey::TrackNumber, row.track_no.as_str()),
            TagField::TotalTracks => (ItemKey::TrackTotal, row.total_tracks.as_str()),
            TagField::DiscNo => (ItemKey::DiscNumber, row.disc_no.as_str()),
            TagField::TotalDiscs => (ItemKey::DiscTotal, row.total_discs.as_str()),
            TagField::Year => {
                // A separate year item would shadow the new date in some readers.
                // The original release date is a different field and stays.
                tag.remove_key(ItemKey::Year);
                (ItemKey::RecordingDate, year.as_deref().unwrap_or_default())
            }
        };
        set_text_key(tag, key, value);
    }
    Ok(())
}

fn apply_standard_row_to_tag(
    path: &Path,
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
) -> Result<(), String> {
    if changed.is_empty() {
        return Ok(());
    }
    let edit = file_edit_lock::lock(path);
    let mut tagged = crate::panic_guard::read_tags(path).map_err(|err| {
        format!(
            "failed to read tag data for {}: {err}",
            path.to_string_lossy()
        )
    })?;
    let file_type = tagged.file_type();
    let primary_type = tagged.primary_tag_type();
    let writable_type = if tagged.tag_support(primary_type).is_writable() {
        primary_type
    } else if let Some(existing) = tagged
        .tags()
        .iter()
        .find(|tag| tagged.tag_support(tag.tag_type()).is_writable())
        .map(lofty::tag::Tag::tag_type)
    {
        existing
    } else {
        return Err(format!(
            "no writable tag type available for {}",
            path.to_string_lossy()
        ));
    };

    if tagged.tag(writable_type).is_none() {
        // Start from the tag the editor displayed, so a new tag written with
        // only the changed fields does not hide the others.
        let seeded = seeded_tag(
            tagged.primary_tag().or_else(|| tagged.first_tag()),
            writable_type,
        );
        tagged.insert_tag(seeded);
    }
    let Some(tag) = tagged.tag_mut(writable_type) else {
        return Err(format!(
            "failed to create writable tag for {}",
            path.to_string_lossy()
        ));
    };
    let target = TagTarget::for_tag(
        writable_type,
        writes_id3v23(path, file_type, writable_type)?,
    );
    apply_changed_fields(tag, row, changed, target)?;
    #[cfg(test)]
    edit.checkpoint();
    let saved = save_tag(path, file_type, tag);
    drop(edit);
    saved
}

fn apply_raw_row(
    path: &Path,
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
) -> Result<(), String> {
    let target = TagTarget::RAW_APEV2;
    let year = validated_changes(row, changed, target)?;
    // A number and its total share one item, so the unchanged one is written
    // back as the file holds it. `validated_changes` has checked the changed
    // ones, and this tag keeps no labels, so each is empty or a number.
    let number = |field| number_value(row, field).trim().parse::<u32>().ok();
    let mut edits = Vec::new();
    for (field, ape_field, value) in [
        (TagField::Title, ApeTextField::Title, &row.title),
        (TagField::Artist, ApeTextField::Artist, &row.artist),
        (TagField::Album, ApeTextField::Album, &row.album),
        (
            TagField::AlbumArtist,
            ApeTextField::AlbumArtist,
            &row.album_artist,
        ),
        (TagField::Genre, ApeTextField::Genre, &row.genre),
        (TagField::Comment, ApeTextField::Comment, &row.comment),
    ] {
        if changed.contains(&field) {
            edits.push((ape_field, Some(value.trim().to_string())));
        }
    }
    if let Some(year) = year {
        edits.push((ApeTextField::Year, Some(year)));
    }
    // APEv2 stores a number and its total in one item.
    if changed.contains(&TagField::TrackNo) || changed.contains(&TagField::TotalTracks) {
        edits.push((
            ApeTextField::Track,
            format_ape_number_pair(number(TagField::TrackNo), number(TagField::TotalTracks)),
        ));
    }
    if changed.contains(&TagField::DiscNo) || changed.contains(&TagField::TotalDiscs) {
        edits.push((
            ApeTextField::Disc,
            format_ape_number_pair(number(TagField::DiscNo), number(TagField::TotalDiscs)),
        ));
    }
    if edits.is_empty() {
        return Ok(());
    }
    let _edit = file_edit_lock::lock(path);
    edit_appended_apev2_text_items(path, &edits)
}

pub(crate) fn parse_paths_blob(blob: &[u8]) -> Result<Vec<PathBuf>, String> {
    match serde_json::from_slice::<PathsEnvelope>(blob) {
        Ok(PathsEnvelope::Paths(paths)) => Ok(paths.into_iter().map(PathBuf::from).collect()),
        Ok(PathsEnvelope::Request(request)) => resolve_selection_paths(&request.selections),
        Ok(PathsEnvelope::Entries(entries)) => resolve_selection_paths(&entries),
        Err(error) => Err(format!("invalid tag editor paths blob: {error}")),
    }
}

pub(crate) fn load_rows_for_paths(paths: &[PathBuf]) -> LoadResponse {
    let mut rows = Vec::new();
    let mut failures = Vec::new();
    for path in paths {
        // A panic while reading one file fails that file, not the whole load.
        let loaded = panic_guard::catch(&format!("loading '{}'", path.display()), || {
            if is_raw_surround_file(path) {
                load_raw_row(path)
            } else {
                load_standard_row(path)
            }
        })
        .and_then(|loaded| loaded);
        match loaded {
            Ok(row) => rows.push(row),
            Err(error) => failures.push(LoadFailure {
                path: path.to_string_lossy().into_owned(),
                error,
            }),
        }
    }
    LoadResponse {
        rows,
        resolved_paths: paths
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect(),
        failures,
        error: None,
    }
}

pub(crate) fn parse_save_request(blob: &[u8]) -> Result<SaveRequest, String> {
    serde_json::from_slice::<SaveRequestEnvelope>(blob)
        .map(SaveRequestEnvelope::into_rows)
        .map(|rows| SaveRequest { rows })
        .map_err(|error| format!("invalid tag editor save request: {error}"))
}

pub(crate) fn parse_rename_request(blob: &[u8]) -> Result<RenameRequest, String> {
    serde_json::from_slice::<RenameRequestEnvelope>(blob)
        .map(RenameRequestEnvelope::into_rows)
        .map(|rows| RenameRequest { rows })
        .map_err(|error| format!("invalid tag editor rename request: {error}"))
}

pub(crate) fn save_rows(request: SaveRequest) -> SaveResponse {
    let mut results = Vec::new();
    let mut successful_paths = Vec::new();
    for save_row in request.rows {
        let changed = save_row.changed_fields();
        let row = save_row.row;
        let path = PathBuf::from(&row.path);
        // A panic while saving one file fails that file, and the others are
        // still saved and reported.
        let result = panic_guard::catch(&format!("saving '{}'", path.display()), || {
            if is_raw_surround_file(&path) {
                apply_raw_row(&path, &row, &changed)
            } else {
                apply_standard_row_to_tag(&path, &row, &changed)
            }
        })
        .and_then(|result| result);
        match result {
            Ok(()) => {
                successful_paths.push(row.path.clone());
                results.push(SaveResultRow {
                    path: row.path,
                    ok: true,
                    error: None,
                });
            }
            Err(error) => {
                results.push(SaveResultRow {
                    path: row.path,
                    ok: false,
                    error: Some(error),
                });
            }
        }
    }
    SaveResponse {
        results,
        successful_paths,
        error: None,
    }
}

pub(crate) fn rename_rows(request: RenameRequest) -> RenameResponse {
    rename_rows_with_refresh(request).0
}

/// [`rename_rows`], also returning what the library index holds for the
/// renamed files once they are renamed.
// Sequential file-rename pipeline — splitting would fragment the transactional logic.
#[allow(clippy::too_many_lines)]
pub(crate) fn rename_rows_with_refresh(request: RenameRequest) -> (RenameResponse, TrackRefresh) {
    let mut results = Vec::new();
    let mut successful_paths = Vec::new();
    let mut renames = Vec::new();
    let mut renamed_pairs = Vec::new();
    let source_paths = request
        .rows
        .iter()
        .map(|row| PathBuf::from(&row.path))
        .collect::<HashSet<_>>();
    let mut claimed_targets = HashMap::<PathBuf, String>::new();

    for row in &request.rows {
        let current_path = PathBuf::from(&row.path);
        let target_path = match rename_target_path_for_row(row) {
            Ok(path) => path,
            Err(error) => {
                results.push(RenameResultRow {
                    path: row.path.clone(),
                    new_path: None,
                    ok: false,
                    error: Some(error),
                });
                continue;
            }
        };
        if target_path == current_path {
            results.push(RenameResultRow {
                path: row.path.clone(),
                new_path: Some(row.path.clone()),
                ok: true,
                error: None,
            });
            continue;
        }
        if let Some(previous_owner) = claimed_targets.get(&target_path) {
            results.push(RenameResultRow {
                path: row.path.clone(),
                new_path: Some(target_path.to_string_lossy().into_owned()),
                ok: false,
                error: Some(format!(
                    "target conflicts with another selected file: {previous_owner}"
                )),
            });
            continue;
        }
        // A target that is the file itself, in another case on a
        // case-insensitive filesystem, matches its source.
        if path_entry_exists(&target_path) && !path_matches_any_source(&source_paths, &target_path)
        {
            if !current_path.exists() {
                claimed_targets.insert(target_path.clone(), row.path.clone());
                renames.push((current_path, target_path.clone()));
                renamed_pairs.push((row.path.clone(), target_path.to_string_lossy().into_owned()));
                continue;
            }
            results.push(RenameResultRow {
                path: row.path.clone(),
                new_path: Some(target_path.to_string_lossy().into_owned()),
                ok: false,
                error: Some(format!(
                    "target already exists: {}",
                    target_path.to_string_lossy()
                )),
            });
            continue;
        }
        claimed_targets.insert(target_path.clone(), row.path.clone());
        renames.push((current_path, target_path.clone()));
        renamed_pairs.push((row.path.clone(), target_path.to_string_lossy().into_owned()));
    }

    let refresh = match rename_indexed_metadata_paths(&renames) {
        Ok(refresh) => refresh,
        Err(error) => {
            for (old_path, new_path) in renamed_pairs {
                results.push(RenameResultRow {
                    path: old_path,
                    new_path: Some(new_path),
                    ok: false,
                    error: Some(error.clone()),
                });
            }
            let response = RenameResponse {
                results,
                successful_paths: Vec::new(),
                error: None,
            };
            return (response, TrackRefresh::default());
        }
    };

    for row in request.rows {
        let Some((_, new_path)) = renamed_pairs
            .iter()
            .find(|(old_path, _)| old_path == &row.path)
            .cloned()
            .or_else(|| Some((row.path.clone(), row.path.clone())))
        else {
            continue;
        };
        if results.iter().any(|result| result.path == row.path) {
            continue;
        }
        successful_paths.push(new_path.clone());
        results.push(RenameResultRow {
            path: row.path,
            new_path: Some(new_path),
            ok: true,
            error: None,
        });
    }

    let response = RenameResponse {
        results,
        successful_paths,
        error: None,
    };
    (response, refresh)
}

pub(crate) fn serialize_load_response(response: &LoadResponse) -> Result<Vec<u8>, String> {
    serde_json::to_vec(response)
        .map_err(|error| format!("failed to serialize tag editor load response: {error}"))
}

pub(crate) fn serialize_save_response(response: &SaveResponse) -> Result<Vec<u8>, String> {
    serde_json::to_vec(response)
        .map_err(|error| format!("failed to serialize tag editor save response: {error}"))
}

pub(crate) fn serialize_rename_response(response: &RenameResponse) -> Result<Vec<u8>, String> {
    serde_json::to_vec(response)
        .map_err(|error| format!("failed to serialize tag editor rename response: {error}"))
}

/// A response that reports `message` instead of any file's result, for a
/// request that failed as a whole.
pub(crate) fn serialize_error_response(message: &str) -> Vec<u8> {
    serde_json::json!({ "error": message })
        .to_string()
        .into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;
    use lofty::config::{ParseOptions, WriteOptions};
    use lofty::file::AudioFile;
    use lofty::flac::FlacFile;
    use lofty::tag::TagType;
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;
    use std::time::{SystemTime, UNIX_EPOCH};

    use lofty::ape::{ApeFile, ApeItem, ApeTag};
    use lofty::mp4::{Atom, AtomData, AtomIdent, Ilst, Mp4File};
    use lofty::ogg::{OggPictureStorage, OpusFile, VorbisComments};
    use lofty::picture::{MimeType, Picture, PictureInformation, PictureType};
    use lofty::tag::ItemValue;
    use lofty::wavpack::WavPackFile;

    use crate::tag_write::{
        write_test_flac as write_flac, write_test_monkeys_audio, write_test_mp3_with_id3v23,
        write_test_mp3_with_only_id3v1, write_test_mp4, write_test_opus, write_test_wavpack,
    };

    #[test]
    fn album_and_section_keys_with_a_separator_select_their_tracks() {
        let root = PathBuf::from("/music|archive");
        let track = |path: &str| crate::library::LibraryTrack {
            path: root.join(path),
            root_path: root.clone(),
            ..crate::library::LibraryTrack::default()
        };
        let library = LibrarySnapshot {
            roots: vec![crate::library::LibraryRoot {
                path: root.clone(),
                name: String::new(),
            }],
            tracks: crate::library::shared_tracks(vec![
                track("A|B/C/01.flac"),
                track("A/B|C/01.flac"),
                track("A|B/C/Bonus/02.flac"),
            ]),
            ..LibrarySnapshot::default()
        };
        let selection = |row_type: &str, key: String| SelectionEntry {
            path: None,
            row_type: Some(row_type.to_string()),
            key: Some(key),
            artist: None,
            name: None,
            track_path: None,
        };
        let root_key = root.to_string_lossy();
        assert_eq!(
            resolve_library_selection_paths(
                &library,
                &selection("album", album_row_key(&root_key, "A|B", "C"))
            ),
            [root.join("A|B/C/01.flac")]
        );
        assert_eq!(
            resolve_library_selection_paths(
                &library,
                &selection("section", section_row_key(&root_key, "A|B", "C", "Bonus"))
            ),
            [root.join("A|B/C/Bonus/02.flac")]
        );
    }

    pub(super) fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-tag-editor-{name}-{}-{nanos}",
            std::process::id()
        ));
        fs::create_dir_all(&dir).expect("create test dir");
        dir
    }

    pub(super) fn dir_entries(dir: &Path) -> Vec<String> {
        let mut names = fs::read_dir(dir)
            .expect("read test dir")
            .map(|entry| {
                entry
                    .expect("dir entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        names.sort();
        names
    }

    fn vorbis_values(path: &Path, key: &str) -> Vec<String> {
        let mut file = fs::File::open(path).expect("open FLAC");
        let flac = FlacFile::read_from(&mut file, ParseOptions::new()).expect("parse FLAC");
        flac.vorbis_comments()
            .map(|comments| comments.get_all(key).map(str::to_string).collect())
            .unwrap_or_default()
    }

    pub(super) fn load_one(path: &Path) -> TagEditorRow {
        let response = load_rows_for_paths(&[path.to_path_buf()]);
        assert!(response.failures.is_empty(), "{:?}", response.failures);
        response.rows.into_iter().next().expect("loaded row")
    }

    pub(super) fn save_json(rows: &[serde_json::Value]) -> SaveResponse {
        let blob = serde_json::to_vec(&serde_json::json!({ "rows": rows })).expect("encode");
        save_rows(parse_save_request(&blob).expect("parse save request"))
    }

    pub(super) fn edited(row: &TagEditorRow, changed: &[&str]) -> serde_json::Value {
        let mut value = serde_json::to_value(row).expect("encode row");
        value["changedFields"] = serde_json::json!(changed);
        value
    }

    pub(super) fn assert_saved(response: &SaveResponse) {
        assert!(
            response.results.iter().all(|result| result.ok),
            "{:?}",
            response.results
        );
    }

    fn flac_with_full_date(dir: &Path) -> PathBuf {
        let path = dir.join("song.flac");
        write_flac(
            &path,
            &[
                ("TITLE", "Old"),
                ("ARTIST", "A"),
                ("ARTIST", "B"),
                ("GENRE", "Rock"),
                ("GENRE", "Pop"),
                ("DATE", "1997-05-12"),
                ("ORIGINALDATE", "1995-01-01"),
                ("CUSTOM", "kept"),
            ],
        );
        path
    }

    #[test]
    fn editing_one_field_keeps_dates_and_multiple_values() {
        let dir = test_dir("flac-roundtrip");
        let path = flac_with_full_date(&dir);
        #[cfg(unix)]
        fs::set_permissions(&path, fs::Permissions::from_mode(0o640)).expect("chmod");

        let mut row = load_one(&path);
        assert_eq!(row.year, "1997-05-12");
        assert_eq!(row.artist, "A");
        row.title = "New".to_string();
        let response = save_json(&[edited(&row, &["title"])]);

        assert_saved(&response);
        assert_eq!(response.successful_paths, vec![row.path.clone()]);
        assert_eq!(vorbis_values(&path, "TITLE"), ["New"]);
        assert_eq!(vorbis_values(&path, "ARTIST"), ["A", "B"]);
        assert_eq!(vorbis_values(&path, "GENRE"), ["Rock", "Pop"]);
        assert_eq!(vorbis_values(&path, "DATE"), ["1997-05-12"]);
        assert_eq!(vorbis_values(&path, "ORIGINALDATE"), ["1995-01-01"]);
        assert_eq!(vorbis_values(&path, "CUSTOM"), ["kept"]);
        assert_eq!(dir_entries(&dir), ["song.flac"]);
        #[cfg(unix)]
        assert_eq!(
            fs::metadata(&path).expect("metadata").permissions().mode() & 0o777,
            0o640
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn requests_without_changed_fields_accept_full_dates() {
        let dir = test_dir("legacy-request");
        let path = flac_with_full_date(&dir);
        let mut row = load_one(&path);
        row.title = "New".to_string();

        let response = save_json(&[serde_json::to_value(&row).expect("encode row")]);

        assert_saved(&response);
        assert_eq!(vorbis_values(&path, "TITLE"), ["New"]);
        assert_eq!(vorbis_values(&path, "DATE"), ["1997-05-12"]);
        assert_eq!(vorbis_values(&path, "ORIGINALDATE"), ["1995-01-01"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn editing_the_year_replaces_only_the_recording_date() {
        let dir = test_dir("year-edit");
        let path = dir.join("song.flac");
        write_flac(
            &path,
            &[
                ("DATE", "1997-05-12"),
                ("YEAR", "1997"),
                ("ORIGINALDATE", "1995"),
            ],
        );
        let mut row = load_one(&path);

        row.year = "1998-06".to_string();
        assert_saved(&save_json(&[edited(&row, &["year"])]));
        assert_eq!(vorbis_values(&path, "DATE"), ["1998-06"]);
        assert!(vorbis_values(&path, "YEAR").is_empty());
        assert_eq!(vorbis_values(&path, "ORIGINALDATE"), ["1995"]);

        row.year = String::new();
        assert_saved(&save_json(&[edited(&row, &["year"])]));
        assert!(vorbis_values(&path, "DATE").is_empty());
        assert_eq!(vorbis_values(&path, "ORIGINALDATE"), ["1995"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn invalid_year_fails_without_touching_the_file() {
        let dir = test_dir("invalid-year");
        let path = flac_with_full_date(&dir);
        let before = fs::read(&path).expect("read fixture");
        let mut row = load_one(&path);
        row.year = "May 1998".to_string();

        let response = save_json(&[edited(&row, &["year"])]);

        assert!(response.successful_paths.is_empty());
        let error = response.results[0].error.as_deref().unwrap_or_default();
        assert!(error.contains("invalid year"), "{error}");
        assert_eq!(fs::read(&path).expect("read fixture"), before);
        assert_eq!(dir_entries(&dir), ["song.flac"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn id3v23_files_stay_id3v23() {
        let dir = test_dir("id3v23");
        let path = dir.join("song.mp3");
        write_test_mp3_with_id3v23(&path);

        let mut row = load_one(&path);
        assert_eq!(row.year, "1997-05-12");
        row.title = "New".to_string();
        assert_saved(&save_json(&[edited(&row, &["title"])]));

        assert_eq!(&fs::read(&path).expect("read result")[..4], b"ID3\x03");
        let tagged = lofty::read_from_path(&path).expect("read result");
        let tag = tagged.tag(TagType::Id3v2).expect("ID3v2 tag");
        assert_eq!(tag.get_string(ItemKey::TrackTitle), Some("New"));
        assert_eq!(tag.get_string(ItemKey::TrackArtist), Some("Artist"));
        assert_eq!(tag.get_string(ItemKey::RecordingDate), Some("1997-05-12"));
        assert_eq!(tag.get_string(ItemKey::OriginalReleaseDate), Some("1995"));
        assert_eq!(tag.pictures().len(), 1);
        assert_eq!(tag.pictures()[0].data(), b"\xff\xd8\xff\xd9");
        assert!(tagged.properties().duration() > std::time::Duration::ZERO);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn new_tag_starts_from_the_displayed_tag() {
        let dir = test_dir("id3v1-seed");
        let path = dir.join("song.mp3");
        write_test_mp3_with_only_id3v1(
            &path,
            &[
                (ItemKey::TrackTitle, "Old"),
                (ItemKey::TrackArtist, "Artist"),
                (ItemKey::AlbumTitle, "Album"),
                (ItemKey::Year, "1997"),
            ],
        );

        let mut row = load_one(&path);
        assert_eq!(row.artist, "Artist");
        assert_eq!(row.year, "1997");
        row.title = "New".to_string();
        assert_saved(&save_json(&[edited(&row, &["title"])]));

        let reloaded = load_one(&path);
        assert_eq!(reloaded.title, "New");
        assert_eq!(reloaded.artist, "Artist");
        assert_eq!(reloaded.album, "Album");
        assert_eq!(reloaded.year, "1997");
        let tagged = lofty::read_from_path(&path).expect("read tags");
        let primary = tagged.primary_tag().expect("ID3v2 tag");
        assert_eq!(primary.tag_type(), TagType::Id3v2);
        assert_eq!(primary.get_string(ItemKey::RecordingDate), Some("1997"));
        assert_eq!(
            tagged
                .tag(TagType::Id3v1)
                .expect("ID3v1 tag")
                .get_string(ItemKey::Year),
            Some("1997")
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn raw_surround_saves_change_only_edited_items() {
        let dir = test_dir("raw");
        let path = dir.join("song.ac3");
        crate::raw_audio::write_test_apev2_file(
            &path,
            &[
                ("Title", "Old"),
                ("Year", "1997-05-12"),
                ("Composer", "Kept"),
                ("Track", "1/9"),
            ],
            true,
        );

        let mut row = load_one(&path);
        assert_eq!(row.track_no, "01");
        row.title = "New".to_string();
        row.track_no = "2".to_string();
        assert_saved(&save_json(&[edited(&row, &["title", "trackNo"])]));

        let bytes = fs::read(&path).expect("read result");
        let text = String::from_utf8_lossy(&bytes);
        for expected in ["TitleNew", "Year1997-05-12", "ComposerKept", "Track2/9"] {
            let found = text.replace('\0', "").contains(expected);
            assert!(found, "missing {expected} in {text:?}");
        }
        assert_eq!(dir_entries(&dir), ["song.ac3"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn raw_surround_total_edits_keep_the_number() {
        let dir = test_dir("raw-totals");
        let path = dir.join("song.ac3");
        crate::raw_audio::write_test_apev2_file(&path, &[("Track", "3"), ("Disc", "1/2")], true);

        let mut row = load_one(&path);
        row.total_tracks = "12".to_string();
        row.total_discs = "3".to_string();
        assert_saved(&save_json(&[edited(&row, &["totalTracks", "totalDiscs"])]));

        let reloaded = load_one(&path);
        assert_eq!(
            [
                reloaded.track_no,
                reloaded.total_tracks,
                reloaded.disc_no,
                reloaded.total_discs
            ],
            ["03", "12", "1", "3"]
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn loading_reports_unreadable_files_and_keeps_the_rest() {
        let dir = test_dir("load-failures");
        let good = flac_with_full_date(&dir);
        let garbage = dir.join("garbage.flac");
        fs::write(&garbage, b"not audio").expect("write garbage");
        let missing = dir.join("missing.mp3");
        let missing_raw = dir.join("missing.ac3");
        let raw = dir.join("song.ac3");
        crate::raw_audio::write_test_apev2_file(&raw, &[("Title", "Raw")], true);
        let paths = vec![
            garbage.clone(),
            good.clone(),
            missing.clone(),
            missing_raw.clone(),
            raw.clone(),
        ];

        let response = load_rows_for_paths(&paths);

        let loaded = response
            .rows
            .iter()
            .map(|row| PathBuf::from(&row.path))
            .collect::<Vec<_>>();
        assert_eq!(loaded, [good, raw]);
        let failed = response
            .failures
            .iter()
            .map(|failure| PathBuf::from(&failure.path))
            .collect::<Vec<_>>();
        assert_eq!(failed, [garbage, missing, missing_raw]);
        assert!(response
            .failures
            .iter()
            .all(|failure| failure.error.contains("failed to read tag data")));
        assert_eq!(response.resolved_paths.len(), paths.len());
        assert_eq!(response.error, None);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn saves_that_would_delete_unreadable_comments_report_why() {
        use crate::vorbis_comment_scan::fixtures;

        let dir = test_dir("unreadable-comments");
        let path = dir.join("song.opus");
        let comments: [&[u8]; 3] = [b"TITLE=Old", b"ARTIST=\xff not UTF-8", b"ALBUM=Album"];
        fs::write(&path, fixtures::opus(&comments)).expect("write Opus");
        let before = fs::read(&path).expect("read fixture");
        let mut row = load_one(&path);
        row.title = "New".to_string();

        let response = save_json(&[edited(&row, &["title"])]);

        assert!(response.successful_paths.is_empty());
        assert!(!response.results[0].ok);
        let error = response.results[0].error.as_deref().unwrap_or_default();
        assert!(
            error.ends_with(
                "1 comment in this file can't be read safely (1 with a value that is not \
                 UTF-8); saving would delete it"
            ),
            "{error}"
        );
        assert_eq!(fs::read(&path).expect("read file"), before);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn save_request_parses_changed_fields() {
        let row = TagEditorRow {
            path: "/music/song.flac".to_string(),
            ..TagEditorRow::default()
        };
        let mut with_fields = edited(&row, &["albumArtist", "totalDiscs"]);
        let request: SaveRequest = parse_save_request(
            &serde_json::to_vec(&serde_json::json!({ "rows": [with_fields.clone()] }))
                .expect("encode"),
        )
        .expect("parse");
        assert_eq!(
            request.rows[0].changed_fields(),
            HashSet::from([TagField::AlbumArtist, TagField::TotalDiscs])
        );

        let legacy = parse_save_request(
            &serde_json::to_vec(&serde_json::json!([
                serde_json::to_value(&row).expect("row")
            ]))
            .expect("encode"),
        )
        .expect("parse bare rows");
        assert_eq!(legacy.rows[0].row.path, "/music/song.flac");
        assert_eq!(
            legacy.rows[0].changed_fields(),
            TagField::ALL.into_iter().collect()
        );

        with_fields["changedFields"] = serde_json::json!(["fileName"]);
        let unknown =
            serde_json::to_vec(&serde_json::json!({ "rows": [with_fields] })).expect("encode");
        assert!(parse_save_request(&unknown).is_err());
    }

    fn picture(data: &[u8], mime_type: MimeType) -> Picture {
        Picture::unchecked(data.to_vec())
            .mime_type(mime_type)
            .pic_type(PictureType::CoverFront)
            .build()
    }

    const JPEG: &[u8] = b"\xff\xd8\xff\xd9";
    const PNG: &[u8] = b"\x89PNG\r\n\x1a\n";

    const TITLE_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"\xa9nam");
    const ARTIST_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"\xa9ART");
    const GENRE_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"\xa9gen");
    const DATE_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"\xa9day");
    const TRACK_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"trkn");

    fn text_atom(ident: AtomIdent<'static>, values: &[&str]) -> Atom<'static> {
        let data = values
            .iter()
            .map(|value| AtomData::UTF8((*value).to_string()))
            .collect();
        Atom::from_collection(ident, data).expect("atom values")
    }

    fn read_ilst(path: &Path) -> Ilst {
        let mut file = fs::File::open(path).expect("open MP4");
        Mp4File::read_from(&mut file, ParseOptions::new())
            .expect("parse MP4")
            .ilst()
            .cloned()
            .expect("ilst")
    }

    fn atom_values(ilst: &Ilst, ident: &AtomIdent<'_>) -> Vec<AtomData> {
        ilst.get(ident)
            .map(|atom| atom.data().cloned().collect())
            .unwrap_or_default()
    }

    fn atoms_except(ilst: &Ilst, idents: &[AtomIdent<'_>]) -> Vec<Atom<'static>> {
        ilst.into_iter()
            .filter(|atom| !idents.contains(atom.ident()))
            .cloned()
            .collect()
    }

    /// An MP4 with two artists, two covers, a track pair, and atoms lofty
    /// has no generic key for, as read back from the file.
    fn mp4_fixture(dir: &Path) -> (PathBuf, Ilst) {
        let path = dir.join("song.m4a");
        let mut ilst = Ilst::new();
        ilst.insert(text_atom(TITLE_ATOM, &["Old"]));
        ilst.insert(text_atom(ARTIST_ATOM, &["A", "B"]));
        ilst.insert(text_atom(GENRE_ATOM, &["Rock", "Pop"]));
        ilst.insert(text_atom(DATE_ATOM, &["1997-05-12"]));
        ilst.insert(text_atom(
            AtomIdent::Freeform {
                mean: "com.apple.iTunes".into(),
                name: "CUSTOM".into(),
            },
            &["kept"],
        ));
        ilst.insert(text_atom(AtomIdent::Fourcc(*b"zzzz"), &["unknown"]));
        ilst.set_track(1);
        ilst.set_track_total(9);
        ilst.insert_picture(picture(JPEG, MimeType::Jpeg));
        ilst.insert_picture(picture(PNG, MimeType::Png));
        write_test_mp4(&path, &ilst);
        let written = read_ilst(&path);
        assert_eq!(atom_values(&written, &ARTIST_ATOM).len(), 2);
        assert_eq!(written.pictures().expect("covers").count(), 2);
        (path, written)
    }

    #[test]
    fn title_only_mp4_save_keeps_every_value_and_picture() {
        let dir = test_dir("mp4-title");
        let (path, before) = mp4_fixture(&dir);

        let mut row = load_one(&path);
        assert_eq!(row.artist, "A");
        row.title = "New".to_string();
        assert_saved(&save_json(&[edited(&row, &["title"])]));

        let after = read_ilst(&path);
        assert_eq!(
            atoms_except(&after, &[TITLE_ATOM]),
            atoms_except(&before, &[TITLE_ATOM])
        );
        assert_eq!(
            atom_values(&after, &TITLE_ATOM),
            [AtomData::UTF8("New".to_string())]
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn edited_mp4_fields_replace_all_their_values() {
        let dir = test_dir("mp4-edit");
        let (path, before) = mp4_fixture(&dir);

        let mut row = load_one(&path);
        row.artist = "C".to_string();
        row.genre = "Jazz".to_string();
        row.track_no = "2".to_string();
        row.year = "1998".to_string();
        assert_saved(&save_json(&[edited(
            &row,
            &["artist", "genre", "trackNo", "year"],
        )]));

        let after = read_ilst(&path);
        let edited_atoms = [ARTIST_ATOM, GENRE_ATOM, DATE_ATOM, TRACK_ATOM];
        assert_eq!(
            atoms_except(&after, &edited_atoms),
            atoms_except(&before, &edited_atoms)
        );
        assert_eq!(
            atom_values(&after, &ARTIST_ATOM),
            [AtomData::UTF8("C".to_string())]
        );
        assert_eq!(
            atom_values(&after, &GENRE_ATOM),
            [AtomData::UTF8("Jazz".to_string())]
        );
        assert_eq!(
            atom_values(&after, &DATE_ATOM),
            [AtomData::UTF8("1998".to_string())]
        );
        assert_eq!((after.track(), after.track_total()), (Some(2), Some(9)));
        let reloaded = load_one(&path);
        assert_eq!(
            (reloaded.artist.as_str(), reloaded.track_no.as_str()),
            ("C", "02")
        );
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    const VORBIS_FIXTURE: [(&str, &str); 6] = [
        ("TITLE", "Old"),
        ("ARTIST", "A"),
        ("ENCODER", "Encoder X"),
        ("artist", "B"),
        ("TRACKNUMBER", "1"),
        ("CUSTOM", "kept"),
    ];

    fn read_flac(path: &Path) -> FlacFile {
        let mut file = fs::File::open(path).expect("open FLAC");
        FlacFile::read_from(&mut file, ParseOptions::new()).expect("parse FLAC")
    }

    /// The comments of a FLAC or Opus file, with FLAC's picture blocks.
    fn read_vorbis(path: &Path) -> (VorbisComments, Vec<(Picture, PictureInformation)>) {
        if path
            .extension()
            .is_some_and(|extension| extension == "flac")
        {
            let flac = read_flac(path);
            let comments = flac.vorbis_comments().cloned().expect("comments");
            (comments, flac.pictures().to_vec())
        } else {
            let mut file = fs::File::open(path).expect("open Opus");
            let opus = OpusFile::read_from(&mut file, ParseOptions::new()).expect("parse Opus");
            let comments = opus.vorbis_comments().clone();
            let pictures = comments.pictures().to_vec();
            (comments, pictures)
        }
    }

    fn vorbis_items(comments: &VorbisComments) -> Vec<(String, String)> {
        comments
            .items()
            .map(|(key, value)| (key.to_string(), value.to_string()))
            .collect()
    }

    /// FLAC and Opus files with an explicit encoder comment, two artists
    /// spelled differently, and a cover, as read back from the files.
    fn vorbis_fixtures(dir: &Path) -> Vec<PathBuf> {
        let info = PictureInformation {
            width: 1,
            height: 1,
            color_depth: 24,
            num_colors: 0,
        };
        let flac = dir.join("song.flac");
        write_flac(&flac, &VORBIS_FIXTURE);
        let mut file = read_flac(&flac);
        file.insert_picture(picture(JPEG, MimeType::Jpeg), Some(info))
            .expect("add FLAC picture");
        file.save_to_path(&flac, WriteOptions::new())
            .expect("write FLAC picture");

        let opus = dir.join("song.opus");
        let mut comments = VorbisComments::new();
        comments.set_vendor("ferrous-test".to_string());
        for (key, value) in VORBIS_FIXTURE {
            comments.push(key.to_string(), value.to_string());
        }
        comments
            .insert_picture(picture(JPEG, MimeType::Jpeg), Some(info))
            .expect("add Opus picture");
        write_test_opus(&opus, &comments);

        for path in [&flac, &opus] {
            let (comments, pictures) = read_vorbis(path);
            assert_eq!(comments.get("ENCODER"), Some("Encoder X"), "{path:?}");
            assert_eq!(pictures.len(), 1, "{path:?}");
        }
        vec![flac, opus]
    }

    #[test]
    fn title_only_vorbis_saves_keep_the_encoder_comment_and_vendor() {
        let dir = test_dir("vorbis-title");
        for path in vorbis_fixtures(&dir) {
            let (before, pictures_before) = read_vorbis(&path);

            let mut row = load_one(&path);
            row.title = "New".to_string();
            assert_saved(&save_json(&[edited(&row, &["title"])]));

            let (after, pictures_after) = read_vorbis(&path);
            let mut expected = vorbis_items(&before);
            expected[0].1 = "New".to_string();
            assert_eq!(vorbis_items(&after), expected, "{path:?}");
            assert_eq!(after.vendor(), before.vendor(), "{path:?}");
            assert_eq!(pictures_after, pictures_before, "{path:?}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn loads_report_the_values_after_the_first_until_an_edit_replaces_them() {
        let dir = test_dir("vorbis-extra-values");
        for path in vorbis_fixtures(&dir) {
            let mut row = load_one(&path);
            assert_eq!(row.artist, "A", "{path:?}");
            let expected: BTreeMap<String, Vec<String>> =
                [("artist".to_string(), vec!["B".to_string()])].into();
            assert_eq!(row.extra_values, expected, "{path:?}");
            let encoded = serde_json::to_value(&row).expect("encode row");
            assert_eq!(encoded["extraValues"]["artist"], serde_json::json!(["B"]));

            row.artist = "C".to_string();
            assert_saved(&save_json(&[edited(&row, &["artist"])]));
            assert!(load_one(&path).extra_values.is_empty(), "{path:?}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn raw_surround_loads_split_multi_value_ape_items() {
        let dir = test_dir("raw-extra-values");
        let path = dir.join("song.dts");
        let mut bytes = b"stub".to_vec();
        bytes.append(&mut crate::raw_audio::build_test_apev2_tag(
            &[("Title", "Old"), ("Artist", "A\0B\0C"), ("Genre", "Rock")],
            true,
        ));
        fs::write(&path, &bytes).expect("write file");

        let row = load_one(&path);
        assert_eq!((row.title.as_str(), row.artist.as_str()), ("Old", "A"));
        let expected: BTreeMap<String, Vec<String>> =
            [("artist".to_string(), vec!["B".to_string(), "C".to_string()])].into();
        assert_eq!(row.extra_values, expected);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn edited_vorbis_fields_replace_all_their_values() {
        let dir = test_dir("vorbis-edit");
        for path in vorbis_fixtures(&dir) {
            let (before, pictures_before) = read_vorbis(&path);

            let mut row = load_one(&path);
            row.artist = "C".to_string();
            row.track_no = "2".to_string();
            assert_saved(&save_json(&[edited(&row, &["artist", "trackNo"])]));

            let (after, pictures_after) = read_vorbis(&path);
            let expected = [
                ("TITLE", "Old"),
                ("ARTIST", "C"),
                ("ENCODER", "Encoder X"),
                ("TRACKNUMBER", "2"),
                ("CUSTOM", "kept"),
            ]
            .map(|(key, value)| (key.to_string(), value.to_string()));
            assert_eq!(vorbis_items(&after), expected, "{path:?}");
            assert_eq!(after.vendor(), before.vendor(), "{path:?}");
            assert_eq!(pictures_after, pictures_before, "{path:?}");
            assert_eq!(load_one(&path).artist, "C");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// The bytes of `path` with the only occurrence of `from` replaced by
    /// `to`, which has the same length.
    fn patched(path: &Path, from: &[u8], to: &[u8]) -> Vec<u8> {
        assert_eq!(from.len(), to.len());
        let mut bytes = fs::read(path).expect("read fixture");
        let matches = bytes
            .windows(from.len())
            .enumerate()
            .filter(|(_, window)| *window == from)
            .map(|(at, _)| at)
            .collect::<Vec<_>>();
        assert_eq!(matches.len(), 1, "{from:?}");
        bytes[matches[0]..matches[0] + to.len()].copy_from_slice(to);
        bytes
    }

    fn contains(bytes: &[u8], part: &[u8]) -> bool {
        bytes.windows(part.len()).any(|window| window == part)
    }

    /// The `data` atom holding `text` as UTF-8, and the same atom holding
    /// `utf16`, which has as many bytes, as big-endian UTF-16.
    fn utf16_data_atom(text: &str, utf16: &str) -> (Vec<u8>, Vec<u8>) {
        let atom = |data_type: u8, payload: &[u8]| {
            let mut atom = b"data\0\0\0".to_vec();
            atom.push(data_type);
            atom.extend_from_slice(&[0; 4]);
            atom.extend_from_slice(payload);
            atom
        };
        let utf16 = utf16
            .encode_utf16()
            .flat_map(u16::to_be_bytes)
            .collect::<Vec<_>>();
        (atom(1, text.as_bytes()), atom(2, &utf16))
    }

    const CUSTOM_ATOM: AtomIdent<'static> = AtomIdent::Freeform {
        mean: std::borrow::Cow::Borrowed("com.apple.iTunes"),
        name: std::borrow::Cow::Borrowed("CUSTOM"),
    };

    /// An MP4 whose first artist and freeform atom are UTF-16. lofty cannot
    /// write UTF-16, so placeholders are written as UTF-8 and then patched.
    fn utf16_mp4_fixture(dir: &Path) -> PathBuf {
        let path = dir.join("song.m4a");
        let mut ilst = Ilst::new();
        ilst.insert(text_atom(TITLE_ATOM, &["Old"]));
        ilst.insert(text_atom(ARTIST_ATOM, &["WXYZ", "Cd"]));
        ilst.insert(text_atom(CUSTOM_ATOM, &["PQRS"]));
        ilst.insert(text_atom(DATE_ATOM, &["1997"]));
        write_test_mp4(&path, &ilst);
        for (placeholder, text) in [("WXYZ", "AB"), ("PQRS", "KX")] {
            let (from, to) = utf16_data_atom(placeholder, text);
            let bytes = patched(&path, &from, &to);
            fs::write(&path, bytes).expect("write fixture");
        }
        let written = read_ilst(&path);
        assert_eq!(
            atom_values(&written, &ARTIST_ATOM),
            [
                AtomData::UTF16("AB".to_string()),
                AtomData::UTF8("Cd".to_string())
            ]
        );
        assert_eq!(
            atom_values(&written, &CUSTOM_ATOM),
            [AtomData::UTF16("KX".to_string())]
        );
        path
    }

    #[test]
    fn mp4_saves_keep_the_text_of_unedited_utf16_values() {
        let dir = test_dir("mp4-utf16");
        for field in ["title", "year"] {
            let path = utf16_mp4_fixture(&dir);

            let mut row = load_one(&path);
            assert_eq!(row.artist, "AB");
            let (title, year) = if field == "title" {
                ("New", "1997")
            } else {
                ("Old", "1998")
            };
            row.title = title.to_string();
            row.year = year.to_string();
            assert_saved(&save_json(&[edited(&row, &[field])]));

            let after = read_ilst(&path);
            assert_eq!(
                atom_values(&after, &ARTIST_ATOM),
                [
                    AtomData::UTF8("AB".to_string()),
                    AtomData::UTF8("Cd".to_string())
                ],
                "{field}"
            );
            assert_eq!(
                atom_values(&after, &CUSTOM_ATOM),
                [AtomData::UTF8("KX".to_string())],
                "{field}"
            );
            let reloaded = load_one(&path);
            assert_eq!(
                (
                    reloaded.artist.as_str(),
                    reloaded.title.as_str(),
                    reloaded.year.as_str()
                ),
                ("AB", title, year),
                "{field}"
            );
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    const COMPILATION_ATOM: AtomIdent<'static> = AtomIdent::Fourcc(*b"cpil");

    fn atom_count(ilst: &Ilst, ident: &AtomIdent<'_>) -> usize {
        ilst.into_iter()
            .filter(|atom| atom.ident() == ident)
            .count()
    }

    /// Write `ilst` to an MP4 at `path`, then rename each placeholder atom of
    /// `renames` to an identifier the `ilst` already holds. lofty merges atoms
    /// with one identifier, so it cannot write them as separate atoms.
    fn write_split_atoms_mp4(path: &Path, ilst: &Ilst, renames: &[(&[u8; 4], &[u8; 4])]) {
        write_test_mp4(path, ilst);
        for (placeholder, ident) in renames {
            let bytes = patched(path, *placeholder, *ident);
            fs::write(path, bytes).expect("write fixture");
        }
    }

    /// An MP4 with separate `©ART` (`A`, `B`), `trkn` (1/9, 2/10), and `cpil`
    /// (false, true) atoms, and a UTF-16 freeform atom (`KX`).
    fn split_atoms_mp4_fixture(dir: &Path) -> PathBuf {
        let path = dir.join("song.m4a");
        let mut ilst = Ilst::new();
        ilst.insert(text_atom(TITLE_ATOM, &["Old"]));
        ilst.insert(text_atom(ARTIST_ATOM, &["A"]));
        ilst.insert(text_atom(AtomIdent::Fourcc(*b"\xa9ARx"), &["B"]));
        ilst.set_track(1);
        ilst.set_track_total(9);
        ilst.insert(Atom::new(
            AtomIdent::Fourcc(*b"trkx"),
            AtomData::Unknown {
                code: lofty::mp4::DataType::Reserved,
                data: vec![0, 0, 0, 2, 0, 10, 0, 0],
            },
        ));
        ilst.insert(Atom::new(COMPILATION_ATOM, AtomData::Bool(false)));
        ilst.insert(Atom::new(AtomIdent::Fourcc(*b"cpix"), AtomData::Bool(true)));
        ilst.insert(text_atom(CUSTOM_ATOM, &["PQRS"]));
        write_split_atoms_mp4(
            &path,
            &ilst,
            &[
                (b"\xa9ARx", b"\xa9ART"),
                (b"trkx", b"trkn"),
                (b"cpix", b"cpil"),
            ],
        );
        let (from, to) = utf16_data_atom("PQRS", "KX");
        let bytes = patched(&path, &from, &to);
        fs::write(&path, bytes).expect("write fixture");

        let written = read_ilst(&path);
        for ident in [ARTIST_ATOM, TRACK_ATOM, COMPILATION_ATOM] {
            assert_eq!(atom_count(&written, &ident), 2, "{ident:?}");
        }
        assert_eq!(
            atom_values(&written, &CUSTOM_ATOM),
            [AtomData::UTF16("KX".to_string())]
        );
        path
    }

    /// Only the UTF-16 atom is rewritten; every other atom keeps its
    /// boundaries, which decide what lofty and players read.
    #[test]
    fn mp4_saves_keep_separate_atoms_next_to_utf16_values() {
        let dir = test_dir("mp4-utf16-split");
        let path = split_atoms_mp4_fixture(&dir);
        let before = read_ilst(&path);
        let generic = |ilst: &Ilst| {
            let tag = Tag::from(ilst.clone());
            let artists = tag
                .get_strings(ItemKey::TrackArtist)
                .map(str::to_string)
                .collect::<Vec<_>>();
            (artists, tag.track(), tag.track_total())
        };
        assert_eq!(
            generic(&before),
            (vec!["A".to_string(), "B".to_string()], Some(2), Some(10))
        );

        // The second save starts from the first one's output.
        for title in ["New", "Newer"] {
            let mut row = load_one(&path);
            row.title = title.to_string();
            assert_saved(&save_json(&[edited(&row, &["title"])]));

            let after = read_ilst(&path);
            assert_eq!(
                atoms_except(&after, &[TITLE_ATOM, CUSTOM_ATOM]),
                atoms_except(&before, &[TITLE_ATOM, CUSTOM_ATOM]),
                "{title}"
            );
            assert_eq!(
                atom_values(&after, &CUSTOM_ATOM),
                [AtomData::UTF8("KX".to_string())],
                "{title}"
            );
            assert_eq!(
                atom_values(&after, &TITLE_ATOM),
                [AtomData::UTF8(title.to_string())]
            );
            assert_eq!(generic(&after), generic(&before), "{title}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// Edited atoms replace every atom with their identifier, so none is
    /// merged into a kept one.
    #[test]
    fn edited_mp4_fields_replace_every_separate_atom() {
        let dir = test_dir("mp4-split-edit");
        let path = split_atoms_mp4_fixture(&dir);
        let before = read_ilst(&path);

        let mut row = load_one(&path);
        row.artist = "C".to_string();
        row.track_no = "3".to_string();
        assert_saved(&save_json(&[edited(&row, &["artist", "trackNo"])]));

        let after = read_ilst(&path);
        let edited_atoms = [ARTIST_ATOM, TRACK_ATOM, CUSTOM_ATOM];
        assert_eq!(
            atoms_except(&after, &edited_atoms),
            atoms_except(&before, &edited_atoms)
        );
        assert_eq!(
            atom_values(&after, &CUSTOM_ATOM),
            [AtomData::UTF8("KX".to_string())]
        );
        assert_eq!(atom_count(&after, &ARTIST_ATOM), 1);
        assert_eq!(
            atom_values(&after, &ARTIST_ATOM),
            [AtomData::UTF8("C".to_string())]
        );
        assert_eq!(atom_count(&after, &TRACK_ATOM), 1);
        assert_eq!(after.track(), Some(3));
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// Converting a UTF-16 value means putting its atom back, which would
    /// merge it with another atom of its identifier, so the save is refused.
    #[test]
    fn mp4_saves_refuse_utf16_values_in_split_atoms() {
        let dir = test_dir("mp4-utf16-dup");
        let path = dir.join("song.m4a");
        let mut ilst = Ilst::new();
        ilst.insert(text_atom(TITLE_ATOM, &["Old"]));
        ilst.insert(text_atom(ARTIST_ATOM, &["WXYZ"]));
        ilst.insert(text_atom(AtomIdent::Fourcc(*b"\xa9ARx"), &["C"]));
        write_split_atoms_mp4(&path, &ilst, &[(b"\xa9ARx", b"\xa9ART")]);
        let (from, to) = utf16_data_atom("WXYZ", "AB");
        let bytes = patched(&path, &from, &to);
        fs::write(&path, &bytes).expect("write fixture");
        let written = read_ilst(&path);
        assert_eq!(atom_count(&written, &ARTIST_ATOM), 2);

        let mut row = load_one(&path);
        row.title = "New".to_string();
        let response = save_json(&[edited(&row, &["title"])]);

        assert!(response.successful_paths.is_empty());
        let error = response.results[0].error.as_deref().unwrap_or_default();
        assert!(
            error.contains("more than one ©ART atom") && error.contains("UTF-16"),
            "{error}"
        );
        assert_eq!(fs::read(&path).expect("read file"), bytes);
        assert_eq!(dir_entries(&dir), ["song.m4a"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    const UPPER_BPM_ATOM: AtomIdent<'static> = AtomIdent::Freeform {
        mean: std::borrow::Cow::Borrowed("com.apple.iTunes"),
        name: std::borrow::Cow::Borrowed("BPM"),
    };
    const LOWER_BPM_ATOM: AtomIdent<'static> = AtomIdent::Freeform {
        mean: std::borrow::Cow::Borrowed("com.apple.iTunes"),
        name: std::borrow::Cow::Borrowed("bpm"),
    };

    /// An MP4 with a UTF-16 `----:com.apple.iTunes:BPM` (`90`) followed by a
    /// UTF-8 `----:com.apple.iTunes:bpm` (`120`). lofty reads both names as
    /// the BPM and takes the first, 90. With `split`, it also has two
    /// separate `©ART` atoms (`A`, `B`).
    fn bpm_mp4_fixture(dir: &Path, split: bool) -> PathBuf {
        let path = dir.join("song.m4a");
        let mut ilst = Ilst::new();
        ilst.insert(text_atom(TITLE_ATOM, &["Old"]));
        ilst.insert(text_atom(ARTIST_ATOM, &["A"]));
        if split {
            ilst.insert(text_atom(AtomIdent::Fourcc(*b"\xa9ARx"), &["B"]));
        }
        ilst.insert(text_atom(UPPER_BPM_ATOM, &["WXYZ"]));
        ilst.insert(text_atom(LOWER_BPM_ATOM, &["120"]));
        ilst.insert(text_atom(DATE_ATOM, &["1997"]));
        let renames: &[(&[u8; 4], &[u8; 4])] = if split {
            &[(b"\xa9ARx", b"\xa9ART")]
        } else {
            &[]
        };
        write_split_atoms_mp4(&path, &ilst, renames);
        let (from, to) = utf16_data_atom("WXYZ", "90");
        let bytes = patched(&path, &from, &to);
        fs::write(&path, bytes).expect("write fixture");

        let written = read_ilst(&path);
        assert_eq!(
            atom_count(&written, &ARTIST_ATOM),
            if split { 2 } else { 1 }
        );
        assert_eq!(
            atom_values(&written, &UPPER_BPM_ATOM),
            [AtomData::UTF16("90".to_string())]
        );
        assert_eq!(
            Tag::from(written).get_string(ItemKey::Bpm),
            Some("90"),
            "lofty reads the first BPM atom"
        );
        path
    }

    /// With no repeated identifier, the UTF-16 atom is converted where it
    /// was, so it still comes before the other atom read as the BPM.
    #[test]
    fn mp4_saves_keep_utf16_atoms_in_place() {
        let dir = test_dir("mp4-utf16-order");
        let path = bpm_mp4_fixture(&dir, false);
        let before = read_ilst(&path);

        let mut row = load_one(&path);
        row.title = "New".to_string();
        assert_saved(&save_json(&[edited(&row, &["title"])]));

        let after = read_ilst(&path);
        let expected = atoms_except(&before, &[TITLE_ATOM])
            .into_iter()
            .map(|atom| {
                if atom.ident() == &UPPER_BPM_ATOM {
                    text_atom(UPPER_BPM_ATOM, &["90"])
                } else {
                    atom
                }
            })
            .collect::<Vec<_>>();
        assert_eq!(atoms_except(&after, &[TITLE_ATOM]), expected);
        assert_eq!(
            atom_values(&after, &TITLE_ATOM),
            [AtomData::UTF8("New".to_string())]
        );
        assert_eq!(Tag::from(after).get_string(ItemKey::Bpm), Some("90"));
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// With a repeated identifier, a UTF-16 atom can only be moved to the end,
    /// which would put it after the other atom read as the BPM and change the
    /// BPM read, so the save is refused.
    #[test]
    fn mp4_saves_refuse_moving_utf16_atoms_past_atoms_of_their_field() {
        let dir = test_dir("mp4-utf16-field");
        let path = bpm_mp4_fixture(&dir, true);
        let bytes = fs::read(&path).expect("read fixture");

        let mut row = load_one(&path);
        row.title = "New".to_string();
        let response = save_json(&[edited(&row, &["title"])]);

        assert!(response.successful_paths.is_empty());
        let error = response.results[0].error.as_deref().unwrap_or_default();
        assert!(
            error.contains("----:com.apple.iTunes:BPM atom holds UTF-16 text")
                && error.contains("same field as its ----:com.apple.iTunes:bpm atom"),
            "{error}"
        );
        assert_eq!(fs::read(&path).expect("read file"), bytes);
        assert_eq!(dir_entries(&dir), ["song.m4a"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    /// The Ogg CRC-32 of `bytes`.
    fn ogg_crc(bytes: &[u8]) -> u32 {
        bytes.iter().fold(0, |crc, &byte| {
            (0..8).fold(crc ^ (u32::from(byte) << 24), |crc, _| {
                if crc & 0x8000_0000 == 0 {
                    crc << 1
                } else {
                    (crc << 1) ^ 0x04c1_1db7
                }
            })
        })
    }

    /// Recompute the checksum of every Ogg page, after the pages were patched.
    fn fix_ogg_checksums(bytes: &mut [u8]) {
        let mut start = 0;
        while start < bytes.len() {
            assert_eq!(&bytes[start..start + 4], b"OggS");
            let segments = usize::from(bytes[start + 26]);
            let table = start + 27;
            let body = bytes[table..table + segments]
                .iter()
                .map(|&len| usize::from(len))
                .sum::<usize>();
            let end = table + segments + body;
            bytes[start + 22..start + 26].fill(0);
            let crc = ogg_crc(&bytes[start..end]);
            bytes[start + 22..start + 26].copy_from_slice(&crc.to_le_bytes());
            start = end;
        }
    }

    /// A comment with an empty name, which lofty reads and writes but will
    /// not push. It is written as a placeholder of the same length and
    /// patched.
    const EMPTY_NAME_PLACEHOLDER: &str = "Q=keep me";
    const EMPTY_NAME_COMMENT: &str = "==keep me";

    /// FLAC and Opus files holding [`EMPTY_NAME_COMMENT`] between two artists.
    fn empty_name_vorbis_fixtures(dir: &Path) -> Vec<PathBuf> {
        let items = [
            ("TITLE", "Old"),
            ("ARTIST", "A"),
            ("Q", "keep me"),
            ("artist", "B"),
            ("CUSTOM", "kept"),
        ];
        let flac = dir.join("song.flac");
        write_flac(&flac, &items);
        let opus = dir.join("song.opus");
        let mut comments = VorbisComments::new();
        comments.set_vendor("ferrous-test".to_string());
        for (key, value) in items {
            comments.push(key.to_string(), value.to_string());
        }
        write_test_opus(&opus, &comments);
        for path in [&flac, &opus] {
            let mut bytes = patched(
                path,
                EMPTY_NAME_PLACEHOLDER.as_bytes(),
                EMPTY_NAME_COMMENT.as_bytes(),
            );
            if path == &opus {
                fix_ogg_checksums(&mut bytes);
            }
            fs::write(path, bytes).expect("write fixture");
            let (comments, _) = read_vorbis(path);
            let mut expected = items.map(|(key, value)| (key.to_string(), value.to_string()));
            expected[2] = (String::new(), "=keep me".to_string());
            assert_eq!(vorbis_items(&comments), expected, "{path:?}");
        }
        vec![flac, opus]
    }

    /// Whether the file stores [`EMPTY_NAME_COMMENT`] unchanged: its length,
    /// then its bytes.
    fn stores_empty_name_comment(path: &Path) -> bool {
        let comment = EMPTY_NAME_COMMENT.as_bytes();
        let mut stored = u32::try_from(comment.len())
            .expect("comment length")
            .to_le_bytes()
            .to_vec();
        stored.extend_from_slice(comment);
        contains(&fs::read(path).expect("read file"), &stored)
    }

    #[test]
    fn title_only_vorbis_saves_keep_comments_with_an_empty_name() {
        let dir = test_dir("vorbis-empty-name-title");
        for path in empty_name_vorbis_fixtures(&dir) {
            let (before, _) = read_vorbis(&path);

            let mut row = load_one(&path);
            row.title = "New".to_string();
            assert_saved(&save_json(&[edited(&row, &["title"])]));

            let (after, _) = read_vorbis(&path);
            // Comments with a generic key follow the ones without.
            let expected = [
                ("", "=keep me"),
                ("CUSTOM", "kept"),
                ("TITLE", "New"),
                ("ARTIST", "A"),
                ("artist", "B"),
            ]
            .map(|(key, value)| (key.to_string(), value.to_string()));
            assert_eq!(vorbis_items(&after), expected, "{path:?}");
            assert_eq!(after.vendor(), before.vendor(), "{path:?}");
            assert!(stores_empty_name_comment(&path), "{path:?}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn edited_vorbis_fields_keep_comments_with_an_empty_name() {
        let dir = test_dir("vorbis-empty-name-edit");
        for path in empty_name_vorbis_fixtures(&dir) {
            let mut row = load_one(&path);
            row.artist = "C".to_string();
            assert_saved(&save_json(&[edited(&row, &["artist"])]));

            let (after, _) = read_vorbis(&path);
            let expected = [
                ("", "=keep me"),
                ("CUSTOM", "kept"),
                ("TITLE", "Old"),
                ("ARTIST", "C"),
            ]
            .map(|(key, value)| (key.to_string(), value.to_string()));
            assert_eq!(vorbis_items(&after), expected, "{path:?}");
            assert!(stores_empty_name_comment(&path), "{path:?}");
            assert_eq!(load_one(&path).artist, "C");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    fn ape_item(key: &str, value: ItemValue, read_only: bool) -> ApeItem {
        let mut item = ApeItem::new(key.to_string(), value).expect("APE item");
        item.read_only = read_only;
        item
    }

    fn ape_text(key: &str, value: &str, read_only: bool) -> ApeItem {
        ape_item(key, ItemValue::Text(value.to_string()), read_only)
    }

    /// Items with read-only flags, a multi-value artist, a track pair, an
    /// unknown key, and a cover.
    fn ape_fixture_items() -> Vec<ApeItem> {
        vec![
            ape_text("Title", "Old", false),
            ape_text("ARTIST", "A\0B", true),
            ape_text("Track", "1/9", false),
            ape_text("Year", "1997-05-12", false),
            ape_text("Custom", "kept", true),
            ape_item(
                "Cover Art (Front)",
                ItemValue::Binary(b"cover.jpg\0\xff\xd8\xff\xd9".to_vec()),
                false,
            ),
        ]
    }

    /// WavPack and Monkey's Audio files holding [`ape_fixture_items`].
    fn ape_fixtures(dir: &Path) -> Vec<PathBuf> {
        let mut tag = ApeTag::new();
        for item in ape_fixture_items() {
            tag.insert(item);
        }
        let wavpack = dir.join("song.wv");
        write_test_wavpack(&wavpack, &tag);
        let monkeys_audio = dir.join("song.ape");
        write_test_monkeys_audio(&monkeys_audio, &tag);
        for path in [&wavpack, &monkeys_audio] {
            assert_eq!(read_ape_items(path), ape_fixture_items(), "{path:?}");
        }
        vec![wavpack, monkeys_audio]
    }

    fn read_ape_items(path: &Path) -> Vec<ApeItem> {
        let mut file = fs::File::open(path).expect("open APE-tagged file");
        let options = ParseOptions::new();
        let tag = if path.extension().is_some_and(|extension| extension == "wv") {
            WavPackFile::read_from(&mut file, options)
                .expect("parse WavPack")
                .ape()
                .cloned()
        } else {
            ApeFile::read_from(&mut file, options)
                .expect("parse Monkey's Audio")
                .ape()
                .cloned()
        };
        tag.expect("APE tag").into_iter().collect()
    }

    #[test]
    fn title_only_ape_saves_keep_read_only_items() {
        let dir = test_dir("ape-title");
        for path in ape_fixtures(&dir) {
            let mut row = load_one(&path);
            assert_eq!(row.artist, "A");
            row.title = "New".to_string();
            assert_saved(&save_json(&[edited(&row, &["title"])]));

            let mut expected = ape_fixture_items();
            expected[0] = ape_text("Title", "New", false);
            assert_eq!(read_ape_items(&path), expected, "{path:?}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn ape_loads_report_the_values_after_the_first() {
        let dir = test_dir("ape-extra-values");
        for path in ape_fixtures(&dir) {
            let row = load_one(&path);
            assert_eq!(row.artist, "A", "{path:?}");
            let expected: BTreeMap<String, Vec<String>> =
                [("artist".to_string(), vec!["B".to_string()])].into();
            assert_eq!(row.extra_values, expected, "{path:?}");
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn edited_ape_items_keep_their_key_and_read_only_flag() {
        let dir = test_dir("ape-edit");
        for path in ape_fixtures(&dir) {
            let mut row = load_one(&path);
            row.artist = "C".to_string();
            row.track_no = "2".to_string();
            assert_saved(&save_json(&[edited(&row, &["artist", "trackNo"])]));

            let mut expected = ape_fixture_items();
            expected[1] = ape_text("ARTIST", "C", true);
            expected[2] = ape_text("Track", "2/9", false);
            assert_eq!(read_ape_items(&path), expected, "{path:?}");
            let reloaded = load_one(&path);
            assert_eq!(
                (reloaded.artist.as_str(), reloaded.track_no.as_str()),
                ("C", "02")
            );
        }
        fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn raw_surround_save_reports_tags_with_uncounted_items() {
        let dir = test_dir("raw-uncounted");
        let path = dir.join("song.dts");
        let mut bytes = b"stub".to_vec();
        let mut tag = crate::raw_audio::build_test_apev2_tag(
            &[("Title", "Old"), ("Composer", "Hidden")],
            true,
        );
        // Declare one item fewer than the tag holds.
        tag[16] = 1;
        let footer = tag.len() - 32;
        tag[footer + 16] = 1;
        bytes.append(&mut tag);
        fs::write(&path, &bytes).expect("write file");

        let mut row = load_one(&path);
        row.title = "New".to_string();
        let response = save_json(&[edited(&row, &["title"])]);

        assert!(response.successful_paths.is_empty());
        let error = response.results[0].error.as_deref().unwrap_or_default();
        assert!(error.contains("malformed"), "{error}");
        assert_eq!(fs::read(&path).expect("read file"), bytes);
        assert_eq!(dir_entries(&dir), ["song.dts"]);
        fs::remove_dir_all(dir).expect("remove test dir");
    }
}
