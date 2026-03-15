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
        const context = root.windowTitleContext()
        return context.length > 0
            ? context + " \u2014 " + root.appDisplayName
            : root.appDisplayName
    }
    property real displayedPositionSeconds: 0
    property bool positionSmoothingPrimed: false
    property real positionSmoothingAnchorSeconds: 0
    property int positionSmoothingAnimationMs: 0
    property real positionSmoothingLastMs: 0
    property string positionSmoothingTrackPath: ""
    property string stoppedSpectrogramTrackPath: ""
    property string lastSpectrogramPlaybackState: ""
    property int albumArtViewResetToken: 0
    property bool albumArtViewerOpen: false
    property bool albumArtInfoVisible: false
    property var albumArtViewerFileInfo: ({})
    property string albumArtViewerInfoSource: ""
    property string albumArtViewerSource: ""
    property bool albumArtViewerShowsCurrentTrack: true
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
    property bool spectrogramViewerOpen: false
    property string pendingFolderDialogContext: ""
    property string pendingFileDialogContext: ""
    property string pendingLibraryRootDialogMode: ""
    property string pendingLibraryRootPath: ""
    property string pendingLibraryRootName: ""
    property string transientBridgeError: ""
    property real rememberedVolumeBeforeMute: 1.0
    property bool volumeMuted: false
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
    }

    Controllers.LibraryController {
        id: libraryController
        uiBridge: root.uiBridge
        libraryModel: root.libraryTreeModel
        tryCaptureGlobalSearchPrefill: globalSearchController.tryCapturePrefill
        rowsForAction: root.rowsForLibraryAction
        playRows: root.playLibraryRows
    }

    readonly property string selectedLibrarySelectionKey: libraryController.selectedSelectionKey
    readonly property int selectedLibrarySourceIndex: libraryController.selectedSourceIndex
    readonly property string selectedLibraryRowType: libraryController.selectedRowType
    readonly property string selectedLibraryArtist: libraryController.selectedArtist
    readonly property string selectedLibraryAlbum: libraryController.selectedAlbum
    readonly property string selectedLibraryTrackPath: libraryController.selectedTrackPath
    readonly property string selectedLibraryOpenPath: libraryController.selectedOpenPath
    readonly property var selectedLibraryPlayPaths: libraryController.selectedPlayPaths
    readonly property var selectedLibrarySelectionKeys: libraryController.selectedSelectionKeys
    readonly property var libraryViewRef: libraryController.view

    function shouldResetSpectrogramForStoppedTrackSwitch(previousPlaybackState, currentPlaybackState, stoppedTrackPath, currentTrackPath) {
        const previousState = previousPlaybackState || ""
        const currentState = currentPlaybackState || ""
        const stoppedPath = stoppedTrackPath || ""
        const currentPath = currentTrackPath || ""
        return currentState === "Playing"
            && previousState === "Stopped"
            && stoppedPath.length > 0
            && stoppedPath !== currentPath
    }

    function mixColor(colorA, colorB, amount) {
        const t = Math.max(0, Math.min(1, amount))
        return Qt.rgba(
            (colorA.r * (1 - t)) + (colorB.r * t),
            (colorA.g * (1 - t)) + (colorB.g * t),
            (colorA.b * (1 - t)) + (colorB.b * t),
            (colorA.a * (1 - t)) + (colorB.a * t))
    }

    function colorLuma(colorValue) {
        return (0.2126 * colorValue.r) + (0.7152 * colorValue.g) + (0.0722 * colorValue.b)
    }

    function basenameFromPath(pathValue) {
        const normalized = (pathValue || "").trim().replace(/\\/g, "/")
        if (normalized.length === 0) {
            return ""
        }
        const parts = normalized.split("/")
        return parts.length > 0 ? parts[parts.length - 1] : normalized
    }

    function windowTitleContext() {
        const playbackState = (uiBridge.playbackState || "").trim()
        if (playbackState === "Stopped") {
            return ""
        }
        const explicitTitle = (uiBridge.currentTrackTitle || "").trim()
        if (explicitTitle.length > 0) {
            return explicitTitle
        }
        const trackPath = (uiBridge.currentTrackPath || "").trim()
        if (trackPath.length > 0) {
            return root.basenameFromPath(trackPath)
        }
        return ""
    }

    function normalizedVolumeValue(value) {
        const numericValue = Number(value)
        if (!isFinite(numericValue)) {
            return 0.0
        }
        return Math.max(0.0, Math.min(1.0, numericValue))
    }

    function syncMutedVolumeState() {
        const currentVolume = normalizedVolumeValue(uiBridge.volume)
        if (currentVolume > 0.0001) {
            root.rememberedVolumeBeforeMute = currentVolume
            root.volumeMuted = false
        } else if (!root.volumeMuted && root.rememberedVolumeBeforeMute <= 0.0001) {
            root.rememberedVolumeBeforeMute = 1.0
        }
    }

    function setAppVolume(value) {
        const nextVolume = normalizedVolumeValue(value)
        if (nextVolume > 0.0001) {
            root.rememberedVolumeBeforeMute = nextVolume
            root.volumeMuted = false
        } else if (!root.volumeMuted) {
            const currentVolume = normalizedVolumeValue(uiBridge.volume)
            if (currentVolume > 0.0001) {
                root.rememberedVolumeBeforeMute = currentVolume
            }
        }
        uiBridge.setVolume(nextVolume)
    }

    function toggleMutedVolume() {
        const currentVolume = normalizedVolumeValue(uiBridge.volume)
        if (root.volumeMuted || currentVolume <= 0.0001) {
            const restoreVolume = root.rememberedVolumeBeforeMute > 0.0001
                ? root.rememberedVolumeBeforeMute
                : 1.0
            root.volumeMuted = false
            uiBridge.setVolume(restoreVolume)
            return
        }

        root.rememberedVolumeBeforeMute = currentVolume
        root.volumeMuted = true
        uiBridge.setVolume(0.0)
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
        property bool spectrogramReset: false
        property real dbRange: 90
        property int fftSize: 8192
        property int spectrogramViewMode: 0
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
        signal snapshotChanged()
        signal analysisChanged()
        signal libraryTreeFrameReceived(int version, var treeBytes)
        signal globalSearchResultsChanged()
        signal itunesArtworkChanged()
        signal diagnosticsChanged()
        signal bridgeError(string message)
        function play() {}
        function pause() {}
        function stop() {}
        function next() {}
        function previous() {}
        function seek(seconds) {}
        function setVolume(value) {}
        function setFftSize(value) {}
        function setSpectrogramViewMode(value) {}
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
        function imageFileDetails(path) { return ({}) }
        function scanRoot(path) {}
        function scanDefaultMusicRoot() {}
        function requestSnapshot() {}
        function shutdown() {}
        function clearDiagnostics() {}
        function reloadDiagnosticsFromDisk() {}
        function takeSpectrogramRowsDeltaPacked() { return ({ channels: [] }) }
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

    function togglePlayPause() {
        if (uiBridge.playbackState === "Playing") {
            uiBridge.pause()
        } else {
            uiBridge.play()
        }
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

    function formatSeekTime(seconds) {
        if (!isFinite(seconds) || seconds < 0) {
            return "00:00"
        }
        const totalSeconds = Math.floor(seconds)
        const hours = Math.floor(totalSeconds / 3600)
        const minutes = Math.floor((totalSeconds % 3600) / 60)
        const secs = totalSeconds % 60
        if (hours > 0) {
            return hours.toString()
                + ":" + minutes.toString().padStart(2, "0")
                + ":" + secs.toString().padStart(2, "0")
        }
        return minutes.toString().padStart(2, "0")
            + ":" + secs.toString().padStart(2, "0")
    }

    function metadataTrackNumberText(trackNumber) {
        if (trackNumber === undefined || trackNumber === null) {
            return "--"
        }
        const value = Number(trackNumber)
        if (!isFinite(value) || value <= 0) {
            return "--"
        }
        return Math.floor(value).toString().padStart(2, "0")
    }

    function playlistOrderText(index) {
        if (index === undefined || index === null || index < 0) {
            return "--"
        }
        return String(index + 1)
    }

    readonly property int playlistOrderColumnWidth: {
        const maxIndex = Math.max(0, uiBridge.queueLength - 1)
        const widestOrderText = playlistOrderText(maxIndex)
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

    function queueTrackNumberText(index) {
        if (index === undefined || index === null || index < 0) {
            return "--"
        }
        return metadataTrackNumberText(uiBridge.queueTrackNumberAt(index))
    }

    FontMetrics {
        id: menuFontMetrics
        font: root.font
    }

    FontMetrics {
        id: playlistOrderFontMetrics
        font: root.font
    }

    Behavior on displayedPositionSeconds {
        enabled: root.positionSmoothingAnimationMs > 0
            && !(transportBar && transportBar.seekPressed)
            && root.visualFeedsEnabled
        NumberAnimation {
            duration: root.positionSmoothingAnimationMs
            easing.type: Easing.Linear
        }
    }

    Timer {
        id: bridgeErrorTimer
        interval: 10000
        repeat: false
        onTriggered: root.transientBridgeError = ""
    }

    function moveSelected(delta) {
        const from = uiBridge.selectedQueueIndex
        if (from < 0 || uiBridge.queueLength <= 0) {
            return
        }
        const to = Math.max(0, Math.min(uiBridge.queueLength - 1, from + delta))
        if (to !== from) {
            uiBridge.moveQueue(from, to)
        }
    }

    function isActionableLibraryRow(rowMap) {
        if (!rowMap) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            return (rowMap.trackPath || "").length > 0
        }
        if (rowType === "album") {
            return (rowMap.artist || "").length > 0 && (rowMap.name || "").length > 0
        }
        if (rowType === "artist") {
            return (rowMap.artist || "").length > 0
        }
        const paths = rowMap.playPaths || []
        return paths.length > 0
    }

    function appendLibraryRow(rowMap) {
        if (!isActionableLibraryRow(rowMap)) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            uiBridge.appendTrack(rowMap.trackPath || "")
            return true
        }
        if (rowType === "album") {
            const albumPaths = rowMap.playPaths || []
            if (albumPaths.length > 0) {
                uiBridge.appendPaths(albumPaths)
            } else {
                uiBridge.appendAlbumByKey(
                    rowMap.artist || "",
                    rowMap.selectionKey || rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            uiBridge.appendArtistByName(rowMap.selectionKey || rowMap.artist || "")
            return true
        }
        uiBridge.appendPaths(rowMap.playPaths || [])
        return true
    }

    function replaceWithLibraryRow(rowMap) {
        if (!isActionableLibraryRow(rowMap)) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            uiBridge.playTrack(rowMap.trackPath || "")
            return true
        }
        if (rowType === "album") {
            const albumPaths = rowMap.playPaths || []
            if (albumPaths.length > 0) {
                uiBridge.replaceWithPaths(albumPaths)
            } else {
                uiBridge.replaceAlbumByKey(
                    rowMap.artist || "",
                    rowMap.selectionKey || rowMap.name || "")
            }
            return true
        }
        if (rowType === "artist") {
            uiBridge.replaceArtistByName(rowMap.selectionKey || rowMap.artist || "")
            return true
        }
        uiBridge.replaceWithPaths(rowMap.playPaths || [])
        return true
    }

    function selectedLibraryRowsSorted() {
        const rows = []
        for (let i = 0; i < selectedLibrarySelectionKeys.length; ++i) {
            const key = selectedLibrarySelectionKeys[i] || ""
            if (key.length === 0) {
                continue
            }
            const rowIndex = libraryModel.indexForSelectionKey(key)
            if (rowIndex < 0) {
                continue
            }
            const rowMap = libraryModel.rowDataForRow(rowIndex)
            if (isActionableLibraryRow(rowMap)) {
                rows.push({ index: rowIndex, row: rowMap })
            }
        }
        rows.sort(function(a, b) { return a.index - b.index })
        return rows
    }

    function rowsForLibraryAction(rowMap) {
        if (rowMap
                && rowMap.selectionKey
                && isLibrarySelectionKeySelected(rowMap.selectionKey)
                && selectedLibrarySelectionKeys.length > 1) {
            const selectedRows = selectedLibraryRowsSorted()
            if (selectedRows.length > 0) {
                return selectedRows.map(function(entry) { return entry.row })
            }
        }
        return rowMap ? [rowMap] : []
    }

    function canPlayLibrarySelection() {
        if (selectedLibrarySelectionKeys.length > 0) {
            return selectedLibraryRowsSorted().length > 0
        }
        return isActionableLibraryRow({
            rowType: selectedLibraryRowType,
            artist: selectedLibraryArtist,
            name: selectedLibraryAlbum,
            sourceIndex: selectedLibrarySourceIndex,
            trackPath: selectedLibraryTrackPath,
            playPaths: selectedLibraryPlayPaths
        })
    }

    function playLibraryRows(rows) {
        if (!rows || rows.length === 0) {
            return
        }
        if (!replaceWithLibraryRow(rows[0])) {
            return
        }
        for (let i = 1; i < rows.length; ++i) {
            appendLibraryRow(rows[i])
        }
    }

    function appendLibraryRows(rows) {
        if (!rows || rows.length === 0) {
            return
        }
        for (let i = 0; i < rows.length; ++i) {
            appendLibraryRow(rows[i])
        }
    }

    function playLibrarySelection() {
        const rows = selectedLibraryRowsSorted().map(function(entry) { return entry.row })
        if (rows.length > 0) {
            playLibraryRows(rows)
            return
        }
        playLibraryRows([{
            rowType: selectedLibraryRowType,
            artist: selectedLibraryArtist,
            name: selectedLibraryAlbum,
            sourceIndex: selectedLibrarySourceIndex,
            trackPath: selectedLibraryTrackPath,
            playPaths: selectedLibraryPlayPaths
        }])
    }

    function appendLibrarySelection() {
        const rows = selectedLibraryRowsSorted().map(function(entry) { return entry.row })
        if (rows.length > 0) {
            appendLibraryRows(rows)
            return
        }
        appendLibraryRows([{
            rowType: selectedLibraryRowType,
            artist: selectedLibraryArtist,
            name: selectedLibraryAlbum,
            sourceIndex: selectedLibrarySourceIndex,
            trackPath: selectedLibraryTrackPath,
            playPaths: selectedLibraryPlayPaths
        }])
    }

    function openTagEditorForPlaylistRow(rowIndex) {
        if (rowIndex < 0) {
            return
        }
        let indices = [rowIndex]
        if (queueController.isIndexSelected(rowIndex) && queueController.selectedIndices.length > 1) {
            indices = queueController.selectedIndices.slice().sort(function(a, b) { return a - b })
        }
        const selections = []
        for (let i = 0; i < indices.length; ++i) {
            const path = uiBridge.queuePathAt(indices[i])
            if (path && path.length > 0) {
                selections.push({ path: path })
            }
        }
        if (selections.length > 0 && tagEditorApi.openSelection(selections)) {
            tagEditorDialog.open()
        }
    }

    function canOpenTagEditorForLibrary(rowMap) {
        if (!rowMap) {
            return false
        }
        const rowType = rowMap.rowType || ""
        if (rowType === "track") {
            return (rowMap.trackPath || "").length > 0
        }
        if (rowType === "album" || rowType === "section") {
            return (rowMap.selectionKey || "").length > 0
        }
        return false
    }

    function openTagEditorForLibrary(rowMap) {
        const rows = root.rowsForLibraryAction(rowMap)
        const selections = []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!root.canOpenTagEditorForLibrary(row)) {
                continue
            }
            selections.push({
                path: row.trackPath || "",
                rowType: row.rowType || "",
                key: row.selectionKey || "",
                artist: row.artist || "",
                name: row.name || "",
                trackPath: row.trackPath || ""
            })
        }
        if (selections.length > 0 && tagEditorApi.openSelection(selections)) {
            tagEditorDialog.open()
        }
    }

    function canPlayAllLibraryTracks() {
        return uiBridge.libraryTrackCount > 0
    }

    function playAllLibraryTracks() {
        if (!canPlayAllLibraryTracks()) {
            return
        }
        uiBridge.replaceAllLibraryTracks()
    }

    function appendAllLibraryTracks() {
        if (!canPlayAllLibraryTracks()) {
            return
        }
        uiBridge.appendAllLibraryTracks()
    }

    function repeatModeText(mode) {
        if (mode === 1) {
            return "repeat-one"
        }
        if (mode === 2) {
            return "repeat-all"
        }
        return "repeat-off"
    }

    function librarySelectionStatusText() {
        if (selectedLibraryRowType === "root" && selectedLibraryAlbum.length > 0) {
            return "root: " + selectedLibraryAlbum
        }
        if (selectedLibraryRowType === "artist" && selectedLibraryArtist.length > 0) {
            return "artist: " + selectedLibraryArtist
        }
        if (selectedLibraryRowType === "album" && selectedLibraryAlbum.length > 0) {
            return "album: " + selectedLibraryAlbum
        }
        if (selectedLibraryRowType === "section" && selectedLibraryAlbum.length > 0) {
            return "section: " + selectedLibraryAlbum
        }
        if (selectedLibraryRowType === "track" && selectedLibraryTrackPath.length > 0) {
            const parts = selectedLibraryTrackPath.split("/")
            return "track: " + parts[parts.length - 1]
        }
        return "none"
    }

    function librarySelectionCount() {
        return selectedLibrarySelectionKeys.length
    }

    function isLibrarySelectionKeySelected(key) {
        return libraryController.isSelectionKeySelected(key)
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

    function formatSampleRateText(sampleRateHz) {
        const rate = Number(sampleRateHz)
        if (!isFinite(rate) || rate <= 0) {
            return ""
        }
        const khz = rate / 1000.0
        const roundedTenth = Math.round(khz * 10) / 10
        const wholeKhz = Math.round(roundedTenth)
        const valueText = Math.abs(roundedTenth - wholeKhz) < 0.05
            ? wholeKhz.toString()
            : roundedTenth.toFixed(1)
        return valueText + " kHz"
    }

    function formatBitDepthSampleRateText(bitDepth, sampleRateHz) {
        const bitValue = Number(bitDepth)
        const sampleRateText = formatSampleRateText(sampleRateHz)
        const bitText = isFinite(bitValue) && bitValue > 0
            ? Math.floor(bitValue).toString() + " bit"
            : ""
        if (bitText.length > 0 && sampleRateText.length > 0) {
            return bitText + "/" + sampleRateText
        }
        if (bitText.length > 0) {
            return bitText
        }
        return sampleRateText
    }

    function playlistStatusSummary() {
        const count = Math.max(0, Number(uiBridge.queueLength) || 0)
        const noun = count === 1 ? "track" : "tracks"
        return count.toString() + " " + noun + " (" + (uiBridge.queueDurationText || "00:00") + ")"
    }

    function statusBarSections() {
        if (root.transientBridgeError.length > 0) {
            return [{
                text: "Error: " + root.transientBridgeError,
                emphasis: true,
                kind: "error"
            }]
        }
        if (!uiBridge.connected) {
            return [{ text: "Bridge disconnected" }]
        }

        const sections = [
            { text: uiBridge.playbackState || "Stopped" },
            { text: (uiBridge.positionText || "00:00") + "/" + (uiBridge.durationText || "00:00") }
        ]

        const formatLabel = (uiBridge.currentTrackFormatLabel || "").trim()
        if (formatLabel.length > 0) {
            sections.push({ text: formatLabel })
        }

        const channelText = (uiBridge.currentTrackChannelLayoutText || "").trim()
        if (channelText.length > 0) {
            sections.push({
                text: channelText,
                iconKey: (uiBridge.currentTrackChannelLayoutIconKey || "").trim()
            })
        }

        const bitDepthSampleRateText = formatBitDepthSampleRateText(
            uiBridge.currentTrackBitDepth,
            uiBridge.currentTrackSampleRateHz)
        if (bitDepthSampleRateText.length > 0) {
            sections.push({ text: bitDepthSampleRateText })
        }

        const bitrateKbps = Number(uiBridge.currentTrackCurrentBitrateKbps)
        if (isFinite(bitrateKbps) && bitrateKbps > 0) {
            sections.push({ text: Math.round(bitrateKbps).toString() + " kbps" })
        }

        sections.push({ text: playlistStatusSummary(), stretch: true })
        return sections
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

    function channelStatusIconCells(iconKey) {
        switch (iconKey) {
        case "mono":
            return [{ x: 7, y: 2, w: 4, h: 3 }]
        case "stereo":
            return [{ x: 2, y: 2, w: 3, h: 3 }, { x: 13, y: 2, w: 3, h: 3 }]
        case "4.0":
            return [
                { x: 2, y: 2, w: 3, h: 3 },
                { x: 13, y: 2, w: 3, h: 3 },
                { x: 2, y: 11, w: 3, h: 3 },
                { x: 13, y: 11, w: 3, h: 3 }
            ]
        case "5.0":
            return [
                { x: 2, y: 2, w: 3, h: 3 },
                { x: 7, y: 2, w: 4, h: 3 },
                { x: 13, y: 2, w: 3, h: 3 },
                { x: 2, y: 11, w: 3, h: 3 },
                { x: 13, y: 11, w: 3, h: 3 }
            ]
        case "5.1":
            return [
                { x: 2, y: 2, w: 3, h: 3 },
                { x: 7, y: 2, w: 4, h: 3 },
                { x: 13, y: 2, w: 3, h: 3 },
                { x: 2, y: 11, w: 3, h: 3 },
                { x: 13, y: 11, w: 3, h: 3 },
                { x: 8, y: 7, w: 2, h: 2, lfe: true }
            ]
        case "6.1":
            return [
                { x: 2, y: 2, w: 3, h: 3 },
                { x: 7, y: 2, w: 4, h: 3 },
                { x: 13, y: 2, w: 3, h: 3 },
                { x: 1, y: 7, w: 3, h: 3 },
                { x: 14, y: 7, w: 3, h: 3 },
                { x: 7, y: 11, w: 4, h: 3 },
                { x: 8, y: 7, w: 2, h: 2, lfe: true }
            ]
        case "7.1":
            return [
                { x: 2, y: 2, w: 3, h: 3 },
                { x: 7, y: 2, w: 4, h: 3 },
                { x: 13, y: 2, w: 3, h: 3 },
                { x: 1, y: 7, w: 3, h: 3 },
                { x: 14, y: 7, w: 3, h: 3 },
                { x: 3, y: 11, w: 3, h: 3 },
                { x: 12, y: 11, w: 3, h: 3 },
                { x: 8, y: 7, w: 2, h: 2, lfe: true }
            ]
        default:
            return []
        }
    }

    function openDiagnostics() {
        diagnosticsDialog.open()
    }

    function urlToLocalPath(urlValue) {
        if (urlValue === undefined || urlValue === null) {
            return ""
        }
        let value = ""
        if (typeof urlValue === "string") {
            value = urlValue
        } else if (urlValue.toString) {
            value = urlValue.toString()
        } else {
            value = String(urlValue)
        }
        if (value.length === 0 || value === "undefined" || value === "null") {
            return ""
        }
        if (value.startsWith("QUrl(\"") && value.endsWith("\")")) {
            value = value.substring(6, value.length - 2)
        }
        if (value.startsWith("file://")) {
            return decodeURIComponent(value.substring(7))
        }
        return value
    }

    function pathFromAnyUrl(urlValue) {
        const localPath = root.urlToLocalPath(urlValue)
        const queryIndex = localPath.indexOf("?")
        const fragmentIndex = localPath.indexOf("#")
        let endIndex = localPath.length
        if (queryIndex >= 0) {
            endIndex = Math.min(endIndex, queryIndex)
        }
        if (fragmentIndex >= 0) {
            endIndex = Math.min(endIndex, fragmentIndex)
        }
        return endIndex < localPath.length ? localPath.substring(0, endIndex) : localPath
    }

    function folderDialogPath(dialogObj) {
        if (!dialogObj) {
            return ""
        }
        const candidates = [dialogObj.folder, dialogObj.selectedFolder, dialogObj.currentFolder]
        for (let i = 0; i < candidates.length; ++i) {
            const path = root.urlToLocalPath(candidates[i])
            if (path.length > 0) {
                return path
            }
        }
        return ""
    }

    function fileDialogPaths(dialogObj) {
        if (!dialogObj) {
            return []
        }
        const candidates = [
            dialogObj.files,
            dialogObj.selectedFiles,
            dialogObj.currentFiles,
            dialogObj.file,
            dialogObj.selectedFile,
            dialogObj.currentFile
        ]
        const paths = []
        for (let i = 0; i < candidates.length; ++i) {
            const candidate = candidates[i]
            if (candidate === undefined || candidate === null) {
                continue
            }
            if (candidate.length !== undefined && typeof candidate !== "string") {
                for (let j = 0; j < candidate.length; ++j) {
                    const path = root.urlToLocalPath(candidate[j])
                    if (path.length > 0) {
                        paths.push(path)
                    }
                }
                if (paths.length > 0) {
                    return paths
                }
                continue
            }
            const path = root.urlToLocalPath(candidate)
            if (path.length > 0) {
                paths.push(path)
            }
        }
        return paths
    }

    function droppedExternalPaths(drop) {
        const paths = []
        if (drop && drop.hasUrls && drop.urls) {
            for (let i = 0; i < drop.urls.length; ++i) {
                const path = root.urlToLocalPath(drop.urls[i])
                if (path.length > 0) {
                    paths.push(path)
                }
            }
        }
        if (paths.length > 0) {
            return paths
        }
        if (drop && drop.hasText && (drop.text || "").length > 0) {
            const lines = (drop.text || "").split(/\r?\n/)
            for (let i = 0; i < lines.length; ++i) {
                const path = root.urlToLocalPath(lines[i])
                if (path.length > 0) {
                    paths.push(path)
                }
            }
        }
        return paths
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

    function syncAlbumArtViewerPresentation() {
        albumArtViewerShell.syncPresentation()
    }

    function syncSpectrogramViewerPresentation() {
        spectrogramViewerShell.syncPresentation()
    }

    function closeAlbumArtViewer() {
        albumArtViewerOpen = false
    }

    function closeSpectrogramViewer() {
        spectrogramViewerOpen = false
    }

    function refreshAlbumArtFileInfo() {
        const infoSource = root.albumArtViewerInfoSource || ""
        albumArtViewerFileInfo = infoSource.length > 0
            ? (uiBridge.imageFileDetails(infoSource) || ({}))
            : ({})
    }

    function albumArtInfoOverlayText() {
        const info = root.albumArtViewerFileInfo || ({})
        const lines = []

        if ((info.fileName || "").length > 0) {
            lines.push("File: " + info.fileName)
        }
        if ((info.resolutionText || "").length > 0) {
            lines.push("Resolution: " + info.resolutionText)
        }
        if ((info.fileSizeText || "").length > 0) {
            lines.push("Size: " + info.fileSizeText)
        }
        if ((info.fileType || "").length > 0) {
            lines.push("Type: " + info.fileType)
        }
        if ((info.mimeType || "").length > 0) {
            lines.push("MIME: " + info.mimeType)
        }
        if ((info.path || "").length > 0) {
            lines.push("Path: " + info.path)
        }

        return lines.join("\n")
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

    function openAlbumArtViewer() {
        if (!uiBridge.currentTrackCoverPath || uiBridge.currentTrackCoverPath.length === 0) {
            return
        }
        albumArtViewerSource = uiBridge.currentTrackCoverPath || ""
        albumArtViewerInfoSource = root.pathFromAnyUrl(albumArtViewerSource)
        albumArtViewerShowsCurrentTrack = true
        albumArtInfoVisible = false
        albumArtViewResetToken += 1
        albumArtViewerFileInfo = ({})
        albumArtViewerOpen = true
    }

    function openAlbumArtViewerForSuggestion(rowMap) {
        const previewSource = (rowMap && (rowMap.normalizedUrl || rowMap.previewSource || "")) || ""
        if (previewSource.length === 0) {
            return
        }
        albumArtViewerSource = previewSource
        albumArtViewerInfoSource = (rowMap && (rowMap.normalizedPath || "")) || root.pathFromAnyUrl(previewSource)
        albumArtViewerShowsCurrentTrack = false
        albumArtInfoVisible = true
        albumArtViewResetToken += 1
        refreshAlbumArtFileInfo()
        albumArtViewerOpen = true
    }

    function toggleAlbumArtInfoVisible() {
        if (!root.albumArtViewerOpen) {
            return
        }
        if (!root.albumArtInfoVisible) {
            refreshAlbumArtFileInfo()
        }
        root.albumArtInfoVisible = !root.albumArtInfoVisible
        albumArtViewerShell.focusFullscreen()
    }

    function currentTrackItunesArtworkDisabledReason() {
        if ((uiBridge.currentTrackPath || "").trim().length === 0) {
            return "No active track."
        }
        if ((uiBridge.currentTrackAlbum || "").trim().length === 0) {
            return "Album metadata is missing."
        }
        if ((uiBridge.currentTrackArtist || "").trim().length === 0) {
            return "Artist metadata is missing."
        }
        return ""
    }

    function openItunesArtworkDialog() {
        itunesArtworkDialog.parent = root.albumArtViewerOpen && root.useWholeScreenViewerMode
            ? albumArtViewerShell.windowHost
            : Overlay.overlay
        uiBridge.searchCurrentTrackArtworkSuggestions()
        itunesArtworkDialog.open()
    }

    function openSpectrogramViewer() {
        spectrogramViewerOpen = true
    }

    onAlbumArtViewerOpenChanged: syncAlbumArtViewerPresentation()
    onSpectrogramViewerOpenChanged: syncSpectrogramViewerPresentation()
    onUseWholeScreenViewerModeChanged: {
        if (albumArtViewerOpen) {
            syncAlbumArtViewerPresentation()
        }
        if (spectrogramViewerOpen) {
            syncSpectrogramViewerPresentation()
        }
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
        enabled: root.canPlayLibrarySelection()
        onTriggered: root.playLibrarySelection()
    }
    Action {
        id: appendLibrarySelectionAction
        text: "Queue Library Selection"
        enabled: root.canPlayLibrarySelection()
        onTriggered: root.appendLibrarySelection()
    }
    Action {
        id: playAllLibraryTracksAction
        text: "Play All Library Tracks"
        enabled: root.canPlayAllLibraryTracks()
        onTriggered: root.playAllLibraryTracks()
    }
    Action {
        id: appendAllLibraryTracksAction
        text: "Queue All Library Tracks"
        enabled: root.canPlayAllLibraryTracks()
        onTriggered: root.appendAllLibraryTracks()
    }
    Action {
        id: replaceFromItunesAction
        text: "Replace From iTunes..."
        enabled: root.currentTrackItunesArtworkDisabledReason().length === 0
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
        onTriggered: root.moveSelected(-1)
    }
    Action {
        id: moveTrackDownAction
        text: "Move Track Down"
        shortcut: "Ctrl+Shift+Down"
        onTriggered: root.moveSelected(1)
    }

    Shortcut {
        sequence: "Space"
        enabled: !(libraryViewRef && libraryViewRef.activeFocus)
            && !globalSearchController.dialogHasActiveInputFocus
        onActivated: root.togglePlayPause()
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
            const localPaths = root.fileDialogPaths(externalFileDialog)
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
            const localPath = root.folderDialogPath(scanFolderDialog)
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
        uiPalette: root.uiPalette
        sections: root.statusBarSections()
        channelStatusIconSource: root.channelStatusIconSource
        mixColor: root.mixColor
        themeIsDark: root.themeIsDark
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
            mixColor: root.mixColor
            themeIsDark: root.themeIsDark
            volumeMuted: root.volumeMuted
            displayedPositionSeconds: root.displayedPositionSeconds
            toggleMutedVolume: root.toggleMutedVolume
            setAppVolume: root.setAppVolume
            normalizedVolumeValue: root.normalizedVolumeValue
            seekCommitted: function(value) {
                root.positionSmoothingAnimationMs = 0
                root.displayedPositionSeconds = value
                root.positionSmoothingPrimed = true
                root.positionSmoothingAnchorSeconds = value
                root.positionSmoothingLastMs = Date.now()
                uiBridge.seek(value)
            }
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
                currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
                openAlbumArtViewer: root.openAlbumArtViewer
                queueTrackNumberText: root.queueTrackNumberText
                popupTransitionMs: root.uiPopupTransitionMs
                snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                stepScrollView: root.stepScrollView
                rowsForLibraryAction: root.rowsForLibraryAction
                playLibraryRows: root.playLibraryRows
                appendLibraryRows: root.appendLibraryRows
                isActionableLibraryRow: root.isActionableLibraryRow
                canOpenTagEditorForLibrary: root.canOpenTagEditorForLibrary
                openTagEditorForLibrary: root.openTagEditorForLibrary
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
                    playlistOrderText: root.playlistOrderText
                    openTagEditorForPlaylistRow: root.openTagEditorForPlaylistRow
                    stepScrollView: root.stepScrollView
                    clearPlaylistAction: clearPlaylistAction
                    popupTransitionMs: root.uiPopupTransitionMs
                    snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                    snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                    rowsForLibraryAction: root.rowsForLibraryAction
                    appendLibraryRows: root.appendLibraryRows
                    droppedExternalPaths: root.droppedExternalPaths
                    submitExternalImport: root.submitExternalImport
                }

                Panes.SpectrogramPane {
                    id: spectrogramPane
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 220
                    openViewer: root.openSpectrogramViewer
                }
            }
        }
    }

    Viewers.SpectrogramSurface {
        id: spectrogramSurface
        parent: root.spectrogramViewerOpen
            ? (root.useWholeScreenViewerMode
                ? spectrogramViewerShell.windowHost
                : spectrogramViewerShell.popupHost)
            : spectrogramPane.hostItem
        visible: parent !== null
        anchors.fill: parent
        uiBridge: root.uiBridge
    }

    Viewers.SpectrogramViewerShell {
        id: spectrogramViewerShell
        windowRoot: root
        viewerOpen: root.spectrogramViewerOpen
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        popupTransitionMs: root.uiPopupTransitionMs
        titleText: root.title
        closeViewer: root.closeSpectrogramViewer
    }

    Dialogs.ItunesArtworkDialog {
        id: itunesArtworkDialog
        uiBridge: root.uiBridge
        uiPalette: root.uiPalette
        windowRoot: root
        pathFromAnyUrl: root.pathFromAnyUrl
        openAlbumArtViewerForSuggestion: root.openAlbumArtViewerForSuggestion
    }

    Viewers.AlbumArtViewerShell {
        id: albumArtViewerShell
        windowRoot: root
        viewerOpen: root.albumArtViewerOpen
        useWholeScreenViewerMode: root.useWholeScreenViewerMode
        popupTransitionMs: root.uiPopupTransitionMs
        titleText: root.title
        closeViewer: root.closeAlbumArtViewer
        toggleInfoVisible: root.toggleAlbumArtInfoVisible
    }

    Viewers.AlbumArtSurface {
        id: albumArtSurface
        parent: root.albumArtViewerOpen
            ? (root.useWholeScreenViewerMode ? albumArtViewerShell.windowHost : albumArtViewerShell.popupHost)
            : albumArtMainHost
        visible: root.albumArtViewerOpen
        anchors.fill: parent
        viewerOpen: root.albumArtViewerOpen
        viewerSource: root.albumArtViewerSource
        infoVisible: root.albumArtInfoVisible
        initialViewToken: root.albumArtViewResetToken
        viewerDecodeWidth: root.albumArtViewerDecodeWidth
        viewerDecodeHeight: root.albumArtViewerDecodeHeight
        infoOverlayText: root.albumArtInfoOverlayText()
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: root.currentTrackItunesArtworkDisabledReason
        closeViewer: root.closeAlbumArtViewer
        toggleInfoVisible: root.toggleAlbumArtInfoVisible
        focusFullscreen: albumArtViewerShell.focusFullscreen
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
        source: !root.albumArtViewerOpen || root.albumArtViewerShowsCurrentTrack
            ? (uiBridge.currentTrackCoverPath || "")
            : ""
        sourceSize.width: root.albumArtViewerDecodeWidth
        sourceSize.height: root.albumArtViewerDecodeHeight
    }

    onClosing: function(close) { uiBridge.shutdown() }

    Connections {
        target: uiBridge
        function applyAnalysisDelta() {
            const delta = uiBridge.takeSpectrogramRowsDeltaPacked()
            if ((uiBridge.playbackState || "") === "Stopped") {
                spectrogramSurface.haltForCurrentMode()
                return
            }
            if (uiBridge.spectrogramReset
                    && root.visualFeedsEnabled
                    && delta.channels
                    && delta.channels.length > 0) {
                spectrogramSurface.resetForCurrentMode(true)
            }
            if (root.visualFeedsEnabled && delta.channels && delta.channels.length > 0) {
                spectrogramSurface.appendPackedDelta(delta.channels)
            }
        }
        function onSnapshotChanged() {
            const stopped = (uiBridge.playbackState || "") === "Stopped"
            const currentTrackPath = uiBridge.currentTrackPath || ""
            if (stopped) {
                const stoppedTrackChanged = root.stoppedSpectrogramTrackPath.length > 0
                    && root.stoppedSpectrogramTrackPath !== currentTrackPath
                if (stoppedTrackChanged) {
                    spectrogramSurface.resetForCurrentMode(true)
                } else {
                    spectrogramSurface.haltForCurrentMode()
                }
                root.stoppedSpectrogramTrackPath = currentTrackPath
            } else {
                root.stoppedSpectrogramTrackPath = currentTrackPath
            }
            root.syncMutedVolumeState()
            if (root.albumArtViewerOpen
                    && root.albumArtViewerShowsCurrentTrack
                    && root.albumArtViewerSource !== (uiBridge.currentTrackCoverPath || "")) {
                root.albumArtViewerSource = uiBridge.currentTrackCoverPath || ""
                root.albumArtViewerInfoSource = root.pathFromAnyUrl(root.albumArtViewerSource)
                if (root.albumArtInfoVisible) {
                    root.refreshAlbumArtFileInfo()
                } else {
                    root.albumArtViewerFileInfo = ({})
                }
            } else if (root.albumArtViewerOpen
                    && root.albumArtViewerInfoSource
                        !== root.pathFromAnyUrl(root.albumArtViewerSource || "")) {
                root.albumArtViewerInfoSource = root.pathFromAnyUrl(root.albumArtViewerSource || "")
                if (root.albumArtInfoVisible) {
                    root.refreshAlbumArtFileInfo()
                } else {
                    root.albumArtViewerFileInfo = ({})
                }
            }
            queueController.handleBridgeSnapshotUpdate()
        }
        function onPlaybackChanged() {
            const playbackState = uiBridge.playbackState || ""
            if (root.shouldResetSpectrogramForStoppedTrackSwitch(
                        root.lastSpectrogramPlaybackState,
                        playbackState,
                        root.stoppedSpectrogramTrackPath,
                        uiBridge.currentTrackPath || "")) {
                spectrogramSurface.resetForCurrentMode(true)
                root.stoppedSpectrogramTrackPath = uiBridge.currentTrackPath || ""
            }
            const incomingPosition = uiBridge.positionSeconds
            const trackChanged = root.positionSmoothingTrackPath !== uiBridge.currentTrackPath
            const nowMs = Date.now()
            const duration = Math.max(uiBridge.durationSeconds, 0)
            if (playbackState !== "Playing") {
                if (playbackState === "Stopped") {
                    spectrogramSurface.haltForCurrentMode()
                }
                root.positionSmoothingAnimationMs = 0
                root.displayedPositionSeconds = incomingPosition
                root.positionSmoothingPrimed = false
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
            } else if (!root.positionSmoothingPrimed || trackChanged) {
                root.positionSmoothingAnimationMs = 0
                root.displayedPositionSeconds = incomingPosition
                root.positionSmoothingPrimed = true
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
            } else {
                const cadenceMs = root.positionSmoothingLastMs > 0
                    ? Math.max(120, Math.min(1200, nowMs - root.positionSmoothingLastMs))
                    : 1000
                const drift = incomingPosition - root.displayedPositionSeconds
                if (Math.abs(drift) > 0.20) {
                    root.positionSmoothingAnimationMs = 0
                    root.displayedPositionSeconds = incomingPosition
                } else {
                    root.positionSmoothingAnimationMs = cadenceMs
                    const predictedTarget = incomingPosition + (cadenceMs / 1000.0)
                    root.displayedPositionSeconds = duration > 0
                        ? Math.min(duration, Math.max(0.0, predictedTarget))
                        : Math.max(0.0, predictedTarget)
                }
                root.positionSmoothingAnchorSeconds = incomingPosition
                root.positionSmoothingLastMs = nowMs
                root.positionSmoothingTrackPath = uiBridge.currentTrackPath
            }
            root.lastSpectrogramPlaybackState = playbackState
        }
        function onLibraryTreeFrameReceived(version, treeBytes) {
            libraryController.requestTreeApply(version, treeBytes || "")
        }
        function onAnalysisChanged() {
            applyAnalysisDelta()
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
        basenameFromPath: root.basenameFromPath
    }

    Component.onCompleted: {
        libraryController.requestTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = uiBridge.positionSeconds
        root.positionSmoothingAnimationMs = 0
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = uiBridge.currentTrackPath
        queueController.initializeFromBridge()
        libraryController.syncSelectionToVisibleRows()
        globalSearchController.syncSelectionAfterResultsChange()
    }
}
