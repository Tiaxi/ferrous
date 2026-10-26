// SPDX-License-Identifier: GPL-3.0-or-later

use std::borrow::Cow;
use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::hash::BuildHasher;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::collation::CollationKey;
use crate::folder_cover::FolderImages;
use crate::library::{LibraryRoot, LibrarySnapshot, LibraryTrack};

use super::ffi::push_string;
use super::LibrarySortMode;

const ROW_TYPE_ROOT: u8 = 0;
const ROW_TYPE_ARTIST: u8 = 1;
const ROW_TYPE_ALBUM: u8 = 2;
const ROW_TYPE_SECTION: u8 = 3;
const ROW_TYPE_TRACK: u8 = 4;

#[derive(Debug, Clone)]
struct TrackLeaf {
    path: PathBuf,
    title: String,
    file_stem: String,
    album_tag: String,
    cover_path: String,
    cover_stamp: u64,
    year: Option<i32>,
    track_no: Option<u32>,
}

#[derive(Debug, Clone)]
struct AlbumNodeBuilder {
    folder_name: String,
    folder_path: PathBuf,
    root_tracks: Vec<TrackLeaf>,
    sections: BTreeMap<String, Vec<TrackLeaf>>,
}

#[derive(Debug, Clone)]
struct ArtistNodeBuilder {
    artist_name: String,
    sort_key: CollationKey,
    artist_path: PathBuf,
    loose_tracks: Vec<TrackLeaf>,
    albums: BTreeMap<String, AlbumNodeBuilder>,
}

#[derive(Debug, Clone)]
struct RootNodeBuilder {
    root_path: PathBuf,
    root_name: String,
    sort_key: CollationKey,
    artists: BTreeMap<String, ArtistNodeBuilder>,
}

#[derive(Debug, Clone)]
struct OrderedTrack {
    label: String,
    path: String,
    number: u32,
}

#[derive(Debug, Clone)]
struct ResolvedAlbum {
    title: String,
    title_key: CollationKey,
    year: Option<i32>,
    folder_name: String,
    folder_path: PathBuf,
    cover_path: Option<String>,
    /// See [`crate::library::LibraryTrack::cover_stamp`].
    cover_stamp: u64,
    root_tracks: Vec<OrderedTrack>,
    sections: Vec<ResolvedSection>,
}

#[derive(Debug, Clone)]
struct ResolvedSection {
    name: String,
    name_key: CollationKey,
    path: PathBuf,
    cover_path: Option<String>,
    cover_stamp: u64,
    year: Option<i32>,
    tracks: Vec<OrderedTrack>,
}

#[derive(Debug, Clone)]
struct FlatTreeRow {
    row_type: u8,
    depth: u16,
    source_index: i32,
    track_number: u32,
    child_count: u32,
    title: String,
    key: String,
    artist: String,
    path: String,
    cover_path: String,
    cover_stamp: u64,
    track_path: String,
    play_paths: Vec<String>,
}

impl FlatTreeRow {
    fn root(depth: u16, path: &str, title: &str, child_count: usize) -> Self {
        Self {
            row_type: ROW_TYPE_ROOT,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u32(child_count),
            title: title.to_string(),
            key: root_row_key(path),
            artist: String::new(),
            path: path.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            track_path: String::new(),
            play_paths: Vec::new(),
        }
    }

    fn artist(
        depth: u16,
        key: String,
        artist_name: &str,
        path: &str,
        child_count: usize,
        album_count: usize,
    ) -> Self {
        Self {
            row_type: ROW_TYPE_ARTIST,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u32(child_count),
            title: format!("{artist_name} ({album_count})"),
            key,
            artist: artist_name.to_string(),
            path: path.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            track_path: String::new(),
            play_paths: Vec::new(),
        }
    }

    fn album(
        depth: u16,
        key: String,
        artist_name: &str,
        album_title: &str,
        album_path: &str,
        album: &ResolvedAlbum,
        play_paths: Vec<String>,
    ) -> Self {
        Self {
            row_type: ROW_TYPE_ALBUM,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u32(album.root_tracks.len() + album.sections.len()),
            title: album_title.to_string(),
            key,
            artist: artist_name.to_string(),
            path: album_path.to_string(),
            cover_path: album.cover_path.as_deref().unwrap_or_default().to_string(),
            cover_stamp: album.cover_stamp,
            track_path: String::new(),
            play_paths,
        }
    }

    fn section(
        depth: u16,
        key: String,
        title: &str,
        path: &str,
        (cover_path, cover_stamp): (&str, u64),
        play_paths: Vec<String>,
        child_count: usize,
    ) -> Self {
        Self {
            row_type: ROW_TYPE_SECTION,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u32(child_count),
            title: title.to_string(),
            key,
            artist: String::new(),
            path: path.to_string(),
            cover_path: cover_path.to_string(),
            cover_stamp,
            track_path: String::new(),
            play_paths,
        }
    }

    fn track(depth: u16, artist_name: &str, title: &str, path: &str, track_number: u32) -> Self {
        Self {
            row_type: ROW_TYPE_TRACK,
            depth,
            source_index: -1,
            track_number,
            child_count: 0,
            title: title.to_string(),
            key: track_row_key(path),
            artist: artist_name.to_string(),
            path: path.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            track_path: path.to_string(),
            play_paths: vec![path.to_string()],
        }
    }
}

struct AlbumRowConfig<'a, S> {
    artist_name: &'a str,
    root_path: &'a str,
    lazy_hydration: bool,
    expanded_keys: Option<&'a HashSet<String, S>>,
    album_depth: u16,
    section_depth: u16,
    section_track_depth: u16,
}

fn root_row_key(root_path: &str) -> String {
    tree_key("root", &[root_path])
}

pub(crate) fn artist_row_key(root_path: &str, artist_name: &str) -> String {
    tree_key("artist", &[root_path, artist_name])
}

pub(crate) fn album_row_key(root_path: &str, artist_name: &str, folder_name: &str) -> String {
    tree_key("album", &[root_path, artist_name, folder_name])
}

pub(crate) fn section_row_key(
    root_path: &str,
    artist_name: &str,
    folder_name: &str,
    section_name: &str,
) -> String {
    tree_key(
        "section",
        &[root_path, artist_name, folder_name, section_name],
    )
}

pub(super) fn track_row_key(path: &str) -> String {
    tree_key("track", &[path])
}

/// The key of a tree row: `kind|part|…|part`. Every part but the last has
/// its `\` and `|` escaped with a `\`. Each kind has a fixed number of
/// parts, so a name with a `|` in it cannot pass for two parts, and names
/// without either character keep the keys they had before parts were
/// escaped.
fn tree_key(kind: &str, parts: &[&str]) -> String {
    let mut key =
        String::with_capacity(kind.len() + parts.iter().map(|part| part.len() + 1).sum::<usize>());
    key.push_str(kind);
    for (index, part) in parts.iter().enumerate() {
        key.push('|');
        if index + 1 == parts.len() {
            key.push_str(part);
        } else {
            for ch in part.chars() {
                if matches!(ch, '\\' | '|') {
                    key.push('\\');
                }
                key.push(ch);
            }
        }
    }
    key
}

/// Whether `key` is the [`tree_key`] of `kind` and `parts`.
pub(super) fn tree_key_is(key: &str, kind: &str, parts: &[&str]) -> bool {
    let Some(mut rest) = key.strip_prefix(kind) else {
        return false;
    };
    for (index, part) in parts.iter().enumerate() {
        let Some(next) = rest.strip_prefix('|') else {
            return false;
        };
        rest = next;
        if index + 1 == parts.len() {
            return rest == *part;
        }
        for ch in part.chars() {
            if matches!(ch, '\\' | '|') {
                let Some(next) = rest.strip_prefix('\\') else {
                    return false;
                };
                rest = next;
            }
            let Some(next) = rest.strip_prefix(ch) else {
                return false;
            };
            rest = next;
        }
    }
    rest.is_empty()
}

/// The key the tree gave a row before [`tree_key`] escaped its parts, if
/// it differs from the escaped key.
fn legacy_tree_key(kind: &str, parts: &[&str]) -> Option<String> {
    let (_, escaped) = parts.split_last()?;
    if !escaped.iter().any(|part| part.contains(['\\', '|'])) {
        return None;
    }
    Some(
        std::iter::once(kind)
            .chain(parts.iter().copied())
            .collect::<Vec<_>>()
            .join("|"),
    )
}

/// The keys of the rows of a library, and the keys from before parts were
/// escaped (see [`tree_key`]) that name one of them alone.
#[derive(Default)]
struct TreeKeys {
    valid: HashSet<String>,
    /// Each legacy key, and the key it is now; `None` if it was the key of
    /// more than one row.
    legacy: HashMap<String, Option<String>>,
}

impl TreeKeys {
    fn insert(&mut self, kind: &str, parts: &[&str]) {
        let key = tree_key(kind, parts);
        if let Some(legacy) = legacy_tree_key(kind, parts) {
            self.legacy
                .entry(legacy)
                .and_modify(|current| {
                    if current.as_ref() != Some(&key) {
                        *current = None;
                    }
                })
                .or_insert_with(|| Some(key.clone()));
        }
        self.valid.insert(key);
    }

    /// Keeps the keys of `keys` that name a row, and replaces each legacy
    /// key with the key its row has now.
    fn retain<S: BuildHasher>(&self, keys: &mut HashSet<String, S>) {
        let mut migrated = Vec::new();
        keys.retain(|key| {
            if self.valid.contains(key) {
                return true;
            }
            if let Some(Some(current)) = self.legacy.get(key) {
                migrated.push(current.clone());
            }
            false
        });
        keys.extend(migrated);
    }
}

#[must_use]
pub fn build_library_tree_flat_binary<S: BuildHasher>(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
    expanded_keys: Option<&HashSet<String, S>>,
) -> Vec<u8> {
    let rows = build_library_tree_flat_rows(library, sort_mode, expanded_keys);
    encode_flat_rows(&rows)
}

pub fn retain_valid_expanded_keys<S: BuildHasher + Default>(
    library: &LibrarySnapshot,
    expanded_keys: &mut HashSet<String, S>,
) {
    if expanded_keys.is_empty() || library.roots.is_empty() {
        return;
    }
    tree_keys(&build_root_builders(&library.roots, &library.tracks)).retain(expanded_keys);
}

fn tree_keys(builders: &BTreeMap<String, RootNodeBuilder>) -> TreeKeys {
    let mut keys = TreeKeys::default();
    for root in builders.values() {
        let root_path = root.root_path.to_string_lossy();
        keys.insert("root", &[&root_path]);
        for artist in root.artists.values() {
            let artist_name = artist.artist_name.as_str();
            keys.insert("artist", &[&root_path, artist_name]);
            for album in artist.albums.values() {
                let folder = album.folder_name.as_str();
                keys.insert("album", &[&root_path, artist_name, folder]);
                for section in album.sections.keys() {
                    keys.insert("section", &[&root_path, artist_name, folder, section]);
                }
            }
        }
    }
    keys
}

