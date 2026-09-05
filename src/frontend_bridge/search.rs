// SPDX-License-Identifier: GPL-3.0-or-later

use std::cmp::Ordering;
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use crossbeam_channel::{Receiver, Sender};
use unicode_normalization::UnicodeNormalization;

use crate::library::{LibraryRoot, LibrarySearchTrack, LibrarySnapshot};

use super::search_query::SearchQuery;

use super::{
    BridgeSearchResultRow, BridgeSearchResultRowType, BridgeSearchResultsFrame, BridgeState,
};

#[cfg(feature = "profiling-logs")]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {
        eprintln!($($arg)*);
    };
}

#[cfg(not(feature = "profiling-logs"))]
macro_rules! profile_eprintln {
    ($($arg:tt)*) => {};
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub(super) struct TreePathContext {
    pub(super) artist_name: String,
    pub(super) artist_key: String,
    pub(super) root_label: String,
    pub(super) album_folder: Option<String>,
    pub(super) album_key: Option<String>,
    pub(super) section_key: Option<String>,
    pub(super) track_key: String,
    pub(super) is_main_level_album_track: bool,
    pub(super) is_disc_section_album_track: bool,
}

#[derive(Default)]
struct HitAlbumAcc {
    match_detail: String,
    artist_name: String,
    album_title: String,
    artist_key: String,
    root_label: String,
    year_counts: HashMap<i32, usize>,
    genre_counts: HashMap<String, usize>,
}

struct SearchResultLimits {
    artist: usize,
    album: usize,
    track: usize,
}

type SearchGroupMap = HashMap<String, (f32, String, String)>;

struct SearchRowBuckets {
    track_count: u32,
    track_rows: Vec<BridgeSearchResultRow>,
    album_cover_paths: HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: HashMap<String, HitAlbumAcc>,
}

struct SearchRowAccumulator {
    track_count: u32,
    roots: Vec<LibraryRoot>,
    roots_by_path: HashMap<PathBuf, PreparedSearchRoot>,
    album_cover_paths: HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: HashMap<String, HitAlbumAcc>,
    track_rows: Vec<BridgeSearchResultRow>,
}

impl SearchRowAccumulator {
    fn new(roots: Vec<LibraryRoot>) -> Self {
        Self {
            track_count: 0,
            roots_by_path: roots_by_path_for_search(&roots),
            roots,
            album_cover_paths: HashMap::new(),
            artist_groups: HashMap::new(),
            album_groups: HashMap::new(),
            album_hit_stats: HashMap::new(),
            track_rows: Vec::new(),
        }
    }

    fn push_hit(
        &mut self,
        hit: &LibrarySearchTrack,
        query: &SearchQuery,
        fields: &BTreeMap<String, String>,
    ) -> Option<BridgeSearchResultRow> {
        let context = derive_hit_context(hit, &self.roots, &self.roots_by_path)?;
        self.track_count = self.track_count.saturating_add(1);
        let hit_path_string = hit.path.to_string_lossy().to_string();
        let hit_artist = if hit.artist.trim().is_empty() {
            context.artist_name.clone()
        } else {
            hit.artist.trim().to_string()
        };
        let hit_album = if hit.album.trim().is_empty() {
            context
                .album_folder
                .clone()
                .unwrap_or_else(|| String::from("Unknown Album"))
        } else {
            hit.album.trim().to_string()
        };
        let album_key = context.album_key.clone();
        let artist_fields =
            BTreeMap::from([("artist".into(), normalize_for_search(&context.artist_name))]);
        if query.matches(&artist_fields) {
            let score = entity_score(
                &query.name_terms("artist"),
                &normalize_for_search(&context.artist_name),
                &[],
            );
            let artist_entry = self
                .artist_groups
                .entry(context.artist_key.clone())
                .or_insert((
                    score,
                    context.artist_name.clone(),
                    context.root_label.clone(),
                ));
            if score < artist_entry.0 {
                artist_entry.0 = score;
                artist_entry.1.clone_from(&context.artist_name);
                artist_entry.2.clone_from(&context.root_label);
            }
        }
        self.push_album(hit, &context, &hit_artist, &hit_album, query, fields);
        let row_cover_path = if let Some(album_key_value) = album_key.clone() {
            if !hit.cover_path.is_empty() {
                self.album_cover_paths
                    .entry(album_key_value.clone())
                    .or_insert_with(|| hit.cover_path.clone());
            }
            self.album_cover_paths
                .get(&album_key_value)
                .cloned()
                .unwrap_or_else(|| hit.cover_path.clone())
        } else {
            hit.cover_path.clone()
        };
        Some(build_track_search_result_row(
            hit,
            &context,
            &hit_artist,
            &hit_album,
            album_key,
            hit_path_string,
            row_cover_path,
        ))
    }

    fn push_album(
        &mut self,
        hit: &LibrarySearchTrack,
        context: &TreePathContext,
        hit_artist: &str,
        hit_album: &str,
        query: &SearchQuery,
        fields: &BTreeMap<String, String>,
    ) {
        // Match the main-album queue: bonus folders remain searchable as tracks.
        if !context.is_main_level_album_track && !context.is_disc_section_album_track {
            return;
        }
        if let Some(album_key_value) = context.album_key.clone() {
            let context_artist = normalize_for_search(&context.artist_name);
            let album_name = normalize_for_search(hit_album);
            if query.matches_with(
                fields,
                |key| !matches!(key, "title" | "path" | "track" | "disc"),
                &[("artist", &context_artist), ("album", &album_name)],
            ) {
                let score = entity_score(
                    &query.name_terms("album"),
                    &normalize_for_search(hit_album),
                    &[
                        &normalize_for_search(hit_artist),
                        &normalize_for_search(&context.artist_name),
                    ],
                );
                let score = metadata_score(query, fields, score, false);
                let album_entry = self.album_groups.entry(album_key_value.clone()).or_insert((
                    score,
                    hit_album.to_string(),
                    context.root_label.clone(),
                ));
                if score < album_entry.0 {
                    album_entry.0 = score;
                    album_entry.1 = hit_album.to_string();
                    album_entry.2.clone_from(&context.root_label);
                }
                update_album_hit_stats(
                    &mut self.album_hit_stats,
                    album_key_value.clone(),
                    context,
                    hit_album,
                    hit.year,
                    hit.genre.trim(),
                );
                if let Some(stats) = self.album_hit_stats.get_mut(&album_key_value) {
                    stats.match_detail = hidden_match_detail(
                        query,
                        fields,
                        &[
                            ("artist", hit_artist),
                            ("album", hit_album),
                            ("genre", &hit.genre),
                            (
                                "year",
                                &hit.year.map_or_else(String::new, |year| year.to_string()),
                            ),
                        ],
                    );
                }
            }
        }
    }

    fn finish(self) -> SearchRowBuckets {
        SearchRowBuckets {
            track_count: self.track_count,
            track_rows: self.track_rows,
            album_cover_paths: self.album_cover_paths,
            artist_groups: self.artist_groups,
            album_groups: self.album_groups,
            album_hit_stats: self.album_hit_stats,
        }
    }
}

#[derive(Debug, Clone)]
struct PreparedSearchTrack {
    path: PathBuf,
    root_path: PathBuf,
    title: String,
    artist: String,
    album: String,
    cover_path: String,
    genre: String,
    year: Option<i32>,
    track_no: Option<u32>,
    duration_secs: Option<f32>,
    title_l: String,
    artist_l: String,
    album_l: String,
    fields: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Default)]
