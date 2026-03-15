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
    property string selectedLibrarySelectionKey: ""
    property int selectedLibrarySourceIndex: -1
    property string selectedLibraryRowType: ""
    property string selectedLibraryArtist: ""
    property string selectedLibraryAlbum: ""
    property string selectedLibraryTrackPath: ""
    property string selectedLibraryOpenPath: ""
    property var selectedLibraryPlayPaths: []
    property var selectedLibrarySelectionKeys: []
    property int librarySelectionAnchorIndex: -1
    property var selectedQueueIndices: []
    property var selectedQueueIndexLookup: ({})
    property int queueSelectionAnchorIndex: -1
    property int lastSyncedBridgeSelectedQueueIndex: -2
    property int lastAppliedLibraryVersion: -1
    property int pendingLibraryVersion: -1
    property bool hasReceivedLibraryTreeFrame: false
    property string pendingLibraryAnchorKey: ""
    property real pendingLibraryAnchorOffset: 0
    property real pendingLibraryAnchorFallbackY: 0
    property bool pendingLibraryAnchorValid: false
    property int lastSeenQueueVersion: -1
    property int lastCenteredQueueIndex: -2
    property string lastAutoCenterPlaybackState: ""
    property string lastAutoCenterTrackPath: ""
    property real playlistViewportRestoreUntilMs: 0
    property real playlistViewportRestoreContentY: 0
    property bool autoCenterQueueSelection: true
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
    property string libraryTypeAheadBuffer: ""
    property string pendingLibraryRevealSelectionKey: ""
    property var pendingLibraryRevealExpandKeys: []
    property int pendingLibraryRevealAttempts: 0
    property string pendingLibraryExpandFitKey: ""
    property int pendingLibraryExpandFitAttempts: 0
    property string pendingSearchOpenSelectionKey: ""
    property var pendingSearchOpenExpandKeys: []
    property int pendingSearchOpenAttempts: 0
    property var libraryViewRef: null
    property var playlistViewRef: null
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
        requestLibraryRevealForSearchRow: root.requestLibraryRevealForSearchRow
        focusLibraryViewForNavigation: root.focusLibraryViewForNavigation
        requestOpenInFileBrowserForSearchRow: root.openGlobalSearchRowInFileBrowser
    }

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

    Timer {
        id: libraryTypeAheadTimer
        interval: 900
        repeat: false
        onTriggered: root.libraryTypeAheadBuffer = ""
    }

    Timer {
        id: libraryRevealRetryTimer
        interval: 80
        repeat: false
        onTriggered: root.applyPendingLibraryReveal()
    }

    Timer {
        id: searchOpenRetryTimer
        interval: 80
        repeat: false
        onTriggered: root.applyPendingSearchOpen()
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
        if (root.isQueueIndexSelected(rowIndex) && root.selectedQueueIndices.length > 1) {
            indices = root.selectedQueueIndices.slice().sort(function(a, b) { return a - b })
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

    function queueSelectionCount() {
        if (selectedQueueIndices.length > 0) {
            return selectedQueueIndices.length
        }
        return uiBridge.selectedQueueIndex >= 0 ? 1 : 0
    }

    function isQueueIndexSelected(index) {
        return !!selectedQueueIndexLookup[index]
    }

    function setSelectedQueueIndices(indices) {
        const next = indices || []
        selectedQueueIndices = next
        const lookup = ({})
        for (let i = 0; i < next.length; ++i) {
            const idx = next[i]
            if (idx >= 0 && idx < uiBridge.queueLength) {
                lookup[idx] = true
            }
        }
        selectedQueueIndexLookup = lookup
    }

    function isLibrarySelectionKeySelected(key) {
        return key.length > 0 && selectedLibrarySelectionKeys.indexOf(key) >= 0
    }

    function clearLibraryPrimarySelection() {
        root.selectedLibrarySelectionKey = ""
        root.selectedLibrarySourceIndex = -1
        root.selectedLibraryRowType = ""
        root.selectedLibraryArtist = ""
        root.selectedLibraryAlbum = ""
        root.selectedLibraryTrackPath = ""
        root.selectedLibraryOpenPath = ""
        root.selectedLibraryPlayPaths = []
    }

    function applyLibraryPrimaryRow(rowMap) {
        root.selectedLibrarySelectionKey = rowMap.selectionKey || ""
        root.selectedLibrarySourceIndex = rowMap.sourceIndex !== undefined ? rowMap.sourceIndex : -1
        root.selectedLibraryRowType = rowMap.rowType || ""
        root.selectedLibraryArtist = rowMap.artist || ""
        root.selectedLibraryAlbum = rowMap.name || ""
        root.selectedLibraryTrackPath = rowMap.trackPath || ""
        root.selectedLibraryOpenPath = rowMap.openPath || ""
        root.selectedLibraryPlayPaths = rowMap.playPaths || []
    }

    function applyLibraryPrimaryFromIndex(index) {
        const rowMap = libraryModel.rowDataForRow(index)
        if (rowMap && rowMap.selectionKey && rowMap.selectionKey.length > 0) {
            applyLibraryPrimaryRow(rowMap)
            return true
        }
        return false
    }

    function setLibrarySingleSelection(index, rowMap) {
        if (!rowMap.selectionKey || rowMap.selectionKey.length === 0) {
            selectedLibrarySelectionKeys = []
            librarySelectionAnchorIndex = -1
            clearLibraryPrimarySelection()
            return
        }
        selectedLibrarySelectionKeys = [rowMap.selectionKey]
        librarySelectionAnchorIndex = index
        applyLibraryPrimaryRow(rowMap)
    }

    function setLibraryRangeSelection(index) {
        const anchor = librarySelectionAnchorIndex >= 0 ? librarySelectionAnchorIndex : index
        const first = Math.min(anchor, index)
        const last = Math.max(anchor, index)
        const keys = []
        for (let i = first; i <= last; ++i) {
            const rowMap = libraryModel.rowDataForRow(i)
            const key = rowMap.selectionKey || ""
            if (key.length > 0 && keys.indexOf(key) < 0) {
                keys.push(key)
            }
        }
        selectedLibrarySelectionKeys = keys
        librarySelectionAnchorIndex = anchor
        applyLibraryPrimaryFromIndex(index)
    }

    function toggleLibrarySelection(index, rowMap) {
        const key = rowMap.selectionKey || ""
        if (key.length === 0) {
            return
        }
        const keys = selectedLibrarySelectionKeys.slice()
        const pos = keys.indexOf(key)
        if (pos >= 0) {
            keys.splice(pos, 1)
        } else {
            keys.push(key)
        }
        selectedLibrarySelectionKeys = keys
        librarySelectionAnchorIndex = index
        if (keys.length === 0) {
            clearLibraryPrimarySelection()
            return
        }
        if (keys.indexOf(selectedLibrarySelectionKey) >= 0) {
            return
        }
        const fallbackKey = keys[keys.length - 1]
        const fallbackIndex = libraryModel.indexForSelectionKey(fallbackKey)
        if (!applyLibraryPrimaryFromIndex(fallbackIndex)) {
            clearLibraryPrimarySelection()
        }
    }

    function handleLibraryRowSelection(index, rowMap, button, modifiers) {
        if (!rowMap.selectionKey || rowMap.selectionKey.length === 0) {
            return
        }
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        if (shift) {
            setLibraryRangeSelection(index)
            return
        }
        if (ctrl) {
            toggleLibrarySelection(index, rowMap)
            return
        }
        if (button === Qt.RightButton && isLibrarySelectionKeySelected(rowMap.selectionKey)) {
            librarySelectionAnchorIndex = index
            applyLibraryPrimaryRow(rowMap)
            return
        }
        setLibrarySingleSelection(index, rowMap)
    }

    function toggleLibraryNode(key) {
        if (!key || key.length === 0) {
            return
        }
        const index = libraryModel.indexForSelectionKey(key)
        let expanding = false
        if (index >= 0) {
            const rowMap = libraryModel.rowDataForRow(index) || ({})
            const rowType = rowMap.rowType || ""
            const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0
            expanding = hasChildren && !Boolean(rowMap.expanded)
        }
        if (expanding) {
            scheduleLibraryExpansionFit(key)
        } else if (pendingLibraryExpandFitKey === key) {
            pendingLibraryExpandFitKey = ""
            pendingLibraryExpandFitAttempts = 0
        }
        pendingLibraryAnchorValid = false
        libraryModel.toggleKey(key)
        if (expanding) {
            Qt.callLater(function() {
                root.applyPendingLibraryExpansionFit()
            })
        }
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

    function captureLibraryViewAnchor() {
        if (!libraryViewRef || libraryModel.count <= 0) {
            return {
                key: "",
                offset: 0,
                fallbackY: libraryViewRef ? libraryViewRef.contentY : 0
            }
        }
        const rowHeight = 24
        const topIndex = Math.max(0, Math.min(
            libraryModel.count - 1,
            Math.floor(libraryViewRef.contentY / rowHeight)))
        return {
            key: libraryModel.selectionKeyForRow(topIndex) || "",
            offset: libraryViewRef.contentY - (topIndex * rowHeight),
            fallbackY: libraryViewRef.contentY
        }
    }

    function restoreLibraryViewAnchor(anchor) {
        if (!libraryViewRef) {
            return
        }
        const rowHeight = 24
        let targetY = anchor && anchor.fallbackY !== undefined ? anchor.fallbackY : 0
        if (anchor && anchor.key && anchor.key.length > 0) {
            const index = libraryModel.indexForSelectionKey(anchor.key)
            if (index >= 0) {
                targetY = (index * rowHeight) + (anchor.offset || 0)
            }
        }
        const restoreY = function() {
            const maxYNow = Math.max(0, libraryViewRef.contentHeight - libraryViewRef.height)
            libraryViewRef.contentY = Math.max(0, Math.min(targetY, maxYNow))
        }
        restoreY()
        Qt.callLater(restoreY)
    }

    function scheduleLibraryExpansionFit(key) {
        if (!key || key.length === 0) {
            return
        }
        pendingLibraryExpandFitKey = key
        pendingLibraryExpandFitAttempts = 4
    }

    function applyPendingLibraryExpansionFit() {
        if (!libraryViewRef || pendingLibraryExpandFitKey.length === 0) {
            return
        }
        const key = pendingLibraryExpandFitKey
        const rowIndex = libraryModel.indexForSelectionKey(key)
        if (rowIndex < 0 || rowIndex >= libraryModel.count) {
            return
        }

        const rowMap = libraryModel.rowDataForRow(rowIndex) || ({})
        if (!rowMap || !rowMap.expanded) {
            pendingLibraryExpandFitKey = ""
            pendingLibraryExpandFitAttempts = 0
            return
        }

        const viewHeight = Math.max(0, libraryViewRef.height)
        if (viewHeight <= 0) {
            if (pendingLibraryExpandFitAttempts > 0) {
                pendingLibraryExpandFitAttempts -= 1
                Qt.callLater(function() {
                    root.applyPendingLibraryExpansionFit()
                })
            } else {
                pendingLibraryExpandFitKey = ""
            }
            return
        }

        const rowHeight = 24
        const baseDepth = rowMap.depth !== undefined ? rowMap.depth : 0
        let lastDescendantIndex = rowIndex
        for (let i = rowIndex + 1; i < libraryModel.count; ++i) {
            const descendant = libraryModel.rowDataForRow(i) || ({})
            const descendantDepth = descendant.depth !== undefined ? descendant.depth : 0
            if (descendantDepth <= baseDepth) {
                break
            }
            lastDescendantIndex = i
        }

        if ((rowMap.count || 0) > 0 && lastDescendantIndex === rowIndex) {
            return
        }

        const blockTop = rowIndex * rowHeight
        const blockBottom = (lastDescendantIndex + 1) * rowHeight
        if ((blockBottom - blockTop) > viewHeight) {
            libraryViewRef.positionViewAtIndex(rowIndex, ListView.Beginning)
        } else {
            libraryViewRef.positionViewAtIndex(lastDescendantIndex, ListView.Contain)
        }
        const visibleTop = libraryViewRef.contentY
        const visibleBottom = visibleTop + viewHeight
        const blockFits = (blockBottom - blockTop) <= viewHeight
        const blockVisible = blockFits
            ? (blockTop >= visibleTop - 0.5 && blockBottom <= visibleBottom + 0.5)
            : Math.abs(visibleTop - blockTop) <= 0.5
        if (blockVisible) {
            pendingLibraryExpandFitKey = ""
            pendingLibraryExpandFitAttempts = 0
            return
        }
    }

    function finishPendingLibraryTreeApply() {
        if (pendingLibraryVersion < 0 || libraryModel.parsing) {
            return
        }
        lastAppliedLibraryVersion = pendingLibraryVersion
        pendingLibraryVersion = -1
        root.syncLibrarySelectionToVisibleRows()
        if (pendingLibraryAnchorValid) {
            if (pendingLibraryExpandFitKey.length === 0) {
                restoreLibraryViewAnchor({
                    key: pendingLibraryAnchorKey,
                    offset: pendingLibraryAnchorOffset,
                    fallbackY: pendingLibraryAnchorFallbackY
                })
            }
            pendingLibraryAnchorValid = false
        }
        if (pendingLibraryExpandFitKey.length > 0) {
            applyPendingLibraryExpansionFit()
        }
    }

    function requestLibraryTreeApply(version, treeBytes) {
        if (version <= 0 && (!treeBytes || treeBytes.length === 0)) {
            return
        }
        if (version < 0 || version === pendingLibraryVersion) {
            return
        }
        if (version === lastAppliedLibraryVersion && pendingLibraryVersion < 0) {
            return
        }
        const anchor = captureLibraryViewAnchor()
        pendingLibraryAnchorKey = anchor.key || ""
        pendingLibraryAnchorOffset = anchor.offset || 0
        pendingLibraryAnchorFallbackY = anchor.fallbackY || 0
        pendingLibraryAnchorValid = true
        hasReceivedLibraryTreeFrame = true
        pendingLibraryVersion = version
        libraryModel.setLibraryTreeFromBinary(treeBytes || "")
        finishPendingLibraryTreeApply()
    }

    function resetQueueSelectionForUpdatedQueue() {
        if (uiBridge.selectedQueueIndex >= 0 && uiBridge.selectedQueueIndex < uiBridge.queueLength) {
            setSelectedQueueIndices([uiBridge.selectedQueueIndex])
            queueSelectionAnchorIndex = uiBridge.selectedQueueIndex
        } else {
            setSelectedQueueIndices([])
            queueSelectionAnchorIndex = -1
        }
    }

    function isLibraryTreeLoading() {
        if (pendingLibraryVersion >= 0 || libraryModel.parsing) {
            return true
        }
        if (!hasReceivedLibraryTreeFrame && lastAppliedLibraryVersion <= 0) {
            return true
        }
        return uiBridge.libraryScanInProgress && (!libraryViewRef || libraryViewRef.count === 0)
    }

    function clearQueueSelection() {
        setSelectedQueueIndices([])
        queueSelectionAnchorIndex = -1
        uiBridge.selectQueueIndex(-1)
    }

    function requestPlaylistViewportRestoreWindow(durationMs) {
        if (!playlistViewRef) {
            return
        }
        const ms = Math.max(100, durationMs || 700)
        playlistViewportRestoreContentY = playlistViewRef.contentY
        playlistViewportRestoreUntilMs = Math.max(playlistViewportRestoreUntilMs, Date.now() + ms)
    }

    function playlistViewportRestoreActive() {
        return playlistViewportRestoreUntilMs > Date.now()
    }

    function applyPendingPlaylistViewportRestore() {
        if (!playlistViewRef || !playlistViewportRestoreActive()) {
            return
        }
        const maxY = Math.max(0, playlistViewRef.contentHeight - playlistViewRef.height)
        const targetY = Math.max(0, Math.min(maxY, playlistViewportRestoreContentY))
        if (Math.abs(playlistViewRef.contentY - targetY) > 0.5) {
            playlistViewRef.contentY = targetY
        }
    }

    function handleQueueSnapshotChanged(view) {
        const playlistView = view || playlistViewRef
        if (!playlistView) {
            return
        }
        root.playlistViewRef = playlistView
        const playbackState = uiBridge.playbackState || ""
        const currentTrackPath = uiBridge.currentTrackPath || ""
        if (!root.autoCenterQueueSelection) {
            root.lastAutoCenterPlaybackState = playbackState
            root.lastAutoCenterTrackPath = currentTrackPath
            return
        }
        if (root.playlistViewportRestoreActive()) {
            root.lastAutoCenterPlaybackState = playbackState
            root.lastAutoCenterTrackPath = currentTrackPath
            return
        }
        const targetIndex = uiBridge.playingQueueIndex
        if (playbackState === "Stopped"
                && root.lastAutoCenterPlaybackState !== "Stopped") {
            root.lastAutoCenterPlaybackState = playbackState
            root.lastAutoCenterTrackPath = currentTrackPath
            return
        }
        const trackChanged = currentTrackPath.length > 0
            && currentTrackPath !== root.lastAutoCenterTrackPath
        const resumedFromStop = playbackState !== "Stopped"
            && root.lastAutoCenterPlaybackState === "Stopped"
        const needsInitialCenter = root.lastCenteredQueueIndex < 0
        if (targetIndex >= 0 && (trackChanged || resumedFromStop || needsInitialCenter)) {
            playlistView.positionViewAtIndex(targetIndex, ListView.Contain)
            root.lastCenteredQueueIndex = targetIndex
        }
        root.lastAutoCenterPlaybackState = playbackState
        root.lastAutoCenterTrackPath = currentTrackPath
    }

    function setQueueSingleSelection(index) {
        if (index < 0 || index >= uiBridge.queueLength) {
            clearQueueSelection()
            return
        }
        if (selectedQueueIndices.length === 1
                && selectedQueueIndices[0] === index
                && queueSelectionAnchorIndex === index
                && uiBridge.selectedQueueIndex === index) {
            return
        }
        setSelectedQueueIndices([index])
        queueSelectionAnchorIndex = index
        uiBridge.selectQueueIndex(index)
    }

    function setQueueRangeSelection(index) {
        if (index < 0 || index >= uiBridge.queueLength) {
            return
        }
        const anchor = queueSelectionAnchorIndex >= 0
            ? queueSelectionAnchorIndex
            : (uiBridge.selectedQueueIndex >= 0 ? uiBridge.selectedQueueIndex : index)
        const first = Math.min(anchor, index)
        const last = Math.max(anchor, index)
        const indices = []
        for (let i = first; i <= last; ++i) {
            indices.push(i)
        }
        setSelectedQueueIndices(indices)
        queueSelectionAnchorIndex = anchor
        uiBridge.selectQueueIndex(index)
    }

    function toggleQueueSelection(index) {
        if (index < 0 || index >= uiBridge.queueLength) {
            return
        }
        const indices = selectedQueueIndices.slice()
        const pos = indices.indexOf(index)
        if (pos >= 0) {
            indices.splice(pos, 1)
        } else {
            indices.push(index)
            indices.sort(function(a, b) { return a - b })
        }
        setSelectedQueueIndices(indices)
        queueSelectionAnchorIndex = index
        if (indices.length > 0) {
            uiBridge.selectQueueIndex(index)
        } else {
            uiBridge.selectQueueIndex(-1)
        }
    }

    function handleQueueRowSelection(index, button, modifiers) {
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        if (shift) {
            setQueueRangeSelection(index)
            return
        }
        if (ctrl) {
            toggleQueueSelection(index)
            return
        }
        if (button === Qt.RightButton && isQueueIndexSelected(index)) {
            queueSelectionAnchorIndex = index
            uiBridge.selectQueueIndex(index)
            return
        }
        setQueueSingleSelection(index)
    }

    function syncQueueSelectionToCurrentQueue() {
        const valid = []
        const seen = ({})
        for (let i = 0; i < selectedQueueIndices.length; ++i) {
            const idx = selectedQueueIndices[i]
            if (idx >= 0 && idx < uiBridge.queueLength && !seen[idx]) {
                seen[idx] = true
                valid.push(idx)
            }
        }
        valid.sort(function(a, b) { return a - b })
        if (valid.length === 0 && uiBridge.selectedQueueIndex >= 0) {
            valid.push(uiBridge.selectedQueueIndex)
        }
        setSelectedQueueIndices(valid)
        if (queueSelectionAnchorIndex < 0 || queueSelectionAnchorIndex >= uiBridge.queueLength) {
            queueSelectionAnchorIndex = valid.length > 0 ? valid[valid.length - 1] : -1
        }
    }

    function syncLibrarySelectionToVisibleRows() {
        const valid = []
        for (let i = 0; i < selectedLibrarySelectionKeys.length; ++i) {
            const key = selectedLibrarySelectionKeys[i]
            if (libraryModel.hasSelectionKey(key) && valid.indexOf(key) < 0) {
                valid.push(key)
            }
        }
        selectedLibrarySelectionKeys = valid
        if (selectedLibrarySelectionKey.length > 0
                && selectedLibrarySelectionKeys.indexOf(selectedLibrarySelectionKey) < 0) {
            if (selectedLibrarySelectionKeys.length > 0) {
                const fallbackIndex =
                    libraryModel.indexForSelectionKey(selectedLibrarySelectionKeys[0])
                if (!applyLibraryPrimaryFromIndex(fallbackIndex)) {
                    clearLibraryPrimarySelection()
                }
            } else {
                clearLibraryPrimarySelection()
            }
        }
        if (librarySelectionAnchorIndex >= libraryModel.count || librarySelectionAnchorIndex < 0) {
            librarySelectionAnchorIndex = selectedLibrarySelectionKey.length > 0
                ? libraryModel.indexForSelectionKey(selectedLibrarySelectionKey)
                : -1
        }
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

    function selectQueueRelative(delta) {
        if (uiBridge.queueLength <= 0) {
            return
        }
        const current = uiBridge.selectedQueueIndex >= 0
            ? uiBridge.selectedQueueIndex
            : uiBridge.playingQueueIndex
        const base = current >= 0 ? current : 0
        const nextIdx = Math.max(0, Math.min(uiBridge.queueLength - 1, base + delta))
        setQueueSingleSelection(nextIdx)
    }

    function firstSelectedQueueIndex() {
        let first = -1
        for (let i = 0; i < selectedQueueIndices.length; ++i) {
            const idx = selectedQueueIndices[i]
            if (idx < 0 || idx >= uiBridge.queueLength) {
                continue
            }
            if (first < 0 || idx < first) {
                first = idx
            }
        }
        if (first >= 0) {
            return first
        }
        if (uiBridge.selectedQueueIndex >= 0 && uiBridge.selectedQueueIndex < uiBridge.queueLength) {
            return uiBridge.selectedQueueIndex
        }
        return -1
    }

    function playFirstSelectedQueueTrack() {
        const target = firstSelectedQueueIndex()
        if (target >= 0) {
            uiBridge.playAt(target)
        }
    }

    function playlistPageStep() {
        const rowHeight = 24
        const viewportHeight = playlistViewRef ? playlistViewRef.height : 240
        return Math.max(1, Math.floor(viewportHeight / rowHeight) - 1)
    }

    function ensurePlaylistIndexVisible(index) {
        if (!playlistViewRef || index < 0) {
            return
        }
        const firstVisible = playlistViewRef.indexAt(0, 0)
        const lastVisible = playlistViewRef.indexAt(0, playlistViewRef.height - 1)
        if (firstVisible >= 0
                && lastVisible >= 0
                && index >= firstVisible
                && index <= lastVisible) {
            return
        }
        playlistViewRef.positionViewAtIndex(index, ListView.Contain)
    }

    function selectAllQueueItems() {
        if (uiBridge.queueLength <= 0) {
            clearQueueSelection()
            return
        }
        const indices = []
        for (let i = 0; i < uiBridge.queueLength; ++i) {
            indices.push(i)
        }
        const primary = uiBridge.selectedQueueIndex >= 0
            ? uiBridge.selectedQueueIndex
            : 0
        setSelectedQueueIndices(indices)
        queueSelectionAnchorIndex = primary
        uiBridge.selectQueueIndex(primary)
    }

    function handlePlaylistKeyPress(event) {
        if (!event) {
            return
        }
        const modifiers = event.modifiers || Qt.NoModifier
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        const shift = (modifiers & Qt.ShiftModifier) !== 0

        if (!ctrl && !shift
                && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            playFirstSelectedQueueTrack()
            event.accepted = true
            return
        }

        if (ctrl && !shift && event.key === Qt.Key_A) {
            selectAllQueueItems()
            event.accepted = true
            return
        }

        let delta = 0
        if (event.key === Qt.Key_Up) {
            delta = -1
        } else if (event.key === Qt.Key_Down) {
            delta = 1
        } else if (event.key === Qt.Key_PageUp) {
            delta = -playlistPageStep()
        } else if (event.key === Qt.Key_PageDown) {
            delta = playlistPageStep()
        } else {
            return
        }

        if (uiBridge.queueLength <= 0) {
            event.accepted = true
            return
        }

        const current = uiBridge.selectedQueueIndex >= 0
            ? uiBridge.selectedQueueIndex
            : (uiBridge.playingQueueIndex >= 0 ? uiBridge.playingQueueIndex : 0)
        const next = Math.max(0, Math.min(uiBridge.queueLength - 1, current + delta))
        if (shift) {
            setQueueRangeSelection(next)
        } else {
            setQueueSingleSelection(next)
        }
        ensurePlaylistIndexVisible(next)
        event.accepted = true
    }

    function removeSelectedQueueTrack() {
        if (selectedQueueIndices.length > 0) {
            const indices = selectedQueueIndices.slice()
            indices.sort(function(a, b) { return b - a })
            if (uiBridge.queueLength > 0 && indices.length >= uiBridge.queueLength) {
                requestPlaylistViewportRestoreWindow(700)
                uiBridge.clearQueue()
                setSelectedQueueIndices([])
                queueSelectionAnchorIndex = -1
                return
            }
            requestPlaylistViewportRestoreWindow(Math.max(700, indices.length * 120))
            for (let i = 0; i < indices.length; ++i) {
                uiBridge.removeAt(indices[i])
            }
            setSelectedQueueIndices([])
            queueSelectionAnchorIndex = -1
            return
        }
        if (uiBridge.selectedQueueIndex >= 0) {
            requestPlaylistViewportRestoreWindow(700)
            uiBridge.removeAt(uiBridge.selectedQueueIndex)
        }
    }

    function currentLibrarySelectionIndex() {
        if (selectedLibrarySelectionKey.length > 0) {
            const selectedIndex = libraryModel.indexForSelectionKey(selectedLibrarySelectionKey)
            if (selectedIndex >= 0) {
                return selectedIndex
            }
        }
        if (libraryModel.count > 0) {
            return 0
        }
        return -1
    }

    function selectLibraryIndex(index) {
        if (index < 0 || index >= libraryModel.count) {
            return false
        }
        const rowMap = libraryModel.rowDataForRow(index)
        if (!rowMap || !(rowMap.selectionKey || "").length) {
            return false
        }
        setLibrarySingleSelection(index, rowMap)
        scrollLibrarySelectionKeyIntoView(rowMap.selectionKey || "")
        return true
    }

    function scrollLibrarySelectionKeyIntoView(selectionKey) {
        if (!libraryViewRef || !selectionKey || selectionKey.length === 0) {
            return
        }
        const immediateIndex = libraryModel.indexForSelectionKey(selectionKey)
        if (immediateIndex >= 0) {
            libraryViewRef.positionViewAtIndex(immediateIndex, ListView.Contain)
        }
        Qt.callLater(function() {
            if (!libraryViewRef) {
                return
            }
            const delayedIndex = libraryModel.indexForSelectionKey(selectionKey)
            if (delayedIndex >= 0) {
                libraryViewRef.positionViewAtIndex(delayedIndex, ListView.Contain)
            }
        })
    }

    function focusLibraryViewForNavigation() {
        if (!libraryViewRef) {
            return
        }
        libraryViewRef.forceActiveFocus()
        Qt.callLater(function() {
            if (libraryViewRef) {
                libraryViewRef.forceActiveFocus()
            }
        })
    }

    function selectLibraryRelative(delta) {
        if (libraryModel.count <= 0) {
            return
        }
        const current = currentLibrarySelectionIndex()
        const base = current >= 0 ? current : 0
        const next = Math.max(0, Math.min(libraryModel.count - 1, base + delta))
        selectLibraryIndex(next)
    }

    function expandLibrarySelection() {
        const index = currentLibrarySelectionIndex()
        if (index < 0) {
            return
        }
        if (selectedLibrarySelectionKey.length === 0) {
            selectLibraryIndex(index)
        }
        const rowMap = libraryModel.rowDataForRow(index)
        const rowType = rowMap.rowType || ""
        const key = rowMap.key || ""
        const expanded = !!rowMap.expanded
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0 && key.length > 0
        if (hasChildren && !expanded) {
            toggleLibraryNode(key)
            return
        }
        if (index + 1 < libraryModel.count) {
            const nextRow = libraryModel.rowDataForRow(index + 1)
            const nextDepth = nextRow.depth !== undefined ? nextRow.depth : 0
            const currentDepth = rowMap.depth !== undefined ? rowMap.depth : 0
            if (nextDepth > currentDepth) {
                selectLibraryIndex(index + 1)
            }
        }
    }

    function collapseLibrarySelection() {
        const index = currentLibrarySelectionIndex()
        if (index < 0) {
            return
        }
        if (selectedLibrarySelectionKey.length === 0) {
            selectLibraryIndex(index)
        }
        const rowMap = libraryModel.rowDataForRow(index)
        const key = rowMap.key || ""
        const expanded = !!rowMap.expanded
        const rowType = rowMap.rowType || ""
        const currentDepth = rowMap.depth !== undefined ? rowMap.depth : 0
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0 && key.length > 0
        if (hasChildren && expanded) {
            toggleLibraryNode(key)
            return
        }
        for (let i = index - 1; i >= 0; --i) {
            const candidate = libraryModel.rowDataForRow(i)
            const candidateDepth = candidate.depth !== undefined ? candidate.depth : 0
            if (candidateDepth < currentDepth) {
                selectLibraryIndex(i)
                return
            }
        }
    }

    function activateLibrarySelection() {
        const index = currentLibrarySelectionIndex()
        if (index < 0) {
            return
        }
        if (selectedLibrarySelectionKey.length === 0) {
            selectLibraryIndex(index)
        }
        const rowMap = libraryModel.rowDataForRow(index)
        const rows = rowsForLibraryAction(rowMap)
        if (rows.length > 0) {
            playLibraryRows(rows)
        }
    }

    function libraryTypeAheadSearch(prefix) {
        if (prefix.length === 0) {
            return false
        }
        const total = libraryModel.count
        if (total <= 0) {
            return false
        }
        for (let i = 0; i < total; ++i) {
            const rowMap = libraryModel.rowDataForRow(i)
            if ((rowMap.rowType || "") !== "artist") {
                continue
            }
            const name = (rowMap.artist || "").toLowerCase()
            if (name.startsWith(prefix)) {
                selectLibraryIndex(i)
                return true
            }
        }
        return false
    }

    function handleLibraryKeyPress(event) {
        if (globalSearchController.tryCapturePrefill(event)) {
            return
        }
        if ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0) {
            return
        }
        if (libraryModel.count <= 0) {
            return
        }
        if (event.key === Qt.Key_Up) {
            selectLibraryRelative(-1)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Down) {
            selectLibraryRelative(1)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Right) {
            expandLibrarySelection()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Left) {
            collapseLibrarySelection()
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Space) {
            const index = currentLibrarySelectionIndex()
            if (index >= 0) {
                const rowMap = libraryModel.rowDataForRow(index)
                const rowType = rowMap.rowType || ""
                if (rowType !== "track" && (rowMap.key || "").length > 0 && (rowMap.count || 0) > 0) {
                    toggleLibraryNode(rowMap.key || "")
                }
            }
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            activateLibrarySelection()
            event.accepted = true
            return
        }

        const text = event.text || ""
        if (text.length === 1 && text !== "\n" && text !== "\r" && text !== "\t") {
            const nextPrefix = (root.libraryTypeAheadBuffer + text).toLowerCase()
            root.libraryTypeAheadBuffer = nextPrefix
            libraryTypeAheadTimer.restart()
            if (libraryTypeAheadSearch(nextPrefix)) {
                event.accepted = true
            }
        }
    }

    function openDiagnostics() {
        diagnosticsDialog.open()
    }

    function requestLibraryRevealForSearchRow(row) {
        if (!row) {
            return
        }
        const expandKeys = []
        if ((row.artistKey || "").length > 0) {
            expandKeys.push(row.artistKey)
        }
        if ((row.albumKey || "").length > 0) {
            expandKeys.push(row.albumKey)
        }
        if ((row.sectionKey || "").length > 0) {
            expandKeys.push(row.sectionKey)
        }
        pendingLibraryRevealExpandKeys = expandKeys
        pendingLibraryRevealSelectionKey = (row.trackKey || row.albumKey || row.artistKey || "")
        pendingLibraryRevealAttempts = 80
        Qt.callLater(root.applyPendingLibraryReveal)
    }

    function ensureLibraryKeyExpanded(key) {
        const normalized = (key || "").trim()
        if (normalized.length === 0) {
            return true
        }
        const rowIndex = libraryModel.indexForSelectionKey(normalized)
        if (rowIndex < 0) {
            return false
        }
        const rowMap = libraryModel.rowDataForRow(rowIndex)
        if (!rowMap) {
            return false
        }
        const rowType = rowMap.rowType || ""
        const hasChildren = rowType !== "track" && (rowMap.count || 0) > 0
        if (!hasChildren) {
            return true
        }
        if (!!rowMap.expanded) {
            return true
        }
        libraryModel.toggleKey(normalized)
        return false
    }

    function applyPendingLibraryReveal() {
        if (pendingLibraryRevealSelectionKey.length === 0) {
            return
        }
        for (let i = 0; i < pendingLibraryRevealExpandKeys.length; ++i) {
            const expandKey = pendingLibraryRevealExpandKeys[i] || ""
            if (expandKey.length > 0) {
                ensureLibraryKeyExpanded(expandKey)
            }
        }
        const index = libraryModel.indexForSelectionKey(pendingLibraryRevealSelectionKey)
        if (index >= 0) {
            selectLibraryIndex(index)
            focusLibraryViewForNavigation()
            pendingLibraryRevealSelectionKey = ""
            pendingLibraryRevealExpandKeys = []
            pendingLibraryRevealAttempts = 0
            return
        }
        if (pendingLibraryRevealAttempts <= 0) {
            pendingLibraryRevealSelectionKey = ""
            pendingLibraryRevealExpandKeys = []
            return
        }
        pendingLibraryRevealAttempts -= 1
        libraryRevealRetryTimer.restart()
    }

    function applyPendingSearchOpen() {
        if (pendingSearchOpenSelectionKey.length === 0) {
            return
        }
        for (let i = 0; i < pendingSearchOpenExpandKeys.length; ++i) {
            const expandKey = pendingSearchOpenExpandKeys[i] || ""
            if (expandKey.length > 0) {
                ensureLibraryKeyExpanded(expandKey)
            }
        }
        const index = libraryModel.indexForSelectionKey(pendingSearchOpenSelectionKey)
        if (index >= 0) {
            const rowMap = libraryModel.rowDataForRow(index)
            const openPath = rowMap.openPath || rowMap.trackPath || ""
            if (openPath.length > 0) {
                uiBridge.openInFileBrowser(openPath)
            }
            pendingSearchOpenSelectionKey = ""
            pendingSearchOpenExpandKeys = []
            pendingSearchOpenAttempts = 0
            return
        }
        if (pendingSearchOpenAttempts <= 0) {
            pendingSearchOpenSelectionKey = ""
            pendingSearchOpenExpandKeys = []
            return
        }
        pendingSearchOpenAttempts -= 1
        searchOpenRetryTimer.restart()
    }

    function openGlobalSearchRowInFileBrowser(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            uiBridge.openContainingFolder(row.trackPath || "")
            return
        }
        const selectionKey = rowType === "album" ? (row.albumKey || "") : (row.artistKey || "")
        if (selectionKey.length === 0) {
            return
        }
        const expandKeys = []
        if ((row.artistKey || "").length > 0) {
            expandKeys.push(row.artistKey)
        }
        if (rowType === "album" && (row.albumKey || "").length > 0) {
            expandKeys.push(row.albumKey)
        }
        pendingSearchOpenSelectionKey = selectionKey
        pendingSearchOpenExpandKeys = expandKeys
        pendingSearchOpenAttempts = 80
        Qt.callLater(root.applyPendingSearchOpen)
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
        enabled: root.queueSelectionCount() > 0
        onTriggered: root.removeSelectedQueueTrack()
    }
    Action {
        id: selectPreviousTrackAction
        text: "Select Previous Track"
        shortcut: "Ctrl+Up"
        enabled: uiBridge.queueLength > 0
        onTriggered: root.selectQueueRelative(-1)
    }
    Action {
        id: selectNextTrackAction
        text: "Select Next Track"
        shortcut: "Ctrl+Down"
        enabled: uiBridge.queueLength > 0
        onTriggered: root.selectQueueRelative(1)
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
        checked: root.autoCenterQueueSelection
        onTriggered: root.autoCenterQueueSelection = checked
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
                pendingLibraryExpandFitKey: root.pendingLibraryExpandFitKey
                applyPendingLibraryExpansionFit: root.applyPendingLibraryExpansionFit
                stepScrollView: root.stepScrollView
                handleLibraryKeyPress: root.handleLibraryKeyPress
                isLibrarySelectionKeySelected: root.isLibrarySelectionKeySelected
                toggleLibraryNode: root.toggleLibraryNode
                handleLibraryRowSelection: root.handleLibraryRowSelection
                rowsForLibraryAction: root.rowsForLibraryAction
                playLibraryRows: root.playLibraryRows
                appendLibraryRows: root.appendLibraryRows
                isActionableLibraryRow: root.isActionableLibraryRow
                canOpenTagEditorForLibrary: root.canOpenTagEditorForLibrary
                openTagEditorForLibrary: root.openTagEditorForLibrary
                isLibraryTreeLoading: root.isLibraryTreeLoading
                playAllLibraryTracksAction: playAllLibraryTracksAction
                appendAllLibraryTracksAction: appendAllLibraryTracksAction
                onViewReady: function(view) {
                    root.libraryViewRef = view
                }
            }

            SplitView {
                orientation: Qt.Vertical
                SplitView.fillWidth: true

                Panes.QueuePane {
                    uiBridge: root.uiBridge
                    uiPalette: root.uiPalette
                    preferredHeight: root.height * 0.58
                    playlistIndicatorColumnWidth: root.playlistIndicatorColumnWidth
                    playlistOrderColumnWidth: root.playlistOrderColumnWidth
                    playlistOrderText: root.playlistOrderText
                    isQueueIndexSelected: root.isQueueIndexSelected
                    handleQueueRowSelection: root.handleQueueRowSelection
                    openTagEditorForPlaylistRow: root.openTagEditorForPlaylistRow
                    requestPlaylistViewportRestoreWindow: root.requestPlaylistViewportRestoreWindow
                    removeSelectedQueueTrack: root.removeSelectedQueueTrack
                    stepScrollView: root.stepScrollView
                    handlePlaylistKeyPress: root.handlePlaylistKeyPress
                    clearPlaylistAction: clearPlaylistAction
                    popupTransitionMs: root.uiPopupTransitionMs
                    snappyScrollFlickDeceleration: root.snappyScrollFlickDeceleration
                    snappyScrollMaxFlickVelocity: root.snappyScrollMaxFlickVelocity
                    selectedQueueIndices: root.selectedQueueIndices
                    rowsForLibraryAction: root.rowsForLibraryAction
                    appendLibraryRows: root.appendLibraryRows
                    droppedExternalPaths: root.droppedExternalPaths
                    submitExternalImport: root.submitExternalImport
                    applyPendingPlaylistViewportRestore: root.applyPendingPlaylistViewportRestore
                    handleQueueSnapshotChanged: root.handleQueueSnapshotChanged
                    onViewReady: function(view) {
                        root.playlistViewRef = view
                    }
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
            if (uiBridge.queueVersion !== root.lastSeenQueueVersion) {
                root.lastSeenQueueVersion = uiBridge.queueVersion
                root.resetQueueSelectionForUpdatedQueue()
                root.applyPendingPlaylistViewportRestore()
                root.syncQueueSelectionToCurrentQueue()
                root.lastSyncedBridgeSelectedQueueIndex = uiBridge.selectedQueueIndex
            }
            if (uiBridge.selectedQueueIndex !== root.lastSyncedBridgeSelectedQueueIndex) {
                root.syncQueueSelectionToCurrentQueue()
                root.lastSyncedBridgeSelectedQueueIndex = uiBridge.selectedQueueIndex
            }
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
            root.requestLibraryTreeApply(version, treeBytes || "")
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
            root.finishPendingLibraryTreeApply()
            root.applyPendingLibraryReveal()
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
        root.requestLibraryTreeApply(uiBridge.libraryVersion, uiBridge.libraryTreeBinary || "")
        root.lastSeenQueueVersion = uiBridge.queueVersion
        root.lastAutoCenterPlaybackState = uiBridge.playbackState
        root.lastAutoCenterTrackPath = uiBridge.currentTrackPath
        root.displayedPositionSeconds = uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = uiBridge.positionSeconds
        root.positionSmoothingAnimationMs = 0
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = uiBridge.currentTrackPath
        root.syncQueueSelectionToCurrentQueue()
        root.lastSyncedBridgeSelectedQueueIndex = uiBridge.selectedQueueIndex
        root.syncLibrarySelectionToVisibleRows()
        globalSearchController.syncSelectionAfterResultsChange()
    }
}
