// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::{HashMap, HashSet};
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use lofty::config::ParsingMode;
use lofty::file::TaggedFileExt;
use lofty::prelude::Accessor;
use lofty::tag::items::Timestamp;
use lofty::tag::{ItemKey, Tag};
use serde::{Deserialize, Serialize};

use crate::library::{
    read_library_snapshot_from_db, rename_indexed_metadata_paths, LibraryRoot, LibrarySnapshot,
};
use crate::raw_audio::{
    edit_appended_apev2_text_items, format_ape_number_pair, is_raw_surround_file,
    read_appended_apev2_text_metadata, ApeTextField,
};
use crate::tag_write::save_tag;

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
    let tagged = lofty::read_from_path(path).map_err(|err| {
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
    }

    Ok(row)
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
        title: tagged.title.unwrap_or_default(),
        artist: tagged.artist.unwrap_or_default(),
        album: tagged.album.unwrap_or_default(),
        album_artist: tagged.album_artist.unwrap_or_default(),
        genre: tagged.genre.unwrap_or_default(),
        year: tagged
            .year
            .map(|value| value.to_string())
            .unwrap_or_default(),
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
        comment: tagged.comment.unwrap_or_default(),
    };
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
        Some(format!(
            "section|{}|{}|{}|{}",
            root_key, artist_name, album_folder, components[2]
        ))
    } else {
        None
    };

    Some(LibraryPathContext {
        album_key: Some(format!("album|{root_key}|{artist_name}|{album_folder}")),
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

fn parse_optional_u32(value: &str) -> Result<Option<u32>, String> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return Ok(None);
    }
    trimmed
        .parse::<u32>()
        .map(Some)
        .map_err(|err| format!("invalid numeric value '{trimmed}': {err}"))
}

