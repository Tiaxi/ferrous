// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;

use crossbeam_channel::{unbounded, Receiver, Sender};

use crate::artwork::apply_artwork_to_track;
use crate::folder_cover::FolderImages;
use crate::lastfm::{
    self, Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    ServiceOptions as LastFmServiceOptions,
};
use crate::library::{
    read_track_info, refresh_cover_paths_for_tracks, refresh_cover_paths_for_tracks_with_override,
    track_file_fingerprint, CoverReadAt, IndexedTrack,
};

use crate::worker_thread::spawn_worker;

use super::config::config_base_path;
use super::search::{run_search_worker, SearchWorkerQuery};
use super::{ApplyAlbumArtEvent, ApplyAlbumArtRequest, BridgeSearchResultsFrame, BridgeSettings};

/// Start the search worker, and return its query and result channels.
pub(super) fn spawn_search_worker() -> (
    Sender<SearchWorkerQuery>,
    Receiver<BridgeSearchResultsFrame>,
) {
    let (query_tx, query_rx) = unbounded();
    let (results_tx, results_rx) = unbounded();
    spawn_worker("ferrous-bridge-search", move || {
        run_search_worker(&query_rx, &results_tx);
    });
    (query_tx, results_rx)
}

/// Start the album art worker, and return its request and result channels.
pub(super) fn spawn_apply_album_art_worker(
) -> (Sender<ApplyAlbumArtRequest>, Receiver<ApplyAlbumArtEvent>) {
    let (request_tx, request_rx) = unbounded();
    let (event_tx, event_rx) = unbounded();
    spawn_worker("ferrous-apply-artwork", move || {
        run_apply_album_art_worker(&request_rx, &event_tx);
    });
    (request_tx, event_rx)
}

pub(super) fn spawn_lastfm_service(
    settings: &BridgeSettings,
) -> (LastFmHandle, Receiver<LastFmEvent>) {
    let lastfm_queue_path = config_base_path().map(|base| lastfm::queue_path(&base));
    let (lastfm, lastfm_rx) = lastfm::spawn(LastFmServiceOptions {
        queue_path: lastfm_queue_path,
        initial_enabled: settings.integrations.lastfm_scrobbling_enabled,
    });
    if !settings.integrations.lastfm_username.trim().is_empty() {
        lastfm.command(LastFmCommand::LoadStoredSession {
            username: settings.integrations.lastfm_username.clone(),
        });
    }
    (lastfm, lastfm_rx)
}

fn run_apply_album_art_worker(
    req_rx: &Receiver<ApplyAlbumArtRequest>,
    event_tx: &Sender<ApplyAlbumArtEvent>,
) {
    while let Ok(request) = req_rx.recv() {
        let event = match apply_artwork_to_track(&request.track_path, &request.artwork_path) {
            Ok(outcome) => {
                let failure_summary = outcome.failure_summary();
                let mut affected_paths = outcome.affected_track_paths;
                affected_paths.sort();
                affected_paths.dedup();

                let (refresh_error, indexed_by_path) =
                    if let Some(cover_path) = outcome.cover_path_override {
                        // The folder holds the new image now, so its stamp is new.
                        let cover_read_at = CoverReadAt::now();
                        let cover_stamp = cover_path
                            .parent()
                            .and_then(|dir| FolderImages::read(dir).ok())
                            .map_or(0, |images| images.stamp());
                        let refresh_error = refresh_cover_paths_for_tracks_with_override(
                            &affected_paths,
                            &cover_path,
                            cover_stamp,
                        )
                        .err()
                        .map(|error| format!("failed to refresh cover paths: {error}"));
                        let cover_path_string = cover_path.to_string_lossy().to_string();
                        let indexed_by_path = affected_paths
                            .into_iter()
                            .map(|path| {
                                let indexed = IndexedTrack {
                                    search_tags: crate::library::SearchTags::default(),
                                    title: String::new(),
                                    artist: String::new(),
                                    album: String::new(),
                                    cover_path: cover_path_string.clone(),
                                    cover_stamp,
                                    cover_read_at,
                                    genre: String::new(),
                                    year: None,
                                    track_no: None,
                                    duration_secs: None,
                                };
                                (path, indexed)
                            })
                            .collect::<HashMap<_, _>>();
                        (refresh_error, indexed_by_path)
                    } else {
                        match refresh_cover_paths_for_tracks(&affected_paths) {
                            // Files that could not be read fully keep the covers
                            // they had, which the problem tells the user.
                            Ok(refresh) => (refresh.problem, refresh.indexed),
                            Err(error) => {
                                let indexed_by_path = affected_paths
                                    .into_iter()
                                    .map(|path| {
                                        let indexed = read_track_info(&path);
                                        (path, indexed)
                                    })
                                    .collect::<HashMap<_, _>>();
                                (
                                    Some(format!("failed to refresh cover paths: {error}")),
                                    indexed_by_path,
                                )
                            }
                        }
                    };

                let error = match (failure_summary, refresh_error) {
                    (Some(failures), Some(refresh)) => Some(format!("{failures}; {refresh}")),
                    (failures, refresh) => failures.or(refresh),
                };
                // Read here, so the bridge thread does not stat the files.
                let fingerprints = indexed_by_path
                    .keys()
                    .filter_map(|path| Some((path.clone(), track_file_fingerprint(path)?)))
                    .collect();
                ApplyAlbumArtEvent {
                    track_path: request.track_path,
                    indexed_by_path,
                    fingerprints,
                    error,
                }
            }
            Err(error) => ApplyAlbumArtEvent {
                track_path: request.track_path,
                indexed_by_path: HashMap::new(),
                fingerprints: HashMap::new(),
                error: Some(format!("failed to apply album art: {error}")),
            },
        };
        let _ = event_tx.send(event);
    }
}
