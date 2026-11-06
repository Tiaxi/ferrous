// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QtQml 2.15
import Qt.labs.platform 1.1 as Platform
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "controllers" as Controllers
import "dialogs" as Dialogs
import "logic/ErrorUtils.js" as ErrorUtils
import "logic/FormatUtils.js" as FormatUtils
import "logic/PathUtils.js" as PathUtils
import "logic/WheelUtils.js" as WheelUtils
import "panes" as Panes
import "viewers" as Viewers

ApplicationWindow {
    id: root
    width: windowLayoutController.initialWidth
    height: windowLayoutController.initialHeight
    minimumWidth: root.contentMinimumWidth
    minimumHeight: root.contentMinimumHeight
    // Shown by windowLayoutController once the saved layout is applied.
    visible: false
    readonly property string appDisplayName: "Ferrous"
    title: {
        const context = playbackController.windowTitleContext()
        return context.length > 0
            ? I18n.i18nc("@title:window now playing, application name", "%1 \u2014 %2", context, root.appDisplayName)
            : root.appDisplayName
    }
    readonly property int albumArtViewerDecodeWidth: Math.max(
        1024,
        Math.ceil(Math.max(
            root.width,
            albumArtViewerShell.popupWidth,
            albumArtViewerShell.wholeScreenWidth)))
    readonly property int albumArtViewerDecodeHeight: Math.max(
        1024,
        Math.ceil(Math.max(
            root.height,
            albumArtViewerShell.popupHeight,
            albumArtViewerShell.wholeScreenHeight)))
    property string pendingFolderDialogContext: ""
    property string pendingFileDialogContext: ""
    property string pendingLibraryRootDialogMode: ""
    property string pendingLibraryRootPath: ""
    property string pendingLibraryRootName: ""
    // The backend stopped without being asked to, so no control works anymore.
    readonly property bool backendStopped: (root.uiBridge.backendFailure || "").length > 0
    readonly property real snappyScrollFlickDeceleration: 18000
    readonly property real snappyScrollMaxFlickVelocity: 1400
    // The dialogs' and the viewers' transitions; menus take the style's.
    readonly property int uiPopupTransitionMs: 0
    // Whether the window is on screen: visible, not minimized, and exposed.
    readonly property bool visualFeedsEnabled: windowPresence.shown
    readonly property bool useWholeScreenViewerMode: uiBridge.viewerFullscreenMode === 1
    readonly property var uiBridge: (typeof bridge !== "undefined" && bridge)
        ? bridge
        : bridgeFallback
    readonly property var tagEditorApi: (typeof tagEditor !== "undefined" && tagEditor)

        ? tagEditor
        : tagEditorFallback
    readonly property var displaySleepInhibitorApi:
        (typeof displaySleepInhibitor !== "undefined" && displaySleepInhibitor)
        ? displaySleepInhibitor
        : null
    readonly property var globalSearchModelApi: (uiBridge
        && uiBridge.globalSearchModel
        && uiBridge.globalSearchModel.nextSelectableIndex)
        ? uiBridge.globalSearchModel
        : globalSearchModelFallback
    readonly property var libraryTreeModel: (typeof libraryModel !== "undefined" && libraryModel)
        ? libraryModel
        : null
    readonly property var spectrogramFftChoices: [512, 1024, 2048, 4096, 8192]
    readonly property var uiPalette: uiPaletteObject
    readonly property var overlayHost: Overlay.overlay
    readonly property bool themeIsDark: uiPalette.themeIsDark
    readonly property color uiPaneColor: uiPalette.uiPaneColor
    readonly property color uiSurfaceColor: uiPalette.uiSurfaceColor
    readonly property color uiSurfaceAltColor: uiPalette.uiSurfaceAltColor
    readonly property color uiSurfaceRaisedColor: uiPalette.uiSurfaceRaisedColor
    readonly property color uiHeaderColor: uiPalette.uiHeaderColor
    readonly property color uiSectionColor: uiPalette.uiSectionColor
    readonly property color uiColumnsColor: uiPalette.uiColumnsColor
    readonly property color uiBorderColor: uiPalette.uiBorderColor
    readonly property color uiTextColor: uiPalette.uiTextColor
    readonly property color uiMutedTextColor: uiPalette.uiMutedTextColor
    readonly property color uiSelectionColor: uiPalette.uiSelectionColor
    readonly property color uiSelectionTextColor: uiPalette.uiSelectionTextColor
    readonly property color uiActiveIndicatorColor: uiPalette.uiActiveIndicatorColor

    WindowPresence {
        id: windowPresence
        window: root
    }

    Components.UiPalette {
        id: uiPaletteObject
        windowRoot: root
    }

    Controllers.GlobalSearchController {
        id: globalSearchController
        objectName: "globalSearchController"
        uiBridge: root.uiBridge
        globalSearchModelApi: root.globalSearchModelApi
        requestLibraryRevealForSearchRow: libraryController.requestRevealForSearchRow
        focusLibraryViewForNavigation: libraryController.focusViewForNavigation
        requestOpenInFileBrowserForSearchRow: libraryController.requestOpenInFileBrowserForSearchRow
    }

    Controllers.QueueController {
        id: queueController
        objectName: "queueController"
        uiBridge: root.uiBridge
        autoCenterSelection: windowLayoutController.initialFollowCurrentTrack
        tagEditorApi: root.tagEditorApi
        openTagEditorDialog: function() { tagEditorDialog.open() }
        playAt: function(index) { playbackController.playAt(index) }
    }

    Controllers.PlaybackController {
        id: playbackController
        objectName: "playbackController"
        uiBridge: root.uiBridge
        visualFeedsEnabled: root.visualFeedsEnabled
        positionStepSeconds: transportBar.positionStepSeconds
    }

    Controllers.LibraryController {
        id: libraryController
        objectName: "libraryController"
        uiBridge: root.uiBridge
        libraryModel: root.libraryTreeModel
        tryCaptureGlobalSearchPrefill: globalSearchController.tryCapturePrefill
        tagEditorApi: root.tagEditorApi
        openTagEditorDialog: function() { tagEditorDialog.open() }
    }

    Controllers.ErrorController {
        id: errorController
        objectName: "errorController"
        onHistoryRequested: recentErrorsDialog.open()
    }

    Controllers.ViewerController {
        id: viewerController
        objectName: "viewerController"
        uiBridge: root.uiBridge
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        visualizationMode: windowLayoutController.initialVisualizationMode
    }

    Controllers.WindowLayoutController {
        id: windowLayoutController
        objectName: "windowLayoutController"
        uiBridge: root.uiBridge
        window: root
        viewerController: viewerController
        queueController: queueController
    }

    readonly property int visualizationMinimumHeight: 220
    // The split handles are the seams between the panes, 1 px thick.
    readonly property int splitHandleAllowance: 1
    // The smallest window that shows every pane at its own minimum size,
    // with the level meter whether it is shown or not, so that the window's
    // minimum does not change with a setting.
    readonly property int contentMinimumWidth: Math.ceil(Math.max(
        sidebarPane.SplitView.minimumWidth + rightSplit.SplitView.minimumWidth
            + root.splitHandleAllowance,
        transportBar.implicitWidth))
    readonly property int contentMinimumHeight: Math.ceil(
        (root.menuBar ? root.menuBar.implicitHeight : 0)
        + transportBar.implicitHeight
        + queuePane.SplitView.minimumHeight
        + root.visualizationMinimumHeight
        + root.splitHandleAllowance + transportSeam.height
        + (root.footer ? root.footer.implicitHeight : 0))

    readonly property var libraryViewRef: libraryController.view

    // A fullscreen viewer is a modal window over the main one, so the recent
    // errors open in its window. Anything else is drawn in the main window,
    // above the viewer popups.
    readonly property var recentErrorsHost: {
        if (root.useWholeScreenViewerMode && viewerController.spectrogramViewerOpen
                && spectrogramViewerShell.windowHost) {
            return spectrogramViewerShell.windowHost
        }
        if (root.useWholeScreenViewerMode && viewerController.albumArtViewerOpen
                && albumArtViewerShell.windowHost) {
            return albumArtViewerShell.windowHost
        }
        return root.overlayHost
    }

    function shouldResetSpectrogramForStoppedTrackSwitch(previousPlaybackState, currentPlaybackState, stoppedTrackPath, currentTrackPath) {
        return playbackController.shouldResetSpectrogramForStoppedTrackSwitch(
            previousPlaybackState,
            currentPlaybackState,
            stoppedTrackPath,
            currentTrackPath)
    }

    function canOpenTagEditorForLibrary(rowMap) {
        return libraryController.canOpenTagEditorForRow(rowMap)
    }

    function openItunesArtworkDialog() {
        viewerController.openItunesArtworkDialog(
            itunesArtworkDialog,
            albumArtViewerShell.windowHost || root.overlayHost,
            root.overlayHost)
    }

    QtObject {
        id: globalSearchModelFallback
        function rowDataAt(index) { return ({}) }
        function isSelectableIndex(index) { return false }
        function nextSelectableIndex(startIndex, step, wrap) { return -1 }
    }

    QtObject {
        id: bridgeFallback
        property string playbackState: "Stopped"
        property var playbackClock: null
        property string durationText: "00:00"
        property real durationSeconds: 0
        property real volume: 1.0
        property int queueLength: 0
        property string queueDurationText: "00:00"
        property var queueRows: []
        property int selectedQueueIndex: -1
        property int playingQueueIndex: -1
        property string currentTrackPath: ""
        property string currentTrackCoverPath: ""
        property string currentTrackTitle: ""
        property string currentTrackArtist: ""
        property string currentTrackAlbum: ""
        property string currentTrackGenre: ""
        property var currentTrackYear: null
        property int currentTrackNumber: 0
        property var currentTrackTreeKeys: []
        property string currentTrackFormatLabel: ""
        property string currentTrackChannelLayoutText: ""
        property string currentTrackChannelLayoutIconKey: ""
        property int currentTrackChannels: 0
        property var currentTrackChannelLabels: []
        property int currentTrackSampleRateHz: 0
        property int currentTrackBitDepth: 0
        property int currentTrackCurrentBitrateKbps: 0
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
        property bool waveformEndsEarly: false
        property var mutedChannels: []
        property real dbRange: 132
        property int fftSize: 8192
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 0
        property int viewerFullscreenMode: 0
        property bool preventDisplaySleepInFullscreen: true
        property bool logScale: false
        property bool spectrogramFrequencyCap: false
        property int repeatMode: 0
        property bool shuffleEnabled: false
        property bool showFps: false
        property bool showLevelMeter: false
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property var soloedChannels: []
        property double mutedChannelsMask: 0
        property int channelButtonsVisibility: 1
        property bool systemMediaControlsEnabled: true
        property bool libraryWatchEnabled: true
        property var queueColumnWidths: ({})
        property bool lastFmScrobblingEnabled: false
        property bool lastFmBuildConfigured: false
        property string lastFmUsername: ""
        property int lastFmAuthState: 0
        property int lastFmPendingScrobbleCount: 0
        property string lastFmStatusText: ""
        property bool lastFmShowScrobbleProgress: true
        property bool lastFmScrobblingPaused: false
        property int lastFmScrobbleState: 0
        property int lastFmScrobbleIneligibility: 0
        property int lastFmScrobbleListenedMs: 0
        property int lastFmScrobbleThresholdMs: 0
        property string lastFmScrobbleMessage: ""
        property int sampleRateHz: 48000
        property var libraryAlbums: []
        property var libraryTreeBinary: ""
        property int libraryVersion: 0
        property bool libraryScanInProgress: false
        property int libraryRootCount: 0
        property int libraryTrackCount: 0
        property int libraryArtistCount: 0
        property int libraryAlbumCount: 0
        property var libraryExpandedKeys: []
        property bool libraryViewStateAvailable: false
        property string libraryViewSelectionKey: ""
        property string libraryViewAnchorKey: ""
        property real libraryViewAnchorOffset: 0
        property var libraryRoots: []
        property var libraryRootEntries: []
        property int librarySortMode: 0
        property string fileBrowserName: I18n.i18nc("@label generic application name", "File Manager")
        property int libraryScanRootsCompleted: 0
        property int libraryScanRootsTotal: 0
        property int libraryScanDiscovered: 0
        property int libraryScanProcessed: 0
        property real libraryScanFilesPerSecond: 0
        property real libraryScanEtaSeconds: -1
        property var globalSearchArtistResults: []
        property var globalSearchAlbumResults: []
        property var globalSearchTrackResults: []
        property int globalSearchArtistCount: 0
        property int globalSearchAlbumCount: 0
        property int globalSearchTrackCount: 0
        property int globalSearchSeq: 0
        property var globalSearchModel: globalSearchModelFallback
        property var itunesArtworkResults: []
        property var itunesArtworkTarget: ({})
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property bool connected: false
        property string backendFailure: ""
        // What saveStateBeforeQuit fails with; empty for a save that succeeds.
        property string stateSaveError: ""
        // Leaves saveStateBeforeQuit unanswered, so tests can answer it
        // with stateSaveFinished in the order they choose.
        property bool holdStateSaveAnswers: false
        property int lastStateSaveRequestId: 0
        signal playbackChanged()
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
        signal libraryChanged()
        signal libraryScanChanged()
        signal analysisChanged()
        signal libraryTreeFrameReceived(int version, var treeBytes)
        signal libraryRevealRequested(var expandKeys, string selectionKey)
        signal globalSearchResultsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal diagnosticsChanged()
        signal bridgeError(string message, int category, int severity, string subject)
        signal precomputedSpectrogramChunkReady(var data, int bins, int channelCount, int columns,
            int startIndex, int totalEstimate, int sampleRate, int hopSize,
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken, var generation)
        signal precomputedSpectrogramChannelsReady(int channelCount, bool bufferReset)
        function play() {}
        function pause() {}
        function stop() {}
        function next() {}
        function previous() {}
        // Where the latest seek went, for tests; -1 before any.
        property real lastSeekSeconds: -1
        function seek(seconds) { lastSeekSeconds = seconds }
        function setVolume(value) { volume = value }
        function setFftSize(value) {}
        function setSpectrogramViewMode(value) {}
        function setSpectrogramDisplayMode(value) {}
        function setViewerFullscreenMode(value) {}
        function setPreventDisplaySleepInFullscreen(value) {}
        function setDbRange(value) {}
        function setLogScale(value) {}
        function setSpectrogramFrequencyCap(value) {}
        function setRepeatMode(mode) { repeatMode = mode }
        function setShuffleEnabled(value) { shuffleEnabled = value }
        function setShowFps(value) {}
        function setShowLevelMeter(value) { showLevelMeter = value }
        function setShowSpectrogramCrosshair(value) {}
        function setShowSpectrogramScale(value) { showSpectrogramScale = value }
        function setSpectrogramZoomEnabled(value) {}
        function setSpectrogramZoomLevel(level) {}
        function setSpectrogramWidgetWidth(width) {}
        function registerSpectrogramItem(item, channelIndex) {}
        function unregisterSpectrogramItem(item) {}
        function setSystemMediaControlsEnabled(value) {}
        function setLibraryWatchEnabled(value) {}
        function setLastFmScrobblingEnabled(value) {}
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
        function setLastFmScrobblingPaused(value) { lastFmScrobblingPaused = value }
        function setLastFmShowScrobbleProgress(value) { lastFmShowScrobbleProgress = value }
        function playAt(index) {}
        function selectQueueIndex(index) {}
        // The queue edits asked for, for tests.
        property var queueCommands: []
        function removeAt(index) { queueCommands = queueCommands.concat(["remove " + index]) }
        function removeIndices(indices) {
            queueCommands = queueCommands.concat(["remove " + indices.join(",")])
        }
        function moveQueue(from, to) {}
        function clearQueue() { queueCommands = queueCommands.concat(["clear"]) }
        // The queue change the user can undo; see BridgeClient.
        property double queueUndoId: 0
        property int queueUndoKind: 0
        property int queueUndoTrackCount: 0
        signal queueUndoRestored(var rows)
        function undoQueueChange() { queueCommands = queueCommands.concat(["undo " + queueUndoId]) }
        function replaceAlbumAt(index) {}
        function appendAlbumAt(index) {}
        function playTrack(path) {}
        function appendTrack(path) {}
        function replaceAlbumByKey(artist, album) {}
        function appendAlbumByKey(artist, album) {}
        function replaceArtistByName(artist) {}
        function appendArtistByName(artist) {}
        function replaceAllLibraryTracks() {}
        function appendAllLibraryTracks() {}
        function replaceWithPaths(paths) {}
        function appendPaths(paths) {}
        function insertPaths(paths, row) {}
        function playLibraryItems(items) {}
        function appendLibraryItems(items) {}
        function insertLibraryItems(items, row) {}
        function moveQueueRows(rows, before) {}
        function shiftQueueRows(rows, direction) {}
        function libraryAlbumCoverAt(index) { return "" }
        function libraryThumbnailSource(path) { return "" }
        function queueTrackNumberAt(index) { return null }
        function queuePathAt(index) { return "" }
        function addLibraryRoot(path, name) {}
        function setLibraryRootName(path, name) {}
        function removeLibraryRoot(path) {}
        function rescanLibraryRoot(path) {}
        function rescanAllLibraryRoots() {}
        function setLibraryNodeExpanded(key, expanded) {}
        function setLibraryViewState(selectionKey, anchorKey, anchorOffset) {}
        function showTrackInLibrary(path) {}
        function setLibrarySortMode(mode) {}
        function setGlobalSearchQuery(query) {}
        function searchCurrentTrackArtworkSuggestions() {}
        function clearItunesArtworkSuggestions() {}
        function itunesArtworkResultAt(index) { return ({}) }
        function prepareItunesArtworkSuggestion(index) {}
        function applyItunesArtworkSuggestion(index) {}
        function openInFileBrowser(path) {}
        function openContainingFolder(path) {}
        function requestImageFileDetails(path) {}
        function cachedImageFileDetails(path) { return ({}) }
        function imageFileDetails(path) { return ({}) }
        function scanRoot(path) {}
        function scanDefaultMusicRoot() {}
        function requestSnapshot() {}
        function hasSavedWindowLayout() { return true }
        function savedWindowLayout() { return ({}) }
        function setWindowLayout(layout) {}
        function setQueueColumnWidth(column, width) {
            const widths = Object.assign({}, bridgeFallback.queueColumnWidths)
            if (width > 0) {
                widths[column] = width
            } else {
                delete widths[column]
            }
            bridgeFallback.queueColumnWidths = widths
        }
        function shutdown() {}
        signal stateSaveFinished(int requestId, bool saved, string message)
        function saveStateBeforeQuit() {
            const requestId = ++bridgeFallback.lastStateSaveRequestId
            const error = bridgeFallback.stateSaveError
            if (!bridgeFallback.holdStateSaveAnswers) {
                Qt.callLater(function() {
                    bridgeFallback.stateSaveFinished(requestId, error.length === 0, error)
                })
            }
            return requestId
        }
        function clearDiagnostics() {}
        function reloadDiagnosticsFromDisk() {}
        function toggleChannelMute(channelIndex) {}
        function soloChannel(channelIndex) {}
        function isChannelSilent(channelIndex) { return false }
        function setChannelButtonsVisibility(value) {}
    }

    QtObject {
        id: tagEditorFallback
        property bool open: false
        property bool loading: false
        property bool saving: false
        property bool dirty: false
        property string statusText: ""
        property string statusDetails: ""
        property bool statusIsError: false
        property int selectedCount: 0
        property var operationFiles: []
        property var tableModel: []
        signal statusChanged()
        signal renamePreviewReady(var rows, string error)
        function openSelection(selections) { return false }
        function openForPaths(paths) { return false }
        function close() {}
        function reload() {}
        function save() { return false }
        function renameSelectedFiles() { return false }
        function previewRenameSelectedFiles() { return false }
        function renamePreviewedFiles() { return false }
        function setSelectedRows(rows) {}
        function selectedRows() { return [] }
        function selectRange(first, last) {}
        function toggleRowSelected(row) {}
        function selectAllRows() {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
        function bulkValueMixed(field) { return false }
        function extraValuesNote(field) { return "" }
        function applyBulkField(field, value) {}
        function applyBulkFieldToRows(rows, field, value) {}
        function setCell(row, field, value) {}
        function applyEnglishTitleCase(field) {}
        function applyFinnishCapitalize(field) {}
        function applyGenreCapitalize() {}
        function autoNumber(startingTrack, startingDisc, writeDiscNumbers, writeTotals, resetOnFolder, resetOnDiscChange) {}
    }

    // The queue's number column: 26 px at the default font, as wide as a
    // row is tall, or as the widest number.
    readonly property int playlistOrderColumnWidth: Math.ceil(Math.max(
        uiPalette.rowHeight, playlistOrderTextMetrics.advanceWidth))
    // The queue's state glyph column: 28 px at the default font.
    readonly property int playlistIndicatorColumnWidth: Math.round(uiPalette.rowHeight * 28 / 26)

    TextMetrics {
        id: playlistOrderTextMetrics
        font.family: uiPalette.baseFont.family
        font.pointSize: uiPalette.basePointSize
        font.features: uiPalette.tabularFigures
        text: FormatUtils.playlistOrderText(Math.max(0, uiBridge.queueLength - 1))
    }

    // Scroll left over from earlier wheel events that the view has not
    // shown yet: pixel-aligned views round each small touchpad or
    // high-resolution wheel step away otherwise.
    property var wheelScrollRemainder: ({ view: null, pixels: 0 })

    function stepScrollView(view, wheel, rowHeight, rowsPerStep) {
        if (!view || !wheel) {
            return
        }
        const rowPx = Math.max(8, rowHeight || 24)
        const notchPx = rowPx * Math.max(1, rowsPerStep || 1)
        const deltaPx = WheelUtils.verticalScrollPixels(wheel, notchPx)
        if (deltaPx === 0) {
            // Horizontal scrolling is for someone else.
            wheel.accepted = false
            return
        }
        wheel.accepted = true
        const minY = view.originY || 0
        const maxY = minY + Math.max(0, view.contentHeight - view.height)
        const remainder = root.wheelScrollRemainder
        const carried = remainder.view === view ? remainder.pixels : 0
        const targetY = Math.max(minY, Math.min(maxY, view.contentY + carried - deltaPx))
        view.contentY = targetY
        root.wheelScrollRemainder = { view: view, pixels: targetY - view.contentY }
    }

    function openDiagnostics() {
        diagnosticsDialog.open()
    }

    function submitExternalImport(paths, replaceQueue) {
        if (!paths || paths.length === 0) {
            return false
        }
        if (replaceQueue) {
            uiBridge.replaceWithPaths(paths)
        } else {
            uiBridge.appendPaths(paths)
        }
        return true
    }

    function openExternalFiles() {
        pendingFileDialogContext = "open"
        externalFileDialog.open()
    }

    function addExternalFiles() {
        pendingFileDialogContext = "append"
        externalFileDialog.open()
    }

    function addExternalFolder() {
        pendingFolderDialogContext = "append-external-folder"
        scanFolderDialog.open()
    }

    // A folder chosen in the folder dialog: added to the queue, or, named
    // first, to the library.
    function takeChosenFolder(localPath) {
        const context = pendingFolderDialogContext
        pendingFolderDialogContext = ""
        if (localPath.length === 0) {
            return
        }
        if (context === "append-external-folder") {
            root.submitExternalImport([localPath], false)
        } else {
            root.openLibraryRootNameDialog("add", localPath, "")
        }
    }

    function promptAddLibraryRoot(contextValue) {
        pendingFolderDialogContext = contextValue || ""
        scanFolderDialog.open()
    }

    function openLibraryRootNameDialog(modeValue, pathValue, nameValue) {
        pendingLibraryRootDialogMode = modeValue || ""
        pendingLibraryRootPath = pathValue || ""
        pendingLibraryRootName = nameValue || ""
        libraryRootNameDialog.open()
    }

    function resetLibraryRootNameDialog() {
        pendingLibraryRootDialogMode = ""
        pendingLibraryRootPath = ""
        pendingLibraryRootName = ""
    }

    // The window's actions. Menus show them; their shortcuts work wherever
    // the main window has focus. Titles are Title Case, and an item that
    // opens a dialog ends in "…".

    // File
    Action {
        id: openFilesAction
        objectName: "openFilesAction"
        text: I18n.i18nc("@action:inmenu", "&Open Files…")
        icon.name: "document-open"
        shortcut: StandardKey.Open
        enabled: !root.viewerOpen
        onTriggered: root.openExternalFiles()
    }
    Action {
        id: addFilesAction
        objectName: "addFilesAction"
        text: I18n.i18nc("@action:inmenu", "&Add Files to Queue…")
        icon.name: "media-playlist-append"
        onTriggered: root.addExternalFiles()
    }
    // The folder dialog picks one folder.
    Action {
        id: addFolderAction
        objectName: "addFolderAction"
        text: I18n.i18nc("@action:inmenu", "Add &Folder to Queue…")
        icon.name: "folder-add"
        onTriggered: root.addExternalFolder()
    }
    // As Configure Ferrous › Library adds one.
    Action {
        id: addLibraryFolderAction
        objectName: "addLibraryFolderAction"
        text: I18n.i18nc("@action:inmenu", "Add Library Fol&der…")
        icon.name: "folder-music"
        onTriggered: root.promptAddLibraryRoot("add-library-root")
    }
    Action {
        id: playAllLibraryTracksAction
        objectName: "playAllLibraryTracksAction"
        text: I18n.i18nc("@action:inmenu", "&Play Whole Library")
        icon.name: "media-playback-start"
        enabled: libraryController.canPlayAllTracks()
        onTriggered: libraryController.playAllTracks()
    }
    Action {
        id: appendAllLibraryTracksAction
        objectName: "appendAllLibraryTracksAction"
        text: I18n.i18nc("@action:inmenu", "Queue &Whole Library")
        icon.name: "media-playlist-append"
        enabled: libraryController.canPlayAllTracks()
        onTriggered: libraryController.appendAllTracks()
    }
    Action {
        id: quitAction
        objectName: "quitAction"
        text: I18n.i18nc("@action:inmenu", "&Quit")
        icon.name: "application-exit"
        shortcut: StandardKey.Quit
        // Closing the window, not quitting, runs onClosing, which saves the
        // settings and the queue and stops the backend.
        onTriggered: root.close()
    }

    // Edit
    Action {
        id: undoQueueChangeAction
        objectName: "undoQueueChangeAction"
        text: {
            switch (uiBridge.queueUndoKind) {
            case 1:
                return I18n.i18nc("@action:inmenu", "&Undo Clear Queue")
            case 2:
                return I18n.i18nc("@action:inmenu", "&Undo Remove from Queue")
            case 3:
                return I18n.i18nc("@action:inmenu", "&Undo Replace Queue")
            case 4:
                return I18n.i18nc("@action:inmenu", "&Undo Move in Queue")
            default:
                return I18n.i18nc("@action:inmenu", "&Undo")
            }
        }
        icon.name: "edit-undo"
        // Text fields keep Ctrl+Z for their own text.
        shortcut: StandardKey.Undo
        enabled: uiBridge.queueUndoId > 0 && !root.viewerOpen
        onTriggered: uiBridge.undoQueueChange()
    }
    // Opens search. Text fields keep Ctrl+F; the search field takes it to
    // edit its query.
    Action {
        id: globalSearchAction
        objectName: "findAction"
        text: I18n.i18nc("@action:inmenu", "&Find…")
        icon.name: "edit-find"
        shortcut: StandardKey.Find
        enabled: !root.viewerOpen
        onTriggered: globalSearchController.openDialog()
    }
    // The one menu item that clears the queue; Undo brings it back.
    Action {
        id: clearQueueAction
        objectName: "clearQueueAction"
        text: I18n.i18nc("@action:inmenu", "&Clear Queue")
        icon.name: "edit-clear-list"
        enabled: uiBridge.queueLength > 0 && !root.viewerOpen
        onTriggered: uiBridge.clearQueue()
    }
    // Queue edits with no menu item of their own in the menu bar: the
    // queue's context menu offers them for its rows.
    Action {
        id: moveTrackUpAction
        objectName: "moveTrackUpAction"
        text: I18n.i18nc("@action:inmenu", "Move Up")
        shortcut: "Ctrl+Shift+Up"
        enabled: !root.viewerOpen
        onTriggered: queueController.moveSelected(-1)
    }
    Action {
        id: moveTrackDownAction
        objectName: "moveTrackDownAction"
        text: I18n.i18nc("@action:inmenu", "Move Down")
        shortcut: "Ctrl+Shift+Down"
        enabled: !root.viewerOpen
        onTriggered: queueController.moveSelected(1)
    }
    Action {
        id: selectPreviousTrackAction
        objectName: "selectPreviousTrackAction"
        text: I18n.i18nc("@action:inmenu", "Select Previous Track")
        shortcut: "Ctrl+Up"
        enabled: uiBridge.queueLength > 0 && !root.viewerOpen
        onTriggered: queueController.selectRelative(-1)
    }
    Action {
        id: selectNextTrackAction
        objectName: "selectNextTrackAction"
        text: I18n.i18nc("@action:inmenu", "Select Next Track")
        shortcut: "Ctrl+Down"
        enabled: uiBridge.queueLength > 0 && !root.viewerOpen
        onTriggered: queueController.selectRelative(1)
    }

    // View
    Action {
        id: visualizationViewerAction
        objectName: "visualizationViewerAction"
        text: I18n.i18nc("@action:inmenu", "Open &Visualization Viewer")
        icon.name: "view-media-visualization"
        shortcut: "F11"
        // Also closes it: the viewer repeats the shortcut.
        onTriggered: viewerController.toggleSpectrogramViewer()
    }
    Action {
        id: albumArtViewerAction
        objectName: "albumArtViewerAction"
        text: I18n.i18nc("@action:inmenu", "Open &Album Art Viewer")
        icon.name: "view-preview"
        shortcut: "Shift+F11"
        enabled: viewerController.albumArtViewerOpen
            || (uiBridge.currentTrackCoverPath || "").length > 0
        onTriggered: viewerController.toggleAlbumArtViewer()
    }
    // The plot's two modes, as its mode switch offers them.
    ActionGroup {
        id: visualizationModeGroup
        exclusive: true
    }
    Action {
        id: spectrogramModeAction
        objectName: "spectrogramModeAction"
        text: I18n.i18nc("@action:inmenu visualization mode", "&Spectrogram")
        shortcut: "Ctrl+1"
        checkable: true
        checked: viewerController.visualizationMode === 0
        ActionGroup.group: visualizationModeGroup
        onTriggered: viewerController.setVisualizationMode(0)
    }
    Action {
        id: waveformModeAction
        objectName: "waveformModeAction"
        text: I18n.i18nc("@action:inmenu visualization mode", "&Waveform")
        shortcut: "Ctrl+2"
        checkable: true
        checked: viewerController.visualizationMode === 1
        ActionGroup.group: visualizationModeGroup
        onTriggered: viewerController.setVisualizationMode(1)
    }
    Action {
        id: showLevelMeterAction
        objectName: "showLevelMeterAction"
        text: I18n.i18nc("@action:inmenu", "Show &Level Meter")
        checkable: true
        checked: uiBridge.showLevelMeter
        onTriggered: uiBridge.setShowLevelMeter(checked)
    }
    // The scale is the spectrogram's only: the waveform has none.
    Action {
        id: showScaleAction
        objectName: "showScaleAction"
        text: I18n.i18nc("@action:inmenu", "Show Frequency and &Time Scale")
        checkable: true
        checked: uiBridge.showSpectrogramScale
        enabled: viewerController.visualizationMode === 0
        onTriggered: uiBridge.setShowSpectrogramScale(checked)
    }
    Action {
        id: autoCenterSelectionAction
        objectName: "followCurrentTrackAction"
        text: I18n.i18nc("@action:inmenu", "&Follow Current Track in Queue")
        checkable: true
        checked: queueController.autoCenterSelection
        onTriggered: queueController.autoCenterSelection = checked
    }
    Action {
        id: resetSpectrogramAction
        objectName: "resetVisualizationAction"
        text: I18n.i18nc("@action:inmenu", "&Reset Visualization View")
        icon.name: "zoom-original"
        onTriggered: {
            if (viewerController.visualizationMode === 0) {
                spectrogramSurface.resetForCurrentMode()
            } else {
                waveformSurface.waveformItem.resetZoom()
            }
        }
    }

    // Playback
    Action {
        id: playPauseAction
        objectName: "playPauseAction"
        // What it does now: Pause while playing, Play otherwise.
        text: playbackController.isPlaying
            ? I18n.i18nc("@action:inmenu", "&Pause")
            : I18n.i18nc("@action:inmenu", "&Play")
        icon.name: playbackController.isPlaying ? "media-playback-pause" : "media-playback-start"
        // Text fields take Space for typing before shortcuts see it.
        shortcut: "Space"
        onTriggered: playbackController.togglePlayPause()
    }
    Action {
        id: playAction
        text: I18n.i18nc("@action:inmenu", "Play")
        icon.name: "media-playback-start"
        onTriggered: uiBridge.play()
    }
    Action {
        id: pauseAction
        text: I18n.i18nc("@action:inmenu", "Pause")
        icon.name: "media-playback-pause"
        onTriggered: uiBridge.pause()
    }
    Action {
        id: stopAction
        objectName: "stopAction"
        text: I18n.i18nc("@action:inmenu", "&Stop")
        icon.name: "media-playback-stop"
        // Not Ctrl+Space, which switches input methods in Fcitx and IBus,
        // nor Shift+Space, which toggles full-width input in Fcitx.
        shortcut: "Ctrl+Shift+Space"
        onTriggered: uiBridge.stop()
    }
    Action {
        id: previousAction
        objectName: "previousAction"
        text: I18n.i18nc("@action:inmenu", "P&revious")
        icon.name: "media-skip-backward"
        shortcut: "Ctrl+Left"
        onTriggered: uiBridge.previous()
    }
    Action {
        id: nextAction
        objectName: "nextAction"
        text: I18n.i18nc("@action:inmenu", "&Next")
        icon.name: "media-skip-forward"
        shortcut: "Ctrl+Right"
        onTriggered: uiBridge.next()
    }
    Action {
        id: seekBackwardAction
        objectName: "seekBackwardAction"
        text: I18n.i18nc("@action:inmenu", "Seek &Backward")
        icon.name: "media-seek-backward"
        shortcut: "Shift+Left"
        enabled: playbackController.canSeek
        onTriggered: playbackController.seekBy(-root.keyboardSeekStepSeconds)
    }
    Action {
        id: seekForwardAction
        objectName: "seekForwardAction"
        text: I18n.i18nc("@action:inmenu", "Seek &Forward")
        icon.name: "media-seek-forward"
        shortcut: "Shift+Right"
        enabled: playbackController.canSeek
        onTriggered: playbackController.seekBy(root.keyboardSeekStepSeconds)
    }
    Action {
        id: shuffleAction
        objectName: "shuffleAction"
        text: I18n.i18nc("@action:inmenu", "S&huffle")
        checkable: true
        checked: uiBridge.shuffleEnabled
        onTriggered: uiBridge.setShuffleEnabled(checked)
    }
    // Repeat: one of three, under the "Repeat" heading. The wording is
    // the status bar's: "Repeat One Track", "Repeat Whole Queue".
    ActionGroup {
        id: repeatModeGroup
        exclusive: true
    }
    Action {
        id: repeatOffAction
        objectName: "repeatOffAction"
        text: I18n.i18nc("@action:inmenu repeat mode", "&Off")
        checkable: true
        checked: uiBridge.repeatMode === 0
        ActionGroup.group: repeatModeGroup
        onTriggered: uiBridge.setRepeatMode(0)
    }
    Action {
        id: repeatOneAction
        objectName: "repeatOneAction"
        text: I18n.i18nc("@action:inmenu repeat mode", "One &Track")
        checkable: true
        checked: uiBridge.repeatMode === 1
        ActionGroup.group: repeatModeGroup
        onTriggered: uiBridge.setRepeatMode(1)
    }
    Action {
        id: repeatAllAction
        objectName: "repeatAllAction"
        text: I18n.i18nc("@action:inmenu repeat mode", "&Whole Queue")
        checkable: true
        checked: uiBridge.repeatMode === 2
        ActionGroup.group: repeatModeGroup
        onTriggered: uiBridge.setRepeatMode(2)
    }
    Action {
        id: volumeUpAction
        objectName: "volumeUpAction"
        text: I18n.i18nc("@action:inmenu", "Volume &Up")
        icon.name: "audio-volume-high"
        shortcut: "Alt+Up"
        onTriggered: playbackController.changeVolumeBy(root.keyboardVolumeStep)
    }
    Action {
        id: volumeDownAction
        objectName: "volumeDownAction"
        text: I18n.i18nc("@action:inmenu", "Volume &Down")
        icon.name: "audio-volume-low"
        shortcut: "Alt+Down"
        onTriggered: playbackController.changeVolumeBy(-root.keyboardVolumeStep)
    }
    Action {
        id: muteAction
        objectName: "muteAction"
        text: I18n.i18nc("@action:inmenu", "&Mute")
        // Not Ctrl+M, which KDE applications use to show the menu bar.
        shortcut: "Ctrl+Shift+M"
        checkable: true
        checked: playbackController.volumeMuted
        onTriggered: playbackController.toggleMutedVolume()
    }

    // Settings
    Action {
        id: preferencesAction
        objectName: "configureAction"
        text: I18n.i18nc("@action:inmenu", "&Configure Ferrous…")
        icon.name: "configure"
        shortcut: StandardKey.Preferences
        enabled: !root.viewerOpen
        onTriggered: preferencesDialog.open()
    }

    // Help
    Action {
        id: reportBugAction
        objectName: "reportBugAction"
        text: I18n.i18nc("@action:inmenu", "&Report Bug…")
        icon.name: "tools-report-bug"
        onTriggered: Qt.openUrlExternally(root.bugReportUrl)
    }
    Action {
        id: diagnosticsAction
        objectName: "diagnosticsAction"
        text: I18n.i18nc("@action:inmenu", "&Diagnostics…")
        icon.name: "view-list-details"
        onTriggered: root.openDiagnostics()
    }
    Action {
        id: aboutAction
        objectName: "aboutAction"
        text: I18n.i18nc("@action:inmenu", "&About Ferrous")
        // The app icon's own 16 px drawing, which is built in.
        icon.source: "qrc:/icons/hicolor/16x16/apps/ferrous.svg"
        onTriggered: aboutDialog.open()
    }
    Action {
        id: aboutKdeAction
        objectName: "aboutKdeAction"
        text: I18n.i18nc("@action:inmenu", "About &KDE")
        icon.name: "kde"
        onTriggered: aboutKdeDialog.open()
    }

    // The artwork of the current track, from the cover's menus.
    Action {
        id: replaceFromItunesAction
        text: I18n.i18nc("@action:inmenu", "Replace from iTunes…")
        enabled: viewerController.currentTrackItunesArtworkDisabledReason().length === 0
        onTriggered: root.openItunesArtworkDialog()
    }

    readonly property string bugReportUrl: "https://github.com/Tiaxi/ferrous/issues/new"
    readonly property real keyboardSeekStepSeconds: 5
    readonly property real keyboardVolumeStep: 0.05
    readonly property bool viewerOpen: viewerController.spectrogramViewerOpen
        || viewerController.albumArtViewerOpen
    // What works in the viewers too. Viewer popups are modal, which blocks
    // the main window's shortcuts, so they repeat these.
    readonly property var viewerActions: [
        playPauseAction, stopAction, previousAction, nextAction,
        seekBackwardAction, seekForwardAction,
        volumeUpAction, volumeDownAction, muteAction,
        visualizationViewerAction, albumArtViewerAction,
        spectrogramModeAction, waveformModeAction, quitAction
    ]
    // The menus take the style's size, icons and motion.
    menuBar: MenuBar {
        objectName: "mainMenuBar"

        Components.AppMenu {
            objectName: "fileMenu"
            title: I18n.i18nc("@title:menu", "&File")
            MenuItem { action: openFilesAction }
            MenuItem { action: addFilesAction }
            MenuItem { action: addFolderAction }
            MenuSeparator {}
            MenuItem { action: addLibraryFolderAction }
            MenuItem { action: playAllLibraryTracksAction }
            MenuItem { action: appendAllLibraryTracksAction }
            MenuSeparator {}
            MenuItem { action: quitAction }
        }
        Components.AppMenu {
            objectName: "editMenu"
            title: I18n.i18nc("@title:menu", "&Edit")
            MenuItem { action: undoQueueChangeAction }
            MenuSeparator {}
            MenuItem { action: globalSearchAction }
            MenuSeparator {}
            MenuItem { action: clearQueueAction }
        }
        Components.AppMenu {
            objectName: "viewMenu"
            title: I18n.i18nc("@title:menu", "&View")
            MenuItem { action: visualizationViewerAction }
            MenuItem { action: albumArtViewerAction }
            MenuSeparator {}
            Components.MenuHeading {
                objectName: "visualizationHeading"
                text: I18n.i18nc("@title:menu visualization modes", "Visualization")
            }
            MenuItem {
                action: spectrogramModeAction
                Accessible.description: I18n.i18nc("@info", "Visualization")
            }
            MenuItem {
                action: waveformModeAction
                Accessible.description: I18n.i18nc("@info", "Visualization")
            }
            MenuSeparator {}
            MenuItem { action: showLevelMeterAction }
            MenuItem { action: showScaleAction }
            MenuSeparator {}
            MenuItem { action: autoCenterSelectionAction }
            MenuItem { action: resetSpectrogramAction }
        }
        Components.AppMenu {
            objectName: "playbackMenu"
            title: I18n.i18nc("@title:menu", "&Playback")
            MenuItem { action: playPauseAction }
            MenuItem { action: stopAction }
            MenuItem { action: previousAction }
            MenuItem { action: nextAction }
            MenuSeparator {}
            MenuItem { action: seekBackwardAction }
            MenuItem { action: seekForwardAction }
            MenuSeparator {}
            MenuItem { action: shuffleAction }
            Components.MenuHeading {
                objectName: "repeatHeading"
                text: I18n.i18nc("@title:menu repeat modes", "Repeat")
            }
            MenuItem {
                action: repeatOffAction
                Accessible.description: I18n.i18nc("@info", "Repeat")
            }
            MenuItem {
                action: repeatOneAction
                Accessible.description: I18n.i18nc("@info", "Repeat")
            }
            MenuItem {
                action: repeatAllAction
                Accessible.description: I18n.i18nc("@info", "Repeat")
            }
            MenuSeparator {}
            MenuItem { action: volumeUpAction }
            MenuItem { action: volumeDownAction }
            MenuItem { action: muteAction }
        }
        Components.AppMenu {
            objectName: "settingsMenu"
            title: I18n.i18nc("@title:menu", "&Settings")
            MenuItem { action: preferencesAction }
        }
        Components.AppMenu {
            objectName: "helpMenu"
            title: I18n.i18nc("@title:menu", "&Help")
            MenuItem { action: reportBugAction }
            MenuItem { action: diagnosticsAction }
            MenuSeparator {}
            MenuItem { action: aboutAction }
            MenuItem { action: aboutKdeAction }
        }
    }

    Dialogs.AboutKdeDialog {
        id: aboutKdeDialog
        uiPalette: root.uiPalette
    }

    Dialogs.AboutDialog {
        id: aboutDialog
        popupTransitionMs: root.uiPopupTransitionMs
    }

    Dialogs.PreferencesDialog {
        id: preferencesDialog
        objectName: "preferencesDialog"
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        popupTransitionMs: root.uiPopupTransitionMs
        spectrogramFftChoices: root.spectrogramFftChoices
        promptAddLibraryRoot: root.promptAddLibraryRoot
        openLibraryRootNameDialog: root.openLibraryRootNameDialog
        stepScrollView: root.stepScrollView
        snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
        snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
    }

    Dialogs.LibraryRootNameDialog {
        id: libraryRootNameDialog
        objectName: "libraryRootNameDialog"
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        popupTransitionMs: root.uiPopupTransitionMs
        dialogMode: root.pendingLibraryRootDialogMode
        pathValue: root.pendingLibraryRootPath
        nameValue: root.pendingLibraryRootName
        onDismissed: root.resetLibraryRootNameDialog()
    }

    Dialogs.GlobalSearchDialog {
        controller: globalSearchController
        uiPalette: root.uiPalette
        windowRoot: root
        anchorItem: transportBar
        snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
        snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
    }

    Dialogs.DiagnosticsDialog {
        id: diagnosticsDialog
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        popupTransitionMs: root.uiPopupTransitionMs
    }

    Dialogs.RecentErrorsDialog {
        id: recentErrorsDialog
        objectName: "recentErrorsDialog"
        parent: root.recentErrorsHost
        errorController: errorController
        uiPalette: root.uiPalette
        windowRoot: root
        popupTransitionMs: root.uiPopupTransitionMs
    }

    Platform.FileDialog {
        id: externalFileDialog
        title: pendingFileDialogContext === "open"
            ? I18n.i18nc("@title:window", "Open Files")
            : I18n.i18nc("@title:window", "Add Files")
        fileMode: Platform.FileDialog.OpenFiles
        nameFilters: [
            I18n.i18nc("@item:inlistbox file type filter; keep the patterns in parentheses",
                "Audio and Playlist Files (%1)", "*.mp3 *.flac *.m4a *.aac *.ogg *.opus *.wav *.ac3 *.dts *.m3u *.m3u8"),
            I18n.i18nc("@item:inlistbox file type filter; keep the patterns in parentheses",
                "Audio Files (%1)", "*.mp3 *.flac *.m4a *.aac *.ogg *.opus *.wav *.ac3 *.dts"),
            I18n.i18nc("@item:inlistbox file type filter; keep the patterns in parentheses",
                "Playlist Files (%1)", "*.m3u *.m3u8"),
            I18n.i18nc("@item:inlistbox file type filter; keep the patterns in parentheses",
                "All Files (%1)", "*")
        ]
        onAccepted: {
            const localPaths = PathUtils.fileDialogPaths(externalFileDialog)
            root.submitExternalImport(localPaths, pendingFileDialogContext === "open")
            pendingFileDialogContext = ""
        }
        onRejected: pendingFileDialogContext = ""
    }

    Platform.FolderDialog {
        id: scanFolderDialog
        objectName: "scanFolderDialog"
        title: pendingFolderDialogContext === "append-external-folder"
            ? I18n.i18nc("@title:window", "Add Folder to Queue")
            : I18n.i18nc("@title:window", "Add Library Folder")
        onAccepted: root.takeChosenFolder(PathUtils.folderDialogPath(scanFolderDialog))
        onRejected: pendingFolderDialogContext = ""
    }

    // A split handle: a 1 px seam, grabbable a few pixels either side.
    Component {
        id: seamHandle

        Rectangle {
            id: seam
            objectName: "seamHandle"
            // How far across the pointer finds it.
            readonly property int grab: Kirigami.Units.smallSpacing * 2 - 1
            implicitWidth: 1
            implicitHeight: 1
            color: seam.SplitHandle.pressed ? root.uiPalette.focusRing : root.uiPalette.seam
            containmentMask: Item {
                x: seam.width > seam.height ? 0 : (seam.width - width) / 2
                y: seam.width > seam.height ? (seam.height - height) / 2 : 0
                width: seam.width > seam.height ? seam.width : seam.grab
                height: seam.width > seam.height ? seam.grab : seam.height
            }
        }
    }

    footer: Panes.StatusBar {
        objectName: "statusBar"
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        themeIsDark: root.themeIsDark
        errorController: errorController
        issuesOpen: recentErrorsDialog.visible
        playbackClock: playbackController.playbackClock
        onLastFmSettingsRequested: preferencesDialog.openLastFmPage()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Kirigami.InlineMessage {
            objectName: "backendStoppedMessage"
            Layout.fillWidth: true
            type: Kirigami.MessageType.Error
            showCloseButton: false
            visible: root.backendStopped
            text: root.uiBridge.backendFailure || ""
        }

        Panes.TransportBar {
            id: transportBar
            objectName: "transportBar"
            Layout.fillWidth: true
            enabled: !root.backendStopped
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            previousAction: previousAction
            playAction: playAction
            pauseAction: pauseAction
            stopAction: stopAction
            nextAction: nextAction
            volumeMuted: playbackController.volumeMuted
            keyboardSeekStepSeconds: root.keyboardSeekStepSeconds
            displayedPositionSeconds: playbackController.positionSeconds
            currentPositionSeconds: playbackController.currentPositionSeconds
            positionText: playbackController.positionText
            positionSecond: playbackController.positionSecond
            toggleMutedVolume: playbackController.toggleMutedVolume
            setAppVolume: playbackController.setAppVolume
            normalizedVolumeValue: playbackController.normalizedVolumeValue
            seekCommitted: playbackController.seekCommitted
        }

        // The first of the four seams: between the transport's chrome and
        // the content. The status bar draws the last, and the split handles
        // the two between the panes.
        Rectangle {
            id: transportSeam
            objectName: "transportSeam"
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: root.uiPalette.chromeLine
        }

        SplitView {
            id: mainSplit
            objectName: "mainSplit"
            Layout.fillWidth: true
            Layout.fillHeight: true
            enabled: !root.backendStopped
            orientation: Qt.Horizontal
            handle: seamHandle

            // A drag of a handle sets the share the pane keeps as the window
            // is resized.
            onResizingChanged: {
                if (!resizing) {
                    windowLayoutController.noteSidebarWidth(sidebarPane.width, mainSplit.width)
                }
            }

            Panes.SidebarPane {
                id: sidebarPane
                objectName: "sidebarPane"
                controller: libraryController
                uiBridge: root.uiBridge
                libraryModel: root.libraryTreeModel
                uiPalette: root.uiPalette
                splitPreferredWidth: windowLayoutController.sidebarWidth(
                    mainSplit.width,
                    sidebarPane.SplitView.minimumWidth,
                    rightSplit.SplitView.minimumWidth + root.splitHandleAllowance,
                    // 360 px in a 1600 px window, 260 px in a 1000 px one.
                    Math.max(260, Math.min(360, Math.round(mainSplit.width * 0.225))))
                replaceFromItunesAction: replaceFromItunesAction
                currentTrackItunesArtworkDisabledReason: viewerController.currentTrackItunesArtworkDisabledReason
                openAlbumArtViewer: viewerController.openAlbumArtViewer
                snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                stepScrollView: root.stepScrollView
                addLibraryFolderAction: addLibraryFolderAction
                errorController: errorController
            }

            SplitView {
                id: rightSplit
                objectName: "rightSplit"
                orientation: Qt.Vertical
                handle: seamHandle
                SplitView.fillWidth: true
                SplitView.minimumWidth: queuePane.minimumContentWidth

                onResizingChanged: {
                    if (!resizing) {
                        windowLayoutController.noteQueueHeight(queuePane.height, rightSplit.height)
                    }
                }

                Panes.QueuePane {
                    id: queuePane
                    objectName: "mainQueuePane"
                    controller: queueController
                    uiBridge: root.uiBridge
                    uiPalette: root.uiPalette
                    preferredHeight: windowLayoutController.queueHeight(
                        rightSplit.height,
                        queuePane.SplitView.minimumHeight,
                        spectrogramPane.SplitView.minimumHeight + root.splitHandleAllowance,
                        Math.max(220, root.height * 0.58))
                    playlistIndicatorColumnWidth: root.playlistIndicatorColumnWidth
                    playlistOrderColumnWidth: root.playlistOrderColumnWidth
                    playlistOrderText: FormatUtils.playlistOrderText
                    libraryController: libraryController
                    stepScrollView: root.stepScrollView
                    moveUpKeyHint: String(moveTrackUpAction.shortcut)
                    moveDownKeyHint: String(moveTrackDownAction.shortcut)
                    snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                    snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                    droppedExternalPaths: PathUtils.droppedExternalPaths
                    errorController: errorController
                    addLibraryFolderAction: addLibraryFolderAction
                }

                Panes.SpectrogramPane {
                    id: spectrogramPane
                    objectName: "spectrogramPane"
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: root.visualizationMinimumHeight
                    openViewer: viewerController.openSpectrogramViewer
                    uiPalette: root.uiPalette
                    visualizationMode: viewerController.visualizationMode
                }
            }
        }
    }

    Item {
        id: visualizationSurface
        objectName: "visualizationSurface"
        // The fullscreen window is made as the viewer first opens, so for a
        // moment it may not be there yet: the views stay where they are
        // until it is, rather than leave every window.
        parent: viewerController.spectrogramViewerOpen
            ? (root.useWholeScreenViewerMode
                ? (spectrogramViewerShell.windowHost || spectrogramPane.hostItem)
                : spectrogramViewerShell.popupHost)
            : spectrogramPane.hostItem
        visible: parent !== null
        anchors.fill: parent
        // What the plot keeps clear of in its top right corner: the mode
        // switch, and in a viewer the close button beside it.
        readonly property size reservedCorner: Qt.size(
            plotLayer.reservedCorner.width - plotLayer.meterWidth,
            Math.max(plotLayer.reservedCorner.height,
                     viewerController.spectrogramViewerOpen
                         ? spectrogramViewerShell.reservedCorner.height : 0))
        readonly property bool spectrogramShown: viewerController.visualizationMode === 0

        Viewers.SpectrogramSurface {
            id: spectrogramSurface
            objectName: "mainSpectrogramSurface"
            Accessible.role: Accessible.Graphic
            Accessible.name: I18n.i18nc("@label", "Spectrogram")
            Accessible.description: I18n.i18nc("@info", "Frequencies of the playing track over time")
            anchors.fill: parent
            plotWidth: plotLayer.plotWidth
            visible: visualizationSurface.spectrogramShown
            viewerMode: viewerController.spectrogramViewerOpen
            interactiveOverlaysVisible: spectrogramViewerShell.fullscreenControlsVisible
            pointerActivity: spectrogramViewerShell.noteFullscreenPointerActivity
            reservedCorner: visualizationSurface.reservedCorner
            markerSeconds: plotLayer.cutSeconds
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            clock: playbackController.playbackClock
            seekCommitted: playbackController.seekCommitted
        }

        Viewers.WaveformSurface {
            id: waveformSurface
            objectName: "mainWaveformSurface"
            anchors.fill: parent
            plotWidth: plotLayer.plotWidth
            visible: !visualizationSurface.spectrogramShown
            interactiveOverlaysVisible: spectrogramViewerShell.fullscreenControlsVisible
            pointerActivity: spectrogramViewerShell.noteFullscreenPointerActivity
            reservedCorner: visualizationSurface.reservedCorner
            markerSeconds: plotLayer.cutSeconds
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            clock: playbackController.playbackClock
            seekCommitted: playbackController.seekCommitted
        }

        // The labels, the mode switch, the level meter and the states, over
        // whichever plot shows, in its colours.
        Viewers.PlotLayer {
            id: plotLayer
            anchors.fill: parent
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            clock: playbackController.playbackClock
            visualizationMode: viewerController.visualizationMode
            setVisualizationMode: viewerController.setVisualizationMode
            controlsVisible: spectrogramViewerShell.fullscreenControlsVisible
            pointerActivity: spectrogramViewerShell.noteFullscreenPointerActivity
            trailingInset: viewerController.spectrogramViewerOpen
                ? spectrogramViewerShell.reservedCorner.width : 0
            laneCount: visualizationSurface.spectrogramShown
                ? spectrogramSurface.laneCount : waveformSurface.laneCount
            laneLabels: visualizationSurface.spectrogramShown
                ? spectrogramSurface.laneLabels : waveformSurface.laneLabels
            hoveredLane: visualizationSurface.spectrogramShown
                ? spectrogramSurface.hoveredLane : waveformSurface.hoveredLane
            analysing: visualizationSurface.spectrogramShown
                ? spectrogramSurface.awaitingData : waveformSurface.awaitingData
            markerX: visualizationSurface.spectrogramShown
                ? spectrogramSurface.markerX : waveformSurface.markerX
            openIssues: function() { errorController.historyRequested() }
        }
    }

    Viewers.SpectrogramViewerShell {
        id: spectrogramViewerShell
        windowRoot: root
        uiPalette: root.uiPalette
        viewerOpen: viewerController.spectrogramViewerOpen
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        popupTransitionMs: root.uiPopupTransitionMs
        titleText: root.title
        closeViewer: viewerController.closeSpectrogramViewer
        visualizationMode: viewerController.visualizationMode
        setVisualizationMode: viewerController.setVisualizationMode
        displaySleepInhibitor: root.displaySleepInhibitorApi
        displaySleepInhibitionEnabled: root.uiBridge.preventDisplaySleepInFullscreen
        playbackPlaying: root.uiBridge.playbackState === "Playing"
        viewerActions: root.viewerActions
        errorController: errorController
    }

    Dialogs.ItunesArtworkDialog {
        id: itunesArtworkDialog
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        openAlbumArtViewerForSuggestion: viewerController.openAlbumArtViewerForSuggestion
        openAlbumArtViewerForCurrentArt: viewerController.openAlbumArtViewerForCurrentArt
    }

    Viewers.AlbumArtViewerShell {
        id: albumArtViewerShell
        windowRoot: root
        viewerOpen: viewerController.albumArtViewerOpen
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        popupTransitionMs: root.uiPopupTransitionMs
        titleText: root.title
        closeViewer: viewerController.closeAlbumArtViewer
        toggleInfoVisible: function() {
            viewerController.toggleAlbumArtInfoVisible(albumArtViewerShell.focusFullscreen)
        }
        switchComparisonImage: viewerController.switchComparisonImage
        openContextMenu: function() { albumArtSurface.openContextMenu() }
        viewerActions: root.viewerActions
        errorController: errorController
    }

    Viewers.AlbumArtSurface {
        id: albumArtSurface
        objectName: "albumArtSurface"
        parent: viewerController.albumArtViewerOpen
            ? (root.useWholeScreenViewerMode
                ? (albumArtViewerShell.windowHost || albumArtMainHost)
                : albumArtViewerShell.popupHost)
            : albumArtMainHost
        visible: viewerController.albumArtViewerOpen
        anchors.fill: parent
        viewerOpen: viewerController.albumArtViewerOpen
        viewerSource: viewerController.albumArtViewerSource
        infoVisible: viewerController.albumArtInfoVisible
        initialViewToken: viewerController.albumArtViewResetToken
        viewerDecodeWidth: root.albumArtViewerDecodeWidth
        viewerDecodeHeight: root.albumArtViewerDecodeHeight
        infoOverlayText: viewerController.albumArtInfoOverlayText()
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: viewerController.currentTrackItunesArtworkDisabledReason
        closeViewer: viewerController.closeAlbumArtViewer
        toggleInfoVisible: function() {
            viewerController.toggleAlbumArtInfoVisible(albumArtViewerShell.focusFullscreen)
        }
        focusFullscreen: albumArtViewerShell.focusFullscreen
        comparisonLabel: viewerController.comparisonLabel
        comparisonModeAvailable: viewerController.comparisonModeAvailable
    }

    Item {
        id: albumArtMainHost
        visible: false
    }

    // Holds the current cover decoded as the album art viewer shows it, so
    // the viewer opens at once. Qt shares a decoded image only between
    // Images with the same source, sourceSize, device pixel ratio, fillMode
    // (Fit or Crop), autoTransform and sourceClipRect: keep these as in
    // AlbumArtSurface's Image, or the viewer decodes the cover again.
    Image {
        id: albumArtViewerPreloadImage
        objectName: "albumArtViewerPreloadImage"
        visible: false
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: true
        retainWhileLoading: true
        source: !viewerController.albumArtViewerOpen || viewerController.albumArtViewerShowsCurrentTrack
            ? (uiBridge.currentTrackCoverPath || "")
            : ""
        sourceSize.width: root.albumArtViewerDecodeWidth
        sourceSize.height: root.albumArtViewerDecodeHeight
    }

    // Set once the settings and the queue are saved for quitting, or the
    // user chose to quit without them, so that the window may close.
    property bool quitStateSettled: false
    // The request ID of the latest save attempt still waiting for its
    // answer, or 0. Only its answer settles quitting; the timeout only
    // shows the dialog, so a late answer still does.
    property real quitStateRequestId: 0

    onClosing: function(close) {
        if (!root.quitStateSettled) {
            // Closes again once the save has answered, while the window can
            // still show a failure.
            close.accepted = false
            if (root.quitStateRequestId !== 0) {
                return
            }
            // Unsaved tag edits are saved or dropped first; the tag editor
            // closes the window again once they are.
            if (!tagEditorDialog.prepareToQuit()) {
                root.bringWindowForward()
                return
            }
            root.saveStateBeforeClosing()
            return
        }
        uiBridge.shutdown()
    }

    function saveStateBeforeClosing() {
        // The library view is saved with the queue, and the window's layout
        // with the settings.
        libraryController.persistLibraryViewState()
        windowLayoutController.persist()
        quitStateSaveTimeout.restart()
        root.quitStateRequestId = uiBridge.saveStateBeforeQuit()
    }

    function finishStateSaveBeforeClosing(requestId, saved, message) {
        if (requestId === 0 || requestId !== root.quitStateRequestId) {
            // An earlier attempt, superseded by a retry, or one the user
            // has stopped waiting for.
            return
        }
        root.forgetStateSaveBeforeClosing()
        if (saved) {
            quitSaveFailedDialog.close()
            root.closeNow()
        } else {
            root.showQuitSaveFailedDialog(message)
        }
    }

    function showQuitSaveFailedDialog(message) {
        // It is drawn above this dialog, which would hide the choice.
        recentErrorsDialog.close()
        quitSaveFailedDialog.message = message
        quitSaveFailedDialog.open()
        root.bringWindowForward()
    }

    // A quit from outside the window, such as from the desktop's media
    // controls, can find it minimized or behind other windows. Dialogs that
    // quitting opens are drawn in the window, so the window comes forward
    // too; window managers raise the window they activate. A fullscreen
    // viewer is a modal window over the main one, and the dialog would open
    // behind it, so the viewers close first.
    function bringWindowForward() {
        viewerController.closeAlbumArtViewer()
        viewerController.closeSpectrogramViewer()
        if (root.visibility === Window.Minimized || !root.visible) {
            if (windowLayoutController.maximizedWhenShown) {
                root.showMaximized()
            } else {
                root.showNormal()
            }
        }
        if (!root.active) {
            root.requestActivate()
        }
    }

    function forgetStateSaveBeforeClosing() {
        root.quitStateRequestId = 0
        quitStateSaveTimeout.stop()
    }

    function closeNow() {
        root.forgetStateSaveBeforeClosing()
        root.quitStateSettled = true
        root.close()
    }

    Timer {
        id: quitStateSaveTimeout
        objectName: "quitStateSaveTimeout"
        // Saving takes milliseconds. A backend that has not answered by then
        // may be stuck, and the user decides whether to wait or quit. The
        // window still closes if the answer is a success that comes later.
        interval: 5000
        onTriggered: root.showQuitSaveFailedDialog(
            I18n.i18nc("@info", "The backend did not answer in time; settings and the queue may not have been saved."))
    }

    Dialogs.QuitSaveFailedDialog {
        id: quitSaveFailedDialog
        uiPalette: root.uiPalette
        windowRoot: root
        onRetryRequested: root.saveStateBeforeClosing()
        onQuitRequested: root.closeNow()
        // Escape keeps Ferrous open, so a late answer must not close it.
        onRejected: root.forgetStateSaveBeforeClosing()
    }

    Connections {
        target: uiBridge
        function onStateSaveFinished(requestId, saved, message) {
            root.finishStateSaveBeforeClosing(requestId, saved, message)
        }
        function onLibraryChanged() {
            libraryController.tryApplyPersistedViewState()
        }
        function onSnapshotChanged() {
            if (uiBridge.profileLogsEnabled) {
                const t0 = Date.now()
                playbackController.handleSnapshotChanged(
                    function() { spectrogramSurface.haltForCurrentMode() },
                    function(forceReset) { spectrogramSurface.resetForCurrentMode(forceReset) })
                const t1 = Date.now()
                viewerController.handleSnapshotChanged()
                const t2 = Date.now()
                queueController.handleBridgeSnapshotUpdate()
                const t3 = Date.now()
                const total = t3 - t0
                if (total >= 5)
                    console.warn("[qml-signal-profile] onSnapshotChanged total=" + total
                        + "ms playback=" + (t1-t0) + " viewer=" + (t2-t1)
                        + " queueUpdate=" + (t3-t2))
            } else {
                playbackController.handleSnapshotChanged(
                    function() { spectrogramSurface.haltForCurrentMode() },
                    function(forceReset) { spectrogramSurface.resetForCurrentMode(forceReset) })
                viewerController.handleSnapshotChanged()
                queueController.handleBridgeSnapshotUpdate()
            }
        }
        function onTrackIdentityChanged() {
            if (uiBridge.profileLogsEnabled) {
                const t0 = Date.now()
                playbackController.handleSnapshotChanged(
                    function() { spectrogramSurface.haltForCurrentMode() },
                    function(forceReset) { spectrogramSurface.resetForCurrentMode(forceReset) })
                const t1 = Date.now()
                queueController.handleBridgeSnapshotUpdate()
                const t2 = Date.now()
                const total = t2 - t0
                if (total >= 5)
                    console.warn("[qml-signal-profile] onTrackIdentityChanged total=" + total
                        + "ms playback=" + (t1-t0) + " queueUpdate=" + (t2-t1))
            } else {
                playbackController.handleSnapshotChanged(
                    function() { spectrogramSurface.haltForCurrentMode() },
                    function(forceReset) { spectrogramSurface.resetForCurrentMode(forceReset) })
                queueController.handleBridgeSnapshotUpdate()
            }
        }
        function onTrackMetadataChanged() {
            if (uiBridge.profileLogsEnabled) {
                const t0 = Date.now()
                viewerController.handleSnapshotChanged()
                const ms = Date.now() - t0
                if (ms >= 5)
                    console.warn("[qml-signal-profile] onTrackMetadataChanged viewer=" + ms + "ms")
            } else {
                viewerController.handleSnapshotChanged()
            }
        }
        function onPlaybackChanged() {
            playbackController.handlePlaybackChanged(
                function() { spectrogramSurface.haltForCurrentMode() },
                function(forceReset) { spectrogramSurface.resetForCurrentMode(forceReset) })
        }
        function onLibraryTreeFrameReceived(version, treeBytes) {
            libraryController.requestTreeApply(version, treeBytes || "")
        }
        function onLibraryRevealRequested(expandKeys, selectionKey) {
            libraryController.requestBridgeReveal(expandKeys || [], selectionKey || "")
        }
        function onGlobalSearchResultsChanged() {
            globalSearchController.syncSelectionAfterResultsChange()
        }
        function onBridgeError(message, category, severity, subject) {
            errorController.report(category, severity, subject, message)
        }
    }

    Connections {
        target: root.tagEditorApi
        ignoreUnknownSignals: true
        function onErrorReported(message) {
            errorController.report(
                ErrorUtils.CategoryTags, ErrorUtils.SeverityError, "", message)
        }
    }

    // Shown when the user asked the fullscreen viewer to keep the screen awake.
    Connections {
        target: root.displaySleepInhibitorApi
        ignoreUnknownSignals: true
        function onInhibitionFailed(message) {
            errorController.report(
                ErrorUtils.CategorySystem, ErrorUtils.SeverityWarning, "", message)
        }
    }

    Connections {
        target: libraryModel
        function onTreeApplied() {
            libraryController.finishPendingTreeApply()
            libraryController.tryApplyPersistedViewState()
        }
        function onNodeExpansionRequested(key, expanded) {
            uiBridge.setLibraryNodeExpanded(key, expanded)
        }
    }

    Dialogs.TagEditorDialog {
        id: tagEditorDialog
        tagEditorApi: root.tagEditorApi
        uiPalette: root.uiPalette
        windowRoot: root
        onQuitMayProceed: root.close()
    }

    Component.onCompleted: {
        windowLayoutController.showWindow()
        libraryController.requestTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        playbackController.initializeFromBridge()
        queueController.initializeFromBridge()
        libraryController.syncSelectionToVisibleRows()
        globalSearchController.syncSelectionAfterResultsChange()
    }
}
