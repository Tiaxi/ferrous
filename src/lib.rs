// Temporary pedantic-lint baseline so strict clippy can be part of regular checks.
// Keep this list shrinking over time; see docs/ROADMAP.md quality/performance section.
#![allow(
    clippy::assigning_clones,
    clippy::cast_lossless,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::match_same_arms,
    clippy::missing_safety_doc,
    clippy::must_use_candidate,
    clippy::needless_pass_by_value,
    clippy::needless_range_loop,
    clippy::ptr_arg,
    clippy::question_mark,
    clippy::result_large_err,
    clippy::semicolon_if_nothing_returned,
    clippy::similar_names,
    clippy::single_match_else,
    clippy::struct_excessive_bools,
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::type_complexity,
    clippy::uninlined_format_args
)]

pub mod analysis;
pub mod frontend_bridge;
pub mod lastfm;
pub mod library;
pub mod metadata;
pub mod playback;
mod raw_audio;
