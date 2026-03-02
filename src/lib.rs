// Temporary pedantic-lint baseline so strict clippy can be part of regular checks.
// Keep this list shrinking over time; see docs/ROADMAP.md quality/performance section.
#![allow(
    clippy::assigning_clones,
    clippy::bool_to_int_with_if,
    clippy::cast_lossless,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    clippy::collapsible_if,
    clippy::default_trait_access,
    clippy::field_reassign_with_default,
    clippy::implicit_hasher,
    clippy::manual_div_ceil,
    clippy::manual_is_multiple_of,
    clippy::manual_let_else,
    clippy::map_unwrap_or,
    clippy::match_same_arms,
    clippy::missing_safety_doc,
    clippy::must_use_candidate,
    clippy::needless_pass_by_value,
    clippy::needless_range_loop,
    clippy::needless_raw_string_hashes,
    clippy::ptr_arg,
    clippy::question_mark,
    clippy::redundant_closure_for_method_calls,
    clippy::result_large_err,
    clippy::semicolon_if_nothing_returned,
    clippy::similar_names,
    clippy::single_match_else,
    clippy::struct_excessive_bools,
    clippy::too_many_arguments,
    clippy::too_many_lines,
    clippy::type_complexity,
    clippy::uninlined_format_args,
    clippy::unnecessary_cast,
    clippy::unreadable_literal
)]

pub mod analysis;
pub mod app;
pub mod frontend_bridge;
pub mod library;
pub mod metadata;
pub mod playback;
pub mod ui;
