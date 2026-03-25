use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::hash::BuildHasher;
use std::path::{Path, PathBuf};

use crate::library::{LibraryRoot, LibrarySnapshot, LibraryTrack};

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
    artist_path: PathBuf,
    loose_tracks: Vec<TrackLeaf>,
    albums: BTreeMap<String, AlbumNodeBuilder>,
}

#[derive(Debug, Clone)]
struct RootNodeBuilder {
    root_path: PathBuf,
    root_name: String,
    artists: BTreeMap<String, ArtistNodeBuilder>,
}

#[derive(Debug, Clone)]
struct OrderedTrack {
    label: String,
    path: String,
    number: u16,
}

#[derive(Debug, Clone)]
struct ResolvedAlbum {
    title: String,
    year: Option<i32>,
    folder_name: String,
    folder_path: PathBuf,
    cover_path: Option<String>,
    root_tracks: Vec<OrderedTrack>,
    sections: Vec<ResolvedSection>,
}

#[derive(Debug, Clone)]
struct ResolvedSection {
    name: String,
    path: PathBuf,
    cover_path: Option<String>,
    year: Option<i32>,
    tracks: Vec<OrderedTrack>,
}

#[derive(Debug, Clone)]
struct FlatTreeRow {
    row_type: u8,
    depth: u16,
    source_index: i32,
    track_number: u16,
    child_count: u16,
    title: String,
    key: String,
    artist: String,
    path: String,
    cover_path: String,
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
            child_count: clamp_u16(child_count),
            title: title.to_string(),
            key: root_row_key(path),
            artist: String::new(),
            path: path.to_string(),
            cover_path: String::new(),
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
            child_count: clamp_u16(child_count),
            title: format!("{artist_name} ({album_count})"),
            key,
            artist: artist_name.to_string(),
            path: path.to_string(),
            cover_path: String::new(),
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
            child_count: clamp_u16(album.root_tracks.len() + album.sections.len()),
            title: album_title.to_string(),
            key,
            artist: artist_name.to_string(),
            path: album_path.to_string(),
            cover_path: album.cover_path.as_deref().unwrap_or_default().to_string(),
            track_path: String::new(),
            play_paths,
        }
    }

    fn section(
        depth: u16,
        key: String,
        title: &str,
        path: &str,
        cover_path: &str,
        play_paths: Vec<String>,
        child_count: usize,
    ) -> Self {
        Self {
            row_type: ROW_TYPE_SECTION,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u16(child_count),
            title: title.to_string(),
            key,
            artist: String::new(),
            path: path.to_string(),
            cover_path: cover_path.to_string(),
            track_path: String::new(),
            play_paths,
        }
    }

    fn track(depth: u16, artist_name: &str, title: &str, path: &str, track_number: u16) -> Self {
        Self {
            row_type: ROW_TYPE_TRACK,
            depth,
            source_index: -1,
            track_number,
            child_count: 0,
            title: title.to_string(),
            key: format!("track|{path}"),
            artist: artist_name.to_string(),
            path: path.to_string(),
            cover_path: String::new(),
            track_path: path.to_string(),
            play_paths: vec![path.to_string()],
        }
    }
}

struct TrackOrderCandidate {
    path: String,
    title: String,
    rank: u8,
    number: u32,
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
    format!("root|{root_path}")
}

fn artist_row_key(root_path: &str, artist_name: &str) -> String {
    format!("artist|{root_path}|{artist_name}")
}

fn album_row_key(root_path: &str, artist_name: &str, folder_name: &str) -> String {
    format!("album|{root_path}|{artist_name}|{folder_name}")
}

fn section_row_key(
    root_path: &str,
    artist_name: &str,
    folder_name: &str,
    section_name: &str,
) -> String {
    format!("section|{root_path}|{artist_name}|{folder_name}|{section_name}")
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

#[must_use]
pub fn compute_artist_album_counts(library: &LibrarySnapshot) -> (usize, usize) {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return (0, 0);
    }

    let mut artists = HashSet::new();
    let mut albums = HashSet::new();
    for track in &library.tracks {
        let Some(root) = pick_root_for_track(&roots, track) else {
            continue;
        };
        let Ok(rel) = track.path.strip_prefix(&root.path) else {
            continue;
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
            continue;
        }

        let root_key = root.path.to_string_lossy().to_string();
        let artist_name = if components.len() >= 2 {
            components[0].clone()
        } else if track.artist.trim().is_empty() {
            String::from("Unknown Artist")
        } else {
            track.artist.trim().to_string()
        };
        artists.insert(artist_row_key(&root_key, &artist_name));

        if components.len() > 2 {
            albums.insert(album_row_key(&root_key, &artist_name, &components[1]));
        }
    }

    (artists.len(), albums.len())
}