/// Reusable hierarchy and album presentation data for one immutable library revision.
/// Build and use on the tree worker because album cover fallbacks access the filesystem.
pub struct LibraryTreeCache {
    builders: BTreeMap<String, RootNodeBuilder>,
    resolved_albums: HashMap<String, Vec<ResolvedAlbum>>,
    keys: TreeKeys,
    pub(crate) counts: (usize, usize),
    /// The tracks each root holds, which removing the root takes out of
    /// the library.
    pub(crate) root_track_counts: Arc<HashMap<PathBuf, usize>>,
}

impl LibraryTreeCache {
    #[must_use]
    pub fn new(library: &LibrarySnapshot) -> Self {
        let builders = build_root_builders(&library.roots, &library.tracks);
        let keys = tree_keys(&builders);
        // The artists and albums are the tree's, so they are counted here,
        // once per library.
        let counts = builders.values().fold((0, 0), |(artists, albums), root| {
            (
                artists + root.artists.len(),
                albums
                    + root
                        .artists
                        .values()
                        .map(|artist| artist.albums.len())
                        .sum::<usize>(),
            )
        });
        let mut root_track_counts = HashMap::new();
        for track in &library.tracks {
            *root_track_counts
                .entry(track.root_path.clone())
                .or_insert(0) += 1;
        }
        Self {
            builders,
            resolved_albums: HashMap::new(),
            keys,
            counts,
            root_track_counts: Arc::new(root_track_counts),
        }
    }

    pub(crate) fn retain_expanded_keys(&self, keys: &mut HashSet<String>) {
        // Initial library loading must not erase the restored view state.
        if !self.builders.is_empty() {
            self.keys.retain(keys);
        }
    }

    #[must_use]
    pub fn build<S: BuildHasher>(
        &mut self,
        sort_mode: LibrarySortMode,
        expanded_keys: Option<&HashSet<String, S>>,
    ) -> Vec<u8> {
        encode_flat_rows(&self.rows(sort_mode, expanded_keys))
    }

    fn rows<S: BuildHasher>(
        &mut self,
        sort_mode: LibrarySortMode,
        expanded_keys: Option<&HashSet<String, S>>,
    ) -> Vec<FlatTreeRow> {
        let multi_root = self.builders.len() >= 2;
        let mut rows = Vec::new();
        for root_builder in sorted_roots(&self.builders) {
            let root_depth = if multi_root { 0 } else { u16::MAX };
            let artist_depth = u16::from(multi_root);
            let root_path = root_builder.root_path.to_string_lossy().to_string();
            let artist_rows = build_artist_rows_flat(
                root_builder,
                &root_path,
                sort_mode,
                artist_depth,
                expanded_keys,
                &mut self.resolved_albums,
            );
            if multi_root {
                rows.push(FlatTreeRow::root(
                    root_depth,
                    &root_path,
                    &root_builder.root_name,
                    root_builder.artists.len(),
                ));
            }
            rows.extend(artist_rows);
        }
        rows
    }
}

fn build_library_tree_flat_rows<S: BuildHasher>(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
    expanded_keys: Option<&HashSet<String, S>>,
) -> Vec<FlatTreeRow> {
    LibraryTreeCache::new(library).rows(sort_mode, expanded_keys)
}

fn build_root_builders(
    roots: &[LibraryRoot],
    tracks: &[Arc<LibraryTrack>],
) -> BTreeMap<String, RootNodeBuilder> {
    let mut builders: BTreeMap<String, RootNodeBuilder> = roots
        .iter()
        .map(|root| {
            (
                root.path.to_string_lossy().to_string(),
                RootNodeBuilder {
                    root_path: root.path.clone(),
                    root_name: root.display_name(),
                    sort_key: CollationKey::new(&root.display_name()),
                    artists: BTreeMap::new(),
                },
            )
        })
        .collect();

    for track in tracks {
        let Some(root) = pick_root_for_track(roots, track) else {
            continue;
        };
        let root_key = root.path.to_string_lossy().to_string();
        let Some(root_builder) = builders.get_mut(&root_key) else {
            continue;
        };
        insert_track_leaf(root_builder, root, track);
    }

    builders
}

fn insert_track_leaf(root_builder: &mut RootNodeBuilder, root: &LibraryRoot, track: &LibraryTrack) {
    let Ok(rel) = track.path.strip_prefix(&root.path) else {
        return;
    };
    let components = rel
        .components()
        .filter_map(|component| {
            let std::path::Component::Normal(name) = component else {
                return None;
            };
            Some(name.to_string_lossy().to_string())
        })
        .collect::<Vec<_>>();
    if components.is_empty() {
        return;
    }

    let leaf = TrackLeaf {
        path: track.path.clone(),
        title: normalized_track_title(track),
        file_stem: track
            .path
            .file_stem()
            .map_or_else(String::new, |name| name.to_string_lossy().into_owned()),
        album_tag: track.album.trim().to_string(),
        cover_path: track.cover_path.clone(),
        cover_stamp: track.cover_stamp,
        year: track.year,
        track_no: track.track_no,
    };
    let (artist_name, artist_path) = artist_parts(root, track, &components);
    let artist_entry = root_builder
        .artists
        .entry(artist_name.clone())
        .or_insert_with(|| ArtistNodeBuilder {
            sort_key: CollationKey::new(&artist_name),
            artist_name: artist_name.clone(),
            artist_path,
            loose_tracks: Vec::new(),
            albums: BTreeMap::new(),
        });

    if components.len() <= 2 {
        artist_entry.loose_tracks.push(leaf);
        return;
    }

    let album_folder = components[1].clone();
    let album_path = root.path.join(&artist_name).join(&album_folder);
    let album_entry = artist_entry
        .albums
        .entry(album_folder.clone())
        .or_insert_with(|| AlbumNodeBuilder {
            folder_name: album_folder.clone(),
            folder_path: album_path,
            root_tracks: Vec::new(),
            sections: BTreeMap::new(),
        });

    if components.len() >= 4 {
        album_entry
            .sections
            .entry(components[2].clone())
            .or_default()
            .push(leaf);
    } else {
        album_entry.root_tracks.push(leaf);
    }
}

fn artist_parts(
    root: &LibraryRoot,
    track: &LibraryTrack,
    components: &[String],
) -> (String, PathBuf) {
    if components.len() >= 2 {
        return (components[0].clone(), root.path.join(&components[0]));
    }
    let fallback = if track.artist.trim().is_empty() {
        String::from("Unknown Artist")
    } else {
        track.artist.trim().to_string()
    };
    (fallback, root.path.clone())
}

fn build_artist_rows_flat<S: BuildHasher>(
    root: &RootNodeBuilder,
    root_path: &str,
    sort_mode: LibrarySortMode,
    artist_depth: u16,
    expanded_keys: Option<&HashSet<String, S>>,
    resolved_cache: &mut HashMap<String, Vec<ResolvedAlbum>>,
) -> Vec<FlatTreeRow> {
    let lazy_hydration = expanded_keys.is_some();
    let mut artists = root.artists.values().collect::<Vec<_>>();
    sort_artists(&mut artists);

    let mut out = Vec::new();
    for artist in artists {
        let album_count = artist.albums.len();
        let artist_key = artist_row_key(root_path, &artist.artist_name);
        let artist_expanded = expanded_keys.is_none_or(|keys| keys.contains(&artist_key));
        let loose_tracks = if artist_expanded {
            order_tracks(&artist.loose_tracks)
        } else {
            Vec::new()
        };

        let mut resolved_albums = Vec::new();
        if artist_expanded {
            resolved_albums.clone_from(
                resolved_cache
                    .entry(artist_key.clone())
                    .or_insert_with(|| artist.albums.values().map(resolve_album).collect()),
            );
            sort_resolved_albums(&mut resolved_albums, sort_mode);
        }

        let artist_path = artist.artist_path.to_string_lossy().to_string();
        let child_count = artist.loose_tracks.len() + artist.albums.len();
        out.push(FlatTreeRow::artist(
            artist_depth,
            artist_key,
            &artist.artist_name,
            &artist_path,
            child_count,
            album_count,
        ));

        if lazy_hydration && !artist_expanded {
            continue;
        }

        let track_depth = artist_depth.saturating_add(1);
        let album_depth = artist_depth.saturating_add(1);
        let section_depth = artist_depth.saturating_add(2);
        let section_track_depth = artist_depth.saturating_add(3);

        for track in &loose_tracks {
            out.push(FlatTreeRow::track(
                track_depth,
                &artist.artist_name,
                &track.label,
                &track.path,
                track.number,
            ));
        }

        append_album_rows(
            &mut out,
            resolved_albums,
            &AlbumRowConfig {
                artist_name: &artist.artist_name,
                root_path,
                lazy_hydration,
                expanded_keys,
                album_depth,
                section_depth,
                section_track_depth,
            },
        );
    }

    out
}

fn append_album_rows<S: BuildHasher>(
    out: &mut Vec<FlatTreeRow>,
    resolved_albums: Vec<ResolvedAlbum>,
    config: &AlbumRowConfig<'_, S>,
) {
    for album in resolved_albums {
        let album_path = album.folder_path.to_string_lossy().to_string();
        let album_title = if let Some(year) = album.year {
            format!("{} ({year})", album.title)
        } else {
            album.title.clone()
        };
        let album_key = album_row_key(config.root_path, config.artist_name, &album.folder_name);
        let album_play_paths = album_play_paths(&album);
        out.push(FlatTreeRow::album(
            config.album_depth,
            album_key.clone(),
            config.artist_name,
            &album_title,
            &album_path,
            &album,
            album_play_paths,
        ));

        let album_expanded = config
            .expanded_keys
            .is_none_or(|keys| keys.contains(&album_key));
        if config.lazy_hydration && !album_expanded {
            continue;
        }
        append_album_tracks(
            out,
            config.artist_name,
            &album,
            config.root_path,
            config.section_depth,
            config.section_track_depth,
        );
    }
}

fn album_play_paths(album: &ResolvedAlbum) -> Vec<String> {
    let mut play_paths =
        Vec::with_capacity(album.root_tracks.len() + album.sections.len().saturating_mul(8));
    for track in &album.root_tracks {
        play_paths.push(track.path.clone());
    }
    // Only include recognised disc sections (Disc 1, CD 2, …) in the
    // album's play paths.  Bonus/extra subfolders are excluded so
    // double-clicking an album queues just the main album content.
    for section in &album.sections {
        if super::is_main_album_disc_section(&section.name) {
            for track in &section.tracks {
                play_paths.push(track.path.clone());
            }
        }
    }
    play_paths
}

