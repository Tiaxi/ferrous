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
    visible: true
    readonly property string appDisplayName: "Ferrous"
    title: {
        const context = playbackController.windowTitleContext()
        return context.length > 0
            ? context + " \u2014 " + root.appDisplayName
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
    readonly property int uiPopupTransitionMs: 0
    readonly property bool visualFeedsEnabled: visible
        && visibility !== Window.Minimized
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
    readonly property int levelMeterHeight: 20
    readonly property int embeddedLevelMeterHeight: uiBridge.showLevelMeter ? levelMeterHeight : 0
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

    Components.UiPalette {
        id: uiPaletteObject
        windowRoot: root
    }

    Controllers.GlobalSearchController {
        id: globalSearchController
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

    // The window opens where it was, unless that is off the screens there
    // are now; see BridgeClient::savedWindowLayout. Set before the window
    // is shown, as a binding would not be.
    Binding {
        target: root
        property: "x"
        value: windowLayoutController.saved.x
        when: windowLayoutController.hasSavedGeometry
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: root
        property: "y"
        value: windowLayoutController.saved.y
        when: windowLayoutController.hasSavedGeometry
        restoreMode: Binding.RestoreNone
    }
    Binding {
        target: root
        property: "visibility"
        value: Window.Maximized
        when: windowLayoutController.initiallyMaximized
        restoreMode: Binding.RestoreNone
    }

    readonly property int visualizationMinimumHeight: 220
    // Room for a split handle, which is a few pixels thick in every style.
    readonly property int splitHandleAllowance: 8
    // The smallest window that shows every pane at its own minimum size,
    // with the level meter whether it is shown or not, so that the window's
    // minimum does not change with a setting. The error banner, which comes
    // and goes, takes its room from the panes.
    readonly property int contentMinimumWidth: Math.ceil(Math.max(
        sidebarPane.SplitView.minimumWidth + rightSplit.SplitView.minimumWidth
            + root.splitHandleAllowance,
        transportBar.implicitWidth))
    readonly property int contentMinimumHeight: Math.ceil(
        (root.menuBar ? root.menuBar.implicitHeight : 0)
        + transportBar.implicitHeight
        + queuePane.SplitView.minimumHeight
        + root.visualizationMinimumHeight + root.levelMeterHeight
        + root.splitHandleAllowance
        + (root.footer ? root.footer.implicitHeight : 0))

    readonly property var libraryViewRef: libraryController.view

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
            albumArtViewerShell.windowHost,
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
        property real dbRange: 132
        property int fftSize: 8192
        property int spectrogramViewMode: 0
        property int spectrogramDisplayMode: 0
        property int viewerFullscreenMode: 0
        property bool preventDisplaySleepInFullscreen: true
        property bool logScale: false
        property int repeatMode: 0
        property bool shuffleEnabled: false
        property bool showFps: false
        property bool showLevelMeter: true
        property bool showSpectrogramCrosshair: false
        property bool showSpectrogramScale: false
        property bool spectrogramZoomEnabled: true
        property int soloedChannel: -1
        property double mutedChannelsMask: 0
        property int channelButtonsVisibility: 1
        property bool systemMediaControlsEnabled: true
        property bool libraryWatchEnabled: true
        property bool lastFmScrobblingEnabled: false
        property bool lastFmBuildConfigured: false
        property string lastFmUsername: ""
        property int lastFmAuthState: 0
        property int lastFmPendingScrobbleCount: 0
        property string lastFmStatusText: ""
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
        property string fileBrowserName: "File Manager"
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
        function setRepeatMode(mode) {}
        function setShuffleEnabled(value) {}
        function setShowFps(value) {}
        function setShowLevelMeter(value) { showLevelMeter = value }
        function setShowSpectrogramCrosshair(value) {}
        function setShowSpectrogramScale(value) {}
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
        function savedWindowLayout() { return ({}) }
        function setWindowLayout(layout) {}
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
        function isChannelMuted(channelIndex) { return false }
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
        function extraValuesNote(field) { return "" }
        function applyBulkField(field, value) {}
        function applyBulkFieldToRows(rows, field, value) {}
        function setCell(row, field, value) {}
        function applyEnglishTitleCase(field) {}
        function applyFinnishCapitalize(field) {}
        function applyGenreCapitalize() {}
        function autoNumber(startingTrack, startingDisc, writeDiscNumbers, writeTotals, resetOnFolder, resetOnDiscChange) {}
    }

    function menuPopupWidth(items) {
        let maxPx = 0
        for (let i = 0; i < items.length; ++i) {
            const item = items[i]
            const label = FormatUtils.withoutMnemonic(item.label)
            const shortcut = item.shortcut || ""
            let px = menuFontMetrics.boundingRect(label).width + 72
            if (shortcut.length > 0) {
                px += menuFontMetrics.boundingRect(shortcut).width + 24
            }
            maxPx = Math.max(maxPx, px)
        }
        return Math.max(140, Math.ceil(maxPx))
    }

    // Fits the widest number, and at least four characters.
    readonly property int playlistOrderColumnWidth: Math.ceil(Math.max(
        playlistOrderFontMetrics.averageCharacterWidth * 4,
        playlistOrderTextMetrics.boundingRect.width + playlistOrderFontMetrics.averageCharacterWidth * 1.5))
    readonly property bool globalSearchShowsRootColumn: (uiBridge.libraryRootCount || 0) >= 2
    // Wide enough for the play, pause or stop symbol.
    readonly property int playlistIndicatorColumnWidth: Math.ceil(playlistOrderFontMetrics.height)

    FontMetrics {
        id: menuFontMetrics
        font: root.font
    }

    FontMetrics {
        id: playlistOrderFontMetrics
        font: root.font
    }

    TextMetrics {
        id: playlistOrderTextMetrics
        font: root.font
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

    function channelStatusIconSource(iconKey) {
        switch (iconKey) {
        case "mono":
            return Qt.resolvedUrl("assets/channel-mono.svg")
        case "stereo":
            return Qt.resolvedUrl("assets/channel-stereo.svg")
        case "4.0":
            return Qt.resolvedUrl("assets/channel-4_0.svg")
        case "5.0":
            return Qt.resolvedUrl("assets/channel-5_0.svg")
        case "5.1":
            return Qt.resolvedUrl("assets/channel-5_1.svg")
        case "7.1":
            return Qt.resolvedUrl("assets/channel-7_1.svg")
        default:
            return ""
        }
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

    Action {
        id: openFilesAction
        text: "&Open File(s)..."
        shortcut: StandardKey.Open
        enabled: !root.viewerOpen
        onTriggered: root.openExternalFiles()
    }
    Action {
        id: addFilesAction
        text: "&Add File(s)..."
        onTriggered: root.addExternalFiles()
    }
    // The folder dialog picks one folder.
    Action {
        id: addFolderAction
        text: "Add &Folder to Queue..."
        onTriggered: root.addExternalFolder()
    }
    // As Preferences > Library adds one.
    Action {
        id: addLibraryFolderAction
        objectName: "addLibraryFolderAction"
        text: "Add Library Fol&der..."
        onTriggered: root.promptAddLibraryRoot("add-library-root")
    }
    Action {
        id: quitAction
        objectName: "quitAction"
        text: "&Quit"
        shortcut: StandardKey.Quit
        // Closing the window, not quitting, runs onClosing, which saves the
        // settings and the queue and stops the backend.
        onTriggered: root.close()
    }
    Action {
        id: playLibrarySelectionAction
        text: "&Play Library Selection"
        enabled: libraryController.canPlaySelection()
        onTriggered: libraryController.playSelection()
    }
    Action {
        id: appendLibrarySelectionAction
        text: "Q&ueue Library Selection"
        enabled: libraryController.canPlaySelection()
        onTriggered: libraryController.appendSelection()
    }
    Action {
        id: playAllLibraryTracksAction
        text: "Play A&ll Library Tracks"
        enabled: libraryController.canPlayAllTracks()
        onTriggered: libraryController.playAllTracks()
    }
    Action {
        id: appendAllLibraryTracksAction
        text: "Queue All Library &Tracks"
        enabled: libraryController.canPlayAllTracks()
        onTriggered: libraryController.appendAllTracks()
    }
    Action {
        id: replaceFromItunesAction
        text: "Replace From iTunes..."
        enabled: viewerController.currentTrackItunesArtworkDisabledReason().length === 0
        onTriggered: root.openItunesArtworkDialog()
    }
    Action {
        id: preferencesAction
        text: "P&references..."
        shortcut: StandardKey.Preferences
        enabled: !root.viewerOpen
        onTriggered: preferencesDialog.open()
    }
    // Delete removes the selected tracks while the queue has focus; the
    // queue view handles it, so it is not a window-wide shortcut.
    Action {
        id: removeSelectedTrackAction
        text: "&Remove Selected Track"
        enabled: queueController.selectionCount() > 0 && !root.viewerOpen
        onTriggered: queueController.removeSelectedTrack()
    }
    Action {
        id: undoQueueChangeAction
        objectName: "undoQueueChangeAction"
        text: {
            switch (uiBridge.queueUndoKind) {
            case 1:
                return "&Undo Clear Playlist"
            case 2:
                return "&Undo Remove Tracks"
            case 3:
                return "&Undo Replace Queue"
            case 4:
                return "&Undo Move Tracks"
            default:
                return "&Undo"
            }
        }
        // Text fields keep Ctrl+Z for their own text.
        shortcut: StandardKey.Undo
        enabled: uiBridge.queueUndoId > 0 && !root.viewerOpen
        onTriggered: uiBridge.undoQueueChange()
    }
    Action {
        id: selectPreviousTrackAction
        text: "Select Pre&vious Track"
        shortcut: "Ctrl+Up"
        enabled: uiBridge.queueLength > 0 && !root.viewerOpen
        onTriggered: queueController.selectRelative(-1)
    }
    Action {
        id: selectNextTrackAction
        text: "Select &Next Track"
        shortcut: "Ctrl+Down"
        enabled: uiBridge.queueLength > 0 && !root.viewerOpen
        onTriggered: queueController.selectRelative(1)
    }
    Action {
        id: globalSearchAction
        text: "&Global Search..."
        shortcut: StandardKey.Find
        enabled: !root.viewerOpen
        onTriggered: globalSearchController.openDialog()
    }
    Action {
        id: diagnosticsAction
        text: "&Diagnostics..."
        onTriggered: root.openDiagnostics()
    }
    Action {
        id: autoCenterSelectionAction
        text: "&Follow Current Track in Playlist"
        checkable: true
        checked: queueController.autoCenterSelection
        onTriggered: queueController.autoCenterSelection = checked
    }
    Action {
        id: resetSpectrogramAction
        text: "&Reset Visualization View"
        onTriggered: {
            if (viewerController.visualizationMode === 0) {
                spectrogramSurface.resetForCurrentMode()
            } else {
                waveformSurface.waveformItem.resetZoom()
            }
        }
    }
    Action {
        id: showFpsOverlayAction
        text: "Show Visualization FP&S"
        checkable: true
        checked: uiBridge.showFps
        onTriggered: uiBridge.setShowFps(checked)
    }
    Action {
        id: shuffleAction
        text: "S&huffle"
        checkable: true
        checked: uiBridge.shuffleEnabled
        onTriggered: uiBridge.setShuffleEnabled(checked)
    }
    Action {
        id: repeatOffAction
        text: "Repeat O&ff"
        checkable: true
        checked: uiBridge.repeatMode === 0
        onTriggered: uiBridge.setRepeatMode(0)
    }
    Action {
        id: repeatOneAction
        text: "Repeat &One"
        checkable: true
        checked: uiBridge.repeatMode === 1
        onTriggered: uiBridge.setRepeatMode(1)
    }
    Action {
        id: repeatAllAction
        text: "Repeat &All"
        checkable: true
        checked: uiBridge.repeatMode === 2
        onTriggered: uiBridge.setRepeatMode(2)
    }
    Action {
        id: aboutAction
        text: "&About Ferrous"
        onTriggered: aboutDialog.open()
    }
    Action {
        id: previousAction
        text: "P&revious"
        icon.name: "media-skip-backward"
        shortcut: "Ctrl+Left"
        onTriggered: uiBridge.previous()
    }
    Action {
        id: playAction
        text: "Play"
        icon.name: "media-playback-start"
        onTriggered: uiBridge.play()
    }
    Action {
        id: pauseAction
        text: "Pause"
        icon.name: "media-playback-pause"
        onTriggered: uiBridge.pause()
    }
    Action {
        id: stopAction
        objectName: "stopAction"
        text: "&Stop"
        icon.name: "media-playback-stop"
        // Not Ctrl+Space, which switches input methods in Fcitx and IBus,
        // nor Shift+Space, which toggles full-width input in Fcitx.
        shortcut: "Ctrl+Shift+Space"
        onTriggered: uiBridge.stop()
    }
    Action {
        id: nextAction
        text: "&Next"
        icon.name: "media-skip-forward"
        shortcut: "Ctrl+Right"
        onTriggered: uiBridge.next()
    }
    Action {
        id: clearPlaylistAction
        text: "&Clear Playlist"
        onTriggered: uiBridge.clearQueue()
    }
    Action {
        id: moveTrackUpAction
        text: "Move Track &Up"
        shortcut: "Ctrl+Shift+Up"
        enabled: !root.viewerOpen
        onTriggered: queueController.moveSelected(-1)
    }
    Action {
        id: moveTrackDownAction
        text: "Move Track &Down"
        shortcut: "Ctrl+Shift+Down"
        enabled: !root.viewerOpen
        onTriggered: queueController.moveSelected(1)
    }

    Action {
        id: playPauseAction
        objectName: "playPauseAction"
        text: "&Play/Pause"
        // Text fields take Space for typing before shortcuts see it.
        shortcut: "Space"
        onTriggered: playbackController.togglePlayPause()
    }
    Action {
        id: seekBackwardAction
        objectName: "seekBackwardAction"
        text: "Seek &Backward"
        shortcut: "Shift+Left"
        enabled: playbackController.canSeek
        onTriggered: playbackController.seekBy(-root.keyboardSeekStepSeconds)
    }
    Action {
        id: seekForwardAction
        objectName: "seekForwardAction"
        text: "Seek &Forward"
        shortcut: "Shift+Right"
        enabled: playbackController.canSeek
        onTriggered: playbackController.seekBy(root.keyboardSeekStepSeconds)
    }
    Action {
        id: volumeUpAction
        objectName: "volumeUpAction"
        text: "Volume &Up"
        shortcut: "Alt+Up"
        onTriggered: playbackController.changeVolumeBy(root.keyboardVolumeStep)
    }
    Action {
        id: volumeDownAction
        objectName: "volumeDownAction"
        text: "Volume &Down"
        shortcut: "Alt+Down"
        onTriggered: playbackController.changeVolumeBy(-root.keyboardVolumeStep)
    }
    Action {
        id: muteAction
        objectName: "muteAction"
        text: "&Mute"
        // Not Ctrl+M, which KDE applications use to show the menu bar.
        shortcut: "Ctrl+Shift+M"
        checkable: true
        checked: playbackController.volumeMuted
        onTriggered: playbackController.toggleMutedVolume()
    }
    Action {
        id: visualizationViewerAction
        objectName: "visualizationViewerAction"
        text: "&Visualization Viewer"
        shortcut: "F11"
        checkable: true
        checked: viewerController.spectrogramViewerOpen
        onTriggered: viewerController.toggleSpectrogramViewer()
    }
    Action {
        id: albumArtViewerAction
        objectName: "albumArtViewerAction"
        text: "&Album Art Viewer"
        shortcut: "Shift+F11"
        checkable: true
        checked: viewerController.albumArtViewerOpen
        enabled: viewerController.albumArtViewerOpen
            || (uiBridge.currentTrackCoverPath || "").length > 0
        onTriggered: viewerController.toggleAlbumArtViewer()
    }

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
        visualizationViewerAction, albumArtViewerAction, quitAction
    ]
    menuBar: MenuBar {
        Menu {
            title: "&File"
            width: root.menuPopupWidth([
                { label: openFilesAction.text, shortcut: String(openFilesAction.shortcut || "") },
                { label: addFilesAction.text, shortcut: String(addFilesAction.shortcut || "") },
                { label: addFolderAction.text, shortcut: String(addFolderAction.shortcut || "") },
                { label: addLibraryFolderAction.text, shortcut: String(addLibraryFolderAction.shortcut || "") },
                { label: playLibrarySelectionAction.text, shortcut: String(playLibrarySelectionAction.shortcut || "") },
                { label: appendLibrarySelectionAction.text, shortcut: String(appendLibrarySelectionAction.shortcut || "") },
                { label: playAllLibraryTracksAction.text, shortcut: String(playAllLibraryTracksAction.shortcut || "") },
                { label: appendAllLibraryTracksAction.text, shortcut: String(appendAllLibraryTracksAction.shortcut || "") },
                { label: quitAction.text, shortcut: String(quitAction.shortcut || "") }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: openFilesAction }
            MenuItem { action: addFilesAction }
            MenuItem { action: addFolderAction }
            MenuItem { action: addLibraryFolderAction }
            MenuSeparator {}
            MenuItem { action: playLibrarySelectionAction }
            MenuItem { action: appendLibrarySelectionAction }
            MenuItem { action: playAllLibraryTracksAction }
            MenuItem { action: appendAllLibraryTracksAction }
            MenuSeparator {}
            MenuItem { action: quitAction }
        }
        Menu {
            title: "&Edit"
            width: root.menuPopupWidth([
                { label: undoQueueChangeAction.text, shortcut: String(undoQueueChangeAction.shortcut || "") },
                { label: removeSelectedTrackAction.text, shortcut: String(removeSelectedTrackAction.shortcut || "") },
                { label: moveTrackUpAction.text, shortcut: String(moveTrackUpAction.shortcut || "") },
                { label: moveTrackDownAction.text, shortcut: String(moveTrackDownAction.shortcut || "") },
                { label: selectPreviousTrackAction.text, shortcut: String(selectPreviousTrackAction.shortcut || "") },
                { label: selectNextTrackAction.text, shortcut: String(selectNextTrackAction.shortcut || "") },
                { label: clearPlaylistAction.text, shortcut: String(clearPlaylistAction.shortcut || "") },
                { label: preferencesAction.text, shortcut: String(preferencesAction.shortcut || "") }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: undoQueueChangeAction }
            MenuSeparator {}
            MenuItem { action: removeSelectedTrackAction }
            MenuItem { action: moveTrackUpAction }
            MenuItem { action: moveTrackDownAction }
            MenuSeparator {}
            MenuItem { action: selectPreviousTrackAction }
            MenuItem { action: selectNextTrackAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
            MenuSeparator {}
            MenuItem { action: preferencesAction }
        }
        Menu {
            title: "&View"
            width: root.menuPopupWidth([
                { label: globalSearchAction.text, shortcut: String(globalSearchAction.shortcut || "") },
                { label: visualizationViewerAction.text, shortcut: String(visualizationViewerAction.shortcut || "") },
                { label: albumArtViewerAction.text, shortcut: String(albumArtViewerAction.shortcut || "") },
                { label: autoCenterSelectionAction.text, shortcut: String(autoCenterSelectionAction.shortcut || "") },
                { label: resetSpectrogramAction.text, shortcut: String(resetSpectrogramAction.shortcut || "") },
                { label: showFpsOverlayAction.text, shortcut: String(showFpsOverlayAction.shortcut || "") }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: globalSearchAction }
            MenuSeparator {}
            MenuItem { action: visualizationViewerAction }
            MenuItem { action: albumArtViewerAction }
            MenuSeparator {}
            MenuItem { action: autoCenterSelectionAction }
            MenuItem { action: resetSpectrogramAction }
            MenuItem { action: showFpsOverlayAction }
        }
        Menu {
            title: "&Playback"
            width: root.menuPopupWidth([
                { label: playPauseAction.text, shortcut: String(playPauseAction.shortcut || "") },
                { label: stopAction.text, shortcut: String(stopAction.shortcut || "") },
                { label: previousAction.text, shortcut: String(previousAction.shortcut || "") },
                { label: nextAction.text, shortcut: String(nextAction.shortcut || "") },
                { label: seekBackwardAction.text, shortcut: String(seekBackwardAction.shortcut || "") },
                { label: seekForwardAction.text, shortcut: String(seekForwardAction.shortcut || "") },
                { label: volumeUpAction.text, shortcut: String(volumeUpAction.shortcut || "") },
                { label: volumeDownAction.text, shortcut: String(volumeDownAction.shortcut || "") },
                { label: muteAction.text, shortcut: String(muteAction.shortcut || "") },
                { label: shuffleAction.text, shortcut: String(shuffleAction.shortcut || "") },
                { label: repeatOffAction.text, shortcut: String(repeatOffAction.shortcut || "") },
                { label: repeatOneAction.text, shortcut: String(repeatOneAction.shortcut || "") },
                { label: repeatAllAction.text, shortcut: String(repeatAllAction.shortcut || "") },
                { label: clearPlaylistAction.text, shortcut: String(clearPlaylistAction.shortcut || "") }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: playPauseAction }
            MenuItem { action: stopAction }
            MenuItem { action: previousAction }
            MenuItem { action: nextAction }
            MenuSeparator {}
            MenuItem { action: seekBackwardAction }
            MenuItem { action: seekForwardAction }
            MenuSeparator {}
            MenuItem { action: volumeUpAction }
            MenuItem { action: volumeDownAction }
            MenuItem { action: muteAction }
            MenuSeparator {}
            MenuItem { action: shuffleAction }
            MenuItem { action: repeatOffAction }
            MenuItem { action: repeatOneAction }
            MenuItem { action: repeatAllAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
        }
        Menu {
            title: "&Help"
            width: root.menuPopupWidth([
                { label: diagnosticsAction.text, shortcut: String(diagnosticsAction.shortcut || "") },
                { label: aboutAction.text, shortcut: String(aboutAction.shortcut || "") }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: diagnosticsAction }
            MenuSeparator {}
            MenuItem { action: aboutAction }
        }
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
        popupTransitionMs: root.uiPopupTransitionMs
        snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
        snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
        globalSearchShowsRootColumn: root.globalSearchShowsRootColumn
    }

    Dialogs.DiagnosticsDialog {
        id: diagnosticsDialog
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        popupTransitionMs: root.uiPopupTransitionMs
    }

    Platform.FileDialog {
        id: externalFileDialog
        title: pendingFileDialogContext === "open" ? "Open Files" : "Add Files"
        fileMode: Platform.FileDialog.OpenFiles
        nameFilters: [
            "Audio and Playlist Files (*.mp3 *.flac *.m4a *.aac *.ogg *.opus *.wav *.ac3 *.dts *.m3u *.m3u8)",
            "Audio Files (*.mp3 *.flac *.m4a *.aac *.ogg *.opus *.wav *.ac3 *.dts)",
            "Playlist Files (*.m3u *.m3u8)",
            "All Files (*)"
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
            ? "Add Folder to Queue"
            : "Add Library Folder"
        onAccepted: root.takeChosenFolder(PathUtils.folderDialogPath(scanFolderDialog))
        onRejected: pendingFolderDialogContext = ""
    }

    footer: Panes.StatusBar {
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        channelStatusIconSource: root.channelStatusIconSource
        themeIsDark: root.themeIsDark
        errorController: errorController
        positionText: playbackController.positionText
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

        Components.ErrorBanner {
            objectName: "mainErrorBanner"
            Layout.fillWidth: true
            errorController: errorController
            uiPalette: root.uiPalette
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
            positionText: playbackController.positionText
            toggleMutedVolume: playbackController.toggleMutedVolume
            setAppVolume: playbackController.setAppVolume
            normalizedVolumeValue: playbackController.normalizedVolumeValue
            seekCommitted: playbackController.seekCommitted
        }

        SplitView {
            id: mainSplit
            objectName: "mainSplit"
            Layout.fillWidth: true
            Layout.fillHeight: true
            enabled: !root.backendStopped
            orientation: Qt.Horizontal

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
                    Math.max(300, mainSplit.width * 0.26))
                replaceFromItunesAction: replaceFromItunesAction
                currentTrackItunesArtworkDisabledReason: viewerController.currentTrackItunesArtworkDisabledReason
                openAlbumArtViewer: viewerController.openAlbumArtViewer
                popupTransitionMs: root.uiPopupTransitionMs
                snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                stepScrollView: root.stepScrollView
                playAllLibraryTracksAction: playAllLibraryTracksAction
                appendAllLibraryTracksAction: appendAllLibraryTracksAction
                addLibraryFolderAction: addLibraryFolderAction
            }

            SplitView {
                id: rightSplit
                objectName: "rightSplit"
                orientation: Qt.Vertical
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
                        Math.max(220, root.height * 0.58 - root.embeddedLevelMeterHeight))
                    playlistIndicatorColumnWidth: root.playlistIndicatorColumnWidth
                    playlistOrderColumnWidth: root.playlistOrderColumnWidth
                    playlistOrderText: FormatUtils.playlistOrderText
                    libraryController: libraryController
                    stepScrollView: root.stepScrollView
                    clearPlaylistAction: clearPlaylistAction
                    popupTransitionMs: root.uiPopupTransitionMs
                    snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                    snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                    droppedExternalPaths: PathUtils.droppedExternalPaths
                }

                Panes.SpectrogramPane {
                    id: spectrogramPane
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: root.visualizationMinimumHeight + root.embeddedLevelMeterHeight
                    openViewer: viewerController.openSpectrogramViewer
                    visualizationMode: viewerController.visualizationMode
                    setVisualizationMode: viewerController.setVisualizationMode
                }
            }
        }
    }

    Item {
        id: visualizationSurface
        objectName: "visualizationSurface"
        parent: viewerController.spectrogramViewerOpen
            ? (root.useWholeScreenViewerMode
                ? spectrogramViewerShell.windowHost
                : spectrogramViewerShell.popupHost)
            : spectrogramPane.hostItem
        visible: parent !== null
        anchors.fill: parent

        Viewers.SpectrogramSurface {
            id: spectrogramSurface
            objectName: "mainSpectrogramSurface"
            Accessible.role: Accessible.Graphic
            Accessible.name: "Spectrogram"
            Accessible.description: "Frequencies of the playing track over time"
            anchors.fill: parent
            anchors.bottomMargin: levelMeter.height
            visible: viewerController.visualizationMode === 0
            viewerMode: viewerController.spectrogramViewerOpen
            interactiveOverlaysVisible: spectrogramViewerShell.fullscreenControlsVisible
            pointerActivity: spectrogramViewerShell.noteFullscreenPointerActivity
            uiBridge: root.uiBridge
            clock: playbackController.playbackClock
            seekCommitted: playbackController.seekCommitted
        }

        Viewers.WaveformSurface {
            id: waveformSurface
            objectName: "mainWaveformSurface"
            anchors.fill: parent
            anchors.bottomMargin: levelMeter.height
            visible: viewerController.visualizationMode === 1
            interactiveOverlaysVisible: spectrogramViewerShell.fullscreenControlsVisible
            pointerActivity: spectrogramViewerShell.noteFullscreenPointerActivity
            uiBridge: root.uiBridge
            clock: playbackController.playbackClock
            seekCommitted: playbackController.seekCommitted
        }

        LevelMeterItem {
            id: levelMeter
            objectName: "levelMeter"
            Accessible.role: Accessible.Graphic
            Accessible.name: "Level meter"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            visible: root.uiBridge.showLevelMeter && !viewerController.spectrogramViewerOpen
            height: visible ? root.levelMeterHeight : 0
            sourcePath: visible && root.uiBridge.playbackState !== "Stopped" ? root.uiBridge.currentTrackPath : ""
            clock: playbackController.playbackClock
            durationSeconds: root.uiBridge.durationSeconds
            playing: root.uiBridge.connected && root.uiBridge.playbackState === "Playing"
            channelCountHint: root.uiBridge.currentTrackChannels > 0 ? root.uiBridge.currentTrackChannels : 2
        }
    }

    Viewers.SpectrogramViewerShell {
        id: spectrogramViewerShell
        windowRoot: root
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
        uiPalette: root.uiPalette
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
        uiPalette: root.uiPalette
    }

    Viewers.AlbumArtSurface {
        id: albumArtSurface
        parent: viewerController.albumArtViewerOpen
            ? (root.useWholeScreenViewerMode ? albumArtViewerShell.windowHost : albumArtViewerShell.popupHost)
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

    Image {
        id: albumArtViewerPreloadImage
        visible: false
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
            root.showNormal()
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
            "The backend did not answer in time; settings and the queue may not have been saved.")
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
        libraryController.requestTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        playbackController.initializeFromBridge()
        queueController.initializeFromBridge()
        libraryController.syncSelectionToVisibleRows()
        globalSearchController.syncSelectionAfterResultsChange()
    }
}