struct PreparedSearchLibrary {
    tracks: Vec<PreparedSearchTrack>,
    album_inventory: HashMap<String, AlbumInventoryAcc>,
}

#[derive(Default)]
pub(super) struct SearchWorkerPreparedCache {
    source_library: Option<Arc<LibrarySnapshot>>,
    source_search_revision: Option<u64>,
    prepared: Option<Arc<PreparedSearchLibrary>>,
}

impl SearchWorkerPreparedCache {
    #[cfg_attr(
        not(feature = "profiling-logs"),
        allow(unused_variables, unused_assignments)
    )]
    fn prepared_for(&mut self, library: &Arc<LibrarySnapshot>) -> Arc<PreparedSearchLibrary> {
        if let (Some(source), Some(prepared)) = (&self.source_library, &self.prepared) {
            let revision = library.search_revision;
            if revision != 0 && self.source_search_revision == Some(revision) {
                self.source_library = Some(Arc::clone(library));
                return Arc::clone(prepared);
            }
            if revision == 0 && Arc::ptr_eq(source, library) {
                return Arc::clone(prepared);
            }
        }
        #[allow(unused_variables)]
        let started = Instant::now();
        let prepared = Arc::new(prepare_search_library(library.as_ref()));
        if search_profile_enabled() {
            profile_eprintln!(
                "[search-worker] cache rebuild tracks={} elapsed_ms={}",
                prepared.tracks.len(),
                started.elapsed().as_millis()
            );
        }
        self.source_library = Some(Arc::clone(library));
        self.source_search_revision =
            (library.search_revision != 0).then_some(library.search_revision);
        self.prepared = Some(Arc::clone(&prepared));
        prepared
    }
}

pub(super) enum SearchBuildOutcome {
    Frame(BridgeSearchResultsFrame),
    Cancelled(SearchWorkerQuery),
}

enum PreparedSearchOutcome {
    Hits(SearchRowBuckets),
    Cancelled(SearchWorkerQuery),
}

#[derive(Debug, Clone, Default)]
struct AlbumInventoryAcc {
    main_track_count: u32,
    main_total_length: f32,
    has_main_duration: bool,
}

#[derive(Debug)]
pub(super) struct SearchWorkerQuery {
    pub(super) limit: u32,
    pub(super) seq: u32,
    pub(super) query: String,
    pub(super) library: Arc<LibrarySnapshot>,
}

#[derive(Clone)]
pub(super) struct PreparedSearchRoot {
    pub(super) path: PathBuf,
    pub(super) root_key: String,
    pub(super) root_label: String,
}

struct RankedSearchRow(BridgeSearchResultRow);

impl PartialEq for RankedSearchRow {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}
impl Eq for RankedSearchRow {}
impl PartialOrd for RankedSearchRow {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for RankedSearchRow {
    fn cmp(&self, other: &Self) -> Ordering {
        search_row_cmp(&self.0, &other.0)
    }
}

// ---------------------------------------------------------------------------
// Config / limit functions
// ---------------------------------------------------------------------------

fn search_profile_enabled() -> bool {
    cfg!(feature = "profiling-logs") && std::env::var_os("FERROUS_SEARCH_PROFILE").is_some()
}

fn search_short_query_char_threshold() -> usize {
    std::env::var("FERROUS_SEARCH_SHORT_QUERY_CHARS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(1, |v| v.clamp(1, 8))
}

fn search_artist_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ARTIST_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(5, |v| v.clamp(1, 400))
}

fn search_artist_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_ARTIST_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(5, |v| v.clamp(1, 400))
}

fn search_album_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_ALBUM_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(10, |v| v.clamp(1, 800))
}

fn search_album_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_ALBUM_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(10, |v| v.clamp(1, 800))
}

fn search_track_row_limit() -> usize {
    std::env::var("FERROUS_SEARCH_TRACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(20, |v| v.clamp(1, 2_000))
}

fn search_track_row_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_TRACK_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(20, |v| v.clamp(1, 2_000))
}

fn search_cancel_poll_rows() -> usize {
    std::env::var("FERROUS_SEARCH_CANCEL_POLL_ROWS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(64, |v| v.clamp(16, 4_096))
}

// ---------------------------------------------------------------------------
// Normalize / split / match
// ---------------------------------------------------------------------------

/// Lowercase and strip diacritical marks so that e.g. "jonsi" matches "Jónsi".
/// Uses NFKD decomposition and then removes combining characters (Unicode
/// category Mark, Nonspacing — `Mn`).
pub(super) fn normalize_for_search(text: &str) -> String {
    text.nfkd()
        .filter(|ch| !unicode_normalization::char::is_combining_mark(*ch))
        .collect::<String>()
        .to_lowercase()
}

pub(super) fn split_search_terms(query: &str) -> Vec<String> {
    query
        .split_whitespace()
        .map(|term| normalize_for_search(term.trim()))
        .filter(|term| !term.is_empty())
        .collect::<Vec<_>>()
}

#[cfg(test)]
fn query_terms_match_text(terms: &[String], text: &str) -> bool {
    if terms.is_empty() {
        return false;
    }
    let text_n = normalize_for_search(text);
    terms.iter().all(|term| text_n.contains(term))
}

// ---------------------------------------------------------------------------
// Disc section detection
// ---------------------------------------------------------------------------

pub(crate) fn is_main_album_disc_section(section_name: &str) -> bool {
    let section = section_name.trim().to_ascii_lowercase();
    if section.is_empty() {
        return false;
    }
    for prefix in ["cd", "disc", "disk", "dvd"] {
        let Some(rest) = section.strip_prefix(prefix) else {
            continue;
        };
        let mut saw_digit = false;
        let mut valid = true;
        for ch in rest.chars() {
            if ch.is_ascii_digit() {
                saw_digit = true;
                continue;
            }
            if !saw_digit && matches!(ch, ' ' | '-' | '_' | '.') {
                continue;
            }
            if saw_digit && matches!(ch, ' ' | '-' | '_' | '.' | '(' | ')' | '[' | ']') {
                continue;
            }
            if saw_digit && ch.is_ascii_alphabetic() {
                continue;
            }
            valid = false;
            break;
        }
        if valid && saw_digit {
            return true;
        }
    }
    false
}

// ---------------------------------------------------------------------------
// Tree path context
// ---------------------------------------------------------------------------

fn pick_root_for_path<'a>(roots: &'a [LibraryRoot], path: &Path) -> Option<&'a LibraryRoot> {
    roots
        .iter()
        .filter(|root| path.starts_with(&root.path))
        .max_by_key(|root| root.path.components().count())
}

