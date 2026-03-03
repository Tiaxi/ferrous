use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

use serde_json::json;

use crate::library::{LibrarySnapshot, LibraryTrack};

use super::LibrarySortMode;

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

struct TrackOrderCandidate {
    path: String,
    title: String,
    rank: u8,
    number: u32,
}

pub fn build_library_tree_json(
    library: &LibrarySnapshot,
    sort_mode: LibrarySortMode,
) -> serde_json::Value {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return serde_json::Value::Array(Vec::new());
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
    let mut top_rows = Vec::new();

    for (_, root_builder) in builders {
        let mut artist_rows = build_artist_rows(&root_builder, sort_mode);
        if multi_root {
            let root_path_string = root_builder.root_path.to_string_lossy().to_string();
            let mut root_play_paths = Vec::new();
            for artist in &artist_rows {
                if let Some(paths) = artist
                    .get("playPaths")
                    .and_then(serde_json::Value::as_array)
                {
                    for path in paths {
                        if let Some(path) = path.as_str() {
                            root_play_paths.push(path.to_string());
                        }
                    }
                }
            }
            let root_title = root_builder.root_path.to_string_lossy().to_string();
            let root_node = json!({
                "rowType": "root",
                "key": format!("root|{}", root_path_string),
                "title": root_title,
                "path": root_path_string,
                "playPaths": root_play_paths,
                "children": artist_rows,
            });
            top_rows.push(root_node);
        } else {
            top_rows.append(&mut artist_rows);
        }
    }

    serde_json::Value::Array(top_rows)
}

fn build_artist_rows(root: &RootNodeBuilder, sort_mode: LibrarySortMode) -> Vec<serde_json::Value> {
    let mut artists = root.artists.values().cloned().collect::<Vec<_>>();
    artists.sort_by(|a, b| natural_cmp(&a.artist_name, &b.artist_name));

    let mut out = Vec::new();
    for artist in artists {
        let album_count = artist.albums.len();
        let loose_tracks = order_tracks(&artist.loose_tracks);

        let mut resolved_albums = Vec::new();
        for album in artist.albums.values() {
            resolved_albums.push(resolve_album(album));
        }
        sort_resolved_albums(&mut resolved_albums, sort_mode);

        let mut artist_play_paths = Vec::new();
        let mut children = Vec::new();

        for track in &loose_tracks {
            artist_play_paths.push(track.path.clone());
            children.push(json!({
                "rowType": "track",
                "key": format!("track|{}", track.path),
                "title": track.label,
                "trackPath": track.path,
                "path": track.path,
                "playPaths": [track.path.clone()],
                "children": [],
            }));
        }

        for album in resolved_albums {
            let album_path = album.folder_path.to_string_lossy().to_string();
            let album_title = if let Some(year) = album.year {
                format!("{} ({year})", album.title)
            } else {
                album.title.clone()
            };

            let mut album_paths = Vec::new();
            let mut album_children = Vec::new();

            for track in &album.root_tracks {
                album_paths.push(track.path.clone());
                album_children.push(json!({
                    "rowType": "track",
                    "key": format!("track|{}", track.path),
                    "title": track.label,
                    "trackPath": track.path,
                    "path": track.path,
                    "playPaths": [track.path.clone()],
                    "children": [],
                }));
            }

            for section in &album.sections {
                let section_path = section.path.to_string_lossy().to_string();
                let mut section_paths = Vec::new();
                let mut section_children = Vec::new();
                for track in &section.tracks {
                    section_paths.push(track.path.clone());
                    section_children.push(json!({
                        "rowType": "track",
                        "key": format!("track|{}", track.path),
                        "title": track.label,
                        "trackPath": track.path,
                        "path": track.path,
                        "playPaths": [track.path.clone()],
                        "children": [],
                    }));
                }
                album_paths.extend(section_paths.iter().cloned());
                album_children.push(json!({
                    "rowType": "section",
                    "key": format!("section|{}", section_path),
                    "title": section.name,
                    "path": section_path,
                    "playPaths": section_paths,
                    "children": section_children,
                }));
            }

            artist_play_paths.extend(album_paths.iter().cloned());
            children.push(json!({
                "rowType": "album",
                "key": format!("album|{}", album_path),
                "title": album_title,
                "name": album.folder_name,
                "path": album_path,
                "coverPath": album.cover_path,
                "playPaths": album_paths,
                "children": album_children,
            }));
        }

        out.push(json!({
            "rowType": "artist",
            "key": format!("artist|{}|{}", root.root_path.to_string_lossy(), artist.artist_name),
            "title": format!("{} ({})", artist.artist_name, album_count),
            "artist": artist.artist_name,
            "path": artist.artist_path.to_string_lossy().to_string(),
            "count": album_count,
            "playPaths": artist_play_paths,
            "children": children,
        }));
    }

    out
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

        let tree = build_library_tree_json(&library, LibrarySortMode::Year);
        let rows = tree.as_array().expect("array");
        assert_eq!(rows.len(), 1);
        assert_eq!(
            rows[0].get("rowType").and_then(|v| v.as_str()),
            Some("artist")
        );
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
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: None,
                },
            ],
            ..LibrarySnapshot::default()
        };

        let tree = build_library_tree_json(&library, LibrarySortMode::Year);
        let rows = tree.as_array().expect("array");
        assert_eq!(rows.len(), 2);
        assert_eq!(
            rows[0].get("rowType").and_then(|v| v.as_str()),
            Some("root")
        );
        assert_eq!(
            rows[1].get("rowType").and_then(|v| v.as_str()),
            Some("root")
        );
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
    }
}
