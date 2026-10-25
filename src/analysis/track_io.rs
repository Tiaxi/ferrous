// SPDX-License-Identifier: GPL-3.0-or-later

//! File and database work for the analysis thread. Reading file metadata,
//! probing a file, or waiting for the shared library database can take long
//! on network storage, so it runs here and seeks never wait for it.

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use crossbeam_channel::{Receiver, Sender};
use rusqlite::Connection;

use super::cache::{
    insert_waveform_cache_entry, load_waveform_from_db, open_waveform_cache_db,
    persist_waveform_to_db, prune_persistent_waveform_cache, remap_waveform_cache_entries,
    touch_waveform_cache_lru, WaveformCacheEntry, WaveformSourceStamp,
    PERSISTENT_WAVEFORM_CACHE_MAX_ROWS, PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL,
};
use super::decoders::{open_symphonia_file, SymphoniaFile};
use crate::playback::{remap_path, PathRenames};
#[cfg(feature = "gst")]
use crate::raw_audio::is_raw_surround_file;
use crate::worker_thread::JoinableThread;

/// What a probe found out about a file's audio.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum TrackFormat {
    Known {
        sample_rate: u32,
        channels: usize,
    },
    /// Symphonia cannot read the file, or it is raw surround audio, which
    /// it is not asked to read. The spectrogram worker finds the format
    /// when it opens the file.
    Unknown,
}

/// The format of a file without reading it, when its name tells.
pub(super) fn format_from_name(path: &Path) -> Option<TrackFormat> {
    // Symphonia misreads raw AC3/DTS and spends long on them on network
    // storage; only GStreamer opens them.
    #[cfg(feature = "gst")]
    if is_raw_surround_file(path) {
        return Some(TrackFormat::Unknown);
    }
    let _ = path;
    None
}

fn probe_format(path: &Path) -> TrackFormat {
    if let Some(format) = format_from_name(path) {
        return format;
    }
    open_symphonia_file(path).map_or(
        TrackFormat::Unknown,
        |SymphoniaFile {
             native_sample_rate,
             native_channels,
             ..
         }| TrackFormat::Known {
            sample_rate: u32::try_from(native_sample_rate).unwrap_or(48_000),
            channels: native_channels,
        },
    )
}

#[derive(Debug)]
pub(super) enum TrackIoRequest {
    /// Read the stamp and saved waveform of the track now playing, and its
    /// format when `probe` is set.
    LoadTrack {
        track_token: u64,
        path: PathBuf,
        probe: bool,
    },
    /// Read the format of a likely gapless successor.
    ProbeCandidate {
        request: u64,
        path: PathBuf,
    },
    /// Keep a decoded waveform for when its file plays again.
    SaveWaveform {
        path: PathBuf,
        stamp: Option<WaveformSourceStamp>,
        peaks: Vec<f32>,
    },
    RemapPaths(Arc<PathRenames>),
}

#[derive(Debug, PartialEq)]
pub(super) enum TrackIoReply {
    TrackLoaded {
        track_token: u64,
        stamp: Option<WaveformSourceStamp>,
        /// `None` unless a probe was asked for.
        format: Option<TrackFormat>,
        /// The saved waveform, if the file has not changed since.
        peaks: Option<Vec<f32>>,
    },
    CandidateProbed {
        request: u64,
        format: TrackFormat,
    },
}

/// Waveforms decoded before, in memory and in the library database.
pub(super) struct TrackIo {
    waveforms: HashMap<PathBuf, WaveformCacheEntry>,
    lru: VecDeque<PathBuf>,
    db: Option<Connection>,
    writes_since_prune: usize,
}

impl TrackIo {
    pub(super) fn new(db: Option<Connection>) -> Self {
        Self {
            waveforms: HashMap::new(),
            lru: VecDeque::new(),
            db,
            writes_since_prune: 0,
        }
    }

    pub(super) fn handle(&mut self, request: TrackIoRequest) -> Option<TrackIoReply> {
        match request {
            TrackIoRequest::LoadTrack {
                track_token,
                path,
                probe,
            } => {
                let stamp = source_stamp(&path);
                let format = probe.then(|| probe_format(&path));
                let peaks = self.saved_waveform(&path, stamp);
                Some(TrackIoReply::TrackLoaded {
                    track_token,
                    stamp,
                    format,
                    peaks,
                })
            }
            TrackIoRequest::ProbeCandidate { request, path } => {
                Some(TrackIoReply::CandidateProbed {
                    request,
                    format: probe_format(&path),
                })
            }
            TrackIoRequest::SaveWaveform { path, stamp, peaks } => {
                self.save_waveform(&path, stamp, peaks);
                None
            }
            TrackIoRequest::RemapPaths(renames) => {
                remap_waveform_cache_entries(&mut self.waveforms, &mut self.lru, &renames);
                None
            }
        }
    }

