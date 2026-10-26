// SPDX-License-Identifier: GPL-3.0-or-later

//! The queue section of the snapshots the relay sends the UI.
//!
//! The relay keeps the queue's rows as the UI has them. The queue update a
//! snapshot carries becomes operations on those rows (insert, remove, move,
//! update), so the UI decodes and changes only the rows that changed. The
//! whole queue goes instead when the UI has none, or when the operations
//! would be larger.
//!
//! The section is little-endian. Every kind starts with a u8 kind, the u64
//! revision of the rows it leaves, the u32 row count, the i32 selected row
//! (-1 for none), the f64 total seconds and the u32 count of rows of
//! unknown length.
//!
//! - Full (0): the rows.
//! - Delta (1): the u64 revision it applies to, a u32 operation count, and
//!   the operations, in order: u8 1 inserts (u32 at, u32 count, the rows),
//!   2 removes (u32 start, u32 count), 3 moves a row (u32 from, u32 to; the
//!   row is taken out, then put back at `to`), 4 updates a row (u32 index,
//!   the row), 5 moves rows (u32 from, u32 count, u32 to; the `count` rows
//!   from `from` on are taken out, then put back from `to` on). Removals of
//!   several ranges come last range first, so each start holds when it is
//!   applied.
//! - Resync (2): the relay lost track of the queue; the UI asks for it whole.
//!
//! A row is the title, artist, album, cover path and genre (strings), an
//! i32 year (`i32::MIN` for none), a u16 track number (0 for none), f32
//! seconds (-1 for unknown) and the path (a string).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use super::ffi::{push_f32, push_f64, push_i32, push_string, push_u16, push_u32, push_u64};
use super::{BridgeSnapshot, QueueEdit, QueueUpdate};
use crate::library::{IndexedTrack, LibrarySnapshot};

pub(super) const QUEUE_FULL: u8 = 0;
pub(super) const QUEUE_DELTA: u8 = 1;
pub(super) const QUEUE_RESYNC: u8 = 2;
const OP_INSERT: u8 = 1;
const OP_REMOVE: u8 = 2;
const OP_MOVE: u8 = 3;
const OP_UPDATE: u8 = 4;
const OP_MOVE_ROWS: u8 = 5;
/// The header every kind starts with.
const HEADER_BYTES: usize = 1 + 8 + 4 + 4 + 8 + 4;
/// Where a delta's base revision, operation count and operations start.
const DELTA_BASE_OFFSET: usize = HEADER_BYTES;
const DELTA_COUNT_OFFSET: usize = DELTA_BASE_OFFSET + 8;
const DELTA_OPS_OFFSET: usize = DELTA_COUNT_OFFSET + 4;
/// Operations up to this size always travel as a delta, which keeps the
/// UI's selection and scroll position; past it, the whole queue goes when
/// it is smaller.
const MIN_FULL_FALLBACK_BYTES: usize = 16 * 1024;

#[derive(Debug, Clone, Default, PartialEq)]
pub(super) struct EncodedQueueTrack {
    pub(super) title: String,
    pub(super) artist: String,
    pub(super) album: String,
    pub(super) cover_path: String,
    pub(super) cover_stamp: u64,
    pub(super) genre: String,
    pub(super) year: Option<i32>,
    pub(super) track_number: Option<u32>,
    pub(super) length_seconds: Option<f32>,
    pub(super) path: String,
}

impl EncodedQueueTrack {
    fn encode(&self, out: &mut Vec<u8>) {
        push_string(out, &self.title);
        push_string(out, &self.artist);
        push_string(out, &self.album);
        push_string(out, &self.cover_path);
        push_u64(out, self.cover_stamp);
        push_string(out, &self.genre);
        push_i32(out, self.year.unwrap_or(i32::MIN));
        push_u16(out, self.track_number.map_or(0, clamp_u32_to_u16));
        push_f32(out, self.length_seconds.unwrap_or(-1.0));
        push_string(out, &self.path);
    }

    fn encoded_len(&self) -> usize {
        6 * 4
            + self.title.len()
            + self.artist.len()
            + self.album.len()
            + self.cover_path.len()
            + self.genre.len()
            + self.path.len()
            + 8
            + 4
            + 2
            + 4
    }

    /// Whether the row counts towards the total length, which needs a
    /// positive one.
    fn known_length(&self) -> Option<f32> {
        self.length_seconds.filter(|seconds| *seconds > 0.0)
    }
}

/// The library's tracks by path, built once per library.
#[derive(Default)]
struct LibraryIndex {
    /// The library indexed. Holding it keeps a newer library from being
    /// built at its address and taken for it.
    library: Option<Arc<LibrarySnapshot>>,
    by_path: HashMap<PathBuf, usize>,
}

