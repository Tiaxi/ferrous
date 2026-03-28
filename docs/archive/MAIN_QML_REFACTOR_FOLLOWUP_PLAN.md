# Main QML Refactor Follow-Up Status

## Summary

Checkpoint `73408ac` (`Refactor QML shell into reusable modules`) completed the first structural split of [ui/qml/Main.qml](../ui/qml/Main.qml). Since then, the remaining large feature slices have also been extracted: sidebar/queue pane adoption, global search, album-art and iTunes artwork flows, the tag editor subtree, and domain-scoped queue/library/global-search controllers.

This document now captures the current status of that refactor, what was completed, and the smaller cleanup-oriented follow-up work that still makes sense if we want [ui/qml/Main.qml](../ui/qml/Main.qml) to move closer to a pure composition root.

## Current Status

The following pieces are extracted and registered in [ui/CMakeLists.txt](../ui/CMakeLists.txt):

- Shared JS/helpers:
  - [ui/qml/logic/ColorUtils.js](../ui/qml/logic/ColorUtils.js)
  - [ui/qml/logic/FormatUtils.js](../ui/qml/logic/FormatUtils.js)
  - [ui/qml/logic/PathUtils.js](../ui/qml/logic/PathUtils.js)
- Shared components:
  - [ui/qml/components/UiPalette.qml](../ui/qml/components/UiPalette.qml)
  - [ui/qml/components/PopupTransition.qml](../ui/qml/components/PopupTransition.qml)
  - [ui/qml/components/SurfaceCard.qml](../ui/qml/components/SurfaceCard.qml)
  - [ui/qml/components/ViewerCloseButton.qml](../ui/qml/components/ViewerCloseButton.qml)
  - [ui/qml/components/MetadataMarqueeRow.qml](../ui/qml/components/MetadataMarqueeRow.qml)
  - [ui/qml/components/AlbumArtTile.qml](../ui/qml/components/AlbumArtTile.qml)
  - [ui/qml/components/TrackMetadataCard.qml](../ui/qml/components/TrackMetadataCard.qml)
  - [ui/qml/components/ColumnHeaderRow.qml](../ui/qml/components/ColumnHeaderRow.qml)
- Dialogs and preference pages:
  - [ui/qml/dialogs/AboutDialog.qml](../ui/qml/dialogs/AboutDialog.qml)
  - [ui/qml/dialogs/DiagnosticsDialog.qml](../ui/qml/dialogs/DiagnosticsDialog.qml)
  - [ui/qml/dialogs/LibraryRootNameDialog.qml](../ui/qml/dialogs/LibraryRootNameDialog.qml)
  - [ui/qml/dialogs/PreferencesDialog.qml](../ui/qml/dialogs/PreferencesDialog.qml)
  - [ui/qml/preferences/LibraryPage.qml](../ui/qml/preferences/LibraryPage.qml)
  - [ui/qml/preferences/SpectrogramPage.qml](../ui/qml/preferences/SpectrogramPage.qml)
  - [ui/qml/preferences/DisplayPage.qml](../ui/qml/preferences/DisplayPage.qml)
  - [ui/qml/preferences/LastFmPage.qml](../ui/qml/preferences/LastFmPage.qml)
  - [ui/qml/preferences/SystemMediaPage.qml](../ui/qml/preferences/SystemMediaPage.qml)
- Shell/pane pieces:
  - [ui/qml/panes/StatusBar.qml](../ui/qml/panes/StatusBar.qml)
  - [ui/qml/panes/TransportBar.qml](../ui/qml/panes/TransportBar.qml)
  - [ui/qml/panes/SpectrogramPane.qml](../ui/qml/panes/SpectrogramPane.qml)
  - [ui/qml/panes/SidebarPane.qml](../ui/qml/panes/SidebarPane.qml)
  - [ui/qml/panes/LibraryPane.qml](../ui/qml/panes/LibraryPane.qml)
  - [ui/qml/panes/QueuePane.qml](../ui/qml/panes/QueuePane.qml)
