# Main QML Refactor Follow-Up Plan

## Summary

Checkpoint `73408ac` (`Refactor QML shell into reusable modules`) completed the first structural split of [ui/qml/Main.qml](../ui/qml/Main.qml). The shared palette/utilities layer, low-coupling dialogs, transport/status chrome, and spectrogram shell are now extracted and validated.

This document captures the remaining work needed to finish the original `Main.qml` decomposition so the composition root is mostly wiring and feature ownership is pushed into scoped QML files.

## Current Checkpoint

The following pieces are already extracted and registered in [ui/CMakeLists.txt](../ui/CMakeLists.txt):

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
  - partial but not yet fully adopted: [ui/qml/panes/SidebarPane.qml](../ui/qml/panes/SidebarPane.qml), [ui/qml/panes/LibraryPane.qml](../ui/qml/panes/LibraryPane.qml), [ui/qml/panes/QueuePane.qml](../ui/qml/panes/QueuePane.qml)
- Spectrogram viewer pieces:
  - [ui/qml/viewers/SpectrogramSurface.qml](../ui/qml/viewers/SpectrogramSurface.qml)
  - [ui/qml/viewers/SpectrogramViewerShell.qml](../ui/qml/viewers/SpectrogramViewerShell.qml)

Validation at this checkpoint:

- `./scripts/run-tests.sh --ui-only` passes.
- [ui/qml/Main.qml](../ui/qml/Main.qml) was reduced from 8,343 lines to 6,520 lines.

## Remaining Work

### 1. Finish main shell extraction

- Replace the remaining inline left-pane library block in [ui/qml/Main.qml](../ui/qml/Main.qml) with [ui/qml/panes/SidebarPane.qml](../ui/qml/panes/SidebarPane.qml).
- Replace the remaining inline playlist block with [ui/qml/panes/QueuePane.qml](../ui/qml/panes/QueuePane.qml).
- Keep direct object-id reach-through out of the extracted files; pass explicit callbacks, actions, and state accessors instead.
- Ensure the pane files own their local menus, delegates, and repeated column/header presentation instead of reintroducing duplication in `Main.qml`.

### 2. Extract global search

- Move the entire `globalSearchDialog` subtree out of [ui/qml/Main.qml](../ui/qml/Main.qml) into `ui/qml/dialogs/GlobalSearchDialog.qml`.
- Keep the row delegate components and context menu in the dialog file because they are feature-local, not global primitives.
- Preserve the current keyboard flow:
  - query-field navigation
  - results-list navigation
  - `Ctrl+F` refocus
  - `Tab`/`Backtab` handoff back to library
  - Enter activation and right-click context actions
- Keep the root-callable flow methods that tests or shortcuts still depend on, or migrate the tests at the same time.

### 3. Extract album-art viewer and iTunes artwork workflow

- Move album-art fullscreen/windowed shell logic into:
  - `ui/qml/viewers/AlbumArtSurface.qml`
  - `ui/qml/viewers/AlbumArtViewerShell.qml`
- Move the iTunes artwork replacement flow into:
  - `ui/qml/dialogs/ItunesArtworkDialog.qml`
- Preserve shared viewer behavior:
  - popup vs whole-screen presentation
  - close gestures and close button
  - pan/zoom, wheel zoom, double-click zoom toggle
  - info overlay and preload behavior
- Keep `Main.qml` responsible only for source-of-truth state wiring unless a clear viewer controller is introduced in the same slice.

### 4. Extract the tag editor subtree

- Move the remaining tag-editor dialogs out of [ui/qml/Main.qml](../ui/qml/Main.qml):
  - `ui/qml/dialogs/TagEditorDialog.qml`
  - `ui/qml/dialogs/AutoNumberDialog.qml`
  - `ui/qml/dialogs/TagEditorCloseConfirmDialog.qml`
  - `ui/qml/dialogs/TagEditorStatusDetailsDialog.qml`
- Keep the tag editor’s local selection model, field metadata, shortcuts, and status handling inside the tag-editor subtree instead of leaving it on the root window.
- Keep explicit inputs for:
  - `tagEditorApi`
  - palette/theme values
  - `basenameFromPath` equivalent helper
  - any open/close/save callbacks still owned by the root

### 5. Introduce domain-scoped controllers where they reduce root coupling

- The next extractions should not continue to rely on hidden cross-file access to `playlistView`, `libraryAlbumView`, `globalSearchResultsView`, or viewer hosts.
- Introduce QML `QtObject` controller files only where they materially reduce coupling:
  - `PlaybackController.qml`
  - `LibraryController.qml`
  - `QueueController.qml`
  - `GlobalSearchController.qml`
  - `ViewerController.qml`
- Move state and helper functions by behavior cluster, not by convenience. Avoid a single app-wide “god controller”.
- If a helper still reaches into child ids, move it closer to the owning extracted feature instead of centralizing it on the root.

## Recommended Order

1. Fully adopt `SidebarPane.qml` and `QueuePane.qml` so the main player shell is mostly composed from external files.
2. Extract `GlobalSearchDialog.qml`.
3. Extract `AlbumArtSurface.qml`, `AlbumArtViewerShell.qml`, and `ItunesArtworkDialog.qml`.
4. Extract the full tag-editor subtree.
5. Move the remaining root state/helpers into domain controllers where the current extraction still depends on child ids.
6. Remove dead helpers and duplicate imports from [ui/qml/Main.qml](../ui/qml/Main.qml) after each step.

## Validation Expectations

Run `./scripts/run-tests.sh --ui-only` after each phase above.

After each major slice, manually verify:

- library selection, expansion, keyboard navigation, and context menu actions
- playlist selection, drag reorder, external drops, and auto-centering
- global search open/focus/navigation/activation behavior
- spectrogram popup/whole-screen presentation
- album-art popup/whole-screen presentation, zoom/pan/info overlay, and iTunes replacement flow
- tag editor open/save/cancel/reload/auto-number workflows

## Completion Criteria

The refactor is complete when all of the following are true:

- [ui/qml/Main.qml](../ui/qml/Main.qml) is primarily composition, global actions/shortcuts, fallback objects, and top-level backend synchronization.
- Feature-local markup and behavior are owned by feature files under `dialogs/`, `panes/`, `viewers/`, and `controllers/`.
- The root no longer depends on deep child-id manipulation for normal library, queue, search, and viewer interactions.
- The UI test entrypoint continues to pass with `./scripts/run-tests.sh --ui-only`.
