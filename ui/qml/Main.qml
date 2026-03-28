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
import "logic/FormatUtils.js" as FormatUtils
import "logic/PathUtils.js" as PathUtils
import "panes" as Panes
import "viewers" as Viewers

Kirigami.ApplicationWindow {
    id: root
    width: 1600
    height: 980
    minimumWidth: 1280
    minimumHeight: 780
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
    property string transientBridgeError: ""
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
        uiBridge: root.uiBridge
        tagEditorApi: root.tagEditorApi
        openTagEditorDialog: function() { tagEditorDialog.open() }
    }

    Controllers.PlaybackController {
        id: playbackController
        uiBridge: root.uiBridge
        visualFeedsEnabled: root.visualFeedsEnabled
        seekPressed: transportBar ? transportBar.seekPressed : false
    }

    Controllers.LibraryController {
        id: libraryController
        uiBridge: root.uiBridge
        libraryModel: root.libraryTreeModel
        tryCaptureGlobalSearchPrefill: globalSearchController.tryCapturePrefill
        tagEditorApi: root.tagEditorApi
        openTagEditorDialog: function() { tagEditorDialog.open() }
    }

    Controllers.ViewerController {
        id: viewerController
        uiBridge: root.uiBridge
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
    }

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
        property string positionText: "00:00"
        property string durationText: "00:00"
        property real positionSeconds: 0
        property real durationSeconds: 0
        property real volume: 1.0
        property int queueLength: 0
        property int queueVersion: 0
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
        property bool logScale: false
        property int repeatMode: 0
        property bool shuffleEnabled: false
        property bool showFps: false
        property bool systemMediaControlsEnabled: true
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
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property string diagnosticsText: ""
        property string diagnosticsLogPath: ""
        property bool connected: false
        signal playbackChanged()
        signal trackIdentityChanged()
        signal trackMetadataChanged()
        signal snapshotChanged()
        signal analysisChanged()
        signal libraryTreeFrameReceived(int version, var treeBytes)
        signal globalSearchResultsChanged()
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal diagnosticsChanged()
        signal bridgeError(string message)
        signal precomputedSpectrogramChunkReady(var data, int bins, int channelCount, int columns,
            int startIndex, int totalEstimate, int sampleRate, int hopSize,
            real coverage, bool complete, bool bufferReset, bool clearHistory, var trackToken)
        function play() {}
        function pause() {}
        function stop() {}
        function next() {}
        function previous() {}
        function seek(seconds) {}
        function setVolume(value) {}
        function setFftSize(value) {}
        function setSpectrogramViewMode(value) {}
        function setSpectrogramDisplayMode(value) {}
        function setViewerFullscreenMode(value) {}
        function setDbRange(value) {}
        function setLogScale(value) {}
        function setRepeatMode(mode) {}
        function setShuffleEnabled(value) {}
        function setShowFps(value) {}
        function setSystemMediaControlsEnabled(value) {}
        function setLastFmScrobblingEnabled(value) {}
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
        function playAt(index) {}
        function selectQueueIndex(index) {}
        function removeAt(index) {}
        function moveQueue(from, to) {}
        function clearQueue() {}
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
        function libraryAlbumCoverAt(index) { return "" }
        function queueTrackNumberAt(index) { return null }
        function queuePathAt(index) { return "" }
        function addLibraryRoot(path, name) {}
        function setLibraryRootName(path, name) {}
        function removeLibraryRoot(path) {}
        function rescanLibraryRoot(path) {}
        function rescanAllLibraryRoots() {}
        function setLibraryNodeExpanded(key, expanded) {}
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
        function shutdown() {}
        function clearDiagnostics() {}
        function reloadDiagnosticsFromDisk() {}
    }

    QtObject {
        id: tagEditorFallback
        property bool open: false
        property bool loading: false
        property bool saving: false
        property bool dirty: false
        property string statusText: ""
        property string statusDetails: ""
        property var tableModel: []
        signal statusChanged()
        function openSelection(selections) { return false }
        function openForPaths(paths) { return false }
        function close() {}
        function reload() {}
        function save() { return false }
        function renameSelectedFiles() { return false }
        function setSelectedRows(rows) {}
        function loadedPaths() { return [] }
        function bulkValue(field) { return "" }
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
            const label = item.label || ""
            const shortcut = item.shortcut || ""
            let px = menuFontMetrics.boundingRect(label).width + 72
            if (shortcut.length > 0) {
                px += menuFontMetrics.boundingRect(shortcut).width + 24
            }
            maxPx = Math.max(maxPx, px)
        }
        return Math.max(140, Math.ceil(maxPx))
    }

    readonly property int playlistOrderColumnWidth: {
        const maxIndex = Math.max(0, uiBridge.queueLength - 1)
        const widestOrderText = FormatUtils.playlistOrderText(maxIndex)
        const valueWidth = playlistOrderFontMetrics.boundingRect(widestOrderText).width
        const headerWidth = playlistOrderFontMetrics.boundingRect("#").width
        return Math.max(28, Math.ceil(Math.max(valueWidth, headerWidth) + 10))
    }
    readonly property bool globalSearchShowsRootColumn: (uiBridge.libraryRootCount || 0) >= 2
    readonly property int globalSearchTrackNumberColumnWidth: Math.max(
        34,
        Math.ceil(Math.max(
            playlistOrderFontMetrics.boundingRect("#").width,
            playlistOrderFontMetrics.boundingRect("00").width) + 10))
    readonly property int globalSearchCoverColumnWidth: 26
    readonly property int globalSearchArtistColumnWidth: 170
    readonly property int globalSearchAlbumColumnWidth: 182
    readonly property int globalSearchYearColumnWidth: 52
    readonly property int globalSearchRootColumnWidth: 140
    readonly property int globalSearchAlbumCountColumnWidth: 40
    readonly property int globalSearchTrackLengthColumnWidth: Math.max(
        40,
        Math.ceil(playlistOrderFontMetrics.boundingRect("00:00").width + 12))
    readonly property int globalSearchTrackGenreColumnWidth: Math.max(
        124,
        Math.ceil(Math.max(
            playlistOrderFontMetrics.boundingRect("Genre").width,
            playlistOrderFontMetrics.boundingRect("Alternative country").width) + 8))
    readonly property int playlistIndicatorColumnWidth: 18

    FontMetrics {
        id: menuFontMetrics
        font: root.font
    }

    FontMetrics {
        id: playlistOrderFontMetrics
        font: root.font
    }

    Timer {
        id: bridgeErrorTimer
        interval: 10000
        repeat: false
        onTriggered: root.transientBridgeError = ""
    }

    function stepScrollView(view, wheel, rowHeight, rowsPerStep) {
        if (!view || !wheel) {
            return
        }
        const maxY = Math.max(0, view.contentHeight - view.height)
        if (maxY <= 0) {
            return
        }
        let deltaY = 0
        if (wheel.angleDelta && wheel.angleDelta.y !== undefined && wheel.angleDelta.y !== 0) {
            deltaY = wheel.angleDelta.y
        } else if (wheel.pixelDelta && wheel.pixelDelta.y !== undefined && wheel.pixelDelta.y !== 0) {
            deltaY = wheel.pixelDelta.y
        }
        if (deltaY === 0) {
            return
        }
        const rowPx = Math.max(8, rowHeight || 24)
        const rows = Math.max(1, rowsPerStep || 1)
        const stepPx = rowPx * rows
        const notches = (wheel.angleDelta && wheel.angleDelta.y !== undefined && wheel.angleDelta.y !== 0)
            ? Math.max(1, Math.round(Math.abs(wheel.angleDelta.y) / 120))
            : Math.max(1, Math.round(Math.abs(deltaY) / stepPx))
        const direction = deltaY > 0 ? -1 : 1
        const targetY = view.contentY + (direction * notches * stepPx)
        view.contentY = Math.max(0, Math.min(maxY, targetY))
        wheel.accepted = true
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
        text: "Open File(s)"
        shortcut: StandardKey.Open
        onTriggered: root.openExternalFiles()
    }
    Action {
        id: addFilesAction
        text: "Add File(s)"
        onTriggered: root.addExternalFiles()
    }
    Action {
        id: addFolderAction
        text: "Add Folder(s)"
        onTriggered: root.addExternalFolder()
    }
    Action {
        id: quitAction
        text: "Quit"
        shortcut: StandardKey.Quit
        onTriggered: Qt.quit()
    }
    Action {
        id: playLibrarySelectionAction
        text: "Play Library Selection"
        enabled: libraryController.canPlaySelection()
        onTriggered: libraryController.playSelection()
    }
    Action {
        id: appendLibrarySelectionAction
        text: "Queue Library Selection"
        enabled: libraryController.canPlaySelection()
        onTriggered: libraryController.appendSelection()
    }
    Action {
        id: playAllLibraryTracksAction
        text: "Play All Library Tracks"
        enabled: libraryController.canPlayAllTracks()
        onTriggered: libraryController.playAllTracks()
    }
    Action {
        id: appendAllLibraryTracksAction
        text: "Queue All Library Tracks"
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
        text: "Preferences..."
        shortcut: StandardKey.Preferences
        onTriggered: preferencesDialog.open()
    }
    Action {
        id: removeSelectedTrackAction
        text: "Remove Selected Track"
        shortcut: "Delete"
        enabled: queueController.selectionCount() > 0
        onTriggered: queueController.removeSelectedTrack()
    }
    Action {
        id: selectPreviousTrackAction
        text: "Select Previous Track"
        shortcut: "Ctrl+Up"
        enabled: uiBridge.queueLength > 0
        onTriggered: queueController.selectRelative(-1)
    }
    Action {
        id: selectNextTrackAction
        text: "Select Next Track"
        shortcut: "Ctrl+Down"
        enabled: uiBridge.queueLength > 0
        onTriggered: queueController.selectRelative(1)
    }
    Action {
        id: globalSearchAction
        text: "Global Search..."
        shortcut: StandardKey.Find
        onTriggered: globalSearchController.openDialog()
    }
    Action {
        id: diagnosticsAction
        text: "Diagnostics..."
        onTriggered: root.openDiagnostics()
    }
    Action {
        id: autoCenterSelectionAction
        text: "Follow Current Track in Playlist"
        checkable: true
        checked: queueController.autoCenterSelection
        onTriggered: queueController.autoCenterSelection = checked
    }
    Action {
        id: resetSpectrogramAction
        text: "Reset Spectrogram View"
        onTriggered: spectrogramSurface.resetForCurrentMode()
    }
    Action {
        id: showFpsOverlayAction
        text: "Show Spectrogram FPS"
        checkable: true
        checked: uiBridge.showFps
        onTriggered: uiBridge.setShowFps(checked)
    }
    Action {
        id: shuffleAction
        text: "Shuffle"
        checkable: true
        checked: uiBridge.shuffleEnabled
        onTriggered: uiBridge.setShuffleEnabled(checked)
    }
    Action {
        id: repeatOffAction
        text: "Repeat Off"
        checkable: true
        checked: uiBridge.repeatMode === 0
        onTriggered: uiBridge.setRepeatMode(0)
    }
    Action {
        id: repeatOneAction
        text: "Repeat One"
        checkable: true
        checked: uiBridge.repeatMode === 1
        onTriggered: uiBridge.setRepeatMode(1)
    }
    Action {
        id: repeatAllAction
        text: "Repeat All"
        checkable: true
        checked: uiBridge.repeatMode === 2
        onTriggered: uiBridge.setRepeatMode(2)
    }
    Action {
        id: aboutAction
        text: "About Ferrous"
        onTriggered: aboutDialog.open()
    }
    Action {
        id: previousAction
        text: "Previous"
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
        text: "Stop"
        icon.name: "media-playback-stop"
        onTriggered: uiBridge.stop()
    }
    Action {
        id: nextAction
        text: "Next"
        icon.name: "media-skip-forward"
        shortcut: "Ctrl+Right"
        onTriggered: uiBridge.next()
    }
    Action {
        id: clearPlaylistAction
        text: "Clear Playlist"
        onTriggered: uiBridge.clearQueue()
    }
    Action {
        id: moveTrackUpAction
        text: "Move Track Up"
        shortcut: "Ctrl+Shift+Up"
        onTriggered: queueController.moveSelected(-1)
    }
    Action {
        id: moveTrackDownAction
        text: "Move Track Down"
        shortcut: "Ctrl+Shift+Down"
        onTriggered: queueController.moveSelected(1)
    }

    Shortcut {
        sequence: "Space"
        enabled: !(libraryViewRef && libraryViewRef.activeFocus)
            && !globalSearchController.dialogHasActiveInputFocus
        onActivated: playbackController.togglePlayPause()
    }
    menuBar: MenuBar {
        Menu {
            title: "File"
            width: root.menuPopupWidth([
                { label: openFilesAction.text, shortcut: String(openFilesAction.shortcut) },
                { label: addFilesAction.text, shortcut: "" },
                { label: addFolderAction.text, shortcut: "" },
                { label: playLibrarySelectionAction.text, shortcut: "" },
                { label: appendLibrarySelectionAction.text, shortcut: "" },
                { label: playAllLibraryTracksAction.text, shortcut: "" },
                { label: appendAllLibraryTracksAction.text, shortcut: "" },
                { label: quitAction.text, shortcut: String(quitAction.shortcut) }
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
            MenuSeparator {}
            MenuItem { action: playLibrarySelectionAction }
            MenuItem { action: appendLibrarySelectionAction }
            MenuItem { action: playAllLibraryTracksAction }
            MenuItem { action: appendAllLibraryTracksAction }
            MenuSeparator {}
            MenuItem { action: quitAction }
        }
        Menu {
            title: "Edit"
            width: root.menuPopupWidth([
                { label: removeSelectedTrackAction.text, shortcut: String(removeSelectedTrackAction.shortcut) },
                { label: moveTrackUpAction.text, shortcut: String(moveTrackUpAction.shortcut) },
                { label: moveTrackDownAction.text, shortcut: String(moveTrackDownAction.shortcut) },
                { label: selectPreviousTrackAction.text, shortcut: String(selectPreviousTrackAction.shortcut) },
                { label: selectNextTrackAction.text, shortcut: String(selectNextTrackAction.shortcut) },
                { label: clearPlaylistAction.text, shortcut: "" },
                { label: preferencesAction.text, shortcut: String(preferencesAction.shortcut) }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
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
            title: "View"
            width: root.menuPopupWidth([
                { label: globalSearchAction.text, shortcut: String(globalSearchAction.shortcut) },
                { label: autoCenterSelectionAction.text, shortcut: "" },
                { label: resetSpectrogramAction.text, shortcut: "" },
                { label: showFpsOverlayAction.text, shortcut: "" }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: globalSearchAction }
            MenuItem { action: autoCenterSelectionAction }
            MenuItem { action: resetSpectrogramAction }
            MenuItem { action: showFpsOverlayAction }
        }
        Menu {
            title: "Playback"
            width: root.menuPopupWidth([
                { label: previousAction.text, shortcut: String(previousAction.shortcut) },
                { label: playAction.text, shortcut: String(playAction.shortcut) },
                { label: pauseAction.text, shortcut: String(pauseAction.shortcut) },
                { label: stopAction.text, shortcut: String(stopAction.shortcut) },
                { label: nextAction.text, shortcut: String(nextAction.shortcut) },
                { label: shuffleAction.text, shortcut: "" },
                { label: repeatOffAction.text, shortcut: "" },
                { label: repeatOneAction.text, shortcut: "" },
                { label: repeatAllAction.text, shortcut: "" },
                { label: clearPlaylistAction.text, shortcut: "" }
            ])
            enter: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            exit: Transition {
                NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
            }
            MenuItem { action: previousAction }
            MenuItem { action: playAction }
            MenuItem { action: pauseAction }
            MenuItem { action: stopAction }
            MenuItem { action: nextAction }
            MenuSeparator {}
            MenuItem { action: shuffleAction }
            MenuItem { action: repeatOffAction }
            MenuItem { action: repeatOneAction }
            MenuItem { action: repeatAllAction }
            MenuSeparator {}
            MenuItem { action: clearPlaylistAction }
        }
        Menu {
            title: "Help"
            width: root.menuPopupWidth([
                { label: diagnosticsAction.text, shortcut: "" },
                { label: aboutAction.text, shortcut: "" }
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
        globalSearchTrackNumberColumnWidth: root.globalSearchTrackNumberColumnWidth
        globalSearchCoverColumnWidth: root.globalSearchCoverColumnWidth
        globalSearchArtistColumnWidth: root.globalSearchArtistColumnWidth
        globalSearchAlbumColumnWidth: root.globalSearchAlbumColumnWidth
        globalSearchRootColumnWidth: root.globalSearchRootColumnWidth
        globalSearchYearColumnWidth: root.globalSearchYearColumnWidth
        globalSearchTrackGenreColumnWidth: root.globalSearchTrackGenreColumnWidth
        globalSearchAlbumCountColumnWidth: root.globalSearchAlbumCountColumnWidth
        globalSearchTrackLengthColumnWidth: root.globalSearchTrackLengthColumnWidth
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
        title: pendingFolderDialogContext === "append-external-folder"
            ? "Add Folder"
            : "Select Music Folder to Scan"
        onAccepted: {
            const localPath = PathUtils.folderDialogPath(scanFolderDialog)
            if (localPath.length > 0) {
                if (pendingFolderDialogContext === "append-external-folder") {
                    root.submitExternalImport([localPath], false)
                } else {
                    root.openLibraryRootNameDialog("add", localPath, "")
                }
            }
            pendingFolderDialogContext = ""
        }
        onRejected: pendingFolderDialogContext = ""
    }

    footer: Panes.StatusBar {
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        channelStatusIconSource: root.channelStatusIconSource
        themeIsDark: root.themeIsDark
        transientError: root.transientBridgeError
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Panes.TransportBar {
            id: transportBar
            Layout.fillWidth: true
            uiBridge: root.uiBridge
            uiPalette: root.uiPalette
            previousAction: previousAction
            playAction: playAction
            pauseAction: pauseAction
            stopAction: stopAction
            nextAction: nextAction
            themeIsDark: root.themeIsDark
            volumeMuted: playbackController.volumeMuted
            displayedPositionSeconds: playbackController.displayedPositionSeconds
            toggleMutedVolume: playbackController.toggleMutedVolume
            setAppVolume: playbackController.setAppVolume
            normalizedVolumeValue: playbackController.normalizedVolumeValue
            seekCommitted: playbackController.seekCommitted
        }

        SplitView {
            id: mainSplit
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            Panes.SidebarPane {
                controller: libraryController
                uiBridge: root.uiBridge
                libraryModel: root.libraryTreeModel
                uiPalette: root.uiPalette
                splitPreferredWidth: Math.max(300, root.width * 0.26)
                replaceFromItunesAction: replaceFromItunesAction
                currentTrackItunesArtworkDisabledReason: viewerController.currentTrackItunesArtworkDisabledReason
                openAlbumArtViewer: viewerController.openAlbumArtViewer
                popupTransitionMs: root.uiPopupTransitionMs
                snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                stepScrollView: root.stepScrollView
                playAllLibraryTracksAction: playAllLibraryTracksAction
                appendAllLibraryTracksAction: appendAllLibraryTracksAction
            }

            SplitView {
                orientation: Qt.Vertical
                SplitView.fillWidth: true

                Panes.QueuePane {
                    controller: queueController
                    uiBridge: root.uiBridge
                    uiPalette: root.uiPalette
                    preferredHeight: root.height * 0.58
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
                    submitExternalImport: root.submitExternalImport
                }

                Panes.SpectrogramPane {
                    id: spectrogramPane
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 220
                    openViewer: viewerController.openSpectrogramViewer
                }
            }
        }
    }

    Viewers.SpectrogramSurface {
        id: spectrogramSurface
        parent: viewerController.spectrogramViewerOpen
            ? (root.useWholeScreenViewerMode
                ? spectrogramViewerShell.windowHost
                : spectrogramViewerShell.popupHost)
            : spectrogramPane.hostItem
        visible: parent !== null
        anchors.fill: parent
        uiBridge: root.uiBridge
        positionSeconds: playbackController.spectrogramPositionSeconds
    }

    Viewers.SpectrogramViewerShell {
        id: spectrogramViewerShell
        windowRoot: root
        viewerOpen: viewerController.spectrogramViewerOpen
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        popupTransitionMs: root.uiPopupTransitionMs
        titleText: root.title
        closeViewer: viewerController.closeSpectrogramViewer
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

    onClosing: function(close) { uiBridge.shutdown() }

    Connections {
        target: uiBridge
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
        function onGlobalSearchResultsChanged() {
            globalSearchController.syncSelectionAfterResultsChange()
        }
        function onBridgeError(message) {
            if (message.indexOf("[analysis]") !== -1
                    || message.indexOf("[gst]") !== -1
                    || message.indexOf("[bridge]") !== -1
                    || message.indexOf("[bridge-json]") !== -1) {
                return
            }
            root.transientBridgeError = message
            bridgeErrorTimer.restart()
            console.warn("bridge error:", message)
        }
    }

    Connections {
        target: libraryModel
        function onTreeApplied() {
            libraryController.finishPendingTreeApply()
            libraryController.applyPendingReveal()
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
    }

    Component.onCompleted: {
        libraryController.requestTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        playbackController.initializeFromBridge()
        queueController.initializeFromBridge()
        libraryController.syncSelectionToVisibleRows()
        globalSearchController.syncSelectionAfterResultsChange()
    }
}
