use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::hash::BuildHasher;
use std::path::{Path, PathBuf};

use crate::library::{LibrarySnapshot, LibraryTrack};

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
    fn root(depth: u16, path: &str, child_count: usize) -> Self {
        Self {
            row_type: ROW_TYPE_ROOT,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u16(child_count),
            title: path.to_string(),
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
            title: format!("{} ({})", artist_name, album_count),
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
        title: &str,
        path: &str,
        cover_path: Option<&str>,
        child_count: usize,
        play_paths: Vec<String>,
    ) -> Self {
        Self {
            row_type: ROW_TYPE_ALBUM,
            depth,
            source_index: -1,
            track_number: 0,
            child_count: clamp_u16(child_count),
            title: title.to_string(),
            key,
            artist: artist_name.to_string(),
            path: path.to_string(),
            cover_path: cover_path.unwrap_or_default().to_string(),
            track_path: String::new(),
            play_paths,
        }
    }

    fn section(
        depth: u16,
        key: String,
        title: &str,
        path: &str,
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
            cover_path: String::new(),
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

pub fn build_library_tree_flat_binary<S: BuildHasher>(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
    expanded_keys: Option<&HashSet<String, S>>,
) -> Vec<u8> {
    let rows = build_library_tree_flat_rows(library, sort_mode, expanded_keys);
    encode_flat_rows(&rows)
}

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
        let Ok(rel) = track.path.strip_prefix(root) else {
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

        let root_key = root.to_string_lossy().to_string();
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
        let Ok(rel) = track.path.strip_prefix(root) else {
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

        let root_key = root.to_string_lossy().to_string();
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

    let mut builders: BTreeMap<String, RootNodeBuilder> = BTreeMap::new();
    for root in &roots {
        builders.insert(
            root.to_string_lossy().to_string(),
            RootNodeBuilder {
                root_path: root.clone(),
                artists: BTreeMap::new(),
            },
        );
    }

    for track in &library.tracks {
        let Some(root) = pick_root_for_track(&roots, track) else {
            continue;
        };
        let root_key = root.to_string_lossy().to_string();
        let Some(root_builder) = builders.get_mut(&root_key) else {
            continue;
        };

        let Ok(rel) = track.path.strip_prefix(root) else {
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

        let leaf = TrackLeaf {
            path: track.path.clone(),
            title: normalized_track_title(track),
            file_stem: track
                .path
                .file_stem()
                .map_or_else(String::new, |s| s.to_string_lossy().into_owned()),
            album_tag: track.album.trim().to_string(),
            year: track.year,
            track_no: track.track_no,
        };

        if components.is_empty() {
            continue;
        }

        let (artist_name, artist_path) = if components.len() >= 2 {
            (components[0].clone(), root.join(&components[0]))
        } else {
            let fallback = if track.artist.trim().is_empty() {
                String::from("Unknown Artist")
            } else {
                track.artist.trim().to_string()
            };
            (fallback, root.clone())
        };

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
            continue;
        }

        let album_folder = components[1].clone();
        let album_path = root.join(&artist_name).join(&album_folder);
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
            let section_name = components[2].clone();
            album_entry
                .sections
                .entry(section_name)
                .or_default()
                .push(leaf);
        } else {
            album_entry.root_tracks.push(leaf);
        }
    }

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
                root_builder.artists.len(),
            ));
        }
        rows.extend(artist_rows);
    }

    rows
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

        for album in resolved_albums {
            let album_path = album.folder_path.to_string_lossy().to_string();
            let album_title = if let Some(year) = album.year {
                format!("{} ({year})", album.title)
            } else {
                album.title.clone()
            };
            let album_key = album_row_key(root_path, &artist.artist_name, &album.folder_name);
            let album_child_count = album.root_tracks.len() + album.sections.len();
            let mut album_play_paths = Vec::with_capacity(
                album.root_tracks.len() + album.sections.len().saturating_mul(8),
            );
            for track in &album.root_tracks {
                album_play_paths.push(track.path.clone());
            }
            for section in &album.sections {
                for track in &section.tracks {
                    album_play_paths.push(track.path.clone());
                }
            }
            out.push(FlatTreeRow::album(
                album_depth,
                album_key.clone(),
                &artist.artist_name,
                &album_title,
                &album_path,
                album.cover_path.as_deref(),
                album_child_count,
                album_play_paths,
            ));

            let album_expanded = expanded_keys.is_none_or(|keys| keys.contains(&album_key));
            if lazy_hydration && !album_expanded {
                continue;
            }

            for track in &album.root_tracks {
                out.push(FlatTreeRow::track(
                    section_depth,
                    &artist.artist_name,
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
                out.push(FlatTreeRow::section(
                    section_depth,
                    section_row_key(
                        root_path,
                        &artist.artist_name,
                        &album.folder_name,
                        &section.name,
                    ),
                    &section.name,
                    &section_path,
                    section_play_paths,
                    section.tracks.len(),
                ));
                for track in &section.tracks {
                    out.push(FlatTreeRow::track(
                        section_track_depth,
                        &artist.artist_name,
                        &track.label,
                        &track.path,
                        track.number,
                    ));
                }
            }
        }
    }

    out
}