impl LibraryIndex {
    fn refresh(&mut self, library: &Arc<LibrarySnapshot>) {
        if self
            .library
            .as_ref()
            .is_some_and(|indexed| Arc::ptr_eq(indexed, library))
        {
            return;
        }
        self.by_path = library
            .tracks
            .iter()
            .enumerate()
            .map(|(index, track)| (track.path.clone(), index))
            .collect();
        self.library = Some(Arc::clone(library));
    }
}

/// The queue changes of a batch of snapshots, before they are encoded.
#[derive(Debug)]
pub(super) enum QueueBatch {
    Full,
    Delta {
        base_revision: u64,
        op_count: u32,
        ops: Vec<u8>,
    },
    Resync,
}

/// The queue's rows as the UI has them once it takes the newest snapshot
/// the relay published.
#[derive(Default)]
pub(super) struct QueueRelay {
    /// The revision of the rows, or none until a whole queue arrives and
    /// after an update that did not fit them.
    revision: Option<u64>,
    paths: Vec<PathBuf>,
    rows: Vec<EncodedQueueTrack>,
    library_index: LibraryIndex,
}

impl QueueRelay {
    /// Apply the queue update `snapshot` carries, adding what changed to
    /// `batch`.
    pub(super) fn apply(&mut self, snapshot: &BridgeSnapshot, batch: &mut Option<QueueBatch>) {
        let Some(update) = snapshot.queue_update.as_ref() else {
            return;
        };
        self.library_index.refresh(&snapshot.library);
        match update {
            QueueUpdate::Full(paths) => {
                let rows = paths.iter().map(|path| self.row(path, snapshot)).collect();
                self.rows = rows;
                self.paths.clone_from(paths);
                self.revision = Some(snapshot.queue_revision);
                *batch = Some(QueueBatch::Full);
            }
            QueueUpdate::Edits {
                base_revision,
                edits,
            } => {
                if self.revision != Some(*base_revision) {
                    self.lose_track(batch);
                    return;
                }
                if batch.is_none() {
                    *batch = Some(QueueBatch::Delta {
                        base_revision: *base_revision,
                        op_count: 0,
                        ops: Vec::new(),
                    });
                }
                for edit in edits {
                    if !self.apply_edit(edit, snapshot, batch) {
                        self.lose_track(batch);
                        return;
                    }
                }
                self.revision = Some(snapshot.queue_revision);
            }
        }
    }

    fn lose_track(&mut self, batch: &mut Option<QueueBatch>) {
        self.revision = None;
        self.paths.clear();
        self.rows.clear();
        *batch = Some(QueueBatch::Resync);
    }

    /// Apply `edit` to the rows. Returns false, before changing anything, if
    /// it does not fit them.
    fn apply_edit(
        &mut self,
        edit: &QueueEdit,
        snapshot: &BridgeSnapshot,
        batch: &mut Option<QueueBatch>,
    ) -> bool {
        let len = self.rows.len();
        match edit {
            QueueEdit::Insert { at, paths } => {
                if *at > len {
                    return false;
                }
                let rows: Vec<EncodedQueueTrack> =
                    paths.iter().map(|path| self.row(path, snapshot)).collect();
                push_op(batch, |out| {
                    out.push(OP_INSERT);
                    push_u32(out, clamp_u32(*at));
                    push_u32(out, clamp_u32(rows.len()));
                    for row in &rows {
                        row.encode(out);
                    }
                });
                self.paths.splice(*at..*at, paths.iter().cloned());
                self.rows.splice(*at..*at, rows);
            }
            QueueEdit::Remove(indices) => {
                let ascending = indices.windows(2).all(|pair| pair[0] < pair[1]);
                if !ascending || indices.last().is_some_and(|last| *last >= len) {
                    return false;
                }
                for (start, count) in descending_ranges(indices) {
                    push_op(batch, |out| {
                        out.push(OP_REMOVE);
                        push_u32(out, clamp_u32(start));
                        push_u32(out, clamp_u32(count));
                    });
                    self.paths.drain(start..start + count);
                    self.rows.drain(start..start + count);
                }
            }
            QueueEdit::Move { from, to } => {
                if *from >= len || *to >= len {
                    return false;
                }
                push_op(batch, |out| {
                    out.push(OP_MOVE);
                    push_u32(out, clamp_u32(*from));
                    push_u32(out, clamp_u32(*to));
                });
                let path = self.paths.remove(*from);
                self.paths.insert(*to, path);
                let row = self.rows.remove(*from);
                self.rows.insert(*to, row);
            }
            QueueEdit::MoveRows { from, count, to } => {
                if *count == 0 || from + count > len || to + count > len {
                    return false;
                }
                push_op(batch, |out| {
                    out.push(OP_MOVE_ROWS);
                    push_u32(out, clamp_u32(*from));
                    push_u32(out, clamp_u32(*count));
                    push_u32(out, clamp_u32(*to));
                });
                let paths: Vec<PathBuf> = self.paths.drain(*from..*from + *count).collect();
                self.paths.splice(*to..*to, paths);
                let rows: Vec<EncodedQueueTrack> = self.rows.drain(*from..*from + *count).collect();
                self.rows.splice(*to..*to, rows);
            }
            QueueEdit::SetPaths(renamed) => {
                if renamed.iter().any(|(index, _)| *index >= len) {
                    return false;
                }
                for (index, path) in renamed {
                    self.paths[*index].clone_from(path);
                    self.update_row(*index, snapshot, batch);
                }
            }
            QueueEdit::Refresh => {
                for index in 0..len {
                    self.update_row(index, snapshot, batch);
                }
            }
        }
        true
    }