    fn saved_waveform(
        &mut self,
        path: &Path,
        stamp: Option<WaveformSourceStamp>,
    ) -> Option<Vec<f32>> {
        let in_memory = self
            .waveforms
            .get(path)
            .filter(|entry| entry.stamp == stamp && !entry.peaks.is_empty())
            .map(|entry| entry.peaks.clone());
        if in_memory.is_some() {
            touch_waveform_cache_lru(&mut self.lru, path);
            return in_memory;
        }
        let peaks = load_waveform_from_db(self.db.as_ref()?, path, stamp?)?;
        insert_waveform_cache_entry(
            &mut self.waveforms,
            &mut self.lru,
            path,
            WaveformCacheEntry {
                stamp,
                peaks: peaks.clone(),
            },
        );
        Some(peaks)
    }

    fn save_waveform(&mut self, path: &Path, stamp: Option<WaveformSourceStamp>, peaks: Vec<f32>) {
        if let (Some(db), Some(stamp)) = (self.db.as_ref(), stamp) {
            let _ = persist_waveform_to_db(db, path, stamp, &peaks);
            self.writes_since_prune = self.writes_since_prune.saturating_add(1);
            if self.writes_since_prune >= PERSISTENT_WAVEFORM_CACHE_PRUNE_INTERVAL {
                let _ = prune_persistent_waveform_cache(db, PERSISTENT_WAVEFORM_CACHE_MAX_ROWS);
                self.writes_since_prune = 0;
            }
        }
        insert_waveform_cache_entry(
            &mut self.waveforms,
            &mut self.lru,
            path,
            WaveformCacheEntry { stamp, peaks },
        );
    }
}

/// Requests in arrival order, without loads and probes that a later one of
/// the same kind supersedes: the analysis thread only waits for its latest.
fn without_superseded(batch: Vec<TrackIoRequest>) -> Vec<TrackIoRequest> {
    let last_load = batch
        .iter()
        .rposition(|request| matches!(request, TrackIoRequest::LoadTrack { .. }));
    let last_probe = batch
        .iter()
        .rposition(|request| matches!(request, TrackIoRequest::ProbeCandidate { .. }));
    batch
        .into_iter()
        .enumerate()
        .filter(|(index, request)| match request {
            TrackIoRequest::LoadTrack { .. } => Some(*index) == last_load,
            TrackIoRequest::ProbeCandidate { .. } => Some(*index) == last_probe,
            _ => true,
        })
        .map(|(_, request)| request)
        .collect()
}

/// Start the thread that answers the analysis thread's requests. It opens
/// the library database itself, and ends when the requests' sender is
/// dropped.
pub(super) fn spawn_track_io(
    requests: Receiver<TrackIoRequest>,
    replies: Sender<TrackIoReply>,
) -> Option<JoinableThread> {
    crate::worker_thread::spawn_worker("ferrous-analysis-io", move || {
        let mut io = TrackIo::new(open_waveform_cache_db().ok());
        while let Ok(first) = requests.recv() {
            let mut batch = vec![first];
            batch.extend(requests.try_iter());
            for request in without_superseded(batch) {
                if let Some(reply) = io.handle(request) {
                    if replies.send(reply).is_err() {
                        return;
                    }
                }
            }
        }
    })
}

pub(super) fn source_stamp(path: &Path) -> Option<WaveformSourceStamp> {
    let meta = std::fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?;
    let since_epoch = modified.duration_since(UNIX_EPOCH).ok()?;
    Some(WaveformSourceStamp {
        size_bytes: meta.len(),
        modified_secs: since_epoch.as_secs(),
        modified_nanos: since_epoch.subsec_nanos(),
    })
}

/// Formats of recently seen files, so a gapless successor that was probed
/// before it plays, or a track played again, is not probed again.
#[derive(Default)]
pub(super) struct FormatCache {
    /// Oldest first.
    entries: Vec<(PathBuf, TrackFormat)>,
}

const FORMAT_CACHE_ENTRIES: usize = 16;

impl FormatCache {
    pub(super) fn get(&self, path: &Path) -> Option<TrackFormat> {
        self.entries
            .iter()
            .find(|(cached, _)| cached == path)
            .map(|&(_, format)| format)
    }

    pub(super) fn insert(&mut self, path: PathBuf, format: TrackFormat) {
        self.entries.retain(|(cached, _)| *cached != path);
        if self.entries.len() >= FORMAT_CACHE_ENTRIES {
            self.entries.remove(0);
        }
        self.entries.push((path, format));
    }

