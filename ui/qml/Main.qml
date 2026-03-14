import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import QtQml 2.15
import Qt.labs.platform 1.1 as Platform
import FerrousUi 1.0
import org.kde.kirigami 2.20 as Kirigami

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
    property real albumArtZoom: 1.0
    property real albumArtPanX: 0.0
    property real albumArtPanY: 0.0
    property bool albumArtInitialViewPending: false
    property bool albumArtViewerOpen: false
    property bool albumArtInfoVisible: false
    property var albumArtViewerFileInfo: ({})
    property string albumArtViewerInfoSource: ""
    property string albumArtViewerSource: ""
    property bool albumArtViewerShowsCurrentTrack: true
    readonly property int albumArtViewerDecodeWidth: Math.max(
        1024,
        Math.ceil(Math.max(root.width, albumArtFullscreenWindow.width, albumArtViewer.width)))
    readonly property int albumArtViewerDecodeHeight: Math.max(
        1024,
        Math.ceil(Math.max(root.height, albumArtFullscreenWindow.height, albumArtViewer.height)))
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
    property int globalSearchSelectedDisplayIndex: -1
    property var globalSearchContextRowData: ({})
    property bool globalSearchOpening: false
    property bool globalSearchIgnoreRefocusFind: false
    readonly property bool themeIsDark: root.colorLuma(Kirigami.Theme.backgroundColor) < 0.45
    readonly property color uiPaneColor: root.themeIsDark
        ? root.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.08)
        : root.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.20)
    readonly property color uiSurfaceColor: root.themeIsDark
        ? root.mixColor(Kirigami.Theme.backgroundColor, "#ffffff", 0.14)
        : "#ffffff"
    readonly property color uiSurfaceAltColor: root.themeIsDark
        ? root.mixColor(root.uiSurfaceColor, Kirigami.Theme.textColor, 0.08)
        : root.mixColor(root.uiSurfaceColor, Kirigami.Theme.textColor, 0.07)
    readonly property color uiSurfaceRaisedColor: root.themeIsDark
        ? root.mixColor(root.uiSurfaceColor, "#ffffff", 0.08)
        : "#ffffff"
    readonly property color uiHeaderColor: root.mixColor(
        root.uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        root.themeIsDark ? 0.12 : 0.10)
    readonly property color uiSectionColor: root.mixColor(
        root.uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        root.themeIsDark ? 0.18 : 0.16)
    readonly property color uiColumnsColor: root.mixColor(
        root.uiSurfaceAltColor,
        Kirigami.Theme.highlightColor,
        root.themeIsDark ? 0.11 : 0.09)
    readonly property color uiBorderColor: root.mixColor(
        root.uiSurfaceColor,
        Kirigami.Theme.textColor,
        root.themeIsDark ? 0.22 : 0.18)
    readonly property color uiTextColor: Kirigami.Theme.textColor
    readonly property color uiMutedTextColor: root.mixColor(
        Kirigami.Theme.disabledTextColor,
        Kirigami.Theme.textColor,
        root.themeIsDark ? 0.12 : 0.06)
    readonly property color uiSelectionColor: root.mixColor(
        Kirigami.Theme.highlightColor,
        root.uiSurfaceColor,
        root.themeIsDark ? 0.08 : 0.06)
    readonly property color uiSelectionTextColor: Kirigami.Theme.highlightedTextColor
    readonly property color uiActiveIndicatorColor: root.mixColor(
        Kirigami.Theme.highlightColor,
        Kirigami.Theme.positiveTextColor,
        0.35)
    readonly property real snappyScrollFlickDeceleration: 18000
    readonly property real snappyScrollMaxFlickVelocity: 1400
    readonly property int uiPopupTransitionMs: 0
    property string pendingGlobalSearchPrefillText: ""
    property string globalSearchOpenInitialText: ""
    readonly property bool visualFeedsEnabled: visible
        && visibility !== Window.Minimized
    readonly property bool useWholeScreenViewerMode: uiBridge.viewerFullscreenMode === 1
    readonly property var uiBridge: bridge ? bridge : bridgeFallback
    readonly property var tagEditorApi: (typeof tagEditor !== "undefined" && tagEditor)
        ? tagEditor
        : tagEditorFallback
    readonly property var globalSearchModelApi: (uiBridge
        && uiBridge.globalSearchModel
        && uiBridge.globalSearchModel.nextSelectableIndex)
        ? uiBridge.globalSearchModel
        : globalSearchModelFallback
    readonly property var spectrogramFftChoices: [512, 1024, 2048, 4096, 8192]

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
        property var globalSearchModel: null
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
            && !seekSlider.pressed
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

    Timer {
        id: globalSearchOpenSettleTimer
        interval: 260
        repeat: false
        onTriggered: root.globalSearchIgnoreRefocusFind = false
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

    function stepGlobalSearchResultsView(wheel) {
        if (!globalSearchResultsView || !wheel) {
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
        const maxY = Math.max(0, globalSearchResultsView.contentHeight - globalSearchResultsView.height)
        if (maxY <= 0) {
            return
        }
        const rowPx = 24
        const stepPx = rowPx * 3
        const notches = (wheel.angleDelta && wheel.angleDelta.y !== undefined && wheel.angleDelta.y !== 0)
            ? Math.max(1, Math.round(Math.abs(wheel.angleDelta.y) / 120))
            : Math.max(1, Math.round(Math.abs(deltaY) / stepPx))
        const direction = deltaY > 0 ? -1 : 1
        let targetY = globalSearchResultsView.contentY + (direction * notches * stepPx)
        targetY = Math.max(0, Math.min(maxY, targetY))

        if (direction > 0 && globalSearchRowCount() > 0) {
            const lastIndex = globalSearchRowCount() - 1
            const lastRowTop = globalSearchRowTop(lastIndex)
            const lastRowBottom = lastRowTop + globalSearchRowHeight(lastIndex)
            const viewportBottom = targetY + globalSearchResultsView.height
            const lastRowPartiallyVisible = lastRowTop < viewportBottom && lastRowBottom > viewportBottom
            if (lastRowPartiallyVisible) {
                targetY = Math.max(0, Math.min(maxY, lastRowBottom - globalSearchResultsView.height))
            } else if ((maxY - targetY) <= globalSearchRowHeight(lastIndex)) {
                targetY = maxY
            }
        }

        globalSearchResultsView.contentY = targetY
        if (direction > 0 && globalSearchRowCount() > 0) {
            const lastIndex = globalSearchRowCount() - 1
            const probeX = Math.max(0, Math.min(8, globalSearchResultsView.width - 1))
            const probeY = Math.max(0, globalSearchResultsView.height - 2)
            const bottomIndex = globalSearchResultsView.indexAt(probeX, probeY)
            if (bottomIndex >= lastIndex - 1
                    || (maxY - globalSearchResultsView.contentY) <= globalSearchRowHeight(lastIndex)) {
                globalSearchResultsView.positionViewAtIndex(lastIndex, ListView.End)
                Qt.callLater(function() {
                    if (globalSearchResultsView) {
                        globalSearchResultsView.positionViewAtIndex(lastIndex, ListView.End)
                    }
                })
            }
        }
        wheel.accepted = true
    }

    function globalSearchRowHeight(index) {
        if (index < 0 || !globalSearchModelApi) {
            return 24
        }
        const row = globalSearchModelApi.rowDataAt(index)
        return row && (row.kind || "") === "section" ? 30 : 24
    }

    function globalSearchRowTop(index) {
        if (index <= 0) {
            return 0
        }
        let y = 0
        for (let i = 0; i < index; ++i) {
            y += globalSearchRowHeight(i)
        }
        return y
    }

    function captureLibraryViewAnchor() {
        if (!libraryAlbumView || libraryModel.count <= 0) {
            return {
                key: "",
                offset: 0,
                fallbackY: libraryAlbumView ? libraryAlbumView.contentY : 0
            }
        }
        const rowHeight = 24
        const topIndex = Math.max(0, Math.min(
            libraryModel.count - 1,
            Math.floor(libraryAlbumView.contentY / rowHeight)))
        return {
            key: libraryModel.selectionKeyForRow(topIndex) || "",
            offset: libraryAlbumView.contentY - (topIndex * rowHeight),
            fallbackY: libraryAlbumView.contentY
        }
    }

    function restoreLibraryViewAnchor(anchor) {
        if (!libraryAlbumView) {
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
            const maxYNow = Math.max(0, libraryAlbumView.contentHeight - libraryAlbumView.height)
            libraryAlbumView.contentY = Math.max(0, Math.min(targetY, maxYNow))
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
        if (!libraryAlbumView || pendingLibraryExpandFitKey.length === 0) {
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

        const viewHeight = Math.max(0, libraryAlbumView.height)
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
            libraryAlbumView.positionViewAtIndex(rowIndex, ListView.Beginning)
        } else {
            libraryAlbumView.positionViewAtIndex(lastDescendantIndex, ListView.Contain)
        }
        const visibleTop = libraryAlbumView.contentY
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
        return uiBridge.libraryScanInProgress && libraryAlbumView.count === 0
    }

    function clearQueueSelection() {
        setSelectedQueueIndices([])
        queueSelectionAnchorIndex = -1
        uiBridge.selectQueueIndex(-1)
    }

    function requestPlaylistViewportRestoreWindow(durationMs) {
        if (!playlistView) {
            return
        }
        const ms = Math.max(100, durationMs || 700)
        playlistViewportRestoreContentY = playlistView.contentY
        playlistViewportRestoreUntilMs = Math.max(playlistViewportRestoreUntilMs, Date.now() + ms)
    }

    function playlistViewportRestoreActive() {
        return playlistViewportRestoreUntilMs > Date.now()
    }

    function applyPendingPlaylistViewportRestore() {
        if (!playlistView || !playlistViewportRestoreActive()) {
            return
        }
        const maxY = Math.max(0, playlistView.contentHeight - playlistView.height)
        const targetY = Math.max(0, Math.min(maxY, playlistViewportRestoreContentY))
        if (Math.abs(playlistView.contentY - targetY) > 0.5) {
            playlistView.contentY = targetY
        }
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
        const viewportHeight = playlistView ? playlistView.height : 240
        return Math.max(1, Math.floor(viewportHeight / rowHeight) - 1)
    }

    function ensurePlaylistIndexVisible(index) {
        if (!playlistView || index < 0) {
            return
        }
        const firstVisible = playlistView.indexAt(0, 0)
        const lastVisible = playlistView.indexAt(0, playlistView.height - 1)
        if (firstVisible >= 0
                && lastVisible >= 0
                && index >= firstVisible
                && index <= lastVisible) {
            return
        }
        playlistView.positionViewAtIndex(index, ListView.Contain)
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
        if (!libraryAlbumView || !selectionKey || selectionKey.length === 0) {
            return
        }
        const immediateIndex = libraryModel.indexForSelectionKey(selectionKey)
        if (immediateIndex >= 0) {
            libraryAlbumView.positionViewAtIndex(immediateIndex, ListView.Contain)
        }
        Qt.callLater(function() {
            if (!libraryAlbumView) {
                return
            }
            const delayedIndex = libraryModel.indexForSelectionKey(selectionKey)
            if (delayedIndex >= 0) {
                libraryAlbumView.positionViewAtIndex(delayedIndex, ListView.Contain)
            }
        })
    }

    function focusLibraryViewForNavigation() {
        if (!libraryAlbumView) {
            return
        }
        libraryAlbumView.forceActiveFocus()
        Qt.callLater(function() {
            if (libraryAlbumView) {
                libraryAlbumView.forceActiveFocus()
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
        if (root.tryCaptureGlobalSearchPrefill(event)) {
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

    function globalSearchRowCount() {
        return globalSearchResultsView ? (globalSearchResultsView.count || 0) : 0
    }

    function syncGlobalSearchSelectionAfterResultsChange() {
        const firstIndex = nextSearchSelectableIndex(-1, 1, false)
        if (globalSearchSelectedDisplayIndex < 0 || !isSearchRowSelectable(globalSearchSelectedDisplayIndex)) {
            globalSearchSelectedDisplayIndex = firstIndex
        } else if (globalSearchSelectedDisplayIndex >= globalSearchRowCount()) {
            globalSearchSelectedDisplayIndex = firstIndex
        }
    }

    function searchFirstSelectableIndex() {
        return nextSearchSelectableIndex(-1, 1, false)
    }

    function searchLastSelectableIndex() {
        return nextSearchSelectableIndex(globalSearchRowCount(), -1, false)
    }

    function isSearchRowSelectable(index) {
        return globalSearchModelApi ? !!globalSearchModelApi.isSelectableIndex(index) : false
    }

    function nextSearchSelectableIndex(startIndex, step, wrap) {
        if (!globalSearchModelApi) {
            return -1
        }
        return globalSearchModelApi.nextSelectableIndex(startIndex, step, wrap)
    }

    function moveGlobalSearchSelectionByPage(direction) {
        if (globalSearchRowCount() === 0) {
            return false
        }
        const stepDir = direction < 0 ? -1 : 1
        const pageRows = Math.max(
            1,
            Math.floor(((globalSearchResultsView ? globalSearchResultsView.height : 240) / 24)) - 1)
        let index = globalSearchSelectedDisplayIndex
        if (!isSearchRowSelectable(index)) {
            index = stepDir > 0 ? searchFirstSelectableIndex() : searchLastSelectableIndex()
        }
        if (index < 0) {
            return false
        }
        let moved = false
        for (let i = 0; i < pageRows; ++i) {
            const next = nextSearchSelectableIndex(index, stepDir, false)
            if (next < 0) {
                break
            }
            index = next
            moved = true
        }
        if (!moved) {
            return false
        }
        return selectGlobalSearchDisplayIndex(index)
    }

    function selectGlobalSearchDisplayIndex(index) {
        if (!isSearchRowSelectable(index)) {
            return false
        }
        globalSearchSelectedDisplayIndex = index
        if (globalSearchResultsView && index >= 0 && index < globalSearchRowCount()) {
            const firstSelectable = searchFirstSelectableIndex()
            if (index === firstSelectable && globalSearchModelApi) {
                globalSearchResultsView.contentY = 0
                Qt.callLater(function() {
                    if (globalSearchResultsView) {
                        globalSearchResultsView.contentY = 0
                    }
                })
            } else {
                globalSearchResultsView.positionViewAtIndex(index, ListView.Contain)
            }
        }
        return true
    }

    function selectedGlobalSearchRow() {
        if (!isSearchRowSelectable(globalSearchSelectedDisplayIndex)) {
            return null
        }
        const row = globalSearchModelApi
            ? globalSearchModelApi.rowDataAt(globalSearchSelectedDisplayIndex)
            : null
        return row || null
    }

    function openGlobalSearch() {
        if (globalSearchDialog.visible) {
            focusGlobalSearchQueryField(!root.globalSearchIgnoreRefocusFind)
            return
        }
        beginGlobalSearchOpen()
        globalSearchDialog.open()
    }

    function focusGlobalSearchQueryField(selectAll) {
        if (!globalSearchQueryField) {
            return
        }
        globalSearchQueryField.forceActiveFocus()
        if (selectAll) {
            globalSearchQueryField.selectAll()
        } else {
            globalSearchQueryField.cursorPosition = (globalSearchQueryField.text || "").length
        }
    }

    function beginGlobalSearchOpen() {
        root.globalSearchOpening = true
        root.globalSearchIgnoreRefocusFind = true
        root.pendingGlobalSearchPrefillText = ""
        root.globalSearchOpenInitialText = globalSearchQueryField
            ? (globalSearchQueryField.text || "")
            : ""
    }

    function endGlobalSearchOpen(closeDialog) {
        root.globalSearchOpening = false
        root.globalSearchIgnoreRefocusFind = false
        globalSearchOpenSettleTimer.stop()
        root.pendingGlobalSearchPrefillText = ""
        root.globalSearchOpenInitialText = ""
        if (closeDialog) {
            uiBridge.setGlobalSearchQuery("")
        }
    }

    function isGlobalSearchPrintableChar(text) {
        return text.length === 1
            && text !== "\n"
            && text !== "\r"
            && text !== "\t"
    }

    function trimInitialSearchPrefix(currentText, initialText) {
        const current = currentText || ""
        const initial = initialText || ""
        if (initial.length > 0 && current !== initial && current.startsWith(initial)) {
            return current.slice(initial.length)
        }
        return current
    }

    function applyGlobalSearchOpenText() {
        if (!globalSearchQueryField) {
            return
        }
        if ((root.pendingGlobalSearchPrefillText || "").length > 0) {
            globalSearchQueryField.text = root.pendingGlobalSearchPrefillText
            root.pendingGlobalSearchPrefillText = ""
            return
        }

        const current = globalSearchQueryField.text || ""
        const initial = root.globalSearchOpenInitialText || ""
        if (current.length <= 0) {
            return
        }
        const trimmed = trimInitialSearchPrefix(current, initial)
        if (trimmed !== current) {
            globalSearchQueryField.text = trimmed
            globalSearchQueryField.cursorPosition = (globalSearchQueryField.text || "").length
            return
        }
        if (current === initial) {
            globalSearchQueryField.selectAll()
        }
    }

    function tryCaptureGlobalSearchPrefill(event) {
        const shouldCapture = root.globalSearchOpening
            || (globalSearchDialog.visible
                && root.globalSearchIgnoreRefocusFind
                && (!globalSearchQueryField || !globalSearchQueryField.activeFocus))
        if (!shouldCapture) {
            return false
        }
        if ((event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)) !== 0) {
            return false
        }
        const openingText = event.text || ""
        if (!isGlobalSearchPrintableChar(openingText)) {
            return false
        }
        if (globalSearchDialog.visible && !root.globalSearchOpening && globalSearchQueryField) {
            const hasSelection = (globalSearchQueryField.selectedText || "").length > 0
            const current = globalSearchQueryField.text || ""
            if (hasSelection) {
                globalSearchQueryField.text = openingText
            } else {
                const alreadyTyped = trimInitialSearchPrefix(current, root.globalSearchOpenInitialText || "")
                globalSearchQueryField.text = alreadyTyped + openingText
            }
            globalSearchQueryField.cursorPosition = (globalSearchQueryField.text || "").length
            root.focusGlobalSearchQueryField(false)
        } else {
            root.pendingGlobalSearchPrefillText += openingText
        }
        event.accepted = true
        return true
    }

    function openDiagnostics() {
        diagnosticsDialog.open()
    }

    function refreshDiagnosticsTextView() {
        if (!diagnosticsTextArea) {
            return
        }
        diagnosticsTextArea.text = uiBridge.diagnosticsText || ""
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

    function navigateGlobalSearchSelectionToLibrary() {
        let row = selectedGlobalSearchRow()
        if (!row) {
            const first = searchFirstSelectableIndex()
            if (first >= 0) {
                selectGlobalSearchDisplayIndex(first)
                row = selectedGlobalSearchRow()
            }
        }
        if (!row) {
            return
        }
        requestLibraryRevealForSearchRow(row)
        globalSearchDialog.close()
        Qt.callLater(root.focusLibraryViewForNavigation)
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

    function activateGlobalSearchRow(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            uiBridge.replaceWithPaths([row.trackPath || ""])
        } else if (rowType === "album") {
            const albumName = (row.album || row.label || "").trim()
            uiBridge.replaceAlbumByKey(
                (row.artistKey || row.artist || "").trim(),
                (row.albumKey || albumName).trim())
        } else if (rowType === "artist") {
            uiBridge.replaceArtistByName((row.artistKey || row.artist || row.label || "").trim())
        }
        requestLibraryRevealForSearchRow(row)
        globalSearchDialog.close()
    }

    function queueGlobalSearchRow(row) {
        if (!row || row.kind !== "item") {
            return
        }
        const rowType = row.rowType || ""
        if (rowType === "track") {
            uiBridge.appendTrack(row.trackPath || "")
            return
        }
        if (rowType === "album") {
            const albumName = (row.album || row.label || "").trim()
            uiBridge.appendAlbumByKey(
                (row.artistKey || row.artist || "").trim(),
                (row.albumKey || albumName).trim())
            return
        }
        if (rowType === "artist") {
            uiBridge.appendArtistByName((row.artistKey || row.artist || row.label || "").trim())
        }
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

    function activateGlobalSearchSelection() {
        const row = selectedGlobalSearchRow()
        if (row) {
            activateGlobalSearchRow(row)
        }
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
        const useWholeScreen = root.useWholeScreenViewerMode
        if (albumArtViewerOpen && !useWholeScreen) {
            if (!albumArtViewer.visible) {
                albumArtViewer.open()
            }
        } else if (albumArtViewer.visible) {
            albumArtViewer.close()
        }
        if (albumArtViewerOpen && useWholeScreen) {
            albumArtFullscreenWindow.requestActivate()
            root.applyAlbumArtInitialView()
        }
    }

    function syncSpectrogramViewerPresentation() {
        const useWholeScreen = root.useWholeScreenViewerMode
        if (spectrogramViewerOpen && !useWholeScreen) {
            if (!spectrogramViewer.visible) {
                spectrogramViewer.open()
            }
        } else if (spectrogramViewer.visible) {
            spectrogramViewer.close()
        }
        if (spectrogramViewerOpen && useWholeScreen) {
            spectrogramFullscreenWindow.requestActivate()
        }
    }

    function closeAlbumArtViewer() {
        albumArtViewerOpen = false
    }

    function closeSpectrogramViewer() {
        spectrogramViewerOpen = false
    }

    function clampAlbumArtPan() {
        const scaledW = albumArtTransform.width * root.albumArtZoom
        const scaledH = albumArtTransform.height * root.albumArtZoom
        const limitX = Math.max(0, (scaledW - albumArtViewport.width) / 2)
        const limitY = Math.max(0, (scaledH - albumArtViewport.height) / 2)
        root.albumArtPanX = Math.max(-limitX, Math.min(limitX, root.albumArtPanX))
        root.albumArtPanY = Math.max(-limitY, Math.min(limitY, root.albumArtPanY))
    }

    function isPointOnAlbumArtImage(item, x, y) {
        const p = albumArtImageFull.mapFromItem(item, x, y)
        const xOff = (albumArtImageFull.width - albumArtImageFull.paintedWidth) / 2
        const yOff = (albumArtImageFull.height - albumArtImageFull.paintedHeight) / 2
        return p.x >= xOff
            && p.y >= yOff
            && p.x <= xOff + albumArtImageFull.paintedWidth
            && p.y <= yOff + albumArtImageFull.paintedHeight
    }

    function applyAlbumArtInitialView() {
        if (!albumArtInitialViewPending || !albumArtViewerOpen) {
            return
        }
        if (albumArtViewport.width <= 0 || albumArtViewport.height <= 0) {
            return
        }
        if (albumArtImageFull.status === Image.Loading) {
            return
        }
        root.albumArtZoom = 1.0
        root.albumArtPanX = 0.0
        root.albumArtPanY = 0.0
        root.clampAlbumArtPan()
        albumArtInitialViewPending = false
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
        albumArtZoom = 1.0
        albumArtPanX = 0.0
        albumArtPanY = 0.0
        albumArtInfoVisible = false
        albumArtInitialViewPending = true
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
        albumArtZoom = 1.0
        albumArtPanX = 0.0
        albumArtPanY = 0.0
        albumArtInfoVisible = true
        albumArtInitialViewPending = true
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
        if (root.useWholeScreenViewerMode
                && albumArtFullscreenWindow.visibility === Window.FullScreen) {
            albumArtFullscreenFocusSink.forceActiveFocus()
        }
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
            ? albumArtWindowHost
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
        onTriggered: root.openGlobalSearch()
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
        enabled: !(libraryAlbumView && libraryAlbumView.activeFocus)
            && !(globalSearchDialog.visible
                && ((globalSearchQueryField && globalSearchQueryField.activeFocus)
                    || (globalSearchResultsView && globalSearchResultsView.activeFocus)))
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

    Dialog {
        id: aboutDialog
        modal: true
        title: "About Ferrous"
        standardButtons: Dialog.Ok
        width: 420
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        contentItem: Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Ferrous is a KDE-first audio player prototype with a Qt/QML UI and Rust backend."
            color: Kirigami.Theme.textColor
        }
    }

    Dialog {
        id: preferencesDialog
        modal: true
        title: "Preferences"
        standardButtons: Dialog.Close
        property int pageIndex: 0
        width: Math.min(760, root.width - 80)
        height: Math.min(620, root.height - 80)
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }

        contentItem: ColumnLayout {
            spacing: 14

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: preferencesTabsRow.implicitHeight
                color: root.uiSurfaceAltColor
                radius: 8
                border.color: root.uiBorderColor
                clip: true

                RowLayout {
                    id: preferencesTabsRow
                    anchors.fill: parent
                    spacing: 0

                    Repeater {
                        model: ["Library", "Spectrogram", "Display", "Last.fm", "System Media"]

                        delegate: Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            color: preferencesDialog.pageIndex === index
                                ? root.uiSelectionColor
                                : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: modelData
                                color: root.uiTextColor
                                font.weight: preferencesDialog.pageIndex === index
                                    ? Font.DemiBold
                                    : Font.Normal
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: preferencesDialog.pageIndex = index
                            }
                        }
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: preferencesDialog.pageIndex

                ScrollView {
                    id: libraryPrefsScroll
                    clip: true
                    contentWidth: availableWidth
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: libraryPrefsScroll.availableWidth
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            color: root.uiSurfaceColor
                            radius: 10
                            border.color: root.uiBorderColor
                            implicitHeight: libraryPrefsColumn.implicitHeight + 36

                            ColumnLayout {
                                id: libraryPrefsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                Label {
                                    Layout.fillWidth: true
                                    text: "Library"
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        text: "Add Root..."
                                        onClicked: root.promptAddLibraryRoot("preferences")
                                    }
                                    Button {
                                        text: "Rescan All"
                                        onClicked: uiBridge.rescanAllLibraryRoots()
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        text: "Album Sort"
                                        Layout.preferredWidth: 120
                                    }
                                    ComboBox {
                                        model: ["Year", "Title"]
                                        currentIndex: Math.max(0, Math.min(1, uiBridge.librarySortMode))
                                        onActivated: uiBridge.setLibrarySortMode(currentIndex)
                                        Layout.preferredWidth: 180
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: uiBridge.libraryRootEntries.length === 0
                                        ? "No library roots configured."
                                        : "Configured roots"
                                    color: Kirigami.Theme.disabledTextColor
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: Math.min(260, (60 * Math.max(1, uiBridge.libraryRootEntries.length)) + 12)
                                    color: root.uiSurfaceAltColor
                                    border.color: root.uiBorderColor
                                    radius: 8
                                    visible: uiBridge.libraryRootEntries.length > 0

                                    ListView {
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        clip: true
                                        model: uiBridge.libraryRootEntries
                                        boundsBehavior: Flickable.StopAtBounds
                                        boundsMovement: Flickable.StopAtBounds
                                        flickDeceleration: root.snappyScrollFlickDeceleration
                                        maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                                        pixelAligned: true
                                        spacing: 4
                                        MouseArea {
                                            anchors.fill: parent
                                            acceptedButtons: Qt.NoButton
                                            preventStealing: true
                                            onWheel: function(wheel) {
                                                root.stepScrollView(parent, wheel, 30, 3)
                                            }
                                        }
                                        delegate: Rectangle {
                                            readonly property var rootEntry: modelData || ({})
                                            readonly property string rootPath: rootEntry.path || ""
                                            readonly property string rootName: rootEntry.name || ""
                                            readonly property string rootDisplayName: rootEntry.displayName || rootPath
                                            width: ListView.view.width
                                            height: 52
                                            radius: 6
                                            color: root.uiSurfaceRaisedColor
                                            border.color: Qt.rgba(0, 0, 0, 0.06)

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 10
                                                anchors.rightMargin: 10
                                                spacing: 8

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2
                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: rootDisplayName
                                                        elide: Text.ElideRight
                                                    }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        visible: rootName.length > 0
                                                        text: rootPath
                                                        elide: Text.ElideMiddle
                                                        color: root.uiMutedTextColor
                                                        font.pixelSize: Math.max(11, root.font.pixelSize - 1)
                                                    }
                                                }
                                                ToolButton {
                                                    text: "Open"
                                                    onClicked: uiBridge.openInFileBrowser(rootPath)
                                                }
                                                ToolButton {
                                                    text: "Rename"
                                                    onClicked: root.openLibraryRootNameDialog(
                                                        "rename",
                                                        rootPath,
                                                        rootName)
                                                }
                                                ToolButton {
                                                    text: "Rescan"
                                                    onClicked: uiBridge.rescanLibraryRoot(rootPath)
                                                }
                                                ToolButton {
                                                    text: "Remove"
                                                    onClicked: uiBridge.removeLibraryRoot(rootPath)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                ScrollView {
                    id: spectrogramPrefsScroll
                    clip: true
                    contentWidth: availableWidth
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: spectrogramPrefsScroll.availableWidth
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            color: root.uiSurfaceColor
                            radius: 10
                            border.color: root.uiBorderColor
                            implicitHeight: spectrogramPrefsColumn.implicitHeight + 36

                            ColumnLayout {
                                id: spectrogramPrefsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                Label {
                                    Layout.fillWidth: true
                                    text: "Spectrogram"
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: Kirigami.Theme.disabledTextColor
                                    text: "Spectrogram-specific rendering and analysis options."
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        text: "View"
                                        Layout.preferredWidth: 120
                                    }
                                    ComboBox {
                                        Layout.preferredWidth: 220
                                        model: ["Downmix", "Per-channel"]
                                        currentIndex: Math.max(0, Math.min(1, uiBridge.spectrogramViewMode))
                                        onActivated: uiBridge.setSpectrogramViewMode(currentIndex)
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        text: "FFT Window"
                                        Layout.preferredWidth: 120
                                    }
                                    ComboBox {
                                        Layout.preferredWidth: 220
                                        model: root.spectrogramFftChoices
                                        currentIndex: {
                                            const index = root.spectrogramFftChoices.indexOf(uiBridge.fftSize)
                                            return index >= 0 ? index : 0
                                        }
                                        onActivated: uiBridge.setFftSize(
                                            root.spectrogramFftChoices[Math.max(0, currentIndex)])
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        text: "dB Range"
                                        Layout.preferredWidth: 120
                                    }
                                    Slider {
                                        id: prefsDbRangeSlider
                                        Layout.fillWidth: true
                                        from: 50
                                        to: 120
                                        stepSize: 1
                                        value: uiBridge.dbRange
                                        onMoved: uiBridge.setDbRange(value)
                                        onPressedChanged: {
                                            if (!pressed) {
                                                uiBridge.setDbRange(value)
                                            }
                                        }
                                    }
                                    Label {
                                        text: Math.round(prefsDbRangeSlider.value).toString()
                                        Layout.preferredWidth: 32
                                        horizontalAlignment: Text.AlignRight
                                    }
                                }

                                CheckBox {
                                    text: "Log Scale Spectrogram"
                                    focusPolicy: Qt.NoFocus
                                    checked: uiBridge.logScale
                                    onToggled: uiBridge.setLogScale(checked)
                                }
                                CheckBox {
                                    text: "Show Spectrogram FPS Overlay"
                                    focusPolicy: Qt.NoFocus
                                    checked: uiBridge.showFps
                                    onToggled: uiBridge.setShowFps(checked)
                                }
                            }
                        }
                    }
                }

                ScrollView {
                    id: displayPrefsScroll
                    clip: true
                    contentWidth: availableWidth
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: displayPrefsScroll.availableWidth
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            color: root.uiSurfaceColor
                            radius: 10
                            border.color: root.uiBorderColor
                            implicitHeight: displayPrefsColumn.implicitHeight + 36

                            ColumnLayout {
                                id: displayPrefsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                Label {
                                    Layout.fillWidth: true
                                    text: "Display"
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: Kirigami.Theme.disabledTextColor
                                    text: "Shared viewer presentation options for album art and spectrogram."
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12
                                    Label {
                                        text: "Viewer Fullscreen"
                                        Layout.preferredWidth: 120
                                    }
                                    ComboBox {
                                        Layout.preferredWidth: 220
                                        model: ["Within app window", "Whole screen"]
                                        currentIndex: Math.max(0, Math.min(1, uiBridge.viewerFullscreenMode))
                                        onActivated: uiBridge.setViewerFullscreenMode(currentIndex)
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }

                ScrollView {
                    id: lastFmPrefsScroll
                    clip: true
                    contentWidth: availableWidth
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: lastFmPrefsScroll.availableWidth
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            color: root.uiSurfaceColor
                            radius: 10
                            border.color: root.uiBorderColor
                            implicitHeight: lastFmPrefsColumn.implicitHeight + 36

                            ColumnLayout {
                                id: lastFmPrefsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                Label {
                                    Layout.fillWidth: true
                                    text: "Last.fm"
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                CheckBox {
                                    text: "Enable Last.fm scrobbling"
                                    focusPolicy: Qt.NoFocus
                                    checked: uiBridge.lastFmScrobblingEnabled
                                    onToggled: uiBridge.setLastFmScrobblingEnabled(checked)
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: Kirigami.Theme.disabledTextColor
                                    text: "Ferrous follows Last.fm's rule: only tracks longer than 30 seconds are eligible, and a scrobble is sent when playback stops or the track ends after at least half the track or 4 minutes has been listened, whichever comes first."
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    text: !uiBridge.lastFmBuildConfigured
                                        ? "Last.fm is not configured in this build."
                                        : (uiBridge.lastFmUsername.length > 0
                                            ? "Connected account: " + uiBridge.lastFmUsername
                                            : "No Last.fm account connected.")
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    visible: uiBridge.lastFmStatusText.length > 0
                                    color: Kirigami.Theme.disabledTextColor
                                    text: uiBridge.lastFmStatusText
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: uiBridge.lastFmPendingScrobbleCount > 0
                                    color: Kirigami.Theme.disabledTextColor
                                    text: "Pending scrobbles: " + uiBridge.lastFmPendingScrobbleCount
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    Button {
                                        text: uiBridge.lastFmUsername.length > 0 ? "Reconnect" : "Connect"
                                        enabled: uiBridge.lastFmBuildConfigured
                                        onClicked: uiBridge.beginLastFmAuth()
                                    }

                                    Button {
                                        text: "Complete Connection"
                                        enabled: uiBridge.lastFmBuildConfigured && uiBridge.lastFmAuthState === 1
                                        onClicked: uiBridge.completeLastFmAuth()
                                    }

                                    Button {
                                        text: "Disconnect"
                                        enabled: uiBridge.lastFmUsername.length > 0 || uiBridge.lastFmAuthState !== 0
                                        onClicked: uiBridge.disconnectLastFm()
                                    }

                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }

                ScrollView {
                    id: systemMediaPrefsScroll
                    clip: true
                    contentWidth: availableWidth
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    ColumnLayout {
                        width: systemMediaPrefsScroll.availableWidth
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            color: root.uiSurfaceColor
                            radius: 10
                            border.color: root.uiBorderColor
                            implicitHeight: systemMediaPrefsColumn.implicitHeight + 36

                            ColumnLayout {
                                id: systemMediaPrefsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                Label {
                                    Layout.fillWidth: true
                                    text: "System Media"
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                CheckBox {
                                    text: "Enable KDE media controls and media buttons"
                                    focusPolicy: Qt.NoFocus
                                    checked: uiBridge.systemMediaControlsEnabled
                                    onToggled: uiBridge.setSystemMediaControlsEnabled(checked)
                                }

                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.Wrap
                                    color: Kirigami.Theme.disabledTextColor
                                    text: "When enabled, Ferrous appears in Plasma's media controls and responds to Play/Pause, Previous, Next, and Stop media buttons. Keyboard volume buttons always control system volume, not Ferrous volume."
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: libraryRootNameDialog
        modal: true
        title: pendingLibraryRootDialogMode === "rename"
            ? "Rename Library Root"
            : "Add Library Root"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: Math.min(560, root.width - 80)
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        onOpened: {
            libraryRootNameField.forceActiveFocus()
            libraryRootNameField.selectAll()
        }
        onAccepted: {
            const resolvedPath = pendingLibraryRootPath || ""
            const resolvedName = (libraryRootNameField.text || "").trim()
            if (resolvedPath.length > 0) {
                if (pendingLibraryRootDialogMode === "rename") {
                    uiBridge.setLibraryRootName(resolvedPath, resolvedName)
                } else {
                    uiBridge.addLibraryRoot(resolvedPath, resolvedName)
                }
            }
            root.resetLibraryRootNameDialog()
        }
        onRejected: root.resetLibraryRootNameDialog()

        contentItem: ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: "Path"
                color: root.uiMutedTextColor
            }
            TextField {
                Layout.fillWidth: true
                readOnly: true
                text: pendingLibraryRootPath || ""
                selectByMouse: true
            }

            Label {
                Layout.fillWidth: true
                text: "Custom Name (optional)"
                color: root.uiMutedTextColor
            }
            TextField {
                id: libraryRootNameField
                Layout.fillWidth: true
                text: pendingLibraryRootName || ""
                placeholderText: "Leave blank to use the path"
                selectByMouse: true
                onAccepted: libraryRootNameDialog.accept()
            }
        }
    }

    Dialog {
        id: globalSearchDialog
        modal: true
        title: "Global Search"
        standardButtons: Dialog.Close
        width: Math.min(1240, root.width - 64)
        height: Math.min(720, root.height - 80)
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        onOpened: {
            root.globalSearchOpening = false
            root.globalSearchIgnoreRefocusFind = true
            globalSearchOpenSettleTimer.restart()
            root.syncGlobalSearchSelectionAfterResultsChange()
            root.focusGlobalSearchQueryField(false)
            root.applyGlobalSearchOpenText()
            uiBridge.setGlobalSearchQuery(globalSearchQueryField.text || "")
        }
        onClosed: {
            root.endGlobalSearchOpen(true)
        }

        contentItem: ColumnLayout {
            spacing: 8

            TextField {
                id: globalSearchQueryField
                Layout.fillWidth: true
                placeholderText: "Type artist, album, or track"
                onTextChanged: {
                    uiBridge.setGlobalSearchQuery(text)
                }
                Keys.onPressed: function(event) {
                    if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
                        root.focusGlobalSearchQueryField(!root.globalSearchIgnoreRefocusFind)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                        root.navigateGlobalSearchSelectionToLibrary()
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Down) {
                        const next = root.nextSearchSelectableIndex(
                            root.globalSearchSelectedDisplayIndex,
                            1,
                            true)
                        if (next >= 0) {
                            root.selectGlobalSearchDisplayIndex(next)
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Up) {
                        const next = root.nextSearchSelectableIndex(
                            root.globalSearchSelectedDisplayIndex,
                            -1,
                            true)
                        if (next >= 0) {
                            root.selectGlobalSearchDisplayIndex(next)
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageDown) {
                        if (root.moveGlobalSearchSelectionByPage(1)) {
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_PageUp) {
                        if (root.moveGlobalSearchSelectionByPage(-1)) {
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Home) {
                        const first = root.searchFirstSelectableIndex()
                        if (first >= 0) {
                            root.selectGlobalSearchDisplayIndex(first)
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_End) {
                        const last = root.searchLastSelectableIndex()
                        if (last >= 0) {
                            root.selectGlobalSearchDisplayIndex(last)
                            globalSearchResultsView.forceActiveFocus()
                        }
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        root.activateGlobalSearchSelection()
                        event.accepted = true
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                color: root.uiMutedTextColor
                text: "Artists: " + (uiBridge.globalSearchArtistCount || 0)
                    + " | Albums: " + (uiBridge.globalSearchAlbumCount || 0)
                    + " | Tracks: " + (uiBridge.globalSearchTrackCount || 0)
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: root.uiSurfaceRaisedColor
                border.color: root.uiBorderColor

                ListView {
                    id: globalSearchResultsView
                    anchors.fill: parent
                    clip: true
                    model: uiBridge.globalSearchModel || []
                    reuseItems: true
                    spacing: 0
                    boundsBehavior: Flickable.StopAtBounds
                    boundsMovement: Flickable.StopAtBounds
                    flickDeceleration: root.snappyScrollFlickDeceleration
                    maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                    pixelAligned: true
                    readonly property int reservedRightPadding: (globalSearchResultsScrollBar.visible
                        ? globalSearchResultsScrollBar.width + 8
                        : 8)
                    ScrollBar.vertical: ScrollBar {
                        id: globalSearchResultsScrollBar
                        policy: ScrollBar.AsNeeded
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton
                        preventStealing: true
                        onWheel: function(wheel) {
                            root.stepGlobalSearchResultsView(wheel)
                        }
                    }

                    Keys.onPressed: function(event) {
                        if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_F) {
                            root.focusGlobalSearchQueryField(!root.globalSearchIgnoreRefocusFind)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                            root.navigateGlobalSearchSelectionToLibrary()
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Down) {
                            const next = root.nextSearchSelectableIndex(
                                root.globalSearchSelectedDisplayIndex,
                                1,
                                true)
                            if (next >= 0) {
                                root.selectGlobalSearchDisplayIndex(next)
                            }
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Up) {
                            const next = root.nextSearchSelectableIndex(
                                root.globalSearchSelectedDisplayIndex,
                                -1,
                                true)
                            if (next >= 0) {
                                root.selectGlobalSearchDisplayIndex(next)
                            }
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_PageDown) {
                            root.moveGlobalSearchSelectionByPage(1)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_PageUp) {
                            root.moveGlobalSearchSelectionByPage(-1)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Home) {
                            const first = root.searchFirstSelectableIndex()
                            if (first >= 0) {
                                root.selectGlobalSearchDisplayIndex(first)
                            }
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_End) {
                            const last = root.searchLastSelectableIndex()
                            if (last >= 0) {
                                root.selectGlobalSearchDisplayIndex(last)
                            }
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            root.activateGlobalSearchSelection()
                            event.accepted = true
                        }
                    }

                    delegate: Rectangle {
                        readonly property string rowKind: kind || ""
                        readonly property string rowTypeValue: rowType || ""
                        readonly property string sectionTitleValue: sectionTitle || ""
                        readonly property string labelValue: label || ""
                        readonly property string artistValue: artist || ""
                        readonly property string albumValue: album || ""
                        readonly property string rootLabelValue: rootLabel || ""
                        readonly property string genreValue: genre || ""
                        readonly property string coverUrlValue: coverUrl || ""
                        readonly property string lengthTextValue: lengthText || ""
                        readonly property var yearValue: year
                        readonly property var trackNumberValue: trackNumber
                        readonly property var countValue: count
                        readonly property color rowTextColor: index === root.globalSearchSelectedDisplayIndex
                            ? root.uiSelectionTextColor
                            : root.uiTextColor
                        width: Math.max(
                            0,
                            ListView.view.width - (globalSearchResultsView.reservedRightPadding || 0))
                        height: rowKind === "section" ? 30 : 24
                        color: rowKind === "section"
                            ? root.uiSectionColor
                            : (rowKind === "columns"
                                ? root.uiColumnsColor
                                : (index === root.globalSearchSelectedDisplayIndex
                                    ? root.uiSelectionColor
                                    : (index % 2 === 0
                                        ? root.uiSurfaceRaisedColor
                                        : root.uiSurfaceAltColor)))

                        border.width: rowKind === "item" ? 0 : 1
                        border.color: rowKind === "section"
                            ? Qt.darker(root.uiSectionColor, 1.12)
                            : (rowKind === "columns"
                                ? Qt.darker(root.uiColumnsColor, 1.1)
                                : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            Label {
                                visible: rowKind === "section"
                                Layout.fillWidth: true
                                text: sectionTitleValue || ""
                                font.weight: Font.DemiBold
                                color: root.uiTextColor
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "artist"
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    text: "Name"
                                    Layout.fillWidth: true
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "album"
                                Layout.fillWidth: true
                                spacing: 8
                                Label { text: ""; Layout.preferredWidth: root.globalSearchCoverColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Title"; Layout.fillWidth: true; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Artist"; Layout.preferredWidth: root.globalSearchArtistColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Genre"
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                                Label { text: "#"; Layout.preferredWidth: root.globalSearchAlbumCountColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor; horizontalAlignment: Text.AlignRight }
                                Label {
                                    text: "Length"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                            }

                            RowLayout {
                                visible: rowKind === "columns" && rowTypeValue === "track"
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    text: "#"
                                    Layout.preferredWidth: root.globalSearchTrackNumberColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label { text: "Title"; Layout.fillWidth: true; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Artist"; Layout.preferredWidth: root.globalSearchArtistColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: ""; Layout.preferredWidth: root.globalSearchCoverColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label { text: "Album"; Layout.preferredWidth: root.globalSearchAlbumColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: "Root"
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                                Label {
                                    text: "Year"
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                    horizontalAlignment: Text.AlignRight
                                }
                                Label {
                                    text: "Genre"
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    font.weight: Font.DemiBold
                                    color: root.uiMutedTextColor
                                }
                                Label { text: "Length"; Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth; font.weight: Font.DemiBold; color: root.uiMutedTextColor; horizontalAlignment: Text.AlignRight }
                            }

                            Loader {
                                visible: rowKind === "item"
                                Layout.fillWidth: true
                                sourceComponent: rowTypeValue === "artist"
                                    ? globalSearchArtistItemComponent
                                    : (rowTypeValue === "album"
                                        ? globalSearchAlbumItemComponent
                                        : globalSearchTrackItemComponent)
                            }
                        }

                        Component {
                            id: globalSearchArtistItemComponent
                            RowLayout {
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: labelValue || ""
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: rootLabelValue || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                            }
                        }

                        Component {
                            id: globalSearchAlbumItemComponent
                            RowLayout {
                                spacing: 8
                                Item {
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    Layout.preferredHeight: 20
                                    Image {
                                        anchors.fill: parent
                                        source: coverUrlValue || ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: true
                                        sourceSize.width: 32
                                        sourceSize.height: 32
                                    }
                                }
                                Label {
                                    text: labelValue || ""
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: artistValue || ""
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: rootLabelValue || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: genreValue || ""
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: countValue !== undefined ? countValue : ""
                                    Layout.preferredWidth: root.globalSearchAlbumCountColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: lengthTextValue || "--:--"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                            }
                        }

                        Component {
                            id: globalSearchTrackItemComponent
                            RowLayout {
                                spacing: 8
                                Label {
                                    text: trackNumberValue !== undefined && trackNumberValue !== null
                                        ? String(trackNumberValue).padStart(2, "0")
                                        : ""
                                    Layout.preferredWidth: root.globalSearchTrackNumberColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: labelValue || ""
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: artistValue || ""
                                    Layout.preferredWidth: root.globalSearchArtistColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Item {
                                    Layout.preferredWidth: root.globalSearchCoverColumnWidth
                                    Layout.preferredHeight: 18
                                    Image {
                                        anchors.fill: parent
                                        source: coverUrlValue || ""
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                        cache: true
                                        sourceSize.width: 24
                                        sourceSize.height: 24
                                    }
                                }
                                Label {
                                    text: albumValue || ""
                                    Layout.preferredWidth: root.globalSearchAlbumColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    visible: root.globalSearchShowsRootColumn
                                    text: rootLabelValue || ""
                                    Layout.preferredWidth: root.globalSearchRootColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: yearValue !== undefined && yearValue !== null ? yearValue : ""
                                    Layout.preferredWidth: root.globalSearchYearColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: genreValue || ""
                                    Layout.preferredWidth: root.globalSearchTrackGenreColumnWidth
                                    elide: Text.ElideRight
                                    color: rowTextColor
                                }
                                Label {
                                    text: lengthTextValue || "--:--"
                                    Layout.preferredWidth: root.globalSearchTrackLengthColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: rowTextColor
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: rowKind === "item"
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(mouse) {
                                root.selectGlobalSearchDisplayIndex(index)
                                if (mouse.button === Qt.RightButton) {
                                    root.globalSearchContextRowData = globalSearchModelApi
                                        ? globalSearchModelApi.rowDataAt(index)
                                        : ({})
                                    globalSearchContextMenu.popup()
                                    return
                                }
                                if (mouse.button === Qt.LeftButton) {
                                    globalSearchResultsView.forceActiveFocus()
                                }
                            }
                            onDoubleClicked: function(mouse) {
                                if (mouse.button === Qt.LeftButton) {
                                    root.selectGlobalSearchDisplayIndex(index)
                                    root.activateGlobalSearchSelection()
                                }
                            }
                        }

                    }
                }

                Menu {
                    id: globalSearchContextMenu
                    property var rowData: root.globalSearchContextRowData || ({})
                    enter: Transition {
                        NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                    }
                    exit: Transition {
                        NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                    }

                    MenuItem {
                        text: "Play"
                        enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                        onTriggered: root.activateGlobalSearchRow(globalSearchContextMenu.rowData)
                    }
                    MenuItem {
                        text: "Queue"
                        enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                        onTriggered: root.queueGlobalSearchRow(globalSearchContextMenu.rowData)
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "Open in " + uiBridge.fileBrowserName
                        visible: (globalSearchContextMenu.rowData.rowType || "") !== "track"
                        enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                        onTriggered: root.openGlobalSearchRowInFileBrowser(globalSearchContextMenu.rowData)
                    }
                    MenuItem {
                        text: "Open containing folder"
                        visible: (globalSearchContextMenu.rowData.rowType || "") === "track"
                        enabled: (globalSearchContextMenu.rowData.kind || "") === "item"
                        onTriggered: root.openGlobalSearchRowInFileBrowser(globalSearchContextMenu.rowData)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: (uiBridge.globalSearchArtistCount || 0) === 0
                    && (uiBridge.globalSearchAlbumCount || 0) === 0
                    && (uiBridge.globalSearchTrackCount || 0) === 0
                text: (globalSearchQueryField.text || "").trim().length === 0
                    ? "Type to search"
                    : "No matches"
                color: Kirigami.Theme.disabledTextColor
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    Dialog {
        id: diagnosticsDialog
        modal: true
        title: "Diagnostics"
        standardButtons: Dialog.Close
        width: Math.min(980, root.width - 80)
        height: Math.min(680, root.height - 80)
        enter: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        exit: Transition {
            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
        }
        onOpened: {
            uiBridge.reloadDiagnosticsFromDisk()
            root.refreshDiagnosticsTextView()
        }
        onClosed: {
            if (diagnosticsTextArea) {
                diagnosticsTextArea.text = ""
            }
        }

        contentItem: ColumnLayout {
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Log file:"
                    color: Kirigami.Theme.disabledTextColor
                }
                TextField {
                    Layout.fillWidth: true
                    readOnly: true
                    text: uiBridge.diagnosticsLogPath || ""
                    selectByMouse: true
                }
                Button {
                    text: "Open Folder"
                    enabled: (uiBridge.diagnosticsLogPath || "").length > 0
                    onClicked: uiBridge.openContainingFolder(uiBridge.diagnosticsLogPath || "")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: "Reload"
                    onClicked: {
                        uiBridge.reloadDiagnosticsFromDisk()
                        root.refreshDiagnosticsTextView()
                    }
                }
                Button {
                    text: "Clear"
                    onClicked: {
                        uiBridge.clearDiagnostics()
                        root.refreshDiagnosticsTextView()
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Copy All"
                    onClicked: {
                        if ((diagnosticsTextArea.text || "").length > 0) {
                            diagnosticsTextArea.selectAll()
                            diagnosticsTextArea.copy()
                        }
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                TextArea {
                    id: diagnosticsTextArea
                    text: ""
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    font.family: "Monospace"
                    persistentSelection: true

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.RightButton
                        propagateComposedEvents: true
                        cursorShape: Qt.IBeamCursor
                        onPressed: function(mouse) {
                            if (mouse.button !== Qt.RightButton) {
                                mouse.accepted = false
                            }
                        }
                        onClicked: function(mouse) {
                            if (mouse.button === Qt.RightButton) {
                                diagnosticsTextArea.forceActiveFocus()
                                diagnosticsContextMenu.popup()
                            } else {
                                mouse.accepted = false
                            }
                        }
                    }

                    Menu {
                        id: diagnosticsContextMenu
                        enter: Transition {
                            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                        }
                        exit: Transition {
                            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                        }

                        MenuItem {
                            text: "Copy"
                            enabled: (diagnosticsTextArea.selectedText || "").length > 0
                            onTriggered: diagnosticsTextArea.copy()
                        }
                        MenuItem {
                            text: "Select All"
                            enabled: (diagnosticsTextArea.text || "").length > 0
                            onTriggered: diagnosticsTextArea.selectAll()
                        }
                        MenuItem {
                            text: "Copy All"
                            enabled: (diagnosticsTextArea.text || "").length > 0
                            onTriggered: {
                                diagnosticsTextArea.selectAll()
                                diagnosticsTextArea.copy()
                            }
                        }
                    }
                }
            }
        }
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

    footer: ToolBar {
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        leftPadding: 14
        rightPadding: 10
        topPadding: 2
        bottomPadding: 2

        contentItem: RowLayout {
            spacing: 6

            Repeater {
                model: statusBarSections()

                delegate: RowLayout {
                    required property int index
                    required property var modelData

                    spacing: 6
                    Layout.fillWidth: !!modelData.stretch

                    Label {
                        visible: index > 0
                        text: "|"
                        color: root.uiMutedTextColor
                    }

                    RowLayout {
                        readonly property string channelIconSource: root.channelStatusIconSource(modelData.iconKey || "")
                        spacing: channelIconSource.length > 0 ? 4 : 0
                        Layout.fillWidth: !!modelData.stretch

                        Item {
                            id: channelIconItem
                            visible: parent.channelIconSource.length > 0
                            Layout.preferredWidth: visible ? 22 : 0
                            Layout.preferredHeight: 20
                            property url iconSource: parent.channelIconSource.length > 0
                                ? parent.channelIconSource
                                : ""

                            Image {
                                anchors.fill: parent
                                source: channelIconItem.iconSource
                                asynchronous: false
                                fillMode: Image.PreserveAspectFit
                                smooth: false
                                sourceSize.width: 44
                                sourceSize.height: 40
                            }
                        }

                        Label {
                            Layout.fillWidth: !!modelData.stretch
                            text: modelData.text || ""
                            elide: Text.ElideRight
                            color: modelData.kind === "error"
                                ? (modelData.emphasis
                                    ? root.mixColor(
                                        Kirigami.Theme.negativeTextColor,
                                        root.uiTextColor,
                                        root.themeIsDark ? 0.18 : 0.08)
                                    : Kirigami.Theme.negativeTextColor)
                                : (modelData.emphasis
                                    ? Kirigami.Theme.highlightColor
                                    : root.uiTextColor)
                            font.weight: modelData.emphasis ? Font.DemiBold : Font.Normal
                        }
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            id: transportBar
            Layout.fillWidth: true
            implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
            leftPadding: 8
            rightPadding: 12
            topPadding: 4
            bottomPadding: 4

            contentItem: RowLayout {
                anchors.fill: parent
                anchors.leftMargin: transportBar.leftPadding
                anchors.rightMargin: transportBar.rightPadding
                anchors.topMargin: transportBar.topPadding
                anchors.bottomMargin: transportBar.bottomPadding
                spacing: 8

                RowLayout {
                    spacing: 2
                    ToolButton { action: previousAction; display: AbstractButton.IconOnly }
                    ToolButton { action: playAction; display: AbstractButton.IconOnly }
                    ToolButton { action: pauseAction; display: AbstractButton.IconOnly }
                    ToolButton { action: stopAction; display: AbstractButton.IconOnly }
                    ToolButton { action: nextAction; display: AbstractButton.IconOnly }
                }

                Slider {
                    id: seekSlider
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(uiBridge.durationSeconds, 1.0)
                    readonly property bool durationKnown: uiBridge.durationSeconds > 1.0
                    readonly property bool seekAllowed: durationKnown && uiBridge.playbackState !== "Stopped"
                    readonly property real stableVisualPosition: seekAllowed ? visualPosition : 0.0
                    enabled: seekAllowed
                    stepSize: 0
                    onPressedChanged: {
                        if (!pressed && seekAllowed) {
                            root.positionSmoothingAnimationMs = 0
                            root.displayedPositionSeconds = value
                            root.positionSmoothingPrimed = true
                            root.positionSmoothingAnchorSeconds = value
                            root.positionSmoothingLastMs = Date.now()
                            uiBridge.seek(value)
                        }
                    }

                    background: Item {
                        implicitHeight: 24
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.fill: parent
                            color: "white"
                            border.color: "#a0a9b3"
                            radius: 1
                        }

                        WaveformItem {
                            id: waveformItem
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: uiBridge.playbackState !== "Stopped"
                            peaksData: uiBridge.playbackState === "Stopped"
                                       ? ""
                                       : uiBridge.waveformPeaksPacked
                            generatedSeconds: uiBridge.waveformCoverageSeconds
                            waveformComplete: uiBridge.waveformComplete
                            positionSeconds: root.displayedPositionSeconds
                            durationSeconds: uiBridge.durationSeconds
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            visible: seekSlider.seekAllowed
                            width: Math.round(parent.width * seekSlider.stableVisualPosition)
                            color: Qt.rgba(120 / 255, 190 / 255, 1.0, 0.26)
                        }

                        Rectangle {
                            visible: seekSlider.seekAllowed
                            width: 1
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            x: Math.round(seekSlider.stableVisualPosition * (parent.width - 1))
                            color: "#2f7cd6"
                        }
                    }

                    handle: Rectangle {
                        visible: seekSlider.seekAllowed
                        x: seekSlider.leftPadding + seekSlider.stableVisualPosition * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                        implicitWidth: 3
                        implicitHeight: seekSlider.height - 4
                        radius: 1
                        color: "#2f7cd6"
                        border.color: "#1f5aa7"
                    }

                    Item {
                        id: seekDragOverlay
                        visible: seekSlider.pressed && seekSlider.seekAllowed
                        z: 20
                        property real playheadX: seekSlider.leftPadding
                            + seekSlider.stableVisualPosition * seekSlider.availableWidth
                        property real leftCandidateX: playheadX - width - 8
                        property real rightCandidateX: playheadX + 8
                        width: dragTimeLabel.implicitWidth + 14
                        height: Math.max(18, seekSlider.availableHeight - 4)
                        y: seekSlider.topPadding + (seekSlider.availableHeight - height) / 2
                        x: {
                            const minX = 2
                            const maxX = seekSlider.width - width - 2
                            if (leftCandidateX >= minX) {
                                return Math.min(maxX, leftCandidateX)
                            }
                            return Math.max(minX, Math.min(maxX, rightCandidateX))
                        }

                        Rectangle {
                            id: bubbleRect
                            anchors.fill: parent
                            radius: 2
                            color: Qt.rgba(52 / 255, 137 / 255, 235 / 255, 0.76)
                            border.color: Qt.rgba(198 / 255, 229 / 255, 1.0, 0.52)

                            Label {
                                id: dragTimeLabel
                                anchors.centerIn: parent
                                text: root.formatSeekTime(seekSlider.value)
                                color: "white"
                            }
                        }
                    }
                }

                Binding {
                    target: seekSlider
                    property: "value"
                    value: seekSlider.durationKnown ? root.displayedPositionSeconds : 0
                    when: !seekSlider.pressed
                }

                Label {
                    text: uiBridge.positionText + "/" + uiBridge.durationText
                    horizontalAlignment: Text.AlignHCenter
                    Layout.preferredWidth: 96
                    Layout.alignment: Qt.AlignVCenter
                }

                ToolButton {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    display: AbstractButton.IconOnly
                    flat: true
                    icon.name: (root.volumeMuted || root.normalizedVolumeValue(uiBridge.volume) <= 0.0001)
                        ? "audio-volume-muted"
                        : "audio-volume-high"
                    icon.color: root.mixColor(root.uiTextColor, "#ffffff", root.themeIsDark ? 0.16 : 0.04)
                    onClicked: root.toggleMutedVolume()
                }

                Slider {
                    id: volumeSlider
                    Layout.preferredWidth: 140
                    from: 0
                    to: 1
                    stepSize: 0
                    onMoved: root.setAppVolume(value)
                    onPressedChanged: {
                        if (!pressed) {
                            root.setAppVolume(value)
                        }
                    }
                }

                Binding {
                    target: volumeSlider
                    property: "value"
                    value: uiBridge.volume
                    when: !volumeSlider.pressed
                }
            }
        }

        SplitView {
            id: mainSplit
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            Rectangle {
                color: root.uiPaneColor
                SplitView.preferredWidth: Math.max(300, root.width * 0.26)
                SplitView.minimumWidth: 250

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: width
                        color: "#0c0c0c"

                        Image {
                            id: albumArtImage
                            anchors.fill: parent
                            source: uiBridge.currentTrackCoverPath
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            asynchronous: true
                            cache: true
                            retainWhileLoading: true
                            sourceSize.width: Math.max(256, width)
                            sourceSize.height: Math.max(256, height)
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "Album Art"
                            color: "#8c8c8c"
                            visible: uiBridge.currentTrackCoverPath.length === 0
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onPressed: function(mouse) {
                                if (mouse.button === Qt.RightButton) {
                                    nowPlayingAlbumArtMenu.popup()
                                }
                            }
                            onDoubleClicked: function(mouse) {
                                if (mouse.button === Qt.LeftButton) {
                                    root.openAlbumArtViewer()
                                }
                            }
                        }

                        Menu {
                            id: nowPlayingAlbumArtMenu
                            MenuItem { action: replaceFromItunesAction }
                            MenuItem {
                                enabled: false
                                visible: !replaceFromItunesAction.enabled
                                text: root.currentTrackItunesArtworkDisabledReason()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: root.uiPaneColor
                        border.color: root.uiBorderColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 6
                            spacing: 6

                            Rectangle {
                                id: nowPlayingCard
                                Layout.fillWidth: true
                                readonly property bool hasTrackContext: {
                                    const hasResolvedMetadata = (uiBridge.currentTrackTitle || "").trim().length > 0
                                        || (uiBridge.currentTrackArtist || "").trim().length > 0
                                        || (uiBridge.currentTrackAlbum || "").trim().length > 0
                                    const playbackState = (uiBridge.playbackState || "").trim()
                                    const hasActivePath = playbackState !== "Stopped"
                                        && (uiBridge.currentTrackPath || "").trim().length > 0
                                    return hasResolvedMetadata || hasActivePath
                                }
                                readonly property string marqueeResetKey: {
                                    return (uiBridge.currentTrackPath || "")
                                        + "|"
                                        + (uiBridge.currentTrackTitle || "")
                                        + "|"
                                        + (uiBridge.currentTrackArtist || "")
                                        + "|"
                                        + (uiBridge.currentTrackAlbum || "")
                                }
                                readonly property string resolvedTitle: {
                                    if (!hasTrackContext) {
                                        return "No track loaded"
                                    }
                                    const explicitTitle = (uiBridge.currentTrackTitle || "").trim()
                                    if (explicitTitle.length > 0) {
                                        return explicitTitle
                                    }
                                    const pathValue = (uiBridge.currentTrackPath || "").trim()
                                    if (pathValue.length > 0) {
                                        const normalized = pathValue.replace(/\\/g, "/")
                                        const parts = normalized.split("/")
                                        const tail = parts.length > 0 ? parts[parts.length - 1] : ""
                                        return tail.length > 0 ? tail : pathValue
                                    }
                                    return "Nothing playing"
                                }
                                readonly property string resolvedArtist: {
                                    if (!hasTrackContext) {
                                        return "—"
                                    }
                                    const artistValue = (uiBridge.currentTrackArtist || "").trim()
                                    return artistValue.length > 0 ? artistValue : "Unknown artist"
                                }
                                readonly property string resolvedAlbum: {
                                    if (!hasTrackContext) {
                                        return "—"
                                    }
                                    const albumValue = (uiBridge.currentTrackAlbum || "").trim()
                                    return albumValue.length > 0 ? albumValue : "Unknown album"
                                }
                                readonly property string resolvedGenre: {
                                    if (!hasTrackContext) {
                                        return "—"
                                    }
                                    const genreValue = (uiBridge.currentTrackGenre || "").trim()
                                    return genreValue.length > 0 ? genreValue : "Unknown genre"
                                }
                                readonly property string resolvedTrackNumber: {
                                    if (!hasTrackContext) {
                                        return "—"
                                    }
                                    if (uiBridge.playingQueueIndex !== undefined
                                            && uiBridge.playingQueueIndex !== null
                                            && uiBridge.playingQueueIndex >= 0) {
                                        return root.queueTrackNumberText(uiBridge.playingQueueIndex)
                                    }
                                    if (uiBridge.selectedQueueIndex !== undefined
                                            && uiBridge.selectedQueueIndex !== null
                                            && uiBridge.selectedQueueIndex >= 0) {
                                        return root.queueTrackNumberText(uiBridge.selectedQueueIndex)
                                    }
                                    return "--"
                                }
                                readonly property string resolvedYear: {
                                    if (!hasTrackContext) {
                                        return "—"
                                    }
                                    const yearValue = uiBridge.currentTrackYear
                                    if (yearValue !== undefined && yearValue !== null && String(yearValue).length > 0) {
                                        return String(yearValue)
                                    }
                                    return "----"
                                }
                                implicitHeight: nowPlayingColumn.implicitHeight + 12
                                radius: 6
                                color: root.uiSurfaceRaisedColor
                                border.color: root.uiBorderColor

                                ColumnLayout {
                                    id: nowPlayingColumn
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Title:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Item {
                                            id: titleMarquee
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 18
                                            clip: true
                                            property string resetKey: nowPlayingCard.marqueeResetKey
                                            property real overflowPx: Math.max(0, titleInfoText.implicitWidth - width)
                                            property real offsetPx: 0
                                            onOverflowPxChanged: {
                                                if (overflowPx <= 1) {
                                                    offsetPx = 0
                                                } else if (offsetPx > overflowPx) {
                                                    offsetPx = overflowPx
                                                }
                                            }
                                            onResetKeyChanged: {
                                                offsetPx = 0
                                                if (titleMarqueeAnimation.running) {
                                                    titleMarqueeAnimation.restart()
                                                }
                                            }

                                            Text {
                                                id: titleInfoText
                                                anchors.verticalCenter: titleMarquee.verticalCenter
                                                x: -titleMarquee.offsetPx
                                                text: nowPlayingCard.resolvedTitle
                                                font.weight: Font.DemiBold
                                                font.pixelSize: 12
                                                color: root.uiTextColor
                                                textFormat: Text.PlainText
                                            }

                                            SequentialAnimation {
                                                id: titleMarqueeAnimation
                                                running: titleMarquee.visible
                                                    && titleMarquee.overflowPx > 1
                                                    && root.visible
                                                loops: Animation.Infinite
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: titleMarquee
                                                    property: "offsetPx"
                                                    to: titleMarquee.overflowPx
                                                    duration: Math.max(900, titleMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: titleMarquee.offsetPx = titleMarquee.overflowPx
                                                }
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: titleMarquee
                                                    property: "offsetPx"
                                                    to: 0
                                                    duration: Math.max(900, titleMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: titleMarquee.offsetPx = 0
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Artist:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Item {
                                            id: artistMarquee
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 18
                                            clip: true
                                            property string resetKey: nowPlayingCard.marqueeResetKey
                                            property real overflowPx: Math.max(0, artistInfoText.implicitWidth - width)
                                            property real offsetPx: 0
                                            onOverflowPxChanged: {
                                                if (overflowPx <= 1) {
                                                    offsetPx = 0
                                                } else if (offsetPx > overflowPx) {
                                                    offsetPx = overflowPx
                                                }
                                            }
                                            onResetKeyChanged: {
                                                offsetPx = 0
                                                if (artistMarqueeAnimation.running) {
                                                    artistMarqueeAnimation.restart()
                                                }
                                            }

                                            Text {
                                                id: artistInfoText
                                                anchors.verticalCenter: artistMarquee.verticalCenter
                                                x: -artistMarquee.offsetPx
                                                text: nowPlayingCard.resolvedArtist
                                                color: root.uiTextColor
                                                font.pixelSize: 12
                                                textFormat: Text.PlainText
                                            }

                                            SequentialAnimation {
                                                id: artistMarqueeAnimation
                                                running: artistMarquee.visible
                                                    && artistMarquee.overflowPx > 1
                                                    && root.visible
                                                loops: Animation.Infinite
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: artistMarquee
                                                    property: "offsetPx"
                                                    to: artistMarquee.overflowPx
                                                    duration: Math.max(900, artistMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: artistMarquee.offsetPx = artistMarquee.overflowPx
                                                }
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: artistMarquee
                                                    property: "offsetPx"
                                                    to: 0
                                                    duration: Math.max(900, artistMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: artistMarquee.offsetPx = 0
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Album:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Item {
                                            id: albumMarquee
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 18
                                            clip: true
                                            property string resetKey: nowPlayingCard.marqueeResetKey
                                            property real overflowPx: Math.max(0, albumInfoText.implicitWidth - width)
                                            property real offsetPx: 0
                                            onOverflowPxChanged: {
                                                if (overflowPx <= 1) {
                                                    offsetPx = 0
                                                } else if (offsetPx > overflowPx) {
                                                    offsetPx = overflowPx
                                                }
                                            }
                                            onResetKeyChanged: {
                                                offsetPx = 0
                                                if (albumMarqueeAnimation.running) {
                                                    albumMarqueeAnimation.restart()
                                                }
                                            }

                                            Text {
                                                id: albumInfoText
                                                anchors.verticalCenter: albumMarquee.verticalCenter
                                                x: -albumMarquee.offsetPx
                                                text: nowPlayingCard.resolvedAlbum
                                                color: root.uiTextColor
                                                font.pixelSize: 12
                                                textFormat: Text.PlainText
                                            }

                                            SequentialAnimation {
                                                id: albumMarqueeAnimation
                                                running: albumMarquee.visible
                                                    && albumMarquee.overflowPx > 1
                                                    && root.visible
                                                loops: Animation.Infinite
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: albumMarquee
                                                    property: "offsetPx"
                                                    to: albumMarquee.overflowPx
                                                    duration: Math.max(900, albumMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: albumMarquee.offsetPx = albumMarquee.overflowPx
                                                }
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: albumMarquee
                                                    property: "offsetPx"
                                                    to: 0
                                                    duration: Math.max(900, albumMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: albumMarquee.offsetPx = 0
                                                }
                                            }
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Track:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: nowPlayingCard.resolvedTrackNumber
                                            elide: Text.ElideRight
                                            color: root.uiTextColor
                                            font.pixelSize: 12
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Year:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: nowPlayingCard.resolvedYear
                                            elide: Text.ElideRight
                                            color: root.uiTextColor
                                            font.pixelSize: 12
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        Label {
                                            text: "Genre:"
                                            Layout.preferredWidth: 44
                                            horizontalAlignment: Text.AlignRight
                                            color: root.uiMutedTextColor
                                            font.pixelSize: 12
                                        }
                                        Item {
                                            id: genreMarquee
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 18
                                            clip: true
                                            property string resetKey: nowPlayingCard.marqueeResetKey
                                            property real overflowPx: Math.max(0, genreInfoText.implicitWidth - width)
                                            property real offsetPx: 0
                                            onOverflowPxChanged: {
                                                if (overflowPx <= 1) {
                                                    offsetPx = 0
                                                } else if (offsetPx > overflowPx) {
                                                    offsetPx = overflowPx
                                                }
                                            }
                                            onResetKeyChanged: {
                                                offsetPx = 0
                                                if (genreMarqueeAnimation.running) {
                                                    genreMarqueeAnimation.restart()
                                                }
                                            }

                                            Text {
                                                id: genreInfoText
                                                anchors.verticalCenter: genreMarquee.verticalCenter
                                                x: -genreMarquee.offsetPx
                                                text: nowPlayingCard.resolvedGenre
                                                color: root.uiTextColor
                                                font.pixelSize: 12
                                                textFormat: Text.PlainText
                                            }

                                            SequentialAnimation {
                                                id: genreMarqueeAnimation
                                                running: genreMarquee.visible
                                                    && genreMarquee.overflowPx > 1
                                                    && root.visible
                                                loops: Animation.Infinite
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: genreMarquee
                                                    property: "offsetPx"
                                                    to: genreMarquee.overflowPx
                                                    duration: Math.max(900, genreMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: genreMarquee.offsetPx = genreMarquee.overflowPx
                                                }
                                                PauseAnimation { duration: 1400 }
                                                NumberAnimation {
                                                    target: genreMarquee
                                                    property: "offsetPx"
                                                    to: 0
                                                    duration: Math.max(900, genreMarquee.overflowPx * 24)
                                                    easing.type: Easing.Linear
                                                }
                                                ScriptAction {
                                                    script: genreMarquee.offsetPx = 0
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    readonly property int scanBacklog: Math.max(
                                        0,
                                        uiBridge.libraryScanDiscovered - uiBridge.libraryScanProcessed)
                                    text: "Artists: " + uiBridge.libraryArtistCount
                                          + " | albums: " + uiBridge.libraryAlbumCount
                                          + " | tracks: " + uiBridge.libraryTrackCount
                                          + (uiBridge.libraryScanInProgress
                                              ? (" | scanning " + uiBridge.libraryScanProcessed
                                                 + (scanBacklog > 0
                                                     ? (" (+" + scanBacklog + " queued)")
                                                     : "")
                                                 + (uiBridge.libraryScanFilesPerSecond > 0
                                                     ? (" @ " + uiBridge.libraryScanFilesPerSecond.toFixed(1) + "/s")
                                                     : "")
                                                 + (uiBridge.libraryScanEtaSeconds >= 0
                                                     ? (" ETA " + Math.ceil(uiBridge.libraryScanEtaSeconds) + "s")
                                                     : ""))
                                              : "")
                                    color: Kirigami.Theme.disabledTextColor
                                    elide: Text.ElideRight
                                }
                            }

                            ListView {
                                id: libraryAlbumView
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: libraryModel
                                activeFocusOnTab: true
                                focus: true
                                reuseItems: true
                                cacheBuffer: 200
                                boundsBehavior: Flickable.StopAtBounds
                                boundsMovement: Flickable.StopAtBounds
                                flickDeceleration: root.snappyScrollFlickDeceleration
                                maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                                pixelAligned: true
                                onContentHeightChanged: {
                                    if (root.pendingLibraryExpandFitKey.length > 0) {
                                        Qt.callLater(function() {
                                            root.applyPendingLibraryExpansionFit()
                                        })
                                    }
                                }
                                ScrollBar.vertical: ScrollBar {
                                    policy: ScrollBar.AlwaysOn
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.NoButton
                                    preventStealing: true
                                    onWheel: function(wheel) {
                                        root.stepScrollView(libraryAlbumView, wheel, 24, 3)
                                    }
                                }
                                Keys.onPressed: function(event) {
                                    root.handleLibraryKeyPress(event)
                                }

                                delegate: Rectangle {
                                    id: libraryRow
                                    readonly property string rowTypeResolved: rowType || ""
                                    readonly property bool isAlbumRow: rowTypeResolved === "album"
                                    readonly property bool isTrackRow: rowTypeResolved === "track"
                                    readonly property bool hasChildren: !isTrackRow && (count || 0) > 0
                                    readonly property string selectionKeyResolved: selectionKey || ""
                                    readonly property string artistResolved: artist || ""
                                    readonly property string nameResolved: name || ""
                                    readonly property string trackPathResolved: trackPath || ""
                                    readonly property string openPathResolved: openPath || ""
                                    readonly property var playPathsResolved: playPaths || []
                                    readonly property bool draggableLibraryItem: isTrackRow
                                        || rowTypeResolved === "album"
                                        || rowTypeResolved === "artist"
                                        || playPathsResolved.length > 0
                                    readonly property string rowTitle: title || name || artist || ""
                                    readonly property bool albumCoverInViewport: isAlbumRow
                                        && (y + height >= libraryAlbumView.contentY - 48)
                                        && (y <= libraryAlbumView.contentY + libraryAlbumView.height + 48)
                                    readonly property int sourceIndexResolved: sourceIndex !== undefined ? sourceIndex : -1
                                    readonly property int depthResolved: depth !== undefined ? depth : 0
                                    width: ListView.view.width
                                    height: 24
                                    color: root.isLibrarySelectionKeySelected(selectionKey || "")
                                        ? root.uiSelectionColor
                                        : (index % 2 === 0
                                            ? root.uiSurfaceRaisedColor
                                            : root.uiSurfaceAltColor)

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 6
                                        anchors.rightMargin: 6
                                        spacing: 3

                                        Item {
                                            Layout.preferredWidth: Math.max(0, depthResolved * 18)
                                        }

                                        Label {
                                            id: expanderIcon
                                            Layout.preferredWidth: 24
                                            Layout.fillHeight: true
                                            Layout.alignment: Qt.AlignVCenter
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                            text: hasChildren ? (expanded ? "▾" : "▸") : ""
                                            font.pixelSize: 20
                                            font.bold: true
                                            color: root.isLibrarySelectionKeySelected(selectionKey || "")
                                                ? root.uiSelectionTextColor
                                                : root.uiMutedTextColor
                                        }

                                        Item {
                                            visible: isAlbumRow
                                            Layout.preferredWidth: 18
                                            Layout.preferredHeight: 18
                                            Layout.alignment: Qt.AlignVCenter

                                            Image {
                                                anchors.fill: parent
                                                source: albumCoverInViewport
                                                    ? uiBridge.libraryThumbnailSource(coverPath || "")
                                                    : ""
                                                fillMode: Image.PreserveAspectFit
                                                smooth: false
                                                asynchronous: true
                                                cache: true
                                                sourceSize.width: 32
                                                sourceSize.height: 32
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            Layout.alignment: Qt.AlignVCenter
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                            text: rowTitle
                                            color: root.isLibrarySelectionKeySelected(selectionKey || "")
                                                ? root.uiSelectionTextColor
                                                : root.uiTextColor
                                        }
                                    }

                                    Drag.active: libraryRowMouseArea.drag.active && draggableLibraryItem
                                    Drag.source: libraryRow
                                    Drag.hotSpot.x: 16
                                    Drag.hotSpot.y: height * 0.5
                                    Drag.dragType: Drag.Automatic
                                    Drag.supportedActions: Qt.CopyAction

                                    Item {
                                        id: libraryDragProxy
                                        visible: false
                                    }

                                    MouseArea {
                                        id: libraryRowMouseArea
                                        anchors.fill: parent
                                        preventStealing: true
                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        drag.target: draggableLibraryItem ? libraryDragProxy : null
                                        drag.smoothed: false
                                        onReleased: {
                                            libraryDragProxy.x = 0
                                            libraryDragProxy.y = 0
                                        }
                                        onClicked: function(mouse) {
                                            libraryAlbumView.forceActiveFocus()
                                            const rowMap = {
                                                selectionKey: selectionKeyResolved,
                                                sourceIndex: sourceIndexResolved,
                                                rowType: rowTypeResolved,
                                                artist: artist || "",
                                                name: name || "",
                                                title: rowTitle,
                                                trackPath: trackPathResolved,
                                                openPath: openPathResolved,
                                                playPaths: playPathsResolved
                                            }
                                            if (mouse.button === Qt.LeftButton
                                                    && hasChildren
                                                    && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                                                root.toggleLibraryNode(key)
                                                return
                                            }
                                            root.handleLibraryRowSelection(
                                                index,
                                                rowMap,
                                                mouse.button,
                                                mouse.modifiers || Qt.NoModifier)
                                            if (mouse.button === Qt.RightButton) {
                                                libraryContextMenu.rowMap = rowMap
                                                libraryContextMenu.popup()
                                            }
                                        }
                                        onDoubleClicked: function(mouse) {
                                            const rowMap = {
                                                selectionKey: selectionKeyResolved,
                                                sourceIndex: sourceIndexResolved,
                                                rowType: rowTypeResolved,
                                                artist: artist || "",
                                                name: name || "",
                                                title: rowTitle,
                                                trackPath: trackPathResolved,
                                                openPath: openPathResolved,
                                                playPaths: playPathsResolved
                                            }
                                            if (hasChildren
                                                    && mouse.x <= expanderIcon.x + expanderIcon.width + 6) {
                                                root.toggleLibraryNode(key)
                                                return
                                            }
                                            const rows = root.rowsForLibraryAction(rowMap)
                                            if (rows.length > 0) {
                                                root.playLibraryRows(rows)
                                            }
                                        }
                                    }

                                    Menu {
                                        id: libraryContextMenu
                                        property var rowMap: ({})
                                        enter: Transition {
                                            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                                        }
                                        exit: Transition {
                                            NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                                        }

                                        MenuItem {
                                            text: "Play"
                                            enabled: root.isActionableLibraryRow(libraryContextMenu.rowMap)
                                            onTriggered: {
                                                const rows = root.rowsForLibraryAction(libraryContextMenu.rowMap)
                                                if (rows.length > 0) {
                                                    root.playLibraryRows(rows)
                                                }
                                            }
                                        }
                                        MenuItem {
                                            text: "Queue"
                                            enabled: root.isActionableLibraryRow(libraryContextMenu.rowMap)
                                            onTriggered: {
                                                const rows = root.rowsForLibraryAction(libraryContextMenu.rowMap)
                                                if (rows.length > 0) {
                                                    root.appendLibraryRows(rows)
                                                }
                                            }
                                        }
                                        MenuItem {
                                            text: "Edit Tags"
                                            visible: root.canOpenTagEditorForLibrary(libraryContextMenu.rowMap)
                                            enabled: root.canOpenTagEditorForLibrary(libraryContextMenu.rowMap)
                                            onTriggered: root.openTagEditorForLibrary(libraryContextMenu.rowMap)
                                        }
                                        MenuSeparator {}
                                        MenuItem { action: playAllLibraryTracksAction }
                                        MenuItem { action: appendAllLibraryTracksAction }
                                        MenuSeparator {}
                                        MenuItem {
                                            text: "Open in " + uiBridge.fileBrowserName
                                            visible: libraryContextMenu.rowMap.rowType !== "track"
                                            enabled: (libraryContextMenu.rowMap.openPath || "").length > 0
                                            onTriggered: uiBridge.openInFileBrowser(libraryContextMenu.rowMap.openPath || "")
                                        }
                                        MenuItem {
                                            text: "Open containing folder"
                                            visible: libraryContextMenu.rowMap.rowType === "track"
                                            enabled: (libraryContextMenu.rowMap.trackPath || "").length > 0
                                            onTriggered: uiBridge.openContainingFolder(libraryContextMenu.rowMap.trackPath || "")
                                        }
                                    }
                                }
                            }

                                    Label {
                                        visible: libraryAlbumView.count === 0
                                        text: root.isLibraryTreeLoading()
                                            ? "Loading library..."
                                            : "Library is empty"
                                        color: root.uiMutedTextColor
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignHCenter
                                    }
                        }
                    }
                }
            }

            SplitView {
                orientation: Qt.Vertical
                SplitView.fillWidth: true

                Rectangle {
                    color: root.uiSurfaceRaisedColor
                    SplitView.fillWidth: true
                    SplitView.preferredHeight: root.height * 0.58
                    SplitView.minimumHeight: 220
                    border.color: root.uiBorderColor

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 26
                            color: root.uiHeaderColor
                            border.color: root.uiBorderColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8 + (playlistView ? playlistView.reservedRightPadding : 0)
                                Label {
                                    text: "▶"
                                    Layout.preferredWidth: root.playlistIndicatorColumnWidth
                                    horizontalAlignment: Text.AlignHCenter
                                    color: root.uiMutedTextColor
                                }
                                Label {
                                    text: "#"
                                    Layout.preferredWidth: root.playlistOrderColumnWidth
                                    horizontalAlignment: Text.AlignRight
                                    color: root.uiMutedTextColor
                                }
                                Label { text: "Title"; Layout.fillWidth: true; color: root.uiMutedTextColor }
                                Label { text: "Artist"; Layout.preferredWidth: 170; color: root.uiMutedTextColor }
                                Label { text: "Album"; Layout.preferredWidth: 190; color: root.uiMutedTextColor }
                                Label {
                                    text: "Length"
                                    Layout.preferredWidth: 76
                                    horizontalAlignment: Text.AlignRight
                                    color: root.uiMutedTextColor
                                }
                            }
                        }

                        ListView {
                            id: playlistView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            activeFocusOnTab: true
                            model: uiBridge.queueRows
                            boundsBehavior: Flickable.StopAtBounds
                            boundsMovement: Flickable.StopAtBounds
                            flickDeceleration: root.snappyScrollFlickDeceleration
                            maximumFlickVelocity: root.snappyScrollMaxFlickVelocity
                            pixelAligned: true
                            property real reservedRightPadding: playlistVerticalScrollBar.visible
                                ? (playlistVerticalScrollBar.width + 6)
                                : 0
                            onContentYChanged: root.applyPendingPlaylistViewportRestore()
                            onContentHeightChanged: root.applyPendingPlaylistViewportRestore()
                            onCountChanged: root.applyPendingPlaylistViewportRestore()
                            onHeightChanged: root.applyPendingPlaylistViewportRestore()
                            Keys.onPressed: function(event) {
                                root.handlePlaylistKeyPress(event)
                            }
                            ScrollBar.vertical: ScrollBar {
                                id: playlistVerticalScrollBar
                                policy: ScrollBar.AsNeeded
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                preventStealing: true
                                onWheel: function(wheel) {
                                    root.stepScrollView(playlistView, wheel, 24, 3)
                                }
                            }

                            delegate: Rectangle {
                                id: playlistRow
                                readonly property string titleValue: (typeof title !== "undefined" && title !== undefined)
                                    ? title
                                    : ((modelData && typeof modelData === "object") ? (modelData.title || "") : "")
                                readonly property string artistValue: (typeof artist !== "undefined" && artist !== undefined)
                                    ? artist
                                    : ((modelData && typeof modelData === "object") ? (modelData.artist || "") : "")
                                readonly property string albumValue: (typeof album !== "undefined" && album !== undefined)
                                    ? album
                                    : ((modelData && typeof modelData === "object") ? (modelData.album || "") : "")
                                readonly property string lengthTextValue: (typeof lengthText !== "undefined" && lengthText !== undefined)
                                    ? lengthText
                                    : ((modelData && typeof modelData === "object") ? (modelData.lengthText || "--:--") : "--:--")
                                readonly property bool isCurrentQueueRow: index === uiBridge.playingQueueIndex
                                readonly property bool draggableQueueItem: true
                                readonly property int queueRowIndex: index
                                width: Math.max(
                                    0,
                                    ListView.view.width - (playlistView.reservedRightPadding || 0))
                                height: 24
                                Drag.active: playlistRowMouseArea.drag.active
                                Drag.source: playlistRow
                                Drag.hotSpot.x: width * 0.5
                                Drag.hotSpot.y: height * 0.5
                                Drag.dragType: Drag.Automatic
                                Drag.supportedActions: Qt.MoveAction
                                color: root.isQueueIndexSelected(index)
                                    ? root.uiSelectionColor
                                    : (index % 2 === 0 ? root.uiSurfaceRaisedColor
                                                        : root.uiSurfaceAltColor)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 6
                                    Label {
                                        text: {
                                            if (!playlistRow.isCurrentQueueRow) {
                                                return ""
                                            }
                                            if (uiBridge.playbackState === "Paused") {
                                                return "⏸"
                                            }
                                            if (uiBridge.playbackState === "Stopped") {
                                                return "■"
                                            }
                                            return "▶"
                                        }
                                        Layout.preferredWidth: root.playlistIndicatorColumnWidth
                                        horizontalAlignment: Text.AlignHCenter
                                        font.bold: true
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : (playlistRow.isCurrentQueueRow
                                                ? (uiBridge.playbackState === "Playing"
                                                    ? root.uiActiveIndicatorColor
                                                    : root.uiMutedTextColor)
                                                : root.uiTextColor)
                                    }
                                    Label {
                                        text: root.playlistOrderText(index)
                                        Layout.preferredWidth: root.playlistOrderColumnWidth
                                        horizontalAlignment: Text.AlignRight
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : root.uiTextColor
                                    }
                                    Label {
                                        text: titleValue
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : root.uiTextColor
                                    }
                                    Label {
                                        text: artistValue
                                        Layout.preferredWidth: 170
                                        elide: Text.ElideRight
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : root.uiTextColor
                                    }
                                    Label {
                                        text: albumValue
                                        Layout.preferredWidth: 190
                                        elide: Text.ElideRight
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : root.uiTextColor
                                    }
                                    Label {
                                        text: lengthTextValue
                                        Layout.preferredWidth: 76
                                        horizontalAlignment: Text.AlignRight
                                        color: root.isQueueIndexSelected(index)
                                            ? root.uiSelectionTextColor
                                            : root.uiTextColor
                                    }
                                }

                                MouseArea {
                                    id: playlistRowMouseArea
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    drag.target: (pressedButtons & Qt.LeftButton) ? playlistDragProxy : null
                                    drag.smoothed: false
                                    onReleased: {
                                        playlistDragProxy.x = 0
                                        playlistDragProxy.y = 0
                                    }
                                    onPressed: function(mouse) {
                                        playlistView.forceActiveFocus()
                                        root.handleQueueRowSelection(
                                            index,
                                            mouse.button,
                                            mouse.modifiers || Qt.NoModifier)
                                    }
                                    onClicked: function(mouse) {
                                        if (mouse.button === Qt.RightButton) {
                                            playlistContextMenu.rowIndex = index
                                            playlistContextMenu.popup()
                                        }
                                    }
                                    onDoubleClicked: function(mouse) {
                                        if (mouse.button === Qt.LeftButton) {
                                            uiBridge.playAt(index)
                                        }
                                    }
                                }

                                Item {
                                    id: playlistDragProxy
                                    x: 0
                                    y: 0
                                    width: 1
                                    height: 1
                                    visible: false
                                }
                            }

                            Menu {
                                id: playlistContextMenu
                                property int rowIndex: -1
                                enter: Transition {
                                    NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                                }
                                exit: Transition {
                                    NumberAnimation { properties: "opacity,scale,x,y"; duration: root.uiPopupTransitionMs }
                                }

                                MenuItem {
                                    text: "Play Track"
                                    enabled: playlistContextMenu.rowIndex >= 0
                                    onTriggered: {
                                        if (playlistContextMenu.rowIndex >= 0) {
                                            uiBridge.playAt(playlistContextMenu.rowIndex)
                                        }
                                    }
                                }
                                MenuItem {
                                    text: "Open containing folder"
                                    enabled: playlistContextMenu.rowIndex >= 0
                                    onTriggered: {
                                        const path = uiBridge.queuePathAt(playlistContextMenu.rowIndex)
                                        if (path && path.length > 0) {
                                            uiBridge.openContainingFolder(path)
                                        }
                                    }
                                }
                                MenuItem {
                                    text: "Edit Tags"
                                    enabled: playlistContextMenu.rowIndex >= 0
                                    onTriggered: root.openTagEditorForPlaylistRow(playlistContextMenu.rowIndex)
                                }
                                MenuItem {
                                    text: "Remove Track"
                                    enabled: playlistContextMenu.rowIndex >= 0
                                    onTriggered: {
                                        if (playlistContextMenu.rowIndex < 0) {
                                            return
                                        }
                                        if (root.isQueueIndexSelected(playlistContextMenu.rowIndex)
                                                && root.selectedQueueIndices.length > 1) {
                                            root.removeSelectedQueueTrack()
                                        } else {
                                            root.requestPlaylistViewportRestoreWindow(700)
                                            uiBridge.removeAt(playlistContextMenu.rowIndex)
                                        }
                                    }
                                }
                                MenuSeparator {}
                                MenuItem {
                                    text: "Move Up"
                                    enabled: playlistContextMenu.rowIndex > 0
                                    onTriggered: {
                                        const from = playlistContextMenu.rowIndex
                                        if (from > 0) {
                                            uiBridge.moveQueue(from, from - 1)
                                        }
                                    }
                                }
                                MenuItem {
                                    text: "Move Down"
                                    enabled: playlistContextMenu.rowIndex >= 0
                                        && playlistContextMenu.rowIndex < uiBridge.queueLength - 1
                                    onTriggered: {
                                        const from = playlistContextMenu.rowIndex
                                        if (from >= 0 && from < uiBridge.queueLength - 1) {
                                            uiBridge.moveQueue(from, from + 1)
                                        }
                                    }
                                }
                                MenuSeparator {}
                                MenuItem { action: clearPlaylistAction }
                            }
                        }

                        Label {
                            visible: uiBridge.queueLength === 0
                            text: "Playlist is empty"
                            color: Kirigami.Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignHCenter
                            Layout.topMargin: 10
                        }

                        Connections {
                            target: uiBridge
                            function onSnapshotChanged() {
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
                        }
                    }

                    DropArea {
                        id: playlistDropArea
                        anchors.fill: parent
                        property bool queueReorderDragActive: false
                        property int queueDropInsertIndex: -1
                        property real queueDropIndicatorY: 0

                        function updateQueueDropIndicator(dropY) {
                            const rowHeight = 24
                            const yInList = dropY - playlistView.y + playlistView.contentY
                            let insertIndex = Math.floor((yInList + rowHeight * 0.5) / rowHeight)
                            insertIndex = Math.max(0, Math.min(uiBridge.queueLength, insertIndex))
                            queueDropInsertIndex = insertIndex

                            const contentLineY = insertIndex * rowHeight
                            const viewLineY = playlistView.y + contentLineY - playlistView.contentY
                            const minY = playlistView.y
                            const maxY = playlistView.y + playlistView.height - 2
                            queueDropIndicatorY = Math.max(minY, Math.min(maxY, viewLineY))
                        }

                        onEntered: function(drop) {
                            queueReorderDragActive = !!(drop.source && drop.source.draggableQueueItem)
                            if (queueReorderDragActive) {
                                updateQueueDropIndicator(drop.y)
                            } else {
                                queueDropInsertIndex = -1
                            }
                        }

                        onPositionChanged: function(drop) {
                            if (queueReorderDragActive) {
                                updateQueueDropIndicator(drop.y)
                            }
                        }

                        onExited: {
                            queueReorderDragActive = false
                            queueDropInsertIndex = -1
                        }

                        onDropped: function(drop) {
                            const src = drop.source
                            if (!src) {
                                const externalPaths = root.droppedExternalPaths(drop)
                                if (externalPaths.length > 0
                                        && root.submitExternalImport(externalPaths, false)) {
                                    queueReorderDragActive = false
                                    queueDropInsertIndex = -1
                                    drop.acceptProposedAction()
                                }
                                return
                            }
                            if (src.draggableQueueItem) {
                                const from = src.queueRowIndex !== undefined ? src.queueRowIndex : -1
                                if (from < 0 || uiBridge.queueLength <= 1) {
                                    return
                                }
                                let insertIndex = queueDropInsertIndex
                                if (insertIndex < 0) {
                                    updateQueueDropIndicator(drop.y)
                                    insertIndex = queueDropInsertIndex
                                }
                                let to = insertIndex > from ? insertIndex - 1 : insertIndex
                                to = Math.max(0, Math.min(uiBridge.queueLength - 1, to))
                                if (to !== from) {
                                    uiBridge.moveQueue(from, to)
                                }
                                queueReorderDragActive = false
                                queueDropInsertIndex = -1
                                drop.acceptProposedAction()
                                return
                            }
                            if (!src.draggableLibraryItem) {
                                return
                            }
                            const rowMap = {
                                selectionKey: src.selectionKeyResolved || "",
                                sourceIndex: src.sourceIndexResolved !== undefined ? src.sourceIndexResolved : -1,
                                rowType: src.rowTypeResolved || "",
                                artist: src.artistResolved || "",
                                name: src.nameResolved || "",
                                title: src.rowTitle || "",
                                trackPath: src.trackPathResolved || "",
                                openPath: src.openPathResolved || "",
                                playPaths: src.playPathsResolved || []
                            }
                            const rows = root.rowsForLibraryAction(rowMap)
                            if (rows.length > 0) {
                                root.appendLibraryRows(rows)
                                queueReorderDragActive = false
                                queueDropInsertIndex = -1
                                drop.acceptProposedAction()
                            }
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: playlistDropArea.containsDrag
                            && !playlistDropArea.queueReorderDragActive
                            ? 2
                            : 0
                        border.color: Kirigami.Theme.highlightColor
                        visible: playlistDropArea.containsDrag
                            && !playlistDropArea.queueReorderDragActive
                    }

                    Rectangle {
                        x: playlistView.x + 4
                        width: Math.max(0, playlistView.width - (playlistView.reservedRightPadding || 0) - 8)
                        height: 2
                        y: playlistDropArea.queueDropIndicatorY
                        radius: 1
                        color: Kirigami.Theme.highlightColor
                        visible: playlistDropArea.containsDrag
                            && playlistDropArea.queueReorderDragActive
                            && playlistDropArea.queueDropInsertIndex >= 0
                    }

                    Rectangle {
                        x: playlistView.x + 4
                        y: playlistDropArea.queueDropIndicatorY - 2
                        width: 6
                        height: 6
                        radius: 3
                        color: Kirigami.Theme.highlightColor
                        visible: playlistDropArea.containsDrag
                            && playlistDropArea.queueReorderDragActive
                            && playlistDropArea.queueDropInsertIndex >= 0
                    }
                }

                Rectangle {
                    SplitView.fillWidth: true
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 220
                    color: "#0b0b0f"
                    border.color: Qt.rgba(0, 0, 0, 0.25)

                    Item {
                        id: spectrogramMainHost
                        anchors.fill: parent
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onDoubleClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton) {
                                root.openSpectrogramViewer()
                            }
                        }
                    }
                }
            }
        }
    }

    Item {
        id: spectrogramSurface
        parent: root.spectrogramViewerOpen
            ? (root.useWholeScreenViewerMode ? spectrogramWindowHost : spectrogramFullscreenHost)
            : spectrogramMainHost
        visible: parent !== null
        anchors.fill: parent
        property var channelDescriptors: []

        function placeholderDescriptors() {
            return uiBridge.spectrogramViewMode === 1
                ? [{ label: "M", showLabel: true }]
                : [{ label: "", showLabel: false }]
        }

        function sameDescriptors(next) {
            if (channelDescriptors.length !== next.length) {
                return false
            }
            for (let i = 0; i < next.length; ++i) {
                if (channelDescriptors[i].label !== next[i].label
                        || channelDescriptors[i].showLabel !== next[i].showLabel) {
                    return false
                }
            }
            return true
        }

        function syncChannelDescriptors(channels) {
            let next = []
            if (channels && channels.length > 0) {
                const showLabels = uiBridge.spectrogramViewMode === 1
                for (let i = 0; i < channels.length; ++i) {
                    const labelText = (channels[i].label || "").trim()
                    next.push({
                        label: labelText,
                        showLabel: showLabels && labelText.length > 0
                    })
                }
            }
            if (next.length === 0) {
                next = placeholderDescriptors()
            }
            if (!sameDescriptors(next)) {
                channelDescriptors = next
            }
        }

        function resetForCurrentMode() {
            syncChannelDescriptors([])
            for (let i = 0; i < spectrogramRepeater.count; ++i) {
                const pane = spectrogramRepeater.itemAt(i)
                if (pane && pane.spectrogramItem) {
                    pane.spectrogramItem.reset()
                }
            }
        }

        function appendPackedDelta(channels) {
            if (!channels || channels.length === 0) {
                return
            }
            syncChannelDescriptors(channels)
            for (let i = 0; i < channels.length; ++i) {
                const pane = spectrogramRepeater.itemAt(i)
                const channel = channels[i]
                if (!pane || !pane.spectrogramItem || !channel) {
                    continue
                }
                if ((channel.rows || 0) > 0 && (channel.bins || 0) > 0) {
                    pane.spectrogramItem.appendPackedRows(channel.data, channel.rows, channel.bins)
                }
            }
        }

        Component.onCompleted: resetForCurrentMode()

        ColumnLayout {
            anchors.fill: parent
            spacing: spectrogramSurface.channelDescriptors.length > 1 ? 2 : 0

            Repeater {
                id: spectrogramRepeater
                model: spectrogramSurface.channelDescriptors

                delegate: Item {
                    property alias spectrogramItem: spectrogramPaneItem
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 1
                    Layout.minimumHeight: 0

                    Rectangle {
                        anchors.fill: parent
                        color: "#0b0b0f"
                    }

                    SpectrogramItem {
                        id: spectrogramPaneItem
                        anchors.fill: parent
                        maxColumns: Math.max(640, Math.min(1600, Math.floor(width)))
                        dbRange: uiBridge.dbRange
                        logScale: uiBridge.logScale
                        showFpsOverlay: index === 0 ? uiBridge.showFps : false
                        sampleRateHz: uiBridge.sampleRateHz
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 8
                        width: labelText.implicitWidth + 8
                        height: labelText.implicitHeight + 2
                        radius: 4
                        color: Qt.rgba(0.0, 0.0, 0.0, 0.18)
                        visible: modelData.showLabel

                        Text {
                            id: labelText
                            anchors.centerIn: parent
                            text: modelData.label
                            color: Qt.rgba(0.90, 0.93, 0.98, 0.74)
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: spectrogramViewer
        parent: Overlay.overlay
        x: 0
        y: 0
        width: root.width
        height: root.height
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        enter: Transition {
            NumberAnimation {
                properties: "opacity,scale,x,y"
                duration: root.uiPopupTransitionMs
            }
        }
        exit: Transition {
            NumberAnimation {
                properties: "opacity,scale,x,y"
                duration: root.uiPopupTransitionMs
            }
        }
        onClosed: {
            if (root.spectrogramViewerOpen && !root.useWholeScreenViewerMode) {
                root.spectrogramViewerOpen = false
            }
        }
        background: Rectangle {
            color: "#000000"
            opacity: 0.87
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: root.closeSpectrogramViewer()
        }

        Rectangle {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            width: 40
            height: 40
            radius: 8
            color: Qt.rgba(0, 0, 0, 0.45)
            border.color: Qt.rgba(1, 1, 1, 0.24)

            ToolButton {
                anchors.fill: parent
                icon.name: "window-close"
                onClicked: root.closeSpectrogramViewer()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0f"
            border.color: Qt.rgba(1, 1, 1, 0.12)

            Item {
                id: spectrogramFullscreenHost
                anchors.fill: parent
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton) {
                        root.closeSpectrogramViewer()
                    }
                }
            }
        }
    }

    Window {
        id: spectrogramFullscreenWindow
        screen: root.screen
        transientParent: root
        modality: Qt.ApplicationModal
        flags: Qt.Window | Qt.FramelessWindowHint
        visibility: root.spectrogramViewerOpen && root.useWholeScreenViewerMode
            ? Window.FullScreen
            : Window.Hidden
        color: "#000000"
        title: root.title
        onVisibilityChanged: function() {
            if (spectrogramFullscreenWindow.visibility === Window.FullScreen) {
                requestActivate()
                spectrogramFullscreenFocusSink.forceActiveFocus()
            }
        }
        onClosing: function(close) {
            if (root.spectrogramViewerOpen && root.useWholeScreenViewerMode) {
                root.spectrogramViewerOpen = false
            }
        }

        FocusScope {
            id: spectrogramFullscreenFocusSink
            anchors.fill: parent
            focus: spectrogramFullscreenWindow.visibility === Window.FullScreen
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.closeSpectrogramViewer()
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: spectrogramFullscreenFocusSink.forceActiveFocus()
            onClicked: root.closeSpectrogramViewer()
        }

        Rectangle {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            width: 40
            height: 40
            radius: 8
            color: Qt.rgba(0, 0, 0, 0.45)
            border.color: Qt.rgba(1, 1, 1, 0.24)

            ToolButton {
                anchors.fill: parent
                icon.name: "window-close"
                onClicked: root.closeSpectrogramViewer()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "#0b0b0f"

            Item {
                id: spectrogramWindowHost
                anchors.fill: parent
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: spectrogramFullscreenFocusSink.forceActiveFocus()
                onDoubleClicked: function(mouse) {
                    if (mouse.button === Qt.LeftButton) {
                        root.closeSpectrogramViewer()
                    }
                }
            }
        }
    }

    Dialog {
        id: itunesArtworkDialog
        parent: Overlay.overlay
        modal: true
        focus: true
        z: 100
        property int pendingPreviewIndex: -1
        property int pendingApplyIndex: -1
        property string currentArtworkSource: ""
        property var currentArtworkInfo: ({})
        readonly property real hostWidth: (parent && parent.width > 0) ? parent.width : root.width
        readonly property real hostHeight: (parent && parent.height > 0) ? parent.height : root.height
        width: Math.min(Math.max(320, hostWidth - 48), 920)
        height: Math.min(Math.max(320, hostHeight - 48), 680)
        x: (hostWidth - width) / 2
        y: (hostHeight - height) / 2
        title: "Replace From iTunes"
        standardButtons: Dialog.Close
        function clearPendingActionState() {
            pendingPreviewIndex = -1
            pendingApplyIndex = -1
        }
        function refreshCurrentArtworkInfo() {
            currentArtworkSource = uiBridge.currentTrackCoverPath || ""
            const infoSource = root.pathFromAnyUrl(currentArtworkSource)
            currentArtworkInfo = infoSource.length > 0
                ? (uiBridge.imageFileDetails(infoSource) || ({}))
                : ({})
        }
        function suggestionRowAt(index) {
            return uiBridge.itunesArtworkResultAt(index) || ({})
        }
        function suggestionRowReady(row) {
            return ((row && (row.normalizedPath || "")) || "").length > 0
        }
        function requestSuggestionPreview(index) {
            const row = suggestionRowAt(index)
            if (suggestionRowReady(row)) {
                pendingPreviewIndex = -1
                pendingApplyIndex = -1
                root.openAlbumArtViewerForSuggestion(row)
                return
            }
            pendingApplyIndex = -1
            pendingPreviewIndex = index
            uiBridge.prepareItunesArtworkSuggestion(index)
        }
        function requestSuggestionApply(index) {
            const row = suggestionRowAt(index)
            if (suggestionRowReady(row)) {
                pendingPreviewIndex = -1
                pendingApplyIndex = -1
                uiBridge.applyItunesArtworkSuggestion(index)
                itunesArtworkDialog.close()
                return
            }
            pendingPreviewIndex = -1
            pendingApplyIndex = index
            uiBridge.prepareItunesArtworkSuggestion(index)
        }
        function processPendingSuggestionAction() {
            if (!visible) {
                return
            }
            if (pendingApplyIndex >= 0) {
                const applyRow = suggestionRowAt(pendingApplyIndex)
                if (suggestionRowReady(applyRow)) {
                    const resolvedIndex = pendingApplyIndex
                    clearPendingActionState()
                    uiBridge.applyItunesArtworkSuggestion(resolvedIndex)
                    itunesArtworkDialog.close()
                    return
                }
                if ((((applyRow && (applyRow.assetError || "")) || "").length > 0)
                        && !((applyRow && (applyRow.assetLoading || false)) || false)) {
                    pendingApplyIndex = -1
                }
            }
            if (pendingPreviewIndex >= 0) {
                const previewRow = suggestionRowAt(pendingPreviewIndex)
                if (suggestionRowReady(previewRow)) {
                    clearPendingActionState()
                    root.openAlbumArtViewerForSuggestion(previewRow)
                    return
                }
                if ((((previewRow && (previewRow.assetError || "")) || "").length > 0)
                        && !((previewRow && (previewRow.assetLoading || false)) || false)) {
                    pendingPreviewIndex = -1
                }
            }
        }
        onOpened: {
            clearPendingActionState()
            refreshCurrentArtworkInfo()
        }
        onClosed: {
            clearPendingActionState()
            uiBridge.clearItunesArtworkSuggestions()
            itunesArtworkDialog.parent = Overlay.overlay
        }

        Connections {
            target: uiBridge
            function onItunesArtworkChanged() {
                itunesArtworkDialog.processPendingSuggestionAction()
            }
            function onSnapshotChanged() {
                if (itunesArtworkDialog.visible) {
                    itunesArtworkDialog.refreshCurrentArtworkInfo()
                }
            }
        }

        contentItem: ColumnLayout {
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                BusyIndicator {
                    running: uiBridge.itunesArtworkLoading
                    visible: running
                }

                Text {
                    Layout.fillWidth: true
                    text: uiBridge.itunesArtworkStatusText || ""
                    color: root.uiTextColor
                    wrapMode: Text.Wrap
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                implicitHeight: currentArtworkSummaryRow.implicitHeight + 20
                radius: 12
                color: root.uiPaneColor
                border.color: root.uiBorderColor
                clip: true

                RowLayout {
                    id: currentArtworkSummaryRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12

                    Item {
                        Layout.preferredWidth: 92
                        Layout.preferredHeight: 92
                        clip: true

                        Image {
                            id: currentArtworkImage
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            source: itunesArtworkDialog.currentArtworkSource
                            smooth: true
                            asynchronous: true
                            cache: true
                            visible: (itunesArtworkDialog.currentArtworkSource || "").length > 0
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: 10
                            color: root.uiSurfaceAltColor
                            border.color: root.uiBorderColor
                            visible: !currentArtworkImage.visible

                            Text {
                                anchors.centerIn: parent
                                text: "No art"
                                color: root.uiMutedTextColor
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            Layout.fillWidth: true
                            text: "Current album art"
                            color: root.uiTextColor
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            text: [
                                (itunesArtworkDialog.currentArtworkInfo.resolutionText || ""),
                                (itunesArtworkDialog.currentArtworkInfo.fileType || ""),
                                (itunesArtworkDialog.currentArtworkInfo.fileSizeText || "")
                            ].filter(Boolean).join("  |  ")
                            color: root.uiMutedTextColor
                            wrapMode: Text.Wrap
                            visible: text.length > 0
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: (itunesArtworkDialog.currentArtworkInfo.mimeType || "").length > 0
                            text: "MIME: " + (itunesArtworkDialog.currentArtworkInfo.mimeType || "")
                            color: root.uiMutedTextColor
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: (itunesArtworkDialog.currentArtworkSource || "").length === 0
                            text: "No current album art is available for this track."
                            color: root.uiMutedTextColor
                            wrapMode: Text.Wrap
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: (itunesArtworkDialog.currentArtworkSource || "").length > 0
                                && Object.keys(itunesArtworkDialog.currentArtworkInfo || {}).length === 0
                            text: "Current artwork metadata is not available."
                            color: root.uiMutedTextColor
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            ListView {
                id: itunesArtworkResultsView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 10
                boundsBehavior: Flickable.StopAtBounds
                rightMargin: itunesArtworkScrollBar.visible ? (itunesArtworkScrollBar.width + 10) : 0
                model: uiBridge.itunesArtworkResults
                visible: count > 0

                ScrollBar.vertical: ScrollBar {
                    id: itunesArtworkScrollBar
                    policy: ScrollBar.AsNeeded
                }

                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    x: ListView.view.leftMargin
                    width: Math.max(0, ListView.view.width - ListView.view.leftMargin - ListView.view.rightMargin)
                    implicitHeight: 136
                    radius: 12
                    color: root.uiPaneColor
                    border.color: root.uiBorderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 12

                        Image {
                            Layout.preferredWidth: 92
                            Layout.preferredHeight: 92
                            fillMode: Image.PreserveAspectFit
                            source: (modelData && (modelData.previewSource || "")) || ""
                            smooth: true
                            asynchronous: true
                            cache: true
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                Layout.fillWidth: true
                                text: (modelData && (modelData.albumTitle || "")) || ""
                                color: root.uiTextColor
                                font.pixelSize: 16
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                text: (modelData && (modelData.artistName || "")) || ""
                                color: root.uiMutedTextColor
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                text: [
                                    (modelData && (modelData.resolutionText || "")) || "",
                                    (modelData && (modelData.fileType || "")) || "",
                                    (modelData && (modelData.fileSizeText || "")) || ""
                                ].filter(Boolean).join("  |  ")
                                color: root.uiMutedTextColor
                                wrapMode: Text.Wrap
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: ((modelData && (modelData.mimeType || "")) || "").length > 0
                                text: "MIME: " + (((modelData && (modelData.mimeType || "")) || ""))
                                color: root.uiMutedTextColor
                                elide: Text.ElideRight
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                visible: ((modelData && (modelData.assetLoading || false)) || false)
                                spacing: 8

                                BusyIndicator {
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    running: true
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Loading high-resolution artwork..."
                                    color: root.uiMutedTextColor
                                    wrapMode: Text.Wrap
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: ((modelData && (modelData.assetError || "")) || "").length > 0
                                text: (modelData && (modelData.assetError || "")) || ""
                                color: Kirigami.Theme.negativeTextColor
                                wrapMode: Text.Wrap
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !((modelData && (modelData.assetLoading || false)) || false)
                                    && ((modelData && (modelData.detailStatusText || "")) || "").length > 0
                                text: (modelData && (modelData.detailStatusText || "")) || ""
                                color: root.uiMutedTextColor
                                wrapMode: Text.Wrap
                            }
                        }

                        ColumnLayout {
                            spacing: 8

                            Button {
                                text: ((modelData && (modelData.assetLoading || false)) || false)
                                    && itunesArtworkDialog.pendingPreviewIndex === index
                                    ? "Loading..."
                                    : "Preview"
                                enabled: !((modelData && (modelData.assetLoading || false)) || false)
                                onClicked: itunesArtworkDialog.requestSuggestionPreview(index)
                            }

                            Button {
                                text: ((modelData && (modelData.assetLoading || false)) || false)
                                    && itunesArtworkDialog.pendingApplyIndex === index
                                    ? "Loading..."
                                    : "Apply"
                                enabled: !((modelData && (modelData.assetLoading || false)) || false)
                                onClicked: itunesArtworkDialog.requestSuggestionApply(index)
                            }
                        }
                    }
                }
            }
        }
    }

    Popup {
        id: albumArtViewer
        parent: Overlay.overlay
        x: 0
        y: 0
        width: root.width
        height: root.height
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape
        enter: Transition {
            NumberAnimation {
                properties: "opacity,scale,x,y"
                duration: root.uiPopupTransitionMs
            }
        }
        exit: Transition {
            NumberAnimation {
                properties: "opacity,scale,x,y"
                duration: root.uiPopupTransitionMs
            }
        }
        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtViewer.visible
            onActivated: root.toggleAlbumArtInfoVisible()
        }
        onOpened: root.applyAlbumArtInitialView()
        onClosed: {
            if (root.albumArtViewerOpen && !root.useWholeScreenViewerMode) {
                root.albumArtViewerOpen = false
            }
        }
        background: Rectangle {
            color: "#000000"
            opacity: 0.87
        }

        MouseArea {
            id: albumArtDismissArea
            z: 0
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onClicked: root.closeAlbumArtViewer()
        }

        Rectangle {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            width: 40
            height: 40
            radius: 8
            color: Qt.rgba(1, 1, 1, 0.16)
            border.color: Qt.rgba(1, 1, 1, 0.52)

            ToolButton {
                anchors.fill: parent
                icon.name: "window-close"
                icon.color: "#ffffff"
                onClicked: root.closeAlbumArtViewer()
            }
        }

        Item {
            id: albumArtPopupHost
            anchors.fill: parent
        }
    }

    Window {
        id: albumArtFullscreenWindow
        screen: root.screen
        transientParent: root
        modality: Qt.ApplicationModal
        flags: Qt.Window | Qt.FramelessWindowHint
        visibility: root.albumArtViewerOpen && root.useWholeScreenViewerMode
            ? Window.FullScreen
            : Window.Hidden
        color: "#000000"
        title: root.title
        onVisibilityChanged: function() {
            if (albumArtFullscreenWindow.visibility === Window.FullScreen) {
                requestActivate()
                albumArtFullscreenFocusSink.forceActiveFocus()
                root.applyAlbumArtInitialView()
            }
        }
        onClosing: function(close) {
            if (root.albumArtViewerOpen && root.useWholeScreenViewerMode) {
                root.albumArtViewerOpen = false
            }
        }

        FocusScope {
            id: albumArtFullscreenFocusSink
            anchors.fill: parent
            focus: albumArtFullscreenWindow.visibility === Window.FullScreen
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Escape) {
                    event.accepted = true
                    root.closeAlbumArtViewer()
                }
            }
        }

        Shortcut {
            sequence: "I"
            context: Qt.WindowShortcut
            enabled: albumArtFullscreenWindow.visibility === Window.FullScreen
            onActivated: root.toggleAlbumArtInfoVisible()
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onPressed: albumArtFullscreenFocusSink.forceActiveFocus()
            onClicked: root.closeAlbumArtViewer()
        }

        Rectangle {
            z: 20
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            width: 40
            height: 40
            radius: 8
            color: Qt.rgba(1, 1, 1, 0.16)
            border.color: Qt.rgba(1, 1, 1, 0.52)

            ToolButton {
                anchors.fill: parent
                icon.name: "window-close"
                icon.color: "#ffffff"
                onClicked: root.closeAlbumArtViewer()
            }
        }

        Item {
            anchors.fill: parent
            Item {
                id: albumArtWindowHost
                anchors.fill: parent
            }
        }
    }

    Item {
        id: albumArtSurface
        parent: root.albumArtViewerOpen
            ? (root.useWholeScreenViewerMode ? albumArtWindowHost : albumArtPopupHost)
            : albumArtMainHost
        visible: root.albumArtViewerOpen
        anchors.fill: parent
        clip: true
        onWidthChanged: root.applyAlbumArtInitialView()
        onHeightChanged: root.applyAlbumArtInitialView()

        Item {
            id: albumArtViewport
            anchors.fill: parent
            clip: true
            onWidthChanged: root.applyAlbumArtInitialView()
            onHeightChanged: root.applyAlbumArtInitialView()

            Item {
                id: albumArtTransform
                readonly property real nativeWidth: albumArtImageFull.sourceSize.width > 0
                    ? albumArtImageFull.sourceSize.width
                    : albumArtViewport.width
                readonly property real nativeHeight: albumArtImageFull.sourceSize.height > 0
                    ? albumArtImageFull.sourceSize.height
                    : albumArtViewport.height
                readonly property real fitScale: {
                    const w = nativeWidth > 0 ? nativeWidth : 1
                    const h = nativeHeight > 0 ? nativeHeight : 1
                    const scaleX = albumArtViewport.width / w
                    const scaleY = albumArtViewport.height / h
                    return Math.min(1.0, scaleX, scaleY)
                }
                width: Math.max(1, nativeWidth * fitScale)
                height: Math.max(1, nativeHeight * fitScale)
                x: (albumArtViewport.width - width) / 2 + root.albumArtPanX
                y: (albumArtViewport.height - height) / 2 + root.albumArtPanY
                scale: root.albumArtZoom
                transformOrigin: Item.Center

                Image {
                    id: albumArtImageFull
                    anchors.fill: parent
                    source: root.albumArtViewerSource
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    asynchronous: true
                    cache: true
                    retainWhileLoading: false
                    sourceSize.width: root.albumArtViewerDecodeWidth
                    sourceSize.height: root.albumArtViewerDecodeHeight
                    onStatusChanged: root.applyAlbumArtInitialView()
                }
            }

            MouseArea {
                id: albumArtPanArea
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                preventStealing: true
                property real lastX: 0
                property real lastY: 0
                cursorShape: root.albumArtZoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                onPressed: function(mouse) {
                    if (mouse.button === Qt.RightButton) {
                        albumArtViewerContextMenu.popup()
                        return
                    }
                    if (!root.isPointOnAlbumArtImage(albumArtPanArea, mouse.x, mouse.y)) {
                        root.closeAlbumArtViewer()
                        return
                    }
                    lastX = mouse.x
                    lastY = mouse.y
                    cursorShape = Qt.ClosedHandCursor
                }
                onReleased: {
                    cursorShape = root.albumArtZoom > 1.0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                }
                onPositionChanged: function(mouse) {
                    if (!pressed || root.albumArtZoom <= 1.0) {
                        return
                    }
                    root.albumArtPanX += mouse.x - lastX
                    root.albumArtPanY += mouse.y - lastY
                    lastX = mouse.x
                    lastY = mouse.y
                    root.clampAlbumArtPan()
                }
                onDoubleClicked: function(mouse) {
                    if (mouse.button !== Qt.LeftButton) {
                        return
                    }
                    if (root.albumArtZoom > 1.0) {
                        root.albumArtZoom = 1.0
                        root.albumArtPanX = 0.0
                        root.albumArtPanY = 0.0
                    } else {
                        root.albumArtZoom = 2.0
                        root.clampAlbumArtPan()
                    }
                }
                onWheel: function(wheel) {
                    const oldZoom = root.albumArtZoom
                    const delta = wheel.angleDelta.y > 0 ? 1.1 : 0.9
                    const nextZoom = Math.max(1.0, Math.min(6.0, oldZoom * delta))
                    if (Math.abs(nextZoom - oldZoom) < 0.0001) {
                        wheel.accepted = true
                        return
                    }
                    const pivotX = wheel.x - albumArtViewport.width / 2
                    const pivotY = wheel.y - albumArtViewport.height / 2
                    const ratio = nextZoom / oldZoom
                    root.albumArtZoom = nextZoom
                    root.albumArtPanX = (root.albumArtPanX + pivotX) * ratio - pivotX
                    root.albumArtPanY = (root.albumArtPanY + pivotY) * ratio - pivotY
                    root.clampAlbumArtPan()
                    wheel.accepted = true
                }
            }

            Menu {
                id: albumArtViewerContextMenu
                MenuItem { action: replaceFromItunesAction }
                MenuItem {
                    enabled: false
                    visible: !replaceFromItunesAction.enabled
                    text: root.currentTrackItunesArtworkDisabledReason()
                }
            }
        }

        Column {
            z: 30
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 12
            spacing: 8

            Rectangle {
                width: 40
                height: 40
                radius: 8
                color: Qt.rgba(1, 1, 1, 0.16)
                border.color: Qt.rgba(1, 1, 1, 0.52)

                ToolButton {
                    anchors.fill: parent
                    contentItem: Text {
                        text: "i"
                        color: "#ffffff"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: root.toggleAlbumArtInfoVisible()
                }
            }

            Rectangle {
                visible: root.albumArtInfoVisible && albumArtInfoLabel.text.length > 0
                width: Math.min(540, albumArtSurface.width - 24)
                color: Qt.rgba(0, 0, 0, 0.58)
                border.color: Qt.rgba(1, 1, 1, 0.24)
                radius: 10
                implicitHeight: albumArtInfoLabel.implicitHeight + 20

                Text {
                    id: albumArtInfoLabel
                    anchors.fill: parent
                    anchors.margins: 10
                    color: "#f2f2f2"
                    text: root.albumArtInfoOverlayText()
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                    hoverEnabled: true
                    preventStealing: true
                    onPressed: albumArtFullscreenFocusSink.forceActiveFocus()
                    onWheel: function(wheel) {
                        albumArtFullscreenFocusSink.forceActiveFocus()
                        wheel.accepted = true
                    }
                }
            }
        }
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
            if (uiBridge.spectrogramReset && root.visualFeedsEnabled) {
                spectrogramSurface.resetForCurrentMode()
            }
            const delta = uiBridge.takeSpectrogramRowsDeltaPacked()
            if (root.visualFeedsEnabled && delta.channels && delta.channels.length > 0) {
                spectrogramSurface.appendPackedDelta(delta.channels)
            }
        }
        function onSnapshotChanged() {
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
            const incomingPosition = uiBridge.positionSeconds
            const trackChanged = root.positionSmoothingTrackPath !== uiBridge.currentTrackPath
            const nowMs = Date.now()
            const duration = Math.max(uiBridge.durationSeconds, 0)
            if (uiBridge.playbackState !== "Playing") {
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
        }
        function onLibraryTreeFrameReceived(version, treeBytes) {
            root.requestLibraryTreeApply(version, treeBytes || "")
        }
        function onAnalysisChanged() {
            applyAnalysisDelta()
        }
        function onGlobalSearchResultsChanged() {
            root.syncGlobalSearchSelectionAfterResultsChange()
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

    Dialog {
        id: autoNumberDialog
        modal: true
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 420
        title: "Auto Number"
        standardButtons: Dialog.NoButton
        background: Rectangle {
            radius: 16
            color: root.uiSurfaceRaisedColor
            border.color: root.uiBorderColor
        }

        property alias startingTrackField: startingTrackField
        property alias startingDiscField: startingDiscField
        property alias writeDiscCheck: writeDiscCheck
        property alias writeTotalsCheck: writeTotalsCheck
        property alias resetOnFolderCheck: resetOnFolderCheck
        property alias resetOnDiscCheck: resetOnDiscCheck

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: root.uiMutedTextColor
                text: "Number checked rows, or all rows if none are checked, using the current table order."
            }

            GridLayout {
                columns: 2
                columnSpacing: 10
                rowSpacing: 8
                Layout.fillWidth: true

                Label { text: "Starting Track" }
                TextField {
                    id: startingTrackField
                    Layout.fillWidth: true
                    text: "1"
                    inputMethodHints: Qt.ImhDigitsOnly
                }

                Label { text: "Starting Disc" }
                TextField {
                    id: startingDiscField
                    Layout.fillWidth: true
                    text: "1"
                    enabled: writeDiscCheck.checked
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }

            CheckBox {
                id: writeDiscCheck
                checked: false
                text: "Write disc numbers"
            }
            CheckBox {
                id: writeTotalsCheck
                checked: false
                text: "Write totals"
            }
            CheckBox {
                id: resetOnFolderCheck
                checked: false
                text: "Reset track numbers on folder or section change"
            }
            CheckBox {
                id: resetOnDiscCheck
                checked: false
                enabled: writeDiscCheck.checked
                text: "Reset track numbers when disc changes in current values"
            }

            RowLayout {
                Layout.fillWidth: true

                Item { Layout.fillWidth: true }

                Button {
                    text: "Cancel"
                    onClicked: autoNumberDialog.close()
                }
                Button {
                    text: "Apply"
                    onClicked: {
                        tagEditorApi.autoNumber(
                            Number(startingTrackField.text || "1"),
                            Number(startingDiscField.text || "1"),
                            writeDiscCheck.checked,
                            writeTotalsCheck.checked,
                            resetOnFolderCheck.checked,
                            resetOnDiscCheck.checked)
                        autoNumberDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: tagEditorDialog
        modal: true
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: Math.min(root.width - 28, 1440)
        height: Math.min(root.height - 24, 820)
        title: "Edit Tags"
        standardButtons: Dialog.NoButton
        closePolicy: Popup.NoAutoClose
        property var selectedRows: []
        property int selectionAnchor: -1
        property int totalRows: 0
        property bool operationInFlight: false
        property string operationText: ""
        readonly property string keepText: "<keep>"
        readonly property bool statusHasFailure: tagEditorApi.statusText.toLowerCase().indexOf("failed") >= 0
            || tagEditorApi.statusText.toLowerCase().indexOf("error") >= 0
            || tagEditorApi.statusDetails.indexOf("Failed:") >= 0
        readonly property var editorFields: [
            { key: "title", label: "Title", allowTitleCase: true, allowCapitalize: true },
            { key: "artist", label: "Artist", allowTitleCase: true, allowCapitalize: true },
            { key: "album", label: "Album", allowTitleCase: true, allowCapitalize: true },
            { key: "genre", label: "Genre", allowTitleCase: false, allowCapitalize: true },
            { key: "year", label: "Year", allowTitleCase: false, allowCapitalize: false },
            { key: "albumArtist", label: "Album Artist", allowTitleCase: true, allowCapitalize: true },
            { key: "trackNo", label: "Track", allowTitleCase: false, allowCapitalize: false },
            { key: "discNo", label: "Disc", allowTitleCase: false, allowCapitalize: false },
            { key: "totalTracks", label: "Tracks", allowTitleCase: false, allowCapitalize: false },
            { key: "totalDiscs", label: "Discs", allowTitleCase: false, allowCapitalize: false },
            { key: "comment", label: "Comment", allowTitleCase: false, allowCapitalize: false }
        ]
        readonly property var tableColumns: [
            { key: "fileName", label: "File", width: 168 },
            { key: "title", label: "Title", width: 210 },
            { key: "artist", label: "Artist", width: 136 },
            { key: "album", label: "Album", width: 182 },
            { key: "trackNo", label: "#", width: 42 },
            { key: "genre", label: "Genre", width: 96 },
            { key: "year", label: "Year", width: 52 },
            { key: "discNo", label: "Disc", width: 46 },
            { key: "formatKind", label: "Fmt", width: 52 },
            { key: "directory", label: "Folder", width: 156 },
            { key: "status", label: "Status", width: 120 }
        ]
        readonly property int tableContentWidth: tableColumns.reduce(
            function(sum, column) { return sum + column.width + 6 }, 20)

        function closeEditor() {
            tagEditorApi.close()
            tagEditorDialog.close()
        }

        function commitPendingEditorEdits() {
            tagEditorListFocusSink.forceActiveFocus()
        }

        function triggerSave(closeAfterSuccess) {
            if (tagEditorApi.loading || tagEditorApi.saving || !tagEditorApi.dirty) {
                return
            }
            commitPendingEditorEdits()
            operationInFlight = true
            operationText = "Saving tags..."
            Qt.callLater(function() {
                const ok = tagEditorApi.save()
                operationInFlight = false
                operationText = ""
                if (ok && closeAfterSuccess) {
                    tagEditorCloseConfirmDialog.close()
                    closeEditor()
                }
                tagEditorStatusFlash.restart()
            })
        }

        function triggerRename() {
            if (tagEditorApi.loading || tagEditorApi.saving || selectedRows.length === 0) {
                return
            }
            commitPendingEditorEdits()
            operationInFlight = true
            operationText = "Renaming files..."
            Qt.callLater(function() {
                tagEditorApi.renameSelectedFiles()
                operationInFlight = false
                operationText = ""
                tagEditorStatusFlash.restart()
            })
        }

        function requestClose() {
            if (tagEditorApi.loading || tagEditorApi.saving) {
                return
            }
            if (tagEditorApi.dirty) {
                tagEditorCloseConfirmDialog.open()
                return
            }
            closeEditor()
        }

        function saveAndClose() {
            if (tagEditorApi.loading || tagEditorApi.saving) {
                return
            }
            triggerSave(true)
        }

        function normalizedSelection(rows) {
            const next = []
            for (let i = 0; i < rows.length; ++i) {
                const row = rows[i]
                if (row < 0 || row >= totalRows || next.indexOf(row) >= 0) {
                    continue
                }
                next.push(row)
            }
            next.sort(function(a, b) { return a - b })
            return next
        }

        function isSelected(row) {
            return selectedRows.indexOf(row) >= 0
        }

        function updateSelection(rows, anchor) {
            selectedRows = normalizedSelection(rows)
            selectionAnchor = anchor
            tagEditorApi.setSelectedRows(selectedRows)
        }

        function selectOnlyRow(row) {
            updateSelection([row], row)
        }

        function toggleRow(row) {
            let next = selectedRows.slice()
            const existing = next.indexOf(row)
            if (existing >= 0) {
                next.splice(existing, 1)
            } else {
                next.push(row)
            }
            updateSelection(next, row)
        }

        function selectRange(row) {
            if (selectionAnchor < 0) {
                selectOnlyRow(row)
                return
            }
            const start = Math.min(selectionAnchor, row)
            const end = Math.max(selectionAnchor, row)
            const next = []
            for (let current = start; current <= end; ++current) {
                next.push(current)
            }
            updateSelection(next, selectionAnchor)
        }

        function handleRowClick(row, modifiers) {
            tagEditorListFocusSink.forceActiveFocus()
            const ctrl = (modifiers & Qt.ControlModifier) !== 0
            const shift = (modifiers & Qt.ShiftModifier) !== 0
            if (shift) {
                selectRange(row)
                return
            }
            if (ctrl) {
                toggleRow(row)
                return
            }
            selectOnlyRow(row)
        }

        function initializeSelection() {
            totalRows = tagEditorApi.loadedPaths().length
            if (totalRows > 0 && selectedRows.length === 0) {
                selectOnlyRow(0)
            }
        }

        function clearSelection() {
            selectedRows = []
            selectionAnchor = -1
            tagEditorApi.setSelectedRows([])
        }

        function selectAllRows() {
            const next = []
            for (let row = 0; row < totalRows; ++row) {
                next.push(row)
            }
            updateSelection(next, totalRows > 0 ? 0 : -1)
        }

        function rowText(columnKey, rowData) {
            switch (columnKey) {
            case "fileName": return rowData.fileName || ""
            case "directory": return root.basenameFromPath(rowData.directory || "")
            case "formatKind": return rowData.formatKind || ""
            case "title": return rowData.title || ""
            case "artist": return rowData.artist || ""
            case "album": return rowData.album || ""
            case "albumArtist": return rowData.albumArtist || ""
            case "genre": return rowData.genre || ""
            case "year": return rowData.year || ""
            case "trackNo": return rowData.trackNo || ""
            case "discNo": return rowData.discNo || ""
            case "status":
                return rowData.errorText && rowData.errorText.length > 0
                    ? rowData.errorText
                    : (rowData.dirty ? "Modified" : "")
            default:
                return ""
            }
        }

        onOpened: initializeSelection()
        onClosed: {
            clearSelection()
            totalRows = 0
            operationInFlight = false
            operationText = ""
            tagEditorStatusFlash.stop()
            tagEditorStatusFlash.visible = false
        }

        Timer {
            id: tagEditorStatusFlash
            interval: 2200
            repeat: false
            property bool visible: false
            onTriggered: visible = false
        }

        Connections {
            target: tagEditorApi
            function onStatusChanged() {
                if (!tagEditorApi.statusText.length) {
                    tagEditorStatusFlash.stop()
                    tagEditorStatusFlash.visible = false
                    return
                }
                tagEditorStatusFlash.visible = true
            }
        }

        background: Rectangle {
            radius: 6
            color: root.uiSurfaceRaisedColor
            border.color: root.uiBorderColor
        }

        header: Frame {
            padding: 8
            background: Rectangle {
                radius: 6
                color: root.uiHeaderColor
                border.color: root.uiBorderColor
            }

            contentItem: RowLayout {
                spacing: 8

                Label {
                    text: "Unified Tag Editor"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    visible: tagEditorDialog.operationInFlight || tagEditorApi.loading || tagEditorApi.saving
                        || tagEditorStatusFlash.visible || tagEditorApi.statusText.length > 0
                    radius: 4
                    color: tagEditorDialog.operationInFlight || tagEditorApi.loading || tagEditorApi.saving
                        ? Qt.rgba(0.12, 0.44, 0.72, 0.16)
                        : (tagEditorDialog.statusHasFailure
                            ? Qt.rgba(0.78, 0.20, 0.20, 0.14)
                            : Qt.rgba(0.20, 0.55, 0.24, 0.14))
                    border.color: tagEditorDialog.operationInFlight || tagEditorApi.loading || tagEditorApi.saving
                        ? Qt.rgba(0.12, 0.44, 0.72, 0.42)
                        : (tagEditorDialog.statusHasFailure
                            ? Qt.rgba(0.78, 0.20, 0.20, 0.42)
                            : Qt.rgba(0.20, 0.55, 0.24, 0.42))
                    Layout.fillWidth: true
                    implicitHeight: 28

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        BusyIndicator {
                            running: tagEditorDialog.operationInFlight || tagEditorApi.loading || tagEditorApi.saving
                            visible: running
                            implicitWidth: 16
                            implicitHeight: 16
                        }

                        Label {
                            text: tagEditorDialog.operationInFlight || tagEditorApi.loading || tagEditorApi.saving
                                ? tagEditorDialog.operationText
                                : tagEditorApi.statusText
                            color: tagEditorDialog.statusHasFailure
                                ? Kirigami.Theme.negativeTextColor
                                : Kirigami.Theme.textColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Button {
                            visible: !tagEditorDialog.operationInFlight
                                && !tagEditorApi.loading
                                && !tagEditorApi.saving
                                && tagEditorApi.statusDetails.length > 0
                            text: "Details"
                            padding: 3
                            implicitHeight: 22
                            onClicked: tagEditorStatusDetailsDialog.open()
                        }
                    }
                }

                Item {
                    visible: !tagEditorDialog.operationInFlight
                        && !tagEditorApi.loading
                        && !tagEditorApi.saving
                        && !tagEditorStatusFlash.visible
                        && !tagEditorApi.statusText.length
                    Layout.fillWidth: true
                }

                Label {
                    text: tagEditorDialog.selectedRows.length > 0
                        ? tagEditorDialog.selectedRows.length + " selected"
                        : tagEditorDialog.totalRows + " loaded"
                    color: tagEditorApi.dirty ? Kirigami.Theme.negativeTextColor : root.uiMutedTextColor
                    font.weight: Font.DemiBold
                }

                Button {
                    text: "\u00d7"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving
                    padding: 2
                    implicitWidth: 26
                    implicitHeight: 24
                    onClicked: tagEditorDialog.requestClose()
                }
            }
        }

        footer: Frame {
            padding: 8
            background: Rectangle {
                radius: 6
                color: root.uiHeaderColor
                border.color: root.uiBorderColor
            }

            contentItem: RowLayout {
                spacing: 8

                Button {
                    text: "Clear Selection"
                    enabled: tagEditorDialog.selectedRows.length > 0
                    onClicked: tagEditorDialog.clearSelection()
                }

                Item { Layout.fillWidth: true }

                Button {
                    text: "Reload"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving
                    onClicked: {
                        tagEditorDialog.clearSelection()
                        tagEditorApi.reload()
                        tagEditorDialog.initializeSelection()
                    }
                }
                Button {
                    text: "Auto Number"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving
                    onClicked: autoNumberDialog.open()
                }
                Button {
                    text: "Rename Files"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving && tagEditorDialog.selectedRows.length > 0
                    onClicked: tagEditorDialog.triggerRename()
                }
                Button {
                    text: "Cancel"
                    onClicked: {
                        tagEditorDialog.closeEditor()
                    }
                }
                Button {
                    text: tagEditorApi.saving ? "Saving..." : "Save"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving && tagEditorApi.dirty
                    highlighted: true
                    onClicked: tagEditorDialog.triggerSave(false)
                }
            }
        }

        contentItem: RowLayout {
            spacing: 6

            Shortcut {
                sequences: ["Ctrl+S"]
                context: Qt.WindowShortcut
                enabled: tagEditorDialog.visible && !tagEditorApi.loading && !tagEditorApi.saving && tagEditorApi.dirty
                onActivated: tagEditorDialog.triggerSave(false)
            }

            Shortcut {
                sequences: ["Ctrl+A"]
                context: Qt.WindowShortcut
                enabled: tagEditorDialog.visible && tagEditorDialog.totalRows > 0
                onActivated: tagEditorDialog.selectAllRows()
            }

            Shortcut {
                sequences: ["Esc"]
                context: Qt.WindowShortcut
                enabled: tagEditorDialog.visible && !tagEditorCloseConfirmDialog.visible
                onActivated: tagEditorDialog.requestClose()
            }

            FocusScope {
                id: tagEditorListFocusSink
                visible: false
                width: 0
                height: 0
            }

            Frame {
                id: tagEditorEditorPane
                Layout.preferredWidth: 332
                Layout.minimumWidth: 320
                Layout.fillHeight: true
                padding: 3
                background: Rectangle {
                    radius: 4
                    color: root.uiSurfaceColor
                    border.color: root.uiBorderColor
                }

                contentItem: ScrollView {
                    clip: true
                    padding: 0
                    contentWidth: availableWidth
                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                    Column {
                        width: tagEditorEditorPane.availableWidth
                        spacing: 4

                        Repeater {
                            model: tagEditorDialog.editorFields
                            delegate: Column {
                                required property var modelData
                                width: tagEditorEditorPane.availableWidth
                                spacing: 1

                                Row {
                                    width: parent.width
                                    spacing: 2

                                    Label {
                                        text: modelData.label
                                        font.pixelSize: 11
                                        color: root.uiMutedTextColor
                                        width: parent.width - (caseButton.visible ? caseButton.width + parent.spacing : 0)
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    Button {
                                        id: caseButton
                                        visible: modelData.allowTitleCase || modelData.allowCapitalize
                                        text: "Aa"
                                        padding: 1
                                        implicitWidth: 22
                                        implicitHeight: 18
                                        onClicked: {
                                            const popupPosition = mapToItem(
                                                tagEditorDialog.contentItem,
                                                0,
                                                height + 2)
                                            casePopup.x = popupPosition.x
                                            casePopup.y = popupPosition.y
                                            casePopup.open()
                                        }
                                    }
                                }

                                TextField {
                                    id: fieldEditor
                                    width: parent.width
                                    selectByMouse: true
                                    persistentSelection: true
                                    property bool touched: false
                                    property var editRowsSnapshot: []

                                    function captureEditRowsSnapshot() {
                                        editRowsSnapshot = tagEditorDialog.selectedRows.slice()
                                    }

                                    function syncFromController() {
                                        const value = tagEditorApi.bulkValue(modelData.key)
                                        const mixed = value === tagEditorDialog.keepText
                                        if (activeFocus && touched) {
                                            return
                                        }
                                        text = mixed ? "" : value
                                        placeholderText = mixed ? tagEditorDialog.keepText : ""
                                        editRowsSnapshot = []
                                        touched = false
                                    }

                                    onActiveFocusChanged: {
                                        if (activeFocus && !touched) {
                                            captureEditRowsSnapshot()
                                        }
                                    }
                                    onTextEdited: {
                                        if (!touched) {
                                            captureEditRowsSnapshot()
                                        }
                                        touched = true
                                    }
                                    onEditingFinished: {
                                        if (!touched) {
                                            return
                                        }
                                        tagEditorApi.applyBulkFieldToRows(
                                            editRowsSnapshot,
                                            modelData.key,
                                            text)
                                        editRowsSnapshot = []
                                        touched = false
                                        syncFromController()
                                    }

                                    Component.onCompleted: syncFromController()

                                    Connections {
                                        target: tagEditorApi
                                        ignoreUnknownSignals: true
                                        function onSelectionChanged() { fieldEditor.syncFromController() }
                                        function onBulkSummaryChanged() { fieldEditor.syncFromController() }
                                    }
                                }

                                Popup {
                                    id: casePopup
                                    parent: tagEditorDialog.contentItem
                                    width: 128
                                    padding: 6
                                    z: 1000
                                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                    background: Rectangle {
                                        radius: 4
                                        color: root.uiSurfaceRaisedColor
                                        border.color: root.uiBorderColor
                                    }

                                    contentItem: ColumnLayout {
                                        spacing: 4

                                        Button {
                                            visible: modelData.allowTitleCase
                                            text: "Title Case"
                                            Layout.fillWidth: true
                                            onClicked: {
                                                tagEditorApi.applyEnglishTitleCase(modelData.key)
                                                fieldEditor.syncFromController()
                                                casePopup.close()
                                            }
                                        }

                                        Button {
                                            visible: modelData.allowCapitalize
                                            text: "Capital Case"
                                            Layout.fillWidth: true
                                            onClicked: {
                                                if (modelData.key === "genre") {
                                                    tagEditorApi.applyGenreCapitalize()
                                                } else {
                                                    tagEditorApi.applyFinnishCapitalize(modelData.key)
                                                }
                                                fieldEditor.syncFromController()
                                                casePopup.close()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Frame {
                Layout.fillWidth: true
                Layout.fillHeight: true
                padding: 0
                background: Rectangle {
                    radius: 4
                    color: root.uiSurfaceColor
                    border.color: root.uiBorderColor
                }

                contentItem: ScrollView {
                    clip: true
                    Flickable {
                        id: tagEditorTableFlick
                        contentWidth: tagEditorDialog.tableContentWidth
                        contentHeight: tagEditorTableColumn.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        Column {
                            id: tagEditorTableColumn
                            width: tagEditorTableFlick.contentWidth
                            spacing: 0

                            Rectangle {
                                width: parent.width
                                height: 26
                                color: root.uiColumnsColor

                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: 6
                                    anchors.rightMargin: 6
                                    spacing: 6

                                    Repeater {
                                        model: tagEditorDialog.tableColumns
                                        delegate: Label {
                                            required property var modelData
                                            width: modelData.width
                                            height: parent.height
                                            text: modelData.label
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            verticalAlignment: Text.AlignVCenter
                                            horizontalAlignment: Text.AlignLeft
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }

                            Repeater {
                                model: tagEditorApi.tableModel
                                delegate: Rectangle {
                                    required property int index
                                    required property string path
                                    required property string fileName
                                    required property string directory
                                    required property string formatKind
                                    required property string title
                                    required property string artist
                                    required property string album
                                    required property string albumArtist
                                    required property string genre
                                    required property string year
                                    required property string trackNo
                                    required property string discNo
                                    required property string totalTracks
                                    required property string totalDiscs
                                    required property string comment
                                    required property bool dirty
                                    required property string errorText

                                    width: tagEditorTableColumn.width
                                    height: 24
                                    color: tagEditorDialog.isSelected(index)
                                        ? Qt.rgba(0.12, 0.44, 0.72, 0.14)
                                        : (index % 2 === 0
                                            ? Qt.rgba(1, 1, 1, 0.02)
                                            : "transparent")
                                    border.color: errorText.length > 0
                                        ? Kirigami.Theme.negativeTextColor
                                        : (tagEditorDialog.isSelected(index)
                                            ? Qt.rgba(0.12, 0.44, 0.72, 0.34)
                                            : root.uiBorderColor)
                                    border.width: errorText.length > 0 || tagEditorDialog.isSelected(index) ? 1 : 0

                                    MouseArea {
                                        anchors.fill: parent
                                        acceptedButtons: Qt.LeftButton
                                        onClicked: function(mouse) {
                                            tagEditorDialog.handleRowClick(index, mouse.modifiers)
                                        }
                                    }

                                    Row {
                                        anchors.fill: parent
                                        anchors.leftMargin: 6
                                        anchors.rightMargin: 6
                                        spacing: 6

                                        Repeater {
                                            model: tagEditorDialog.tableColumns
                                            delegate: Label {
                                                required property var modelData
                                                width: modelData.width
                                                height: parent.height
                                                text: tagEditorDialog.rowText(modelData.key, {
                                                    fileName: fileName,
                                                    directory: directory,
                                                    formatKind: formatKind,
                                                    title: title,
                                                    artist: artist,
                                                    album: album,
                                                    albumArtist: albumArtist,
                                                    genre: genre,
                                                    year: year,
                                                    trackNo: trackNo,
                                                    discNo: discNo,
                                                    dirty: dirty,
                                                    errorText: errorText
                                                })
                                                color: modelData.key === "status" && errorText.length > 0
                                                    ? Kirigami.Theme.negativeTextColor
                                                    : Kirigami.Theme.textColor
                                                verticalAlignment: Text.AlignVCenter
                                                horizontalAlignment: Text.AlignLeft
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: tagEditorCloseConfirmDialog
        parent: Overlay.overlay
        modal: true
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: 400
        title: "Unsaved Changes"
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 6
            color: root.uiSurfaceRaisedColor
            border.color: root.uiBorderColor
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Save changes before closing the tag editor?"
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: root.uiMutedTextColor
                text: "Save writes the current edits. Discard closes the dialog and drops every unsaved change."
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                Button {
                    text: "Keep Editing"
                    onClicked: tagEditorCloseConfirmDialog.close()
                }
                Button {
                    text: "Discard"
                    onClicked: {
                        tagEditorCloseConfirmDialog.close()
                        tagEditorDialog.closeEditor()
                    }
                }
                Button {
                    text: "Save"
                    enabled: !tagEditorApi.loading && !tagEditorApi.saving && tagEditorApi.dirty
                    highlighted: true
                    onClicked: tagEditorDialog.saveAndClose()
                }
            }
        }
    }

    Dialog {
        id: tagEditorStatusDetailsDialog
        parent: Overlay.overlay
        modal: true
        x: Math.round((root.width - width) / 2)
        y: Math.round((root.height - height) / 2)
        width: Math.min(root.width - 80, 640)
        height: Math.min(root.height - 120, 520)
        title: "Tag Editor Details"
        standardButtons: Dialog.NoButton
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 6
            color: root.uiSurfaceRaisedColor
            border.color: root.uiBorderColor
        }

        contentItem: ColumnLayout {
            spacing: 10

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                TextArea {
                    readOnly: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: tagEditorApi.statusDetails
                    selectByMouse: true
                    background: null
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: "Close"
                    onClicked: tagEditorStatusDetailsDialog.close()
                }
            }
        }
    }

    Connections {
        target: tagEditorApi
        ignoreUnknownSignals: true
        function onOpenChanged() {
            if (tagEditorApi.open && !tagEditorDialog.visible) {
                tagEditorDialog.open()
            } else if (!tagEditorApi.open && tagEditorDialog.visible) {
                tagEditorDialog.close()
            }
        }
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
        root.syncGlobalSearchSelectionAfterResultsChange()
    }
}