fn append_album_tracks(
    out: &mut Vec<FlatTreeRow>,
    artist_name: &str,
    album: &ResolvedAlbum,
    root_path: &str,
    section_depth: u16,
    section_track_depth: u16,
) {
    for track in &album.root_tracks {
        out.push(FlatTreeRow::track(
            section_depth,
            artist_name,
            &track.label,
            &track.path,
            track.number,
        ));
    }

    for section in &album.sections {
        let section_path = section.path.to_string_lossy().to_string();
        let section_play_paths = section
            .tracks
            .iter()
            .map(|track| track.path.clone())
            .collect::<Vec<_>>();
        let section_title = if let Some(year) = section.year {
            format!("{} ({year})", section.name)
        } else {
            section.name.clone()
        };
        let section_cover = section.cover_path.as_deref().unwrap_or_default();
        out.push(FlatTreeRow::section(
            section_depth,
            section_row_key(root_path, artist_name, &album.folder_name, &section.name),
            &section_title,
            &section_path,
            (section_cover, section.cover_stamp),
            section_play_paths,
            section.tracks.len(),
        ));
        for track in &section.tracks {
            out.push(FlatTreeRow::track(
                section_track_depth,
                artist_name,
                &track.label,
                &track.path,
                track.number,
            ));
        }
    }
}

fn encode_flat_rows(rows: &[FlatTreeRow]) -> Vec<u8> {
    let mut out = Vec::with_capacity(rows.len() * 96 + 4);
    push_u32(&mut out, u32::try_from(rows.len()).unwrap_or(u32::MAX));
    for row in rows {
        push_u8(&mut out, row.row_type);
        push_u16(&mut out, row.depth);
        push_i32(&mut out, row.source_index);
        push_u32(&mut out, row.track_number);
        push_u32(&mut out, row.child_count);
        push_string(&mut out, &row.title);
        push_string(&mut out, &row.key);
        push_string(&mut out, &row.artist);
        push_string(&mut out, &row.path);
        push_string(&mut out, &row.cover_path);
        push_u64(&mut out, row.cover_stamp);
        push_string(&mut out, &row.track_path);
        push_u32(
            &mut out,
            u32::try_from(row.play_paths.len()).unwrap_or(u32::MAX),
        );
        for path in &row.play_paths {
            push_string(&mut out, path);
        }
    }
    out
}

