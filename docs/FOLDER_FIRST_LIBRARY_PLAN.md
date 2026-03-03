# Folder-First Library Plan (Revised for Network Share + Multi-Root Settings)

## Summary
Build a read-only, folder-structure-based library over configurable root folders (initially empty).
Single-root view keeps artists at top level; multi-root view shows roots first.
Album title/year come from metadata with deterministic fallbacks, disc/subfolder sections are preserved, loose tracks are shown directly under artist, and context menus include native file-browser actions (`Open in Dolphin` on the current KDE setup).

## Functional Spec
1. Library roots are user-configurable in Settings, support many entries, persist across restarts, and start empty.
2. Startup loads cached index immediately and does not auto-rescan by default.
3. Adding a root persists it and starts scanning immediately.
4. Removing a root removes that root and purges indexed tracks under that root path.
5. UI offers per-root rescan plus `Rescan all`.
6. Tree top-level behavior:
   1. If one root is configured, top-level rows are artists.
   2. If two or more roots are configured, top-level rows are roots.
7. Artist rows format as `Artist name (Number of albums)`, where count includes only real album folders.
8. Loose tracks directly under artist folder are shown as track rows directly under that artist (not as synthetic album).
9. Album rows format as `<Cover art> Title` or `<Cover art> Title (Year)` when year exists.
10. Album title/year resolution:
   1. Year = most common parsed year from tracks in album scope, tie -> earliest year.
   2. Title = metadata album title only when consistent; otherwise fallback to album folder name.
11. Album ordering per artist:
   1. Default = year ascending, unknown-year albums last, tie-break by title.
   2. Alternate = title ascending, tie-break by year.
   3. Sort mode is controlled by library toolbar dropdown and persisted.
12. Subfolder/disc behavior:
   1. Any immediate album subfolder containing supported audio becomes a subsection row with the exact folder name.
   2. Non-audio folders (for example `Artwork`) are excluded.
   3. Mixed albums keep root-level album tracks directly under album, and also show subsection rows.
13. Track rows format as `<zeropadded number> - <title>`.
14. Track number fallback chain is metadata track number -> leading filename digits -> stable positional index.
15. Cover art rules:
   1. Album cover can be any `*.jpg` or `*.png` inside album folder.
   2. If album has no image in album folder and has subsections, use image from first subsection (natural sort order).
   3. Subsection rows do not show cover art.
16. Supported formats minimum: FLAC, MP3, M4A, AAC (index + playback support target).
17. Additional trivial formats are added where backend support already exists without major refactor (planned include: OGG, OPUS, WAV).
18. All actions remain read-only against music source; no tag/file/folder modifications are performed.

## Scan Performance and Progress
1. Scanning remains asynchronous on a background worker so UI never blocks.
2. Scans are optimized for network share:
   1. Single-pass walk (no expensive pre-count pass).
   2. Early extension filtering before metadata parsing.
   3. Incremental reindex using mtime+size to skip unchanged metadata reads.
   4. Batched DB writes in transaction.
   5. One active root scan at a time to avoid network thrash.
3. Progress model exposed to UI:
   1. Current root path.
   2. Roots completed / total.
   3. Supported files discovered.
   4. Supported files processed.
   5. Files per second (smoothed).
   6. Estimated time remaining (rough).
4. ETA strategy is rough-by-design for speed:
   1. `estimated_total = max(discovered_supported_files, previous_index_count_for_root)`.
   2. `eta = (estimated_total - processed) / rate` when rate is stable; otherwise hidden.
5. UI shows spinner + progress text + optional progress bar with indeterminate fallback when ETA is unavailable.

## Context Menu and File Browser Behavior
1. Every library row gets `Open in <FileBrowserName>`:
   1. Root -> open root folder.
   2. Artist -> open artist folder.
   3. Album -> open album folder.
   4. Subsection -> open subsection folder.
   5. Track -> open containing folder.
2. Track rows additionally expose explicit `Open containing folder`.
3. Playlist rows get `Open containing folder`.
4. File browser name is detected dynamically from system directory handler; current environment resolves to `Dolphin`, so label is `Open in Dolphin`; fallback label is `Open in File Manager`.

## API / Interface Changes
1. Rust bridge/library commands gain root-management operations: add/remove/rescan-one/rescan-all.
2. JSON bridge protocol gains matching commands and snapshot fields for root list plus scan progress metrics.
3. Library snapshot payload changes from tag-grouped album list to folder-driven hierarchical tree with row types: root, artist, album, section, track.
4. UI bridge adds invokables for open-folder actions and queue track path lookup.
5. Library model gains new row type (`section`, plus `root` for multi-root mode) and sort-mode input.
6. Settings persistence adds library-root configuration and library sort-mode persistence.

## Implementation Work Plan
1. Extend scanner and DB schema to persist needed per-track metadata for folder-tree rendering and album year derivation.
2. Implement root lifecycle commands and purge semantics in library service.
3. Implement folder-tree builder in backend snapshot encoder with single-root/multi-root top-level logic.
4. Add scan-progress and ETA fields from scanner to bridge snapshots.
5. Update bridge client and model to consume new tree and progress payloads.
6. Update QML library pane for row rendering, subsection rows, sort dropdown, and scanning status UI.
7. Add Settings UI for root management and persistence wiring.
8. Add context-menu open-folder actions in library and playlist.
9. Keep existing playback/queue semantics while switching library actions to path-driven operations.

## Test Cases
1. Single-root tree: artist top level, album/year formatting, track formatting, loose tracks under artist.
2. Multi-root tree: root top-level rows, no accidental cross-root merge of artists.
3. Subfolder filtering: `Artwork` and non-audio directories excluded.
4. Mixed album layout: root tracks + subsection rows together.
5. Multi-disc cover selection: first subsection image used for album cover when needed.
6. Sorting: year mode and title mode with tie-breakers and unknown-year handling.
7. Format handling: FLAC/MP3/M4A/AAC visible and playable; extra formats handled when supported.
8. Scan UX: non-blocking UI during scan, progress fields update, ETA present/hidden appropriately.
9. Root lifecycle: add/scan, remove/purge, rescan one, rescan all, persistence across restart.
10. Context menus: open-folder behavior for all library row types and playlist rows.

## Assumptions and Defaults
1. Zero modifications are made to source music files/folders.
2. Rough ETA is preferred over slower pre-counting on network storage.
3. Artist album count excludes loose tracks.
4. Single-root mode hides root row; multi-root mode shows root rows.
5. Unknown album year omits year parentheses in label.