    /// Keep each format with its file. A file renamed onto another's path
    /// replaces it, and a swap moves each entry once.
    pub(super) fn remap(&mut self, renames: &PathRenames) {
        self.entries.retain(|(path, _)| {
            renames.contains_key(path) || !renames.values().any(|new| new == path)
        });
        for (path, _) in &mut self.entries {
            remap_path(path, renames);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::cache::init_waveform_cache_schema;
    use super::*;

    fn stamp(size_bytes: u64) -> WaveformSourceStamp {
        WaveformSourceStamp {
            size_bytes,
            modified_secs: 1,
            modified_nanos: 0,
        }
    }

    fn database() -> Connection {
        let db = Connection::open_in_memory().expect("database");
        init_waveform_cache_schema(&db).expect("schema");
        db
    }

    #[test]
    fn saved_waveforms_come_back_while_their_files_are_unchanged() {
        let mut io = TrackIo::new(Some(database()));
        let path = Path::new("/music/a.flac");
        io.save_waveform(path, Some(stamp(10)), vec![0.5, 0.25]);
        assert_eq!(
            io.saved_waveform(path, Some(stamp(10))),
            Some(vec![0.5, 0.25])
        );
        assert_eq!(io.saved_waveform(path, Some(stamp(11))), None);

        // A new session finds it in the database.
        let mut reopened = TrackIo::new(io.db.take());
        assert_eq!(
            reopened.saved_waveform(path, Some(stamp(10))),
            Some(vec![0.5, 0.25])
        );
    }

    #[test]
    fn a_swap_keeps_each_file_with_its_own_waveform() {
        let (a, b) = (Path::new("/music/a.flac"), Path::new("/music/b.flac"));
        let mut io = TrackIo::new(None);
        io.save_waveform(a, Some(stamp(10)), vec![0.1]);
        io.save_waveform(b, Some(stamp(10)), vec![0.2]);
        io.handle(TrackIoRequest::RemapPaths(Arc::new(PathRenames::from([
            (a.to_path_buf(), b.to_path_buf()),
            (b.to_path_buf(), a.to_path_buf()),
        ]))));
        assert_eq!(io.saved_waveform(b, Some(stamp(10))), Some(vec![0.1]));
        assert_eq!(io.saved_waveform(a, Some(stamp(10))), Some(vec![0.2]));
    }

    #[test]
    fn a_load_reads_the_stamp_and_probes_only_when_asked() {
        let path =
            std::env::temp_dir().join(format!("ferrous-track-io-{}.mp3", std::process::id()));
        std::fs::write(&path, include_bytes!("fixtures/seek-tone.mp3")).expect("fixture");
        let mut io = TrackIo::new(None);
        for probe in [false, true] {
            let reply = io.handle(TrackIoRequest::LoadTrack {
                track_token: 3,
                path: path.clone(),
                probe,
            });
            let Some(TrackIoReply::TrackLoaded {
                track_token: 3,
                stamp: Some(_),
                format,
                peaks: None,
            }) = reply
            else {
                panic!("unexpected reply {reply:?}");
            };
            assert_eq!(format.is_some(), probe);
            if let Some(format) = format {
                assert!(matches!(format, TrackFormat::Known { channels: 1, .. }));
            }
        }
        std::fs::remove_file(path).expect("remove fixture");
    }

    #[test]
    fn only_the_latest_load_and_probe_of_a_batch_run() {
        let load = |track_token| TrackIoRequest::LoadTrack {
            track_token,
            path: PathBuf::from("/music/a.flac"),
            probe: true,
        };
        let probe = |request| TrackIoRequest::ProbeCandidate {
            request,
            path: PathBuf::from("/music/b.flac"),
        };
        let save = TrackIoRequest::SaveWaveform {
            path: PathBuf::from("/music/a.flac"),
            stamp: None,
            peaks: vec![0.5],
        };
        let kept: Vec<_> = without_superseded(vec![load(1), probe(1), save, load(2), probe(2)])
            .into_iter()
            .map(|request| match request {
                TrackIoRequest::LoadTrack { track_token, .. } => format!("load {track_token}"),
                TrackIoRequest::ProbeCandidate { request, .. } => format!("probe {request}"),
                TrackIoRequest::SaveWaveform { .. } => "save".to_string(),
                TrackIoRequest::RemapPaths(_) => "remap".to_string(),
            })
            .collect();
        assert_eq!(kept, ["save", "load 2", "probe 2"]);
    }

    #[test]
    fn formats_follow_renames_and_stay_bounded() {
        let known = |channels| TrackFormat::Known {
            sample_rate: 48_000,
            channels,
        };
        let mut cache = FormatCache::default();
        let (a, b, c) = (
            PathBuf::from("/music/a.flac"),
            PathBuf::from("/music/b.flac"),
            PathBuf::from("/music/c.flac"),
        );
        cache.insert(a.clone(), known(1));
        cache.insert(b.clone(), known(2));
        cache.insert(c.clone(), known(6));
        // A and B swap; C is replaced by the file renamed from D.
        cache.remap(&PathRenames::from([
            (a.clone(), b.clone()),
            (b.clone(), a.clone()),
            (PathBuf::from("/music/d.flac"), c.clone()),
        ]));
        assert_eq!(cache.get(&a), Some(known(2)));
        assert_eq!(cache.get(&b), Some(known(1)));
        assert_eq!(cache.get(&c), None);

        for index in 0..FORMAT_CACHE_ENTRIES {
            cache.insert(PathBuf::from(format!("/music/{index}.flac")), known(2));
        }
        assert_eq!(cache.entries.len(), FORMAT_CACHE_ENTRIES);
        assert_eq!(cache.get(&a), None, "the oldest entries are dropped");
    }
}