fn clamp_u32(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

fn push_u8(out: &mut Vec<u8>, value: u8) {
    out.push(value);
}

fn push_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u64(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn resolve_album(album: &AlbumNodeBuilder) -> ResolvedAlbum {
    // Classify sections: disc sections (CD1, DVD1, etc.) contribute to album metadata,
    // non-disc sections (Bonus, independent sub-albums) are independent.
    let mut album_tracks: Vec<TrackLeaf> = album.root_tracks.clone();
    let mut sections = Vec::new();

    for (name, tracks) in &album.sections {
        let is_disc = super::is_main_album_disc_section(name);
        if is_disc {
            album_tracks.extend(tracks.iter().cloned());
        }

        // Disc sections show the album's cover.
        let (section_cover, section_cover_stamp) = if is_disc {
            (None, 0)
        } else {
            split_cover(
                resolve_album_cover(tracks)
                    .or_else(|| find_image_in_dir(&album.folder_path.join(name))),
            )
        };
        let section_year = if is_disc {
            None // disc sections don't get independent year
        } else {
            resolve_album_year(tracks)
        };

        sections.push(ResolvedSection {
            name: name.clone(),
            name_key: CollationKey::new(name),
            path: album.folder_path.join(name),
            cover_path: section_cover,
            cover_stamp: section_cover_stamp,
            year: section_year,
            tracks: order_tracks(tracks),
        });
    }
    sections.sort_by(|a, b| by_name((&a.name_key, &a.name), (&b.name_key, &b.name)));

    let title = resolve_album_title(&album_tracks, &album.folder_name);
    let year = resolve_album_year(&album_tracks);

    // Album cover fallback: embedded art from album tracks, then image in album dir,
    // then image in disc section dirs (not non-disc sections).
    let (cover_path, cover_stamp) = split_cover(
        resolve_album_cover(&album_tracks)
            .or_else(|| find_image_in_dir(&album.folder_path))
            .or_else(|| {
                sections
                    .iter()
                    .filter(|s| super::is_main_album_disc_section(&s.name))
                    .find_map(|s| find_image_in_dir(&s.path))
            }),
    );

    ResolvedAlbum {
        title_key: CollationKey::new(&title),
        title,
        year,
        folder_name: album.folder_name.clone(),
        folder_path: album.folder_path.clone(),
        cover_path,
        cover_stamp,
        root_tracks: order_tracks(&album.root_tracks),
        sections,
    }
}

fn sort_resolved_albums(albums: &mut [ResolvedAlbum], sort_mode: LibrarySortMode) {
    albums.sort_by(|a, b| match sort_mode {
        LibrarySortMode::Year => {
            let a_unknown = a.year.is_none();
            let b_unknown = b.year.is_none();
            a_unknown
                .cmp(&b_unknown)
                .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
                .then_with(|| by_name((&a.title_key, &a.title), (&b.title_key, &b.title)))
        }
        LibrarySortMode::Title => a
            .title_key
            .cmp(&b.title_key)
            .then_with(|| {
                let a_unknown = a.year.is_none();
                let b_unknown = b.year.is_none();
                a_unknown
                    .cmp(&b_unknown)
                    .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
            })
            .then_with(|| a.title.cmp(&b.title)),
    });
}

fn resolve_album_title(all_tracks: &[TrackLeaf], folder_name: &str) -> String {
    if all_tracks.is_empty() {
        return folder_name.to_string();
    }

    let mut non_empty = all_tracks
        .iter()
        .map(|track| track.album_tag.trim())
        .filter(|value| !value.is_empty())
        .collect::<Vec<_>>();

    if non_empty.len() != all_tracks.len() {
        return folder_name.to_string();
    }

    non_empty.sort_unstable();
    non_empty.dedup();
    if non_empty.len() == 1 {
        non_empty[0].to_string()
    } else {
        folder_name.to_string()
    }
}

fn resolve_album_year(all_tracks: &[TrackLeaf]) -> Option<i32> {
    super::resolve_uniform_year(all_tracks.iter().map(|track| track.year))
}

/// A cover and its stamp; see [`crate::library::LibraryTrack::cover_stamp`].
type TreeCover = (String, u64);

fn split_cover(cover: Option<TreeCover>) -> (Option<String>, u64) {
    cover.map_or((None, 0), |(path, stamp)| (Some(path), stamp))
}

fn resolve_album_cover(all_tracks: &[TrackLeaf]) -> Option<TreeCover> {
    all_tracks
        .iter()
        .find(|track| !track.cover_path.trim().is_empty())
        .map(|track| (track.cover_path.trim().to_string(), track.cover_stamp))
}

fn find_image_in_dir(dir: &Path) -> Option<TreeCover> {
    let images = FolderImages::read(dir).ok()?;
    let cover = images.cover()?.to_string_lossy().into_owned();
    Some((cover, images.stamp()))
}

fn order_tracks(tracks: &[TrackLeaf]) -> Vec<OrderedTrack> {
    if tracks.is_empty() {
        return Vec::new();
    }

    let inputs = tracks
        .iter()
        .map(|track| TrackSortInput {
            path: track.path.to_string_lossy(),
            track_no: track.track_no,
            filename_number: leading_track_number(&track.file_stem),
        })
        .collect::<Vec<_>>();
    let order = tree_track_order(&inputs);

    let max_number = order.iter().map(|(_, number)| *number).max().unwrap_or(0);
    let width = std::cmp::max(2, max_number.to_string().len());

    order
        .into_iter()
        .map(|(index, number)| OrderedTrack {
            label: format!(
                "{:0width$} - {}",
                number,
                tracks[index].title,
                width = width
            ),
            path: inputs[index].path.to_string(),
            number,
        })
        .collect()
}

/// What the tree orders the tracks of an album or section by.
struct TrackSortInput<'a> {
    path: Cow<'a, str>,
    track_no: Option<u32>,
    /// The number the file name starts with.
    filename_number: Option<u32>,
}

impl<'a> TrackSortInput<'a> {
    fn of(track: &'a LibraryTrack) -> Self {
        Self {
            path: track.path.to_string_lossy(),
            track_no: track.track_no,
            filename_number: track
                .path
                .file_stem()
                .and_then(|stem| leading_track_number(&stem.to_string_lossy())),
        }
    }
}

/// The order the tree shows `tracks` in: those with a track number tag by
/// it, then those whose file name starts with a number by that number,
/// then the others by path; equal numbers by path. Returns each track's
/// index in `tracks` and the number the tree shows for it, in that order.
fn tree_track_order(tracks: &[TrackSortInput<'_>]) -> Vec<(usize, u32)> {
    let keys = tracks
        .iter()
        .map(|track| CollationKey::new(&track.path))
        .collect::<Vec<_>>();
    let mut by_path = (0..tracks.len()).collect::<Vec<_>>();
    by_path.sort_by(|&a, &b| by_name((&keys[a], &tracks[a].path), (&keys[b], &tracks[b].path)));

    let mut order = by_path
        .into_iter()
        .enumerate()
        .map(|(position, index)| {
            let track = &tracks[index];
            let number = track
                .track_no
                .or(track.filename_number)
                .unwrap_or_else(|| u32::try_from(position + 1).unwrap_or(u32::MAX));
            let rank: u8 = if track.track_no.is_some() {
                0
            } else if track.filename_number.is_some() {
                1
            } else {
                2
            };
            (rank, number, index)
        })
        .collect::<Vec<_>>();
    // A stable sort of tracks already in the order of their paths: equal
    // ranks and numbers keep that order.
    order.sort_by_key(|&(rank, number, _)| (rank, number));
    order
        .into_iter()
        .map(|(_, number, index)| (index, number))
        .collect()
}

/// Appends the paths of an album's main tracks to `out` in the order the
/// tree plays them (see [`album_play_paths`]): the tracks in the album
/// folder, then those of each disc section, the sections by name.
pub(super) fn push_album_paths_in_tree_order(
    root_tracks: &[&LibraryTrack],
    sections: &mut [(Cow<'_, str>, Vec<&LibraryTrack>)],
    out: &mut Vec<PathBuf>,
) {
    let mut push = |tracks: &[&LibraryTrack]| {
        let inputs = tracks
            .iter()
            .map(|track| TrackSortInput::of(track))
            .collect::<Vec<_>>();
        out.extend(
            tree_track_order(&inputs)
                .into_iter()
                .map(|(index, _)| tracks[index].path.clone()),
        );
    };
    push(root_tracks);
    sections.sort_by_cached_key(|(name, _)| (CollationKey::new(name), name.to_string()));
    for (_, tracks) in sections.iter() {
        push(tracks);
    }
}

fn normalized_track_title(track: &LibraryTrack) -> String {
    if !track.title.trim().is_empty() {
        return track.title.trim().to_string();
    }
    track.path.file_stem().map_or_else(
        || track.path.to_string_lossy().to_string(),
        |name| name.to_string_lossy().to_string(),
    )
}

fn pick_root_for_track<'a>(
    roots: &'a [LibraryRoot],
    track: &LibraryTrack,
) -> Option<&'a LibraryRoot> {
    if !track.root_path.as_os_str().is_empty() {
        if let Some(root) = roots.iter().find(|root| root.path == track.root_path) {
            return Some(root);
        }
    }

    roots
        .iter()
        .filter(|root| track.path.starts_with(&root.path))
        .max_by_key(|root| root.path.components().count())
}

fn leading_track_number(input: &str) -> Option<u32> {
    let mut n: u32 = 0;
    let mut saw_digit = false;
    for ch in input.chars() {
        if let Some(d) = ch.to_digit(10) {
            saw_digit = true;
            n = n.saturating_mul(10).saturating_add(d);
        } else {
            break;
        }
    }
    if saw_digit {
        Some(n)
    } else {
        None
    }
}

/// Orders names by their collation keys, then by their own text.
fn by_name(a: (&CollationKey, &str), b: (&CollationKey, &str)) -> Ordering {
    a.0.cmp(b.0).then_with(|| a.1.cmp(b.1))
}

fn sort_artists(artists: &mut [&ArtistNodeBuilder]) {
    artists.sort_by(|a, b| by_name((&a.sort_key, &a.artist_name), (&b.sort_key, &b.artist_name)));
}

/// The roots in the order the tree shows them: by name, then by path.
fn sorted_roots(builders: &BTreeMap<String, RootNodeBuilder>) -> Vec<&RootNodeBuilder> {
    let mut roots = builders.values().collect::<Vec<_>>();
    // The builders are in the order of their paths, which the stable sort
    // keeps for equal names.
    roots.sort_by(|a, b| by_name((&a.sort_key, &a.root_name), (&b.sort_key, &b.root_name)));
    roots
}

/// Collect all track paths for the given artist in the same order the
/// library tree displays them.  Albums are sorted by `sort_mode` (year
/// or title).  Within each album: root tracks first, then disc sections
/// in alphabetical order, then non-disc sections sorted by year (or
/// name when year is absent).
/// Emit one artist's tracks in tree display order into `out`.
fn emit_artist_paths(
    artist: &ArtistNodeBuilder,
    sort_mode: LibrarySortMode,
    out: &mut Vec<PathBuf>,
) {
    // Loose tracks (directly under the artist folder).
    let loose = order_tracks(&artist.loose_tracks);
    for track in &loose {
        out.push(PathBuf::from(&track.path));
    }

    // Resolve and sort albums identically to the tree.
    let mut resolved_albums: Vec<ResolvedAlbum> =
        artist.albums.values().map(resolve_album).collect();
    sort_resolved_albums(&mut resolved_albums, sort_mode);

    for album in &resolved_albums {
        // Root-level album tracks.
        for track in &album.root_tracks {
            out.push(PathBuf::from(&track.path));
        }

        // Disc sections first (alphabetical, already sorted by
        // resolve_album which sorts sections by natural_cmp).
        for section in &album.sections {
            if super::is_main_album_disc_section(&section.name) {
                for track in &section.tracks {
                    out.push(PathBuf::from(&track.path));
                }
            }
        }

        // Non-disc sections: sort by year (or name if no year).
        let mut non_disc: Vec<&ResolvedSection> = album
            .sections
            .iter()
            .filter(|s| !super::is_main_album_disc_section(&s.name))
            .collect();
        non_disc.sort_by(|a, b| {
            let a_unknown = a.year.is_none();
            let b_unknown = b.year.is_none();
            a_unknown
                .cmp(&b_unknown)
                .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
                .then_with(|| by_name((&a.name_key, &a.name), (&b.name_key, &b.name)))
        });
        for section in non_disc {
            for track in &section.tracks {
                out.push(PathBuf::from(&track.path));
            }
        }
    }
}

/// Emit all artists in a root builder in alphabetical order into `out`.
fn emit_root_artists_paths(
    root_builder: &RootNodeBuilder,
    sort_mode: LibrarySortMode,
    out: &mut Vec<PathBuf>,
) {
    let mut artists: Vec<&ArtistNodeBuilder> = root_builder.artists.values().collect();
    sort_artists(&mut artists);
    for artist in artists {
        emit_artist_paths(artist, sort_mode, out);
    }
}

pub(crate) fn collect_artist_paths_tree_order(
    library: &LibrarySnapshot,
    artist_selector: &str,
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    let builders = build_root_builders(&library.roots, &library.tracks);
    let mut out = Vec::new();
    emit_selected_artist_paths(&builders, artist_selector, sort_mode, &mut out);
    out
}

/// Appends the paths of the artists `artist_selector` names, a tree key
/// or a name, in tree order.
fn emit_selected_artist_paths(
    builders: &BTreeMap<String, RootNodeBuilder>,
    artist_selector: &str,
    sort_mode: LibrarySortMode,
    out: &mut Vec<PathBuf>,
) {
    let artist_selector_is_key = artist_selector.starts_with("artist|");
    for root_builder in builders.values() {
        for artist in root_builder.artists.values() {
            let matches = if artist_selector_is_key {
                let root_path = root_builder.root_path.to_string_lossy();
                tree_key_is(
                    artist_selector,
                    "artist",
                    &[&root_path, &artist.artist_name],
                )
            } else {
                artist.artist_name == artist_selector
            };
            if !matches {
                continue;
            }

            emit_artist_paths(artist, sort_mode, out);
        }
    }
}

/// The tracks of library rows to queue, in their order: each row's tracks
/// in tree order. The library is grouped into its tree once, however many
/// artists and roots the rows name.
pub(crate) fn collect_queue_item_paths(
    library: &LibrarySnapshot,
    items: &[super::LibraryQueueItem],
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    use super::LibraryQueueItem;
    let mut builders = None;
    let mut out = Vec::new();
    for item in items {
        match item {
            LibraryQueueItem::Paths(paths) => out.extend(paths.iter().cloned()),
            LibraryQueueItem::Artist(selector) => {
                let builders = builders
                    .get_or_insert_with(|| build_root_builders(&library.roots, &library.tracks));
                emit_selected_artist_paths(builders, selector.trim(), sort_mode, &mut out);
            }
            LibraryQueueItem::Root(root) => {
                let builders = builders
                    .get_or_insert_with(|| build_root_builders(&library.roots, &library.tracks));
                if let Some(root_builder) = builders.get(root.as_str()) {
                    emit_root_artists_paths(root_builder, sort_mode, &mut out);
                }
            }
        }
    }
    out
}

/// Collect all tracks under a single library root in tree display order
/// (artists alphabetical, albums sorted by `sort_mode`).
pub(crate) fn collect_root_paths_tree_order(
    library: &LibrarySnapshot,
    root_path: &str,
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    let builders = build_root_builders(&library.roots, &library.tracks);
    let mut out = Vec::new();
    if let Some(root_builder) = builders.get(root_path) {
        emit_root_artists_paths(root_builder, sort_mode, &mut out);
    }
    out
}

/// Collect all library tracks across all roots in tree display order
/// (roots in `BTreeMap` order, artists alphabetical, albums sorted by `sort_mode`).
pub(crate) fn collect_all_paths_tree_order(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
) -> Vec<PathBuf> {
    let builders = build_root_builders(&library.roots, &library.tracks);
    let mut out = Vec::new();
    for root_builder in sorted_roots(&builders) {
        emit_root_artists_paths(root_builder, sort_mode, &mut out);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::{LibraryRoot, LibrarySnapshot};

    #[derive(Debug)]
    struct DecodedRow {
        row_type: u8,
        depth: u16,
        source_index: i32,
        track_number: u32,
        child_count: u32,
        title: String,
        key: String,
        artist: String,
        path: String,
        cover_path: String,
        cover_stamp: u64,
        track_path: String,
        play_paths: Vec<String>,
    }

    fn read_u8(input: &[u8], offset: &mut usize) -> Option<u8> {
        if *offset + 1 > input.len() {
            return None;
        }
        let value = input[*offset];
        *offset += 1;
        Some(value)
    }

    fn read_u16(input: &[u8], offset: &mut usize) -> Option<u16> {
        if *offset + 2 > input.len() {
            return None;
        }
        let value = u16::from_le_bytes([input[*offset], input[*offset + 1]]);
        *offset += 2;
        Some(value)
    }

    fn read_i32(input: &[u8], offset: &mut usize) -> Option<i32> {
        if *offset + 4 > input.len() {
            return None;
        }
        let value = i32::from_le_bytes([
            input[*offset],
            input[*offset + 1],
            input[*offset + 2],
            input[*offset + 3],
        ]);
        *offset += 4;
        Some(value)
    }

    fn read_u32(input: &[u8], offset: &mut usize) -> Option<u32> {
        if *offset + 4 > input.len() {
            return None;
        }
        let value = u32::from_le_bytes([
            input[*offset],
            input[*offset + 1],
            input[*offset + 2],
            input[*offset + 3],
        ]);
        *offset += 4;
        Some(value)
    }

    fn read_string(input: &[u8], offset: &mut usize) -> Option<String> {
        let len = usize::try_from(read_u32(input, offset)?).ok()?;
        if *offset + len > input.len() {
            return None;
        }
        let bytes = &input[*offset..*offset + len];
        *offset += len;
        Some(String::from_utf8_lossy(bytes).to_string())
    }

    fn decode_rows(bytes: &[u8]) -> Vec<DecodedRow> {
        let mut offset = 0usize;
        let Some(row_count) = read_u32(bytes, &mut offset) else {
            return Vec::new();
        };
        let mut rows = Vec::with_capacity(usize::try_from(row_count).unwrap_or(usize::MAX));
        for _ in 0..row_count {
            let Some(row_type) = read_u8(bytes, &mut offset) else {
                break;
            };
            let Some(depth) = read_u16(bytes, &mut offset) else {
                break;
            };
            let Some(source_index) = read_i32(bytes, &mut offset) else {
                break;
            };
            let Some(track_number) = read_u32(bytes, &mut offset) else {
                break;
            };
            let Some(child_count) = read_u32(bytes, &mut offset) else {
                break;
            };
            let Some(title) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(key) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(artist) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(path) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(cover_path) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(cover_stamp) = bytes
                .get(offset..offset + 8)
                .and_then(|stamp| stamp.try_into().ok())
                .map(u64::from_le_bytes)
            else {
                break;
            };
            offset += 8;
            let Some(track_path) = read_string(bytes, &mut offset) else {
                break;
            };
            let Some(play_path_count) = read_u32(bytes, &mut offset) else {
                break;
            };
            let mut play_paths = Vec::new();
            let mut ok = true;
            for _ in 0..play_path_count {
                let Some(play_path) = read_string(bytes, &mut offset) else {
                    ok = false;
                    break;
                };
                play_paths.push(play_path);
            }
            if !ok {
                break;
            }
            rows.push(DecodedRow {
                row_type,
                depth,
                source_index,
                track_number,
                child_count,
                title,
                key,
                artist,
                path,
                cover_path,
                cover_stamp,
                track_path,
                play_paths,
            });
        }
        rows
    }

    fn track(
        path: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
        title: &str,
    ) -> LibraryTrack {
        LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: PathBuf::from(path),
            root_path: PathBuf::from("/music"),
            title: title.to_string(),
            artist: String::new(),
            album: album.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            cover_read_at: crate::library::CoverReadAt::default(),
            genre: String::new(),
            year,
            track_no,
            duration_secs: None,
        }
    }

    fn root(path: &str) -> LibraryRoot {
        LibraryRoot {
            path: PathBuf::from(path),
            name: String::new(),
        }
    }

    #[test]
    fn cached_expansion_matches_fresh_tree_and_reuses_cover_resolution() {
        let dir = std::env::temp_dir().join(format!("ferrous-tree-cache-{}", std::process::id()));
        let album_dir = dir.join("Artist/Album");
        std::fs::create_dir_all(&album_dir).expect("fixture directory");
        let cover = album_dir.join("cover.png");
        std::fs::write(&cover, b"fixture").expect("cover fixture");
        let mut song = track(
            &album_dir.join("01.flac").to_string_lossy(),
            "Album",
            Some(2020),
            Some(1),
            "Song",
        );
        song.root_path.clone_from(&dir);
        let library = LibrarySnapshot {
            roots: vec![root(&dir.to_string_lossy())],
            tracks: crate::library::shared_tracks(vec![song]),
            ..Default::default()
        };
        let mut cache = LibraryTreeCache::new(&library);
        let artist = artist_row_key(&dir.to_string_lossy(), "Artist");
        let album = album_row_key(&dir.to_string_lossy(), "Artist", "Album");
        let mut expanded = HashSet::from([artist]);
        let initial = cache.build(LibrarySortMode::Year, Some(&expanded));
        assert_eq!(
            initial,
            build_library_tree_flat_binary(&library, LibrarySortMode::Year, Some(&expanded))
        );
        std::fs::remove_file(&cover).expect("remove cover to detect new directory lookups");
        expanded.insert(album);
        let expanded_bytes = cache.build(LibrarySortMode::Title, Some(&expanded));
        let rows = decode_rows(&expanded_bytes);
        assert!(rows.iter().any(|row| row.row_type == ROW_TYPE_TRACK));
        assert!(rows
            .iter()
            .any(|row| row.cover_path == cover.to_string_lossy()));
        expanded.clear();
        let _ = cache.build(LibrarySortMode::Year, Some(&expanded));
        let mut fresh = LibraryTreeCache::new(&library);
        let expanded = HashSet::from([artist_row_key(&dir.to_string_lossy(), "Artist")]);
        let fresh_bytes = fresh.build(LibrarySortMode::Year, Some(&expanded));
        assert!(decode_rows(&fresh_bytes)
            .iter()
            .all(|row| row.cover_path.is_empty()));
        std::fs::remove_dir_all(dir).expect("remove fixture");
    }

    #[test]
    fn mixed_album_years_omit_album_year() {
        let album = AlbumNodeBuilder {
            folder_name: "Folder".to_string(),
            folder_path: PathBuf::from("/music/Artist/Folder"),
            root_tracks: vec![
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/01.flac"),
                    title: "A".to_string(),
                    file_stem: "01".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    year: Some(2024),
                    track_no: Some(1),
                },
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/02.flac"),
                    title: "B".to_string(),
                    file_stem: "02".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    year: Some(2023),
                    track_no: Some(2),
                },
            ],
            sections: BTreeMap::new(),
        };
        let resolved = resolve_album(&album);
        assert_eq!(resolved.year, None);
    }

    #[test]
    fn uniform_album_years_are_preserved() {
        let album = AlbumNodeBuilder {
            folder_name: "Folder".to_string(),
            folder_path: PathBuf::from("/music/Artist/Folder"),
            root_tracks: vec![
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/01.flac"),
                    title: "A".to_string(),
                    file_stem: "01".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    year: Some(2023),
                    track_no: Some(1),
                },
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/02.flac"),
                    title: "B".to_string(),
                    file_stem: "02".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    year: Some(2023),
                    track_no: Some(2),
                },
            ],
            sections: BTreeMap::new(),
        };
        let resolved = resolve_album(&album);
        assert_eq!(resolved.year, Some(2023));
    }

    #[test]
    fn single_root_hides_root_rows() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track",
            )]),
            ..LibrarySnapshot::default()
        };

        let tree = build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Option::<&HashSet<String>>::None,
        );
        let rows = decode_rows(&tree);
        assert!(!rows.is_empty());
        assert_eq!(rows[0].row_type, ROW_TYPE_ARTIST);
    }

    #[test]
    fn multi_root_keeps_roots_split() {
        let library = LibrarySnapshot {
            roots: vec![root("/music-a"), root("/music-b")],
            tracks: crate::library::shared_tracks(vec![
                LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: PathBuf::from("/music-a/Artist/Album/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    cover_read_at: crate::library::CoverReadAt::default(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
                LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: PathBuf::from("/music-b/Artist/Album/01.flac"),
                    root_path: PathBuf::from("/music-b"),
                    title: "Track B".to_string(),
                    artist: String::new(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    cover_read_at: crate::library::CoverReadAt::default(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
            ]),
            ..LibrarySnapshot::default()
        };

        let tree = build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Option::<&HashSet<String>>::None,
        );
        let rows = decode_rows(&tree);
        let root_rows = rows
            .iter()
            .filter(|row| row.row_type == ROW_TYPE_ROOT)
            .collect::<Vec<_>>();
        assert_eq!(root_rows.len(), 2);
    }

    #[test]
    fn artist_keys_include_root_identity() {
        let library = LibrarySnapshot {
            roots: vec![root("/music-a"), root("/music-b")],
            tracks: crate::library::shared_tracks(vec![
                LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: PathBuf::from("/music-a/Same Artist/Album A/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album A".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    cover_read_at: crate::library::CoverReadAt::default(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
                LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: PathBuf::from("/music-b/Same Artist/Album B/01.flac"),
                    root_path: PathBuf::from("/music-b"),
                    title: "Track B".to_string(),
                    artist: String::new(),
                    album: "Album B".to_string(),
                    cover_path: String::new(),
                    cover_stamp: 0,
                    cover_read_at: crate::library::CoverReadAt::default(),
                    genre: String::new(),
                    year: Some(2021),
                    track_no: Some(1),
                    duration_secs: None,
                },
            ]),
            ..LibrarySnapshot::default()
        };

        let tree = build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Option::<&HashSet<String>>::None,
        );
        let rows = decode_rows(&tree);
        let artist_keys = rows
            .iter()
            .filter(|row| row.row_type == ROW_TYPE_ARTIST)
            .map(|row| row.key.clone())
            .collect::<Vec<_>>();
        assert!(artist_keys.contains(&"artist|/music-a|Same Artist".to_string()));
        assert!(artist_keys.contains(&"artist|/music-b|Same Artist".to_string()));
    }

    #[test]
    fn lazy_hydration_emits_artist_then_album_then_tracks() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )]),
            ..LibrarySnapshot::default()
        };

        let expanded = HashSet::new();
        let bytes =
            build_library_tree_flat_binary(&library, LibrarySortMode::Year, Some(&expanded));
        let rows = decode_rows(&bytes);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].row_type, ROW_TYPE_ARTIST);
        assert_eq!(rows[0].child_count, 1);

        let mut expanded = HashSet::new();
        expanded.insert("artist|/music|Artist A".to_string());
        let bytes =
            build_library_tree_flat_binary(&library, LibrarySortMode::Year, Some(&expanded));
        let rows = decode_rows(&bytes);
        assert!(rows.iter().any(|row| row.row_type == ROW_TYPE_ALBUM));
        assert!(!rows.iter().any(|row| row.row_type == ROW_TYPE_TRACK));

        expanded.insert("album|/music|Artist A|Album A".to_string());
        let bytes =
            build_library_tree_flat_binary(&library, LibrarySortMode::Year, Some(&expanded));
        let rows = decode_rows(&bytes);
        assert!(rows.iter().any(|row| row.row_type == ROW_TYPE_ALBUM));
        assert!(rows.iter().any(|row| row.row_type == ROW_TYPE_TRACK));
    }

    fn titles_of(rows: &[DecodedRow], row_type: u8) -> Vec<String> {
        rows.iter()
            .filter(|row| row.row_type == row_type)
            .map(|row| row.title.clone())
            .collect()
    }

    #[test]
    fn accented_names_sort_with_their_base_letters_and_numbers_naturally() {
        let paths = [
            "/music/Zappa/Album/01.flac",
            "/music/Émilie Simon/Album/01.flac",
            "/music/Ørjan/Album/01.flac",
            "/music/Abba/Album/01.flac",
            "/music/Eels/Album/01.flac",
            "/music/Oasis/Album/01.flac",
            "/music/Oasis/Ålbum/01.flac",
            "/music/Oasis/Zed/01.flac",
            "/music/Oasis/Boxed/Disc 10/01.flac",
            "/music/Oasis/Boxed/Disc 2/01.flac",
            "/music/Oasis/Boxed/Disc 1/01.flac",
            "/music/Oasis/Loose/Track 10.flac",
            "/music/Oasis/Loose/Track 9.flac",
        ];
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(
                paths
                    .iter()
                    .map(|path| track(path, "", None, None, ""))
                    .collect(),
            ),
            ..LibrarySnapshot::default()
        };
        let rows = build_library_tree_flat_rows::<std::collections::hash_map::RandomState>(
            &library,
            LibrarySortMode::Title,
            None,
        );
        let rows = decode_rows(&encode_flat_rows(&rows));
        assert_eq!(
            titles_of(&rows, ROW_TYPE_ARTIST),
            [
                "Abba (1)",
                "Eels (1)",
                "Émilie Simon (1)",
                "Oasis (5)",
                "Ørjan (1)",
                "Zappa (1)"
            ]
        );
        assert_eq!(
            titles_of(&rows, ROW_TYPE_ALBUM),
            [
                "Album", "Album", "Album", "Album", "Ålbum", "Boxed", "Loose", "Zed", "Album",
                "Album"
            ]
        );
        assert_eq!(
            titles_of(&rows, ROW_TYPE_SECTION),
            ["Disc 1", "Disc 2", "Disc 10"]
        );
        let loose: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type == ROW_TYPE_TRACK && row.path.contains("/Loose/"))
            .map(|row| row.path.as_str())
            .collect();
        assert_eq!(
            loose,
            [
                "/music/Oasis/Loose/Track 9.flac",
                "/music/Oasis/Loose/Track 10.flac"
            ]
        );
        // Queueing follows the tree.
        let queued = collect_artist_paths_tree_order(&library, "Oasis", LibrarySortMode::Title);
        assert_eq!(
            queued
                .iter()
                .map(|path| path.to_string_lossy().into_owned())
                .collect::<Vec<_>>()[..5],
            [
                "/music/Oasis/Album/01.flac",
                "/music/Oasis/Ålbum/01.flac",
                "/music/Oasis/Boxed/Disc 1/01.flac",
                "/music/Oasis/Boxed/Disc 2/01.flac",
                "/music/Oasis/Boxed/Disc 10/01.flac",
            ]
        );
    }

    #[test]
    fn roots_sort_by_their_names() {
        let mut library = LibrarySnapshot {
            roots: vec![root("/a"), root("/b"), root("/c")],
            tracks: crate::library::shared_tracks(
                ["/a/X/Y/1.flac", "/b/X/Y/1.flac", "/c/X/Y/1.flac"]
                    .iter()
                    .map(|path| LibraryTrack {
                        root_path: Path::new(path).ancestors().nth(3).expect("root").into(),
                        ..track(path, "", None, None, "")
                    })
                    .collect(),
            ),
            ..LibrarySnapshot::default()
        };
        library.roots[0].name = "Zebra".to_string();
        library.roots[1].name = "Éclair".to_string();
        library.roots[2].name = "apple".to_string();
        let rows = build_library_tree_flat_rows::<std::collections::hash_map::RandomState>(
            &library,
            LibrarySortMode::Year,
            None,
        );
        let roots: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type == ROW_TYPE_ROOT)
            .map(|row| row.title.as_str())
            .collect();
        assert_eq!(roots, ["apple", "Éclair", "Zebra"]);
        let all = collect_all_paths_tree_order(&library, LibrarySortMode::Year);
        assert_eq!(
            all,
            [
                PathBuf::from("/c/X/Y/1.flac"),
                PathBuf::from("/b/X/Y/1.flac"),
                PathBuf::from("/a/X/Y/1.flac")
            ]
        );
    }

    #[test]
    fn an_artist_of_a_root_with_a_separator_is_queued_by_its_key() {
        let root_path = "/music|archive";
        let library = LibrarySnapshot {
            roots: vec![root(root_path)],
            tracks: crate::library::shared_tracks(vec![LibraryTrack {
                root_path: PathBuf::from(root_path),
                ..track("/music|archive/Artist/Album/01.flac", "", None, Some(1), "")
            }]),
            ..LibrarySnapshot::default()
        };
        let key = artist_row_key(root_path, "Artist");
        assert_eq!(key, "artist|/music\\|archive|Artist");
        assert_eq!(
            collect_artist_paths_tree_order(&library, &key, LibrarySortMode::Year),
            [PathBuf::from("/music|archive/Artist/Album/01.flac")]
        );
    }

    #[test]
    fn names_with_a_separator_have_keys_of_their_own() {
        // Joined without escapes, both albums had the key
        // `album|/music|A|B|C`, and both sections `section|/music|A|B|C|D`.
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track("/music/A|B/C/D/01.flac", "", None, Some(1), "One"),
                track("/music/A/B|C/D/01.flac", "", None, Some(1), "Two"),
                track("/music/A\\/B|C/D/01.flac", "", None, Some(1), "Three"),
            ]),
            ..LibrarySnapshot::default()
        };
        let rows = build_library_tree_flat_rows::<std::collections::hash_map::RandomState>(
            &library,
            LibrarySortMode::Year,
            None,
        );
        let keys: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type != ROW_TYPE_TRACK)
            .map(|row| row.key.as_str())
            .collect();
        let unique: HashSet<_> = keys.iter().collect();
        assert_eq!(unique.len(), keys.len(), "{keys:?}");
        assert!(keys.contains(&"album|/music|A\\|B|C"), "{keys:?}");
        assert!(keys.contains(&"album|/music|A|B|C"), "{keys:?}");
        assert!(keys.contains(&"section|/music|A|B\\|C|D"), "{keys:?}");

        // Expanding one album expands only that album.
        let expanded = HashSet::from([
            "artist|/music|A|B".to_string(),
            "artist|/music|A".to_string(),
            "album|/music|A\\|B|C".to_string(),
        ]);
        let bytes =
            build_library_tree_flat_binary(&library, LibrarySortMode::Year, Some(&expanded));
        let tracks: Vec<_> = decode_rows(&bytes)
            .into_iter()
            .filter(|row| row.row_type == ROW_TYPE_SECTION)
            .map(|row| row.path)
            .collect();
        assert_eq!(tracks, ["/music/A|B/C/D"]);

        // Each key names its row alone.
        assert!(tree_key_is(
            "album|/music|A\\|B|C",
            "album",
            &["/music", "A|B", "C"]
        ));
        assert!(!tree_key_is(
            "album|/music|A\\|B|C",
            "album",
            &["/music", "A", "B|C"]
        ));
        assert!(tree_key_is(
            "album|/music|A|B|C",
            "album",
            &["/music", "A", "B|C"]
        ));
        assert!(!tree_key_is(
            "album|/music|A|B|C",
            "album",
            &["/music", "A|B", "C"]
        ));
        assert!(tree_key_is(
            "album|/music|A\\\\/B\\|C|D",
            "album",
            &["/music", "A\\/B|C", "D"]
        ));
    }

    #[test]
    fn expanded_keys_from_before_escaping_move_to_their_rows() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track(
                    "/music/AC|DC/Back in Black/01.flac",
                    "",
                    None,
                    Some(1),
                    "One",
                ),
                track("/music/X|Y/Z/S/01.flac", "", None, Some(1), "Two"),
                track("/music/X/Y|Z/S/01.flac", "", None, Some(1), "Three"),
                track("/music/Plain/Album/01.flac", "", None, Some(1), "Four"),
            ]),
            ..LibrarySnapshot::default()
        };
        let mut expanded = HashSet::from([
            "artist|/music|AC|DC".to_string(),
            "album|/music|AC|DC|Back in Black".to_string(),
            // The legacy key of two sections: which one is not known.
            "section|/music|X|Y|Z|S".to_string(),
            "album|/music|Plain|Album".to_string(),
            "album|/music|Gone|Album".to_string(),
        ]);
        let cache = LibraryTreeCache::new(&library);
        cache.retain_expanded_keys(&mut expanded);
        let mut kept: Vec<_> = expanded.iter().map(String::as_str).collect();
        kept.sort_unstable();
        assert_eq!(
            kept,
            [
                "album|/music|AC\\|DC|Back in Black",
                "album|/music|Plain|Album",
                "artist|/music|AC|DC",
            ]
        );

        let mut expanded = HashSet::from(["album|/music|AC|DC|Back in Black".to_string()]);
        retain_valid_expanded_keys(&library, &mut expanded);
        assert_eq!(
            expanded,
            HashSet::from(["album|/music|AC\\|DC|Back in Black".to_string()])
        );
    }

    #[test]
    fn retain_valid_expanded_keys_prunes_missing_nodes() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )]),
            ..LibrarySnapshot::default()
        };

        let mut expanded = HashSet::new();
        expanded.insert("artist|/music|Artist A".to_string());
        expanded.insert("album|/music|Artist A|Album A".to_string());
        expanded.insert("artist|/music|Missing Artist".to_string());
        expanded.insert("album|/music|Artist A|Missing Album".to_string());

        retain_valid_expanded_keys(&library, &mut expanded);

        assert!(expanded.contains("artist|/music|Artist A"));
        assert!(expanded.contains("album|/music|Artist A|Album A"));
        assert!(!expanded.contains("artist|/music|Missing Artist"));
        assert!(!expanded.contains("album|/music|Artist A|Missing Album"));
    }

    #[test]
    fn retain_valid_expanded_keys_waits_for_library_roots() {
        let mut expanded = HashSet::from([
            "artist|/music|Artist A".to_string(),
            "album|/music|Artist A|Album A".to_string(),
        ]);

        retain_valid_expanded_keys(&LibrarySnapshot::default(), &mut expanded);

        assert!(expanded.contains("artist|/music|Artist A"));
        assert!(expanded.contains("album|/music|Artist A|Album A"));
    }

    #[test]
    fn track_label_uses_zero_padded_number() {
        let ordered = order_tracks(&[
            TrackLeaf {
                path: PathBuf::from("/music/a/1.flac"),
                title: "One".to_string(),
                file_stem: "1".to_string(),
                album_tag: String::new(),
                cover_path: String::new(),
                cover_stamp: 0,
                year: None,
                track_no: Some(1),
            },
            TrackLeaf {
                path: PathBuf::from("/music/a/10.flac"),
                title: "Ten".to_string(),
                file_stem: "10".to_string(),
                album_tag: String::new(),
                cover_path: String::new(),
                cover_stamp: 0,
                year: None,
                track_no: Some(10),
            },
        ]);
        assert_eq!(ordered[0].label, "01 - One");
        assert_eq!(ordered[1].label, "10 - Ten");
        assert_eq!(ordered[0].number, 1);
        assert_eq!(ordered[1].number, 10);
    }

    #[test]
    fn binary_row_tracks_include_paths_and_play_paths() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )]),
            ..LibrarySnapshot::default()
        };
        let bytes = build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Option::<&HashSet<String>>::None,
        );
        let rows = decode_rows(&bytes);
        let track_row = rows
            .iter()
            .find(|row| row.row_type == ROW_TYPE_TRACK)
            .expect("track row");
        assert_eq!(track_row.track_path, "/music/Artist A/Album A/01.flac");
        assert_eq!(track_row.path, "/music/Artist A/Album A/01.flac");
        assert_eq!(track_row.play_paths.len(), 1);
        assert_eq!(track_row.play_paths[0], "/music/Artist A/Album A/01.flac");
        assert!(track_row.key.starts_with("track|"));
        assert_eq!(track_row.source_index, -1);
        assert_eq!(track_row.track_number, 1);
        assert_eq!(track_row.child_count, 0);
        assert!(!track_row.title.is_empty());
        assert!(!track_row.artist.is_empty());
        assert!(track_row.cover_path.is_empty());
        assert!(track_row.depth > 0);
    }

    #[test]
    fn counts_and_track_numbers_above_16_bits_are_sent_whole() {
        let loose = 70_000;
        let mut tracks: Vec<LibraryTrack> = (0..loose)
            .map(|index| LibraryTrack {
                path: PathBuf::from(format!("/music/Loose Artist/{index:05}.flac")),
                root_path: PathBuf::from("/music"),
                title: format!("Loose {index}"),
                ..LibraryTrack::default()
            })
            .collect();
        tracks.push(track(
            "/music/Artist A/Album A/01.flac",
            "Album A",
            None,
            Some(100_000),
            "Track",
        ));
        let library = LibrarySnapshot::new(vec![root("/music")], tracks);
        let expanded = HashSet::from([
            "artist|/music|Artist A".to_string(),
            "album|/music|Artist A|Album A".to_string(),
        ]);

        let rows = decode_rows(&build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Some(&expanded),
        ));

        let artist = rows
            .iter()
            .find(|row| row.key == "artist|/music|Loose Artist")
            .expect("artist row");
        assert_eq!(artist.child_count, 70_000);
        let numbered = rows
            .iter()
            .find(|row| row.track_path == "/music/Artist A/Album A/01.flac")
            .expect("track row");
        assert_eq!(numbered.track_number, 100_000);
    }

    #[test]
    fn the_tree_counts_the_artists_and_albums_it_builds_and_each_roots_tracks() {
        let library = LibrarySnapshot::new(
            vec![root("/music"), root("/more")],
            vec![
                track(
                    "/music/Artist A/Album A/01.flac",
                    "Album A",
                    None,
                    Some(1),
                    "One",
                ),
                track(
                    "/music/Artist A/Album A/CD1/01.flac",
                    "Album A",
                    None,
                    Some(1),
                    "Disc",
                ),
                track(
                    "/music/Artist A/Album B/01.flac",
                    "Album B",
                    None,
                    Some(1),
                    "Two",
                ),
                // A loose track: an artist without an album.
                track("/music/Artist B/loose.flac", "", None, None, "Loose"),
                LibraryTrack {
                    root_path: PathBuf::from("/more"),
                    ..track(
                        "/more/Artist A/Album C/01.flac",
                        "Album C",
                        None,
                        Some(1),
                        "Three",
                    )
                },
                // Outside every root: not in the tree.
                track(
                    "/elsewhere/Artist C/Album D/01.flac",
                    "Album D",
                    None,
                    Some(1),
                    "Out",
                ),
            ],
        );

        let cache = LibraryTreeCache::new(&library);

        // Artist A in each root, and Artist B; albums A, B and C.
        assert_eq!(cache.counts, (3, 3));
        // Each root counts the tracks it owns, which removing it takes out
        // of the library, even one the tree cannot place.
        assert_eq!(
            *cache.root_track_counts,
            HashMap::from([(PathBuf::from("/music"), 5), (PathBuf::from("/more"), 1)])
        );
    }

    fn leaf(path: &str, album: &str, year: Option<i32>, track_no: Option<u32>) -> TrackLeaf {
        let file_stem = Path::new(path)
            .file_stem()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string();
        TrackLeaf {
            path: PathBuf::from(path),
            title: file_stem.clone(),
            file_stem,
            album_tag: album.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            year,
            track_no,
        }
    }

    fn leaf_with_cover(
        path: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
        cover: &str,
    ) -> TrackLeaf {
        let mut l = leaf(path, album, year, track_no);
        l.cover_path = cover.to_string();
        l
    }

    #[test]
    fn disc_only_album_inherits_year_from_all_disc_sections() {
        let album = AlbumNodeBuilder {
            folder_name: "The Incident".to_string(),
            folder_path: PathBuf::from("/music/PT/The Incident"),
            root_tracks: vec![],
            sections: BTreeMap::from([
                (
                    "CD1".to_string(),
                    vec![
                        leaf(
                            "/music/PT/The Incident/CD1/01.flac",
                            "The Incident",
                            Some(2009),
                            Some(1),
                        ),
                        leaf(
                            "/music/PT/The Incident/CD1/02.flac",
                            "The Incident",
                            Some(2009),
                            Some(2),
                        ),
                    ],
                ),
                (
                    "CD2".to_string(),
                    vec![leaf(
                        "/music/PT/The Incident/CD2/01.flac",
                        "The Incident",
                        Some(2009),
                        Some(1),
                    )],
                ),
            ]),
        };
        let resolved = resolve_album(&album);
        assert_eq!(
            resolved.year,
            Some(2009),
            "album year should come from disc sections"
        );
        assert_eq!(resolved.title, "The Incident");
        // Disc sections should NOT have independent cover/year
        for section in &resolved.sections {
            assert!(
                section.cover_path.is_none(),
                "disc section should not have independent cover"
            );
            assert!(
                section.year.is_none(),
                "disc section should not have independent year"
            );
        }
    }

    #[test]
    fn album_folder_cover_prefers_front_over_an_alphabetically_earlier_back_scan() {
        let dir =
            std::env::temp_dir().join(format!("ferrous-tree-front-cover-{}", std::process::id()));
        let album_dir = dir.join("Artist/Album");
        std::fs::create_dir_all(&album_dir).expect("fixture directory");
        for image in ["back.jpg", "front.jpg"] {
            std::fs::write(album_dir.join(image), b"fixture").expect("image fixture");
        }
        let album = AlbumNodeBuilder {
            folder_name: "Album".to_string(),
            folder_path: album_dir.clone(),
            root_tracks: vec![leaf(
                &album_dir.join("01.flac").to_string_lossy(),
                "Album",
                Some(2020),
                Some(1),
            )],
            sections: BTreeMap::new(),
        };

        let resolved = resolve_album(&album);

        let front = album_dir.join("front.jpg");
        assert_eq!(resolved.cover_path.as_deref(), front.to_str());
        assert_eq!(
            resolved.cover_stamp,
            FolderImages::read(&album_dir).expect("folder").stamp(),
            "a cover found in the folder has the folder's stamp"
        );
        std::fs::remove_dir_all(dir).expect("remove fixture directory");
    }

    #[test]
    fn album_rows_carry_the_cover_stamp_of_their_tracks_cover() {
        let mut song = track(
            "/music/Artist/Album/01.flac",
            "Album",
            None,
            Some(1),
            "Song",
        );
        song.cover_path = "/music/Artist/Album/cover.jpg".to_string();
        song.cover_stamp = 0x0102_0304_0506_0708;
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![song]),
            ..Default::default()
        };
        let expanded = HashSet::from([artist_row_key("/music", "Artist")]);

        let rows = decode_rows(&build_library_tree_flat_binary(
            &library,
            LibrarySortMode::Year,
            Some(&expanded),
        ));

        let album = rows
            .iter()
            .find(|row| row.row_type == ROW_TYPE_ALBUM)
            .expect("album row");
        assert_eq!(album.cover_path, "/music/Artist/Album/cover.jpg");
        assert_eq!(album.cover_stamp, 0x0102_0304_0506_0708);
    }

    #[test]
    fn disc_only_album_inherits_cover_from_disc_section_tracks() {
        let album = AlbumNodeBuilder {
            folder_name: "Album".to_string(),
            folder_path: PathBuf::from("/music/Artist/Album"),
            root_tracks: vec![],
            sections: BTreeMap::from([(
                "CD1".to_string(),
                vec![leaf_with_cover(
                    "/music/Artist/Album/CD1/01.flac",
                    "Album",
                    Some(2020),
                    Some(1),
                    "/music/Artist/Album/CD1/cover.jpg",
                )],
            )]),
        };
        let resolved = resolve_album(&album);
        assert_eq!(
            resolved.cover_path.as_deref(),
            Some("/music/Artist/Album/CD1/cover.jpg"),
            "album should inherit embedded cover from disc section tracks"
        );
    }

    #[test]
    fn non_disc_sections_do_not_contribute_to_album_metadata() {
        let album = AlbumNodeBuilder {
            folder_name: "Muut".to_string(),
            folder_path: PathBuf::from("/music/PT/Muut"),
            root_tracks: vec![],
            sections: BTreeMap::from([
                (
                    "Lightbulb Sun".to_string(),
                    vec![leaf_with_cover(
                        "/music/PT/Muut/Lightbulb Sun/01.flac",
                        "Lightbulb Sun",
                        Some(2000),
                        Some(1),
                        "/music/PT/Muut/Lightbulb Sun/cover.jpg",
                    )],
                ),
                (
                    "Voyage 34".to_string(),
                    vec![leaf_with_cover(
                        "/music/PT/Muut/Voyage 34/01.flac",
                        "Voyage 34",
                        Some(1992),
                        Some(1),
                        "/music/PT/Muut/Voyage 34/cover.jpg",
                    )],
                ),
            ]),
        };
        let resolved = resolve_album(&album);
        // Album should NOT inherit cover or year from non-disc sections
        assert!(
            resolved.cover_path.is_none(),
            "grouping folder should have no cover"
        );
        assert!(
            resolved.year.is_none(),
            "grouping folder should have no year"
        );
        // Each section should have its own cover and year
        let ls = resolved
            .sections
            .iter()
            .find(|s| s.name == "Lightbulb Sun")
            .unwrap();
        assert_eq!(
            ls.cover_path.as_deref(),
            Some("/music/PT/Muut/Lightbulb Sun/cover.jpg")
        );
        assert_eq!(ls.year, Some(2000));
        let v34 = resolved
            .sections
            .iter()
            .find(|s| s.name == "Voyage 34")
            .unwrap();
        assert_eq!(
            v34.cover_path.as_deref(),
            Some("/music/PT/Muut/Voyage 34/cover.jpg")
        );
        assert_eq!(v34.year, Some(1992));
    }

    #[test]
    fn mixed_disc_and_non_disc_sections() {
        let album = AlbumNodeBuilder {
            folder_name: "Album".to_string(),
            folder_path: PathBuf::from("/music/Artist/Album"),
            root_tracks: vec![leaf(
                "/music/Artist/Album/01.flac",
                "Album",
                Some(2010),
                Some(1),
            )],
            sections: BTreeMap::from([
                (
                    "CD1".to_string(),
                    vec![leaf(
                        "/music/Artist/Album/CD1/01.flac",
                        "Album",
                        Some(2010),
                        Some(1),
                    )],
                ),
                (
                    "CD2".to_string(),
                    vec![leaf(
                        "/music/Artist/Album/CD2/01.flac",
                        "Album",
                        Some(2010),
                        Some(1),
                    )],
                ),
                (
                    "Bonus".to_string(),
                    vec![leaf_with_cover(
                        "/music/Artist/Album/Bonus/01.flac",
                        "Bonus Tracks",
                        Some(2015),
                        Some(1),
                        "/music/Artist/Album/Bonus/cover.jpg",
                    )],
                ),
            ]),
        };
        let resolved = resolve_album(&album);
        // Album metadata from root + CD1 + CD2 only
        assert_eq!(resolved.year, Some(2010));
        assert_eq!(resolved.title, "Album");
        // Bonus section has its own cover and year
        let bonus = resolved
            .sections
            .iter()
            .find(|s| s.name == "Bonus")
            .unwrap();
        assert_eq!(
            bonus.cover_path.as_deref(),
            Some("/music/Artist/Album/Bonus/cover.jpg")
        );
        assert_eq!(bonus.year, Some(2015));
        // CD sections should not have independent metadata
        let cd1 = resolved.sections.iter().find(|s| s.name == "CD1").unwrap();
        assert!(cd1.cover_path.is_none());
        assert!(cd1.year.is_none());
    }

    #[test]
    fn section_year_appears_in_title_in_flat_rows() {
        let album = ResolvedAlbum {
            title: "Muut".to_string(),
            title_key: CollationKey::new("Muut"),
            year: None,
            folder_name: "Muut".to_string(),
            folder_path: PathBuf::from("/music/PT/Muut"),
            cover_path: None,
            cover_stamp: 0,
            root_tracks: vec![],
            sections: vec![ResolvedSection {
                name: "Lightbulb Sun".to_string(),
                name_key: CollationKey::new("Lightbulb Sun"),
                path: PathBuf::from("/music/PT/Muut/Lightbulb Sun"),
                cover_path: Some("/cover.jpg".to_string()),
                cover_stamp: 7,
                year: Some(2000),
                tracks: vec![OrderedTrack {
                    label: "01 - Track".to_string(),
                    path: "/music/PT/Muut/Lightbulb Sun/01.flac".to_string(),
                    number: 1,
                }],
            }],
        };
        let mut rows = Vec::new();
        append_album_tracks(&mut rows, "PT", &album, "/music", 2, 3);
        let section_row = rows
            .iter()
            .find(|r| r.row_type == ROW_TYPE_SECTION)
            .unwrap();
        assert_eq!(section_row.title, "Lightbulb Sun (2000)");
        assert_eq!(section_row.cover_path, "/cover.jpg");
        assert_eq!(section_row.cover_stamp, 7);
    }

    #[test]
    fn collect_artist_paths_tree_order_sorts_albums_and_sections() {
        // Album B (1998): root track, "Disc 2" section track, "Bonus" section track
        // Album A (2000): root tracks 01, 02
        // Year sort → Album B first, then Album A.
        // Within Album B: root, disc sections (alphabetical), non-disc sections (by year/name).
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                // Album A — year 2000
                track(
                    "/music/TestArtist/Album A/01.flac",
                    "Album A",
                    Some(2000),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/TestArtist/Album A/02.flac",
                    "Album A",
                    Some(2000),
                    Some(2),
                    "Track 02",
                ),
                // Album B — year 1998, root track
                track(
                    "/music/TestArtist/Album B/01.flac",
                    "Album B",
                    Some(1998),
                    Some(1),
                    "Track 01",
                ),
                // Album B — "Disc 2" section track (disc section)
                track(
                    "/music/TestArtist/Album B/Disc 2/01.flac",
                    "Album B",
                    Some(1998),
                    Some(1),
                    "Track 01",
                ),
                // Album B — "Bonus" section track (non-disc section)
                track(
                    "/music/TestArtist/Album B/Bonus/01.flac",
                    "Bonus Tracks",
                    Some(1998),
                    Some(1),
                    "Bonus 01",
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let paths = collect_artist_paths_tree_order(&library, "TestArtist", LibrarySortMode::Year);

        assert_eq!(
            paths,
            vec![
                // Album B (1998) first — root track
                PathBuf::from("/music/TestArtist/Album B/01.flac"),
                // Album B — disc section "Disc 2"
                PathBuf::from("/music/TestArtist/Album B/Disc 2/01.flac"),
                // Album B — non-disc section "Bonus"
                PathBuf::from("/music/TestArtist/Album B/Bonus/01.flac"),
                // Album A (2000) — root tracks in track_no order
                PathBuf::from("/music/TestArtist/Album A/01.flac"),
                PathBuf::from("/music/TestArtist/Album A/02.flac"),
            ]
        );
    }

    fn track_in_root(
        path: &str,
        root_path: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
        title: &str,
    ) -> LibraryTrack {
        LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: PathBuf::from(path),
            root_path: PathBuf::from(root_path),
            title: title.to_string(),
            artist: String::new(),
            album: album.to_string(),
            cover_path: String::new(),
            cover_stamp: 0,
            cover_read_at: crate::library::CoverReadAt::default(),
            genre: String::new(),
            year,
            track_no,
            duration_secs: None,
        }
    }

    #[test]
    fn collect_root_paths_tree_order_sorts_by_artist_then_album_year() {
        // Root has two artists: "Beta" and "Alpha" (should sort alphabetically).
        // Alpha has Album X (2020), Beta has Album Y (1990) and Album Z (2010).
        // Year sort → Beta's albums: Y(1990), Z(2010); Alpha's album: X(2020).
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track(
                    "/music/Beta/Album Z/01.flac",
                    "Album Z",
                    Some(2010),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Beta/Album Y/01.flac",
                    "Album Y",
                    Some(1990),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Alpha/Album X/01.flac",
                    "Album X",
                    Some(2020),
                    Some(1),
                    "Track 01",
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let paths = collect_root_paths_tree_order(&library, "/music", LibrarySortMode::Year);
        assert_eq!(
            paths,
            vec![
                // Alpha first (alphabetical), then Beta
                PathBuf::from("/music/Alpha/Album X/01.flac"),
                // Beta: Album Y (1990) before Album Z (2010)
                PathBuf::from("/music/Beta/Album Y/01.flac"),
                PathBuf::from("/music/Beta/Album Z/01.flac"),
            ]
        );
    }

    #[test]
    fn queued_rows_keep_their_order_and_each_its_tree_order() {
        use super::super::LibraryQueueItem;
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track(
                    "/music/Beta/Album Y/02.flac",
                    "Album Y",
                    None,
                    Some(2),
                    "Two",
                ),
                track(
                    "/music/Beta/Album Y/01.flac",
                    "Album Y",
                    None,
                    Some(1),
                    "One",
                ),
                track(
                    "/music/Alpha/Album X/01.flac",
                    "Album X",
                    None,
                    Some(1),
                    "One",
                ),
            ]),
            ..LibrarySnapshot::default()
        };
        let paths = collect_queue_item_paths(
            &library,
            &[
                LibraryQueueItem::Artist("Beta".to_string()),
                LibraryQueueItem::Paths(vec![PathBuf::from("/elsewhere/song.flac")]),
                LibraryQueueItem::Root("/music".to_string()),
                LibraryQueueItem::Root("/not-a-root".to_string()),
            ],
            LibrarySortMode::Year,
        );
        assert_eq!(
            paths,
            [
                "/music/Beta/Album Y/01.flac",
                "/music/Beta/Album Y/02.flac",
                "/elsewhere/song.flac",
                "/music/Alpha/Album X/01.flac",
                "/music/Beta/Album Y/01.flac",
                "/music/Beta/Album Y/02.flac",
            ]
            .map(PathBuf::from)
        );
    }

    #[test]
    fn collect_root_paths_tree_order_sorts_by_artist_then_album_title() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track(
                    "/music/Artist/Zebra/01.flac",
                    "Zebra",
                    Some(1990),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Artist/Alpha/01.flac",
                    "Alpha",
                    Some(2020),
                    Some(1),
                    "Track 01",
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let paths = collect_root_paths_tree_order(&library, "/music", LibrarySortMode::Title);
        assert_eq!(
            paths,
            vec![
                // Title sort → Alpha before Zebra, regardless of year
                PathBuf::from("/music/Artist/Alpha/01.flac"),
                PathBuf::from("/music/Artist/Zebra/01.flac"),
            ]
        );
    }

    #[test]
    fn collect_all_paths_tree_order_spans_multiple_roots() {
        // Two roots: /a and /b, each with one artist and one album.
        let library = LibrarySnapshot {
            roots: vec![root("/a"), root("/b")],
            tracks: crate::library::shared_tracks(vec![
                track_in_root(
                    "/b/Artist/Album/01.flac",
                    "/b",
                    "Album",
                    Some(2000),
                    Some(1),
                    "T1",
                ),
                track_in_root(
                    "/a/Artist/Album/01.flac",
                    "/a",
                    "Album",
                    Some(2000),
                    Some(1),
                    "T1",
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let paths = collect_all_paths_tree_order(&library, LibrarySortMode::Year);
        assert_eq!(
            paths,
            vec![
                // Root /a first (BTreeMap order), then root /b
                PathBuf::from("/a/Artist/Album/01.flac"),
                PathBuf::from("/b/Artist/Album/01.flac"),
            ]
        );
    }

    #[test]
    fn collect_all_paths_tree_order_sorts_albums_by_year_across_artists() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: crate::library::shared_tracks(vec![
                track(
                    "/music/Zeppelin/Houses (1973)/01.flac",
                    "Houses (1973)",
                    Some(1973),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Zeppelin/Presence (1976)/01.flac",
                    "Presence (1976)",
                    Some(1976),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Floyd/Wall (1979)/01.flac",
                    "Wall (1979)",
                    Some(1979),
                    Some(1),
                    "Track 01",
                ),
                track(
                    "/music/Floyd/Meddle (1971)/01.flac",
                    "Meddle (1971)",
                    Some(1971),
                    Some(1),
                    "Track 01",
                ),
            ]),
            ..LibrarySnapshot::default()
        };

        let paths = collect_all_paths_tree_order(&library, LibrarySortMode::Year);
        assert_eq!(
            paths,
            vec![
                // Floyd first (alphabetical), albums by year
                PathBuf::from("/music/Floyd/Meddle (1971)/01.flac"),
                PathBuf::from("/music/Floyd/Wall (1979)/01.flac"),
                // Zeppelin second, albums by year
                PathBuf::from("/music/Zeppelin/Houses (1973)/01.flac"),
                PathBuf::from("/music/Zeppelin/Presence (1976)/01.flac"),
            ]
        );
    }
}
