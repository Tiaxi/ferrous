// SPDX-License-Identifier: GPL-3.0-or-later

use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use crossbeam_channel::{Receiver, Sender};
use unicode_normalization::UnicodeNormalization;

use crate::library::{search_tracks_fts, LibraryRoot, LibrarySearchTrack, LibrarySnapshot};

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
    artist_name: String,
    album_title: String,
    artist_key: String,
    root_label: String,
    year_counts: HashMap<i32, usize>,
    genre_counts: HashMap<String, usize>,
}

struct SearchResultLimits {
    fallback: usize,
    artist: usize,
    album: usize,
    track: usize,
}

type SearchGroupMap = HashMap<String, (f32, String, String)>;

struct SearchRowBuckets {
    track_rows: Vec<BridgeSearchResultRow>,
    album_cover_paths: HashMap<String, String>,
    artist_groups: SearchGroupMap,
    album_groups: SearchGroupMap,
    album_hit_stats: HashMap<String, HitAlbumAcc>,
}

struct SearchRowAccumulator {
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
            roots_by_path: roots_by_path_for_search(&roots),
            roots,
            album_cover_paths: HashMap::new(),
            artist_groups: HashMap::new(),
            album_groups: HashMap::new(),
            album_hit_stats: HashMap::new(),
            track_rows: Vec::new(),
        }
    }

    fn push_hit(&mut self, hit: &LibrarySearchTrack, query_terms: &[String]) {
        let Some(context) = derive_hit_context(hit, &self.roots, &self.roots_by_path) else {
            return;
        };
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
        if query_terms_match_text(query_terms, &context.artist_name) {
            let artist_entry = self
                .artist_groups
                .entry(context.artist_key.clone())
                .or_insert((
                    hit.score,
                    context.artist_name.clone(),
                    context.root_label.clone(),
                ));
            if hit.score < artist_entry.0 {
                artist_entry.0 = hit.score;
                artist_entry.1.clone_from(&context.artist_name);
                artist_entry.2.clone_from(&context.root_label);
            }
        }
        if let Some(album_key_value) = album_key.clone() {
            let album_query = format!("{} {}", context.artist_name, hit_album);
            if query_terms_match_text(query_terms, &album_query) {
                let album_entry = self.album_groups.entry(album_key_value.clone()).or_insert((
                    hit.score,
                    hit_album.clone(),
                    context.root_label.clone(),
                ));
                if hit.score < album_entry.0 {
                    album_entry.0 = hit.score;
                    album_entry.1.clone_from(&hit_album);
                    album_entry.2.clone_from(&context.root_label);
                }
                update_album_hit_stats(
                    &mut self.album_hit_stats,
                    album_key_value,
                    &context,
                    &hit_album,
                    hit.year,
                    hit.genre.trim(),
                );
            }
        }
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
        self.track_rows.push(build_track_search_result_row(
            hit,
            &context,
            &hit_artist,
            &hit_album,
            album_key,
            hit_path_string,
            row_cover_path,
        ));
    }

    fn finish(self) -> SearchRowBuckets {
        SearchRowBuckets {
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
    path_string: String,
    path_lower: String,
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
    genre_l: String,
    haystack_l: String,
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

enum SearchFallbackOutcome {
    Hits(Vec<LibrarySearchTrack>),
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

#[derive(Clone)]
struct FallbackRankedHit {
    score: f32,
    path_lower: String,
    track_index: usize,
}

impl PartialEq for FallbackRankedHit {
    fn eq(&self, other: &Self) -> bool {
        compare_fallback_rank(self.score, &self.path_lower, other.score, &other.path_lower)
            == Ordering::Equal
    }
}

impl Eq for FallbackRankedHit {}

impl PartialOrd for FallbackRankedHit {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for FallbackRankedHit {
    fn cmp(&self, other: &Self) -> Ordering {
        compare_fallback_rank(self.score, &self.path_lower, other.score, &other.path_lower)
    }
}

// ---------------------------------------------------------------------------
// Config / limit functions
// ---------------------------------------------------------------------------

fn search_profile_enabled() -> bool {
    cfg!(feature = "profiling-logs") && std::env::var_os("FERROUS_SEARCH_PROFILE").is_some()
}

fn search_fallback_limit() -> usize {
    std::env::var("FERROUS_SEARCH_FALLBACK_LIMIT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(256, |v| v.clamp(64, 5_000))
}

fn search_short_query_char_threshold() -> usize {
    std::env::var("FERROUS_SEARCH_SHORT_QUERY_CHARS")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(1, |v| v.clamp(1, 8))
}

fn search_fallback_limit_short() -> usize {
    std::env::var("FERROUS_SEARCH_FALLBACK_LIMIT_SHORT")
        .ok()
        .and_then(|raw| raw.trim().parse::<usize>().ok())
        .map_or(128, |v| v.clamp(64, 5_000))
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

pub(super) fn query_terms_match_text(terms: &[String], text: &str) -> bool {
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
    })
}

fn search_result_limits(query_text: &str) -> SearchResultLimits {
    let is_short_query = query_text.chars().count() <= search_short_query_char_threshold();
    SearchResultLimits {
        fallback: if is_short_query {
            search_fallback_limit_short()
        } else {
            search_fallback_limit()
        },
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

fn search_fts_enabled() -> bool {
    std::env::var_os("FERROUS_SEARCH_DISABLE_FTS").is_none()
}

fn populate_search_rows(
    roots: &[LibraryRoot],
    hits: &[LibrarySearchTrack],
    query_terms: &[String],
) -> SearchRowBuckets {
    let mut rows = SearchRowAccumulator::new(roots.to_vec());
    for hit in hits {
        rows.push_hit(hit, query_terms);
    }
    rows.finish()
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

fn accumulate_album_inventory_for_hits(
    library: &LibrarySnapshot,
    roots_by_path: &HashMap<PathBuf, PreparedSearchRoot>,
    album_keys: &HashSet<String>,
) -> HashMap<String, AlbumInventoryAcc> {
    if album_keys.is_empty() {
        return HashMap::new();
    }

    let mut album_inventory: HashMap<String, AlbumInventoryAcc> =
        HashMap::with_capacity(album_keys.len());
    for track in &library.tracks {
        let artist = track.artist.trim().to_string();
        let Some(context) = roots_by_path
            .get(&track.root_path)
            .and_then(|root| derive_tree_path_context_for_root(&track.path, root, &artist))
        else {
            continue;
        };
        let Some(album_key) = context.album_key else {
            continue;
        };
        if !album_keys.contains(&album_key) {
            continue;
        }
        let include_in_main_album =
            context.is_main_level_album_track || context.is_disc_section_album_track;
        if !include_in_main_album {
            continue;
        }

        let inventory = album_inventory.entry(album_key).or_default();
        inventory.main_track_count = inventory.main_track_count.saturating_add(1);
        if let Some(duration) = track.duration_secs {
            if duration.is_finite() && duration >= 0.0 {
                inventory.main_total_length += duration;
                inventory.has_main_duration = true;
            }
        }
    }

    album_inventory
}

fn build_search_rows_from_hits(
    library: &LibrarySnapshot,
    hits: &[LibrarySearchTrack],
    query_terms: &[String],
    limits: &SearchResultLimits,
) -> Vec<BridgeSearchResultRow> {
    let buckets = populate_search_rows(&library.roots, hits, query_terms);
    let album_keys = buckets.album_groups.keys().cloned().collect::<HashSet<_>>();
    let album_inventory = accumulate_album_inventory_for_hits(
        library,
        &roots_by_path_for_search(&library.roots),
        &album_keys,
    );
    finalize_search_rows(
        &album_inventory,
        limits,
        &buckets.album_cover_paths,
        buckets.artist_groups,
        buckets.album_groups,
        &buckets.album_hit_stats,
        buckets.track_rows,
    )
}

// ---------------------------------------------------------------------------
// Prepare search library
// ---------------------------------------------------------------------------

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
        let path_lower = path_string.to_lowercase();
        let title = track.title.trim().to_string();
        let artist = track.artist.trim().to_string();
        let album = track.album.trim().to_string();
        let genre = track.genre.trim().to_string();
        let title_l = normalize_for_search(&title);
        let artist_l = normalize_for_search(&artist);
        let album_l = normalize_for_search(&album);
        let genre_l = normalize_for_search(&genre);
        let path_n = normalize_for_search(&path_string);
        let haystack_l = format!("{title_l} {artist_l} {album_l} {genre_l} {path_n}");

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
            path_string,
            path_lower,
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
            genre_l,
            haystack_l,
        });
    }

    PreparedSearchLibrary {
        tracks,
        album_inventory,
    }
}

// ---------------------------------------------------------------------------
// Fallback search
// ---------------------------------------------------------------------------

fn compare_fallback_rank(
    a_score: f32,
    a_path_lower: &str,
    b_score: f32,
    b_path_lower: &str,
) -> Ordering {
    a_score
        .partial_cmp(&b_score)
        .unwrap_or(Ordering::Equal)
        .then_with(|| a_path_lower.cmp(b_path_lower))
}

fn search_tracks_fallback_prepared(
    query: &str,
    prepared: &PreparedSearchLibrary,
    limit: usize,
    query_rx: &Receiver<SearchWorkerQuery>,
) -> SearchFallbackOutcome {
    let terms = split_search_terms(query);
    if terms.is_empty() {
        return SearchFallbackOutcome::Hits(Vec::new());
    }

    let capped_limit = limit.clamp(1, 5_000);
    let mut heap =
        std::collections::BinaryHeap::<FallbackRankedHit>::with_capacity(capped_limit + 1);
    let cancel_poll_rows = search_cancel_poll_rows();
    for (index, track) in prepared.tracks.iter().enumerate() {
        if index % cancel_poll_rows == 0 {
            if let Some(next) = poll_latest_search_query(query_rx) {
                return SearchFallbackOutcome::Cancelled(next);
            }
        }
        if !terms.iter().all(|term| track.haystack_l.contains(term)) {
            continue;
        }

        let mut score = 0.0f32;
        for term in &terms {
            score += if track.title_l.starts_with(term) {
                0.0
            } else if track.title_l.contains(term) {
                0.8
            } else if track.artist_l.starts_with(term) {
                1.2
            } else if track.artist_l.contains(term) {
                1.8
            } else if track.album_l.starts_with(term) {
                2.0
            } else if track.album_l.contains(term) {
                2.6
            } else if track.genre_l.contains(term) {
                3.2
            } else {
                4.0
            };
        }
        score += f32::from(
            u16::try_from(track.path_string.len().min(usize::from(u16::MAX))).unwrap_or(u16::MAX),
        ) / 10_000.0;

        if heap.len() >= capped_limit {
            if let Some(worst) = heap.peek() {
                let is_better =
                    compare_fallback_rank(score, &track.path_lower, worst.score, &worst.path_lower)
                        == Ordering::Less;
                if !is_better {
                    continue;
                }
            }
            let _ = heap.pop();
        }
        heap.push(FallbackRankedHit {
            score,
            path_lower: track.path_lower.clone(),
            track_index: index,
        });
    }

    if let Some(next) = poll_latest_search_query(query_rx) {
        return SearchFallbackOutcome::Cancelled(next);
    }

    let mut ranked = heap.into_vec();
    ranked.sort_by(|a, b| compare_fallback_rank(a.score, &a.path_lower, b.score, &b.path_lower));

    let mut out = Vec::with_capacity(ranked.len());
    for rank in ranked {
        let track = &prepared.tracks[rank.track_index];
        out.push(LibrarySearchTrack {
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
            score: rank.score,
        });
    }
    SearchFallbackOutcome::Hits(out)
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
    let limits = search_result_limits(query_text);
    let library = query.library.as_ref();
    if library.roots.is_empty() {
        return empty_search_results_frame(seq);
    }
    if search_fts_enabled() {
        if let Ok(hits) = search_tracks_fts(query_text, limits.fallback) {
            if !hits.is_empty() {
                let rows = build_search_rows_from_hits(library, &hits, &query_terms, &limits);
                return SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, rows });
            }
        }
    }

    let prepared = prepared_cache.prepared_for(&query.library);
    let hits = match search_tracks_fallback_prepared(
        query_text,
        prepared.as_ref(),
        limits.fallback,
        query_rx,
    ) {
        SearchFallbackOutcome::Hits(rows) => rows,
        SearchFallbackOutcome::Cancelled(next) => return SearchBuildOutcome::Cancelled(next),
    };
    if hits.is_empty() {
        return empty_search_results_frame(seq);
    }
    let buckets = populate_search_rows(&library.roots, &hits, &query_terms);
    let rows = finalize_search_rows(
        &prepared.album_inventory,
        &limits,
        &buckets.album_cover_paths,
        buckets.artist_groups,
        buckets.album_groups,
        &buckets.album_hit_stats,
        buckets.track_rows,
    );
    SearchBuildOutcome::Frame(BridgeSearchResultsFrame { seq, rows })
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
        latest = Some(next);
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

    fn test_guard() -> std::sync::MutexGuard<'static, ()> {
        use std::sync::{Mutex, OnceLock};
        static TEST_MUTEX: OnceLock<Mutex<()>> = OnceLock::new();
        TEST_MUTEX
            .get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
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
    fn fallback_search_cancels_when_newer_query_arrives() {
        let root = p("/music");
        let snapshot = LibrarySnapshot {
            roots: vec![library_root(&root)],
            tracks: vec![crate::library::LibraryTrack {
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
            seq: 99,
            query: "new".to_string(),
            library: Arc::new(snapshot),
        })
        .expect("queue newer search");

        match search_tracks_fallback_prepared("song", &prepared, 10, &rx) {
            SearchFallbackOutcome::Cancelled(next) => assert_eq!(next.seq, 99),
            SearchFallbackOutcome::Hits(_) => panic!("expected cancellation"),
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
        let _guard = test_guard();
        std::env::set_var("FERROUS_SEARCH_DISABLE_FTS", "1");
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
        std::env::remove_var("FERROUS_SEARCH_DISABLE_FTS");
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
}
