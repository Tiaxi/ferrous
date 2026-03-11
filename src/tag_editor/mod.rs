use std::collections::{HashMap, HashSet};
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use lofty::config::WriteOptions;
use lofty::file::TaggedFileExt;
use lofty::prelude::Accessor;
use lofty::tag::{ItemKey, Tag, TagExt};
use serde::{Deserialize, Serialize};

use crate::library::{
    read_library_snapshot_from_db, rename_indexed_metadata_paths, LibraryRoot, LibrarySnapshot,
};
use crate::raw_audio::{
    is_raw_surround_file, read_appended_apev2_text_metadata, write_appended_apev2_text_metadata,
    RawAudioTagMetadata,
};

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

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct SaveRequest {
    rows: Vec<TagEditorRow>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
enum SaveRequestEnvelope {
    Request(SaveRequest),
    Rows(Vec<TagEditorRow>),
}

impl SaveRequestEnvelope {
    fn into_rows(self) -> Vec<TagEditorRow> {
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
pub(crate) struct LoadResponse {
    rows: Vec<TagEditorRow>,
    resolved_paths: Vec<String>,
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

fn normalize_number_text(value: Option<&str>) -> String {
    value.unwrap_or_default().trim().to_string()
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

fn load_raw_row(path: &Path) -> TagEditorRow {
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
    row
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

fn parse_optional_i32(value: &str) -> Result<Option<i32>, String> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return Ok(None);
    }
    trimmed
        .parse::<i32>()
        .map(Some)
        .map_err(|err| format!("invalid year '{trimmed}': {err}"))
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

fn apply_standard_row_to_tag(path: &Path, row: &TagEditorRow) -> Result<(), String> {
    let mut tagged = lofty::read_from_path(path).map_err(|err| {
        format!(
            "failed to read tag data for {}: {err}",
            path.to_string_lossy()
        )
    })?;
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

    if tagged.tag_mut(writable_type).is_none() {
        tagged.insert_tag(Tag::new(writable_type));
    }
    let Some(tag) = tagged.tag_mut(writable_type) else {
        return Err(format!(
            "failed to create writable tag for {}",
            path.to_string_lossy()
        ));
    };

    if row.title.trim().is_empty() {
        tag.remove_title();
    } else {
        tag.set_title(row.title.trim().to_string());
    }
    if row.artist.trim().is_empty() {
        tag.remove_artist();
    } else {
        tag.set_artist(row.artist.trim().to_string());
    }
    if row.album.trim().is_empty() {
        tag.remove_album();
    } else {
        tag.set_album(row.album.trim().to_string());
    }
    if row.genre.trim().is_empty() {
        tag.remove_genre();
    } else {
        tag.set_genre(row.genre.trim().to_string());
    }
    if row.comment.trim().is_empty() {
        tag.remove_comment();
    } else {
        tag.set_comment(row.comment.trim().to_string());
    }

    set_text_key(tag, ItemKey::AlbumArtist, &row.album_artist);
    tag.remove_key(ItemKey::RecordingDate);
    tag.remove_key(ItemKey::OriginalReleaseDate);
    if !row.year.trim().is_empty() {
        let year =
            parse_optional_i32(&row.year)?.ok_or_else(|| "missing year value".to_string())?;
        tag.insert_text(ItemKey::RecordingDate, year.to_string());
    }

    tag.remove_track();
    tag.remove_track_total();
    tag.remove_disk();
    tag.remove_disk_total();
    set_text_key(
        tag,
        ItemKey::TrackNumber,
        &normalize_number_text(Some(&row.track_no)),
    );
    set_text_key(
        tag,
        ItemKey::TrackTotal,
        &normalize_number_text(Some(&row.total_tracks)),
    );
    set_text_key(
        tag,
        ItemKey::DiscNumber,
        &normalize_number_text(Some(&row.disc_no)),
    );
    set_text_key(
        tag,
        ItemKey::DiscTotal,
        &normalize_number_text(Some(&row.total_discs)),
    );

    tag.clone()
        .save_to_path(path, WriteOptions::default())
        .map_err(|err| format!("failed to save tags for {}: {err}", path.to_string_lossy()))
}

fn apply_raw_row(path: &Path, row: &TagEditorRow) -> Result<(), String> {
    let metadata = RawAudioTagMetadata {
        title: (!row.title.trim().is_empty()).then(|| row.title.trim().to_string()),
        artist: (!row.artist.trim().is_empty()).then(|| row.artist.trim().to_string()),
        album: (!row.album.trim().is_empty()).then(|| row.album.trim().to_string()),
        album_artist: (!row.album_artist.trim().is_empty())
            .then(|| row.album_artist.trim().to_string()),
        genre: (!row.genre.trim().is_empty()).then(|| row.genre.trim().to_string()),
        year: parse_optional_i32(&row.year)?,
        track_no: parse_optional_u32(&row.track_no)?,
        track_total: parse_optional_u32(&row.total_tracks)?,
        disc_no: parse_optional_u32(&row.disc_no)?,
        disc_total: parse_optional_u32(&row.total_discs)?,
        comment: (!row.comment.trim().is_empty()).then(|| row.comment.trim().to_string()),
    };
    write_appended_apev2_text_metadata(path, &metadata)
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
    for path in paths {
        let loaded = if is_raw_surround_file(path) {
            Ok(load_raw_row(path))
        } else {
            load_standard_row(path)
        };
        match loaded {
            Ok(row) => rows.push(row),
            Err(error) => {
                return LoadResponse {
                    rows,
                    resolved_paths: paths
                        .iter()
                        .map(|path| path.to_string_lossy().into_owned())
                        .collect(),
                    error: Some(error),
                };
            }
        }
    }
    LoadResponse {
        rows,
        resolved_paths: paths
            .iter()
            .map(|path| path.to_string_lossy().into_owned())
            .collect(),
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
    for row in request.rows {
        let path = PathBuf::from(&row.path);
        let result = if is_raw_surround_file(&path) {
            apply_raw_row(&path, &row)
        } else {
            apply_standard_row_to_tag(&path, &row)
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