- Controllers:
  - [ui/qml/controllers/GlobalSearchController.qml](../ui/qml/controllers/GlobalSearchController.qml)
  - [ui/qml/controllers/LibraryController.qml](../ui/qml/controllers/LibraryController.qml)
  - [ui/qml/controllers/QueueController.qml](../ui/qml/controllers/QueueController.qml)
- Search and artwork flows:
  - [ui/qml/dialogs/GlobalSearchDialog.qml](../ui/qml/dialogs/GlobalSearchDialog.qml)
  - [ui/qml/viewers/AlbumArtSurface.qml](../ui/qml/viewers/AlbumArtSurface.qml)
  - [ui/qml/viewers/AlbumArtViewerShell.qml](../ui/qml/viewers/AlbumArtViewerShell.qml)
  - [ui/qml/dialogs/ItunesArtworkDialog.qml](../ui/qml/dialogs/ItunesArtworkDialog.qml)
- Spectrogram viewer pieces:
  - [ui/qml/viewers/SpectrogramSurface.qml](../ui/qml/viewers/SpectrogramSurface.qml)
  - [ui/qml/viewers/SpectrogramViewerShell.qml](../ui/qml/viewers/SpectrogramViewerShell.qml)
- Tag editor pieces:
  - [ui/qml/dialogs/TagEditorDialog.qml](../ui/qml/dialogs/TagEditorDialog.qml)
  - [ui/qml/dialogs/AutoNumberDialog.qml](../ui/qml/dialogs/AutoNumberDialog.qml)
  - [ui/qml/dialogs/TagEditorCloseConfirmDialog.qml](../ui/qml/dialogs/TagEditorCloseConfirmDialog.qml)
  - [ui/qml/dialogs/TagEditorStatusDetailsDialog.qml](../ui/qml/dialogs/TagEditorStatusDetailsDialog.qml)

Validation at the current checkpoint:

- `./scripts/run-tests.sh --ui-only` passes.
- The QML smoke harness now fails on runtime warnings instead of only checking for successful instantiation.
- [ui/qml/Main.qml](../ui/qml/Main.qml) was reduced from 8,343 lines to roughly 2,100 lines.

## Completed Against The Original Follow-Up Plan

The following original plan items are complete:

- Main shell extraction:
  - [ui/qml/panes/SidebarPane.qml](../ui/qml/panes/SidebarPane.qml) and [ui/qml/panes/QueuePane.qml](../ui/qml/panes/QueuePane.qml) are adopted in [ui/qml/Main.qml](../ui/qml/Main.qml).
- Global search extraction:
  - `globalSearchDialog` now lives in [ui/qml/dialogs/GlobalSearchDialog.qml](../ui/qml/dialogs/GlobalSearchDialog.qml), backed by [ui/qml/controllers/GlobalSearchController.qml](../ui/qml/controllers/GlobalSearchController.qml).
- Album-art and iTunes artwork extraction:
  - album-art viewer shell/surface logic now lives in [ui/qml/viewers/AlbumArtViewerShell.qml](../ui/qml/viewers/AlbumArtViewerShell.qml) and [ui/qml/viewers/AlbumArtSurface.qml](../ui/qml/viewers/AlbumArtSurface.qml)
  - iTunes artwork flow now lives in [ui/qml/dialogs/ItunesArtworkDialog.qml](../ui/qml/dialogs/ItunesArtworkDialog.qml)
- Tag editor extraction:
  - the tag-editor subtree now lives in [ui/qml/dialogs/TagEditorDialog.qml](../ui/qml/dialogs/TagEditorDialog.qml) and its child dialogs
- Domain controllers:
  - queue and library interaction state are extracted to [ui/qml/controllers/QueueController.qml](../ui/qml/controllers/QueueController.qml) and [ui/qml/controllers/LibraryController.qml](../ui/qml/controllers/LibraryController.qml)
  - global-search interaction state is extracted to [ui/qml/controllers/GlobalSearchController.qml](../ui/qml/controllers/GlobalSearchController.qml)