pub(super) fn derive_tree_path_context_for_root(
    path: &Path,
    root: &PreparedSearchRoot,
    fallback_artist: &str,
) -> Option<TreePathContext> {
    let rel = path.strip_prefix(&root.path).ok()?;
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
        return None;
    }

    let artist_name = if components.len() >= 2 {
        components[0].clone()
    } else if fallback_artist.trim().is_empty() {
        String::from("Unknown Artist")
    } else {
        fallback_artist.trim().to_string()
    };
    let artist_key = format!("artist|{}|{artist_name}", root.root_key);
    let track_path = path.to_string_lossy().to_string();
    let track_key = format!("track|{track_path}");

    if components.len() <= 2 {
        return Some(TreePathContext {
            artist_name,
            artist_key,
            root_label: root.root_label.clone(),
            album_folder: None,
            album_key: None,
            section_key: None,
            track_key,
            is_main_level_album_track: false,
            is_disc_section_album_track: false,
        });
    }

    let album_folder = components[1].clone();
    let album_key = format!("album|{}|{artist_name}|{album_folder}", root.root_key);
    let section_key = if components.len() >= 4 {
        Some(format!(
            "section|{}|{artist_name}|{album_folder}|{}",
            root.root_key, components[2]
        ))
    } else {
        None
    };
    let is_main_level_album_track = components.len() == 3;
    let is_disc_section_album_track =
        components.len() == 4 && is_main_album_disc_section(&components[2]);
    Some(TreePathContext {
        artist_name: artist_name.clone(),
        artist_key,
        root_label: root.root_label.clone(),
        album_folder: Some(album_folder.clone()),
        album_key: Some(album_key),
        section_key,
        track_key,
        is_main_level_album_track,
        is_disc_section_album_track,
    })
}

pub(super) fn derive_tree_path_context(
    path: &Path,
    roots: &[LibraryRoot],
    fallback_artist: &str,
) -> Option<TreePathContext> {
    let root = pick_root_for_path(roots, path)?;
    let prepared = PreparedSearchRoot {
        path: root.path.clone(),
        root_key: root.path.to_string_lossy().to_string(),
        root_label: root.search_label(),
    };
    derive_tree_path_context_for_root(path, &prepared, fallback_artist)
}

// ---------------------------------------------------------------------------
// Search building helpers
// ---------------------------------------------------------------------------

fn update_album_hit_stats(
    album_hit_stats: &mut HashMap<String, HitAlbumAcc>,
    album_key: String,
    context: &TreePathContext,
    hit_album: &str,
    year: Option<i32>,
    genre: &str,
) {
    let stats_entry = album_hit_stats.entry(album_key).or_default();
    if stats_entry.artist_name.is_empty() {
        stats_entry.artist_name.clone_from(&context.artist_name);
    }
    if stats_entry.artist_key.is_empty() {
        stats_entry.artist_key.clone_from(&context.artist_key);
    }
    if stats_entry.root_label.is_empty() {
        stats_entry.root_label.clone_from(&context.root_label);
    }
    if stats_entry.album_title.is_empty() {
        stats_entry.album_title.clone_from(&hit_album.to_string());
    }
    if let Some(year) = year {
        *stats_entry.year_counts.entry(year).or_insert(0) += 1;
    }
    if !genre.is_empty() {
        *stats_entry
            .genre_counts
            .entry(genre.to_string())
            .or_insert(0) += 1;
    }
}

fn build_track_search_result_row(
    hit: &LibrarySearchTrack,
    context: &TreePathContext,
    hit_artist: &str,
    hit_album: &str,
    album_key: Option<String>,
    hit_path_string: String,
    cover_path: String,
) -> BridgeSearchResultRow {
    BridgeSearchResultRow {
        match_detail: String::new(),
        row_type: BridgeSearchResultRowType::Track,
        score: hit.score,
        year: hit.year,
        track_number: hit.track_no,
        count: 0,
        length_seconds: hit.duration_secs,
        label: if hit.title.trim().is_empty() {
            hit.path
                .file_name()
                .map_or_else(String::new, |name| name.to_string_lossy().to_string())
        } else {
            hit.title.trim().to_string()
        },
        artist: hit_artist.to_string(),
        album: hit_album.to_string(),
        root_label: context.root_label.clone(),
        genre: hit.genre.trim().to_string(),
        cover_path,
        artist_key: context.artist_key.clone(),
        album_key: album_key.unwrap_or_default(),
        section_key: context.section_key.clone().unwrap_or_default(),
        track_key: context.track_key.clone(),
        track_path: hit_path_string,
    }
}

fn empty_search_results_frame(seq: u32) -> SearchBuildOutcome {
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame {
        seq,
        rows: Vec::new(),
        totals: [0; 3],
    })
}

fn search_result_limits(query_text: &str) -> SearchResultLimits {
    let is_short_query = query_text.chars().count() <= search_short_query_char_threshold();
    SearchResultLimits {
        artist: if is_short_query {
            search_artist_row_limit_short()
        } else {
            search_artist_row_limit()
        },
        album: if is_short_query {
            search_album_row_limit_short()
        } else {
            search_album_row_limit()
        },
        track: if is_short_query {
            search_track_row_limit_short()
        } else {
            search_track_row_limit()
        },
    }
}

fn choose_most_common_year(counts: &HashMap<i32, usize>) -> Option<i32> {
    let mut best: Option<(i32, usize)> = None;
    for (&year, &count) in counts {
        best = match best {
            Some((best_year, best_count))
                if count > best_count || (count == best_count && year < best_year) =>
            {
                Some((year, count))
            }
            None => Some((year, count)),
            other => other,
        };
    }
    best.map(|(year, _)| year)
}

fn choose_most_common_genre(counts: &HashMap<String, usize>) -> String {
    let mut best: Option<(&str, usize)> = None;
    for (genre, &count) in counts {
        let key = genre.as_str();
        best = match best {
            Some((best_genre, best_count))
                if count > best_count || (count == best_count && key < best_genre) =>
            {
                Some((key, count))
            }
            None => Some((key, count)),
            other => other,
        };
    }
    best.map_or_else(String::new, |(genre, _)| genre.to_string())
}

fn finalize_search_rows(
    album_inventory: &HashMap<String, AlbumInventoryAcc>,
    limits: &SearchResultLimits,
    album_cover_paths: &HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: &HashMap<String, HitAlbumAcc>,
    mut track_rows: Vec<BridgeSearchResultRow>,
) -> Vec<BridgeSearchResultRow> {
    let mut artist_rows = artist_groups
        .into_iter()
        .map(
            |(artist_key, (score, artist_name, root_label))| BridgeSearchResultRow {
                match_detail: String::new(),
                row_type: BridgeSearchResultRowType::Artist,
                score,
                year: None,
                track_number: None,
                count: 0,
                length_seconds: None,
                label: artist_name.clone(),
                artist: artist_name,
                album: String::new(),
                root_label,
                genre: String::new(),
                cover_path: String::new(),
                artist_key,
                album_key: String::new(),
                section_key: String::new(),
                track_key: String::new(),
                track_path: String::new(),
            },
        )
        .collect::<Vec<_>>();

    let mut album_rows = album_groups
        .into_iter()
        .filter_map(|(album_key, (score, fallback_title, root_label))| {
            let stats = album_hit_stats.get(&album_key)?;
            let inventory = album_inventory.get(&album_key);
            Some(BridgeSearchResultRow {
                match_detail: stats.match_detail.clone(),
                row_type: BridgeSearchResultRowType::Album,
                score,
                year: choose_most_common_year(&stats.year_counts),
                track_number: None,
                count: inventory.map_or(0, |value| value.main_track_count),
                length_seconds: inventory
                    .and_then(|value| value.has_main_duration.then_some(value.main_total_length)),
                label: if stats.album_title.is_empty() {
                    fallback_title
                } else {
                    stats.album_title.clone()
                },
                artist: stats.artist_name.clone(),
                album: if stats.album_title.is_empty() {
                    String::new()
                } else {
                    stats.album_title.clone()
                },
                root_label: if stats.root_label.is_empty() {
                    root_label
                } else {
                    stats.root_label.clone()
                },
                genre: choose_most_common_genre(&stats.genre_counts),
                cover_path: album_cover_paths
                    .get(&album_key)
                    .cloned()
                    .unwrap_or_default(),
                artist_key: stats.artist_key.clone(),
                album_key,
                section_key: String::new(),
                track_key: String::new(),
                track_path: String::new(),
            })
        })
        .collect::<Vec<_>>();

    artist_rows.sort_by(search_row_cmp);
    album_rows.sort_by(search_row_cmp);
    track_rows.sort_by(search_row_cmp);
    artist_rows.truncate(limits.artist);
    album_rows.truncate(limits.album);
    track_rows.truncate(limits.track);

    let mut rows = Vec::with_capacity(artist_rows.len() + album_rows.len() + track_rows.len());
    rows.extend(artist_rows);
    rows.extend(album_rows);
    rows.extend(track_rows);
    rows
}

