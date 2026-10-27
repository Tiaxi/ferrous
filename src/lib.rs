// SPDX-License-Identifier: GPL-3.0-or-later

// CI and scripts/run-tests.sh deny Clippy's pedantic group for all targets,
// but test code exempts it: tests compare exact float fixtures, cast fixture
// values, and spell out long scenarios step by step on purpose. Shipped code
// is still checked with the group, because it builds without `cfg(test)`;
// default lints, `-D warnings` and the unsafe lints still apply to tests.
#![cfg_attr(test, allow(clippy::pedantic))]

pub mod analysis;
pub mod artwork;
mod collation;
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
