// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashMap;

use crossbeam_channel::{Receiver, Sender};

use crate::artwork::apply_artwork_to_track;
use crate::lastfm::{
    self, Command as LastFmCommand, Event as LastFmEvent, Handle as LastFmHandle,
    ServiceOptions as LastFmServiceOptions,
};
use crate::library::{
    load_external_track_cache, read_track_info, refresh_cover_paths_for_tracks,
    refresh_cover_paths_for_tracks_with_override, store_external_track_cache, IndexedTrack,
};

use super::config::config_base_path;
use super::search::{run_search_worker, SearchWorkerQuery};
use super::{
    ApplyAlbumArtEvent, ApplyAlbumArtRequest, BridgeSearchResultsFrame, BridgeSettings,
    ExternalQueueDetailsEvent, ExternalQueueDetailsRequest,
};

pub(super) fn spawn_bridge_support_threads(
    search_query_rx: Receiver<SearchWorkerQuery>,
    search_results_tx: Sender<BridgeSearchResultsFrame>,
    external_queue_details_rx: Receiver<ExternalQueueDetailsRequest>,
    external_queue_details_event_tx: Sender<ExternalQueueDetailsEvent>,
    apply_album_art_rx: Receiver<ApplyAlbumArtRequest>,
    apply_album_art_event_tx: Sender<ApplyAlbumArtEvent>,
) {
    let _ = std::thread::Builder::new()
        .name("ferrous-bridge-search".to_string())
        .spawn(move || run_search_worker(&search_query_rx, &search_results_tx));
    let _ = std::thread::Builder::new()
        .name("ferrous-queue-details".to_string())
        .spawn(move || {
            run_external_queue_detail_worker(
                &external_queue_details_rx,
                &external_queue_details_event_tx,
            );
        });
    let _ = std::thread::Builder::new()
        .name("ferrous-apply-artwork".to_string())
        .spawn(move || run_apply_album_art_worker(&apply_album_art_rx, &apply_album_art_event_tx));
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

fn run_external_queue_detail_worker(
    req_rx: &Receiver<ExternalQueueDetailsRequest>,
    event_tx: &Sender<ExternalQueueDetailsEvent>,
) {
    while let Ok(request) = req_rx.recv() {
        let indexed =
            load_external_track_cache(&request.path, request.fingerprint).unwrap_or_else(|| {
                let indexed = read_track_info(&request.path);
                let _ = store_external_track_cache(&request.path, request.fingerprint, &indexed);
                indexed
            });
        let _ = event_tx.send(ExternalQueueDetailsEvent {
            path: request.path,
            fingerprint: request.fingerprint,
            indexed,
        });
    }
}

fn run_apply_album_art_worker(
    req_rx: &Receiver<ApplyAlbumArtRequest>,
    event_tx: &Sender<ApplyAlbumArtEvent>,
) {
    while let Ok(request) = req_rx.recv() {
        let event = match apply_artwork_to_track(&request.track_path, &request.artwork_path) {
            Ok(outcome) => {
                let mut affected_paths = outcome.affected_track_paths;
                affected_paths.sort();
                affected_paths.dedup();

                let (refresh_error, indexed_by_path) = if let Some(cover_path) =
                    outcome.cover_path_override
                {
                    let refresh_error =
                        refresh_cover_paths_for_tracks_with_override(&affected_paths, &cover_path)
                            .err();
                    let cover_path_string = cover_path.to_string_lossy().to_string();
                    let indexed_by_path = affected_paths
                        .into_iter()
                        .map(|path| {
                            let indexed = IndexedTrack {
                                title: String::new(),
                                artist: String::new(),
                                album: String::new(),
                                cover_path: cover_path_string.clone(),
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
                    let refresh_error = refresh_cover_paths_for_tracks(&affected_paths).err();
                    let indexed_by_path = affected_paths
                        .into_iter()
                        .map(|path| {
                            let indexed = read_track_info(&path);
                            (path, indexed)
                        })
                        .collect::<HashMap<_, _>>();
                    (refresh_error, indexed_by_path)
                };

                ApplyAlbumArtEvent {
                    track_path: request.track_path,
                    indexed_by_path,
                    error: refresh_error
                        .map(|error| format!("failed to refresh cover paths: {error}")),
                }
            }
            Err(error) => ApplyAlbumArtEvent {
                track_path: request.track_path,
                indexed_by_path: HashMap::new(),
                error: Some(format!("failed to apply album art: {error}")),
            },
        };
        let _ = event_tx.send(event);
    }
}