fn search_row_cmp(a: &BridgeSearchResultRow, b: &BridgeSearchResultRow) -> Ordering {
    a.score
        .partial_cmp(&b.score)
        .unwrap_or(Ordering::Equal)
        .then_with(|| a.label.to_lowercase().cmp(&b.label.to_lowercase()))
        .then_with(|| a.artist.to_lowercase().cmp(&b.artist.to_lowercase()))
        .then_with(|| a.artist_key.cmp(&b.artist_key))
        .then_with(|| a.album_key.cmp(&b.album_key))
        .then_with(|| {
            a.track_path
                .to_lowercase()
                .cmp(&b.track_path.to_lowercase())
        })
}

// ---------------------------------------------------------------------------
// Roots / hit context / inventory
// ---------------------------------------------------------------------------

fn roots_by_path_for_search(roots: &[LibraryRoot]) -> HashMap<PathBuf, PreparedSearchRoot> {
    roots
        .iter()
        .map(|root| {
            (
                root.path.clone(),
                PreparedSearchRoot {
                    path: root.path.clone(),
                    root_key: root.path.to_string_lossy().to_string(),
                    root_label: root.search_label(),
                },
            )
        })
        .collect::<HashMap<_, _>>()
}

fn derive_hit_context(
    hit: &LibrarySearchTrack,
    roots: &[LibraryRoot],
    roots_by_path: &HashMap<PathBuf, PreparedSearchRoot>,
) -> Option<TreePathContext> {
    roots_by_path
        .get(&hit.root_path)
        .and_then(|root| derive_tree_path_context_for_root(&hit.path, root, &hit.artist))
        .or_else(|| derive_tree_path_context(&hit.path, roots, &hit.artist))
}

fn prepare_search_library(library: &LibrarySnapshot) -> PreparedSearchLibrary {
    let roots = library.roots.clone();
    if roots.is_empty() {
        return PreparedSearchLibrary::default();
    }
    let roots_by_path = roots_by_path_for_search(&roots);

    let mut tracks = Vec::with_capacity(library.tracks.len());
    let mut album_inventory: HashMap<String, AlbumInventoryAcc> = HashMap::new();

    for track in &library.tracks {
        let path_string = track.path.to_string_lossy().to_string();
        let title = track.title.trim().to_string();
        let artist = track.artist.trim().to_string();
        let album = track.album.trim().to_string();
        let genre = track.genre.trim().to_string();
        let title_l = normalize_for_search(&title);
        let artist_l = normalize_for_search(&artist);
        let album_l = normalize_for_search(&album);
        let genre_l = normalize_for_search(&genre);
        let path_n = normalize_for_search(&path_string);
        let mut fields: BTreeMap<String, String> = track
            .search_tags
            .iter()
            .map(|(key, values)| (key.clone(), normalize_for_search(&values.join(" "))))
            .collect();
        for (key, value) in [
            ("title", &title_l),
            ("artist", &artist_l),
            ("album", &album_l),
            ("genre", &genre_l),
            ("path", &path_n),
        ] {
            let field = fields.entry(key.into()).or_default();
            field.push(' ');
            field.push_str(value);
        }
        if let Some(year) = track.year {
            fields.insert("year".into(), year.to_string());
        }
        if let Some(number) = track.track_no {
            fields.insert("track".into(), number.to_string());
        }
        if let Some(root) = roots_by_path.get(&track.root_path) {
            fields.insert("root".into(), normalize_for_search(&root.root_label));
        }

        if let Some(context) = roots_by_path
            .get(&track.root_path)
            .and_then(|root| derive_tree_path_context_for_root(&track.path, root, &artist))
        {
            if let Some(album_key) = context.album_key.clone() {
                let include_in_main_album =
                    context.is_main_level_album_track || context.is_disc_section_album_track;
                let inventory = album_inventory.entry(album_key).or_default();
                if include_in_main_album {
                    inventory.main_track_count = inventory.main_track_count.saturating_add(1);
                    if let Some(duration) = track.duration_secs {
                        if duration.is_finite() && duration >= 0.0 {
                            inventory.main_total_length += duration;
                            inventory.has_main_duration = true;
                        }
                    }
                }
            }
        }

        tracks.push(PreparedSearchTrack {
            path: track.path.clone(),
            root_path: track.root_path.clone(),
            title,
            artist,
            album,
            cover_path: track.cover_path.clone(),
            genre,
            year: track.year,
            track_no: track.track_no,
            duration_secs: track.duration_secs,
            title_l,
            artist_l,
            album_l,
            fields,
        });
    }

    PreparedSearchLibrary {
        tracks,
        album_inventory,
    }
}

// ---------------------------------------------------------------------------
// Prepared library search
// ---------------------------------------------------------------------------

/// Rank an entity by its own name before supporting metadata. Scores are
/// independent of library size, tag repetition, and filesystem path length.
fn entity_score(terms: &[String], name: &str, secondary: &[&str]) -> f32 {
    if terms.is_empty() {
        return 5.0;
    }
    let phrase = terms.join(" ");
    if name == phrase {
        return 0.0;
    }
    if name.starts_with(&phrase) {
        return 1.0;
    }
    if name.match_indices(&phrase).any(|(offset, _)| {
        name[..offset]
            .chars()
            .next_back()
            .is_none_or(|ch| !ch.is_alphanumeric())
    }) {
        return 2.0;
    }
    if name.contains(&phrase) {
        return 3.0;
    }
    if terms.iter().all(|term| name.contains(term)) {
        return 3.5;
    }
    if terms
        .iter()
        .all(|term| name.contains(term) || secondary.iter().any(|field| field.contains(term)))
    {
        return 4.0;
    }
    5.0
}