    /// Encode the row at `index` again, and send it if it changed.
    fn update_row(
        &mut self,
        index: usize,
        snapshot: &BridgeSnapshot,
        batch: &mut Option<QueueBatch>,
    ) {
        let row = self.row(&self.paths[index], snapshot);
        if row == self.rows[index] {
            return;
        }
        push_op(batch, |out| {
            out.push(OP_UPDATE);
            push_u32(out, clamp_u32(index));
            row.encode(out);
        });
        self.rows[index] = row;
    }

    fn row(&self, path: &Path, snapshot: &BridgeSnapshot) -> EncodedQueueTrack {
        let path_text = path.to_string_lossy().into_owned();
        let fallback_title = path.file_name().map_or_else(
            || path_text.clone(),
            |name| name.to_string_lossy().into_owned(),
        );
        let library_track = self
            .library_index
            .by_path
            .get(path)
            .and_then(|index| snapshot.library.tracks.get(*index));
        let track = if let Some(track) = library_track {
            TrackDetails {
                title: &track.title,
                artist: &track.artist,
                album: &track.album,
                cover_path: &track.cover_path,
                cover_stamp: track.cover_stamp,
                genre: &track.genre,
                year: track.year,
                track_number: track.track_no,
                duration_secs: track.duration_secs,
            }
        } else if let Some(track) = snapshot.queue_details.get(path) {
            TrackDetails::indexed(track)
        } else {
            return EncodedQueueTrack {
                title: fallback_title,
                path: path_text,
                ..EncodedQueueTrack::default()
            };
        };
        EncodedQueueTrack {
            title: if track.title.trim().is_empty() {
                fallback_title
            } else {
                track.title.to_string()
            },
            artist: track.artist.to_string(),
            album: track.album.to_string(),
            cover_path: track.cover_path.to_string(),
            cover_stamp: track.cover_stamp,
            genre: track.genre.to_string(),
            year: track.year,
            track_number: track.track_number,
            length_seconds: track
                .duration_secs
                .filter(|seconds| seconds.is_finite() && *seconds >= 0.0),
            path: path_text,
        }
    }

    /// The section for `batch`, with the selection of `snapshot`, the
    /// newest snapshot of the batch.
    pub(super) fn section(&self, batch: QueueBatch, snapshot: &BridgeSnapshot) -> Vec<u8> {
        let selected = snapshot.selected_queue_index.map_or(-1, clamp_i32);
        match batch {
            QueueBatch::Full => self.full_section(selected),
            QueueBatch::Resync => self.resync_section(),
            QueueBatch::Delta {
                base_revision,
                op_count,
                ops,
            } => {
                if self.full_is_smaller(ops.len()) {
                    return self.full_section(selected);
                }
                let mut out = Vec::with_capacity(DELTA_OPS_OFFSET + ops.len());
                self.push_header(&mut out, QUEUE_DELTA, selected);
                push_u64(&mut out, base_revision);
                push_u32(&mut out, op_count);
                out.extend_from_slice(&ops);
                out
            }
        }
    }

    /// The section for a snapshot that replaces one the UI has not taken:
    /// `newer` follows `older`, so a delta in it builds on what `older`
    /// leaves. The rows are at `newer`'s revision now.
    pub(super) fn merge(&self, newer: &[u8], older: &[u8]) -> Vec<u8> {
        let Some(newer_delta) = Delta::parse(newer) else {
            return newer.to_vec();
        };
        let selected = header_selected(newer);
        let Some(older_delta) = Delta::parse(older) else {
            // The UI's queue is older still, or whole: it gets the rows.
            return if self.revision.is_some() {
                self.full_section(selected)
            } else {
                self.resync_section()
            };
        };
        if older_delta.revision != newer_delta.base_revision
            || self.full_is_smaller(older_delta.ops.len() + newer_delta.ops.len())
        {
            return self.full_section(selected);
        }
        let mut out = Vec::with_capacity(newer.len() + older_delta.ops.len());
        out.extend_from_slice(&newer[..DELTA_BASE_OFFSET]);
        push_u64(&mut out, older_delta.base_revision);
        push_u32(
            &mut out,
            older_delta.op_count.saturating_add(newer_delta.op_count),
        );
        out.extend_from_slice(older_delta.ops);
        out.extend_from_slice(newer_delta.ops);
        out
    }

