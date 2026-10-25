// SPDX-License-Identifier: GPL-3.0-or-later

pub mod analysis;
pub mod artwork;
mod file_edit_lock;
mod file_identity;
mod folder_cover;
pub mod frontend_bridge;
pub mod fuzzy_match;
pub mod lastfm;
pub mod library;
pub mod metadata;
mod no_replace;
mod panic_guard;
mod paths;
pub mod playback;
mod profile_logging;
mod raw_audio;
mod safe_save;
pub mod tag_editor;
mod tag_write;
#[cfg(test)]
mod test_alloc;
mod vorbis_comment_scan;
mod worker_thread;