fn encode_flat_rows(rows: &[FlatTreeRow]) -> Vec<u8> {
    let mut out = Vec::with_capacity(rows.len() * 96 + 4);
    push_u32(&mut out, rows.len() as u32);
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
    value.min(u16::MAX as usize) as u16
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
    let len = bytes.len().min(u16::MAX as usize);
    push_u16(out, len as u16);
    out.extend_from_slice(&bytes[..len]);
}

fn resolve_album(album: &AlbumNodeBuilder) -> ResolvedAlbum {
    let mut all_tracks = Vec::new();
    all_tracks.extend(album.root_tracks.iter().cloned());
    for section_tracks in album.sections.values() {
        all_tracks.extend(section_tracks.iter().cloned());
    }

    let title = resolve_album_title(&all_tracks, &album.folder_name);
    let year = resolve_album_year(&all_tracks);

    let mut sections = album
        .sections
        .iter()
        .map(|(name, tracks)| ResolvedSection {
            name: name.clone(),
            path: album.folder_path.join(name),
            tracks: order_tracks(tracks),
        })
        .collect::<Vec<_>>();
    sections.sort_by(|a, b| natural_cmp(&a.name, &b.name));

    let cover_path = select_album_cover(&album.folder_path, &sections);

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
    let mut counts: HashMap<i32, usize> = HashMap::new();
    for year in all_tracks.iter().filter_map(|track| track.year) {
        *counts.entry(year).or_insert(0) += 1;
    }
    if counts.is_empty() {
        return None;
    }

    let mut items = counts.into_iter().collect::<Vec<_>>();
    items.sort_by(|(a_year, a_count), (b_year, b_count)| {
        b_count.cmp(a_count).then_with(|| a_year.cmp(b_year))
    });
    items.first().map(|(year, _)| *year)
}

fn select_album_cover(album_path: &Path, sections: &[ResolvedSection]) -> Option<String> {
    if let Some(path) = find_image_in_dir(album_path) {
        return Some(path);
    }

    for section in sections {
        if let Some(path) = find_image_in_dir(&section.path) {
            return Some(path);
        }
    }

    None
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
            number: candidate.number.min(u16::MAX as u32) as u16,
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

fn pick_root_for_track<'a>(roots: &'a [PathBuf], track: &LibraryTrack) -> Option<&'a PathBuf> {
    if !track.root_path.as_os_str().is_empty() {
        if let Some(root) = roots.iter().find(|root| *root == &track.root_path) {
            return Some(root);
        }
    }

    roots
        .iter()
        .filter(|root| track.path.starts_with(root))
        .max_by_key(|root| root.components().count())
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
    use crate::library::LibrarySnapshot;

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
        let len = read_u16(input, offset)? as usize;
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
        let mut rows = Vec::with_capacity(row_count as usize);
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
            let mut play_paths = Vec::with_capacity(play_path_count as usize);
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
            genre: String::new(),
            year,
            track_no,
            duration_secs: None,
        }
    }

    #[test]
    fn year_tie_breaks_to_earliest() {
        let album = AlbumNodeBuilder {
            folder_name: "Folder".to_string(),
            folder_path: PathBuf::from("/music/Artist/Folder"),
            root_tracks: vec![
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/01.flac"),
                    title: "A".to_string(),
                    file_stem: "01".to_string(),
                    album_tag: "Tag".to_string(),
                    year: Some(2024),
                    track_no: Some(1),
                },
                TrackLeaf {
                    path: PathBuf::from("/music/Artist/Folder/02.flac"),
                    title: "B".to_string(),
                    file_stem: "02".to_string(),
                    album_tag: "Tag".to_string(),
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
            roots: vec![PathBuf::from("/music")],
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
            roots: vec![PathBuf::from("/music-a"), PathBuf::from("/music-b")],
            tracks: vec![
                LibraryTrack {
                    path: PathBuf::from("/music-a/Artist/Album/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album".to_string(),
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
            roots: vec![PathBuf::from("/music-a"), PathBuf::from("/music-b")],
            tracks: vec![
                LibraryTrack {
                    path: PathBuf::from("/music-a/Same Artist/Album A/01.flac"),
                    root_path: PathBuf::from("/music-a"),
                    title: "Track A".to_string(),
                    artist: String::new(),
                    album: "Album A".to_string(),
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
            roots: vec![PathBuf::from("/music")],
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
            roots: vec![PathBuf::from("/music")],
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
                year: None,
                track_no: Some(1),
            },
            TrackLeaf {
                path: PathBuf::from("/music/a/10.flac"),
                title: "Ten".to_string(),
                file_stem: "10".to_string(),
                album_tag: String::new(),
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
            roots: vec![PathBuf::from("/music")],
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
}