    fn rows_bytes(&self) -> usize {
        self.rows.iter().map(EncodedQueueTrack::encoded_len).sum()
    }

    /// Whether the whole queue is smaller than operations of `ops_bytes`.
    fn full_is_smaller(&self, ops_bytes: usize) -> bool {
        ops_bytes > MIN_FULL_FALLBACK_BYTES && ops_bytes > self.rows_bytes()
    }

    fn push_header(&self, out: &mut Vec<u8>, kind: u8, selected: i32) {
        let mut total_seconds = 0.0;
        let mut unknown = 0usize;
        for row in &self.rows {
            match row.known_length() {
                Some(seconds) => total_seconds += f64::from(seconds),
                None => unknown += 1,
            }
        }
        out.push(kind);
        push_u64(out, self.revision.unwrap_or(0));
        push_u32(out, clamp_u32(self.rows.len()));
        push_i32(out, selected);
        push_f64(out, total_seconds);
        push_u32(out, clamp_u32(unknown));
    }

    fn full_section(&self, selected: i32) -> Vec<u8> {
        let mut out = Vec::with_capacity(HEADER_BYTES + self.rows_bytes());
        self.push_header(&mut out, QUEUE_FULL, selected);
        for row in &self.rows {
            row.encode(&mut out);
        }
        out
    }

    fn resync_section(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(HEADER_BYTES);
        self.push_header(&mut out, QUEUE_RESYNC, -1);
        out
    }

    #[cfg(test)]
    pub(super) fn rows(&self) -> &[EncodedQueueTrack] {
        &self.rows
    }
}

struct TrackDetails<'a> {
    title: &'a str,
    artist: &'a str,
    album: &'a str,
    cover_path: &'a str,
    cover_stamp: u64,
    genre: &'a str,
    year: Option<i32>,
    track_number: Option<u32>,
    duration_secs: Option<f32>,
}

impl<'a> TrackDetails<'a> {
    fn indexed(track: &'a IndexedTrack) -> Self {
        Self {
            title: &track.title,
            artist: &track.artist,
            album: &track.album,
            cover_path: &track.cover_path,
            cover_stamp: track.cover_stamp,
            genre: &track.genre,
            year: track.year,
            track_number: track.track_no,
            duration_secs: track.duration_secs,
        }
    }
}

/// The parts of a delta section.
struct Delta<'a> {
    revision: u64,
    base_revision: u64,
    op_count: u32,
    ops: &'a [u8],
}

impl<'a> Delta<'a> {
    fn parse(section: &'a [u8]) -> Option<Self> {
        if section.first() != Some(&QUEUE_DELTA) || section.len() < DELTA_OPS_OFFSET {
            return None;
        }
        let u64_at = |offset: usize| {
            u64::from_le_bytes(section[offset..offset + 8].try_into().unwrap_or_default())
        };
        Some(Self {
            revision: u64_at(1),
            base_revision: u64_at(DELTA_BASE_OFFSET),
            op_count: u32::from_le_bytes(
                section[DELTA_COUNT_OFFSET..DELTA_OPS_OFFSET]
                    .try_into()
                    .unwrap_or_default(),
            ),
            ops: &section[DELTA_OPS_OFFSET..],
        })
    }
}

/// The selected row in a section's header.
fn header_selected(section: &[u8]) -> i32 {
    section
        .get(13..17)
        .and_then(|bytes| bytes.try_into().ok())
        .map_or(-1, i32::from_le_bytes)
}

fn push_op(batch: &mut Option<QueueBatch>, write: impl FnOnce(&mut Vec<u8>)) {
    if let Some(QueueBatch::Delta { op_count, ops, .. }) = batch {
        *op_count = op_count.saturating_add(1);
        write(ops);
    }
}

/// Ascending distinct `indices` as (start, count) ranges, last first.
fn descending_ranges(indices: &[usize]) -> Vec<(usize, usize)> {
    let mut ranges: Vec<(usize, usize)> = Vec::new();
    for &index in indices {
        match ranges.last_mut() {
            Some((start, count)) if *start + *count == index => *count += 1,
            _ => ranges.push((index, 1)),
        }
    }
    ranges.reverse();
    ranges
}

fn clamp_u32(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

fn clamp_i32(value: usize) -> i32 {
    i32::try_from(value).unwrap_or(i32::MAX)
}

fn clamp_u32_to_u16(value: u32) -> u16 {
    u16::try_from(value).unwrap_or(u16::MAX)
}