pub fn retain_valid_expanded_keys<S: BuildHasher + Default>(
    library: &LibrarySnapshot,
    expanded_keys: &mut HashSet<String, S>,
) {
    if expanded_keys.is_empty() {
        return;
    }

    let roots = library.roots.clone();
    if roots.is_empty() {
        expanded_keys.clear();
        return;
    }

    let mut valid: HashSet<String> = HashSet::new();
    for track in &library.tracks {
        let Some(root) = pick_root_for_track(&roots, track) else {
            continue;
        };
        let Ok(rel) = track.path.strip_prefix(&root.path) else {
            continue;
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
            continue;
        }

        let root_key = root.path.to_string_lossy().to_string();
        let artist_name = if components.len() >= 2 {
            components[0].clone()
        } else if track.artist.trim().is_empty() {
            String::from("Unknown Artist")
        } else {
            track.artist.trim().to_string()
        };
        valid.insert(artist_row_key(&root_key, &artist_name));

        if components.len() > 2 {
            valid.insert(album_row_key(&root_key, &artist_name, &components[1]));
        }
    }

    expanded_keys.retain(|key| valid.contains(key));
}

fn build_library_tree_flat_rows<S: BuildHasher>(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
    expanded_keys: Option<&HashSet<String, S>>,
) -> Vec<FlatTreeRow> {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return Vec::new();
    }

    let builders = build_root_builders(&roots, &library.tracks);

    let multi_root = roots.len() >= 2;
    let mut rows = Vec::new();

    for (_, root_builder) in builders {
        let root_depth = if multi_root { 0 } else { u16::MAX };
        let artist_depth = u16::from(multi_root);
        let root_path = root_builder.root_path.to_string_lossy().to_string();
        let artist_rows = build_artist_rows_flat(
            &root_builder,
            &root_path,
            sort_mode,
            artist_depth,
            expanded_keys,
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

fn build_root_builders(
    roots: &[LibraryRoot],
    tracks: &[LibraryTrack],
) -> BTreeMap<String, RootNodeBuilder> {
    let mut builders: BTreeMap<String, RootNodeBuilder> = roots
        .iter()
        .map(|root| {
            (
                root.path.to_string_lossy().to_string(),
                RootNodeBuilder {
                    root_path: root.path.clone(),
                    root_name: root.display_name(),
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
        year: track.year,
        track_no: track.track_no,
    };
    let (artist_name, artist_path) = artist_parts(root, track, &components);
    let artist_entry = root_builder
        .artists
        .entry(artist_name.clone())
        .or_insert_with(|| ArtistNodeBuilder {
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
) -> Vec<FlatTreeRow> {
    let lazy_hydration = expanded_keys.is_some();
    let mut artists = root.artists.values().cloned().collect::<Vec<_>>();
    artists.sort_by(|a, b| natural_cmp(&a.artist_name, &b.artist_name));

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
            for album in artist.albums.values() {
                resolved_albums.push(resolve_album(album));
            }
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
            section_cover,
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
        push_u16(&mut out, row.track_number);
        push_u16(&mut out, row.child_count);
        push_u16_string(&mut out, &row.title);
        push_u16_string(&mut out, &row.key);
        push_u16_string(&mut out, &row.artist);
        push_u16_string(&mut out, &row.path);
        push_u16_string(&mut out, &row.cover_path);
        push_u16_string(&mut out, &row.track_path);
        push_u16(&mut out, clamp_u16(row.play_paths.len()));
        for path in &row.play_paths {
            push_u16_string(&mut out, path);
        }
    }
    out
}

fn clamp_u16(value: usize) -> u16 {
    u16::try_from(value.min(usize::from(u16::MAX))).unwrap_or(u16::MAX)
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

fn push_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn push_u16_string(out: &mut Vec<u8>, value: &str) {
    let bytes = value.as_bytes();
    let len = bytes.len().min(usize::from(u16::MAX));
    push_u16(out, u16::try_from(len).unwrap_or(u16::MAX));
    out.extend_from_slice(&bytes[..len]);
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

        let section_cover = resolve_album_cover(tracks)
            .or_else(|| find_image_in_dir(&album.folder_path.join(name)));
        let section_year = if is_disc {
            None // disc sections don't get independent year
        } else {
            resolve_album_year(tracks)
        };

        sections.push(ResolvedSection {
            name: name.clone(),
            path: album.folder_path.join(name),
            cover_path: if is_disc { None } else { section_cover },
            year: section_year,
            tracks: order_tracks(tracks),
        });
    }
    sections.sort_by(|a, b| natural_cmp(&a.name, &b.name));

    let title = resolve_album_title(&album_tracks, &album.folder_name);
    let year = resolve_album_year(&album_tracks);

    // Album cover fallback: embedded art from album tracks, then image in album dir,
    // then image in disc section dirs (not non-disc sections).
    let cover_path = resolve_album_cover(&album_tracks)
        .or_else(|| find_image_in_dir(&album.folder_path))
        .or_else(|| {
            sections
                .iter()
                .filter(|s| super::is_main_album_disc_section(&s.name))
                .find_map(|s| find_image_in_dir(&s.path))
        });

    ResolvedAlbum {
        title,
        year,
        folder_name: album.folder_name.clone(),
        folder_path: album.folder_path.clone(),
        cover_path,
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
                .then_with(|| natural_cmp(&a.title, &b.title))
        }
        LibrarySortMode::Title => natural_cmp(&a.title, &b.title).then_with(|| {
            let a_unknown = a.year.is_none();
            let b_unknown = b.year.is_none();
            a_unknown
                .cmp(&b_unknown)
                .then_with(|| a.year.unwrap_or(i32::MAX).cmp(&b.year.unwrap_or(i32::MAX)))
        }),
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

fn resolve_album_cover(all_tracks: &[TrackLeaf]) -> Option<String> {
    all_tracks
        .iter()
        .map(|track| track.cover_path.trim())
        .find(|path| !path.is_empty())
        .map(ToString::to_string)
}

fn find_image_in_dir(dir: &Path) -> Option<String> {
    let Ok(read_dir) = std::fs::read_dir(dir) else {
        return None;
    };

    let mut candidates = Vec::new();
    for entry in read_dir.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
            continue;
        };
        let ext = ext.to_ascii_lowercase();
        if ext == "jpg" || ext == "jpeg" || ext == "png" {
            candidates.push(path.to_string_lossy().to_string());
        }
    }
    if candidates.is_empty() {
        return None;
    }
    candidates.sort_by(|a, b| natural_cmp(a, b));
    candidates.into_iter().next()
}

fn order_tracks(tracks: &[TrackLeaf]) -> Vec<OrderedTrack> {
    if tracks.is_empty() {
        return Vec::new();
    }

    let mut positional = tracks
        .iter()
        .map(|track| track.path.to_string_lossy().to_string())
        .collect::<Vec<_>>();
    positional.sort_by(|a, b| natural_cmp(a, b));
    let positional_map = positional
        .into_iter()
        .enumerate()
        .map(|(idx, path)| (path, idx + 1))
        .collect::<HashMap<_, _>>();

    let mut candidates = tracks
        .iter()
        .map(|track| {
            let path = track.path.to_string_lossy().to_string();
            let filename_number = leading_track_number(&track.file_stem);
            let number = track.track_no.or(filename_number).unwrap_or_else(|| {
                positional_map
                    .get(&path)
                    .copied()
                    .unwrap_or(1)
                    .try_into()
                    .unwrap_or(1)
            });
            let rank = if track.track_no.is_some() {
                0
            } else if filename_number.is_some() {
                1
            } else {
                2
            };
            TrackOrderCandidate {
                path,
                title: track.title.clone(),
                rank,
                number,
            }
        })
        .collect::<Vec<_>>();

    candidates.sort_by(|a, b| {
        a.rank
            .cmp(&b.rank)
            .then_with(|| a.number.cmp(&b.number))
            .then_with(|| natural_cmp(&a.path, &b.path))
    });

    let max_number = candidates.iter().map(|c| c.number).max().unwrap_or(0);
    let width = std::cmp::max(2, max_number.to_string().len());

    candidates
        .into_iter()
        .map(|candidate| OrderedTrack {
            label: format!(
                "{:0width$} - {}",
                candidate.number,
                candidate.title,
                width = width
            ),
            path: candidate.path,
            number: u16::try_from(candidate.number.min(u32::from(u16::MAX))).unwrap_or(u16::MAX),
        })
        .collect()
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

fn natural_cmp(a: &str, b: &str) -> Ordering {
    let a = a.as_bytes();
    let b = b.as_bytes();
    let mut ia = 0usize;
    let mut ib = 0usize;

    while ia < a.len() && ib < b.len() {
        let ca = a[ia];
        let cb = b[ib];

        if ca.is_ascii_digit() && cb.is_ascii_digit() {
            let start_a = ia;
            let start_b = ib;
            while ia < a.len() && a[ia].is_ascii_digit() {
                ia += 1;
            }
            while ib < b.len() && b[ib].is_ascii_digit() {
                ib += 1;
            }

            let mut na = &a[start_a..ia];
            let mut nb = &b[start_b..ib];
            while na.len() > 1 && na[0] == b'0' {
                na = &na[1..];
            }
            while nb.len() > 1 && nb[0] == b'0' {
                nb = &nb[1..];
            }

            let cmp = na
                .len()
                .cmp(&nb.len())
                .then_with(|| na.cmp(nb))
                .then_with(|| (ia - start_a).cmp(&(ib - start_b)));
            if cmp != Ordering::Equal {
                return cmp;
            }
            continue;
        }

        let la = ca.to_ascii_lowercase();
        let lb = cb.to_ascii_lowercase();
        let cmp = la.cmp(&lb);
        if cmp != Ordering::Equal {
            return cmp;
        }
        ia += 1;
        ib += 1;
    }

    a.len().cmp(&b.len())
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
        track_number: u16,
        child_count: u16,
        title: String,
        key: String,
        artist: String,
        path: String,
        cover_path: String,
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

    fn read_u16_string(input: &[u8], offset: &mut usize) -> Option<String> {
        let len = usize::from(read_u16(input, offset)?);
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
            let Some(track_number) = read_u16(bytes, &mut offset) else {
                break;
            };
            let Some(child_count) = read_u16(bytes, &mut offset) else {
                break;
            };
            let Some(title) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(key) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(artist) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(path) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(cover_path) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(track_path) = read_u16_string(bytes, &mut offset) else {
                break;
            };
            let Some(play_path_count) = read_u16(bytes, &mut offset) else {
                break;
            };
            let mut play_paths = Vec::with_capacity(usize::from(play_path_count));
            let mut ok = true;
            for _ in 0..play_path_count {
                let Some(play_path) = read_u16_string(bytes, &mut offset) else {
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
            path: PathBuf::from(path),
            root_path: PathBuf::from("/music"),
            title: title.to_string(),
            artist: String::new(),
            album: album.to_string(),
            cover_path: String::new(),
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
                    year: Some(2024),
                    track_no: Some(1),
                },
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/02.flac"),
                    title: "B".to_string(),
                    file_stem: "02".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
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
                    year: Some(2023),
                    track_no: Some(1),
                },
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/02.flac"),
                    title: "B".to_string(),
                    file_stem: "02".to_string(),
                    album_tag: "Tag".to_string(),
                    cover_path: String::new(),
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
            tracks: vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track",
            )],
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
            tracks: vec![
                LibraryTrack {
                    path: PathBuf::from("/music-a/Artist/Album/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
                LibraryTrack {
                    path: PathBuf::from("/music-b/Artist/Album/01.flac"),
                    root_path: PathBuf::from("/music-b"),
                    title: "Track B".to_string(),
                    artist: String::new(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
            ],
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
            tracks: vec![
                LibraryTrack {
                    path: PathBuf::from("/music-a/Same Artist/Album A/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album A".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
                LibraryTrack {
                    path: PathBuf::from("/music-b/Same Artist/Album B/01.flac"),
                    root_path: PathBuf::from("/music-b"),
                    title: "Track B".to_string(),
                    artist: String::new(),
                    album: "Album B".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2021),
                    track_no: Some(1),
                    duration_secs: None,
                },
            ],
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
            tracks: vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )],
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

    #[test]
    fn retain_valid_expanded_keys_prunes_missing_nodes() {
        let library = LibrarySnapshot {
            roots: vec![root("/music")],
            tracks: vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )],
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
    fn track_label_uses_zero_padded_number() {
        let ordered = order_tracks(&[
            TrackLeaf {
                path: PathBuf::from("/music/a/1.flac"),
                title: "One".to_string(),
                file_stem: "1".to_string(),
                album_tag: String::new(),
                cover_path: String::new(),
                year: None,
                track_no: Some(1),
            },
            TrackLeaf {
                path: PathBuf::from("/music/a/10.flac"),
                title: "Ten".to_string(),
                file_stem: "10".to_string(),
                album_tag: String::new(),
                cover_path: String::new(),
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
            tracks: vec![track(
                "/music/Artist A/Album A/01.flac",
                "Album A",
                Some(2020),
                Some(1),
                "Track 01",
            )],
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
            year: None,
            folder_name: "Muut".to_string(),
            folder_path: PathBuf::from("/music/PT/Muut"),
            cover_path: None,
            root_tracks: vec![],
            sections: vec![ResolvedSection {
                name: "Lightbulb Sun".to_string(),
                path: PathBuf::from("/music/PT/Muut/Lightbulb Sun"),
                cover_path: Some("/cover.jpg".to_string()),
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
    }
}