fn metadata_score(
    query: &SearchQuery,
    fields: &BTreeMap<String, String>,
    score: f32,
    include_title: bool,
) -> f32 {
    if score < 5.0 {
        return score;
    }
    if query.matches_with(
        fields,
        |key| {
            (include_title || key != "title")
                && !matches!(key, "comment" | "lyrics" | "path" | "root")
        },
        &[],
    ) {
        return 5.0;
    }
    if query.matches_with(
        fields,
        |key| (include_title || key != "title") && !matches!(key, "path" | "root"),
        &[],
    ) {
        return 6.0;
    }
    7.0
}

fn hidden_match_detail(
    query: &SearchQuery,
    fields: &BTreeMap<String, String>,
    visible: &[(&str, &str)],
) -> String {
    let mut reasons = Vec::new();
    for term in &query.terms {
        if visible
            .iter()
            .any(|(key, value)| term.matches(key, &normalize_for_search(value)))
        {
            continue;
        }
        let priority = [
            "title",
            "artist",
            "album",
            "albumartist",
            "genre",
            "year",
            "date",
            "composer",
            "conductor",
            "performer",
            "label",
            "comment",
            "lyrics",
            "track",
            "disc",
            "path",
            "root",
        ];
        if let Some(field) = priority.iter().find(|field| {
            fields
                .get(**field)
                .is_some_and(|value| term.matches(field, value))
        }) {
            let label = match *field {
                "albumartist" => "Album artist",
                "artist" => "Artist",
                "title" => "Title",
                "composer" => "Composer",
                "conductor" => "Conductor",
                "performer" => "Performer",
                "comment" => "Comment",
                "lyrics" => "Lyrics",
                "label" => "Label",
                "date" => "Date",
                "path" => "Path",
                "root" => "Library",
                "track" => "Track number",
                "disc" => "Disc",
                _ => field,
            };
            let reason = format!(
                "{label}: {}",
                term.value.chars().take(80).collect::<String>()
            );
            if !reasons.contains(&reason) {
                reasons.push(reason);
            }
        }
        if reasons.len() >= 3 {
            break;
        }
    }
    reasons.join(" · ")
}

fn search_tracks_prepared(
    query: &str,
    prepared: &PreparedSearchLibrary,
    roots: &[LibraryRoot],
    limit: usize,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> PreparedSearchOutcome {
    let query = SearchQuery::parse(query);
    let terms = query.name_terms("title");
    let mut accumulator = SearchRowAccumulator::new(roots.to_vec());
    let mut ranked = std::collections::BinaryHeap::<RankedSearchRow>::new();
    let cancel_poll_rows = search_cancel_poll_rows();
    for (index, track) in prepared.tracks.iter().enumerate() {
        if index % cancel_poll_rows == 0 {
            if let Some(next) = poll_latest_search_query(query_rx) {
                return PreparedSearchOutcome::Cancelled(next);
            }
        }
        if !query.matches(&track.fields) {
            continue;
        }
        let score = metadata_score(
            &query,
            &track.fields,
            entity_score(&terms, &track.title_l, &[&track.artist_l, &track.album_l]),
            true,
        );
        let hit = LibrarySearchTrack {
            path: track.path.clone(),
            root_path: track.root_path.clone(),
            title: track.title.clone(),
            artist: track.artist.clone(),
            album: track.album.clone(),
            cover_path: track.cover_path.clone(),
            genre: track.genre.clone(),
            year: track.year,
            track_no: track.track_no,
            duration_secs: track.duration_secs,
            score,
        };
        // Collect every matching group before bounding track rows: one large
        // album must not consume the candidate budget for all other albums.
        if let Some(mut row) = accumulator.push_hit(&hit, &query, &track.fields) {
            row.match_detail = hidden_match_detail(
                &query,
                &track.fields,
                &[
                    ("title", &hit.title),
                    ("artist", &hit.artist),
                    ("album", &hit.album),
                    ("genre", &hit.genre),
                    (
                        "year",
                        &hit.year.map_or_else(String::new, |year| year.to_string()),
                    ),
                ],
            );
            let candidate = RankedSearchRow(row);
            if ranked.len() < limit {
                ranked.push(candidate);
            } else if ranked.peek().is_some_and(|worst| candidate < *worst) {
                ranked.pop();
                ranked.push(candidate);
            }
        }
    }
    if let Some(next) = poll_latest_search_query(query_rx) {
        return PreparedSearchOutcome::Cancelled(next);
    }
    accumulator.track_rows = ranked.into_iter().map(|row| row.0).collect();
    PreparedSearchOutcome::Hits(accumulator.finish())
}

// ---------------------------------------------------------------------------
// Build search results frame
// ---------------------------------------------------------------------------

fn build_search_results_frame(
    query: &SearchWorkerQuery,
    prepared_cache: &mut SearchWorkerPreparedCache,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> SearchBuildOutcome {
    let seq = query.seq;
    let query_text = query.query.trim();
    if query_text.is_empty() {
        return empty_search_results_frame(seq);
    }
    let query_terms = split_search_terms(query_text);
    if query_terms.is_empty() {
        return empty_search_results_frame(seq);
    }
    let mut limits = search_result_limits(query_text);
    if query.limit > 0 {
        let limit = usize::try_from(query.limit)
            .unwrap_or(2_000)
            .clamp(20, 2_000);
        limits = SearchResultLimits {
            artist: limit,
            album: limit,
            track: limit,
        };
    }
    let library = query.library.as_ref();
    if library.roots.is_empty() {
        return empty_search_results_frame(seq);
    }
    let prepared = prepared_cache.prepared_for(&query.library);
    let buckets = match search_tracks_prepared(
        query_text,
        prepared.as_ref(),
        &library.roots,
        limits.track,
        query_rx,
    ) {
        PreparedSearchOutcome::Hits(rows) => rows,
        PreparedSearchOutcome::Cancelled(next) => return SearchBuildOutcome::Cancelled(next),
    };
    let totals = [
        u32::try_from(buckets.artist_groups.len()).unwrap_or(u32::MAX),
        u32::try_from(buckets.album_groups.len()).unwrap_or(u32::MAX),
        buckets.track_count,
    ];
    let rows = finalize_search_rows(
        &prepared.album_inventory,
        &limits,
        &buckets.album_cover_paths,
        buckets.artist_groups,
        buckets.album_groups,
        &buckets.album_hit_stats,
        buckets.track_rows,
    );
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, totals, rows })
}

// ---------------------------------------------------------------------------
// Worker / drain / process
// ---------------------------------------------------------------------------