## Remaining Cleanup Opportunities

These items are not required to call the refactor successful, but they are the sensible remaining improvements if we want [ui/qml/Main.qml](../ui/qml/Main.qml) to be closer to the ideal end state.

### 1. Extract playback state and transport smoothing into `PlaybackController.qml`

- Move the remaining playback-oriented root state out of [ui/qml/Main.qml](../ui/qml/Main.qml):
  - `displayedPositionSeconds`
  - position smoothing fields
  - mute/restore volume state
  - playback-follow logic from the root `Connections` block
- Candidate responsibilities:
  - seek-position smoothing
  - mute toggle / remembered volume
  - transport-facing computed playback state

### 2. Extract viewer orchestration into `ViewerController.qml`

- Move the remaining album-art and spectrogram viewer state out of [ui/qml/Main.qml](../ui/qml/Main.qml):
  - `albumArtViewerOpen`
  - `albumArtInfoVisible`
  - `albumArtViewerSource`
  - `albumArtViewerInfoSource`
  - `albumArtViewerFileInfo`
  - `spectrogramViewerOpen`
- Candidate responsibilities:
  - open/close/toggle/info-overlay behavior
  - whole-screen vs popup presentation sync
  - current-track artwork info refresh and iTunes workflow coordination

### 3. Finish helper deduplication by using the extracted JS modules

- [ui/qml/Main.qml](../ui/qml/Main.qml) still duplicates helpers that already have dedicated homes:
  - `mixColor` / `colorLuma` overlap with [ui/qml/logic/ColorUtils.js](../ui/qml/logic/ColorUtils.js)
  - `basenameFromPath`, `formatSeekTime`, and sample-rate formatting overlap with [ui/qml/logic/FormatUtils.js](../ui/qml/logic/FormatUtils.js)
  - URL/path/file-dialog/drop parsing overlaps with [ui/qml/logic/PathUtils.js](../ui/qml/logic/PathUtils.js)
- Finish that cleanup so feature files and the root use the same helpers instead of carrying parallel implementations.

### 4. Optionally extract library and queue action semantics further

- The current library/queue controllers cover selection and view-state behavior well.
- What still remains on the root is mostly action semantics:
  - play/append selected library rows
  - open tag editor from queue/library selections
  - queue reordering helpers
- This is lower priority than playback/viewer extraction, but it remains a valid cleanup target if [ui/qml/Main.qml](../ui/qml/Main.qml) should be reduced further.

## Recommended Order For Any Further Refactor Work

1. Add `PlaybackController.qml`.
2. Add `ViewerController.qml`.
3. Deduplicate the remaining helper functions into the existing JS modules.
4. Reassess whether the remaining library/queue action helpers are worth extracting.

## Validation Expectations

Run `./scripts/run-tests.sh --ui-only` after each follow-up slice.

After each major slice, manually verify:

- library selection, expansion, keyboard navigation, and context menu actions
- playlist selection, drag reorder, external drops, and auto-centering
- global search open/focus/navigation/activation behavior
- spectrogram popup/whole-screen presentation
- album-art popup/whole-screen presentation, zoom/pan/info overlay, and iTunes replacement flow
- tag editor open/save/cancel/reload/auto-number workflows

## Completion Criteria

The original large-structure refactor is complete. The stricter cleanup pass is complete when all of the following are true:

- [ui/qml/Main.qml](../ui/qml/Main.qml) is primarily composition, global actions/shortcuts, fallback objects, and top-level backend synchronization.
- Feature-local markup and behavior are owned by feature files under `dialogs/`, `panes/`, `viewers/`, and `controllers/`.
- The root no longer depends on deep child-id manipulation for normal library, queue, search, and viewer interactions.
- Playback and viewer state machines are no longer root-owned.
- Duplicate helper logic has been removed from the root in favor of the shared JS modules.
- The UI test entrypoint continues to pass with `./scripts/run-tests.sh --ui-only`.