/// Check an edited Year value: a year or an ISO 8601 date such as
/// `1997-05-12`. Returns the trimmed text to store; empty clears the date.
///
/// Only text that lofty writes back unchanged is accepted. Its `ID3v2.3`
/// writer drops recording dates it cannot parse.
fn validated_year(value: &str) -> Result<String, String> {
    let trimmed = value.trim();
    let round_trips = || {
        trimmed.len() <= Timestamp::MAX_LENGTH
            && Timestamp::parse(&mut trimmed.as_bytes(), ParsingMode::Strict)
                .ok()
                .flatten()
                .is_some_and(|timestamp| timestamp.to_string() == trimmed)
    };
    if trimmed.is_empty() || round_trips() {
        Ok(trimmed.to_string())
    } else {
        Err(format!(
            "invalid year '{trimmed}': use a year or a date such as 1997-05-12"
        ))
    }
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

fn is_case_only_rename(current: &Path, target: &Path) -> bool {
    if current == target {
        return false;
    }
    current.to_string_lossy().to_lowercase() == target.to_string_lossy().to_lowercase()
}

fn set_text_key(tag: &mut Tag, key: ItemKey, value: &str) {
    tag.remove_key(key);
    let trimmed = value.trim();
    if !trimmed.is_empty() {
        tag.insert_text(key, trimmed.to_string());
    }
}

/// Write the changed fields of `row` into `tag`, leaving every other item alone.
fn apply_changed_fields(
    tag: &mut Tag,
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
) -> Result<(), String> {
    let year = changed
        .contains(&TagField::Year)
        .then(|| validated_year(&row.year))
        .transpose()?;
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
    let mut tagged = lofty::read_from_path(path).map_err(|err| {
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
        let mut seeded = tagged
            .primary_tag()
            .or_else(|| tagged.first_tag())
            .cloned()
            .unwrap_or_else(|| Tag::new(writable_type));
        seeded.re_map(writable_type);
        tagged.insert_tag(seeded);
    }
    let Some(tag) = tagged.tag_mut(writable_type) else {
        return Err(format!(
            "failed to create writable tag for {}",
            path.to_string_lossy()
        ));
    };
    apply_changed_fields(tag, row, changed)?;
    save_tag(path, file_type, tag)
}

fn apply_raw_row(
    path: &Path,
    row: &TagEditorRow,
    changed: &HashSet<TagField>,
) -> Result<(), String> {
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
    if changed.contains(&TagField::Year) {
        edits.push((ApeTextField::Year, Some(validated_year(&row.year)?)));
    }
    // APEv2 stores a number and its total in one item.
    if changed.contains(&TagField::TrackNo) || changed.contains(&TagField::TotalTracks) {
        edits.push((
            ApeTextField::Track,
            format_ape_number_pair(
                parse_optional_u32(&row.track_no)?,
                parse_optional_u32(&row.total_tracks)?,
            ),
        ));
    }
    if changed.contains(&TagField::DiscNo) || changed.contains(&TagField::TotalDiscs) {
        edits.push((
            ApeTextField::Disc,
            format_ape_number_pair(
                parse_optional_u32(&row.disc_no)?,
                parse_optional_u32(&row.total_discs)?,
            ),
        ));
    }
    if edits.is_empty() {
        return Ok(());
    }
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
        let loaded = if is_raw_surround_file(path) {
            load_raw_row(path)
        } else {
            load_standard_row(path)
        };
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
        let result = if is_raw_surround_file(&path) {
            apply_raw_row(&path, &row, &changed)
        } else {
            apply_standard_row_to_tag(&path, &row, &changed)
        };
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

// Sequential file-rename pipeline — splitting would fragment the transactional logic.
#[allow(clippy::too_many_lines)]
pub(crate) fn rename_rows(request: RenameRequest) -> RenameResponse {
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
        if target_path.exists()
            && !path_matches_any_source(&source_paths, &target_path)
            && !is_case_only_rename(&current_path, &target_path)
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

    if let Err(error) = rename_indexed_metadata_paths(&renames) {
        for (old_path, new_path) in renamed_pairs {
            results.push(RenameResultRow {
                path: old_path,
                new_path: Some(new_path),
                ok: false,
                error: Some(error.clone()),
            });
        }
        return RenameResponse {
            results,
            successful_paths: Vec::new(),
            error: None,
        };
    }

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

    RenameResponse {
        results,
        successful_paths,
        error: None,
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use lofty::config::{ParseOptions, WriteOptions};
    use lofty::file::AudioFile;
    use lofty::flac::FlacFile;
    use lofty::tag::{TagExt, TagType};
    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;
    use std::time::{SystemTime, UNIX_EPOCH};

    use crate::tag_write::{write_test_flac as write_flac, write_test_mp3_with_id3v23, TEST_MP3};

    fn test_dir(name: &str) -> PathBuf {
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

    fn dir_entries(dir: &Path) -> Vec<String> {
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

    fn load_one(path: &Path) -> TagEditorRow {
        let response = load_rows_for_paths(&[path.to_path_buf()]);
        assert!(response.failures.is_empty(), "{:?}", response.failures);
        response.rows.into_iter().next().expect("loaded row")
    }

    fn save_json(rows: &[serde_json::Value]) -> SaveResponse {
        let blob = serde_json::to_vec(&serde_json::json!({ "rows": rows })).expect("encode");
        save_rows(parse_save_request(&blob).expect("parse save request"))
    }

    fn edited(row: &TagEditorRow, changed: &[&str]) -> serde_json::Value {
        let mut value = serde_json::to_value(row).expect("encode row");
        value["changedFields"] = serde_json::json!(changed);
        value
    }

    fn assert_saved(response: &SaveResponse) {
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
        fs::write(&path, TEST_MP3).expect("write MP3 fixture");
        TagType::Id3v2
            .remove_from_path(&path)
            .expect("strip ID3v2 tag");
        let mut id3v1 = Tag::new(TagType::Id3v1);
        id3v1.insert_text(ItemKey::TrackTitle, "Old".to_string());
        id3v1.insert_text(ItemKey::TrackArtist, "Artist".to_string());
        id3v1.insert_text(ItemKey::AlbumTitle, "Album".to_string());
        id3v1
            .save_to_path(&path, WriteOptions::new())
            .expect("write ID3v1 tag");

        let mut row = load_one(&path);
        assert_eq!(row.artist, "Artist");
        row.title = "New".to_string();
        assert_saved(&save_json(&[edited(&row, &["title"])]));

        let reloaded = load_one(&path);
        assert_eq!(reloaded.title, "New");
        assert_eq!(reloaded.artist, "Artist");
        assert_eq!(reloaded.album, "Album");
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

    #[test]
    fn year_validation_accepts_years_and_dates() {
        for value in [
            "",
            "1997",
            " 1997 ",
            "1997-05",
            "1997-05-12",
            "1997-05-12T10:30:00",
        ] {
            assert_eq!(
                validated_year(value),
                Ok(value.trim().to_string()),
                "{value}"
            );
        }
        for value in [
            "May 1997",
            "1997/05/12",
            "12.05.1997",
            "97",
            "1997-5-12",
            "1997-05-12x",
        ] {
            assert!(validated_year(value).is_err(), "{value}");
        }
    }
}