pub(super) fn run_search_worker(
    query_rx: &Receiver<SearchWorkerQuery>,
    results_tx: &Sender<BridgeSearchResultsFrame>,
) {
    let Ok(mut query) = query_rx.recv() else {
        return;
    };
    let mut prepared_cache = SearchWorkerPreparedCache::default();
    let profile_search = search_profile_enabled();
    loop {
        while let Ok(next) = query_rx.try_recv() {
            // Cache warmups are advisory; a library refresh must never erase
            // the response owed to an interactive query (including a clear).
            if next.seq == 0 && next.query.is_empty() && query.seq != 0 {
                continue;
            }
            query = next;
        }

        #[allow(unused_variables)]
        let query_started = Instant::now();
        if query.seq == 0 && query.query.is_empty() {
            let _ = prepared_cache.prepared_for(&query.library);
            match query_rx.recv() {
                Ok(next) => {
                    query = next;
                }
                Err(_) => break,
            }
            continue;
        }
        match build_search_results_frame(&query, &mut prepared_cache, query_rx) {
            SearchBuildOutcome::Frame(frame) => {
                if profile_search {
                    profile_eprintln!(
                        "[search-worker] seq={} chars={} tracks={} rows={} elapsed_ms={}",
                        query.seq,
                        query.query.chars().count(),
                        query.library.tracks.len(),
                        frame.rows.len(),
                        query_started.elapsed().as_millis()
                    );
                }
                let _ = results_tx.send(frame);
            }
            SearchBuildOutcome::Cancelled(next) => {
                if profile_search {
                    profile_eprintln!(
                        "[search-worker] cancel seq={} -> {} elapsed_ms={}",
                        query.seq,
                        next.seq,
                        query_started.elapsed().as_millis()
                    );
                }
                query = next;
                continue;
            }
        }

        match query_rx.recv() {
            Ok(next) => {
                query = next;
            }
            Err(_) => break,
        }
    }
}

pub(super) fn process_search_results(frame: BridgeSearchResultsFrame, state: &mut BridgeState) {
    state.pending_search_results = Some(frame);
}

pub(super) fn drain_search_results(
    search_rx: &Receiver<BridgeSearchResultsFrame>,
    state: &mut BridgeState,
) {
    let mut latest = None;
    while let Ok(frame) = search_rx.try_recv() {
        latest = Some(frame);
    }

    if let Some(frame) = latest {
        process_search_results(frame, state);
    }
}

