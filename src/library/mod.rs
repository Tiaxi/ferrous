// SPDX-License-Identifier: GPL-3.0-or-later

mod scan;
mod schema;

use std::path::PathBuf;

use crossbeam_channel::{unbounded, Receiver, Sender};

pub(crate) use scan::read_library_snapshot_from_db;
use scan::{
    handle_add_root, handle_rescan_all, handle_rescan_root, remove_root_and_purge, rename_root,
};
pub(crate) use scan::{
    is_supported_audio, load_external_track_cache, load_external_track_caches, read_track_info,
    refresh_cover_paths_for_tracks, refresh_cover_paths_for_tracks_with_override,
    refresh_indexed_metadata_for_paths, rename_indexed_metadata_paths, store_external_track_cache,
    track_file_fingerprint,
};
pub use schema::search_tracks_fts;
use schema::{init_schema, load_snapshot, open_library_db};

#[derive(Debug, Clone, Default)]
pub struct LibraryTrack {
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_path: String,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LibraryRoot {
    pub path: PathBuf,
    pub name: String,
}

impl LibraryRoot {
    #[must_use]
    pub fn display_name(&self) -> String {
        let trimmed = self.name.trim();
        if trimmed.is_empty() {
            self.path.to_string_lossy().to_string()
        } else {
            trimmed.to_string()
        }
    }

    #[must_use]
    pub fn search_label(&self) -> String {
        let trimmed = self.name.trim();
        if !trimmed.is_empty() {
            return trimmed.to_string();
        }
        self.path.file_name().map_or_else(
            || self.path.to_string_lossy().to_string(),
            |value| value.to_string_lossy().to_string(),
        )
    }
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySearchTrack {
    pub path: PathBuf,
    pub root_path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub cover_path: String,
    pub genre: String,
    pub year: Option<i32>,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
    pub score: f32,
}

#[derive(Debug, Clone, Default)]
pub struct LibraryScanProgress {
    pub current_root: Option<PathBuf>,
    pub roots_completed: usize,
    pub roots_total: usize,
    pub supported_files_discovered: usize,
    pub supported_files_processed: usize,
    pub files_per_second: Option<f32>,
    pub eta_seconds: Option<f32>,
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySnapshot {
    pub roots: Vec<LibraryRoot>,
    pub tracks: Vec<LibraryTrack>,
    pub search_revision: u64,
    pub scan_in_progress: bool,
    pub scan_progress: Option<LibraryScanProgress>,
    pub last_error: Option<String>,
}

impl LibrarySnapshot {
    fn bump_search_revision(&mut self) {
        self.search_revision = self.search_revision.saturating_add(1);
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct TrackFileFingerprint {
    pub(crate) mtime_ns: i64,
    pub(crate) size_bytes: i64,
}

#[derive(Debug, Clone)]
pub enum LibraryCommand {
    ScanRoot(PathBuf),
    AddRoot { path: PathBuf, name: String },
    RenameRoot { path: PathBuf, name: String },
    RemoveRoot(PathBuf),
    RescanRoot(PathBuf),
    RescanAll,
}

#[derive(Debug, Clone)]
pub enum LibraryEvent {
    Snapshot(LibrarySnapshot),
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct IndexedTrack {
    pub(crate) title: String,
    pub(crate) artist: String,
    pub(crate) album: String,
    pub(crate) cover_path: String,
    pub(crate) genre: String,
    pub(crate) year: Option<i32>,
    pub(crate) track_no: Option<u32>,
    pub(crate) duration_secs: Option<f32>,
}

pub struct LibraryService {
    tx: Sender<LibraryCommand>,
}

fn emit_snapshot(event_tx: &Sender<LibraryEvent>, snapshot: &LibrarySnapshot) {
    let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));
}

impl LibraryService {
    #[must_use]
    pub fn new() -> (Self, Receiver<LibraryEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<LibraryCommand>();
        let (event_tx, event_rx) = unbounded::<LibraryEvent>();

        let _ = std::thread::Builder::new()
            .name("ferrous-library".to_string())
            .spawn(move || {
                let mut snapshot = LibrarySnapshot::default();

                match open_library_db() {
                    Ok(conn) => {
                        if let Err(err) = init_schema(&conn) {
                            snapshot.last_error = Some(format!("library DB init failed: {err}"));
                            emit_snapshot(&event_tx, &snapshot);
                            return;
                        }
                        load_snapshot(&conn, &mut snapshot);
                        snapshot.bump_search_revision();
                        emit_snapshot(&event_tx, &snapshot);

                        while let Ok(cmd) = cmd_rx.recv() {
                            snapshot.last_error = None;
                            let reload_search_data_on_success = matches!(
                                &cmd,
                                LibraryCommand::RenameRoot { .. } | LibraryCommand::RemoveRoot(_)
                            );
                            let result = match cmd {
                                LibraryCommand::ScanRoot(root) => {
                                    handle_add_root(&conn, &root, "", &mut snapshot, &event_tx)
                                }
                                LibraryCommand::AddRoot { path, name } => {
                                    handle_add_root(&conn, &path, &name, &mut snapshot, &event_tx)
                                }
                                LibraryCommand::RenameRoot { path, name } => {
                                    rename_root(&conn, &path, &name)
                                }
                                LibraryCommand::RemoveRoot(root) => {
                                    remove_root_and_purge(&conn, &root)
                                }
                                LibraryCommand::RescanRoot(root) => {
                                    handle_rescan_root(&conn, &root, &mut snapshot, &event_tx)
                                }
                                LibraryCommand::RescanAll => {
                                    handle_rescan_all(&conn, &mut snapshot, &event_tx)
                                }
                            };

                            if let Err(err) = &result {
                                snapshot.last_error = Some(err.clone());
                            }

                            load_snapshot(&conn, &mut snapshot);
                            if result.is_ok() && reload_search_data_on_success {
                                snapshot.bump_search_revision();
                            }
                            snapshot.scan_in_progress = false;
                            snapshot.scan_progress = None;
                            emit_snapshot(&event_tx, &snapshot);
                        }
                    }
                    Err(err) => {
                        snapshot.last_error = Some(format!("library DB open failed: {err}"));
                        emit_snapshot(&event_tx, &snapshot);
                    }
                }
            });

        (Self { tx: cmd_tx }, event_rx)
    }

    pub fn command(&self, cmd: LibraryCommand) {
        let _ = self.tx.send(cmd);
    }
}
