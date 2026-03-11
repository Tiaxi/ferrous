# Unified Tag Editor (MP3Tag-style v1)

## Summary
Build a single large, resizable modal tag-editor dialog that is launched from the playlist and library context menus and mirrors MP3Tag's [Main Window](https://docs.mp3tag.de/getting-started/main-window/) pattern: editable file list on the right, bulk-edit tag panel on the left, explicit Save/Cancel, and multi-file `< keep >` semantics. Keep MP3Tag-like extended/custom fields from [Extended Tags](https://docs.mp3tag.de/extended-tags/) out of v1; this slice covers the main editor flow only.

Use current selection semantics:
- Playlist: `Edit Tags` opens the current multi-selection if the clicked row is selected; otherwise only the clicked track.
- Library track row: open that one file.
- Library album row: open only that album folder's root-level tracks, never descendant section/disc folders.
- Library section/disc row: open only that folder's files.
- Mixed multi-selection: combine scopes in visible order and deduplicate paths.

Standard writable formats use [Lofty 0.23.2](https://docs.rs/crate/lofty/0.23.2); AC3/DTS use appended APEv2 read/write in-repo.

## Key Changes
### UI and interaction
- Add `Edit Tags` to:
  - playlist row context menu
  - library track context menu
  - library album context menu
  - library section/disc context menu
- Implement one shared editor dialog with:
  - left tag panel fields: `Title`, `Artist`, `Album`, `Album Artist`, `Genre`, `Year`, `Track No`, `Disc No`, `Total Tracks`, `Total Discs`, `Comment`
  - right editable table of opened files with read-only filename/path context and editable tag columns for the same fields
  - explicit `Save`, `Cancel`, `Reload`, `Auto Number`, and per-field case-action buttons
- Bulk-edit semantics:
  - if all selected rows share a value, show it
  - if values differ, show `< keep >`
  - leaving `< keep >` untouched preserves per-file values
  - entering an empty value clears that field for the targeted rows
- Case actions are manual only:
  - English fields (`Title`, `Artist`, `Album`, `Album Artist`): `English Title Case`
  - Finnish fields (`Title`, `Artist`, `Album`, `Album Artist`): `Finnish Capitalize` (first letter uppercase, rest lowercase)
  - `Genre`: `Capitalize Genre` (first letter uppercase, rest lowercase)
  - actions apply to selected table rows; if no rows are selected, apply to all loaded rows
- Auto-number sub-dialog:
  - target selected rows or all rows if none selected
  - inputs: starting track, starting disc, write totals, reset-on-section/folder toggle, reset-on-disc-change toggle
  - numbering follows current table order
  - display uses leading zeros based on the computed maximum width
  - save preserves padded text where the tag format supports textual number fields, and falls back to native numeric storage where the format is numeric-only

### Data flow and interfaces
- Keep transient editor state out of the global snapshot.
- Add direct Rust FFI helpers for editor-specific I/O:
  - `ferrous_ffi_tag_editor_load(paths_blob) -> session_blob`
  - `ferrous_ffi_tag_editor_save(save_blob) -> result_blob`
  - matching free-buffer helpers
- Add a Qt-side `TagEditorController` plus `TagEditorTableModel` to own:
  - loaded row data
  - dirty tracking
  - selection-aware bulk edits
  - case transforms
  - numbering transforms
  - save/reload lifecycle
- After save succeeds, send one bridge command to refresh edited paths in runtime state:
  - re-read current-track metadata if affected
  - refresh queue detail cache for edited paths
  - refresh library/external track cache for edited paths
  - rebuild library tree/search data if any library-backed path changed

### Rust tag I/O
- Introduce an editor-focused tag row type separate from playback/library snapshot structs so album artist, totals, and comment do not have to become global snapshot fields.
- Standard formats:
  - read/write canonical fields through Lofty tags, creating a writable primary tag if missing
  - use native track/disc + total fields where available
- AC3/DTS:
  - extend `raw_audio` APEv2 parsing/writing to support `Title`, `Artist`, `Album`, `Album Artist`, `Genre`, `Year`, `Track`, `Disc`, `Comment`
  - encode totals in `Track` and `Disc` as `NN/TT` or `N/T`
  - preserve/update existing appended APEv2 blocks safely rather than stacking duplicates
- Save results are per-file so the UI can surface partial failures without losing successful writes.

## Public Interfaces / Types
- New C-ABI helpers for tag-editor load/save blobs.
- New Qt types: `TagEditorController`, `TagEditorTableModel`.
- New bridge command for post-save refresh of edited paths.
- New Rust editor row/save payload types carrying:
  - path
  - writable-format kind
  - field values
  - mixed-value/common-value summaries
  - per-file save outcome

## Test Plan
- Rust roundtrip tests for standard-format read/write of all v1 fields.
- Rust roundtrip tests for AC3/DTS appended APEv2 read/write, including album artist, comment, `track/total`, and `disc/total`.
- Scope tests:
  - album row excludes descendant section/disc files
  - section row includes only section files
  - mixed playlist/library multi-selection deduplicates in visible order
- Refresh tests:
  - saving edited files updates current-track metadata if the playing file was edited
  - saving edited library files refreshes tree/search-visible fields without a full rescan
- QML/Qt tests:
  - context menus expose `Edit Tags` only on supported rows
  - bulk panel shows `< keep >` for differing values
  - inline list edits mark rows dirty
  - case actions and auto-numbering modify the intended target rows only
  - save surfaces partial failures and keeps unsaved dirty state until resolved

## Assumptions and Defaults
- Save behavior is explicit `Save`/`Cancel`; no autosave on selection change.
- V1 does not include cover-art editing, custom fields, or a separate extended-tags dialog.
- Global search and now-playing views do not get `Edit Tags` in this slice.
- English/Finnish casing is user-invoked, not auto-detected and not auto-applied on plain save.
- MP4/native numeric formats may not retain textual zero padding on disk; the feature guarantees padded persistence only where the underlying format stores text.