fn poll_latest_search_query(query_rx: &Receiver<SearchWorkerQuery>) -> Option<SearchWorkerQuery> {
    let mut latest = None;
    while let Ok(next) = query_rx.try_recv() {
        if next.seq != 0 || !next.query.is_empty() {
            latest = Some(next);
        }
    }
    latest
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::library::LibraryRoot;

    fn p(path: &str) -> PathBuf {
        PathBuf::from(path)
    }

    fn library_root(path: &PathBuf) -> LibraryRoot {
        LibraryRoot {
            path: path.clone(),
            name: String::new(),
        }
    }

    fn library_track(
        path: &str,
        root: &PathBuf,
        artist: &str,
        album: &str,
        year: Option<i32>,
        track_no: Option<u32>,
    ) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: p(path),
            root_path: root.clone(),
            title: String::new(),
            artist: artist.to_string(),
            album: album.to_string(),
            cover_path: String::new(),
            genre: String::new(),
            year,
            track_no,
            duration_secs: None,
        }
    }

    #[test]
    fn disc_section_detection_accepts_common_main_disc_names() {
        assert!(is_main_album_disc_section("CD1"));
        assert!(is_main_album_disc_section("CD 2"));
        assert!(is_main_album_disc_section("disc-03"));
        assert!(is_main_album_disc_section("Disk 4 (bonus)"));
        assert!(is_main_album_disc_section("DVD1"));
        assert!(is_main_album_disc_section("DVD 2"));
        assert!(!is_main_album_disc_section("Live"));
        assert!(!is_main_album_disc_section("discography"));
    }

    #[test]
    fn prepare_search_library_counts_main_album_tracks_with_cd_sections() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![
                crate::library::LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: p("/music/Artist/Album/01 - Intro.flac"),
                    root_path: root.clone(),
                    title: "Intro".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(1),
                    duration_secs: Some(100.0),
                },
                crate::library::LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: p("/music/Artist/Album/CD1/02 - Song.flac"),
                    root_path: root.clone(),
                    title: "Song".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(2),
                    duration_secs: Some(120.0),
                },
                crate::library::LibraryTrack {
                    search_tags: crate::library::SearchTags::default(),
                    path: p("/music/Artist/Album/Bonus/03 - Extra.flac"),
                    root_path: root.clone(),
                    title: "Extra".to_string(),
                    artist: "Artist".to_string(),
                    album: "Album".to_string(),
                    cover_path: String::new(),
                    genre: String::new(),
                    year: Some(2020),
                    track_no: Some(3),
                    duration_secs: Some(80.0),
                },
            ],
            ..LibrarySnapshot::default()
        };

        let prepared = prepare_search_library(&snapshot);
        let album_key = "album|/music|Artist|Album".to_string();
        let inv = prepared
            .album_inventory
            .get(&album_key)
            .expect("album inventory present");
        assert_eq!(inv.main_track_count, 2);
        assert!(inv.has_main_duration);
        assert!((inv.main_total_length - 220.0).abs() < 0.01);
    }

    #[test]
    fn prepared_search_cancels_when_newer_query_arrives() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![crate::library::LibraryTrack {
                search_tags: crate::library::SearchTags::default(),
                path: p("/music/Artist/Album/01 - Song.flac"),
                root_path: root,
                title: "Song".to_string(),
                artist: "Artist".to_string(),
                album: "Album".to_string(),
                cover_path: String::new(),
                genre: String::new(),
                year: Some(2020),
                track_no: Some(1),
                duration_secs: Some(60.0),
            }],
            ..LibrarySnapshot::default()
        };
        let prepared = prepare_search_library(&snapshot);
        let (tx, rx) = crossbeam_channel::unbounded::<SearchWorkerQuery>();
        tx.send(SearchWorkerQuery {
            limit: 0,
            seq: 99,
            query: "new".to_string(),
            library: Arc::new(snapshot),
        })
        .expect("queue newer search");

        match search_tracks_prepared("song", &prepared, &[library_root(&p("/music"))], 10, &rx) {
            PreparedSearchOutcome::Cancelled(next) => assert_eq!(next.seq, 99),
            PreparedSearchOutcome::Hits(_) => panic!("expected cancellation"),
        }
    }

    #[test]
    fn prepared_cache_reuses_same_search_revision_across_snapshot_arcs() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )],
            search_revision: 7,
            ..LibrarySnapshot::default()
        };
        let first = Arc::new(library.clone());
        let second = Arc::new(LibrarySnapshot {
            last_error: Some("scan still running".to_string()),
            ..library
        });

        let mut cache = SearchWorkerPreparedCache::default();
        let prepared_first = cache.prepared_for(&first);
        let prepared_second = cache.prepared_for(&second);

        assert!(Arc::ptr_eq(&prepared_first, &prepared_second));
    }

    #[test]
    fn prepared_cache_rebuilds_when_search_revision_changes() {
        let root = p("/music");
        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![library_track(
                "/music/Artist/Album/01 - Song.flac",
                &root,
                "Artist",
                "Album",
                Some(2020),
                Some(1),
            )],
            search_revision: 7,
            ..LibrarySnapshot::default()
        };
        let first = Arc::new(library.clone());
        let second = Arc::new(LibrarySnapshot {
            search_revision: 8,
            ..library
        });

        let mut cache = SearchWorkerPreparedCache::default();
        let prepared_first = cache.prepared_for(&first);
        let prepared_second = cache.prepared_for(&second);

        assert!(!Arc::ptr_eq(&prepared_first, &prepared_second));
    }

    #[test]
    fn album_search_rows_include_album_cover_path() {
        let root = PathBuf::from(format!(
            "/tmp/ferrous-search-album-cover-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|v| v.as_nanos())
                .unwrap_or(0)
        ));
        let album_dir = root.join("Artist").join("Album");
        let cover = album_dir.join("cover.jpg");
        let track = album_dir.join("01 - Song.flac");

        let library = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![crate::library::LibraryTrack {
                search_tags: crate::library::SearchTags::default(),
                path: track,
                root_path: root,
                title: "Song".to_string(),
                artist: "Artist".to_string(),
                album: "Album".to_string(),
                cover_path: cover.to_string_lossy().to_string(),
                genre: "Rock".to_string(),
                year: Some(2020),
                track_no: Some(1),
                duration_secs: Some(60.0),
            }],
            search_revision: 1,
            ..LibrarySnapshot::default()
        };
        let (_tx, rx) = crossbeam_channel::unbounded::<SearchWorkerQuery>();
        let mut prepared_cache = SearchWorkerPreparedCache::default();
        let outcome = build_search_results_frame(
            &SearchWorkerQuery {
                limit: 0,
                seq: 1,
                query: "album".to_string(),
                library: Arc::new(library),
            },
            &mut prepared_cache,
            &rx,
        );

        let frame = match outcome {
            SearchBuildOutcome::Frame(frame) => frame,
            SearchBuildOutcome::Cancelled(_) => panic!("unexpected cancellation"),
        };
        let album_row = frame
            .rows
            .iter()
            .find(|row| row.row_type == BridgeSearchResultRowType::Album)
            .expect("album row present");
        assert_eq!(album_row.cover_path, cover.to_string_lossy());
    }

    #[test]
    fn normalize_for_search_strips_diacritics() {
        assert_eq!(normalize_for_search("Jónsi"), "jonsi");
        assert_eq!(normalize_for_search("Björk"), "bjork");
        assert_eq!(normalize_for_search("Sigur Rós"), "sigur ros");
        assert_eq!(normalize_for_search("Ásgeir"), "asgeir");
        assert_eq!(normalize_for_search("café"), "cafe");
        assert_eq!(normalize_for_search("naïve"), "naive");
        // Plain ASCII unchanged
        assert_eq!(normalize_for_search("Pink Floyd"), "pink floyd");
    }

    #[test]
    fn query_terms_match_text_accent_insensitive() {
        let terms = split_search_terms("jonsi");
        assert!(query_terms_match_text(&terms, "Jónsi"));
        assert!(query_terms_match_text(&terms, "Jónsi & Alex"));

        let terms2 = split_search_terms("jónsi");
        assert!(query_terms_match_text(&terms2, "Jónsi"));
        assert!(query_terms_match_text(&terms2, "Jonsi"));

        let terms3 = split_search_terms("sigur ros");
        assert!(query_terms_match_text(&terms3, "Sigur Rós"));
    }
    fn fixture_track(
        title: &str,
        artist: &str,
        album: &str,
        file: &str,
    ) -> crate::library::LibraryTrack {
        crate::library::LibraryTrack {
            search_tags: crate::library::SearchTags::default(),
            path: p(&format!("/music/{artist}/{album}/{file}.flac")),
            root_path: p("/music"),
            title: title.into(),
            artist: artist.into(),
            album: album.into(),
            ..crate::library::LibraryTrack::default()
        }
    }

    fn fixture_search(
        query: &str,
        tracks: Vec<crate::library::LibraryTrack>,
        limit: usize,
    ) -> Vec<BridgeSearchResultRow> {
        let library = LibrarySnapshot {
            roots: vec![library_root(&p("/music"))],
            tracks,
            ..LibrarySnapshot::default()
        };
        let prepared = prepare_search_library(&library);
        let (_tx, rx) = crossbeam_channel::unbounded();
        let PreparedSearchOutcome::Hits(buckets) =
            search_tracks_prepared(query, &prepared, &library.roots, limit, &rx)
        else {
            panic!("unexpected cancellation")
        };
        finalize_search_rows(
            &prepared.album_inventory,
            &SearchResultLimits {
                artist: limit,
                album: limit,
                track: limit,
            },
            &buckets.album_cover_paths,
            buckets.artist_groups,
            buckets.album_groups,
            &buckets.album_hit_stats,
            buckets.track_rows,
        )
    }

    #[test]
    fn exact_title_beats_repeated_metadata_and_prefix_matches() {
        let rows = fixture_search(
            "blue",
            vec![
                fixture_track("Intro", "Blue", "Blue", "01"),
                fixture_track("Blue Skies", "Example", "Songs", "02"),
                fixture_track(
                    "Blue",
                    "The Example Ensemble",
                    "Collected Songs of the Northern Landscape",
                    "03",
                ),
            ],
            20,
        );
        let titles: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type == BridgeSearchResultRowType::Track)
            .map(|row| row.label.as_str())
            .collect();
        assert_eq!(titles, ["Blue", "Blue Skies", "Intro"]);
    }

    #[test]
    fn substring_and_path_matches_survive_other_prefix_matches() {
        let rows = fixture_search(
            "light",
            vec![
                fixture_track("Moonlight", "Example", "Night", "01"),
                fixture_track("Light Years", "Another", "Day", "02"),
                fixture_track("Instrumental", "Third", "Songs", "light-recording"),
            ],
            20,
        );
        let titles: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type == BridgeSearchResultRowType::Track)
            .map(|row| row.label.as_str())
            .collect();
        assert_eq!(titles, ["Light Years", "Moonlight", "Instrumental"]);
    }

    #[test]
    fn all_terms_can_match_across_accented_metadata() {
        let mut track = fixture_track("Svefn", "Sigur Rós", "Ágætis byrjun", "01");
        track.genre = "Post-rock".into();
        let rows = fixture_search("SIGUR agætis rock", vec![track.clone()], 20);
        assert_eq!(
            rows.iter()
                .filter(|row| row.row_type == BridgeSearchResultRowType::Track)
                .count(),
            1
        );
        assert!(fixture_search("sigur missing", vec![track], 20).is_empty());
    }

    #[test]
    fn groups_are_ranked_by_name_before_track_truncation() {
        let mut tracks: Vec<_> = (0..300)
            .map(|i| {
                fixture_track(
                    "Blue",
                    "Blue Orchestra",
                    "Blue Collection",
                    &format!("{i:03}"),
                )
            })
            .collect();
        tracks.push(fixture_track("Intro", "Blue", "Blue", "01"));
        let rows = fixture_search("blue", tracks, 1);
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].row_type, BridgeSearchResultRowType::Artist);
        assert_eq!(rows[0].label, "Blue");
        assert_eq!(rows[1].row_type, BridgeSearchResultRowType::Album);
        assert_eq!(rows[1].label, "Blue");
        assert_eq!(rows[1].count, 1);
    }

    #[test]
    fn equal_relevance_uses_title_before_path() {
        let rows = fixture_search(
            "artist",
            vec![
                fixture_track("Zulu", "Artist", "Album", "01"),
                fixture_track("Alpha", "Artist", "Album", "99"),
            ],
            1,
        );
        assert_eq!(rows.last().expect("track result").label, "Alpha");
    }
    #[test]
    fn totals_count_all_matches_and_expansion_retrieves_more_rows() {
        let library = Arc::new(LibrarySnapshot {
            roots: vec![library_root(&p("/music"))],
            tracks: (0..65)
                .map(|i| {
                    fixture_track(
                        &format!("Song {i:03}"),
                        "Artist",
                        "Album",
                        &format!("{i:03}"),
                    )
                })
                .collect(),
            ..LibrarySnapshot::default()
        });
        let (_tx, rx) = crossbeam_channel::unbounded();
        let mut cache = SearchWorkerPreparedCache::default();
        for (limit, expected) in [(0, 20), (40, 40), (80, 65)] {
            let SearchBuildOutcome::Frame(frame) = build_search_results_frame(
                &SearchWorkerQuery {
                    seq: 1,
                    query: "song".into(),
                    library: Arc::clone(&library),
                    limit,
                },
                &mut cache,
                &rx,
            ) else {
                panic!("unexpected cancellation")
            };
            assert_eq!(frame.totals, [0, 0, 65]);
            assert_eq!(frame.rows.len(), expected);
        }
    }
    #[test]
    fn searches_extended_tags_filters_and_phrases_with_match_details() {
        let mut track = fixture_track("Blue Skies", "Soloist", "Concert", "01");
        track.year = Some(1997);
        track.track_no = Some(1);
        track.search_tags = crate::library::SearchTags::from([
            (
                "albumartist".into(),
                vec!["London Symphony Orchestra".into()],
            ),
            (
                "artist".into(),
                vec!["Guest Singer".into(), "Second Singer".into()],
            ),
            ("genre".into(), vec!["Classical".into(), "Choral".into()]),
            ("composer".into(), vec!["Jean Sibelius".into()]),
            ("conductor".into(), vec!["Esa-Pekka Salonen".into()]),
            ("performer".into(), vec!["Helsinki Choir".into()]),
            ("label".into(), vec!["Example Records".into()]),
            ("comment".into(), vec!["Anniversary remaster".into()]),
            ("lyrics".into(), vec!["The silver moon is rising".into()]),
            ("date".into(), vec!["1997-05-12".into()]),
            ("disc".into(), vec!["2".into()]),
        ]);
        for query in [
            r#"albumartist:"london symphony" year:1997"#,
            "second singer",
            "genre:choral",
            "composer:sibelius",
            "conductor:salonen",
            "performer:helsinki",
            "label:records",
            "comment:remaster",
            r#"lyrics:"silver moon""#,
            "date:1997-05",
            "track:1 disc:2",
            r#""blue skies""#,
            "root:music",
        ] {
            let rows = fixture_search(query, vec![track.clone()], 20);
            assert!(
                rows.iter()
                    .any(|row| row.row_type == BridgeSearchResultRowType::Track),
                "missing {query}"
            );
        }
        for query in [
            "year:199",
            "track:10",
            "disc:1",
            "artist:sibelius",
            r#""skies blue""#,
        ] {
            assert!(
                fixture_search(query, vec![track.clone()], 20).is_empty(),
                "unexpected {query}"
            );
        }
        let rows = fixture_search("comment:remaster", vec![track], 20);
        assert!(rows
            .iter()
            .any(|row| row.row_type == BridgeSearchResultRowType::Album));
        assert!(rows
            .iter()
            .all(|row| row.match_detail.contains("Comment: remaster")));
    }

    #[test]
    fn album_metadata_matches_only_tracks_in_the_main_album_queue() {
        for (section, expect_album) in [("Bonus", false), ("CD1", true)] {
            let main = fixture_track("Intro", "Artist", "Album", "01");
            let mut extra = fixture_track("Extra", "Artist", "Album", "02");
            extra.path = p(&format!("/music/Artist/Album/{section}/02.flac"));
            extra
                .search_tags
                .insert("comment".into(), vec!["Anniversary remaster".into()]);
            let rows = fixture_search("comment:remaster", vec![main, extra], 20);
            assert!(rows.iter().any(|row| {
                row.row_type == BridgeSearchResultRowType::Track && row.label == "Extra"
            }));
            assert_eq!(
                rows.iter()
                    .any(|row| row.row_type == BridgeSearchResultRowType::Album),
                expect_album,
                "unexpected album match from {section}"
            );
        }
    }

    #[test]
    fn long_comments_and_lyrics_cannot_outrank_title_matches() {
        let mut comment = fixture_track("Intro", "A", "A", "01");
        comment
            .search_tags
            .insert("comment".into(), vec!["blue ".repeat(1000)]);
        let mut lyrics = fixture_track("Another Song", "B", "B", "01");
        lyrics
            .search_tags
            .insert("lyrics".into(), vec!["blue ".repeat(1000)]);
        let rows = fixture_search(
            "blue",
            vec![comment, lyrics, fixture_track("Blue", "C", "C", "01")],
            20,
        );
        let first_track = rows
            .iter()
            .find(|row| row.row_type == BridgeSearchResultRowType::Track)
            .expect("track");
        assert_eq!(first_track.label, "Blue");
    }
    #[test]
    fn word_prefixes_rank_before_infix_substrings() {
        let rows = fixture_search(
            "light",
            vec![
                fixture_track("Moonlight", "Artist", "Album", "01"),
                fixture_track("Years of Light", "Artist", "Album", "02"),
            ],
            20,
        );
        let titles: Vec<_> = rows
            .iter()
            .filter(|row| row.row_type == BridgeSearchResultRowType::Track)
            .map(|row| row.label.as_str())
            .collect();
        assert_eq!(titles, ["Years of Light", "Moonlight"]);
    }
    #[test]
    fn cache_warmup_does_not_replace_a_queued_query() {
        let library = Arc::new(LibrarySnapshot {
            roots: vec![library_root(&p("/music"))],
            tracks: vec![fixture_track("Blue", "Artist", "Album", "01")],
            ..LibrarySnapshot::default()
        });
        let (tx, rx) = crossbeam_channel::unbounded();
        let (results_tx, results_rx) = crossbeam_channel::unbounded();
        tx.send(SearchWorkerQuery {
            limit: 0,
            seq: 42,
            query: "blue".into(),
            library: Arc::clone(&library),
        })
        .expect("query");
        tx.send(SearchWorkerQuery {
            limit: 0,
            seq: 0,
            query: String::new(),
            library,
        })
        .expect("warmup");
        drop(tx);
        run_search_worker(&rx, &results_tx);
        let frame = results_rx.try_recv().expect("interactive response");
        assert_eq!(frame.seq, 42);
        assert_eq!(frame.totals[2], 1);
    }

    #[test]
    fn cache_warmup_does_not_cancel_in_progress_search() {
        let library = LibrarySnapshot {
            roots: vec![library_root(&p("/music"))],
            tracks: vec![fixture_track("Blue", "Artist", "Album", "01")],
            ..LibrarySnapshot::default()
        };
        let prepared = prepare_search_library(&library);
        let (tx, rx) = crossbeam_channel::unbounded();
        tx.send(SearchWorkerQuery {
            limit: 0,
            seq: 0,
            query: String::new(),
            library: Arc::new(library.clone()),
        })
        .expect("warmup");
        let PreparedSearchOutcome::Hits(rows) =
            search_tracks_prepared("blue", &prepared, &library.roots, 20, &rx)
        else {
            panic!("warmup cancelled query")
        };
        assert_eq!(rows.track_count, 1);
    }
}
